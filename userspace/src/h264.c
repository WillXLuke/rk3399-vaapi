/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
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
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

#include "request.h"
#include "utils.h"
#include "surface.h"
#include "v4l2.h"
#include "h264_slice_header.h"

enum h264_slice_type {
	H264_SLICE_P    = 0,
	H264_SLICE_B    = 1,
};

static bool is_picture_null(VAPictureH264 *pic)
{
	return pic->picture_id == VA_INVALID_SURFACE;
}

static struct h264_dpb_entry *
dpb_find_invalid_entry(struct object_context *context)
{
	unsigned int i;

	for (i = 0; i < H264_DPB_SIZE; i++) {
		struct h264_dpb_entry *entry = &context->dpb.entries[i];

		if (!entry->valid && !entry->reserved)
			return entry;
	}

	return NULL;
}

static struct h264_dpb_entry *
dpb_find_oldest_unused_entry(struct object_context *context)
{
	unsigned int min_age = UINT_MAX;
	unsigned int i;
	struct h264_dpb_entry *match = NULL;

	for (i = 0; i < H264_DPB_SIZE; i++) {
		struct h264_dpb_entry *entry = &context->dpb.entries[i];

		if (!entry->used && (entry->age < min_age)) {
			min_age = entry->age;
			match = entry;
		}
	}

	return match;
}

static struct h264_dpb_entry *dpb_find_entry(struct object_context *context)
{
	struct h264_dpb_entry *entry;

	entry = dpb_find_invalid_entry(context);
	if (!entry)
		entry = dpb_find_oldest_unused_entry(context);

	return entry;
}

static struct h264_dpb_entry *dpb_lookup(struct object_context *context,
					 VAPictureH264 *pic, unsigned int *idx,
					 unsigned char *fields)
{
	unsigned int i;

	for (i = 0; i < H264_DPB_SIZE; i++) {
		struct h264_dpb_entry *entry = &context->dpb.entries[i];

		if (!entry->valid)
			continue;

		if (entry->pic.picture_id == pic->picture_id) {
			if (idx)
				*idx = i;

			if (fields) {
				//if (entry->pic.TopFieldOrderCnt < entry->pic.BottomFieldOrderCnt) {
				//	*fields = V4L2_H264_TOP_FIELD_REF;
				//} else if (entry->pic.TopFieldOrderCnt > entry->pic.BottomFieldOrderCnt) {
				//	*fields = V4L2_H264_BOTTOM_FIELD_REF;
				//} else {
					*fields = V4L2_H264_FRAME_REF;
				//}
			}

			return entry;
		}
	}

	return NULL;
}

static void dpb_clear_entry(struct h264_dpb_entry *entry, bool reserved)
{
	memset(entry, 0, sizeof(*entry));

	if (reserved)
		entry->reserved = true;
}

static void dpb_insert(struct object_context *context, VAPictureH264 *pic,
		       struct h264_dpb_entry *entry)
{
	if (is_picture_null(pic))
		return;

	if (dpb_lookup(context, pic, NULL, NULL))
		return;

	if (!entry)
		entry = dpb_find_entry(context);

	memcpy(&entry->pic, pic, sizeof(entry->pic));
	entry->age = context->dpb.age;
	entry->valid = true;
	entry->reserved = false;

	if (!(pic->flags & VA_PICTURE_H264_INVALID))
		entry->used = true;
}

static void dpb_update(struct object_context *context,
		       VAPictureParameterBufferH264 *parameters)
{
	unsigned int i;

	context->dpb.age++;

	for (i = 0; i < H264_DPB_SIZE; i++) {
		struct h264_dpb_entry *entry = &context->dpb.entries[i];

		entry->used = false;
	}

	for (i = 0; i < parameters->num_ref_frames; i++) {
		VAPictureH264 *pic = &parameters->ReferenceFrames[i];
		struct h264_dpb_entry *entry;

		if (is_picture_null(pic))
			continue;

		entry = dpb_lookup(context, pic, NULL, NULL);
		if (entry) {
			/*
			 * The consumer reuses VA surface ids.  When a surface
			 * that was decoded before an IDR is recycled, its DPB
			 * entry still carries the pre-IDR frame_num / POC and
			 * would poison the kernel's reference list builder.
			 * The current VAPictureParameterBufferH264 (and its
			 * ReferenceFrames[]) is authoritative, so refresh the
			 * stored picture fields on every match.
			 */
			memcpy(&entry->pic, pic, sizeof(entry->pic));
			entry->age = context->dpb.age;
			entry->used = true;
		} else {
			dpb_insert(context, pic, NULL);
		}
	}
}

/*
 * Strip ffmpeg-vaapi's POC sentinel.
 *
 * ffmpeg's H264POCContext initialises prev_poc_msb to (1 << 16) =
 * 0x10000 in libavcodec/h264dec.c (lines 301 and 444 of v8.0). After
 * an IDR the idr() helper resets prev_poc_msb to that same sentinel.
 * ff_h264_init_poc (libavcodec/h264_parse.c lines 296-305) then
 * computes pc->poc_msb as prev_poc_msb when the slice header's
 * poc_lsb hasn't wrapped — which is the typical case for normal
 * content. The sentinel leaks into field_poc[] and from there into
 * VAPictureH264.TopFieldOrderCnt / BottomFieldOrderCnt at
 * libavcodec/vaapi_h264.c::fill_vaapi_pic.
 *
 * Working VAAPI backends (intel-iHD, i965 verified empirically on
 * meitner 2026-05-02) tolerate the high word — they either mask it
 * or treat POCs as relative comparisons. V4L2 stateless H.264
 * driver-side consumers (hantro_h264.c::prepare_table feeds the
 * value direct to tbl->poc[]) need the spec value, so we strip the
 * sentinel here at the libva-v4l2-request boundary.
 *
 * Detection by bit-16-set rather than blind subtraction so that a
 * future ffmpeg version that fixes the sentinel leak degrades
 * gracefully. POC values for non-degenerate H.264 content rarely
 * exceed 16 bits; bit 16 set is a strong signal of the sentinel.
 *
 * Empty DPB slots (VA_PICTURE_H264_INVALID) carry POC=0 by
 * libavcodec/vaapi_h264.c::init_vaapi_pic and need no fix-up.
 */
