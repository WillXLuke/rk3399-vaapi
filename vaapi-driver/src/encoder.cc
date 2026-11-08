#include "encoder.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <ctime>
#include <stdexcept>

extern "C" {
#include <linux/media.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
}

#include "driver.h"
#include "hantro_h264_encode.h"
#include "media.h"
#include "surface.h"

namespace {

class BitWriter {
public:
    void put_bit(unsigned bit)
    {
        bit_buf = (bit_buf << 1) | (bit & 1);
        if (++bit_count == 8) {
            data.push_back(bit_buf);
            bit_buf = 0;
            bit_count = 0;
        }
    }
    void put_bits(unsigned val, unsigned count)
    {
        while (count--)
            put_bit((val >> count) & 1);
    }
    void ue(unsigned val)
    {
        unsigned code = val + 1;
        unsigned len = 0;
        for (unsigned v = code; v; v >>= 1)
            len++;
        put_bits(0, len - 1);
        put_bits(code, len);
    }
    void se(int val)
    {
        ue(val > 0 ? 2 * val - 1 : -2 * val);
    }
    void trailing()
    {
        put_bit(1);
        while (bit_count)
            put_bit(0);
    }
    std::vector<uint8_t> take()
    {
        trailing();
        return std::move(data);
    }

private:
    std::vector<uint8_t> data;
    unsigned bit_buf = 0;
    unsigned bit_count = 0;
};

void append_nal(std::vector<uint8_t>& out, unsigned type, const std::vector<uint8_t>& rbsp)
{
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    out.insert(out.end(), sc, sc + 4);
    out.push_back(0x60 | type);
    out.insert(out.end(), rbsp.begin(), rbsp.end());
}

} // namespace

std::set<VAProfile> H264EncContext::supported_profiles(const V4L2M2MDevice& device)
{
    if (!device.format_supported(device.capture_buf_type, V4L2_PIX_FMT_H264)
        || !device.format_supported(device.output_buf_type, V4L2_PIX_FMT_YUV420M))
        return {};
    return { VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High };
}

H264EncContext::H264EncContext(DriverData* driver_data, V4L2M2MDevice& device, VAProfile profile,
    int picture_width, int picture_height)
    : Context(driver_data, device, V4L2_PIX_FMT_YUV420M, picture_width, picture_height, {})
    , profile(profile)
{
    is_encoder = true;
    device.set_format(device.capture_buf_type, V4L2_PIX_FMT_H264, picture_width, picture_height);
    out_count = device.request_buffers(device.output_buf_type, 4);
    cap_count = device.request_buffers(device.capture_buf_type, 4);
    device.set_streaming(true);
    request_fd = media_request_alloc(device.media_fd);
}

H264EncContext::~H264EncContext()
{
    device.set_streaming(false);
    if (request_fd >= 0)
        close(request_fd);
}

VAStatus H264EncContext::store_buffer(const Buffer& buffer) const
{
    switch (buffer.type) {
    case VAEncSequenceParameterBufferType:
        std::memcpy(&seq, buffer.data.get(), std::min<size_t>(buffer.size, sizeof(seq)));
        have_seq = true;
        break;
    case VAEncPictureParameterBufferType:
        std::memcpy(&pic, buffer.data.get(), std::min<size_t>(buffer.size, sizeof(pic)));
        have_pic = true;
        break;
    case VAEncSliceParameterBufferType:
        std::memcpy(&slice, buffer.data.get(), std::min<size_t>(buffer.size, sizeof(slice)));
        have_slice = true;
        break;
    case VAEncPackedHeaderParameterBufferType:
        std::memcpy(&pending_header, buffer.data.get(), std::min<size_t>(buffer.size, sizeof(pending_header)));
        have_pending_header = true;
        break;
    case VAEncPackedHeaderDataBufferType:
        if (have_pending_header) {
            if (pending_header.type == VAEncPackedHeaderSequence)
                sps_pps.assign(buffer.data.get(), buffer.data.get() + buffer.size);
            else if (pending_header.type == VAEncPackedHeaderH264_SEI)
                sei.assign(buffer.data.get(), buffer.data.get() + buffer.size);
            have_pending_header = false;
        }
        break;
    case VAEncMiscParameterBufferType: {
        auto* misc = reinterpret_cast<VAEncMiscParameterBuffer*>(buffer.data.get());
        if (misc->type == VAEncMiscParameterTypeRateControl) {
            auto* rc = reinterpret_cast<VAEncMiscParameterRateControl*>(misc->data);
            target_bps = rc->bits_per_second;
            have_rc = target_bps != 0;
        } else if (misc->type == VAEncMiscParameterTypeFrameRate) {
            auto* fr = reinterpret_cast<VAEncMiscParameterFrameRate*>(misc->data);
            // The field is written as frames per second, but libva clients differ in
            // how they pack it: a plain integer, 16.16 fixed point, or the low 16 bits
            // with a count in the high half. Take the first reading that is a rate.
            unsigned raw = fr->framerate;
            unsigned candidates[] = { raw, raw >> 16, raw & 0xFFFF };
            for (unsigned v : candidates) {
                if (v >= 1 && v <= 240) {
                    framerate = v;
                    break;
                }
            }
        }
        break;
    }
    default:
        break;
    }
    return VA_STATUS_SUCCESS;
}

