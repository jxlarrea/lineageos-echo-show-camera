# Shared adb helpers, sourced by the other scripts.
#
# adb_wait_root: bring the device up with adbd running as root, and do not
# return until it actually is.
#
# 'adb root' restarts adbd on the device, which drops the transport. On a
# bare-metal host the reconnect takes a fraction of a second and a single
# 'adb wait-for-device' bridges it. Through a VM's USB passthrough the
# disconnect goes to the HOST first, which reclaims the device before the
# hypervisor re-captures and re-attaches it to the guest - seconds, and two
# separate transitions, during which the device does not exist on the
# guest's bus at all ('device not found', not 'offline').
# 'adb wait-for-device' can return in the gap between those transitions, so
# the caller's next command or two still failed. Every script here used to
# race this (one papered over it with a bare 'sleep 3'), and it presented
# as 'run it a bunch of times, eventually it works': the first failed run
# left adbd rooted, so a later run had no restart left to trip on.
#
# Polling 'id -u' until it says 0 is the honest version: it tolerates the
# device vanishing however many times the passthrough needs, and it proves
# root rather than assuming the restart worked.
adb_wait_root() {
    local serial="$1" deadline=$((SECONDS + 60)) uid
    command adb -s "$serial" root >/dev/null 2>&1 || true
    while (( SECONDS < deadline )); do
        uid="$(command adb -s "$serial" shell id -u 2>/dev/null | tr -d '\r')"
        [[ "$uid" == "0" ]] && return 0
        sleep 1
    done
    echo "adb did not come back as root on $serial within 60s." >&2
    echo "On a userdebug build this usually means the USB link is gone -" >&2
    echo "check 'adb devices'. On a user build 'adb root' cannot work." >&2
    return 1
}
