// SPDX-License-Identifier: MIT
// Minimal V4L2 M2M H.264 encoder smoke test for the Hantro/Rockchip VPU2.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define W 1920
#define H 1088
#define CID_PARAMS 0x00982900
#define PARAMS_SIZE 174

struct params {
    uint32_t frame_coding_type;
    int32_t pic_init_qp, slice_alpha_offset, slice_beta_offset, chroma_qp_index_offset;
    int32_t filter_disable;
    uint16_t idr_pic_id;
    int32_t pps_id, frame_num, slice_size_mb_rows, h264_inter4x4_disabled;
    int32_t enable_cabac, transform8x8_mode, cabac_init_idc;
    int32_t qp, mad_qp_delta, mad_threshold, qp_min, qp_max, cp_distance_mbs;
    int32_t cp_target[10], target_error[7], delta_qp[7];
} __attribute__((packed));

struct buf { void *plane[3]; size_t len[3]; unsigned nplanes; };

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

static int reqbufs(int fd, enum v4l2_buf_type type, unsigned n, unsigned nplanes, struct buf **out) {
    struct v4l2_requestbuffers rb = { .type = type, .memory = V4L2_MEMORY_MMAP, .count = n };
    if (xioctl(fd, VIDIOC_REQBUFS, &rb) < 0) { perror("REQBUFS"); return -1; }
    struct buf *b = calloc(rb.count, sizeof(*b));
    for (unsigned i = 0; i < rb.count; i++) {
        struct v4l2_buffer q = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
        q.m.planes = planes; q.length = nplanes;
        if (xioctl(fd, VIDIOC_QUERYBUF, &q) < 0) { perror("QUERYBUF"); return -1; }
        b[i].nplanes = nplanes;
        for (unsigned j = 0; j < nplanes; j++) {
            b[i].len[j] = planes[j].length;
            b[i].plane[j] = mmap(NULL, planes[j].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                                 fd, planes[j].m.mem_offset);
            if (b[i].plane[j] == MAP_FAILED) { perror("mmap"); return -1; }
        }
    }
    *out = b;
    return rb.count;
}

static int set_fmt(int fd, enum v4l2_buf_type type, uint32_t fourcc, unsigned w, unsigned h, unsigned planes) {
    struct v4l2_format f = { .type = type };
    f.fmt.pix_mp.width = w; f.fmt.pix_mp.height = h;
    f.fmt.pix_mp.pixelformat = fourcc;
    f.fmt.pix_mp.num_planes = planes;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = w * h * 3 / 2;
    if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) { perror("S_FMT"); return -1; }
    fprintf(stderr, "fmt type=%d %ux%u fourcc=%08x planes=%u size0=%u\n", type, f.fmt.pix_mp.width,
            f.fmt.pix_mp.height, f.fmt.pix_mp.pixelformat, f.fmt.pix_mp.num_planes,
            f.fmt.pix_mp.plane_fmt[0].sizeimage);
    return 0;
}

int main(int argc, char **argv) {
    const char *video = argc > 1 ? argv[1] : "/dev/video1";
    const char *media = argc > 2 ? argv[2] : "/dev/media0";
    const char *inpath = argc > 3 ? argv[3] : "test/frame0.nv12";
    const char *outpath = argc > 4 ? argv[4] : "/tmp/enc.h264";
    int vfd = open(video, O_RDWR);
    int mfd = open(media, O_RDWR);
    if (vfd < 0 || mfd < 0) { perror("open"); return 1; }

    if (set_fmt(vfd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_PIX_FMT_H264, W, H, 1) < 0) return 1;
    if (set_fmt(vfd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_PIX_FMT_NV12M, W, H, 2) < 0) return 1;

    struct buf *cap, *out;
    if (reqbufs(vfd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 2, 1, &cap) < 0) return 1;
    if (reqbufs(vfd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 2, 2, &out) < 0) return 1;

    FILE *f = fopen(inpath, "rb");
    if (!f) { perror("input"); return 1; }
    uint8_t *frame = malloc(W * H * 3 / 2);
    if (fread(frame, 1, W * H * 3 / 2, f) != W * H * 3 / 2) { fprintf(stderr, "short input\n"); return 1; }
    fclose(f);
    memcpy(out[0].plane[0], frame, W * H);
    memcpy(out[0].plane[1], frame + W * H, W * H / 2);

    struct params p = {0};
    p.frame_coding_type = 1;
    p.pic_init_qp = 26; p.qp = 26; p.qp_min = 0; p.qp_max = 51;
    p.slice_size_mb_rows = H / 16;
    p.enable_cabac = 0; p.cabac_init_idc = 0;

    int reqfd;
    if (ioctl(mfd, MEDIA_IOC_REQUEST_ALLOC, &reqfd) < 0) { perror("REQUEST_ALLOC"); return 1; }

    struct v4l2_ext_control c = { .id = CID_PARAMS, .size = PARAMS_SIZE, .ptr = &p };
    struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_REQUEST_VAL, .count = 1,
                                    .controls = &c, .request_fd = reqfd };
    if (xioctl(vfd, VIDIOC_S_EXT_CTRLS, &cs) < 0) { perror("S_EXT_CTRLS"); return 1; }

    enum v4l2_buf_type cap_t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    enum v4l2_buf_type out_t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(vfd, VIDIOC_STREAMON, &cap_t) < 0) { perror("STREAMON cap"); return 1; }
    if (xioctl(vfd, VIDIOC_STREAMON, &out_t) < 0) { perror("STREAMON out"); return 1; }

    struct v4l2_plane op[2] = { { .bytesused = W * H }, { .bytesused = W * H / 2 } };
    struct v4l2_buffer oq = { .type = out_t, .memory = V4L2_MEMORY_MMAP, .index = 0,
                              .m.planes = op, .length = 2,
                              .flags = V4L2_BUF_FLAG_REQUEST_FD, .request_fd = reqfd,
                              .timestamp = { .tv_sec = 0, .tv_usec = 1 } };
    if (xioctl(vfd, VIDIOC_QBUF, &oq) < 0) { perror("QBUF out"); return 1; }

    struct v4l2_plane cp[1] = {};
    struct v4l2_buffer cq = { .type = cap_t, .memory = V4L2_MEMORY_MMAP, .index = 0,
                              .m.planes = cp, .length = 1 };
    if (xioctl(vfd, VIDIOC_QBUF, &cq) < 0) { perror("QBUF cap"); return 1; }

    if (ioctl(reqfd, MEDIA_REQUEST_IOC_QUEUE, 0) < 0) { perror("REQUEST_QUEUE"); return 1; }

    struct pollfd pfd = { .fd = vfd, .events = POLLIN };
    if (poll(&pfd, 1, 3000) <= 0) { perror("poll"); return 1; }

    struct v4l2_plane dp[1] = {};
    struct v4l2_buffer dq = { .type = cap_t, .memory = V4L2_MEMORY_MMAP, .m.planes = dp, .length = 1 };
    if (xioctl(vfd, VIDIOC_DQBUF, &dq) < 0) { perror("DQBUF cap"); return 1; }
    fprintf(stderr, "captured %u bytes, index %u, flags %#x\n", dp[0].bytesused, dq.index, dq.flags);

    FILE *of = fopen(outpath, "wb");
    fwrite(cap[dq.index].plane[0], 1, dp[0].bytesused, of);
    fclose(of);
    fprintf(stderr, "wrote %s\n", outpath);
    return 0;
}
