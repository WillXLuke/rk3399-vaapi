/*
 * VA-API H.264 encoder frontend for the Rockchip RK3399 VEPU2 (hantro).
 *
 * This maps VAEntrypointEncSlice onto the V4L2 stateless H.264 encode UAPI
 * (`V4L2_CID_STATELESS_H264_ENCODE_PARAMS` / `_RC`) implemented by the
 * hantro-vpu driver for the `rockchip,rk3399-vpu-enc` instance.
 *
 * Hardware constraints that shape this frontend (verified against the
 * Bootlin reference tool, see kernel/hantro-enc/VA-FRONTEND-VERIFIED.md):
 *
 *   - The hardware emits only the slice NAL.  SPS and PPS are written by
 *     userspace, so PackedHeaders is advertised as 0 and we serialise
 *     SPS+PPS ourselves on IDR pictures.
 *   - VEPU2 writes a 16-bit frame_num and no pic_order_cnt_lsb, hence the
 *     SPS must use log2_max_frame_num_minus4 = 12 and
 *     pic_order_cnt_type = 2 regardless of what the application asks for.
 *   - A P picture's reference is located by the kernel with
 *     vb2_find_buffer() on the CAPTURE queue using the ENCODE_PARAMS
 *     reference_ts; that is the timestamp the peer CAPTURE buffer inherited
 *     from the previous OUTPUT queueing (TIMESTAMP_COPY).
 *   - Exactly one reference frame, no B frames, CQP only.
 */

#include "encoder.h"

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <va/va_drmcommon.h>

/* File-gated diagnostics: touch /tmp/libva-enc-debug to enable. */
static FILE *enc_debug_file(void)
{
	static FILE *f;
	static bool checked;

	if (!checked) {
		checked = true;
		if (access("/tmp/libva-enc-debug", F_OK) == 0)
			f = fopen("/tmp/libva-enc-debug.log", "a");
	}
	return f;
}

#define ENCDBG(...) do { \
	FILE *dbgf = enc_debug_file(); \
	if (dbgf) { fprintf(dbgf, "[%d] ", (int)getpid()); \
		    fprintf(dbgf, __VA_ARGS__); fflush(dbgf); } \
} while (0)


#include "buffer.h"
#include "config.h"
#include "context.h"
#include "image.h"
#include "request.h"
#include "surface.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/media.h>
#include <linux/videodev2.h>

#include "media.h"
#include "utils.h"
#include "v4l2.h"

/* Number of mmap'd buffers per queue (Bootlin reference uses 3; we keep a
 * little more headroom so a consumer can hold an input surface across
 * frames without starving the OUTPUT rotation). */
#define ENCODER_OUTPUT_BUFFERS		4
#define ENCODER_CAPTURE_BUFFERS		4

/* CAPTURE sizeimage: the encoder writes one slice NAL per frame.  1 MiB is
 * comfortably above 1080p worst case and matches what the driver defaults
 * to for 640x480 (w*h*2). */
#define ENCODER_CAPTURE_SIZEIMAGE	(1024 * 1024)

/* ------------------------------------------------------------------------
 * Bit writer (RBSP) + Annex-B NAL packing
 * --------------------------------------------------------------------- */

struct encoder_bitwriter {
	uint8_t *data;
	unsigned int capacity;
	unsigned int bit_offset;
};

static void bw_init(struct encoder_bitwriter *bw, uint8_t *data,
		    unsigned int capacity)
{
	bw->data = data;
	bw->capacity = capacity;
	bw->bit_offset = 0;
	memset(data, 0, capacity);
}

static void bw_put_bits(struct encoder_bitwriter *bw, uint32_t value,
			unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		unsigned int bit = (value >> (count - 1 - i)) & 1;
		unsigned int byte = bw->bit_offset >> 3;
		unsigned int shift = 7 - (bw->bit_offset & 7);

		if (byte >= bw->capacity)
			return;
		bw->data[byte] |= bit << shift;
		bw->bit_offset++;
	}
}

static void bw_put_ue(struct encoder_bitwriter *bw, uint32_t value)
{
	unsigned int bits = 0;
	uint32_t v = value + 1;

	while ((v >> bits) > 1)
		bits++;

	bw_put_bits(bw, 0, bits);
	bw_put_bits(bw, v, bits + 1);
}

static void bw_put_se(struct encoder_bitwriter *bw, int32_t value)
{
	uint32_t mapped = value > 0 ? (uint32_t)(2 * value - 1)
				    : (uint32_t)(-2 * value);

	bw_put_ue(bw, mapped);
}

/* rbsp_stop_one_bit followed by zero-bit alignment to a byte boundary. */
static unsigned int bw_finish(struct encoder_bitwriter *bw)
{
	bw_put_bits(bw, 1, 1);
	while (bw->bit_offset & 7)
		bw_put_bits(bw, 0, 1);

	return (bw->bit_offset + 7) / 8;
}

/*
 * Wrap `rbsp` in an Annex-B NAL unit: 4-byte start code, then the bytes
 * with emulation prevention applied (0x03 inserted after 00 00 followed by
 * a byte <= 0x03).  Returns the number of bytes written, 0 on overflow.
 */
static unsigned int nal_pack(uint8_t *out, unsigned int out_size,
			     const uint8_t *rbsp, unsigned int rbsp_size)
{
	unsigned int i, o = 0;
	unsigned int zeros = 0;

	if (out_size < 4)
		return 0;

	out[o++] = 0x00;
	out[o++] = 0x00;
	out[o++] = 0x00;
	out[o++] = 0x01;

	for (i = 0; i < rbsp_size; i++) {
		uint8_t byte = rbsp[i];

		if (zeros >= 2 && byte <= 0x03) {
			if (o + 1 >= out_size)
				return 0;
			out[o++] = 0x03;
			zeros = 0;
		}
		if (o >= out_size)
			return 0;
		out[o++] = byte;

		if (byte == 0x00)
			zeros++;
		else
			zeros = 0;
	}

	/* Never let the last emitted byte be a zero: it could be absorbed by
	 * the following start code's leading zero bytes. */
	if (o > 0 && out[o - 1] == 0x00) {
		if (o >= out_size)
			return 0;
		out[o++] = 0x03;
	}

	return o;
}

/* ------------------------------------------------------------------------
 * SPS / PPS serialisation
 * --------------------------------------------------------------------- */

static unsigned int encoder_profile_idc(VAProfile profile)
{
	switch (profile) {
	case VAProfileH264ConstrainedBaseline:
		return 66;
	case VAProfileH264Main:
		return 77;
	case VAProfileH264High:
		return 100;
	default:
		return 100;
	}
}

static bool encoder_profile_has_high_syntax(VAProfile profile)
{
	return profile == VAProfileH264High;
}

/*
 * SPS.  Deliberately mirrors the reference stream layout:
 *   profile High, constraint_set0|1, level from the VA sequence buffer,
 *   pic_order_cnt_type = 2, log2_max_frame_num_minus4 = 12, one reference,
 *   frame_mbs_only, direct_8x8_inference, cropping straight from the VA
 *   sequence buffer (ffmpeg fills the crop from the *display* width, which
 *   we can't reconstruct from the MB count alone).
 */
/*
 * Smallest H.264 level whose frame-size and macroblock-rate limits fit this
 * stream.  ffmpeg's level hint is often too low for the actual frame rate
 * (e.g. 1600x900@60 was declared level 4.0, which caps out at ~30 fps for
 * that size), and hardware decoders do enforce these limits.
 */
static unsigned int encoder_h264_level(unsigned int mb_width,
				       unsigned int mb_height,
				       unsigned int fps)
{
	static const struct { unsigned int idc, max_fs, max_mbps; } levels[] = {
		{ 10, 99, 1485 },     { 11, 396, 3000 },    { 12, 396, 6000 },
		{ 13, 396, 11880 },   { 20, 396, 11880 },   { 21, 792, 19800 },
		{ 22, 1620, 20250 },  { 30, 1620, 40500 },  { 31, 3600, 108000 },
		{ 32, 5120, 216000 }, { 40, 8192, 245760 }, { 41, 8192, 245760 },
		{ 42, 8704, 522240 }, { 50, 22080, 589824 }, { 51, 36864, 983040 },
		{ 52, 36864, 2073600 }, { 60, 139264, 4177920 },
	};
	unsigned int fs = mb_width * mb_height;
	unsigned int mbps = fs * (fps ? fps : 30);
	unsigned int i;

	for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		if (levels[i].max_fs >= fs && levels[i].max_mbps >= mbps)
			return levels[i].idc;
	}
	return 62;
}

