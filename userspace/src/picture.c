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

#include "picture.h"
#include "buffer.h"
#include "encoder.h"
#include "config.h"
#include "context.h"
#include "request.h"
#include "surface.h"

#include "h264.h"
#include "h265.h"
#include "mpeg2.h"
#include "vp8.h"
#include "vp9.h"
#include "av1.h"
#include "request_pool.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>

#include <sys/ioctl.h>

#include <linux/videodev2.h>

#include "media.h"
#include "utils.h"
#include "v4l2.h"

#include "autoconfig.h"

/*
 * iter#15 — issue #15: ensure the in-flight surface's OUTPUT mmap has
 * room for `delta` more bytes appended to slices_size; if not, grow the
 * pool transparently via request_pool_resize.
 *
 * Sequence on overflow:
 *   1. Snapshot the surface's accumulated bytes to a temp heap buffer.
 *   2. Release the surface's OUTPUT pool slot back to FREE (resize
 *      requires no slot be borrowed).
 *   3. Compute new sizeimage = roundup(needed * 2, 4 KiB), and at least
 *      double the current source_size so geometric growth amortises
 *      repeated overruns at the same resolution.
 *   4. Call request_pool_resize.
 *   5. Re-acquire a pool slot (the new pool has fresh indices and fds).
 *   6. Re-mirror surface_object->source_{index,data,size,request_fd}
 *      from the new slot.
 *   7. Restore the saved bytes via memcpy into the new mmap.
 *
 * Returns VA_STATUS_SUCCESS on clean resize (or no resize needed) and
 * VA_STATUS_ERROR_ALLOCATION_FAILED on heap-alloc / V4L2 / kernel
 * failure — the libva client falls back to surface re-creation as
 * before the resize hook landed.
 *
 * NOTE on inline-Sync invariant: RequestEndPicture calls
 * RequestSyncSurface inline, so when codec_store_buffer runs no other
 * pool slot is borrowed across libva-driver-API entry points. The
 * temporary release-then-reacquire of the in-flight slot here keeps
 * that invariant intact across the resize.
 */
