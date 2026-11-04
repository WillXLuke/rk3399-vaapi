/*
 * H.264 slice header bit-parser implementation.
 *
 * Implements just enough of ITU-T Rec. H.264 (08/2024) §7.3.3
 * slice_header to populate the V4L2 DECODE_PARAMS bit-position
 * fields (idr_pic_id, pic_order_cnt_lsb, delta_pic_order_cnt_*,
 * pic_order_cnt_bit_size, dec_ref_pic_marking_bit_size).
 *
 * Skips through ref_pic_list_modification() and pred_weight_table()
 * because dec_ref_pic_marking() (whose bit length we need) comes
 * after them. MVC extensions (nal_unit_type 20/21) are not handled
 * — this fork strips MVC alongside HEVC.
 */

#include "h264_slice_header.h"

#include <errno.h>
#include <string.h>

/*
 * Minimal RBSP bit reader. Reads bits MSB-first. Tracks bit_pos for
 * caller use (e.g. computing the size of a syntax element by
 * pre/post bit_pos delta).
 */
struct br {
	const uint8_t	*data;
	size_t		length;	/* bytes */
	size_t		bit_pos;
	bool		error;
};

static uint32_t br_read_u(struct br *b, unsigned n)
{
	uint32_t v = 0;
	while (n--) {
		if (b->bit_pos >= b->length * 8) {
			b->error = true;
			return 0;
		}
		v = (v << 1) | ((b->data[b->bit_pos >> 3] >>
				 (7 - (b->bit_pos & 7))) & 1u);
		b->bit_pos++;
	}
	return v;
}

static uint32_t br_read_ue(struct br *b)
{
	unsigned zeros = 0;
	while (br_read_u(b, 1) == 0) {
		if (b->error || ++zeros >= 32)
			return 0;
	}
	if (zeros == 0)
		return 0;
	return (1u << zeros) - 1u + br_read_u(b, zeros);
}

static int32_t br_read_se(struct br *b)
{
	uint32_t v = br_read_ue(b);
	if (v & 1u)
		return (int32_t)((v + 1u) >> 1);
	return -(int32_t)(v >> 1);
}

/*
 * RBSP unescape: strip emulation prevention bytes (after every
 * 0x00 0x00 in the encoded stream, an extra 0x03 is inserted to
 * prevent byte-aligned start-code emulation; we strip those before
 * bit-parsing). Output buffer must be at least as large as input.
 *
 * Slice headers are short (<100 bits typically), so we unescape
 * only the first H264_SLICE_HEADER_SCAN_BYTES = 64 input bytes.
 * That covers any realistic slice header including
 * dec_ref_pic_marking() and a generous safety margin.
 */
#define H264_SLICE_HEADER_SCAN_BYTES 64

static size_t rbsp_unescape(uint8_t *out, const uint8_t *in,
			    size_t in_len)
{
	size_t out_len = 0;
	int zero_run = 0;
	size_t i;
	size_t cap = in_len < H264_SLICE_HEADER_SCAN_BYTES ?
		     in_len : H264_SLICE_HEADER_SCAN_BYTES;

	for (i = 0; i < cap; i++) {
		if (zero_run >= 2 && in[i] == 0x03) {
			zero_run = 0;
			continue;
		}
		out[out_len++] = in[i];
		zero_run = (in[i] == 0x00) ? zero_run + 1 : 0;
	}
	return out_len;
}

/*
 * §7.3.3.1 ref_pic_list_modification() — skip past it without
 * keeping any values. Length depends on slice_type and the loop
 * terminator modification_of_pic_nums_idc == 3.
 */
static void skip_ref_pic_list_modification(struct br *b,
					   uint32_t slice_type)
{
	uint32_t st_mod5 = slice_type % 5;

	if (st_mod5 != 2 && st_mod5 != 4) {
		/* P, SP, B */
		uint32_t ref_pic_list_modification_flag_l0 = br_read_u(b, 1);
		if (ref_pic_list_modification_flag_l0) {
			uint32_t mod_idc;
			do {
				mod_idc = br_read_ue(b);
				if (mod_idc == 0 || mod_idc == 1)
					br_read_ue(b); /* abs_diff_pic_num_minus1 */
				else if (mod_idc == 2)
					br_read_ue(b); /* long_term_pic_num */
				if (b->error)
					return;
			} while (mod_idc != 3);
		}
	}
	if (st_mod5 == 1) {
		/* B */
		uint32_t ref_pic_list_modification_flag_l1 = br_read_u(b, 1);
		if (ref_pic_list_modification_flag_l1) {
			uint32_t mod_idc;
			do {
				mod_idc = br_read_ue(b);
				if (mod_idc == 0 || mod_idc == 1)
					br_read_ue(b);
				else if (mod_idc == 2)
					br_read_ue(b);
				if (b->error)
					return;
			} while (mod_idc != 3);
		}
	}
}