static unsigned int encoder_build_sps(struct encoder_context *ec, uint8_t *out,
				      unsigned int out_size)
{
	const VAEncSequenceParameterBufferH264 *seq = &ec->seq;
	unsigned int profile_idc = encoder_profile_idc(ec->profile);
	unsigned int mb_width = seq->picture_width_in_mbs ?
				seq->picture_width_in_mbs : ec->mb_width;
	unsigned int mb_height = seq->picture_height_in_mbs ?
				 seq->picture_height_in_mbs : ec->mb_height;
	uint8_t rbsp[256];
	struct encoder_bitwriter bw;
	unsigned int len;

	bw_init(&bw, rbsp, sizeof(rbsp));

	/* NAL header: nal_ref_idc = 3, nal_unit_type = 7 (SPS). */
	bw_put_bits(&bw, 0, 1);
	bw_put_bits(&bw, 3, 2);
	bw_put_bits(&bw, 7, 5);

	bw_put_bits(&bw, profile_idc, 8);
	/*
	 * Constraint flags: Baseline/Constrained Baseline set
	 * constraint_set0, Main additionally constraint_set1.  High profile
	 * streams leave them clear.
	 */
	bw_put_bits(&bw, profile_idc == 100 ? 0 : 1, 1);
	bw_put_bits(&bw, profile_idc == 100 ? 0 :
		    (ec->profile != VAProfileH264ConstrainedBaseline), 1);
	bw_put_bits(&bw, 0, 1);
	bw_put_bits(&bw, 0, 1);
	bw_put_bits(&bw, 0, 1);
	bw_put_bits(&bw, 0, 1);
	bw_put_bits(&bw, 0, 2);
	{
		unsigned int level = encoder_h264_level(mb_width, mb_height,
						       ec->rc_fps);

		if (seq->level_idc > level)
			level = seq->level_idc;
		bw_put_bits(&bw, level, 8);
	}
	bw_put_ue(&bw, seq->seq_parameter_set_id);

	if (encoder_profile_has_high_syntax(ec->profile) || profile_idc >= 100) {
		bw_put_ue(&bw, 1);	/* chroma_format_idc = 4:2:0 */
		bw_put_ue(&bw, 0);	/* bit_depth_luma_minus8 */
		bw_put_ue(&bw, 0);	/* bit_depth_chroma_minus8 */
		bw_put_bits(&bw, 0, 1);	/* qpprime_y_zero_transform_bypass */
		bw_put_bits(&bw, 0, 1);	/* seq_scaling_matrix_present */
	}

	/* Mandatory for VEPU2: 16-bit frame_num, no POC LSB. */
	bw_put_ue(&bw, 12);	/* log2_max_frame_num_minus4 */
	bw_put_ue(&bw, 2);	/* pic_order_cnt_type */

	bw_put_ue(&bw, 1);	/* max_num_ref_frames = 1 */
	bw_put_bits(&bw, 0, 1);	/* gaps_in_frame_num_value_allowed_flag */
	bw_put_ue(&bw, mb_width - 1);
	bw_put_ue(&bw, mb_height - 1);
	bw_put_bits(&bw, 1, 1);	/* frame_mbs_only_flag */
	bw_put_bits(&bw, 1, 1);	/* direct_8x8_inference_flag */

	bw_put_bits(&bw, seq->frame_cropping_flag ? 1 : 0, 1);
	if (seq->frame_cropping_flag) {
		bw_put_ue(&bw, seq->frame_crop_left_offset);
		bw_put_ue(&bw, seq->frame_crop_right_offset);
		bw_put_ue(&bw, seq->frame_crop_top_offset);
		bw_put_ue(&bw, seq->frame_crop_bottom_offset);
	}

	/* VUI: timing only.  Hardware decoders and browser pipelines use it
	 * to derive the display frame rate. */
	bw_put_bits(&bw, 1, 1);	/* vui_parameters_present_flag */
	bw_put_bits(&bw, 0, 1);	/* aspect_ratio_info_present_flag */
	bw_put_bits(&bw, 0, 1);	/* overscan_info_present_flag */
	bw_put_bits(&bw, 0, 1);	/* video_signal_type_present_flag */
	bw_put_bits(&bw, 0, 1);	/* chroma_loc_info_present_flag */
	bw_put_bits(&bw, 1, 1);	/* timing_info_present_flag */
	bw_put_bits(&bw, 1, 32);	/* num_units_in_tick */
	bw_put_bits(&bw, 2 * (ec->rc_fps ? ec->rc_fps : 30), 32); /* time_scale */
	bw_put_bits(&bw, 1, 1);	/* fixed_frame_rate_flag */
	bw_put_bits(&bw, 0, 1);	/* nal_hrd_parameters_present_flag */
	bw_put_bits(&bw, 0, 1);	/* vcl_hrd_parameters_present_flag */
	bw_put_bits(&bw, 0, 1);	/* pic_struct_present_flag */
	bw_put_bits(&bw, 0, 1);	/* bitstream_restriction_flag */

	len = bw_finish(&bw);

	return nal_pack(out, out_size, rbsp, len);
}

/*
 * PPS.  deblocking_filter_control_present_flag = 1 and CABAC are fixed by
 * the hardware (it writes the slice header and expects the matching PPS).
 * For High profile ffmpeg enables transform_8x8_mode, which adds the
 * more_rbsp_data() PPS tail; Baseline/Main must not carry it.
 */
static unsigned int encoder_build_pps(struct encoder_context *ec, uint8_t *out,
				      unsigned int out_size)
{
	const VAEncPictureParameterBufferH264 *pic = &ec->pic;
	bool high = encoder_profile_has_high_syntax(ec->profile);
	bool cabac = !!pic->pic_fields.bits.entropy_coding_mode_flag;
	bool transform_8x8 = high && !!pic->pic_fields.bits.transform_8x8_mode_flag;
	uint8_t rbsp[256];
	struct encoder_bitwriter bw;
	unsigned int len;

	/*
	 * CABAC is only forbidden for Baseline/Constrained Baseline.  Main
	 * allows CABAC (and normally uses it), so gating this on the "high
	 * syntax" helper made the PPS advertise CAVLC while the hardware was
	 * programmed for CABAC -> a decodable but completely wrong stream.
	 */
	if (ec->profile == VAProfileH264ConstrainedBaseline)
		cabac = false;

	bw_init(&bw, rbsp, sizeof(rbsp));

	/* NAL header: nal_ref_idc = 3, nal_unit_type = 8 (PPS). */
	bw_put_bits(&bw, 0, 1);
	bw_put_bits(&bw, 3, 2);
	bw_put_bits(&bw, 8, 5);

	bw_put_ue(&bw, 0);	/* pic_parameter_set_id */
	bw_put_ue(&bw, 0);	/* seq_parameter_set_id */
	bw_put_bits(&bw, cabac, 1);	/* entropy_coding_mode_flag */
	bw_put_bits(&bw, 0, 1);	/* bottom_field_pic_order_in_frame_present */
	bw_put_ue(&bw, 0);	/* num_slice_groups_minus1 */
	bw_put_ue(&bw, pic->num_ref_idx_l0_active_minus1);
	bw_put_ue(&bw, 0);	/* num_ref_idx_l1_default_active_minus1 */
	bw_put_bits(&bw, pic->pic_fields.bits.weighted_pred_flag, 1);
	bw_put_bits(&bw, 0, 2);	/* weighted_bipred_idc */
	bw_put_se(&bw, (int8_t)pic->pic_init_qp - 26);	/* pic_init_qp_minus26 */
	bw_put_se(&bw, 0);	/* pic_init_qs_minus26 */
	bw_put_se(&bw, (int8_t)pic->chroma_qp_index_offset);
	bw_put_bits(&bw, 1, 1);	/* deblocking_filter_control_present_flag */
	bw_put_bits(&bw, pic->pic_fields.bits.constrained_intra_pred_flag, 1);
	bw_put_bits(&bw, 0, 1);	/* redundant_pic_cnt_present_flag */

	if (transform_8x8) {
		bw_put_bits(&bw, 1, 1);	/* transform_8x8_mode_flag */
		bw_put_bits(&bw, 0, 1);	/* pic_scaling_matrix_present_flag */
		bw_put_se(&bw, (int8_t)pic->second_chroma_qp_index_offset);
	}

	len = bw_finish(&bw);

	return nal_pack(out, out_size, rbsp, len);
}

/* ------------------------------------------------------------------------
 * V4L2 device helpers
 * --------------------------------------------------------------------- */

