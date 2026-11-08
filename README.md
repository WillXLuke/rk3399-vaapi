# VA-API hardware H.264 encode and H.265 decode on the RK3399 (NanoPC-T4)

Everything needed to expose the Rockchip RK3399 video engine through VA-API on a
mainline kernel: H.264 encoding from the VPU2, and H.264/HEVC decoding from
`rkvdec`:

| Piece | What it does |
| --- | --- |
| `patches/0001-media-verisilicon-rk3399-h264-encoder.patch` | Adds the H.264 encoder to the mainline `hantro`/`verisilicon` driver, exposed as a V4L2 memory-to-memory encoder on the VPU2 (`/dev/videoN`) |
| `vaapi-driver/` | `libva-v4l2` with an `VAEntrypointEncSlice` H.264 backend, `VAEntrypointVLD` H.264/HEVC Main/Main10 decode, and the video-processing fixes described below |
| `tools/v4l2_h264_enc_test.c` | Standalone smoke test for the V4L2 encoder, no VA-API in the path |
| `tools/test_hevc_vaapi.sh` | End-to-end HEVC decode test: profile check, 4K60 validation, throughput, and optional bit-exact comparison with the software decoder |

Tested on Armbian/Ubuntu 26.04, kernel 7.3.3 (aarch64, NanoPC-T4), with FFmpeg 8
and Selkies/pixelflux as the consumers.

## Why this exists

Mainline `hantro-vpu` on the RK3399 registers the VPU2 encoder as a **JPEG-only**
memory-to-memory device. H.264 encoding support for the VPU2 lives in the 2020
RFC series ("media: verisilicon: Add H.264 stateless encoder") and never landed.
The patch here is that work rebased onto 7.3.3, with the private per-frame
register block (including the encoder's rate-control inputs) passed through
`V4L2_CID_HANTRO_H264_ENCODE_PARAMS`.

On the decode side the kernel already has a working stateless HEVC backend for
`rkvdec`, but `libva-v4l2` never advertised it. This tree contains the missing
VA-API HEVC context: VAPictureParameterBufferHEVC/VASliceParameterBufferHEVC are
translated to the modern stateless V4L2 controls, with the DPB, reference lists
and inline short-term RPS offsets handled correctly. Both Main (8-bit) and
Main10 (10-bit) are exposed; Main10 is handed to clients as a P010 surface and
unpacked from rkvdec's packed NV15 capture buffers.

## Install from a release

```sh
wget https://github.com/WillXLuke/rk3399-vaapi/releases/latest/download/libva-v4l2-rk3399_1.0.3-1_arm64.deb
wget https://github.com/WillXLuke/rk3399-vaapi/releases/latest/download/rk3399-vpu-modules-7.3.3-nanopc-t4_1.0.3-1_arm64.deb
sudo dpkg -i ./*.deb
```

The module package carries both `hantro-vpu.ko` (encoder) and `rockchip-vdec.ko`
(decoder) and is tied to the kernel it was built against (7.3.3-nanopc-t4); on
another kernel use the patch and build them yourself. After installing the
driver, `LIBVA_DRIVER_NAME=v4l2 vainfo` should list `VAEntrypointEncSlice` for
H.264 and `VAEntrypointVLD` for `VAProfileHEVCMain`/`VAProfileHEVCMain10`.

## Build

### 1. Kernel modules

Against a configured kernel tree, then load them:

```sh
cd <kernel-source>
patch -p1 < patches/0001-media-verisilicon-rk3399-h264-encoder.patch
make -j$(nproc) ARCH=arm64 KBUILD_MODPOST_WARN=1 M=drivers/media/platform/verisilicon modules
sudo cp drivers/media/platform/verisilicon/hantro-vpu.ko /lib/modules/$(uname -r)/updates/
sudo depmod -a
sudo modprobe -r hantro_vpu; sudo modprobe hantro_vpu
```

`KBUILD_MODPOST_WARN=1` is needed when only `modules_prepare` was run rather
than a full build; the symbols resolve against the running kernel.

The encoder node appears as `rockchip,rk3399-vpu-enc`, with `H264` among its
capture formats:

```sh
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoN --list-formats        # [1]: 'H264'
```

The decoder needs only the mainline `rockchip-vdec.ko`; the modules package
ships it for the 7.3.3-nanopc-t4 kernel.

### 2. VA-API driver

```sh
./scripts/build-vaapi-driver.sh --install
# or by hand:
cd vaapi-driver
CC=gcc-14 CXX=g++-14 meson setup build --buildtype=release
ninja -C build
sudo cp build/src/v4l2_drv_video.so /usr/lib/aarch64-linux-gnu/dri/
```

GCC 15 on this board ICEs with `internal compiler error: Illegal instruction`;
gcc-14 builds it cleanly, which is why the script prefers it when present.

The driver replaces the stock `v4l2_drv_video.so` and is selected with
`LIBVA_DRIVER_NAME=v4l2` (or by pointing `panfrost_drv_video.so` at it, which is
how a Mali render node selects it automatically).

```sh
LIBVA_DRIVER_NAME=v4l2 vainfo
# VAProfileH264High : VAEntrypointEncSlice
# VAProfileHEVCMain / VAProfileHEVCMain10 : VAEntrypointVLD
```

## Use

```sh
# encode a frame, no VA-API involved
./tools/v4l2_h264_enc_test /dev/videoN out.h264

# encode through VA-API
LIBVA_DRIVER_NAME=v4l2 ffmpeg -vaapi_device /dev/dri/renderD128 \
    -f lavfi -i testsrc2=size=1280x720:rate=30 -t 3 \
    -vf format=nv12,hwupload -c:v h264_vaapi -b:v 8M out.h264

# decode HEVC through VA-API, benchmark only (no frame download)
LIBVA_DRIVER_NAME=v4l2 ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
    -hwaccel_output_format vaapi -i input.mp4 -f null -

# download the decoded frames (NV12 for Main, P010 for Main10)
LIBVA_DRIVER_NAME=v4l2 ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
    -hwaccel_output_format vaapi -i input.mp4 \
    -vf hwdownload,format=p010le -f rawvideo out.p010

# branded test: require 4K, 60 fps and at least 30 seconds, then compare
# the hardware output with the software decoder
./tools/test_hevc_vaapi.sh input.mp4 --require-4k60 --compare
```

The decoder side (RKVDU, H.264/HEVC/VP9, 4K) is mainline `rkvdec` driven by the
same driver's `VAEntrypointVLD` paths, so this replaces `libva-v4l2-request` as
well. HEVC Main and Main10 are advertised; VP9 remains hidden unless the driver
is built with the GStreamer codec-parser headers.

## What the driver adds on top of upstream libva-v4l2

* **HEVC decode** (`VAEntrypointVLD`, `VAProfileHEVCMain` and
  `VAProfileHEVCMain10`): maps the VA picture/slice buffers to
  `V4L2_CID_STATELESS_HEVC_{SPS,PPS,DECODE_PARAMS,SLICE_PARAMS,SCALING_MATRIX}`,
  fills DPB entries with the capture-buffer timestamps the kernel uses to find
  references, translates VA's `RefPicList[]` (indices into `ReferenceFrames[]`,
  not surface IDs) to V4L2 DPB indices, and passes `st_rps_bits` as the inline
  short-term RPS bit offset. Without that last field rkvdec feeds the RPS bits to
  the hardware as slice data and every P-frame job errors.
