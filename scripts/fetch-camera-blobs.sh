#!/usr/bin/env bash
#
# Download the stock cronos camera stack into stock/lib.
#
# The Echo Show devices never received a Treble camera HAL, so the only source
# of OV9734 tuning is Amazon's own image. This pulls it from a public firmware
# dump rather than requiring a device backup.
#
set -euo pipefail

# Each device needs the blobs from ITS OWN firmware, because the tuning
# inside libcameracustom.so is per sensor: cronos carries ov02b10_mipi_raw,
# crown and checkers carry ov9734_mipi_raw. Using another device's blobs
# gives a working camera with visibly wrong colour, because every AWB, CCM,
# shading and black level table describes a sensor you do not have.
DEVICE="${1:-cronos}"
case "$DEVICE" in
    cronos)
        REPO="el-vertedero/amazon_cronos_dump"
        BRANCH="cronos-user-6.0-NS6573-6567-amz-p,release-keys"
        ;;
    crown)
        REPO="el-vertedero/amazon_crown_dump"
        BRANCH="crown-user-6.0-NS6565-5565-amz-p,release-keys"
        ;;
    checkers)
        REPO="el-vertedero/amazon_checkers_dump"
        BRANCH="checkers-user-6.0-NS6534-2264-amz-p,release-keys"
        ;;
    *)
        echo "usage: $0 [cronos|crown|checkers]" >&2
        exit 2
        ;;
esac
echo "fetching $DEVICE camera blobs from $REPO"
RAW="https://raw.githubusercontent.com/${REPO}/${BRANCH}"

OUT="stock/lib"
mkdir -p "$OUT"

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
    # rather than hardcoding the exception.
    curl -fsSL -o "$OUT/$lib.so" "$RAW/system/vendor/lib/$lib.so" ||
        curl -fsSL -o "$OUT/$lib.so" "$RAW/system/lib/$lib.so" || {
            rm -f "$OUT/$lib.so"
            echo "could not fetch $lib.so from the dump" >&2
            exit 1
        }
done

# The HAL1 module itself lives under hw/.
if [[ ! -f "$OUT/camera.mt8163.so" ]]; then
    echo "fetching camera.mt8163.so"
    curl -fsSL -o "$OUT/camera.mt8163.so" "$RAW/system/vendor/lib/hw/camera.mt8163.so"
fi

echo
echo "$(ls "$OUT" | wc -l) libraries in $OUT ($(du -sh "$OUT" | cut -f1))"
