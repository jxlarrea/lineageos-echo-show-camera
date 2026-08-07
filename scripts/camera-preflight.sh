#!/usr/bin/env bash
#
# Check that everything the camera needs is actually installed on the device.
#
# camera-test.sh reports what the camera stack did. This reports whether the
# stack is there at all, which is a different question and the one that goes
# wrong first. A checkers user on issue #2 spent an evening on "0 cameras
# found" when the real state was that cameraserver had never started: every
# app-side symptom looked like a HAL problem, and nothing pointed at the
# service being absent.
#
# Read-only. Run it any time, before or after camera-test.sh.
#
set -euo pipefail

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial>" >&2
    exit 2
fi

adb() { command adb -s "$DEVICE" "$@"; }
. "$(dirname "$0")/adb-lib.sh"
# Read-only checks mostly work without root, so a failed wait degrades to
# a warning rather than aborting the report.
adb_wait_root "$DEVICE" || echo "  warning: no root; some checks may misreport" >&2

pass=0 fail=0
ok()   { printf '  \033[32mok\033[0m    %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; fail=$((fail + 1));
         [[ -n "${2:-}" ]] && printf '        %s\n' "$2"; return 0; }
# Worth knowing but not a blocker: crown runs a correct camera without it.
note() { printf '  \033[33mnote\033[0m  %s\n' "$1"
         [[ -n "${2:-}" ]] && printf '        %s\n' "$2"; return 0; }

# Quote-safe helpers. Everything runs in one shell per check so a missing
# file cannot abort the rest.
has_file() { [[ "$(adb shell "[ -f '$1' ] && echo y" | tr -d '\r')" == "y" ]]; }
readfile() { adb shell "cat '$1' 2>/dev/null" | tr -d '\r'; }

echo "== the camera service =="
if adb shell 'ps -A 2>/dev/null || ps' | grep -q '[c]ameraserver'; then
    ok "cameraserver is running"
else
    bad "cameraserver is NOT running" \
        "nothing below matters until this starts; see the cameraserver.rc checks"
fi

RC=/system/etc/init/cameraserver.rc
if has_file "$RC"; then
    rc="$(readfile "$RC")"
    if grep -q '^[[:space:]]*disabled[[:space:]]*$' <<<"$rc"; then
        bad "cameraserver.rc still says 'disabled'" \
            "patch 0013 disables the service at build time and scripts/install-cmdq-event-shim.sh removes that line. Re-run it, then reboot. Reinstalling the ROM zip puts the line back."
    else
        ok "cameraserver.rc does not disable the service"
    fi
    if grep -q 'libcmdqevent_shim' <<<"$rc"; then
        ok "cameraserver.rc sets LD_PRELOAD for the shim"
    else
        bad "cameraserver.rc has no LD_PRELOAD line" \
            "run scripts/install-cmdq-event-shim.sh <serial>"
    fi
else
    bad "$RC is missing"
fi

echo
echo "== the shim (installed by scripts/install-cmdq-event-shim.sh) =="
for f in /system/lib/libcmdqevent_shim.so /system/etc/init/camera-bringup.rc; do
    has_file "$f" && ok "$f" || bad "$f is missing" "run scripts/install-cmdq-event-shim.sh <serial>"
done

echo
echo "== the vendor blobs (baked into the ROM by step 6) =="
for f in /vendor/lib/hw/camera.mt8163.so /vendor/lib/libdpframework_cam.so \
         /vendor/lib/libcamera_shim.so; do
    has_file "$f" && ok "$f" || bad "$f is missing" \
        "step 6 did not reach the ROM. Check that setup-makefiles.sh ran in 6.1 and rebuild."
done
n="$(adb shell 'ls /vendor/lib/libcam*.so 2>/dev/null | wc -l' | tr -d '\r')"
if [[ "${n:-0}" -ge 30 ]]; then
    ok "$n libcam*.so blobs in /vendor/lib"
else
    bad "only ${n:-0} libcam*.so blobs in /vendor/lib (expect 30+)" \
        "the blobs are not in the ROM; see step 6.1"
fi

echo
echo "== the camera feature declaration (patch 0011, mt8163.mk) =="
perms="$(adb shell 'ls /vendor/etc/permissions/ /system/etc/permissions/ 2>/dev/null' | tr -d '\r')"
if grep -q 'android.hardware.camera.front.xml' <<<"$perms"; then
    ok "android.hardware.camera.front.xml is installed"
else
    bad "android.hardware.camera.front.xml is not installed" \
        "patch 0011's mt8163.mk hunk did not reach this build"
fi
if grep -q '^android.hardware.camera.xml$' <<<"$perms"; then
    bad "android.hardware.camera.xml is still installed" \
        "that declares a BACK camera these devices do not have, and CameraX then fails device-wide with 'LENS_FACING_BACK verification failed'. Patch 0011 replaces it."
else
    ok "the back-camera android.hardware.camera.xml is not installed"
fi

echo
echo "== the provider declaration (patch 0011, manifest.xml) =="
vintf="$(readfile /vendor/etc/vintf/manifest.xml)"
if grep -q 'legacy/0' <<<"$vintf"; then
    ok "vintf declares the passthrough legacy camera provider"
elif grep -q 'internal/0' <<<"$vintf"; then
    bad "vintf still declares MediaTek's internal/0 provider" \
        "nothing serves it on these devices; patch 0011 replaces it with legacy/0"
else
    bad "vintf declares no camera provider at all"
fi

echo
echo "== device tree bits that patch 0011 only applies to cronos =="
ori="$(adb shell getprop ro.camera.sensor_orientation | tr -d '\r')"
if [[ -n "$ori" ]]; then
    ok "ro.camera.sensor_orientation=$ori"
else
    note "ro.camera.sensor_orientation is unset" \
         "patch 0011 sets this in device/amazon/cronos/device.mk, so crown and checkers do not get it. Not a blocker - crown runs a correct camera without it - but if your picture is rotated 90 degrees, set it in your own device tree. Try it live first: adb shell setprop ro.camera.sensor_orientation 0"
fi

echo
if (( fail == 0 )); then
    echo "$pass checks passed. The stack is installed; run scripts/camera-test.sh next."
else
    echo "$fail of $((pass + fail)) checks FAILED. Fix those before reading camera-test.sh output:"
    echo "app-side errors like 'No available camera can be found' are downstream of all of them."
    exit 1
fi
