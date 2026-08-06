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
. "$(dirname "$0")/device-config.sh"
PROP="$(device_vendor_dir "$TREE")/vendor/lib"
PATCHELF="$TREE/prebuilts/extract-tools/linux-x86/bin/patchelf-0_9"
# The cronos and crown dumps ship a byte identical libdpframework.so;
# checkers ships a different build of the same size. The patcher below
# locates its targets by pattern for that reason, so it does not care which.
RAW="$DUMP_RAW"

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

echo "== applying the three binary patches =="
python3 - "$PROP/$PRIVATE" <<'PYEOF'
import sys

# The patch sites are located by searching for the instruction bytes, not by
# fixed offset. checkers ships a different build of this library from cronos
# and crown (same 296600 bytes, different content), and everything sits at a
# different address there: the abortPoll thunk moves by 0x20 and
# queryEngUsages by 0xe0. Hardcoded offsets made the script abort on checkers
# before it repointed the blobs, which then silently still linked the system
# libdpframework.so. All code is Thumb.
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())


def apply(label, old, new, verify=None):
    """Replace the single occurrence of `old` with `new`, or report why not.

    `old` may be shorter than `new`, in which case it is a prefix: the match
    locates the site and `new` overwrites the full length from there. Anything
    beyond the prefix is checked by `verify` rather than compared literally.
    """
    n, already = d.count(old), d.count(new)
    if n == 1:
        off = d.find(old)
        if verify:
            err = verify(off)
            if err:
                sys.exit("  %s: matched at 0x%x but %s" % (label, off, err))
        d[off:off + len(new)] = new
        print("  patched %s at 0x%x" % (label, off))
    elif n == 0 and already >= 1:
        print("  %s already patched" % label)
    else:
        sys.exit("  %s: expected 1 match, found %d (and %d already patched). "
                 "This is not a libdpframework.so this script knows how to "
                 "patch." % (label, n, already))


# 1. Device node path: the old library opens /proc/mtk_cmdq, but this kernel
#    creates /dev/mtk_cmdq. Without this every open fails with "can't open
#    display driver". NUL padding preserved so the string stays the same size.
apply("device node path /proc/mtk_cmdq -> /dev/mtk_cmdq",
      b"/proc/mtk_cmdq\x00", b"/dev/mtk_cmdq\x00\x00")

# 2. DpPortAdapt::abortPoll made a no-op. The thunk
#    (ldr r0,[r0,#0x20]; ldr r2,[r0]; ldr r2,[r2,#0x3c]; bx r2) dereferences
#    a buffer pool pointer that is already freed on the teardown path,
#    crashing dequeueDstBuffer's error handling. Aborting an in-flight poll
#    early is an optimization, not a correctness requirement.
#    Replacement: movs r0,#0; bx lr; nop; nop.
apply("DpPortAdapt::abortPoll thunk -> no-op",
      bytes.fromhex("006a0268d26b1047"),
      bytes.fromhex("0020704700bf00bf"))

# 3. DpDriver::queryEngUsages(uint[36]) made to report all engines idle.
#    The stock code issues CMDQ_IOCTL_QUERY_USAGE (_IOW('x', 4, 144)), an
#    ioctl this kernel's MDP driver does not service compatibly, leaving the
#    usage array garbage. Replacement zero-fills the caller's 36-word array:
#    movs r0,#0; mov r1,sp; movs r2,#36; loop: str r0,[r1],#4; subs r2,#1;
#    bne loop; nop.
#
#    Only the first 12 bytes are matched. The last four are the BLX to the
#    ioctl helper, and a BLX encodes its target as a relative offset, so those
#    bytes legitimately differ between builds - they are f0f7b6ea on cronos
#    and f0f73cea on checkers. Matching them too is what made this patch miss.
#    The replacement overwrites the call regardless of where it pointed, so
#    the tail is checked to be a BL/BLX rather than compared literally.
def is_branch_link(off):
    hw2 = int.from_bytes(d[off + 14:off + 16], "little")
    if (hw2 & 0xC000) != 0xC000:
        return "the trailing instruction is not a BL/BLX (%s)" % d[off + 12:off + 16].hex()
    return None


apply("DpDriver::queryEngUsages -> report idle",
      bytes.fromhex("286847f60401c4f290016a46"),          # 12-byte prefix
      bytes.fromhex("00206946242241f8040b013afbd100bf"),  # 16-byte replacement
      verify=is_branch_link)

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