* **Main10 output**: rkvdec produces packed NV15 (15 bits/pixel), while VA-API
  clients expect the P010 16-bit container. The driver exposes P010 surfaces,
  negotiates them with FFmpeg, and unpacks NV15 to P010 in
  `vaGetImage`/`vaDeriveImage`. `vaExportSurfaceHandle` (DRM PRIME) is not
  implemented for 10-bit, so 10-bit consumers should download frames.
* **H.264 encode** (`VAEntrypointEncSlice`) writing the VPU2's private register
  block per frame, including SPS/PPS the hardware cannot template (16-bit
  `frame_num`, CAVLC-only slice headers).
* **Video processing** (`VAEntrypointVideoProc`, `VAProfileNone`): the encoder
  wants NV12 surfaces, applications upload packed RGB32, so `vaPutImage`,
  `vaBeginPicture`/`vaRenderPicture`/`vaEndPicture` and `vaQueryVideoProcPipelineCaps`
  are implemented and convert on the CPU (BT.709 limited, point-sampled chroma).
  Without it encoders such as pixelflux refuse the hardware path entirely.
* **Rate control fixed**: the frame rate is decoded from the packed
  `VAEncMiscParameterFrameRate` value (pixelflux sends `(1 << 16) | fps`, which
  read raw is 65566 fps); the per-frame budget is `bps / 8 / fps` in bytes rather
  than bits; and a new PPS is emitted whenever rate control moves the QP, because
  the VPU2 writes no slice QP delta and a stale `pic_init_qp_minus26` decodes
  every later frame at the wrong QP (soft, flickering, then grey mush).
* **4K memory budget**: one capture buffer per bound surface, with a 24-buffer
  floor and 4 MiB compressed-source buffers rather than one raw frame each.
  The old 32-buffer/raw-size allocation needed about 670 MB at 4K on this 4 GB
  board; the current one is about 0.5 GB for 10-bit 4K.

## Performance and limits

