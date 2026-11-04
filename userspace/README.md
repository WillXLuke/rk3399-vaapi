# libva-v4l2-request-fourier

VA-API ICD backend for V4L2 stateless video decoders. Fourier-campaign
fork of the dormant `bootlin/libva-v4l2-request` upstream.

> **TL;DR for "I want hardware-accelerated YouTube in Firefox on my
> Rockchip board":** skip to the [§ Quickstart](#quickstart) below.
> Fresnel (RK3399) and ampere (RK3588) are validated targets; ohm
> (RK3566 PineTab2) is the chromium-fourier validation rig.

## What works

| SoC / host | HW-accelerated codecs | Bit-exact vs `kdirect` |
|---|---|---|
| RK3399 (fresnel — Pinebook Pro) | H.264, HEVC Main, VP9 Profile 0, VP8, MPEG-2 | 5/5 at iter38; preserved through iter40b |
| RK3588 (ampere) | H.264 + HEVC (iter1+iter2 ampere-fourier); **mainline rkvdec / VDPU381 + VDPU383 landed February 2026** — VP9 / AV1 verification next | iter1 H.264 PASS; remaining codecs gated on mainline-driver bring-up |
| RK3568 / RK3566 (ohm — PineTab2) | H.264, MPEG-2, VP8 via hantro multi-planar | iter1-5 baseline (libva-multiplanar campaign) |
| BCM2712 (higgs — Pi 5 / CM5) | — | infrastructure landed (iter40 / iter40b), bit-exact NOT achieved, [see § Pi 5 standoff](#the-pi-5-standoff) |

`kdirect` is the reference: `ffmpeg -hwaccel v4l2request
-hwaccel_output_format drm_prime ...` via Kwiboo's downstream ffmpeg
patches (packaged here as **`ffmpeg-v4l2-request-fourier`**, FFmpeg 8.1
tip @ Kwiboo `v4l2-request-n8.1` commit `b57fbbe`).

## Quickstart

### What you need for HW-accelerated YouTube in Firefox

The full stack, top to bottom, with the package this campaign provides
at each layer:

| Layer | Package(s) | Notes |
|---|---|---|
| Linux kernel with V4L2 stateless decoders | `linux-fresnel-fourier` (RK3399), `linux-ampere-fourier` (RK3588) | Mainline rkvdec / hantro / VDPU381 / VDPU383. ohm typically rides on a Beryllium OS host kernel. |
| `ffmpeg` with Kwiboo's v4l2-request hwaccel | `ffmpeg-v4l2-request-fourier` | Provides `-hwaccel drm -c:v hevc` (and h264/vp9) routes via libavcodec hwdevice DRM. |
| `libva` VA-API runtime + this backend ICD | `libva` (stock) + **`libva-v4l2-request-fourier`** | This repo. Auto-detects rkvdec / hantro / cedrus on probe. |
| Firefox patched to call libavcodec stateless | `firefox-fourier` | 5-patch series, ~+169 LoC over stock Firefox. Validated on fresnel: **~5 % CPU at 1080p30 H.264** (vs 64 % software). |
| (Wayland alt) Chromium patched for V4L2VDA | `chromium-fourier` + `kwin-fourier` | Validated on ohm under KDE Plasma 6.6.5 Wayland. Needs `kwin-fourier` for the dmabuf-fence latency fix. |
| (Optional) panfrost / panthor GPU stack | `vulkan-panfrost` | Wayland compositor + 3D. |

The actual VA-API path is mostly historical inside this campaign — the
**user-facing browser HW decode story rides libavcodec's
`v4l2_request` hwaccel directly**, not VAAPI-via-libva. Firefox-fourier
attaches an `AV_HWDEVICE_TYPE_DRM` context to libavcodec's generic
`h264`/`hevc`/`vp9` decoder; libavcodec then auto-binds the
`v4l2_request` hwaccel from its `hw_configs`. No `LIBVA_DRIVER_NAME`
incantation needed for browser use. libva-v4l2-request-fourier matters
for mpv, ffmpeg-as-vaapi, and other VA-API direct consumers.

### Install on Arch ALARM (fresnel / ampere / ohm)

Add the marfrit repo if you haven't already:

```ini
# /etc/pacman.conf
[marfrit]
SigLevel = Required
Server = https://packages.reauktion.de/arch/$arch
```

Import the signing key (one-time):

```bash
sudo pacman-key --recv-keys <KEY-ID>   # see https://packages.reauktion.de
sudo pacman-key --lsign-key <KEY-ID>
sudo pacman -Sy
```

Then per host:

```bash
# Fresnel — RK3399 Pinebook Pro
sudo pacman -S \
    linux-fresnel-fourier linux-fresnel-fourier-headers \
    ffmpeg-v4l2-request-fourier \
    libva-v4l2-request-fourier \
    firefox-fourier

# Ampere — RK3588
sudo pacman -S \
    linux-ampere-fourier linux-ampere-fourier-headers \
    ffmpeg-v4l2-request-fourier \
    libva-v4l2-request-fourier \
    firefox-fourier

# Ohm — RK3566 PineTab2 (chromium-fourier validated path)
sudo pacman -S \
    ffmpeg-v4l2-request-fourier \
    libva-v4l2-request-fourier \
    kwin-fourier
# chromium-fourier currently still a local build — see § Status
```

Reboot if a new kernel landed. Then:

```bash
# Smoke-test: vainfo should list HEVCMain + H264 entries
LIBVA_DRIVER_NAME=v4l2_request vainfo

# Browser launch with verbose decoder logging
MOZ_LOG="PlatformDecoderModule:5,FFmpegVideo:5" \
  firefox-fourier 2>&1 | tee /tmp/fx.log

# Then open a YouTube 1080p H.264 video and grep for:
#   "Choosing FFmpeg pixel format for V4L2 video decoding"
#   "av_hwdevice_ctx_create(DRM, /dev/dri/renderD128) ok"
# If you DON'T see those: HW path didn't engage, fell back to software.
```

### Status of the published vs locally-built packages

As of May 2026, the live marfrit repo at
<https://packages.reauktion.de/arch/aarch64/> has:

- ✓ `libva-v4l2-request-fourier-1:1.0.0.r361.cf8cd9d-1` (iter40b tip)
- ✓ `ffmpeg-v4l2-request-fourier-2:8.1.r123329.b57fbbe-3` (Kwiboo's
  v4l2-request-n8.1 + libudev-bypass; smoke-tested on fresnel —
  HEVC via `-hwaccel v4l2request` PASS)
- ✓ `firefox-fourier-150.0.1-16` (5-patch series, sandboxed RDD HW
  decode validated on RK3399: ~5 % CPU at 1080p30 H.264)
- ✓ `linux-fresnel-fourier-7.0-14` + headers (RK3399)
- ✓ `linux-ampere-fourier-7.0rc3.kafr1-1` + headers (RK3588)
- ✓ `kwin-fourier-1:6.6.5-1` (Wayland dmabuf-fence fix for chromium-fourier)
- ✓ `vulkan-panfrost-1:26.0.5-1` (GPU stack)

NOT yet published but **present in `marfrit-packages/arch/` source
tree** (build + publish pending):

- ⏳ `chromium-fourier` (Chromium 147 + V4L2VDA-on-mainline patches —
  blocked on Arch ALARM bumping clang 22 → 23).
- ⏳ `qt6-base-fourier` (GL_ALPHA → GL_R8 fix — needed by KDE Plasma
  Wayland on the panfrost stack).

If you need those locally before they ship:

```bash
git clone ssh://git@git.reauktion.de:2222/marfrit/marfrit-packages.git
cd marfrit-packages/arch/<package>
makepkg -si
```

## What does NOT work, and why it's stalled

| Target | Status | Blocker |
|---|---|---|
| H264 Hi10P on RK3399 | enumerated, decode returns all-zero | RK3399 silicon doesn't implement 10-bit despite kernel advertising the profile (iter39 close, Option B applied) |
| HEVC Main10 on RK3399 | not enumerated | same as Hi10P |
| **Pi 5 / CM5 (BCM2712 / `rpi-hevc-dec`)** | infrastructure landed (iter40 / iter40b), bit-exact NOT achieved | see "The Pi 5 standoff" below |

## What does NOT work, and why it's stalled

| Target | Status | Blocker |
|---|---|---|
| H264 Hi10P on RK3399 | enumerated, decode returns all-zero | RK3399 silicon doesn't implement 10-bit despite kernel advertising the profile (iter39 close, Option B applied) |
| HEVC Main10 on RK3399 | not enumerated | same as Hi10P |
| **Pi 5 / CM5 (BCM2712 / `rpi-hevc-dec`)** | infrastructure landed (iter40 / iter40b), bit-exact NOT achieved | see "The Pi 5 standoff" below |

### The Pi 5 standoff

iter40 + iter40b add a third multi-device-probe slot for
`rpi-hevc-dec`, an NC12 SAND128 detile primitive, per-driver gates
around the SPS pre-seed + start-code-prepend + scaling_matrix submission,
and a (fragile, fixture-specific) SPS field override using the
GStreamer 1.28.2 H.265 parser. ICD discovery works, `vainfo` lists
`VAProfileHEVCMain`, S\_FMT / REQBUFS / STREAMON all succeed.

**Decode itself never succeeds** — every CAPTURE DQBUF returns
`V4L2_BUF_FLAG_ERROR`. Driver author John Cox confirmed strict SPS
validation is intentional ("`try_ext_ctrls returned an error (22)` is
expected as it is validating the SPS"), and VAAPI's
`VAPictureParameterBufferHEVC` simply doesn't carry the bitstream-true
scalars (`sps_max_num_reorder_pics`, `sps_max_latency_increase_plus1`,
slice-level `num_entry_point_offsets`) that the driver wants. We can't
fish the SPS out of `source_data` either, because ffmpeg-vaapi parses
the SPS itself and passes only slice NAL bytes to libva backends.

This is not a bug in our backend, in libva, in ffmpeg, or in the kernel
driver. It's an ecosystem coordination failure of long standing:

- **Kwiboo's `ffmpeg-v4l2request` hwaccel** has been in production via
  LibreELEC since December 2018. Re-submitted to ffmpeg-devel as a v2
  series in August 2024. Still un-merged in May 2026 — **eight years
  in the upstream review queue**.
- **`libva-v4l2-request`** (this project's upstream) hasn't taken
  meaningful commits since ~2021. Nobody wants to own the impedance
  mismatch between VAAPI's Intel-shaped "give me raw bitstream, I'll
  parse" and V4L2 stateless's kernel-shaped "give me parsed structs,
  I'll just drive the HW."
- **`rpi-hevc-dec` mainline submission** is at v4 (July 2025), 17
  months in review. The Pi 6.18.x downstream kernel meanwhile has
  active HEVC regressions ([raspberrypi/linux#7228](https://github.com/raspberrypi/linux/issues/7228),
  [#7306](https://github.com/raspberrypi/linux/issues/7306)) that
  aren't being fast-tracked because "the new uAPI is coming."
- **Mozilla is implementing Pi 5 HEVC via ffmpeg's hwaccel-context
  path** (bug [1969297](https://bugzilla.mozilla.org/show_bug.cgi?id=1969297)),
  not via libva — explicit acknowledgement from David Turner that
  libavcodec needs to retain the SPS context for the strict driver to
  accept the control batch.

What end-users actually do today: run Pi OS (downstream-patched ffmpeg
+ downstream kernel) or LibreELEC (Kwiboo's patches + downstream
kernel). Anyone on a stock distro outside those two: no HW HEVC on
Pi 5.

Nobody who has authority to merge has skin in the game. Everyone with
skin in the game lacks authority. Result: 8-year stalemate, three
forks of working code, no merged upstream.

### What this means for this backend

We chose to extend `libva-v4l2-request` into Pi 5 territory because
the architecture maps cleanly onto the existing iter38 multi-device
probe. That work landed (iter40 commit `3ffa9d0`, iter40b commit
`071b08d`). It's reusable infrastructure for any future strict V4L2
stateless decoder that ffmpeg ships before libva does.

But the *user-facing* Pi 5 HEVC story will not come from this
backend. The backend was a clean architectural target inside a
coordination dead-end. The actual Pi 5 HEVC path through libva
requires either:

- a VAAPI extension exposing the SPS scalars rpi-hevc-dec validates
  against (Intel-driven; no Pi-aligned principal),
- a libva-internal `VABufferType` for raw SPS/PPS NAL bytes (no
  maintainer),
- ffmpeg-vaapi forwarding `num_entry_point_offsets` to backends
  (small upstream patch; no champion), OR
- the political situation around Kwiboo's series unblocks (no
  visible movement).

iter40 + iter40b are **landed but parked**. The fresnel + ampere
sibling paths are unaffected (5/5 fresnel + 9 profiles ampere
verified post-iter40b, no regression). Phase 8 packaging is
deliberately skipped — shipping a `.deb` whose primary advertised
target (Pi 5) doesn't actually decode would mislead users.

See `phase0_pi5_hevc.md`, `phase1_pi5_hevc.md`,
`phase5_pi5_hevc_review.md`, `phase7_pi5_hevc_close.md` for the
chapter's full empirical record.

## Instructions

In order to use this backend, set the `LIBVA_DRIVER_NAME` environment
variable:

	export LIBVA_DRIVER_NAME=v4l2_request

Then a VA-API-capable player can decode supported codecs on a probed
device:

	vlc path/to/video.mp4
	mpv --hwdec=vaapi path/to/video.mp4
	ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i in.mp4 -f null -

The backend auto-detects available decoders via the V4L2 media
topology walk; honors `LIBVA_V4L2_REQUEST_VIDEO_PATH` and
`LIBVA_V4L2_REQUEST_MEDIA_PATH` for explicit device selection.

## Technical Notes

### Multi-device probe (iter38)

A single libva session opens both `rkvdec` and `hantro-vpu` (and, on
hosts where it's present, `rpi-hevc-dec`) at init. `RequestCreateConfig`
re-targets the active fd per profile via
`request_switch_device_for_profile()`. Pool teardown happens at
switch time; the next `CreateContext` rebuilds against the right
device.

### Surface / Context / Picture / Image

A Surface is an internal data structure containing rendering output.
A Context owns the V4L2 lifecycle (S\_FMT, CAPTURE pool, ctrl-batch
defaults) for one decode session. A Picture is one encoded input
frame's set of buffers. An Image is a Standard VA pixel-format view
on a decoded Surface — the backend detiles SAND/COL128 or unpacks
NV15 to NV12/P010 here so consumers see linear pitches.

The real rendering is in `EndPicture`, not `RenderPicture`, because
the kernel needs the full extended-control batch when the OUTPUT
buffer is queued, and `RenderPicture` order is consumer-defined.
