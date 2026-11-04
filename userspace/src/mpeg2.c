/*
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
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

/*
 * fresnel-fourier iter1 Phase 6 commit B: rewrite against new split
 * V4L2_CID_STATELESS_MPEG2_{SEQUENCE,PICTURE,QUANTISATION} stateless
 * controls (mainline kernel <linux/v4l2-controls.h>:1985-2105).
 *
 * Replaces the staging-era V4L2_CID_MPEG_VIDEO_MPEG2_{SLICE_PARAMS,
 * QUANTIZATION} combined-struct API that the fork previously used
 * via include/mpeg2-ctrls.h (deleted in commit C).
 *
 * Per-frame submission: one batched VIDIOC_S_EXT_CTRLS with three
 * controls (12-byte SEQUENCE + 32-byte PICTURE + 256-byte QUANTISATION),
 * matching FFmpeg libavcodec/v4l2_request_mpeg2.c:130-155 reference
 * implementation. Verified empirically in fresnel-fourier Phase 0
 * cross-validator sweep and Phase 3 Baseline C verbatim payload.
 *
 * Quantisation matrix order: zigzag scanning order per kernel doc
 * v4l2-controls.h:2076. VAAPI VAIQMatrixBufferMPEG2 also stores in
 * zigzag scanning order (per VAAPI spec). Direct memcpy works; no
 * permutation in the libva backend. Kernel hantro_mpeg2.c::
 * hantro_mpeg2_dec_copy_qtable applies the zigzag-to-raster
 * permutation when copying to the hardware quantisation table.
 *
 * Default matrices (when iqmatrix_set==false): MPEG-2 spec defaults
 * per ISO/IEC 13818-2 Table 7-3, transcribed from Phase 3 Baseline C
 * QUANTISATION verbatim payload (256 bytes captured from
 * ffmpeg-v4l2request decode of bbb_720p10s_mpeg2.ts).
 */

#include "mpeg2.h"
#include "context.h"
#include "request.h"
#include "surface.h"

#include <assert.h>
#include <string.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#include "v4l2.h"

/*
 * MPEG-2 default intra quantisation matrix in zigzag scanning order
 * (ISO/IEC 13818-2 Table 7-3, verified empirically against
 * fresnel-fourier Phase 3 Baseline C QUANTISATION payload bytes 0..63
 * from a ffmpeg-v4l2request decode of the BBB 720p10s MPEG-2 fixture).
 */
static const __u8 mpeg2_default_intra_matrix[64] = {
	  8,  16,  16,  19,  16,  19,  22,  22,
	 22,  22,  22,  22,  26,  24,  26,  27,
	 27,  27,  26,  26,  26,  26,  27,  27,
	 27,  29,  29,  29,  34,  34,  34,  29,
	 29,  29,  27,  27,  29,  29,  32,  32,
	 34,  34,  37,  38,  37,  35,  35,  34,
	 35,  38,  38,  40,  40,  40,  48,  48,
	 46,  46,  56,  56,  58,  69,  69,  83,
};

/*
 * MPEG-2 default non-intra quantisation matrix is uniformly 16 in spec.
 * Verified against Phase 3 Baseline C QUANTISATION payload bytes
 * 64..127 (all 0x10 = 16). Same applies to chroma_non_intra
 * (bytes 192..255). Filled at runtime via memset rather than a
 * separate const array to keep the binary smaller.
 */

int mpeg2_set_controls(struct request_data *driver_data,
		       struct object_context *context_object,
		       struct object_surface *surface_object)
{
	VAPictureParameterBufferMPEG2 *picture =
		&surface_object->params.mpeg2.picture;
	VAIQMatrixBufferMPEG2 *iqmatrix =
		&surface_object->params.mpeg2.iqmatrix;
	bool iqmatrix_set = surface_object->params.mpeg2.iqmatrix_set;

	/* Clause 2: v4l2_ctrl_mpeg2_sequence (12 bytes) */
	struct v4l2_ctrl_mpeg2_sequence sequence;
	/* Clause 3: v4l2_ctrl_mpeg2_picture (32 bytes; reserved[5] must be zero) */
	struct v4l2_ctrl_mpeg2_picture pic;
	/* Clause 4: v4l2_ctrl_mpeg2_quantisation (256 bytes) */
	struct v4l2_ctrl_mpeg2_quantisation quant;

	struct object_surface *forward_reference_surface;
	struct object_surface *backward_reference_surface;
	int rc;

	memset(&sequence, 0, sizeof sequence);
	memset(&pic, 0, sizeof pic);  /* zeros pic.reserved[5] per Clause 3 */
	memset(&quant, 0, sizeof quant);

	/* === Clause 2: SEQUENCE ===
	 *
	 * VAAPI's VAPictureParameterBufferMPEG2 doesn't expose the
	 * sequence-extension's progressive_sequence flag separately;
	 * use progressive_frame from the picture-coding extension as a
	 * proxy. They're identical for typical streams (BBB is
	 * progressive throughout).
	 */
	sequence.horizontal_size = picture->horizontal_size;
	sequence.vertical_size = picture->vertical_size;
	sequence.vbv_buffer_size = surface_object->source_size;
	sequence.profile_and_level_indication = 0;  /* not exposed by VAAPI */
	sequence.chroma_format = 1;  /* 4:2:0 — campaign codec scope */
	if (picture->picture_coding_extension.bits.progressive_frame)
		sequence.flags |= V4L2_MPEG2_SEQ_FLAG_PROGRESSIVE;

