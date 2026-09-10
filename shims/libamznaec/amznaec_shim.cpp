/*
 * amznaec_shim: acoustic echo cancellation for the Amazon Echo Show
 * microphone path on LineageOS.
 *
 * The Echo Show mic front end is an FPGA that delivers a 6 channel,
 * 24 bit, 16 kHz stream over SPI: channels 0 to 3 are the microphones,
 * channels 4 and 5 are a loopback of the DAC output, sample aligned with
 * the microphones and exactly zero while nothing plays. Amazon's audio HAL
 * blob reads that stream with tinyalsa and mixes it down to mono, and
 * nothing in the ROM ever uses the loopback channels, so every capture
 * hears the speaker at full strength.
 *
 * This library is LD_PRELOADed into android.hardware.audio.service. It
 * interposes pcm_open/pcm_read/pcm_close, recognises the microphone PCM by
 * its parameters, and rewrites the four microphone channels in place with
 * WebRTC's echo canceller (plus optional noise suppression and high pass
 * filter) using the loopback channels as the far end. The blob sees the
 * same buffer layout it always did, so its own channel mixing, gain and
 * format conversion are untouched. Processing runs in 10 ms blocks, which
 * adds one block (10 ms) of latency to capture.
 *
 * Properties (read when the PCM is opened):
 *   persist.vendor.amznaec.enable     1   master switch (0 = pass through)
 *   persist.vendor.amznaec.aec        1   echo cancellation
 *   persist.vendor.amznaec.aec_level  0   0 low, 1 moderate, 2 high suppression. Low keeps
 *                                         the near end usable during playback (a wake word
 *                                         loses 10 dB instead of 19 dB at high), high gives
 *                                         the cleanest recording
 *   persist.vendor.amznaec.ns         1   noise suppression
 *   persist.vendor.amznaec.ns_level   0   0 low, 1 moderate, 2 high, 3 very high
 *   persist.vendor.amznaec.hpf        1   high pass filter
 *   persist.vendor.amznaec.delay_ms   0   reported stream delay hint
 *   persist.vendor.amznaec.extended   1   WebRTC extended filter (about 10 points of CPU)
 *   persist.vendor.amznaec.agnostic   0   WebRTC delay agnostic mode (costly)
 *   persist.vendor.amznaec.mics       1   bitmask of mic channels to process (the blob
 *                                         keeps channel 0; 15 processes all four)
 *   persist.vendor.amznaec.mute       0   debug: bitmask of mic channels to zero
 *   persist.vendor.amznaec.array      0   0 = process each selected mic on its own,
 *                                         1 = average the selected mics into channel 0
 *                                             and process that (linear, no geometry),
 *                                         2 = WebRTC beamformer over the selected mics
 *                                             into channel 0 (needs geometry)
 *   persist.vendor.amznaec.geom       ""  mic positions in metres, "x,y,z,x,y,z,...",
 *                                         one triple per selected mic; default is a
 *                                         line along x with 2 cm spacing
 *   persist.vendor.amznaec.target     90  beam target azimuth in degrees (90 = broadside)
 *   persist.vendor.amznaec.engine     0   0 = WebRTC (linear filter plus nonlinear
 *                                         suppression, attenuates the near end during
 *                                         double talk), 1 = Speex MDF (linear only,
 *                                         near end untouched, residual echo higher)
 *   persist.vendor.amznaec.spx_stereo 1   Speex models the two loopback channels as
 *                                         two speakers (0 = mono average reference)
 *   persist.vendor.amznaec.spx_headroom_db 12  attenuation applied before Speex's 16 bit
 *                                         input and undone after; Speex stops adapting
 *                                         when its input nears full scale
 *   persist.vendor.amznaec.spx_filter_ms   64  Speex filter length in ms
 *   persist.vendor.amznaec.spx_echo_suppress        0  Speex residual echo suppression
 *                                         in dB (0 disables the preprocessor entirely)
 *   persist.vendor.amznaec.spx_echo_suppress_active 0  same, while the near end is active
 *   persist.vendor.amznaec.spx_denoise    0   Speex noise suppression on/off
 *   persist.vendor.amznaec.spx_noise_suppress -15  Speex noise suppression in dB
 *   persist.vendor.amznaec.gain_db    20  digital gain applied to the processed channel
 *                                         after cancellation. Pairs with the codec's analog
 *                                         mic gain at Amazon's +20 dB (audio_device.xml
 *                                         MICPGA 40); the port's +40 dB clips the ADC at
 *                                         loud playback
 *   persist.vendor.amznaec.ref_clip   0   experimental: hard clip the loopback reference at
 *                                         this fraction of full scale (x1000, e.g. 250) so
 *                                         the filter can model an amplifier that clips
 *   persist.vendor.amznaec.log        0   1 = log levels every 5 s
 *   persist.vendor.amznaec.replay     ""  debug: path of a raw 6 channel S24_3LE file
 *                                         whose frames replace the captured ones
 *                                         (looped), so settings can be compared on
 *                                         identical audio
 *   persist.vendor.amznaec.dump       ""  debug: path prefix; the frames entering the
 *                                         processing go to <prefix>.in.raw and the
 *                                         frames returned to the blob to <prefix>.out.raw
 */

