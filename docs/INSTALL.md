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

```sh
mkdir -p ~/lineage-18.1 && cd ~/lineage-18.1
repo init -u https://github.com/LineageOS/android.git -b lineage-18.1 --depth=1
git clone https://github.com/amazon-oss/local_manifests.git -b lineage-18.1 \
    .repo/local_manifests
# apply the manifest corrections from this repository before syncing:
#   ~/lineageos-echo-show-camera/patches/local-manifest-fixes.xml
# (see docs/building.md for what each fix is and why)
repo sync -c --no-clone-bundle --no-tags -j8
./patches/apply.sh
. build/envsetup.sh
breakfast lineage_cronos-userdebug
```

You need roughly 150 GB of disk.

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

cd ~/lineage-18.1
patch -p1 < ~/lineageos-echo-show-camera/patches/0011-*.patch
```

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

## Step 6: extract the camera blobs into the vendor tree

```sh
cat ~/lineageos-echo-show-camera/patches/cronos-camera-proprietary-files.txt \
    >> ~/lineage-18.1/device/amazon/cronos/proprietary-files.txt
```

Then re-run the device's `extract-files.sh` against a stock image, or let
the build consume the blobs fetched in step 9 (the on-device install script
also pushes them directly; both routes work, the vendor-tree route survives
future ROM rebuilds).

## Step 7: build

```sh
cd ~/lineage-18.1
. build/envsetup.sh
lunch lineage_cronos-userdebug
mka bacon           # full ROM zip; takes a while the first time
```

The boot image with the kernel patches is produced as part of the build
(`out/target/product/cronos/boot.img`).

## Step 8: flash

ROM zip first, via TWRP as usual (sideload or SD card).

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
scripts/flash-boot.sh --self-test
```

Boot the device and re-establish adb root before continuing.

## Step 9: install the on-device userspace

All from the repository root, in this order:

```sh
cd ~/lineageos-echo-show-camera

# 1. Fetch the stock camera blobs from the public firmware dump (to stock/lib)
scripts/fetch-camera-blobs.sh

# 2. Add the shim to the blobs' DT_NEEDED so the linker resolves through it
scripts/patch-shim-needed.sh ~/lineage-18.1

# 3. Build the private API 25 display framework: fetch, rename soname,
#    apply the three binary patches (each verifies the original bytes first)
scripts/install-private-dpframework.sh ~/lineage-18.1

# 4. Push blobs and libraries to the device
scripts/install-camera.sh <serial> ~/lineage-18.1

# 5. Build and install the LD_PRELOAD shim (cmdq event translation + AWB
#    correction) and the camera-bringup init script
shims/libcmdqevent/build.sh <serial>
scripts/install-cmdq-event-shim.sh <serial>

# 6. Neutralize the double white balance in the AWB reference
scripts/patch-awb-d65.sh <serial>

# 7. Correct the black level for the OV02B10 (the stock tuning leaves a
#    30% grey floor over every image)
scripts/patch-obc-pedestal.sh <serial>

adb -s <serial> reboot
```

Each script prints what it changed; none of them proceeds silently past an
unexpected state.

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
   patch (step 9.7) did not apply.

There is also `scripts/camera-test.sh <serial>`, which runs a scripted
cold-boot capture cycle and collects the relevant logs.

## Step 11: calibrate the color for your unit (optional)

The defaults were calibrated on one device and land close on any unit of
the same model, but panel aging and lighting differ. If greys look tinted:

1. Point the camera at anything grey or white under your normal lighting.
2. Take a snapshot, look at it, and adjust:

```sh
# too magenta under warm light: lower r.warm by 2-3; too green: raise it
adb -s <serial> shell setprop persist.camera.awbtrim.r.warm 431
# blue axis, same logic
adb -s <serial> shell setprop persist.camera.awbtrim.b.warm 412
# under daylight, use the cool anchors instead:
#   persist.camera.awbtrim.r / persist.camera.awbtrim.b
```

Values are read every 64 frames, so changes apply within a few seconds
without restarting anything. Steps of 2-3 matter: the color matrix
amplifies these values roughly 5x in the rendered image. To persist your
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
