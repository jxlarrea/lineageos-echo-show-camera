#!/usr/bin/env python3
"""Report symbols the stock camera blobs import that this device cannot provide.

Reads the blobs from stock/lib and the device's platform libraries from devlib,
both populated by the sibling fetch scripts.

Symbol table lines are parsed from the right rather than by column index: libc
exports its string functions as ifuncs, which widens the type column and shifts
every following field, making those symbols look absent when they are not.
"""

import collections
import os
import subprocess
import sys

STOCK = "stock/lib"
DEVICE = "devlib"


def symbols(path):
    """Return (undefined, defined) symbol name sets for an ELF file."""
    out = subprocess.run(["readelf", "--dyn-syms", "-W", path],
                         capture_output=True, text=True).stdout
    undefined, defined = set(), set()
    for line in out.splitlines():
        fields = line.split()
        # Symbol table rows start with "N:" and end with the symbol name,
        # optionally followed by a "(N)" version reference.
        if len(fields) < 8 or not fields[0].endswith(":"):
            continue
        if fields[-1].startswith("(") and fields[-1].endswith(")"):
            fields = fields[:-1]
        name = fields[-1].split("@")[0]
        if not name:
            continue
        # The section index sits immediately before the name.
        if fields[-2] == "UND":
            undefined.add(name)
        else:
            defined.add(name)
    return undefined, defined


def main():
    for d in (STOCK, DEVICE):
        if not os.path.isdir(d):
            sys.exit("missing %s - run the fetch scripts first" % d)

    provided = set()
    for f in os.listdir(DEVICE):
        provided |= symbols(os.path.join(DEVICE, f))[1]

    imports = {}
    for f in sorted(os.listdir(STOCK)):
        undefined, defined = symbols(os.path.join(STOCK, f))
        imports[f] = undefined
        provided |= defined  # the camera stack satisfies much of itself

    missing = collections.defaultdict(set)
    for f, undefined in imports.items():
        for s in undefined:
            if s not in provided:
                missing[f].add(s)

    every = set()
    for v in missing.values():
        every |= v

    print("camera libraries analysed: %d" % len(imports))
    print("unresolved symbols: %d\n" % len(every))
    for f in sorted(missing):
        if missing[f]:
            print(f)
            for s in sorted(missing[f]):
                print("    %s" % s)


if __name__ == "__main__":
    main()