static inline int32_t h264_strip_ffmpeg_poc_sentinel(int32_t poc, uint32_t flags)
{
	if (flags & VA_PICTURE_H264_INVALID)
		return 0;
	/*
	 * iter8 α-2: pass POC values through unchanged for rkvdec. The
	 * sentinel-subtract was added for hantro's tbl->poc[] prepare_table
	 * which fed the value through unmasked. rkvdec writes POC to MMIO
	 * via writel_relaxed (rkvdec-h264.c:975-978) and the macro
	 * RKVDEC_CUR_POC is a 32-bit passthrough. kdirect (ffmpeg-v4l2request)
	 * delivers the sentinel-encoded value directly and decodes
	 * correctly; libva's strip was the cause of the 16x32 partial-fill
	 * Bug 4 symptom. Hantro+H.264 isn't exercised on RK3399 (hantro-dec
	 * doesn't advertise H.264 there) — restoring the strip per-driver
	 * is iter9 work if it ever surfaces.
	 */
	return poc;
}

static void h264_fill_dpb(struct request_data *data,
			  struct object_context *context,
			  VAPictureParameterBufferH264 *VAPicture,
			  struct v4l2_ctrl_h264_decode_params *decode)
{
	const int max_frame_num =
		1 << (VAPicture->seq_fields.bits.log2_max_frame_num_minus4 + 4);
	const int cur_frame_num = (int)VAPicture->frame_num;
	int i;

	for (i = 0; i < H264_DPB_SIZE; i++) {
		struct v4l2_h264_dpb_entry *dpb = &decode->dpb[i];
		struct h264_dpb_entry *entry = &context->dpb.entries[i];
		struct object_surface *surface =
			SURFACE(data, entry->pic.picture_id);
		uint64_t timestamp;

		/*
		 * Skip entries no longer referenced by the consumer's
		 * VAPictureParameterBufferH264.ReferenceFrames[]. dpb_update()
		 * clears `used` for all entries then re-marks only those in the
		 * current ReferenceFrames list; entries with valid=true but
		 * used=false are stale (a frame the libva consumer has retired
		 * from its DPB).
		 *
		 * Without this skip, our V4L2 dpb[] grows monotonically until
		 * H264_DPB_SIZE; by frame_num=10 it carries 7+ entries while
		 * SPS.max_num_ref_frames may be 4. The kernel reflist builder /
		 * cluster validator rejects the request with EINVAL once the
		 * count exceeds the SPS contract — which iter1+iter2+iter3
		 * surfaced as the "frame-11 EINVAL" carryover. iter4 fix:
		 * report only currently-used entries to match FFmpeg's
		 * libavcodec/v4l2_request_h264.c::fill_dpb behaviour (which
		 * iterates h->short_ref[] / h->long_ref[] — exactly the
		 * currently-referenced set).
		 */
		if (!entry->valid || !entry->used)
			continue;

		if (surface) {
			timestamp = v4l2_timeval_to_ns(&surface->timestamp);
			dpb->reference_ts = timestamp;
		}

		dpb->frame_num = entry->pic.frame_idx;

		/*
		 * Per ext-ctrls-codec-stateless.rst, dpb[].pic_num must
		 * equal the H.264 spec's PicNum (8-28) for short-term refs
		 * or LongTermPicNum (8-29) for long-term refs.
		 *
		 * For frames (not field-coded), PicNum = FrameNumWrap.
		 * FrameNumWrap = (frame_num > cur_frame_num)
		 *                ? frame_num - max_frame_num
		 *                : frame_num
		 * (per spec section 8.2.4.1, frame_num wraparound).
		 *
		 * VAAPI convention (libavcodec/vaapi_h264.c::fill_vaapi_pic
		 * line 64): VAPictureH264.frame_idx holds long_term_frame_idx
		 * for long-term refs and frame_num for short-term refs. So
		 * for long-term entries we copy frame_idx straight through
		 * as LongTermPicNum.
		 *
		 * fourier's previous code set pic_num to picture_id (the
		 * VAAPI surface id) which is unrelated to H.264 PicNum;
		 * mediatek's vdec_h264_req_common.c::dst_entry->pic_num is
		 * one consumer that fails on that. Hantro doesn't read
		 * pic_num at all (uses reference_ts for ref resolution),
		 * which is why fourier's wrong value never surfaced on
		 * PineTab2 (RK3566 via hantro/rk3568-vpu).
		 */
		if (entry->pic.flags & VA_PICTURE_H264_LONG_TERM_REFERENCE) {
			dpb->pic_num = entry->pic.frame_idx;
		} else {
			int frame_num = (int)entry->pic.frame_idx;
			dpb->pic_num = (frame_num > cur_frame_num)
				? frame_num - max_frame_num
				: frame_num;
		}

		dpb->top_field_order_cnt =
			h264_strip_ffmpeg_poc_sentinel(entry->pic.TopFieldOrderCnt,
						       entry->pic.flags);
		dpb->bottom_field_order_cnt =
			h264_strip_ffmpeg_poc_sentinel(entry->pic.BottomFieldOrderCnt,
						       entry->pic.flags);

		dpb->flags = V4L2_H264_DPB_ENTRY_FLAG_VALID;

		if (entry->used)
			dpb->flags |= V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;

		if (entry->pic.flags & VA_PICTURE_H264_LONG_TERM_REFERENCE)
			dpb->flags |= V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;

		/*
		 * Mark this DPB entry as a frame reference (both top + bottom
		 * fields). The kernel's v4l2_h264_init_reflist_builder iterates
		 * dpb[] and skips entries whose `fields` member is zero — they
		 * count as "no valid field reference for this entry." For
		 * frame-coded streams (BBB and most desktop H.264) every
		 * reference is a frame reference; per UAPI doc
		 * (ext-ctrls-codec-stateless.rst), fields must be set to
		 * V4L2_H264_FRAME_REF (= TOP|BOTTOM) for frames.
		 *
		 * Cross-reference: FFmpeg libavcodec/v4l2_request_h264.c::
		 * fill_dpb_entry sets entry->fields from pic->reference; for
		 * frames pic->reference includes V4L2_H264_FRAME_REF. Without
		 * this, P-slices that need to walk the reference list (the
		 * first one in BBB is at frame 11) hit "no valid refs" inside
		 * the kernel's reflist builder and S_EXT_CTRLS rejects the
		 * whole request with EINVAL (error_idx == count, the kernel's
		 * "application bug" sentinel).
		 */
		dpb->fields = V4L2_H264_FRAME_REF;
	}

