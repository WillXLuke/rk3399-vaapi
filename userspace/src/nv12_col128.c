/*
 * V4L2_PIX_FMT_NV12_COL128 → linear NV12 detile primitive. Pi 5 / CM5
 * rpi-hevc-dec CAPTURE. iter40 (2026-05-17).
 *
 * Math derived from kernel hevc_d_video.c (size formula) +
 * ffmpeg/Kynesim libavutil/rpi_sand_fn_pw.h (per-pixel offset). The
 * single-stripe fast path memcpy's 128 bytes at a time when an output
 * row falls entirely within one tile column (the common case);
 * straddling rows are split into two memcpy halves.
 *
 * No NEON / SIMD here — correctness first. Each output row generates
 * (width / 128) + ~1 memcpys of up to 128 bytes; for 1920x1080 that's
 * ~17000 small memcpys per frame, fine for Phase 1 PoC.
 */

#include "nv12_col128.h"

#include <string.h>

/*
 * Tile column width in bytes. The 'COL128' name embeds this; if it ever
 * varies, take it from V4L2_PIX_FMT_NV12_COL128's kernel definition.
 */
#define NC12_TILE_W   128

/*
 * Common Y / UV plane detile — the layout is identical (single-byte per
 * pixel, column-major 128-wide tiles). The only thing that varies is
 * what plane the caller passes in. width here is plane width in bytes
 * (= image width for both Y and CbCr-interleaved NV12 UV); height is
 * plane height in pixels (image height for Y, image height / 2 for UV).
 */
static void nv12_col128_detile_plane(uint8_t *dst, unsigned int dst_stride,
                                     const uint8_t *src,
                                     unsigned int src_col_stride,
                                     unsigned int width, unsigned int height)
{
	unsigned int y, x;

	for (y = 0; y < height; y++) {
		uint8_t *drow = dst + y * dst_stride;
		x = 0;
		while (x < width) {
			unsigned int col = x / NC12_TILE_W;
			unsigned int in_col = x % NC12_TILE_W;
			unsigned int n = NC12_TILE_W - in_col;
			if (n > width - x)
				n = width - x;
			/*
			 * Source byte = base + col*128*col_stride + y*128 + in_col
			 * Copy n contiguous bytes (all within this tile column,
			 * since n is capped at the remaining width-in-column).
			 */
			const uint8_t *p = src
				+ (size_t)col * NC12_TILE_W * src_col_stride
				+ (size_t)y * NC12_TILE_W
				+ in_col;
			memcpy(drow + x, p, n);
			x += n;
		}
	}
}

void nv12_col128_detile_y(uint8_t *dst, unsigned int dst_stride,
                          const uint8_t *src_y, unsigned int src_col_stride,
                          unsigned int width, unsigned int height)
{
	nv12_col128_detile_plane(dst, dst_stride, src_y, src_col_stride,
				 width, height);
}

void nv12_col128_detile_uv(uint8_t *dst, unsigned int dst_stride,
                           const uint8_t *src_uv, unsigned int src_col_stride,
                           unsigned int width, unsigned int uv_height)
{
	/* UV plane (CbCr interleaved): byte-width equals Y-plane width
	 * (one Cb + one Cr per 2x2 Y block → 2 bytes per 2 horizontal Y
	 * samples → 1 byte per Y pixel horizontally). Height is half. */
	nv12_col128_detile_plane(dst, dst_stride, src_uv, src_col_stride,
				 width, uv_height);
}

unsigned int nv12_col128_uv_plane_offset(unsigned int image_width,
                                         unsigned int image_height)
{
	unsigned int aligned_h = (image_height + 7) & ~7u;

	/*
	 * In the COL128 SAND layout, Y and UV are NOT separate planes
	 * concatenated end-to-end. Within EACH 128-pixel-wide column:
	 *   first 128 * height bytes  = Y data for this column strip
	 *   next  128 * height / 2 bytes = UV data for this column strip
	 *   total 128 * bytesperline (= 128 * height * 3/2) bytes per column
	 *
	 * The "UV plane base" pointer (data[1] in AVFrame convention) is
	 * just data[0] + (128 * height) — the offset of the UV bytes
	 * WITHIN the first column. All subsequent UV bytes are reached by
	 * the same column-stride arithmetic the Y plane uses (col *
	 * 128 * bytesperline + y * 128 + in_col), so passing this offset
	 * pointer + iterating y over [0, height/2) traverses all UV rows
	 * across all columns correctly.
	 *
	 * Earlier wrong formula was num_columns * 128 * aligned_h (i.e.
	 * sizeof(linear Y plane)) — that pushed past the end of the SAND
	 * buffer because the layout isn't planes-end-to-end.
	 *
	 * Cross-check: kernel sizeimage = bytesperline * width =
	 * (aligned_h * 3/2) * num_columns * 128 = num_columns * 128 *
	 * aligned_h * 3/2. Per column: 128 * aligned_h * 3/2. Y portion
	 * per column: 128 * aligned_h. UV portion per column: half of Y.
	 * Sum across columns: matches sizeimage.
	 */
	return NC12_TILE_W * aligned_h;
}