#define LOG_TAG "amznaec"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include <cutils/properties.h>
#include <log/log.h>
#include <tinyalsa/asoundlib.h>

#include "webrtc/modules/audio_processing/include/audio_processing.h"
#include "webrtc/modules/include/module_common_types.h"
#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"

namespace {

constexpr int kRate = 16000;
constexpr unsigned kChannels = 6;
constexpr int kMics = 4;
constexpr int kRefFirst = 4;
constexpr int kBlock = 160;  /* 10 ms at 16 kHz, what WebRTC wants */
constexpr unsigned kFrameBytes = kChannels * 3;
constexpr unsigned kBlockBytes = kBlock * kFrameBytes;

typedef struct pcm* (*pcm_open_t)(unsigned, unsigned, unsigned, struct pcm_config*);
typedef int (*pcm_read_t)(struct pcm*, void*, unsigned);
typedef int (*pcm_close_t)(struct pcm*);

pcm_open_t real_pcm_open;
pcm_read_t real_pcm_read;
pcm_close_t real_pcm_close;

struct Settings {
    bool enable, aec, ns, hpf, log, extended, agnostic;
    int aec_level, ns_level, delay_ms, mics, mute, array, target_deg;
    int engine, spx_filter_ms, spx_echo_suppress, spx_echo_suppress_active, spx_denoise, spx_noise_suppress, spx_stereo;
    int spx_headroom_db, gain_db, ref_clip;
    char geom[PROPERTY_VALUE_MAX];
    char replay[PROPERTY_VALUE_MAX];
    char dump[PROPERTY_VALUE_MAX];
};

struct State {
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    struct pcm* pcm = nullptr;
    Settings s{};
    webrtc::AudioProcessing* apm[kMics] = {}; /* array 0: one per selected mic; else apm[0] */
    int mode = 0;                              /* validated array mode */
    int nsel = 0;                              /* selected mics */
    int sel[kMics] = {};
    std::vector<webrtc::Point> geometry;
    SpeexEchoState* spx = nullptr;          /* engine 1 */
    SpeexPreprocessState* spx_pp = nullptr;
    float hpf_x1 = 0, hpf_y1 = 0;           /* engine 1 high pass state */
    FILE* replay = nullptr;    /* debug replay source, see the header */
    FILE* dump_in = nullptr;   /* debug dumps, see the header */
    FILE* dump_out = nullptr;
    std::vector<uint8_t> in;   /* raw frames waiting for a full block */
    std::vector<uint8_t> out;  /* processed frames waiting to be returned */
    float near_[kMics][kBlock];
    float mix[kBlock];
    float outbuf[kBlock];
    float ref[kBlock];
    float refl[kBlock], refr[kBlock];  /* the two loopback channels separately */
    /* stats */
    double e_ref = 0, e_in = 0, e_out = 0;
    unsigned blocks = 0;
};

State g;

/* The blob reaches libtinyalsa through hw_get_module's dlopen, a local
 * group that RTLD_NEXT does not walk, so look the symbols up on the library
 * itself. dlopen returns the instance the blob already holds. */
void resolve() {
    if (real_pcm_open) return;
    void* h = dlopen("libtinyalsa.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("libtinyalsa.so", RTLD_NOW);
    if (h) {
        real_pcm_open = (pcm_open_t)dlsym(h, "pcm_open");
        real_pcm_read = (pcm_read_t)dlsym(h, "pcm_read");
        real_pcm_close = (pcm_close_t)dlsym(h, "pcm_close");
    }
    if (!real_pcm_open) real_pcm_open = (pcm_open_t)dlsym(RTLD_NEXT, "pcm_open");
    if (!real_pcm_read) real_pcm_read = (pcm_read_t)dlsym(RTLD_NEXT, "pcm_read");
    if (!real_pcm_close) real_pcm_close = (pcm_close_t)dlsym(RTLD_NEXT, "pcm_close");
    if (!real_pcm_open || !real_pcm_read || !real_pcm_close)
        LOG_ALWAYS_FATAL("failed to resolve tinyalsa symbols: open=%p read=%p close=%p (%s)",
                         real_pcm_open, real_pcm_read, real_pcm_close, dlerror());
}

Settings read_settings() {
    Settings s;
    s.enable = property_get_bool("persist.vendor.amznaec.enable", true);
    s.aec = property_get_bool("persist.vendor.amznaec.aec", true);
    s.ns = property_get_bool("persist.vendor.amznaec.ns", true);
    s.hpf = property_get_bool("persist.vendor.amznaec.hpf", true);
    s.log = property_get_bool("persist.vendor.amznaec.log", false);
    s.aec_level = property_get_int32("persist.vendor.amznaec.aec_level", 0);
    s.ns_level = property_get_int32("persist.vendor.amznaec.ns_level", 0);
    s.delay_ms = property_get_int32("persist.vendor.amznaec.delay_ms", 0);
    s.extended = property_get_bool("persist.vendor.amznaec.extended", true);
    s.agnostic = property_get_bool("persist.vendor.amznaec.agnostic", false);
    s.mics = property_get_int32("persist.vendor.amznaec.mics", 0x1) & 0xf;
    s.mute = property_get_int32("persist.vendor.amznaec.mute", 0) & 0xf;
    s.array = property_get_int32("persist.vendor.amznaec.array", 0);
    s.target_deg = property_get_int32("persist.vendor.amznaec.target", 90);
    property_get("persist.vendor.amznaec.geom", s.geom, "");
    property_get("persist.vendor.amznaec.replay", s.replay, "");
    property_get("persist.vendor.amznaec.dump", s.dump, "");
    s.engine = property_get_int32("persist.vendor.amznaec.engine", 0);
    s.spx_filter_ms = property_get_int32("persist.vendor.amznaec.spx_filter_ms", 64);
    s.spx_echo_suppress = property_get_int32("persist.vendor.amznaec.spx_echo_suppress", 0);
    s.spx_echo_suppress_active = property_get_int32("persist.vendor.amznaec.spx_echo_suppress_active", 0);
    s.spx_denoise = property_get_int32("persist.vendor.amznaec.spx_denoise", 0);
    s.spx_noise_suppress = property_get_int32("persist.vendor.amznaec.spx_noise_suppress", -15);
    s.spx_stereo = property_get_int32("persist.vendor.amznaec.spx_stereo", 1);
    s.spx_headroom_db = property_get_int32("persist.vendor.amznaec.spx_headroom_db", 12);
    s.gain_db = property_get_int32("persist.vendor.amznaec.gain_db", 20);
    s.ref_clip = property_get_int32("persist.vendor.amznaec.ref_clip", 0);
    return s;
}

webrtc::AudioProcessing* make_apm(const Settings& s, int in_channels, bool beamform) {
    /* The FPGA delivers the loopback in the same frame as the microphones,
     * so the far end is already aligned and the plain filter with a fixed
     * delay converges fine. The robust modes are kept as options. */
    webrtc::Config cfg;
    cfg.Set<webrtc::ExtendedFilter>(new webrtc::ExtendedFilter(s.extended));
    cfg.Set<webrtc::DelayAgnostic>(new webrtc::DelayAgnostic(s.agnostic));
    if (beamform) {
        float az = (float)s.target_deg * (float)M_PI / 180.f;
        cfg.Set<webrtc::Beamforming>(new webrtc::Beamforming(
            true, g.geometry, webrtc::SphericalPointf(az, 0.f, 1.f)));
    }
    webrtc::AudioProcessing* apm = webrtc::AudioProcessing::Create(cfg);
    if (!apm) return nullptr;
    const webrtc::ProcessingConfig pc = {
        {{kRate, (size_t)in_channels}, {kRate, 1}, {kRate, 1}, {kRate, 1}}};
    if (apm->Initialize(pc) != 0) {
        ALOGE("AudioProcessing::Initialize failed");
        delete apm;
        return nullptr;
    }
    apm->high_pass_filter()->Enable(s.hpf);
    if (s.aec) {
        webrtc::EchoCancellation* ec = apm->echo_cancellation();
        ec->enable_drift_compensation(false);
        ec->set_suppression_level(
            s.aec_level <= 0 ? webrtc::EchoCancellation::kLowSuppression
            : s.aec_level == 1 ? webrtc::EchoCancellation::kModerateSuppression
                               : webrtc::EchoCancellation::kHighSuppression);
        ec->Enable(true);
    }
    if (s.ns) {
        webrtc::NoiseSuppression* ns = apm->noise_suppression();
        ns->set_level(s.ns_level <= 0 ? webrtc::NoiseSuppression::kLow
                      : s.ns_level == 1 ? webrtc::NoiseSuppression::kModerate
                      : s.ns_level == 2 ? webrtc::NoiseSuppression::kHigh
                                        : webrtc::NoiseSuppression::kVeryHigh);
        ns->Enable(true);
    }
    return apm;
}

void teardown_l() {
    for (int i = 0; i < kMics; i++) {
        delete g.apm[i];
        g.apm[i] = nullptr;
    }
    g.pcm = nullptr;
    if (g.spx_pp) { speex_preprocess_state_destroy(g.spx_pp); g.spx_pp = nullptr; }
    if (g.spx) { speex_echo_state_destroy(g.spx); g.spx = nullptr; }
    g.hpf_x1 = g.hpf_y1 = 0;
    g.in.clear();
    g.out.clear();
    g.geometry.clear();
    if (g.replay) { fclose(g.replay); g.replay = nullptr; }
    if (g.dump_in) { fclose(g.dump_in); g.dump_in = nullptr; }
    if (g.dump_out) { fclose(g.dump_out); g.dump_out = nullptr; }
}

inline int32_t s24(const uint8_t* p) {
    int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
    return (v & 0x800000) ? v - (1 << 24) : v;
}

inline void put24(uint8_t* p, int32_t v) {
    if (v > 0x7fffff) v = 0x7fffff;
    if (v < -0x800000) v = -0x800000;
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
}

/* Parse "x,y,z,..." metres for nsel mics; fall back to a 2 cm line along x. */
void build_geometry(const Settings& s) {
    g.geometry.clear();
    std::vector<float> v;
    const char* p = s.geom;
    while (*p) {
        char* end;
        float f = strtof(p, &end);
        if (end == p) break;
        v.push_back(f);
        p = (*end == ',') ? end + 1 : end;
    }
    bool ok = v.size() == (size_t)g.nsel * 3;
    if (ok) {
        for (int i = 0; i < g.nsel; i++)
            g.geometry.push_back(webrtc::Point(v[3 * i], v[3 * i + 1], v[3 * i + 2]));
        /* the beamformer divides by the minimum spacing */
        for (int i = 0; i < g.nsel && ok; i++)
            for (int j = i + 1; j < g.nsel; j++) {
                float dx = v[3 * i] - v[3 * j], dy = v[3 * i + 1] - v[3 * j + 1],
                      dz = v[3 * i + 2] - v[3 * j + 2];
                if (dx * dx + dy * dy + dz * dz < 0.005f * 0.005f) ok = false;
            }
    }
    if (!ok) {
        if (s.geom[0]) ALOGW("ignoring persist.vendor.amznaec.geom \"%s\"", s.geom);
        g.geometry.clear();
        for (int i = 0; i < g.nsel; i++)
            g.geometry.push_back(webrtc::Point(((float)i - ((float)g.nsel - 1) / 2.f) * 0.02f, 0.f, 0.f));
    }
}

inline float s24f(const uint8_t* p) { return (float)s24(p) / 8388608.f; }

inline void putf(uint8_t* p, float v) {
    if (v > 0.9999999f) v = 0.9999999f;
    if (v < -1.f) v = -1.f;
    put24(p, (int32_t)(v * 8388608.f));
}

void run_apm(webrtc::AudioProcessing* apm, const float* const* src, int in_channels, float* dst) {
    const float* rev[1] = {g.ref};
    apm->AnalyzeReverseStream(rev, kBlock, kRate, webrtc::AudioProcessing::kMono);
    apm->set_stream_delay_ms(g.s.delay_ms);
    float* out[1] = {dst};
    if (apm->ProcessStream(src, webrtc::StreamConfig(kRate, in_channels),
                           webrtc::StreamConfig(kRate, 1), out) != 0)
        memcpy(dst, src[0], sizeof(float) * kBlock);
}

/* Speex path: linear MDF canceller on one float block, optional preprocessor. */
void run_speex(const float* src, float* dst) {
    int16_t near16[kBlock], ref16[kBlock * 2], out16[kBlock];
    /* first order high pass at ~80 Hz, keeps the DC and rumble out of the filter */
    const float a = 0.969f;
    const float sc = 32768.f * powf(10.f, -(float)g.s.spx_headroom_db / 20.f);
    for (int i = 0; i < kBlock; i++) {
        float x = src[i];
        float y = a * (g.hpf_y1 + x - g.hpf_x1);
        g.hpf_x1 = x; g.hpf_y1 = y;
        float v = g.s.hpf ? y : x;
        near16[i] = (int16_t)fmaxf(-32768.f, fminf(32767.f, v * sc));
        if (g.s.spx_stereo) {
            ref16[2 * i] = (int16_t)fmaxf(-32768.f, fminf(32767.f, g.refl[i] * sc));
            ref16[2 * i + 1] = (int16_t)fmaxf(-32768.f, fminf(32767.f, g.refr[i] * sc));
        } else {
            ref16[i] = (int16_t)fmaxf(-32768.f, fminf(32767.f, g.ref[i] * sc));
        }
    }
    speex_echo_cancellation(g.spx, near16, ref16, out16);
    if (g.spx_pp) speex_preprocess_run(g.spx_pp, out16);
    for (int i = 0; i < kBlock; i++) dst[i] = out16[i] / sc;
}

bool make_speex(const Settings& s) {
    int taps = (s.spx_filter_ms * kRate / 1000 / kBlock) * kBlock;
    if (taps < kBlock * 2) taps = kBlock * 2;
    g.spx = s.spx_stereo ? speex_echo_state_init_mc(kBlock, taps, 1, 2)
                         : speex_echo_state_init(kBlock, taps);
    if (!g.spx) return false;
    int rate = kRate;
    speex_echo_ctl(g.spx, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    if (s.spx_echo_suppress != 0 || s.spx_denoise) {
        g.spx_pp = speex_preprocess_state_init(kBlock, kRate);
        if (!g.spx_pp) return false;
        int on = s.spx_denoise ? 1 : 0, off = 0;
        speex_preprocess_ctl(g.spx_pp, SPEEX_PREPROCESS_SET_DENOISE, &on);
        speex_preprocess_ctl(g.spx_pp, SPEEX_PREPROCESS_SET_AGC, &off);
        speex_preprocess_ctl(g.spx_pp, SPEEX_PREPROCESS_SET_DEREVERB, &off);
        int ns = s.spx_noise_suppress;
        speex_preprocess_ctl(g.spx_pp, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &ns);
        if (s.spx_echo_suppress != 0) {
            speex_preprocess_ctl(g.spx_pp, SPEEX_PREPROCESS_SET_ECHO_STATE, g.spx);
            int es = s.spx_echo_suppress, esa = s.spx_echo_suppress_active;
            speex_preprocess_ctl(g.spx_pp, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &es);
            speex_preprocess_ctl(g.spx_pp, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &esa);
        }
    }
    return true;
}

/* Process one 10 ms block of raw frames in place. */
void process_block_l(uint8_t* frames) {
    double e_ref = 0, e_in = 0, e_out = 0;
    for (int f = 0; f < kBlock; f++) {
        const uint8_t* fr = frames + f * kFrameBytes;
        for (int m = 0; m < kMics; m++) g.near_[m][f] = s24f(fr + m * 3);
        g.refl[f] = s24f(fr + kRefFirst * 3);
        g.refr[f] = s24f(fr + (kRefFirst + 1) * 3);
        if (g.s.ref_clip > 0) {
            float c = (float)g.s.ref_clip / 1000.f;
            g.refl[f] = fmaxf(-c, fminf(c, g.refl[f]));
            g.refr[f] = fmaxf(-c, fminf(c, g.refr[f]));
        }
        g.ref[f] = (g.refl[f] + g.refr[f]) * 0.5f;
        e_ref += (double)g.ref[f] * g.ref[f];
        e_in += (double)g.near_[g.sel[0]][f] * g.near_[g.sel[0]][f];
    }

    if (g.spx && g.mode == 0) {
        run_speex(g.near_[g.sel[0]], g.outbuf);
        memcpy(g.near_[g.sel[0]], g.outbuf, sizeof(g.outbuf));
    } else if (g.spx && g.mode == 1) {
        float scale = 1.f / (float)g.nsel;
        for (int f = 0; f < kBlock; f++) {
            float a = 0;
            for (int k = 0; k < g.nsel; k++) a += g.near_[g.sel[k]][f];
            g.mix[f] = a * scale;
        }
        run_speex(g.mix, g.outbuf);
        memcpy(g.near_[0], g.outbuf, sizeof(g.outbuf));
    } else if (g.mode == 0) {
        for (int k = 0; k < g.nsel; k++) {
            int m = g.sel[k];
            if (!g.apm[m]) continue;
            const float* src[1] = {g.near_[m]};
            run_apm(g.apm[m], src, 1, g.outbuf);
            memcpy(g.near_[m], g.outbuf, sizeof(g.outbuf));
        }
    } else if (g.mode == 1) {
        float scale = 1.f / (float)g.nsel;
        for (int f = 0; f < kBlock; f++) {
            float a = 0;
            for (int k = 0; k < g.nsel; k++) a += g.near_[g.sel[k]][f];
            g.mix[f] = a * scale;
        }
        const float* src[1] = {g.mix};
        run_apm(g.apm[0], src, 1, g.outbuf);
        memcpy(g.near_[0], g.outbuf, sizeof(g.outbuf));
    } else {
        const float* src[kMics];
        for (int k = 0; k < g.nsel; k++) src[k] = g.near_[g.sel[k]];
        run_apm(g.apm[0], src, g.nsel, g.outbuf);
        memcpy(g.near_[0], g.outbuf, sizeof(g.outbuf));
    }

    const float gain = powf(10.f, (float)g.s.gain_db / 20.f);
    for (int f = 0; f < kBlock; f++) {
        uint8_t* fr = frames + f * kFrameBytes;
        for (int m = 0; m < kMics; m++) {
            bool processed = (g.mode == 0) ? (g.apm[m] != nullptr || (g.spx && m == g.sel[0])) : (m == 0);
            if (g.s.mute & (1 << m))
                put24(fr + m * 3, 0);
            else if (processed)
                putf(fr + m * 3, g.near_[m][f] * gain);
        }
        int o = (g.mode == 0) ? g.sel[0] : 0;
        e_out += (double)g.near_[o][f] * g.near_[o][f];
    }

    g.e_ref += e_ref;
    g.e_in += e_in;
    g.e_out += e_out;
    if (++g.blocks % 500 == 0 && g.s.log) {
        double n = 500.0 * kBlock;
        auto dbf = [n](double e) { return e > 0 ? 10.0 * log10(e / n) : -120.0; };
        webrtc::AudioProcessing* a0 = (g.mode == 0) ? g.apm[g.sel[0]] : g.apm[0];
        ALOGI("5s: ref %.1f dBFS, mic in %.1f dBFS, out %.1f dBFS, echo %s, mode %d, engine %s",
              dbf(g.e_ref), dbf(g.e_in), dbf(g.e_out),
              a0 ? (a0->echo_cancellation()->stream_has_echo() ? "yes" : "no") : "n/a", g.mode,
              g.spx ? "speex" : "webrtc");
        g.e_ref = g.e_in = g.e_out = 0;
    }
}

bool is_mic_pcm(unsigned device, unsigned flags, const struct pcm_config* c) {
    if (!(flags & PCM_IN) || !c) return false;
    /* Recognise the FPGA stream by shape rather than by device number, so a
     * renumbered card cannot make this touch the wrong stream. */
    return c->channels == kChannels && c->rate == (unsigned)kRate &&
           c->format == PCM_FORMAT_S24_3LE && device < 64;
}

}  // namespace

extern "C" struct pcm* pcm_open(unsigned int card, unsigned int device, unsigned int flags,
                                struct pcm_config* config) {
    resolve();
    struct pcm* pcm = real_pcm_open(card, device, flags, config);
    if (!pcm || !is_mic_pcm(device, flags, config)) return pcm;

    pthread_mutex_lock(&g.lock);
    teardown_l();
    g.s = read_settings();
    if (!g.s.enable) {
        ALOGI("mic PCM %u:%u opened, processing disabled by property", card, device);
        pthread_mutex_unlock(&g.lock);
        return pcm;
    }
    g.nsel = 0;
    for (int i = 0; i < kMics; i++)
        if (g.s.mics & (1 << i)) g.sel[g.nsel++] = i;
    if (g.nsel == 0) { g.sel[0] = 0; g.nsel = 1; }
    g.mode = g.s.array;
    if (g.mode < 0 || g.mode > 2 || (g.mode != 0 && g.nsel < 2)) g.mode = 0;
    build_geometry(g.s);
    bool ok = true;
    if (g.s.engine == 1 && g.mode != 2) {
        ok = make_speex(g.s);
    } else if (g.mode == 0) {
        for (int k = 0; k < g.nsel && ok; k++) {
            g.apm[g.sel[k]] = make_apm(g.s, 1, false);
            ok = g.apm[g.sel[k]] != nullptr;
        }
    } else {
        g.apm[0] = make_apm(g.s, g.mode == 2 ? g.nsel : 1, g.mode == 2);
        ok = g.apm[0] != nullptr;
    }
    if (!ok) {
        ALOGE("could not create the audio processors, passing audio through");
        teardown_l();
    } else {
        g.pcm = pcm;
        if (g.s.replay[0]) {
            g.replay = fopen(g.s.replay, "rb");
            ALOGW("replaying %s instead of the microphones: %s", g.s.replay, g.replay ? "ok" : "open failed");
        }
        if (g.s.dump[0]) {
            std::string a = std::string(g.s.dump) + ".in.raw", b = std::string(g.s.dump) + ".out.raw";
            g.dump_in = fopen(a.c_str(), "wb");
            g.dump_out = fopen(b.c_str(), "wb");
            ALOGW("dumping to %s.{in,out}.raw: %s", g.s.dump, (g.dump_in && g.dump_out) ? "ok" : "open failed");
        }
        g.in.reserve(4 * kBlockBytes);
        g.out.assign(kBlockBytes, 0); /* one block of priming silence */
        std::string geom;
        for (auto& pt : g.geometry) {
            char b[48];
            snprintf(b, sizeof(b), "%s(%.3f,%.3f,%.3f)", geom.empty() ? "" : " ", pt.x(), pt.y(), pt.z());
            geom += b;
        }
        ALOGI("mic PCM %u:%u opened: %u ch %u Hz period %u; aec=%d(level %d) ns=%d(level %d) "
              "hpf=%d delay=%dms extended=%d agnostic=%d mics=0x%x mute=0x%x array=%d target=%d "
              "geometry %s; engine %s (filter %d ms, echo suppress %d/%d dB, denoise %d/%d dB, stereo %d); gain %d dB",
              card, device, config->channels, config->rate, config->period_size,
              g.s.aec, g.s.aec_level, g.s.ns, g.s.ns_level, g.s.hpf, g.s.delay_ms,
              g.s.extended, g.s.agnostic, g.s.mics, g.s.mute, g.mode, g.s.target_deg, geom.c_str(),
              g.spx ? "speex" : "webrtc", g.s.spx_filter_ms, g.s.spx_echo_suppress,
              g.s.spx_echo_suppress_active, g.s.spx_denoise, g.s.spx_noise_suppress, g.s.spx_stereo, g.s.gain_db);
    }
    pthread_mutex_unlock(&g.lock);
    return pcm;
}

extern "C" int pcm_read(struct pcm* pcm, void* data, unsigned int count) {
    resolve();
    int rc = real_pcm_read(pcm, data, count);
    if (rc != 0 || pcm != g.pcm || count == 0 || (count % kFrameBytes) != 0) return rc;

    pthread_mutex_lock(&g.lock);
    if (pcm == g.pcm) {
        uint8_t* buf = (uint8_t*)data;
        if (g.replay) {
            size_t got = fread(buf, 1, count, g.replay);
            if (got < count) { rewind(g.replay); fread(buf + got, 1, count - got, g.replay); }
        }
        if (g.dump_in) fwrite(buf, 1, count, g.dump_in);
        g.in.insert(g.in.end(), buf, buf + count);
        size_t off = 0;
        while (g.in.size() - off >= kBlockBytes) {
            process_block_l(g.in.data() + off);
            g.out.insert(g.out.end(), g.in.begin() + off, g.in.begin() + off + kBlockBytes);
            off += kBlockBytes;
        }
        if (off) g.in.erase(g.in.begin(), g.in.begin() + off);
        /* By construction out always holds at least count bytes here: it was
         * primed with one block and every call adds as much as it removes. */
        if (g.out.size() >= count) {
            memcpy(buf, g.out.data(), count);
            g.out.erase(g.out.begin(), g.out.begin() + count);
            if (g.dump_out) fwrite(buf, 1, count, g.dump_out);
        } else {
            ALOGW("output underrun (%zu < %u), returning unprocessed audio", g.out.size(), count);
        }
    }
    pthread_mutex_unlock(&g.lock);
    return rc;
}

extern "C" int pcm_close(struct pcm* pcm) {
    resolve();
    pthread_mutex_lock(&g.lock);
    if (pcm == g.pcm) {
        ALOGI("mic PCM closed after %u blocks", g.blocks);
        teardown_l();
        g.blocks = 0;
    }
    pthread_mutex_unlock(&g.lock);
    return real_pcm_close(pcm);
}
