// SPDX-License-Identifier: GPL-2.0
/*
 * Hantro VPU codec driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 */

#include <linux/unaligned.h>
#include <media/v4l2-mem2mem.h>
#include "hantro_jpeg.h"
#include "hantro.h"
#include "hantro_v4l2.h"
#include "hantro_hw.h"
#include "rk3399_vpu_regs.h"

/* H.264 motion estimation parameters */
static const u32 h264_pred_mode_favor[52] = {
	7, 7, 8, 8, 9, 9, 10, 10, 11, 12, 12, 13, 14, 15, 16, 17, 18,
	19, 20, 21, 22, 24, 25, 27, 29, 30, 32, 34, 36, 38, 41, 43, 46,
	49, 51, 55, 58, 61, 65, 69, 73, 78, 82, 87, 93, 98, 104, 110,
	117, 124, 132, 140
};

/* sqrt(2^((qp-12)/3))*8 */
static const u32 h264_diff_mv_penalty[52] = {
	2, 2, 3, 3, 3, 4, 4, 4, 5, 6,
	6, 7, 8, 9, 10, 11, 13, 14, 16, 18,
	20, 23, 26, 29, 32, 36, 40, 45, 51, 57,
	64, 72, 81, 91, 102, 114, 128, 144, 161, 181,
	203, 228, 256, 287, 323, 362, 406, 456, 512, 575,
	645, 724
};

/* 31*sqrt(2^((qp-12)/3))/4 */
static const u32 h264_diff_mv_penalty4p[52] = {
	2, 2, 2, 3, 3, 3, 4, 4, 5, 5,
	6, 7, 8, 9, 10, 11, 12, 14, 16, 17,
	20, 22, 25, 28, 31, 35, 39, 44, 49, 55,
	62, 70, 78, 88, 98, 110, 124, 139, 156, 175,
	197, 221, 248, 278, 312, 351, 394, 442, 496, 557,
	625, 701
};

static const u32 h264_intra16_favor[52] = {
	24, 24, 24, 26, 27, 30, 32, 35, 39, 43, 48, 53, 58, 64, 71, 78,
	85, 93, 102, 111, 121, 131, 142, 154, 167, 180, 195, 211, 229,
	248, 271, 296, 326, 361, 404, 457, 523, 607, 714, 852, 1034,
	1272, 1588, 2008, 2568, 3318, 4323, 5672, 7486, 9928, 13216,
	17648
};

static const u32 h264_inter_favor[52] = {
	40, 40, 41, 42, 43, 44, 45, 48, 51, 53, 55, 60, 62, 67, 69, 72,
	78, 84, 90, 96, 110, 120, 135, 152, 170, 189, 210, 235, 265,
	297, 335, 376, 420, 470, 522, 572, 620, 670, 724, 770, 820,
	867, 915, 970, 1020, 1076, 1132, 1180, 1230, 1275, 1320, 1370
};

static u32 h264_skip_sad_penalty[52] = {
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 233, 205, 182, 163,
	146, 132, 120, 109, 100,  92,  84,  78,  71,  66,  61,  56,  52,  48,
	44,  41,  38,  35,  32,  30,  27,  25,  23,  21,  19,  17,  15,  14,
	12,  11,   9,   8,   7,   5,   4,   3,   2,   1
};

static s32 hantro_h264_enc_exp_golomb_signed(s32 value)
{
	s32 tmp = 0;

	if (value > 0)
		value = 2 * value;
	else
		value = -2 * value + 1;

	while (value >> ++tmp) ;

	return tmp * 2 - 1;
}

static unsigned int rec_luma_size(unsigned int width, unsigned int height)
{
	return round_up(width, MB_DIM) * round_up(height, MB_DIM);
}

static unsigned int rec_image_size(unsigned int width, unsigned int height)
{
	/* Reconstructed image is YUV 4:2:0 with 1.5 bpp. */
	return rec_luma_size(width, height) * 3 / 2;
}