	/*
	 * Compact the emitted entries towards dpb[0..n-1].  The kernel
	 * builds its reference lists from dec_params->dpb itself, so the
	 * absolute slot index is irrelevant; keeping the set dense matches
	 * what GStreamer/FFmpeg feed rkvdec (dpb[0..n-1]) and avoids the
	 * slot numbers drifting far above the active reference count as
	 * the internal table recycles entries.
	 */
	{
		unsigned int w = 0;

		for (i = 0; i < H264_DPB_SIZE; i++) {
			if (!(decode->dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID))
				continue;
			if (w != (unsigned int)i)
				decode->dpb[w] = decode->dpb[i];
			w++;
		}
		for (; w < H264_DPB_SIZE; w++)
			memset(&decode->dpb[w], 0, sizeof(decode->dpb[w]));
	}
}

static void h264_va_picture_to_v4l2(struct request_data *driver_data,
				    struct object_context *context,
				    struct object_surface *surface,
				    VAPictureParameterBufferH264 *VAPicture,
				    struct v4l2_ctrl_h264_decode_params *decode,
				    struct v4l2_ctrl_h264_pps *pps,
				    struct v4l2_ctrl_h264_sps *sps)
{
	unsigned char *b;
	unsigned char nal_ref_idc;
	unsigned char nal_unit_type;

	/* Extract missing nal_ref_idc and nal_unit_type */
	b = surface->source_data;
	if (context->h264_start_code)
		b += 3;
	nal_ref_idc = (b[0] >> 5) & 0x3;
	nal_unit_type = b[0] & 0x1f;

	/*
	 * Bit-parse the slice_header() to recover fields VAAPI doesn't
	 * forward and that hantro G1 hardware reads out of DECODE_PARAMS:
	 *
	 *   - dec_ref_pic_marking_bit_size  -> G1_REG_DEC_CTRL5_REFPIC_MK_LEN
	 *   - idr_pic_id                    -> G1_REG_DEC_CTRL5_IDR_PIC_ID
	 *   - pic_order_cnt_bit_size        -> G1_REG_DEC_CTRL6_POC_LENGTH
	 *   - pic_order_cnt_lsb / delta_pic_order_cnt_* (used by hantro
	 *     reference-list builder for poc_type=0/1 inter prediction)
	 *
	 * Without these set correctly, hantro's hardware bitstream parser
	 * walks past zero bits, lands on garbage, decodes zero pixels —
	 * the all-zero CAPTURE output observed during 2026-05-04 Phase 0.
	 *
	 * Spec: ITU-T H.264 §7.3.3 slice_header. Cross-reference (proven
	 * working): FFmpeg libavcodec/h264_slice.c populates
	 * H264SliceContext::ref_pic_marking_bit_size and
	 * pic_order_cnt_bit_size by the same bit-precise parse.
	 */
	{
		const struct h264_slice_header_context sh_ctx = {
			.separate_colour_plane_flag =
				(VAPicture->seq_fields.bits.residual_colour_transform_flag != 0),
			.log2_max_frame_num_minus4 =
				VAPicture->seq_fields.bits.log2_max_frame_num_minus4,
			.frame_mbs_only_flag =
				(VAPicture->seq_fields.bits.frame_mbs_only_flag != 0),
			.pic_order_cnt_type =
				VAPicture->seq_fields.bits.pic_order_cnt_type,
			.log2_max_pic_order_cnt_lsb_minus4 =
				VAPicture->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4,
			.delta_pic_order_always_zero_flag =
				(VAPicture->seq_fields.bits.delta_pic_order_always_zero_flag != 0),
			.bottom_field_pic_order_in_frame_present_flag =
				(VAPicture->pic_fields.bits.pic_order_present_flag != 0),
			.redundant_pic_cnt_present_flag =
				(VAPicture->pic_fields.bits.redundant_pic_cnt_present_flag != 0),
			.weighted_pred_flag =
				(VAPicture->pic_fields.bits.weighted_pred_flag != 0),
			.weighted_bipred_idc =
				VAPicture->pic_fields.bits.weighted_bipred_idc,
			.num_ref_idx_l0_default_active_minus1 =
				surface->params.h264.slice.num_ref_idx_l0_active_minus1,
			.num_ref_idx_l1_default_active_minus1 =
				surface->params.h264.slice.num_ref_idx_l1_active_minus1,
			.chroma_format_idc =
				VAPicture->seq_fields.bits.chroma_format_idc,
			.bit_depth_luma_minus8 =
				VAPicture->bit_depth_luma_minus8,
			.bit_depth_chroma_minus8 =
				VAPicture->bit_depth_chroma_minus8,
			.nal_unit_type = nal_unit_type,
			.nal_ref_idc   = nal_ref_idc,
		};
		struct h264_slice_header_info sh = { 0 };
		unsigned char *nal_payload = b + 1; /* past NAL header byte */
		size_t nal_payload_len = surface->slices_size -
			(size_t)((nal_payload) - (unsigned char *)surface->source_data);
		int sh_rc = h264_parse_slice_header(nal_payload, nal_payload_len,
						    &sh_ctx, &sh);
		if (sh_rc == 0) {
			decode->idr_pic_id		= sh.idr_pic_id;
			decode->pic_order_cnt_lsb	= sh.pic_order_cnt_lsb;
			decode->delta_pic_order_cnt_bottom = sh.delta_pic_order_cnt_bottom;
			decode->delta_pic_order_cnt0	= sh.delta_pic_order_cnt0;
			decode->delta_pic_order_cnt1	= sh.delta_pic_order_cnt1;
			decode->pic_order_cnt_bit_size	= sh.pic_order_cnt_bit_size;
			decode->dec_ref_pic_marking_bit_size = sh.dec_ref_pic_marking_bit_size;
		} else {
			request_log("slice_header parse FAILED rc=%d "
				    "(payload_len=%zu) — DECODE_PARAMS bit_size "
				    "fields left zero, hantro will likely produce zeros\n",
				    sh_rc, nal_payload_len);
		}
	}

