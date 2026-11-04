# VA-API H.264 encode frontend — implementation spec

Goal: extend `/home/durian/workspace/libva-v4l2-request-fourier` with an encoder
that exposes `VAEntrypointEncSlice` for VAProfileH264{Baseline,Main,High} on
`/dev/video1` (hantro/rk3399 VEPU2), mapping VA buffers to the *Bootlin*
V4L2 stateless-encode UAPI that is being backported to 7.3.1:

- `V4L2_CID_STATELESS_H264_ENCODE_PARAMS` (`struct v4l2_ctrl_h264_encode_params`)
- `V4L2_CID_STATELESS_H264_ENCODE_RC` (`struct v4l2_ctrl_h264_encode_rc`)
- `V4L2_CID_STATELESS_H264_ENCODE_FEEDBACK` (read-only, optional)
- OUTPUT queue: raw NV12 (single-plane `V4L2_PIX_FMT_NV12` or 3-plane YUV420M)
- CAPTURE queue: `V4L2_PIX_FMT_H264`; the hardware writes only the slice NAL,
  bytesused is updated by the driver on completion.

## Buffer/flow contract (verified from Bootlin's driver + test tool)

Per frame, userspace must:
1. Fill the OUTPUT buffer with the raw NV12 frame.
2. Queue both OUTPUT and CAPTURE buffers with a fresh media request fd.
3. Set ENCODE_PARAMS on the request:
   - `slice_type` = I for IDR, P otherwise (`V4L2_H264_SLICE_TYPE_I/P`)
   - `frame_num` (mod 1<<(log2_max_frame_num_minus4+4)), `idr_pic_id`
   - `pic_parameter_set_id`, `cabac_init_idc`, `pic_init_qp_minus26`,
     `chroma_qp_index_offset`
   - `flags` = ENTROPY_CODING_MODE | TRANSFORM_8X8_MODE | CONSTRAINED_INTRA_PRED
   - `reference_ts` = timestamp of the previous CAPTURE buffer when P
   - `disable_deblocking_filter_idc`, `slice_alpha_c0_offset_div2`,
     `slice_beta_offset_div2`, `slice_size_mb_rows`
4. Set ENCODE_RC: `qp` (0..51), `qp_min`, `qp_max` (fixed-QP only for now).
5. Queue the request (`MEDIA_REQUEST_IOC_QUEUE`), wait for completion, then
   read the CAPTURE buffer (`bytesused`) — that is the slice NAL.
6. The stream played back is SPS + PPS (built by us) + the slice NAL.

The reference frame is located by the driver with `vb2_find_timestamp()` on the
CAPTURE queue, so CAPTURE buffer timestamps must be unique and stable, and the
reference buffer must still be allocated/known to the driver.

## What the VA frontend must implement

`vaCreateConfig` / `vaQueryConfigProfiles` / `vaQueryConfigEntrypoints` /
`vaGetConfigAttributes`:
- profiles H264 Baseline/Main/High, entrypoint EncSlice, RTFormat YUV420.
- RateControl: only `VA_RC_CQP` (VA_RC_NONE also accepted).
- PackedHeaders: **0** (we generate SPS/PPS ourselves, ffmpeg then does not
  send VAEncPackedHeader* buffers and relies on us).
- EncMaxRefFrames: 1 reference, no B frames (`0x00010001` style value).

`vaCreateContext`: open `/dev/video1`, `S_FMT` OUTPUT NV12 (w,h) and CAPTURE
H264 (w,h), `REQBUFS` (output ~4, capture ~6), `STREAMON` both, allocate a
media request per capture buffer.

`vaCreateBuffer`: store VAEncSequenceParameterBufferH264,
VAEncPictureParameterBufferH264, VAEncSliceParameterBufferH264,
VAEncMiscParameterBuffer (rate control / quality), VAEncCodedBufferType.

`vaBeginPicture`: remember the input surface and the coded buffer.
`vaRenderPicture`: collect the parameter buffers.
`vaEndPicture`:
1. Serialise SPS + PPS from the sequence/picture buffers (bit writer, H.264
   RBSP, Annex-B start codes `00 00 00 01`, NAL types 7 and 8, emulation
   prevention).
2. Copy the input surface (NV12, from its V4L2 capture buffer/dmabuf or an
   internal copy) into the OUTPUT buffer.
3. Set the two controls on the request; queue both buffers; queue the request;
   wait.
4. `DQBUF` capture; append the slice NAL bytes after the SPS/PPS prefix into
   the VA coded buffer; fill `VACodedBufferSegment` (size, status=0, next=NULL)
   so `vaMapBuffer` returns one segment with the full access unit.
5. Mark the surface encoded; `vaSyncSurface` can then return immediately.

`vaMapBuffer`/`vaUnmapBuffer` for VAEncCodedBufferType must expose the
VACodedBufferSegment list (single segment is fine).

## SPS/PPS serialisation

Use `VAEncSequenceParameterBufferH264` + `VAEncPictureParameterBufferH264`:
- SPS: profile_idc, constraint flags, level_idc, seq id, log2_max_frame_num,
  pic_order_cnt_type=2 (fixed, matches the hardware: VEPU2 writes no POC LSB,
  so an SPS with type 0 would desync the decoder), max_num_ref_frames=1,
  gaps flag 0, width/height in MBs, frame_mbs_only=1, direct_8x8=1, cropping
  from the VA picture width/height vs coded size.
- PPS: pps id, sps id, entropy_coding_mode (CABAC), bottom_field_pic_order=0,
  num_slice_groups_minus1=0, num_ref_idx defaults 0, weighted_pred=0,
  weighted_bipred_idc=0, pic_init_qp_minus26, pic_init_qs_minus26=0,
  chroma_qp_index_offset, deblocking_filter_control_present=1,
  constrained_intra_pred, redundant_pic_cnt=0, transform_8x8_mode,
  second_chroma_qp_index_offset, no scaling lists.
- `pic_order_cnt_type = 2` is mandatory (see above).

## Key correctness notes from the reference stack

- `log2_max_frame_num_minus4` must be >= 12-4 in the reference tool (it sets
  12) because the hardware writes a 16-bit frame_num; use 12 to be safe.
- The hardware writes the slice header itself; do not append one.
- The driver needs `slice_size_mb_rows` for slice configuration (set to the
  whole frame for one-slice-per-frame).
- The reference stack uses fixed-QP only; CBR/VBR would require the checkpoint
  / MAD controls (leave unimplemented, report only CQP).

## Validation

```
ffmpeg -vaapi_device /dev/dri/renderD128 -f lavfi -i testsrc2=size=1280x720:rate=30 \
  -t 2 -vf format=nv12,hwupload -c:v h264_vaapi -rc_mode CQP -qp 26 -bf 0 -g 30 out.mp4
ffmpeg -i out.mp4 -f null -            # must decode cleanly
ffmpeg -i out.mp4 -f framemd5 -        # compare with libx264 at same QP (not bit-exact,
                                       # but structure/PSNR should be sane)
```
