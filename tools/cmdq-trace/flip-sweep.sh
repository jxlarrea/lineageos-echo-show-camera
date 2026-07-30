#!/usr/bin/env bash
#
# Find the OV02B10 mirror/flip register empirically.
#
# The reference driver left set_mirror_flip() stubbed out, so the register is
# undocumented in every source available. The driver now takes the page and
# register as module parameters and re-applies them on every mode set, so
# candidates can be tried by reopening the camera app - no kernel rebuild, no
# boot flash, and no cameraserver restart (which livelocks the device).
#
# A hit is detected by comparing each capture against the 180-degree rotation
# of the baseline: a working flip makes the rotated baseline the better match.
#
set -euo pipefail

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial> [outdir]" >&2
    exit 2
fi
OUTDIR="${2:-logs/flip-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUTDIR"

PARAM=/sys/module/ov02b10mipiraw_Sensor/parameters

shot() {  # shot <file>
    adb -s "$DEVICE" shell 'am force-stop com.android.camera2' >/dev/null 2>&1
    sleep 1
    adb -s "$DEVICE" shell 'input keyevent KEYCODE_WAKEUP; am start -a android.media.action.STILL_IMAGE_CAMERA' >/dev/null 2>&1
    sleep 9
    adb -s "$DEVICE" exec-out screencap -p > "$1" 2>/dev/null
}

# Baseline with the flip disabled.
adb -s "$DEVICE" shell "echo 0 > $PARAM/ov02b10_mirror" >/dev/null 2>&1
shot "$OUTDIR/baseline.png"
echo "baseline captured"

for page in 0 1 2 3; do
    for reg in 3f 12; do
        adb -s "$DEVICE" shell "
            echo $page   > $PARAM/ov02b10_flip_page
            echo \$((0x$reg)) > $PARAM/ov02b10_flip_reg
            echo 3       > $PARAM/ov02b10_mirror
        " >/dev/null 2>&1
        shot "$OUTDIR/p${page}_r${reg}.png"
        echo "captured page=$page reg=0x$reg"
    done
done

# Leave the sensor unflipped until a winner is known.
adb -s "$DEVICE" shell "echo 0 > $PARAM/ov02b10_mirror" >/dev/null 2>&1

python3 - "$OUTDIR" <<'EOF'
import sys, pathlib
from PIL import Image, ImageChops

out = pathlib.Path(sys.argv[1])
base = Image.open(out / 'baseline.png').convert('L')
w, h = base.size
box = (int(w * .28), int(h * .05), int(w * .82), int(h * .85))
b = base.crop(box)
b180 = b.rotate(180)

def rms(a, c):
    d = ImageChops.difference(a, c).getdata()
    return (sum(x * x for x in d) / len(d)) ** .5

print()
print("%-14s %8s %8s   %s" % ("candidate", "vs base", "vs base180", "verdict"))
for p in sorted(out.glob('p*_r*.png')):
    im = Image.open(p).convert('L').crop(box)
    d0, d180 = rms(im, b), rms(im, b180)
    verdict = "FLIPPED" if d180 < d0 * 0.8 else ("changed" if d0 > 6 else "no change")
    print("%-14s %8.1f %8.1f   %s" % (p.stem, d0, d180, verdict))
EOF
echo
echo "images in $OUTDIR"