	h264_fill_dpb(driver_data, context, VAPicture, decode);

	/*
	 * Populate every V4L2_CID_STATELESS_H264_DECODE_PARAMS field
	 * we can derive from VAAPI's pre-parsed VAPictureParameterBuffer
	 * + bitstream byte. Cross-reference: GStreamer
	 * gstv4l2codech264dec.c::gst_v4l2_codec_h264_dec_fill_decoder_params
	 * (lines 632-678).
	 *
	 * Fields not derivable from VAAPI (idr_pic_id, pic_order_cnt_lsb,
	 * delta_pic_order_cnt_*, dec_ref_pic_marking_bit_size,
	 * pic_order_cnt_bit_size, slice_group_change_cycle) require a
	 * full slice_header() bit-level parse, which libva-v4l2-request
	 * does not currently do. They are left at zero-init and the
	 * kernel-side hantro-vpu may compute them itself when scanning
	 * the OUTPUT bitstream — a hypothesis verified empirically by
	 * running this patch and inspecting the CAPTURE buffer.
	 */
	decode->nal_ref_idc = nal_ref_idc;
	decode->frame_num = VAPicture->frame_num;
	decode->top_field_order_cnt =
		h264_strip_ffmpeg_poc_sentinel(VAPicture->CurrPic.TopFieldOrderCnt,
					       VAPicture->CurrPic.flags);
	decode->bottom_field_order_cnt =
		h264_strip_ffmpeg_poc_sentinel(VAPicture->CurrPic.BottomFieldOrderCnt,
					       VAPicture->CurrPic.flags);

	if (nal_unit_type == 5)
		decode->flags |= V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
	if (VAPicture->pic_fields.bits.field_pic_flag)
		decode->flags |= V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC;
	if (VAPicture->CurrPic.flags & VA_PICTURE_H264_BOTTOM_FIELD)
		decode->flags |= V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD;

	pps->weighted_bipred_idc =
		VAPicture->pic_fields.bits.weighted_bipred_idc;
	pps->pic_init_qs_minus26 = VAPicture->pic_init_qs_minus26;
	pps->pic_init_qp_minus26 = VAPicture->pic_init_qp_minus26;
	pps->chroma_qp_index_offset = VAPicture->chroma_qp_index_offset;
	pps->second_chroma_qp_index_offset =
		VAPicture->second_chroma_qp_index_offset;

