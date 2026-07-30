# Building this port

Notes from getting a LineageOS 18.1 tree for `cronos` to build from the
published manifests, including two things that are missing or stale upstream.

## Tree setup

Follow the maintainer's own instructions in
[amazon-oss/local_manifests](https://github.com/amazon-oss/local_manifests),
with the corrections below.

```sh
mkdir -p lineage-18.1 && cd lineage-18.1
repo init -u https://github.com/LineageOS/android.git -b lineage-18.1 --depth=1
git clone https://github.com/amazon-oss/local_manifests.git -b lineage-18.1 \
    .repo/local_manifests
repo sync -c --no-clone-bundle --no-tags -j8
./patches/apply.sh          # required, and again after every sync
. build/envsetup.sh
breakfast lineage_cronos-userdebug
```

Put the tree on a volume with room for roughly 150 GB. Prefer `-j8` over
`-j$(nproc)` for the sync; more parallel fetches mostly earns dropped
connections, and see the disk note at the bottom for what those cost.

Debian 13 needed no special handling despite being far newer than this tree.

## Stale manifest entry: hardware/mediatek/mt66xx

`amazon_mt8163.xml` lists `android_hardware_mediatek_mt66xx`, which does not
exist on GitHub (404), so every sync fails on it:

```
GitCommandError: 'fetch ... android_hardware_mediatek_mt66xx failed
stdout: remote: Repository not found.
```

Nothing needs it. No device tree under `device/amazon/` references `mt66xx`, and
cronos, crown and checkers all select `mt76xx` in their `BoardConfig.mk`. The
`kernel/amazon/mt66xx-*` projects are unrelated and do exist.

Work around it without touching the maintainer's clone, in
`.repo/local_manifests/zz-local-fixes.xml` (sorted after `amazon_mt8163.xml` so
the project is defined by the time it is removed):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<manifest>
  <remove-project name="android_hardware_mediatek_mt66xx" />
</manifest>
```

## Missing toolchain: prebuilts/linaro

`device/amazon/mt8163-common/BoardConfigCommon.mk` sets:

```make
TARGET_KERNEL_CROSS_COMPILE_PREFIX := $(shell pwd)/prebuilts/linaro/linux-x86/aarch64/aarch64-linux-gnu/bin/aarch64-linux-gnu-
```

`prebuilts/linaro` is in neither the LineageOS manifest nor the amazon-oss local
manifest, so `m bootimage` fails:

```
aarch64-linux-gnu-gcc: No such file or directory
Cannot use CONFIG_CC_STACKPROTECTOR_STRONG: -fstack-protector-strong not supported by compiler
```

The tree's own `prebuilts/gcc/.../aarch64-linux-android-4.9` is not a substitute
- this kernel rejects it:

```
compiler-gcc.h:157:3: error: #error Sorry, your version of GCC is too old - please use 5.1 or newer.
```

[mt8163/android_prebuilts_linaro](https://github.com/mt8163/android_prebuilts_linaro)
has the right layout and Linaro GCC 6.3.1:

```sh
git clone --depth 1 -b lineage-21.0 \
    https://github.com/mt8163/android_prebuilts_linaro prebuilts/linaro
```

The branch name does not matter; a toolchain is not branch-specific. Adding this
as a manifest project would be the tidier fix.

## Watch the disk

`repo` warns when projects accumulate unoptimized data:

```
warning: Project "platform/prebuilts/rust" is accumulating unoptimized data.
Please run "git repack -a -d" ... or "repo gc --repack"
```

Take it seriously. After a few interrupted syncs, three projects had grown far
past any plausible size:

| Project | Object store | Working tree after a clean refetch |
| --- | --- | --- |
| `prebuilts/rust` | 77 GB | 1.1 GB |
| `prebuilts/tools` | 60 GB | 1.2 GB |
| `prebuilts/android-emulator` | 19 GB | 1.3 GB |

Each interrupted fetch adds another pack without repacking. Repacking needs free
space, so if the volume is already full the way out is to delete the object store
and refetch the project:

```sh
rm -rf .repo/project-objects/platform/prebuilts/rust.git \
       .repo/projects/prebuilts/rust.git prebuilts/rust
repo sync -c --no-clone-bundle --no-tags prebuilts/rust
```

`du` on `.repo` can lag well behind what `df` reports; trust `df`.

## Building only what you need

A full `mka bacon` is rarely necessary while iterating. The camera work needs:

```sh
m libcamera_shim \
  android.hardware.camera.provider@2.4-impl \
  android.hardware.camera.provider@2.4-legacy \
  android.hardware.camera.provider@2.4-external \
  camera.device@1.0-impl camera.device@3.2-impl camera.device@3.3-impl \
  camera.device@3.4-impl camera.device@3.5-impl \
  camera.device@3.4-external-impl camera.device@3.5-external-impl \
  camera.device@3.6-external-impl \
  out/target/product/cronos/system/vendor/etc/vintf/manifest.xml
```

Roughly two minutes once `out/` is warm. Note that module-only builds do not run
`PRODUCT_COPY_FILES`, so vendor blobs are not staged into `out/` - push them from
`vendor/amazon/cronos/proprietary/` instead, which is what
`scripts/install-camera.sh` does.

## Do not trust wrapper exit codes

When running builds through a wrapper that reports its own status, check the log.
Several builds here reported success at the wrapper level while `m` had actually
exited 1. `grep 'FAILED:'` on the log, or check for `#### build completed
successfully ####`.
