#!/usr/bin/env bash
#
# Install the camera stack onto a running device, straight from the tree.
#
# /vendor is a symlink to /system/vendor on this device, so this writes to the
# system partition. There is no A/B slot and no stock backup, so:
#   - everything additive is just new filenames, which cannot shadow anything
#   - the one existing file replaced (the vintf manifest) is backed up first,
#     next to the original and also pulled to the host
#
# Recovery path if the device will not boot: TWRP, restore
# /vendor/etc/vintf/manifest.xml from manifest.xml.orig.
#
set -euo pipefail

DEVICE="${1:-}"
TREE="${2:-${LINEAGE_TREE:-$HOME/lineage-18.1}}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial> [build-tree]" >&2
    exit 2
fi

. "$(dirname "$0")/device-config.sh"
OUT="$(device_out_dir "$TREE")"
BACKUP="backups/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$BACKUP"

adb() { command adb -s "$DEVICE" "$@"; }

echo "== checking build artifacts =="
for f in system/vendor/lib/libcamera_shim.so \
         system/vendor/lib/hw/android.hardware.camera.provider@2.4-impl.so \
         system/vendor/lib/camera.device@1.0-impl.so \
         system/vendor/etc/vintf/manifest.xml; do
    if [[ ! -f "$OUT/$f" ]]; then
        echo "missing: $OUT/$f" >&2
        exit 1
    fi
done

echo "== remounting system read-write =="
# Without root the pushes fail in ways that are easy to miss, and a camera
# stack that half-landed enumerates zero cameras with nothing in the log
# saying why (seen on issue #2). adb_wait_root polls through the adbd
# restart (and a VM's USB re-attach) and refuses to continue without root.
. "$(dirname "$0")/adb-lib.sh"
adb_wait_root "$DEVICE"
adb remount >/dev/null 2>&1 || adb shell 'mount -o rw,remount /system'
if ! adb shell 'touch /vendor/.rwtest && rm /vendor/.rwtest' >/dev/null 2>&1; then
    echo "/vendor is still read-only after remount, refusing to continue" >&2
    exit 1
fi

echo "== backing up the vintf manifest =="
adb pull /vendor/etc/vintf/manifest.xml "$BACKUP/manifest.xml" >/dev/null
adb shell '[ -f /vendor/etc/vintf/manifest.xml.orig ] || cp /vendor/etc/vintf/manifest.xml /vendor/etc/vintf/manifest.xml.orig'
echo "   host copy: $BACKUP/manifest.xml"

# A module-only build does not run the PRODUCT_COPY_FILES steps, so the blobs
# are not staged in out/. They come straight from the vendor tree instead; the
# list is the same one appended to proprietary-files.txt.
echo "== pushing camera blobs =="
BLOBS="$(device_vendor_dir "$TREE")/vendor/lib"
adb shell 'mkdir -p /vendor/lib/hw'
count=0
while read -r rel; do
    src="$(device_vendor_dir "$TREE")/$rel"
    [[ -f "$src" ]] || { echo "missing blob: $src" >&2; exit 1; }
    adb push "$src" "/$rel" >/dev/null
    count=$((count + 1))
done < <(grep -E '^vendor/' "$(dirname "$0")/../patches/camera-proprietary-files.txt")
echo "   $count blobs"

# The passthrough -impl module links against both backends (-legacy for the
# HAL1 module we care about, -external for USB cameras), so both have to be
# present or the dlopen fails with "library not found".
echo "== pushing provider, HAL1 wrapper and shim =="
adb push "$OUT/system/vendor/lib/hw/android.hardware.camera.provider@2.4-impl.so" /vendor/lib/hw/ >/dev/null
adb push "$OUT/system/vendor/lib/android.hardware.camera.provider@2.4-legacy.so" /vendor/lib/ >/dev/null
adb push "$OUT/system/vendor/lib/android.hardware.camera.provider@2.4-external.so" /vendor/lib/ >/dev/null
# The provider links the whole device-wrapper family, external ones included,
# regardless of which backend actually ends up serving cameras.
for f in "$OUT"/system/vendor/lib/camera.device@*.so; do
    adb push "$f" /vendor/lib/ >/dev/null
done
adb push "$OUT/system/vendor/lib/libcamera_shim.so" /vendor/lib/ >/dev/null

echo "== installing the vintf manifest =="
adb push "$OUT/system/vendor/etc/vintf/manifest.xml" /vendor/etc/vintf/manifest.xml >/dev/null

echo "== fixing permissions =="
adb shell 'chmod 644 /vendor/lib/libcam*.so /vendor/lib/hw/*.so /vendor/lib/libcamera_shim.so /vendor/etc/vintf/manifest.xml 2>/dev/null'
adb shell 'chown root:root /vendor/lib/libcam*.so /vendor/lib/hw/*.so 2>/dev/null'

echo "== verifying what actually landed =="
# Compare device-side hashes against the exact files pushed. This is what
# turns "the push silently did nothing" into a loud failure.
verify=0
vcheck() {
    local host="$1" dev="$2" want got
    want="$(sha256sum "$host" | cut -d' ' -f1)"
    got="$(adb shell "sha256sum $dev 2>/dev/null" | cut -d' ' -f1 | tr -d '\r')"
    if [[ "$want" == "$got" ]]; then
        echo "   ok    $dev"
    else
        echo "   FAIL  $dev did not land intact" >&2
        verify=1
    fi
}
vcheck "$OUT/system/vendor/etc/vintf/manifest.xml" /vendor/etc/vintf/manifest.xml
vcheck "$OUT/system/vendor/lib/libcamera_shim.so" /vendor/lib/libcamera_shim.so
vcheck "$BLOBS/hw/camera.mt8163.so" /vendor/lib/hw/camera.mt8163.so
vcheck "$BLOBS/libdpframework_cam.so" /vendor/lib/libdpframework_cam.so
if (( verify )); then
    echo "the install did NOT complete; fix the transfer before rebooting" >&2
    exit 1
fi

echo
echo "installed and verified. run scripts/camera-test.sh $DEVICE for a cold-boot test."
echo "to roll back the manifest:"
echo "  adb -s $DEVICE shell 'mount -o rw,remount /system; cp /vendor/etc/vintf/manifest.xml.orig /vendor/etc/vintf/manifest.xml'"
