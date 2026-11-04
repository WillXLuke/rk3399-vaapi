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

#ifndef _V4L2_REQUEST_H_
#define _V4L2_REQUEST_H_

#include <stdbool.h>

#include "context.h"
#include "object_heap.h"
#include "request_pool.h"
#include "cap_pool.h"
#include "video.h"
#include <va/va.h>

#include <linux/videodev2.h>

#include "hevc-ctrls/v4l2-hevc-ext-controls.h"

#define V4L2_REQUEST_STR_VENDOR			"v4l2-request"

/*
 * Sized for max-possible enumeration with iter39 Option B reverted:
 * MPEG2(2) + H264(6 incl. Hi10P) + HEVC(2 incl. Main10) + VP8 + VP9 + AV1 = 13.
 * The per-group guards use `if (... && index < (MAX_PROFILES - N))` where N
 * is the push-group size, so MAX must be ≥ total+1 — 14 here. Bumping
 * defensively now so a future re-enable of Hi10P/Main10 doesn't silently
 * drop AV1 through the off-by-one trap that ate ampere-av1's enumeration
 * for a week (see issue marfrit/libva-v4l2-request-fourier#2).
 */
#define V4L2_REQUEST_MAX_PROFILES		14
#define V4L2_REQUEST_MAX_ENTRYPOINTS		5
#define V4L2_REQUEST_MAX_CONFIG_ATTRIBUTES	10
#define V4L2_REQUEST_MAX_IMAGE_FORMATS		10
#define V4L2_REQUEST_MAX_SUBPIC_FORMATS		4
#define V4L2_REQUEST_MAX_DISPLAY_ATTRIBUTES	4

struct request_data {
	struct object_heap config_heap;
	struct object_heap context_heap;
	struct object_heap surface_heap;
	struct object_heap buffer_heap;
	struct object_heap image_heap;
	int video_fd;
	int media_fd;

	/*
	 * iter38: multi-device probe. RK3399 has two V4L2 stateless decoders:
	 *   - rkvdec → H264 / HEVC / VP9
	 *   - hantro-vpu (rk3399-vpu-dec) → MPEG-2 / VP8
	 * At VA_DRIVER_INIT we probe both, open their fds, and store them
	 * here. driver_data->video_fd / media_fd above are the "active" fds
	 * (point at one of the pairs below). RequestCreateConfig retargets
	 * them based on the profile's required device. Pools and video_format
	 * are torn down at retarget time so the next CreateContext rebuilds
	 * them against the right device.
	 *
	 * -1 means that device kind isn't present on this kernel boot.
	 * Honours LIBVA_V4L2_REQUEST_VIDEO_PATH / MEDIA_PATH explicit
	 * overrides — when those are set, only the single requested device
	 * is opened and the alt fds stay -1.
	 */
	int video_fd_rkvdec;
	int media_fd_rkvdec;
	int video_fd_hantro;
	int media_fd_hantro;
	/*
	 * iter40: third multi-device-probe slot for rpi-hevc-dec (Pi 5 /
	 * CM5 / BCM2712). V4L2 stateless HEVC; CAPTURE is NC12/NC30 SAND
	 * 128-pixel-wide column tiled (Pi-specific). On Pi 5 this is the
	 * ONLY decoder slot; on RK hosts it stays -1 and HEVC routes to
	 * rkvdec as before.
	 */
	int video_fd_rpi_hevc_dec;
	int media_fd_rpi_hevc_dec;
	/*
	 * phase 8.10: fifth multi-device-probe slot for daedalus_v4l2 — the
	 * out-of-tree V4L2 stateless decoder shim that forwards bitstream
	 * to a userspace daemon (daedalus-v4l2 sibling repo). Daemon does
	 * FFmpeg-software decode for VP9 / AV1 / H.264 and ships pixels
	 * back via dmabuf into the CAPTURE buffer.  Picked up via the
	 * same media-controller probe + known_decoder_drivers[] entry
	 * pattern as iter40 rpi-hevc-dec.  Stays -1 on hosts without the
	 * daedalus module loaded; HEVC routes to rpi-hevc-dec as before.
	 *
	 * Fields are unconditional (8 bytes per session) so the struct
	 * layout is stable regardless of meson option.  The active
	 * probe + dispatch code in request.c is gated by
	 * HAVE_DAEDALUS_V4L2; when disabled the fields stay at their
	 * -1 init and no codepath touches them.
	 */
	int video_fd_daedalus;
	int media_fd_daedalus;
	/*
	 * ampere-av1-enablement Phase 2: fourth multi-device-probe slot
	 * for vpu981 (RK3588's dedicated AV1 hantro instance, kernel
	 * card="rockchip,rk3588-av1-vpu-dec", driver name "hantro-vpu" —
	 * shared with the legacy MPEG-2/VP8/H.264 hantro). Discriminated
	 * by V4L2_PIX_FMT_AV1_FRAME (AV1F) OUTPUT-pixfmt capability since
	 * the driver name alone is ambiguous on RK3588. Stays -1 on hosts
	 * without the AV1 vpu-dec.
	 *
	 * Named "vpu981" for consistency with the in-progress av1-iter1
	 * operator branch (Phase 3-5 bit-exact AV1 work — when that lands
	 * these fields receive the actual decode dispatch wiring).
	 */
	int video_fd_vpu981;
	int media_fd_vpu981;