	/* === Clause 3: PICTURE ===
	 *
	 * Behavioral correction vs. previous mpeg2.c at this iter1:
	 * old code self-referenced surface_object->timestamp when the
	 * VAAPI ref picture was VA_INVALID_ID. New code sets ts = 0 for
	 * missing refs, matching kernel doc's 0-as-sentinel convention
	 * (verified against Phase 3 Baseline C frame 1: I-frame has both
	 * forward_ref_ts and backward_ref_ts == 0; FFmpeg
	 * libavcodec/v4l2_request_mpeg2.c:98-108 uses same convention).
	 */
	forward_reference_surface =
		SURFACE(driver_data, picture->forward_reference_picture);
	if (forward_reference_surface != NULL)
		pic.forward_ref_ts =
			v4l2_timeval_to_ns(&forward_reference_surface->timestamp);

	backward_reference_surface =
		SURFACE(driver_data, picture->backward_reference_picture);
	if (backward_reference_surface != NULL)
		pic.backward_ref_ts =
			v4l2_timeval_to_ns(&backward_reference_surface->timestamp);

	if (picture->picture_coding_extension.bits.top_field_first)
		pic.flags |= V4L2_MPEG2_PIC_FLAG_TOP_FIELD_FIRST;
	if (picture->picture_coding_extension.bits.frame_pred_frame_dct)
		pic.flags |= V4L2_MPEG2_PIC_FLAG_FRAME_PRED_DCT;
	if (picture->picture_coding_extension.bits.concealment_motion_vectors)
		pic.flags |= V4L2_MPEG2_PIC_FLAG_CONCEALMENT_MV;
	if (picture->picture_coding_extension.bits.q_scale_type)
		pic.flags |= V4L2_MPEG2_PIC_FLAG_Q_SCALE_TYPE;
	if (picture->picture_coding_extension.bits.intra_vlc_format)
		pic.flags |= V4L2_MPEG2_PIC_FLAG_INTRA_VLC;
	if (picture->picture_coding_extension.bits.alternate_scan)
		pic.flags |= V4L2_MPEG2_PIC_FLAG_ALT_SCAN;
	if (picture->picture_coding_extension.bits.repeat_first_field)
		pic.flags |= V4L2_MPEG2_PIC_FLAG_REPEAT_FIRST;
	if (picture->picture_coding_extension.bits.progressive_frame)
		pic.flags |= V4L2_MPEG2_PIC_FLAG_PROGRESSIVE;

	pic.f_code[0][0] = (picture->f_code >> 12) & 0x0f;
	pic.f_code[0][1] = (picture->f_code >>  8) & 0x0f;
	pic.f_code[1][0] = (picture->f_code >>  4) & 0x0f;
	pic.f_code[1][1] = (picture->f_code >>  0) & 0x0f;
	pic.picture_coding_type = picture->picture_coding_type;
	pic.picture_structure =
		picture->picture_coding_extension.bits.picture_structure;
	pic.intra_dc_precision =
		picture->picture_coding_extension.bits.intra_dc_precision;
	/* pic.reserved[5] zeroed by memset above */

	/* === Clause 4: QUANTISATION ===
	 *
	 * Kernel always reads all four matrices unconditionally
	 * (no load_* flags in the new API; kernel hantro_mpeg2.c
	 * doesn't synthesize defaults). When VAAPI's consumer didn't
	 * send VAIQMatrixBufferType (iqmatrix_set==false), populate
	 * with MPEG-2 spec default matrices.
	 *
	 * VAAPI VAIQMatrixBufferMPEG2 stores matrices in zigzag scanning
	 * order (per VAAPI spec). Kernel expects zigzag scanning order
	 * (per v4l2-controls.h:2076). Direct memcpy.
	 */
	if (iqmatrix_set) {
		memcpy(quant.intra_quantiser_matrix,
		       iqmatrix->intra_quantiser_matrix, 64);
		memcpy(quant.non_intra_quantiser_matrix,
		       iqmatrix->non_intra_quantiser_matrix, 64);
		memcpy(quant.chroma_intra_quantiser_matrix,
		       iqmatrix->chroma_intra_quantiser_matrix, 64);
		memcpy(quant.chroma_non_intra_quantiser_matrix,
		       iqmatrix->chroma_non_intra_quantiser_matrix, 64);
	} else {
		memcpy(quant.intra_quantiser_matrix,
		       mpeg2_default_intra_matrix, 64);
		memset(quant.non_intra_quantiser_matrix, 16, 64);
		memcpy(quant.chroma_intra_quantiser_matrix,
		       mpeg2_default_intra_matrix, 64);
		memset(quant.chroma_non_intra_quantiser_matrix, 16, 64);
	}

	/* === Clause 1+5: batched submission ===
	 *
	 * One VIDIOC_S_EXT_CTRLS with all three controls. Matches
	 * src/h264.c:986 pattern (single v4l2_set_controls call) and
	 * FFmpeg ff_v4l2_request_decode_frame contract. Bound to the
	 * surface's permanent request_fd (iter6 per-OUTPUT-slot binding;
	 * picture.c:284 sets surface_object->request_fd at BeginPicture).
	 */
	struct v4l2_ext_control ctrls[3] = {
		{
			.id = V4L2_CID_STATELESS_MPEG2_SEQUENCE,
			.ptr = &sequence,
			.size = sizeof sequence,
		},
		{
			.id = V4L2_CID_STATELESS_MPEG2_PICTURE,
			.ptr = &pic,
			.size = sizeof pic,
		},
		{
			.id = V4L2_CID_STATELESS_MPEG2_QUANTISATION,
			.ptr = &quant,
			.size = sizeof quant,
		},
	};

	rc = v4l2_set_controls(driver_data->video_fd,
			       surface_object->request_fd,
			       ctrls, 3);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	return 0;
}
