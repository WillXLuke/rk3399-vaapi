# Phase 0 — Pi 5 / CM5 HEVC chapter

Opened 2026-05-17 evening, after the failed `libva-v4l2-stateful-fourier`
scaffold attempt. Brother-session empirical Phase 0 on higgs invalidated
the stateful premise: rpi-hevc-dec is V4L2 **stateless**, so Pi 5 HEVC
belongs in this backend, not a separate sibling.

No code in this chapter yet. This doc is the substrate. Phase 1 picks up
from the "Open questions" section.

## Substrate

### Target host

higgs — Pi CM5 module on Pi CM5 IO board. BCM2712 SoC. VPN-only, often
offline; wake via HIS skill recipe (no Fritz!Box plug — runs on power
when on). Debian-based. Sole HW video decoder is rpi-hevc-dec at
`/dev/video19` + `/dev/media1`.

### Backend baseline at chapter open

`libva-v4l2-request-fourier` master tip `cf8cd9d` (iter39 + Option B +
h265 ref-list cap fix). Multi-device probe (iter38) already opens
rkvdec + hantro slots; adding a third decoder slot for rpi-hevc-dec is
a natural extension of that architecture.

iter2 (ampere VDPU381 HEVC EXT_SPS) added the GStreamer 1.28.2 H.265
parser vendor + EXT_SPS_ST_RPS / _LT_RPS dynamic-array submission. That
plumbing is probe-gated (`has_hevc_ext_sps_rps_rkvdec`), so it stays
dormant on hosts where the controls don't exist.

### Empirical higgs probe (brother session)

`v4l2-ctl -d /dev/video19 --list-formats-ext --list-ctrls`:

```
Stateless Codec Controls

  hevc_sequence_parameter_set        (compound, V4L2_CID_STATELESS_HEVC_SPS)
  hevc_picture_parameter_set         (compound, V4L2_CID_STATELESS_HEVC_PPS)
  slice_param_array                  (compound dynamic-array dims=[4096])
  hevc_scaling_matrix                (compound)
  hevc_decode_parameters             (compound)
  hevc_decode_mode                   (menu, "Frame-Based")
  hevc_start_code                    (menu, default "No Start Code")

OUTPUT formats:
  S265  V4L2_PIX_FMT_HEVC_SLICE  (parsed slice payload)

CAPTURE formats:
  NC12  V4L2_PIX_FMT_NV12_COL128       (8-bit  SAND 128-column tiled)
  NC30  V4L2_PIX_FMT_NV12_10_COL128    (10-bit SAND 128-column tiled)
```

Conclusion: this is the standard `V4L2_CID_STATELESS_HEVC_*` control set
exposed under the V4L2-request uAPI, exactly the same family our backend
already drives for rkvdec/hantro/cedrus HEVC paths. The novel parts are
two pixel formats (NC12, NC30) and one driver-id (rpi-hevc-dec).

## What carries forward unchanged

