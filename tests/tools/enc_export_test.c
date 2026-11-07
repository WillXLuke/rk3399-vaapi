/*
 * Standalone VA-API H.264 encode test for the v4l2-request driver.
 *
 * Unlike ffmpeg's hwupload (which ends up using vaDeriveImage on this stack),
 * this exercises the vaCreateImage() + vaPutImage() upload path end to end:
 * every input frame is written into a VAImage and pushed into the VA surface
 * with vaPutImage() before vaBeginPicture().
 *
 * It also doubles as a conformance probe for the encoder frontend: the
 * bitstream it produces must decode cleanly and match the source pattern.
 *
 * Build:
 *   gcc -O2 -o enc_putimage_test enc_putimage_test.c -lva -lva-drm
 * Run:
 *   ./enc_putimage_test /tmp/put.h264 /tmp/put_src.yuv 640 480 10
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <sys/mman.h>
#include <va/va_enc_h264.h>

#define CHECK_VA(_expr)                                                       \
	do {                                                                  \
		VAStatus _s = (_expr);                                        \
		if (_s != VA_STATUS_SUCCESS) {                                \
			fprintf(stderr, "%s:%d: %s failed: %d (%s)\n",        \
				__FILE__, __LINE__, #_expr, _s,               \
				vaErrorStr(_s));                              \
			exit(1);                                              \
		}                                                             \
	} while (0)

static void fill_nv12(uint8_t *y_plane, uint8_t *uv_plane, unsigned int width,
		      unsigned int height, unsigned int pitch, int frame)
{
	unsigned int x, y;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			unsigned int value;

			/* Moving gradient plus a frame-dependent block so every
			 * picture differs and P pictures actually predict. */
			value = (x * 255 / width + y * 127 / height +
				 frame * 16) & 0xff;
			if (x > (unsigned int)((frame * 37) % width) &&
			    x < (unsigned int)((frame * 37) % width) + 80 &&
			    y > height / 3 && y < height / 3 + 80)
				value = 235;
			y_plane[y * pitch + x] = (uint8_t)value;
		}
	}

	for (y = 0; y < height / 2; y++) {
		for (x = 0; x < width; x += 2) {
			unsigned int u = 64 + ((frame * 8 + (int)x) & 0x7f);
			unsigned int v = 192 - ((frame * 8 + (int)y) & 0x7f);

			uv_plane[y * pitch + x] = (uint8_t)u;
			uv_plane[y * pitch + x + 1] = (uint8_t)v;
		}
	}
}

static void fill_rect(uint8_t *y, uint8_t *uv, unsigned int pitch,
		      unsigned int x, unsigned int y0, unsigned int w,
		      unsigned int h, int frame)
{
	unsigned int i, j;

	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			y[(size_t)(y0 + j) * pitch + x + i] =
				(uint8_t)(60 + ((x + i + frame * 5) & 0x7f));
	for (j = 0; j < h / 2; j++)
		for (i = 0; i < w; i += 2) {
			uv[(size_t)(y0 / 2 + j) * pitch + x + i] =
				(uint8_t)(100 + (frame & 0x3f));
			uv[(size_t)(y0 / 2 + j) * pitch + x + i + 1] =
				(uint8_t)(160 - (frame & 0x3f));
		}
}

