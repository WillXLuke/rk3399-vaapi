# Phase 1+2+3+4 — Pi 5 HEVC chapter (iter40)

Per [[feedback_dev_process]], Phase 1 (goal), Phase 2 (situation analysis),
Phase 3 (baselines), Phase 4 (plan) for adding rpi-hevc-dec as a third
multi-device-probe slot in `libva-v4l2-request-fourier`. Phase 0 substrate
+ open-question answers live at `phase0_pi5_hevc.md`.

## Phase 1 — Goal

> **libva-v4l2-request-fourier on higgs** decodes HEVC Main 8-bit input
> producing NV12 output **bit-exact vs kdirect** for three reference
> fixtures (640×360, 1280×720, 1920×1080 — Main profile, libx265
> ultrafast). HW path engagement verified via the kernel-driver lsof
> signal (`/dev/video19` open) AND ffmpeg-vaapi engagement signal
> (`Hwaccel V4L2 HEVC stateless V4; devices: /dev/media1,/dev/video19`).

Measurable:

| Criterion | Metric |
|---|---|
| C1 — vainfo enumeration | `LIBVA_DRIVER_NAME=v4l2_request vainfo` lists `VAProfileHEVCMain : VAEntrypointVLD` |
| C2 — bit-exact decode | sha256 of libva NV12 output == sha256 of kdirect NV12 output, per fixture, N=1 |
| C3 — HW engagement | `lsof` shows `/dev/video19` open by ffmpeg-vaapi during libva run |
| C4 — Stability under N=3 | C2 holds at N=3 repeated runs (deterministic) |
| C5 — Sibling baseline preserved | fresnel iter38 5/5 still PASS post-iter40 (no regression to rkvdec/hantro path) |

Out of scope this iter: Main10 (10-bit / NC30), VP9, AV1, Firefox VA-API
engagement testing, performance benchmarks. All later chapters.

## Phase 2 — Situation Analysis

### Backend architecture already in place

- **Multi-device probe (iter38)**: at `VA_DRIVER_INIT` opens both
  `rkvdec` + `hantro-vpu` via `find_decoder_device_by_driver(name)`.
  Stores per-driver fds (`video_fd_{rkvdec,hantro}`,
  `media_fd_{rkvdec,hantro}`). `RequestCreateConfig` retargets the
  "active" `driver_data->{video,media}_fd` per profile via
  `request_switch_device_for_profile()` (request.c:426-478).
- **Per-driver feature gating**: `request_data->has_hevc_ext_sps_rps_{rkvdec,hantro}`
  pair, with `h265_set_controls` consulting the per-fd flag. Established
  by iter2 / Phase 5 review (request.h:99-100). This is the canonical
  per-driver gating shape for iter40.
- **HEVC ctrl population**: `h265_set_controls` populates the standard
  `V4L2_CID_STATELESS_HEVC_*` set (h265.c). Probe-gates EXT_SPS_*_RPS
  via the iter2 path — naturally dormant for rpi-hevc-dec since the
  controls don't exist.
- **Synthetic SPS pre-seed (iter25/26)**: needed for rkvdec to resolve
  `image_fmt` before CAPTURE alloc. Phase 0 strace shows rpi-hevc-dec
  does NOT need this — it accepts NC12 + explicit dims on `S_FMT
  CAPTURE` directly. The pre-seed code path stays in place for rkvdec;
  rpi-hevc-dec just doesn't trigger it (gate on driver_kind).
- **CAPTURE detile primitive**: `nv15_unpack_plane_to_p010()` (nv15.c)
  is the template — backend already CPU-detiles when a Pi-or-Rockchip-
  specific CAPTURE format meets a linear consumer (VAImage NV12 / P010).
- **Single-plane (S) vs multi-plane (M) handling**: hantro uses MPLANE,
  rkvdec uses both depending on codec. rpi-hevc-dec exposes MPLANE for
  BOTH OUTPUT (HEVC_SLICE) and CAPTURE (NC12) per the strace. iter38
  already supports MPLANE handling for hantro; rpi reuses that.

