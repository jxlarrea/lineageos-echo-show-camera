# Acoustic echo cancellation for the Echo Show microphones on LineageOS 18.1

This describes a working software echo canceller for the Amazon Echo Show 8 (`crown`) on the unofficial LineageOS 18.1 port, measured on two units running `lineage-18.1-crown-v0.5`. It should apply unchanged to the Echo Show 5 (`checkers`, `cronos`), which use the same microphone front end with a 4 channel variant of the stream. Source, build script and install script: `shims/libamznaec/` and `scripts/install-amznaec-shim.sh` in this repository.

## The problem

Every capture on these devices hears the speaker at full strength. Anything that listens while the device plays, a voice assistant listening for a stop word during TTS, a wake word engine during music, a call, gets the playback mixed into the microphone signal. Android reports no echo canceller: `AcousticEchoCanceler.isAvailable()` is false because `/vendor/etc/audio_effects.xml` is the stock AOSP template with the `pre_processing` library, the `aec`, `ns` and `agc` effects and the `voice_communication` preprocess block all inside an XML comment, even though `/vendor/lib/soundfx/libaudiopreprocessing.so` ships in the image.

Uncommenting that block is not a fix. Android 11's AudioFlinger never feeds a playback reference to the software AEC effect: there is no caller of the effect's `processReverse` entry point anywhere in `services/audioflinger`. The WebRTC canceller inside `libaudiopreprocessing.so` would run with an empty far end and cancel nothing. Noise suppression and AGC from that block work, the AEC only advertises itself. Apps that trust an advertised canceller then behave worse than with none.

## What the hardware provides

The microphones do not reach the SoC directly. They go through the TLV320AIC3101 codec pair and an FPGA ("dough" in the kernel sources, firmware `i2s_to_spi_6ch_v183.bin` embedded in the kernel through `CONFIG_EXTRA_FIRMWARE` and programmed over SPI at probe by `amzn_mt_spi_probe`). The FPGA delivers one PCM to the kernel, `TLV320AIC3101 Capture` (`pcmC0D22c`): 6 channels, `S24_3LE`, 16 kHz, period 257 frames.

Channels 0 to 3 are the four microphones. Channels 4 and 5 are a loopback of the DAC output, in the same frame as the microphone samples. Measured on `crown` with a 4 s raw capture while the speaker played: channels 4 and 5 carried the playback at about -25 dBFS and dropped to exactly zero the instant playback stopped (`dough.h` calls this `dac_inactive`). The microphones heard the speaker at about -18 dBFS against a quiet-room floor of -52 dBFS. That loopback is a sample aligned echo reference, the best possible input for a canceller, and nothing in the ROM uses it.

Amazon's audio HAL blob (`audio.primary_amazon.mt8163.so`, wrapped by `audio.primary.amazon_wrapper.so` from `hardware/amazon`) opens that PCM at HAL start, keeps it open for the life of the process, reads it with tinyalsa's `pcm_read`, and keeps a single channel, channel 0, which it converts to the 16 kHz mono 16 bit stream Android sees. The kernel commit "amzn-spi-pcm: Mix both mics for mono capture" already documents that the blob reads one channel.

## The fix

`libamznaec_shim.so` is a small vendor library preloaded into `android.hardware.audio.service`. It interposes tinyalsa's `pcm_open`, `pcm_read` and `pcm_close`. When the blob opens a capture PCM with the FPGA stream's shape (6 channels, `S24_3LE`, 16 kHz), the shim remembers the handle. On every `pcm_read` of that handle it runs WebRTC's echo canceller on microphone channel 0, using the average of channels 4 and 5 as the far end, and writes the result back into the same buffer before the blob sees it. The blob's own channel selection, gain and format conversion are untouched, so nothing else in the audio path changes. Processing happens in 10 ms blocks, which adds one block, 10 ms, of capture latency.

