/*
 * Copyright (C) 2026 claude-noether <claude-noether@reauktion.de>
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

#include "codec.h"

#include <linux/videodev2.h>

unsigned int pixelformat_for_profile(VAProfile profile)
{
	switch (profile) {
	case VAProfileMPEG2Simple:
	case VAProfileMPEG2Main:
		return V4L2_PIX_FMT_MPEG2_SLICE;
	case VAProfileH264Main:
	case VAProfileH264High:
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264MultiviewHigh:
	case VAProfileH264StereoHigh:
	case VAProfileH264High10:
		return V4L2_PIX_FMT_H264_SLICE;
	case VAProfileHEVCMain:
	case VAProfileHEVCMain10:
		return V4L2_PIX_FMT_HEVC_SLICE;
	case VAProfileVP8Version0_3:
		return V4L2_PIX_FMT_VP8_FRAME;
	case VAProfileVP9Profile0:
		return V4L2_PIX_FMT_VP9_FRAME;
	case VAProfileAV1Profile0:
		/*
		 * ampere-av1-enablement Phase 2: AV1 Profile 0 routes to
		 * vpu981 (RK3588's dedicated AV1 hantro). Per-codec ctrl
		 * dispatch (V4L2_CID_STATELESS_AV1_*) is NOT YET WIRED on
		 * master — vainfo lists the profile + RequestCreateConfig
		 * succeeds, but consumers that submit decode buffers hit
		 * a NOP path until the per-codec dispatch lands. The
		 * av1-iter1 operator branch has Phase 3 bit-exact bring-up
		 * underway; this commit gives master the bare enumeration +
		 * fd-routing layer so consumers like ffmpeg-vaapi at least
		 * see VAProfileAV1Profile0 today.
		 */
		return V4L2_PIX_FMT_AV1_FRAME;
	default:
		return 0;
	}
}