int H264EncContext::set_controls()
{
    return 0;
}

std::vector<uint8_t> H264EncContext::build_sps_pps(int qp) const
{
    BitWriter w;
    const unsigned wmbs = (picture_width + 15) / 16;
    const unsigned hmbs = (picture_height + 15) / 16;
    const unsigned profile_idc = (profile == VAProfileH264High) ? 100
        : (profile == VAProfileH264Main) ? 77 : 66;
    const bool cabac = false; /* VPU2 CABAC header padding is not standard */

    w.put_bits(profile_idc, 8);
    w.put_bits(0, 8); /* constraint flags */
    w.put_bits(51, 8); /* level */
    w.ue(0); /* sps id */
    if (profile_idc == 100) {
        w.ue(1); /* 4:2:0 */
        w.ue(0);
        w.ue(0);
        w.put_bit(0);
        w.put_bit(0); /* no scaling matrix */
    }
    /* The VPU2 slice header always writes a 16-bit frame_num. */
    w.ue(12); /* log2_max_frame_num_minus4 */
    w.ue(2); /* pic_order_cnt_type */
    w.ue(1); /* max_num_ref_frames */
    w.put_bit(0); /* gaps */
    w.ue(wmbs - 1);
    w.ue(hmbs - 1);
    w.put_bit(1); /* frame_mbs_only */
    w.put_bit(1); /* direct_8x8 */
    w.put_bit(1); /* cropping */
    w.ue(0); /* left */
    w.ue(0); /* right */
    w.ue(0); /* top */
    w.ue(hmbs * 16 > 1080 ? (hmbs * 16 - 1080) / 2 : 0); /* bottom */
    w.put_bit(0); /* vui */
    auto sps = w.take();

    BitWriter p;
    p.ue(0); /* pps id */
    p.ue(0); /* sps id */
    p.put_bit(cabac);
    p.put_bit(0); /* bottom field pic order */
    p.ue(0); /* num slice groups minus1 */
    p.ue(0); /* num ref idx l0 */
    p.ue(0); /* num ref idx l1 */
    p.put_bit(0); /* weighted pred */
    p.put_bits(0, 2); /* weighted bipred */
    p.se(qp - 26);
    p.se(0); /* pic init qs */
    p.se(0); /* chroma qp offset */
    p.put_bit(1); /* deblocking filter control present */
    p.put_bit(0); /* constrained intra */
    p.put_bit(0); /* redundant pic cnt */
    if (profile_idc == 100) {
        p.put_bit(pic.pic_fields.bits.transform_8x8_mode_flag);
        p.put_bit(0); /* pic_scaling_matrix_present */
        p.se(pic.second_chroma_qp_index_offset);
    }
    auto pps = p.take();

    std::vector<uint8_t> out;
    append_nal(out, 7, sps);
    append_nal(out, 8, pps);
    return out;
}