/*
 * §7.3.3.2 pred_weight_table() — skip past it. Length depends on
 * the active reference counts and chroma_format_idc.
 */
static void skip_pred_weight_table(struct br *b,
				   uint32_t slice_type,
				   uint8_t chroma_format_idc,
				   uint8_t bit_depth_luma_minus8,
				   uint8_t bit_depth_chroma_minus8,
				   uint32_t num_ref_idx_l0_active_minus1,
				   uint32_t num_ref_idx_l1_active_minus1)
{
	uint32_t i, j;
	uint32_t st_mod5 = slice_type % 5;

	(void)bit_depth_luma_minus8;
	(void)bit_depth_chroma_minus8;

	br_read_ue(b); /* luma_log2_weight_denom */
	if (chroma_format_idc != 0)
		br_read_ue(b); /* chroma_log2_weight_denom */

	for (i = 0; i <= num_ref_idx_l0_active_minus1 && !b->error; i++) {
		uint32_t luma_weight_l0_flag = br_read_u(b, 1);
		if (luma_weight_l0_flag) {
			br_read_se(b); /* luma_weight_l0 */
			br_read_se(b); /* luma_offset_l0 */
		}
		if (chroma_format_idc != 0) {
			uint32_t chroma_weight_l0_flag = br_read_u(b, 1);
			if (chroma_weight_l0_flag) {
				for (j = 0; j < 2; j++) {
					br_read_se(b);
					br_read_se(b);
				}
			}
		}
	}

	if (st_mod5 == 1) {
		for (i = 0; i <= num_ref_idx_l1_active_minus1 && !b->error; i++) {
			uint32_t luma_weight_l1_flag = br_read_u(b, 1);
			if (luma_weight_l1_flag) {
				br_read_se(b);
				br_read_se(b);
			}
			if (chroma_format_idc != 0) {
				uint32_t chroma_weight_l1_flag = br_read_u(b, 1);
				if (chroma_weight_l1_flag) {
					for (j = 0; j < 2; j++) {
						br_read_se(b);
						br_read_se(b);
					}
				}
			}
		}
	}
}

int h264_parse_slice_header(const uint8_t *nal_payload,
			    size_t nal_payload_length,
			    const struct h264_slice_header_context *ctx,
			    struct h264_slice_header_info *out)
{
	uint8_t unescaped[H264_SLICE_HEADER_SCAN_BYTES];
	size_t unescaped_len;
	struct br b = { 0 };
	bool idr_pic_flag = (ctx->nal_unit_type == 5);
	uint32_t slice_type;
	uint32_t num_ref_idx_l0_active_minus1;
	uint32_t num_ref_idx_l1_active_minus1;
	size_t pic_order_cnt_start;
	size_t pic_order_cnt_end;
	size_t dec_ref_pic_marking_start;
	size_t dec_ref_pic_marking_end;
	bool field_pic_flag = false;

	memset(out, 0, sizeof(*out));

	if (!nal_payload || nal_payload_length == 0)
		return -EINVAL;

	unescaped_len = rbsp_unescape(unescaped, nal_payload,
				      nal_payload_length);
	if (unescaped_len < 2)
		return -EINVAL;

	b.data = unescaped;
	b.length = unescaped_len;
	b.bit_pos = 0;
	b.error = false;

	/* slice_header() per §7.3.3 */
	out->first_mb_in_slice		= br_read_ue(&b);
	slice_type			= br_read_ue(&b);
	out->slice_type			= slice_type;
	out->pic_parameter_set_id	= br_read_ue(&b);

	if (ctx->separate_colour_plane_flag)
		(void)br_read_u(&b, 2); /* colour_plane_id */

	out->frame_num = br_read_u(&b, ctx->log2_max_frame_num_minus4 + 4u);

	if (!ctx->frame_mbs_only_flag) {
		field_pic_flag = (br_read_u(&b, 1) != 0);
		if (field_pic_flag)
			(void)br_read_u(&b, 1); /* bottom_field_flag */
	}

	if (idr_pic_flag)
		out->idr_pic_id = (uint16_t)br_read_ue(&b);