Measured on the NanoPC-T4 (4 GB), kernel 7.3.3, mainline `rkvdec`:

| Direction | Codec / content | Result |
| --- | --- | --- |
| Decode | HEVC Main10, 3840x2160@59.94, 32.53 s (1950 frames) | **62.4 fps end-to-end** with the CPU governor at `performance`; 57.7-60.5 fps with `ondemand` under load. All frames decoded, no buffer errors |
| Decode | HEVC Main10, 3840x2160@59.94, 6.5 s (390 frames) | P010 output **bit-exact** against FFmpeg software decode |
| Decode | HEVC Main, 1280x720@30, 5 s (150 frames) | **340 fps**, bit-exact against software decode |
| Decode | HEVC Main10 download to P010 | ~5.7 fps, CPU-bound by the scalar NV15->P010 unpack; decode-to-null is unaffected |
| Decode | H.264, 3840x2160 | 80.8 fps (GStreamer `v4l2slh264dec`) |
| Decode | VP9 Profile 0, 3840x2160 | 4.8 fps - CPU-bound, see below |
| Decode | VP8 1080p / MPEG-2 1080p | 75.9 fps / 181.8 fps (hantro) |
| Decode | JPEG up to 8176x8176 | no decoder driver in mainline |
| Decode | MPEG-4, VC-1 1080p | no driver in mainline |
| Encode | H.264 High, 1920x1080, 600 frames | 40 fps sustained (~26 Mbps at QP floor) |
| Encode | H.264, Selkies path, 1920x1080 live | 26 fps (was 21 before 1.0.2) |
| Encode | JPEG, 8176x8176 | 2.7 MB image, works |
| Encode | VP8 1080p | no VP8 encoder in the VPU2 driver |

The 4K60 HEVC result includes decoder setup. With the CPU governor pinned to
`performance`, 1950 frames (32.53 s of content) decoded in 31.26 s: 62.4 fps
end-to-end. With the default `ondemand` governor the same run measures
57.7-60.5 fps depending on CPU load. GStreamer's native `v4l2slh265dec` path
measures 60.8 fps on the same 6.5 s excerpt, so the VA-API layer is within
measurement noise of the kernel decoder.

Notes:

* **Main10 uses P010 surfaces backed by NV15 capture buffers.** The unpack is a
  CPU copy when frames are downloaded; it does not slow `-f null` decode. A
  future revision can export the packed buffer directly as DRM_FORMAT_NV15 or
  add a NEON unpack if 10-bit download throughput matters.
* **VP9 decode is CPU-bound (~5 fps at 4K, ~12 fps at 1080p)** with all six
  cores busy, while the same hardware decodes H.264 and HEVC at 80+ fps. The
  element does select `rkvdec-proc` and NV12, so this is per-frame work in the
  VP9 path (parser/copies) that needs profiling before the 4K60 claim holds.
* **JPEG encode tops out at 8176x8176**, not 8192: the VPU2 JPEG block addresses
  511 macroblocks per row, so a width of 8192 wraps to zero and used to produce
  a 0x0 JPEG silently. The driver now advertises 8176 and rejects larger sizes.
* H.264 encode is I/P only with one reference and CAVLC; 40 fps at 1080p is the
  sustained rate, above the 30 fps the VPU2 is specified for.

## Validation

All VA-API results above were produced with
`tools/test_hevc_vaapi.sh` (or the equivalent FFmpeg command) on the NanoPC-T4.
The earlier kernel-decoder rows in the table above use GStreamer `v4l2codecs`
and are included for comparison with the kernel driver's capability.

| Direction | Codec | Content | Result |
| --- | --- | --- | --- |
| Decode | HEVC Main | 1280x720, 150 frames | 340 fps, bit-exact (MD5) |
| Decode | HEVC Main10 | 3840x2160@59.94, 390 frames | P010 bit-exact (MD5 `34bd5020...`) |
| Decode | HEVC Main10 | 3840x2160@59.94, 1950 frames / 32.53 s | 62.4 fps end-to-end (performance governor), 0 errors |
| Decode | H.264 | 3840x2160 | 80.8 fps (GStreamer) |
| Decode | HEVC Main10 | 3840x2160, 10-bit | 87.4 fps (GStreamer) |
| Encode | H.264 High | 1920x1080, 600 frames | 40 fps sustained |

The 32.53 s test stream is the uploaded 6.5 s 4K60 Main10 clip looped five times
with `-c copy`; it is 1950 frames at 3840x2160, 59.94 fps.

## Licence

`vaapi-driver/` is the MIT-licensed `libva-v4l2` (see its `COPYING`), with local
changes under the same terms. The kernel patch is GPL-2.0, like the tree it
applies to.