std::vector<uint8_t> H264EncContext::build_pps(int qp) const
{
    const unsigned profile_idc = (profile == VAProfileH264High) ? 100
        : (profile == VAProfileH264Main) ? 77 : 66;
    BitWriter p;
    p.ue(0); /* pps id */
    p.ue(0); /* sps id */
    p.put_bit(0); /* cabac */
    p.put_bit(0); /* bottom field pic order */
    p.ue(0); /* num slice groups minus1 */
    p.ue(0); /* num ref idx l0 */
    p.ue(0); /* num ref idx l1 */
    p.put_bit(0); /* weighted pred */
    p.put_bits(0, 2); /* weighted bipred */
    p.se(qp - 26);
    p.se(0); /* pic init qs */
    p.se(0); /* chroma qp offset */
    p.put_bit(1); /* deblocking filter control present */
    p.put_bit(0); /* constrained intra */
    p.put_bit(0); /* redundant pic cnt */
    if (profile_idc == 100) {
        p.put_bit(pic.pic_fields.bits.transform_8x8_mode_flag);
        p.put_bit(0); /* pic_scaling_matrix_present */
        p.se(pic.second_chroma_qp_index_offset);
    }
    std::vector<uint8_t> out;
    append_nal(out, 8, p.take());
    return out;
}

