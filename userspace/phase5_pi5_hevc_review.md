# Phase 5 review — iter40 plan (sonnet review + amendments)

Reviewer verdict: **yellow** — plan substantively sound, 3 concrete blockers
+ 1 fixture gap + 1 verification-only note. All findings verified empirically
against current source (per [[feedback_review_empirical_over_theoretical]])
BEFORE accepting into the amended plan.

## Reviewer findings + verification + amendments

### F1 (CRITICAL accepted) — `__arm__` guard kills detile on AArch64

Empirical verification: `src/image.c` lines 239 + 268 wrap the entire
per-format detile dispatch (incl. `nv15_unpack_plane_to_p010`) in
`#ifdef __arm__`. Pi 5 / fresnel / ampere are all AArch64 → guard never
fires → both NC12 detile (proposed) AND existing NV15→P010 unpack
(iter39) are silently dead code on aarch64. iter39 5/5 PASS on fresnel
was bit-exact for 8-bit codecs only; the 10-bit detile path was never
exercised, so the dead-code didn't manifest as a failure.

**Amendment:** Phase 6 step 5 first sub-action — change guard at lines
239 + 268 from `#ifdef __arm__` to `#if defined(__arm__) || defined(__aarch64__)`.
This re-enables the existing NV15→P010 detile AND lets the new NC12
detile branch execute. No semantic change on x86 (no detile primitives
compiled there). Add explicit comment crediting Phase 5 review + this
finding.

### F2 (CRITICAL accepted, scope clarified) — `destination_sizes` for NC12 in RequestCreateImage

Empirical verification: `src/image.c` lines 90-115 already recompute
`destination_bytesperlines[0]` + `destination_sizes[0]` for `P010`
(line 90: `destination_bytesperlines[0] = width * 2`). The fall-through
"NV12" branch (line 108) uses V4L2-reported stride directly, which for
NC12 source is the column-stride 1080, not the linear Y stride 1280.
That breaks the VAImage's `pitches[0]` consumers expect.

`context.c` lines 379-383 — `destination_sizes[0] = destination_bytesperlines[0] * format_height` — IS used at cap_pool init time to size the
CAPTURE buffer's MMAP region accounting in `driver_data->fmt_sizes[]`.
For NC12: 1080 × 720 = 777600 vs actual `sizeimage` 1382400. cap_pool
allocates the actual `sizeimage` via REQBUFS, so the underlying buffer
is correctly sized; `fmt_sizes[]` is just a back-cache for later access
patterns that don't go through the kernel-reported value.

**Amendment:**

- Phase 6 step 5 second sub-action — in `RequestCreateImage` (image.c
  ~line 107, the "else" / NV12 branch), add detection: if the source
  CAPTURE format is `V4L2_PIX_FMT_NV12_COL128` AND the requested image
  format is `VA_FOURCC_NV12`, override `destination_bytesperlines[0] =
  width` (linear NV12 Y stride). `destination_sizes[0]` then computes
  to `width × format_height` (correct linear Y plane size). Existing
  NV12-source linear path unchanged.
- Phase 6 step 3 video.c — set `v4l2_buffers_count = 1` for NC12 (single
  contiguous buffer holding Y+UV) and document this is the planes-1
  multi-plane case (similar to NV12 MPLANE).
- context.c lines 380-383 (`destination_sizes[0] = bytesperlines * height`)
  stays AS-IS for now. It only affects cap_pool MMAP accounting which
  uses the kernel-reported `sizeimage` via REQBUFS anyway. If a future
  bug emerges from this mismatch on the rkvdec/hantro side, address
  then; not a blocker for iter40 NC12.

### F3 (CRITICAL accepted) — `rpi-hevc-dec` missing from primary-driver detection in probe loop

Empirical verification: `src/request.c` lines 647-657 only have `else if`
branches for `rkvdec` and `hantro-vpu`. On higgs (no rkvdec, no hantro)
the primary device IS `rpi-hevc-dec`, but neither branch matches → no
`primary_driver` set → no fds stored into the new
`video_fd_rpi_hevc_dec` slot → routing silently no-ops with -1 fds.

