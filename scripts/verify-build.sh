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

# These devices are pre-Treble: /vendor is a symlink to /system/vendor, so the
# build stages everything under system/vendor and $OUT/vendor does not exist at
# all. This script originally checked $OUT/vendor and therefore reported every
# blob missing on a perfectly good build - it was written against invented
# fixtures rather than a real tree, so its pass case was unreachable. Resolve
# the staged vendor root instead of assuming one.
if [[ -d "$OUT/system/vendor" ]]; then
    VOUT="$OUT/system/vendor"
elif [[ -d "$OUT/vendor" ]]; then
    VOUT="$OUT/vendor"
else
    echo "no staged vendor directory under $OUT - did the build finish?" >&2
    exit 2
fi
echo "  vendor staged at: ${VOUT#$OUT/}"

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
elif [[ -f "$OUT/system.img" ]] && [[ "$OUT/system.img" -nt "$ZIP" ]]; then
    # Every check below reads the staging directory, not the zip. A partial
    # rebuild (mka systemimage and friends) updates the staging and system.img
    # but leaves the old zip in place, so the checks can all pass while the
    # zip you are about to flash contains none of it.
    echo
    echo "  NOTE: system.img is newer than $(basename "$ZIP")."
    echo "  The staging directory was rebuilt after that zip was packaged, so"
    echo "  the checks below describe a build the zip does not contain. Run"
    echo "  'mka bacon' to repackage before flashing."
    STALE=1
fi

echo
echo "== vendor blobs (step 6) =="
for f in lib/hw/camera.mt8163.so lib/libdpframework_cam.so; do
    [[ -f "$VOUT/$f" ]] && ok "$f" || bad "$f is missing" \
        "step 6 did not reach the build. Re-run 6.1 through 6.5, then rebuild."
done
n=$(find "$VOUT/lib" -maxdepth 1 -name 'libcam*.so' 2>/dev/null | wc -l)
(( n >= 30 )) && ok "$n libcam*.so blobs staged" || bad "only $n libcam*.so blobs staged (expect 30+)" \
    "setup-makefiles.sh in 6.1 regenerates the vendor makefiles; appending to proprietary-files.txt alone does nothing."

echo
echo "== source-built shim (patch 0011 PRODUCT_PACKAGES) =="
[[ -f "$VOUT/lib/libcamera_shim.so" ]] && ok "lib/libcamera_shim.so" \
    || bad "lib/libcamera_shim.so is missing" \
           "patch 0011 adds libcamera_shim to PRODUCT_PACKAGES in mt8163.mk."

echo
echo "== camera feature (patch 0011, mt8163.mk) =="
P="$VOUT/etc/permissions"
[[ -f "$P/android.hardware.camera.front.xml" ]] && ok "android.hardware.camera.front.xml staged" \
    || bad "android.hardware.camera.front.xml is missing" "patch 0011 is not in this build"
[[ -f "$P/android.hardware.camera.xml" ]] && bad "the back-camera android.hardware.camera.xml is still staged" \
    "patch 0011 replaces it; leaving it breaks CameraX device-wide" \
    || ok "the back-camera android.hardware.camera.xml is gone"

echo
echo "== provider declaration (patch 0011, manifest.xml) =="
M="$VOUT/etc/vintf/manifest.xml"
prov="$(python3 - "$M" <<'PY' 2>/dev/null || true
import re, sys
try:
    s = open(sys.argv[1]).read()
except OSError:
    sys.exit(0)
# only the camera *provider* hal; vendor.mediatek...camera.ccap also carries an
# internal/0 instance and is unrelated
for m in re.finditer(r'<hal\b(?:(?!</hal>).)*?</hal>', s, re.S):
    b = m.group(0)
    if '<name>android.hardware.camera.provider</name>' in b:
        print('legacy' if 'legacy/0' in b else ('internal' if 'internal/0' in b else 'other'))
        break
PY
)"
case "$prov" in
    legacy)   ok "vintf declares the passthrough legacy camera provider" ;;
    internal) bad "vintf still declares MediaTek's internal/0 camera provider" \
                  "patch 0011 is not in this build" ;;
    other)    bad "the camera provider in vintf is neither legacy/0 nor internal/0" ;;
    *)        bad "no camera provider declared in $M" ;;