void rk3399_vpu_h264_enc_done(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *dst_buf;
	struct v4l2_ctrl_h264_encode_feedback encode_feedback = { 0 };
	struct v4l2_ctrl *ctrl;
	unsigned int i;
	u32 cp_overflow = 0;
	u32 cp_prev = 0;
	u32 encoded_slices;
	u32 bytesused;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_handler,
			      V4L2_CID_STATELESS_H264_ENCODE_FEEDBACK);
	if (!ctrl)
		return;

	dst_buf = hantro_get_dst_buf(ctx);
	if (!dst_buf)
		return;

	bytesused = vepu_read(vpu, VEPU_REG_STR_BUF_LIMIT) / 8;
	dst_buf->vb2_buf.planes[0].bytesused = bytesused;

	encode_feedback.qp_sum =
		VEPU_REG_QP_SUM(vepu_read(vpu, VEPU_REG_QP_SUM_DIV2));

	encode_feedback.mad_count =
		VEPU_REG_MB_CNT_SET(vepu_read(vpu, VEPU_REG_MB_CTRL));

	encode_feedback.rlc_count =
		VEPU_REG_RLC_SUM_OUT(vepu_read(vpu, VEPU_REG_RLC_SUM));

	for (i = 0; i < ARRAY_SIZE(encode_feedback.cp); i++) {
		/* Each register holds two checkpoint values. */
		u32 cp_index = i / 2;
		u32 cp_reg = VEPU_REG_CHECKPOINT(cp_index);
		u32 cp_read = vepu_read(vpu, cp_reg);
		u32 cp_value = VEPU_REG_CHECKPOINT_RESULT(cp_read, i);

		/* Hardware might overflow, correct it here. */
		if (cp_value < cp_prev)
			cp_overflow += (1 << 21);

		encode_feedback.cp[i] = cp_value + cp_overflow;
		cp_prev = cp_value;
	}

	v4l2_ctrl_s_ctrl_compound(ctrl, V4L2_CTRL_TYPE_H264_ENCODE_FEEDBACK,
				  &encode_feedback);

	encoded_slices = (vepu_read(vpu, VEPU_REG_ENC_CTRL1) >> 16) & 0xff;
}