	/*
	 * RK3399 VEPU2 H.264 encoder instance (hantro-vpu,
	 * card rockchip,rk3399-vpu-enc, CAPTURE V4L2_PIX_FMT_H264_SLICE).
	 * Probed independently of the decoder slots at VA init; -1 when the
	 * board has no encoder.  Encoder contexts open their own V4L2 queues
	 * on this fd pair and never touch the decoder state.
	 */
	int video_fd_enc;
	int media_fd_enc;

	/*
	 * True while an encoder VA context is active.  image.c uses it to
	 * size vaCreateImage()/vaDeriveImage() from the surface geometry
	 * instead of the (irrelevant) decoder CAPTURE format.
	 */
	bool encode_active;

	/*
	 * iter2 (ampere-kernel-decoders campaign) — per-fd probe result
	 * for the V4L2_CID_STATELESS_HEVC_EXT_SPS_{ST,LT}_RPS controls
	 * introduced in Linux 7.0 (Casanova VDPU381/VDPU383 series).
	 * RK3399 rkvdec doesn't have them and the probe returns false;
	 * RK3588 rkvdec (VDPU381/383) registers them and the probe is
	 * true. h265_set_controls consults only the rkvdec entry because
	 * HEVC routes through rkvdec only — hantro's entry stays false
	 * naturally (it doesn't have rkvdec-specific controls).
	 *
	 * The pair-of-flags layout mirrors video_fd_rkvdec /
	 * video_fd_hantro above (iter38 multi-device-probe pattern,
	 * memory feedback_multi_device_probe_design). Phase 5 review
	 * surfaced this as a correctness item: a single scalar on
	 * driver_data would silently misbehave across device-switch
	 * boundaries; per-fd storage is the safe shape.
	 */
	bool has_hevc_ext_sps_rps_rkvdec;
	bool has_hevc_ext_sps_rps_hantro;
	/* iter40: rpi-hevc-dec doesn't expose EXT_SPS_*_RPS controls
	 * (verified Phase 0 higgs probe: QUERY_EXT_CTRL on 0xa97 → EINVAL).
	 * Probed for consistency with the iter2 pair-of-flags pattern;
	 * stays false on Pi 5 and the iter2 vendored-parser path naturally
	 * doesn't engage. */
	bool has_hevc_ext_sps_rps_rpi_hevc_dec;

	/*
	 * iter2 — cached SPS-derived RPS arrays. SPS NALs only appear in
	 * source_data on IDR frames; non-IDR frames' h265_set_controls
	 * reuse the cached arrays so we don't submit zero-filled RPS to
	 * the kernel (which would re-trigger the OOPS the iter2 fix is
	 * designed to prevent). Single-slot cache (sps_id 0 only) —
	 * adequate for the BBB / typical-stream case; multi-SPS streams
	 * would need expanding to a [16] cache keyed by sps_id.
	 *
	 * The cache stores the post-mapped V4L2 control struct arrays
	 * (not the intermediate GstH265SPS) so request.h doesn't need
	 * to know about the vendored GStreamer parser types — only the
	 * V4L2 UAPI structs from hevc-ctrls/v4l2-hevc-ext-controls.h
	 * included above.
	 *
	 * Owned by h265.c; freed at RequestTerminate.
	 */
	struct v4l2_ctrl_hevc_ext_sps_st_rps *hevc_rps_cache_st;
	unsigned int                          hevc_rps_cache_st_count;
	struct v4l2_ctrl_hevc_ext_sps_lt_rps *hevc_rps_cache_lt;
	unsigned int                          hevc_rps_cache_lt_count;
	bool                                  hevc_rps_cache_valid;

