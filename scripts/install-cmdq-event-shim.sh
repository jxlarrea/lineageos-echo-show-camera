#!/usr/bin/env bash
#
# Install the cmdq event-id shim and the durable bring-up rc, so the camera
# works from a cold boot with no manual steps.
#
# The shim is loaded via `setenv LD_PRELOAD` in cameraserver's own init rc
# rather than by patching any blob. That matters: /dev/mtk_cmdq is also used by
# android.hardware.graphics.composer, which already agrees with this kernel's
# event-id ABI, so the fix must reach cameraserver and nothing else.
#
# Re-runnable. Keeps a .orig backup of cameraserver.rc the first time.
#
set -euo pipefail

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial>" >&2
    exit 2
fi

HERE="$(cd "$(dirname "$0")/.." && pwd)"
SHIM="$HERE/shims/libcmdqevent/out/libcmdqevent_shim.so"
RC="$HERE/shims/libcmdqevent/camera-bringup.rc"

[[ -f "$SHIM" ]] || { echo "build it first: shims/libcmdqevent/build.sh $DEVICE" >&2; exit 1; }

adb -s "$DEVICE" root >/dev/null 2>&1 || true
sleep 3
adb -s "$DEVICE" wait-for-device
adb -s "$DEVICE" remount >/dev/null 2>&1 || adb -s "$DEVICE" shell 'mount -o rw,remount /system'

adb -s "$DEVICE" push "$SHIM" /system/lib/libcmdqevent_shim.so >/dev/null
adb -s "$DEVICE" push "$RC" /system/etc/init/camera-bringup.rc >/dev/null
adb -s "$DEVICE" shell '
    set -e
    chmod 644 /system/lib/libcmdqevent_shim.so
    chmod 644 /system/etc/init/camera-bringup.rc

    rc=/system/etc/init/cameraserver.rc
    [ -f $rc.orig ] || cp $rc $rc.orig

    # Load the shim into cameraserver only, and re-enable the service: it was
    # set "disabled" during bring-up so a crashing HAL could not take the boot
    # down with it.
    grep -q libcmdqevent_shim $rc || \
        sed -i "s|^    class main|    class main\n    setenv LD_PRELOAD /system/lib/libcmdqevent_shim.so|" $rc
    sed -i "/^    disabled$/d" $rc

    echo "--- cameraserver.rc ---"
    cat $rc
'
echo
echo "installed. reboot to verify from cold boot."
