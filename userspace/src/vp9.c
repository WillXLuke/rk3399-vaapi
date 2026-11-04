/*
 * Copyright (C) 2026 Markus Fritsche <fritsche.markus@gmail.com>
 *
 * fresnel-fourier iter4 Phase 6 commit B: VP9 codec dispatcher
 * implemented against V4L2_CID_STATELESS_VP9_FRAME (0xa40a2c) +
 * V4L2_CID_STATELESS_VP9_COMPRESSED_HDR (0xa40a2d). rkvdec on
 * RK3399 mandatorily requires both controls per
 * drivers/staging/media/rkvdec/rkvdec-vp9.c::rkvdec_vp9_run_preamble:752.
 *
 * Reference: FFmpeg libavcodec/v4l2_request_vp9.c (kwiboo branch);
 *            FFmpeg libavcodec/vaapi_vp9.c (VAAPI source-side
 *            verification of field semantics);
 *            kernel drivers/media/v4l2-core/v4l2-vp9.c +
 *              drivers/staging/media/rkvdec/rkvdec-vp9.c.
 *
 * Phase 5 review amendments incorporated (see phase5_iter4_review.md):
 *   C1 frame.interpolation_filter = picture->mcomp_filter_type
 *      (NO XOR; vaapi_vp9.c:62 already applied the XOR before storing
 *      into VAAPI's mcomp_filter_type; double-XOR would swap
 *      EIGHTTAP and EIGHTTAP_SMOOTH for inter frames)
 *   C2 LF deltas persisted across frames in object_context.vp9_lf,
 *      init to VP9 spec defaults {1,0,-1,-1,0,0} on
 *      keyframe/intra_only/error_resilient, updated only when parsed
 *      lf_delta.update=1, ALWAYS copied to kernel control
 *   C3 vp9_fill_compressed_hdr takes out_reference_mode pointer
 *      (reference_mode lives in v4l2_ctrl_vp9_frame, NOT in
 *      _compressed_hdr; threaded via parameter)
 *
 * Suggested findings incorporated:
 *   S4 uv_mode memcpy from FFmpeg's fill_compressed_hdr is omitted —
 *      rkvdec reads uv_mode from kernel's persistent
 *      probability_tables, NOT from prob_updates ctrl
 *   S5 lossless_flag semantics align with FFmpeg's s->s.h.lossless
 *      (LosslessFlag = base_qindex==0 && y_dc_delta_q==0 &&
 *      uv_dc_delta_q==0 && uv_ac_delta_q==0)
 */

#include "vp9.h"

#include "v4l2.h"
#include "utils.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

/* Clause 3: compile-time size assertions. UAPI shifts must fail loudly. */
_Static_assert(sizeof(struct v4l2_ctrl_vp9_frame) == 168,
	       "v4l2_ctrl_vp9_frame size mismatch — kernel UAPI changed");
_Static_assert(sizeof(struct v4l2_ctrl_vp9_compressed_hdr) == 2040,
	       "v4l2_ctrl_vp9_compressed_hdr size mismatch — kernel UAPI changed");

/*
 * VPX range coder — minimal port of FFmpeg vpx_rac.[ch] + vp89_rac.h.
 * Stateless static helpers; bitstream-only readers. ~80 LOC.
 */

