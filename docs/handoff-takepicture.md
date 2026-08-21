# RESOLVED: `takePicture` never delivers a JPEG (cronos, LineageOS 18.1)

**This bug is fixed.** Root cause: the OV02B10 driver's `.cap1` and video
mode structs in `imgsensor_info` were commented out (all zeros), `capture()`
selects `cap1` whenever AE has lowered the frame rate, and `write_shutter()`'s
unsigned `(frame_length - 0x4c4)` dummy-line computation then underflows to
0xfb3c, slowing the sensor to one frame every 1.78 s. Every 500 ms
`ISP_WAIT_IRQ` misses and no error fires because nothing is broken - the
sensor is just 54x too slow. Fix:
`patches/0010-imgsensor-ov02b10-restore-capture-and-video-mode-timing.patch`.
Full write-up in `docs/findings.md` ("SOLVED: takePicture never delivered a
JPEG"). Verified: repeatable full-resolution JPEGs, zero waits failing,
preview live after capture.

The investigation notes below are kept because the tooling and the
elimination order are reusable. The headline lesson: `IrqStatus(0)` with no
error interrupt and correct ISP state means *count the vsyncs* - an
out-of-band `ISP_WAIT_IRQ` probe on a second `/dev/camera-isp` fd
(`tools/cmdq-trace/ispwait.c`) measured VS1 arriving at 1.78 s intervals,
which named the sensor and, via 65536 lines x 27.15 us, the exact underflow.

Read `docs/findings.md` for the full port history.

## The defect

Any Camera1 `takePicture` - the stock camera app, or anything the camera2
legacy shim / CameraX `ImageCapture` issues - enters the HAL and never returns
`CAMERA_MSG_COMPRESSED_IMAGE`. The legacy shim gives up after ~4.5 s. **No
capture-error callback is generated**, so callers see silence rather than a
failure, and a caller that guards with an "in progress" flag wedges until
restart.

Userspace, per capture attempt:

```
MtkCam/Cam1Device: [Cam1DeviceBase::takePicture] +
IspDrv: ERROR: ISP_WAIT_IRQ fail(-1). Clear(1), Type(0), Status(0x00000400), Timeout(500).
IspDrv: ERROR: ISP_WAIT_IRQ fail(-1). Clear(1), Type(0), Status(0x00000400), Timeout(500).
IspDrv: ERROR: ISP_WAIT_IRQ fail(-1). Clear(1), Type(0), Status(0x00000001), Timeout(500).
RequestThread-0: Hit timeout for jpeg callback!
```

Kernel, same attempt:

```
[Camera-ISP][ISP_WaitIrq]ERROR:Clear(1),Type(0),IrqStatus(0x00000000),WaitStatus(0x00000400),Timeout(500)
[Camera-ISP][ISP_DumpReg]ERROR:[0x15008008 00003210],[0x15008100 00008001]
[Camera-ISP][ISP_ioctl]ERROR:Fail, Cmd(1074817798), Pid(506), (process, pid, tgid)=(Capture@CmdQue, 2414, 330)
...
+SetIrqEnable(), Irqmode = 0, bEnable = 1
```

## What the evidence actually says

**The interrupts being waited on are pass 1, not pass 2.** Decoded against
`kernel/.../cameraisp/src/mt8163/inc/camera_isp.h`:

| Wait status | Bit | Symbol |
| --- | --- | --- |
| `0x00000400` | 10 | `ISP_IRQ_INT_STATUS_PASS1_TG1_DON_ST` |
| `0x00000001` | 0 | `ISP_IRQ_INT_STATUS_VS1_ST` |

Pass 1 is sensor-into-ISP. So this is **not** the JPEG/encode stage failing.
The sensor stops feeding the ISP the moment the HAL switches to the capture
scenario, and everything downstream waits forever.

**No interrupts are latched at all.** The kernel prints
`IrqStatus(0x00000000)` - the status register is empty, not "wrong bit set".

**The failing thread is the capture thread**: `Capture@CmdQue`.

**The sensor is not reprogrammed differently for capture.** `preview_setting()`
and `capture_setting()` emit the *same 91 register writes with the same
values*. Verified by diffing the two functions programmatically. A wrong
capture register table is therefore not the cause.

## Already ruled out - do not spend time here

1. **"A foreign ioctl is being rejected on the ISP fd."** An earlier revision of
   `findings.md` decoded `Cmd(1074817798)` as magic `'H'` and built a theory on
   it. **The arithmetic was wrong.** It is `0x40106b06`: `_IOW`, size 16, magic
   `0x6b` = `'k'` = `ISP_MAGIC`, nr 6 = **`ISP_CMD_WAIT_IRQ`**. It is the ISP's
   own wait call reporting the timeout already visible in logcat. One symptom,
   not a second bug.

2. **Standby before reprogramming the mode registers.** The mode setters
   reprogram PLL and timing and are reached mid-stream on preview -> capture, so
   bracketing them with `streaming_control(KAL_FALSE/TRUE)` looked right. It was
   implemented, built, flashed and tested: the capture times out on exactly the
   same interrupts. Kept as `patches/0008-*` because the sequence is correct
   regardless, but **it is not a fix**.

3. **The LSC/TSF capture-table errors.** A previous report cited
   `shadingTblAlign Err`, `min_gain is 0` and `MTKTSF_FEATURE_GEN_CAP_TBL`
   failures as likely related. They no longer occur - mounting `/persist` so the
   factory calibration loads fixed them - and `takePicture` still fails
   identically. Separate issue, already closed.

4. **DMA/bandwidth starvation.** No M4U, SMI or EMI errors appear during a
   failing capture. `/dev/MTK_SMI`, `/dev/mtk_cmdq`, `/dev/mdp_sync` and
   `/proc/m4u` are all 0666 from boot via `camera-bringup.rc`.

## Where to look next

The preview path drives DISPO at 640x480 from a 1600x1200 sensor input and runs
indefinitely. Capture switches pass 1 to full-resolution IMGO. The sensor
streams fine until exactly that switch, so the suspicion is the **IMGO / FBC
path or its tpipe configuration**, not the sensor driver.

Concrete starting points:

- Decode the two dumped registers. `0x15008008 = 0x00003210` and
  `0x15008100 = 0x00008001` are in the ISP/camsys block (for reference,
  `ENGBASE_IMGSYS` is `0x15000000` and `ENGBASE_ISP_CAMSYS` is `0x15004000` in
  `cmdq/v2/mt8163/mdp_base.h`). Compare these values against a *working*
  preview frame to see what capture changes.
- Follow `SetIrqEnable()` in `cameraisp/src/mt8163/camera_isp.c`. The trace
  shows `Irqmode = 0, bEnable = 1` arriving *after* two failed waits and
  `bEnable = 0` at teardown. Check whether the capture path enables the pass-1
  interrupt too late, or masks it, or clears it on the wrong `Clear(1)` path.
- Instrument the kernel ISP driver. Its prints are already reaching dmesg
  (unlike the imgsensor driver), so adding detail is cheap.
- Check whether the TG is actually still running during the capture attempt by
  reading the TG registers, which distinguishes "sensor stopped" from
  "interrupt not delivered". This is the key fork in the road: both produce
  `IrqStatus(0)`, and they lead to completely different fixes.

## Reproducing

```sh
adb -s <device-ip>:5555 shell 'input keyevent KEYCODE_WAKEUP; am start -a android.media.action.STILL_IMAGE_CAMERA'
sleep 14
adb -s <device-ip>:5555 shell 'dmesg -c >/dev/null; logcat -c; input tap 887 210'   # shutter button
sleep 10
adb -s <device-ip>:5555 shell 'logcat -d | grep -iE "ISP_WAIT_IRQ|Hit timeout|takePicture"'
adb -s <device-ip>:5555 shell 'dmesg | grep -i camera-isp'
```

`input tap 887 210` is the shutter in the stock camera app on this 960x480
panel. It reproduces 100% of the time.

## Tooling you already have

Built during this port; all of it avoids kernel builds and boot flashes, which
is what made progress possible. See `tools/cmdq-trace/`.

- `build.sh <serial>` - builds everything below against the *device's own*
  bionic (pulled from the target), because these are `-nostdlib` ARM32 binaries.
- `ioctl_hook.c` -> `libcmdqevent_shim.so` (shipped as `shims/libcmdqevent/`) -
  `LD_PRELOAD` ioctl interposer inside cameraserver. Already rewrites cmdq
  event ids; it is the obvious place to log or rewrite ISP ioctls too. Set
  `persist.camera.cmdqtrace 1` for tracing.
- `cmdqprobe.c` - submits hand-built jobs directly to `/dev/mtk_cmdq` to bisect
  what the kernel accepts.
- `sensorpoke.c` - reads/writes sensor registers at runtime via
  `KDIMGSENSORIOC_X_FEATURECONCTROL`. **Caveat: it currently returns 0 for
  everything**, because the HAL closes `/dev/kd_camera_hw` after configuring
  and the kernel's dispatch loop needs an enabled driver. Would need a
  `SET_DRIVER` call first.
- `flip-sweep.sh` - pattern worth copying: expose the thing you are unsure about
  as a **module parameter**, then sweep it by reopening the camera app. That is
  how the OV02B10 mirror register was found without a flash per guess.

Two gotchas that cost time: `RTLD_NEXT` is `0xfffffffe` on 32-bit bionic (not
`-1`), and with `-nostdlib` there is no crt0, so `main` gets no argv - pass
parameters via environment variables.

## Environment

- Device: `cronos`, network adb at `<device-ip>:5555`. `adb root` is needed
  after every reboot.
- Build tree: wherever you synced it (`$LINEAGE_TREE`).
  `source build/envsetup.sh && lunch lineage_cronos-userdebug && mka bootimage`
  takes ~20-40 s incrementally.
- Flash: `scripts/flash-boot.sh <serial>`. It backs up first and verifies
  byte-for-byte. **Requires amonet 2.0.1 or newer**, which writes the build's
  plain `boot.img` straight to the partition; the script refuses to run
  against a still-unupgraded 1.x device. `fastboot flash boot <boot.img>`
  does the same job without a booted system. Note `cronos` may still be on
  1.x: check with the command in [INSTALL.md](INSTALL.md) step 0 and upgrade
  before flashing. Never restore a boot backup taken before that upgrade.
- Kernel ISP driver: `kernel/amazon/mt8163-4.9/drivers/misc/mediatek/cameraisp/src/mt8163/`
  (`camera_isp.c`, `inc/camera_isp.h`).
- Sensor driver: `.../imgsensor/src/mt8163/ov02b10_mipi_raw/`.
- The userspace side (`isp_drv.cpp`, `libcam.*`) is **closed-source blobs** in
  `vendor/amazon/cronos/proprietary/`. Disassembly is the only option there;
  `objdump` from `prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin/`
  works and understands the Thumb code.

## Safety rules - read before touching the device

1. **Never `SIGKILL` or `stop` cameraserver while the ISP is streaming.** It
   leaves M4U ports pointed at freed buffers and the resulting EMI MPU violation
   storm livelocks the entire device - no adb, no ping, no watchdog reboot.
   **Use `adb reboot` instead.** This happened twice during this work.
2. **There is no remote power control for this device.** A livelock means
   someone has to power cycle it by hand, so progress stops until they can.
   Treat rule 1 as hard.
3. Recovery from a bad kernel is TWRP/amonet, which is intact, plus the backups
   under `backups/`. A bad `/vendor` write is *not* trivially recoverable - the
   device has no A/B slot and no stock backup.

## Verify you have not regressed anything

After any change, these should all hold from a cold boot with no manual steps:

```sh
adb shell 'pm list features | grep camera'      # camera.any + camera.front, NO plain camera
adb shell 'dumpsys media.camera | grep Orientation'   # 0
adb shell 'logcat -d | grep -cE "startStream fail|deque DISPO fail"'   # 0
```

Preview should stream (`aaa_state_camera_preview` frames, AWB converging) and
any camera app's still capture should return a JPEG.

Success for *this* bug is the stock camera app's shutter, or any
CameraX/camera2 client's still capture, producing a full-resolution still
through the capture pass instead of a preview-sized YUV fallback.
