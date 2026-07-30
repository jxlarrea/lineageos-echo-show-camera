#!/usr/bin/env bash
#
# Run cameraserver under the cmdq ioctl tracer and collect a trace.
#
# cameraserver cannot simply be started as root: CameraService rejects a
# client whose uid it does not trust with "Untrusted caller ... trying to
# forward camera access", so the app never gets characteristics and the
# stack is never exercised. camwrap drops to uid/gid cameraserver (1047)
# with the rc file's groups plus media (1013, needed for /dev/MTK_SMI and
# /proc/m4u on this port) and re-execs with LD_PRELOAD intact.
#
# Do not SIGKILL cameraserver while the ISP is streaming. It leaves M4U
# ports configured against freed buffers and the resulting EMI MPU
# violation storm livelocks the device, which needs a physical power cycle.
# Prefer `stop cameraserver` or a reboot between runs.
#
set -euo pipefail

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial> [outdir]" >&2
    exit 2
fi
OUTDIR="${2:-logs/cmdq-$(date +%Y%m%d-%H%M%S)}"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUTDIR"

adb -s "$DEVICE" push "$HERE/out/ioctl_hook.so" /data/local/tmp/ >/dev/null
adb -s "$DEVICE" push "$HERE/out/camwrap" /data/local/tmp/ >/dev/null
adb -s "$DEVICE" shell 'chmod 644 /data/local/tmp/ioctl_hook.so; chmod 755 /data/local/tmp/camwrap'

# The bring-up incantation: see "Runtime workarounds needed every boot".
adb -s "$DEVICE" shell '
    stop cameraserver
    chmod 666 /dev/mtk_cmdq /dev/mdp_sync /dev/MTK_SMI /proc/m4u
    setprop debug.lsc_mgr.type 0
    mount -o ro -t ext4 /dev/block/by-name/persist /persist 2>/dev/null
    logcat -c
    (/data/local/tmp/camwrap </dev/null >/dev/null 2>&1 &)
'
sleep 3

# The camera activity pauses immediately if the screen is off, so no frames
# are ever submitted and the trace comes back empty. Wake and unlock first.
adb -s "$DEVICE" shell 'input keyevent KEYCODE_WAKEUP; input keyevent KEYCODE_MENU' >/dev/null
sleep 1
adb -s "$DEVICE" shell 'am start -a android.media.action.STILL_IMAGE_CAMERA' >/dev/null
sleep 10

adb -s "$DEVICE" shell 'logcat -d -s CMDQHOOK' > "$OUTDIR/cmdq-hook.log" || true
adb -s "$DEVICE" shell 'logcat -d' > "$OUTDIR/logcat.log" || true
adb -s "$DEVICE" shell 'cat /proc/mtk_cmdq_debug/status' > "$OUTDIR/cmdq-status.log" || true
adb -s "$DEVICE" shell 'cat /proc/mtk_cmdq_debug/error' > "$OUTDIR/cmdq-error.log" || true
adb -s "$DEVICE" shell 'cat /proc/mtk_cmdq_debug/record' > "$OUTDIR/cmdq-record.log" || true

echo "trace in $OUTDIR"
grep -E 'BAD meta|meta_count|EXEC ret|WAIT job' "$OUTDIR/cmdq-hook.log" | head -40 || true
