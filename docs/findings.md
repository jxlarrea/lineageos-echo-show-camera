# Camera support on LineageOS 18.1 for the MT8163 Echo Show devices

Investigation notes for `cronos` (Echo Show 5 2nd gen, 2021). Everything below
was measured against a running device, build
`18.1-20260418-UNOFFICIAL-cronos`, kernel `4.9.337`.

## What the current state actually is

The received summary of this bug is that the camera is broken because Android 7
blobs are incompatible with the newer kernel. On `cronos` that is not what is
happening, because **there is no camera userspace on the device at all**:

| Component | State |
| --- | --- |
| Kernel sensor driver | present (`CONFIG_CUSTOM_KERNEL_IMGSENSOR="ov9734_mipi_raw"`) |
| Kernel ISP / imgsensor | present (`CONFIG_MTK_CAMERA_ISP=y`, `CONFIG_MTK_IMGSENSOR=y`) |
| Camera device nodes | present (`/dev/camera-isp`, `camera-pipemgr`, `camera-sysram`, `kd_camera_hw`, `kd_camera_hw_bus2`) |
| `cameraserver` | running |
| AOSP device shims | present (`camera.device@1.0-impl.so`, `camera.device@3.2-impl.so`) |
| Camera provider HAL | **absent** |
| MediaTek camera blobs | **absent** (zero `libcam*` in `/vendor/lib`) |

Opening the camera app produces no crash and no denial, just:

```
CAM_Camera2OneCamMgr: No back-facing camera found.
CAM_Camera2OneCamMgr: No front-facing camera found.
```

The vendor manifest declares `android.hardware.camera.provider@2.4` at
`internal/0` over hwbinder. That is MediaTek's own provider, served by a
`camerahalserver` binary that Amazon does not ship on Echo Show devices and that
this ROM does not build. So the camera service resolves the provider to nothing
and enumerates zero devices. Nothing has failed yet — the stack was simply never
wired up on this device.

This matters for expectations: the crash chain documented for `crown`
(`CameraDevice::sGetMemory`, `BsMapper::importBuffer`, and so on) is what you get
*after* the blobs are installed. On `cronos` those crashes are downstream of work
that has not been done yet.

## Where the sensor tuning has to come from

The OV9734 static metadata — sensor, lens, flashlight and 3A tuning — is
compiled into the stock `libcam.halsensor.so`. Strings recovered from it name the
original MediaTek source paths:

```
vendor/mediatek/proprietary_mt8163/custom/mt8163/hal/imgsensor_metadata/ov9734_mipi_raw/
    config_static_metadata.sensor.ov9734mipiraw.h
    config_static_metadata.lens.ov9734mipiraw.h
    config_static_metadata.tuning_3a.ov9734mipiraw.h
    config_static_metadata.project.camera.ov9734mipiraw.h
    config_static_metadata.project.flashlight.ov9734mipiraw.h
```

This rules out the otherwise attractive shortcut of transplanting karnak's
camera stack. karnak (Fire HD 8 2018) is the same SoC, its blob set is a
generation newer (API 28, Treble, `camerahalserver` + mtkcam3), and it is proven
on LineageOS 18.1 through 23 — but its tuning only covers GC2375 sensors:

```
$ strings libcameracustom.so | grep -oiE '\bov[0-9]{4}|gc[0-9]{4}'
gc2375mipi_raw_blx_front
gc2375mipi_raw_chxt_rear
```

No OV9734, and the stack is closed source, so the metadata cannot be added.
MediaTek's MT8163 camera HAL sources are not public either — the one promising
repository (`vendor-mtk-sources/Amazon-Fire_HD8_8th_Gen`) is Amazon's GPL kernel
release, kernel only.

So the stock cronos blob set is the only viable source, and it is API 25
(Android 7.1) even though the surrounding platform is Amazon's `amz-p`
(Android 9) branch. All three Echo Show devices share this: `checkers`,
`crown` and `cronos` fingerprints all end `amz-p`, and all three carry
MediaTek's legacy camera1 HAL rather than the mtkcam3 stack of the same era.

Stock images are on GitHub as firmware dumps
(`el-vertedero/amazon_cronos_dump`, branch
`cronos-user-6.0-NS6573-6567-amz-p,release-keys`), which is where the measurements
below come from.

## How large the compatibility gap really is

This is the part that changes the outlook. Taking the closure of the stock
camera stack and resolving every imported symbol against what this device
actually provides:

- **45 MediaTek libraries** make up the closure (34 `libcam*`/`libmtkcam*`, plus
  11 transitive dependencies). 17 MB total.
- Every other dependency — `libgui`, `libui`, `libbinder`, `libutils`,
  `libcamera_client`, `libcamera_metadata`, `libhardware`, `libcutils`,
  `libmedia`, `libdpframework`, `libsensor`, libc, libc++ — is already on the
  device.
- **11 symbols do not resolve.** That is the entire link-level gap.

They fall into three groups, and all eleven are the same kind of drift: the
current implementation still has the method, it just gained trailing parameters.

### libutils: `android::CallStack` (4 symbols)

```
_ZN7android9CallStackC1Ev
_ZN7android9CallStackD1Ev
_ZN7android9CallStack6updateEii
_ZNK7android9CallStack3logEPKc19android_LogPriorityS2_
```

No longer exported. Used only for diagnostic backtraces, so stubs are fine.

### libui: `GraphicBuffer` / `GraphicBufferMapper` (3 symbols)

| Blob wants | Device has |
| --- | --- |
| `GraphicBuffer(w,h,fmt,usage,stride,handle,keep)` | same plus `layerCount` |
| `GraphicBuffer::lock(usage, vaddr)` | same plus `outBytesPerPixel`, `outBytesPerStride` |
| `GraphicBufferMapper::lock(handle, usage, bounds, vaddr)` | same plus the two out-params |

Forward with `layerCount = 1` and null out-params.

### libdpframework: MediaTek display pipeline (4 symbols)

The ROM's `libdpframework.so` comes from the Android 9 blob set, where these
gained a trailing `DpSecure` argument and a `timeval` deadline:

| Blob wants | Device has |
| --- | --- |
| `DpIspStream::setSrcConfig(...,bool)` | `...,bool,DpSecure` |
| `DpIspStream::setDstConfig(...,bool)` | `...,bool,DpSecure` |
| `DpIspStream::startStream()` | `startStream(timeval*)` |
| `DpBlitStream::invalidate()` | `invalidate(timeval*)` |

Forward with `DP_SECURE_NONE` and a null deadline.

`shims/libcamera_shim/` implements all eleven.

## What happened when it was actually built and installed

The static analysis above held up, but linking was only the first of several
layers. Recorded here in the order they were hit.

### Getting the shim actually used (3 problems)

**The shim was built, installed, and completely unused.** Nothing in the blobs'
`DT_NEEDED` referenced it. This tree's `extract_utils` has no shim mechanism,
and the blobs arrive as plain `PRODUCT_COPY_FILES` with no Soong module, so
there is no `shared_libs` to declare either. The entries have to be patched in
directly with the bundled patchelf across the 28 blobs that import shimmed
symbols - see `scripts/patch-shim-needed.sh`.

**`libdpframework` cannot be linked against.** Soong fails with `depends on
undefined module "libdpframework"` for the same reason: it is copied, not built.
The four `DpIspStream`/`DpBlitStream` forwarders resolve their targets with
`dlsym` instead.

**One blob needed a library, not a shim.** `libcam.utils.sensorlistener.so`
imports `android::SensorManager` and `android::SensorEventQueue` and lists
`libgui.so`, which is where they lived on Android 7. They moved to `libsensor.so`
in Android 8 with unchanged signatures, so naming that library is enough.
Everything else that failed to load (`libcam.client`, `libcam.camadapter`) did
so transitively through this one blob.

With those three done, all 45 libraries load on the device with zero failures,
confirmed by having the device's own linker load each one.

### Getting the provider to load

The passthrough `-impl` module links its whole backend family, so `-legacy`,
`-external` and the `camera.device@3.x-impl` wrappers (external variants
included) must all be installed or the `dlopen` fails with "library not found",
silently, with the camera service simply reporting no providers.

### The kernel interface mismatch

With everything loading, `cameraserver` crash-loops. The call chain is clean and
goes all the way down:

```
CameraService::enumerateProviders
  -> PassthroughServiceManager -> HIDL_FETCH_ICameraProvider
  -> LegacyCameraProviderImpl_2_4::initialize -> CameraModule::init
  -> camera.mt8163.so CamDeviceManagerImp::enumDeviceLocked
  -> HalSensorList::searchSensors -> ImgSensorDrv::impSearchSensor
  -> SIGSEGV in ImgSensorDrv::getCurrentSensorType, fault addr 0x24
```

strace gives the real cause:

```
openat("/dev/kd_camera_hw", O_RDWR)                        = 9
ioctl(9, _IOC(_IOC_READ|_IOC_WRITE, 0x69, 0x3c, 0x8), ...) = -1 ENOTTY
```

`0x3c` is 60, `KDIMGSENSORIOC_X_SET_MCLK_PLL`, requested with an 8-byte struct.
The kernel implements that ioctl, but `_IOWR` encodes `sizeof()` into the ioctl
number and this kernel has two layouts behind a config switch:

| Header | `ACDK_SENSOR_MCLK_STRUCT` | Size |
| --- | --- | --- |
| `kd_imgsensor_define_checkers.h` | `MUINT8 on; enum freq` | 8 |
| `kd_imgsensor_define_default.h` | `MUINT8 on; MUINT32 freq; MUINT8 TG` | 12 |

Only `checkers_defconfig` sets `CONFIG_CHECKERS`, so cronos builds the 12-byte
layout while its own stock blobs ask for 8. Different size, different ioctl
number, `ENOTTY`. The HAL then proceeds without the sensor type it needed and
dereferences null.

So the cmdq/ION risk flagged below was real in category but not in specifics:
the kernel interface did diverge, but as the same trivial parameter drift seen
everywhere else in this port, just expressed through an ioctl number. Fix in
`patches/0001-imgsensor-use-the-Amazon-MCLK-struct-layout-on-cronos.patch`.
It should apply to crown and rook as well, which have the same mismatch.

Testing it requires a kernel build and a boot partition flash, which is where
this currently stands.

## The camera now enumerates

With the kernel header fix in place the camera service reports:

```
Number of camera devices: 1
== Camera Provider HAL legacy/0 (v2.5, passthrough) static info: 1 devices ==
== Camera HAL device device@1.0/legacy/0 (v1.0) static information ==
    Facing: Front
    Orientation: 270
```

`cameraserver` is stable rather than crash-looping, the MediaTek HAL creates a
`Cam1Device`, and 3A / Sensor / Resource HALs all initialize. The kernel
correctly identifies the sensor:

```
$ cat /proc/driver/camera_info
 CAM[1]:ov9734_mipi_raw;
```

An application can open the camera - a third-party CameraX app acquired camera 0
successfully, which is why the stock camera app then reported
`CAMERA_DISCONNECTED` (the legacy HAL1 path allows a single client).

### Which kernel patch was needed, and why the narrow one was not enough

Two candidate fixes were tried. The narrow one - aliasing the ioctl number so
the 8-byte `SET_MCLK_PLL` is accepted (`patches/0002-*`) - **works at the ioctl
level**: after it, `_IOC(READ|WRITE, 0x69, 0x3c, 0x8)` returns 0 and the trace
contains zero `ENOTTY`. But `cameraserver` still crashed in exactly the same
place, because fixing one command's size encoding leaves every other struct
layout mismatched.

