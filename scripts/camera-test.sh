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

# Check the stack is installed before testing what it does. Without this the
# script happily reports app-side symptoms for a device where cameraserver
# never started, which reads like a HAL bug and is not one.
if ! "$(dirname "$0")/camera-preflight.sh" "$DEVICE"; then
    echo
    echo "preflight failed - fix the above first, the test below cannot succeed" >&2
    exit 1
fi
echo

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

# Grant the camera app its runtime permissions first. On a fresh install the
# very first launch spends its time in the permission dance instead of
# streaming: the app connects, the HAL opens camera 0, the app tears it down
# again before a preview starts, and the teardown returns -32. That is a
# benign artifact of an aborted open, but it lands in the log looking exactly
# like a pipeline failure and it wastes the one cold-boot attempt this script
# exists to capture.
for perm in android.permission.CAMERA android.permission.RECORD_AUDIO \
            android.permission.READ_EXTERNAL_STORAGE \
            android.permission.WRITE_EXTERNAL_STORAGE; do
    adb -s "$DEVICE" shell pm grant com.android.camera2 "$perm" >/dev/null 2>&1 || true
done

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
echo "--- pipeline: where it stops ---"
# A uniform green preview means no frames reached the display client at all:
# zero-filled YUV420 is RGB(0,135,0). It is not a colour bug and Bayer order
# cannot cause it - a wrong Bayer pattern corrupts content, it cannot remove
# it. These counts say which stage is actually failing. See docs/findings.md.
# grep -c already prints 0 when there is no match, and exits 1 while doing it.
# A "|| echo 0" here appends a second 0 and every arithmetic test below then
# fails to parse.
c() { local n; n="$(grep -acE "$1" "$OUTDIR/logcat.txt" 2>/dev/null)" || true; echo "${n:-0}"; }
sensor_ok=$(c 'pSensorDrv->open succeed')
tg1_fail=$(c 'ISP_WAIT_IRQ fail|irq_TG1_DONE.*fail|waitIrq.*fail')
p1_fail=$(c 'dequePass1 fail|waitBufReady fail|ERROR:dequeueBuf')
mdp_fail=$(c 'dequeueDstBuffer|abortPoll|MdpMgrImp')
deq=$(c 'dequeueBuffer|enqueueBuffer')
hidl=$(c 'getHidlStatus: unknown HAL status')
printf '  sensor opened over i2c        %s\n' "$sensor_ok"
printf '  ISP irq / TG1 timeouts        %s\n' "$tg1_fail"
printf '  pass-1 dequeue failures       %s\n' "$p1_fail"
printf '  display-framework failures    %s\n' "$mdp_fail"
printf '  buffer dequeue/enqueue calls  %s\n' "$deq"
printf '  unknown HAL status returns    %s\n' "$hidl"
# A -32 with no pipeline failures behind it is the aborted-open teardown
# described above, not a stall. The real thing comes after a multi-second
# hang and brings dequeue or display-framework failures with it.
if (( hidl > 0 && tg1_fail == 0 && p1_fail == 0 && mdp_fail == 0 )); then
    echo "  => the only complaint is a HAL status on teardown, with no pipeline"
    echo "     failures behind it. That is an open the client aborted (usually"
    echo "     the first-launch permission prompt), not a stall. Look at the"
    echo "     preview on the device before treating this as a fault."
elif (( sensor_ok > 0 && tg1_fail > 0 )); then
    echo "  => the sensor answers on i2c but is not streaming pixels: no frame-end"
    echo "     interrupt arrives. Look at the sensor driver's register tables."
elif (( mdp_fail > 0 && deq == 0 )); then
    echo "  => the sensor streams but the display framework never hands over a"
    echo "     buffer. Check scripts/install-private-dpframework.sh applied all"
    echo "     three binary patches, and that the shim is loaded."
elif (( deq == 0 )); then
    echo "  => no buffers were ever dequeued, so nothing could reach the screen."
fi

echo
echo "--- crashes ---"
grep -iE 'signal [0-9]+|SIGSEGV|SIGABRT|Fatal signal' \
    "$OUTDIR/logcat.txt" | head -10 || echo "(none)"
echo
echo "--- sepolicy denials ---"
grep -i 'avc: *denied' "$OUTDIR/logcat.txt" | head -20 || echo "(none)"

# Bundle everything into one file. Reports get attached to issues by hand,
# and three loose .txt files invite exactly the mixup that happened on
# issue #2, where dmesg was uploaded twice and the one file that shows
# userspace - logcat - never arrived.
tar czf "$OUTDIR.tar.gz" -C "$(dirname "$OUTDIR")" "$(basename "$OUTDIR")"
echo
echo "all of the above bundled into: $OUTDIR.tar.gz"
echo "when reporting a problem, attach that one file rather than the pieces"
