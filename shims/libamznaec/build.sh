#!/usr/bin/env bash
#
# Build libamznaec_shim.so inside the LineageOS tree. The shim links against
# libwebrtc_audio_preprocessing, which the ROM ships in /vendor/lib, so it is
# built as an ordinary vendor module rather than with the -nostdlib recipe the
# cmdq shim uses. Output lands in shims/libamznaec/out/.
#
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="${LINEAGE_TREE:-$HOME/lineage-18.1}"
DEST="$TREE/vendor/jxlarrea/amznaec"
[[ -d "$TREE/build/envsetup.sh" || -f "$TREE/build/envsetup.sh" ]] || { echo "no tree at $TREE (set LINEAGE_TREE)" >&2; exit 1; }
LUNCH="${LUNCH_TARGET:-$(sed -n 's/^PREVIOUS_BUILD_CONFIG := //p' "$TREE"/out/target/product/*/previous_build_config.mk 2>/dev/null | head -1)}"
[[ -n "$LUNCH" ]] || { echo "set LUNCH_TARGET (e.g. lineage_crown-userdebug)" >&2; exit 1; }
mkdir -p "$DEST" "$HERE/out"
cp "$HERE/amznaec_shim.cpp" "$HERE/Android.bp" "$DEST/"
# envsetup.sh is not clean under set -u, so the tree build runs in its own shell.
bash -c 'cd "$1" && source build/envsetup.sh >/dev/null 2>&1 && lunch "$2" >/dev/null 2>&1 && m libamznaec_shim' _ "$TREE" "$LUNCH" > "$HERE/out/build.log" 2>&1 \
    || { tail -30 "$HERE/out/build.log" >&2; echo "build failed (full log: $HERE/out/build.log)" >&2; exit 1; }
grep -E "build completed|error:" "$HERE/out/build.log" | tail -3
PRODUCT="${LUNCH#lineage_}"; PRODUCT="${PRODUCT%-*}"
SO="$(find "$TREE/out/target/product/$PRODUCT" -path '*vendor/lib/libamznaec_shim.so' -not -path '*symbols*' -not -path '*obj/*' | head -1)"
[[ -f "$SO" ]] || { echo "built library not found under out/target/product/$PRODUCT" >&2; exit 1; }
cp "$SO" "$HERE/out/libamznaec_shim.so"
echo "built $HERE/out/libamznaec_shim.so"
