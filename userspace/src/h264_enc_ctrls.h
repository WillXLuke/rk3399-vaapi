/*
 * Internal mirror of the V4L2 stateless H.264 *encoder* UAPI.
 *
 * The three controls (and their payload structures) come from the
 * "media: Add V4L2 H.264 stateless encode" series that the RK3399 VEPU2
 * driver in this tree was ported against.  They are present in the
 * kernel headers selected through -Dkernel_headers, but hosts that only
 * ship mainline headers would otherwise fail to compile this driver.
 *
 * The whole block is guarded on the CID macro: a header tree that
 * defines the control also defines the payload structure, so the guard
 * never produces a mixed half-definition.
 */

#ifndef _H264_ENC_CTRLS_H_
#define _H264_ENC_CTRLS_H_

#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#ifndef V4L2_CID_STATELESS_H264_ENCODE_PARAMS

#define V4L2_CID_STATELESS_H264_ENCODE_PARAMS	(V4L2_CID_CODEC_STATELESS_BASE + 8)

struct v4l2_ctrl_h264_encode_params {
	/* Slice parameters */
	__u8 slice_type;
	__u8 pic_parameter_set_id;
	__u16 frame_num;
	__u16 idr_pic_id;
	__u8 cabac_init_idc;
	__u8 disable_deblocking_filter_idc;
	__s8 slice_alpha_c0_offset_div2;
	__s8 slice_beta_offset_div2;

	__s32 slice_size_mb_rows;

	/* PPS parameters */
	__s8 pic_init_qp_minus26;
	__s8 chroma_qp_index_offset;

	__u32 flags;

	/* Reference */
	__u64 reference_ts;
};

#define V4L2_H264_ENCODE_FLAG_ENTROPY_CODING_MODE	0x01
#define V4L2_H264_ENCODE_FLAG_TRANSFORM_8X8_MODE	0x02
#define V4L2_H264_ENCODE_FLAG_CONSTRAINED_INTRA_PRED	0x04

#define V4L2_CID_STATELESS_H264_ENCODE_RC	(V4L2_CID_CODEC_STATELESS_BASE + 9)

struct v4l2_ctrl_h264_encode_rc {
	__u32 qp;
	__u32 qp_min;
	__u32 qp_max;
	__s32 mad_qp_delta;
	__u32 mad_threshold;

	__u32 cp_distance_mbs;
	__u32 cp_target[10];
	__s32 cp_target_error[6];
	__s32 cp_qp_delta[7];
};

#define V4L2_CID_STATELESS_H264_ENCODE_FEEDBACK	(V4L2_CID_CODEC_STATELESS_BASE + 10)

struct v4l2_ctrl_h264_encode_feedback {
	__u32 qp_sum;
	__u32 cp[10];
	__u32 mad_count;
	__u32 rlc_count;
};

#endif /* !V4L2_CID_STATELESS_H264_ENCODE_PARAMS */

#endif /* _H264_ENC_CTRLS_H_ */
