#!/usr/bin/env bash
#
# Correct the AWB D65 reference gain in libcameracustom.so.
#
# The preview came out strongly magenta. AWB was not failing - it ran, settled,
# and applied what it was told:
#
#   awb_algo: [1][PV AWB Gain] LV = 28, Rgain = 933, Ggain = 512, Bgain = 639
#   awb_algo: [1] CCT = 6500
#   awb_algo: [1][updateAWBParam] rD65Gain: R=891, G=512, B=610
#
# 512 is unity, so it asked for R x1.74 and B x1.19 - a strongly red-biased
# correction - and the output tracked that reference almost exactly.
#
# Two independent sources say that reference is wrong for this module:
#
#   lsc_mgr2_thread: [tsfInit] AwbNvramInfo: D65Gain(731, 512, 743)
#   CamCal:          [rCalGain] R = 756, G = 512, B = 742   (this unit's own
#                                                            factory calibration)
#
# Both are balanced (R ~= B), as a sensor behind an IR-cut filter should be.
# The two readers disagreeing about the same NVRAM block, by exactly one
# 12-byte AWB_GAIN_T, is the same struct-offset drift seen everywhere else in
# this port.
#
# Rewriting the red-biased triple to the balanced one is the smallest change
# that puts AWB on the correct anchor. Reversible: keeps a .orig backup.
#
set -euo pipefail

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial> [--restore]" >&2
    exit 2
fi
MODE="${2:-patch}"
# Optional replacement triple, for experiments: R G B. Defaults to unity,
# which is the deployed configuration: the data reaching AWB is already
# balanced by the factory calibration pre-gain, so any non-unity reference
# is a second correction on top. The calibration-implied 731,512,743 was
# measured at R/G 1.87 (magenta); unity measures R/G 1.06.
NEWR="${3:-512}"; NEWG="${4:-512}"; NEWB="${5:-512}"

LIB=/system/vendor/lib/libcameracustom.so
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

adb -s "$DEVICE" root >/dev/null 2>&1 || true
sleep 3
adb -s "$DEVICE" wait-for-device
adb -s "$DEVICE" remount >/dev/null 2>&1 || adb -s "$DEVICE" shell 'mount -o rw,remount /system'

if [[ "$MODE" == "--restore" ]]; then
    adb -s "$DEVICE" shell "[ -f $LIB.orig ] && cp $LIB.orig $LIB && echo restored || echo 'no backup'"
    exit 0
fi

adb -s "$DEVICE" shell "[ -f $LIB.orig ] || cp $LIB $LIB.orig"
adb -s "$DEVICE" pull "$LIB.orig" "$WORK/in.so" >/dev/null

python3 - "$WORK/in.so" "$WORK/out.so" "$NEWR" "$NEWG" "$NEWB" <<'PYEOF'
import struct, sys

src, dst = sys.argv[1], sys.argv[2]
new = tuple(int(x) for x in sys.argv[3:6])
d = bytearray(open(src, 'rb').read())

BAD  = struct.pack('<III', 891, 512, 610)
GOOD = struct.pack('<III', *new)

n = 0
i = d.find(BAD)
while i >= 0:
    d[i:i + len(BAD)] = GOOD
    n += 1
    print("  patched AWB D65 gain at %#x" % i)
    i = d.find(BAD, i + len(BAD))

if not n:
    sys.exit("no occurrences found - already patched, or blob differs")

open(dst, 'wb').write(d)
print("  %d site(s) rewritten 891,512,610 -> %d,%d,%d" % ((n,) + new))
PYEOF

adb -s "$DEVICE" push "$WORK/out.so" "$LIB" >/dev/null
adb -s "$DEVICE" shell "chmod 644 $LIB; ls -l $LIB"
echo "patched. reboot to apply (AWB tuning is read at cameraserver start)."
