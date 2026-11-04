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

#include "context.h"
#include "config.h"
#include "encoder.h"
#include "request.h"
#include "surface.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <assert.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

#include <hevc-ctrls.h>

#include "nv15.h"  /* iter40: fallback V4L2_PIX_FMT_NV15 define for Pi 5
		    * Debian headers that ship NC12 but not NV15. */
#include "nv12_col128.h"  /* iter40: NC12 detile primitive + UV offset helper */
#include "utils.h"
#include "v4l2.h"

#include "autoconfig.h"

VAStatus RequestCreateContext(VADriverContextP context, VAConfigID config_id,
			      int picture_width, int picture_height, int flags,
			      VASurfaceID *surfaces_ids, int surfaces_count,
			      VAContextID *context_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_config *config_object;
	struct object_context *context_object = NULL;
	struct video_format *video_format;
	unsigned int destination_sizes[VIDEO_MAX_PLANES];
	unsigned int destination_bytesperlines[VIDEO_MAX_PLANES];
	unsigned int destination_planes_count;
	unsigned int format_width, format_height;
	unsigned int pixelformat;
	VASurfaceID *ids = NULL;
	VAContextID id;
	VAStatus status;
	unsigned int output_type, capture_type;
	unsigned int j;
	bool found;
	int rc;

	/*
	 * iter5b-β: CreateContext owns the V4L2 OUTPUT-side device-format
	 * lifecycle (S_FMT, CAPTURE-format probe, cap_pool_init, per-surface
	 * destination_* fill). Pre-β these lived in CreateSurfaces2 with a
	 * resolution-change gate; β moves them here because (a) config_id
	 * is known so the right OUTPUT pixel format can be derived from
	 * the bound profile, and (b) STREAMON happens at the end of this
	 * function, so the queue is never streaming when we do S_FMT.
	 *
	 * DestroyContext is the only per-session teardown site under β
	 * (no in-CreateSurfaces2 teardown branch). It STREAMOFFs both
	 * queues, calls request_pool_destroy + cap_pool_destroy, and
	 * REQBUFS(0) — leaving the V4L2 device in a clean slate for the
	 * next CreateContext.
	 */
	config_object = CONFIG(driver_data, config_id);
	if (config_object == NULL) {
		status = VA_STATUS_ERROR_INVALID_CONFIG;
		goto error;
	}

	/*
	 * Encoder contexts bypass the whole decoder setup: they open their
	 * own V4L2 queues on the VEPU2 instance (S_FMT NV12M OUTPUT /
	 * H264_SLICE CAPTURE, REQBUFS, STREAMON) and keep one media request
	 * per OUTPUT buffer.
	 */
	if (config_object->entrypoint == VAEntrypointEncSlice) {
		if (driver_data->video_fd_enc < 0) {
			status = VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
			goto error;
		}

		id = object_heap_allocate(&driver_data->context_heap);
		context_object = CONTEXT(driver_data, id);
		if (context_object == NULL) {
			status = VA_STATUS_ERROR_ALLOCATION_FAILED;
			goto error;
		}
		/* NOTE: never memset the whole object — object_heap_allocate()
		 * stores the object id (and the free-list link) in
		 * object_base, which every later CONTEXT() lookup needs. */
		memset(&context_object->dpb, 0, sizeof(context_object->dpb));
		context_object->timestamp_counter = 0;

		context_object->config_id = config_id;
		context_object->render_surface_id = VA_INVALID_ID;
		context_object->picture_width = picture_width;
		context_object->picture_height = picture_height;
		context_object->flags = flags;

		context_object->encoder = encoder_context_create(
			driver_data, config_object->profile,
			(unsigned int)picture_width,
			(unsigned int)picture_height);
		if (context_object->encoder == NULL) {
			status = VA_STATUS_ERROR_ALLOCATION_FAILED;
			goto error;
		}

		if (surfaces_count > 0) {
			ids = malloc(surfaces_count * sizeof(VASurfaceID));
			if (ids == NULL) {
				status = VA_STATUS_ERROR_ALLOCATION_FAILED;
				goto error;
			}
			memcpy(ids, surfaces_ids,
			       surfaces_count * sizeof(VASurfaceID));
		}

		context_object->surfaces_ids = ids;
		context_object->surfaces_count = surfaces_count;

		driver_data->encode_active = true;

		*context_id = id;

		return VA_STATUS_SUCCESS;
	}

	pixelformat = config_object->pixelformat;
	if (pixelformat == 0) {
		/*
		 * Defensive: CreateConfig rejects unhandled profiles, so
		 * pixelformat is always non-zero by the time we get here.
		 * Belt-and-suspenders.
		 */
		status = VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
		goto error;
	}

	/*
	 * Probe the CAPTURE-side V4L2 format. video_format is a static
	 * pointer into video.c's formats[]; it stays valid for the life of
	 * the driver_data and is cached across CreateContext cycles. The
	 * probe doesn't require any prior S_FMT — v4l2_find_format
	 * enumerates the device's supported formats directly.
	 *
	 * iter39: choose NV15 (10-bit packed) for Hi10P / Main10 profiles,
	 * NV12 (8-bit) otherwise. If the cached video_format doesn't match
	 * the profile's bit-depth requirement, invalidate and re-probe —
	 * sibling pattern to iter38's device-switch invalidation in
	 * request_switch_device_for_profile().
	 */
	{
		bool want_10bit = (config_object->profile == VAProfileH264High10 ||
				   config_object->profile == VAProfileHEVCMain10);
		bool is_rpi = (driver_data->video_fd ==
			       driver_data->video_fd_rpi_hevc_dec);
		/*
		 * iter40: per-fd preferred pixelformat. rpi-hevc-dec exposes
		 * NC12 (8-bit) / NC30 (10-bit), not NV12 / NV15.
		 */
		unsigned int want_pixfmt;
		if (is_rpi)
			want_pixfmt = want_10bit ? V4L2_PIX_FMT_NV12_10_COL128
						 : V4L2_PIX_FMT_NV12_COL128;
		else
			want_pixfmt = want_10bit ? V4L2_PIX_FMT_NV15
						 : V4L2_PIX_FMT_NV12;
		if (driver_data->video_format &&
		    driver_data->video_format->v4l2_format != want_pixfmt &&
		    driver_data->video_format->v4l2_format != V4L2_PIX_FMT_SUNXI_TILED_NV12)
			driver_data->video_format = NULL;
	}
	if (!driver_data->video_format) {
		bool want_10bit = (config_object->profile == VAProfileH264High10 ||
				   config_object->profile == VAProfileHEVCMain10);
		bool is_rpi = (driver_data->video_fd ==
			       driver_data->video_fd_rpi_hevc_dec);
		video_format = NULL;

		if (is_rpi) {
			/*
			 * iter40: rpi-hevc-dec CAPTURE is NC12 (8-bit SAND
			 * 128-pixel-wide column tile) or NC30 (10-bit variant).
			 * Direct map; the kernel exposes BOTH formats in
			 * VIDIOC_ENUM_FMT(CAPTURE_MPLANE) without a pre-SPS
			 * step (verified Phase 0 strace), so find_format would
			 * also succeed — skip it for symmetry with the NV15
			 * iter39 branch below.
			 */
			video_format = video_format_find(
				want_10bit ? V4L2_PIX_FMT_NV12_10_COL128
					   : V4L2_PIX_FMT_NV12_COL128);
		} else if (!want_10bit) {
			found = v4l2_find_format(driver_data->video_fd,
						 V4L2_BUF_TYPE_VIDEO_CAPTURE,
						 V4L2_PIX_FMT_SUNXI_TILED_NV12);
			if (found)
				video_format = video_format_find(V4L2_PIX_FMT_SUNXI_TILED_NV12);

			found = v4l2_find_format(driver_data->video_fd,
						 V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
						 V4L2_PIX_FMT_NV12);
			if (found)
				video_format = video_format_find(V4L2_PIX_FMT_NV12);
		} else {
			/*
			 * iter39 fresnel fix: rkvdec only advertises NV15 in
			 * VIDIOC_ENUM_FMT(CAPTURE) AFTER S_FMT(OUTPUT) +
			 * S_EXT_CTRLS(SPS) resolve image_fmt to 420_10BIT.
			 * Before that, only NV12 is enumerated. Pre-finding
			 * NV15 always fails. Skip the find_format check and
			 * directly map to our NV15 video_format entry; the
			 * later S_FMT(CAPTURE) commits the actual NV15 mode
			 * once the synthetic SPS sets bit_depth_luma_minus8=2.
			 */
			video_format = video_format_find(V4L2_PIX_FMT_NV15);
		}

		if (video_format == NULL) {
			status = VA_STATUS_ERROR_OPERATION_FAILED;
			goto error;
		}

		driver_data->video_format = video_format;
	}
	video_format = driver_data->video_format;

	/* iter39: session-wide flag drives image.c reporting + unpack. */
	driver_data->is_10bit = (config_object->profile == VAProfileH264High10 ||
				 config_object->profile == VAProfileHEVCMain10);

	output_type = v4l2_type_video_output(video_format->v4l2_mplane);
	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	/*
	 * Commit the OUTPUT pixel format. picture_width/picture_height
	 * are the kernel-facing dimensions for this decode session. With
	 * profile-derived pixelformat, hantro's CAPTURE-format derivation
	 * dispatches to the right codec_mode (pre-β hardcoded H264_SLICE
	 * meant hantro silently substituted MPEG2_DECODER for HEVC/VP8/VP9
	 * → all-zero CAPTURE; rkvdec silently dropped HEVC/VP9 → same
	 * outcome).
	 */
	rc = v4l2_set_format(driver_data->video_fd, output_type, pixelformat,
			     picture_width, picture_height);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	/*
	 * iter15 α-19: explicit S_FMT on CAPTURE for rkvdec.
	 *
	 * Original iter5b-β comment: "Do NOT VIDIOC_S_FMT on CAPTURE — hantro
	 * reads the SPS from OUTPUT to set CAPTURE shape internally."
	 *
	 * Empirical finding at iter15 Phase 3 (2026-05-14): kdirect (ffmpeg-
	 * v4l2request) does S_FMT on CAPTURE side after S_FMT(OUTPUT),
	 * then CREATE_BUFS for CAPTURE. libva's old G_FMT-only path skipped
	 * the S_FMT call. For hantro this was deliberate (works); for rkvdec
	 * (HEVC + H.264 + VP9 on RK3399) the absence of explicit S_FMT puts
	 * the driver into a state where it does NOT commit the chosen NV12
	 * pixel format properly — and the resulting decode silently writes
	 * garbage or zero for HEVC + H.264 (Bug 4 + Bug 5).
	 *
	 * Per [[feedback-per-driver-kludge-gating]]: this driver-specific
	 * difference should be gated on driver_kind. For now use a single
	 * always-on S_FMT call as the safe move: kdirect proves S_FMT
	 * CAPTURE works on both hantro AND rkvdec (it's the reference path).
	 * The iter5b-β comment is preserved-but-amended below.
	 *
	 * Sequence: S_FMT OUTPUT (above) → S_FMT CAPTURE (this) → G_FMT
	 * CAPTURE (sanity read-back, matches what S_FMT committed).
	 */
	{
		/*
		 * iter40: take the CAPTURE pixelformat from the resolved
		 * video_format slot — that's per-fd, per-bit-depth correct.
		 *   rkvdec  8-bit  → NV12
		 *   rkvdec 10-bit  → NV15
		 *   hantro  8-bit  → NV12
		 *   rpi-hevc-dec   → NC12 (V4L2_PIX_FMT_NV12_COL128)
		 * Pre-iter40 this was hardcoded NV12/NV15 — the rpi-hevc-dec
		 * fd would then have S_FMT(NV12) issued, and the kernel
		 * "helpfully" substituted V4L2_PIX_FMT_NV12MT_COL128 (the
		 * MULTI-PLANE-NON-CONTIGUOUS variant) instead of the
		 * SINGLE-PLANE NC12 we wanted, breaking cap_pool QUERYBUF
		 * downstream (Phase 7 iter40 first-run discovery).
		 */
		unsigned int capture_pixelformat =
			driver_data->video_format->v4l2_format;
		rc = v4l2_set_format(driver_data->video_fd, capture_type,
				     capture_pixelformat, picture_width,
				     picture_height);
		if (rc < 0) {
			/* Non-fatal: if the kernel rejects S_FMT CAPTURE (some
			 * older hantro variants), fall through to G_FMT. */
			request_log("iter15 α-19: S_FMT CAPTURE failed (continuing): %s\n",
				    strerror(errno));
		}
	}

	rc = v4l2_get_format(driver_data->video_fd, capture_type, &format_width,
			     &format_height, destination_bytesperlines,
			     destination_sizes, NULL);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	/*
	 * iter25 α-25: synthetic-SPS injection to pre-seed ctx->image_fmt
	 * before CAPTURE buffer allocation.
	 *
	 * Root cause (iter17→iter24 kernel-printk chain): rkvdec_s_ctrl for
	 * HEVC_SPS / H264_SPS calls get_image_fmt() and, if the resolved
	 * image_fmt differs from the cached ctx->image_fmt (default
	 * RKVDEC_IMG_FMT_ANY), tries to reset the CAPTURE format. The reset
	 * returns -EBUSY when vb2_is_busy(CAPTURE_queue) — i.e. any CAPTURE
	 * buffer is allocated.
	 *
	 * libva (iter5b-β CAPTURE pool) pre-allocates 24 CAPTURE buffers
	 * via cap_pool_init below — before any per-frame S_EXT_CTRLS
	 * arrives. So the first real HEVC_SPS at decode time fails with
	 * -EBUSY in try_or_set_cluster, breaks v4l2_ctrl_request_setup's
	 * outer loop, and leaves ctx->ctrl_hdl[SPS..DECODE_PARAMS] at all-
	 * zero contents. rkvdec_hevc_run reads zero, hardware sees w=0
	 * h=0, decoded CAPTURE is all-zero (Bug 5 + Bug 4).
	 *
	 * Fix: while CAPTURE is still empty (before cap_pool_init), inject
	 * a synthetic SPS containing the profile's chroma + bit_depth so
	 * rkvdec_s_ctrl resolves image_fmt and updates ctx->image_fmt
	 * before vb2_is_busy can return true. From then on, per-frame
	 * SPS submissions with matching profile parameters see
	 * image_fmt_changed=false → skip reset → commit succeeds.
	 *
	 * Gated by config->profile: only HEVC and H.264 paths set
	 * get_image_fmt in their rkvdec coded_fmt_desc->ops; VP9 / MPEG-2 /
	 * VP8 are unaffected (rkvdec_s_ctrl returns 0 immediately when
	 * get_image_fmt is NULL, or those codecs are routed to hantro).
	 *
	 * Failure is best-effort: if the kernel returns -EBUSY/-EINVAL here
	 * (e.g. driver doesn't expose the control on this DT path), we fall
	 * through and may still hit the original bug for that codec — but
	 * the device-init DECODE_MODE + START_CODE block below ALSO uses
	 * void-cast best-effort, so this is consistent with prior pattern.
	 */
	/*
	 * iter40 (Phase 5 review F6): the synthetic-SPS pre-seed is an
	 * rkvdec-specific quirk fix (the -EBUSY-on-CAPTURE-busy bug in
	 * rkvdec_s_ctrl). rpi-hevc-dec does NOT need it and uses a
	 * different submission ordering (Phase 0 strace: S_FMT_OUTPUT →
	 * REQBUFS_OUTPUT → S_FMT_CAPTURE → CREATE_BUFS_CAPTURE → STREAMON,
	 * with per-frame SPS via S_EXT_CTRLS class=0xf010000). Sending a
	 * stale dummy SPS at context-init time would leave rpi-hevc-dec's
	 * internal state on the dummy until the first real per-frame SPS
	 * arrives — exact behavior unknown but a known divergence from
	 * kdirect.
	 *
	 * Skip pre-seed when the active fd is rpi-hevc-dec. rkvdec /
	 * hantro paths unchanged.
	 */
	if (driver_data->video_fd != driver_data->video_fd_rpi_hevc_dec) {
		/*
		 * iter39: 10-bit profiles set bit_depth_luma_minus8 = 2 in
		 * the synthetic SPS so rkvdec's get_image_fmt resolves to
		 * RKVDEC_IMG_FMT_420_10BIT (per rkvdec-h264-common.c:196 +
		 * rkvdec-hevc-common.c:467). Image_fmt resolution depends
		 * only on bit_depth_luma_minus8 and chroma_format_idc;
		 * profile_idc is ignored for image_fmt and v4l2_ctrl_hevc_sps
		 * has no profile_idc field at all.
		 */
		bool ten = driver_data->is_10bit;
		switch (config_object->profile) {
		case VAProfileHEVCMain:
		case VAProfileHEVCMain10: {
			struct v4l2_ctrl_hevc_sps dummy_sps;
			struct v4l2_ext_control dummy_ctrl;

			memset(&dummy_sps, 0, sizeof(dummy_sps));
			dummy_sps.chroma_format_idc = 1; /* 4:2:0 */
			dummy_sps.bit_depth_luma_minus8 = ten ? 2 : 0;
			dummy_sps.bit_depth_chroma_minus8 = ten ? 2 : 0;
			dummy_sps.pic_width_in_luma_samples = picture_width;
			dummy_sps.pic_height_in_luma_samples = picture_height;

			dummy_ctrl.id = V4L2_CID_STATELESS_HEVC_SPS;
			dummy_ctrl.ptr = &dummy_sps;
			dummy_ctrl.size = sizeof(dummy_sps);
			(void)v4l2_set_controls(driver_data->video_fd, -1,
						&dummy_ctrl, 1);
			break;
		}
		case VAProfileH264Main:
		case VAProfileH264High:
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264MultiviewHigh:
		case VAProfileH264StereoHigh:
		case VAProfileH264High10: {
			struct v4l2_ctrl_h264_sps dummy_sps;
			struct v4l2_ext_control dummy_ctrl;

			memset(&dummy_sps, 0, sizeof(dummy_sps));
			dummy_sps.chroma_format_idc = 1; /* 4:2:0 */
			dummy_sps.bit_depth_luma_minus8 = ten ? 2 : 0;
			dummy_sps.bit_depth_chroma_minus8 = ten ? 2 : 0;
			dummy_sps.pic_width_in_mbs_minus1 =
				(picture_width + 15) / 16 - 1;
			dummy_sps.pic_height_in_map_units_minus1 =
				(picture_height + 15) / 16 - 1;
			dummy_sps.profile_idc = ten ? 110 : 100; /* High10 : High */
			dummy_sps.level_idc = 41;
			/*
			 * FRAME_MBS_ONLY required: rkvdec_h264_validate_sps
			 * doubles height for non-frame-mbs-only streams to
			 * compute frame-height from field-height. Without
			 * this flag, dummy with (height_in_map_units+1)*16
			 * = 1088 doubles to 2176 > coded_fmt 1080 → -EINVAL.
			 */
			dummy_sps.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;

			dummy_ctrl.id = V4L2_CID_STATELESS_H264_SPS;
			dummy_ctrl.ptr = &dummy_sps;
			dummy_ctrl.size = sizeof(dummy_sps);
			(void)v4l2_set_controls(driver_data->video_fd, -1,
						&dummy_ctrl, 1);
			break;
		}
		default:
			break;
		}
	}  /* iter40: end of pre-seed-skip-on-rpi-hevc-dec guard */

	destination_planes_count = video_format->planes_count;

	/*
	 * Initialize the CAPTURE buffer pool (cap_pool). Pool size =
	 * max(surfaces_count, MIN_CAP_POOL). The headroom gives LRU
	 * recycling enough margin to never reuse a buffer within the
	 * consumer's compositor-hold window for typical playback
	 * patterns. cap_pool_init does the V4L2 CREATE_BUFS + per-slot
	 * mmap.
	 *
	 * `pool->initialized` is reset to false by cap_pool_destroy in
	 * DestroyContext; subsequent CreateContext re-inits at the new
	 * resolution.
	 */
	if (!driver_data->capture_pool.initialized) {
		unsigned int pool_count = surfaces_count > MIN_CAP_POOL ?
					  surfaces_count : MIN_CAP_POOL;
		rc = cap_pool_init(&driver_data->capture_pool,
				   driver_data->video_fd, capture_type,
				   pool_count, video_format->v4l2_buffers_count);
		if (rc < 0) {
			status = VA_STATUS_ERROR_ALLOCATION_FAILED;
			goto error;
		}
	}

	/*
	 * Compute format-uniform destination_* values. Same for all
	 * surfaces of this format; written once per surface, never
	 * changed by BeginPicture's slot acquisition.
	 */
	if (video_format->v4l2_buffers_count == 1) {
		if (video_format->v4l2_format == V4L2_PIX_FMT_NV12_COL128) {
			/*
			 * iter40: NC12 SAND layout: Y plane size is
			 * NUM_COLUMNS * TILE_W * ALIGN(height, 8) (= linear
			 * NV12 Y for column-aligned widths), UV plane is half.
			 * The kernel-reported destination_bytesperlines[0] is
			 * the COLUMN stride (ALIGN(height,8)*3/2), not the
			 * linear Y stride — using it × format_height gives the
			 * wrong intra-buffer UV offset (destination_offsets[1]
			 * derives from destination_sizes[0] in
			 * surface_fill_format_uniform).
			 *
			 * Use format_width/format_height (kernel-returned from
			 * G_FMT) not picture_width/height (caller request),
			 * because the kernel applies its own ALIGN rules; the
			 * UV plane location is keyed off the kernel layout.
			 */
			unsigned int uv_off = nv12_col128_uv_plane_offset(
				format_width, format_height);
			destination_sizes[0] = uv_off;
			for (j = 1; j < destination_planes_count; j++)
				destination_sizes[j] = uv_off / 2;
			request_log("iter40: NC12 sizes pic=%ux%u fmt=%ux%u bpl=%u uv_off=%u sizeimage(kernel)=%u\n",
				    picture_width, picture_height,
				    format_width, format_height,
				    destination_bytesperlines[0], uv_off,
				    destination_bytesperlines[0] * format_height);
		} else {
			destination_sizes[0] = destination_bytesperlines[0] *
					       format_height;
			for (j = 1; j < destination_planes_count; j++)
				destination_sizes[j] = destination_sizes[0] / 2;
		}
	}

	/*
	 * iter5b-β Commit D: cache the format-uniform CAPTURE geometry
	 * in driver_data. CreateSurfaces2 calls AFTER this CreateContext
	 * (ffmpeg vaapi-copy late-surface-allocation case) will lazy-fill
	 * via surface_fill_format_uniform(); the surface_heap walk below
	 * fills surfaces that pre-existed when CreateContext fired.
	 */
	driver_data->fmt_planes_count = destination_planes_count;
	driver_data->fmt_buffers_count = video_format->v4l2_buffers_count;
	driver_data->fmt_format_height = format_height;
	for (j = 0; j < destination_planes_count; j++) {
		driver_data->fmt_sizes[j] = destination_sizes[j];
		driver_data->fmt_bytesperlines[j] =
			destination_bytesperlines[j];
	}
	driver_data->fmt_valid = true;

	/*
	 * Walk the surface_heap (not just surfaces_ids[]) to populate
	 * destination_* on every existing surface. Pre-Commit-D we walked
	 * surfaces_ids[], which is empty for ffmpeg vaapi-copy consumers
	 * that call vaCreateContext with surfaces_count=0 — those surfaces
	 * exist in the heap but aren't in the param array. Walking the
	 * heap catches both flows. Late-created surfaces (after this
	 * CreateContext) fill via surface_fill_format_uniform in
	 * CreateSurfaces2's per-surface init.
	 */
	{
		struct object_surface *surface_iter;
		int heap_iter;

		surface_iter = (struct object_surface *)
			object_heap_first(&driver_data->surface_heap,
					  &heap_iter);
		while (surface_iter != NULL) {
			surface_fill_format_uniform(driver_data, surface_iter);
			surface_iter = (struct object_surface *)
				object_heap_next(&driver_data->surface_heap,
						 &heap_iter);
		}
	}

	id = object_heap_allocate(&driver_data->context_heap);
	context_object = CONTEXT(driver_data, id);
	if (context_object == NULL) {
		status = VA_STATUS_ERROR_ALLOCATION_FAILED;
		goto error;
	}
	memset(&context_object->dpb, 0, sizeof(context_object->dpb));
	context_object->timestamp_counter = 0;	/* iter9 α-7 */
	/* Decode context: make sure the encoder slot is empty.  The object
	 * heap is recycled, so this is not implicitly zero. */
	context_object->encoder = NULL;

	/*
	 * Initialize the OUTPUT (bitstream-input) buffer pool. Sized by
	 * codec pipeline depth (4 H.264 frames in flight is sufficient
	 * for current hantro/rkvdec scheduling); independent of caller-
	 * supplied surfaces_count. Pool is owned by driver_data so it
	 * outlives any single context destroy/recreate cycle.
	 *
	 * This replaces the prior per-surface OUTPUT loop, which (a)
	 * created an empty queue when surfaces_count==0 (ffmpeg vaapi-
	 * copy path) and (b) only populated surface->source_* for
	 * surfaces present at vaCreateContext time, NULL-derefing on
	 * surfaces created later.
	 */
	/*
	 * iter6: pool size 16 gives comfortable headroom over typical H.264
	 * MaxDpbFrames (16) for any consumer that pipelines decode requests.
	 * Each slot owns its own request_fd (REINIT'd per use).
	 */
	rc = request_pool_init(&driver_data->output_pool,
			       driver_data->video_fd, driver_data->media_fd,
			       output_type, 16, pixelformat,
			       (unsigned int)picture_width,
			       (unsigned int)picture_height);
	if (rc < 0) {
		status = VA_STATUS_ERROR_ALLOCATION_FAILED;
		goto error;
	}

	/*
	 * The surface_ids array has been allocated by the caller and
	 * we don't have any indication wrt its life time. Let's make sure
	 * its life span is under our control.
	 */
	if (surfaces_count > 0) {
		ids = malloc(surfaces_count * sizeof(VASurfaceID));
		if (ids == NULL) {
			status = VA_STATUS_ERROR_ALLOCATION_FAILED;
			goto error;
		}

		memcpy(ids, surfaces_ids,
		       surfaces_count * sizeof(VASurfaceID));
	}

	/*
	 * Stateless H.264 device-wide controls. The kernel V4L2 stateless
	 * framework requires DECODE_MODE and START_CODE be set on the
	 * device fd (request_fd=-1) before VIDIOC_STREAMON; per-request
	 * controls (SPS/PPS/etc.) attached to a request_fd come later.
	 *
	 * hantro-vpu via rockchip,rk3568-vpu DT compatible (covers RK3568
	 * and RK3566 — PineTab2 silicon — since they're close enough)
	 * accepts only DECODE_MODE_FRAME_BASED.
	 * START_CODE_ANNEX_B preserves leading 0x00000001 in the slice
	 * payload that h264.c assembles. Errors here are not fatal: not
	 * every backing driver supports both controls (e.g. cedrus may
	 * default to SLICE_BASED without exposing DECODE_MODE).
	 */
	{
		struct v4l2_ext_control dev_ctrls[2] = {
			{
				.id = V4L2_CID_STATELESS_H264_DECODE_MODE,
				.value = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
			},
			{
				.id = V4L2_CID_STATELESS_H264_START_CODE,
				.value = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
			},
		};
		(void)v4l2_set_controls(driver_data->video_fd, -1,
					dev_ctrls, 2);
	}

	/*
	 * iter2: HEVC device-wide controls. Same best-effort pattern as
	 * H.264 above — separate batched call so a kernel that does not
	 * advertise HEVC controls (e.g. hantro-vpu-dec on RK3568/RK3399)
	 * silently fails on this batch without invalidating the H.264
	 * batch. rkvdec on RK3399 advertises HEVC and accepts FRAME_BASED
	 * + ANNEX_B (only supported menu values per Phase 0 v4l2_inventory).
	 */
	{
		/*
		 * iter40: per-driver HEVC start_code menu value. rkvdec /
		 * hantro path uses ANNEX_B + start-code-prepended payload.
		 * rpi-hevc-dec uses NONE — confirmed empirically Phase 7
		 * (any other mode → V4L2_BUF_FLAG_ERROR on every CAPTURE
		 * DQBUF, all-zero output). kdirect's strace also shows
		 * start_code=0 on rpi-hevc-dec. Both are accepted by the
		 * driver's QUERY_EXT_CTRL menu (min=0 max=1), but only NONE
		 * actually drives correct decode on the Pi.
		 */
		bool is_rpi = (driver_data->video_fd ==
			       driver_data->video_fd_rpi_hevc_dec);
		struct v4l2_ext_control hevc_dev_ctrls[2] = {
			{
				.id = V4L2_CID_STATELESS_HEVC_DECODE_MODE,
				.value = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
			},
			{
				.id = V4L2_CID_STATELESS_HEVC_START_CODE,
				.value = is_rpi
					? 0 /* V4L2_STATELESS_HEVC_START_CODE_NONE */
					: V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
			},
		};
		(void)v4l2_set_controls(driver_data->video_fd, -1,
					hevc_dev_ctrls, 2);
	}

	/*
	 * Mirror the ANNEX_B start-code mode set on the device above
	 * into context_object->h264_start_code so picture.c::
	 * codec_store_buffer prepends 0x00 0x00 0x01 to each slice
	 * payload it copies into the OUTPUT buffer. Without this, the
	 * kernel — which we just told to expect ANNEX_B — sees a raw
	 * NAL stream with no start codes, fails to find slice
	 * boundaries, and emits a zeroed CAPTURE buffer (visually a
	 * flat dark-green frame).
	 *
	 * iter4 fix: this start-code prepend is ANNEX-B-specific and
	 * applies to H.264 and HEVC ONLY. MPEG-2, VP8, and VP9 use raw
	 * frame bitstreams without start codes — prepending 0x00 0x00 0x01
	 * to a VP9 uncompressed header produces a frame_marker mismatch
	 * (kernel reads 0x00 instead of 0x10), the rkvdec driver silently
	 * fails to find a valid frame, and the CAPTURE slot stays at its
	 * cap_pool init pattern (a dim 0x4c green). Phase 7 verification
	 * caught this for VP9; iter1+iter3 transitive proof masked it for
	 * MPEG-2/VP8 because those iters compared payload bytes, not
	 * decoded pixels.
	 *
	 * h264_get_controls() exists for this purpose but is never
	 * called in the current code path; the planned probe-then-set
	 * commit will replace this hardcoded assignment with a runtime
	 * read of the kernel's accepted START_CODE value.
	 */
	{
		bool is_rpi = (driver_data->video_fd ==
			       driver_data->video_fd_rpi_hevc_dec);
		switch (config_object->profile) {
		case VAProfileH264Main:
		case VAProfileH264High:
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264MultiviewHigh:
		case VAProfileH264StereoHigh:
			context_object->h264_start_code = true;
			break;
		case VAProfileHEVCMain:
			/* iter40: rpi-hevc-dec rejects start-code-prepended
			 * payload (DQBUF error flag on every CAPTURE buffer).
			 * Gate to match the per-driver START_CODE menu value
			 * set above: NONE on rpi → no prepend; ANNEX_B on
			 * rkvdec → prepend. */
			context_object->h264_start_code = !is_rpi;
			break;
		default:
			context_object->h264_start_code = false;
			break;
		}
	}

	rc = v4l2_set_stream(driver_data->video_fd, output_type, true);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	rc = v4l2_set_stream(driver_data->video_fd, capture_type, true);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	context_object->config_id = config_id;
	context_object->render_surface_id = VA_INVALID_ID;
	context_object->surfaces_ids = ids;
	context_object->surfaces_count = surfaces_count;
	context_object->picture_width = picture_width;
	context_object->picture_height = picture_height;
	context_object->flags = flags;

	*context_id = id;

	status = VA_STATUS_SUCCESS;
	goto complete;

error:
	if (ids != NULL)
		free(ids);

	if (context_object != NULL)
		object_heap_free(&driver_data->context_heap,
				 (struct object_base *)context_object);

complete:
	return status;
}