### Surface area to touch (audit)

| File | What changes | Size |
|------|--------------|------|
| `src/request.h` | Add `video_fd_rpi_hevc_dec`, `media_fd_rpi_hevc_dec`, `has_hevc_ext_sps_rps_rpi_hevc_dec` (mirror iter38 + iter2 layout) | ~10 lines |
| `src/request.c` | (a) Extend init -1 block to cover new fds. (b) Recognize `rpi-hevc-dec` as a 3rd primary/alt driver string in the probe loop. (c) Extend `request_device_kind_for_profile` so HEVC→'p' when rpi-hevc-dec is present, else 'r'. (d) Extend `request_switch_device_for_profile` 'p' branch. (e) Probe HEVC ext_sps on the new fd (will be false, mirrors hantro entry). | ~80 lines |
| `src/video.c` | Add `V4L2_PIX_FMT_NV12_COL128` (NC12) `video_format` entry: 4:2:0, planes=1, alignment via dedicated bytesperline/sizeimage formula. NOT marked linear. | ~20 lines |
| `src/nv12_col128.c` (NEW) | `nv12_col128_detile_to_nv12()`: Y plane + UV plane detile primitive. Adapted from ffmpeg/Kynesim `av_rpi_sand_to_planar_y8` core math. Header doc traces back to videodev2.h docstring + raspberrypi/linux `hevc_dec/hevc_d_video.c` size formula. | ~80 lines + 30-line header |
| `src/image.c` | Add NC12 → NV12 branch in `copy_surface_to_image`, gated on `image->format.fourcc == VA_FOURCC_NV12 && video_format->v4l2_format == V4L2_PIX_FMT_NV12_COL128` (sibling to existing NV15→P010 branch). | ~25 lines |
| `src/meson.build` + `src/Makefile.am` | List `nv12_col128.c`/`.h` in sources | 2 lines |

Total estimated diff: ~250 LoC backend + ~100 LoC standalone primitive.
Roughly half the surface area of iter38; smaller than iter2.

### What does NOT change

- iter25/26 SPS pre-seed: stays on rkvdec path only (gated by
  driver_kind check that's already implicit in the rkvdec fd routing).
- iter2 EXT_SPS plumbing: probe-gated off on rpi-hevc-dec; vendored
  GStreamer parser unused. Confirmed via the EINVAL on ctrl 0xa97.
- iter31 α-29 slice_params st_rps_bits: APPLIES to rpi-hevc-dec
  unchanged. Same plumbing.
- iter33 VP8 hantro start-code prepend: not relevant (rpi-hevc-dec is
  HEVC-only; VP8 still goes through hantro on RK).
- iter38 single-libva-session multi-codec semantics: extends from 5
  codecs to 5+1 (HEVC reroutes on Pi).

### NC12 / SAND128 tile geometry — locked contract

From kernel driver `drivers/media/platform/raspberrypi/hevc_dec/hevc_d_video.c`
(via [[github raspberrypi/linux rpi-6.12.y]]):

```c
case V4L2_PIX_FMT_NV12_COL128:
    width = ALIGN(width, 128);           /* Width rounds up to columns */
    height = ALIGN(height, 8);
    bytesperline = constrain2x(bytesperline, height * 3 / 2);
    sizeimage = bytesperline * width;
    break;
```

For 1280×720:
- width = 1280 (already 128-aligned)
- height = 720 (already 8-aligned)
- bytesperline = 720 × 3/2 = **1080** (matches Phase 0 strace observation)
- sizeimage = 1080 × 1280 = **1,382,400** (matches strace; equals linear NV12 byte count coincidentally)

**Geometry interpretation** (cross-verified against ffmpeg/Kynesim
`rpi_sand_fn_pw.h` `av_rpi_sand_to_planar_y8`):
- Image is divided into `(width + 127) / 128` columns; each column is
  **128 px wide × height px tall**.
- Within a column: `128 × height` bytes of Y data, immediately followed
  by `128 × height/2` bytes of interleaved CbCr (so 128 × `bytesperline`
  bytes per column, where `bytesperline` is the column stride).
- Across columns: column N starts at offset `N × stride1 × stride2`
  where `stride1 = 128` (column width) and `stride2 = bytesperline`.
- **Pixel (x, y) byte offset = `(x & 127) + y × 128 + (x & ~127) × bytesperline`**
  for Y; same formula with `y/2` for UV plane (which begins at offset
  `128 × height × num_columns` from the start).

Reference for the detile loop: `av_rpi_sand_to_planar_y8` (Kynesim
ffmpeg, `libavutil/rpi_sand_fn_pw.h` with PW=1). Our primitive copies
the single-stripe fast-path math; we don't import NEON ASM (CPU
detile is the safe path for Phase 1; SIMD a Phase 2 perf bump if needed).