- VAAPI HEVC profile enumeration (`config.c`)
- `h265_set_controls` core path (`h265.c`) — same compound ctrl set
- Synthetic SPS pre-seed pattern (iter25/26) — already runs pre-CAPTURE-alloc
- Multi-device dispatch in `RequestCreateConfig` (iter38)
- VAAPI slice / picture / IQ matrix buffer parsing
- HEVC h264-style start-code policy (we already DON'T prepend for HEVC)

## What needs adding

| Item | Location | Sizing |
|------|----------|--------|
| `RPI_HEVC_DEC` enum in `driver_kind_t` | `request.h` | trivial |
| Multi-device probe extends to `/dev/video19` discovery | `context.c` / `request.c` init | small — mirror hantro slot |
| `V4L2_PIX_FMT_NV12_COL128` (NC12) `video_format` entry | `video.c` | small |
| `V4L2_PIX_FMT_NV12_10_COL128` (NC30) `video_format` entry | `video.c` | small |
| NC12 → NV12 detile primitive | new `nv12_col128.c` | mid — column tile layout, see kernel docs |
| NC30 → P010 detile primitive | new `nv12_col128.c` | mid — 10-bit variant of above |
| `copy_surface_to_image` branch for NC12/NC30 | `image.c` | small (mirror NV15→P010 gating) |
| Per-driver gating for any rpi-specific quirks discovered | various | per [[per-driver-kludge-gating]] |

## Open questions for Phase 1

Lock these before Phase 1 commits to a goal.

1. **EXT_SPS controls on rpi-hevc-dec?** Brother's `--list-ctrls` output
   above shows the standard `V4L2_CID_STATELESS_HEVC_*` family — NOT the
   `EXT_SPS_ST_RPS` / `EXT_SPS_LT_RPS` extensions that VDPU381 needs.
   Verify: does `slice_param_array[4096]` accept `st_rps_bits` /
   `lt_rps_bits` in the per-slice payload, or does rpi-hevc-dec parse RPS
   itself from the slice header? If the latter, the iter2 EXT_SPS path
   stays dormant (probe-gated already), and rpi-hevc-dec just needs the
   `picture->st_rps_bits` → `slice_params->short_term_ref_pic_set_size`
   plumbing that iter31 α-29 already wired. Expectation: works out of the
   box. Confirm before assuming.

2. **`hevc_start_code` ctrl: "No Start Code" vs Annex B?** Brother saw
   default `"No Start Code"` — matches our behavior (we don't prepend on
   HEVC). But the ctrl is configurable. Verify the menu values exposed
   and confirm "No Start Code" passes our raw slice-NAL payload as-is.
   If it doesn't, set the ctrl explicitly per [[unconditional-codec-state]]
   gating.

3. **NC12 / NC30 SAND tile layout — exact spec.** Read
   `Documentation/userspace-api/media/v4l/pixfmt-yuv-planar.rst` for the
   COL128 variants. Confirm: column stride = 128 bytes (Y) / 128 bytes
   (UV interleaved). Row count = `ALIGN(height, 16)` or `ALIGN(height, 8)`?
   Get the exact alignment and tile-traversal order before writing the
   detile primitive. Cite from kernel doc, NOT inferred from a hex dump.

4. **drm_prime / SAND modifier round-trip.** Does ffmpeg-vaapi (and
   Firefox) accept the NC12 buffer via DRM_PRIME export carrying the
   DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT modifier, allowing
   zero-copy to a SAND-aware compositor? Or is libva-side detile to a
   linear NV12 buffer the only viable Firefox path? If detile is
   required for the consumer, the [[rockchip-pixel-verify-path]] rule
   (DMA-BUF GL preferred over cached mmap) might NOT apply since SAND
   is Pi-specific and not in the wider Wayland modifier ecosystem.

5. **rpi-hevc-dec quirks on first SPS submission.** rkvdec needs
   image_fmt pre-seed before CAPTURE alloc (iter25). Does rpi-hevc-dec
   have an analogous "must set OUTPUT pix_fmt + SPS before CAPTURE"
   ordering? Verify with strace early.

6. **higgs OS + libva versioning.** Brother probed on Debian. We package
   for Arch ALARM. What's the install path on higgs — Arch / Debian /
   Raspberry Pi OS? If Debian, the package needs a `debian/` tree, not
   just PKGBUILD. Decide packaging target before Phase 8.

## Phase 1 goal sketch (NOT locked)

> Firefox HW HEVC playback on higgs at ≥30fps for 1080p Main, byte-exact
> libva-vs-kdirect for ≥3 reference fixtures (8-bit Main and 10-bit Main10).

Two measurable subgoals follow naturally:
- libva (this backend, NV12 image output) == kdirect (ffmpeg-v4l2request,
  NV12 image output) byte-exact for the same input.
- Firefox VA-API path engages (verify via `chrome://gpu` equivalent / log
  inspection — `MOZ_LOG=PlatformDecoderModule:5`).

## Phase 3 baseline plan

Before any backend code touches rpi-hevc-dec:
- `kdirect` floor: `ffmpeg -hwaccel v4l2request -hwaccel_output_format drm_prime
  -i bbb_720p10s_hevc.mp4 -vf hwdownload,format=nv12 -frames:v 10 ...` and
  sha256 the YUV.
- `SW reference`: same ffmpeg without `-hwaccel`, sha256 the YUV.
- Both runs N=3 per [[replicate-baseline-first]].
- Capture `strace -f -e ioctl` of the kdirect run — gives the canonical
  ioctl sequence rpi-hevc-dec expects.

## Phase 0 closing

This doc commits the substrate. Phase 1 starts when:
- higgs is up + reachable
- Open questions 1+2 (EXT_SPS + start_code) are answered live, in one
  short probe session
- Phase 3 baseline floors are captured

No work blocks the close of iter39 / fresnel campaign — those are shipped.

## Phase 0 close addendum (2026-05-17 evening, higgs probe session)

Empirical probes on higgs answered Q1, Q2, partial Q3, full Q5, full Q6.
Q4 (DRM modifier round-trip) remains open. Phase 0 is closed; Phase 1
opens with what's below.

### Q1 — EXT_SPS controls on rpi-hevc-dec: NOT present

`v4l2-ctl -d /dev/video19 --list-ctrls` confirms ONLY the standard
`V4L2_CID_STATELESS_HEVC_*` set:
- `hevc_sequence_parameter_set` (0x00a40a90)
- `hevc_picture_parameter_set` (0x00a40a91)
- `slice_param_array` (0x00a40a92, dynamic-array dims=[4096])
- `hevc_scaling_matrix` (0x00a40a93)
- `hevc_decode_parameters` (0x00a40a94)
- `hevc_decode_mode` (0x00a40a95, menu min=1 max=1 default=1 = Frame-Based)
- `hevc_start_code` (0x00a40a96, menu min=0 max=1 default=0 = No Start Code)
- 0x00a40a97 returns EINVAL (no EXT_SPS_*_RPS controls)

ioctl trace confirms ffmpeg's `VIDIOC_QUERY_EXT_CTRL` for `0xa97` returns
EINVAL — same probe pattern our backend uses for
`has_hevc_ext_sps_rps_rkvdec`. **The iter2 path stays dormant; the
iter31 α-29 `slice_params->short_term_ref_pic_set_size` plumbing is the
correct one for rpi-hevc-dec.**

### Q2 — hevc_start_code: default 0 (No Start Code), values {0, 1}

Default 0 matches our backend's "don't prepend HEVC start code" stance.
Confirm in Phase 1: rpi-hevc-dec accepts our raw NAL slice payload as-is.

### Q3 — NC12 / NC30 SAND tile layout: PARTIAL

CAPTURE S_FMT result for 1280×720 NC12:
- `sizeimage=1382400` = `1280 × 720 × 1.5` (linear NV12 byte count)
- `bytesperline=1080` (NOT 1280)

The bytesperline=1080 for a 1280-wide CAPTURE buffer is suspect — likely
encodes SAND column count rather than linear stride. Read
`drivers/staging/media/rpivid/` (or wherever NC12_COL128 lives in 6.12)
kernel source + `drm_fourcc.h` / `nv12_col128.rst` (if it exists) for
exact tile layout BEFORE writing the detile primitive. Do NOT infer
layout from this single observation.

### Q4 — DRM modifier round-trip: BLOCKED on hwdownload

ffmpeg `-hwaccel drm -hwaccel_output_format drm_prime -vf
hwmap=mode=read,format=nv12` returns `Failed to map frame: -38`
(`Function not implemented`). hwdownload cannot consume the SAND
modifier directly.

ffmpeg's path that DOES work: `-hwaccel drm -c:v hevc` WITHOUT
`-hwaccel_output_format drm_prime` lets ffmpeg's internal pipeline pull
back, detile (presumably via a Pi-specific helper or libdrm transform),
and present NV12 to the next filter. Bit-exact vs SW for the test
fixture (1280×720 Main 8-bit) — confirms HW engagement.

Phase 1 / Phase 4 will need to decide:
- Detile in the backend (CPU SIMD), exposing NV12 via VAImage; or
- Pass-through DRM_PRIME with SAND modifier and let the consumer
  (compositor / Firefox) detile. Firefox almost certainly can't, so
  CPU detile is the safe bet.

### Q5 — rpi-hevc-dec submission ordering: empirically locked

`strace -e ioctl` of the kdirect run shows:
1. `MEDIA_IOC_DEVICE_INFO` + `MEDIA_IOC_G_TOPOLOGY` (per media node)
2. `VIDIOC_QUERYCAP` per video node — `driver="rpi-hevc-dec"` identifies
   the right one
3. `VIDIOC_ENUM_FMT` OUTPUT → S265 only
4. `VIDIOC_S_FMT` OUTPUT (HEVC_SLICE, placeholder dims)
5. `VIDIOC_REQBUFS` OUTPUT (DMABUF, count=N) — count=6 in kdirect
6. `VIDIOC_S_FMT` CAPTURE (NC12, actual dims from SPS parse)
7. `VIDIOC_CREATE_BUFS` CAPTURE (DMABUF, count=16)
8. `VIDIOC_STREAMON` both queues
9. `VIDIOC_QUERY_EXT_CTRL` enumeration
10. `VIDIOC_S_EXT_CTRLS` (decode_mode + start_code) — global ctrls
11. Per frame: `VIDIOC_S_EXT_CTRLS` (SPS+PPS+decode_params+slice_array,
    class=0xf010000 = per-request) + `VIDIOC_QBUF` CAPTURE + `VIDIOC_QBUF`
    OUTPUT (with `V4L2_BUF_FLAG_IN_REQUEST | V4L2_BUF_FLAG_REQUEST_FD`) +
    `VIDIOC_DQBUF` OUTPUT + `VIDIOC_DQBUF` CAPTURE

**Two structural notes for the backend:**
- OUTPUT + CAPTURE both use `V4L2_MEMORY_DMABUF` in kdirect. Our backend
  currently uses MMAP for CAPTURE on rkvdec/hantro. For Pi 5 we should
  either follow kdirect (DMABUF, allows zero-copy DRM_PRIME export) or
  use MMAP and CPU-detile. Phase 4 design decision.
- The order `S_FMT OUTPUT → REQBUFS OUTPUT → S_FMT CAPTURE → CREATE_BUFS
  CAPTURE → STREAMON` differs from our iter25 rkvdec pre-seed pattern
  (where SPS via S_EXT_CTRLS must come BEFORE CAPTURE alloc to resolve
  the image_fmt). rpi-hevc-dec apparently DOESN'T need that pre-seed —
  CAPTURE S_FMT just takes the explicit NC12 + caller's dims. Confirm
  in Phase 1 by trying our existing iter25 pre-seed flow against it.

### Q6 — packaging: Debian 13 trixie, NOT Arch

higgs runs Debian 13 trixie (`PRETTY_NAME="Debian GNU/Linux 13 (trixie)"`),
not Arch ALARM. Phase 8 (per the dev-process Phase 8 packaging rule) for
the Pi 5 chapter needs a `debian/` packaging tree, not just a PKGBUILD.

Decide in Phase 1 whether to:
- Add Debian packaging to `marfrit-packages` as a second target, OR
- Use distrobox/podman with an Arch ALARM container on higgs for
  install (test-only, not production), OR
- Pi 5 chapter ships a Debian source pkg via gitea / a personal Debian
  repo.

### Other new findings from the probe session

- **ffmpeg 7.1.3 from Debian 13 is built with `--enable-v4l2-request`**
  — the kdirect path exists. Invocation is `ffmpeg -hwaccel drm -c:v
  hevc` (not just `-hwaccel drm`; the explicit codec flag matters for
  the negotiation). Engagement log line is
  `Hwaccel V4L2 HEVC stateless V4; devices: /dev/media1,/dev/video19;
  buffers: src DMABuf, dst DMABuf; swfmt=rpi4_8`. Per
  [[hw-decode-engagement-check]], grep for that line to confirm HW path
  engaged.
- **No libva ICD installed on higgs** — only `armada-drm_dri.so` ships,
  which doesn't apply. We'd be the first VA-API HW path for HEVC on Pi
  5 once installed.
- **mpv is apt-installable** (`mpv 0.40.0-3+deb13u1`) — useful as a
  pixel-readback verifier once the backend works (`mpv --vo=image` or
  `--vo=drm`).
- **Firefox 145.0.1 + rpi-firefox-mods 20251016 installed** (firefox-esr
  package status was `rc` = removed but config remains). The mods
  package likely contains VA-API plumbing prefs.

### What changes for Phase 1

- Goal is now phrasable: HEVC bit-exact libva-vs-kdirect on higgs for
  the 1280×720 Main 8-bit test fixture (same generator as
  `/tmp/bbb_main.mp4` here). Kdirect engagement signal is the
  `Hwaccel V4L2 HEVC stateless V4` log line.
- Most backend code reuses existing rkvdec/hantro HEVC path: ctrls,
  per-frame submission, request_fd, multi-device probe pattern.
- New code: NC12 video_format entry + detile primitive (sibling to
  `nv15_unpack_plane_to_p010`) + RPI_HEVC_DEC driver_kind.
- Packaging target = Debian, not Arch.