	/*
	 * iter40b: bitstream-derived SPS field cache for VAAPI-omitted
	 * fields. rpi-hevc-dec validates these against bitstream-true
	 * values; the rkvdec/hantro fallback (sps_max_dec_pic_buffering_minus1,
	 * 0) that satisfies §A.4.2 isn't enough for rpi.
	 *
	 * Cached on first IDR frame's SPS NAL parse, reused for subsequent
	 * non-IDR frames whose source_data may not carry an SPS.
	 *
	 * sps_max_sub_layers_minus1 is the index into max_*[] arrays. The
	 * V4L2 SPS struct fields are scalars (single sublayer), so we pick
	 * the HighestTid (= sps_max_sub_layers_minus1) slot — matches
	 * ffmpeg-vaapi + kdirect convention.
	 */
	struct {
		bool valid;
		uint8_t sps_max_sub_layers_minus1;
		uint8_t max_dec_pic_buffering_minus1;
		uint8_t max_num_reorder_pics;
		uint8_t max_latency_increase_plus1;
		bool scaling_list_enabled;
		bool scaling_list_data_present;
	} hevc_sps_field_cache;

	struct video_format *video_format;

	/*
	 * OUTPUT (bitstream-input) buffer pool, decoupled from VA
	 * surfaces. Sized by codec pipeline depth, populated on first
	 * RequestCreateContext, torn down at driver Terminate.
	 */
	struct request_pool output_pool;

	/*
	 * CAPTURE (decoded-frame) buffer pool, decoupled from VA
	 * surfaces (iter2 Fix 3). Each surface acquires a slot at
	 * vaBeginPicture time and releases it on the next acquisition
	 * or vaDestroySurfaces. Pool sized to max(surfaces_count,
	 * MIN_CAP_POOL) at first vaCreateSurfaces2; torn down at
	 * vaDestroyContext.
	 *
	 * Background: pre-iter2 each surface was 1:1 bound to one
	 * CAPTURE buffer index; mpv re-using a surface for a new decode
	 * caused V4L2 to re-QBUF the same physical buffer while a
	 * compositor still held an EXPBUF'd dma_buf fd, producing
	 * visible stutter on mpv vaapi --vo=gpu.
	 */
	struct cap_pool capture_pool;

	/*
	 * iter5b-β: the pre-β last_output_{width,height} cache fields
	 * and surface_reset_format_cache() helper are deleted. They
	 * existed because CreateSurfaces2 owned the OUTPUT-side V4L2
	 * device-format lifecycle and needed to gate re-S_FMT on
	 * resolution change. β moves that lifecycle to CreateContext,
	 * which is naturally one-shot per context cycle; no caching is
	 * required. DestroyContext + next CreateContext rebuild from
	 * scratch.
	 *
	 * iter5b-β Commit D: cache the format-uniform CAPTURE-side
	 * geometry from v4l2_get_format so CreateSurfaces2 can populate
	 * a newly-created surface's destination_* fields without
	 * re-querying the device. Set by CreateContext after the
	 * v4l2_get_format(CAPTURE) call; consumed by both:
	 *   1. CreateContext's surface_heap walk (fills surfaces that
	 *      pre-exist when CreateContext fires);
	 *   2. CreateSurfaces2's per-surface init (fills surfaces
	 *      created AFTER CreateContext, e.g. ffmpeg vaapi-copy
	 *      pool dynamics where the consumer passes surfaces_count=0
	 *      to vaCreateContext and creates surfaces lazily).
	 *
	 * fmt_valid is true once CreateContext has populated the cache;
	 * CreateSurfaces2 only lazy-fills when fmt_valid is true.
	 */
	bool fmt_valid;
	unsigned int fmt_format_height;
	unsigned int fmt_planes_count;
	unsigned int fmt_buffers_count;
	unsigned int fmt_sizes[VIDEO_MAX_PLANES];
	unsigned int fmt_bytesperlines[VIDEO_MAX_PLANES];

	/*
	 * iter39: active session is decoding a 10-bit profile (Hi10P / Main10).
	 * Set in RequestCreateContext from config->profile. Drives:
	 *   - CAPTURE pix_fmt selection (NV15 instead of NV12)
	 *   - image.c DeriveImage / QueryImageFormats fourcc reporting (P010
	 *     instead of NV12)
	 *   - copy_surface_to_image NV15→P010 unpack branch
	 * Reset to false at DestroyContext.
	 */
	bool is_10bit;
};

VAStatus VA_DRIVER_INIT_FUNC(VADriverContextP context);
VAStatus RequestTerminate(VADriverContextP context);

/*
 * iter38: retarget driver_data->{video,media}_fd to the device required by
 * `profile`. Returns 0 on success, -1 on profile not mappable to any kind.
 * Defined in request.c.
 */
int request_switch_device_for_profile(struct request_data *driver_data,
				      VAProfile profile);

#endif
