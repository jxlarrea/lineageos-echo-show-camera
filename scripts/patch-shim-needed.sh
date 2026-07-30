#!/usr/bin/env bash
#
# Add libcamera_shim.so to the DT_NEEDED list of the camera blobs that import
# symbols it provides.
#
# This tree's extract_utils has no shim mechanism, and the blobs arrive as plain
# PRODUCT_COPY_FILES with no Soong module, so there is no shared_libs entry to
# declare. Without this the shim is built and installed but never loaded, and
# the blobs fail to link exactly as they do untouched.
#
# Runs against the vendor tree so the change is part of the build rather than
# something applied to a device by hand. Idempotent.
#
set -euo pipefail

TREE="${1:-/mnt/dev/lineage-18.1}"
BLOBS="$TREE/vendor/amazon/cronos/proprietary/vendor/lib"
PATCHELF="$TREE/prebuilts/extract-tools/linux-x86/bin/patchelf-0_9"
SHIM="libcamera_shim.so"

if [[ ! -x "$PATCHELF" ]]; then
    echo "patchelf not found at $PATCHELF" >&2
    exit 1
fi

# The blobs that reference symbols the shim provides. Derived from syms.py:
# CallStack, GraphicBuffer/GraphicBufferMapper and the DpIspStream/DpBlitStream
# forwarders.
TARGETS=(
    lib3a.so
    libcam.camadapter.so
    libcam.campipe.so
    libcam.camshot.so
    libcam.client.so
    libcam.device1.so
    libcam.exif.so
    libcam.exif.v3.so
    libcam.hal3a.v3.so
    libcam.iopipe.so
    libcam.paramsmgr.so
    libcam.utils.so
    libcam.utils.sensorlistener.so
    libcam1_utils.so
    libcam3_app.so
    libcam3_hwpipeline.so
    libcam3_utils.so
    libcam_utils.so
    libcamalgo.so
    libcameracustom.so
    libfeatureio.so
    libfeatureiodrv.so
    libhal_effects.so
    libimageio_FrmB.so
    libimageio_plat_drv.so
    libimageio_plat_drv_FrmB.so
    libmtkcam_fwkutils.so
    libmtkjpeg.so
)

# libcam.utils.sensorlistener.so is a separate case. It imports
# android::SensorManager and android::SensorEventQueue and lists libgui.so,
# which is where those lived on Android 7. They moved to libsensor.so in
# Android 8 with unchanged signatures, so naming that library is enough - no
# shim required. Everything that fails to load transitively (libcam.client,
# libcam.camadapter) does so through this one blob.
if ! "$PATCHELF" --print-needed "$BLOBS/libcam.utils.sensorlistener.so" 2>/dev/null |
        grep -qx "libsensor.so"; then
    "$PATCHELF" --add-needed libsensor.so "$BLOBS/libcam.utils.sensorlistener.so"
    echo "added libsensor.so to libcam.utils.sensorlistener.so"
fi

patched=0
skipped=0
for lib in "${TARGETS[@]}"; do
    path="$BLOBS/$lib"
    if [[ ! -f "$path" ]]; then
        echo "missing: $lib" >&2
        exit 1
    fi
    if "$PATCHELF" --print-needed "$path" 2>/dev/null | grep -qx "$SHIM"; then
        skipped=$((skipped + 1))
        continue
    fi
    "$PATCHELF" --add-needed "$SHIM" "$path"
    patched=$((patched + 1))
done

echo "patched $patched, already done $skipped, of ${#TARGETS[@]} blobs"