int main(int argc, char **argv)
{
	VADisplay display;
	VAProfile profiles[64];
	VAEntrypoint entrypoints[16];
	VAConfigAttrib attrib;
	VAConfigID config_id;
	VAContextID context_id;
	VASurfaceID surfaces[3];
	VAImage image;
	const char *out_path, *src_path;
	unsigned int width, height;
	int frames, i, j;
	int drm_fd, num_profiles, num_entrypoints;
	FILE *out, *src;
	VAStatus vas;
	int profile_index = -1;

	if (argc < 6) {
		fprintf(stderr,
			"usage: %s <out.h264> <src.yuv> <width> <height> <frames>\n",
			argv[0]);
		return 1;
	}

	out_path = argv[1];
	src_path = argv[2];
	width = (unsigned int)atoi(argv[3]);
	height = (unsigned int)atoi(argv[4]);
	frames = atoi(argv[5]);

	drm_fd = open("/dev/dri/renderD128", O_RDWR);
	if (drm_fd < 0) {
		perror("open renderD128");
		return 1;
	}
	display = vaGetDisplayDRM(drm_fd);
	if (display == NULL) {
		fprintf(stderr, "vaGetDisplayDRM failed\n");
		return 1;
	}
	CHECK_VA(vaInitialize(display, &i, &j));

	num_profiles = (int)(sizeof(profiles) / sizeof(profiles[0]));
	CHECK_VA(vaQueryConfigProfiles(display, profiles, &num_profiles));

	for (i = 0; i < num_profiles; i++) {
		num_entrypoints =
			(int)(sizeof(entrypoints) / sizeof(entrypoints[0]));
		CHECK_VA(vaQueryConfigEntrypoints(display, profiles[i],
						  entrypoints,
						  &num_entrypoints));
		for (j = 0; j < num_entrypoints; j++) {
			if (entrypoints[j] != VAEntrypointEncSlice)
				continue;
			if (profiles[i] != VAProfileH264High &&
			    profiles[i] != VAProfileH264Main &&
			    profiles[i] != VAProfileH264ConstrainedBaseline)
				continue;
			if (profiles[i] == VAProfileH264High)
				profile_index = i;
			else if (profile_index < 0)
				profile_index = i;
		}
	}

	if (profile_index < 0) {
		fprintf(stderr, "no H.264 EncSlice profile found\n");
		return 1;
	}

	printf("using profile 0x%x (%s)\n", profiles[profile_index],
	       profiles[profile_index] == VAProfileH264High ? "High" : "other");

	attrib.type = VAConfigAttribRateControl;
	attrib.value = 0;
	CHECK_VA(vaGetConfigAttributes(display, profiles[profile_index],
				       VAEntrypointEncSlice, &attrib, 1));
	printf("rate control modes: 0x%x\n", attrib.value);
	attrib.value = VA_RC_CQP;

	CHECK_VA(vaCreateConfig(display, profiles[profile_index],
				VAEntrypointEncSlice, &attrib, 1, &config_id));

	CHECK_VA(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width, height,
				  surfaces, 3, NULL, 0));

	CHECK_VA(vaCreateContext(display, config_id, (int)width, (int)height,
				 VA_PROGRESSIVE, surfaces, 3, &context_id));

	/* Export the encoder surface as a DMA-BUF, the way Sunshine does. */
	VADRMPRIMESurfaceDescriptor prime;
	memset(&prime, 0, sizeof(prime));
	CHECK_VA(vaExportSurfaceHandle(display, surfaces[0],
				       VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
				       VA_EXPORT_SURFACE_WRITE_ONLY |
				       VA_EXPORT_SURFACE_SEPARATE_LAYERS,
				       &prime));
	printf("export: fourcc=%08x %ux%u objects=%u layers=%u\n",
	       prime.fourcc, prime.width, prime.height, prime.num_objects,
	       prime.num_layers);
	for (unsigned int l = 0; l < prime.num_layers; l++)
		printf("  layer %u: fmt=%08x planes=%u off=%u pitch=%u\n", l,
		       prime.layers[l].drm_format, prime.layers[l].num_planes,
		       prime.layers[l].offset[0], prime.layers[l].pitch[0]);
	if (prime.num_objects != 1 || prime.num_layers != 2)
		return 1;
	unsigned int epitch = prime.layers[0].pitch[0];
	uint8_t *emap = mmap(NULL, prime.objects[0].size,
			     PROT_READ | PROT_WRITE, MAP_SHARED,
			     prime.objects[0].fd, 0);
	if (emap == MAP_FAILED) { perror("mmap export"); return 1; }

	/* NV12 upload image, driven through vaPutImage. */
	memset(&image, 0, sizeof(image));
	{
		VAImageFormat format;

		memset(&format, 0, sizeof(format));
		format.fourcc = VA_FOURCC_NV12;
		format.byte_order = VA_LSB_FIRST;
		format.bits_per_pixel = 12;
		CHECK_VA(vaCreateImage(display, &format, (int)width, (int)height,
				       &image));
		printf("VAImage %ux%u planes=%u pitches=%u/%u offsets=%u/%u\n",
		       image.width, image.height, image.num_planes,
		       image.pitches[0], image.pitches[1], image.offsets[0],
		       image.offsets[1]);
	}

	out = fopen(out_path, "wb");
	src = fopen(src_path, "wb");
	if (out == NULL || src == NULL) {
		perror("fopen");
		return 1;
	}

	for (i = 0; i < frames; i++) {
		VAEncSequenceParameterBufferH264 seq;
		VAEncPictureParameterBufferH264 pic;
		VAEncSliceParameterBufferH264 slice;
		VAEncMiscParameterBuffer misc;
		VABufferID seq_buf, pic_buf, slice_buf, misc_buf, coded_buf;
		VABufferID buffers[4];
		VACodedBufferSegment *segment;
		void *image_data = NULL;
		void *coded_data = NULL;
		unsigned int mb_width = (width + 15) / 16;
		unsigned int mb_height = (height + 15) / 16;
		int is_idr = (i % 10) == 0;

		/* "GPU render": write the frame straight into the exported
		 * DMA-BUF, exactly like Sunshine's EGL pass. */
		if (i == 0)
			fill_nv12(emap, emap + (size_t)epitch * height, width,
				  height, epitch, 0);
		else {
			unsigned int rx = (width / 4 + (unsigned int)i * 8) %
					  (width - 64);
			fill_rect(emap, emap + (size_t)epitch * height, epitch,
				  rx, height / 3, 64, 64, i);
		}

		/* Keep a copy of what the encoder is being fed. */
		fwrite(emap, 1, (size_t)epitch * height, src);
		fwrite(emap + (size_t)epitch * height, 1,
		       (size_t)epitch * (height / 2), src);

		memset(&seq, 0, sizeof(seq));
		seq.seq_parameter_set_id = 0;
		seq.level_idc = 31;
		seq.intra_period = 10;
		seq.intra_idr_period = 10;
		seq.ip_period = 1;
		seq.max_num_ref_frames = 1;
		seq.picture_width_in_mbs = (uint16_t)mb_width;
		seq.picture_height_in_mbs = (uint16_t)mb_height;
		seq.seq_fields.bits.chroma_format_idc = 1;
		seq.seq_fields.bits.frame_mbs_only_flag = 1;
		seq.seq_fields.bits.direct_8x8_inference_flag = 1;
		seq.seq_fields.bits.log2_max_frame_num_minus4 = 12;
		seq.seq_fields.bits.pic_order_cnt_type = 2;
		seq.frame_cropping_flag = 0;

		memset(&pic, 0, sizeof(pic));
		pic.CurrPic.picture_id = VA_INVALID_ID;
		pic.CurrPic.flags = VA_PICTURE_H264_INVALID;
		pic.coded_buf = VA_INVALID_ID;
		pic.pic_parameter_set_id = 0;
		pic.seq_parameter_set_id = 0;
		pic.frame_num = (uint16_t)(i % 10);
		pic.pic_init_qp = 26;
		pic.num_ref_idx_l0_active_minus1 = 0;
		pic.chroma_qp_index_offset = 0;
		pic.second_chroma_qp_index_offset = 0;
		pic.pic_fields.bits.idr_pic_flag = is_idr;
		pic.pic_fields.bits.reference_pic_flag = 1;
		pic.pic_fields.bits.entropy_coding_mode_flag = 1;
		pic.pic_fields.bits.transform_8x8_mode_flag = 1;
		pic.pic_fields.bits.constrained_intra_pred_flag = 0;
		pic.pic_fields.bits.deblocking_filter_control_present_flag = 1;
		pic.pic_fields.bits.weighted_pred_flag = 0;

		memset(&slice, 0, sizeof(slice));
		slice.macroblock_address = 0;
		slice.num_macroblocks = mb_width * mb_height;
		slice.macroblock_info = VA_INVALID_ID;
		slice.slice_type = is_idr ? 7 : 5;
		slice.pic_parameter_set_id = 0;
		slice.idr_pic_id = (uint16_t)(i / 10);
		slice.cabac_init_idc = 0;
		slice.slice_qp_delta = 0;
		slice.disable_deblocking_filter_idc = 0;
		slice.slice_alpha_c0_offset_div2 = 0;
		slice.slice_beta_offset_div2 = 0;

		memset(&misc, 0, sizeof(misc));
		misc.type = VAEncMiscParameterTypeFrameRate;

		CHECK_VA(vaCreateBuffer(display, context_id,
					VAEncSequenceParameterBufferType,
					sizeof(seq), 1, &seq, &seq_buf));
		CHECK_VA(vaCreateBuffer(display, context_id,
					VAEncPictureParameterBufferType,
					sizeof(pic), 1, &pic, &pic_buf));
		CHECK_VA(vaCreateBuffer(display, context_id,
					VAEncSliceParameterBufferType,
					sizeof(slice), 1, &slice, &slice_buf));
		CHECK_VA(vaCreateBuffer(display, context_id,
					VAEncMiscParameterBufferType,
					sizeof(misc), 1, &misc, &misc_buf));
		CHECK_VA(vaCreateBuffer(display, context_id,
					VAEncCodedBufferType,
					width * height * 3 + 65536, 1, NULL,
					&coded_buf));

		/* The driver takes the coded buffer from the picture params. */
		pic.coded_buf = coded_buf;
		CHECK_VA(vaCreateBuffer(display, context_id,
					VAEncPictureParameterBufferType,
					sizeof(pic), 1, &pic, &pic_buf));

		buffers[0] = seq_buf;
		buffers[1] = pic_buf;
		buffers[2] = slice_buf;
		buffers[3] = misc_buf;

		CHECK_VA(vaBeginPicture(display, context_id, surfaces[0]));
		CHECK_VA(vaRenderPicture(display, context_id, buffers, 4));
		CHECK_VA(vaEndPicture(display, context_id));
		CHECK_VA(vaSyncSurface(display, surfaces[0]));

		CHECK_VA(vaMapBuffer(display, coded_buf, &coded_data));
		segment = coded_data;
		if (segment == NULL || segment->size == 0) {
			fprintf(stderr, "frame %d: empty coded buffer\n", i);
			return 1;
		}
		fwrite(segment->buf, 1, segment->size, out);
		printf("frame %2d: %s %u bytes (status 0x%x)\n", i,
		       is_idr ? "IDR" : "P  ", segment->size, segment->status);
		CHECK_VA(vaUnmapBuffer(display, coded_buf));

		vaDestroyBuffer(display, seq_buf);
		vaDestroyBuffer(display, pic_buf);
		vaDestroyBuffer(display, slice_buf);
		vaDestroyBuffer(display, misc_buf);
		vaDestroyBuffer(display, coded_buf);
	}

	fclose(out);
	fclose(src);

	vaDestroyImage(display, image.image_id);
	vaDestroyContext(display, context_id);
	vaDestroySurfaces(display, surfaces, 3);
	vaDestroyConfig(display, config_id);
	vaTerminate(display);
	close(drm_fd);

	printf("wrote %s (%d frames)\n", out_path, frames);
	return 0;
}
