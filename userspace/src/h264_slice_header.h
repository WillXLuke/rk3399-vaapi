/*
 * H.264 slice header bit-parser for libva-v4l2-request.
 *
 * Extracts the slice-header bit-position and value fields that
 * V4L2_CID_STATELESS_H264_DECODE_PARAMS requires (idr_pic_id,
 * pic_order_cnt_lsb, delta_pic_order_cnt_*, pic_order_cnt_bit_size,
 * dec_ref_pic_marking_bit_size). VAAPI's pre-parsed
 * VAPictureParameterBufferH264 / VASliceParameterBufferH264 do not
 * carry these — they live only in the bitstream's slice_header()
 * syntax. Hantro G1 (drivers/media/platform/verisilicon/
 * hantro_g1_h264_dec.c::set_params) writes the bit_size fields
 * directly into MMIO registers G1_REG_DEC_CTRL5_REFPIC_MK_LEN and
 * G1_REG_DEC_CTRL6_POC_LENGTH; with zeros the hardware bitstream
 * parser walks past zero bits, lands on garbage, decodes nothing.
 *
 * Spec reference: ITU-T Rec. H.264 (08/2024) §7.3.3 slice_header
 * and §7.3.3.1 ref_pic_list_modification, §7.3.3.2 pred_weight_table,
 * §7.3.3.3 dec_ref_pic_marking.
 *
 * Cross-reference (proven working on hantro): FFmpeg's
 * libavcodec/h264_slice.c populates H264SliceContext::ref_pic_marking_
 * bit_size and pic_order_cnt_bit_size from its bit-precise slice
 * header parse, then v4l2_request_h264.c forwards them.
 */

#ifndef H264_SLICE_HEADER_H
#define H264_SLICE_HEADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct h264_slice_header_context {
	/* From SPS (the active SPS at slice-time). */
	bool		separate_colour_plane_flag;
	uint8_t		log2_max_frame_num_minus4;
	bool		frame_mbs_only_flag;
	uint8_t		pic_order_cnt_type;
	uint8_t		log2_max_pic_order_cnt_lsb_minus4;
	bool		delta_pic_order_always_zero_flag;

	/* From PPS (the active PPS at slice-time). */
	bool		bottom_field_pic_order_in_frame_present_flag;
	bool		redundant_pic_cnt_present_flag;
	bool		weighted_pred_flag;
	uint8_t		weighted_bipred_idc;
	uint8_t		num_ref_idx_l0_default_active_minus1;
	uint8_t		num_ref_idx_l1_default_active_minus1;
	uint8_t		chroma_format_idc;
	uint8_t		bit_depth_luma_minus8;
	uint8_t		bit_depth_chroma_minus8;

	/* From the NAL unit header (already extracted by the caller). */
	uint8_t		nal_unit_type;
	uint8_t		nal_ref_idc;
};

struct h264_slice_header_info {
	uint16_t	idr_pic_id;
	uint16_t	pic_order_cnt_lsb;
	int32_t		delta_pic_order_cnt_bottom;
	int32_t		delta_pic_order_cnt0;
	int32_t		delta_pic_order_cnt1;
	uint32_t	pic_order_cnt_bit_size;
	uint32_t	dec_ref_pic_marking_bit_size;

	/* Diagnostic — useful for cross-checking VAAPI vs bitstream values. */
	uint32_t	first_mb_in_slice;
	uint32_t	slice_type;
	uint32_t	pic_parameter_set_id;
	uint32_t	frame_num;
};

/*
 * Parse slice_header() up to dec_ref_pic_marking() (inclusive) of
 * the H.264 RBSP slice_layer_without_partitioning_rbsp() syntax,
 * extracting the V4L2 DECODE_PARAMS fields. Returns 0 on success,
 * negative errno-shaped value on parse failure (insufficient data,
 * malformed exp-Golomb, etc.).
 *
 * @nal_payload: pointer to the byte AFTER the NAL header byte
 *               (i.e. start of the RBSP proper; caller has already
 *               skipped any ANNEX_B start code and the 1-byte
 *               nal_unit_header). Will be RBSP-unescaped internally
 *               before parsing.
 * @nal_payload_length: bytes available at @nal_payload.
 * @ctx: SPS/PPS/NAL context required to drive the parse.
 * @out: filled on success. All fields zero-initialized first.
 */
int h264_parse_slice_header(const uint8_t *nal_payload,
			    size_t nal_payload_length,
			    const struct h264_slice_header_context *ctx,
			    struct h264_slice_header_info *out);

#endif /* H264_SLICE_HEADER_H */
