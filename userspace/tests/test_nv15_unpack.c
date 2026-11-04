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

/*
 * iter39 self-test for nv15_unpack_plane_to_p010.
 *
 * Builds NV15 plane buffers from known 10-bit pixel arrays, runs the
 * unpack, asserts P010 output matches the expected pixel<<6 values.
 * No hardware needed — pure bit layout verification per
 * Documentation/userspace-api/media/v4l/pixfmt-nv15.rst.
 *
 * Build:
 *   cc -Wall -Werror -O2 -o test_nv15_unpack tests/test_nv15_unpack.c src/nv15.c
 *
 * Exit 0 = all asserts pass.
 */

#include "../src/nv15.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pack 4 10-bit pixels into 5 bytes per NV15 layout (LSB-first across
 * bits 0..39). Inverse of nv15_unpack_plane_to_p010's per-group unpack. */
static void pack4(uint16_t a, uint16_t b, uint16_t c, uint16_t d,
		  uint8_t out[5])
{
	out[0] = (uint8_t)(a & 0xFF);
	out[1] = (uint8_t)(((a >> 8) & 0x03) | ((b & 0x3F) << 2));
	out[2] = (uint8_t)(((b >> 6) & 0x0F) | ((c & 0x0F) << 4));
	out[3] = (uint8_t)(((c >> 4) & 0x3F) | ((d & 0x03) << 6));
	out[4] = (uint8_t)((d >> 2) & 0xFF);
}

#define ASSERT_EQ(actual, expected, msg) do {				\
	if ((actual) != (expected)) {					\
		fprintf(stderr, "FAIL %s: actual=0x%04x expected=0x%04x at %s:%d\n", \
			(msg), (unsigned)(actual), (unsigned)(expected), \
			__FILE__, __LINE__);				\
		exit(1);						\
	}								\
} while (0)

static void test_pack_unpack_roundtrip(uint16_t a, uint16_t b, uint16_t c,
				       uint16_t d)
{
	uint8_t packed[5];
	uint16_t dst[4];

	pack4(a, b, c, d, packed);
	nv15_unpack_plane_to_p010(packed, dst, 4, 1, 5);
	ASSERT_EQ(dst[0], (uint16_t)(a << 6), "roundtrip a");
	ASSERT_EQ(dst[1], (uint16_t)(b << 6), "roundtrip b");
	ASSERT_EQ(dst[2], (uint16_t)(c << 6), "roundtrip c");
	ASSERT_EQ(dst[3], (uint16_t)(d << 6), "roundtrip d");
}

static void test_zero(void)
{
	uint8_t packed[5] = { 0, 0, 0, 0, 0 };
	uint16_t dst[4] = { 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD };
	nv15_unpack_plane_to_p010(packed, dst, 4, 1, 5);
	ASSERT_EQ(dst[0], 0, "zero[0]");
	ASSERT_EQ(dst[1], 0, "zero[1]");
	ASSERT_EQ(dst[2], 0, "zero[2]");
	ASSERT_EQ(dst[3], 0, "zero[3]");
}

static void test_all_max(void)
{
	/* All four pixels = 0x3FF (max 10-bit). Packed bits all 1 → all 0xFF. */
	uint8_t packed[5] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	uint16_t dst[4] = { 0, 0, 0, 0 };
	nv15_unpack_plane_to_p010(packed, dst, 4, 1, 5);
	ASSERT_EQ(dst[0], 0xFFC0, "max[0]");
	ASSERT_EQ(dst[1], 0xFFC0, "max[1]");
	ASSERT_EQ(dst[2], 0xFFC0, "max[2]");
	ASSERT_EQ(dst[3], 0xFFC0, "max[3]");
}

static void test_known_vectors(void)
{
	/* Position-sensitive sanity: each pixel = its index+1. */
	test_pack_unpack_roundtrip(1, 2, 3, 4);
	/* Spread patterns that exercise every byte-boundary bit. */
	test_pack_unpack_roundtrip(0x3FF, 0x000, 0x3FF, 0x000);
	test_pack_unpack_roundtrip(0x000, 0x3FF, 0x000, 0x3FF);
	test_pack_unpack_roundtrip(0x155, 0x2AA, 0x155, 0x2AA);
	test_pack_unpack_roundtrip(0x001, 0x002, 0x004, 0x008);
	test_pack_unpack_roundtrip(0x080, 0x040, 0x020, 0x010);
	test_pack_unpack_roundtrip(0x200, 0x100, 0x080, 0x040);
	test_pack_unpack_roundtrip(0x3F0, 0x0F3, 0x33C, 0x2A5);
}

