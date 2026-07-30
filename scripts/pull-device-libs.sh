#!/usr/bin/env bash
#
# Pull the platform libraries the stock camera blobs link against off a running
# device, so the symbol analysis compares against what this ROM really ships
# rather than against a generic AOSP build.
#
set -euo pipefail

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial>   e.g. $0 <device-ip>:5555" >&2
    exit 2
fi

OUT="devlib"
mkdir -p "$OUT"

LIBS=(
    libEGL libGLESv2 libJpgEncPipe libandroid libbase libbinder libbwc
    libc libc++ libcamera_client libcamera_metadata libcutils libdl
    libdpframework libgralloc_extra libgui libhardware libhidlbase libion
    libion_mtk liblog libm libm4u libmedia libmtk_drvb libnativewindow
    libnvram libnvramagentclient libsensor libsensorndkbridge libsync
    libui libutils
)

# Vendor libraries and VNDK copies take precedence in the vendor namespace, so
# search those first and stop at the first hit.
DIRS=(/vendor/lib /system/lib/vndk-30 /system/lib/vndk-sp-30 /system/lib)

for lib in "${LIBS[@]}"; do
    for dir in "${DIRS[@]}"; do
        if adb -s "$DEVICE" pull -a "$dir/$lib.so" "$OUT/$lib.so" >/dev/null 2>&1; then
            echo "$lib.so <- $dir"
            break
        fi
    done
done

echo
echo "$(ls "$OUT" | wc -l) libraries in $OUT"