The canceller is the `libwebrtc_audio_preprocessing.so` the ROM already ships in `/vendor/lib` (it is a dependency of the unused `libaudiopreprocessing.so`), so the shim adds no new code to the image beyond itself. It enables `EchoCancellation` (low suppression, extended filter), `NoiseSuppression` (low) and the high pass filter; every option is a `persist.vendor.amznaec.*` property read when the PCM is opened, including a master switch that turns the shim into a pass through. Because the far end is hardware aligned, the plain WebRTC filter with a reported delay of 0 converges; the costly extended filter and delay agnostic modes are available as properties but off.

Symbol resolution needs one care point: the blob reaches `libtinyalsa.so` through `hw_get_module`'s `dlopen`, a local group that `RTLD_NEXT` does not walk, so the shim resolves the real functions with `dlopen("libtinyalsa.so", RTLD_NOLOAD)` and `dlsym` on that handle. The first build used `RTLD_NEXT` and crash looped the HAL.

## Measured results on crown

With a 1 kHz tone played by an ordinary app at full media volume, the shim's own 5 s level log (`persist.vendor.amznaec.log=1`) reported, per window: far end -9.7 dBFS, microphone in -3.4 dBFS, microphone out -41.9 dBFS, so 38 to 44 dB of echo removed with the plain filter, and 36 to 50 dB with the extended filter. With nothing playing, a -45 dBFS room floor comes out at -58 dBFS from the noise suppressor and the high pass filter. Echo removal was confirmed at the Android side through AudioFlinger's per stream power log on the capture thread a voice app was using.

CPU cost inside the HAL process, measured with `top`, on one core of the MT8163: pass through under 3 percent, plain filter on channel 0 with noise suppression and high pass filter about 13 percent, the same on all four channels about 30 percent, and all four channels with the extended filter and delay agnostic modes about 60 percent.

## Why not the audio_effects.xml patch

