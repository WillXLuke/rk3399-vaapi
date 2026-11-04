/*
 * Copyright (C) 2007 Intel Corporation
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

#include "config.h"
#include "request.h"

#include <assert.h>
#include <string.h>

#include <sys/ioctl.h>

#include <linux/videodev2.h>

#include <hevc-ctrls.h>

#include "codec.h"
#include "utils.h"
#include "v4l2.h"

#include "autoconfig.h"

VAStatus RequestCreateConfig(VADriverContextP context, VAProfile profile,
			     VAEntrypoint entrypoint,
			     VAConfigAttrib *attributes, int attributes_count,
			     VAConfigID *config_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_config *config_object;
	VAConfigID id;
	int i, index;

	/*
	 * Encoder entrypoint: RK3399 VEPU2 H.264 encode, fixed-QP only.
	 * Only reachable when the encoder device was found at VA init.
	 */
	if (entrypoint == VAEntrypointEncSlice) {
		if (driver_data->video_fd_enc < 0)
			return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

		switch (profile) {
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264Main:
		case VAProfileH264High:
			break;
		default:
			return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
		}

		id = object_heap_allocate(&driver_data->config_heap);
		config_object = CONFIG(driver_data, id);
		if (config_object == NULL)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;

		config_object->profile = profile;
		config_object->entrypoint = entrypoint;
		config_object->is_encoder = true;
		config_object->pixelformat = V4L2_PIX_FMT_H264_SLICE;
		config_object->attributes[0].type = VAConfigAttribRTFormat;
		config_object->attributes[0].value = VA_RT_FORMAT_YUV420;
		config_object->attributes_count = 1;

		for (i = 1; i < attributes_count &&
			    config_object->attributes_count <
				    V4L2_REQUEST_MAX_CONFIG_ATTRIBUTES; i++) {
			index = config_object->attributes_count++;
			config_object->attributes[index].type =
				attributes[index].type;
			config_object->attributes[index].value =
				attributes[index].value;
		}

		*config_id = id;
		return VA_STATUS_SUCCESS;
	}

	switch (profile) {
	
	case VAProfileH264Main:
	case VAProfileH264High:
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264MultiviewHigh:
	case VAProfileH264StereoHigh:
	case VAProfileH264High10:
		// FIXME
		// iter39: Hi10P routed through same H264 path; bit-depth gating
		// happens in context.c synthetic SPS and CAPTURE pix_fmt
		// selection.
		break;
	case VAProfileMPEG2Simple:
	case VAProfileMPEG2Main:
		break;
	case VAProfileHEVCMain:
	case VAProfileHEVCMain10:
		// iter39: Main10 routed through same HEVC path; bit-depth
		// gating happens in context.c.
		break;
	case VAProfileVP8Version0_3:
		break;
	case VAProfileVP9Profile0:
		// fresnel-fourier iter4: VP9 Profile 0 enabled on rkvdec.
		// VP9 Profile 2 is NOT supported by RK3399 rkvdec (kernel ctrl
		// cap is V4L2_MPEG_VIDEO_VP9_PROFILE_0). Do not add a case for
		// VAProfileVP9Profile2 — kernel will reject.
		break;
	case VAProfileAV1Profile0:
		// ampere-av1-enablement Phase 2: AV1 Profile 0 routes to
		// vpu981 (RK3588 dedicated AV1 hantro instance). Decode-side
		// ctrl dispatch (V4L2_CID_STATELESS_AV1_*) is NOT YET WIRED
		// on master — vainfo will list the profile + CreateConfig
		// succeeds, but consumers that submit decode buffers hit a
		// NOP path until av1.{c,h} dispatch scaffolding is ported
		// from the av1-iter1 operator branch (where Phase 3-5 has
		// 3/10 frames bit-exact already).
		break;
	default:
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	}

	/*
	 * iter38: retarget the active V4L2 device to whichever physical
	 * decoder (rkvdec or hantro-vpu on RK3399) serves this codec profile.
	 * Safe no-op when the right device is already active. When a switch
	 * is needed, output/capture pools and the video_format cache are
	 * torn down so the next RequestCreateContext rebuilds them on the
	 * new device.
	 */
	(void)request_switch_device_for_profile(driver_data, profile);

	if (attributes_count > V4L2_REQUEST_MAX_CONFIG_ATTRIBUTES)
		attributes_count = V4L2_REQUEST_MAX_CONFIG_ATTRIBUTES;

	id = object_heap_allocate(&driver_data->config_heap);
	config_object = CONFIG(driver_data, id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	config_object->profile = profile;
	config_object->entrypoint = entrypoint;
	config_object->is_encoder = false;
	/*
	 * iter5b-β: cache the V4L2 OUTPUT-side FOURCC for this profile so
	 * context.c::RequestCreateContext can read it without re-running
	 * the profile→pixelformat mapping. Wires up the previously-dead
	 * pixelformat field at config.h:46. The switch above already
	 * rejected unsupported profiles via VA_STATUS_ERROR_UNSUPPORTED_PROFILE,
	 * so pixelformat_for_profile here returns non-zero for every
	 * profile that reaches this assignment.
	 */
	config_object->pixelformat = pixelformat_for_profile(profile);
	config_object->attributes[0].type = VAConfigAttribRTFormat;
	/*
	 * iter39: 10-bit profiles advertise YUV420_10. ffmpeg-vaapi reads
	 * this attribute on vaGetConfigAttributes and refuses surface
	 * allocation if it mismatches the input bitstream's bit depth.
	 */
	if (profile == VAProfileH264High10 || profile == VAProfileHEVCMain10)
		config_object->attributes[0].value = VA_RT_FORMAT_YUV420_10;
	else
		config_object->attributes[0].value = VA_RT_FORMAT_YUV420;
	config_object->attributes_count = 1;

	for (i = 1; i < attributes_count; i++) {
		index = config_object->attributes_count++;
		config_object->attributes[index].type = attributes[index].type;
		config_object->attributes[index].value =
			attributes[index].value;
	}

	*config_id = id;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestDestroyConfig(VADriverContextP context, VAConfigID config_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_config *config_object;

	config_object = CONFIG(driver_data, config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	object_heap_free(&driver_data->config_heap,
			 (struct object_base *)config_object);

	return VA_STATUS_SUCCESS;
}

/*
 * iter38: check whether `fmt` is supported on any of the open V4L2 device
 * fds (active + the two alt fds tracked since iter38 multi-device probe).
 * Tries both VIDEO_OUTPUT and VIDEO_OUTPUT_MPLANE.
 */
static bool any_fd_supports_output_format(struct request_data *driver_data,
					  unsigned int fmt)
{
	int fds[6] = {
		driver_data->video_fd,
		driver_data->video_fd_rkvdec,
		driver_data->video_fd_hantro,
		driver_data->video_fd_rpi_hevc_dec,  /* iter40 */
		driver_data->video_fd_vpu981,        /* ampere-av1 Phase 2 */
#ifdef HAVE_DAEDALUS_V4L2
		driver_data->video_fd_daedalus,      /* LIBVA-1: H.264/VP9/AV1 */
#else
		-1,
#endif
	};
	int i;
	for (i = 0; i < 6; i++) {
		if (fds[i] < 0) continue;
		if (v4l2_find_format(fds[i], V4L2_BUF_TYPE_VIDEO_OUTPUT, fmt))
			return true;
		if (v4l2_find_format(fds[i], V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, fmt))
			return true;
	}
	return false;
}

VAStatus RequestQueryConfigProfiles(VADriverContextP context,
				    VAProfile *profiles, int *profiles_count)
{
	struct request_data *driver_data = context->pDriverData;
	unsigned int index = 0;
	bool found;

	found = any_fd_supports_output_format(driver_data, V4L2_PIX_FMT_MPEG2_SLICE);
	if (found && index < (V4L2_REQUEST_MAX_PROFILES - 2)) {
		profiles[index++] = VAProfileMPEG2Simple;
		profiles[index++] = VAProfileMPEG2Main;
	}

	found = any_fd_supports_output_format(driver_data, V4L2_PIX_FMT_H264_SLICE);
	/* Encode-only hosts still advertise the three H.264 profiles; the
	 * entrypoint query below exposes EncSlice without VLD there. */
	if (driver_data->video_fd_enc >= 0)
		found = true;
	if (found && index < (V4L2_REQUEST_MAX_PROFILES - 5)) {
		profiles[index++] = VAProfileH264Main;
		profiles[index++] = VAProfileH264High;
		profiles[index++] = VAProfileH264ConstrainedBaseline;
		profiles[index++] = VAProfileH264MultiviewHigh;
		profiles[index++] = VAProfileH264StereoHigh;
		/*
		 * iter39 Phase 7 close (Option B): VAProfileH264High10
		 * DELIBERATELY NOT ENUMERATED.
		 *
		 * Hi10P on Rockchip V4L2 stateless decoders requires:
		 *   - HW: ✓ both RK3399 + RK3588 capable (per Rockchip
		 *         datasheets — 4K 10-bit H.264 line items)
		 *   - Kernel: ✓ Karlman v6→v10 series merged in
		 *             mmind v7.0 (rkvdec_h264_decoded_fmts[] has
		 *             NV15/NV20; ctrl cfg.max=HIGH_422_INTRA;
		 *             bit_depth_luma_minus8==2 path live in
		 *             rkvdec-h264-common.c:196)
		 *   - Userspace ffmpeg: ✗ ffmpeg-v4l2-request-fourier
		 *             lacks the userspace plumbing for Hi10P;
		 *             kdirect path fails with EINVAL, libva path
		 *             returns CAPTURE buffer all-zero.
		 *
		 * Empirically verified on both fresnel (RK3399) and ampere
		 * (RK3588) 2026-05-17 — same all-zero / EINVAL failure
		 * mode on both. The backend infrastructure (codec.c,
		 * context.c, image.c, surface.c, nv15.c) is RETAINED for
		 * when the upstream ffmpeg gap closes — just re-add the
		 * profiles[index++] line and bump the (-5) guard back to
		 * (-6). See memory feedback_rk3399_h264_hi10p_advertised_not_functional
		 * for the empirical evidence.
		 */
	}

	found = any_fd_supports_output_format(driver_data, V4L2_PIX_FMT_HEVC_SLICE);
	if (found && index < (V4L2_REQUEST_MAX_PROFILES - 1)) {
		profiles[index++] = VAProfileHEVCMain;
		/*
		 * iter39 Phase 7 close (Option B): VAProfileHEVCMain10
		 * DELIBERATELY NOT ENUMERATED. Same reasoning as
		 * VAProfileH264High10 above — kernel + HW ready,
		 * userspace ffmpeg V4L2 hwaccel plumbing not. Untested
		 * specifically due to no Main10 fixture (system x265
		 * is 8-bit-only on Arch ARM), but same kernel/HW/
		 * userspace stack so same gap likely applies. Re-enable
		 * when ffmpeg-vaapi → V4L2 hwaccel adds 10-bit HEVC.
		 */
	}

	found = any_fd_supports_output_format(driver_data, V4L2_PIX_FMT_VP8_FRAME);
	if (found && index < (V4L2_REQUEST_MAX_PROFILES - 1))
		profiles[index++] = VAProfileVP8Version0_3;

	found = any_fd_supports_output_format(driver_data, V4L2_PIX_FMT_VP9_FRAME);
	if (found && index < (V4L2_REQUEST_MAX_PROFILES - 1))
		profiles[index++] = VAProfileVP9Profile0;

	/*
	 * ampere-av1-enablement Phase 2: AV1 Profile 0 advertised when
	 * vpu981 (RK3588 dedicated AV1 hantro) is probed. MAX_PROFILES
	 * bumped to 14 in request.h to safely fit even if iter39 Option
	 * B is reverted (Hi10P + Main10 back in enumeration → 13 total
	 * with AV1, the `< MAX - 1` guard then needs MAX ≥ 14).
	 */
	found = any_fd_supports_output_format(driver_data, V4L2_PIX_FMT_AV1_FRAME);
	if (found && index < (V4L2_REQUEST_MAX_PROFILES - 1))
		profiles[index++] = VAProfileAV1Profile0;

	*profiles_count = index;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQueryConfigEntrypoints(VADriverContextP context,
				       VAProfile profile,
				       VAEntrypoint *entrypoints,
				       int *entrypoints_count)
{
	struct request_data *driver_data = context->pDriverData;
	int count = 0;

	switch (profile) {
	case VAProfileH264Main:
	case VAProfileH264High:
	case VAProfileH264ConstrainedBaseline:
		/*
		 * RK3399: rkvdec/hantro G1 provide VLD, hantro VEPU2 provides
		 * EncSlice.  Both are exposed on the same profile when the
		 * corresponding device was found; vainfo lists them side by
		 * side.
		 */
		if (any_fd_supports_output_format(driver_data,
						  V4L2_PIX_FMT_H264_SLICE) &&
		    count < V4L2_REQUEST_MAX_ENTRYPOINTS)
			entrypoints[count++] = VAEntrypointVLD;
		if (driver_data->video_fd_enc >= 0 &&
		    count < V4L2_REQUEST_MAX_ENTRYPOINTS)
			entrypoints[count++] = VAEntrypointEncSlice;
		*entrypoints_count = count;
		break;

	case VAProfileMPEG2Simple:
	case VAProfileMPEG2Main:
	case VAProfileH264MultiviewHigh:
	case VAProfileH264StereoHigh:
	case VAProfileH264High10:
	case VAProfileHEVCMain:
	case VAProfileHEVCMain10:
	case VAProfileVP8Version0_3:
	case VAProfileVP9Profile0:
	case VAProfileAV1Profile0:
		entrypoints[0] = VAEntrypointVLD;
		*entrypoints_count = 1;
		break;

	default:
		*entrypoints_count = 0;
		break;
	}

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQueryConfigAttributes(VADriverContextP context,
				      VAConfigID config_id, VAProfile *profile,
				      VAEntrypoint *entrypoint,
				      VAConfigAttrib *attributes,
				      int *attributes_count)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_config *config_object;
	int i;

	config_object = CONFIG(driver_data, config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	if (profile != NULL)
		*profile = config_object->profile;

	if (entrypoint != NULL)
		*entrypoint = config_object->entrypoint;

	if (attributes_count != NULL)
		*attributes_count = config_object->attributes_count;

	/* Attributes might be NULL to retrieve the associated count. */
	if (attributes != NULL)
		for (i = 0; i < config_object->attributes_count; i++)
			attributes[i] = config_object->attributes[i];

	return VA_STATUS_SUCCESS;
}

VAStatus RequestGetConfigAttributes(VADriverContextP context, VAProfile profile,
				    VAEntrypoint entrypoint,
				    VAConfigAttrib *attributes,
				    int attributes_count)
{
	struct request_data *driver_data = context->pDriverData;
	bool encoder = (entrypoint == VAEntrypointEncSlice) &&
		       driver_data->video_fd_enc >= 0;
	unsigned int i;

	for (i = 0; i < attributes_count; i++) {
		switch (attributes[i].type) {
		case VAConfigAttribRTFormat:
			/*
			 * iter39: 10-bit profiles publish YUV420_10. Profile-
			 * less query (this is invoked from vaGetConfigAttributes
			 * before vaCreateConfig) routes off the `profile` arg
			 * directly — same gating as RequestCreateConfig.
			 */
			if (profile == VAProfileH264High10 ||
			    profile == VAProfileHEVCMain10)
				attributes[i].value = VA_RT_FORMAT_YUV420_10;
			else
				attributes[i].value = VA_RT_FORMAT_YUV420;
			break;

		case VAConfigAttribRateControl:
			/* VEPU2 fixed-QP only; VA_RC_NONE is accepted as a
			 * "do whatever the SI says" alias. */
			attributes[i].value = encoder ?
				(VA_RC_CQP | VA_RC_NONE) :
				VA_ATTRIB_NOT_SUPPORTED;
			break;

		case VAConfigAttribEncPackedHeaders:
			/* SPS/PPS are serialised by userspace and prepended to
			 * the slice NAL, so no packed headers are wanted. */
			attributes[i].value = encoder ? 0 :
				VA_ATTRIB_NOT_SUPPORTED;
			break;

		case VAConfigAttribEncMaxRefFrames:
			/* 1 reference frame for P, none for B (B is not
			 * implemented by this frontend). */
			attributes[i].value = encoder ? 0x00010001 :
				VA_ATTRIB_NOT_SUPPORTED;
			break;

		case VAConfigAttribEncMaxSlices:
			attributes[i].value = encoder ? 1 :
				VA_ATTRIB_NOT_SUPPORTED;
			break;

		default:
			attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
			break;
		}
	}

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQueryDisplayAttributes(VADriverContextP context,
				       VADisplayAttribute *attributes,
				       int *attributes_count)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestGetDisplayAttributes(VADriverContextP context,
				     VADisplayAttribute *attributes,
				     int attributes_count)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestSetDisplayAttributes(VADriverContextP context,
				     VADisplayAttribute *attributes,
				     int attributes_count)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}