static VAStatus
codec_store_buffer_ensure_capacity(struct request_data *driver_data,
				   struct object_surface *surface_object,
				   size_t need)
{
	struct request_pool_slot *slot;
	uint8_t *save_buf;
	size_t save_size;
	unsigned int saved_index;
	size_t want_sizeimage;
	unsigned int new_sizeimage;
	int new_index;
	int rc;

	if (need <= surface_object->source_size)
		return VA_STATUS_SUCCESS;

	save_size = surface_object->slices_size;
	save_buf = NULL;
	if (save_size > 0) {
		save_buf = malloc(save_size);
		if (save_buf == NULL) {
			request_log("codec_store_buffer_ensure_capacity: malloc(%zu) for resize-save failed\n",
				    save_size);
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		}
		memcpy(save_buf, surface_object->source_data, save_size);
	}

	/*
	 * Temporarily release the in-flight slot. The slot's V4L2 buffer
	 * has NOT been QBUF'd yet (QBUF lives in RequestEndPicture, after
	 * this codec_store_buffer call), so the release is a clean
	 * busy=false flip; no kernel state is in question.  The slot's
	 * stale request_fd does not need to be saved — the resize closes
	 * every slot's fd and the post-resize acquire below re-mirrors a
	 * fresh slot's request_fd into surface_object->request_fd.
	 */
	saved_index = surface_object->source_index;
	request_pool_release(&driver_data->output_pool, saved_index);

	/*
	 * Geometric growth: at least 2× the current source_size, but no
	 * less than 2× the required total — so a single resize covers the
	 * triggering append plus comfortable headroom for the rest of
	 * this frame. Round up to a 4 KiB page boundary so the kernel's
	 * own alignment doesn't waste pages.  Compute in size_t so the
	 * 2× doubling can't silently wrap at 2 GiB on 32-bit unsigned int
	 * (sizeimage stays bounded by V4L2's u32, but the doubling target
	 * could otherwise overflow before the clamp).
	 */
	want_sizeimage = need * 2;
	if (want_sizeimage < (size_t)surface_object->source_size * 2)
		want_sizeimage = (size_t)surface_object->source_size * 2;
	if (want_sizeimage > 0x40000000u) /* 1 GiB hard cap — V4L2 sizeimage is u32 */
		want_sizeimage = 0x40000000u;
	want_sizeimage = (want_sizeimage + 0xFFFu) & ~(size_t)0xFFFu;
	new_sizeimage = (unsigned int)want_sizeimage;

	request_log("codec_store_buffer: OUTPUT-pool resize (need %zu > cap %u → new_sizeimage %u)\n",
		    need, surface_object->source_size, new_sizeimage);

	rc = request_pool_resize(&driver_data->output_pool, new_sizeimage);
	if (rc < 0) {
		/*
		 * Resize failed. The original slot was already released
		 * above, so surface_object->source_data is now pointing
		 * at a FREE-but-still-borrowable mmap. Restore the
		 * surface's slot mirror so EndPicture / DestroyContext
		 * unwind paths see a consistent (if partial) state.
		 *
		 * If the resize aborted early (pre-STREAMOFF), the slot
		 * is intact: re-acquiring the same index is the inverse
		 * of the temporary release above. If it aborted later
		 * (post-teardown), the slot's data/size were zeroed in
		 * place by request_pool_resize and the re-acquire flips
		 * busy=true on a dead slot — still safe, because the
		 * caller will return ERROR_ALLOCATION_FAILED and the
		 * libva consumer destroys the surface/context.
		 */
		(void)request_pool_acquire(&driver_data->output_pool);
		free(save_buf);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	new_index = request_pool_acquire(&driver_data->output_pool);
	if (new_index < 0) {
		free(save_buf);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	slot = request_pool_slot(&driver_data->output_pool,
				 (unsigned int)new_index);
	if (slot == NULL) {
		request_pool_release(&driver_data->output_pool,
				     (unsigned int)new_index);
		free(save_buf);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	surface_object->source_index = slot->index;
	surface_object->source_data = slot->data;
	surface_object->source_size = slot->size;
	surface_object->request_fd = slot->request_fd;

	if (need > surface_object->source_size) {
		/*
		 * Kernel rounded the new sizeimage down below what we
		 * needed — drivers may clamp at their per-codec ceiling.
		 * Don't corrupt memory; surface the error to libva.
		 */
		request_log("codec_store_buffer_ensure_capacity: kernel returned sizeimage %u < required %zu\n",
			    surface_object->source_size, need);
		free(save_buf);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	if (save_buf != NULL) {
		memcpy(surface_object->source_data, save_buf, save_size);
		free(save_buf);
	}

	return VA_STATUS_SUCCESS;
}

static VAStatus codec_store_buffer(struct request_data *driver_data,
				   struct object_context *context,
				   VAProfile profile,
				   struct object_surface *surface_object,
				   struct object_buffer *buffer_object)
{
	switch (buffer_object->type) {
	case VASliceDataBufferType: {
		/*
		 * Since there is no guarantee that the allocation
		 * order is the same as the submission order (via
		 * RenderPicture), we can't use a V4L2 buffer directly
		 * and have to copy from a regular buffer.
		 *
		 * Capacity guard (issue #13 + #15): surface_object->source_data
		 * points at an OUTPUT-pool mmap of size source_size, negotiated
		 * at S_FMT time. A stream-level resolution upshift can produce
		 * a slice larger than this allocation. Each append site below
		 * computes the post-append running total and calls
		 * codec_store_buffer_ensure_capacity, which transparently grows
		 * the OUTPUT pool (request_pool_resize) so the existing memcpy
		 * has room. The hard error path (VA_STATUS_ERROR_ALLOCATION_FAILED)
		 * only fires if both the heap save buffer AND the kernel-side
		 * grow fail — at which point libavcodec recreates the surface.
		 */
		size_t need;
		VAStatus ensure_rc;

		if (context->h264_start_code) {
			static const char start_code[3] = { 0x00, 0x00, 0x01 };

			need = (size_t)surface_object->slices_size +
			       sizeof(start_code);
			ensure_rc = codec_store_buffer_ensure_capacity(
				driver_data, surface_object, need);
			if (ensure_rc != VA_STATUS_SUCCESS)
				return ensure_rc;
			memcpy(surface_object->source_data +
			       surface_object->slices_size,
			       start_code, sizeof(start_code));
			surface_object->slices_size += sizeof(start_code);
		}
		/*
		 * iter33 α-30: VP8 OUTPUT buffer needs the uncompressed
		 * frame header that ffmpeg-vaapi stripped before submitting
		 * VASliceData. Hantro's vp8_dec_run reads OUTPUT[0..N] with
		 * an assumed offset of 10 bytes (keyframe) or 3 bytes
		 * (interframe) before the first_partition data — see
		 * rockchip_vpu2_hw_vp8_dec.c:349.
		 *
		 * ffmpeg-vaapi (vaapi_vp8.c:191-192) strips
		 *   header_size = 3 + 7 * s->keyframe
		 * before submitting the slice data, so libva needs to
		 * pre-pad the OUTPUT with that many bytes. Hantro only
		 * uses these bytes for offset arithmetic, not parsing,
		 * so zero-filled placeholder is sufficient.
		 *
		 * ffmpeg-v4l2request (kdirect path) does NOT strip the
		 * header, hence its OUTPUT is byte-equal to SW reference
		 * and decode works correctly. This is the only material
		 * difference between the two front-ends for VP8.
		 *
		 * key_frame in VAAPI's pic_fields.bits is INVERTED:
		 *   0 → keyframe, 1 → interframe.
		 */
		if (profile == VAProfileVP8Version0_3 &&
		    surface_object->params.vp8.iqmatrix_set /* picture parsed by now */) {
			unsigned int header_size =
				surface_object->params.vp8.picture.pic_fields.bits.key_frame == 0 ?
					10 : 3;
			need = (size_t)surface_object->slices_size + header_size;
			ensure_rc = codec_store_buffer_ensure_capacity(
				driver_data, surface_object, need);
			if (ensure_rc != VA_STATUS_SUCCESS)
				return ensure_rc;
			memset(surface_object->source_data +
			       surface_object->slices_size,
			       0, header_size);
			surface_object->slices_size += header_size;
		}
		{
			size_t payload = (size_t)buffer_object->size *
					 buffer_object->count;
			need = (size_t)surface_object->slices_size + payload;
			ensure_rc = codec_store_buffer_ensure_capacity(
				driver_data, surface_object, need);
			if (ensure_rc != VA_STATUS_SUCCESS)
				return ensure_rc;
			memcpy(surface_object->source_data +
				       surface_object->slices_size,
			       buffer_object->data, payload);
			surface_object->slices_size += payload;
		}
		surface_object->slices_count++;
		break;
	}

	case VAPictureParameterBufferType:
		switch (profile) {
		case VAProfileMPEG2Simple:
		case VAProfileMPEG2Main:
			memcpy(&surface_object->params.mpeg2.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.mpeg2.picture));
			break;

		case VAProfileH264Main:
		case VAProfileH264High:
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264MultiviewHigh:
		case VAProfileH264StereoHigh:
		case VAProfileH264High10:
			memcpy(&surface_object->params.h264.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.h264.picture));
			break;

		case VAProfileHEVCMain:
		case VAProfileHEVCMain10:
			memcpy(&surface_object->params.h265.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.h265.picture));
			break;

		case VAProfileVP8Version0_3:
			memcpy(&surface_object->params.vp8.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.vp8.picture));
			break;

		case VAProfileVP9Profile0:
			memcpy(&surface_object->params.vp9.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.vp9.picture));
			break;

		case VAProfileAV1Profile0:
			memcpy(&surface_object->params.av1.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.av1.picture));
			break;

		default:
			break;
		}
		break;

	case VASliceParameterBufferType:
		switch (profile) {
		case VAProfileH264Main:
		case VAProfileH264High:
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264MultiviewHigh:
		case VAProfileH264StereoHigh:
		case VAProfileH264High10:
			memcpy(&surface_object->params.h264.slice,
			       buffer_object->data,
			       sizeof(surface_object->params.h264.slice));
			break;

		case VAProfileHEVCMain:
		case VAProfileHEVCMain10: {
			unsigned int n = surface_object->params.h265.num_slices;
			if (n < HEVC_MAX_SLICES_PER_FRAME) {
				memcpy(&surface_object->params.h265.slices[n],
				       buffer_object->data,
				       sizeof(VASliceParameterBufferHEVC));
				surface_object->params.h265.num_slices = n + 1;
			}
			/* Keep .slice mirror populated as last-slice ref for
			 * h265_fill_pps which reads dependent_slice_segment_flag */
			memcpy(&surface_object->params.h265.slice,
			       buffer_object->data,
			       sizeof(surface_object->params.h265.slice));
			break;
		}

		case VAProfileVP8Version0_3:
			memcpy(&surface_object->params.vp8.slice,
			       buffer_object->data,
			       sizeof(surface_object->params.vp8.slice));
			break;

		case VAProfileVP9Profile0:
			memcpy(&surface_object->params.vp9.slice,
			       buffer_object->data,
			       sizeof(surface_object->params.vp9.slice));
			break;

		default:
			break;
		}
		break;

	case VAIQMatrixBufferType:
		switch (profile) {
		case VAProfileMPEG2Simple:
		case VAProfileMPEG2Main:
			memcpy(&surface_object->params.mpeg2.iqmatrix,
			       buffer_object->data,
			       sizeof(surface_object->params.mpeg2.iqmatrix));
			surface_object->params.mpeg2.iqmatrix_set = true;
			break;

		case VAProfileH264Main:
		case VAProfileH264High:
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264MultiviewHigh:
		case VAProfileH264StereoHigh:
		case VAProfileH264High10:
			memcpy(&surface_object->params.h264.matrix,
			       buffer_object->data,
			       sizeof(surface_object->params.h264.matrix));
			surface_object->params.h264.matrix_set = true;
			break;

		case VAProfileHEVCMain:
		case VAProfileHEVCMain10:
			memcpy(&surface_object->params.h265.iqmatrix,
			       buffer_object->data,
			       sizeof(surface_object->params.h265.iqmatrix));
			surface_object->params.h265.iqmatrix_set = true;
			break;

		case VAProfileVP8Version0_3:
			memcpy(&surface_object->params.vp8.iqmatrix,
			       buffer_object->data,
			       sizeof(surface_object->params.vp8.iqmatrix));
			surface_object->params.vp8.iqmatrix_set = true;
			break;

		default:
			break;
		}
		break;

	case VAProbabilityBufferType:
		switch (profile) {
		case VAProfileVP8Version0_3:
			memcpy(&surface_object->params.vp8.probability,
			       buffer_object->data,
			       sizeof(surface_object->params.vp8.probability));
			surface_object->params.vp8.probability_set = true;
			break;

		default:
			break;
		}
		break;

	default:
		break;
	}

	return VA_STATUS_SUCCESS;
}