VAStatus H264EncContext::encode(VASurfaceID surface_id)
{
    if (!driver_data->surfaces.contains(surface_id))
        return VA_STATUS_ERROR_INVALID_SURFACE;
    auto& surface = driver_data->surfaces.at(surface_id);
    if (surface.host.empty()) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (pic.coded_buf != VA_INVALID_ID && driver_data->buffers.contains(pic.coded_buf)) {
        current_coded = pic.coded_buf;
    } else if (last_coded != VA_INVALID_ID && driver_data->buffers.contains(last_coded)) {
        current_coded = last_coded;
    } else if (current_coded == VA_INVALID_ID) {
        if (coded_buffers.empty()) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        current_coded = coded_buffers.front();
        coded_buffers.pop_front();
    }
    auto& coded = driver_data->buffers.at(current_coded);
    auto* seg = reinterpret_cast<VACodedBufferSegment*>(coded.data.get());

    const unsigned out_idx = frame_counter % out_count;
    const unsigned cap_idx = frame_counter % cap_count;
    auto& ob = device.buffer(device.output_buf_type, out_idx);
    auto oplanes = ob.mapping();
    if (oplanes.size() < 3)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    const unsigned src_h = std::min<unsigned>(surface.height, picture_height);
    const unsigned src_ch = std::min<unsigned>(surface.height / 2, picture_height / 2);
    if (src_h == static_cast<unsigned>(picture_height))
        std::memcpy(oplanes[0].data(), surface.host.data(),
            static_cast<size_t>(picture_width) * picture_height);
    else {
        for (unsigned y = 0; y < src_h; y++)
            std::memcpy(oplanes[0].data() + y * picture_width, surface.host.data() + y * picture_width, picture_width);
        for (unsigned y = src_h; y < static_cast<unsigned>(picture_height); y++)
            std::memset(oplanes[0].data() + y * picture_width, 0, picture_width);
    }
    for (unsigned y = 0; y < src_ch; y++) {
        const uint8_t* uv = surface.host.data() + picture_width * surface.height + y * picture_width;
        uint8_t* u = oplanes[1].data() + y * (picture_width / 2);
        uint8_t* v = oplanes[2].data() + y * (picture_width / 2);
        unsigned n = picture_width / 2;
#if defined(__aarch64__)
        for (; n >= 8; n -= 8, uv += 16, u += 8, v += 8) {
            uint8x8x2_t d = vld2_u8(uv);
            vst1_u8(u, d.val[0]);
            vst1_u8(v, d.val[1]);
        }
#endif
        for (unsigned x = 0; x < n; x++) {
            u[x] = uv[2 * x];
            v[x] = uv[2 * x + 1];
        }
    }
    for (unsigned y = src_ch; y < static_cast<unsigned>(picture_height) / 2; y++) {
        std::memset(oplanes[1].data() + y * (picture_width / 2), 0, picture_width / 2);
        std::memset(oplanes[2].data() + y * (picture_width / 2), 0, picture_width / 2);
    }

    const bool intra = (slice.slice_type % 5) == 2 || pic.pic_fields.bits.idr_pic_flag
       ;
    int qp = pic.pic_init_qp ? pic.pic_init_qp : 26;
    if (have_rc && target_bps) {
        if (rc_qp == 0)
            rc_qp = qp;
        qp = rc_qp;
    }
    qp = std::clamp(qp + (intra ? -2 : 0), 10, 45);

    struct hantro_h264_enc_reg_params params = {};
    params.frame_coding_type = intra ? 1 : 0;
    params.pic_init_qp = qp;
    params.qp = qp;
    params.qp_min = 0;
    params.qp_max = 51;
    params.slice_alpha_offset = slice.slice_alpha_c0_offset_div2;
    params.slice_beta_offset = slice.slice_beta_offset_div2;
    params.chroma_qp_index_offset = pic.chroma_qp_index_offset;
    params.filter_disable = slice.disable_deblocking_filter_idc;
    params.idr_pic_id = pic.pic_fields.bits.idr_pic_flag ? 0 : 0;
    params.pps_id = pic.pic_parameter_set_id;
    params.frame_num = pic.frame_num;
    params.slice_size_mb_rows = picture_height / 16;
    params.enable_cabac = 0;
    params.transform8x8_mode = pic.pic_fields.bits.transform_8x8_mode_flag;
    params.cabac_init_idc = 0;
    struct v4l2_ext_control ctrl = {
        .id = V4L2_CID_HANTRO_H264_ENCODE_PARAMS,
        .size = sizeof(params),
        .ptr = &params,
    };
    try {
        device.set_ext_controls(request_fd, std::span(&ctrl, 1));
    } catch (std::exception& e) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    try {
        struct timeval ts = { .tv_sec = static_cast<time_t>(frame_counter / 1000000),
                              .tv_usec = static_cast<suseconds_t>(frame_counter % 1000000) };
        ob.queue(request_fd, &ts, std::array<unsigned, 3> {
            static_cast<unsigned>(picture_width * picture_height),
            static_cast<unsigned>(picture_width * picture_height / 4),
            static_cast<unsigned>(picture_width * picture_height / 4) });
        auto& cb = device.buffer(device.capture_buf_type, cap_idx);
        cb.queue(-1, nullptr, cb.mapping()[0].size());
        media_request_queue(request_fd);
    } catch (std::exception& e) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    struct v4l2_buffer dq = { .type = device.capture_buf_type, .memory = V4L2_MEMORY_MMAP };
    struct v4l2_plane dqp[VIDEO_MAX_PLANES] = {};
    dq.m.planes = dqp;
    dq.length = 1;
    if (ioctl(device.video_fd, VIDIOC_DQBUF, &dq) < 0) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    struct v4l2_buffer oq = { .type = device.output_buf_type, .memory = V4L2_MEMORY_MMAP };
    struct v4l2_plane oqp[VIDEO_MAX_PLANES] = {};
    oq.m.planes = oqp;
    oq.length = device.buffer(device.output_buf_type, out_idx).mapping().size();
    if (ioctl(device.video_fd, VIDIOC_DQBUF, &oq) < 0) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    try {
        media_request_reinit(request_fd);
    } catch (std::exception& e) {
    }

    const auto& cbuf = device.buffer(device.capture_buf_type, dq.index);
    const unsigned bytes = dqp[0].bytesused;
    const auto cmap = cbuf.mapping();
    if (cmap.empty() || bytes > cmap[0].size())
        return VA_STATUS_ERROR_OPERATION_FAILED;

    std::vector<uint8_t> prefix;
    if (intra) {
        prefix = build_sps_pps(qp);
        last_pps_qp = qp;
        if (!sei.empty())
            prefix.insert(prefix.end(), sei.begin(), sei.end());
    } else if (qp != last_pps_qp) {
        prefix = build_pps(qp);
        last_pps_qp = qp;
    }

    if (prefix.size() + bytes > coded.size) {
        return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
    }

    std::memcpy(seg->buf, prefix.data(), prefix.size());
    std::memcpy(static_cast<uint8_t*>(seg->buf) + prefix.size(), cmap[0].data(), bytes);
    seg->size = prefix.size() + bytes;
    seg->status = 0;
    seg->next = nullptr;

    if (have_rc && target_bps && framerate) {
        const unsigned target = target_bps / 8 / framerate;
        if (target && seg->size > target * 11 / 10)
            rc_qp = std::min(rc_qp + 1, 45);
        else if (target && seg->size < target * 8 / 10)
            rc_qp = std::max(rc_qp - 1, 10);
    }
    current_coded = VA_INVALID_ID;
    frame_counter++;
    return VA_STATUS_SUCCESS;
}
