/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Hantro/Rockchip VPU2 H.264 encoder parameters.
 *
 * Passed from userspace through one private array control.  Keep this
 * layout in sync with the VA-API driver.
 */
#ifndef __HANTRO_H264_ENCODE_H__
#define __HANTRO_H264_ENCODE_H__

#include <cstdint>
using __u8 = uint8_t; using __u16 = uint16_t; using __u32 = uint32_t; using __s32 = int32_t;

#define V4L2_CID_USER_HANTRO_BASE	(V4L2_CID_USER_BASE + 0x2000)
#define V4L2_CID_HANTRO_H264_ENCODE_PARAMS \
	(V4L2_CID_USER_HANTRO_BASE + 0)

struct hantro_h264_enc_reg_params {
	__u32 frame_coding_type;
	__s32 pic_init_qp;
	__s32 slice_alpha_offset;
	__s32 slice_beta_offset;
	__s32 chroma_qp_index_offset;
	__s32 filter_disable;
	__u16 idr_pic_id;
	__s32 pps_id;
	__s32 frame_num;
	__s32 slice_size_mb_rows;
	__s32 h264_inter4x4_disabled;
	__s32 enable_cabac;
	__s32 transform8x8_mode;
	__s32 cabac_init_idc;

	/* Rate control */
	__s32 qp;
	__s32 mad_qp_delta;
	__s32 mad_threshold;
	__s32 qp_min;
	__s32 qp_max;
	__s32 cp_distance_mbs;
	__s32 cp_target[10];
	__s32 target_error[7];
	__s32 delta_qp[7];
} __attribute__((packed));

#endif /* __HANTRO_H264_ENCODE_H__ */