esac

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
# The cronos hardware carries an OV02B10, but the stock cronos_defconfig
# selects only the OV9734 driver, so a kernel built without step 3.2/3.3
# probes the wrong sensor and the camera enumerates yet never delivers a
# frame (an endless ISP_WAIT_IRQ retry loop). This is the one camera
# failure the staging checks above cannot see, and it is exactly what
# happens when a tree walked through the guide for crown or checkers is
# later asked to build cronos: the device-specific kernel steps were
# never run for it. Check the kernel config the build actually used.
if [[ "$CAMERA_DEVICE" == "cronos" ]]; then
echo "== kernel sensor driver (step 3.2/3.3, cronos) =="
KCONF="$OUT/obj/KERNEL_OBJ/.config"
if [[ ! -f "$KCONF" ]]; then
    bad "no built kernel config at $KCONF" "the kernel has not been built in this tree"
elif grep -q '^CONFIG_CUSTOM_KERNEL_IMGSENSOR=.*ov02b10_mipi_raw' "$KCONF"; then
    ok "built kernel selects the OV02B10 sensor driver"
else
    bad "built kernel does not select ov02b10_mipi_raw" \
        "steps 3.2/3.3 were never run in this tree: untar the driver and apply patch 0004, then rebuild. The camera will enumerate but never produce a frame without it."
fi
fi

echo
# Patch 0017 lands in device/amazon/crown and device/amazon/cronos: the two
# devices it is tested and verified on. Whether checkers needs the same fixes
# has not been checked, so its builds are not expected to carry them and
# these checks would only mislead there.
if [[ "$CAMERA_DEVICE" == "crown" || "$CAMERA_DEVICE" == "cronos" ]]; then
echo "== bluetooth (patch 0017, $CAMERA_DEVICE device.mk) =="
[[ -f "$VOUT/etc/permissions/android.hardware.bluetooth_le.xml" ]] \
    && ok "android.hardware.bluetooth_le.xml staged" \
    || bad "android.hardware.bluetooth_le.xml is missing" \
           "patch 0017 is not in this build. Every BLE scan will fail with SCAN_FAILED_INTERNAL_ERROR."

# The A2DP sink bools must be compiled INTO Bluetooth.apk. When they ship
# only as a runtime RRO, the manifest's android:enabled="@bool/..." on the
# sink services resolves the APK-internal false at package scan while the
# runtime profile list sees the RRO's true, and the adapter crash-loops
# every ~8 seconds on enable.
BTAPK="$OUT/system/app/Bluetooth/Bluetooth.apk"
AAPT2="$TREE/out/host/linux-x86/bin/aapt2"
if [[ ! -f "$BTAPK" ]]; then
    bad "Bluetooth.apk is not in the build output"
elif [[ ! -x "$AAPT2" ]]; then
    bad "cannot check Bluetooth.apk: no host aapt2 at $AAPT2" \
        "the check needs a completed build; rebuild and run this again."
elif "$AAPT2" dump resources "$BTAPK" 2>/dev/null \
        | grep -A1 'bool/profile_supported_a2dp_sink' | grep -q 'true'; then
    ok "profile_supported_a2dp_sink is true inside Bluetooth.apk"
else
    bad "profile_supported_a2dp_sink is false inside Bluetooth.apk" \
        "patch 0017's RRO exemption is not in this build; the adapter will crash-loop on enable."
fi

# With the exemption in place the build stops generating the RRO. A leftover
# staged copy from an earlier incremental build is benign once the APK carries
# true (the values agree), but a clean build must not produce it.
if [[ -f "$VOUT/overlay/Bluetooth__auto_generated_rro_vendor.apk" ]]; then
    bad "Bluetooth__auto_generated_rro_vendor.apk is still staged in vendor/overlay" \
        "stale from an incremental build: delete it and 'mka systemimage', or run 'mka installclean' and rebuild."
else
    ok "no Bluetooth RRO staged in vendor/overlay"
fi
fi

echo
if (( fail == 0 )) && (( STALE )); then
    echo "$pass checks passed, but they describe the staging directory, not"
    echo "the zip (see the note above). Run 'mka bacon', then run this again."
    exit 1
elif (( fail == 0 )); then
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
