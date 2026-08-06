#!/usr/bin/env bash
#
# Build libcmdqevent_shim.so, the durable form of the cmdq event-id fix.
#
# Linked against the device's own bionic rather than an NDK sysroot so the
# interposed ioctl() is ABI-identical to the one it replaces. Pull the
# libraries off the target first (build.sh does this automatically).
#
set -euo pipefail

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
    echo "usage: $0 <adb-serial>" >&2
    exit 2
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
TREE="${LINEAGE_TREE:-$HOME/lineage-18.1}"
CLANG="$TREE/prebuilts/clang/host/linux-x86/clang-r377782c/bin/clang"
OUT="$HERE/out"

[[ -x "$CLANG" ]] || { echo "clang not found at $CLANG (set LINEAGE_TREE)" >&2; exit 1; }

mkdir -p "$OUT"
for lib in libc.so libdl.so liblog.so; do
    [[ -f "$OUT/$lib" ]] || adb -s "$DEVICE" pull "/system/lib/$lib" "$OUT/$lib" >/dev/null
done

# Regenerate the event map from the kernel sources so it cannot drift.
python3 "$REPO/tools/cmdq-trace/gen_event_map.py" "$TREE/kernel/amazon/mt8163-4.9" >/dev/null
cp "$REPO/tools/cmdq-trace/event_map.h" "$HERE/event_map.h"

# -Wno-builtin-requires-header: the shim interposes fopen and is built
# -nostdlib with its own minimal declarations, so clang's note that fopen
# normally needs <stdio.h> does not apply. It is a warning, not an error,
# but it was mistaken for a build failure, so it is silenced deliberately.
"$CLANG" --target=armv7a-linux-androideabi -march=armv7-a -mthumb -Os \
    -fPIC -shared -nostdlib -fuse-ld=lld -Wl,--no-undefined \
    -Wno-builtin-requires-header \
    -I"$HERE" -o "$OUT/libcmdqevent_shim.so" \
    "$HERE/cmdq_event_shim.c" "$OUT"/lib{c,dl,log}.so

echo "built $OUT/libcmdqevent_shim.so"