/* S_FMT for a multiplanar queue.  sizeimage == 0 lets the driver pick. */
static int encoder_set_format(int video_fd, unsigned int type,
			      unsigned int pixelformat, unsigned int width,
			      unsigned int height, unsigned int sizeimage)
{
	struct v4l2_format format;
	int rc;

	memset(&format, 0, sizeof(format));
	format.type = type;
	format.fmt.pix_mp.width = width;
	format.fmt.pix_mp.height = height;
	format.fmt.pix_mp.pixelformat = pixelformat;
	format.fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage;

	rc = ioctl(video_fd, VIDIOC_S_FMT, &format);
	if (rc < 0) {
		request_log("encoder: S_FMT type %u fourcc %08x %ux%u failed: %s\n",
			    type, pixelformat, width, height, strerror(errno));
		return -1;
	}

	return 0;
}

static void encoder_buffer_teardown(struct encoder_buffer *buffer)
{
	unsigned int i;

	for (i = 0; i < buffer->planes_count; i++) {
		if (buffer->map[i] != NULL && buffer->map[i] != MAP_FAILED)
			munmap(buffer->map[i], buffer->lengths[i]);
		buffer->map[i] = NULL;
	}

	if (buffer->request_fd >= 0)
		close(buffer->request_fd);

	buffer->request_fd = -1;
	buffer->planes_count = 0;
}

static int encoder_buffer_setup(struct encoder_context *ec,
				struct encoder_buffer *buffer,
				unsigned int type, unsigned int index)
{
	struct v4l2_plane planes[ENCODER_MAX_PLANES];
	struct v4l2_buffer buffer_v4l2;
	unsigned int i;
	int rc;

	buffer->request_fd = -1;

	memset(planes, 0, sizeof(planes));
	memset(&buffer_v4l2, 0, sizeof(buffer_v4l2));

	buffer_v4l2.type = type;
	buffer_v4l2.memory = V4L2_MEMORY_MMAP;
	buffer_v4l2.index = index;
	buffer_v4l2.length = ENCODER_MAX_PLANES;
	buffer_v4l2.m.planes = planes;

	rc = ioctl(ec->video_fd, VIDIOC_QUERYBUF, &buffer_v4l2);
	if (rc < 0) {
		request_log("encoder: QUERYBUF type %u index %u failed: %s\n",
			    type, index, strerror(errno));
		return -1;
	}

	buffer->index = index;
	buffer->planes_count = buffer_v4l2.length;

	for (i = 0; i < buffer->planes_count; i++) {
		buffer->lengths[i] = planes[i].length;
		buffer->map[i] = mmap(NULL, planes[i].length,
				      PROT_READ | PROT_WRITE, MAP_SHARED,
				      ec->video_fd, planes[i].m.mem_offset);
		if (buffer->map[i] == MAP_FAILED) {
			request_log("encoder: mmap type %u index %u plane %u failed: %s\n",
				    type, index, i, strerror(errno));
			buffer->map[i] = NULL;
			encoder_buffer_teardown(buffer);
			return -1;
		}
	}

	/*
	 * One media request per OUTPUT buffer, reinitialised with
	 * MEDIA_REQUEST_IOC_REINIT after each completed encode.
	 */
	buffer->request_fd = -1;
	if (type == ec->output_type) {
		buffer->request_fd = media_request_alloc(ec->media_fd);
		if (buffer->request_fd < 0)
			return -1;
	}

	buffer->timestamp = 0;

	return 0;
}

/* ------------------------------------------------------------------------
 * Device probe
 * --------------------------------------------------------------------- */

static int encoder_media_device_for(const char *bus_info, char *media_out,
				    size_t media_out_size)
{
	struct media_device_info info;
	char path[32];
	int fd, i;

	for (i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/dev/media%d", i);
		fd = open(path, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			continue;

		memset(&info, 0, sizeof(info));
		if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) == 0 &&
		    strcmp(info.bus_info, bus_info) == 0) {
			snprintf(media_out, media_out_size, "%s", path);
			close(fd);
			return 0;
		}
		close(fd);
	}

	return -1;
}

/*
 * Auto-detect the VEPU2 encoder instance:
 *   driver "hantro-vpu", card "rockchip,rk3399-vpu-enc" and
 *   V4L2_PIX_FMT_H264_SLICE ('S264') among CAPTURE formats.
 * Node numbering changes across module reloads, so scan /dev/video0..31.
 */
int encoder_device_probe(int *video_fd_out, int *media_fd_out)
{
	struct v4l2_capability capability;
	char path[32];
	int i;

	*video_fd_out = -1;
	*media_fd_out = -1;

	for (i = 0; i < 32; i++) {
		char media_path[32];
		int fd, vfd, mfd;

		snprintf(path, sizeof(path), "/dev/video%d", i);
		fd = open(path, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			continue;

		memset(&capability, 0, sizeof(capability));
		if (ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0 ||
		    strcmp((const char *)capability.driver, "hantro-vpu") != 0 ||
		    strstr((const char *)capability.card, "vpu-enc") == NULL ||
		    !v4l2_find_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				      V4L2_PIX_FMT_H264_SLICE)) {
			close(fd);
			continue;
		}

		/* The V4L2 node's bus_info selects the media device that
		 * owns its request API (same platform device). */
		if (encoder_media_device_for((const char *)capability.bus_info,
					     media_path, sizeof(media_path)) < 0) {
			request_log("encoder: no media device for %s (%s)\n",
				    path, capability.bus_info);
			close(fd);
			return -1;
		}

		vfd = fd;
		mfd = open(media_path, O_RDWR | O_NONBLOCK);
		if (mfd < 0) {
			request_log("encoder: open %s failed: %s\n", media_path,
				    strerror(errno));
			close(vfd);
			continue;
		}

		request_log("encoder: using %s + %s (%s)\n", path, media_path,
			    capability.card);
		*video_fd_out = vfd;
		*media_fd_out = mfd;
		return 0;
	}

	return -1;
}

/* ------------------------------------------------------------------------
 * Context lifecycle
 * --------------------------------------------------------------------- */

struct encoder_context *encoder_context_create(struct request_data *driver_data,
					       VAProfile profile,
					       unsigned int width,
					       unsigned int height)
{
	struct encoder_context *ec;
	struct v4l2_format format;
	unsigned int i;
	int rc;

	ec = calloc(1, sizeof(*ec));
	if (ec == NULL)
		return NULL;

	ec->video_fd = driver_data->video_fd_enc;
	ec->media_fd = driver_data->media_fd_enc;
	ec->profile = profile;
	ec->width = width;
	ec->height = height;
	ec->mb_width = (width + 15) / 16;
	ec->mb_height = (height + 15) / 16;
	ec->output_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ec->capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ec->coded_buf = VA_INVALID_ID;
	ec->last_ts = 0;
	/* Keep ec->timestamp monotonic: the V4L2 queue is reused, so timestamps
	 * must not go backwards across contexts. */
	ec->frame_count = 0;
	for (i = 0; i < 8; i++) {
		ec->output[i].request_fd = -1;
		ec->capture[i].request_fd = -1;
	}

	if (ec->video_fd < 0 || ec->media_fd < 0) {
		free(ec);
		return NULL;
	}

	/*
	 * Order matters: the coded (CAPTURE) format must be committed first,
	 * because hantro_set_fmt_cap() resets the raw OUTPUT format to its
	 * default when S_FMT is issued after it.  This matches the Bootlin
	 * reference tool.
	 */
	rc = encoder_set_format(ec->video_fd, ec->capture_type,
				V4L2_PIX_FMT_H264_SLICE, width, height,
				ENCODER_CAPTURE_SIZEIMAGE);
	if (rc < 0)
		goto error;

	rc = encoder_set_format(ec->video_fd, ec->output_type,
				V4L2_PIX_FMT_NV12M, width, height, 0);
	if (rc < 0)
		goto error;

	/* Read back the geometry the driver settled on. */
	memset(&format, 0, sizeof(format));
	format.type = ec->output_type;
	if (ioctl(ec->video_fd, VIDIOC_G_FMT, &format) < 0) {
		request_log("encoder: G_FMT OUTPUT failed: %s\n", strerror(errno));
		goto error;
	}

	ec->output_planes_count = format.fmt.pix_mp.num_planes;
	if (ec->output_planes_count > ENCODER_MAX_PLANES)
		ec->output_planes_count = ENCODER_MAX_PLANES;

	for (i = 0; i < ec->output_planes_count; i++) {
		ec->output_bytesperlines[i] =
			format.fmt.pix_mp.plane_fmt[i].bytesperline;
		ec->output_sizes[i] = format.fmt.pix_mp.plane_fmt[i].sizeimage;
	}
	ec->output_width = format.fmt.pix_mp.width;
	ec->output_height = format.fmt.pix_mp.height;

	memset(&format, 0, sizeof(format));
	format.type = ec->capture_type;
	if (ioctl(ec->video_fd, VIDIOC_G_FMT, &format) < 0) {
		request_log("encoder: G_FMT CAPTURE failed: %s\n", strerror(errno));
		goto error;
	}
	ec->capture_sizeimage = format.fmt.pix_mp.plane_fmt[0].sizeimage;
	ec->capture_width = format.fmt.pix_mp.width;
	ec->capture_height = format.fmt.pix_mp.height;

	request_log("encoder: %ux%u NV12M planes=%u bpl=%u/%u size=%u/%u, "
		    "H264_SLICE sizeimage=%u\n",
		    width, height, ec->output_planes_count,
		    ec->output_bytesperlines[0], ec->output_bytesperlines[1],
		    ec->output_sizes[0], ec->output_sizes[1],
		    ec->capture_sizeimage);

	/* CAPTURE buffers first: mplane coded queue, single plane. */
	rc = v4l2_request_buffers(ec->video_fd, ec->capture_type,
				  ENCODER_CAPTURE_BUFFERS);
	if (rc < 0)
		goto error;

	for (i = 0; i < ENCODER_CAPTURE_BUFFERS; i++) {
		rc = encoder_buffer_setup(ec, &ec->capture[i],
					  ec->capture_type, i);
		if (rc < 0)
			goto error;
	}
	ec->capture_count = ENCODER_CAPTURE_BUFFERS;

	rc = v4l2_request_buffers(ec->video_fd, ec->output_type,
				  ENCODER_OUTPUT_BUFFERS);
	if (rc < 0)
		goto error;

	for (i = 0; i < ENCODER_OUTPUT_BUFFERS; i++) {
		rc = encoder_buffer_setup(ec, &ec->output[i], ec->output_type,
					  i);
		if (rc < 0)
			goto error;
	}
	ec->output_count = ENCODER_OUTPUT_BUFFERS;

	rc = v4l2_set_stream(ec->video_fd, ec->output_type, true);
	if (rc < 0)
		goto error;

	rc = v4l2_set_stream(ec->video_fd, ec->capture_type, true);
	if (rc < 0)
		goto error;

	ec->streaming = true;

	ENCDBG("CREATE fd=%d media=%d %ux%u out=%u cap=%u\n", ec->video_fd,
	       ec->media_fd, width, height, ec->output_count, ec->capture_count);

	return ec;

error:
	ENCDBG("CREATE failed rc=%d\n", rc);
	encoder_context_destroy(ec);
	return NULL;
}

