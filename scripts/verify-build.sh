#!/usr/bin/env bash
#
# Check that the ROM you are about to flash actually contains the camera work.
#
# This is the cheapest check in the whole procedure and it catches the most
# expensive mistake. A checkers user on issue #2 had a correctly patched tree
# and a ROM built before those patches landed: every source-side check they ran
# passed, and the device still came up with no camera blobs at all, no feature
# file and MediaTek's unserved provider. Finding that costs a rebuild, a flash,
# a boot and a test cycle. Finding it here costs a second.
#
# Run after `mka bacon` and before flashing anything.
#
set -euo pipefail

TREE="${1:-${LINEAGE_TREE:-$HOME/lineage-18.1}}"
. "$(dirname "$0")/device-config.sh"
OUT="$(device_out_dir "$TREE")"

[[ -d "$OUT" ]] || { echo "no build output at $OUT" >&2
                     echo "check CAMERA_DEVICE (currently $CAMERA_DEVICE) and that the build ran" >&2
                     exit 2; }

pass=0 fail=0
ok()  { printf '  \033[32mok\033[0m    %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; fail=$((fail + 1))
        [[ -n "${2:-}" ]] && printf '        %s\n' "$2"; return 0; }

echo "checking $OUT"

# "Not rebuilt yet" and "rebuilt and still wrong" produce identical failures
# below, and the fix for each is completely different. Say which it is up
# front: if the vendor tree is newer than the zip, the build simply has not
# caught up and every failure below is expected.
ZIP="$(ls -t "$OUT"/lineage-18.1-*.zip 2>/dev/null | head -1 || true)"
VENDOR="$(device_vendor_dir "$TREE")"
STALE=0
if [[ -z "$ZIP" ]]; then
    echo
    echo "  no ROM zip in $OUT - the build has not produced one yet."
    STALE=1
elif [[ -d "$VENDOR" ]] && [[ -n "$(find "$VENDOR" -type f -newer "$ZIP" -print -quit 2>/dev/null)" ]]; then
    echo
    echo "  NOTE: files in $VENDOR are newer than"
    echo "  $(basename "$ZIP")."
    echo "  You have changed the tree since the last build, so the failures"
    echo "  below are expected. Run 'mka bacon' and check again."
    STALE=1
fi

echo
echo "== vendor blobs (step 6) =="
for f in vendor/lib/hw/camera.mt8163.so vendor/lib/libdpframework_cam.so; do
    [[ -f "$OUT/$f" ]] && ok "$f" || bad "$f is missing" \
        "step 6 did not reach the build. Re-run 6.1 through 6.5, then rebuild."
done
n=$(find "$OUT/vendor/lib" -maxdepth 1 -name 'libcam*.so' 2>/dev/null | wc -l)
(( n >= 30 )) && ok "$n libcam*.so blobs staged" || bad "only $n libcam*.so blobs staged (expect 30+)" \
    "setup-makefiles.sh in 6.1 regenerates the vendor makefiles; appending to proprietary-files.txt alone does nothing."

echo
echo "== source-built shim (patch 0011 PRODUCT_PACKAGES) =="
[[ -f "$OUT/vendor/lib/libcamera_shim.so" ]] && ok "vendor/lib/libcamera_shim.so" \
    || bad "vendor/lib/libcamera_shim.so is missing" \
           "patch 0011 adds libcamera_shim to PRODUCT_PACKAGES in mt8163.mk."

echo
echo "== camera feature (patch 0011, mt8163.mk) =="
P="$OUT/vendor/etc/permissions"
[[ -f "$P/android.hardware.camera.front.xml" ]] && ok "android.hardware.camera.front.xml staged" \
    || bad "android.hardware.camera.front.xml is missing" "patch 0011 is not in this build"
[[ -f "$P/android.hardware.camera.xml" ]] && bad "the back-camera android.hardware.camera.xml is still staged" \
    "patch 0011 replaces it; leaving it breaks CameraX device-wide" \
    || ok "the back-camera android.hardware.camera.xml is gone"

echo
echo "== provider declaration (patch 0011, manifest.xml) =="
M="$OUT/vendor/etc/vintf/manifest.xml"
if [[ -f "$M" ]] && grep -q 'legacy/0' "$M"; then
    ok "vintf declares the passthrough legacy provider"
elif [[ -f "$M" ]] && grep -q 'internal/0' "$M"; then
    bad "vintf still declares MediaTek's internal/0 provider" "patch 0011 is not in this build"
else
    bad "no camera provider in $M"
fi

echo
echo "== cameraserver held back until the shim is installed (patch 0013) =="
RC="$OUT/system/etc/init/cameraserver.rc"
if [[ -f "$RC" ]] && grep -q '^[[:space:]]*disabled[[:space:]]*$' "$RC"; then
    ok "cameraserver.rc is disabled in the ROM, as patch 0013 intends"
else
    bad "cameraserver.rc is not disabled" \
        "patch 0013 is not in this build. The camera cannot work before step 9, and a client that opens it first can wedge the device."
fi

echo
if (( fail == 0 )); then
    echo "$pass checks passed."
    [[ -n "$ZIP" ]] && echo "ready to flash: $ZIP"
elif (( STALE )); then
    echo "$fail of $((pass + fail)) checks failed, and the build is out of date."
    echo
    echo "This is the expected result before rebuilding. Run 'mka bacon', then"
    echo "run this again. No re-sync and no kernel rebuild are needed."
    exit 1
else
    echo "$fail of $((pass + fail)) checks FAILED."
    echo
    echo "Do not flash this build. The zip is newer than everything in the"
    echo "vendor tree, so this is not simply a build that has not caught up:"
    echo "something in step 4 or step 6 did not reach the image. Check the"
    echo "specific causes above."
    exit 1
fi