	/*
	 * pic_order_cnt syntax — measure bit length from the start of
	 * pic_order_cnt_lsb / delta_pic_order_cnt[0] to the end of
	 * delta_pic_order_cnt_bottom / delta_pic_order_cnt[1]. This is
	 * what V4L2 calls pic_order_cnt_bit_size and what hantro G1
	 * writes into G1_REG_DEC_CTRL6_POC_LENGTH.
	 */
	pic_order_cnt_start = b.bit_pos;
	if (ctx->pic_order_cnt_type == 0) {
		out->pic_order_cnt_lsb = (uint16_t)br_read_u(
			&b, ctx->log2_max_pic_order_cnt_lsb_minus4 + 4u);
		if (ctx->bottom_field_pic_order_in_frame_present_flag &&
		    !field_pic_flag)
			out->delta_pic_order_cnt_bottom = br_read_se(&b);
	} else if (ctx->pic_order_cnt_type == 1 &&
		   !ctx->delta_pic_order_always_zero_flag) {
		out->delta_pic_order_cnt0 = br_read_se(&b);
		if (ctx->bottom_field_pic_order_in_frame_present_flag &&
		    !field_pic_flag)
			out->delta_pic_order_cnt1 = br_read_se(&b);
	}
	pic_order_cnt_end = b.bit_pos;
	out->pic_order_cnt_bit_size = (uint32_t)(pic_order_cnt_end -
						 pic_order_cnt_start);

	if (ctx->redundant_pic_cnt_present_flag)
		(void)br_read_ue(&b); /* redundant_pic_cnt */

	if (slice_type % 5 == 1) /* B */
		(void)br_read_u(&b, 1); /* direct_spatial_mv_pred_flag */

	num_ref_idx_l0_active_minus1 = ctx->num_ref_idx_l0_default_active_minus1;
	num_ref_idx_l1_active_minus1 = ctx->num_ref_idx_l1_default_active_minus1;

	{
		uint32_t st = slice_type % 5;
		if (st == 0 || st == 3 || st == 1) {
			/* P, SP, B */
			uint32_t override = br_read_u(&b, 1);
			if (override) {
				num_ref_idx_l0_active_minus1 = br_read_ue(&b);
				if (st == 1)
					num_ref_idx_l1_active_minus1 = br_read_ue(&b);
			}
		}
	}

	skip_ref_pic_list_modification(&b, slice_type);
	if (b.error)
		return -EIO;

	{
		uint32_t st = slice_type % 5;
		bool do_pwt =
			(ctx->weighted_pred_flag && (st == 0 || st == 3)) ||
			(ctx->weighted_bipred_idc == 1 && st == 1);
		if (do_pwt) {
			skip_pred_weight_table(&b, slice_type,
					       ctx->chroma_format_idc,
					       ctx->bit_depth_luma_minus8,
					       ctx->bit_depth_chroma_minus8,
					       num_ref_idx_l0_active_minus1,
					       num_ref_idx_l1_active_minus1);
			if (b.error)
				return -EIO;
		}
	}

	/*
	 * dec_ref_pic_marking() per §7.3.3.3 — measure bit length;
	 * hantro G1 writes this into G1_REG_DEC_CTRL5_REFPIC_MK_LEN.
	 */
	dec_ref_pic_marking_start = b.bit_pos;
	if (ctx->nal_ref_idc != 0) {
		if (idr_pic_flag) {
			(void)br_read_u(&b, 1); /* no_output_of_prior_pics_flag */
			(void)br_read_u(&b, 1); /* long_term_reference_flag */
		} else {
			uint32_t adaptive = br_read_u(&b, 1);
			if (adaptive) {
				uint32_t mmco;
				do {
					mmco = br_read_ue(&b);
					if (mmco == 1 || mmco == 3)
						br_read_ue(&b); /* difference_of_pic_nums_minus1 */
					if (mmco == 2)
						br_read_ue(&b); /* long_term_pic_num */
					if (mmco == 3 || mmco == 6)
						br_read_ue(&b); /* long_term_frame_idx */
					if (mmco == 4)
						br_read_ue(&b); /* max_long_term_frame_idx_plus1 */
					if (b.error)
						return -EIO;
				} while (mmco != 0);
			}
		}
	}
	dec_ref_pic_marking_end = b.bit_pos;
	out->dec_ref_pic_marking_bit_size =
		(uint32_t)(dec_ref_pic_marking_end - dec_ref_pic_marking_start);

	if (b.error)
		return -EIO;

	return 0;
}