/* Reset per-picture state when a VA context reuses an existing session. */
void encoder_context_reset(struct encoder_context *ec)
{
	ec->seq_valid = false;
	ec->pic_valid = false;
	ec->slice_valid = false;
	ec->coded_buf = VA_INVALID_ID;
	ec->misc_qp = -1;
	ec->last_ts = 0;
	/* The V4L2 queue is reused, so its timestamps must stay monotonic
	 * across contexts: do not reset ec->timestamp here. */
	ec->frame_count = 0;
	ec->has_pending = false;
	ec->pending_request_fd = -1;
	ec->rc_active = false;
	ec->rc_qp = 0;
	ec->rc_qp_min = 10;
	ec->rc_qp_max = 45;
	ec->rc_target_bps = 0;
	ec->rc_fps = 0;
}

void encoder_context_destroy(struct encoder_context *ec)
{
	unsigned int i;

	if (ec == NULL)
		return;

	if (ec->video_fd >= 0) {
		if (ec->has_pending && ec->pending_request_fd >= 0) {
			request_log("encoder: draining request %d before destroy\n",
				    ec->pending_request_fd);
			if (media_request_wait_completion_timeout(
				    ec->pending_request_fd, 3000) >= 0) {
				v4l2_dequeue_buffer_info(ec->video_fd,
							 ec->output_type,
							 ec->pending_output_index,
							 ec->output_planes_count,
							 NULL, NULL);
				v4l2_dequeue_buffer_info(ec->video_fd,
							 ec->capture_type,
							 ec->pending_capture_index,
							 1, NULL, NULL);
			}
			ec->has_pending = false;
		}

		if (ec->streaming) {
			int s1 = v4l2_set_stream(ec->video_fd, ec->output_type, false);
			int s2 = v4l2_set_stream(ec->video_fd, ec->capture_type, false);
			ENCDBG("DESTROY streamoff out=%d cap=%d\n", s1, s2);
		}

		for (i = 0; i < ec->output_count; i++)
			encoder_buffer_teardown(&ec->output[i]);
		for (i = 0; i < ec->capture_count; i++)
			encoder_buffer_teardown(&ec->capture[i]);

		{
			int r1 = v4l2_request_buffers(ec->video_fd, ec->output_type, 0);
			int r2 = v4l2_request_buffers(ec->video_fd, ec->capture_type, 0);
			ENCDBG("DESTROY reqbufs0 out=%d cap=%d\n", r1, r2);
		}
	}

	free(ec);
}

/* ------------------------------------------------------------------------
 * Surface NV12 storage (encoder-owned)
 * --------------------------------------------------------------------- */

int encoder_surface_ensure_storage(struct object_surface *surface_object)
{
	unsigned int width = surface_object->width;
	unsigned int height = surface_object->height;
	unsigned int pitch, size;

	if (surface_object->enc_data != NULL &&
	    surface_object->enc_pitch == width &&
	    surface_object->enc_width == width &&
	    surface_object->enc_height == height)
		return 0;

	if (surface_object->enc_data != NULL) {
		/* The DMA-BUF mirror is sized for the old geometry; drop it
		 * before the storage is replaced. */
		encoder_surface_free_export(surface_object);
		free(surface_object->enc_data);
		surface_object->enc_data = NULL;
	}

	if (width == 0 || height == 0)
		return -1;

	pitch = width;
	size = pitch * height * 3 / 2;

	surface_object->enc_data = malloc(size);
	if (surface_object->enc_data == NULL)
		return -1;

	memset(surface_object->enc_data, 0, size);
	surface_object->enc_pitch = pitch;
	surface_object->enc_width = width;
	surface_object->enc_height = height;

	return 0;
}

void encoder_surface_free_export(struct object_surface *surface_object)
{
	if (surface_object->export_map != NULL)
		munmap(surface_object->export_map, surface_object->export_size);
	if (surface_object->export_fd >= 0)
		close(surface_object->export_fd);
	surface_object->export_map = NULL;
	surface_object->export_fd = -1;
	surface_object->export_size = 0;
}

/* Copy GPU-written contents of the exported DMA-BUF back into enc_data. */
static void encoder_surface_pull_export(struct object_surface *surface_object)
{
	struct dma_buf_sync sync;

	if (surface_object->export_map == NULL || surface_object->enc_data == NULL)
		return;

	/* Never copy more than the current storage can hold: a resolution
	 * change can leave a stale (larger) mirror behind. */
	if (surface_object->export_size !=
	    surface_object->enc_pitch * surface_object->enc_height * 3 / 2) {
		encoder_surface_free_export(surface_object);
		return;
	}

	sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
	ioctl(surface_object->export_fd, DMA_BUF_IOCTL_SYNC, &sync);
	memcpy(surface_object->enc_data, surface_object->export_map,
	       surface_object->export_size);
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	ioctl(surface_object->export_fd, DMA_BUF_IOCTL_SYNC, &sync);
}

/*
 * Export the surface as a linear NV12 DMA-BUF.
 *
 * The buffer comes from the kernel's system heap so it can be mmap'd by us
 * and imported by a GPU driver (panfrost) at the same time.  Consumers such
 * as Sunshine render the captured frame into it through EGL; the contents
 * are copied back into enc_data before the next encode.
 */
