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

#include "image.h"
#include "buffer.h"
#include "encoder.h"
#include "request.h"
#include "surface.h"
#include "video.h"

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <linux/dma-buf.h>

#include "nv15.h"
#include "nv12_col128.h"
#include "tiled_yuv.h"
#include "utils.h"
#include "v4l2.h"

VAStatus RequestCreateImage(VADriverContextP context, VAImageFormat *format,
			    int width, int height, VAImage *image)
{
	struct request_data *driver_data = context->pDriverData;
	unsigned int destination_sizes[VIDEO_MAX_PLANES];
	unsigned int destination_bytesperlines[VIDEO_MAX_PLANES];
	unsigned int destination_planes_count;
	unsigned int planes_count;
	unsigned int format_width, format_height;
	unsigned int size;
	unsigned int capture_type;
	struct video_format *video_format;
	struct object_image *image_object;
	VABufferID buffer_id;
	VAImageID id;
	VAStatus status;
	unsigned int i;
	int rc;

	request_log("image: CreateImage fourcc %08x %dx%d encode=%d vf=%p\n",
		    format->fourcc, width, height,
		    driver_data->encode_active,
		    (void *)driver_data->video_format);

	/*
	 * Encoder (or-not-yet-created) sessions: images are plain NV12 with a
	 * tightly packed layout derived from the requested width/height.
	 *
	 * ffmpeg's hwupload creates its upload image before the encoder's
	 * vaCreateContext() runs, so `video_format` is still NULL there.  The
	 * old code failed in that window; falling back to NV12 geometry is
	 * safe because a decoder context always populates video_format before
	 * any of its own image calls.
	 */
	if (driver_data->encode_active || driver_data->video_format == NULL) {
		if (format->fourcc != VA_FOURCC_NV12)
			return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

		destination_planes_count = 2;
		format_width = width;
		format_height = height;
		destination_bytesperlines[0] = width;
		destination_bytesperlines[1] = width;
		destination_sizes[0] = width * height;
		destination_sizes[1] = width * (height / 2);

		size = destination_sizes[0] + destination_sizes[1];
		goto allocate;
	}

	video_format = driver_data->video_format;
	if (video_format == NULL)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	/*
	 * FIXME: This should be replaced by per-pixelformat hadling to
	 * determine the logical plane offsets and sizes;
	 */
	rc = v4l2_get_format(driver_data->video_fd, capture_type,
			     &format_width, &format_height,
			     destination_bytesperlines, destination_sizes,
			     &planes_count);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	destination_planes_count = video_format->planes_count;
	size = 0;

	/* The size returned by V4L2 covers buffers, not logical planes. */
	for (i = 0; i < planes_count; i++)
		size += destination_sizes[i];

	if (format->fourcc == VA_FOURCC_P010) {
		/*
		 * iter39: P010 image overrides V4L2-side NV15 sizing. The
		 * source is the kernel-reported NV15 packed plane; the image
		 * buffer holds dense P010 (2 bytes per pixel, 16bpp).
		 * Recompute sizes/pitches against P010 layout so consumers
		 * (vaGetImage, vaDeriveImage) see standard P010 geometry.
		 */
		destination_bytesperlines[0] = width * 2;
		destination_sizes[0] = destination_bytesperlines[0] * format_height;
		for (i = 1; i < destination_planes_count; i++) {
			destination_bytesperlines[i] = destination_bytesperlines[0];
			destination_sizes[i] = destination_sizes[0] / 2;
		}
		size = 0;
		for (i = 0; i < destination_planes_count; i++)
			size += destination_sizes[i];
	} else if (format->fourcc == VA_FOURCC_NV12 &&
		   video_format->v4l2_format == V4L2_PIX_FMT_NV12_COL128) {
		/*
		 * iter40 Phase 5 review F2: NC12 source, NV12 image output.
		 * V4L2-reported destination_bytesperlines[0] is the NC12
		 * column stride (= ALIGN(height,8) * 3/2 — e.g. 1080 for
		 * 1280×720), NOT the linear NV12 Y stride. Override to the
		 * linear stride (width) so VAImage pitches reflect the
		 * detile-output layout the consumer reads.
		 */
		destination_bytesperlines[0] = width;
		destination_sizes[0] = destination_bytesperlines[0] * format_height;
		for (i = 1; i < destination_planes_count; i++) {
			destination_bytesperlines[i] = destination_bytesperlines[0];
			destination_sizes[i] = destination_sizes[0] / 2;
		}
		size = 0;
		for (i = 0; i < destination_planes_count; i++)
			size += destination_sizes[i];
	} else {
		/* NV12: V4L2 stride is correct, sizes derived from height. */
		destination_sizes[0] = destination_bytesperlines[0] * format_height;

		for (i = 1; i < destination_planes_count; i++) {
			destination_bytesperlines[i] = destination_bytesperlines[0];
			destination_sizes[i] = destination_sizes[0] / 2;
		}
	}

allocate:
	id = object_heap_allocate(&driver_data->image_heap);
	image_object = IMAGE(driver_data, id);
	if (image_object == NULL)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	status = RequestCreateBuffer(context, 0, VAImageBufferType, size, 1,
				     NULL, &buffer_id);
	if (status != VA_STATUS_SUCCESS) {
		object_heap_free(&driver_data->image_heap,
				 (struct object_base *)image_object);
		return status;
	}

	memset(image, 0, sizeof(*image));

	image->format = *format;
	image->width = width;
	image->height = height;
	image->buf = buffer_id;
	image->image_id = id;

	image->num_planes = destination_planes_count;
	image->data_size = size;

	for (i = 0; i < image->num_planes; i++) {
		image->pitches[i] = destination_bytesperlines[i];
		image->offsets[i] = i > 0 ? destination_sizes[i - 1] : 0;
	}

	image_object->image = *image;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestDestroyImage(VADriverContextP context, VAImageID image_id)
{

	struct request_data *driver_data = context->pDriverData;
	struct object_image *image_object;
	VAStatus status;

	image_object = IMAGE(driver_data, image_id);
	if (image_object == NULL)
		return VA_STATUS_ERROR_INVALID_IMAGE;

	status = RequestDestroyBuffer(context, image_object->image.buf);
	if (status != VA_STATUS_SUCCESS)
		return status;

	object_heap_free(&driver_data->image_heap,
			 (struct object_base *)image_object);

	return VA_STATUS_SUCCESS;
}

static bool encoder_derive_disabled(void)
{
	static int disabled = -1;

	if (disabled < 0)
		disabled = getenv("LIBVA_V4L2_ENCODER_NO_DERIVE") != NULL;

	return disabled != 0;
}

static VAStatus copy_surface_to_image (struct request_data *driver_data,
				       struct object_surface *surface_object,
				       VAImage *image)
{
	struct object_buffer *buffer_object;
	unsigned int i;
	int sync_fds[VIDEO_MAX_PLANES];
	unsigned int n_sync_fds = 0;

	buffer_object = BUFFER(driver_data, image->buf);
	if (buffer_object == NULL)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	for (i = 0; i < VIDEO_MAX_PLANES; i++)
		sync_fds[i] = -1;

	/*
	 * iter13 α-17: explicit cache sync around the CAPTURE buffer read.
	 *
	 * The CAPTURE buffer is V4L2_MEMORY_MMAP and was mapped at
	 * cap_pool_init time with cached attributes. Kernel decode writes to
	 * the buffer via DMA, which doesn't propagate to the CPU's cache
	 * observer for that virtual mapping. Reading from
	 * surface_object->destination_data[] without an explicit cache
	 * invalidation returns stale data — observed empirically as Bug 4
	 * (H.264 partial-fill) and Bug 5 (HEVC all-zero) when libva went
	 * through the SAME readback path that kdirect ffmpeg-v4l2request +
	 * DRM_PRIME-mmap successfully reads (kdirect's drm-prime mmap
	 * implicitly handles sync).
	 *
	 * DMA_BUF_IOCTL_SYNC(START | READ) makes the CPU mapping coherent
	 * with the producing engine's writes; END releases the sync.
	 * Per V4L2 + dma-buf spec, this is the userspace contract for
	 * cached-mmap'd buffers (Tomasz Figa, linaro-mm-sig 2024-07-11).
	 *
	 * Requires a dma-buf fd: get one via VIDIOC_EXPBUF, sync, close.
	 * Per-call cost is one ioctl pair + one fd open/close per plane.
	 * Could be optimised by caching the EXPBUF fd on the cap_pool slot,
	 * but doing it just-in-time keeps the lifecycle uncomplicated. The
	 * EXPBUF fd's dup count doesn't affect the V4L2 buffer's underlying
	 * pages; closing the fd is a no-op on memory.
	 *
	 * If EXPBUF fails (e.g., consumer-held EXPBUF prevents a second one
	 * — only true for hantro G1 oddity), we skip the sync silently. The
	 * existing pre-iter13 behavior is preserved on the error path.
	 */
	if (surface_object->current_slot != NULL &&
	    driver_data->video_format != NULL) {
		unsigned int capture_type =
			v4l2_type_video_capture(driver_data->video_format->v4l2_mplane);
		if (v4l2_export_buffer(driver_data->video_fd, capture_type,
				       surface_object->destination_index,
				       O_RDONLY, sync_fds,
				       surface_object->destination_buffers_count) >= 0) {
			n_sync_fds = surface_object->destination_buffers_count;
			for (i = 0; i < n_sync_fds; i++) {
				struct dma_buf_sync s = {
					.flags = DMA_BUF_SYNC_START |
						 DMA_BUF_SYNC_READ,
				};
				/* failure is non-fatal: we continue with the read */
				(void)ioctl(sync_fds[i], DMA_BUF_IOCTL_SYNC, &s);
			}
		}
	}

	for (i = 0; i < surface_object->destination_planes_count; i++) {
		/*
		 * iter40 Phase 5 review F1: guard extended from __arm__ to
		 * __arm__ || __aarch64__. Without this, the detile primitives
		 * silently compiled out on aarch64 (fresnel RK3399, ampere
		 * RK3588, higgs Pi CM5) and the memcpy fall-through delivered
		 * raw tiled bytes to NV12/P010 image consumers. iter39 5/5
		 * PASS masked the issue because no 10-bit path was exercised.
		 */
#if defined(__arm__) || defined(__aarch64__)
		/*
		 * Sunxi tiled_to_planar lives in tiled_yuv.S which is
		 * #ifdef __arm__ — symbol absent on aarch64. Keep this
		 * branch arm-only; aarch64 Sunxi support would need a C or
		 * aarch64-ASM port (no Sunxi aarch64 board in current fleet).
		 */
#if defined(__arm__)
		if (!video_format_is_linear(driver_data->video_format))
			tiled_to_planar(surface_object->destination_data[i],
					buffer_object->data + image->offsets[i],
					image->pitches[i], image->width,
					i == 0 ? image->height :
						 image->height / 2);
		else
#endif
		if (driver_data->is_10bit &&
			 image->format.fourcc == VA_FOURCC_P010) {
			/*
			 * iter39: rkvdec emits NV15 (4×10-bit packed in 5
			 * bytes); the VA image buffer is dense P010 (2B/pixel,
			 * value in bits[15:6]). Source stride is the V4L2-
			 * reported NV15 bytesperline (= ceil(width/4)*5,
			 * possibly aligned higher by the kernel); destination
			 * stride is image->pitches[i] = width * 2.
			 */
			unsigned int plane_h = (i == 0) ? image->height
							: image->height / 2;
			nv15_unpack_plane_to_p010(
				surface_object->destination_data[i],
				(uint16_t *)(buffer_object->data + image->offsets[i]),
				image->width, plane_h,
				surface_object->destination_bytesperlines[i]);
		} else if (driver_data->video_format != NULL &&
			   driver_data->video_format->v4l2_format ==
			   V4L2_PIX_FMT_NV12_COL128 &&
			   image->format.fourcc == VA_FOURCC_NV12) {
			/*
			 * iter40: Pi 5 rpi-hevc-dec emits NV12_COL128 (SAND
			 * 128-pixel-wide column tiles). Detile to linear NV12
			 * via the per-plane primitive. surface_object->
			 * destination_data[i] is the V4L2 CAPTURE mmap (single
			 * buffer, planes_count==2): i==0 is the Y plane base,
			 * i==1 is the UV plane base offset within the SAME
			 * physical buffer (per cap_pool plane[1] offset = Y
			 * plane size in COL128 layout).
			 *
			 * src_col_stride = destination_bytesperlines[i] = the
			 * kernel-reported NC12 bytesperline (column stride,
			 * = ALIGN(image_h, 8) * 3/2). Same for both planes
			 * since column geometry is plane-agnostic.
			 *
			 * dst stride is image->pitches[i] = image->width
			 * (overridden in RequestCreateImage NC12 branch below).
			 */
			if (i == 0) {
				nv12_col128_detile_y(
					(uint8_t *)(buffer_object->data + image->offsets[i]),
					image->pitches[i],
					surface_object->destination_data[i],
					surface_object->destination_bytesperlines[i],
					image->width, image->height);
			} else {
				nv12_col128_detile_uv(
					(uint8_t *)(buffer_object->data + image->offsets[i]),
					image->pitches[i],
					surface_object->destination_data[i],
					surface_object->destination_bytesperlines[i],
					image->width, image->height / 2);
			}
		} else {
#endif
			memcpy(buffer_object->data + image->offsets[i],
			       surface_object->destination_data[i],
			       surface_object->destination_sizes[i]);
#if defined(__arm__) || defined(__aarch64__)
		}
#endif
	}

	/* iter13 α-17: release cache sync. END pairs with each START. */
	for (i = 0; i < n_sync_fds; i++) {
		struct dma_buf_sync s = {
			.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ,
		};
		(void)ioctl(sync_fds[i], DMA_BUF_IOCTL_SYNC, &s);
		close(sync_fds[i]);
	}

	return VA_STATUS_SUCCESS;
}

VAStatus RequestDeriveImage(VADriverContextP context, VASurfaceID surface_id,
			    VAImage *image)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;
	struct object_buffer *buffer_object;
	VAImageFormat format;
	VAStatus status;


	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	/*
	 * Encoder surfaces derive an image that aliases their NV12 storage,
	 * so a mapped derived image *is* the surface (this is the upload path
	 * ffmpeg uses once its vaDeriveImage probe succeeds).  Surfaces that
	 * have never been bound to a decode CAPTURE slot are treated the same
	 * way: an encoder's input surface has no slot by construction, and a
	 * not-yet-decoded surface has no pixels to export either.
	 *
	 * LIBVA_V4L2_ENCODER_NO_DERIVE=1 disables this so the alternative
	 * vaCreateImage()+vaPutImage() upload path can be exercised (both are
	 * implemented; the derive alias is just the faster one).
	 */
	if (!encoder_derive_disabled() &&
	    (surface_object->enc_data != NULL ||
	     surface_object->current_slot == NULL))
		return encoder_derive_image(context, surface_object, image);

	if (surface_object->status == VASurfaceRendering) {
		status = RequestSyncSurface(context, surface_id);
		if (status != VA_STATUS_SUCCESS)
			return status;
	}

	/* Fully populate VAImageFormat to match QueryImageFormats output. */
	memset(&format, 0, sizeof(format));
	if (driver_data->is_10bit) {
		/* iter39: 10-bit session derives a P010 image. NV15-source
		 * unpack happens in copy_surface_to_image. */
		format.fourcc = VA_FOURCC_P010;
		format.byte_order = VA_LSB_FIRST;
		format.bits_per_pixel = 24;
	} else {
		format.fourcc = VA_FOURCC_NV12;
		format.byte_order = VA_LSB_FIRST;
		format.bits_per_pixel = 12;
	}

	status = RequestCreateImage(context, &format, surface_object->width,
				    surface_object->height, image);
	if (status != VA_STATUS_SUCCESS)
		return status;

	/*
	 * Iter2 Fix 3: skip the surface→image copy when no CAPTURE slot is
	 * bound. ffmpeg's av_hwframe_ctx_init probes vaDeriveImage on a
	 * never-decoded surface to learn the format; it doesn't read the
	 * data. With the cap_pool decoupling, destination_data[] is NULL
	 * until BeginPicture binds a slot — copying from a NULL source
	 * crashed in memcpy. The image's buffer remains zero-initialized;
	 * subsequent post-decode DeriveImage on the same surface (after
	 * BeginPicture has bound a slot) does the real copy.
	 */
	if (surface_object->current_slot != NULL) {
		status = copy_surface_to_image (driver_data, surface_object,
						image);
		if (status != VA_STATUS_SUCCESS)
			return status;
	}

	surface_object->status = VASurfaceReady;

	buffer_object = BUFFER(driver_data, image->buf);
	buffer_object->derived_surface_id = surface_id;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQueryImageFormats(VADriverContextP context,
				  VAImageFormat *formats, int *formats_count)
{
	struct request_data *driver_data = context->pDriverData;
	int n = 0;

	/*
	 * Populate the VAImageFormat fully per VAAPI spec — not just
	 * .fourcc. Consumers (FFmpeg's hwcontext_vaapi, mpv, Firefox)
	 * read .byte_order and .bits_per_pixel; leaving them
	 * uninitialized inherits caller-stack garbage and produces
	 * non-deterministic behavior. Reference: Mesa's
	 * gallium/frontends/va/image.c::vlVaQueryImageFormats and
	 * intel-vaapi-driver's i965_drv_video.c.
	 *
	 * iter39: advertise P010 when an active session is 10-bit so
	 * ffmpeg-vaapi sees a valid 10-bit-compatible entry during
	 * vaQueryImageFormats. NV12 stays advertised unconditionally so
	 * the 8-bit catalog query response is unchanged.
	 */
	memset(&formats[n], 0, sizeof(formats[n]));
	formats[n].fourcc = VA_FOURCC_NV12;
	formats[n].byte_order = VA_LSB_FIRST;
	formats[n].bits_per_pixel = 12;
	n++;

	/*
	 * iter39 Option B revert (2026-05-17): P010 advertisement is
	 * gated on driver_data->is_10bit again. Previously advertised
	 * unconditionally (63fed87) so ffmpeg-vaapi's early
	 * vaQueryImageFormats (pre-vaCreateContext) could see it for
	 * 10-bit profiles — but that broke HEVC 8-bit on fresnel:
	 * ffmpeg-vaapi picked P010 for the HEVC hwframe pool, EndPicture
	 * SEGV'd in the .so when the consumer-side P010 expectations met
	 * an 8-bit NV12 CAPTURE buffer.
	 * Safe because Option B drops VAProfileHEVCMain10 + Hi10P from
	 * enumeration — no 10-bit decode pipeline will reach this catalog
	 * query so the gate-on-is_10bit (which stays false for 8-bit
	 * profiles) correctly returns NV12-only.
	 */
	if (driver_data->is_10bit && n < V4L2_REQUEST_MAX_IMAGE_FORMATS) {
		memset(&formats[n], 0, sizeof(formats[n]));
		formats[n].fourcc = VA_FOURCC_P010;
		formats[n].byte_order = VA_LSB_FIRST;
		formats[n].bits_per_pixel = 24;
		n++;
	}

	*formats_count = n;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestSetImagePalette(VADriverContextP context, VAImageID image_id,
				unsigned char *palette)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestGetImage(VADriverContextP context, VASurfaceID surface_id,
			 int x, int y, unsigned int width, unsigned int height,
			 VAImageID image_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;
	struct object_image *image_object;
	VAImage *image;


	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	image_object = IMAGE(driver_data, image_id);
	if (image_object == NULL)
		return VA_STATUS_ERROR_INVALID_IMAGE;

	image = &image_object->image;
	if (x != 0 || y != 0 || width != image->width || height != image->height)
		return VA_STATUS_ERROR_UNIMPLEMENTED;

	if (driver_data->encode_active &&
	    surface_object->current_slot == NULL) {
		struct object_buffer *buffer_object =
			BUFFER(driver_data, image->buf);

		if (buffer_object == NULL)
			return VA_STATUS_ERROR_INVALID_BUFFER;
		if (encoder_surface_ensure_storage(surface_object) < 0)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;

		return encoder_surface_get_image(surface_object, image,
						 buffer_object->data) < 0 ?
			VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS;
	}

	return copy_surface_to_image (driver_data, surface_object, image);
}

VAStatus RequestPutImage(VADriverContextP context, VASurfaceID surface_id,
			 VAImageID image, int src_x, int src_y,
			 unsigned int src_width, unsigned int src_height,
			 int dst_x, int dst_y, unsigned int dst_width,
			 unsigned int dst_height)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;
	struct object_image *image_object;
	struct object_buffer *buffer_object;
	VAImage *va_image;

	/*
	 * Encoder surfaces own their NV12 storage, so vaPutImage() is a plain
	 * row-wise copy into it.  This must also work before vaCreateContext:
	 * ffmpeg's hwupload uploads the first frame while the encoder context
	 * is still being created.  A populated decoder session keeps its
	 * historical unimplemented behaviour.
	 */
	request_log("image: putimage surface=%u encode=%d video_format=%p\n",
		    (unsigned int)surface_id, driver_data->encode_active,
		    (void *)driver_data->video_format);

	if (!driver_data->encode_active && driver_data->video_format != NULL)
		return VA_STATUS_ERROR_UNIMPLEMENTED;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	image_object = IMAGE(driver_data, image);
	if (image_object == NULL)
		return VA_STATUS_ERROR_INVALID_IMAGE;

	va_image = &image_object->image;
	buffer_object = BUFFER(driver_data, va_image->buf);
	if (buffer_object == NULL)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	if (src_x != 0 || src_y != 0 ||
	    src_width != va_image->width || src_height != va_image->height ||
	    dst_x != 0 || dst_y != 0 ||
	    dst_width != va_image->width || dst_height != va_image->height)
		return VA_STATUS_ERROR_UNIMPLEMENTED;

	if (va_image->format.fourcc != VA_FOURCC_NV12)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

	if (encoder_surface_ensure_storage(surface_object) < 0)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	request_log("encoder: putimage surface %u (%dx%d via %ux%u image)\n",
		    (unsigned int)surface_object->base.id,
		    surface_object->width, surface_object->height,
		    va_image->width, va_image->height);

	if (encoder_surface_put_image(surface_object, va_image,
				      buffer_object->data) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	surface_object->status = VASurfaceReady;

	return VA_STATUS_SUCCESS;
}
