#!/usr/bin/env bash
#
# Flash a freshly built boot.img to an Echo Show unlocked with amonet 2.0.1
# or newer.
#
# amonet 2.0.1 IS REQUIRED. On 2.x the microloader and lk-payload are gone:
# the exploit lives in preloader, lk, tee1, tee2 and expdb, fastbrick never
# touches the boot partition, and boot is an ORDINARY Android boot image
# written verbatim. That is the only layout this script writes.
#
# The retired amonet 1.x kept an exploit header in the first two blocks of
# boot and relocated the real boot header to 0x400. Those layouts are
# mutually unbootable, so the script checks the recovery partition (1.x
# leaves a microloader in its first 1 KB, 2.x does not) and refuses to touch
# a 1.x device rather than writing something that will hang it at the vendor
# logo. Recovery is the signal to trust: a device that has just been
# fastbricked can have a boot partition that parses as neither layout.
#
# Upgrading 1.x to 2.0.1 also gives the device its full 2 GB of RAM instead
# of 1 GB, which is reason enough on its own.
#
# There is a simpler alternative that needs no booted system and no root,
# useful when the device will not boot far enough for adb. fastbrick leaves
# the device in unlocked fastboot, and from there:
#
#     fastboot flash boot <tree>/out/target/product/<device>/boot.img
#
# This script exists for the case where the device is up: it backs the
# partition up first, verifies the transfer by hash, and reads the partition
# back afterwards, none of which fastboot does.
#
set -euo pipefail

DEVICE="${1:-}"
TREE="${2:-${LINEAGE_TREE:-$HOME/lineage-18.1}}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial> [build-tree]" >&2
    exit 2
fi

. "$(dirname "$0")/device-config.sh"
NEW="$(device_out_dir "$TREE")/boot.img"
PART=/dev/block/by-name/boot
RECOVERY_PART=/dev/block/by-name/recovery
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

adb() { command adb -s "$DEVICE" "$@"; }

[[ -f "$NEW" ]] || { echo "no boot.img at $NEW - run 'm bootimage'" >&2; exit 1; }

# This runs against a booted Android system, not TWRP. In recovery the adb
# state is "recovery", which "adb wait-for-device" never matches, so the
# script would sit there forever with no output.
state="$(command adb -s "$DEVICE" get-state 2>/dev/null | tr -d '\r' || true)"
if [[ "$state" == "recovery" || "$state" == "sideload" ]]; then
    echo "the device is in $state (TWRP), and this script needs a booted" >&2
    echo "Android system with working 'adb root'. Reboot to Android first:" >&2
    echo "    adb -s $DEVICE reboot" >&2
    echo "or flash from fastboot instead:" >&2
    echo "    fastboot flash boot $NEW" >&2
    exit 1
fi

. "$(dirname "$0")/adb-lib.sh"
adb_wait_root "$DEVICE"

echo "== checking the amonet generation =="
adb shell "dd if=$RECOVERY_PART of=/data/local/tmp/rec.hdr bs=512 count=2" 2>/dev/null
adb pull /data/local/tmp/rec.hdr "$WORK/rec.hdr" >/dev/null
adb shell "rm /data/local/tmp/rec.hdr"
if grep -qa microloader "$WORK/rec.hdr"; then
    cat >&2 <<EOF
   this device is still on amonet 1.x (recovery carries a microloader).

amonet 1.x is retired and this script no longer writes its boot layout.
Upgrade the device to amonet 2.0.1 or newer, then run this again.

Do not use amonet 2.0.0: it has a bug that leaves TWRP not properly
updated when upgrading from a 1.x unlock, which is exactly this path.

Upgrading also gives the device its full 2 GB of RAM instead of 1 GB.

Nothing has been written.
EOF
    exit 1
fi
echo "   amonet 2.x (no microloader; plain boot layout)"

echo "== backing up the current boot partition =="
mkdir -p backups/partitions
STAMP="$(date +%Y%m%d-%H%M%S)"
adb shell "dd if=$PART of=/data/local/tmp/pre.img bs=1M" 2>/dev/null
adb pull /data/local/tmp/pre.img "backups/partitions/boot-pre-$STAMP.img" >/dev/null
adb shell "rm /data/local/tmp/pre.img"
if [[ "$(sha256sum "backups/partitions/boot-pre-$STAMP.img" | cut -d' ' -f1)" \
      != "$(adb shell "sha256sum $PART" | cut -d' ' -f1)" ]]; then
    echo "backup does not match the device, refusing to flash" >&2
    exit 1
fi
echo "   backups/partitions/boot-pre-$STAMP.img (verified)"

part_size=$(adb shell "blockdev --getsize64 $PART" | tr -d '\r')
size=$(stat -c%s "$NEW")
echo "   $size bytes into a $part_size byte partition"
(( size <= part_size )) || { echo "image too large, refusing" >&2; exit 1; }

