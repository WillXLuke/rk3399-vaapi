# Handoff: RK3399 H.264 hardware encoder (VEPU2) — VA-API

Status at end of this session: **not implemented on the board.** The decoder
(VA-API via rkvdec) is complete, installed and bit-exact; the encoder path is
planned and its reference material is collected here. Two parallel attempts
(kernel port, VA-API frontend) did not complete within the session.

## Why this is a driver port and not a small change

- Mainline 7.3.1 implements **only JPEG encoding** for RK3399
  (`rk3399_vpu_variant.codec = HANTRO_JPEG_ENCODER | MPEG2_DEC | VP8_DEC`;
  `/dev/video1` enumerates `CAPTURE=JPEG`).
- The May-2026 stateless-encode series (14 patches, `media: Add V4L2 H.264
  stateless encode and VC8000E support`, Paul Kocialkowski) adds the UAPI/core
  plus a **VC8000E** backend (i.MX8MP/RK3588 class) — not RK3399 VEPU2.
- Bootlin's `hantro/h264-encoding-v5.11` **does** contain an RK3399 VEPU2
  H.264 encoder (`rk3399_vpu_hw_h264_enc.c`) and was reported working on RK3399
  by Pengutronix on LKML (2022). That is the port source.

## Assets in this directory

- `diffs/01-hantro.diff` — full encoder delta, Bootlin branch vs vanilla v5.11
  (1386 lines: `rk3399_vpu_hw_h264_enc.c`, encoder parts of `hantro_h264.c`,
  `hantro.h`, `hantro_hw.h`, `hantro_v4l2.c`, `hantro_drv.c`,
  `rockchip_vpu_hw.c`).
- `diffs/02-videodev2.h.diff`, `diffs/03-v4l2-controls.h.diff`,
  `diffs/04-v4l2-ctrls.c.diff` — UAPI/core additions (3 CIDs, 3 control types).
- `VA-FRONTEND-SPEC.md` — the VA-API encode frontend contract and SPS/PPS rules.
- Reference trees (read-only):
  - `/home/durian/workspace/refs/bootlin-linux` (bootlin/h264-encoding-v5.11)
  - `/home/durian/workspace/refs/v511` (vanilla v5.11)
  - `/home/durian/workspace/refs/enc-tool` (Bootlin userspace test encoder)
  - `/home/durian/workspace/refs/mpp` (Rockchip MPP, `hal/vpu/h264e/hal_h264e_vepu2_v2.c`)

## Port plan (kernel)

1. Copy `linux-7.3.1/drivers/media/platform/verisilicon` to a work dir; apply
   `01-hantro.diff` adapting names: `rk3399_vpu_regs.h` → `rockchip_vpu2_regs.h`,
   `hantro_v4l2.c` control plumbing to 7.3.1, `hantro_enc_buf`/`rec_buf` fields,
   add `HANTRO_H264_ENCODER` to the rk3399 variant + `enc_fmt` for H264.
2. Port UAPI/core: `V4L2_CID_STATELESS_H264_ENCODE_{PARAMS,RC,FEEDBACK}`,
   structs, `V4L2_CTRL_TYPE_H264_ENCODE_*`. In 7.3.1 the table lives in
   `drivers/media/v4l2-core/v4l2-ctrls-core.c` (+ `-defs.c` names,
   `v4l2-ctrls-priv.h` type ops). `videodev.ko` contains v4l2-ctrls → must be
   rebuilt and swapped **with backups** (see `kernel/build-rkvdec.sh` pattern).
3. Build: configure `linux-7.3.1` with `/boot/config-7.3.1-nanopc-t4`
   (`cp`, `make olddefconfig`, `make modules_prepare`), then
   `make drivers/media/v4l2-core/` and the verisilicon dir (or M= for the
   driver alone).
4. Load order: unload `rockchip_vdec`, `hantro_vpu`, `rockchip_rga`,
   `videodev`, `videobuf2_*`, `v4l2_*`; install new `.ko`s; `modprobe` back.
   Verify `/dev/video1` CAPTURE=`H264`; keep `backup/` of every replaced `.ko`
   and restore on failure.
5. Validate with the Bootlin tool or raw V4L2: encode one NV12 frame, decode
   the stream with `ffmpeg -i out.h264 -f null -`.

## Port plan (VA-API)

Follow `VA-FRONTEND-SPEC.md`: `VAEntrypointEncSlice` for H.264 Baseline/Main/
High, CQP only, SPS/PPS serialised from `VAEncSequenceParameterBufferH264`
(`pic_order_cnt_type = 2`, `log2_max_frame_num_minus4 = 12`), one
`VACodedBufferSegment` per frame. Keep the existing decode path untouched.

## Risk notes

- No public VEPU2 H.264 driver for 7.3.1 exists; this is bring-up work with
  hardware trial and error (the Bootlin stack is ~5.11 era but proven on this
  SoC).
