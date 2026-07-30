#!/usr/bin/env bash
#
# Fix the upside-down preview by correcting the reported sensor orientation.
#
# The OV02B10 is mounted rotated 180 degrees relative to what the tuning blob
# assumes, and the driver cannot compensate: the ported sensor driver declares
# .mirror = IMAGE_NORMAL and its set_mirror_flip() is #if 0'd out with empty
# cases, so the sensor is never told to flip.
#
# SENSOR_ORIENTATION is exactly how Android expresses "the sensor is mounted
# rotated", and both the preview and the JPEG EXIF follow it, so correcting it
# is the right layer - and it needs no kernel build or boot flash.
#
# NSCamCustomSensor::getSensorOrientation() returns a pointer to a table of
# three entries (main, sub, main2), not a constant:
#
#   ldr r0, [pc, #4] ; add r0, pc ; bx lr   ->  { 90, 270, 90 }
#
# cronos enumerates its camera as Facing: Front, i.e. the sub slot, so entry 1
# is the one in play. Rotating it by 180 means 270 -> 90.
#
# Reversible: keeps a .orig backup.
#
set -euo pipefail

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial> [new-orientation|--restore]" >&2
    exit 2
fi
NEW="${2:-90}"

LIB=/system/vendor/lib/libcameracustom.so
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

adb -s "$DEVICE" root >/dev/null 2>&1 || true
sleep 3
adb -s "$DEVICE" wait-for-device
adb -s "$DEVICE" remount >/dev/null 2>&1 || adb -s "$DEVICE" shell 'mount -o rw,remount /system'

if [[ "$NEW" == "--restore" ]]; then
    adb -s "$DEVICE" shell "[ -f $LIB.orig ] && cp $LIB.orig $LIB && echo restored || echo 'no backup'"
    exit 0
fi

adb -s "$DEVICE" shell "[ -f $LIB.orig ] || cp $LIB $LIB.orig"
adb -s "$DEVICE" pull "$LIB" "$WORK/in.so" >/dev/null

python3 - "$WORK/in.so" "$WORK/out.so" "$NEW" <<'PYEOF'
import struct, sys

src, dst, new = sys.argv[1], sys.argv[2], int(sys.argv[3])
d = bytearray(open(src, 'rb').read())

# Resolve getSensorOrientation()'s PC-relative table pointer rather than
# hunting for the values, so this stays correct if the blob shifts.
SEGS = [(0x000000, 0x00003000, 0x38cde0),
        (0x38db38, 0x00391b38, 0x1e7f8),
        (0x57e000, 0x0057e000, 0x63d24)]

def v2f(v):
    for off, va, sz in SEGS:
        if va <= v < va + sz:
            return off + (v - va)
    raise SystemExit("vaddr %#x outside all LOAD segments" % v)

LITERAL_VA = 0xdba2c      # literal pool slot inside getSensorOrientation()
ADD_PC_VA = 0xdba2a       # value of PC at the `add r0, pc`

lit = struct.unpack_from('<I', d, v2f(LITERAL_VA))[0]
table = v2f((lit + ADD_PC_VA) & 0xffffffff)

cur = struct.unpack_from('<3i', d, table)
print("  orientation table at file %#x: main=%d sub=%d main2=%d" % ((table,) + cur))

if cur[1] == new:
    print("  sub already %d, nothing to do" % new)
else:
    struct.pack_into('<i', d, table + 4, new)
    print("  sub orientation %d -> %d" % (cur[1], new))

open(dst, 'wb').write(d)
PYEOF

adb -s "$DEVICE" push "$WORK/out.so" "$LIB" >/dev/null
adb -s "$DEVICE" shell "chmod 644 $LIB"
echo "patched. reboot to apply."
