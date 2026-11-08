#pragma once

#include <set>
#include <vector>

extern "C" {
#include <va/va.h>
#include <va/va_enc_h264.h>
}

#include "buffer.h"
#include "context.h"
#include "v4l2.h"

struct DriverData;

class H264EncContext : public Context {
public:
    static std::set<VAProfile> supported_profiles(const V4L2M2MDevice& device);

    H264EncContext(DriverData* driver_data, V4L2M2MDevice& device, VAProfile profile, int picture_width,
        int picture_height);
    ~H264EncContext() override;

    VAStatus store_buffer(const Buffer& buffer) const override;
    int set_controls() override;
    VAStatus encode(VASurfaceID surface_id) override;

private:
    std::vector<uint8_t> build_sps_pps(int qp) const;
    std::vector<uint8_t> build_pps(int qp) const;

    VAProfile profile;
    unsigned out_count = 0;
    unsigned cap_count = 0;
    unsigned frame_counter = 0;
    int request_fd = -1;
    VABufferID current_coded = VA_INVALID_ID;

    mutable VAEncSequenceParameterBufferH264 seq = {};
    mutable VAEncPictureParameterBufferH264 pic = {};
    mutable VAEncSliceParameterBufferH264 slice = {};
    mutable bool have_seq = false;
    mutable bool have_pic = false;
    mutable bool have_slice = false;

    mutable VAEncPackedHeaderParameterBuffer pending_header = {};
    mutable bool have_pending_header = false;
    mutable std::vector<uint8_t> sps_pps;
    mutable std::vector<uint8_t> sei;

    /* Rate control state. */
    mutable bool have_rc = false;
    mutable unsigned target_bps = 0;
    mutable unsigned framerate = 30;
    // The QP the last picture parameter set was written at: the decoder takes
    // the QP from the set, the hardware writes no slice delta of its own, so a
    // set that lags the encoder's rate control decodes at the wrong QP.
    mutable int last_pps_qp = -1;
    mutable int rc_qp = 0;
};
