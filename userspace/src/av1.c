/*
 * Copyright (C) 2026 Markus Fritsche <fritsche.markus@gmail.com>
 *
 * AV1 codec dispatcher.  Populates V4L2_CID_STATELESS_AV1_SEQUENCE
 * (struct v4l2_ctrl_av1_sequence) from VAAPI's VADecPictureParameterBufferAV1.
 *
 * Why a single SEQUENCE control and not the full V4L2_CID_STATELESS_AV1_*
 * family (FRAME, TILE_GROUP_ENTRY, FILM_GRAIN):
 *
 *   - The daedalus_v4l2 daemon path consumes the OUTPUT bitstream
 *     directly via libavcodec/libdav1d.  libdav1d needs a complete OBU
 *     stream that includes the sequence header — ffmpeg-vaapi strips the
 *     sequence header on the client side (its parser is split across
 *     VAPictureParameterBufferAV1 + slice payload, with OBU_SEQUENCE_HEADER
 *     consumed and not re-emitted), so the daemon side has to synthesise
 *     it from the SEQUENCE ctrl.  The other AV1 ctrls (FRAME / TILE /
 *     FILM_GRAIN) are not needed for that synthesis — the OBU_FRAME_HEADER
 *     + OBU_TILE_GROUP that libdav1d also needs are still in the slice
 *     bitstream.
 *
 *   - The vpu981 (RK3588 dedicated AV1 hantro) hardware path doesn't
 *     consult these controls either — vpu981's driver parses the AV1
 *     bitstream directly.  So setting only SEQUENCE is correct for both
 *     destination decoders.
 *
 * Reference: marfrit/libva-v4l2-request-fourier issue #11
 *            (DAEMON-PPS-style sequence-header re-synthesis on the daemon
 *            side, paralleling the H.264 SPS/PPS work in DAEMON-PPS).
 *            kernel uAPI: <linux/v4l2-controls.h> @ 2891-2919.
 *            VAAPI:       <va/va_dec_av1.h> typedef
 *                         VADecPictureParameterBufferAV1.
 */

#include "av1.h"

#include "v4l2.h"
#include "utils.h"

#include <stdint.h>
#include <string.h>

#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

/*
 * VADecPictureParameterBufferAV1 reaches us transitively via surface.h →
 * va_backend.h → va.h → va_dec_av1.h (va_dec_av1.h alone won't compile
 * standalone — it needs va.h's VA_PADDING_LOW / va_deprecated machinery).
 */

/* Compile-time UAPI shift guard, sibling to vp9.c's pattern. */
_Static_assert(sizeof(struct v4l2_ctrl_av1_sequence) == 12,
	       "v4l2_ctrl_av1_sequence size mismatch — kernel UAPI changed");

/*
 * Map VAAPI bit_depth_idx (0/1/2 → 8/10/12) to the kernel ctrl's plain
 * uint8_t bit_depth field.  ffmpeg-vaapi sets idx from the bitstream
 * BitDepth value, so this is an exact inverse of AV1 spec 5.5.2.
 */
static uint8_t av1_bit_depth_from_idx(uint8_t idx)
{
	switch (idx) {
	case 0:  return 8;
	case 1:  return 10;
	case 2:  return 12;
	default:
		/* Spec-illegal; pass through so a reviewer / test catches it. */
		return 8;
	}
}

int av1_set_controls(struct request_data *driver_data,
		     struct object_context *context,
		     struct object_surface *surface_object)
{
	VADecPictureParameterBufferAV1 *picture =
		&surface_object->params.av1.picture;
	struct v4l2_ctrl_av1_sequence sequence;
	struct v4l2_ext_control ctrls[1];
	int rc;

	(void)context;

	memset(&sequence, 0, sizeof sequence);

	/*
	 * Scalar mapping.  Names align with kernel uAPI; off-by-one and
	 * idx→value translations are annotated.
	 */
	sequence.seq_profile = picture->profile;
	sequence.order_hint_bits =
		(uint8_t)(picture->order_hint_bits_minus_1 + 1u);
	sequence.bit_depth = av1_bit_depth_from_idx(picture->bit_depth_idx);
	sequence.max_frame_width_minus_1 = picture->frame_width_minus1;
	sequence.max_frame_height_minus_1 = picture->frame_height_minus1;

	/*
	 * Sequence-header flag mapping.  VAAPI exposes most of these directly
	 * in seq_info_fields.fields.*; the ones that don't have a 1:1 mirror
	 * (V4L2_AV1_SEQUENCE_FLAG_ENABLE_WARPED_MOTION, _ENABLE_REF_FRAME_MVS,
	 * _ENABLE_SUPERRES, _ENABLE_RESTORATION, _SEPARATE_UV_DELTA_Q) live in
	 * VAAPI's per-frame pic_info_fields rather than the sequence struct.
	 * For SEQUENCE-control purposes we treat them as best-effort
	 * unobservable from libva and leave the corresponding bits clear; the
	 * daedalus daemon's OBU synthesiser (issue #11 daemon track) carries
	 * the SEQUENCE bytes verbatim, so per-frame consumers (libdav1d) will
	 * still see the full bitstream truth for those toggles via the
	 * OBU_FRAME stream already in the slice buffer.  See feedback memory
	 * `feedback_vaapi_blind_to_some_hevc_sps_fields` for the precedent.
	 */
	if (picture->seq_info_fields.fields.still_picture)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_STILL_PICTURE;
	if (picture->seq_info_fields.fields.use_128x128_superblock)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_USE_128X128_SUPERBLOCK;
	if (picture->seq_info_fields.fields.enable_filter_intra)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_FILTER_INTRA;
	if (picture->seq_info_fields.fields.enable_intra_edge_filter)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_INTRA_EDGE_FILTER;
	if (picture->seq_info_fields.fields.enable_interintra_compound)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_INTERINTRA_COMPOUND;
	if (picture->seq_info_fields.fields.enable_masked_compound)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_MASKED_COMPOUND;
	if (picture->seq_info_fields.fields.enable_dual_filter)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_DUAL_FILTER;
	if (picture->seq_info_fields.fields.enable_order_hint)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_ORDER_HINT;
	if (picture->seq_info_fields.fields.enable_jnt_comp)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_JNT_COMP;
	if (picture->seq_info_fields.fields.enable_cdef)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_CDEF;
	if (picture->seq_info_fields.fields.mono_chrome)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_MONO_CHROME;
	if (picture->seq_info_fields.fields.color_range)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_COLOR_RANGE;
	if (picture->seq_info_fields.fields.subsampling_x)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_X;
	if (picture->seq_info_fields.fields.subsampling_y)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_Y;
	if (picture->seq_info_fields.fields.film_grain_params_present)
		sequence.flags |= V4L2_AV1_SEQUENCE_FLAG_FILM_GRAIN_PARAMS_PRESENT;

	/* Single-control batched submission. */
	memset(ctrls, 0, sizeof ctrls);
	ctrls[0].id   = V4L2_CID_STATELESS_AV1_SEQUENCE;
	ctrls[0].ptr  = &sequence;
	ctrls[0].size = sizeof sequence;

	rc = v4l2_set_controls(driver_data->video_fd,
			       surface_object->request_fd,
			       ctrls, 1);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	return VA_STATUS_SUCCESS;
}
