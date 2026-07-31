#!/usr/bin/env bash
#
# Flash a freshly built boot.img to an amonet-unlocked cronos.
#
# The boot partition is NOT a plain boot image, and it is not a plain boot image
# shifted along either. amonet keeps the payload at its normal offset and steals
# the first two blocks for its exploit header, relocating the real boot header
# to blocks 2-3:
#
#   0x000-0x3ff   amonet exploit header, byte-identical to recovery[0x000:0x400]
#   0x400-0x7ff   copy of the real boot image's first 1 KB (its header)
#   0x800-...     boot image payload, kernel at its usual page offset
#
# This mirrors what TWRP's own /sbin/fix-bootpatch.sh does. Writing a plain
# boot.img over the partition, or writing it at 0x400, both produce a kernel at
# the wrong offset and the device hangs at the vendor logo.
#
# Run with --self-test to verify the assembly logic against known-good images
# without touching the device.
#
set -euo pipefail

EXPLOIT_BYTES=1024   # 0x400, blocks 0-1
PAYLOAD_OFF=2048     # 0x800, where the boot payload starts

# Rebuild a partition image: exploit header, then the new image's first 1 KB at
# 0x400, then the new image's payload from 0x800 on.
assemble() {
    local exploit_src="$1" new_img="$2" out="$3"
    head -c "$EXPLOIT_BYTES" "$exploit_src" > "$out"
    head -c "$EXPLOIT_BYTES" "$new_img" >> "$out"
    dd if="$new_img" bs=$PAYLOAD_OFF skip=1 >> "$out" 2>/dev/null
}

if [[ "${1:-}" == "--self-test" ]]; then
    # Uses a known-good pair from this device: the current (working) boot
    # partition and the recovery partition it borrows its exploit header
    # from. With an adb serial, dump them first; otherwise use the newest
    # pair already in backups/partitions.
    mkdir -p backups/partitions
    if [[ -n "${2:-}" ]]; then
        echo "== dumping boot and recovery from ${2} =="
        for part in boot recovery; do
            command adb -s "$2" shell \
                "dd if=/dev/block/by-name/$part of=/data/local/tmp/$part.img" \
                >/dev/null 2>&1
            command adb -s "$2" pull "/data/local/tmp/$part.img" \
                "backups/partitions/$part-selftest.img" >/dev/null
            command adb -s "$2" shell "rm -f /data/local/tmp/$part.img"
        done
    fi
    B="$(ls -t backups/partitions/boot-*.img 2>/dev/null | head -1 || true)"
    R="$(ls -t backups/partitions/recovery-*.img 2>/dev/null | head -1 || true)"
    if [[ -z "$B" || -z "$R" ]]; then
        cat >&2 <<EOF
self-test needs a known-good boot and recovery image from a working device.
Either pass an adb serial so they can be dumped for you:

    $0 --self-test <adb-serial>

or place them yourself as backups/partitions/boot-*.img and recovery-*.img.
EOF
        exit 1
    fi
    echo "   boot:     $B"
    echo "   recovery: $R"
    T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
    # Reconstruct what the plain boot.img must have looked like: its header,
    # padding out to the payload offset, then the payload.
    head -c "$EXPLOIT_BYTES" /dev/zero > "$T/pad"
    { dd if="$B" bs=1 skip=1024 count=1024 2>/dev/null; cat "$T/pad"; \
      dd if="$B" bs=$PAYLOAD_OFF skip=1 2>/dev/null; } > "$T/plain.img"
    assemble "$R" "$T/plain.img" "$T/rebuilt.img"
    if cmp -s "$T/rebuilt.img" "$B"; then
        echo "self-test PASSED: assembly reproduces the working partition exactly"
        exit 0
    fi
    echo "self-test FAILED" >&2
    cmp "$T/rebuilt.img" "$B" | head -3 >&2
    exit 1
fi

DEVICE="${1:-}"
TREE="${2:-${LINEAGE_TREE:-$HOME/lineage-18.1}}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial> [build-tree]" >&2
    echo "       $0 --self-test [adb-serial]" >&2
    exit 2
fi

NEW="$TREE/out/target/product/cronos/boot.img"
PART=/dev/block/by-name/boot
RECOVERY_PART=/dev/block/by-name/recovery
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

adb() { command adb -s "$DEVICE" "$@"; }

[[ -f "$NEW" ]] || { echo "no boot.img at $NEW - run 'm bootimage'" >&2; exit 1; }

adb root >/dev/null 2>&1 || true
adb wait-for-device

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

echo "== taking the exploit header from the recovery partition =="
# fix-bootpatch.sh treats recovery's first 1 KB as the authoritative copy.
adb shell "dd if=$RECOVERY_PART of=/data/local/tmp/exploit.hdr bs=512 count=2" 2>/dev/null
adb pull /data/local/tmp/exploit.hdr "$WORK/exploit.hdr" >/dev/null
adb shell "rm /data/local/tmp/exploit.hdr"
if ! grep -qa microloader "$WORK/exploit.hdr"; then
    echo "recovery's first 1 KB has no microloader; this is not the expected" >&2
    echo "amonet layout. Refusing to flash." >&2
    exit 1
fi
if ! cmp -s <(head -c $EXPLOIT_BYTES "backups/partitions/boot-pre-$STAMP.img") \
            "$WORK/exploit.hdr"; then
    echo "   note: boot's exploit header differs from recovery's; using recovery's"
fi

echo "== assembling =="
assemble "$WORK/exploit.hdr" "$NEW" "$WORK/combined.img"
part_size=$(adb shell "blockdev --getsize64 $PART" | tr -d '\r')
size=$(stat -c%s "$WORK/combined.img")
echo "   $size bytes into a $part_size byte partition"
(( size <= part_size )) || { echo "image too large, refusing" >&2; exit 1; }

# Sanity-check the result before it goes anywhere.
python3 - "$WORK/combined.img" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
ok = True
if d[0x400:0x408] != b"ANDROID!":
    print("   FAIL: no boot header at 0x400"); ok = False
if d[0x800:0x803].hex() != "1f8b08":
    print("   FAIL: no gzip kernel at 0x800"); ok = False
if b"microloader" not in d[:0x400]:
    print("   FAIL: no microloader in the first 1 KB"); ok = False
if not ok:
    sys.exit(1)
print("   layout verified: microloader, header at 0x400, kernel at 0x800")
PY

echo "== flashing =="
adb push "$WORK/combined.img" /data/local/tmp/combined.img >/dev/null
adb shell "dd if=/data/local/tmp/combined.img of=$PART bs=1M conv=fsync" 2>&1 | tail -1

echo "== verifying =="
adb shell "dd if=$PART of=/data/local/tmp/rb.img bs=1 count=$size" 2>/dev/null
adb pull /data/local/tmp/rb.img "$WORK/rb.img" >/dev/null
adb shell "rm /data/local/tmp/combined.img /data/local/tmp/rb.img"
if cmp -s "$WORK/combined.img" "$WORK/rb.img"; then
    echo "   partition matches byte for byte"
else
    echo "   READBACK MISMATCH - do not reboot" >&2
    exit 1
fi

echo
echo "flashed. if it does not boot, in TWRP:"
echo "  dd if=/sdcard/boot-known-good.img of=/dev/block/platform/soc/by-name/boot bs=1M"