int encoder_surface_export(struct object_surface *surface_object,
			   uint32_t flags, void *descriptor)
{
	VADRMPRIMESurfaceDescriptor *desc = descriptor;
	unsigned int size;
	int heap_fd;
	void *map;

	if (encoder_surface_ensure_storage(surface_object) < 0)
		return -1;

	size = surface_object->enc_pitch * surface_object->enc_height * 3 / 2;

	if (surface_object->export_map != NULL && surface_object->export_size != size) {
		encoder_surface_free_export(surface_object);
	}

	if (surface_object->export_map == NULL) {
		struct dma_heap_allocation_data req = {
			.len = size,
			.fd_flags = O_RDWR | O_CLOEXEC,
			.heap_flags = 0,
		};

		heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
		if (heap_fd < 0)
			return -1;
		if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &req) < 0) {
			close(heap_fd);
			return -1;
		}
		close(heap_fd);

		map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			   req.fd, 0);
		if (map == MAP_FAILED) {
			close(req.fd);
			return -1;
		}
		surface_object->export_fd = req.fd;
		surface_object->export_map = map;
		surface_object->export_size = size;
		memcpy(map, surface_object->enc_data, size);
	}

	/* Keep the dmabuf in sync with any uploads done through the
	 * vaPutImage/vaDeriveImage paths. */
	memcpy(surface_object->export_map, surface_object->enc_data,
	       surface_object->export_size);

	desc->fourcc = VA_FOURCC_NV12;
	desc->width = surface_object->enc_width;
	desc->height = surface_object->enc_height;
	desc->num_objects = 1;
	desc->objects[0].fd = dup(surface_object->export_fd);
	if (desc->objects[0].fd < 0)
		return -1;
	desc->objects[0].size = surface_object->export_size;
	desc->objects[0].drm_format_modifier = 0; /* DRM_FORMAT_MOD_LINEAR */

	if (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) {
		desc->num_layers = 2;
		/* Y plane. */
		desc->layers[0].drm_format = VA_FOURCC('R', '8', ' ', ' ');
		desc->layers[0].num_planes = 1;
		desc->layers[0].object_index[0] = 0;
		desc->layers[0].offset[0] = 0;
		desc->layers[0].pitch[0] = surface_object->enc_pitch;
		/* Interleaved UV plane. */
		desc->layers[1].drm_format = VA_FOURCC('G', 'R', '8', '8');
		desc->layers[1].num_planes = 1;
		desc->layers[1].object_index[0] = 0;
		desc->layers[1].offset[0] =
			surface_object->enc_pitch * surface_object->enc_height;
		desc->layers[1].pitch[0] = surface_object->enc_pitch;
	} else {
		desc->num_layers = 1;
		desc->layers[0].drm_format = VA_FOURCC_NV12;
		desc->layers[0].num_planes = 2;
		desc->layers[0].object_index[0] = 0;
		desc->layers[0].object_index[1] = 0;
		desc->layers[0].offset[0] = 0;
		desc->layers[0].offset[1] =
			surface_object->enc_pitch * surface_object->enc_height;
		desc->layers[0].pitch[0] = surface_object->enc_pitch;
		desc->layers[0].pitch[1] = surface_object->enc_pitch;
	}

	return 0;
}

void encoder_surface_free_storage(struct object_surface *surface_object)
{
	if (surface_object->enc_data != NULL)
		free(surface_object->enc_data);

	surface_object->enc_data = NULL;
	surface_object->enc_pitch = 0;
	surface_object->enc_width = 0;
	surface_object->enc_height = 0;
}

/*
 * Copy a rectangle between an NV12 VAImage and the surface's NV12 storage.
 *
 * Coordinates are in pixels; the source and destination rectangles must
 * have the same size (no scaling).  Chroma is handled at half resolution,
 * so chroma coordinates are rounded down to even values.  This is the path
 * used by compositors (e.g. KasmVNC) that upload only damaged rectangles.
 */
static int encoder_image_copy_rect(struct object_surface *surface_object,
				   VAImage *image, void *image_data,
				   bool to_image,
				   int src_x, int src_y,
				   int dst_x, int dst_y,
				   unsigned int width, unsigned int height)
{
	unsigned int surface_width = surface_object->enc_width;
	unsigned int surface_height = surface_object->enc_height;
	unsigned int surface_pitch = surface_object->enc_pitch;
	uint8_t *surface_data = surface_object->enc_data;
	unsigned int plane;

	if (image->format.fourcc != VA_FOURCC_NV12 || image->num_planes < 2)
		return -1;
	if (width == 0 || height == 0)
		return 0;

	for (plane = 0; plane < 2; plane++) {
		unsigned int plane_width, plane_height, sx, sy, dx, dy;
		unsigned int image_pitch = image->pitches[plane];
		unsigned int surface_plane_offset = plane == 0 ? 0 :
			surface_pitch * surface_height;
		uint8_t *image_plane = (uint8_t *)image_data +
				       image->offsets[plane];
		uint8_t *surface_plane = surface_data + surface_plane_offset;
		unsigned int y;

		if (plane == 0) {
			plane_width = width;
			plane_height = height;
			sx = (unsigned int)src_x;
			sy = (unsigned int)src_y;
			dx = (unsigned int)dst_x;
			dy = (unsigned int)dst_y;
		} else {
			/*
			 * NV12 chroma is interleaved U/V and half resolution,
			 * but one chroma row is still `width` bytes long (one
			 * U/V pair per two luma samples).  Only the vertical
			 * geometry is halved.
			 */
			plane_width = width & ~1U;
			plane_height = height / 2;
			sx = (unsigned int)src_x & ~1U;
			sy = (unsigned int)src_y / 2;
			dx = (unsigned int)dst_x & ~1U;
			dy = (unsigned int)dst_y / 2;
		}

		/* Clip to the surface. */
		if (dx >= surface_width || dy >= surface_height / (plane + 1))
			continue;
		if (dx + plane_width > surface_width)
			plane_width = surface_width - dx;
		if (dy + plane_height > surface_height / (plane + 1))
			plane_height = surface_height / (plane + 1) - dy;
		/* Clip to the source image. */
		if (sx + plane_width > image->width)
			plane_width = image->width - sx;
		if (sy + plane_height > image->height / (plane + 1))
			plane_height = image->height / (plane + 1) - sy;
		if (plane_width == 0 || plane_height == 0)
			continue;

		for (y = 0; y < plane_height; y++) {
			uint8_t *sp = surface_plane +
				      (size_t)(dy + y) * surface_pitch + dx;
			uint8_t *ip = image_plane +
				      (size_t)(sy + y) * image_pitch + sx;

			if (to_image)
				memcpy(ip, sp, plane_width);
			else
				memcpy(sp, ip, plane_width);
		}
	}

	return 0;
}

static int encoder_image_copy(struct object_surface *surface_object,
			      VAImage *image, void *image_data, bool to_image)
{
	return encoder_image_copy_rect(surface_object, image, image_data,
				       to_image, 0, 0, 0, 0,
				       surface_object->enc_width,
				       surface_object->enc_height);
}

int encoder_surface_put_image(struct object_surface *surface_object,
			      VAImage *image, void *image_data)
{
	return encoder_image_copy(surface_object, image, image_data, false);
}

int encoder_surface_get_image(struct object_surface *surface_object,
			      VAImage *image, void *image_data)
{
	return encoder_image_copy(surface_object, image, image_data, true);
}

int encoder_surface_put_image_rect(struct object_surface *surface_object,
				   VAImage *image, void *image_data,
				   int src_x, int src_y, int dst_x, int dst_y,
				   unsigned int width, unsigned int height)
{
	int ret = encoder_image_copy_rect(surface_object, image, image_data,
					  false, src_x, src_y, dst_x, dst_y,
					  width, height);

	ENCDBG("put  surface=%u rect=%d,%d %ux%u img=%ux%u pitch=%u ret=%d\n",
	       (unsigned)surface_object->base.id, dst_x, dst_y, width, height,
	       image->width, image->height, image->pitches[0], ret);

	return ret;
}

int encoder_surface_get_image_rect(struct object_surface *surface_object,
				   VAImage *image, void *image_data,
				   int src_x, int src_y, int dst_x, int dst_y,
				   unsigned int width, unsigned int height)
{
	return encoder_image_copy_rect(surface_object, image, image_data, true,
				       src_x, src_y, dst_x, dst_y,
				       width, height);
}

/* ------------------------------------------------------------------------
 * vaDeriveImage support: an image whose buffer aliases the surface storage
 * --------------------------------------------------------------------- */

VAStatus encoder_derive_image(VADriverContextP context,
			      struct object_surface *surface_object,
			      VAImage *image)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_image *image_object;
	VABufferID buffer_id;
	VAImageID id;
	VAStatus status;
	unsigned int size;

	if (surface_object->enc_data == NULL)
		request_log("encoder: derive surface %u (%ux%u)\n",
			    (unsigned int)surface_object->base.id,
			    surface_object->width, surface_object->height);

	if (encoder_surface_ensure_storage(surface_object) < 0)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	size = surface_object->enc_pitch * surface_object->enc_height * 3 / 2;

	id = object_heap_allocate(&driver_data->image_heap);
	image_object = IMAGE(driver_data, id);
	if (image_object == NULL)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	/*
	 * The image buffer borrows the surface's NV12 storage: vaMapBuffer on
	 * this image hands back the surface pixels and writes to it land
	 * directly in the surface, which is exactly what ffmpeg's
	 * vaDeriveImage-based hwupload path expects.
	 */
	status = RequestCreateBufferForeign(context, VAImageBufferType,
					    surface_object->enc_data, size,
					    &buffer_id);
	if (status != VA_STATUS_SUCCESS) {
		object_heap_free(&driver_data->image_heap,
				 (struct object_base *)image_object);
		return status;
	}

	memset(image, 0, sizeof(*image));
	image->format.fourcc = VA_FOURCC_NV12;
	image->format.byte_order = VA_LSB_FIRST;
	image->format.bits_per_pixel = 12;
	image->width = surface_object->enc_width;
	image->height = surface_object->enc_height;
	image->buf = buffer_id;
	image->image_id = id;
	image->num_planes = 2;
	image->data_size = size;
	image->pitches[0] = surface_object->enc_pitch;
	image->pitches[1] = surface_object->enc_pitch;
	image->offsets[0] = 0;
	image->offsets[1] = surface_object->enc_pitch * surface_object->enc_height;

	image_object->image = *image;

	BUFFER(driver_data, buffer_id)->derived_surface_id =
		(VASurfaceID)surface_object->base.id;

	return VA_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------
 * Picture submission
 * --------------------------------------------------------------------- */