- The alternative (vendor BSP kernel + MPP) gives working VPU encode but
  replaces the Armbian kernel and loses the VA-API stack; still no VA-API.

## Attempt log (this session)

- `patch` was missing at first (explains earlier bogus "applies with fuzz" result).
  Installed; the Bootlin→7.3.1 delta is real but needs manual merging.
- `driver/` here is a copy of 7.3.1 `verisilicon/` with the Bootlin encoder diff
  applied. Applied cleanly: `hantro_h264.c` (encoder functions), `hantro_hw.h`,
  `rk3399_vpu_hw_h264_enc.c` (new file).
- Rejected hunks (4): `hantro.h` (codec mode + h264_enc union + `result` field),
  `hantro_hw.h` (thread helpers), `hantro_v4l2.c` (fmt height typo hunk),
  `hantro_drv.c` (queue setup + `hantro_enc_buf` + threaded completion).
- Key mismatch: Bootlin's encoder uses a threaded completion model
  (`hantro_thread_done`, `ctx->result`, `hantro_watchdog_kick`) that 7.3.1's
  driver does not have (`hantro_drv.c` only has `hantro_run`/`hantro_irq_done`).
  That infrastructure has to be ported or the encoder rewritten to the current
  completion model.
- Not yet done: `rk3399_vpu_hw.c` → 7.3.1 `rockchip_vpu_hw.c` mapping (enc fmt,
  codec ops, variant codec bits), regs header (`rk3399_vpu_regs.h` →
  add encoder macros locally over `rockchip_vpu2_regs.h`), UAPI/core
  (`v4l2-ctrls-core.c`), build, install, hardware test.

### Round 2 progress
- Kernel UAPI/core additions for the three encode controls are DONE and compile:
  modified `include/uapi/linux/v4l2-controls.h`, `include/uapi/linux/videodev2.h`,
  `drivers/media/v4l2-core/v4l2-ctrls-core.c`, `v4l2-ctrls-defs.c` in
  `linux-7.3.1`; copies in `uapi/`. `videodev.ko` builds with matching vermagic
  (needs `KBUILD_MODPOST_WARN=1` because the tree has no full Module.symvers).
- `videodev.ko` was installed once, then the original was restored and verified
  (decode works). Activate only together with the encoder driver.
- Still blocking the driver: Bootlin's threaded completion model
  (`hantro_thread_done`, `ctx->result`, watchdog kick) does not exist in 7.3.1;
  `hantro.h`/`hantro_drv.c`/`hantro_v4l2.c` rejected hunks revolve around it.
  Remaining: port that model (or rework the encoder onto `hantro_irq_done`),
  map `rk3399_vpu_hw.c` -> `rockchip_vpu_hw.c` (enc fmt/codec ops/variant),
  add encoder register macros, then build/load/test.

## BREAKTHROUGH (fresh session): encoder works

- The "threaded completion model" was already present in 7.3.1
  (`hantro_job_done`/`hantro_thread_done`); the only fix needed was making the
  VEPU IRQ call `hantro_thread_done()` instead of `hantro_irq_done()` so the
  request is completed for encoders.
- Ported driver builds as `driver/hantro-vpu.ko`; node /dev/video0
  (rockchip,rk3399-vpu-enc) advertises CAPTURE JPEG + `S264`, and registers
  h264_encode_parameters / _rate_control / _feedback controls.
- Rebuilt `videodev.ko` (UAPI/core) is now ACTIVE (media stack reloaded; note
  snd_usb_audio had to be unloaded to release `mc`).
- Bootlin tool (`refs/enc-tool/build/v4l2-hantro-h264-encoder`) encodes
  10 frames 640x480 and `ffmpeg -i capture.h264 -f null -` decodes them cleanly
  (High profile, level 3.1). Stream starts: SPS(67) PPS(68) slice(25).
- VA-API decode still verified working with the swapped videodev.
- Kernel-side acceptance: DONE. Remaining: VA-API encode frontend.

### Installed state (persistent until kernel upgrade)
- `/lib/modules/$(uname -r)/kernel/drivers/media/platform/verisilicon/hantro-vpu.ko`
  = `driver/hantro-vpu.ko` (encoder support).
- `/lib/modules/$(uname -r)/kernel/drivers/media/v4l2-core/videodev.ko`
  = `uapi/videodev-enc.ko` (encode UAPI/ctrl types).
- Originals in `backup/` (`hantro-vpu.ko.orig`, `videodev.ko.orig`).
- Reload procedure: unload `hantro_vpu`, `rkvdec`, `rockchip_rga`, all
  `videobuf2_*`/`v4l2_*`, then `videodev`, `mc` (unloading `snd_usb_audio` may be
  needed to release `mc`), then `modprobe videodev hantro_vpu rkvdec`.
- Rebuild: `make -C /home/durian/workspace/kernel/linux-7.3.1 M=$PWD KBUILD_MODPOST_WARN=1 modules`
  (the 7.3.1 tree is configured from /boot/config-7.3.1-nanopc-t4).
