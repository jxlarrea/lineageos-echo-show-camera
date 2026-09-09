#!/usr/bin/env bash
#
# Install the echo cancellation shim into the audio HAL on an Echo Show.
#
# The shim is loaded with `setenv LD_PRELOAD` in the audio HAL's own init rc,
# the same mechanism upstream uses for camerahalserver, so nothing in the
# blob is patched. init only reads rc files at boot, so a reboot is required
# for the preload to take effect; the script does it unless --no-reboot.
#
# Re-runnable. Keeps a .orig backup of the rc the first time.
#
set -euo pipefail
DEVICE="${1:-}"
[[ -n "$DEVICE" ]] || { echo "usage: $0 <adb-serial> [--no-reboot]" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SHIM="$HERE/shims/libamznaec/out/libamznaec_shim.so"
[[ -f "$SHIM" ]] || { echo "build it first: shims/libamznaec/build.sh" >&2; exit 1; }
. "$(dirname "$0")/adb-lib.sh"
adb_wait_root "$DEVICE"
adb -s "$DEVICE" remount >/dev/null 2>&1 || adb -s "$DEVICE" shell 'mount -o rw,remount /system'
adb -s "$DEVICE" shell 'ls /vendor/lib/libwebrtc_audio_preprocessing.so >/dev/null' \
    || { echo "libwebrtc_audio_preprocessing.so missing in /vendor/lib; this ROM cannot host the shim" >&2; exit 1; }
adb -s "$DEVICE" push "$SHIM" /vendor/lib/libamznaec_shim.so >/dev/null
adb -s "$DEVICE" shell '
    set -e
    chmod 644 /vendor/lib/libamznaec_shim.so
    chcon u:object_r:vendor_file:s0 /vendor/lib/libamznaec_shim.so 2>/dev/null || true
    rc=/vendor/etc/init/android.hardware.audio.service.rc
    [ -f $rc.orig ] || cp $rc $rc.orig
    if ! grep -q libamznaec_shim $rc; then
        sed -i "/^service vendor.audio-hal /a\\    setenv LD_PRELOAD libamznaec_shim.so" $rc
    fi
    grep -n "LD_PRELOAD" $rc
'
echo "installed. The audio HAL picks the shim up at the next boot."
if [[ "${2:-}" != "--no-reboot" ]]; then
    adb -s "$DEVICE" reboot
    echo "rebooting $DEVICE; check with: adb -s $DEVICE shell logcat -d | grep amznaec"
fi
