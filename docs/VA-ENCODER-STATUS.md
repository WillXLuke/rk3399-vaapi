# VA-API H.264 hardware encode (RK3399 VEPU2) — implemented & verified

Status: **done**. `VAEntrypointEncSlice` for H.264 Constrained Baseline / Main /
High is implemented in `libva-v4l2-request-fourier` on top of the stateless
H.264 encode UAPI of the ported hantro driver, and installed system-wide
(`/usr/lib/aarch64-linux-gnu/dri/v4l2_request_drv_video.so`). The existing
decode path is unchanged and still bit-exact against software decode.

## Implementation

New files:

| file | purpose |
|---|---|
| `src/encoder.c` / `src/encoder.h` | encoder device probe, V4L2 queue/buffer/request lifecycle, SPS/PPS serialiser (bit writer + emulation prevention), per-frame submit/wait, VA coded-buffer assembly |
| `src/h264_enc_ctrls.h` | fallback mirror of the three stateless-encode CIDs/structs (guarded; used only when the kernel headers lack them) |

Touched files (decode code paths unchanged, only new branches):

- `request.{c,h}` — encoder device auto-detect at driver init
  (`hantro-vpu` + card `rockchip,rk3399-vpu-enc` + CAPTURE `H264_SLICE`),
  `video_fd_enc`/`media_fd_enc`/`encode_active`, fd cleanup at terminate.
- `config.{c,h}` — `VAEntrypointEncSlice` configs, EncSlice in
  `vaQueryConfigEntrypoints` alongside VLD, attributes: RTFormat YUV420,
  RateControl `VA_RC_CQP|VA_RC_NONE`, EncPackedHeaders=0, EncMaxRefFrames
  `0x00010001`, EncMaxSlices=1.
- `context.{c,h}` — encoder contexts own a private `struct encoder_context`;
  decoder contexts got an explicit `encoder = NULL` (the object heap recycles
  memory, so this is not implicitly zero).
- `picture.c` — Begin/Render/EndPicture dispatch to the encoder frontend.
- `surface.{c,h}` — encoder-owned NV12 storage per surface (lazily allocated),
  encode-aware `vaSyncSurface` (encode is synchronous in vaEndPicture),
  decode BeginPicture drops any storage left by a pre-context derive probe.
- `image.c` — encode `vaCreateImage` sizing, `vaPutImage`/`vaGetImage` copies,
  and `vaDeriveImage` that aliases the surface NV12 storage (the upload path
  ffmpeg 7.x actually uses).
- `buffer.{c,h}` — accepts the VAEnc* buffer types; foreign-memory buffers for
  derived images.
- `v4l2.{c,h}` — timestamped per-plane QBUF and DQBUF-with-bytesused helpers.
- `media.{c,h}` — request wait with explicit timeout (2.5 s, matching the
  hantro watchdog).
- `src/meson.build` — new sources.

### Flow per frame (vaEndPicture)

1. SPS+PPS serialised from `VAEncSequenceParameterBufferH264` /
   `VAEncPictureParameterBufferH264` on IDR pictures only. SPS forces
   `pic_order_cnt_type = 2`, `log2_max_frame_num_minus4 = 12` (VEPU2 writes a
   16-bit frame_num and no POC LSB); Annex-B 4-byte start codes; emulation
   prevention applied.
2. Surface NV12 copied into the OUTPUT `NV12M` planes (row-wise, kernel
   bytesperline aware).
