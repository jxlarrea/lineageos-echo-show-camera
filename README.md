# Working camera for LineageOS on the Amazon Echo Show 5 (2nd gen)

This repository makes the front camera fully functional on unofficial
LineageOS 18.1 for the Amazon Echo Show 5 2nd gen (`cronos`, MT8163). The
stock ROM port ships no camera stack at all; with this work applied, the
device gets:

| Feature | Status |
| --- | --- |
| Camera enumeration (front, correct orientation) | working |
| Live preview with correct colors | working |
| Still capture (`takePicture`, camera2/CameraX) | working, full 1600x1200 JPEG |
| Video mode sensor timing | working |
| Lens shading correction (factory calibration) | working |
| Auto exposure | working |
| Adaptive auto white balance | working, with a calibrated correction |
| Black level | corrected (the stock tuning left a 30% grey floor) |

Everything here was reverse-engineered against a live device; the complete
investigation, including every dead end, is in
[docs/findings.md](docs/findings.md). It is long, but if you maintain a
MediaTek device port it is probably the most useful file in the repository.

## Why this was hard, in one paragraph

The Echo Show 5 ships an Android 7.1 (API 25) camera stack on an Android 9
era kernel, and this LineageOS port runs Android 11 userspace on that
kernel. Nothing agrees with anything: the blobs need symbols Android 11
removed, an older display framework than the ROM ships, a cmdq event
encoding the kernel no longer speaks, and Amazon's kernel struct layouts
rather than upstream MediaTek's. On top of that, the ROM's kernel selected
the 1st gen camera sensor (OV9734) while the 2nd gen device has an OV02B10,
so a sensor driver had to be ported, and all of MediaTek's image tuning
(white balance, black level) is for the wrong sensor. Each of those is
fixed here, layer by layer.

## Supported devices

| Device | Codename | Sensor | Status |
| --- | --- | --- | --- |
| Echo Show 5 (2nd gen, 2021) | `cronos` | OV02B10 | fully working, verified |
| Echo Show 5 (1st gen) | `checkers` | OV9734 | untested; kernel struct patch applies, no driver port needed |
| Echo Show 8 (1st gen) | `crown` | OV9734 | untested; same as checkers |
| Echo Show (2nd gen) | `rook` | GC0312 | untested |

Everything below is written for `cronos`. For the OV9734 devices, skip the
sensor driver patches (0004, 0007, 0008, 0010) and expect to redo the
color calibration.

## Prerequisites

