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

#ifndef _NV15_H_
#define _NV15_H_

#include <stdint.h>

#include <linux/videodev2.h>

/*
 * Older or downstream linux-api-headers / kernel-headers packages may
 * not define V4L2_PIX_FMT_NV15. Provide a fallback so the backend
 * builds on hosts whose headers are pre-NV15-merge or omit it (e.g.
 * Pi 5 Debian trixie 6.12.62 headers include NC12 but not NV15).
 * Same numeric value as mainline.
 */
#ifndef V4L2_PIX_FMT_NV15
#define V4L2_PIX_FMT_NV15  \
	((unsigned int)('N') | ((unsigned int)('V') << 8) | \
	 ((unsigned int)('1') << 16) | ((unsigned int)('5') << 24))
#endif

/*
 * Unpack one plane of V4L2_PIX_FMT_NV15 (4 × 10-bit values packed into
 * 5 consecutive bytes, LSB-first) into VA_FOURCC_P010 (16-bit per pixel,
 * value in bits [15:6], zeros in [5:0]).
 *
 * Layout per Documentation/userspace-api/media/v4l/pixfmt-nv15.rst.
 * Call once per plane: luma (W × H, src_stride = ceil(W/4)*5) and chroma
 * (W × H/2 — same width because UV are interleaved 10-bit values).
 *
 * src_stride must be the kernel-reported bytesperline for the NV15 plane.
 * The destination is dense P010 with row pitch = width * 2 bytes.
 */
void nv15_unpack_plane_to_p010(const uint8_t *src, uint16_t *dst,
			       unsigned int width, unsigned int height,
			       unsigned int src_stride);

#endif
