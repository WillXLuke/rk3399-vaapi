/*
 * Mimic a real screencast/WebRTC encoder (KasmVNC/OBS style) on top of
 * libavcodec's h264_vaapi: one hw device, one reused NV12 hw frame,
 * configurable rate control, and optional mid-stream re-initialisation
 * (what a frame-rate / quality change in the UI does).
 *
 * usage: enc_ffmpeg_like <w> <h> <fps> <rc_mode> <qp|bitrate> <frames> <reinit_every>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

static AVBufferRef *hw_device;
static AVBufferRef *hw_frames;
static enum AVPixelFormat sw_fmt = AV_PIX_FMT_NV12;
static int W, H, FPS;
static const char *RC;
static int QP;
static long long RATE;

static void fill_nv12(AVFrame *f, int frame_no, int w, int h)
{
	int y, x;
	uint8_t *yp = f->data[0], *uv = f->data[1];
	int ypitch = f->linesize[0], uvpitch = f->linesize[1];

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			yp[y * ypitch + x] = (uint8_t)(x * 255 / w + y * 63 / h +
						       frame_no * 3);
	for (y = 0; y < h / 2; y++)
		for (x = 0; x < w; x += 2) {
			uv[y * uvpitch + x] = (uint8_t)(90 + (x * 40 / w));
			uv[y * uvpitch + x + 1] = (uint8_t)(160 - (y * 40 / h));
		}
	/* moving rectangle so P frames carry real changes */
	for (y = h / 3; y < h / 3 + 48 && y < h; y++)
		for (x = (frame_no * 13) % (w - 48); x < (frame_no * 13) % (w - 48) + 48; x++)
			yp[y * ypitch + x] = 235;
	(void)uv;
}

static void dump_err(int err, const char *what)
{
	char buf[128];
	av_strerror(err, buf, sizeof(buf));
	fprintf(stderr, "%s: %s\n", what, buf);
}

int main(int argc, char **argv)
{
	const AVCodec *codec;
	AVCodecContext *ctx = NULL;
	AVFrame *sw = NULL, *hw = NULL;
	AVPacket *pkt = NULL;
	int64_t pts = 0;
	int i, err;

	if (argc < 8) {
		fprintf(stderr, "usage: %s <w> <h> <fps> <rc_mode> <qp|bitrate> "
			"<frames> <reinit_every>\n", argv[0]);
		return 2;
	}
	W = atoi(argv[1]); H = atoi(argv[2]); FPS = atoi(argv[3]);
	RC = argv[4];
	QP = atoi(argv[5]);
	RATE = atoll(argv[6]);
	int reinit = atoi(argv[7]);

	codec = avcodec_find_encoder_by_name("h264_vaapi");
	if (!codec) { fprintf(stderr, "no h264_vaapi\n"); return 2; }

	err = av_hwdevice_ctx_create(&hw_device, AV_HWDEVICE_TYPE_VAAPI,
				     "/dev/dri/renderD128", NULL, 0);
	if (err < 0) { dump_err(err, "hwdevice"); return 2; }

	for (i = 0; i < RATE; i++) {
		if (i == 0 || (reinit > 0 && i > 0 && i % reinit == 0)) {
			if (ctx) {
				avcodec_flush_buffers(ctx);
				avcodec_free_context(&ctx);
			}
			if (hw_frames) av_buffer_unref(&hw_frames);
			if (sw) av_frame_free(&sw);
			if (hw) av_frame_free(&hw);

			ctx = avcodec_alloc_context3(codec);
			ctx->width = W; ctx->height = H;
			ctx->time_base = (AVRational){1, FPS};
			ctx->framerate = (AVRational){FPS, 1};
			ctx->gop_size = 12;
			ctx->max_b_frames = 0;
			ctx->pix_fmt = AV_PIX_FMT_VAAPI;
			ctx->profile = FF_PROFILE_H264_HIGH;
			ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
			ctx->delay = 0;
			if (!strcmp(RC, "CQP"))
				av_opt_set(ctx->priv_data, "rc_mode", "CQP", 0);
			else if (!strcmp(RC, "CBR")) {
				av_opt_set(ctx->priv_data, "rc_mode", "CBR", 0);
				ctx->bit_rate = QP * 1000;
			} else if (!strcmp(RC, "VBR")) {
				av_opt_set(ctx->priv_data, "rc_mode", "VBR", 0);
				ctx->bit_rate = QP * 1000;
			} else if (!strcmp(RC, "ICQ")) {
				av_opt_set(ctx->priv_data, "rc_mode", "ICQ", 0);
				av_opt_set_int(ctx->priv_data, "qp", QP, 0);
			} else if (!strcmp(RC, "QVBR")) {
				av_opt_set(ctx->priv_data, "rc_mode", "QVBR", 0);
				ctx->bit_rate = QP * 1000;
			}
			av_opt_set_int(ctx->priv_data, "qp", QP, 0);
			av_opt_set_int(ctx->priv_data, "async_depth", 1, 0);

			hw_frames = av_hwframe_ctx_alloc(hw_device);
			AVHWFramesContext *fc = (AVHWFramesContext *)hw_frames->data;
			fc->format = AV_PIX_FMT_VAAPI;
			fc->sw_format = sw_fmt;
			fc->width = W; fc->height = H;
			fc->initial_pool_size = 4;
			err = av_hwframe_ctx_init(hw_frames);
			if (err < 0) { dump_err(err, "hwframe ctx"); return 2; }
			ctx->hw_frames_ctx = av_buffer_ref(hw_frames);

			err = avcodec_open2(ctx, codec, NULL);
			if (err < 0) { dump_err(err, "avcodec_open2"); return 2; }

			sw = av_frame_alloc();
			sw->format = sw_fmt; sw->width = W; sw->height = H;
			av_frame_get_buffer(sw, 0);
			hw = av_frame_alloc();
			av_hwframe_get_buffer(hw_frames, hw, 0);

			pkt = av_packet_alloc();
		}

		av_frame_make_writable(sw);
		fill_nv12(sw, i, W, H);
		sw->pts = pts++;
		av_hwframe_transfer_data(hw, sw, 0);
		hw->pts = sw->pts;
		/* like a normal encoder: let the GOP decide I/P */

		err = avcodec_send_frame(ctx, hw);
		if (err < 0) {
			fprintf(stderr, "send_frame failed at %d\n", i);
			dump_err(err, "send_frame");
			return 1;
		}
		while ((err = avcodec_receive_packet(ctx, pkt)) >= 0) {
			printf("frame %d: pkt %d bytes key=%d\n", i, pkt->size,
			       !!(pkt->flags & AV_PKT_FLAG_KEY));
			av_packet_unref(pkt);
		}
		if (err != AVERROR(EAGAIN)) {
			fprintf(stderr, "receive_packet at %d\n", i);
			dump_err(err, "receive");
			return 1;
		}
	}
	printf("done %d frames\n", i);
	return 0;
}