The decisive one is the header selection (`patches/0001-*`).
`KDIMGSENSORIOC_X_GETINFO` passes the kernel a *pointer*, and the kernel writes
`ACDK_SENSOR_INFO_STRUCT` through it. That struct differs between the two header
variants by 20 `SensorGrabStartX/Y_*` fields plus a reordering, so the HAL reads
every subsequent field at the wrong offset. No ioctl number can guard against a
mismatch inside a buffer the kernel fills in. Selecting the Amazon layout fixes
the whole family at once and makes `ACDK_SENSOR_MCLK_STRUCT` 8 bytes natively,
which subsumes the alias.

The two patches are mutually exclusive: applied together they produce duplicate
`case` labels and fail to compile.

## Flashing: the boot partition layout

Do not `dd` a plain boot.img over the boot partition, and do not write it at an
offset either. On these amonet-unlocked devices the partition is:

| Range | Content |
| --- | --- |
| `0x000-0x3FF` | amonet exploit header, byte-identical to `recovery[0x000:0x400]` |
| `0x400-0x7FF` | copy of the real boot image's first 1 KB (its header) |
| `0x800-` | boot image payload, kernel at its normal page offset |

amonet does not shift the image - it steals blocks 0-1 and relocates the real
header to blocks 2-3, exactly as TWRP's `/sbin/fix-bootpatch.sh` assumes. Two
devices were bricked here by assuming the payload moved with the header, which
put the kernel at `0xC00`; it hangs at the vendor logo with no other symptom, and
looks identical to a bad kernel. The tell is the gzip magic: a good image has
`1f8b` at `0x800`.

`scripts/flash-boot.sh` implements this and has a `--self-test` that rebuilds a
known-good partition from its parts and requires a byte-for-byte match.

Also worth knowing while debugging kernels here: the running kernel embeds its
source commit in its version string (`4.9.337-gc374d6392fa0`), and
`/proc/config.gz` lets you diff your build config against the shipped one - both
were needed to rule out toolchain and config as causes. The shipped kernel was
built with the same Linaro GCC 6.3-2017.02 that `prebuilts/linaro` provides.

## Progress after the OV02B10 driver landed

With the correct sensor driver the stack goes much further. In order, each fix
exposing the next layer:

1. **Kernel panic in the ported driver's `feature_control`.** Fixed. It took the
   output pointer from the parameter buffer, but
   `adopt_CAMERA_HW_FeatureControl()` copies that buffer into a kernel
   allocation and `copy_to_user()`s it back - this feature id is in its
   writeback list - so word 1 is an output *slot*, not a userspace pointer.
   Dereferencing it panicked at `feature_control+0x6c0`, `str w0,[x1]` with
   w0 = 300 (30fps x 10). Write into the buffer and let the wrapper return it.

2. **`Unsupport SensorOutputDataFormat`.** Fixed. The reference driver reported
   `SENSOR_OUTPUT_FORMAT_RAW_MONO`, which this platform's HAL rejects and which
   is wrong for a colour Bayer sensor anyway. Set to `SENSOR_OUTPUT_FORMAT_RAW_B`
   (matching the in-tree ov9734); if colours come out swapped, rotate this value.

3. **The sensor is now correctly identified end to end:**

   ```
   $ cat /proc/driver/camera_info
    CAM[1]:ov02b10_mipi_raw;

   ImgSensorDrv: [impSearchSensor]found <0x2b/ov02b10_mipi_raw/SENSOR_DRVNAME_OV02B_MIPI_RAW>
   ImgSensorDrv: [getInfo2]prv w=0x640,h=0x4b0      # 1600x1200, the 2MP geometry
   ```

   Core OV02B static metadata resolves; only optional SCALER/FEATURE/REQUEST
   entries are reported missing.

4. **`CameraDevice::sGetMemory+18`, fault addr `0x8c`.** Fixed **in source**.
   This is the same crash the crown investigation hit as its "Bug 1" and solved
   by binary-patching the library with LIEF; because we build
   `camera.device@1.0-impl` ourselves, it is a normal patch instead.

   The cause is simpler than that investigation concluded: MediaTek's
   `libcam.client.so` calls the `get_memory` callback **without forwarding the
   cookie** passed to `set_callbacks()`, so `user` arrives NULL and
   `object->mDeviceCallback` faults at offset `0x8c`. Only one CameraDevice is
   ever open in the process, so recording it at `open()` and falling back to it
   when the cookie is missing fixes it cleanly. See
   `patches/0005-camera-device-1.0-tolerate-HALs-that-drop-the-callback-cookie.patch`.

### Solved: the DpIspStream object-size mismatch

The private-copy approach **works**. Giving the camera blobs their own API 25
`libdpframework` under a private soname eliminates the crash entirely, and the
display and media stacks keep the Android 9 library untouched. See
`scripts/install-private-dpframework.sh`; the `DpIspStream`/`DpBlitStream`
forwarders were removed from `libcamera_shim` because the private library
exports those signatures natively.

