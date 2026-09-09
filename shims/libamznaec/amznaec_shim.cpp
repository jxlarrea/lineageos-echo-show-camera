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
 *   persist.vendor.amznaec.aec_level  2   0 low, 1 moderate, 2 high suppression
 *   persist.vendor.amznaec.ns         1   noise suppression
 *   persist.vendor.amznaec.ns_level   1   0 low, 1 moderate, 2 high, 3 very high
 *   persist.vendor.amznaec.hpf        1   high pass filter
 *   persist.vendor.amznaec.delay_ms   0   reported stream delay hint
 *   persist.vendor.amznaec.extended   0   WebRTC extended filter (costly)
 *   persist.vendor.amznaec.agnostic   0   WebRTC delay agnostic mode (costly)
 *   persist.vendor.amznaec.mics       1   bitmask of mic channels to process (the blob
 *                                         keeps channel 0; 15 processes all four)
 *   persist.vendor.amznaec.mute       0   debug: bitmask of mic channels to zero
 *   persist.vendor.amznaec.log        0   1 = log levels every 5 s
 */

#define LOG_TAG "amznaec"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <cutils/properties.h>
#include <log/log.h>
#include <tinyalsa/asoundlib.h>

#include "webrtc/modules/audio_processing/include/audio_processing.h"
#include "webrtc/modules/include/module_common_types.h"

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
    int aec_level, ns_level, delay_ms, mics, mute;
};

struct State {
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    struct pcm* pcm = nullptr;
    Settings s{};
    webrtc::AudioProcessing* apm[kMics] = {};
    std::vector<uint8_t> in;   /* raw frames waiting for a full block */
    std::vector<uint8_t> out;  /* processed frames waiting to be returned */
    int16_t near_[kMics][kBlock];
    int16_t ref[kBlock];
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
    s.aec_level = property_get_int32("persist.vendor.amznaec.aec_level", 2);
    s.ns_level = property_get_int32("persist.vendor.amznaec.ns_level", 1);
    s.delay_ms = property_get_int32("persist.vendor.amznaec.delay_ms", 0);
    s.extended = property_get_bool("persist.vendor.amznaec.extended", false);
    s.agnostic = property_get_bool("persist.vendor.amznaec.agnostic", false);
    s.mics = property_get_int32("persist.vendor.amznaec.mics", 0x1) & 0xf;
    s.mute = property_get_int32("persist.vendor.amznaec.mute", 0) & 0xf;
    return s;
}

webrtc::AudioProcessing* make_apm(const Settings& s) {
    /* The FPGA delivers the loopback in the same frame as the microphones,
     * so the far end is already aligned and the plain filter with a fixed
     * delay converges fine. The robust modes are kept as options. */
    webrtc::Config cfg;
    cfg.Set<webrtc::ExtendedFilter>(new webrtc::ExtendedFilter(s.extended));
    cfg.Set<webrtc::DelayAgnostic>(new webrtc::DelayAgnostic(s.agnostic));
    webrtc::AudioProcessing* apm = webrtc::AudioProcessing::Create(cfg);
    if (!apm) return nullptr;
    const webrtc::ProcessingConfig pc = {
        {{kRate, 1}, {kRate, 1}, {kRate, 1}, {kRate, 1}}};
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
    g.in.clear();
    g.out.clear();
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

/* Process one 10 ms block of raw frames in place. */
void process_block_l(uint8_t* frames) {
    double e_ref = 0, e_in = 0, e_out = 0;
    for (int f = 0; f < kBlock; f++) {
        const uint8_t* fr = frames + f * kFrameBytes;
        for (int m = 0; m < kMics; m++)
            g.near_[m][f] = (int16_t)(s24(fr + m * 3) >> 8);
        int32_t r = (s24(fr + kRefFirst * 3) + s24(fr + (kRefFirst + 1) * 3)) / 2;
        g.ref[f] = (int16_t)(r >> 8);
        e_ref += (double)g.ref[f] * g.ref[f];
        e_in += (double)g.near_[0][f] * g.near_[0][f];
    }

    webrtc::AudioFrame frame;
    for (int m = 0; m < kMics; m++) {
        webrtc::AudioProcessing* apm = g.apm[m];
        if (!apm) continue;
        frame.UpdateFrame(0, 0, g.ref, kBlock, kRate,
                          webrtc::AudioFrame::kNormalSpeech,
                          webrtc::AudioFrame::kVadUnknown, 1);
        apm->AnalyzeReverseStream(&frame);
        apm->set_stream_delay_ms(g.s.delay_ms);
        frame.UpdateFrame(0, 0, g.near_[m], kBlock, kRate,
                          webrtc::AudioFrame::kNormalSpeech,
                          webrtc::AudioFrame::kVadUnknown, 1);
        if (apm->ProcessStream(&frame) == 0)
            memcpy(g.near_[m], frame.data_, sizeof(g.near_[m]));
    }

    for (int f = 0; f < kBlock; f++) {
        uint8_t* fr = frames + f * kFrameBytes;
        for (int m = 0; m < kMics; m++) {
            if (g.s.mute & (1 << m))
                put24(fr + m * 3, 0);
            else if (g.apm[m])
                put24(fr + m * 3, (int32_t)g.near_[m][f] << 8);
        }
        e_out += (double)g.near_[0][f] * g.near_[0][f];
    }

    g.e_ref += e_ref;
    g.e_in += e_in;
    g.e_out += e_out;
    if (++g.blocks % 500 == 0 && g.s.log) {
        double n = 500.0 * kBlock;
        auto db = [](double e) { return e > 0 ? 10.0 * log10(e / (32768.0 * 32768.0)) : -120.0; };
        ALOGI("5s: ref %.1f dBFS, mic0 in %.1f dBFS, mic0 out %.1f dBFS, echo %s",
              db(g.e_ref / n), db(g.e_in / n), db(g.e_out / n),
              g.apm[0] && g.apm[0]->echo_cancellation()->stream_has_echo() ? "yes" : "no");
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
    bool ok = true;
    for (int i = 0; i < kMics && ok; i++) {
        if (!(g.s.mics & (1 << i))) continue;
        g.apm[i] = make_apm(g.s);
        ok = g.apm[i] != nullptr;
    }
    if (!ok) {
        ALOGE("could not create WebRTC processors, passing audio through");
        teardown_l();
    } else {
        g.pcm = pcm;
        g.in.reserve(4 * kBlockBytes);
        g.out.assign(kBlockBytes, 0); /* one block of priming silence */
        ALOGI("mic PCM %u:%u opened: %u ch %u Hz period %u; aec=%d(level %d) ns=%d(level %d) "
              "hpf=%d delay=%dms extended=%d agnostic=%d mics=0x%x mute=0x%x",
              card, device, config->channels, config->rate, config->period_size,
              g.s.aec, g.s.aec_level, g.s.ns, g.s.ns_level, g.s.hpf, g.s.delay_ms,
              g.s.extended, g.s.agnostic, g.s.mics, g.s.mute);
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