static void test_remainder_width(void)
{
	/* width=1: only A unpacked, B/C/D undefined */
	{
		uint8_t packed[5];
		uint16_t dst[1] = { 0xDEAD };
		pack4(0x123, 0x000, 0x000, 0x000, packed);
		nv15_unpack_plane_to_p010(packed, dst, 1, 1, 5);
		ASSERT_EQ(dst[0], 0x123 << 6, "rem1[0]");
	}
	/* width=2 */
	{
		uint8_t packed[5];
		uint16_t dst[2] = { 0, 0 };
		pack4(0x111, 0x222, 0x000, 0x000, packed);
		nv15_unpack_plane_to_p010(packed, dst, 2, 1, 5);
		ASSERT_EQ(dst[0], 0x111 << 6, "rem2[0]");
		ASSERT_EQ(dst[1], 0x222 << 6, "rem2[1]");
	}
	/* width=3 */
	{
		uint8_t packed[5];
		uint16_t dst[3] = { 0, 0, 0 };
		pack4(0x111, 0x222, 0x333, 0x000, packed);
		nv15_unpack_plane_to_p010(packed, dst, 3, 1, 5);
		ASSERT_EQ(dst[0], 0x111 << 6, "rem3[0]");
		ASSERT_EQ(dst[1], 0x222 << 6, "rem3[1]");
		ASSERT_EQ(dst[2], 0x333 << 6, "rem3[2]");
	}
	/* width=7: one full group + 3 remainder */
	{
		uint8_t packed[10];
		uint16_t dst[7] = { 0 };
		pack4(0x100, 0x200, 0x300, 0x010, &packed[0]);
		pack4(0x011, 0x022, 0x033, 0x000, &packed[5]);
		nv15_unpack_plane_to_p010(packed, dst, 7, 1, 10);
		ASSERT_EQ(dst[0], 0x100 << 6, "rem7[0]");
		ASSERT_EQ(dst[1], 0x200 << 6, "rem7[1]");
		ASSERT_EQ(dst[2], 0x300 << 6, "rem7[2]");
		ASSERT_EQ(dst[3], 0x010 << 6, "rem7[3]");
		ASSERT_EQ(dst[4], 0x011 << 6, "rem7[4]");
		ASSERT_EQ(dst[5], 0x022 << 6, "rem7[5]");
		ASSERT_EQ(dst[6], 0x033 << 6, "rem7[6]");
	}
	/* width=8: two full groups */
	{
		uint8_t packed[10];
		uint16_t dst[8] = { 0 };
		pack4(0x101, 0x202, 0x303, 0x101, &packed[0]);
		pack4(0x202, 0x303, 0x101, 0x202, &packed[5]);
		nv15_unpack_plane_to_p010(packed, dst, 8, 1, 10);
		ASSERT_EQ(dst[7], 0x202 << 6, "w8[7]");
	}
}

static void test_multi_row_stride_padding(void)
{
	/* 4-pixel-wide, 3-row plane; stride = 8 bytes (3 bytes padding). */
	uint8_t packed[24];  /* 3 rows × 8 bytes */
	uint16_t dst[12];    /* 3 rows × 4 pixels */
	memset(packed, 0xCC, sizeof(packed));  /* padding poison */

	pack4(0x111, 0x222, 0x333, 0x044, &packed[0 * 8]);
	pack4(0x055, 0x166, 0x177, 0x188, &packed[1 * 8]);
	pack4(0x099, 0x1AA, 0x2BB, 0x3CC, &packed[2 * 8]);

	memset(dst, 0xAB, sizeof(dst));
	nv15_unpack_plane_to_p010(packed, dst, 4, 3, 8);

	ASSERT_EQ(dst[0], 0x111 << 6, "row0[0]");
	ASSERT_EQ(dst[3], 0x044 << 6, "row0[3]");
	ASSERT_EQ(dst[4], 0x055 << 6, "row1[0]");
	ASSERT_EQ(dst[7], 0x188 << 6, "row1[3]");
	ASSERT_EQ(dst[8], 0x099 << 6, "row2[0]");
	ASSERT_EQ(dst[11], 0x3CC << 6, "row2[3]");
}

static void test_chroma_half_height(void)
{
	/* 4-pixel-wide × 2-row chroma (matches 4×4 luma quadrant).
	 * NV15 chroma uses same packing as luma, just half-height. */
	uint8_t packed[10];  /* 2 rows × 5 bytes */
	uint16_t dst[8];     /* 2 rows × 4 pixels (UV pairs in interleaved form) */

	pack4(0x080, 0x180, 0x280, 0x380, &packed[0]);
	pack4(0x040, 0x140, 0x240, 0x340, &packed[5]);

	nv15_unpack_plane_to_p010(packed, dst, 4, 2, 5);

	ASSERT_EQ(dst[0], 0x080 << 6, "chroma row0[0]");
	ASSERT_EQ(dst[3], 0x380 << 6, "chroma row0[3]");
	ASSERT_EQ(dst[4], 0x040 << 6, "chroma row1[0]");
	ASSERT_EQ(dst[7], 0x340 << 6, "chroma row1[3]");
}

int main(void)
{
	test_zero();
	test_all_max();
	test_known_vectors();
	test_remainder_width();
	test_multi_row_stride_padding();
	test_chroma_half_height();
	printf("test_nv15_unpack: all PASS\n");
	return 0;
}