[amazon-oss/android_device_amazon_mt8163-common#2](https://github.com/amazon-oss/android_device_amazon_mt8163-common/issues/2) proposes uncommenting the AOSP `pre_processing` library, the `aec`/`ns`/`agc` effects and the `voice_communication` mapping. That patch does make the effects attach: the Amazon blob's `AudioPreProcess` module logs `addAudioEffect()` for `Acoustic Echo Canceler` and `Noise Suppression`, calls `start_echo_reference()` with a 2 channel 16 kHz reference and `in_configure_reverse()` returns 0. It does not cancel anything measurable. Measured on crown with that patch applied (plus a `mic` mapping so a recorder app gets the same chain), the shim disabled and a -30 dBFS 1 kHz tone played by an app so nothing clips: the recorder captured the tone at -7.7 dBFS with no processing at all, -7.7 dBFS with the patch, and -37.7 dBFS averaged over 3 s with the shim (-69 dBFS once the filter had converged after a quarter second). The noise floor between tones was -47 dBFS unprocessed, -47 dBFS with the patch and -70 dBFS with the shim, so the noise suppressor in that chain is not measurably active either.

Two structural reasons make this expected. The AOSP effect uses WebRTC's mobile canceller, `EchoControlMobile` in earpiece mode, a suppression based design that needs an accurately aligned reference and gives at best partial attenuation; the shim uses the full linear canceller. And the reference the blob provides comes from MediaTek's own capture path (`AudioALSACaptureDataProviderEchoRef`, the codec loopback), which was designed for the SoC's internal codec and has no fixed timing relationship with the FPGA microphone stream and its 160 ms of SPI buffering, whereas channels 4 and 5 of that very stream are the DAC output aligned to the sample. Advertising an echo canceller that does not cancel is worse than none: applications that find one, Kiosk Satellite included, stop applying their own mitigations.

## Double talk: the wake word problem

A clean recording and a usable wake word are different targets. With the canceller at high suppression the RTSP stream from the device is free of music at any volume, yet wake word detection collapses while music plays. The cause is the canceller's nonlinear stage: WebRTC follows its linear filter with a suppressor that attenuates whatever remains, and during double talk that includes the talker.

This was measured on identical audio through the real pipeline. The shim has two debug properties for that: `persist.vendor.amznaec.dump` writes the frames entering and leaving the processing to files, and `persist.vendor.amznaec.replay` feeds a raw 6 channel file in place of the microphones. A 34 s test stream was spliced from a dump of loud music on the device (mics and loopback channels, music at -7 dBFS at the mic) and a raw take of a person 1 m in front: 10 s of music alone, 12 s of music plus the speech, 12 s of the same speech alone. Speech level was then compared between the double talk and speech only sections over the same speech windows.

| Setting | Speech loss during music | Residual echo, music only | Residual in speech gaps |
| --- | --- | --- | --- |
| WebRTC high suppression, NS moderate (initial default) | 19.4 dB | -57 dBFS | -60 dBFS |
| WebRTC moderate suppression | 14.4 dB | -51 dBFS | -60 dBFS |
| WebRTC low suppression, NS low | 10.2 dB | -47 dBFS | -55 dBFS |
| WebRTC low suppression, NS low, extended filter (new default) | 9.5 dB, even across bands | -44 dBFS | -51 dBFS |
| WebRTC low suppression, no NS | 9.9 dB | -40 dBFS | -46 dBFS |
| Speex MDF, linear only (`engine=1`) | none | -23 dBFS | -23 dBFS |

WebRTC's suppressor cannot be switched off, only lowered. A purely linear canceller keeps the talker intact but is bounded by physics here: an offline least squares fit on the music section, with the stereo loopback and up to 2048 taps, removes at most 21 to 24 dB, so roughly a hundredth of what the mic hears at that volume is not predictable from the loopback. That is speaker and amplifier distortion, it grows with volume, and it is why lower volume helps the wake word so much. Speex (vendored under `speexdsp/`, selectable with `persist.vendor.amznaec.engine=1`, stereo loopback reference, headroom scaling because it stops adapting near full scale) lands at 15 dB and leaves the residual louder than the talker; it stays as an option for moderate playback levels where distortion is small.

The default is therefore low suppression with the extended filter and low noise suppression: the talker keeps about 90 percent of the energy, uniformly across bands so a gain setting in the application compensates, against a residual 10 to 17 dB below the speech. For a recording where playback must vanish completely, set `persist.vendor.amznaec.aec_level=2` and `persist.vendor.amznaec.ns_level=1` and restart the audio HAL.

## The analog mic gain clips at loud playback

The port's crown device tree raises the TLV320AIC3101 microphone PGA from Amazon's register value 40 (+20 dB) to 80 (+40 dB) in `etc/audio_device.xml` (commit "crown: Boost built-in mic capture gain"), because capture was too quiet past a foot. At loud playback that gain saturates the ADC: in a dump taken while music played at a level the mic heard at -7 dBFS average, 24 percent of all samples on all four microphones sat at full scale. A clipped echo is nonlinear by definition, no canceller can remove it, and any voice on top of it is destroyed before processing starts. This, not the canceller, is why a wake word never triggers at high volume on the stock configuration, and why lowering the volume helps so abruptly.

The fix keeps the loudness Android sees but moves the boost after the canceller: the installer sets the two `MICPGA Volume Ctrl` values in `/system/etc/audio_device.xml` back to 40 (the HAL re-applies that file at every start, so a `tinymix` change alone does not survive) and sets `persist.vendor.amznaec.gain_db=20`, a digital gain the shim applies to the processed channel after cancellation. Measured after the change: at a playback level the mic hears at -25 dBFS average (peaks -6.8 dBFS) not one sample clips, cancellation holds at 33 dB, and a person talking over the music reaches the application 10 to 17 dB above the residual. At moderate level (-33 dBFS at the mic) the margin is 20 dB across the whole band.

What remains at the very loudest level is in the speaker, not in the software: the residual there is concentrated above 1.5 kHz, where the talker's margin drops to about 3 dB, while below 1.5 kHz it stays at 16 to 17 dB. That is harmonic distortion from the driver and amplifier, which no reference derived from the DAC signal can predict; nonlinear references (cubic, absolute value, saturation) added to an offline fit removed nothing further, and the suppression level does not change the margin, only the absolute level. For the port: revert the PGA to 40 and add the boost digitally wherever capture is processed, or at least document that +40 dB is unusable with playback.

## The microphone array

The Echo Show 8 has four microphones under two ports on the top edge, about 2.54 cm apart, and the FPGA stream carries all four; the blob keeps only channel 0. The shim can process all four (`persist.vendor.amznaec.array`: 0 processes each selected mic on its own, 1 averages the selected mics into channel 0, 2 runs WebRTC's nonlinear beamformer over them with a geometry from `persist.vendor.amznaec.geom` and a target azimuth from `persist.vendor.amznaec.target`). Measured on crown, none of it helps.

Averaging is worthless: on identical raw captures a two mic or four mic average improves the speech to noise ratio by 0.1 to 0.3 dB and loses about 1 dB above 3 kHz, because room noise is already coherent across mics this close. The beamformer runs at 24 to 31 percent of one core without overruns and keeps echo cancellation intact, but it cannot find the talker: with the measured geometry a person 1 m in front lost 21 dB and 9 dB of SNR, and a person at the side lost 15 dB with no SNR change, every band alike including 100 to 500 Hz where a 2.5 cm baseline has no directivity. It suppresses everyone rather than steering. Arrival time analysis (GCC PHAT on raw captures, validated on synthetic shifts) puts all four mics within about 1.2 cm of each other in projection along the edge, so no algorithm can separate talkers by direction at speech frequencies from this stream. Amazon's own front end, which did (AFE.cfg with a 12 beam fixed beamformer, an adaptive beamformer and an interference canceller, engine `libasp.so`), is loaded in the HAL process but only called from the playback handler; there is no capture entry point in the blob. Single microphone plus echo cancellation and noise suppression is the right end state; the array modes remain as properties, off by default, for boards where the geometry differs.

## How to integrate it in the ROM

The shim is built inside the LineageOS tree as an ordinary vendor module (`compile_multilib: "32"`, the audio HAL is a 32 bit process) linking `libwebrtc_audio_preprocessing`, `libcutils`, `liblog` and `libdl`, with `external/webrtc` and `external/tinyalsa/include` on the include path and `-DWEBRTC_POSIX`. `shims/libamznaec/Android.bp` is the complete definition. It is loaded the same way the port already loads `libcamsensormeta_shim.so` into `camerahalserver`: a `setenv LD_PRELOAD libamznaec_shim.so` line in the audio HAL's init rc, `/vendor/etc/init/android.hardware.audio.service.rc`, directly under the `service vendor.audio-hal` line. init reads rc files at boot, so the preload takes effect from the next boot. For a device tree, add the module to `PRODUCT_PACKAGES` and carry the rc line in the same way the camera preload is carried; `patchelf --add-needed` on the blob is an alternative to the preload if the rc is inconvenient.

Once the shim is in place the `voice_communication` preprocess block in `audio_effects.xml` can stay as it is. Do not enable the AOSP `aec` effect on top: it would advertise a canceller that does nothing, and apps that check for one would stop applying their own measures.

## Verifying on a device

```
adb shell setprop persist.vendor.amznaec.log 1
adb shell 'kill -9 $(pidof android.hardware.audio.service)'   # audioserver restarts with it
adb shell logcat -d | grep amznaec
```

The `opened` line confirms the shim found the FPGA PCM and lists the active settings. While something plays, the `5s:` lines show the far end level, the microphone level before and after cancellation, and whether WebRTC currently detects echo. A pass through (`persist.vendor.amznaec.enable=0`) restarts the HAL with the audio untouched; `persist.vendor.amznaec.mics=15` processes all four channels for a port whose blob keeps a different one, and `persist.vendor.amznaec.mute` zeroes chosen channels to find out which channel a blob keeps.
