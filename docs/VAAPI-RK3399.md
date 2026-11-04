# VA-API on NanoPC-T4 (RK3399, Armbian, Linux 7.3.1)

**Status: working.** Hardware decode through VA-API is enabled and bit-exact for
H.264 and HEVC on this board.

```
$ LIBVA_DRIVER_NAME=v4l2_request vainfo --display drm --device /dev/dri/renderD128
vainfo: Driver version: v4l2-request
      VAProfileH264Main               : VAEntrypointVLD
      VAProfileH264High               : VAEntrypointVLD
      VAProfileH264ConstrainedBaseline: VAEntrypointVLD
      VAProfileHEVCMain               : VAEntrypointVLD
      VAProfileVP9Profile0            : VAEntrypointVLD
      VAProfileMPEG2Simple/Main       : VAEntrypointVLD   (hantro, not rkvdec)
      VAProfileVP8Version0_3          : VAEntrypointVLD   (hantro, not rkvdec)
```

## 1. Why this is not a literal port of `WillXLuke/libva-v4l2`

`WillXLuke/libva-v4l2` is a **stateful** V4L2 M2M backend written for Qualcomm
Iris (`decoder.cpp` only accepts a device whose `VIDIOC_QUERYCAP` driver is
`iris_driver`, it needs the Iris "decode-order output" kernel patches,
`V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY`, MSM GEM allocations and FastCV VPP).

The RK3399 has **no stateful V4L2 decoder**:

| Block | Driver | API |
|---|---|---|
| `ff660000.video-codec` (rkvdec) | `rkvdec` | **stateless** request API: `S264`/`S265`/`VP9F`, frame-based, Annex-B |
| `ff650000.video-codec` (hantro) | `hantro-vpu` | stateless, rk3399 variant only exposes **MPEG-2 + VP8** (no H.264) |

On top of that, the 7.3.1 kernel **does not even build rkvdec**
(`# CONFIG_VIDEO_ROCKCHIP_VDEC is not set`), and the 7.3 rkvdec driver is a
rewrite of the old one (in-kernel H.264 CABAC tables, `rkvdec-h264-common.c`,
VDPU381/383 backends).

Porting the stateful backend would therefore mean writing a complete H.264/HEVC
stateless parser+DPB from scratch. Instead the port uses the existing VA-API
backend built for the V4L2 stateless request API — `libva-v4l2-request`
(Bootlin lineage, maintained "fourier" fork) — and adds the missing RK3399
pieces. That is the same control-mapping work, already written.

## 2. What was installed

| Component | Location |
|---|---|
| rkvdec kernel module (upstream 7.3.1 driver, out-of-tree) | `/lib/modules/7.3.1-nanopc-t4/extra/rockchip-vdec.ko` |
| Module sources + out-of-tree Makefile | [`kernel/rkvdec-module`](kernel/rkvdec-module) |
| `modprobe rkvdec` alias | `/etc/modprobe.d/rkvdec.conf` |
| Load at boot | `/etc/modules-load.d/rkvdec.conf` |
| VA-API driver | `/usr/lib/aarch64-linux-gnu/dri/v4l2_request_drv_video.so` |
| Driver sources (fourier fork + fixes) | [`libva-v4l2-request-fourier`](libva-v4l2-request-fourier) |
| Patch with all changes vs. upstream fork | [`patches/0001-rk3399-rkvdec-vaapi-port.patch`](libva-v4l2-request-fourier/patches/0001-rk3399-rkvdec-vaapi-port.patch) |
| Driver aliases so no env var is needed (`panfrost`/`rockchip` DRM nodes) | `/usr/lib/aarch64-linux-gnu/dri/{panfrost,rockchip}_drv_video.so` |
| Optional `LIBVA_DRIVER_NAME` for login shells | `/etc/profile.d/vaapi-rkvdec.sh` |
| Build/load/unload helpers | [`kernel/build-rkvdec.sh`](kernel/build-rkvdec.sh), [`kernel/load-rkvdec.sh`](kernel/load-rkvdec.sh) |

Device nodes after loading: `/dev/video3` (`rkvdec`, `S264`/`S265`/`VP9F` in,
`NV12` out) and `/dev/media1`. The backend auto-detects them by driver name, so
no `LIBVA_V4L2_REQUEST_VIDEO_PATH`/`MEDIA_PATH` is needed.

