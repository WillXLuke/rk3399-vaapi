/*
 * V4L2_PIX_FMT_NV12_COL128 (NC12) SAND-tiled → linear NV12 detile.
 *
 * Pi 5 / CM5 (BCM2712) rpi-hevc-dec CAPTURE format. iter40 (2026-05-17).
 *
 * Layout (kernel drivers/media/platform/raspberrypi/hevc_dec/hevc_d_video.c
 * size-formula + ffmpeg/Kynesim libavutil/rpi_sand_fn_pw.h per-pixel
 * offset math):
 *
 *   width  ALIGN(image_width,  128)   -- columns are 128 px wide
 *   height ALIGN(image_height,   8)
 *   col_stride (= bytesperline) = height * 3 / 2
 *               (bytes per [128-wide column] vertical unit incl. Y + UV)
 *   sizeimage  = col_stride * width = total bytes
 *
 *   For pixel (x, y) in the Y plane:
 *     col      = x / 128
 *     in_col_x = x % 128
 *     offset   = col * col_stride * 128 + y * 128 + in_col_x
 *
 *   UV plane starts at offset (128 * height * num_columns_y) — the same
 *   per-column layout, h/2 rows tall (CbCr interleaved).
 *
 * The primitive copies the entire image extent at once. width/height are
 * the cropped consumer-visible dimensions; src_col_stride is the kernel-
 * reported bytesperline (i.e. ALIGN(height,8) * 3/2).
 */

#ifndef _NV12_COL128_H_
#define _NV12_COL128_H_

#include <stdint.h>

#include <linux/videodev2.h>

/*
 * Pre-Pi-kernel headers (Arch ALARM linux-api-headers, older mainline
 * kernel-headers packages) may not define V4L2_PIX_FMT_NV12_COL128. The
 * fourcc is Pi-specific. Provide a private fallback so the backend
 * builds on hosts that target NON-Pi codecs too.
 */
#ifndef V4L2_PIX_FMT_NV12_COL128
#define V4L2_PIX_FMT_NV12_COL128  \
	((unsigned int)('N') | ((unsigned int)('C') << 8) | \
	 ((unsigned int)('1') << 16) | ((unsigned int)('2') << 24))
#endif

#ifndef V4L2_PIX_FMT_NV12_10_COL128
/* 10-bit SAND variant: 3 pixels packed into 4 bytes in 128-byte / 96-pixel
 * wide columns. iter40 references the fourcc for completeness; the 10-bit
 * Pi 5 HEVC chapter (Main10) is post-iter40. */
#define V4L2_PIX_FMT_NV12_10_COL128  \
	((unsigned int)('N') | ((unsigned int)('C') << 8) | \
	 ((unsigned int)('3') << 16) | ((unsigned int)('0') << 24))
#endif

/* Detile the Y plane of an NC12 source to a linear NV12 Y plane.
 *   dst         : pointer to linear NV12 Y plane (caller-owned, dst_stride * height bytes)
 *   dst_stride  : linear Y plane stride in bytes (= width for plain NV12)
 *   src_y       : pointer to start of NC12 Y plane (= NC12 buffer base)
 *   src_col_stride: kernel-reported bytesperline (= ALIGN(height,8) * 3/2)
 *   width, height: cropped image dimensions in pixels
 */
void nv12_col128_detile_y(uint8_t *dst, unsigned int dst_stride,
                          const uint8_t *src_y, unsigned int src_col_stride,
                          unsigned int width, unsigned int height);

/* Detile the UV plane (CbCr interleaved, half-height) of an NC12 source.
 *   dst         : pointer to linear NV12 UV plane
 *   dst_stride  : linear UV plane stride in bytes (= width for NV12)
 *   src_uv      : pointer to start of NC12 UV plane (= src_y + Y-plane-size)
 *   src_col_stride: same as Y plane (same column geometry)
 *   width       : Y-plane width in pixels (UV plane has same byte width)
 *   uv_height   : UV plane height = height / 2
 */
void nv12_col128_detile_uv(uint8_t *dst, unsigned int dst_stride,
                           const uint8_t *src_uv, unsigned int src_col_stride,
                           unsigned int width, unsigned int uv_height);

/* Compute the offset of the UV plane within an NC12 buffer.
 *   image_width, image_height: cropped image dimensions in pixels
 *   Returns: byte offset from buffer start to UV plane start
 *           (= 128 * ALIGN(image_height, 8) * num_columns_y)
 */
unsigned int nv12_col128_uv_plane_offset(unsigned int image_width,
                                         unsigned int image_height);

#endif /* _NV12_COL128_H_ */