VAStatus RequestDestroyContext(VADriverContextP context, VAContextID context_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *context_object;
	struct video_format *video_format;
	unsigned int output_type, capture_type;
	VAStatus status;
	int rc;

	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	/*
	 * Encoder contexts tear down only their own V4L2 queues and buffers;
	 * the decoder-side format cache / pools belong to other sessions.
	 */
	if (context_object->encoder != NULL) {
		encoder_context_destroy(context_object->encoder);
		context_object->encoder = NULL;
		driver_data->encode_active = false;

		free(context_object->surfaces_ids);
		context_object->surfaces_ids = NULL;
		context_object->surfaces_count = 0;

		object_heap_free(&driver_data->context_heap,
				 (struct object_base *)context_object);
		return VA_STATUS_SUCCESS;
	}

	video_format = driver_data->video_format;
	if (video_format == NULL)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	output_type = v4l2_type_video_output(video_format->v4l2_mplane);
	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	rc = v4l2_set_stream(driver_data->video_fd, output_type, false);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	rc = v4l2_set_stream(driver_data->video_fd, capture_type, false);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/* Buffers liberation */

	/*
	 * Do NOT destroy the surfaces here: VA surfaces are owned by the
	 * application (vaCreateSurfaces/vaDestroySurfaces), not by the
	 * context.  Freeing them on vaDestroyContext made a subsequent
	 * vaDestroySurfaces() free the same object heap entry again, which
	 * corrupts the heap (seen as SIGSEGV/SIGBUS/SIGABRT in FFmpeg when
	 * it re-initialises the decoder, e.g. with -stream_loop).
	 *
	 * Their capture-pool bindings must be dropped before the pool is
	 * destroyed below, otherwise a later vaDestroySurfaces() would
	 * release slots into freed pool storage.
	 */
	{
		struct object_surface *surface_object;
		int iterator;

		surface_object = (struct object_surface *)
			object_heap_first(&driver_data->surface_heap, &iterator);
		while (surface_object != NULL) {
			surface_unbind_slot(driver_data, surface_object);
			surface_object = (struct object_surface *)
				object_heap_next(&driver_data->surface_heap,
						 &iterator);
		}
	}

	free(context_object->surfaces_ids);
	context_object->surfaces_ids = NULL;
	context_object->surfaces_count = 0;

	object_heap_free(&driver_data->context_heap,
			 (struct object_base *)context_object);

	/*
	 * iter5b-β: tear down the OUTPUT pool (mmap unmaps) BEFORE
	 * REQBUFS(0) frees the kernel-side buffers. Pre-β this was done
	 * only by surface.c's resolution-change branch — which β removed.
	 * Without this here, the next CreateContext's request_pool_init
	 * sees pool->initialized=true with stale slot pointers, returns
	 * 0 without re-CREATE_BUFS, and the next QBUF EINVALs because
	 * the slots reference buffer indices that no longer exist
	 * (Phase 5 v2 review CRIT-2).
	 */
	if (driver_data->output_pool.initialized)
		request_pool_destroy(&driver_data->output_pool);

	rc = v4l2_request_buffers(driver_data->video_fd, output_type, 0);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/*
	 * Iter2 Fix 3 (still relevant under β): cap_pool owns the
	 * CAPTURE buffers' mmaps + any outstanding our_export_fds. Tear
	 * it down (which also issues REQBUFS(0) on CAPTURE), so the next
	 * CreateContext cycle sees a clean slate.
	 */
	cap_pool_destroy(&driver_data->capture_pool, driver_data->video_fd,
			 capture_type);

	/*
	 * iter5b-β: driver_data->video_format is a static-ref pointer
	 * into video.c's formats[]; it stays valid for the life of the
	 * driver_data and intentionally survives DestroyContext cycles.
	 * The next CreateContext's `if (!driver_data->video_format)`
	 * guard skips the probe — correct, because the device's CAPTURE
	 * format menu doesn't change.
	 *
	 * The pre-β surface_reset_format_cache() call here is removed:
	 * β doesn't have a last_output_{width,height,pixelformat} cache
	 * (those fields are deleted). Each CreateContext is a fresh
	 * S_FMT(OUTPUT) cycle.
	 *
	 * Commit D: invalidate the format-uniform cache so a CreateSurfaces2
	 * call between DestroyContext and the next CreateContext doesn't
	 * lazy-fill with stale geometry from the now-torn-down session.
	 * The next CreateContext re-populates the cache.
	 */
	driver_data->fmt_valid = false;
	/* iter39: clear 10-bit session flag — next CreateContext re-sets. */
	driver_data->is_10bit = false;

	return VA_STATUS_SUCCESS;
}