**Amendment:** Phase 6 step 2 sub-action — add explicit `else if (strcmp(info.driver, "rpi-hevc-dec") == 0)` branch in the primary-driver
detection block. Sets `video_fd_rpi_hevc_dec = video_fd` + `media_fd_rpi_hevc_dec = media_fd`. Pi has no alt — `alt_driver` stays NULL,
no second-decoder probe runs for higgs. (rkvdec + hantro alt-probes
remain dead on higgs because the find_decoder_device_by_driver walk
returns absent for them.)

Also: extend `find_decoder_device_by_driver`'s driver-name table at
request.c:94-95 if needed to include `rpi-hevc-dec` — verify it's a
free-form string match (it is, per the code), not a hard table — so the
caller passes `"rpi-hevc-dec"` and the walk just looks for it.

### F4 (ACCEPTED, partial) — 1366×768 fixture catches column-misalignment bugs

The N=3 baseline uses 640 / 1280 / 1920 — all 128-aligned widths. A
1366-wide fixture exercises the `ALIGN(width, 128) → 1408` column
padding path. The right-edge 42 pixels (cols 1366-1407) are padding;
the detile primitive must not write past the requested width.

**Amendment:** Phase 7 sub-action — add `bbb_1366_main.mp4` (1366×768)
to the Phase 7 verification set. Phase 3 baseline retroactively
captured at Phase 7 time. Goal: same kdirect/SW bit-exact PASS at
N=1 (no need to redo the deterministic N=3 — one rep proves the
edge-case). If libva differs from kdirect on 1366 but matches on
1280/1920, the detile column-base math is buggy.

### F5 (ACCEPTED, verify-only) — explicit `hevc_decode_mode` + `hevc_start_code` setting

**Empirical NEW issue surfaced during verification (not in reviewer's
report):** `src/context.c` lines 516-528 unconditionally sets
`V4L2_CID_STATELESS_HEVC_START_CODE` to `_ANNEX_B` (value 1) AND
prepends `0x00 0x00 0x01` start codes to each slice payload (per the
H.264 mirror block at line 532+). But Phase 0 strace shows kdirect uses
`start_code=0` = `_NONE` and submits raw NAL slice payload WITHOUT start
codes.

Both modes are in rpi-hevc-dec's menu range (min=0 max=1). Open
question: does rpi-hevc-dec correctly parse start-code-prepended
payload when in ANNEX_B mode? Two possibilities:
  (a) Yes — driver implements both modes, ANNEX_B works, libva PASSes
      bit-exact in our default code path.
  (b) No — driver only really implements NONE; ANNEX_B is a degenerate
      menu entry; we'd need per-driver gating to send `_NONE` for
      rpi-hevc-dec + suppress start-code prepend.

**Amendment:** Phase 7 — verify empirically via the first libva-vs-kdirect
diff. If (a), no code change needed. If (b), add per-driver gate around
the START_CODE set (mirror rkvdec/hantro pattern). Don't pre-emptively
gate; let empiricism decide.

### F6 (CRITICAL accepted) — Synthetic SPS pre-seed fires on rpi-hevc-dec

Empirical verification: `src/context.c` lines 277-346 — the iter25
synthetic-SPS injection block runs for `VAProfileHEVCMain` regardless
of active driver_kind. On higgs, `driver_data->video_fd` will be
`video_fd_rpi_hevc_dec` at this point → `v4l2_set_controls(...SPS...)`
fires on rpi-hevc-dec. Phase 0 strace shows rpi-hevc-dec doesn't need
this AND uses a different submission ordering (S_FMT_OUTPUT → REQBUFS_OUTPUT → S_FMT_CAPTURE → CREATE_BUFS_CAPTURE → STREAMON, then global
ctrls per-frame).

The pre-seed is wrapped in `(void)v4l2_set_controls(...)` — failure is
silently ignored, BUT the call may also succeed in an unintended way
on rpi-hevc-dec (it has the HEVC_SPS ctrl), potentially leaving its
internal state stuck on the dummy SPS until the first real per-frame
SPS arrives.

