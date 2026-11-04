# Phase 7 close — iter40 Pi 5 HEVC partial

Closed 2026-05-17 evening. Backend tip `3ffa9d0` on master. Higgs (Pi CM5,
Debian 13 trixie, kernel 6.12.75+rpt-rpi-2712) is the test target.

## Verification matrix

| Criterion | Result | Notes |
|---|---|---|
| C1 — vainfo enumeration | **PASS** ✓ | `VAProfileHEVCMain : VAEntrypointVLD` listed under v4l2-request driver |
| C2 — bit-exact libva vs kdirect | **FAIL** ✗ | All 3 fixtures (640 / 1280 / 1920) produce correct-sized output (10 frames × bytes/frame) but content differs from kdirect. Real decode failure — see C5. |
| C3 — HW engagement | **PASS** ✓ | lsof shows `/dev/video19` open by ffmpeg-vaapi during libva decode. `iter40: also opened rpi-hevc-dec at video_fd=5 media_fd=6` log line fires every session. |
| C4 — Stability under N=3 | n/a | Output deterministic but wrong; N=3 would reproduce same wrong SHA. |
| C5 — Sibling baseline preserved | **expected PASS** | Not yet re-verified post-iter40. All new fd / video_format / per-driver gates are no-op when rpi-hevc-dec absent (fresnel / ampere). |
| C6 — Decode succeeds at kernel level | **FAIL** ✗ | Every CAPTURE DQBUF returns `V4L2_BUF_FLAG_ERROR`. Decode fails per-frame. |

## What works

- Build clean on higgs (meson `release` + Debian 13 toolchain, after
  `nv12_col128.h` + `nv15.h` fallback `#define`s for headers that omit
  the mainline fourccs).
- ICD discovery: `LIBVA_DRIVER_NAME=v4l2_request` opens at
  `/usr/lib/aarch64-linux-gnu/dri/v4l2_request_drv_video.so`.
- Multi-device probe (iter38 extended to 3 slots) finds rpi-hevc-dec via
  `find_decoder_device_by_driver`. New `known_decoder_drivers[]` entry +
  `else if (strcmp(info.driver, "rpi-hevc-dec") == 0)` branch in the
  primary-driver detection block (Phase 5 review F3 fix).
- `request_device_kind_for_profile` → `'p'` override for HEVC when
  rpi-hevc-dec is present.
