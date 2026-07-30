#!/usr/bin/env bash
#
# Download the stock cronos camera stack into stock/lib.
#
# The Echo Show devices never received a Treble camera HAL, so the only source
# of OV9734 tuning is Amazon's own image. This pulls it from a public firmware
# dump rather than requiring a device backup.
#
set -euo pipefail

REPO="el-vertedero/amazon_cronos_dump"
BRANCH="cronos-user-6.0-NS6573-6567-amz-p,release-keys"
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
    curl -fsSL -o "$OUT/$lib.so" "$RAW/system/vendor/lib/$lib.so"
done

# The HAL1 module itself lives under hw/.
if [[ ! -f "$OUT/camera.mt8163.so" ]]; then
    echo "fetching camera.mt8163.so"
    curl -fsSL -o "$OUT/camera.mt8163.so" "$RAW/system/vendor/lib/hw/camera.mt8163.so"
fi

echo
echo "$(ls "$OUT" | wc -l) libraries in $OUT ($(du -sh "$OUT" | cut -f1))"