## 3. Fixes applied to the stateless backend

The upstream fork targets an older rkvdec/kernel and did not work on this
kernel. Changes (see patch):

1. **VA-API init symbol** — the build derived `__vaDriverInit_<maj>_<min>` from
   the libva *package* version (`2.22`), but libva looks for the *VA-API*
   version (`1.22`), so the driver never loaded. Now taken from
   `va/va_version.h`.
2. **HEVC EXT_SPS_RPS definitions** — the vendored copy collided with the
   definitions that 7.x UAPI headers already provide; the vendored header now
   just includes `<linux/v4l2-controls.h>`.
3. **DPB compaction (the important one)** — the backend emitted reference
   pictures at drifting DPB slot indices (…, 7, 8, 9, 0, …). rkvdec resolves
   the hardware RPS table through those indices, and once the index crossed a
   threshold decoding broke at the `frame_num` wraparound (every 16 frames with
   x264's `log2_max_frame_num_minus4=0` — green garbage frames). Emitting the
   active entries densely at `dpb[0..n-1]`, like GStreamer and FFmpeg do, fixed
   it. Bisected against `v4l2slh264dec`, which was bit-exact on the same clip.
4. **Reference picture refresh on surface reuse** — VA surface IDs are recycled;
   a stale DPB entry kept the old `frame_num`/POC and poisoned the kernel's
   reference-list builder. The current `VAPictureParameterBufferH264` is now
   authoritative for every matched entry.
5. **Scaling matrix handling** — VAAPI does not expose the PPS
   `scaling_matrix_present_flag`, and FFmpeg always forwards an IQ-matrix buffer
   (flat when the stream has none). Advertising
   `V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT` for a flat matrix made rkvdec
   program the quantiser where the spec default applies (≈0.3 mean pixel error).
   The flag is now only set for non-flat matrices, unpopulated entries are
   normalised to the flat value 16, and the 4:2:0 chroma 8×8 list is emitted at
   slot 1 instead of slot 3.
6. **Surface ownership / teardown** — `vaDestroyContext` used to destroy the
   application-owned surfaces, and the capture pool was freed while surfaces
   still held slot pointers. FFmpeg's decoder re-initialisation (`-stream_loop`)
   aborted with SIGSEGV/SIGBUS/SIGABRT (valgrind: invalid write in
   `cap_pool_release` from `RequestDestroySurfaces`). Surfaces now survive
   context destruction, their slot bindings are dropped before the pool is torn
   down, and `surface_unbind_slot()` is safe with a destroyed pool.

## 4. Validation

`ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i <clip> -vf
hwdownload,format=nv12 -f rawvideo` compared byte-for-byte against the software
decoder (`-pix_fmt nv12`), on clips generated locally with libx264/libx265:

| Clip | Frames | Result |
|---|---|---|
| H.264 720p, High, B-frames, 60-frame GOP (frame_num wrap) | 60 | **bit-exact** |
| H.264 720p, High, B-frames, keyint 12/30 | 60 | **bit-exact** |
| H.264 720p with custom quantisation matrix (`cqm=jvt`) | 45 | **bit-exact** |
| H.264 1080p, High, B-frames | 60 | bit-exact (1 pixel differs) |
| HEVC Main 720p / 1080p | 90 / 60 | **bit-exact** |
| VP9 Profile 0 720p | 60 | decodes, mean error 0.17/byte — *not* bit-exact (see §7) |

Cross-checked with GStreamer's `v4l2slh264dec`/`v4l2slvp9dec` on the same
kernel driver (both were bit-exact), which is how the DPB and matrix issues were
isolated.

mpv: `mpv --hwdec=vaapi file.mp4` selects `h264-vaapi` and decodes in hardware.

## 5. Performance (1080p, 600 frames, `-f null`)

| Codec | VA-API | software |
|---|---:|---:|
| H.264 High | **576 fps** | 228 fps |
| HEVC Main | **291 fps** | 91 fps |

720p H.264 reaches ~680 fps. 4K is not supported by the rk3399 rkvdec variant
(max 4096×2304 input, NV12 output).

## 6. Usage

No environment variable is required: `/usr/lib/aarch64-linux-gnu/dri/panfrost_drv_video.so`
(and `rockchip_drv_video.so`) are symlinks to the backend, so libva picks it up
from the DRM render node automatically. `LIBVA_DRIVER_NAME=v4l2_request` still
works and is set for login shells by `/etc/profile.d/vaapi-rkvdec.sh`.

```sh
vainfo --display drm --device /dev/dri/renderD128

ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
       -hwaccel_output_format vaapi -i input.mp4 -f null -

ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
       -hwaccel_output_format vaapi -i input.mp4 \
       -vf hwdownload,format=nv12 -c:v libx264 -preset ultrafast out.mp4

mpv --hwdec=vaapi input.mp4
```

Set `LIBVA_V4L2_REQUEST_DEBUG=1` to get the backend's diagnostics on stderr
(it is silent by default).

## 7. Known limitations

- **Decode only.** RK3399 has no HEVC encoder, and the Hantro H.264 encoder is
  a separate V4L2 M2M device that this backend does not drive.
- **HEVC Main 8-bit only.** rkvdec on rk3399 enumerates only `NV12`; Main10
  would need `NV15`, which this variant does not expose.
- **VP9 decodes but is not bit-exact** (mean 0.17/byte against software). The
  H.264/HEVC paths are exact; VP9 may need segmentation/probability-table
  mapping work. GStreamer's VP9 decoder is bit-exact on the same kernel driver,
  so the gap is in the VA-API VP9 mapping.
- **mp4/MKV with `-stream_loop` and H.264** makes FFmpeg re-initialise the
  hwaccel; FFmpeg then reports "hwaccel changed" and aborts the filter graph.
  This is an FFmpeg reinit limitation, not a driver crash anymore (it no longer
  segfaults). Real playback (mpv, a single file) is unaffected.
- The out-of-tree rkvdec module is tied to `7.3.1-nanopc-t4`: after every kernel
  upgrade it must be rebuilt (`kernel/build-rkvdec.sh`) or the node disappears.
- The 7.3 rkvdec driver is new and the module taints the kernel ("out-of-tree
  module"). `v4l2-compliance -d /dev/video3` passes 49/49.

## 8. Rebuilding

Kernel module (after a kernel upgrade):

```sh
/home/durian/workspace/kernel/build-rkvdec.sh   # builds + installs + depmod
sudo modprobe rkvdec
```

VA-API driver:

```sh
cd /home/durian/workspace/libva-v4l2-request-fourier
meson setup build --buildtype=release \
      -Dkernel_headers=/home/durian/workspace/kernel/hdrs \
      --prefix=/usr -Dlibdir=lib/aarch64-linux-gnu
meson compile -C build && sudo meson install -C build
```

(`kernel/hdrs` is the sanitized UAPI header tree generated with
`make headers_install` from the 7.3.1 sources; the stateless control structs
must match the running kernel exactly.)

Test assets live in [`va-test/`](va-test) (H.264/HEVC/VP9 clips at 720p and
1080p, including a custom-quantiser-matrix clip).

## H.264 hardware encoding (added later)

Encoding is now also available through VA-API: `VAEntrypointEncSlice` for
H.264 Constrained Baseline / Main / High on the VEPU2 encoder
(`/dev/videoN` with card `rockchip,rk3399-vpu-enc`). CQP only (`-qp`), one
reference, no B frames, SPS/PPS generated by the driver backend.

```sh
ffmpeg -vaapi_device /dev/dri/renderD128 -f lavfi \
  -i testsrc2=size=1280x720:rate=30 -t 2 \
  -vf format=nv12,hwupload -c:v h264_vaapi -qp 26 -bf 0 out.h264
```

This required porting Bootlin's VEPU2 H.264 encoder into the 7.3.1 hantro
driver and adding the stateless-encode UAPI/ctrl types (rebuilt `videodev.ko`),
plus the VA-API frontend. Details, limitations and rollback:
[VA-ENCODER-STATUS.md](VA-ENCODER-STATUS.md) and
[kernel/hantro-enc/HANDOFF.md](kernel/hantro-enc/HANDOFF.md).