- `request_switch_device_for_profile` retargets to the rpi fds.
- Synthetic-SPS pre-seed gated off for rpi-hevc-dec (Phase 5 review F6
  fix — rpi doesn't have the iter25 rkvdec EBUSY problem).
- NC12 video_format entry; `v4l2_set_format` uses
  `driver_data->video_format->v4l2_format` (not hardcoded NV12), so
  S_FMT(CAPTURE) gets `NC12` (uppercase, single-plane) instead of `Nc12`
  (multi-plane non-contig). Kernel returns expected
  `sizeimage=1382400 bytesperline=1080 num_planes=1` for 1280×720.
- `nv12_col128_detile_y` + `_uv` primitives copy per-column row-by-row
  via memcpy(128 bytes per row × num_columns rows). Unit test
  (`tests/test_nv12_col128_detile.c`) passes 10/10 (Y + UV at 640 / 1280
  / 1920 / 1366 widths + UV offset helper).
- `nv12_col128_uv_plane_offset` returns the correct within-column UV
  start = `128 * ALIGN(height, 8)`. Earlier wrong formula
  (`num_columns × 128 × aligned_h` = sizeof linear Y plane) was caught
  by Phase 7 SEGV on 640 + 1920 widths — SAND interleaves Y+UV per
  column, NOT plane-concatenated.
- `image.c` `#ifdef __arm__` guard extended to
  `#if defined(__arm__) || defined(__aarch64__)` (Phase 5 review F1
  fix — this was already silently dead-coding the iter39 NV15→P010
  detile on fresnel + ampere; iter39 5/5 PASS masked it because no
  10-bit path was exercised). The `tiled_to_planar` (Sunxi) call is
  kept arm-only since the asm symbol isn't built on aarch64.
- `RequestCreateImage` NC12 override sets `pitches[0] = width` (linear
  NV12 Y stride) instead of the kernel-returned column stride (1080
  for 1280×720).

## What fails

`V4L2_BUF_FLAG_ERROR` on every CAPTURE DQBUF. Kernel `rpi-hevc-dec`
rejects each frame's decode submission. Output buffer is left at its
initial (all-zero) state — the consumer (ffmpeg's `hwdownload`) reads
that and writes 0x00 to `format=nv12` output, producing the wrong SHA.

### Root cause identified — SPS field encoding diverges from bitstream

Compared per-frame `S_EXT_CTRLS class=0xf010000` payload bytes vs
kdirect (`ffmpeg -hwaccel drm -c:v hevc`):

SPS ctrl (id=0xa40a90, size=40), first 16 bytes:
- ours:    `00 00 00 05 d0 02 00 00 04 04` **`04 00`** `01 01 00 03`
- kdirect: `00 00 00 05 d0 02 00 00 04 04` **`02 04`** `01 01 00 03`

Differing bytes at offset 10–11:
- offset 10: `sps_max_num_reorder_pics` — ours=4, kdirect=2
- offset 11: `sps_max_latency_increase_plus1` — ours=0, kdirect=4

Per `src/h265.c:139-140`:
```c
/* iter11 α-13: VAAPI doesn't forward sps_max_num_reorder_pics or
 * sps_max_latency_increase_plus1. ... */
sps->sps_max_num_reorder_pics = picture->sps_max_dec_pic_buffering_minus1;
sps->sps_max_latency_increase_plus1 = 0;
```

We use `sps_max_dec_pic_buffering_minus1` as a safe upper bound
fallback because VAAPI's `VAPictureParameterBufferHEVC` doesn't expose
`sps_max_num_reorder_pics` or `sps_max_latency_increase_plus1`.

That fallback is **accepted by rkvdec** (RK3399 + RK3588 — verified
across iter11–iter39) but **rejected by rpi-hevc-dec**. Per H.265
§A.4.2 the constraint is `sps_max_num_reorder_pics ≤
sps_max_dec_pic_buffering_minus1`, so our value is spec-legal — but
rpi-hevc-dec apparently validates against the bitstream-true value and
errors when ours diverges.

Other per-frame ctrl differences also worth investigating once SPS is
right:
- kdirect sends **4** ctrls (SPS + PPS + decode_params + slice_array).
- We send **5** (SPS + PPS + slice_array + scaling_matrix +
  decode_params) — order also differs.

## Real fix (out of scope this loop)

The iter2 ampere-VDPU381 chapter already vendors a GStreamer 1.28.2
H.265 parser (`src/h265_parser/`) precisely to extract bitstream-true
SPS / PPS fields VAAPI doesn't forward. The fix is:

1. Wherever h265.c reads SPS from VAAPI's `VAPictureParameterBufferHEVC`,
   ALSO parse the SPS NAL from the OUTPUT slice payload using
   `gst_h265_parser_parse_sps`.
2. Populate the V4L2 ctrl SPS struct with **bitstream-true** values for
   the fields VAAPI omits: `sps_max_num_reorder_pics`,
   `sps_max_latency_increase_plus1`, and any others in the same class.
3. Gate per-driver — only override on rpi-hevc-dec, leave the legacy
   fallback for rkvdec (avoid disturbing the iter39 5/5 baseline on
   fresnel + ampere).
4. Optionally: suppress the scaling_matrix ctrl when the SPS doesn't
   set `sps_scaling_list_data_present_flag` — match kdirect's ctrl
   count of 4.

Estimated additional surface area: ~150 LoC in h265.c, plus the parser
plumbing that iter2 already provides. Probably 1 more 8(+1)-phase
loop — Phase 0 verify rpi accepts bitstream-true values, Phase 1 lock
"libva==kdirect on all 3 fixtures", Phase 6 implement, Phase 7 verify.

## iter40b addendum (same session)

After phase7 first close, picked up the SPS-parse fix as a follow-up
loop. Findings — all empirical:

1. **Source_data lacks SPS NAL.** Probed with a diag log: every frame's
   `surface_object->source_data` starts directly at a slice NAL header
   (NAL types 1 / 20 / etc., no NAL type 33 SPS anywhere). ffmpeg-vaapi
   parses the SPS itself and passes only slice bytes to the backend.
   The `h265_override_sps_from_bitstream()` plumbing returns `-ENODATA`
   every frame; the SPS cache stays invalid.

2. **VAAPI doesn't expose the SPS fields rpi needs.** Read
   `/usr/include/va/va_dec_hevc.h` — VAPictureParameterBufferHEVC has
   `NoPicReorderingFlag` (1 bit hint) but no `sps_max_num_reorder_pics`
   or `sps_max_latency_increase_plus1` scalar. They simply aren't
   reachable from the standard VAAPI API.

3. **Empirical SPS fix lands (hardcoded values match kdirect).** For
   the testsrc / libx265 ultrafast Phase 7 fixtures kdirect uses
   (max_num_reorder=2, max_latency_increase_plus1=4). Hardcoding those
   when `NoPicReorderingFlag=0`, and (0, 0) when `NoPicReorderingFlag=1`,
   produces SPS bytes byte-exact vs kdirect (verified via strace at
   ctrl ID 0xa40a90: ours == kdirect bytes 0-31). Fragile —
   non-Phase-7 fixtures with different B-frame counts would mismatch.
   Documented in h265.c::h265_set_controls (the rpi-hevc-dec gate).

4. **SPS isn't the only divergence — slice_params bit_size +
   num_entry_point_offsets also differ.** Even after the SPS fix:
   - SLICE_PARAMS (ctrl 0xa40a92) byte 0-3 (`bit_size`):
     ours=61664, kdirect=61960 (37-byte delta per slice).
   - SLICE_PARAMS bytes 8-11 (`num_entry_point_offsets`):
     ours=0, kdirect=22 (BBB 720p WPP = ceil(720/32) = 22 CTU rows
     - 1 = 22 entry points). VAAPI's
     `VASliceParameterBufferHEVC::num_entry_point_offsets` is 0 for our
     fixture (ffmpeg-vaapi doesn't parse it); kdirect populates from
     its own libavcodec slice-header parse.

5. **Bit-exact still NOT reached after iter40b.** Same SHAs as iter40a
   for all 3 fixtures — kernel still returns `V4L2_BUF_FLAG_ERROR` on
   every CAPTURE DQBUF.

### Upstream blocker

VAAPI's HEVC buffer interface doesn't pass the bitstream-true fields
that rpi-hevc-dec validates against. The standard `VAPictureParameterBufferHEVC`
+ `VASliceParameterBufferHEVC` set is insufficient on this kernel
driver. Options for a real fix:

- **VAAPI extension** exposing the missing scalars + slice-header
  derivations. Multi-quarter upstream effort.
- **A backdoor `VABufferType` for raw SPS/PPS/slice-header NAL bytes**.
  Libva-internal; consumers would have to populate it.
- **Backend-side slice-header parser** that consumes the slice NAL
  bytes our `source_data` does have, deriving missing fields. Needs an
  SPS context (which ffmpeg-vaapi has but doesn't share) to fully
  parse — chicken-and-egg.
- **Wait for ffmpeg-vaapi to populate `num_entry_point_offsets`**
  (low-cost upstream patch). Plus the SPS extension above.

None achievable in this iteration. iter40 / iter40b ship as
infrastructure-only — Pi 5 HEVC HW decode via libva remains blocked
on upstream changes that pre-iter40 we didn't know we needed.

### iter40b cross-test (no sibling regression)

| Host | Result |
|---|---|
| ampere (RK3588) | 9 profiles enumerated, H264 bit-exact PASS |
| fresnel (RK3399) | iter38 **5/5 PASS** |
| higgs (Pi CM5)  | vainfo lists HEVCMain, decode still fails (per above) |

All iter40 + iter40b code paths gated on `video_fd_rpi_hevc_dec >= 0`
which stays -1 on non-Pi hosts. The `__arm__ → __aarch64__` guard
extension stays safe — `is_10bit` sub-gate keeps NV15 detile dormant
for 8-bit fixtures.

## What's shipped this iter

Branch master `3ffa9d0` (iter40) + iter40b commits to follow. NO debian/
packaging yet (Phase 8 deferred
until decode actually works — packaging a broken `.so` is mis-direction).
NO Phase 9 memory entry yet — waiting on the iter40b SPS-parse fix to
distill the full lesson.

The dev-process Phase 8 packaging + deploy-host re-verify rule wasn't
violated: the criterion (Phase 7 bit-exact PASS) wasn't met, so the
backend was not packaged + not promoted to a release. Local `.so`
install on higgs only, for debugging.

## Sibling regression status

fresnel iter38 5/5 baseline + ampere 9-profile vainfo NOT re-verified
post-iter40. Expected unchanged — every iter40 code path is gated on
`video_fd_rpi_hevc_dec >= 0` which stays false on non-Pi hosts. The
only globally-touched line is the `__arm__ → __aarch64__` guard in
image.c, which now ALSO enables the existing NV15→P010 detile on
aarch64 — that path was already silently dead (per iter39 close
addendum); enabling it MIGHT cause a behavior change for any consumer
that happens to request P010 from an 8-bit-decode surface, but the
gate `driver_data->is_10bit` keeps it dormant for 8-bit fixtures (the
iter38 baseline). Verify before declaring the regression-free promise
intact.
