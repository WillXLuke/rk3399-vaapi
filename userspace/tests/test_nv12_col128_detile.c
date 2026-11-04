/*
 * Copyright (C) 2026 claude-noether <claude-noether@reauktion.de>
 *
 * MIT-licensed per project. iter40 self-test for nv12_col128 detile.
 *
 * Build an NC12-tiled source buffer from a known linear NV12 image,
 * run the detile primitive, assert output matches the original. No
 * hardware needed — pure bit-layout verification of the kernel math
 * (drivers/media/platform/raspberrypi/hevc_dec/hevc_d_video.c
 * V4L2_PIX_FMT_NV12_COL128 case + ffmpeg/Kynesim per-pixel offset).
 *
 * Build:
 *   cc -Wall -Werror -O2 -o test_nv12_col128_detile \
 *      tests/test_nv12_col128_detile.c src/nv12_col128.c
 *
 * Exit 0 = all asserts pass.
 */

#include "../src/nv12_col128.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILE_W 128

static unsigned int align_up(unsigned int v, unsigned int a)
{
	return (v + a - 1) & ~(a - 1);
}

/* Pack a linear plane (width × height bytes, stride=width) into NC12
 * layout: each 128-wide column held contiguously, columns at offsets
 * col * col_stride * 128. col_stride is the kernel-reported bytesperline
 * = ALIGN(height, 8) * 3/2. Returns the buffer + sizes. */
static uint8_t *pack_to_nc12(const uint8_t *linear,
			     unsigned int width, unsigned int height,
			     unsigned int *out_col_stride,
			     size_t *out_size)
{
	unsigned int aligned_w = align_up(width, TILE_W);
	unsigned int aligned_h = align_up(height, 8);
	unsigned int col_stride = aligned_h * 3 / 2;
	unsigned int num_cols = aligned_w / TILE_W;
	size_t total = (size_t)col_stride * aligned_w;
	uint8_t *buf;
	unsigned int col, y, in_col;

	buf = calloc(1, total);
	assert(buf != NULL);

	for (col = 0; col < num_cols; col++) {
		uint8_t *col_base = buf + (size_t)col * TILE_W * col_stride;
		for (y = 0; y < height; y++) {
			for (in_col = 0; in_col < TILE_W; in_col++) {
				unsigned int x = col * TILE_W + in_col;
				if (x >= width)
					break;
				col_base[(size_t)y * TILE_W + in_col] =
					linear[(size_t)y * width + x];
			}
		}
	}

	*out_col_stride = col_stride;
	*out_size = total;
	return buf;
}

static void test_detile_y(unsigned int width, unsigned int height)
{
	uint8_t *linear, *tiled, *recovered;
	unsigned int col_stride;
	size_t tile_size, i;

	linear = malloc((size_t)width * height);
	assert(linear != NULL);
	/* Distinctive content per pixel: y * 17 + x * 13 — avoids byte-
	 * aliasing patterns that could mask off-by-one bugs. */
	for (unsigned int y = 0; y < height; y++)
		for (unsigned int x = 0; x < width; x++)
			linear[(size_t)y * width + x] = (uint8_t)(y * 17 + x * 13);

	tiled = pack_to_nc12(linear, width, height, &col_stride, &tile_size);

	recovered = calloc(1, (size_t)width * height);
	assert(recovered != NULL);

	nv12_col128_detile_y(recovered, width, tiled, col_stride, width, height);

	for (i = 0; i < (size_t)width * height; i++) {
		if (recovered[i] != linear[i]) {
			fprintf(stderr,
				"FAIL %ux%u Y: pixel %zu (x=%zu y=%zu) "
				"linear=0x%02x recovered=0x%02x\n",
				width, height, i,
				i % width, i / width,
				linear[i], recovered[i]);
			free(linear); free(tiled); free(recovered);
			exit(1);
		}
	}
	printf("PASS %ux%u Y plane (%u columns, col_stride=%u, tile_size=%zu)\n",
	       width, height, align_up(width, TILE_W) / TILE_W,
	       col_stride, tile_size);

	free(linear);
	free(tiled);
	free(recovered);
}

static void test_detile_uv(unsigned int width, unsigned int height)
{
	unsigned int uv_h = height / 2;
	uint8_t *linear, *tiled, *recovered;
	unsigned int col_stride;
	size_t tile_size, i;

	linear = malloc((size_t)width * uv_h);
	assert(linear != NULL);
	for (unsigned int y = 0; y < uv_h; y++)
		for (unsigned int x = 0; x < width; x++)
			linear[(size_t)y * width + x] = (uint8_t)(y * 23 + x * 7);

	tiled = pack_to_nc12(linear, width, uv_h, &col_stride, &tile_size);

	recovered = calloc(1, (size_t)width * uv_h);
	assert(recovered != NULL);

	nv12_col128_detile_uv(recovered, width, tiled, col_stride, width, uv_h);

	for (i = 0; i < (size_t)width * uv_h; i++) {
		if (recovered[i] != linear[i]) {
			fprintf(stderr,
				"FAIL %ux%u UV: pixel %zu linear=0x%02x recovered=0x%02x\n",
				width, height, i,
				linear[i], recovered[i]);
			free(linear); free(tiled); free(recovered);
			exit(1);
		}
	}
	printf("PASS %ux%u UV plane\n", width, height);

	free(linear);
	free(tiled);
	free(recovered);
}

static void test_uv_offset(void)
{
	/* Per the SAND COL128 layout, Y and UV are interleaved within
	 * EACH column (not concatenated as separate planes), so the UV
	 * plane base pointer is offset by 128 * ALIGN(height, 8) — the
	 * Y portion of column 0. NOT 128 * height * num_columns (the
	 * size of all Y across all columns), which was an earlier wrong
	 * formula caught by Phase 7 SEGV on higgs. */
	unsigned int off = nv12_col128_uv_plane_offset(1280, 720);
	if (off != 128u * 720) {
		fprintf(stderr, "FAIL UV offset 1280×720: got %u expected %u\n",
			off, 128u * 720);
		exit(1);
	}
	printf("PASS UV offset 1280×720 = %u\n", off);

	off = nv12_col128_uv_plane_offset(1366, 768);
	if (off != 128u * 768) {
		fprintf(stderr, "FAIL UV offset 1366×768: got %u expected %u\n",
			off, 128u * 768);
		exit(1);
	}
	printf("PASS UV offset 1366×768 (column-misaligned width)\n");
}

int main(void)
{
	/* Phase 3 fixture sizes — all 128-aligned, 8-line-aligned. */
	test_detile_y(640, 360);
	test_detile_y(1280, 720);
	test_detile_y(1920, 1080);

	/* Phase 5 review F4: column-misaligned width (1366 → 1408 padding). */
	test_detile_y(1366, 768);

	/* UV plane (half-height) at each width. */
	test_detile_uv(640, 360);
	test_detile_uv(1280, 720);
	test_detile_uv(1920, 1080);
	test_detile_uv(1366, 768);

	test_uv_offset();

	printf("All NC12 detile asserts pass.\n");
	return 0;
}