static const uint8_t vpx_norm_shift[256] = {
	8,7,6,6,5,5,5,5,4,4,4,4,4,4,4,4,
	3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
	2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
	2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

struct vp9_rac {
	int high;
	int bits;
	const uint8_t *buffer;
	const uint8_t *end;
	unsigned int code_word;
};

static int vp9_rac_init(struct vp9_rac *c, const uint8_t *buf, int size)
{
	if (size < 1)
		return -1;
	c->high = 255;
	c->bits = -16;
	c->buffer = buf;
	c->end = buf + size;
	c->code_word = ((unsigned)buf[0] << 16) | ((unsigned)buf[1] << 8) | buf[2];
	c->buffer += 3;
	return 0;
}

static unsigned vp9_rac_renorm(struct vp9_rac *c)
{
	int shift = vpx_norm_shift[c->high];
	int bits = c->bits;
	unsigned code_word = c->code_word;

	c->high <<= shift;
	code_word <<= shift;
	bits += shift;
	if (bits >= 0 && c->buffer + 1 < c->end) {
		code_word |= (((unsigned)c->buffer[0] << 8) | c->buffer[1]) << bits;
		c->buffer += 2;
		bits -= 16;
	}
	c->bits = bits;
	return code_word;
}

static int vp9_rac_get_prob(struct vp9_rac *c, uint8_t prob)
{
	unsigned code_word = vp9_rac_renorm(c);
	unsigned low = 1 + (((c->high - 1) * prob) >> 8);
	unsigned low_shift = low << 16;
	int bit = code_word >= low_shift;

	c->high = bit ? c->high - low : low;
	c->code_word = bit ? code_word - low_shift : code_word;
	return bit;
}

static int vp9_rac_get_branchy(struct vp9_rac *c, int prob)
{
	return vp9_rac_get_prob(c, (uint8_t)prob);
}

static int vp9_rac_bit(struct vp9_rac *c)
{
	return vp9_rac_get_prob(c, 128);
}

static int vp9_rac_uint(struct vp9_rac *c, int bits)
{
	int value = 0;

	while (bits--)
		value = (value << 1) | vp9_rac_bit(c);
	return value;
}

/* inv_map_table: VP9 differential probability update table.
 * Verbatim copy from FFmpeg v4l2_request_vp9.c:44-64. */
static const uint8_t inv_map_table[255] = {
	  7,  20,  33,  46,  59,  72,  85,  98, 111, 124, 137, 150, 163, 176,
	189, 202, 215, 228, 241, 254,   1,   2,   3,   4,   5,   6,   8,   9,
	 10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  21,  22,  23,  24,
	 25,  26,  27,  28,  29,  30,  31,  32,  34,  35,  36,  37,  38,  39,
	 40,  41,  42,  43,  44,  45,  47,  48,  49,  50,  51,  52,  53,  54,
	 55,  56,  57,  58,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,
	 70,  71,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,
	 86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  99, 100,
	101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 112, 113, 114, 115,
	116, 117, 118, 119, 120, 121, 122, 123, 125, 126, 127, 128, 129, 130,
	131, 132, 133, 134, 135, 136, 138, 139, 140, 141, 142, 143, 144, 145,
	146, 147, 148, 149, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160,
	161, 162, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
	177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 190, 191,
	192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 203, 204, 205, 206,
	207, 208, 209, 210, 211, 212, 213, 214, 216, 217, 218, 219, 220, 221,
	222, 223, 224, 225, 226, 227, 229, 230, 231, 232, 233, 234, 235, 236,
	237, 238, 239, 240, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251,
	252, 253, 253,
};

static int read_prob_delta(struct vp9_rac *c)
{
	int d;

	if (!vp9_rac_bit(c)) {
		d = vp9_rac_uint(c, 4);
	} else if (!vp9_rac_bit(c)) {
		d = vp9_rac_uint(c, 4) + 16;
	} else if (!vp9_rac_bit(c)) {
		d = vp9_rac_uint(c, 5) + 32;
	} else {
		d = vp9_rac_uint(c, 7);
		if (d >= 65)
			d = (d << 1) - 65 + vp9_rac_bit(c);
		d += 64;
	}
	return inv_map_table[d];
}

/*
 * Clause 6: minimal big-endian bit reader over the uncompressed header
 * for the fields VAAPI doesn't expose: lf_delta_enabled / lf_delta_update /
 * lf_ref_deltas / lf_mode_deltas / base_q_idx / delta_q_y_dc / delta_q_uv_dc /
 * delta_q_uv_ac.
 *
 * Walks: frame_marker(2) + profile(2 or 3) + show_existing_frame(1) +
 *        frame_type(1) + show_frame(1) + error_resilient(1) +
 *        if keyframe: sync_code(24) + color_config + frame_size + render_size
 *        else: intra_only(1 if !show_frame) + reset(2) +
 *              if intra_only: sync_code(24) + (if profile>0: color_config) +
 *                             refresh_flags(8) + frame_size + render_size
 *              else: refresh_flags(8) + 3*(ref_idx(3)+sign_bias(1)) +
 *                    frame_size_with_refs + allow_hpmv(1) + interp_filter(2 or 3)
 *        loop_filter_params + quantization_params
 *
 * Only profile-0 paths are exercised for BBB; non-profile-0 fields read
 * their bits but do not write them back. Keep targeted, not general.
 */

struct uh_reader {
	const uint8_t *buf;
	size_t size;
	size_t bit_pos;
};

static unsigned uh_read_bits(struct uh_reader *r, int n)
{
	unsigned v = 0;
	int i;

	for (i = 0; i < n; i++) {
		size_t byte = r->bit_pos >> 3;
		int bit = 7 - (r->bit_pos & 7);

		if (byte >= r->size)
			return 0;
		v = (v << 1) | ((r->buf[byte] >> bit) & 1);
		r->bit_pos++;
	}
	return v;
}

/* Phase 7 fix: VP9 spec s(N) is N magnitude bits + 1 sign bit (total N+1).
 * Previous uh_read_signed_6 read 4+1=5 bits instead of 6+1=7; bit drift of
 * 2 bits per ref_delta accumulated across the lf_delta updates and shifted
 * base_q_idx by 8 bits, producing 0x41 (frame 1 keyframe) instead of 0x2e.
 * Phase 3 anchor cross-check confirmed the corrected 7-bit read places
 * base_q_idx at bit 111 with value 0x2e=46. */
static int uh_read_sbits(struct uh_reader *r, int n)
{
	int v = (int)uh_read_bits(r, n);
	int sign = (int)uh_read_bits(r, 1);

	return sign ? -v : v;
}

static int uh_read_delta_q(struct uh_reader *r)
{
	/* read_delta_q(): if delta_coded bit set, read s(4) = 4 mag + 1 sign */
	if (uh_read_bits(r, 1))
		return uh_read_sbits(r, 4);
	return 0;
}

static void vp9_parse_uncompressed_header_lf_quant(
	const uint8_t *data, uint32_t size,
	struct v4l2_ctrl_vp9_frame *frame,
	int8_t persistent_ref_deltas[4],
	int8_t persistent_mode_deltas[2],
	bool *out_keyframe_or_intraonly,
	bool *out_lf_delta_updated)
{
	struct uh_reader r = { .buf = data, .size = size, .bit_pos = 0 };
	bool keyframe, intra_only = false, show_frame, error_resilient;
	int profile;
	int i;

	*out_lf_delta_updated = false;

	uh_read_bits(&r, 2);                 /* frame_marker */
	{
		int p_lo = uh_read_bits(&r, 1);
		int p_hi = uh_read_bits(&r, 1);
		profile = p_lo + (p_hi << 1);
		if (profile == 3)
			uh_read_bits(&r, 1);
	}

	if (uh_read_bits(&r, 1))             /* show_existing_frame */
		return;                      /* no LF/quant in the bitstream */

	keyframe = !uh_read_bits(&r, 1);
	show_frame = uh_read_bits(&r, 1);
	error_resilient = uh_read_bits(&r, 1);

	if (keyframe) {
		uh_read_bits(&r, 24);        /* sync_code */
		/* color_config (profile=0): just bt709 + range bit */
		if (profile >= 2)
			uh_read_bits(&r, 1); /* ten_or_twelve_bit */
		uh_read_bits(&r, 3);         /* color_space */
		if (1) {                     /* color_space != CS_RGB */
			uh_read_bits(&r, 1); /* color_range */
			if (profile == 1 || profile == 3) {
				uh_read_bits(&r, 2); /* subsampling */
				uh_read_bits(&r, 1); /* reserved */
			}
		} else if (profile == 1 || profile == 3) {
			uh_read_bits(&r, 1); /* reserved */
		}
		uh_read_bits(&r, 16);        /* frame_width_minus_1 */
		uh_read_bits(&r, 16);        /* frame_height_minus_1 */
		if (uh_read_bits(&r, 1)) {
			uh_read_bits(&r, 16);
			uh_read_bits(&r, 16);
		}
	} else {
		intra_only = show_frame ? 0 : uh_read_bits(&r, 1);
		if (!error_resilient)
			uh_read_bits(&r, 2); /* reset_frame_context */
		if (intra_only) {
			uh_read_bits(&r, 24); /* sync_code */
			if (profile > 0) {
				if (profile >= 2)
					uh_read_bits(&r, 1);
				uh_read_bits(&r, 3); /* color_space */
				uh_read_bits(&r, 1); /* color_range */
				if (profile == 1 || profile == 3) {
					uh_read_bits(&r, 2);
					uh_read_bits(&r, 1);
				}
			}
			uh_read_bits(&r, 8); /* refresh_frame_flags */
			uh_read_bits(&r, 16);
			uh_read_bits(&r, 16);
			if (uh_read_bits(&r, 1)) {
				uh_read_bits(&r, 16);
				uh_read_bits(&r, 16);
			}
		} else {
			uh_read_bits(&r, 8); /* refresh_frame_flags */
			for (i = 0; i < 3; i++) {
				uh_read_bits(&r, 3);
				uh_read_bits(&r, 1);
			}
			/* frame_size_with_refs: up to 3 found_ref bits, then
			 * if no found_ref: explicit width+height; else ref-pick.
			 * Then render_size. Just walk it. */
			{
				bool found = false;
				for (i = 0; i < 3; i++) {
					if (uh_read_bits(&r, 1))
						found = true;
				}
				if (!found) {
					uh_read_bits(&r, 16);
					uh_read_bits(&r, 16);
				}
				if (uh_read_bits(&r, 1)) {
					uh_read_bits(&r, 16);
					uh_read_bits(&r, 16);
				}
			}
			uh_read_bits(&r, 1); /* allow_hpmv */
			if (uh_read_bits(&r, 1)) /* interp_filter switchable */
				;
			else
				uh_read_bits(&r, 2); /* interp_filter literal */
		}
	}

	*out_keyframe_or_intraonly = keyframe || intra_only;

	uh_read_bits(&r, 1);                 /* refresh_frame_context */
	uh_read_bits(&r, 1);                 /* frame_parallel_decoding_mode */
	if (!error_resilient || keyframe || intra_only)
		uh_read_bits(&r, 2);         /* frame_context_idx + reset_frame_context */

	/* loop_filter_params */
	uh_read_bits(&r, 6);                 /* filter_level (already in VAAPI) */
	uh_read_bits(&r, 3);                 /* sharpness (already in VAAPI) */
	if (uh_read_bits(&r, 1)) {           /* lf_delta.enabled */
		frame->lf.flags |= V4L2_VP9_LOOP_FILTER_FLAG_DELTA_ENABLED;
		if (uh_read_bits(&r, 1)) {   /* lf_delta.updated */
			frame->lf.flags |= V4L2_VP9_LOOP_FILTER_FLAG_DELTA_UPDATE;
			*out_lf_delta_updated = true;
			for (i = 0; i < 4; i++) {
				if (uh_read_bits(&r, 1))
					persistent_ref_deltas[i] =
						(int8_t)uh_read_sbits(&r, 6);
			}
			for (i = 0; i < 2; i++) {
				if (uh_read_bits(&r, 1))
					persistent_mode_deltas[i] =
						(int8_t)uh_read_sbits(&r, 6);
			}
		}
	}

	/* quantization_params */
	frame->quant.base_q_idx     = (uint8_t)uh_read_bits(&r, 8);
	frame->quant.delta_q_y_dc   = (int8_t)uh_read_delta_q(&r);
	frame->quant.delta_q_uv_dc  = (int8_t)uh_read_delta_q(&r);
	frame->quant.delta_q_uv_ac  = (int8_t)uh_read_delta_q(&r);
}

/*
 * Clause 9: compressed-header parser — port of FFmpeg
 * v4l2_request_vp9.c:99-261::fill_compressed_hdr.
 *
 * Phase 5 C3: out_reference_mode threaded via out-param. Callers
 * derive `allowcompinter` from VAAPI sign-bias bits and pass it.
 */
#define V4L2_VP9_TX_MODE_ONLY_4X4_LOCAL    0
#define V4L2_VP9_TX_MODE_ALLOW_32X32_LOCAL 3
#define V4L2_VP9_TX_MODE_SELECT_LOCAL      4

static void vp9_fill_compressed_hdr(
	struct v4l2_ctrl_vp9_compressed_hdr *ctrl,
	const uint8_t *buffer, uint32_t size,
	uint8_t lossless_flag,
	bool keyframe_or_intraonly,
	bool allowcompinter,
	bool highprecision_mvs,
	int interp_filter_switchable,
	uint8_t *out_reference_mode)
{
	struct vp9_rac c;
	int comppredmode = 0;
	int i, j, k, l, m, n;

	*out_reference_mode = 0;

	if (vp9_rac_init(&c, buffer, size) < 0)
		return;

	if (vp9_rac_get_branchy(&c, 128))    /* marker bit */
		return;

	if (lossless_flag) {
		ctrl->tx_mode = V4L2_VP9_TX_MODE_ONLY_4X4_LOCAL;
	} else {
		ctrl->tx_mode = (uint8_t)vp9_rac_uint(&c, 2);
		if (ctrl->tx_mode == V4L2_VP9_TX_MODE_ALLOW_32X32_LOCAL)
			ctrl->tx_mode = (uint8_t)(ctrl->tx_mode + vp9_rac_bit(&c));
		if (ctrl->tx_mode == V4L2_VP9_TX_MODE_SELECT_LOCAL) {
			for (i = 0; i < 2; i++)
				if (vp9_rac_get_branchy(&c, 252))
					ctrl->tx8[i][0] = (uint8_t)read_prob_delta(&c);
			for (i = 0; i < 2; i++)
				for (j = 0; j < 2; j++)
					if (vp9_rac_get_branchy(&c, 252))
						ctrl->tx16[i][j] = (uint8_t)read_prob_delta(&c);
			for (i = 0; i < 2; i++)
				for (j = 0; j < 3; j++)
					if (vp9_rac_get_branchy(&c, 252))
						ctrl->tx32[i][j] = (uint8_t)read_prob_delta(&c);
		}
	}

	for (i = 0; i < 4; i++) {
		if (vp9_rac_bit(&c)) {
			for (j = 0; j < 2; j++)
				for (k = 0; k < 2; k++)
					for (l = 0; l < 6; l++)
						for (m = 0; m < 6; m++) {
							if (m >= 3 && l == 0)
								break;
							for (n = 0; n < 3; n++)
								if (vp9_rac_get_branchy(&c, 252))
									ctrl->coef[i][j][k][l][m][n] =
										(uint8_t)read_prob_delta(&c);
						}
		}
		if (ctrl->tx_mode == i)
			break;
	}

	for (i = 0; i < 3; i++)
		if (vp9_rac_get_branchy(&c, 252))
			ctrl->skip[i] = (uint8_t)read_prob_delta(&c);

	if (!keyframe_or_intraonly) {
		for (i = 0; i < 7; i++)
			for (j = 0; j < 3; j++)
				if (vp9_rac_get_branchy(&c, 252))
					ctrl->inter_mode[i][j] = (uint8_t)read_prob_delta(&c);

		if (interp_filter_switchable)
			for (i = 0; i < 4; i++)
				for (j = 0; j < 2; j++)
					if (vp9_rac_get_branchy(&c, 252))
						ctrl->interp_filter[i][j] =
							(uint8_t)read_prob_delta(&c);

		for (i = 0; i < 4; i++)
			if (vp9_rac_get_branchy(&c, 252))
				ctrl->is_inter[i] = (uint8_t)read_prob_delta(&c);

		if (allowcompinter) {
			comppredmode = vp9_rac_bit(&c);
			if (comppredmode)
				comppredmode += vp9_rac_bit(&c);
			if (comppredmode == 2)   /* PRED_SWITCHABLE */
				for (i = 0; i < 5; i++)
					if (vp9_rac_get_branchy(&c, 252))
						ctrl->comp_mode[i] = (uint8_t)read_prob_delta(&c);
		} else {
			comppredmode = 0;        /* PRED_SINGLEREF */
		}

		if (comppredmode != 1) {        /* != PRED_COMPREF */
			for (i = 0; i < 5; i++) {
				if (vp9_rac_get_branchy(&c, 252))
					ctrl->single_ref[i][0] = (uint8_t)read_prob_delta(&c);
				if (vp9_rac_get_branchy(&c, 252))
					ctrl->single_ref[i][1] = (uint8_t)read_prob_delta(&c);
			}
		}
		if (comppredmode != 0) {        /* != PRED_SINGLEREF */
			for (i = 0; i < 5; i++)
				if (vp9_rac_get_branchy(&c, 252))
					ctrl->comp_ref[i] = (uint8_t)read_prob_delta(&c);
		}

		for (i = 0; i < 4; i++)
			for (j = 0; j < 9; j++)
				if (vp9_rac_get_branchy(&c, 252))
					ctrl->y_mode[i][j] = (uint8_t)read_prob_delta(&c);

		for (i = 0; i < 4; i++)
			for (j = 0; j < 4; j++)
				for (k = 0; k < 3; k++)
					if (vp9_rac_get_branchy(&c, 252))
						ctrl->partition[(i * 4) + j][k] =
							(uint8_t)read_prob_delta(&c);

		for (i = 0; i < 3; i++)
			if (vp9_rac_get_branchy(&c, 252))
				ctrl->mv.joint[i] = (uint8_t)((vp9_rac_uint(&c, 7) << 1) | 1);
		for (i = 0; i < 2; i++) {
			if (vp9_rac_get_branchy(&c, 252))
				ctrl->mv.sign[i] = (uint8_t)((vp9_rac_uint(&c, 7) << 1) | 1);
			for (j = 0; j < 10; j++)
				if (vp9_rac_get_branchy(&c, 252))
					ctrl->mv.classes[i][j] = (uint8_t)((vp9_rac_uint(&c, 7) << 1) | 1);
			if (vp9_rac_get_branchy(&c, 252))
				ctrl->mv.class0_bit[i] = (uint8_t)((vp9_rac_uint(&c, 7) << 1) | 1);
			for (j = 0; j < 10; j++)
				if (vp9_rac_get_branchy(&c, 252))
					ctrl->mv.bits[i][j] = (uint8_t)((vp9_rac_uint(&c, 7) << 1) | 1);
		}
		for (i = 0; i < 2; i++) {
			for (j = 0; j < 2; j++)
				for (k = 0; k < 3; k++)
					if (vp9_rac_get_branchy(&c, 252))
						ctrl->mv.class0_fr[i][j][k] =
							(uint8_t)((vp9_rac_uint(&c, 7) << 1) | 1);
			for (j = 0; j < 3; j++)
				if (vp9_rac_get_branchy(&c, 252))
					ctrl->mv.fr[i][j] = (uint8_t)((vp9_rac_uint(&c, 7) << 1) | 1);
		}
		if (highprecision_mvs) {
			for (i = 0; i < 2; i++) {
				if (vp9_rac_get_branchy(&c, 252))
					ctrl->mv.class0_hp[i] = (uint8_t)((vp9_rac_uint(&c, 7) << 1) | 1);
				if (vp9_rac_get_branchy(&c, 252))
					ctrl->mv.hp[i] = (uint8_t)((vp9_rac_uint(&c, 7) << 1) | 1);
			}
		}
	}

	*out_reference_mode = (uint8_t)comppredmode;
}

/*
 * Clause 1+2+4+5+7+10+11+12: orchestrate VP9 control submission.
 * 2 batched controls per frame: VP9_FRAME + VP9_COMPRESSED_HDR.
 */
int vp9_set_controls(struct request_data *driver_data,
		     struct object_context *context,
		     struct object_surface *surface_object)
{
	VADecPictureParameterBufferVP9 *picture =
		&surface_object->params.vp9.picture;
	VASliceParameterBufferVP9 *slice =
		&surface_object->params.vp9.slice;

	struct v4l2_ctrl_vp9_frame frame;
	struct v4l2_ctrl_vp9_compressed_hdr compressed_hdr;
	struct v4l2_ext_control ctrls[2];
	int rc, i;
	bool keyframe = !picture->pic_fields.bits.frame_type;
	bool intra_only = picture->pic_fields.bits.intra_only;
	bool error_resilient = picture->pic_fields.bits.error_resilient_mode;
	bool allowcompinter;
	bool keyframe_or_intraonly_parsed = false;
	bool lf_delta_updated = false;
	uint8_t parsed_reference_mode = 0;

	memset(&frame, 0, sizeof frame);
	memset(&compressed_hdr, 0, sizeof compressed_hdr);

	/* Clause 4: frame geometry + per-frame scalars */
	frame.frame_width_minus_1   = (uint16_t)(picture->frame_width - 1);
	frame.frame_height_minus_1  = (uint16_t)(picture->frame_height - 1);
	frame.render_width_minus_1  = frame.frame_width_minus_1;
	frame.render_height_minus_1 = frame.frame_height_minus_1;

	frame.profile           = picture->profile;
	frame.bit_depth         = picture->bit_depth;
	frame.tile_cols_log2    = picture->log2_tile_columns;
	frame.tile_rows_log2    = picture->log2_tile_rows;
	frame.frame_context_idx = picture->pic_fields.bits.frame_context_idx;

	frame.lf.level     = picture->filter_level;
	frame.lf.sharpness = picture->sharpness_level;

	frame.uncompressed_header_size = picture->frame_header_length_in_bytes;
	frame.compressed_header_size   = picture->first_partition_size;

	/* Clause 5: DPB timestamp resolution */
	{
		VASurfaceID last_id = picture->reference_frames[picture->pic_fields.bits.last_ref_frame];
		VASurfaceID gold_id = picture->reference_frames[picture->pic_fields.bits.golden_ref_frame];
		VASurfaceID alt_id  = picture->reference_frames[picture->pic_fields.bits.alt_ref_frame];
		struct object_surface *last_ref =
			(last_id != VA_INVALID_SURFACE) ? SURFACE(driver_data, last_id) : NULL;
		struct object_surface *gold_ref =
			(gold_id != VA_INVALID_SURFACE) ? SURFACE(driver_data, gold_id) : NULL;
		struct object_surface *alt_ref =
			(alt_id  != VA_INVALID_SURFACE) ? SURFACE(driver_data, alt_id)  : NULL;

		if (last_ref) frame.last_frame_ts   = v4l2_timeval_to_ns(&last_ref->timestamp);
		if (gold_ref) frame.golden_frame_ts = v4l2_timeval_to_ns(&gold_ref->timestamp);
		if (alt_ref)  frame.alt_frame_ts    = v4l2_timeval_to_ns(&alt_ref->timestamp);
	}

	if (picture->pic_fields.bits.last_ref_frame_sign_bias)
		frame.ref_frame_sign_bias |= V4L2_VP9_SIGN_BIAS_LAST;
	if (picture->pic_fields.bits.golden_ref_frame_sign_bias)
		frame.ref_frame_sign_bias |= V4L2_VP9_SIGN_BIAS_GOLDEN;
	if (picture->pic_fields.bits.alt_ref_frame_sign_bias)
		frame.ref_frame_sign_bias |= V4L2_VP9_SIGN_BIAS_ALT;

	allowcompinter = !(
		picture->pic_fields.bits.last_ref_frame_sign_bias ==
		    picture->pic_fields.bits.golden_ref_frame_sign_bias &&
		picture->pic_fields.bits.golden_ref_frame_sign_bias ==
		    picture->pic_fields.bits.alt_ref_frame_sign_bias);

	/* Clause 6: persistent LF delta state — Phase 5 C2 */
	if (!context->vp9_lf.initialized || keyframe || intra_only || error_resilient) {
		context->vp9_lf.ref_deltas[0] = 1;
		context->vp9_lf.ref_deltas[1] = 0;
		context->vp9_lf.ref_deltas[2] = -1;
		context->vp9_lf.ref_deltas[3] = -1;
		context->vp9_lf.mode_deltas[0] = 0;
		context->vp9_lf.mode_deltas[1] = 0;
		context->vp9_lf.initialized = true;
	}

	vp9_parse_uncompressed_header_lf_quant(
		surface_object->source_data,
		surface_object->source_size,
		&frame,
		context->vp9_lf.ref_deltas,
		context->vp9_lf.mode_deltas,
		&keyframe_or_intraonly_parsed,
		&lf_delta_updated);
	(void)lf_delta_updated;

	for (i = 0; i < 4; i++)
		frame.lf.ref_deltas[i] = context->vp9_lf.ref_deltas[i];
	for (i = 0; i < 2; i++)
		frame.lf.mode_deltas[i] = context->vp9_lf.mode_deltas[i];

	/* Clause 7: segmentation mapping */
	for (i = 0; i < 7; i++)
		frame.seg.tree_probs[i] = picture->mb_segment_tree_probs[i];
	for (i = 0; i < 3; i++)
		frame.seg.pred_probs[i] = picture->segment_pred_probs[i];

	if (picture->pic_fields.bits.segmentation_enabled)
		frame.seg.flags |= V4L2_VP9_SEGMENTATION_FLAG_ENABLED;
	if (picture->pic_fields.bits.segmentation_update_map)
		frame.seg.flags |= V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP;
	if (picture->pic_fields.bits.segmentation_temporal_update)
		frame.seg.flags |= V4L2_VP9_SEGMENTATION_FLAG_TEMPORAL_UPDATE;

	for (i = 0; i < 8; i++) {
		if (slice->seg_param[i].segment_flags.fields.segment_reference_enabled) {
			frame.seg.feature_enabled[i] |= 1 << V4L2_VP9_SEG_LVL_REF_FRAME;
			frame.seg.feature_data[i][V4L2_VP9_SEG_LVL_REF_FRAME] =
				(int16_t)slice->seg_param[i].segment_flags.fields.segment_reference;
		}
		if (slice->seg_param[i].segment_flags.fields.segment_reference_skipped)
			frame.seg.feature_enabled[i] |= 1 << V4L2_VP9_SEG_LVL_SKIP;
	}

	/* Clause 10: frame flags + reference_mode + interpolation_filter */
	if (keyframe)
		frame.flags |= V4L2_VP9_FRAME_FLAG_KEY_FRAME;
	if (picture->pic_fields.bits.show_frame)
		frame.flags |= V4L2_VP9_FRAME_FLAG_SHOW_FRAME;
	if (error_resilient)
		frame.flags |= V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT;
	if (intra_only)
		frame.flags |= V4L2_VP9_FRAME_FLAG_INTRA_ONLY;
	if (picture->pic_fields.bits.allow_high_precision_mv)
		frame.flags |= V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV;
	if (picture->pic_fields.bits.refresh_frame_context)
		frame.flags |= V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX;
	if (picture->pic_fields.bits.frame_parallel_decoding_mode)
		frame.flags |= V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE;
	if (picture->pic_fields.bits.subsampling_x)
		frame.flags |= V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING;
	if (picture->pic_fields.bits.subsampling_y)
		frame.flags |= V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING;

	/* Phase 5 C1: NO XOR. VAAPI's mcomp_filter_type is already post-XOR. */
	frame.interpolation_filter = picture->pic_fields.bits.mcomp_filter_type;

	/* reset_frame_context: FFmpeg's (resetctx > 0 ? resetctx - 1 : 0) */
	frame.reset_frame_context =
		picture->pic_fields.bits.reset_frame_context > 0
		? (uint8_t)(picture->pic_fields.bits.reset_frame_context - 1)
		: 0;

	/* Clause 9: compressed-header parser fills both compressed_hdr and
	 * out_reference_mode. allowcompinter derived from sign biases above. */
	{
		int interp_switchable = (frame.interpolation_filter == V4L2_VP9_INTERP_FILTER_SWITCHABLE);

		vp9_fill_compressed_hdr(
			&compressed_hdr,
			surface_object->source_data + frame.uncompressed_header_size,
			frame.compressed_header_size,
			picture->pic_fields.bits.lossless_flag,
			keyframe || intra_only,
			allowcompinter,
			picture->pic_fields.bits.allow_high_precision_mv,
			interp_switchable,
			&parsed_reference_mode);
	}
	frame.reference_mode = parsed_reference_mode;

	/* Clause 11: 2-control batched submission */
	memset(ctrls, 0, sizeof ctrls);
	ctrls[0].id   = V4L2_CID_STATELESS_VP9_FRAME;
	ctrls[0].ptr  = &frame;
	ctrls[0].size = sizeof frame;
	ctrls[1].id   = V4L2_CID_STATELESS_VP9_COMPRESSED_HDR;
	ctrls[1].ptr  = &compressed_hdr;
	ctrls[1].size = sizeof compressed_hdr;

	rc = v4l2_set_controls(driver_data->video_fd,
			       surface_object->request_fd,
			       ctrls, 2);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	return VA_STATUS_SUCCESS;
}
