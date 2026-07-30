#!/usr/bin/env python3
"""Report the dependency closure of the stock camera stack.

Lists every DT_NEEDED entry the blobs in stock/lib pull in, and says for each
whether it is already on the device or still has to be extracted from the stock
image.

Optional inputs, both produced while surveying:
  stock_filelist.txt  tab-separated path/size listing of the stock image
  device_libs.txt     one library name per line, from `ls` on the device
"""

import os
import re
import subprocess
import sys

STOCK = "stock/lib"
FILELIST = "stock_filelist.txt"
DEVICE_LIBS = "device_libs.txt"


def needed(path):
    out = subprocess.run(["readelf", "-d", path],
                         capture_output=True, text=True).stdout
    return re.findall(r"NEEDED\).*\[(.+?)\]", out)


def load(path, transform=lambda x: x):
    if not os.path.exists(path):
        return set()
    return {transform(line.strip()) for line in open(path) if line.strip()}


def main():
    if not os.path.isdir(STOCK):
        sys.exit("missing %s - run fetch-camera-blobs.sh first" % STOCK)

    have = set(os.listdir(STOCK))
    in_stock_image = load(FILELIST,
                          lambda l: os.path.basename(l.split("\t")[0]))
    on_device = load(DEVICE_LIBS)

    every = set()
    for lib in have:
        every.update(needed(os.path.join(STOCK, lib)))

    outside = sorted(d for d in every if d not in have)

    print("camera libraries: %d" % len(have))
    print("dependencies outside the camera set: %d\n" % len(outside))
    for dep in outside:
        where = []
        if dep in on_device:
            where.append("on device")
        if dep in in_stock_image:
            where.append("in stock image")
        print("  %-32s %s" % (dep, ", ".join(where) or "NOT FOUND"))


if __name__ == "__main__":
    main()
