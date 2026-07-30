#!/usr/bin/env bash
#
# On-device camera test cycle.
#
# Always tests from a cold boot. The crown investigation found at least one
# failure that reproduces 8/8 on a clean boot and never on a warm one, because
# init respawns the crashed provider and the second attempt usually succeeds.
# Opening the camera app twice in a row hides that class of bug completely.
#
set -euo pipefail

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial> [outdir]" >&2
    exit 2
fi
OUTDIR="${2:-logs/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUTDIR"

echo "rebooting"
adb -s "$DEVICE" reboot
adb -s "$DEVICE" wait-for-device

echo "waiting for boot to complete"
until [[ "$(adb -s "$DEVICE" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" == "1" ]]; do
    sleep 2
done

adb -s "$DEVICE" shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1 || true
adb -s "$DEVICE" shell wm dismiss-keyguard >/dev/null 2>&1 || true

adb -s "$DEVICE" logcat -c 2>/dev/null || true
adb -s "$DEVICE" logcat > "$OUTDIR/logcat.txt" 2>&1 &
LOGCAT_PID=$!
trap 'kill $LOGCAT_PID 2>/dev/null || true' EXIT

echo "opening camera (first attempt after boot)"
adb -s "$DEVICE" shell am start -a android.media.action.STILL_IMAGE_CAMERA >/dev/null
sleep 10

adb -s "$DEVICE" shell dmesg > "$OUTDIR/dmesg.txt" 2>&1 || true
adb -s "$DEVICE" shell 'ls /data/tombstones/ 2>/dev/null' > "$OUTDIR/tombstone-list.txt" 2>&1 || true
adb -s "$DEVICE" pull /data/tombstones "$OUTDIR/tombstones" >/dev/null 2>&1 || true

kill $LOGCAT_PID 2>/dev/null || true

echo
echo "results in $OUTDIR"
echo
echo "--- cameras enumerated ---"
grep -iE 'No (back|front|external)-facing camera|Connecting to camera service|camera.*found' \
    "$OUTDIR/logcat.txt" | tail -5 || echo "(nothing)"
echo
echo "--- provider / HAL errors ---"
grep -iE 'CameraProvider|CamDev|camera.*(fail|error|denied)|libcam' \
    "$OUTDIR/logcat.txt" | head -20 || echo "(nothing)"
echo
echo "--- crashes ---"
grep -iE 'signal [0-9]+|SIGSEGV|SIGABRT|Fatal signal' \
    "$OUTDIR/logcat.txt" | head -10 || echo "(none)"
echo
echo "--- sepolicy denials ---"
grep -i 'avc: *denied' "$OUTDIR/logcat.txt" | head -20 || echo "(none)"
