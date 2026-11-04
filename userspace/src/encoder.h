/*
 * VA-API H.264 encoder frontend for the RK3399 VEPU2 (hantro-vpu).
 */

#ifndef _ENCODER_H_
#define _ENCODER_H_

#include <stdbool.h>
#include <stdint.h>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_enc_h264.h>

#include <linux/videodev2.h>

#include "h264_enc_ctrls.h"

struct request_data;
struct object_context;
struct object_surface;

#define ENCODER_MAX_PLANES		2

struct encoder_buffer {
	unsigned int index;
	unsigned int planes_count;
	void *map[ENCODER_MAX_PLANES];
	unsigned int lengths[ENCODER_MAX_PLANES];
	int request_fd;
	uint64_t timestamp;
};

struct encoder_context {
	int video_fd;
	int media_fd;

	VAProfile profile;
	unsigned int width;
	unsigned int height;
	unsigned int mb_width;
	unsigned int mb_height;

	unsigned int output_type;
	unsigned int capture_type;
	bool streaming;

	/* Driver-negotiated NV12M geometry. */
	unsigned int output_width;
	unsigned int output_height;
	unsigned int output_planes_count;
	unsigned int output_bytesperlines[ENCODER_MAX_PLANES];
	unsigned int output_sizes[ENCODER_MAX_PLANES];

	/* Driver-negotiated H264_SLICE geometry. */
	unsigned int capture_width;
	unsigned int capture_height;
	unsigned int capture_sizeimage;

	struct encoder_buffer output[8];
	unsigned int output_count;
	unsigned int output_index;

	struct encoder_buffer capture[8];
	unsigned int capture_count;
	unsigned int capture_index;

	/* Per-picture VA parameter buffers collected in vaRenderPicture(). */
	bool seq_valid;
	bool pic_valid;
	bool slice_valid;
	VAEncSequenceParameterBufferH264 seq;
	VAEncPictureParameterBufferH264 pic;
	VAEncSliceParameterBufferH264 slice;
	VABufferID coded_buf;
	int misc_qp;

	/* Rate control (CBR/VBR): frame-level QP controller driven by the
	 * misc RateControl/FrameRate parameters and the encoded size. */
	bool rc_active;
	unsigned int rc_target_bps;
	unsigned int rc_fps;
	int rc_qp;
	int rc_qp_min;
	int rc_qp_max;

	/* Prepared V4L2 control payloads. */
	struct v4l2_ctrl_h264_encode_params params;
	struct v4l2_ctrl_h264_encode_rc rc;

	/* Reference bookkeeping (vb2_find_buffer on the CAPTURE queue). */
	uint64_t last_ts;
	uint64_t timestamp;
	unsigned long long frame_count;
};

/*
 * Probe /dev/video0..31 for the hantro VEPU2 encoder instance and open it
 * together with the media device that owns its request API.  Returns 0 and
 * fills the two fd outputs on success, -1 when no encoder is present.
 */
int encoder_device_probe(int *video_fd_out, int *media_fd_out);

struct encoder_context *encoder_context_create(struct request_data *driver_data,
					       VAProfile profile,
					       unsigned int width,
					       unsigned int height);
void encoder_context_destroy(struct encoder_context *ec);

void encoder_begin_picture(struct request_data *driver_data,
			   struct object_context *context_object,
			   struct object_surface *surface_object);
VAStatus encoder_render_picture(struct request_data *driver_data,
				struct object_context *context_object,
				VABufferID *buffer_ids, int buffers_count);
VAStatus encoder_end_picture(struct request_data *driver_data,
			     struct object_context *context_object);

/* Encoder-owned per-surface NV12 storage. */
int encoder_surface_ensure_storage(struct object_surface *surface_object);
void encoder_surface_free_storage(struct object_surface *surface_object);
int encoder_surface_put_image(struct object_surface *surface_object,
			      VAImage *image, void *image_data);
int encoder_surface_get_image(struct object_surface *surface_object,
			      VAImage *image, void *image_data);
int encoder_surface_put_image_rect(struct object_surface *surface_object,
				   VAImage *image, void *image_data,
				   int src_x, int src_y, int dst_x, int dst_y,
				   unsigned int width, unsigned int height);
int encoder_surface_get_image_rect(struct object_surface *surface_object,
				   VAImage *image, void *image_data,
				   int src_x, int src_y, int dst_x, int dst_y,
				   unsigned int width, unsigned int height);
VAStatus encoder_derive_image(VADriverContextP context,
			      struct object_surface *surface_object,
			      VAImage *image);

#endif /* _ENCODER_H_ */
