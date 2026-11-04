# VA-API H.264 encode frontend — VERIFIED kernel contract

The kernel side now works (see HANDOFF.md). Implement the VA-API encoder in
`/home/durian/workspace/libva-v4l2-request-fourier` against this exact contract.

## Device

Encoder node: `/dev/video0` (auto-detect: `VIDIOC_QUERYCAP` driver `hantro-vpu`,
card `rockchip,rk3399-vpu-enc`; CAPTURE formats include `V4L2_PIX_FMT_H264_SLICE`
('S264')). It coexists with the rkvdec decode node used by the existing decoder.
Note node numbers can change between module reloads — always auto-detect.

## Formats

- OUTPUT (raw): `V4L2_PIX_FMT_NV12M` (2 planes) or `V4L2_PIX_FMT_YUV420M`
  (3 planes). The Bootlin tool uses NV12M; use that.
- CAPTURE (encoded): `V4L2_PIX_FMT_H264_SLICE`. On completion the driver sets
  `planes[0].bytesused` to the number of bytes the hardware wrote (the slice
  NAL, including its start code), via `codec_ops->done`.

## Controls (per media request, set with V4L2_CTRL_WHICH_REQUEST_VAL)

- `V4L2_CID_STATELESS_H264_ENCODE_PARAMS` (32 bytes):
  slice_type, pic_parameter_set_id, frame_num, idr_pic_id, cabac_init_idc,
  disable_deblocking_filter_idc, slice_alpha_c0_offset_div2,
  slice_beta_offset_div2, slice_size_mb_rows, pic_init_qp_minus26,
  chroma_qp_index_offset, flags, reference_ts.
- `V4L2_CID_STATELESS_H264_ENCODE_RC` (116 bytes): qp, qp_min, qp_max,
  mad_qp_delta, mad_threshold, cp_* fields.
- `V4L2_CID_STATELESS_H264_ENCODE_FEEDBACK` (read-only, completed request).

`reference_ts` for a P frame is the timestamp of the *previous CAPTURE buffer*
(the driver resolves it with `vb2_find_buffer()` on the capture queue and uses
that buffer's reconstruction area). Use `V4L2_BUF_FLAG_TIMESTAMP_COPY`
timestamps; keep them unique.

## Buffer flow per frame (proven by the Bootlin tool)

1. Fill OUTPUT buffer (NV12M) with the frame.
2. QBUF OUTPUT with `V4L2_BUF_FLAG_REQUEST_FD` + request fd.
3. QBUF CAPTURE (no request fd).
4. `VIDIOC_S_EXT_CTRLS` with the params + rc controls on the request.
5. `MEDIA_REQUEST_IOC_QUEUE`, poll the request for completion.
6. DQBUF OUTPUT, DQBUF CAPTURE; `bytesused` = slice NAL length.
7. `MEDIA_REQUEST_IOC_REINIT` before reusing the request.

SPS and PPS are **not** produced by the hardware: userspace writes them into
the stream itself. For VA-API, prepend SPS+PPS (Annex-B, NAL 7/8, with
emulation-prevention) to the slice NAL when filling the coded buffer.
SPS must use `pic_order_cnt_type = 2` and `log2_max_frame_num_minus4 = 12`
(the hardware writes a 16-bit frame number and no POC LSB).

## Known-good reference numbers

Bootlin tool, 640x480, 10 frames: `capture.h264` = 24,360 bytes, starts
`00 00 00 01 67 64 c0 1f ... 00 00 00 01 68 ee ... 00 00 00 01 25 b8 ...`,
decodes with `ffmpeg -i capture.h264 -f null -` to 10 frames, High profile L3.1.

## Acceptance test for the VA frontend

```
ffmpeg -hide_banner -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc2=size=640x480:rate=25 -t 1 \
  -vf format=nv12,hwupload -c:v h264_vaapi -qp 26 -bf 0 out.h264
ffmpeg -i out.h264 -f null -     # must decode cleanly
```
`vainfo` must show `VAProfileH264High : VAEntrypointEncSlice` in addition to
the existing VLD entries, and the existing decode regression must still pass:
`ffmpeg -hwaccel vaapi ... -i /home/durian/workspace/va-test/h_keyint60.mp4 -f null -`.