- An Echo Show 5 2nd gen already running unofficial LineageOS 18.1 from
  [amazon-oss](https://github.com/amazon-oss) (R0rt1z2's port),
  amonet-unlocked, with TWRP intact. If you cannot boot TWRP, stop: some of
  these steps can brick a device that has no recovery path.
- A Linux host with `adb`, network adb access to the device, and `python3`.
- A LineageOS 18.1 build environment for `cronos` (needed to build the
  patched boot image and the patched system libraries). Set up the source
  tree per the amazon-oss instructions; `patches/local-manifest-fixes.xml`
  contains the manifest fixes this work needed.

## Installation overview

There are three layers. They must all be applied; each fixes failures the
next layer would otherwise hit.

1. **Kernel** (boot image): Amazon struct layouts, the OV02B10 driver,
   sensor timing fixes. Requires building and flashing a boot image.
2. **ROM system libraries** (device tree + AOSP patches): camera provider
   declaration, front-camera feature declaration, sensor orientation,
   two AOSP camera fixes. Requires building the ROM (or at minimum the
   affected libraries) with the patches applied.
3. **On-device userspace**: the proprietary camera blobs (fetched from a
   public firmware dump, never shipped here), the compatibility shims, the
   patched private display framework, and the tuning corrections. All
   applied by scripts over adb.

### Layer 1: kernel

Apply to `kernel/amazon/mt8163-4.9`:

```
patches/0001-imgsensor-use-the-Amazon-struct-layouts-on-all-echo-show.patch
patches/0004-imgsensor-ov02b10-driver-support.patch
patches/0007-imgsensor-ov02b10-mirror-flip-180-rotation.patch
patches/0008-imgsensor-ov02b10-standby-before-mode-rewrite.patch
patches/0010-imgsensor-ov02b10-restore-capture-and-video-mode-timing.patch
```

(0002 is an alternative to 0001 and must NOT be applied together with it;
0003 is debug logging only. See [patches/README.md](patches/README.md).)

Set the sensor in `cronos_defconfig`:

```
CONFIG_CUSTOM_KERNEL_IMGSENSOR="ov02b10_mipi_raw"
```

Build the boot image (`mka bootimage`), then flash it with
`scripts/flash-boot.sh <adb-serial>`.

**Do not `dd` a plain boot image to the boot partition.** On
amonet-unlocked devices the partition begins with the exploit header and a
relocated copy of the boot header; writing a plain image, or writing it at
a guessed offset, produces a hang at the vendor logo that looks exactly
like a bad kernel. Two devices were bricked learning this. The flash
script implements the correct layout, backs up the partition first, and
verifies byte for byte; it also has a `--self-test`.

### Layer 2: ROM changes

Follow [patches/README.md](patches/README.md) for the device tree changes:
the passthrough camera provider declaration, the corrected vintf manifest,
the blob list for `proprietary-files.txt`, and the required permissions.
Additionally apply:

```
patches/0005-camera-device-1.0-cookie-fallback-and-ANativeWindowBuffer-preview.patch
patches/0006-camera-flatten-tolerate-zero-size-String8-from-legacy-HALs.patch
patches/0009-camera-front-camera-feature-and-sensor-orientation.patch
```

0005 and 0006 patch AOSP camera code the tree builds
(`camera.device@1.0-impl`, `libcamera_client`); 0009 fixes the device
advertising a back camera it does not have (which hangs every CameraX app
on the device) and the sensor orientation.

### Layer 3: on-device userspace

With the patched ROM booted and adb root available:

```sh
scripts/fetch-camera-blobs.sh              # stock camera blobs from the public dump
scripts/patch-shim-needed.sh               # link the compatibility shim into the blobs
scripts/install-private-dpframework.sh     # API 25 display framework, private soname, 3 binary patches
scripts/install-camera.sh                  # push everything to the device
scripts/install-cmdq-event-shim.sh <serial>  # cmdq event translation + AWB correction shim
scripts/patch-awb-d65.sh <serial>          # neutralize the double white balance
scripts/patch-obc-pedestal.sh <serial>     # correct the black level for the OV02B10
adb -s <serial> reboot
```

The blobs come from the public
[amazon_cronos_dump](https://github.com/el-vertedero/amazon_cronos_dump)
firmware dump. This repository contains no proprietary code; the scripts
fetch, patch, and install it on your own device.

## Verify

From a cold boot, with no manual steps:

```sh
adb shell 'pm list features | grep camera'   # camera.any + camera.front, NO plain "camera"
adb shell 'dumpsys media.camera | grep Orientation'   # 0
adb shell 'logcat -d | grep -cE "startStream fail|deque DISPO fail"'   # 0
```

Open the camera app: live preview, then take a photo - it should produce a
full resolution 1600x1200 JPEG in about a second. In a fully dark room a
photo should be essentially black (that is the black-level fix; without it
you get a grey-cyan haze).

## Color calibration

The MediaTek tuning in the blobs is for the OV9734 sensor, not the OV02B10,
so adaptive white balance lands with an illuminant-dependent bias. The shim
corrects it by rescaling the AWB algorithm's output, interpolated between
two calibrated anchors (daylight and warm LED), keyed on the algorithm's
own blue gain. Four 512-based properties control it, re-read every 64
frames so tuning needs no restart:

```
persist.camera.awbtrim.r        cool (daylight) anchor, red
persist.camera.awbtrim.b        cool anchor, blue
persist.camera.awbtrim.r.warm   warm (2600K) anchor, red
persist.camera.awbtrim.b.warm   warm anchor, blue
```

Defaults are in `shims/libcmdqevent/camera-bringup.rc`, calibrated against
a grey surface on one device. To touch up for your unit: point the camera
at anything grey or white, take snapshots, and adjust in steps of 2 or 3
(the color matrix amplifies these values roughly 5x in the image, so small
steps):

- image too magenta: lower `.r` (or `.r.warm` under warm light)
- image too green: raise it
- same logic for blue with `.b` / `.b.warm`

## Known limitations

- **Scene-to-scene color variance of roughly +-10%.** The blob's AWB output
  wobbles a few percent between sessions on identical scenes and the color
  matrix amplifies it. Inherent to the closed algorithm.
- **Dim mixed lighting can render greenish.** When the AWB classifier finds
  no recognizable illuminant it falls back to its reference gains, which
  bypass part of the correction. A handler for this state is planned.
- **Daytime color gradient on aged units.** Strong daylight through the
  front glass produces a cyan-to-magenta gradient across the frame (IR
  leakage through the aged IR-cut filter plus internal veiling glare). It
  disappears under uniform artificial light. No global correction can fix
  it, and the front glass cannot be cleaned from outside.
- **Single client.** The legacy HAL1 stack allows one camera client at a
  time.

## Safety rules

Learned the hard way; both of these can take the device down completely.

1. **Never SIGKILL or `stop` cameraserver while the ISP is streaming.** It
   leaves the memory management unit pointed at freed buffers and the
   resulting bus violation storm livelocks the entire device - no adb, no
   ping, no watchdog. Use `adb reboot`, or `am force-stop` the camera app
   first and give it two seconds.
2. **Only flash boot images with `scripts/flash-boot.sh`** (see the amonet
   partition layout warning above).

## Repository contents

| Path | What it is |
| --- | --- |
| [docs/findings.md](docs/findings.md) | The full investigation, in order, with every dead end kept and marked |
| [docs/handoff-takepicture.md](docs/handoff-takepicture.md) | Worked example of debugging one bug end to end (the capture stall) |
| [patches/](patches/) | Kernel and AOSP patches, numbered in application order |
| [shims/libcamera_shim/](shims/libcamera_shim/) | Source shim closing the 11-symbol gap between the API 25 blobs and Android 11 |
| [shims/libcmdqevent/](shims/libcmdqevent/) | LD_PRELOAD shim: cmdq event-id translation, AWB output correction, diagnostic tracers |
| [scripts/](scripts/) | Fetching, patching, installing, flashing, calibration |
| [tools/cmdq-trace/](tools/cmdq-trace/) | On-device diagnostic tools (ioctl tracers, ISP register/IRQ probes, sensor register poke) |

## Credit

The `crown` diagnosis by CesarAmores
([writeup](https://github.com/CesarAmores/echo-show-crown-camera-diagnosis))
mapped the HAL1 shim crash chain and established that the stock blobs are
2017-era camera1 code. R0rt1z2 maintains the LineageOS ports these devices
run ([amazon-oss](https://github.com/amazon-oss)) and documented the
original kernel/blob mismatch in
[releases#5](https://github.com/amazon-oss/releases/issues/5). The stock
firmware dump is maintained at
[el-vertedero/amazon_cronos_dump](https://github.com/el-vertedero/amazon_cronos_dump).

## License

Original code (shims, scripts, tools) is MIT. Kernel patches are GPL-2.0;
AOSP patches are Apache-2.0. See [LICENSE](LICENSE). No proprietary
binaries are distributed in this repository.
