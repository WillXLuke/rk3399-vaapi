/*
 * Copyright (C) 2026 Markus Fritsche <fritsche.markus@gmail.com>
 *
 * fresnel-fourier iter3 Phase 6 commit B: VP8 codec dispatcher
 * implemented against V4L2_CID_STATELESS_VP8_FRAME (kernel UAPI
 * <linux/v4l2-controls.h>:1900-1958). Single batched control per
 * frame, no init-time device-wide menus (VP8 has no DECODE_MODE/
 * START_CODE — confirmed by Phase 0 V4L2 inventory + Phase 3
 * cross-validator strace).
 *
 * Reference: FFmpeg libavcodec/v4l2_request_vp8.c (kwiboo branch);
 *            FFmpeg libavcodec/vaapi_vp8.c (VAAPI source-side
 *            verification of the field semantics);
 *            kernel drivers/media/platform/verisilicon/
 *              hantro_g1_vp8_dec.c (RK3399 hardware reads
 *              first_part_header_bits + first_part_size to compute
 *              MB-data DMA offset).
 *
 * Phase 5 review amendments incorporated (see phase5_iter3_review.md):
 *   C1 first_part_header_bits = slice->macroblock_offset
 *      (NOT 0; kernel reads it unconditionally; same formula as
 *      v4l2_request_vp8.c uses internally)
 *   C2 first_part_size = slice->partition_size[0] +
 *                        ((macroblock_offset + 7) / 8)
 *      (recover total partition size from VAAPI's post-parse
 *      remainder)
 *   C3 VAProbabilityBufferType (not VAProbabilityDataBufferType)
 *   C4 (int8_t) cast (not (s8); kernel-internal typedef not in
 *      userspace UAPI)
 *   S3 assert(probability_set) runtime guard (kernel has NO
 *      coeff_probs default fallback; consumer MUST send
 *      VAProbabilityBufferType per frame)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "vp8.h"
#include "context.h"
#include "request.h"
#include "surface.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <sys/ioctl.h>

#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#include "v4l2.h"

int vp8_set_controls(struct request_data *driver_data,
		     struct object_context *context_object,
		     struct object_surface *surface_object)
{
	VAPictureParameterBufferVP8 *picture =
		&surface_object->params.vp8.picture;
	VASliceParameterBufferVP8 *slice =
		&surface_object->params.vp8.slice;
	VAIQMatrixBufferVP8 *iqmatrix =
		&surface_object->params.vp8.iqmatrix;
	VAProbabilityDataBufferVP8 *probability =
		&surface_object->params.vp8.probability;
	bool iqmatrix_set = surface_object->params.vp8.iqmatrix_set;
	bool probability_set = surface_object->params.vp8.probability_set;

	struct v4l2_ctrl_vp8_frame frame;
	struct object_surface *last_ref;
	struct object_surface *golden_ref;
	struct object_surface *alt_ref;
	int rc;
	int i, j;

	memset(&frame, 0, sizeof frame);

	/* Phase 5 S3: kernel has no coeff_probs default fallback. The
	 * VAAPI consumer chain (FFmpeg's vaapi_vp8.c:146-148, used by
	 * mpv and ffmpeg-vaapi) always sends VAProbabilityBufferType
	 * per frame. Surface immediately if a future consumer doesn't. */
	assert(probability_set);

	/* Clause 3: frame geometry + per-frame scalars */
	frame.width = picture->frame_width;
	frame.height = picture->frame_height;
	frame.horizontal_scale = 0;  /* not exposed by VAAPI */
	frame.vertical_scale = 0;
	frame.version = picture->pic_fields.bits.version;
	frame.prob_skip_false = picture->prob_skip_false;
	frame.prob_intra = picture->prob_intra;
	frame.prob_last = picture->prob_last;
	frame.prob_gf = picture->prob_gf;
	/* Phase 3 Q2: VAAPI counts include control partition;
	 * kernel counts DCT only — off-by-one. */
	frame.num_dct_parts = slice->num_of_partitions - 1;

	/* Clause 4: DPB timestamp resolution (mirrors mpeg2.c pattern;
	 * NULL surface → timestamp stays 0 from memset). */
	last_ref = SURFACE(driver_data, picture->last_ref_frame);
	golden_ref = SURFACE(driver_data, picture->golden_ref_frame);
	alt_ref = SURFACE(driver_data, picture->alt_ref_frame);
	if (last_ref != NULL)
		frame.last_frame_ts =
			v4l2_timeval_to_ns(&last_ref->timestamp);
	if (golden_ref != NULL)
		frame.golden_frame_ts =
			v4l2_timeval_to_ns(&golden_ref->timestamp);
	if (alt_ref != NULL)
		frame.alt_frame_ts =
			v4l2_timeval_to_ns(&alt_ref->timestamp);

	/* Clause 5: loop filter mapping */
	for (i = 0; i < 4; i++) {
		frame.lf.ref_frm_delta[i] =
			picture->loop_filter_deltas_ref_frame[i];
		frame.lf.mb_mode_delta[i] =
			picture->loop_filter_deltas_mode[i];
	}
	frame.lf.sharpness_level = picture->pic_fields.bits.sharpness_level;
	frame.lf.level = picture->loop_filter_level[0];
	if (picture->pic_fields.bits.loop_filter_adj_enable)
		frame.lf.flags |= V4L2_VP8_LF_ADJ_ENABLE;
	if (picture->pic_fields.bits.mode_ref_lf_delta_update)
		frame.lf.flags |= V4L2_VP8_LF_DELTA_UPDATE;
	if (picture->pic_fields.bits.filter_type)
		frame.lf.flags |= V4L2_VP8_LF_FILTER_TYPE_SIMPLE;

	/* Clause 6: quantization base + delta derivation */
	if (iqmatrix_set) {
		frame.quant.y_ac_qi =
			iqmatrix->quantization_index[0][0];
		frame.quant.y_dc_delta = (int8_t)
			(iqmatrix->quantization_index[0][1] -
			 iqmatrix->quantization_index[0][0]);
		frame.quant.y2_dc_delta = (int8_t)
			(iqmatrix->quantization_index[0][2] -
			 iqmatrix->quantization_index[0][0]);
		frame.quant.y2_ac_delta = (int8_t)
			(iqmatrix->quantization_index[0][3] -
			 iqmatrix->quantization_index[0][0]);
		frame.quant.uv_dc_delta = (int8_t)
			(iqmatrix->quantization_index[0][4] -
			 iqmatrix->quantization_index[0][0]);
		frame.quant.uv_ac_delta = (int8_t)
			(iqmatrix->quantization_index[0][5] -
			 iqmatrix->quantization_index[0][0]);
	}

	if (picture->pic_fields.bits.segmentation_enabled && iqmatrix_set) {
		for (i = 1; i < 4; i++)
			frame.segment.quant_update[i] = (int8_t)
				(iqmatrix->quantization_index[i][0] -
				 iqmatrix->quantization_index[0][0]);
	}

	/* Clause 7: segment fields */
	for (i = 0; i < 3; i++)
		frame.segment.segment_probs[i] =
			picture->mb_segment_tree_probs[i];
	if (picture->pic_fields.bits.segmentation_enabled)
		frame.segment.flags |= V4L2_VP8_SEGMENT_FLAG_ENABLED;
	if (picture->pic_fields.bits.update_mb_segmentation_map)
		frame.segment.flags |= V4L2_VP8_SEGMENT_FLAG_UPDATE_MAP;
	if (picture->pic_fields.bits.update_segment_feature_data)
		frame.segment.flags |=
			V4L2_VP8_SEGMENT_FLAG_UPDATE_FEATURE_DATA;
	/* DELTA_VALUE_MODE: VAAPI doesn't expose abs_delta. FFmpeg sets
	 * unconditionally per !s->segmentation.absolute_vals (default).
	 * Kernel ignores when ENABLED bit clear (BBB case). */
	frame.segment.flags |= V4L2_VP8_SEGMENT_FLAG_DELTA_VALUE_MODE;

	if (picture->pic_fields.bits.segmentation_enabled) {
		for (i = 0; i < 4; i++)
			frame.segment.lf_update[i] = (int8_t)
				(picture->loop_filter_level[i] -
				 picture->loop_filter_level[0]);
	}

	/* Clause 8: entropy table mapping (3 VAAPI sources merged) */
	for (i = 0; i < 4; i++)
		frame.entropy.y_mode_probs[i] = picture->y_mode_probs[i];
	for (i = 0; i < 3; i++)
		frame.entropy.uv_mode_probs[i] = picture->uv_mode_probs[i];
	for (i = 0; i < 2; i++)
		for (j = 0; j < 19; j++)
			frame.entropy.mv_probs[i][j] =
				picture->mv_probs[i][j];
	/* coeff_probs[4][8][3][11]: VAAPI layout matches kernel exactly;
	 * direct memcpy. Both vaapi_vp8.c:133-143 and v4l2_request_vp8.c:
	 * 141-153 apply identical coeff_bands_inverse reordering before
	 * writing — VAAPI consumer has done the reordering for us. */
	memcpy(frame.entropy.coeff_probs,
	       probability->dct_coeff_probs,
	       sizeof frame.entropy.coeff_probs);

	/* Clause 9: coder state + first-partition fields */
	frame.coder_state.range = picture->bool_coder_ctx.range;
	frame.coder_state.value = picture->bool_coder_ctx.value;
	frame.coder_state.bit_count = picture->bool_coder_ctx.count;

	/* Phase 5 C1+C2: macroblock_offset IS first_part_header_bits by
	 * source identity; kernel hantro_g1_vp8_dec.c:260 reads it
	 * unconditionally to compute MB-data DMA offset. partition_size[0]
	 * is the post-parse REMAINDER; recover total via
	 * + ceil(macroblock_offset/8). */
	frame.first_part_header_bits = slice->macroblock_offset;
	frame.first_part_size =
		slice->partition_size[0] +
		((uint32_t)slice->macroblock_offset + 7) / 8;

	for (i = 0; i < 8; i++)
		frame.dct_part_sizes[i] = slice->partition_size[i + 1];

	/* Clause 9: flags assembly (6 mainline-documented bits only;
	 * EXPERIMENTAL + bit 0x40 NOT replicated despite ffmpeg-v4l2-
	 * request-git setting them — kernel hantro_vp8.c only inspects
	 * KEY_FRAME bit). VAAPI inverts: key_frame=0 means it IS a
	 * keyframe per VP8 spec. */
	if (!picture->pic_fields.bits.key_frame)
		frame.flags |= V4L2_VP8_FRAME_FLAG_KEY_FRAME;
	frame.flags |= V4L2_VP8_FRAME_FLAG_SHOW_FRAME;
	if (picture->pic_fields.bits.mb_no_coeff_skip)
		frame.flags |= V4L2_VP8_FRAME_FLAG_MB_NO_SKIP_COEFF;
	if (picture->pic_fields.bits.sign_bias_golden)
		frame.flags |= V4L2_VP8_FRAME_FLAG_SIGN_BIAS_GOLDEN;
	if (picture->pic_fields.bits.sign_bias_alternate)
		frame.flags |= V4L2_VP8_FRAME_FLAG_SIGN_BIAS_ALT;

	/* Clause 1+10: single-control batched submission */
	struct v4l2_ext_control ctrls[1] = {
		{
			.id = V4L2_CID_STATELESS_VP8_FRAME,
			.ptr = &frame,
			.size = sizeof frame,
		},
	};

	rc = v4l2_set_controls(driver_data->video_fd,
			       surface_object->request_fd,
			       ctrls, 1);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	return 0;
}
