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

The canceller is the `libwebrtc_audio_preprocessing.so` the ROM already ships in `/vendor/lib` (it is a dependency of the unused `libaudiopreprocessing.so`), so the shim adds no new code to the image beyond itself. It enables `EchoCancellation` (high suppression), `NoiseSuppression` (moderate) and the high pass filter; every option is a `persist.vendor.amznaec.*` property read when the PCM is opened, including a master switch that turns the shim into a pass through. Because the far end is hardware aligned, the plain WebRTC filter with a reported delay of 0 converges; the costly extended filter and delay agnostic modes are available as properties but off.

Symbol resolution needs one care point: the blob reaches `libtinyalsa.so` through `hw_get_module`'s `dlopen`, a local group that `RTLD_NEXT` does not walk, so the shim resolves the real functions with `dlopen("libtinyalsa.so", RTLD_NOLOAD)` and `dlsym` on that handle. The first build used `RTLD_NEXT` and crash looped the HAL.

## Measured results on crown

With a 1 kHz tone played by an ordinary app at full media volume, the shim's own 5 s level log (`persist.vendor.amznaec.log=1`) reported, per window: far end -9.7 dBFS, microphone in -3.4 dBFS, microphone out -41.9 dBFS, so 38 to 44 dB of echo removed with the plain filter, and 36 to 50 dB with the extended filter. With nothing playing, a -45 dBFS room floor comes out at -58 dBFS from the noise suppressor and the high pass filter. Echo removal was confirmed at the Android side through AudioFlinger's per stream power log on the capture thread a voice app was using.

CPU cost inside the HAL process, measured with `top`, on one core of the MT8163: pass through under 3 percent, plain filter on channel 0 with noise suppression and high pass filter about 13 percent, the same on all four channels about 30 percent, and all four channels with the extended filter and delay agnostic modes about 60 percent.

## Why not the audio_effects.xml patch

[amazon-oss/android_device_amazon_mt8163-common#2](https://github.com/amazon-oss/android_device_amazon_mt8163-common/issues/2) proposes uncommenting the AOSP `pre_processing` library, the `aec`/`ns`/`agc` effects and the `voice_communication` mapping. That patch does make the effects attach: the Amazon blob's `AudioPreProcess` module logs `addAudioEffect()` for `Acoustic Echo Canceler` and `Noise Suppression`, calls `start_echo_reference()` with a 2 channel 16 kHz reference and `in_configure_reverse()` returns 0. It does not cancel anything measurable. Measured on crown with that patch applied (plus a `mic` mapping so a recorder app gets the same chain), the shim disabled and a -30 dBFS 1 kHz tone played by an app so nothing clips: the recorder captured the tone at -7.7 dBFS with no processing at all, -7.7 dBFS with the patch, and -37.7 dBFS averaged over 3 s with the shim (-69 dBFS once the filter had converged after a quarter second). The noise floor between tones was -47 dBFS unprocessed, -47 dBFS with the patch and -70 dBFS with the shim, so the noise suppressor in that chain is not measurably active either.

Two structural reasons make this expected. The AOSP effect uses WebRTC's mobile canceller, `EchoControlMobile` in earpiece mode, a suppression based design that needs an accurately aligned reference and gives at best partial attenuation; the shim uses the full linear canceller. And the reference the blob provides comes from MediaTek's own capture path (`AudioALSACaptureDataProviderEchoRef`, the codec loopback), which was designed for the SoC's internal codec and has no fixed timing relationship with the FPGA microphone stream and its 160 ms of SPI buffering, whereas channels 4 and 5 of that very stream are the DAC output aligned to the sample. Advertising an echo canceller that does not cancel is worse than none: applications that find one, Kiosk Satellite included, stop applying their own mitigations.

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