void encoder_begin_picture(struct request_data *driver_data,
			   struct object_context *context_object,
			   struct object_surface *surface_object)
{
	struct encoder_context *ec = context_object->encoder;

	(void)driver_data;

	ec->seq_valid = false;
	ec->pic_valid = false;
	ec->slice_valid = false;
	ec->coded_buf = VA_INVALID_ID;
	ec->misc_qp = -1;

	encoder_surface_ensure_storage(surface_object);

	surface_object->status = VASurfaceRendering;
	context_object->render_surface_id = (VASurfaceID)surface_object->base.id;
}

static VAStatus encoder_store_buffer(struct encoder_context *ec,
				     struct object_buffer *buffer_object)
{
	switch (buffer_object->type) {
	case VAEncSequenceParameterBufferType:
		if (buffer_object->size < sizeof(VAEncSequenceParameterBufferH264))
			return VA_STATUS_ERROR_INVALID_PARAMETER;
		memcpy(&ec->seq, buffer_object->data, sizeof(ec->seq));
		ec->seq_valid = true;

		/*
		 * FFmpeg carries the rate-control target in the sequence
		 * buffer (bits_per_second), not only in the misc buffer.
		 */
		if (ec->seq.bits_per_second > 0) {
			/*
			 * FFmpeg's CBR setup puts 2x the average bitrate in
			 * bits_per_second (the peak/HRD rate), so target the
			 * average here.
			 */
			ec->rc_active = true;
			ec->rc_target_bps = ec->seq.bits_per_second / 2;
		}
		if (ec->seq.num_units_in_tick > 0 && ec->seq.time_scale > 0) {
			unsigned int fps = (ec->seq.time_scale +
					    ec->seq.num_units_in_tick) /
					   (2 * ec->seq.num_units_in_tick);
			if (fps > 0 && fps <= 240)
				ec->rc_fps = fps;
		}
		break;

	case VAEncPictureParameterBufferType:
		if (buffer_object->size < sizeof(VAEncPictureParameterBufferH264))
			return VA_STATUS_ERROR_INVALID_PARAMETER;
		memcpy(&ec->pic, buffer_object->data, sizeof(ec->pic));
		ec->pic_valid = true;
		/*
		 * ffmpeg >= 5.1 hands the coded buffer through
		 * VAEncPictureParameterBufferH264.coded_buf instead of
		 * passing it to vaRenderPicture().
		 */
		if (ec->pic.coded_buf != VA_INVALID_ID)
			ec->coded_buf = ec->pic.coded_buf;
		break;

	case VAEncSliceParameterBufferType:
		if (buffer_object->size < sizeof(VAEncSliceParameterBufferH264))
			return VA_STATUS_ERROR_INVALID_PARAMETER;
		/* Single slice per picture: the first slice buffer wins. */
		if (!ec->slice_valid) {
			memcpy(&ec->slice, buffer_object->data,
			       sizeof(ec->slice));
			ec->slice_valid = true;
		}
		break;

	case VAEncMiscParameterBufferType: {
		VAEncMiscParameterBuffer *misc = buffer_object->data;

		if (buffer_object->size < sizeof(VAEncMiscParameterBuffer))
			break;

		/*
		 * ffmpeg builds one misc parameter per buffer:
		 * VAEncMiscParameterBuffer header followed by the payload.
		 * Only RateControl carries anything we can act on (a
		 * non-CQP consumer's initial QP); FrameRate / QualityLevel /
		 * HRD are accepted and ignored.
		 */
		if (buffer_object->size >= sizeof(VAEncMiscParameterBuffer) +
					   sizeof(VAEncMiscParameterRateControl) &&
		    misc->type == VAEncMiscParameterTypeRateControl) {
			VAEncMiscParameterRateControl *rc =
				(VAEncMiscParameterRateControl *)misc->data;

			if (rc->initial_qp > 0 && rc->initial_qp <= 51)
				ec->misc_qp = rc->initial_qp;
			if (rc->bits_per_second > 0) {
				ec->rc_active = true;
				ec->rc_target_bps = rc->bits_per_second / 2;
				ec->rc_qp_min = rc->min_qp > 0 ? rc->min_qp : 10;
				ec->rc_qp_max = rc->max_qp > 0 ? rc->max_qp : 45;
				if (ec->rc_qp_max < ec->rc_qp_min)
					ec->rc_qp_max = ec->rc_qp_min;
			}
		} else if (misc->type == VAEncMiscParameterTypeFrameRate &&
			   buffer_object->size >= sizeof(VAEncMiscParameterBuffer) +
						  sizeof(VAEncMiscParameterFrameRate)) {
			VAEncMiscParameterFrameRate *fr =
				(VAEncMiscParameterFrameRate *)misc->data;
			uint32_t v = fr->framerate;

			/* 16.16 fixed point in libva >= 1.x, plain integer in
			 * older callers. */
			if (v > 1000)
				v >>= 16;
			if (v >= 5 && v <= 240)
				ec->rc_fps = v;
		}
		break;
	}

	case VAEncCodedBufferType:
		ec->coded_buf = (VABufferID)(buffer_object->base.id);
		break;

	default:
		/* Packed headers and macroblock params are not used: the
		 * former are not advertised (PackedHeaders = 0) and the
		 * latter only appear when per-MB config is requested. */
		break;
	}

	return VA_STATUS_SUCCESS;
}

VAStatus encoder_render_picture(struct request_data *driver_data,
				struct object_context *context_object,
				VABufferID *buffer_ids, int buffers_count)
{
	struct encoder_context *ec = context_object->encoder;
	int i;

	(void)driver_data;

	for (i = 0; i < buffers_count; i++) {
		struct object_buffer *buffer_object =
			BUFFER(driver_data, buffer_ids[i]);
		VAStatus status;

		if (buffer_object == NULL) {
			ENCDBG("render: null buffer id=%u\n",
			       (unsigned)buffer_ids[i]);
			return VA_STATUS_ERROR_INVALID_BUFFER;
		}

		status = encoder_store_buffer(ec, buffer_object);
		if (status != VA_STATUS_SUCCESS)
			ENCDBG("render: store failed type=%u size=%u status=%d\n",
			       (unsigned)buffer_object->type,
			       buffer_object->size, (int)status);
		if (status != VA_STATUS_SUCCESS)
			return status;
	}

	return VA_STATUS_SUCCESS;
}

static unsigned int encoder_v4l2_slice_type(unsigned int va_slice_type)
{
	/* VA/H.264 slice_type: 5 bits, value % 5 gives the base type where
	 * 0 = P, 1 = B, 2 = I. */
	switch (va_slice_type % 5) {
	case 0:
		return V4L2_H264_SLICE_TYPE_P;
	case 1:
		return V4L2_H264_SLICE_TYPE_B;
	case 2:
	default:
		return V4L2_H264_SLICE_TYPE_I;
	}
}

