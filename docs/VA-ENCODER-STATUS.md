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