**Amendment:** Phase 6 step 2 sub-action — gate the synthetic-SPS
injection block at context.c:277 with
`if (driver_data->video_fd != driver_data->video_fd_rpi_hevc_dec)`. The
pre-seed only fires when active fd is NOT rpi-hevc-dec. rkvdec /
hantro paths unchanged.

### F7 (No findings) — `image.c` gate predicate (focus area 3)

Verified: rpi-hevc-dec only exposes NC12/NC30 on CAPTURE per Phase 0
`--list-formats-ext`. No legitimate NV12-linear case exists on Pi. Gate
predicate `image->format.fourcc == VA_FOURCC_NV12 && video_format->v4l2_format == V4L2_PIX_FMT_NV12_COL128` is sound — fires only when
both conditions hold, excludes legitimate NV12-linear on RK / Allwinner.

### F8 (No findings) — cross-device regression scope (focus area 4)

Verified: new fd fields initialise to -1; probe loop extensions are
additive (no-op when string doesn't match); `request_device_kind_for_profile`'s 'p' branch only fires when `video_fd_rpi_hevc_dec >= 0`;
new video.c entry is additive. fresnel + ampere paths unchanged.

## Final amended Phase 6 step list

1. `src/request.h` — add `video_fd_rpi_hevc_dec`, `media_fd_rpi_hevc_dec`,
   `has_hevc_ext_sps_rps_rpi_hevc_dec` (mirror iter38 + iter2 layout).
2. `src/request.c` — (a) extend init -1 block; (b) **add `else if
   (strcmp(info.driver, "rpi-hevc-dec") == 0)` branch in primary-driver
   detection** [F3]; (c) extend `request_device_kind_for_profile` so
   HEVC→'p' when rpi present, else 'r'; (d) extend `request_switch_device_for_profile` 'p' branch; (e) probe ext_sps on new fd.
3. `src/context.c` — **gate synthetic-SPS pre-seed (lines 277-346) on
   `driver_data->video_fd != driver_data->video_fd_rpi_hevc_dec`** [F6].
4. `src/video.c` — NC12 entry with `v4l2_buffers_count=1`,
   `v4l2_mplane=true`, NOT marked linear.
5. `src/image.c`:
   - **Extend `#ifdef __arm__` guards (lines 239, 268) to `#if defined(__arm__) || defined(__aarch64__)`** [F1].
   - **Add NC12 detection in RequestCreateImage** (line 107 area): if
     source format is NC12 + VAImage format is NV12, override
     `destination_bytesperlines[0] = width` [F2].
   - **Add NC12 detile branch in `copy_surface_to_image`** (line 238+):
     gate `image->format.fourcc == VA_FOURCC_NV12 && video_format->v4l2_format == V4L2_PIX_FMT_NV12_COL128`; call new detile primitive.
6. `src/nv12_col128.c` + `.h` (NEW) — detile primitive.
7. `tests/test_nv12_col128_detile.c` (NEW) — unit test with hand-crafted
   NC12 bytes + known linear output.
8. `src/meson.build` + `src/Makefile.am` — source list updates.
9. Build clean on higgs; if `tests/` doesn't auto-run, run manually.

## Final amended Phase 7 verification

- Build cleanly on higgs.
- Local install `.so` to `/usr/lib/aarch64-linux-gnu/dri/`.
- `LIBVA_DRIVER_NAME=v4l2_request vainfo` lists `VAProfileHEVCMain`.
- Phase 3 fixtures (640 / 1280 / 1920) + new 1366×768 fixture: libva
  output SHA == kdirect SHA [F4].
- `lsof` during libva decode shows `/dev/video19` open.
- `strace -e ioctl` shows pre-seed pattern ABSENT on rpi-hevc-dec [F6
  verification].
- HEVC_START_CODE behavior verified empirically: if libva-vs-kdirect
  fails for HEVC, add per-driver `_NONE` gate per F5 amendment.
- Sibling regression: re-run fresnel iter38 5/5 test rig — no change
  expected since iter40 path is gated on new fd.

Total amended LoC estimate: ~280 backend + 100 primitive (was 250 + 100;
F1 + F2 + F6 add ~30 LoC of gates / overrides).
