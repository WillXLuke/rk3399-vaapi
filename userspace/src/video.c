/*
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

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include <drm_fourcc.h>
#include <linux/videodev2.h>

#include "nv12_col128.h"  /* fallback V4L2_PIX_FMT_NV12_COL128 define */
#include "nv15.h"         /* fallback V4L2_PIX_FMT_NV15 define */
#include "utils.h"
#include "video.h"

static struct video_format formats[] = {
	{
		.description		= "NV12 YUV",
		.v4l2_format		= V4L2_PIX_FMT_NV12,
		.v4l2_buffers_count	= 1,
		.v4l2_mplane		= true,
		.drm_format		= DRM_FORMAT_NV12,
		.drm_modifier		= DRM_FORMAT_MOD_NONE,
		.planes_count		= 2,
		.bpp			= 16,
	},
	{
		.description		= "NV15 YUV (10-bit, rkvdec)",
		.v4l2_format		= V4L2_PIX_FMT_NV15,
		.v4l2_buffers_count	= 1,
		.v4l2_mplane		= true,
		.drm_format		= DRM_FORMAT_NV15,
		.drm_modifier		= DRM_FORMAT_MOD_NONE,
		.planes_count		= 2,
		.bpp			= 24,
	},
	{
		/*
		 * iter40: Pi 5 / CM5 rpi-hevc-dec CAPTURE format. 8-bit NV12
		 * stored as 128-pixel-wide column tiles (SAND128 layout).
		 * Pi-specific; not in mainline drm_fourcc.h (uses NV12 + a
		 * BROADCOM_SAND128 modifier for DRM_PRIME). Our consumer path
		 * always detiles to linear NV12 in copy_surface_to_image, so
		 * we don't expose the SAND modifier downstream — drm_format is
		 * still DRM_FORMAT_NV12 and drm_modifier MOD_NONE so the
		 * format-is-linear gate doesn't pull us into tiled_to_planar
		 * (Sunxi-specific). image.c branches on v4l2_format ==
		 * V4L2_PIX_FMT_NV12_COL128 to invoke the dedicated detile.
		 */
		.description		= "NV12 SAND128 (8-bit, rpi-hevc-dec)",
		.v4l2_format		= V4L2_PIX_FMT_NV12_COL128,
		.v4l2_buffers_count	= 1,
		.v4l2_mplane		= true,
		.drm_format		= DRM_FORMAT_NV12,
		.drm_modifier		= DRM_FORMAT_MOD_NONE,
		.planes_count		= 2,
		.bpp			= 16,
	},
// Code to handle this DRM_FORMAT is __arm__ only
#ifdef __arm__
	{
		.description		= "Sunxi tiled NV12 YUV",
		.v4l2_format		= V4L2_PIX_FMT_SUNXI_TILED_NV12,
		.v4l2_buffers_count	= 1,
		.v4l2_mplane		= false,
		.drm_format		= DRM_FORMAT_NV12,
		.drm_modifier		= DRM_FORMAT_MOD_ALLWINNER_TILED,
		.planes_count		= 2,
		.bpp			= 16
	},
#endif
};

static unsigned int formats_count = sizeof(formats) / sizeof(formats[0]);

struct video_format *video_format_find(unsigned int pixelformat)
{
	unsigned int i;

	for (i = 0; i < formats_count; i++)
		if (formats[i].v4l2_format == pixelformat)
			return &formats[i];

	return NULL;
}

bool video_format_is_linear(struct video_format *format)
{
	if (format == NULL)
		return true;

	return format->drm_modifier == DRM_FORMAT_MOD_NONE;
}
