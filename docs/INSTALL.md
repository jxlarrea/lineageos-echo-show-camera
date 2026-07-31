# Step-by-step installation

This walks from a stock unofficial LineageOS 18.1 install to a fully working
camera, with every command spelled out. Read the whole document once before
starting; the [safety rules](#safety-rules) at the bottom are not optional.

Throughout, replace:

- `<serial>` with your device's adb serial (for network adb this looks like
  `192.168.0.50:5555`)
- `~/lineage-18.1` with wherever you put the source tree
- `~/lineageos-echo-show-camera` with wherever you cloned this repository

Time budget: the source tree sync and first ROM build dominate (hours,
depending on hardware and bandwidth). Everything else is minutes.

## Step 0: starting point, host dependencies, and backups

You need an Echo Show 5 2nd gen (`cronos`) that:

- runs unofficial LineageOS 18.1 from [amazon-oss](https://github.com/amazon-oss)
- was unlocked with amonet and still boots TWRP
- has adb access with root (`adb root` works)

Verify:

```sh
adb -s <serial> shell getprop ro.product.device       # cronos
adb -s <serial> shell getprop ro.build.version.release # 11
adb -s <serial> root && adb -s <serial> shell id      # uid=0(root)
```

If any of that fails, stop and fix it first. If you cannot boot TWRP, do not
proceed at all: a bad flash with no recovery path bricks the device.

### Host dependencies

On Debian/Ubuntu (Debian 13 is what this was developed on):

```sh
sudo apt update
sudo apt install -y \
    bc bison build-essential ccache curl flex g++-multilib gcc-multilib \
    git git-lfs gnupg gperf imagemagick libelf-dev liblz4-tool \
    libncurses-dev libsdl1.2-dev libssl-dev libxml2 libxml2-utils lzop \
    pngcrush python3 python-is-python3 rsync schedtool squashfs-tools \
    unzip xsltproc zip zlib1g-dev adb
```

Then initialise Git LFS **before syncing anything**:

```sh
git lfs install
```

Some LineageOS projects store large prebuilts through LFS -
`external/chromium-webview` is the one that matters here. Without the
filter active at checkout time, `repo sync` writes 133-byte pointer files
instead of the real payloads, and the build runs happily for an hour
before failing at 97% with `failed opening zip: Invalid file`.

Plus the `repo` tool if you do not have it:

```sh
mkdir -p ~/.local/bin
curl -o ~/.local/bin/repo https://storage.googleapis.com/git-repo-downloads/repo
chmod +x ~/.local/bin/repo
export PATH=~/.local/bin:$PATH
```

On older releases some 32-bit dev packages carry different names
(`lib32ncurses5-dev`, `lib32z1-dev`); install whatever your release calls
them if the build later complains about missing 32-bit libraries.

### Back up the device first

Do this before touching anything. The scripts here back up what they
modify, but a full pre-project backup is what saves you if something goes
sideways in a way nobody predicted.

1. **Full TWRP backup.** Boot into TWRP, choose Backup, select at least
   Boot, System and Vendor, and back up to external storage (or to
   `/sdcard` and then pull it). This is your restore point for everything
   the ROM layers touch.

2. **Raw boot partition image**, pulled to your host:

```sh
adb -s <serial> root
adb -s <serial> shell 'dd if=/dev/block/by-name/boot of=/sdcard/boot-preproject.img'
adb -s <serial> pull /sdcard/boot-preproject.img ./boot-preproject.img
adb -s <serial> shell 'rm /sdcard/boot-preproject.img'
```

   Keep this file somewhere safe. It preserves the amonet exploit layout
   exactly as it is on your working device, so restoring it from TWRP
   (`dd` it back to `/dev/block/platform/soc/by-name/boot`) recovers from
   any bad boot flash.

3. **Know your recovery path before you need it.** Confirm you can
   actually get into TWRP right now (power + volume key combination, or
   `adb reboot recovery`), not just that it is installed.

## Step 1: clone this repository

```sh
git clone https://github.com/jxlarrea/lineageos-echo-show-camera.git \
    ~/lineageos-echo-show-camera
cd ~/lineageos-echo-show-camera
```

## Step 2: set up the LineageOS source tree

Follow [docs/building.md](building.md) in full; it documents two upstream
manifest problems you will hit otherwise. Condensed:

The tree needs **about 150 GB**, and the build needs tens more. Check
before you start:

```sh
df -h ~
```

If your home directory does not have it, put the tree on a volume that
does and symlink it, so every command below still works verbatim:

```sh
sudo mkdir -p /big/volume/lineage-18.1
sudo chown "$USER:$USER" /big/volume/lineage-18.1
ln -sfn /big/volume/lineage-18.1 ~/lineage-18.1
```

```sh
mkdir -p ~/lineage-18.1 && cd ~/lineage-18.1
repo init -u https://github.com/LineageOS/android.git -b lineage-18.1 --depth=1
git clone https://github.com/amazon-oss/local_manifests.git -b lineage-18.1 \
    .repo/local_manifests

# Manifest corrections, WITHOUT which the sync fails: one project in the
# upstream manifest no longer exists on GitHub. The filename sorts after
# amazon_mt8163.xml so repo has defined the projects before removing them.
cp ~/lineageos-echo-show-camera/patches/local-manifest-fixes.xml \
    .repo/local_manifests/zz-local-fixes.xml

repo sync -c --no-clone-bundle --no-tags -j8

# The maintainer's own patch script, from the local_manifests clone - not
# this repository's patches/. Required, and again after every repo sync.
./patches/apply.sh

. build/envsetup.sh
breakfast lineage_cronos-userdebug
```

[docs/building.md](building.md) explains each manifest correction and the
missing-toolchain problem you may hit next.

The sync moves roughly 75 GB and takes hours; `-j8` is deliberate, since
more parallel fetches mostly earn dropped connections. The build then adds
tens of GB more on top.

Sanity-check the sync before moving on - a tree that is missing projects
fails much later, in a way that does not name the cause:

```sh
for d in build frameworks/av hardware/interfaces prebuilts/tools \
         prebuilts/clang kernel/amazon/mt8163-4.9 device/amazon/cronos; do
    [ -d "$d" ] || echo "MISSING: $d"
done
```

`prebuilts/tools` in particular carries jars that dozens of modules
depend on; without it Soong stops in seconds with a wall of "depends on
undefined module".

Check that the LFS payloads really arrived, too:

```sh
file external/chromium-webview/prebuilt/arm/webview.apk
# Android package (APK) - NOT "ASCII text"
```

If it says ASCII text, LFS was not active during the sync. Fix it in
place rather than re-syncing:

```sh
git lfs install
cd external/chromium-webview/prebuilt/arm && git lfs install --local && git lfs pull
```

## Step 3: prepare the kernel

Three things go into the kernel, in this exact order. Run each command
separately and read its output.

```sh
cd ~/lineage-18.1/kernel/amazon/mt8163-4.9

# 3.1  Amazon's kernel struct layouts (every Echo Show device needs this;
#      without it cameraserver crash-loops on the first ioctl)
patch -p1 < ~/lineageos-echo-show-camera/patches/0001-imgsensor-use-the-Amazon-struct-layouts-on-all-echo-show.patch

# 3.2  The OV02B10 sensor driver itself (cronos only). This is a complete
#      driver directory, shipped as a tarball rather than a patch:
tar xzf ~/lineageos-echo-show-camera/patches/ov02b10_mipi_raw-driver.tar.gz \
    -C drivers/misc/mediatek/imgsensor/src/mt8163/

# 3.3  Wire the driver into the build: sensor IDs, sensor list entry, and
#      the defconfig (cronos only)
patch -p1 < ~/lineageos-echo-show-camera/patches/0004-imgsensor-ov02b10-driver-support.patch
```

Each `patch` command lists the files it modified. If you see `FAILED` or a
`.rej` file is created, you are in the wrong directory or the wrong tree
state - stop and sort that out before building.

Verify:

```sh
grep CONFIG_CUSTOM_KERNEL_IMGSENSOR arch/arm64/configs/cronos_defconfig
# CONFIG_CUSTOM_KERNEL_IMGSENSOR="ov9734_mipi_raw ov02b10_mipi_raw"

ls drivers/misc/mediatek/imgsensor/src/mt8163/ov02b10_mipi_raw/
# Makefile  ov02b10mipiraw_Sensor.c  ov02b10mipiraw_Sensor.h
```

Things NOT to do in this step, because each looks plausible:

- **Do not apply patches 0007, 0008 or 0010.** They document how the
  driver fixes were developed; the tarball from 3.2 already contains all
  of them, and applying them on top fails with "already applied".
- **Do not apply 0002.** It is a narrower alternative to 0001 kept for
  the record; the two conflict and will not compile together.
- **Do not edit the defconfig by hand.** 0004 sets it to compile both
  sensor drivers, which is the verified configuration: the HAL binds the
  OV02B10 correctly, and keeping the OV9734 driver compiled costs
  nothing.
- 0003 is driver bring-up debug logging; skip it for a normal build.

For `checkers`/`crown` (OV9734 devices): apply only 0001 and skip 3.2 and
3.3 entirely; their sensor driver is already in the tree and selected.

## Step 4: apply the ROM patches

Three patches against three different repositories in the tree:

```sh
cd ~/lineage-18.1/hardware/interfaces
patch -p1 < ~/lineageos-echo-show-camera/patches/0005-*.patch

cd ~/lineage-18.1/frameworks/av
patch -p1 < ~/lineageos-echo-show-camera/patches/0006-*.patch
patch -p1 < ~/lineageos-echo-show-camera/patches/0013-*.patch

cd ~/lineage-18.1
patch -p1 < ~/lineageos-echo-show-camera/patches/0011-*.patch
patch -p1 < ~/lineageos-echo-show-camera/patches/0012-*.patch
```

0013 stops `cameraserver` starting at boot. That matters: the ROM ships
the whole camera stack, but it cannot work until step 9 installs the shim,
and a client that opens the camera before then wedges the device (see the
warning in step 8). `install-cmdq-event-shim.sh` re-enables it.

0012 is a host-environment fix rather than a camera one: it lets
`MKE2FS_CONFIG` reach `mke2fs`, without which the ART apex fails to build
on any distribution shipping a recent e2fsprogs (see step 7).

0011 changes the device tree: the camera provider declaration in the vintf
manifest, the provider packages, the front-camera feature file, the HAL1
dynamic native handle flag, and the sensor orientation override.
[patches/README.md](../patches/README.md) explains each change.

## Step 5: install the compatibility shim into the tree

The 11-symbol shim is built by the ROM as a normal Soong module. Copy it
into the common device tree, where 0011's `PRODUCT_PACKAGES += libcamera_shim`
expects it:

```sh
cp -r ~/lineageos-echo-show-camera/shims/libcamera_shim \
    ~/lineage-18.1/device/amazon/mt8163-common/
```

## Step 6: camera blobs into the vendor tree

This is the layer the ROM cannot build from source: 45 proprietary
libraries from the stock firmware, plus one derived from them. It all
happens **before** the build, so the ROM you flash already contains it.

```sh
cd ~/lineageos-echo-show-camera

# 6.1  Declare the blobs to the build, then regenerate the vendor
#      makefiles from that list (appending alone does nothing - the build
#      reads the generated vendor/amazon/cronos/*.mk, not this file)
cat patches/cronos-camera-proprietary-files.txt \
    >> ~/lineage-18.1/device/amazon/cronos/proprietary-files.txt
(cd ~/lineage-18.1/device/amazon/cronos && ./setup-makefiles.sh)

# 6.2  Download the stock camera stack from the public firmware dump
#      (45 libraries, ~17 MB, into stock/lib). These are proprietary
#      Amazon/MediaTek binaries: stock/ is gitignored, keep it that way.
scripts/fetch-camera-blobs.sh

# 6.3  Place them at the paths the blob list declares
scripts/install-blobs-to-tree.sh ~/lineage-18.1

# 6.4  Build the private display framework: fetch the stock API 25
#      libdpframework.so, give it a private soname, apply three binary
#      patches, and repoint the ten blobs that link it
scripts/install-private-dpframework.sh ~/lineage-18.1

# 6.5  Add the compatibility shim to the blobs' DT_NEEDED, so the linker
#      resolves the 11 removed symbols through it
scripts/patch-shim-needed.sh ~/lineage-18.1
```

Order matters: 6.4 creates `libdpframework_cam.so`, and 6.5 must run after
it so the repointed libraries also get the shim entry.

Verify:

```sh
ls ~/lineage-18.1/vendor/amazon/cronos/proprietary/vendor/lib/libdpframework_cam.so
# should be about 333 KB - if it is ~11 KB you built the shelved adapter
# from shims/libdpframework_cam/ instead; see that directory's README

~/lineage-18.1/prebuilts/extract-tools/linux-x86/bin/patchelf-0_9 --print-needed \
    ~/lineage-18.1/vendor/amazon/cronos/proprietary/vendor/lib/libcam.client.so \
    | grep -E 'libcamera_shim|libdpframework'
# libcamera_shim.so
# libdpframework_cam.so
```

If `libcam.client.so` still says plain `libdpframework.so`, step 6.4 did
not run against this tree.

```sh
grep -c 'libcam\|dpframework_cam' ~/lineage-18.1/vendor/amazon/cronos/cronos-vendor.mk
# a non-zero count: the generated vendor makefile knows about the blobs
```

If that count is 0, `setup-makefiles.sh` in 6.1 did not run, and the build
will silently produce a ROM with no camera blobs in it.

## Step 7: build

On a current distribution, first make AOSP's prebuilt clang runnable. It
is linked against `libncurses.so.5` / `libtinfo.so.5`, which Debian 13 and
similar releases no longer package at all. Without this the build dies
around 3%, in the RenderScript step, with `error while loading shared
libraries: libncurses.so.5`:

```sh
~/lineageos-echo-show-camera/scripts/fix-ncurses5-prebuilts.sh ~/lineage-18.1
```

It symlinks the ncurses 6 you do have next to the binaries that want
version 5 (they carry `RPATH $ORIGIN/../lib64`), so nothing is installed
system-wide and no root is needed. It covers both the prebuilt toolchains
and `out/soong/host/linux-x86/lib64`, where the build puts host tools it
compiles itself - `llvm-tblgen` hits the same wall around 51%. If a later
build reports the same error (a `make clean` wipes `out/`), just run it
again.

```sh
cd ~/lineage-18.1
. build/envsetup.sh
lunch lineage_cronos-userdebug

# The tree's mke2fs is from 2019 and reads the host's /etc/mke2fs.conf,
# which on a current distribution enables features it does not know
# (orphan_file, metadata_csum_seed). Building the ART apex then dies with
# "Invalid filesystem option set". The tree ships a compatible config:
export MKE2FS_CONFIG=$PWD/system/extras/ext4_utils/mke2fs.conf

mka bacon           # full ROM zip; hours on the first build
```

`MKE2FS_CONFIG` has to be set in the shell you build from, so re-export it
if you come back to a fresh terminal.

Two outputs matter:

```sh
ls -la out/target/product/cronos/lineage-18.1-*.zip   # the ROM, for TWRP
ls -la out/target/product/cronos/boot.img             # the patched kernel
```

**Build `userdebug`, not `user`.** This work is verified on a userdebug
build, where SELinux is permissive and no policy work is required. On an
enforcing build the camera will fail on denials that are not diagnosed
here; you would have to collect them with `dmesg | grep avc` and write the
rules (see `patches/README.md` section 4). `adb root`, which several
install steps need, also only works on userdebug.

## Step 8: flash

ROM zip first. Reboot to TWRP (`adb reboot recovery`), then either sideload
it:

```sh
adb sideload out/target/product/cronos/lineage-18.1-*.zip
```

or push it and install from TWRP's own file browser. This is a dirty flash
over the same ROM, so `/data` is preserved; no wipe is needed.

Reboot into Android once and confirm it comes up before touching the boot
partition - that way, if the next step goes wrong, you know the ROM itself
was fine.

The ROM zip already contains `boot.img`, and TWRP re-applies the amonet
header for you on install, so the device comes up on the new kernel after
this step alone. Running the flash script below is still worth it: it
backs the partition up first and verifies the result byte for byte, which
the zip install does not.

Boot image second, and **only** with the flash script:

```sh
cd ~/lineageos-echo-show-camera
scripts/flash-boot.sh <serial> ~/lineage-18.1
```

The script backs up the current boot partition to `backups/`, rebuilds the
amonet layout (exploit header, relocated boot header, payload at 0x800),
flashes, and verifies byte for byte. Never `dd` a plain `boot.img` to the
boot partition on these devices: amonet steals the first two blocks, and a
plain image produces a hang at the vendor logo that is indistinguishable
from a bad kernel. If you want to convince yourself first:

```sh
scripts/flash-boot.sh --self-test <serial>
```

That dumps your device's current (working) boot and recovery partitions,
rebuilds the boot partition from its parts, and requires the result to
match the original byte for byte.

Boot the device and re-establish adb root before continuing.

> **Do not open the camera between here and the end of step 9.** The ROM
> carries the whole camera stack but not yet the shim that makes it work,
> so any client that opens it - including a kiosk or security app that
> starts automatically - puts the HAL into an endless retry loop that
> leaves the device unresponsive with a lit but black screen, recoverable
> only by holding the power button. Patch 0013 prevents this by shipping
> `cameraserver` disabled; if you skipped it, disable any auto-starting
> camera app before rebooting.

## Step 9: install the on-device userspace

The ROM you just flashed already contains the blobs, the provider and the
shim. What remains is the `LD_PRELOAD` shim (which is built against the
device's own bionic, so it cannot be built earlier) and the two tuning
corrections, which patch a library in place on the device.

```sh
cd ~/lineageos-echo-show-camera

# 9.1  Build the LD_PRELOAD shim against this device's libc, and install it
#      together with the camera-bringup init script
shims/libcmdqevent/build.sh <serial>
scripts/install-cmdq-event-shim.sh <serial>

# 9.2  Neutralize the double white balance in the AWB reference
scripts/patch-awb-d65.sh <serial>

# 9.3  Correct the black level for the OV02B10 (the stock tuning leaves a
#      ~30% grey floor over every image)
scripts/patch-obc-pedestal.sh <serial>

adb -s <serial> reboot
```

Each script prints what it changed and refuses to proceed past an
unexpected state; 9.2 and 9.3 keep a `.orig` backup on the device.

If you would rather not reflash the whole ROM while iterating, or you
changed something in the vendor tree after building,
`scripts/install-camera.sh <serial> ~/lineage-18.1` pushes the blobs, the
provider libraries, the shim and the vintf manifest straight to the device
from the build tree. It checks that the build artifacts exist first.

## Step 10: verify

All of this from a cold boot, with no manual steps in between:

```sh
adb -s <serial> shell 'pm list features | grep camera'
# feature:android.hardware.camera.any
# feature:android.hardware.camera.front
#   (and NO plain feature:android.hardware.camera)

adb -s <serial> shell 'dumpsys media.camera | grep Orientation'
# Orientation: 0

adb -s <serial> shell 'logcat -d | grep -cE "startStream fail|deque DISPO fail"'
# 0
```

Then the functional test:

1. Open the camera app. You should see a live, right-side-up preview with
   believable colors within about two seconds.
2. Take a photo. It should complete in about one second and produce a
   1600x1200 JPEG (check with `adb shell ls -la /sdcard/DCIM/Camera/`).
3. Cover the camera or darken the room and take another photo. It should be
   essentially black. If it is a grey or cyan haze instead, the black-level
   patch (step 9.3) did not apply.

There is also `scripts/camera-test.sh <serial>`, which runs a scripted
cold-boot capture cycle and collects the relevant logs.

## Step 11: calibrate the color for your unit (optional)

The defaults were calibrated on one device and land close on any unit of
the same model, but panel aging and lighting differ. If greys look tinted:

1. Point the camera at anything grey or white under your normal lighting.
2. Take a snapshot, look at it, and adjust:

```sh
# too magenta under warm light: lower r.warm by 2-3; too green: raise it
adb -s <serial> shell setprop persist.camera.awbtrim.r.warm 564
# blue axis, same logic
adb -s <serial> shell setprop persist.camera.awbtrim.b.warm 536
# under daylight, use the cool anchors instead:
#   persist.camera.awbtrim.r / persist.camera.awbtrim.b
```

Values are read every 64 frames, so changes apply within a few seconds
without restarting anything. Steps of 2-3 matter: the color matrix
amplifies a change here several times over in the rendered image - moving
`r.warm` by 4 (under 1%) was enough to take a test scene from neutral to
visibly warm. To persist your
values across factory resets, edit the defaults in
`shims/libcmdqevent/camera-bringup.rc` and re-run
`scripts/install-cmdq-event-shim.sh`.

## Troubleshooting

**Camera app says no camera / `Number of camera devices: 0`.**
The provider is not loading. Check `adb logcat -d | grep -i provider` for
`HIDL_FETCH_ICameraProvider` errors; a "library not found" here usually
means a blob or one of the `camera.device@*-impl` variants is missing.
Confirm the manifest change (0011) made it into the build:
`adb shell cat /vendor/etc/vintf/manifest.xml | grep -A6 camera`.

**cameraserver crash-loops with `ImgSensorDrv` in the backtrace.**
The kernel struct patch (0001) is not in the running kernel, or the boot
image was not actually flashed. `adb shell uname -a` and compare the build
timestamp to your build.

**Preview is uniform green.**
No frames are being delivered. Usually the cmdq event shim is not loaded:
check `adb shell 'getprop | grep cmdq'` and that
`/system/etc/init/cameraserver.rc` contains the `LD_PRELOAD` line
(`install-cmdq-event-shim.sh` adds it).

**Capture times out (`ISP_WAIT_IRQ fail`, `Hit timeout for jpeg callback`).**
The sensor driver timing patches (0008, 0010) are missing from the kernel.

**Image is magenta everywhere.**
`patch-awb-d65.sh` was not applied, or was applied with the wrong values
(the default is unity, which is correct).

**Blacks are grey, everything hazy.**
`patch-obc-pedestal.sh` was not applied. Verify with the dark-room test in
step 10.

**Device completely unresponsive (no adb, no ping).**
You most likely killed cameraserver while the ISP was streaming; see the
safety rules. Power cycle it physically.

**Boot hangs at the vendor logo after flashing.**
The boot partition layout is wrong (this is what happens when a plain image
is written). Boot TWRP and restore the backup that `flash-boot.sh` made:
its exact restore command is printed at the end of every flash.

## Safety rules

1. **Never SIGKILL or `stop` cameraserver while the camera is streaming.**
   It leaves the IOMMU pointed at freed buffers and the resulting bus
   violation storm livelocks the entire device: no adb, no network, no
   watchdog reboot. Only a physical power cycle recovers it. Use
   `adb reboot`, or close the camera app and give it two seconds first.
2. **Only flash the boot partition with `scripts/flash-boot.sh`.** See
   step 8 for why.
3. Keep the `backups/` directory the scripts create. A bad `/vendor` write
   is not trivially recoverable on these devices; the boot backups are.