static void encoder_fill_controls(struct encoder_context *ec)
{
	struct v4l2_ctrl_h264_encode_params *params = &ec->params;
	struct v4l2_ctrl_h264_encode_rc *rc = &ec->rc;
	const VAEncPictureParameterBufferH264 *pic = &ec->pic;
	const VAEncSliceParameterBufferH264 *slice = &ec->slice;
	int qp;

	memset(params, 0, sizeof(*params));
	memset(rc, 0, sizeof(*rc));

	params->slice_type = encoder_v4l2_slice_type(slice->slice_type);
	params->pic_parameter_set_id = 0;
	params->frame_num = pic->frame_num;
	params->idr_pic_id = slice->idr_pic_id;
	params->cabac_init_idc = slice->cabac_init_idc;
	params->disable_deblocking_filter_idc =
		slice->disable_deblocking_filter_idc;
	params->slice_alpha_c0_offset_div2 = slice->slice_alpha_c0_offset_div2;
	params->slice_beta_offset_div2 = slice->slice_beta_offset_div2;
	/* One slice per frame, so the whole picture. */
	params->slice_size_mb_rows = (int32_t)ec->mb_height;
	params->pic_init_qp_minus26 = (int8_t)pic->pic_init_qp - 26;
	params->chroma_qp_index_offset = (int8_t)pic->chroma_qp_index_offset;

	if (pic->pic_fields.bits.entropy_coding_mode_flag)
		params->flags |= V4L2_H264_ENCODE_FLAG_ENTROPY_CODING_MODE;
	if (encoder_profile_has_high_syntax(ec->profile) &&
	    pic->pic_fields.bits.transform_8x8_mode_flag)
		params->flags |= V4L2_H264_ENCODE_FLAG_TRANSFORM_8X8_MODE;
	if (pic->pic_fields.bits.constrained_intra_pred_flag)
		params->flags |= V4L2_H264_ENCODE_FLAG_CONSTRAINED_INTRA_PRED;

	if (params->slice_type == V4L2_H264_SLICE_TYPE_P)
		params->reference_ts = ec->last_ts;

	/*
	 * QP: the effective frame QP is the PPS init QP plus the slice delta.
	 * ffmpeg puts the CQP value in pic_init_qp and leaves the delta at
	 * zero; a misc RateControl buffer (non-CQP users) may override it.
	 */
	qp = (int)pic->pic_init_qp + (int)slice->slice_qp_delta;
	if (ec->rc_active) {
		/* CBR/VBR: frame-level QP controller.  Seed from the
		 * consumer's initial QP (or 26) and adapt after each frame. */
		if (ec->rc_qp <= 0)
			ec->rc_qp = ec->misc_qp > 0 ? ec->misc_qp : 26;
		qp = ec->rc_qp;
		rc->qp_min = (uint32_t)ec->rc_qp_min;
		rc->qp_max = (uint32_t)ec->rc_qp_max;
	} else {
		if (ec->misc_qp >= 0)
			qp = ec->misc_qp;
		rc->qp_min = 0;
		rc->qp_max = 51;
	}
	if (qp < 0)
		qp = 0;
	if (qp > 51)
		qp = 51;

	rc->qp = (uint32_t)qp;
	rc->mad_qp_delta = 0;
	rc->mad_threshold = 0;
}

/* Copy the input surface's NV12 storage into the V4L2 OUTPUT planes. */
static int encoder_copy_surface(struct encoder_context *ec,
				struct object_surface *surface_object,
				struct encoder_buffer *output)
{
	unsigned int plane;
	unsigned int width = surface_object->enc_width;
	unsigned int height = surface_object->enc_height;
	uint8_t *source = surface_object->enc_data;

	if (source == NULL)
		return -1;

	if (width == 0 || height == 0)
		return -1;

	if (width > ec->output_width)
		width = ec->output_width;

	for (plane = 0; plane < ec->output_planes_count; plane++) {
		unsigned int rows = plane == 0 ? height : height / 2;
		unsigned int bytes = width;
		unsigned int src_offset = plane == 0 ? 0 :
			surface_object->enc_pitch * surface_object->enc_height;
		unsigned int dst_pitch = ec->output_bytesperlines[plane] ?
			ec->output_bytesperlines[plane] : width;
		uint8_t *dst = output->map[plane];
		uint8_t *src = source + src_offset;
		unsigned int y;

		if (dst == NULL)
			return -1;

		if (plane > 0 && rows > ec->output_height / 2)
			rows = ec->output_height / 2;
		if (plane == 0 && rows > ec->output_height)
			rows = ec->output_height;

		for (y = 0; y < rows; y++)
			memcpy(dst + (size_t)y * dst_pitch,
			       src + (size_t)y * surface_object->enc_pitch,
			       bytes);

	}

	return 0;
}

VAStatus encoder_end_picture(struct request_data *driver_data,
			     struct object_context *context_object)
{
	struct encoder_context *ec = context_object->encoder;
	struct object_surface *surface_object;
	struct encoder_buffer *output;
	struct encoder_buffer *capture;
	struct v4l2_ext_control controls[2];
	struct object_buffer *coded_object;
	VACodedBufferSegment *segment;
	uint64_t capture_ts = 0;
	uint8_t sps[512], pps[512];
	unsigned int sps_size = 0, pps_size = 0;
	unsigned int output_bytesused[ENCODER_MAX_PLANES];
	unsigned int capture_bytesused[1];
	unsigned int payload_size, header_size, total;
	uint64_t timestamp;
	uint8_t *payload;
	int rc;

	(void)driver_data;