int rk3399_vpu_h264_enc_run(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct hantro_h264_enc_hw_ctx *h264_ctx = &ctx->h264_enc;
	struct hantro_h264_enc_ctrls *ctrls = &h264_ctx->ctrls;
	const struct v4l2_ctrl_h264_encode_params *encode_params;
	const struct v4l2_ctrl_h264_encode_rc *encode_rc;
	struct v4l2_pix_format_mplane *src_fmt = &ctx->src_fmt;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct hantro_aux_buf *cabac_table = h264_ctx->cabac_table;
	struct hantro_enc_buf *enc_buf;
	struct hantro_aux_buf *rec_buf;
	u32 mbs_in_col, mbs_in_row;
	u32 pred_mode_favor;
	u8 dmv_penalty[128];
	u8 dmv_qpel_penalty[128];
	u32 diff_mv_penalty[3];
	u32 skip_penalty;
	s32 scaler;
	unsigned int i;
	u32 product;
	u32 reg;

	src_buf = hantro_get_src_buf(ctx);
	dst_buf = hantro_get_dst_buf(ctx);

	enc_buf = hantro_get_enc_buf(dst_buf);
	rec_buf = &enc_buf->rec_buf;

	/* Prepare the H264 encoder context. */
	if (hantro_h264_enc_prepare_run(ctx))
		return -EINVAL;

	encode_params = ctrls->encode;
	encode_rc = ctrls->rc;
	mbs_in_row = MB_WIDTH(src_fmt->width);
	mbs_in_col = MB_HEIGHT(src_fmt->height);

	/* Select encoder before writing registers. */

	reg = VEPU_REG_ENCODE_FORMAT_H264;
	vepu_write(vpu, reg, VEPU_REG_ENCODE_START);

	/* AXI bus control */

	reg = VEPU_REG_AXI_CTRL_READ_ID(0) |
	      VEPU_REG_AXI_CTRL_WRITE_ID(0) |
	      VEPU_REG_AXI_CTRL_BURST_LEN(16) |
	      VEPU_REG_AXI_CTRL_INCREMENT_MODE(0) |
	      VEPU_REG_AXI_CTRL_BIRST_DISCARD(0);

	vepu_write(vpu, reg, VEPU_REG_AXI_CTRL);

	/* Endianness */

	reg = VEPU_REG_OUTPUT_SWAP32 |
	      VEPU_REG_OUTPUT_SWAP16 |
	      VEPU_REG_OUTPUT_SWAP8 |
	      VEPU_REG_INPUT_SWAP8 |
	      VEPU_REG_INPUT_SWAP16 |
	      VEPU_REG_INPUT_SWAP32;

	vepu_write(vpu, reg, VEPU_REG_DATA_ENDIAN);

	/* Input */

	reg = VEPU_REG_IN_IMG_CHROMA_OFFSET(0) |
	      VEPU_REG_IN_IMG_LUMA_OFFSET(0) |
	      VEPU_REG_IN_IMG_CTRL_ROW_LEN(mbs_in_row * MB_DIM);

	vepu_write(vpu, reg, VEPU_REG_INPUT_LUMA_INFO);

	reg = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	vepu_write(vpu, reg, VEPU_REG_ADDR_IN_PLANE_0);

	if (src_fmt->num_planes > 1) {
		reg = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 1);
		vepu_write(vpu, reg, VEPU_REG_ADDR_IN_PLANE_1);
	}

	if (src_fmt->num_planes > 2) {
		reg = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 2);
		vepu_write(vpu, reg, VEPU_REG_ADDR_IN_PLANE_2);
	}

	/* Reconstruction */

	reg = rec_buf->dma;
	vepu_write(vpu, reg, VEPU_REG_ADDR_REC_LUMA);

	reg += rec_luma_size(src_fmt->width, src_fmt->height);
	vepu_write(vpu, reg, VEPU_REG_ADDR_REC_CHROMA);

	/* Reference */

	if (encode_params->slice_type == V4L2_H264_SLICE_TYPE_P) {
		struct vb2_queue *queue;
		struct vb2_v4l2_buffer *ref_buf;
		u64 reference_ts;

		struct vb2_buffer *vb;

		queue = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
					V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

		reference_ts = encode_params->reference_ts;
		vb = vb2_find_buffer(queue, reference_ts);
		if (!vb)
			return -EINVAL;

		ref_buf = to_vb2_v4l2_buffer(vb);
		enc_buf = hantro_get_enc_buf(ref_buf);
		rec_buf = &enc_buf->rec_buf;

		reg = rec_buf->dma;
		vepu_write(vpu, reg, VEPU_REG_ADDR_REF_LUMA);

		reg += rec_luma_size(src_fmt->width, src_fmt->height);
		vepu_write(vpu, reg, VEPU_REG_ADDR_REF_CHROMA);
	}

	/* Output */

	vepu_write(vpu, 0, VEPU_REG_STR_HDR_REM_MSB);
	vepu_write(vpu, 0, VEPU_REG_STR_HDR_REM_LSB);

	reg = vb2_plane_size(&dst_buf->vb2_buf, 0);
	vepu_write(vpu, reg, VEPU_REG_STR_BUF_LIMIT);

	reg = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
	vepu_write(vpu, reg, VEPU_REG_ADDR_OUTPUT_STREAM);

	vepu_write(vpu, 0, VEPU_REG_ADDR_OUTPUT_CTRL);

	/* Intra coding */

	reg = VEPU_REG_INTRA_AREA_TOP(mbs_in_col) |
	      VEPU_REG_INTRA_AREA_BOTTOM(mbs_in_col) |
	      VEPU_REG_INTRA_AREA_LEFT(mbs_in_row) |
	      VEPU_REG_INTRA_AREA_RIGHT(mbs_in_row);

	vepu_write(vpu, reg, VEPU_REG_INTRA_AREA_CTRL);

	/* CABAC table */

	if (encode_params->flags & V4L2_H264_ENCODE_FLAG_ENTROPY_CODING_MODE) {
		if (encode_params->cabac_init_idc >= HANTRO_H264_ENC_CABAC_TABLE_COUNT)
			return -EINVAL;

		reg = cabac_table[encode_params->cabac_init_idc].dma;
		vepu_write(vpu, reg, VEPU_REG_ADDR_CABAC_TBL);
	}

	/* Encoding control */

	reg = 0;

	if (mbs_in_row * mbs_in_col > 3600)
		reg |= VEPU_REG_DISABLE_QUARTER_PIXEL_MV;

	if (encode_params->flags & V4L2_H264_ENCODE_FLAG_ENTROPY_CODING_MODE)
		reg |= VEPU_REG_ENTROPY_CODING_MODE |
		       VEPU_REG_CABAC_INIT_IDC(encode_params->cabac_init_idc);

	if (encode_params->flags & V4L2_H264_ENCODE_FLAG_TRANSFORM_8X8_MODE)
		reg |= VEPU_REG_H264_TRANS8X8_MODE;

	reg |= VEPU_REG_H264_SLICE_SIZE(encode_params->slice_size_mb_rows);

	reg |= VEPU_REG_DEBLOCKING_FILTER_MODE(encode_params->disable_deblocking_filter_idc);

	vepu_write(vpu, reg, VEPU_REG_ENC_CTRL0);

	reg = VEPU_REG_MAD_THRESHOLD(encode_rc->mad_threshold) |
	      VEPU_REG_IN_IMG_CTRL_FMT(ctx->vpu_src_fmt->enc_fmt) |
	      VEPU_REG_IN_IMG_ROTATE_MODE(0);

	vepu_write(vpu, reg, VEPU_REG_ENC_CTRL1);

	reg = VEPU_REG_PPS_INIT_QP(encode_params->pic_init_qp_minus26 + 26) |
	      VEPU_REG_SLICE_FILTER_ALPHA(encode_params->slice_alpha_c0_offset_div2 * 2) |
	      VEPU_REG_SLICE_FILTER_BETA(encode_params->slice_beta_offset_div2 * 2) |
	      VEPU_REG_CHROMA_QP_OFFSET(encode_params->chroma_qp_index_offset) |
	      VEPU_REG_IDR_PIC_ID(encode_params->idr_pic_id);

	if (encode_params->flags & V4L2_H264_ENCODE_FLAG_CONSTRAINED_INTRA_PRED)
		reg |= VEPU_REG_CONSTRAINED_INTRA_PREDICTION;

	vepu_write(vpu, reg, VEPU_REG_ENC_CTRL2);

	pred_mode_favor = h264_pred_mode_favor[encode_rc->qp];

	reg = VEPU_REG_PPS_ID(encode_params->pic_parameter_set_id) |
	      VEPU_REG_INTRA_PRED_MODE(pred_mode_favor) |
	      VEPU_REG_FRAME_NUM(encode_params->frame_num);

	vepu_write(vpu, reg, VEPU_REG_ENC_CTRL3);

	scaler = max(1U, 200 / (mbs_in_row + mbs_in_col));
	skip_penalty = min(255U, h264_skip_sad_penalty[encode_rc->qp] * scaler);

	/* Overfill is used to crop the destination. */
	reg = VEPU_REG_STREAM_START_OFFSET(0) |
	      VEPU_REG_SKIP_MACROBLOCK_PENALTY(skip_penalty) |
	      VEPU_REG_IN_IMG_CTRL_OVRFLR_D4(0) |
	      VEPU_REG_IN_IMG_CTRL_OVRFLB(0);

	vepu_write(vpu, reg, VEPU_REG_ENC_OVER_FILL_STRM_OFFSET);

	/* Multi-view control */

	reg = VEPU_REG_ZERO_MV_FAVOR_D2(10);
	vepu_write(vpu, reg, VEPU_REG_MVC_RELATE);

	/* Intra/inter modes */

	reg = VEPU_REG_INTRA16X16_MODE(h264_intra16_favor[encode_rc->qp]) |
	      VEPU_REG_INTER_MODE(h264_inter_favor[encode_rc->qp]);

	vepu_write(vpu, reg, VEPU_REG_INTRA_INTER_MODE);

	/* QP control */

	reg = VEPU_REG_MAD_QP_ADJUSTMENT(encode_rc->mad_qp_delta);
	vepu_write(vpu, reg, VEPU_QP_ADJUST_MAD_DELTA_ROI);

	reg = VEPU_REG_H264_LUMA_INIT_QP(encode_rc->qp) |
	      VEPU_REG_H264_QP_MAX(encode_rc->qp_max) |
	      VEPU_REG_H264_QP_MIN(encode_rc->qp_min) |
	      VEPU_REG_H264_CHKPT_DISTANCE(encode_rc->cp_distance_mbs);

	vepu_write(vpu, reg, VEPU_REG_QP_VAL);

	for (i = 0; i < ARRAY_SIZE(encode_rc->cp_target); i += 2) {
		u32 cp_index = i / 2;
		u32 cp_reg = VEPU_REG_CHECKPOINT(cp_index);

		/* Checkpoint target i is stored in the upper bits (CHECK1). */
		reg = VEPU_REG_CHECKPOINT_CHECK0(encode_rc->cp_target[i + 1]) |
		      VEPU_REG_CHECKPOINT_CHECK1(encode_rc->cp_target[i]);

		vepu_write(vpu, reg, cp_reg);
	}

	for (i = 0; i < ARRAY_SIZE(encode_rc->cp_target_error); i += 2) {
		u32 error_index = i / 2;
		u32 error_reg = VEPU_REG_CHKPT_WORD_ERR(error_index);

		/* Checkpoint error i is stored in the upper bits (CHK1). */
		reg = VEPU_REG_CHKPT_WORD_ERR_CHK0(encode_rc->cp_target_error[i + 1]) |
		      VEPU_REG_CHKPT_WORD_ERR_CHK1(encode_rc->cp_target_error[i]);

		vepu_write(vpu, reg, error_reg);
	}

	reg = VEPU_REG_CHKPT_DELTA_QP_CHK0(encode_rc->cp_qp_delta[0]) |
	      VEPU_REG_CHKPT_DELTA_QP_CHK1(encode_rc->cp_qp_delta[1]) |
	      VEPU_REG_CHKPT_DELTA_QP_CHK2(encode_rc->cp_qp_delta[2]) |
	      VEPU_REG_CHKPT_DELTA_QP_CHK3(encode_rc->cp_qp_delta[3]) |
	      VEPU_REG_CHKPT_DELTA_QP_CHK4(encode_rc->cp_qp_delta[4]) |
	      VEPU_REG_CHKPT_DELTA_QP_CHK5(encode_rc->cp_qp_delta[5]) |
	      VEPU_REG_CHKPT_DELTA_QP_CHK6(encode_rc->cp_qp_delta[6]);

	vepu_write(vpu, reg, VEPU_REG_CHKPT_DELTA_QP);

	/* Regions of interest */

	vepu_write(vpu, 0, VEPU_REG_ROI1);
	vepu_write(vpu, 0, VEPU_REG_ROI2);

	/* Motion-vector penalty */

	diff_mv_penalty[0] = h264_diff_mv_penalty4p[encode_rc->qp];
	diff_mv_penalty[1] = h264_diff_mv_penalty[encode_rc->qp];
	diff_mv_penalty[2] = h264_diff_mv_penalty[encode_rc->qp];

	reg = VEPU_REG_4MV_PENALTY(diff_mv_penalty[0]) |
	      VEPU_REG_1MV_PENALTY(diff_mv_penalty[1]) |
	      VEPU_REG_QMV_PENALTY(diff_mv_penalty[2]) |
	      VEPU_REG_SPLIT_MV_MODE_EN;

	vepu_write(vpu, reg, VEPU_REG_MV_PENALTY);

	for (i = 0; i < ARRAY_SIZE(dmv_penalty); i++) {
		dmv_penalty[i] = i;
		dmv_qpel_penalty[i] = min(255, hantro_h264_enc_exp_golomb_signed(i));
	}

	for (i = 0; i < ARRAY_SIZE(dmv_penalty); i += 4) {
		u32 penalty_index = i / 4;
		u32 penalty_reg;

		penalty_reg = VEPU_REG_DMV_PENALTY_TBL(penalty_index);
		reg = VEPU_REG_DMV_PENALTY_TABLE_BIT(dmv_penalty[i], 3) |
		      VEPU_REG_DMV_PENALTY_TABLE_BIT(dmv_penalty[i + 1], 2) |
		      VEPU_REG_DMV_PENALTY_TABLE_BIT(dmv_penalty[i + 2], 1) |
		      VEPU_REG_DMV_PENALTY_TABLE_BIT(dmv_penalty[i + 3], 0);

		vepu_write(vpu, reg, penalty_reg);

		penalty_reg = VEPU_REG_DMV_Q_PIXEL_PENALTY_TBL(penalty_index);
		reg = VEPU_REG_DMV_Q_PIXEL_PENALTY_TABLE_BIT(dmv_qpel_penalty[i], 3) |
		      VEPU_REG_DMV_Q_PIXEL_PENALTY_TABLE_BIT(dmv_qpel_penalty[i + 1], 2) |
		      VEPU_REG_DMV_Q_PIXEL_PENALTY_TABLE_BIT(dmv_qpel_penalty[i + 2], 1) |
		      VEPU_REG_DMV_Q_PIXEL_PENALTY_TABLE_BIT(dmv_qpel_penalty[i + 3], 0);

		vepu_write(vpu, reg, penalty_reg);
	}

	/* Unused extra features */

	vepu_write(vpu, 0, VEPU_REG_ADDR_NEXT_PIC);
	vepu_write(vpu, 0, VEPU_REG_ADDR_MV_OUT);
	vepu_write(vpu, 0, VEPU_REG_STABILIZATION_OUTPUT);

	/* CSC */

	vepu_write(vpu, 0, VEPU_REG_RGB2YUV_CONVERSION_COEF1);
	vepu_write(vpu, 0, VEPU_REG_RGB2YUV_CONVERSION_COEF2);
	vepu_write(vpu, 0, VEPU_REG_RGB2YUV_CONVERSION_COEF3);
	vepu_write(vpu, 0, VEPU_REG_RGB_MASK_MSB);

	/* Interrupt */

	reg = VEPU_REG_INTERRUPT_TIMEOUT_EN;
	vepu_write(vpu, reg, VEPU_REG_INTERRUPT);

	/* Start the hardware. */

	hantro_watchdog_kick(ctx);

	reg = VEPU_REG_MB_HEIGHT(mbs_in_col) |
	      VEPU_REG_MB_WIDTH(mbs_in_row) |
	      VEPU_REG_ENCODE_FORMAT_H264 |
	      VEPU_REG_ENCODE_ENABLE;

	if (encode_params->slice_type == V4L2_H264_SLICE_TYPE_I) {
		reg |= VEPU_REG_FRAME_TYPE_INTRA;
		/* FIXME: Keyframes are IDR frames, not any I frame. */
		dst_buf->flags |= V4L2_BUF_FLAG_KEYFRAME;
	} else {
		reg |= VEPU_REG_FRAME_TYPE_INTER;
	}

	vepu_write(vpu, reg, VEPU_REG_ENCODE_START);

	return 0;
}
