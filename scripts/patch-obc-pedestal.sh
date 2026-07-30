#!/usr/bin/env bash
#
# Correct the ISP black-level subtraction (OBC) for the OV02B10 pedestal.
#
# Measured with the room fully dark, the frame averaged R=43 G=79 B=84 with a
# minimum pixel of 25 - a ~30% grey floor where black should be, rendering as
# a pervasive haze and grey blacks in every scene. Zero light means it cannot
# be optical: it is the sensor's black-level pedestal surviving the ISP's OBC
# stage, multiplied by AE gain and lifted further by the gamma curve.
#
# The cause is the usual wrong-sensor tuning: the per-ISO ISP tuning in
# libcameracustom.so carries OBC offsets for the OV9734. debug.obc_apply.log=1
# shows what is applied:
#
#   [ISP_MGR_OBC_T] OBGAIN 0-3 (520,520,520,520), offset 0-3 (8128,...)
#
# 8128 = 0x1FC0 = -64 in the 13-bit two's-complement register field. The
# OV02B10 outputs 10-bit RAW with the standard OmniVision pedestal of 64,
# which is 256 in the ISP's 12-bit domain. Subtracting 64 leaves a residual
# of 192 (4.7% linear), which the gamma curve turns into the observed floor.
#
# This rewrites every ISP_NVRAM_OBC_T block (four offsets then four gains;
# 140 blocks, one per ISO/scenario entry) from -64 to -256. Verified on
# device: a dark-room frame drops from R=43 G=79 B=84 to R=8 G=7 B=8.
#
# Reversible: keeps a .orig backup (shared with patch-awb-d65.sh - restoring
# it reverts both patches).
#
set -euo pipefail

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial> [new_offset]" >&2
    exit 2
fi
# 13-bit two's complement register value; default -256. -64 (0x1FC0) is stock.
NEWOFF="${2:-0x1F00}"

LIB=/system/vendor/lib/libcameracustom.so
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

adb -s "$DEVICE" root >/dev/null 2>&1 || true
sleep 3
adb -s "$DEVICE" wait-for-device
adb -s "$DEVICE" remount >/dev/null 2>&1 || adb -s "$DEVICE" shell 'mount -o rw,remount /system'

adb -s "$DEVICE" shell "[ -f $LIB.orig ] || cp $LIB $LIB.orig"
# Patch the CURRENT lib, not .orig: this stacks with patch-awb-d65.sh.
adb -s "$DEVICE" pull "$LIB" "$WORK/in.so" >/dev/null

python3 - "$WORK/in.so" "$WORK/out.so" "$NEWOFF" <<'PYEOF'
import struct, sys

src, dst, newoff = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
d = open(src, 'rb').read()

STOCK = struct.pack('<8I', 0x1FC0, 0x1FC0, 0x1FC0, 0x1FC0, 520, 520, 520, 520)
NEW = struct.pack('<8I', newoff, newoff, newoff, newoff, 520, 520, 520, 520)

n = d.count(STOCK)
if n:
    d = d.replace(STOCK, NEW)
    print("  %d OBC blocks: offset 0x1FC0 (-64) -> %#x" % (n, newoff))
else:
    # Already patched to some other value? Find blocks by the gain signature
    # and report so the caller can decide.
    sys.exit("no stock OBC blocks found - already patched? "
             "restore first: patch-awb-d65.sh <serial> --restore")

open(dst, 'wb').write(d)
PYEOF

adb -s "$DEVICE" push "$WORK/out.so" "$LIB" >/dev/null
adb -s "$DEVICE" shell "chmod 644 $LIB; ls -l $LIB"
echo "patched. restart cameraserver (or reboot) to apply."
