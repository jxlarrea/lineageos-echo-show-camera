# libdpframework_cam adapter - SHELVED, do not install

This directory is **not part of the working installation**. It is kept for
the record because the approach is sound and the analysis in
[docs/findings.md](../../docs/findings.md) refers to it.

## What it is

An adapter that lets the API 25 camera blobs call this ROM's Android 9
`libdpframework.so`. The camera blobs allocate a `DpIspStream` sized to the
Android 7 definition while the Android 9 constructor initialises a larger
object, so it writes past the caller's allocation. The adapter gives the
caller's undersized object a single pointer to a real heap-allocated
Android 9 object and forwards every method. 24 of the 28 imported symbols
are byte-identical between versions; the other 4 are adapted here.

## Why it is shelved

It works up to the point of moving pixels. The Android 9 library only
accepts ION buffers, while the old camera stack hands the display framework
plain malloc'd virtual addresses:

```
not support alloc by va
```

Bridging that inside the adapter would mean allocating shadow ION buffers
and copying every frame in both directions. The working solution instead
gives the camera blobs their own copy of the API 25 library under a private
soname, with three small binary patches for the kernel interface drift -
see `scripts/install-private-dpframework.sh`.

## If you build this by mistake

The Soong module here is named `libdpframework_cam`, the same soname the
working private library uses. Building it produces an ~11 KB adapter that
will shadow the ~333 KB patched library and the camera will fail to move
frames. Do not add it to `PRODUCT_PACKAGES`, and do not copy this directory
into the device tree.