static VAStatus codec_set_controls(struct request_data *driver_data,
				   struct object_context *context,
				   VAProfile profile,
				   struct object_surface *surface_object)
{
	int rc;

	switch (profile) {
	case VAProfileMPEG2Simple:
	case VAProfileMPEG2Main:
		rc = mpeg2_set_controls(driver_data, context, surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	case VAProfileH264Main:
	case VAProfileH264High:
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264MultiviewHigh:
	case VAProfileH264StereoHigh:
	case VAProfileH264High10:
		rc = h264_set_controls(driver_data, context, profile,
				       surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	case VAProfileHEVCMain:
	case VAProfileHEVCMain10:
		rc = h265_set_controls(driver_data, context, surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	case VAProfileVP8Version0_3:
		rc = vp8_set_controls(driver_data, context, surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	case VAProfileVP9Profile0:
		rc = vp9_set_controls(driver_data, context, surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	case VAProfileAV1Profile0:
		/*
		 * Populates V4L2_CID_STATELESS_AV1_SEQUENCE from
		 * VAPictureParameterBufferAV1.  The daedalus_v4l2 daemon
		 * (issue #11 daemon track) synthesises an OBU_SEQUENCE_HEADER
		 * from this ctrl and prepends it to the slice bitstream
		 * before handing it to libavcodec/libdav1d, which otherwise
		 * cannot parse the (sequence-header-stripped) OUTPUT buffer
		 * that ffmpeg-vaapi delivers.
		 *
		 * On the RK3588 vpu981 hardware path the same SEQUENCE ctrl
		 * is harmless: vpu981's driver parses the OBU stream
		 * directly and ignores the ctrl payload, so no per-decoder
		 * gating is required here.
		 */
		rc = av1_set_controls(driver_data, context, surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	default:
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	}

	return VA_STATUS_SUCCESS;
}

VAStatus RequestBeginPicture(VADriverContextP context, VAContextID context_id,
			     VASurfaceID surface_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *context_object;
	struct object_surface *surface_object;
	struct request_pool_slot *slot;
	int slot_index;


	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	/* Encoder entrypoint: no cap_pool / request_pool involvement. */
	if (context_object->encoder != NULL) {
		encoder_begin_picture(driver_data, context_object,
				      surface_object);
		return VA_STATUS_SUCCESS;
	}

	if (surface_object->status == VASurfaceRendering)
		RequestSyncSurface(context, surface_id);

	/*
	 * A decode session must not inherit encoder-owned NV12 storage: an
	 * earlier vaDeriveImage() can have allocated it (ffmpeg probes
	 * derive_works on a fresh surface before any context exists).  The
	 * decoded pixels live in the cap_pool slot, so drop the storage.
	 */
	if (surface_object->enc_data != NULL)
		encoder_surface_free_storage(surface_object);

	/*
	 * Iter2 Fix 3: acquire a CAPTURE-pool slot for this decode cycle.
	 * If the surface still holds a slot from a prior decode (DECODED
	 * or EXPORTED — the consumer is done with it by definition since
	 * we got back to BeginPicture for the same surface), release it
	 * first. The new slot is bound and its V4L2 index + mmap pointers
	 * are mirrored into surface_object->destination_* so the existing
	 * QBUF/DQBUF/EXPBUF code paths see no behavioral change.
	 */
	if (surface_object->current_slot != NULL)
		surface_unbind_slot(driver_data, surface_object);
	{
		struct cap_pool_slot *cap_slot =
			cap_pool_acquire(&driver_data->capture_pool, surface_id);
		if (cap_slot == NULL)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		surface_bind_slot(surface_object, cap_slot);

		/*
		 * iter8 Phase 7 IMP-1 experiment: env-gated CAPTURE buffer
		 * pre-zero. LIBVA_V4L2_ZERO_CAPTURE=1 wipes the slot's mmap'd
		 * region before kernel decode. Discriminates "kernel writes
		 * partial then aborts" from "kernel writes nothing and we
		 * see stale residue."
		 */
		{
			static const char *zero_env = NULL;
			static bool zero_env_checked = false;
			if (!zero_env_checked) {
				zero_env = getenv("LIBVA_V4L2_ZERO_CAPTURE");
				zero_env_checked = true;
			}
			if (zero_env != NULL && zero_env[0] == '1') {
				unsigned int b;
				for (b = 0; b < cap_slot->buffers_count; b++)
					if (cap_slot->map[b] != NULL)
						memset(cap_slot->map[b], 0,
						       cap_slot->map_lengths[b]);
			}
		}
	}

	/*
	 * Borrow an OUTPUT (bitstream-input) slot from the driver-wide
	 * pool for the duration of this Begin/Render/End cycle. The
	 * surface's source_* fields hold the borrow's mmap pointer/size/
	 * V4L2 buffer index until RequestSyncSurface releases it after
	 * VIDIOC_DQBUF.
	 */
	slot_index = request_pool_acquire(&driver_data->output_pool);
	if (slot_index < 0)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	slot = request_pool_slot(&driver_data->output_pool,
				 (unsigned int)slot_index);
	if (slot == NULL) {
		request_pool_release(&driver_data->output_pool,
				     (unsigned int)slot_index);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	surface_object->source_index = slot->index;
	surface_object->source_data = slot->data;
	surface_object->source_size = slot->size;
	/*
	 * iter6: bind the slot's permanent request_fd to this surface for the
	 * duration of the decode cycle. Replaces the iter4 close+alloc-per-
	 * frame model. The fd is REINIT'd (not closed) at RequestSyncSurface,
	 * so the kernel-side request object is reset in place — no fd-reuse
	 * race with another slot's pending decode.
	 */
	surface_object->request_fd = slot->request_fd;
	surface_object->slices_size = 0;
	surface_object->slices_count = 0;
	surface_object->params.h264.matrix_set = false;
	surface_object->params.h265.num_slices = 0;
	surface_object->params.vp8.iqmatrix_set = false;
	surface_object->params.vp8.probability_set = false;

	surface_object->status = VASurfaceRendering;
	context_object->render_surface_id = surface_id;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestRenderPicture(VADriverContextP context, VAContextID context_id,
			      VABufferID *buffers_ids, int buffers_count)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *context_object;
	struct object_config *config_object;
	struct object_surface *surface_object;
	struct object_buffer *buffer_object;
	int rc;
	int i;

	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	if (context_object->encoder != NULL)
		return encoder_render_picture(driver_data, context_object,
					      buffers_ids, buffers_count);

	config_object = CONFIG(driver_data, context_object->config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	surface_object =
		SURFACE(driver_data, context_object->render_surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	for (i = 0; i < buffers_count; i++) {
		buffer_object = BUFFER(driver_data, buffers_ids[i]);
		if (buffer_object == NULL)
			return VA_STATUS_ERROR_INVALID_BUFFER;

		rc = codec_store_buffer(driver_data, context_object,
					config_object->profile,
					surface_object, buffer_object);
		if (rc != VA_STATUS_SUCCESS)
			return rc;
	}

	return VA_STATUS_SUCCESS;
}

VAStatus RequestEndPicture(VADriverContextP context, VAContextID context_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *context_object;
	struct object_config *config_object;
	struct object_surface *surface_object;
	struct video_format *video_format;
	unsigned int output_type, capture_type;
	int request_fd;
	VAStatus status;
	int rc;

	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	/*
	 * Encoder entrypoint: vaEndPicture() does the whole submit/wait and
	 * fills the VA coded buffer, so the decoder's video_format gate below
	 * must not apply (encoder sessions never populate it).
	 */
	if (context_object->encoder != NULL)
		return encoder_end_picture(driver_data, context_object);

	video_format = driver_data->video_format;
	if (video_format == NULL)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	output_type = v4l2_type_video_output(video_format->v4l2_mplane);
	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	config_object = CONFIG(driver_data, context_object->config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	surface_object =
		SURFACE(driver_data, context_object->render_surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	/*
	 * iter9 α-7: monotonic per-context counter instead of gettimeofday,
	 * so DPB.reference_ts / OUTPUT QBUF ts stay small (matches
	 * ffmpeg-v4l2request's pattern). Confirmed in iter30 sweep
	 * (1×, 1000×, 1000000× multipliers all produce identical output);
	 * the counter scheme works on both rkvdec and hantro vb2_find_buffer.
	 */
	context_object->timestamp_counter++;
	surface_object->timestamp.tv_sec =
		(time_t)(context_object->timestamp_counter / 1000000);
	surface_object->timestamp.tv_usec =
		(suseconds_t)(context_object->timestamp_counter % 1000000);

	/*
	 * iter6: request_fd was bound to the surface in BeginPicture from
	 * the OUTPUT pool slot's permanent fd. Per-frame allocation is gone.
	 */
	request_fd = surface_object->request_fd;
	if (request_fd < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	rc = codec_set_controls(driver_data, context_object,
				config_object->profile, surface_object);
	if (rc != VA_STATUS_SUCCESS)
		return rc;

	/*
	 * iter14 α-16: env-gated dump of OUTPUT bitstream bytes immediately
	 * before QBUF. LIBVA_V4L2_DUMP_OUTPUT=<dir> writes source_data[0..
	 * slices_size] to <dir>/output_<profile>_<surface>_<frame>.bin.
	 * Discriminates whether libva writes the same H.264/HEVC slice bytes
	 * as kdirect — if YES, Bug 4/5 are not in the OUTPUT-side; if NO,
	 * narrow to which slice-write path produces the divergence.
	 *
	 * Off by default; no behavior change when env unset.
	 */
	{
		static const char *dump_env = NULL;
		static bool dump_env_checked = false;
		if (!dump_env_checked) {
			dump_env = getenv("LIBVA_V4L2_DUMP_OUTPUT");
			dump_env_checked = true;
		}
		if (dump_env != NULL && dump_env[0] != '\0' &&
		    surface_object->source_data != NULL &&
		    surface_object->slices_size > 0) {
			char path[256];
			snprintf(path, sizeof(path),
				 "%s/output_p%d_s%u_t%llu.bin",
				 dump_env, (int)config_object->profile,
				 (unsigned int)surface_object->base.id,
				 (unsigned long long)context_object->timestamp_counter);
			FILE *fp = fopen(path, "wb");
			if (fp != NULL) {
				size_t w = fwrite(surface_object->source_data,
						  1, surface_object->slices_size,
						  fp);
				request_log("α-16: dumped %zu bytes to %s "
					    "(slices_count=%u)\n",
					    w, path,
					    surface_object->slices_count);
				fclose(fp);
			} else {
				request_log("α-16: fopen(%s) failed: %s\n",
					    path, strerror(errno));
			}
		}
	}

	rc = v4l2_queue_buffer(driver_data->video_fd, -1, capture_type, NULL,
			       surface_object->destination_index, 0,
			       surface_object->destination_buffers_count);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	rc = v4l2_queue_buffer(driver_data->video_fd, request_fd, output_type,
			       &surface_object->timestamp,
			       surface_object->source_index,
			       surface_object->slices_size, 1);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	surface_object->slices_size = 0;

	status = RequestSyncSurface(context, context_object->render_surface_id);
	if (status != VA_STATUS_SUCCESS)
		return status;

	context_object->render_surface_id = VA_INVALID_ID;

	return VA_STATUS_SUCCESS;
}