# Sanity-check the image before it goes anywhere. A 1.x partition dump fed in
# here by mistake trips the microloader check rather than bricking the device.
python3 - "$NEW" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
ok = True
if d[0x000:0x008] != b"ANDROID!":
    print("   FAIL: no boot header at 0x000"); ok = False
if d[0x800:0x803].hex() != "1f8b08":
    print("   FAIL: no gzip kernel at 0x800"); ok = False
if b"microloader" in d[:0x400]:
    print("   FAIL: microloader in the first 1 KB - this is a retired amonet")
    print("         1.x partition image, not a plain boot.img")
    ok = False
if not ok:
    sys.exit(1)
print("   layout verified: plain image, header at 0x000, kernel at 0x800")
PY

echo "== transferring =="
# A multi-megabyte "adb push" fails on some hosts while small pushes and large
# pulls both succeed: it moves the bytes, prints "adb: error: failed to read
# copy response" and exits non-zero. USB passed through to a virtual machine is
# the usual cause. Errors here used to go to /dev/null, so set -e killed the
# script immediately after printing "== flashing ==" and it looked like the
# flash had happened when nothing had been written at all.
#
# So: never hide the transfer, always confirm what landed on the device by
# hash, and fall back to chunks before giving up. Nothing is written to the
# boot partition until the device holds a byte-perfect copy.
DEST=/data/local/tmp/flash-boot.img
want="$(sha256sum "$NEW" | cut -d' ' -f1)"

device_hash() { adb shell "sha256sum $DEST 2>/dev/null" | cut -d' ' -f1 | tr -d '\r'; }

adb shell "rm -f $DEST" >/dev/null 2>&1 || true
adb push "$NEW" "$DEST" || echo "   push reported an error, checking what arrived"

if [[ "$(device_hash)" != "$want" ]]; then
    echo "   the whole-file push did not verify, retrying in 1 MB chunks"
    adb shell "rm -f $DEST" >/dev/null 2>&1 || true
    split -b 1048576 -d -a 3 "$NEW" "$WORK/part."
    for chunk in "$WORK"/part.*; do
        cname="$(basename "$chunk")"
        chash="$(sha256sum "$chunk" | cut -d' ' -f1)"
        for attempt in 1 2 3; do
            adb push "$chunk" "/data/local/tmp/$cname" >/dev/null 2>&1 || true
            got="$(adb shell "sha256sum /data/local/tmp/$cname 2>/dev/null" \
                   | cut -d' ' -f1 | tr -d '\r')"
            [[ "$got" == "$chash" ]] && break
            [[ $attempt == 3 ]] && {
                echo "   $cname will not transfer intact after 3 attempts." >&2
                echo "   The USB link to this device is dropping data. If the host" >&2
                echo "   is a virtual machine, try a bare-metal host or a different" >&2
                echo "   USB port or cable. Nothing has been written to the boot" >&2
                echo "   partition." >&2
                exit 1
            }
        done
        adb shell "cat /data/local/tmp/$cname >> $DEST && rm /data/local/tmp/$cname"
        printf '.'
    done
    echo
fi

got="$(device_hash)"
if [[ "$got" != "$want" ]]; then
    echo "   the image on the device does not match the one built here." >&2
    echo "     want $want" >&2
    echo "     got  ${got:-<nothing>}" >&2
    echo "   Refusing to flash. Nothing has been written." >&2
    exit 1
fi
echo "   $size bytes on the device, sha256 verified"

echo "== flashing =="
adb shell "dd if=$DEST of=$PART bs=1M conv=fsync" 2>&1 | tail -1

echo "== verifying =="
# Read back in 64 KB blocks. This used to use bs=1, which issues one syscall
# per byte and takes many minutes for an 8 MB image.
blocks=$(( (size + 65535) / 65536 ))
adb shell "dd if=$PART of=/data/local/tmp/rb.img bs=64k count=$blocks" 2>/dev/null
adb pull /data/local/tmp/rb.img "$WORK/rb.raw" >/dev/null
adb shell "rm -f $DEST /data/local/tmp/rb.img"
head -c "$size" "$WORK/rb.raw" > "$WORK/rb.img"
if cmp -s "$NEW" "$WORK/rb.img"; then
    echo "   partition matches byte for byte"
else
    echo "   READBACK MISMATCH - do not reboot" >&2
    exit 1
fi

echo
echo "flashed."
echo "if it does not boot, the quickest fix is fastboot:"
echo "  fastboot flash boot $NEW"
echo
echo "or restore the backup this run just made. Push"
echo "backups/partitions/boot-pre-$STAMP.img to the device and, in TWRP:"
echo "  dd if=/sdcard/boot-pre-$STAMP.img of=/dev/block/platform/soc/by-name/boot bs=1M"
