#!/usr/bin/env bash
#
# Let AOSP's prebuilt clang run on distributions without ncurses 5.
#
# LineageOS 18.1 ships clang binaries linked against libncurses.so.5 and
# libtinfo.so.5. Debian 13 (and other current releases) dropped those
# packages entirely - libncurses5 and libtinfo5 are not in the archive any
# more - so the build dies partway through with:
#
#   llvm-as: error while loading shared libraries: libncurses.so.5:
#   cannot open shared object file: No such file or directory
#
# It surfaces around 3% of a full build, in the RenderScript bitcode step,
# long after everything looks fine.
#
# The binaries carry RPATH $ORIGIN/../lib64, so a symlink beside them is
# enough. That keeps the workaround inside the tree: no root, nothing
# installed system-wide, and it disappears with the tree.
#
# ncurses 6 is ABI-compatible for what these tools use (terminal capability
# lookups for diagnostics).
#
set -euo pipefail

TREE="${1:-${LINEAGE_TREE:-$HOME/lineage-18.1}}"
CLANG_DIR="$TREE/prebuilts/clang/host/linux-x86"

if [[ ! -d "$CLANG_DIR" ]]; then
    echo "no prebuilt clang at $CLANG_DIR" >&2
    echo "usage: $0 [build-tree]" >&2
    exit 2
fi

# Find the real ncurses/tinfo on this host, whatever the multiarch dir is.
find_lib() {
    local soname="$1" hit
    for d in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib /lib/x86_64-linux-gnu; do
        hit="$(ls "$d/$soname".* 2>/dev/null | sort -V | tail -1 || true)"
        [[ -n "$hit" ]] && { echo "$hit"; return 0; }
    done
    return 1
}

NCURSES="$(find_lib libncurses.so || true)"
TINFO="$(find_lib libtinfo.so || true)"

if [[ -z "$NCURSES" || -z "$TINFO" ]]; then
    echo "could not find libncurses/libtinfo on this host." >&2
    echo "install the ncurses runtime (libncurses6, libtinfo6) and retry." >&2
    exit 1
fi

linked=0
for d in "$CLANG_DIR"/*/; do
    [[ -d "$d/lib64" ]] || continue
    # Only toolchains that actually want the old soname. grep -c rather
    # than grep -q: -q exits on the first match, readelf takes SIGPIPE, and
    # under `set -o pipefail` that turns a successful match into a failed
    # pipeline.
    hits="$(readelf -d "$d"bin/* 2>/dev/null | grep -c 'libncurses\.so\.5\|libtinfo\.so\.5' || true)"
    [[ "$hits" -gt 0 ]] || continue
    ln -sf "$NCURSES" "$d/lib64/libncurses.so.5"
    ln -sf "$TINFO" "$d/lib64/libtinfo.so.5"
    echo "   $(basename "$d")"
    linked=$((linked + 1))
done

if (( linked == 0 )); then
    echo "no toolchain needed the compatibility symlinks (nothing to do)"
    exit 0
fi

echo
echo "linked ncurses 5 compatibility into $linked toolchain(s):"
echo "   libncurses.so.5 -> $NCURSES"
echo "   libtinfo.so.5   -> $TINFO"
