# VA-API hardware H.264 on the RK3399 (NanoPC-T4)

Everything needed to expose the Rockchip RK3399 video engine through VA-API on a
mainline kernel, and to encode H.264 from it:

| Piece | What it does |
| --- | --- |
| `patches/0001-media-verisilicon-rk3399-h264-encoder.patch` | Adds the H.264 encoder to the mainline `hantro`/`verisilicon` driver, exposed as a V4L2 memory-to-memory encoder on the VPU2 (`/dev/videoN`) |
| `vaapi-driver/` | `libva-v4l2` with an `VAEntrypointEncSlice` H.264 backend on top of that V4L2 encoder, plus the fixes described below |
| `tools/v4l2_h264_enc_test.c` | Standalone smoke test for the V4L2 encoder, no VA-API in the path |

Tested on Armbian/Ubuntu 26.04, kernel 7.3.3 (aarch64, NanoPC-T4), with FFmpeg 8
and Selkies/pixelflux as the consumers.

## Why this exists

Mainline `hantro-vpu` on the RK3399 registers the VPU2 encoder as a **JPEG-only**
memory-to-memory device. H.264 encoding support for the VPU2 lives in the 2020
RFC series ("media: verisilicon: Add H.264 stateless encoder") and never landed.
The patch here is that work rebased onto 7.3.3, with the private per-frame
register block (including the encoder's rate-control inputs) passed through
`V4L2_CID_HANTRO_H264_ENCODE_PARAMS`.

## Install from a release

```sh
wget https://github.com/WillXLuke/rk3399-vaapi/releases/latest/download/libva-v4l2-rk3399_1.0.0-1_arm64.deb
wget https://github.com/WillXLuke/rk3399-vaapi/releases/latest/download/hantro-vpu-rk3399-modules-7.3.3-nanopc-t4_1.0.0-1_arm64.deb
sudo dpkg -i ./*.deb
```

The module package is tied to the kernel it was built against (7.3.3-nanopc-t4);
on another kernel use the patch and build it yourself. After installing the
driver, `LIBVA_DRIVER_NAME=v4l2 vainfo` should list `VAEntrypointEncSlice`.

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
vainfo        # VAProfileH264High : VAEntrypointEncSlice
```

## Use

```sh
# encode a frame, no VA-API involved
./tools/v4l2_h264_enc_test /dev/videoN out.h264

# encode through VA-API
LIBVA_DRIVER_NAME=v4l2 ffmpeg -vaapi_device /dev/dri/renderD128 \
    -f lavfi -i testsrc2=size=1280x720:rate=30 -t 3 \
    -vf format=nv12,hwupload -c:v h264_vaapi -b:v 8M out.h264
```

The decoder side (RKVDU, H.264/HEVC/VP9, 4K) is mainline `rkvdec` driven by the
same driver's `VAEntrypointVLD` paths, so this replaces `libva-v4l2-request` as
well.

## What the driver adds on top of upstream libva-v4l2

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

## Performance and limits

Measured on the NanoPC-T4 (4 GB):

| Size | Throughput | Notes |
| --- | --- | --- |
| 1920x1080 | ~33 fps | encoder ceiling |
| 1280x720 | ~49 fps | CPU conversion still costs about one core |

* I/P frames only, one reference, CAVLC (the VPU2 CABAC slice-header padding is
  not spec-compliant), max 1080p.
* Quality per bit is below x264; a dense 720p desktop needs roughly 8-16 Mbps to
  stay sharp.
* The SPS carries no VUI, so players assume HD = BT.709 limited.

## Licence

`vaapi-driver/` is the MIT-licensed `libva-v4l2` (see its `COPYING`), with local
changes under the same terms. The kernel patch is GPL-2.0, like the tree it
applies to.