	if (ec == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	surface_object = SURFACE(driver_data, context_object->render_surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	encoder_surface_pull_export(surface_object);

	ENCDBG("BEGIN ctx=%p frame=%llu surface=%u enc=%dx%d data=%p pitch=%u idr=%d slice=%u\n",
	       (void *)ec, ec->frame_count, (unsigned)surface_object->base.id,
	       surface_object->enc_width, surface_object->enc_height,
	       surface_object->enc_data, surface_object->enc_pitch,
	       (int)ec->pic.pic_fields.bits.idr_pic_flag,
	       (unsigned)ec->slice.slice_type);

	/*
	 * ffmpeg only submits VAEncSequenceParameterBufferType on IDR
	 * pictures, so a P picture legitimately has no sequence buffer.
	 */
	if (!ec->pic_valid || !ec->slice_valid ||
	    (ec->pic.pic_fields.bits.idr_pic_flag && !ec->seq_valid)) {
		ENCDBG("FAIL incomplete seq=%d pic=%d slice=%d idr=%d\n", ec->seq_valid, ec->pic_valid, ec->slice_valid, (int)ec->pic.pic_fields.bits.idr_pic_flag);
		request_log("encoder: incomplete picture (seq=%d pic=%d slice=%d idr=%d)\n",
			    ec->seq_valid, ec->pic_valid, ec->slice_valid,
			    ec->pic.pic_fields.bits.idr_pic_flag);
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	}

	if (ec->coded_buf == VA_INVALID_ID) {
		ENCDBG("FAIL no coded_buf\n");
		request_log("encoder: no VAEncCodedBufferType buffer for picture\n");
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	}

	coded_object = BUFFER(driver_data, ec->coded_buf);
	if (coded_object == NULL ||
	    coded_object->type != VAEncCodedBufferType)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	if (surface_object->enc_data == NULL) {
		ENCDBG("FAIL no enc_data surface=%u\n", (unsigned)surface_object->base.id);
		request_log("encoder: input surface %u has no uploaded pixels\n",
			    (unsigned int)surface_object->base.id);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	/*
	 * A P/B picture with no preceding reference (the first picture of a
	 * freshly created context) cannot be encoded: the hardware would
	 * wait forever for a reference buffer.  Promote it to an IDR so the
	 * stream stays self-contained and the application does not hang.
	 */
	if (!ec->pic.pic_fields.bits.idr_pic_flag && ec->last_ts == 0 &&
	    (ec->slice.slice_type % 5) != 2) {
		ENCDBG("promote first non-I picture to IDR (slice=%u)\n",
		       (unsigned)ec->slice.slice_type);
		ec->pic.pic_fields.bits.idr_pic_flag = 1;
		ec->slice.slice_type = 7;
	}

	/*
	 * SPS + PPS only on IDR pictures: the hardware writes nothing but the
	 * slice NAL, so these are the only in-band parameter sets a decoder
	 * ever sees.
	 */
	if (ec->pic.pic_fields.bits.idr_pic_flag) {
		sps_size = encoder_build_sps(ec, sps, sizeof(sps));
		pps_size = encoder_build_pps(ec, pps, sizeof(pps));
		if (sps_size == 0 || pps_size == 0) {
			ENCDBG("FAIL sps/pps serialisation\n");
		request_log("encoder: SPS/PPS serialisation failed\n");
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}
	}

	encoder_fill_controls(ec);

	output = &ec->output[ec->output_index];
	capture = &ec->capture[ec->capture_index];

	ec->output_index = (ec->output_index + 1) % ec->output_count;
	ec->capture_index = (ec->capture_index + 1) % ec->capture_count;

	rc = encoder_copy_surface(ec, surface_object, output);
	if (rc < 0) {
		ENCDBG("FAIL copy surface\n");
		request_log("encoder: failed to copy input surface\n");
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	/*
	 * V4L2 timestamps are nanoseconds and must be unique: the CAPTURE
	 * buffer inherits this value (TIMESTAMP_COPY) and a later P picture
	 * resolves its reference with vb2_find_buffer(reference_ts).  Spacing
	 * by 1 ms keeps values far apart even at very long session lengths.
	 */
	ec->timestamp += 1000000;
	timestamp = ec->timestamp;

	for (unsigned int i = 0; i < ec->output_planes_count; i++) {
		unsigned int pitch = ec->output_bytesperlines[i] ?
				     ec->output_bytesperlines[i] :
				     ec->output_width;

		output_bytesused[i] = pitch * ec->output_height;
		if (i > 0)
			output_bytesused[i] /= 2;
	}

	/* 1. OUTPUT with the request attached. */
	rc = v4l2_queue_buffer_timestamped(ec->video_fd, output->request_fd,
					   ec->output_type, output->index,
					   ec->output_planes_count,
					   output_bytesused, timestamp);
	if (rc < 0) {
		ENCDBG("FAIL qbuf out rc=%d\n", rc);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	/* 2. CAPTURE, no request fd (request ownership is on OUTPUT). */
	capture_bytesused[0] = 0;
	rc = v4l2_queue_buffer_timestamped(ec->video_fd, -1, ec->capture_type,
					   capture->index, 1,
					   capture_bytesused, 0);
	if (rc < 0) {
		ENCDBG("FAIL qbuf cap rc=%d\n", rc);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	/* 3. Controls on the request. */
	memset(controls, 0, sizeof(controls));
	controls[0].id = V4L2_CID_STATELESS_H264_ENCODE_PARAMS;
	controls[0].ptr = &ec->params;
	controls[0].size = sizeof(ec->params);
	controls[1].id = V4L2_CID_STATELESS_H264_ENCODE_RC;
	controls[1].ptr = &ec->rc;
	controls[1].size = sizeof(ec->rc);

	rc = v4l2_set_controls(ec->video_fd, output->request_fd, controls, 2);
	if (rc < 0) {
		ENCDBG("FAIL set controls rc=%d\n", rc);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	/* 4. Queue and wait. */
	rc = media_request_queue(output->request_fd);
	if (rc < 0) {
		ENCDBG("FAIL media_request_queue rc=%d\n", rc);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	ec->has_pending = true;
	ec->pending_request_fd = output->request_fd;
	ec->pending_output_index = output->index;
	ec->pending_capture_index = capture->index;

	request_log("encoder: submitted out=%u req=%d cap=%u ts=%llu type=%u\n",
		    output->index, output->request_fd, capture->index,
		    (unsigned long long)timestamp, ec->params.slice_type);

	/* 2 s matches the hantro watchdog, so a stuck hardware job is
	 * reported as a timeout rather than a short-poll false negative. */
	rc = media_request_wait_completion_timeout(output->request_fd, 2500);
	if (rc < 0) {
		unsigned int diag_used[2] = { 0, 0 };
		int rc2;

		ENCDBG("FAIL wait timeout ts=%llu\n", (unsigned long long)timestamp);
		request_log("encoder: request completion failed (ts=%llu)\n",
			    (unsigned long long)timestamp);

		/* Give the driver watchdog (2 s) a chance to finish and
		 * reap the job ourselves from the dequeue side. */
		rc = media_request_wait_completion_timeout(output->request_fd,
							   3000);
		if (rc >= 0) {
			if (v4l2_dequeue_buffer_info(ec->video_fd,
						     ec->output_type,
						     output->index,
						     ec->output_planes_count,
						     NULL, NULL) >= 0)
				v4l2_dequeue_buffer_info(ec->video_fd,
							 ec->capture_type,
							 capture->index, 1,
							 capture_bytesused,
							 &capture_ts);
			media_request_reinit(output->request_fd);
			ec->has_pending = false;
			if (capture_bytesused[0] > 0)
				goto got_payload;
		}

		rc2 = v4l2_dequeue_buffer_info(ec->video_fd, ec->output_type,
					       output->index,
					       ec->output_planes_count,
					       diag_used, NULL);
		request_log("encoder: diag OUT dq rc=%d used=%u/%u\n", rc2,
			    diag_used[0], diag_used[1]);
		rc2 = v4l2_dequeue_buffer_info(ec->video_fd, ec->capture_type,
					       capture->index, 1, diag_used,
					       NULL);
		request_log("encoder: diag CAP dq rc=%d used=%u\n", rc2,
			    diag_used[0]);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	/* 5. Reclaim both buffers. */
	rc = v4l2_dequeue_buffer_info(ec->video_fd, ec->output_type,
				      output->index, ec->output_planes_count,
				      NULL, NULL);
	if (rc < 0) {
		ENCDBG("FAIL dq out rc=%d\n", rc);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	rc = v4l2_dequeue_buffer_info(ec->video_fd, ec->capture_type,
				      capture->index, 1, capture_bytesused,
				      &capture_ts);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	rc = media_request_reinit(output->request_fd);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	ec->last_ts = timestamp;

	ec->has_pending = false;

got_payload:
	/* 6. Assemble the access unit in the VA coded buffer. */
	payload_size = capture_bytesused[0];

	ENCDBG("END   frame=%llu surface=%u idr=%d slice=%u qp=%u payload=%u\n",
	       ec->frame_count, (unsigned)surface_object->base.id,
	       ec->pic.pic_fields.bits.idr_pic_flag,
	       (unsigned)ec->slice.slice_type, ec->rc.qp, payload_size);

	if (enc_debug_file() && ec->frame_count < 3) {
		char path[64];
		FILE *au;
		unsigned int yy;

		snprintf(path, sizeof(path), "/tmp/enc-au-%llu.h264",
			 ec->frame_count);
		au = fopen(path, "wb");
		if (au) {
			fwrite(sps, 1, sps_size, au);
			fwrite(pps, 1, pps_size, au);
			for (yy = 0; yy < payload_size; yy++)
				fwrite(capture->map[0] + yy, 1, 1, au);
			fclose(au);
		}
	}

	/* CBR/VBR: adapt the QP towards the per-frame bit budget. */
	if (ec->rc_active && payload_size > 0) {
		unsigned int fps = ec->rc_fps ? ec->rc_fps : 30;
		double budget = (double)ec->rc_target_bps / 8.0 / fps;
		double err = (double)payload_size - budget;
		int step = 0;

		if (budget > 0) {
			if (err > 0.15 * budget)
				step = 2;
			else if (err > 0.05 * budget)
				step = 1;
			else if (err < -0.15 * budget)
				step = -2;
			else if (err < -0.05 * budget)
				step = -1;
		}
		ec->rc_qp += step;
		if (ec->rc_qp < ec->rc_qp_min)
			ec->rc_qp = ec->rc_qp_min;
		if (ec->rc_qp > ec->rc_qp_max)
			ec->rc_qp = ec->rc_qp_max;
		if (ec->rc_qp < 1)
			ec->rc_qp = 1;
		if (ec->rc_qp > 51)
			ec->rc_qp = 51;
	}
	if (payload_size == 0) {
		request_log("encoder: hardware produced an empty slice NAL\n");
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	header_size = sizeof(VACodedBufferSegment);
	total = header_size + sps_size + pps_size + payload_size;

	if (coded_object->size < total) {
		ENCDBG("FAIL coded buffer small %u < %u\n", coded_object->size, total);
		request_log("encoder: coded buffer too small (%u < %u: "
			    "sps %u pps %u slice %u)\n",
			    coded_object->size, total, sps_size, pps_size,
			    payload_size);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	payload = (uint8_t *)coded_object->data + header_size;
	memcpy(payload, sps, sps_size);
	memcpy(payload + sps_size, pps, pps_size);
	memcpy(payload + sps_size + pps_size, capture->map[0], payload_size);

	segment = coded_object->data;
	memset(segment, 0, sizeof(*segment));
	segment->size = sps_size + pps_size + payload_size;
	segment->bit_offset = 0;
	segment->status = 0;
	segment->buf = payload;
	segment->next = NULL;

	ec->frame_count++;
	surface_object->status = VASurfaceDisplaying;
	context_object->render_surface_id = VA_INVALID_ID;

	request_log("encoder: frame %llu %s slice=%u bytes sps=%u pps=%u qp=%u "
		    "frame_num=%u ts=%llu cap_ts=%llu ref_ts=%llu\n",
		    (unsigned long long)ec->frame_count,
		    ec->params.slice_type == V4L2_H264_SLICE_TYPE_I ? "IDR" : "P",
		    payload_size, sps_size, pps_size, ec->rc.qp,
		    ec->params.frame_num,
		    (unsigned long long)timestamp,
		    (unsigned long long)capture_ts,
		    (unsigned long long)ec->params.reference_ts);

	return VA_STATUS_SUCCESS;
}
