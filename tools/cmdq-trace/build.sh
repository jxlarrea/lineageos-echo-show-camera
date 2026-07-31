#!/usr/bin/env bash
#
# Build the cmdq ioctl tracer and the cameraserver privilege wrapper.
#
# The tracer is an LD_PRELOAD interposer on ioctl() that decodes the
# CMDQ_IOCTL_ASYNC_EXEC / ASYNC_WAIT traffic between the API 25
# libdpframework_cam.so and this kernel's MDP driver. It exists because
# cmdq_ioctl() in cmdq/v2/cmdq_driver.c assigns every handler's result to
# `status` and then unconditionally returns 0, so a failed submit is
# indistinguishable from a successful one at the syscall boundary. The
# tracer recovers the failure by reading back mdp_submit.job_id, which the
# kernel only writes on the success path, and by validating every op_meta
# against the same rules translate_meta() applies.
#
# Both binaries link against the device's own bionic rather than an NDK
# sysroot: pull libc.so, libdl.so and liblog.so off the target first. That
# keeps the interposer's ioctl() ABI-identical to the one it replaces.
#
set -euo pipefail

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial>" >&2
    exit 2
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="${LINEAGE_TREE:-$HOME/lineage-18.1}"
CLANG="$TREE/prebuilts/clang/host/linux-x86/clang-r377782c/bin/clang"
OUT="$HERE/out"

if [[ ! -x "$CLANG" ]]; then
    echo "clang not found at $CLANG (set LINEAGE_TREE)" >&2
    exit 1
fi

mkdir -p "$OUT"

# The target's own libraries, so the interposed ioctl() matches exactly.
for lib in libc.so libdl.so liblog.so; do
    if [[ ! -f "$OUT/$lib" ]]; then
        adb -s "$DEVICE" pull "/system/lib/$lib" "$OUT/$lib" >/dev/null
    fi
done

# armv7a/thumb: cronos runs a 32-bit userspace.
COMMON=(--target=armv7a-linux-androideabi -march=armv7-a -mthumb -Os
        -fPIC -nostdlib -fuse-ld=lld -Wl,--no-undefined)

"$CLANG" "${COMMON[@]}" -shared -o "$OUT/ioctl_hook.so" \
    "$HERE/ioctl_hook.c" "$OUT"/lib{c,dl,log}.so

# -Wl,-e,main because there is no crt0 in a -nostdlib link.
"$CLANG" "${COMMON[@]}" -pie -Wl,-e,main -o "$OUT/camwrap" \
    "$HERE/camwrap.c" "$OUT"/lib{c,log}.so

"$CLANG" "${COMMON[@]}" -pie -Wl,-e,main -o "$OUT/cmdqprobe" \
    "$HERE/cmdqprobe.c" "$OUT"/lib{c,log}.so

"$CLANG" "${COMMON[@]}" -pie -Wl,-e,main -o "$OUT/sensorpoke" \
    "$HERE/sensorpoke.c" "$OUT"/lib{c,log}.so

# ISP interrupt prober: waits on pass-1 IRQ bits from a second fd, using the
# kernel's own accumulate path. This is what measured the 1.78 s vsync period
# that solved the takePicture bug.
"$CLANG" "${COMMON[@]}" -pie -Wl,-e,main -o "$OUT/ispwait" \
    "$HERE/ispwait.c" "$OUT"/lib{c,log}.so

# ISP register read/write through the driver's REG ioctls (CAMINF-relative
# addressing, ISP block at 0x4000).
"$CLANG" "${COMMON[@]}" -pie -Wl,-e,main -o "$OUT/isppoke" \
    "$HERE/isppoke.c" "$OUT"/lib{c,log}.so

echo "built $OUT/ioctl_hook.so $OUT/camwrap $OUT/cmdqprobe $OUT/sensorpoke $OUT/ispwait $OUT/isppoke"
