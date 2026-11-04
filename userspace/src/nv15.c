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

#include "nv15.h"

void nv15_unpack_plane_to_p010(const uint8_t *src, uint16_t *dst,
			       unsigned int width, unsigned int height,
			       unsigned int src_stride)
{
	unsigned int x, y;
	unsigned int dst_pitch_px = width;

	for (y = 0; y < height; y++) {
		const uint8_t *s = src + y * src_stride;
		uint16_t *d = dst + y * dst_pitch_px;

		for (x = 0; x + 4 <= width; x += 4) {
			uint16_t a = (uint16_t)s[0] | ((uint16_t)(s[1] & 0x03) << 8);
			uint16_t b = ((uint16_t)s[1] >> 2) | ((uint16_t)(s[2] & 0x0F) << 6);
			uint16_t c = ((uint16_t)s[2] >> 4) | ((uint16_t)(s[3] & 0x3F) << 4);
			uint16_t e = ((uint16_t)s[3] >> 6) | ((uint16_t)s[4] << 2);

			d[0] = (uint16_t)(a << 6);
			d[1] = (uint16_t)(b << 6);
			d[2] = (uint16_t)(c << 6);
			d[3] = (uint16_t)(e << 6);

			d += 4;
			s += 5;
		}

		if (x < width) {
			unsigned int rem = width - x;
			uint16_t pix[4] = { 0, 0, 0, 0 };

			pix[0] = (uint16_t)s[0] | ((uint16_t)(s[1] & 0x03) << 8);
			if (rem >= 2)
				pix[1] = ((uint16_t)s[1] >> 2) |
					 ((uint16_t)(s[2] & 0x0F) << 6);
			if (rem >= 3)
				pix[2] = ((uint16_t)s[2] >> 4) |
					 ((uint16_t)(s[3] & 0x3F) << 4);
			if (rem >= 4)
				pix[3] = ((uint16_t)s[3] >> 6) |
					 ((uint16_t)s[4] << 2);

			{
				unsigned int j;
				for (j = 0; j < rem; j++)
					d[j] = (uint16_t)(pix[j] << 6);
			}
		}
	}
}
