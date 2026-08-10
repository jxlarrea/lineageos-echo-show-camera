#!/usr/bin/env bash
#
# Download the stock cronos camera stack into stock/lib.
#
# The Echo Show devices never received a Treble camera HAL, so the only source
# of OV9734 tuning is Amazon's own image. This pulls it from a public firmware
# dump rather than requiring a device backup.
#
set -euo pipefail

# Blobs must come from the device's own firmware; see device-config.sh.
# CAMERA_DEVICE selects it, and a positional argument overrides that so
# `fetch-camera-blobs.sh crown` keeps working.
[[ -n "${1:-}" ]] && CAMERA_DEVICE="$1"
. "$(dirname "$0")/device-config.sh"
echo "fetching $CAMERA_DEVICE camera blobs from $DUMP_REPO"
RAW="$DUMP_RAW"

OUT="stock/lib"
mkdir -p "$OUT"

# stock/lib is a cache keyed by device, and the loop below skips any library
# already present. Without this stamp, fetching for a second device silently
# keeps the first device's blobs: the build succeeds, the camera works, and
# the colour is wrong in a way no calibration can fix - which is the exact
# failure the per-device fetch exists to prevent. Found while bringing up a
# crown on a host that still had cronos blobs cached.
STAMP="$OUT/.device"
if [[ -f "$STAMP" ]]; then
    cached="$(cat "$STAMP")"
    if [[ "$cached" != "$CAMERA_DEVICE" ]]; then
        echo "stock/lib holds $cached blobs, but CAMERA_DEVICE is $CAMERA_DEVICE."
        echo "clearing the cache so it is refetched for $CAMERA_DEVICE"
        rm -f "$OUT"/*.so "$STAMP"
    fi
elif compgen -G "$OUT/*.so" >/dev/null; then
    # Cache predates the stamp; provenance unknown, so it cannot be trusted.
    echo "stock/lib has blobs from an unknown device, clearing to be safe"
    rm -f "$OUT"/*.so
fi

# The camera closure: everything matching libcam*/libmtkcam*/camera.mt8163,
# plus the transitive MediaTek dependencies those pull in. Listing the
# dependencies explicitly keeps this reproducible without a resolver.
LIBS=(
    libcam.camadapter libcam.campipe libcam.camshot libcam.client
    libcam.device1 libcam.device3 libcam.exif libcam.exif.v3
    libcam.hal3a.v3 libcam.hal3a.v3.dng libcam.halsensor libcam.iopipe
    libcam.iopipe_FrmB libcam.metadata libcam.metadataprovider
    libcam.paramsmgr libcam.utils libcam.utils.sensorlistener
    libcam1_utils libcam3_app libcam3_hwnode libcam3_hwpipeline
    libcam3_pipeline libcam3_utils libcam_hwutils libcam_mmp
    libcam_platform libcam_utils libcamalgo libcamdrv libcamdrv_FrmB
    libcameracustom libmtkcam_fwkutils
    lib3a libfeatureio libfeatureiodrv libhal_effects libhdrproc
    libimageio libimageio_FrmB libimageio_plat_drv libimageio_plat_drv_FrmB
    libmtk_mmutils libmtkjpeg
)

for lib in "${LIBS[@]}"; do
    if [[ -f "$OUT/$lib.so" ]]; then
        continue
    fi
    echo "fetching $lib.so"
    # Almost everything is under system/vendor/lib in the stock (pre-Treble)
    # layout, but a few - libhal_effects.so - sit in system/lib. Try both
    # rather than hardcoding the exception. The first attempt's 404 is the
    # normal path for those, so its error output is discarded; only failing
    # both locations is worth reporting.
    curl -fsSL -o "$OUT/$lib.so" "$RAW/system/vendor/lib/$lib.so" 2>/dev/null ||
        curl -fsSL -o "$OUT/$lib.so" "$RAW/system/lib/$lib.so" 2>/dev/null || {
            rm -f "$OUT/$lib.so"
            echo "could not fetch $lib.so from either system/vendor/lib or" >&2
            echo "system/lib in $DUMP_REPO" >&2
            exit 1
        }
done

# The HAL1 module itself lives under hw/.
if [[ ! -f "$OUT/camera.mt8163.so" ]]; then
    echo "fetching camera.mt8163.so"
    curl -fsSL -o "$OUT/camera.mt8163.so" "$RAW/system/vendor/lib/hw/camera.mt8163.so"
fi

echo "$CAMERA_DEVICE" > "$OUT/.device"

echo
echo "$(ls "$OUT"/*.so | wc -l) libraries in $OUT ($(du -sh "$OUT" | cut -f1)) for $CAMERA_DEVICE"
