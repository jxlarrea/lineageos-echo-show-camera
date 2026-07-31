#!/usr/bin/env bash
#
# This is the CURRENT approach. An adapter over the Android 9 library
# (shims/libdpframework_cam/) was tried and shelved: the new library only
# accepts ION buffers while the camera blobs pass malloc'd virtual addresses
# ("not support alloc by va"). The API 25 library under a private soname,
# plus the two binary patches below, is what works.
#
# Give the camera blobs their own matching-vintage copy of libdpframework.
#
# libcam.campipe.so and friends are API 25 and allocate a DpIspStream sized to
# the Android 7 definition. The libdpframework.so this ROM ships comes from the
# Android 9 blob set, and its constructor initialises a larger object, so it
# writes past the caller's allocation:
#
#   #00 DpIspStream::DpIspStream(ISPStreamType)+264   libdpframework.so
#   #01 NSCamPipe::XdpPipe::init()+16                 libcam.campipe.so
#
# A signature shim cannot fix that - the allocation is already too small. But
# the coupling that makes downgrading the system library impossible (display and
# media depend on the new one) does not apply if only the camera gets the old
# one: install the API 25 build under a private soname and rewrite the camera
# blobs to link that instead.
#
set -euo pipefail

TREE="${1:-${LINEAGE_TREE:-$HOME/lineage-18.1}}"
PROP="$TREE/vendor/amazon/cronos/proprietary/vendor/lib"
PATCHELF="$TREE/prebuilts/extract-tools/linux-x86/bin/patchelf-0_9"
DUMP_BRANCH="cronos-user-6.0-NS6573-6567-amz-p,release-keys"
RAW="https://raw.githubusercontent.com/el-vertedero/amazon_cronos_dump/$DUMP_BRANCH"

PRIVATE=libdpframework_cam.so

[[ -x "$PATCHELF" ]] || { echo "patchelf missing at $PATCHELF" >&2; exit 1; }

# Blobs that link libdpframework. Derived by scanning DT_NEEDED.
CONSUMERS=(
    libcam.camadapter.so libcam.campipe.so libcam.camshot.so libcam.client.so
    libcam.hal3a.v3.so libcam.iopipe.so libcam.iopipe_FrmB.so
    libfeatureio.so libimageio_plat_drv.so libimageio_plat_drv_FrmB.so
)

if [[ ! -f "$PROP/$PRIVATE" ]]; then
    echo "== fetching the API 25 libdpframework =="
    curl -fsSL -o "$PROP/$PRIVATE" "$RAW/system/vendor/lib/libdpframework.so"
    echo "== renaming its soname to $PRIVATE =="
    "$PATCHELF" --set-soname "$PRIVATE" "$PROP/$PRIVATE"
fi
"$PATCHELF" --print-soname "$PROP/$PRIVATE"

echo "== applying the two binary patches =="
python3 - "$PROP/$PRIVATE" <<'PYEOF'
import sys

# Both patches target the stock API 25 libdpframework.so from the cronos dump.
# File offset = vaddr - 0x1000 in this library; all code is Thumb.
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())

# 1. Device node path: the old library opens /proc/mtk_cmdq, but this kernel
#    creates /dev/mtk_cmdq. Without this every open fails with "can't open
#    display driver". One occurrence, NUL padding preserved.
old = b"/proc/mtk_cmdq\x00"
new = b"/dev/mtk_cmdq\x00\x00"
n = d.count(old)
if n == 1:
    d = d.replace(old, new)
    print("  patched device node path /proc/mtk_cmdq -> /dev/mtk_cmdq")
elif d.count(new) == 1:
    print("  device node path already patched")
else:
    sys.exit("  unexpected: %d occurrences of the /proc path" % n)

# 2. DpPortAdapt::abortPoll made a no-op. The thunk at vaddr 0x14b2c
#    (ldr r0,[r0,#0x20]; ldr r2,[r0]; ldr r2,[r2,#0x3c]; bx r2) dereferences
#    a buffer pool pointer that is already freed on the teardown path,
#    crashing dequeueDstBuffer's error handling. Aborting an in-flight poll
#    early is an optimization, not a correctness requirement.
#    Replacement: movs r0,#0; bx lr; nop; nop.
off = 0x14b2c - 0x1000
old = bytes.fromhex("00 6a 02 68 d2 6b 10 47".replace(" ", ""))
new = bytes.fromhex("00 20 70 47 00 bf 00 bf".replace(" ", ""))
cur = bytes(d[off:off + 8])
if cur == old:
    d[off:off + 8] = new
    print("  patched DpPortAdapt::abortPoll thunk at 0x14b2c to a no-op")
elif cur == new:
    print("  abortPoll thunk already patched")
else:
    sys.exit("  unexpected bytes at 0x13b2c: %s" % cur.hex())

# 3. DpDriver::queryEngUsages(uint[36]) made to report all engines idle.
#    The stock code issues CMDQ_IOCTL_QUERY_USAGE (_IOW('x', 4, 144)), an
#    ioctl this kernel's MDP driver does not service compatibly, leaving the
#    usage array garbage. Replacement zero-fills the caller's 36-word array:
#    movs r0,#0; mov r1,sp; movs r2,#36; loop: str r0,[r1],#4; subs r2,#1;
#    bne loop; nop.
off = 0x1d696 - 0x1000
old = bytes.fromhex("2868 47f6 0401 c4f2 9001 6a46 f0f7 b6ea".replace(" ", ""))
new = bytes.fromhex("0020 6946 2422 41f8 040b 013a fbd1 00bf".replace(" ", ""))
cur = bytes(d[off:off + 16])
if cur == old:
    d[off:off + 16] = new
    print("  patched DpDriver::queryEngUsages at 0x1d696 to report idle")
elif cur == new:
    print("  queryEngUsages already patched")
else:
    sys.exit("  unexpected bytes at 0x1c696: %s" % cur.hex())

open(p, 'wb').write(bytes(d))
PYEOF

echo "== repointing the camera blobs =="
for lib in "${CONSUMERS[@]}"; do
    [[ -f "$PROP/$lib" ]] || { echo "missing $lib" >&2; exit 1; }
    if "$PATCHELF" --print-needed "$PROP/$lib" | grep -qx "$PRIVATE"; then
        continue
    fi
    "$PATCHELF" --replace-needed libdpframework.so "$PRIVATE" "$PROP/$lib"
    echo "   $lib"
done

echo
echo "done. push $PRIVATE and the rewritten blobs with scripts/install-camera.sh"