	if (VAPicture->pic_fields.bits.entropy_coding_mode_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;

	if (VAPicture->pic_fields.bits.weighted_pred_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;

	if (VAPicture->pic_fields.bits.transform_8x8_mode_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;

	if (VAPicture->pic_fields.bits.constrained_intra_pred_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;

	if (VAPicture->pic_fields.bits.pic_order_present_flag)
		pps->flags |=
			V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;

	if (VAPicture->pic_fields.bits.deblocking_filter_control_present_flag)
		pps->flags |=
			V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;

	if (VAPicture->pic_fields.bits.redundant_pic_cnt_present_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;

	sps->max_num_ref_frames = VAPicture->num_ref_frames;
	sps->chroma_format_idc = VAPicture->seq_fields.bits.chroma_format_idc;
	sps->bit_depth_luma_minus8 = VAPicture->bit_depth_luma_minus8;
	sps->bit_depth_chroma_minus8 = VAPicture->bit_depth_chroma_minus8;
	sps->log2_max_frame_num_minus4 =
		VAPicture->seq_fields.bits.log2_max_frame_num_minus4;
	sps->log2_max_pic_order_cnt_lsb_minus4 =
		VAPicture->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4;
	sps->pic_order_cnt_type = VAPicture->seq_fields.bits.pic_order_cnt_type;
	sps->pic_width_in_mbs_minus1 = VAPicture->picture_width_in_mbs_minus1;
	sps->pic_height_in_map_units_minus1 =
		VAPicture->picture_height_in_mbs_minus1;

	if (VAPicture->seq_fields.bits.residual_colour_transform_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE;
	if (VAPicture->seq_fields.bits.gaps_in_frame_num_value_allowed_flag)
		sps->flags |=
			V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;
	if (VAPicture->seq_fields.bits.frame_mbs_only_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
	if (VAPicture->seq_fields.bits.mb_adaptive_frame_field_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
	if (VAPicture->seq_fields.bits.direct_8x8_inference_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
	if (VAPicture->seq_fields.bits.delta_pic_order_always_zero_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO;
}

static void h264_va_matrix_to_v4l2(struct request_data *driver_data,
				   struct object_context *context,
				   VAIQMatrixBufferH264 *VAMatrix,
				   struct v4l2_ctrl_h264_scaling_matrix *v4l2_matrix)
{
	memcpy(v4l2_matrix->scaling_list_4x4, &VAMatrix->ScalingList4x4,
	       sizeof(VAMatrix->ScalingList4x4));

	/*
	 * In YUV422, there's only two matrices involved, while YUV444
	 * needs 6. However, in the former case, the two matrices
	 * should be placed at the 0 and 3 offsets.
	 */
	memcpy(v4l2_matrix->scaling_list_8x8[0], &VAMatrix->ScalingList8x8[0],
	       sizeof(v4l2_matrix->scaling_list_8x8[0]));
	memcpy(v4l2_matrix->scaling_list_8x8[1], &VAMatrix->ScalingList8x8[1],
	       sizeof(v4l2_matrix->scaling_list_8x8[1]));
}

/*
 * H.264 spec default scaling matrices: Flat_4x4_16 and Flat_8x8_16
 * (every entry = 16). When sps_scaling_matrix_present_flag and
 * pps_scaling_matrix_present_flag are both false, the bitstream
 * carries no explicit scaling lists and the decoder uses these
 * flat defaults — matching ITU-T H.264 (08/2024) §7.4.2.1.1.1
 * (sequence scaling) and §7.4.2.2 (picture scaling).
 *
 * Why we always provide the matrix: hantro G1's set_params reads
 * pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT to drive
 * the G1_REG_DEC_CTRL2_TYPE1_QUANT_E hardware bit. FFmpeg's
 * v4l2_request_h264.c always submits the SCALING_MATRIX control
 * with the spec default when the bitstream omits explicit lists,
 * and always sets the SCALING_MATRIX_PRESENT flag (commit
 * comment: "FFmpeg always provide a scaling matrix"). We mirror
 * that so the kernel sees a consistent control set across drivers.
 */
static void h264_default_flat_scaling_matrix(
	struct v4l2_ctrl_h264_scaling_matrix *v4l2_matrix)
{
	memset(v4l2_matrix->scaling_list_4x4, 16,
	       sizeof(v4l2_matrix->scaling_list_4x4));
	memset(v4l2_matrix->scaling_list_8x8, 16,
	       sizeof(v4l2_matrix->scaling_list_8x8));
}

static void h264_copy_pred_table(struct v4l2_h264_weight_factors *factors,
				 unsigned int num_refs,
				 int16_t luma_weight[32],
				 int16_t luma_offset[32],
				 int16_t chroma_weight[32][2],
				 int16_t chroma_offset[32][2])
{
	unsigned int i;

	for (i = 0; i < num_refs; i++) {
		unsigned int j;

		factors->luma_weight[i] = luma_weight[i];
		factors->luma_offset[i] = luma_offset[i];

		for (j = 0; j < 2; j++) {
			factors->chroma_weight[i][j] = chroma_weight[i][j];
			factors->chroma_offset[i][j] = chroma_offset[i][j];
		}
	}
}

static void h264_va_slice_to_v4l2(struct request_data *driver_data,
				  struct object_context *context,
				  VASliceParameterBufferH264 *VASlice,
				  VAPictureParameterBufferH264 *VAPicture,
				  struct v4l2_ctrl_h264_slice_params *slice,
				  struct v4l2_ctrl_h264_pred_weights *weights)
{
	slice->header_bit_size = VASlice->slice_data_bit_offset;
	//if (context->h264_start_code)	
	//	slice->header_bit_size += 3 * 8;
	slice->first_mb_in_slice = VASlice->first_mb_in_slice;
	slice->slice_type = VASlice->slice_type;
	slice->cabac_init_idc = VASlice->cabac_init_idc;
	slice->slice_qp_delta = VASlice->slice_qp_delta;
	slice->disable_deblocking_filter_idc =
		VASlice->disable_deblocking_filter_idc;
	slice->slice_alpha_c0_offset_div2 = VASlice->slice_alpha_c0_offset_div2;
	slice->slice_beta_offset_div2 = VASlice->slice_beta_offset_div2;

	if (((VASlice->slice_type % 5) == H264_SLICE_P) ||
	    ((VASlice->slice_type % 5) == H264_SLICE_B)) {
		unsigned int i;

		slice->num_ref_idx_l0_active_minus1 =
			VASlice->num_ref_idx_l0_active_minus1;

		for (i = 0; i < VASlice->num_ref_idx_l0_active_minus1 + 1; i++) {
			VAPictureH264 *pic = &VASlice->RefPicList0[i];
			struct h264_dpb_entry *entry;
			unsigned int idx;
			unsigned char fields;

			entry = dpb_lookup(context, pic, &idx, &fields);
			if (!entry)
				continue;

			slice->ref_pic_list0[i].index = idx;
			slice->ref_pic_list0[i].fields = fields;
		}
	}

	if ((VASlice->slice_type % 5) == H264_SLICE_B) {
		unsigned int i;

		slice->num_ref_idx_l1_active_minus1 =
			VASlice->num_ref_idx_l1_active_minus1;

		for (i = 0; i < VASlice->num_ref_idx_l1_active_minus1 + 1; i++) {
			VAPictureH264 *pic = &VASlice->RefPicList1[i];
			struct h264_dpb_entry *entry;
			unsigned int idx;
			unsigned char fields;

			entry = dpb_lookup(context, pic, &idx, &fields);
			if (!entry)
				continue;

			slice->ref_pic_list1[i].index = idx;
			slice->ref_pic_list1[i].fields = fields;
		}
	}

	if (VASlice->direct_spatial_mv_pred_flag)
		slice->flags |= V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED;

	weights->chroma_log2_weight_denom =
		VASlice->chroma_log2_weight_denom;
	weights->luma_log2_weight_denom =
		VASlice->luma_log2_weight_denom;

	if (((VASlice->slice_type % 5) == H264_SLICE_P) ||
	    ((VASlice->slice_type % 5) == H264_SLICE_B))
		h264_copy_pred_table(&weights->weight_factors[0],
				     slice->num_ref_idx_l0_active_minus1 + 1,
				     VASlice->luma_weight_l0,
				     VASlice->luma_offset_l0,
				     VASlice->chroma_weight_l0,
				     VASlice->chroma_offset_l0);

	if ((VASlice->slice_type % 5) == H264_SLICE_B)
		h264_copy_pred_table(&weights->weight_factors[1],
				     slice->num_ref_idx_l1_active_minus1 + 1,
				     VASlice->luma_weight_l1,
				     VASlice->luma_offset_l1,
				     VASlice->chroma_weight_l1,
				     VASlice->chroma_offset_l1);
}

int h264_get_controls(struct request_data *driver_data,
		      struct object_context *context)
{
	struct v4l2_ext_control controls[2] = {
		{
			.id = V4L2_CID_STATELESS_H264_DECODE_MODE,
		}, {
			.id = V4L2_CID_STATELESS_H264_START_CODE,
		}
	};
	int rc;

	rc = v4l2_get_controls(driver_data->video_fd, -1, controls, 2);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	switch (controls[0].value) {
	case V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED:
		break;
	case V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED:
		break;
	default:
		request_log("Unsupported decode mode\n");
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	switch (controls[1].value) {
	case V4L2_STATELESS_H264_START_CODE_NONE:
		context->h264_start_code = false;
		break;
	case V4L2_STATELESS_H264_START_CODE_ANNEX_B:
		context->h264_start_code = true;
		break;
	default:
		request_log("Unsupported start code\n");
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	return VA_STATUS_SUCCESS;
}

static inline __u8 h264_profile_to_idc(VAProfile profile)
{
	switch (profile) {
	case VAProfileH264Main:
		return 77;
	case VAProfileH264High:
		return 100;
	case VAProfileH264ConstrainedBaseline:
		return 66;
	case VAProfileH264MultiviewHigh:
		return 118;
	case VAProfileH264StereoHigh:
		return 128;
	default:
		return 0;
	}
}

/*
 * Derive sps.level_idc from the encoded frame size in macroblocks per
 * H.264 Annex A.3 (Table A-1) MaxFS thresholds. Each level's MaxFS is
 * the maximum encoded frame size in MBs the level supports; we pick
 * the smallest level whose MaxFS contains the actual frame size.
 *
 * Level decoding for the V4L2 control: level_idc = level * 10
 *   Level 1.0 → 10, Level 4.1 → 41, Level 5.1 → 51, Level 6.0 → 60.
 *
 * VAAPI does not expose the bitstream's actual level_idc on the
 * decode side (VAPictureParameterBufferH264 has no such field) — see
 * va.h. The H.264 SPS NAL is parsed client-side by ffmpeg-vaapi /
 * mpv and only slice data is forwarded in VASliceDataBuffer, so a
 * SPS-NAL byte parser is not viable at this layer.
 *
 * Without framerate we cannot also check MaxMBPS / MaxBR / MaxCPB.
 * That gap is acceptable in practice: consumers that push
 * temporally-dense streams (high MBPS) almost always also push
 * spatially-large frames (high MaxFS), so frame-size-based level
 * selection over-allocates on the temporal axis but never
 * under-allocates a level the consumer relies on for correct
 * decode-resource sizing.
 *
 * Picks for typical content:
 *   1080p (8160 MBs) → Level 4.1 (level_idc = 41)
 *   4K   (32400 MBs) → Level 5.1 (level_idc = 51)
 *   8K  (138240 MBs) → Level 6.0 (level_idc = 60)
 *
 * Replaces the hardcoded level_idc=51 from patch 0013.
 */
static inline __u8 h264_derive_level_idc(unsigned int width_in_mbs,
					 unsigned int height_in_mbs)
{
	const unsigned int frame_size_mbs = width_in_mbs * height_in_mbs;

	if (frame_size_mbs <= 99)     return 10;  /* Level 1.0 */
	if (frame_size_mbs <= 396)    return 11;  /* Level 1.1 - 2.0 */
	if (frame_size_mbs <= 792)    return 21;  /* Level 2.1 */
	if (frame_size_mbs <= 1620)   return 22;  /* Level 2.2 - 3.0 */
	if (frame_size_mbs <= 3600)   return 31;  /* Level 3.1 */
	if (frame_size_mbs <= 5120)   return 32;  /* Level 3.2 */
	if (frame_size_mbs <= 8192)   return 41;  /* Level 4.0 - 4.1 */
	if (frame_size_mbs <= 8704)   return 42;  /* Level 4.2 */
	if (frame_size_mbs <= 22080)  return 50;  /* Level 5.0 */
	if (frame_size_mbs <= 36864)  return 51;  /* Level 5.1 - 5.2 */
	if (frame_size_mbs <= 139264) return 60;  /* Level 6.0 - 6.2 */
	return 62;                                /* > Level 6 ceiling */
}

int h264_set_controls(struct request_data *driver_data,
		      struct object_context *context,
		      VAProfile profile,
		      struct object_surface *surface)
{
	struct v4l2_ctrl_h264_scaling_matrix matrix = { 0 };
	struct v4l2_ctrl_h264_decode_params decode = { 0 };
	struct v4l2_ctrl_h264_slice_params slice = { 0 };
	struct v4l2_ctrl_h264_pred_weights weights = { 0 };
	struct v4l2_ctrl_h264_pps pps = { 0 };
	struct v4l2_ctrl_h264_sps sps = { 0 };
	struct h264_dpb_entry *output;
	int rc;

	output = dpb_lookup(context, &surface->params.h264.picture.CurrPic,
			    NULL, NULL);
	if (!output)
		output = dpb_find_entry(context);

	dpb_clear_entry(output, true);

	dpb_update(context, &surface->params.h264.picture);

	/*
	 * Dump the raw VAAPI fields at the libva boundary so issue #8
	 * follow-up can disambiguate "ffmpeg-vaapi didn't populate" from
	 * "downstream consumer (daedalus_v4l2 wire protocol) corrupted the
	 * value". One-line; safe to leave in — costs a single printf per frame.
	 */
	request_log("h264_set_controls: VAProfile=%d seq_fields=0x%08x pic_fields=0x%08x num_ref_frames=%u bit_depth_luma_m8=%u bit_depth_chroma_m8=%u w_mbs_m1=%u h_mbs_m1=%u\n",
		    (int)profile,
		    surface->params.h264.picture.seq_fields.value,
		    surface->params.h264.picture.pic_fields.value,
		    surface->params.h264.picture.num_ref_frames,
		    surface->params.h264.picture.bit_depth_luma_minus8,
		    surface->params.h264.picture.bit_depth_chroma_minus8,
		    surface->params.h264.picture.picture_width_in_mbs_minus1,
		    surface->params.h264.picture.picture_height_in_mbs_minus1);

	h264_va_picture_to_v4l2(driver_data, context, surface,
				&surface->params.h264.picture,
				&decode, &pps, &sps);

	/*
	 * max_num_ref_frames fallback. Some VAAPI clients (older ffmpeg-vaapi
	 * paths, some daedalus_v4l2 consumers) leave VAPicture->num_ref_frames
	 * at zero. Hardware decoders tolerate; libavcodec-via-daedalus enforces
	 * sps.max_num_ref_frames strictly and rejects every frame.
	 *
	 * Count valid DPB entries first (the bitstream-true reference count we
	 * can see); fall back to a per-profile spec minimum if even that is 0.
	 * See marfrit/libva-v4l2-request-fourier issue #8.
	 */
	if (sps.max_num_ref_frames == 0) {
		unsigned int valid = 0;
		unsigned int i;
		for (i = 0; i < 16; i++) {
			const VAPictureH264 *ref =
				&surface->params.h264.picture.ReferenceFrames[i];
			if (!(ref->flags & VA_PICTURE_H264_INVALID))
				valid++;
		}
		if (valid > 0) {
			sps.max_num_ref_frames = (uint8_t)valid;
		} else {
			switch (profile) {
			case VAProfileH264ConstrainedBaseline:
				sps.max_num_ref_frames = 1;
				break;
			case VAProfileH264Main:
			case VAProfileH264High:
			case VAProfileH264MultiviewHigh:
			case VAProfileH264StereoHigh:
			default:
				sps.max_num_ref_frames = 4;
				break;
			}
		}
	}

	/*
	 * Populate the scaling matrix unconditionally: from VAAPI's
	 * VAIQMatrixBufferH264 when the consumer sent one this frame
	 * (matrix_set), otherwise from the H.264 spec flat defaults.
	 * Submitted to the kernel as V4L2_CID_STATELESS_H264_SCALING_MATRIX
	 * for every request — required for FFmpeg/hantro contract parity
	 * (see h264_default_flat_scaling_matrix() docblock).
	 */
	if (surface->params.h264.matrix_set)
		h264_va_matrix_to_v4l2(driver_data, context,
				       &surface->params.h264.matrix, &matrix);
	else
		h264_default_flat_scaling_matrix(&matrix);

	/* Zero means "not provided"; flat (16) is the spec default. */
	{
		unsigned int zi;

		for (zi = 0; zi < 6 * 16; zi++)
			if (((unsigned char *)matrix.scaling_list_4x4)[zi] == 0)
				((unsigned char *)matrix.scaling_list_4x4)[zi] = 16;
		for (zi = 0; zi < 6 * 64; zi++)
			if (((unsigned char *)matrix.scaling_list_8x8)[zi] == 0)
				((unsigned char *)matrix.scaling_list_8x8)[zi] = 16;
	}

	h264_va_slice_to_v4l2(driver_data, context,
			      &surface->params.h264.slice,
			      &surface->params.h264.picture, &slice, &weights);

	/*
	 * Only advertise SCALING_MATRIX_PRESENT when the matrix is
	 * non-flat: VAAPI does not carry the PPS's
	 * scaling_matrix_present_flag, FFmpeg always forwards an IQ matrix
	 * buffer (flat when the stream has none), and a flat list is
	 * bit-identical to no list.  Claiming it for flat matrices changes
	 * the reconstruction slightly on rkvdec.
	 */
	{
		unsigned int si;
		bool nonflat = false;

		for (si = 0; si < 6 && !nonflat; si++) {
			unsigned int sj;

			for (sj = 0; sj < 16; sj++)
				if (matrix.scaling_list_4x4[si][sj] != 16) {
					nonflat = true;
					break;
				}
			for (sj = 0; sj < 64; sj++)
				if (matrix.scaling_list_8x8[si][sj] != 16) {
					nonflat = true;
					break;
				}
		}
		if (nonflat)
			pps.flags |= V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT;
	}

	/*
	 * Populate pps->num_ref_idx_l0/l1_default_active_minus1. Hantro G1
	 * writes both into G1_REG_DEC_CTRL6_REFIDX0_ACTIVE / REFIDX1_ACTIVE
	 * MMIO registers (via "(field) + 1", so an uninitialized 0 here
	 * would advertise "1 active reference per list" to hardware, wrong
	 * for I/IDR frames with 0 refs and wrong for B frames with >1).
	 *
	 * VAAPI's VAPictureParameterBufferH264 does not carry the parsed
	 * PPS num_ref_idx_l*_default_active_minus1 fields — those are in
	 * the bitstream's PPS NAL which VAAPI consumers parse client-side
	 * but don't forward. The closest available source is VASlice's
	 * num_ref_idx_l*_active_minus1, which is the per-slice override
	 * defaulting to the PPS value (H.264 §7.4.3 num_ref_idx_active_
	 * override_flag). For most streams these values match; mismatch
	 * only on streams with explicit per-slice overrides.
	 *
	 * For IDR frames (no references), the values are not used by
	 * hantro's reference list builder, so a wrong value here is
	 * harmless. For inter frames it matters and slice-derived is
	 * the best we can do without a full PPS-NAL parser.
	 */
	pps.num_ref_idx_l0_default_active_minus1 =
		surface->params.h264.slice.num_ref_idx_l0_active_minus1;
	pps.num_ref_idx_l1_default_active_minus1 =
		surface->params.h264.slice.num_ref_idx_l1_active_minus1;

	/*
	 * Derive PFRAME / BFRAME flags in v4l2_ctrl_h264_decode_params.flags
	 * from VASliceParameterBufferH264.slice_type. VAAPI's slice_type
	 * matches the H.264 spec slice_type semantic: 0=P, 1=B, 2=I, 3=SP,
	 * 4=SI; values 5..9 mean "all slices in the picture have this
	 * slice_type" (mod 5 yields the underlying type). VAAPI consumers
	 * (ffmpeg, mpv) populate this for every slice; in FRAME_BASED mode
	 * we only see the most-recent slice's params, but slice_type is
	 * uniform across a single coded picture for our purposes.
	 *
	 * Kernel consumers that read these flags: tegra-vde
	 * (drivers/media/platform/nvidia/tegra-vde/h264.c lines 783-799 of
	 * 6.19.x) selects the inter-frame decode kernel. Hantro / rkvdec /
	 * cedrus / mediatek / qcom-iris-stateless do not consume them.
	 * Setting them keeps the libva-v4l2-request fork upstreamable
	 * across drivers without affecting hantro behaviour.
	 *
	 * Cross-reference: ext-ctrls-codec-stateless.rst Decode Parameters
	 * Flags — V4L2_H264_DECODE_PARAM_FLAG_PFRAME / _BFRAME.
	 */
	switch (surface->params.h264.slice.slice_type % 5) {
	case H264_SLICE_P:
		decode.flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
		break;
	case H264_SLICE_B:
		decode.flags |= V4L2_H264_DECODE_PARAM_FLAG_BFRAME;
		break;
	default:
		/* I / SP / SI: no extra flag. */
		break;
	}

	sps.profile_idc = h264_profile_to_idc(profile);

	/*
	 * Derive level_idc from encoded frame size per H.264 Annex A.3.
	 * VAAPI doesn't expose level_idc on the decode side (see
	 * h264_derive_level_idc()'s docblock for the rationale); we pick
	 * the smallest level whose MaxFS contains the picture dimensions.
	 * Replaces patch 0013's intermediate hardcode of 51.
	 */
	sps.level_idc = h264_derive_level_idc(
		(unsigned int)surface->params.h264.picture.picture_width_in_mbs_minus1 + 1u,
		(unsigned int)surface->params.h264.picture.picture_height_in_mbs_minus1 + 1u);

	/*
	 * Build the per-request control list incrementally:
	 *   - SPS, PPS, DECODE_PARAMS, SCALING_MATRIX: always required.
	 *     Hantro G1 reads the SCALING_MATRIX_PRESENT flag from PPS to
	 *     gate hardware register G1_REG_DEC_CTRL2_TYPE1_QUANT_E and
	 *     reads the matrix entries directly into hardware tables when
	 *     decoding. FFmpeg always submits the matrix (with spec-default
	 *     flat values when no explicit lists are in the bitstream); we
	 *     match that — see h264_default_flat_scaling_matrix() docblock.
	 *     Earlier patch 0012 made SCALING_MATRIX submission conditional
	 *     on VAAPI's VAIQMatrixBuffer arrival; that was corpus-correct
	 *     (bbb has no explicit scaling lists) but inconsistent with the
	 *     hantro contract — replaced 2026-05-04.
	 *   - SLICE_PARAMS: SLICE_BASED only. Kernel doc
	 *     ext-ctrls-codec-stateless.rst (FRAME_BASED entry):
	 *     "When this mode is selected, the
	 *     V4L2_CID_STATELESS_H264_SLICE_PARAMS control shall not be
	 *     set." Submitting it under FRAME_BASED triggers cluster-
	 *     validation EINVAL at error_idx=count.
	 *   - PRED_WEIGHTS: SLICE_BASED + V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED.
	 *
	 * Patch 0002 unconditionally sets the device to FRAME_BASED,
	 * so slice_based is hardcoded false here. When the planned
	 * probe-then-set commit lands, this becomes
	 *     context->decode_mode == V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED.
	 */
	struct v4l2_ext_control controls[6] = { 0 };
	unsigned int num_controls = 0;
	const bool slice_based = false; /* TODO: probe via context->decode_mode */

	controls[num_controls].id = V4L2_CID_STATELESS_H264_SPS;
	controls[num_controls].p_h264_sps = &sps;
	controls[num_controls].size = sizeof(sps);
	num_controls++;

	controls[num_controls].id = V4L2_CID_STATELESS_H264_PPS;
	controls[num_controls].p_h264_pps = &pps;
	controls[num_controls].size = sizeof(pps);
	num_controls++;

	controls[num_controls].id = V4L2_CID_STATELESS_H264_DECODE_PARAMS;
	controls[num_controls].p_h264_decode_params = &decode;
	controls[num_controls].size = sizeof(decode);
	num_controls++;

	controls[num_controls].id = V4L2_CID_STATELESS_H264_SCALING_MATRIX;
	controls[num_controls].p_h264_scaling_matrix = &matrix;
	controls[num_controls].size = sizeof(matrix);
	num_controls++;

	if (slice_based) {
		controls[num_controls].id = V4L2_CID_STATELESS_H264_SLICE_PARAMS;
		controls[num_controls].p_h264_slice_params = &slice;
		controls[num_controls].size = sizeof(slice);
		num_controls++;

		if (V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(&pps, &slice)) {
			controls[num_controls].id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS;
			controls[num_controls].ptr = &weights;
			controls[num_controls].size = sizeof(weights);
			num_controls++;
		}
	}

	rc = v4l2_set_controls(driver_data->video_fd, surface->request_fd,
			       controls, num_controls);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	dpb_insert(context, &surface->params.h264.picture.CurrPic, output);

	return VA_STATUS_SUCCESS;
}