## Phase 3 — Baselines

### Test fixtures (generated on higgs)

| Fixture | Size | Profile | Generator |
|---------|------|---------|-----------|
| `bbb_640_main.mp4`  | 640×360   | Main 8-bit | `ffmpeg -f lavfi -i testsrc=duration=2 -pix_fmt yuv420p -c:v libx265 -preset ultrafast -profile:v main` |
| `bbb_1280_main.mp4` | 1280×720  | Main 8-bit | same |
| `bbb_1920_main.mp4` | 1920×1080 | Main 8-bit | same |

### Captured 2026-05-17 evening on higgs

For each fixture, N=3 reps. Both SW (no hwaccel) and kdirect
(`ffmpeg -hwaccel drm -c:v hevc`) → `-frames:v 10 -f rawvideo -pix_fmt nv12`,
sha256 of first 16 chars:

```
bbb_640_main  SW={9a81038065e9b7cd} HW={9a81038065e9b7cd}  → BIT-EXACT × N=3
bbb_1280_main SW={d3bb055655d6f195} HW={d3bb055655d6f195}  → BIT-EXACT × N=3
bbb_1920_main SW={0bc2bd6f693db039} HW={0bc2bd6f693db039}  → BIT-EXACT × N=3
```

HW engagement signal (per-run): `Hwaccel V4L2 HEVC stateless V4; devices: /dev/media1,/dev/video19; buffers: src DMABuf, dst DMABuf; swfmt=rpi4_8`

This is the kdirect baseline. Phase 7 verification will compare libva
output against these SHAs.

### Strace-derived submission ordering (Phase 0 close addendum)

Captured in `phase0_pi5_hevc.md`. Briefly: standard V4L2-request
stateless flow, both queues DMABUF, no SPS pre-seed dance needed
(rpi-hevc-dec accepts NC12 + dims directly on CAPTURE S_FMT).

## Phase 4 — Plan

### Implementation steps (sequenced)

1. **`request.h`**: extend `request_data` with the new fd pair + ext_sps
   flag, mirroring iter38/iter2 layout. (no behavior change yet)
2. **`request.c`**:
   - `find_decoder_device_by_driver("rpi-hevc-dec", ...)` accepts new
     driver string.
   - Init -1 block extends to new fds.
   - Probe loop: if primary is `rkvdec` or `hantro-vpu`, also probe
     `rpi-hevc-dec` (third slot). On Pi 5 there's no `rkvdec` or
     `hantro-vpu`, so primary becomes `rpi-hevc-dec` and the alt-probes
     for the other two return absent (their fds stay -1).
   - `request_device_kind_for_profile`: when profile is `VAProfileHEVCMain`,
     prefer `'p'` (rpi-hevc-dec) IF `video_fd_rpi_hevc_dec >= 0`, else
     fall through to `'r'` (rkvdec). All other profiles stay routed as
     today.
   - `request_switch_device_for_profile`: add `'p'` branch.
   - ext_sps probe runs on the new fd; result stored in
     `has_hevc_ext_sps_rps_rpi_hevc_dec`. Will be false (controls absent).
