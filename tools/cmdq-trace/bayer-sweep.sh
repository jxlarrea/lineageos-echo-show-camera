#!/usr/bin/env bash
#
# Measure the preview colour balance for each Bayer order.
#
# SensorOutputDataFormat is read exactly once, when cameraserver enumerates
# sensors at startup, so each value needs cameraserver restarted. That is done
# with a full reboot on purpose: `stop cameraserver` / SIGKILL while the ISP is
# streaming leaves M4U ports pointed at freed buffers, and the resulting EMI
# MPU violation storm livelocks the device hard enough to need a physical power
# cycle. Two devices were lost to that during this work.
#
# Verifies the override actually took effect before trusting any measurement:
# an override that silently never fires is indistinguishable from one that
# fires and changes nothing.
#
set -euo pipefail

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial> [outdir]" >&2
    exit 2
fi
OUTDIR="${2:-logs/bayer-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUTDIR"

wait_boot() {
    until adb -s "$DEVICE" shell "getprop sys.boot_completed" 2>/dev/null | grep -q 1; do
        sleep 5
        adb connect "$DEVICE" >/dev/null 2>&1 || true
    done
    sleep 3
}

for b in 0 1 2 3; do
    echo "=== bayer=$b ==="
    adb -s "$DEVICE" shell "setprop persist.camera.bayer $b; setprop persist.camera.cmdqtrace 1" >/dev/null
    adb -s "$DEVICE" reboot
    sleep 10
    wait_boot

    adb -s "$DEVICE" shell 'input keyevent KEYCODE_WAKEUP; am start -a android.media.action.STILL_IMAGE_CAMERA' >/dev/null 2>&1
    sleep 12

    # Proof the override was applied, not just requested.
    adb -s "$DEVICE" shell "logcat -d -s CmdqEventShim 2>/dev/null | grep getinfo | head -3" \
        > "$OUTDIR/bayer$b.trace" || true
    adb -s "$DEVICE" exec-out screencap -p > "$OUTDIR/bayer$b.png" 2>/dev/null

    echo "  $(head -1 "$OUTDIR/bayer$b.trace" || echo 'NO TRACE')"
done

python3 - "$OUTDIR" <<'EOF'
import sys, pathlib
from PIL import Image
out = pathlib.Path(sys.argv[1])
names = {0: 'RAW_B  (BGGR)', 1: 'RAW_Gb (GBRG)', 2: 'RAW_Gr (GRBG)', 3: 'RAW_R  (RGGB)'}
print()
for b in range(4):
    p = out / f'bayer{b}.png'
    if not p.exists():
        continue
    im = Image.open(p).convert('RGB')
    w, h = im.size
    box = im.crop((int(w * .28), int(h * .05), int(w * .82), int(h * .85)))
    px = list(box.getdata())
    n = len(px)
    r, g, bl = (sum(q[i] for q in px) / n for i in range(3))
    print(f'bayer={b} {names[b]}  R={r:6.1f} G={g:6.1f} B={bl:6.1f}   R/G={r/g:.2f} B/G={bl/g:.2f}')
EOF
echo
echo "results in $OUTDIR"