3. OUTPUT queued with `V4L2_BUF_FLAG_REQUEST_FD` + a unique nanosecond
   timestamp (`TIMESTAMP_COPY` propagates it to the paired CAPTURE buffer,
   which is what the kernel's `vb2_find_buffer()` resolves P references by),
   CAPTURE queued without a request.
4. `ENCODE_PARAMS` + `ENCODE_RC` set on the request; request queued; poll for
   completion; both buffers dequeued; request `REINIT`ed for reuse.
5. Slice NAL (`bytestatus[0]`) appended after SPS/PPS into the
   `VAEncCodedBufferType` buffer as one `VACodedBufferSegment`
   (`segment->size` = payload length, `segment->buf` = payload).

`reference_ts` is the previous frame's output timestamp in **nanoseconds**
(vb2 timestamp domain). Using microseconds there is what initially made every
P picture fail: `vb2_find_buffer()` compares ns.

## Acceptance results

### (a) vainfo

```
$ vainfo --display drm --device /dev/dri/renderD128
...
vainfo: Driver version: v4l2-request
      VAProfileMPEG2Simple            :	VAEntrypointVLD
      VAProfileMPEG2Main              :	VAEntrypointVLD
      VAProfileH264Main               :	VAEntrypointVLD
      VAProfileH264Main               :	VAEntrypointEncSlice
      VAProfileH264High               :	VAEntrypointVLD
      VAProfileH264High               :	VAEntrypointEncSlice
      VAProfileH264ConstrainedBaseline:	VAEntrypointVLD
      VAProfileH264ConstrainedBaseline:	VAEntrypointEncSlice
      VAProfileH264MultiviewHigh      :	VAEntrypointVLD
      VAProfileH264StereoHigh         :	VAEntrypointVLD
      VAProfileHEVCMain               :	VAEntrypointVLD
      VAProfileVP8Version0_3          :	VAEntrypointVLD
      VAProfileVP9Profile0            :	VAEntrypointVLD
```

### (b) encode

```
$ ffmpeg -hide_banner -vaapi_device /dev/dri/renderD128 \
    -f lavfi -i testsrc2=size=640x480:rate=25 -t 1 \
    -vf format=nv12,hwupload -c:v h264_vaapi -qp 26 -bf 0 -f h264 /tmp/va_enc.h264
[h264_vaapi @ ...] Driver does not support some wanted packed headers (wanted 0xd, found 0).
Stream #0:0: Video: h264 (High), vaapi(tv, progressive), 640x480 ...
frame=   25 fps=0.0 q=-0.0 Lsize=     170KiB time=00:00:01.00 bitrate=1394.9kbits/s speed=8.42x
```
Stream header bytes (matches the known-good Bootlin layout):
`00 00 00 01 67 64 c0 1e ... 00 00 00 01 68 ee ... 00 00 00 01 25 b8 ...`

### (c) decode the encoded stream

```
$ ffmpeg -hide_banner -i /tmp/va_enc.h264 -f null -
Input #0, h264, from '/tmp/va_enc.h264':
  Stream #0:0: Video: h264 (High), yuv420p(progressive), 640x480, 25 fps
frame=   25 fps=0.0 q=-0.0 Lsize=N/A time=00:00:01.00 bitrate=N/A speed=26.1x
```

### (d) decode regression

```
$ ffmpeg -hide_banner -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
    -hwaccel_output_format vaapi -i /home/durian/workspace/va-test/h_keyint60.mp4 -f null -
frame=   60 fps=0.0 q=-0.0 Lsize=N/A time=00:00:02.00 bitrate=N/A speed=14.6x
(exit 0)
```

## Extra verification

- **Pixel fidelity** (640x480 testsrc2, `-qp 26`): PSNR 43.8–47.0 dB
  (avg 45.5 dB) versus the untouched source, i.e. real picture content, not a
  blank/green frame.
- **Bit-exact decode**: `test_h264.mp4` decoded through VA-API + `hwdownload`
  has the same per-frame MD5 as the software decode reference
  (`va-test/sw_h264.md5`, 90 frames, byte-identical).
- **Profiles**: Constrained Baseline (CAVLC), Main and High all encode and
  decode cleanly (`-profile:v constrained_baseline|main|high`).
- **Geometry**: 1280x720/30 fps (`-g 30`, 60 frames) and non-MB-aligned
  638x358 both encode and decode at the exact display size (SPS cropping from
  the VA sequence buffer).
- **Other codecs**: HEVC 90/90, VP9 60/60, MPEG-2 25/25, VP8 25/25 decode
  unchanged.
- **vaPutImage upload path**: standalone test
  `/home/durian/workspace/va-test/enc_putimage_test.c` (build:
  `gcc -O2 -o enc_putimage_test enc_putimage_test.c -lva -lva-drm`) encodes
  10 frames uploaded only via `vaCreateImage`+`vaPutImage`; decoded output is
  34.8–45.4 dB PSNR vs. the generated source. (ffmpeg 7.x itself takes the
  derive-alias path.)
- **Mixed decode+encode in one process**: decoding `h_keyint60.mp4` with VA-API
  and encoding the same frames with h264_vaapi in one ffmpeg run produces a
  clean 60-frame stream.

## Known limitations

- CQP only (`VA_RC_CQP`, `VA_RC_NONE` accepted). CBR/VBR would need the
  checkpoint/MAD rate-control loop, which is not implemented; the misc
  RateControl buffer's `initial_qp` is honoured if present.
- One slice per picture, one reference frame, no B frames. EncMaxSlices=1;
  EncSliceStructure is reported unsupported (multi-slice requests fail early
  in ffmpeg).
- SPS/PPS are generated by the driver (PackedHeaders advertised as 0) and are
  emitted on IDR pictures only.
- One encoder context per process (each context owns the V4L2 queues of the
  probed encoder node).
- Media requests are bound to OUTPUT buffers (the verified flow: QBUF OUTPUT
  with the request fd, QBUF CAPTURE without), one request per buffer, reused
  via `MEDIA_REQUEST_IOC_REINIT`. The CAPTURE buffer still gets the request's
  timestamp via `TIMESTAMP_COPY`, which is what `reference_ts` resolves
  against.
- `LIBVA_V4L2_ENCODER_NO_DERIVE=1` forces the create+put upload path for
  debugging; with ffmpeg 7.x it produces black frames because that ffmpeg
  build defers the image write-back, so leave it unset.

## Installed state / rollback

```
/usr/lib/aarch64-linux-gnu/dri/v4l2_request_drv_video.so  (encoder build)
/home/durian/workspace/libva-backup/v4l2_request_drv_video.so.decode-only  (previous decoder-only .so)
```

Rollback: `sudo cp /home/durian/workspace/libva-backup/v4l2_request_drv_video.so.decode-only /usr/lib/aarch64-linux-gnu/dri/v4l2_request_drv_video.so`.

Rebuild: the documented command works from a clean directory (verified, and it
produces a byte-identical .so):

```
meson setup build --buildtype=release \
  -Dkernel_headers=/home/durian/workspace/kernel/hantro-enc/hdrs \
  --prefix=/usr -Dlibdir=lib/aarch64-linux-gnu
ninja -C build && sudo meson install -C build
```

## Update: 10-second matrix, Main-profile fix, CBR/VBR

A 10-second (300-frame) matrix across profiles and resolutions uncovered two
real bugs and a missing feature:

1. **Main profile produced garbage** (PSNR ~11.8 dB): the PPS writer forced
   `entropy_coding_mode_flag = 0` for every non-High profile, while the encode
   parameters still enabled CABAC for Main. The decoder got CAVLC while the
   hardware emitted CABAC. Fixed by gating CAVLC only on Constrained Baseline.
2. **CBR/VBR were rejected** (`vaCreateConfig` advertised CQP only), which is
   what browser/WebRTC encoders request. A frame-level QP controller was added:
   the target comes from `VAEncSequenceParameterBufferH264.bits_per_second`
   (FFmpeg puts the CBR peak there, i.e. 2x the average) or the misc
   RateControl buffer, and the QP adapts on the encoded frame size.
3. **10-bit/odd geometry**: 854x480 encodes and decodes at the exact size.

Measured after the fixes (10 s, 300 frames, testsrc2):

| Resolution | Profile | Result |
|---|---|---|
| 640x480 | Constrained Baseline / Main / High | 45.1-45.2 dB |
| 1280x720 | Constrained Baseline / Main / High | 45.2-45.4 dB |
| 1920x1080 | Constrained Baseline / Main / High | 46.1-46.2 dB |
| 1280x720 | Constrained Baseline, CBR 2500 kbps | 2606 kbps, decodes |
| 1920x1080 | High, CBR 4000 kbps | 4089 kbps, decodes |

Run `tests/encode-matrix.sh` to reproduce.

## Update: partial (damage-rectangle) uploads — KasmVNC green screen

Symptom: with KasmVNC the first frame was fully green and only dragged
rectangles showed content. KasmVNC uploads the first frame in full and then
only damaged rectangles with `vaPutImage()`, relying on the surface keeping
the rest of its contents.

Two bugs in the VA backend:

1. `vaPutImage()` returned `VA_STATUS_ERROR_UNIMPLEMENTED` for any rectangle
   that was not the whole image, so damage updates were dropped.
2. The new rectangle copier treated the NV12 chroma row as `width/2` bytes.
   A chroma row is `width` bytes (interleaved U/V, one pair per two luma
   samples); only the *vertical* geometry is halved. The truncated copy left
   the right half of every chroma row zero, which renders exactly as a green
   area while luma looks correct — the KasmVNC symptom.

Both fixed: `vaPutImage()`/`vaGetImage()` accept rectangles (no scaling), and
the chroma plane copies full-width rows. Verified with
`tests/tools/enc_damage_test.c` (full first frame, then 64x64 damage rects for
59 frames): all 60 frames decode at 42-45 dB against the exact frames fed to
the encoder, and the KasmVNC-style partial-update pattern no longer produces
green regions. ffmpeg full-frame paths (Baseline/Main/High) still measure
45+ dB.

## Update: H.264 level / VUI conformance

KasmVNC's web client configures WebCodecs for `avc1.64001F` and decodes the
server's Annex-B stream with a hardware decoder where available. Our SPS
declared **level 4.0 for a 1600x900@60** stream — 1600x900 is 5700 macroblocks
per frame and 60 fps needs 342000 MB/s, while level 4.0 allows only 245760 —
and carried no VUI. Hardware decoders enforce those limits, so they rejected
the stream (green canvas) while file decoders in ffmpeg were lenient.

The SPS writer now:
* computes the smallest conformant level from frame size and frame rate
  (4.2 for 1600x900@60, 4.2 for 1080p60) and only ever raises ffmpeg's hint;
* emits a VUI with timing info (`num_units_in_tick=1`,
  `time_scale=2*fps`, `fixed_frame_rate_flag=1`);
* clears the constraint flags for High profile (Baseline/Main still set
  constraint_set0/1).

Regression: 720p Constrained Baseline/Main/High still 45.4 dB, decode unchanged.

## Update: vaExportSurfaceHandle (DMA-BUF) — Sunshine support

Sunshine's VAAPI encoder failed at start-up with
`Couldn't export va surface handle: operation failed` and fell back to
libx264. It renders the captured frame into a VA surface through EGL and
therefore needs the surface exported as a DMA-BUF
(`vaExportSurfaceHandle(..., DRM_PRIME_2, WRITE_ONLY | SEPARATE_LAYERS)`).

The driver now implements this for encoder surfaces:

* the surface's NV12 storage gets a mirror allocated from the kernel system
  heap (`/dev/dma_heap/system`), mmap'd by the driver and importable by the
  GPU;
* the descriptor exposes one DMA-BUF object, two layers (`DRM_FORMAT_R8` for
  luma, `DRM_FORMAT_GR88` for interleaved chroma, linear modifier), with the
  correct offsets and pitch;
* the DMA-BUF contents are copied back into the encoder storage (with
  `DMA_BUF_IOCTL_SYNC`) before every encode, so GPU rendering into the
  exported buffer reaches the encoder.

`/dev/dma_heap/system` needs to be accessible to the session user; a udev
rule (`/etc/udev/rules.d/60-dma-heap.rules`) sets mode 0666 and
`/etc/modules-load.d/dma-heap.conf` loads `system_heap` at boot.

Verified with `enc_export_test` (write frames only into the exported DMA-BUF,
encode, decode): 30 frames at 42 dB PSNR, and 720p CBR regression unchanged.

## Update: encoder teardown safety

Rapid encoder re-initialisation (KasmVNC changing frame rate/quality, Sunshine
probing encoders) could tear the VA context down while a V4L2 request was
still in flight; the old code returned an error on a request timeout and left
that request queued, so the next re-init freed buffers under a running job.

`vaDestroyContext` now drains a pending request (wait for completion, dequeue
both buffers) before releasing the queues, and a timed-out encode gets one
extra wait so the hantro watchdog can finish before the buffers are touched.
Stale DMA-BUF mirrors are dropped whenever the surface storage is
reallocated, so a resolution change can no longer overrun the new buffer.