This matters beyond this one crash: the coupling described upstream - that the
camera needs an older `cmdq`/display stack while display and media need the
newer one - is what made this port look unfixable. It is only a conflict if both
have to use the *same* library. Per-consumer versioning dissolves it, and all
ten camera blobs that link the library still resolve cleanly afterwards
(verified with the device's own linker).

### Solved: the preview window contract

Fixed in the shim. `sDequeueBuffer` used to hand back `&mCirculatingBuffers[id]`
- the address of a standalone `buffer_handle_t` in a map. A real `ANativeWindow`
returns `&anwBuffer->handle`, a pointer *into* an `ANativeWindowBuffer`, and
MediaTek's blob recovers that object by offsetting back from it. Each imported
handle is now wrapped in a `GraphicBuffer` (`WRAP_HANDLE`, geometry recorded by
`set_buffers_geometry`) and `&gb->handle` is returned instead. All crashes stop;
the camera app reaches its normal UI rather than "Can't connect to the camera".

### Solved: the sensor now streams

The reference driver deferred stream-on to `streaming_control()`, which is only
reached from `SENSOR_FEATURE_SET_STREAMING_SUSPEND`/`RESUME` - features belonging
to MediaTek's newer imgsensor framework that **this platform's HAL never
issues**. So the sensor was fully configured and never told to start. The
contrast with the in-tree ov9734 driver is stark: its entire `preview_setting()`
is one line, `write_cmos_sensor(0x0100, 0x01) /* Stream on */`.

Fixed by calling `streaming_control(KAL_TRUE)` at the end of all five mode
setters (preview/capture/normal_video/hs_video/slim_video). Result:

```
irq_TG1_DONE timeouts: 0        (was: every frame, in a reset loop)
Capture failed:         0
Camera error:           0
```

The camera app now shows a live preview surface with a working shutter button and
no error dialogs, and `takePicture` runs through to JPEG encoding.

### Superseded: frames never reach the display

An earlier version of this document said frames were being delivered but arrived
empty. That was wrong, and the correction matters for what to do next:

```
dequeueBuffer calls: 0
enqueueBuffer calls: 0
```

Nothing is ever handed to the display client. The uniform green is simply the
preview surface's initial state - zero-filled YUV420 converts to RGB(0,135,0) in
BT.601, which is exactly that shade. It looked like "delivered but blank"; it is
"never delivered". Bayer order is therefore *not* a candidate: a wrong Bayer
pattern corrupts content, it cannot remove it.

What actually happens is a crash in the private display framework:

```
#05 DpIspStream::dequeueDstBuffer(int, void**, bool)+188   libdpframework_cam.so
#04 DpStream::stopStream()+18
#03 DpPathControl::onStopStream()+110
#00 DpPortAdapt::abortPoll(CLIENT_TYPE_ENUM)+2             null deref, fault 0x0
   called from MdpMgrImp::dequeueBuf                       libimageio_plat_drv.so
```

`dequeueDstBuffer` cannot obtain a buffer, tears the stream down, and the
teardown path itself dereferences null. The null deref is a symptom; the primary
failure is the dequeue.

**This is likely the kernel-ABI half of the coupling.** The private-copy trick
fixed the userspace C++ object-size mismatch, but the API 25 `libdpframework`
also talks to the kernel's MDP/cmdq driver, and this kernel has the newer
`CONFIG_MTK_CMDQ_TAB`. Two caveats before treating that as established: dmesg
shows no cmdq errors at all, and the failure could equally be a buffer the old
library cannot map. Unproven either way.

Separately, and unexplained: during the *capture* path the kernel logs

```
[Camera-ISP][ISP_ioctl] ERROR: Fail, Cmd(1074817798)
```

**CORRECTED - this was a bad lead, do not chase it.** An earlier revision
decoded `1074817798` as `0x40144806`, magic `0x48` ('H'), and concluded some
foreign driver's ioctl was being issued on the ISP fd. That arithmetic was
wrong. It is `0x40106b06`: `_IOW`, size 16, magic `0x6b` = `'k'` = `ISP_MAGIC`,
**nr 6 = `ISP_CMD_WAIT_IRQ`**.

So it is not a foreign ioctl at all - it is the ISP's own wait-for-interrupt
call reporting the timeout that is already visible in logcat as
`ISP_WAIT_IRQ fail`. It is one symptom, not a second bug. See "Open: takePicture
never delivers a JPEG" and `docs/handoff-takepicture.md`.

The `abortPoll` crash itself is now fixed by binary patch (see "the private
library, binary patched" below), and the dequeue failure has been traced further
down, to the cmdq submit/wait handshake.

### Superseded: the adapter, shelved over VA versus ION

`shims/libdpframework_cam/` replaces the private old library with an **adapter**
over this ROM's Android 9 `libdpframework`. Of the 28 symbols the camera blobs
import, 24 have byte-identical signatures in both versions; the caller's
undersized object is used to hold nothing but a pointer to a real, heap-allocated
Android 9 object (over-allocated to 8 KB, since its true size is unknowable from
outside), and every method loads that pointer and forwards. The four drifted
methods are adapted here rather than in `libcamera_shim`.

Result: `dequeueDstBuffer` no longer fails, no `DpPortAdapt::abortPoll` null
deref, no TG1 timeouts. This keeps userspace aligned with the kernel, which
removes the whole kernel-ABI question the private-copy approach ran into.

**One trap worth knowing:** the blobs import more from `libdpframework` than the
Dp classes - plain functions like
`tpipe_main_query_platform_working_buffer_size(int)` too. Defining only the 28
broke `camera.mt8163.so` loading outright and enumeration dropped to zero.
The fix is to `patchelf --add-needed libdpframework.so` onto the adapter, so
anything it does not define resolves transitively while its own 28 still win by
search order.

**Why it was shelved:** the adapter ran, the crashes stopped, but no frames
could move. The Android 9 `libdpframework` rejects the camera blobs' buffers
with `not support alloc by va`: the old stack hands the display framework plain
malloc'd virtual addresses and the API 25 library translated them internally,
while the Android 9 library only accepts ION buffers. Bridging that inside the
adapter would mean allocating shadow ION buffers and copying every frame both
ways. Rather than build that, the work returned to the private API 25 library
and fixed its kernel interface directly, which turned out to be far closer to
working than assumed (see the cmdq ABI section below).

### The private library, binary patched

The current on-device approach: the stock API 25 `libdpframework.so` installed
under the private soname `libdpframework_cam.so` (as in the private-copy section
above), plus exactly three binary patches. For patching, file offset = vaddr
minus 0x1000 in this library, and all code is Thumb.

1. **Device node path.** The old library opens `/proc/mtk_cmdq`; this kernel
   creates `/dev/mtk_cmdq` instead, so every open failed with
   `can't open display driver`. String patch, one occurrence:
   `/proc/mtk_cmdq` becomes `/dev/mtk_cmdq` (NUL padded).
2. **`DpPortAdapt::abortPoll` made a no-op.** The thunk at vaddr `0x14b2c`
   (`ldr r0,[r0,#0x20]; ldr r2,[r0]; ldr r2,[r2,#0x3c]; bx r2`) is replaced
   with `movs r0,#0; bx lr; nop; nop`. It dereferences a buffer pool pointer at
   `this+0x20` that is already freed on the teardown path, and aborting an
   in-flight poll early is an optimization, not a correctness requirement.
3. **`DpDriver::queryEngUsages` reports all engines idle.** The stock code at
   vaddr `0x1d696` issues `CMDQ_IOCTL_QUERY_USAGE` (`_IOW('x', 4, 144)`),
   which this kernel's MDP driver does not service compatibly, leaving the
   36-word usage array garbage. The call is replaced with an inline loop that
   zero-fills the array.

`scripts/install-private-dpframework.sh` now reproduces the patched library
byte for byte from the public firmware dump (fetch, soname rename, all three
patches, with the original bytes verified before each write).

### Solved: the locked-port loop was self-inflicted

An earlier iteration of the patched library instead NOPed the
`blx DpStream::stopStream` at `0x18358` in `dequeueDstBuffer`'s failure path,
reasoning that teardown-on-transient-failure was what crashed in `abortPoll`.
That produced an endless loop of
`DpChannel: source port is in locked state` then `startStream fail(-7)`.

Disassembly of `DpStream::stopStream` (`0x153b8`) shows why: it is the unlock.
It takes the stream mutex, calls `DpPathControl::onStopStream`, walks the
channel list at `this+0x1f8` calling `DpChannel::unlockChannel` on every node,
and marks the stream stopped. (`lockChannel` stores 0 to `DpChannel+4`,
`unlockChannel` stores 1.) Skipping stopStream therefore leaves the source port
locked forever after the first transient dequeue failure, and every subsequent
start fails with -7.

Two lessons recorded: patch the crashing leaf, never the teardown path above
it; and distrust "first error" readings from logcat, since logd chatty
suppression (`expire N lines`) had swallowed the earlier cycles and made the
already-locked state look like the initial failure.

### The cmdq ioctl ABI lines up after all

The standing fear (the "kernel-ABI half of the coupling" above) was that the
API 25 library speaks the old cmdq ioctl dialect to a kernel that only accepts
the new MDP one. Measured, that theory is dead:

- `DpDriver::submitCommand` (`0x1d6e0`) issues ioctl `0x40887814` on the cmdq
  fd, which decodes as `_IOW('x', 20, 136 bytes)`, and on success reads a
  64-bit job handle from struct offset `0x40`.
- The kernel's `CMDQ_IOCTL_ASYNC_EXEC` is `_IOW('x', 20, struct mdp_submit)`
  (`mdp/mdp_def_ex.h`). With `CONFIG_MTK_CMDQ_TAB` set, as on this kernel,
  `cmdqSecDataStruct` is 64 bytes and `sizeof(struct mdp_submit)` is exactly
  136. The request codes are bit-for-bit identical.
- The library's field placement matches `struct mdp_submit`, not the older
  `cmdqCommandStruct`: priority at `0x0c`, `engine_flag` at `0x10` (with a
  special case mapping engine mask `0x8967` to `0x800`), `job_id` at `0x40`.
  `cmdqCommandStruct` would have engineFlag at `0x08`.

So Amazon's Android 7 library already speaks this kernel's `mdp_submit` ABI;
Amazon evidently backported the newer MDP ioctl interface into their userspace.
With the `/dev/mtk_cmdq` path patch the device opens, the submit request code
is recognized, and dmesg shows no `unrecognized ioctl` complaints.

### SOLVED: the preview works. The cmdq event ids were the last blocker

**The camera now produces a live preview with real image content.** All of
the following are zero in a clean run: `startStream fail(-26)`,
`waitSubmit is failed`, `dequeueDstBuffer for dispo fail`, `MDP deque DISPO
fail`. Consecutive screenshots differ, so it is a live stream and not a stuck
frame.

The cause was one more instance of this port's recurring theme - the old
userspace and the new kernel agreeing on the request code and the struct but
not on the *encoding of a field*.

`op_meta.event` carries a **raw GCE hardware event value** in the API 25
library, because that is what its vintage of the MDP interface took. This
kernel's `translate_meta()` instead passes that field straight to
`cmdq_op_wait()`, which treats it as a **`CMDQ_EVENT_ENUM` index** and
translates it to a hardware value through the DTS-populated event table. The
two numbering schemes are unrelated, and the mismatch is fatal rather than
merely wrong, because `cmdq_core_init_DTS_data()` defaults every hardware-event
slot to `CMDQ_SYNC_TOKEN_INVALID - 1 - i`, which is always negative. So a
hardware value read as an enum index almost always lands on an event the DTS
never named, `cmdq_op_wait()` returns `-EINVAL`, `translate_user_job()` aborts,
and **the entire submit is discarded** - one bad meta out of 167 kills the job.

The four events the camera waits on decode perfectly once read as hardware
values, which is what confirms the diagnosis rather than merely fitting it:

| Library sends | `mt8163.dtsi` | This kernel read it as |
| --- | --- | --- |
| 28 | `mdp_rdma0_frame_done` | `DISP_CCORR0_SOF` (no DTS entry -> -EINVAL) |
| 32 | `mdp_wdma_frame_done` | `DISP_AAL1_SOF` (no DTS entry -> -EINVAL) |
| 33 | `mdp_wrot_write_frame_done` | `DISP_GAMMA_SOF` (DTS has it -> accepted, wrong event) |
| 131 | `isp_frame_done_a` | `DISP_RDMA2_UNDERRUN` (no DTS entry -> -EINVAL) |

Those are exactly the MDP and ISP frame-done events an MDP pipeline must wait
on. The library was never wrong; only the field encoding drifted.

Note the third row: event 33 is *accepted* under the wrong interpretation. That
rules out any "reject what looks invalid" heuristic as a fix, because a
hardware value can collide with a legitimate enum index.

**The fix is applied in userspace, not the kernel.** `tools/cmdq-trace/`
rewrites `op_meta.event` from hardware value to enum index in the
`CMDQ_IOCTL_ASYNC_EXEC` path, using a table generated by composing the
kernel's own two sources (`gen_event_map.py`: `cmdq_event_common.h` gives
enum -> dts name, `mt8163.dtsi` gives dts name -> hardware value). Set
`CMDQ_FIX_EVENTS=0` to trace without rewriting.

Userspace is the right layer here because **`/dev/mtk_cmdq` has two clients**:

```
/system/bin/cameraserver                                  -> /dev/mtk_cmdq
/vendor/bin/hw/android.hardware.graphics.composer@2.1-service -> /dev/mtk_cmdq
```

The composer is Android 9 vintage and already agrees with this kernel. A
kernel-side remap would have to apply to both and would break display - the
same coupling that made downgrading `libdpframework` impossible. Interposing
only on cameraserver sidesteps it entirely.

Before and after, same build, same library:

```
before:  EXEC ... count=167 ... ret=0 errno=0 job_id(after)=0     (rejected)
after:   EXEC ... count=167 events(hw): 28 33 ... remapped=6
         EXEC ret=0 errno=0 job_id(after)=ad                      (accepted)
```

The fix now ships as `shims/libcmdqevent/libcmdqevent_shim.so`; see below.

### The camera now works from a cold boot with no manual steps

`scripts/install-cmdq-event-shim.sh` installs the shim and
`shims/libcmdqevent/camera-bringup.rc`, which replaces the whole "runtime
workarounds needed every boot" incantation. Verified from an actual cold boot:
`cameraserver` running, `/dev/MTK_SMI`, `/dev/mtk_cmdq`, `/dev/mdp_sync` and
`/proc/m4u` all `0666`, `/persist` mounted, `debug.lsc_mgr.type=0` set, and the
camera app opens straight to a working preview with zero MDP errors.

The shim is loaded with `setenv LD_PRELOAD` **in cameraserver's own init rc**,
not by patching a blob. That is what scopes it to the one client that needs it
and leaves the graphics composer alone. `cameraserver.rc` also loses its
`disabled` line (kept during bring-up so a crashing HAL could not stop the
boot); the original is preserved as `cameraserver.rc.orig`.

An `LD_PRELOAD` interposer is a legitimate long-term answer here rather than a
stopgap. `ioctl` cannot be interposed from `libcamera_shim` instead, because
that library is reached through the blobs' `DT_NEEDED` and loses symbol
resolution order to libc; only a preload (or binary-patching the import) wins.

### RETRACTED: "Bayer order is ruled out"

An earlier revision of this document claimed all four Bayer orders had been
measured and none was neutral, concluding the Bayer order was not the cause of
the magenta cast. **That conclusion was wrong and the measurement was void.**

The runtime override it relied on never actually ran. Tracing later showed the
shim's `KDIMGSENSORIOC_X_GETINFO2` branch logging nothing at all, so all four
"measurements" were taken with the driver's compiled-in `RAW_B`, and the small
differences between them (R/G 2.30 to 2.71) were nothing but scene and AE drift
between captures.

Recorded as a caution rather than deleted, because the failure mode is easy to
repeat: **an override that silently never fires looks exactly like an override
that fires and changes nothing.** Both produce "I tried all the values and it
made no difference". Always make the interposer log the value it actually saw
and the value it wrote, and confirm those lines appear, before reading any
result as a negative.

Why it did not fire is still open. The HAL demonstrably takes the format from
the kernel - it rejected the reference driver's `RAW_MONO` with
`Unsupport SensorOutputDataFormat` - so it must arrive through some ioctl. The
likely candidate is `KDIMGSENSORIOC_X_GETINFO` (`_IOWR('i', 5, ...)`) rather
than `GETINFO2`; its `ACDK_SENSOR_GETINFO_STRUCT` carries `pInfo[2]` at
offsets 8 and 12 on 32-bit, and `ACDK_SENSOR_INFO_STRUCT` shares the same
prefix layout, so `SensorOutputDataFormat` is at offset 36 there too. Handle
both, and log every `'i'`-magic ioctl to find out which the HAL really uses.

### SOLVED: the magenta cast was a double-applied white balance

Fixed by `scripts/patch-awb-d65.sh`. Measured on the preview, same scene:

| | R/G | B/G |
| --- | --- | --- |
| before | 2.44 | 1.46 |
| AWB reference set to the calibration-implied D65 (731,512,743) | 1.87 | 1.73 |
| **AWB reference set to unity (512,512,512)** | **1.11** | **1.11** |

R and B now sit equal and barely above G; what warmth remains is the room's
actual lighting.

The cast was never a missing correction - it was **one correction too many**.
This unit's factory `calibration.bin` (now that `/persist` is mounted) is read
successfully and applied as a raw pre-gain:

```
CamCal: ver8900~ [rCalGain.u4R] = 756   (G = 512, B = 742)
awb_algo: [1][updateAWBParam] rUnitGain: R=756, G=512, B=742
```

So the data reaching AWB is already white balanced. AWB then applied its own
D65 reference on top, which is why the output tracked that reference almost
exactly. The proof is the unity row above: with AWB's reference neutralised the
image is neutral, which can only be true if something upstream had already done
the balancing.

The original reference was also simply wrong for this module -
`rD65Gain: R=891, G=512, B=610`, a strongly red-biased correction, while both
the unit's own calibration (756/512/742) and the LSC path's view of the same
NVRAM block (`AwbNvramInfo: D65Gain(731, 512, 743)`) are balanced. Two readers
disagreeing about one NVRAM block by exactly one 12-byte `AWB_GAIN_T` is the
same struct-offset drift seen everywhere else in this port, and it explains why
the bias was red rather than neutral.

Note AWB never adapted anyway: `CCT` sat at 6500 and the gains were identical
frame after frame for a minute, so nothing is lost by neutralising the
reference. If adaptive AWB is wanted later, the statistics path is the thing to
investigate, not the reference gain.

**CORRECTED - AWB does adapt; the freeze was an artifact of a static scene.**
Measured by driving the room's smart lights through home automation while logging
`awb_algo`: small illuminant shifts (a desk lamp, a dim cove) sit inside the
algorithm's stability deadband and the gains hold - which is what the
"identical for a minute" observation was actually seeing. Flooding the
camera's field of view (ceiling cove at full power) moved LV 25 -> 41, made
AE write new sensor exposures, and swung the AWB gains hard
(933/512/639 -> 791/512/723 on stock tuning; 536/512/536 -> 707/512/807 on a
731/512/743 reference). The state machine transitions Searching/Converged
throughout. The adaptation machinery - statistics, classification, smoothing -
is fully alive.

The remaining defect is *magnitude*, not adaptivity: when AWB genuinely
adapts, it overshoots into magenta (ceiling measured R/G 1.9 on stock tuning,
1.6 on the 731 reference, under warm LED flood). The statistics it reads are
already balanced by the factory CalGain pre-gain, but the OV9734 tuning
tables compute corrections sized for raw-domain data, so every real
correction is roughly double-applied. That is the same double-WB mechanism as
the original magenta cast, now expressed dynamically. Fixing it properly
needs OV02B10 tuning tables, which do not exist in any available blob set.
For this device's use (fixed mounting, fixed lighting) the unity reference is
the right anchor; if lighting-tracking color is ever needed, do it in the
consumer (gray-world on the snapshot) rather than in these blobs.

Debug tooling added for this investigation, all reusable: the preload shim
redirects the 3A blob's hardcoded `/sdcard/awb/` AWB statistics dumps
(`awb.debug.dump.enable=1`) to `/data/awbdump/`, which `camera-bringup.rc`
creates; the dump is a per-frame list of 2300 tagged records of algorithm
state plus per-window data. `debug.awb.enable=1` / `debug.ae.enable=1` turn
on the 3A logs. `cameraserver` also carries `sdcard_rw media_rw` groups on
the device (added by hand to `/system/etc/init/cameraserver.rc`, not in the
repo installer).

**Trap, recorded after losing an hour to it:** `patch-awb-d65.sh` originally
defaulted its replacement triple to the calibration-implied 731/512/743, while
the deployed configuration was unity 512/512/512 passed explicitly.
Re-running the script without arguments therefore silently regressed the
device to the magenta reference (ceiling R/G 2.0). The default is now unity.

**Daylight measurements, and where the adaptive gains actually come from.**
Under natural daylight (LV 78, LEDs off) the white ceiling measures:

| AWB gains applied | Ceiling R/G | B/G |
| --- | --- | --- |
| unity 512/512/512 (locked) | 0.71 | 0.70 |
| adaptive output 851/512/638 | 1.39 | 0.96 |
| implied correct ~722/512/732 | ~1.0 | ~1.0 |

The unity row is the key fact: with no WB gain the image is strongly green,
i.e. **the pixel path carries raw sensor response - the factory CalGain
pre-gain balances only the AWB statistics, not the image**. So per-illuminant
WB gains are genuinely required, no static triple can cover both daylight
(needs ~722/732) and warm LED (where near-unity happens to look right only
because the warm spectrum offsets the sensor's green dominance), and the
in-stack adaptation overshoots red under daylight by a fixed spectral bias
(R +18%, B -13%) - the OV9734-golden versus OV02B10 module difference.

Two attempts to retarget the adaptation failed identically: patching the
six-entry per-illuminant gain table at `0xef910` (three copies, entries
(512,557)(549,948)(657,847)(742,870)(758,735)(891,610)) and the D65
reference both left the adapted output byte-for-byte unchanged (851/512/638).
The applied gains are therefore computed from the measured white point plus
the calibration constants (UnitGain 756/512/742, GoldenGain 778/551/763),
not read from those tables. Correcting the adaptation in-stack means
disassembling the gain computation in `lib3a.so` to find what to bias -
doable (the constants to search for are known) but unfinished.

### SOLVED: adaptive AWB corrected in-stack via the handleAWB trim

The spectral bias is now corrected generically, inside the camera stack, with
no environment- or app-specific pieces. The shim interposes lib3a's exported
per-frame entry `AwbAlgo::handleAWB(AWB_INPUT_T&, AWB_OUTPUT_T&)` (the gains
themselves are applied through mmap'd register stores and cannot be
intercepted at the ISP layer) and rescales the gain triples in the output
struct. Triples are located by pattern - (R, 512, B), both channels in
gain range - because the struct layout is not public; three sit at words
0, 6 and 15 of AWB_OUTPUT_T.

The bias turned out to be illuminant-dependent (the two modules' spectral
responses diverge differently under different spectra), so the trim is
interpolated between two calibrated anchors. The interpolation key is the
algorithm's own untrimmed B gain - it tracks illuminant warmth
monotonically (~640 under daylight, ~835 under 2600K LED) and is available
inside the very triple being corrected, so no CCT field needs locating.
Four 512-based multipliers control it, re-read every 64 frames so tuning
needs no restart:

```
persist.camera.awbtrim.r        507    cool anchor (daylight)
persist.camera.awbtrim.b        692
persist.camera.awbtrim.r.warm   564    warm anchor (2600K LED)
persist.camera.awbtrim.b.warm   536
```

**Recalibrated after the black-level fix.** The anchors above replace an
earlier set (390/532 cool, 431/412 warm) measured while the OBC pedestal
was still lifting every frame, which biased the statistics AWB reads and
left a visible green cast. Raising all four by 30% removed it: on a grey
wall the render went from R/G 0.91, B/G 0.82 - both channels below green,
which is what a green cast is - to R/G 1.02, B/G 1.00. Confirmed by eye
as well as by measurement, and cross-checked against the same scene
corrected by hand in Photoshop (Color Balance, green/magenta toward
magenta).

Both anchors are now verified on device. The warm pair under uniform
2600K LED; the cool pair under natural midday daylight the following
morning, where the grey ceiling renders R/G 1.05, B/G 1.06-1.08 and the
window-side wall runs slightly cool (B/G 1.23) as it physically should.
The cool pair was originally scaled from the warm one rather than
measured, and the measurement confirmed it rather than correcting it.

`camera-bringup.rc` sets the defaults, calibrated against a grey ceiling
(a grey surface is a valid neutral reference) and verified to render it
neutral under natural daylight (R/G 0.97 vs 1.39 uncorrected) and under
uniform 2600K LED (R/G 0.96-1.09 vs 0.85/1.37 blue-shifted uncorrected),
both from a cold boot. Two accuracy limits, measured and worth knowing:
the blob's AWB output wobbles a few percent between sessions on an
identical scene, and the CCM amplifies gain error roughly 3x in the
rendered image, so scene-to-scene rendering varies about +-10% around
neutral. That is inherent to the closed algorithm, not to the trim.

The magenta-vs-cyan *spatial* gradient investigated along the way
(same-paint surfaces rendering 30% apart across one frame) is a
low-sun artifact, not a general daylight one: it was measured at golden
hour with direct low sunlight entering the room, and is not visible in
midday diffuse daylight or under uniform artificial light, where the same
surfaces agree within ~5%. It
is IR and low-sun light through the aged front glass plus veiling glare
from the internal window, which cannot be cleaned from outside. It is a
per-unit hardware trait, not correctable by any global gain, and belongs
in a known-limitations note.

### SOLVED: black level pedestal - "blacks render grey", the haze

**Fixed by `scripts/patch-obc-pedestal.sh`.** `debug.obc_apply.log=1` showed
the ISP subtracting OBC offset 8128 = 0x1FC0 = -64 in the 13-bit register
field - the OV9734 value. The OV02B10's standard 10-bit pedestal of 64 is
256 in the ISP's 12-bit domain, so 192 counts survived into every frame,
which gamma turned into the ~30% floor. Rewriting all 140 per-ISO
ISP_NVRAM_OBC_T blocks in libcameracustom.so from -64 to -256 takes the
dark-room frame from R=43 G=79 B=84 to **R=8 G=7 B=8**. The pedestal also
contaminated the AWB statistics and desaturated all colors, so the trim
anchors must be recalibrated after this fix.

The original characterisation, kept because the measurement method (a
lightless room as a free black-frame rig) is the thing to reuse:

Measured with the room fully dark (blackouts closed, all lights off): the
frame averages R=43 G=79 B=84 with a minimum pixel of 25, where a black
frame should be near zero. A ~30% grey floor with zero light entering is
electronic, not optical: the sensor's black-level pedestal is not fully
subtracted by the ISP's OBC stage, the residual is multiplied by AE's
maxed analog gain, and the gamma curve lifts it further. This pedestal,
not just the glare, is a large part of the perceived haze, and dark image
regions carry it in lit scenes too. The cyan tint of the pedestal is the
AWB trim's cool-anchor R-cut applied to the fallback gains on top of a
roughly equal-channel grey.

Suspected mismatch: the OBC per-ISO offsets come from the OV9734 tuning
tables while the OV02B10's pedestal differs. Attack surface, in order:
find the OBC per-ISO values (per-module isp_mgr debug enables, or locate
the per-ISO ISP tuning arrays in libcameracustom.so and patch the OBC
entries); alternatively lower the sensor-side BLC target in the driver we
own (register not documented in any public OV02B10 driver - would need
the module-parameter sweep technique). Verification is trivial: a dark
room frame must go to near-black.

Remaining IQ backlog, all characterised: this OBC fix; recalibrate the
cool trim anchors in real daylight (they predate the anti-compounding
fix); a fallback-state handler in the trim (when AWB cannot classify it
outputs exactly the reference triple - detectable - and the cool-anchor
trim is then wrong); confirm standard EV compensation works.

One resolver gotcha inside the wrapper: lib3a.so is dlopen'd by the
passthrough HAL into a local namespace group, so `dlsym(RTLD_NEXT, ...)`
from the preload cannot see it - and returning failure from the wrapper
silently pins AWB in `Searching` forever. The wrapper resolves the real
function via `dlopen("lib3a.so", RTLD_NOLOAD)` instead.

`debug.awb_mgr.lock=1` is confirmed working as an instrument: it stops
`handleAWB` entirely and freezes the gains at whatever the session start
computed - note it inherits the previous session's gains unless cameraserver
itself is restarted.

### SOLVED: the ROM advertised a back camera it does not have

**This one broke every CameraX app on the device, and it was a one-line device
tree bug.** `pm list features` reported:

```
feature:android.hardware.camera        <- a BACK camera, which does not exist
feature:android.hardware.camera.any
                                       <- camera.front missing entirely
```

`mt8163-common/mt8163.mk` copied `android.hardware.camera.xml`, which declares
`android.hardware.camera` (the back-camera feature). These devices have a
single front camera and no back camera.

It is not cosmetic. CameraX's `CameraValidator` only looks for a back camera
**when `FEATURE_CAMERA` is advertised**, so the bogus feature made validation
fail device-wide:

```
CameraValidator: Verifying camera lens facing on cronos, lensFacingInteger: null
CameraValidator: Camera LensFacing verification failed, existing cameras: [Camera@...[id=0]]
CameraX: Expected camera missing from device.
Caused by: java.lang.IllegalArgumentException: No available camera can be found
```

CameraX then retries initialization forever, so any app using it hangs with no
error rather than failing - which is exactly how it presented (a snapshot that
silently never completed). Fixed by copying
`android.hardware.camera.front.xml` instead, which declares `camera.any` +
`camera.front`. Verified: features correct after reboot, CameraX initializes,
zero validator failures, and the app's snapshot returns a real JPEG.

Note the fix belongs in the common makefile because all four Echo devices here
are single-front-camera.

### SOLVED: SENSOR_ORIENTATION was 270 on a landscape-mounted sensor

The sensor is landscape-mounted, matching the landscape-natural 960x480 panel,
and the driver now flips it 180 degrees so frames arrive upright - but the HAL
still reported 270, so every client that correctly rotates by
`SENSOR_ORIENTATION` produced a sideways image. Confirmed on a real capture:
the JPEG came back rotated 90 degrees while the preview was upright.

The value comes from `DevMetaInfo::queryDeviceWantedOrientation()` inside
`libcam.utils.so` and cannot be corrected in the blobs - see the retracted
`getSensorOrientation()` attempt below. It is now overridden in
`camera.device@1.0-impl`, which this tree builds, from the
`ro.camera.sensor_orientation` property (set in `device/amazon/cronos/device.mk`).
Both HAL1 clients and the camera2 legacy shim pick it up there.

Verified: `dumpsys media.camera` reports `Orientation: 0` and captures come out
upright.

### SOLVED: takePicture never delivered a JPEG

**Fixed in the sensor driver** - `patches/0010-imgsensor-ov02b10-restore-capture-and-video-mode-timing.patch`.
Verified on device: `takePicture` returns a full-resolution 1600x1200 JPEG in
about one second, repeatably, with zero `ISP_WAIT_IRQ` errors, and the preview
stays live afterwards.

The root cause was neither the ISP nor the IMGO path. The reference OV02B10
driver ships with the `.cap1`, `.normal_video`, `.hs_video` and `.slim_video`
entries of `imgsensor_info` inside one comment block, so all four are zeros.
`capture()` takes the `cap1` branch whenever `current_fps != 300`, and AE
lowers the preview frame rate indoors, so that branch is effectively always
taken: `pclk`, `line_length`, `frame_length` and `min_frame_length` all become
0. The first AE exposure write then underflows in `write_shutter()`:
`shutter > min_frame_length - margin` is unsigned (`0u - 7` is huge, so
false), `frame_length` stays 0, and the dummy-line registers get
`0 - 0x4c4 = 0xfb3c`. 64316 dummy lines on the 1220-line base is a 65536-line
frame: **one frame every 1.78 s**. Every 500 ms IRQ wait misses, and because
nothing is actually broken, no error interrupt ever fires. The preview restart
reprograms the mode registers, which is why preview always recovered.

What pinned it: an out-of-band `ISP_WAIT_IRQ` probe (`tools/cmdq-trace/ispwait.c`)
showed VS1 interrupts arriving during the stall at exactly 1.78 s intervals -
the sensor was streaming, just 54x too slowly. The `LD_PRELOAD` shim's ISP
ioctl tracer (`debug.isptrace`) then showed `GET_PIXEL_CLOCK_FREQ` and
`GET_PERIOD` returning zero right after the scenario switch, which named the
zeroed struct.

The fix uncomments the four mode structs (all five modes run the same
1600x1200@30 register table) and floors `frame_length` at the mode base in
`write_shutter()`/`set_shutter_frame_length()` so no future zeroed state can
underflow the dummy-line computation again. The struct fix also repairs video
recording, which reads the same zeroed entries.

The original characterisation, kept for the record:

```
MtkCam/Cam1Device: [Cam1DeviceBase::takePicture] +
IspDrv: ERROR: ISP_WAIT_IRQ fail(-1). Clear(1), Type(0), Status(0x00000400), Timeout(500).
IspDrv: ERROR: ISP_WAIT_IRQ fail(-1). Clear(1), Type(0), Status(0x00000400), Timeout(500).
IspDrv: ERROR: ISP_WAIT_IRQ fail(-1). Clear(1), Type(0), Status(0x00000001), Timeout(500).
RequestThread-0: Hit timeout for jpeg callback!
```

**The interesting part is which interrupts these are.** Decoding against
`cameraisp/src/mt8163/inc/camera_isp.h`:

| Status | Bit | Meaning |
| --- | --- | --- |
| `0x00000400` | 10 | `ISP_IRQ_INT_STATUS_PASS1_TG1_DON_ST` |
| `0x00000001` | 0 | `ISP_IRQ_INT_STATUS_VS1_ST` |

Both are **pass 1** (sensor into ISP) interrupts, not pass 2. So this is not the
capture/JPEG stage failing - the sensor stops feeding the ISP the moment the
HAL switches to the capture scenario, and everything downstream waits forever.
No capture-error callback is generated, which is why callers see silence.

Facts established while investigating, worth not re-deriving:

- **The capture register table is byte-identical to preview.** Both mode setters
  emit the same 91 writes with the same values, so the capture scenario does not
  reprogram the sensor differently and a bad capture table is not the cause.
- **The LSC/TSF capture-table errors are gone.** An earlier report cited
  `shadingTblAlign Err`, `min_gain is 0` and `MTKTSF_FEATURE_GEN_CAP_TBL`
  failures as likely related. They no longer appear - mounting `/persist` so the
  factory calibration loads fixed them - and `takePicture` still fails
  identically, so they were a separate issue.
- **No M4U, SMI or EMI errors** appear during the failing capture, so the DMA
  remapping is not obviously starved.

**Disproved hypothesis (do not retry).** The mode setters rewrite PLL and timing
registers, and on a preview -> capture switch they are reached mid-stream, so
standby-before-reprogram looked like the answer. Implemented (`patches/0008-*`),
built, flashed: the capture still times out on exactly the same interrupts. The
change is kept because the sequence is correct regardless, but it is not a fix.

Where to look next: what the HAL reconfigures on pass 1 for the capture
scenario. The preview path drives DISPO at 640x480 out of a 1600x1200 input;
capture switches pass 1 to full-resolution IMGO. That the sensor keeps
streaming in preview but stops the instant this switch happens points at the
IMGO/FBC path or its tpipe configuration rather than at the sensor driver.

Kernel side adds two facts: `IrqStatus(0x00000000)` (nothing is latched at all,
rather than the wrong bit being set), and the failing thread is `Capture@CmdQue`.
The register dump is `[0x15008008 00003210]` and `[0x15008100 00008001]`.

**A full handoff for this bug lives in `docs/handoff-takepicture.md`** - repro,
ruled-out leads, tooling, and the device safety rules.

Fixed in the driver (`patches/0007-*`), so the readout itself is rotated and
captures are correct too, not just the preview. Verified from a cold boot:
upright, R/G 1.08, B/G 1.02, zero errors.

`set_mirror_flip()` is now implemented as **page 1, register 0x12**, bit[0]
horizontal / bit[1] vertical, with `.mirror = IMAGE_HV_MIRROR`.

Finding that register was the whole problem: it is in no source available.
The reference driver this was ported from has `set_mirror_flip()` `#if 0`'d out
with all four cases empty, so no vendor ever filled it in. OmniVision's OV02A10
- a sibling part with the same `0xfd` paged architecture - uses `0x3f`, and
that was tried first and **does nothing** on this sensor.

**The technique that solved it is the reusable part.** Rather than guess a
register and pay a kernel build plus a boot flash per guess, the driver exposes
the page and register as module parameters and re-applies them on every mode
set. `tools/cmdq-trace/flip-sweep.sh` then sweeps candidates by reopening the
camera app - no rebuild, no flash, and no `cameraserver` restart (which
livelocks the device). It scores each capture against the 180-degree rotation
of an unflipped baseline, so a hit is unambiguous:

```
candidate       vs base vs base180   verdict
p1_r12             25.5     13.9   FLIPPED
p1_r3f              2.1     29.5   no change      <- the OV02A10 guess
...
```

The parameters remain in the shipped driver so a future correction is free.

**Two changes that must move together.** `IMAGE_HV_MIRROR` shifts the Bayer
phase by one pixel in each axis, so the native BGGR reads out as RGGB. The
intermediate build that flipped without updating
`sensor_output_dataformat` produced a strong green cast - red and blue swapped.
Setting `SENSOR_OUTPUT_FORMAT_RAW_R` alongside the flip restores correct colour.
This is also, incidentally, the first evidence that the declared data format
*does* reach the ISP: the earlier runtime sweep over all four values changed
nothing, because with no phase shift to correct there was nothing to see.

### Superseded: the preview is upside down

The sensor is mounted rotated 180 degrees and nothing compensates. The ported
driver cannot: it declares `.mirror = IMAGE_NORMAL`, and its `set_mirror_flip()`
is `#if 0`'d out with all four cases empty, so the sensor is never told to flip.

The HAL reports the rotation but with the wrong value:

```
MtkCam/devicemgr: [enumDeviceLocked] iFacing:1, iWantedOrientation:270,
                  iSetupOrientation:270
```

**A dead end worth recording so it is not repeated** (the working fix is the
sensor-level flip above).
`NSCamCustomSensor::getSensorOrientation()` in `libcameracustom.so` looks like
the source - it returns a PC-relative pointer to a three-entry table
`{main=90, sub=270, main2=90}`, and cronos enumerates as `Facing: Front`, the
sub slot, whose 270 matches exactly. Patching that entry to 90
(`scripts/patch-camera-orientation.sh`, which resolves the pointer rather than
hunting for the constant) **changes nothing**: verified on-device that the bytes
became `90 90 90`, yet the HAL still reported 270 and the preview was unchanged.

The value actually comes from `DevMetaInfo::queryDeviceWantedOrientation()` in
`libcam.utils.so`, which reads a per-device registry populated during setup, not
that table. There is no property override in either blob. Options, none cheap:

1. Implement the OV02B10 mirror/flip registers in the sensor driver. The correct
   fix - it rotates the readout itself, so captures are fixed too, not just the
   preview - but it needs the register set for this paged sensor and a boot
   flash, and a wrong guess can wedge the sensor.
2. Override `SENSOR_ORIENTATION` in the AOSP layer, which this tree builds
   (`LegacyMetadataMapper` maps HAL1 `CameraInfo.orientation` to
   `SENSOR_ORIENTATION` for camera2 clients). Fixes the app-visible rotation and
   EXIF without touching the kernel, at the cost of a framework build.

Option 1 was taken and worked; see the section above.

### Background: why the cast pointed at 3A rather than plumbing

With `debug.awb.enable 1` the picture is clear. AWB **is** running - the absence
of any `awb` tag in a default log is only because its logging is off, which
briefly looked like AWB never starting:

```
awb_algo: [1][PV AWB Gain] LV = 28, Rgain = 933, Ggain = 512, Bgain = 639
awb_algo: [1] CCT = 6500
awb_algo: [1] CurrentFullAWBGain: R = 933, G = 512,  B = 639
```

512 is unity, so AWB asks for R x1.82 and B x1.25 and holds those values
steadily for as long as the preview runs. Applied, and then pushed further by
the CCM's strong red term (`refine_CCM` returns `526,-289,19 / -73,411,-82 /
-4,-219,479`), they land almost exactly on the measured output of R/G 2.41 and
B/G 1.46. So the cast is not a missing correction - it is AWB converging on the
wrong illuminant and the pipeline faithfully applying it.

That points at tuning data rather than code, and matches what this port already
knows: the stock blob set's compiled-in tuning is **OV9734** (the gen-1
`checkers` sensor), while the hardware is **OV02B10**. Worth checking next which
of `libcam.halsensor.so` and cronos's own `libcameracustom.so` actually supplies
the AWB/CCM tables at runtime, since only the latter carries OV02B10 entries.
`debug.awb_mgr.lock`, `debug.isp_apply.disable` and `debug.isp_debug.enable`
are the useful levers.

This is image quality, not function: preview, streaming and capture all work.

### Superseded: startStream fails with -26, and the kernel swallows why

State with the current patched library (run15): zero crashes, zero locked
ports, clean teardown and retry every cycle, but no frame ever arrives.
Each cycle logs:

```
DpIspStream: wait submit stream failed(-26)
DpIspStream: waitSubmit is failed in asynchronize mode (-26)
MdpMgr startMdp ERROR: startStream fail(-26)
DpIspStream: Both JobID and Framedone List are empty!!!
dequeueDstBuffer for dispo fail(-1)
MtkCam/VSSScen [dequePass2] MDP deque DISPO fail
```

`-26` is the library's own failure code (`mvn r0,#0x19`), used both after a
failed ioctl and by `DpDriver::checkHandle`; it is not a kernel errno.

The key kernel-side fact: `cmdq_ioctl` in `cmdq/v2/cmdq_driver.c` assigns each
handler's result to `status` and then unconditionally `return 0`. Every
recognized ioctl reports success to userspace regardless of what
`mdp_ioctl_async_exec` / `mdp_ioctl_async_wait` actually returned. So the
userspace ioctl cannot be what returns the failure; the working hypothesis is
that the submit fails inside the kernel, the error is swallowed, the returned
`job_id` stays zero or garbage, and the library's own handle check then fails
with -26 on the wait side. That also fits the otherwise puzzling dmesg silence:
the `CMDQ_MSG` entry lines are log-level gated and the dropped status is never
printed.

Next steps, in order:

1. Read `mdp_ioctl_async_exec` and `mdp_ioctl_async_wait` in
   `mdp/mdp_ioctl_ex.c` end to end: enumerate the failure paths, what each
   logs, and what `struct mdp_submit.metas` is expected to contain
   (`struct op_meta` descriptors, which the kernel validates and translates,
   versus the raw cmdq instruction words an old userspace would build).
2. Disassemble `DpDriver::waitFramedone` (`0x1d7d8`) to confirm which call
   generates the -26 and what it passes to `_IOR('x', 21, struct mdp_wait)`
   (`job_id` at offset 0, then `read_v1_result`, then `read_result`).
3. If still ambiguous, build a kernel that logs or stops swallowing the
   handler status, or strace cameraserver's cmdq fd to capture the exact
   submitted struct.

### Confirmed: the submit fails inside the kernel, and job_id proves it

Step 3 turned out not to need a kernel build. `tools/cmdq-trace/` is an
`LD_PRELOAD` interposer on `ioctl()` that decodes the cmdq traffic in place.
It recovers the swallowed status without touching the kernel, using the fact
that `mdp_ioctl_async_exec` writes `user_job.job_id` **only** on its success
path, at the very end, after `cmdq_mdp_handle_flush` has returned:

```
EXEC fd=95 cmd=40887814 metas=ead21100 count=167 prio=20 eng=150 ... job=0
EXEC ret=0 errno=0 job_id(after)=0
WAIT job=0 ret=0 errno=0
```

`ret=0` with `job_id` still zero is only reachable through an early return.
So the hypothesis above is now established fact rather than a working theory:
the ioctl is recognized and dispatched, the handler fails, `cmdq_ioctl`
discards the status and reports success, and the library's `checkHandle` then
turns the zero job handle into its own -26. The `-26` is downstream noise; the
real error is whatever `mdp_ioctl_async_exec` returned and threw away.

Note also that the wait ioctl is issued with `job=0`, which cannot match any
entry in `job_mapping_list`, so `mdp_ioctl_async_wait` takes its
`job not exists` path. That error is swallowed identically.

Two platform facts narrow which early return it is, and both are worth
knowing before reading further:

- **`mdp_engine_port[]` is all zeros for mt8163** (`cmdq/v2/mt8163/mdp_base.h`).
  `translate_fd()` starts with `port = cmdq_mdp_get_hw_port(meta->engine)` and
  returns 0 when that is 0, so **any `CMDQ_MOP_WRITE_FD` meta fails
  unconditionally on this SoC**. A userspace that passes buffers as dma-buf fds
  cannot work here at all; it has to pass MVAs as plain register writes.
- **`CMDQ_MOP_READ` requires a preallocated readback slot.** `translate_read_id`
  resolves `meta->readback_id` against the `rb_slot[]` table that
  `CMDQ_IOCTL_ALLOC_READBACK_SLOTS` (nr 22) fills in, and returns 0 for an
  unknown id, which `translate_meta` turns into `-EINVAL`. The trace contains
  **no nr-22 ioctl at all**, so if the library emits a single READ meta, the
  whole submit dies.

The good news from the same trace is that the parts that could have been
wrong are right. The library passes buffer addresses as ordinary masked
register writes to real mt8163 MDP offsets, not as fds:

```
meta[15] op=0 eng=1 off=0f00 val=1d900000 mask=ffffffff   # RDMA0 src base
meta[18] op=0 eng=1 off=0100 val=1d94b000 mask=ffffffff
meta[21] op=0 eng=1 off=0060 val=00000280 mask=001fffff   # 640 px
```

`eng` 1-5 are all `< ENGBASE_COUNT`, every `off` is `<= 0xFFC` and 4-byte
aligned, and the ops observed are only WRITE (0) and POLL (2). So the first 24
metas of each submit would all translate cleanly. The failure is in the
remaining 143 (or 207) metas, or in `handle_flush`.

**The instrumented run that answers this did not complete.** The tracer was
extended to validate all `meta_count` metas against `translate_meta`'s exact
rules and report the first eight violations with a per-op histogram, but the
device livelocked before its log could be read - see the warning below. Re-run
`tools/cmdq-trace/run.sh` after a power cycle; it is one cycle from naming the
failing meta.

If the metas all validate, the remaining suspect is `cmdq_mdp_handle_flush`
and the `engine_flag` the library sends (`0x150` and `0x369` here). Those bits
are `enum CMDQ_ENG_ENUM` positions, and a flag naming an engine this kernel
does not schedule would fail thread acquisition inside flush.

**Do not SIGKILL cameraserver while the ISP is streaming.** Doing so between
trace runs is what took the device down: it leaves M4U ports pointed at freed
buffers and the EMI MPU violation storm livelocks the whole system, exactly as
the permissions item below describes. The device stops answering ping and
needs a physical power cycle - there is no ADB, no watchdog reboot, and no
smart plug on it. Use `stop cameraserver`, or reboot
between runs.

### How to run cameraserver under a tracer

Two non-obvious requirements, both encoded in `tools/cmdq-trace/`:

`RTLD_NEXT` is `(void *)-1` in the glibc headers but **`0xfffffffe` on 32-bit
bionic**. Getting this wrong makes `dlsym` return NULL, the interposer
tail-calls through it, and the process spins at 100% CPU in a way that looks
exactly like a hang in the camera stack. The first attempt here lost time to
precisely that.

`cameraserver` also cannot just be started as root. `CameraService` checks the
caller's uid and rejects the app with

```
SecurityException: validateClientPermissionsLocked:1151: Untrusted caller
(calling PID 5656, UID 0) trying to forward camera access to camera 0
for client cameraserver (PID 0, UID -1)
```

so the camera app crashes in `getCameraCharacteristics` and the stack is never
exercised. `camwrap.c` drops to uid/gid 1047 with the rc file's groups plus
`media` (1013), then re-execs `cameraserver` with `LD_PRELOAD` preserved.
Since SELinux is permissive on this build, no policy work is needed.

### Superseded: runtime workarounds needed every boot

**These are now durable** - see "the camera now works from a cold boot" above.
`shims/libcmdqevent/camera-bringup.rc` performs all of it at `post-fs-data`.
Kept for the explanation of why each step matters.

Bring-up currently depends on this incantation as root after every boot, before
starting the camera:

```sh
chmod 666 /dev/mtk_cmdq /dev/mdp_sync /dev/MTK_SMI /proc/m4u
setprop debug.lsc_mgr.type 0
mount -o ro -t ext4 /dev/block/by-name/persist /persist
start cameraserver    # disabled in its rc during bring-up
am start -a android.media.action.STILL_IMAGE_CAMERA
```

Missing the m4u/SMI permissions is not benign: the ISP then DMAs through
unconfigured M4U ports and the resulting EMI MPU violation storm (`cam_aao`)
livelocks the whole device, which presents as a total freeze needing a power
cycle. Durable fixes wanted: ueventd entries or cameraserver group membership
for the device nodes, an init `chmod` for `/proc/m4u`, the lsc property as a
build property, the persist fstab entry, and re-enabling cameraserver's rc once
the stack stops taking the device down.

### Port bug: /persist is never mounted, so the camera has no calibration

The partition exists and contains this unit's factory data:

```
$ mount -o ro -t ext4 /dev/block/by-name/persist /persist
$ ls -l /persist
-rw-rw-rw- root root 1895 2021-11-10 12:56 calibration.bin
-rw-rw---- keystore  5490 2022-11-10 20:15 dha_certificate.pem
```

The ROM never mounts it, which is why the HAL logged
`can't open calibration data /persist/calibration.bin` and
`lsc_mgr2: [importEEPromData] Error(ERR_NO_SHADING)` from the very first run.
Mounting it makes both go away. This wants an fstab entry in the device tree, and
is likely worth reporting upstream independently of the camera - a factory
calibration partition going unmounted may affect other subsystems too.

### Worked around: lens shading crash

`debug.lsc_mgr.type=0` avoids the `LscRatioImp::genHwTbl` crash entirely - the
default selects the "Rto" (ratio) implementation, which is the one that null
derefs. With type 0 the 3A thread survives. `debug.lsc_mgr.enable` exists too but
does not prevent that path.

MediaTek's tuning libraries expose a useful set of these:

```
debug.lsc_mgr.enable   debug.lsc_mgr.type    debug.lsc_mgr.manual_tsf
debug.lsc_mgr.ratio    debug.lsc_mgr.ct      debug.isp_apply.disable
debug.isp_tuning_mgr.enable                  debug.ae_mgr.disableISP
```

Worth exploring further during bring-up. Note these are runtime-only; a real fix
either sets them as build properties or resolves why the ratio path has no table.

### Solved: ParamsManager::flatten aborts

```
signal 6 (SIGABRT)
#01 android::String8::append(String8 const&)+210      libutils.so
#02 android::CameraParameters::flatten() const+152    libcamera_client.so
#03 android::ParamsManager::flatten() const+52        libcam.paramsmgr.so
```

The HAL builds its parameter string and `String8::append` aborted on a
zero-size `String8` the legacy HAL produces, a case API 25 tolerated and
Android 11 made fatal. Fixed in source in `CameraParameters::flatten`, since
`libcamera_client` is built from this tree: see
`patches/0006-camera-flatten-tolerate-zero-size-String8-from-legacy-HALs.patch`,
built and installed on the device. `getParameters` now succeeds and the camera
app reaches its normal preview UI.

### Honest assessment of the remaining work

**This assessment has been overtaken by events: the preview now works.** It is
kept because its reasoning held up exactly.

It predicted a long tail of API 25 versus Android 11 differences, each needing
its own diagnosis, with no wall but no reason to think any given one was the
last. That is what happened, and the cmdq event encoding was the final entry in
the tail. Every blocker in this port turned out to be the same shape - the two
sides agreeing on the call and disagreeing on an encoding - and none of them
required the architectural surgery the original pessimism assumed.

What is left is bring-up polish rather than architecture: image quality (Bayer
order, lens shading, AWB), making the runtime workarounds durable, and moving
the cmdq event fix out of the bring-up `LD_PRELOAD` into `libcamera_shim`.

### Superseded: lens shading table generation

Even with the calibration file loading, the 3A thread still crashes:

```
#00 NSIspTuning::LscRatioImp::genHwTbl(uint, const uint*, uint*)+162   libfeatureio.so
#01 NSIspTuning::LscMgr2RtoCycle::tsfBatch()+66            null deref, fault 0x10
#02 NSIspTuning::LscMgr2Rto::tsfPostCmd(...)+228
#03 NSIspTuning::LscMgr2::updateTsf(...)+216
#04 NS3A::StateCameraPreview::sendIntent(intent2type<10>)+1254
```

So the calibration blob is read but the hardware shading table is still not
built. Candidates: the calibration format may be keyed to a sensor id the HAL
resolves differently, the TSF (shading transfer function) path may want tuning
data that lives in a `libcameracustom` entry for OV02B that this blob set does
not carry, or LSC may simply need disabling for bring-up. Worth checking whether
the 3A path can be told to skip TSF - a shading-less image is still an image.

### Superseded: where to go from here

The pattern so far has been "fix the next crash", and that worked through six
layers. It has now reached blob internals where each step is smaller and the
information per attempt is lower. Two strategic options rather than another
increment:

1. **Go back to the Android 9 `libdpframework` and mitigate the object size
   differently.** It matches the kernel, which removes the whole class of
   kernel-ABI doubt. The problem was `libcam.campipe.so` under-allocating
   `DpIspStream`. That could potentially be addressed by interposing an
   allocator, or by shimming the constructor to initialise only the fields the
   old layout has.
2. **Add old-MDP compat to the kernel**, the same technique that fixed
   `SET_MCLK_PLL`: find what the old library's ioctls encode versus what this
   kernel accepts, and alias them. This keeps the private library.

Option 1 is probably the better bet: it aligns userspace with the kernel rather
than adding more compatibility surface, and the object-size problem is bounded
and inspectable.

### Superseded: pipeline runs, buffers arrive empty

The preview surface renders **uniform green**, which is what zero-filled YUV
looks like - frames are being delivered on schedule but carry no image data. The
still path gets as far as `JpegCodec::encode` and then crashes inside
`libcam.camshot.so`, consistent with it being handed the same empty/unmapped
buffer.

Ruled out already:

- **M4U.** `m4u_config_port failed` is gone once `/proc/m4u` and `/dev/MTK_SMI`
  are accessible to `cameraserver` (see the permissions item below). The DMA
  remapper is configured.
- **Sensor streaming.** `irq_TG1_DONE` fires, so the ISP is receiving frame-end
  interrupts from a streaming sensor.
- **Buffer plumbing at the HIDL layer.** No import failures, no linker errors,
  and the display client accepts and cycles the buffers.

Remaining leads, in order:

1. **Bayer order.** `sensor_output_dataformat` was set to
   `SENSOR_OUTPUT_FORMAT_RAW_B` as a starting guess when the reference driver's
   `RAW_MONO` was rejected. The four `RAW_B`/`RAW_Gb`/`RAW_Gr`/`RAW_R` values are
   cheap to rotate through, and a wrong one plausibly produces a degenerate
   image. Try this first - it is a one-line change.
2. **Exposure and gain.** Check `set_shutter()` / the AE path actually programs
   non-zero values for this module; the paged register layout means an address
   from another module's table writes into the wrong bank silently.
3. **ISP output buffer address.** Confirm the ISP is writing to the buffer the
   display client later reads, rather than one the wrapping in
   `sDequeueBuffer` replaced. The `GraphicBuffer` wrapper passes
   `GRALLOC_USAGE_HW_CAMERA_WRITE | GRALLOC_USAGE_HW_TEXTURE` and the stride
   reported by `dequeueBuffer`; a mismatch with what the ISP assumes would give
   exactly this symptom.
4. **`BWC: fail to open mem_bw_ctrl driver file`** is still logged. That is EMI
   bandwidth profiling, expected to be cosmetic, but worth excluding if the
   above do not explain it.

### Superseded: the sensor does not stream

Every software layer now works. The failure is in the sensor itself:

```
IspDrv:     ISP_WAIT_IRQ fail(-1). Clear(1), Type(0), Status(0x00000400), Timeout(500)
iio/ifunc:  waitIrq( irq_TG1_DONE ) fail
iio/pathp1: waitBufReady fail
iio/camio:  ERROR:dequeueBuf
VSSScen:    dequePass1 fail
HalSensor:  [reset] pSensorDrv->open succeed, retry = 0 ... setScenario ... End reset
   (loops)
```

`irq_TG1_DONE` is the timing-generator interrupt raised when a MIPI frame
arrives. It never comes, so the HAL resets the sensor and retries forever.

**The sensor is confirmed present in hardware.** `pSensorDrv->open succeed` can
only happen if `return_sensor_id()` read back `0x2b` over I2C at write address
`0x78`, because `open()` returns `ERROR_SENSOR_CONNECT_FAIL` otherwise. So
OV02B10 is real, powered, clocked enough for I2C, and answering. What is not
happening is pixel streaming.

Likely candidates, roughly in order:

1. **Register init tables.** The ported driver's `sensor_init()` /
   `preview_setting()` came from an mt6765 device's OV02B10 module. OV02B10 is
   paged (register `0xfd` selects the bank) and vendors ship substantially
   different init sequences per module. This is the most likely culprit.
2. **MIPI configuration.** The driver declares `SENSOR_MIPI_1_LANE` and
   `mclk = 24`. If cronos wires two lanes, or a different MCLK, the ISP will wait
   forever for a frame that never arrives in the expected format.
3. **Streaming enable.** Check that `streaming_control(KAL_TRUE)` is reached and
   that its register write matches this module.
4. **Power sequence.** `camera_hw/camera_project/cronos/kd_camera_hw.c` has a
   dedicated OV02B branch with its own ordering and `NEED_MANUAL_VCAMI2C_POWER`
   handling; confirm the driver's expectations match it.

Debugging this needs the driver's own logs, so build with
`patches/0003-DEBUG-unsuppress-imgsensor-driver-logging.patch` **and** switch the
driver's `LOG_INF` from `no_printk` to `pr_err` - by default it is compiled out
entirely. Note that with `cameraserver` set to `disabled` in
`/system/etc/init/cameraserver.rc` (see below) a bad driver no longer stops the
device booting, so this is iterable.

This is ordinary sensor bring-up: I2C works, streaming does not. It wants a
datasheet or a known-good register dump for this module, not more architectural
work.

### Superseded: preview window contract analysis

```
#00 pc c714a0c2  <unknown>                                   ← call through a garbage pointer
#01 sp<ANativeWindowBuffer>::operator=(ANativeWindowBuffer*)+14      libcam.client.so
#02 NSDisplayClient::StreamImgBuf::StreamImgBuf(...)+104             libcam.client.so
```

MediaTek's `NSDisplayClient` assigns a preview buffer into an
`sp<ANativeWindowBuffer>`, which calls `incRef` through the buffer's
`android_native_base_t` function pointer - and that pointer is garbage.

The likely cause is a contract mismatch rather than a signature one. AOSP's HAL1
shim implements `preview_stream_ops` with HIDL callbacks
(`CameraPreviewWindow`), and its `sDequeueBuffer` hands back a plain
`buffer_handle_t*`. MediaTek's blob appears to expect the preview window to be
backed by a real `ANativeWindow`, whose buffers are full `ANativeWindowBuffer`
objects with working refcount pointers - which is how this worked on stock,
where the HAL ran against a Surface directly.

This is a deeper class of problem than everything above it: not a symbol or a
struct size, but what kind of object the two sides think they are exchanging.
Fixing it likely means making the shim's preview window present buffers as
proper `ANativeWindowBuffer`s (for example by wrapping each imported handle in a
`GraphicBuffer` and handing over its `ANativeWindowBuffer` view) rather than
passing raw handles. The shim is built from source, so this is editable - but it
is a design change, not a patch.

### Superseded analysis of the DpIspStream mismatch

```
#00 DpIspStream::DpIspStream(DpIspStream::ISPStreamType)+264   libdpframework.so
#01 NSCamPipe::XdpPipe::init()+16                              libcam.campipe.so
#02 NSCamPipe::ICamIOPipeBridge::init()+30
```

This is the blob-mixing problem the port maintainer described, and it is real.
`libcam.campipe.so` is API 25 and allocates a `DpIspStream` sized to the Android
7 definition; the `libdpframework.so` on this ROM is from the Android 9 blob set
and its constructor initialises a larger object, writing past the allocation.

Unlike the signature drift handled by `libcamera_shim`, a shim cannot fix this -
the caller's allocation is already too small before any of our code runs.

**Proposed approach, not yet tried:** give the camera stack its own
matching-vintage copy of the library rather than downgrading the system one.
Take the API 25 `libdpframework.so` from the stock dump, set its soname to
something private (`libdpframework_cam.so`), install it alongside, and
`patchelf --replace-needed` it into the camera blobs that link it. The display
and media stacks keep the Android 9 library, so the coupling that made
downgrading impossible no longer applies. Note that if the camera blobs get the
old library, the `DpIspStream`/`DpBlitStream` forwarders in `libcamera_shim`
become unnecessary and must be removed to avoid providing those symbols twice.

Risk: two copies of the display framework in one process may contend for the
same kernel resources (cmdq, m4u). Untested.

## Remaining work before capture succeeds

Enumeration works; frames do not yet flow. Known blockers, in the order they
appear:

1. **Device node permissions.** The HAL runs in `cameraserver` (uid 1047, groups
   audio camera input drmrpc), but on Android 7 it ran in `mediaserver`, so the
   nodes it needs are owned by `media`:

   ```
   crw-rw---- media  media  /dev/MTK_SMI
   -rw-r----- system media  /proc/m4u
   ```

   This produces `M4U_L: m4u_config_port failed` and
   `BWC: Open SMI(/dev/MTK_SMI) driver file failed: Permission denied`.
   `chmod 666` on both clears those errors, confirming the cause. The durable fix
   is to add `media` to cameraserver's groups, or relabel the nodes to `camera`
   via ueventd; `/proc/m4u` needs an init `chmod` since ueventd does not manage
   procfs.

2. **cronos has the wrong sensor driver, and the right one does not exist in this
   kernel.** This is the blocker, and it is not fixable by configuration.

   The stock cronos kernel's symbol table, from the firmware dump's
   `boot/boot.img-kallsyms`, settles it:

   ```
   $ grep -iE 'ov02b|ov9734' boot.img-kallsyms
   c051a1e0 T OV02B_MIPI_RAW_SensorInit      # present
                                             # zero ov9734 matches
   ```

   The Echo Show 5 2nd gen uses an **OV02B10** (2 MP). OV9734 is a 1 MP part -
   the 1st gen `checkers` sensor. Corroborating evidence:

   - both public cronos firmware dumps ship a `libcameracustom.so` whose sensor
     table contains *only* `ov02b10_mipi_raw` and `ov02b10_mipi_raw_vc`, no
     OV9734 at all;
   - the HAL's own search resolves index 0 to
     `<0x2b/ov02b10_mipi_raw/SENSOR_DRVNAME_OV02B_MIPI_RAW>`;
   - Echo Show 5 gen 2 is specced with a 2 MP camera, gen 1 with 1 MP.

   But every defconfig in R0rt1z2's kernel sets the gen-1 sensor:

   ```
   cronos    CONFIG_CUSTOM_KERNEL_IMGSENSOR="ov9734_mipi_raw"
   checkers  CONFIG_CUSTOM_KERNEL_IMGSENSOR="ov9734_mipi_raw"
   crown     CONFIG_CUSTOM_KERNEL_IMGSENSOR="ov9734_mipi_raw"
   rook      CONFIG_CUSTOM_KERNEL_IMGSENSOR="gc0312_mipi_raw"
   ```

   and **no ov02b10 driver exists anywhere in the tree** - not on
   `lineage-18.1`, not on the cronos-specific `cronos/lineage-18.1` branch. The
   kernel header declares `OV02B_MIPI_RAW_SensorInit` behind
   `#if defined(OV02B10_MIPI_RAW)`, so the slot is there and the driver was
   simply never included.

   This is probably why the `crown` work got further than cronos ever could:
   crown is a gen-1 device, so its `ov9734` setting is correct.

   ### Driver port attempt: builds correctly, does not boot

   The driver has been ported and is saved as
   `patches/ov02b10_mipi_raw-driver-WIP.tar.gz` plus
   `patches/0004-imgsensor-add-ov02b10-support-WIP-does-not-boot.patch`.

   It went in easily. A reference `ov02bmipiraw_Sensor.c` from an mt6765 tree
   (`danya2271/kuroneko_r_mt6765`) already uses the same
   `SENSOR_FUNCTION_STRUCT` and the exact `OV02B_MIPI_RAW_SensorInit` name our
   `kd_sensorlist.h` declares, so it compiled with a single fix - adding the
   missing forward declaration. What was needed:

   - `imgsensor/src/mt8163/ov02b10_mipi_raw/` with the sensor `.c`/`.h` and a
     one-line Makefile (`obj-y += ov02b10mipiraw_Sensor.o`);
   - `OV02B_SENSOR_ID 0x2b`, `OV02BMIPI_SENSOR_ID` and `OV02B_III_SENSOR_ID
     0x2c` in `kd_imgsensor.h` (none were defined);
   - the `OV02B_MIPI_RAW_SensorInit` prototype in `kd_sensorlist.h`;
   - `CONFIG_CUSTOM_KERNEL_IMGSENSOR="ov02b10_mipi_raw"` in `cronos_defconfig`.

   No Makefile plumbing beyond that: `imgsensor/src/Makefile` uppercases
   `CONFIG_CUSTOM_KERNEL_IMGSENSOR` into a `-D` flag, so `ov02b10_mipi_raw`
   automatically defines `OV02B10_MIPI_RAW` and activates the existing
   `kd_sensorlist.h` entry. Selecting only this sensor also puts it at kernel
   table index 0, which is where the HAL expects it - so the index-alignment
   problem resolves itself.

   The resulting kernel exports `OV02B_MIPI_RAW_SensorInit` and no ov9734
   symbols, matching the stock kernel's table exactly. **But it does not boot** -
   it hangs at the vendor logo and reboots. Verified as an isolated change: same
   base as a kernel that booted four times, no logging changes bundled in, only
   the sensor selection and new driver.

   **Bisected, and the cause is narrow.** Three flashes, one variable each:

   | `kdSensorList[]` index 0 | Boots? |
   | --- | --- |
   | ov02b10 (only sensor compiled) | no |
   | ov9734 (both compiled, canonical order) | **yes** |
   | ov02b10 (both compiled, reordered) | no |

   The determining factor is **ov02b10 sitting at index 0**, not whether ov9734
   is compiled in. So something on the boot path touches `kdSensorList[0]` -
   consistent with the ADC-based proto/HVT board identification in
   `kd_sensorlist.c` - and the ported driver hangs when it is the one exercised.

   That localises the bug to the ported driver's power-up path, most likely
   `get_imgsensor_id()` or `open()`: the mt6765 reference spins on I2C reads
   while polling for the chip ID, and on mt8163 the regulators, MCLK and reset
   GPIO are sequenced by `camera_hw/camera_project/cronos/kd_camera_hw.c`, whose
   OV02B branch has its own ordering and `NEED_MANUAL_VCAMI2C_POWER` handling.
   If the driver reads before that sequence has completed, an I2C read with no
   timeout will wedge.

   **Next step, no flash required:** read the reference driver's
   `get_imgsensor_id()`/`open()` retry loops against the in-tree
   `ov9734mipiraw_Sensor.c` equivalents and against the cronos OV02B power
   branch, and bound every I2C wait. Only flash once that is done, and keep
   ov9734 at index 0 while iterating so a failed driver cannot stop the device
   booting - the HAL will bind the wrong sensor, but the device stays usable and
   the driver can be exercised deliberately.

   Also worth checking:

   - `kd_sensorlist.c` does have a boot-time platform/i2c probe, and there is
     ADC-based board identification in there (`g_adc_id_iio_channel is null`) -
     the cronos board distinguishes proto from HVT, so something may key off the
     compiled sensor;
   - `camera_hw/camera_project/cronos/kd_camera_hw.c` implements both sensors'
     power sequences, so check whether its OV02B branch touches a regulator or
     GPIO that is unsafe early;
   - bisect by compiling **both** sensors
     (`CONFIG_CUSTOM_KERNEL_IMGSENSOR="ov9734_mipi_raw ov02b10_mipi_raw"`) to
     separate "the new driver breaks boot" from "removing ov9734 breaks boot".
     That last one is a single flash with a clear prediction either way.

   **What finishing this requires:** porting an ov02b10 sensor driver into
   `drivers/misc/mediatek/imgsensor/src/mt8163/ov02b10_mipi_raw/`, then setting
   `CONFIG_CUSTOM_KERNEL_IMGSENSOR="ov02b10_mipi_raw"` in `cronos_defconfig`.
   Existing ov02b10 drivers are available (for example
   `danya2271/kuroneko_r_mt6765` and `ramabondanp/kernel_xiaomi_angelica`, both
   at `imgsensor/src/common/v1/ov02b_mipi_raw/ov02bmipiraw_Sensor.c`), but they
   target MediaTek's newer `common/v1` framework while mt8163 uses the legacy
   per-platform layout. The register tables, ID check and mode setup port over;
   the surrounding `SENSOR_FUNCTION_STRUCT` plumbing has to be rewritten to
   match the in-tree `ov9734_mipi_raw` driver's shape.

   `patches/0003-DEBUG-unsuppress-imgsensor-driver-logging.patch` makes this
   tractable to debug: the driver routes every macro including `PK_ERR` through
   `pr_debug`, so by default none of its diagnostics reach dmesg at all.

   **Do not bother with sensor-table index alignment.** Adding a placeholder
   entry so the HAL's and kernel's table indices agree does change which sensor
   the HAL resolves (it moved from `ov02b10_mipi_raw` to
   `ov02b10_mipi_raw_vc`), but with no OV02B driver behind it there is nothing to
   talk to, and capture still fails. Tried and reverted.

### Superseded: sensor identification via table index

Kept because the mechanism is worth knowing. The kernel reports the sensor on
`CAM[1]` (MediaTek's sub/front slot) and names it after whatever driver is
compiled in:

   ```
   $ cat /proc/driver/camera_info
    CAM[1]:ov9734_mipi_raw;
   ```

   But the HAL only ever tries to build metadata for `OV02B`, a sensor this unit
   does not have, and never attempts OV9734 at all:

   ```
   constructCustStaticMetadata_DEVICE_SENSOR_SENSOR_DRVNAME_OV02B_MIPI_RAW
       returns status[Unknown error -1(-1)]
   ```

   With no valid static metadata there are no camera parameters, so
   `getParameters failed (empty parameters)` and every capture request fails.
   This is why the camera still does not produce frames.

   How the identification works: `kdSetDriver()` takes an *index* into
   `kdSensorList[]`, binds `pSensorList[idx].SensorInit`, and returns `-EIO` if
   that slot is NULL. `kdSensorList[]` is a compile-time array whose entries are
   each `#if defined(<SENSOR>_MIPI_RAW)`, so it collapses to just the sensors
   built in - with only `ov9734_mipi_raw` enabled it has one entry at index 0.
   The HAL carries its own copy of the same table (the drvname strings live in
   `libcameracustom.so`, which has exactly `ov02b10_mipi_raw` and
   `ov9734_mipi_raw`) and the header comment says the two "should be the same".

   **Attempted and reverted:** adding a placeholder `{0,
   SENSOR_DRVNAME_OV02B_MIPI_RAW, NULL}` entry ahead of OV9734, on the theory
   that the HAL's index 0 is ov02b10 and a NULL init would make the kernel
   reject it so the HAL would move on. The result changed `Facing` from Front to
   Back but the HAL still resolved OV02B and still built no OV9734 metadata, so
   the theory is wrong or incomplete. Reverted.

   **Next step, and do it with evidence rather than guessing:** `kdSetDriver()`
   already has `PK_INFO("pDrvIndex:0x%08x/0x%08x")` at its head, but that log
   level is not reaching dmesg. Build a kernel with that promoted to `pr_err`
   (and one in the NULL-init rejection path), then read exactly which indices
   and socket bits the HAL requests and in what order. That pins the mapping in
   a single flash cycle instead of guessing at orderings. Note the HAL also logs
   `GetCameraCalData(MainSensorIdx=2b)`, so `0x2b` is worth understanding - it
   may be an ID rather than an index.

3. **`getParameters failed (empty parameters)`** from the HAL1 shim, which is
   likely downstream of (2) - with no valid static metadata there are no
   parameters to return.

4. **ION**, still logging `ioctl c0044901 failed with code -1: Invalid argument`,
   as the `crown` investigation also saw. Not yet known to be fatal.

## What this does and does not prove

It proves the stack will *link*, and that the mixing problem is far smaller than
"the ABI does not line up" suggests — 11 shimmable symbols, all clean parameter
drift, no struct-layout surgery and no binary patching required to get loaded.

It does not prove the stack will *work*. Symbol resolution says nothing about
semantics, and the known hazards are all semantic:

- **cmdq.** The kernel's command-queue driver is the newer `CONFIG_MTK_CMDQ_TAB`
  variant from the Android 9 karnak kernel, while these blobs were built against
  the older one. Whether the ioctl surface they use still behaves identically is
  untested. This is the risk that motivated the original pessimism and it is not
  addressed by anything above.
- **ION.** The `crown` investigation found the gralloc mapper's first
  passthrough fetch failing on cold boot with `ion: ioctl c0044901 failed`,
  which suggests allocator readiness is genuinely marginal at that point in boot.
- **Struct layouts** shared between the camera blobs and `libdpframework` /
  `libgralloc_extra` (for example `ISP_TPIPE_CONFIG_STRUCT`) are not verifiable
  statically. A silently changed field offset would surface as a crash or
  corrupted frames rather than a link error.

The honest read is that this converts an open-ended research problem into a
bounded engineering one, with a real chance of failing somewhere in the cmdq or
ION layer that no amount of static analysis will predict. The next step is
empirical: build it, install it, and read the tombstones.

## Approaches ruled out

- **Transplant karnak's Treble camera stack.** Best-looking option on paper —
  newer blobs, matching kernel vintage, proven on modern LineageOS — but no
  OV9734 tuning and no way to add it.
- **Build the HAL from source.** MediaTek MT8163 camera HAL sources are not
  available.
- **Downgrade the kernel's cmdq driver.** The display and media stacks depend on
  it; this is the tradeoff the port maintainer described.

## Reproducing the measurements

```sh
scripts/fetch-camera-blobs.sh      # download stock camera stack from the dump
scripts/pull-device-libs.sh        # pull this device's platform libraries
python3 scripts/deps.py            # dependency closure
python3 scripts/syms.py            # unresolved symbol report
```

Note when reading `syms.py` output that libc exports `memcpy`, `memset`,
`strlen` and friends as ifuncs, which naive readelf column parsing reports as
missing. They are present.
