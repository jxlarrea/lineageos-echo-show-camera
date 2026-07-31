#!/usr/bin/env bash
#
# Place the fetched camera blobs into the vendor tree.
#
# The usual route for proprietary files is extract-files.sh against a stock
# image, which means downloading a full firmware dump. The camera stack does
# not need that: fetch-camera-blobs.sh pulls exactly the 45 libraries listed
# in patches/cronos-camera-proprietary-files.txt from the public dump, and
# this maps them onto the destination paths that list declares (the stock
# layout is pre-Treble, so /system/vendor/lib becomes /vendor/lib and
# camera.mt8163.so lands in /vendor/lib/hw).
#
# Run after fetch-camera-blobs.sh and before patch-shim-needed.sh.
#
set -euo pipefail

TREE="${1:-${LINEAGE_TREE:-$HOME/lineage-18.1}}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$HERE/stock/lib"
LIST="$HERE/patches/cronos-camera-proprietary-files.txt"
DEST="$TREE/vendor/amazon/cronos/proprietary"

if [[ ! -d "$SRC" ]]; then
    echo "no blobs at $SRC - run scripts/fetch-camera-blobs.sh first" >&2
    exit 1
fi
if [[ ! -d "$TREE" ]]; then
    echo "build tree not found at $TREE" >&2
    echo "usage: $0 [build-tree]" >&2
    exit 2
fi

copied=0
while read -r rel; do
    [[ -n "$rel" ]] || continue
    name="$(basename "$rel")"
    # libdpframework_cam.so is not in the dump: it is derived from the stock
    # libdpframework.so by install-private-dpframework.sh, which runs next.
    if [[ "$name" == "libdpframework_cam.so" ]]; then
        continue
    fi
    if [[ ! -f "$SRC/$name" ]]; then
        echo "missing: $SRC/$name (re-run fetch-camera-blobs.sh)" >&2
        exit 1
    fi
    mkdir -p "$DEST/$(dirname "$rel")"
    cp "$SRC/$name" "$DEST/$rel"
    copied=$((copied + 1))
done < <(grep -E '^vendor/' "$LIST")

echo "placed $copied blobs into $DEST"
echo "next: scripts/patch-shim-needed.sh $TREE"
