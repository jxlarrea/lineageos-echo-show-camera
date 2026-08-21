# Device tree changes for camera support on cronos

> **These changes are applied for you by
> `0011-device-tree-camera-enablement.patch`** (except section 3, the blob
> list, which `docs/INSTALL.md` step 6 covers, and section 4, sepolicy).
> This file explains *why* each one is needed - follow
> [docs/INSTALL.md](../docs/INSTALL.md) to actually install.


Three separate problems have to be fixed together. Applying only some of them
leaves the camera enumerating zero devices exactly as it does today.

## 1. The declared camera provider does not exist

`device/amazon/mt8163-common/manifest.xml` currently declares:

```xml
<hal format="hidl">
    <name>android.hardware.camera.provider</name>
    <transport>hwbinder</transport>
    <fqname>@2.4::ICameraProvider/internal/0</fqname>
</hal>
```

`internal/0` over hwbinder is MediaTek's own provider, served by the
`camerahalserver` binary. That binary exists in the karnak (Fire HD 8 2018) blob
set but Amazon never shipped it on the Echo Show devices, and nothing in this ROM
provides it. The camera service looks the provider up, finds nothing, and reports
no cameras, which is the behaviour observed on device.

The stock Echo Show blobs are a legacy camera1 HAL module
(`camera.mt8163.so`), so the matching provider is AOSP's passthrough legacy one:

```xml
<hal format="hidl">
    <name>android.hardware.camera.provider</name>
    <transport arch="32+64">passthrough</transport>
    <version>2.4</version>
    <interface>
        <name>ICameraProvider</name>
        <instance>legacy/0</instance>
    </interface>
</hal>
```

## 2. The provider implementation is never built

`device/amazon/mt8163-common/mt8163.mk` builds the two device shims but no
provider:

```make
# Camera
PRODUCT_PACKAGES += \
    camera.device@1.0-impl \
    camera.device@3.2-impl \
    libsensorndkbridge
```

It needs the passthrough provider and the shim:

```make
# Camera
PRODUCT_PACKAGES += \
    android.hardware.camera.provider@2.4-impl \
    camera.device@1.0-impl \
    camera.device@3.2-impl \
    libcamera_shim \
    libsensorndkbridge
```

`android.hardware.camera.provider@2.4-impl` pulls in
`android.hardware.camera.provider@2.4-legacy`
(`LegacyCameraProviderImpl_2_4.cpp`), which is what loads `camera.mt8163.so`
through libhardware.

Add to `device/amazon/<device>/BoardConfig.mk`:

```make
# Camera
TARGET_NEEDS_LEGACY_CAMERA_HAL1_DYN_NATIVE_HANDLE := true
```

## 3. The blobs are not extracted

The stock camera stack is absent from
`vendor/amazon/<device>`. Append the contents of
`camera-proprietary-files.txt` to
`device/amazon/<device>/proprietary-files.txt`, then re-run `extract-files.sh`
against a stock image (see `scripts/fetch-camera-blobs.sh`). The paths in that
list are the same on all three devices; the blobs behind them are not, and must
come from the device's own firmware (see `scripts/device-config.sh`).

45 libraries are required: the 34 `libcam*`/`libmtkcam*` libraries plus 11
transitive MediaTek dependencies (`lib3a`, `libfeatureio`, `libfeatureiodrv`,
`libhal_effects`, `libhdrproc`, `libimageio*`, `libmtk_mmutils`, `libmtkjpeg`).
Everything else the blobs need is already present on the device.

Note the stock layout is pre-Treble: the blobs live in `/system/vendor/lib` and
name `/system/bin/linker` as their interpreter. The list rewrites them into
`/vendor/lib`; `camera.mt8163.so` goes to `/vendor/lib/hw/`.

## 4. sepolicy

`cameraserver` needs access to the MediaTek camera nodes, which are already
present on the device (`/dev/camera-isp`, `/dev/camera-pipemgr`,
`/dev/camera-sysram`, `/dev/kd_camera_hw`, `/dev/kd_camera_hw_bus2`). Expect to
label those and grant `cameraserver` and the passthrough provider access to
them, plus the usual ion / m4u permissions. Start with permissive logging to
collect the denials rather than guessing the rules.