3. **`video.c`**: add NC12 video_format entry. Mark it MPLANE-only (per
   Phase 0 strace). bytesperline/sizeimage formula encoded per kernel
   driver math.
4. **`src/nv12_col128.c` + `.h`** (NEW): single-file primitive,
   `nv12_col128_detile_to_nv12(dst_y, dst_uv, src_y, src_uv, width,
   height, src_stride2)`. CPU per-column row-memcpy loop; not NEON
   for Phase 1 (correctness first). Self-test in `tests/test_nv12_col128_detile.c`.
5. **`image.c`**: branch in `copy_surface_to_image`. Gate:
   `image->format.fourcc == VA_FOURCC_NV12 && video_format->v4l2_format == V4L2_PIX_FMT_NV12_COL128`.
   Calls the primitive. Existing NV12-linear path stays.
6. **`meson.build` + `Makefile.am`**: source list updates.
7. **Build clean on higgs** — first build target IS higgs (since iter40
   only matters on Pi). Cross-build for ampere/fresnel is unaffected
   because they don't have rpi-hevc-dec — the new fd stays -1 and the
   per-driver routing falls through to existing rkvdec/hantro paths.

### Verification gates (Phase 7 acceptance)

- Build cleanly on higgs (Debian 13 trixie, libva-dev 2.22.0-3,
  libdrm-dev 2.4.131).
- Local-install the resulting `.so` to `/usr/lib/aarch64-linux-gnu/dri/v4l2_request_drv_video.so`.
- `LIBVA_DRIVER_NAME=v4l2_request vainfo` lists `VAProfileHEVCMain`.
- For each Phase 3 fixture: libva output SHA == kdirect SHA (the Phase 3
  recorded value).
- `lsof` during libva decode shows `/dev/video19` open.
- Sibling regression check: fresnel `phase7_iter39_test_rig` equivalent
  still 5/5 PASS (no regression to existing routing).

### Risks + mitigations

| Risk | Mitigation |
|------|-----------|
| NC12 detile math wrong → libva ≠ kdirect | Tight unit test in `tests/test_nv12_col128_detile.c` with hand-crafted NC12 bytes + known linear output, before integration. |
| `request_switch_device_for_profile` falls through wrong path on systems with BOTH rkvdec AND rpi-hevc-dec | Prefer rpi-hevc-dec for HEVC when present. Explicit comment in switch. Test on fresnel = no rpi → falls to 'r'; test on higgs = no rkvdec → falls to 'p'. |
| Debian build env differs from Arch — see [[feedback_package_build_flags_unmask_bugs]] | Build with explicit `-O2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong` flags to match Debian dpkg-buildflags. |
| Synthetic SPS pre-seed accidentally fires on rpi-hevc-dec | Gate on `driver_kind != 'p'` in the pre-seed call site. Verify via strace: pre-seed ioctl pattern absent. |
| iter2 EXT_SPS path accidentally engages on rpi | Already probe-gated; `has_hevc_ext_sps_rps_rpi_hevc_dec` = false naturally. |

### Phase 5 review explicitly requested

Per CLAUDE.md global "Reviews are never skippable" + [[feedback_review_empirical_over_theoretical]]:
this plan goes to a sonnet Plan-agent review. Specific review focus:
- Routing correctness when 0/1/2/3 of the three drivers are present.
- NC12 geometry: did we copy ffmpeg's per-row memcpy math correctly?
  Did we miss UV stride considerations?
- `image.c` gate predicate — does it exclude any legitimate NV12-linear
  case on Pi? (No: rpi only exposes NC12/NC30 CAPTURE, no plain NV12.)
- Cross-device regression scope (fresnel + ampere paths untouched?).

Empty-result review IS a green light; "we should have skipped it" is the
prohibited move.
