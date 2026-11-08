/*
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2026 William Luke
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

#include "h265.h"

#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
#include <va/va_dec_hevc.h>
}

#include "buffer.h"
#include "driver.h"
#include "surface.h"
#include "v4l2.h"

namespace {

constexpr unsigned kMaxDpb = 15;

// ISO/IEC 23008-2 NAL unit header.
inline unsigned nal_unit_type(const uint8_t* data)
{
    return (data[0] >> 1) & 0x3f;
}

inline unsigned nuh_temporal_id_plus1(const uint8_t* data)
{
    return data[1] & 0x7;
}

void fill_sps(const VAPictureParameterBufferHEVC* picture, v4l2_ctrl_hevc_sps* sps)
{
    *sps = {};
    sps->chroma_format_idc = picture->pic_fields.bits.chroma_format_idc;
    sps->pic_width_in_luma_samples = picture->pic_width_in_luma_samples;
    sps->pic_height_in_luma_samples = picture->pic_height_in_luma_samples;
    sps->bit_depth_luma_minus8 = picture->bit_depth_luma_minus8;
    sps->bit_depth_chroma_minus8 = picture->bit_depth_chroma_minus8;
    sps->log2_max_pic_order_cnt_lsb_minus4 = picture->log2_max_pic_order_cnt_lsb_minus4;
    sps->sps_max_dec_pic_buffering_minus1 = picture->sps_max_dec_pic_buffering_minus1;
    sps->sps_max_num_reorder_pics = 0;
    sps->sps_max_latency_increase_plus1 = 0;
    sps->log2_min_luma_coding_block_size_minus3 = picture->log2_min_luma_coding_block_size_minus3;
    sps->log2_diff_max_min_luma_coding_block_size = picture->log2_diff_max_min_luma_coding_block_size;
    sps->log2_min_luma_transform_block_size_minus2 = picture->log2_min_transform_block_size_minus2;
    sps->log2_diff_max_min_luma_transform_block_size = picture->log2_diff_max_min_transform_block_size;
    sps->max_transform_hierarchy_depth_inter = picture->max_transform_hierarchy_depth_inter;
    sps->max_transform_hierarchy_depth_intra = picture->max_transform_hierarchy_depth_intra;
    sps->pcm_sample_bit_depth_luma_minus1 = picture->pcm_sample_bit_depth_luma_minus1;
    sps->pcm_sample_bit_depth_chroma_minus1 = picture->pcm_sample_bit_depth_chroma_minus1;
    sps->log2_min_pcm_luma_coding_block_size_minus3 = picture->log2_min_pcm_luma_coding_block_size_minus3;
    sps->log2_diff_max_min_pcm_luma_coding_block_size = picture->log2_diff_max_min_pcm_luma_coding_block_size;
    sps->num_short_term_ref_pic_sets = picture->num_short_term_ref_pic_sets;
    sps->num_long_term_ref_pics_sps = picture->num_long_term_ref_pic_sps;

    sps->flags = (picture->pic_fields.bits.separate_colour_plane_flag ? V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE : 0)
        | (picture->pic_fields.bits.scaling_list_enabled_flag ? V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED : 0)
        | (picture->pic_fields.bits.amp_enabled_flag ? V4L2_HEVC_SPS_FLAG_AMP_ENABLED : 0)
        | (picture->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag
                ? V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET
                : 0)
        | (picture->pic_fields.bits.pcm_enabled_flag ? V4L2_HEVC_SPS_FLAG_PCM_ENABLED : 0)
        | (picture->pic_fields.bits.pcm_loop_filter_disabled_flag ? V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED : 0)
        | (picture->slice_parsing_fields.bits.long_term_ref_pics_present_flag
                ? V4L2_HEVC_SPS_FLAG_LONG_TERM_REF_PICS_PRESENT
                : 0)
        | (picture->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag
                ? V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED
                : 0)
        | (picture->pic_fields.bits.strong_intra_smoothing_enabled_flag
                ? V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED
                : 0);
}

void fill_pps(const VAPictureParameterBufferHEVC* picture, v4l2_ctrl_hevc_pps* pps)
{
    *pps = {};
    pps->num_extra_slice_header_bits = picture->num_extra_slice_header_bits;
    pps->num_ref_idx_l0_default_active_minus1 = picture->num_ref_idx_l0_default_active_minus1;
    pps->num_ref_idx_l1_default_active_minus1 = picture->num_ref_idx_l1_default_active_minus1;
    pps->init_qp_minus26 = picture->init_qp_minus26;
    pps->diff_cu_qp_delta_depth = picture->diff_cu_qp_delta_depth;
    pps->pps_cb_qp_offset = picture->pps_cb_qp_offset;
    pps->pps_cr_qp_offset = picture->pps_cr_qp_offset;
    pps->num_tile_columns_minus1 = picture->num_tile_columns_minus1;
    pps->num_tile_rows_minus1 = picture->num_tile_rows_minus1;
    for (unsigned i = 0; i < std::size(pps->column_width_minus1); i++)
        pps->column_width_minus1[i] = static_cast<uint8_t>(picture->column_width_minus1[i]);
    for (unsigned i = 0; i < std::size(pps->row_height_minus1); i++)
        pps->row_height_minus1[i] = static_cast<uint8_t>(picture->row_height_minus1[i]);
    pps->pps_beta_offset_div2 = picture->pps_beta_offset_div2;
    pps->pps_tc_offset_div2 = picture->pps_tc_offset_div2;
    pps->log2_parallel_merge_level_minus2 = picture->log2_parallel_merge_level_minus2;

    pps->flags =
        (picture->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag
                ? V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED
                : 0)
        | (picture->slice_parsing_fields.bits.output_flag_present_flag ? V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT : 0)
        | (picture->pic_fields.bits.sign_data_hiding_enabled_flag ? V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED : 0)
        | (picture->slice_parsing_fields.bits.cabac_init_present_flag ? V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT : 0)
        | (picture->pic_fields.bits.constrained_intra_pred_flag ? V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED : 0)
        | (picture->pic_fields.bits.transform_skip_enabled_flag ? V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED : 0)
        | (picture->pic_fields.bits.cu_qp_delta_enabled_flag ? V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED : 0)
        | (picture->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag
                ? V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT
                : 0)
        | (picture->pic_fields.bits.weighted_pred_flag ? V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED : 0)
        | (picture->pic_fields.bits.weighted_bipred_flag ? V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED : 0)
        | (picture->pic_fields.bits.transquant_bypass_enabled_flag ? V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED : 0)
        | (picture->pic_fields.bits.tiles_enabled_flag ? V4L2_HEVC_PPS_FLAG_TILES_ENABLED : 0)
        | (picture->pic_fields.bits.entropy_coding_sync_enabled_flag ? V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED : 0)
        | (picture->pic_fields.bits.loop_filter_across_tiles_enabled_flag
                ? V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED
                : 0)
        | (picture->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag
                ? V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED
                : 0)
        | (picture->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag
                ? V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED
                : 0)
        | (picture->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag
                ? V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER
                : 0)
        | (picture->slice_parsing_fields.bits.lists_modification_present_flag
                ? V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT
                : 0)
        | (picture->slice_parsing_fields.bits.slice_segment_header_extension_present_flag
                ? V4L2_HEVC_PPS_FLAG_SLICE_SEGMENT_HEADER_EXTENSION_PRESENT
                : 0);
}

// Number of entries added to the V4L2 DPB, in the same order as the VA
// ReferenceFrames[] array.  VA slice RefPicList[] entries index that array
// directly.
struct RefMap {
    unsigned count = 0;
};

void fill_decode_params(
    DriverData* driver_data, Surface& surface, const VAPictureParameterBufferHEVC* picture, RefMap& refs,
    v4l2_ctrl_hevc_decode_params* params)
{
    *params = {};
    params->pic_order_cnt_val = picture->CurrPic.pic_order_cnt;
    // The NAL type is the authoritative signal; VA's RapPicFlag/IdrPicFlag are
    // optional hints and are not always filled in.
    unsigned type = 0;
    if (!surface.h265_slice_offsets.empty()) {
        const uint8_t* first = surface.source_buffer->get().mapping()[0].data() + surface.h265_slice_offsets[0];
        type = nal_unit_type(first);
    }
    params->flags = ((type >= 16 && type <= 23) ? V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC : 0)
        | ((type == 19 || type == 20) ? V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC : 0);

    unsigned num_st_before = 0;
    unsigned num_st_after = 0;
    unsigned num_lt = 0;

    for (unsigned i = 0; i < kMaxDpb; i++) {
        const VAPictureHEVC& ref = picture->ReferenceFrames[i];
        if (ref.picture_id == VA_INVALID_SURFACE || (ref.flags & VA_PICTURE_HEVC_INVALID))
            break;

        const auto surface_it = driver_data->surfaces.find(ref.picture_id);
        if (surface_it == driver_data->surfaces.end())
            break;

        const unsigned index = params->num_active_dpb_entries++;
        params->dpb[index] = v4l2_hevc_dpb_entry {
            .timestamp = static_cast<uint64_t>(v4l2_timeval_to_ns(&surface_it->second.timestamp)),
            .flags = static_cast<uint8_t>((ref.flags & VA_PICTURE_HEVC_LONG_TERM_REFERENCE)
                    ? V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE
                    : 0),
            .field_pic = static_cast<uint8_t>((ref.flags & VA_PICTURE_HEVC_FIELD_PIC) ? 1 : 0),
            .reserved = 0,
            .pic_order_cnt_val = ref.pic_order_cnt,
        };
        refs.count++;

        if ((ref.flags & VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE) && num_st_before < kMaxDpb)
            params->poc_st_curr_before[num_st_before++] = static_cast<uint8_t>(index);
        if ((ref.flags & VA_PICTURE_HEVC_RPS_ST_CURR_AFTER) && num_st_after < kMaxDpb)
            params->poc_st_curr_after[num_st_after++] = static_cast<uint8_t>(index);
        if ((ref.flags & VA_PICTURE_HEVC_RPS_LT_CURR) && num_lt < kMaxDpb)
            params->poc_lt_curr[num_lt++] = static_cast<uint8_t>(index);
    }

    params->num_poc_st_curr_before = static_cast<uint8_t>(num_st_before);
    params->num_poc_st_curr_after = static_cast<uint8_t>(num_st_after);
    params->num_poc_lt_curr = static_cast<uint8_t>(num_lt);

    (void)surface;
}

void fill_slice_params(const Surface& surface, const VAPictureParameterBufferHEVC* picture, unsigned index,
    const RefMap& refs, v4l2_ctrl_hevc_slice_params* params)
{
    *params = {};

    const auto* slice = surface.h265_slices[index];
    const size_t offset = surface.h265_slice_offsets[std::min<size_t>(index, surface.h265_slice_offsets.size() - 1)];
    const uint8_t* data = surface.source_buffer->get().mapping()[0].data() + offset;

    // rkvdec wants the whole NAL, start code included, and the byte offset of
    // the slice data past the slice header, which VA hands us per slice.
    // Follow FFmpeg's v4l2-request backend: bit_size 0 means "to the end of the
    // buffer", and the byte offset is relative to the start of the NAL, not to
    // the start code we added in front of it.
    params->bit_size = 0;
    params->num_entry_point_offsets = slice->num_entry_point_offsets;
    // VA reports the number of bits the inline short_term_ref_pic_set() takes
    // in this slice header (0 when the slice uses an SPS RPS). rkvdec needs it
    // as the bit offset to the slice data, otherwise it decodes the RPS bits as
    // slice data and the job errors on the first P frame.
    params->short_term_ref_pic_set_size = static_cast<uint16_t>(picture->st_rps_bits);
    params->data_byte_offset = static_cast<uint32_t>(slice->slice_data_offset + slice->slice_data_byte_offset);
    params->nal_unit_type = static_cast<uint8_t>(nal_unit_type(data));
    params->nuh_temporal_id_plus1 = static_cast<uint8_t>(nuh_temporal_id_plus1(data));

    params->slice_type = slice->LongSliceFlags.fields.slice_type;
    params->colour_plane_id = slice->LongSliceFlags.fields.color_plane_id;
    params->slice_pic_order_cnt = picture->CurrPic.pic_order_cnt;
    params->num_ref_idx_l0_active_minus1 = slice->num_ref_idx_l0_active_minus1;
    params->num_ref_idx_l1_active_minus1 = slice->num_ref_idx_l1_active_minus1;
    params->collocated_ref_idx = slice->LongSliceFlags.fields.slice_temporal_mvp_enabled_flag
        ? slice->collocated_ref_idx
        : 0;
    params->five_minus_max_num_merge_cand = slice->five_minus_max_num_merge_cand;
    params->slice_qp_delta = slice->slice_qp_delta;
    params->slice_cb_qp_offset = slice->slice_cb_qp_offset;
    params->slice_cr_qp_offset = slice->slice_cr_qp_offset;
    params->slice_beta_offset_div2 = slice->slice_beta_offset_div2;
    params->slice_tc_offset_div2 = slice->slice_tc_offset_div2;
    params->slice_segment_addr = slice->slice_segment_address;
    params->pic_struct = 0;

    params->flags =
        (slice->LongSliceFlags.fields.slice_sao_luma_flag ? V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA : 0)
        | (slice->LongSliceFlags.fields.slice_sao_chroma_flag ? V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA : 0)
        | (slice->LongSliceFlags.fields.slice_temporal_mvp_enabled_flag
                ? V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED
                : 0)
        | (slice->LongSliceFlags.fields.mvd_l1_zero_flag ? V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO : 0)
        | (slice->LongSliceFlags.fields.cabac_init_flag ? V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT : 0)
        | (slice->LongSliceFlags.fields.collocated_from_l0_flag ? V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0 : 0)
        | (slice->LongSliceFlags.fields.slice_deblocking_filter_disabled_flag
                ? V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED
                : 0)
        | (slice->LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag
                ? V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED
                : 0)
        | (slice->LongSliceFlags.fields.dependent_slice_segment_flag
                ? V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT
                : 0);

    // VASliceParameterBufferHEVC::RefPicList[] entries are indices into the
    // picture's ReferenceFrames[], and fill_decode_params() puts those same
    // entries into the V4L2 DPB in order, so the index maps straight through.
    // Treating them as surface IDs (as an earlier revision did) silently
    // rewrote every list to DPB 0.
    const unsigned count_l0 = slice->num_ref_idx_l0_active_minus1 + 1;
    for (unsigned i = 0; i < count_l0 && i < kMaxDpb; i++) {
        const unsigned ref = slice->RefPicList[0][i];
        params->ref_idx_l0[i] = static_cast<uint8_t>(ref < refs.count ? ref : 0xff);
    }

    const unsigned count_l1 = slice->num_ref_idx_l1_active_minus1 + 1;
    for (unsigned i = 0; i < count_l1 && i < kMaxDpb; i++) {
        const unsigned ref = slice->RefPicList[1][i];
        params->ref_idx_l1[i] = static_cast<uint8_t>(ref < refs.count ? ref : 0xff);
    }

    params->pred_weight_table.luma_log2_weight_denom = slice->luma_log2_weight_denom;
    params->pred_weight_table.delta_chroma_log2_weight_denom = slice->delta_chroma_log2_weight_denom;
    for (unsigned i = 0; i < kMaxDpb; i++) {
        params->pred_weight_table.delta_luma_weight_l0[i] = slice->delta_luma_weight_l0[i];
        params->pred_weight_table.luma_offset_l0[i] = slice->luma_offset_l0[i];
        params->pred_weight_table.delta_luma_weight_l1[i] = slice->delta_luma_weight_l1[i];
        params->pred_weight_table.luma_offset_l1[i] = slice->luma_offset_l1[i];
        for (unsigned j = 0; j < 2; j++) {
            params->pred_weight_table.delta_chroma_weight_l0[i][j] = slice->delta_chroma_weight_l0[i][j];
            params->pred_weight_table.chroma_offset_l0[i][j] = slice->ChromaOffsetL0[i][j];
            params->pred_weight_table.delta_chroma_weight_l1[i][j] = slice->delta_chroma_weight_l1[i][j];
            params->pred_weight_table.chroma_offset_l1[i][j] = slice->ChromaOffsetL1[i][j];
        }
    }
}

} // namespace

H265Context::H265Context(DriverData* driver_data, V4L2M2MDevice& device, VAProfile profile, int picture_width,
    int picture_height, std::span<VASurfaceID> surface_ids)
    : Context(driver_data, device, V4L2_PIX_FMT_HEVC_SLICE, picture_width, picture_height, surface_ids, true)
    , profile(profile)
{
    // Device-level controls: rkvdec only does frame-based Annex B, but say so
    // explicitly instead of relying on the default.
    uint32_t decode_mode = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED;
    uint32_t start_code = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B;
    v4l2_ext_control controls[2] = {
        { .id = V4L2_CID_STATELESS_HEVC_DECODE_MODE, .size = sizeof(decode_mode), .ptr = &decode_mode },
        { .id = V4L2_CID_STATELESS_HEVC_START_CODE, .size = sizeof(start_code), .ptr = &start_code },
    };
    try {
        device.set_ext_controls(-1, std::span(controls, 2));
    } catch (std::runtime_error&) {
    }

    prime_sps(picture_width, picture_height, profile == VAProfileHEVCMain10);
    if (!surface_ids.empty())
        initialize_surfaces(surface_ids);
}

void H265Context::prime_sps(int width, int height, bool ten_bit)
{
    // rkvdec derives its capture format from the SPS when that control is
    // applied, and refuses with -EBUSY once the capture queue is busy. Priming
    // it here, before any buffer exists, with the format this context uses
    // makes the per-request SPS match and never triggers a reset.
    v4l2_ctrl_hevc_sps priming = {};
    priming.chroma_format_idc = 1;
    priming.bit_depth_luma_minus8 = ten_bit ? 2 : 0;
    priming.bit_depth_chroma_minus8 = ten_bit ? 2 : 0;
    priming.pic_width_in_luma_samples = static_cast<uint16_t>(width);
    priming.pic_height_in_luma_samples = static_cast<uint16_t>(height);
    v4l2_ext_control prime = {
        .id = V4L2_CID_STATELESS_HEVC_SPS,
        .size = sizeof(priming),
        .ptr = &priming,
    };
    try {
        device.set_ext_controls(-1, std::span(&prime, 1));
    } catch (std::runtime_error& e) {
        fprintf(stderr, "[h265] priming sps: %s\n", e.what());
    }
}

void H265Context::prime_capture(const Surface& surface)
{
    prime_sps(surface.width, surface.height,
        profile == VAProfileHEVCMain10 || surface.format == VA_RT_FORMAT_YUV420_10);
}

VAStatus H265Context::store_buffer(const Buffer& buffer) const
{
    auto& surface = driver_data->surfaces.at(render_surface_id);

    const auto source_data = surface.source_buffer->get().mapping()[0];

    switch (buffer.type) {
    case VAPictureParameterBufferType:
        surface.h265_picture = reinterpret_cast<VAPictureParameterBufferHEVC*>(buffer.data.get());
        surface.h265_iqmatrix = nullptr;
        surface.h265_slices.clear();
        surface.h265_slice_offsets.clear();
        return VA_STATUS_SUCCESS;

    case VAIQMatrixBufferType:
        surface.h265_iqmatrix = reinterpret_cast<VAIQMatrixBufferHEVC*>(buffer.data.get());
        return VA_STATUS_SUCCESS;

    case VASliceParameterBufferType:
        surface.h265_slices.push_back(reinterpret_cast<VASliceParameterBufferHEVC*>(buffer.data.get()));
        return VA_STATUS_SUCCESS;

    case VASliceDataBufferType: {
        // rkvdec decodes in FRAME_BASED mode with Annex B start codes: every
        // NAL unit is prefixed and the whole frame is one V4L2 buffer.
        static const uint8_t start_code[3] = { 0, 0, 1 };
        const size_t size = buffer.size * buffer.count;
        if (surface.source_size_used + sizeof(start_code) + size > source_data.size())
            return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;

        memcpy(source_data.data() + surface.source_size_used, start_code, sizeof(start_code));
        surface.source_size_used += sizeof(start_code);
        const size_t offset = surface.source_size_used;
        memcpy(source_data.data() + surface.source_size_used, buffer.data.get(), size);
        surface.source_size_used += size;
        surface.h265_slice_offsets.push_back(offset);
        return VA_STATUS_SUCCESS;
    }

    default:
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
}

int H265Context::set_controls()
{
    auto& surface = driver_data->surfaces.at(render_surface_id);

    if (surface.h265_picture == nullptr || surface.h265_slices.empty())
        return VA_STATUS_ERROR_OPERATION_FAILED;

    const auto* picture = surface.h265_picture;

    v4l2_ctrl_hevc_sps sps;
    v4l2_ctrl_hevc_pps pps;
    v4l2_ctrl_hevc_decode_params decode_params;
    v4l2_ctrl_hevc_scaling_matrix scaling;
    RefMap refs;
    std::vector<v4l2_ctrl_hevc_slice_params> slices(surface.h265_slices.size());

    fill_sps(picture, &sps);
    fill_pps(picture, &pps);
    fill_decode_params(driver_data, surface, picture, refs, &decode_params);
    scaling = {};
    if (surface.h265_iqmatrix != nullptr) {
        const auto* m = surface.h265_iqmatrix;
        memcpy(scaling.scaling_list_4x4, m->ScalingList4x4, sizeof(scaling.scaling_list_4x4));
        memcpy(scaling.scaling_list_8x8, m->ScalingList8x8, sizeof(scaling.scaling_list_8x8));
        memcpy(scaling.scaling_list_16x16, m->ScalingList16x16, sizeof(scaling.scaling_list_16x16));
        memcpy(scaling.scaling_list_32x32, m->ScalingList32x32, sizeof(scaling.scaling_list_32x32));
        memcpy(scaling.scaling_list_dc_coef_16x16, m->ScalingListDC16x16,
            sizeof(scaling.scaling_list_dc_coef_16x16));
        memcpy(scaling.scaling_list_dc_coef_32x32, m->ScalingListDC32x32,
            sizeof(scaling.scaling_list_dc_coef_32x32));
    }
    for (size_t i = 0; i < surface.h265_slices.size(); i++)
        fill_slice_params(surface, picture, static_cast<unsigned>(i), refs, &slices[i]);

    v4l2_ext_control controls[9] = {};
    unsigned count = 0;
    controls[count].id = V4L2_CID_STATELESS_HEVC_SPS;
    controls[count].size = sizeof(sps);
    controls[count++].ptr = &sps;
    controls[count].id = V4L2_CID_STATELESS_HEVC_PPS;
    controls[count].size = sizeof(pps);
    controls[count++].ptr = &pps;
    controls[count].id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS;
    controls[count].size = sizeof(decode_params);
    controls[count++].ptr = &decode_params;
    controls[count].id = V4L2_CID_STATELESS_HEVC_SCALING_MATRIX;
    controls[count].size = sizeof(scaling);
    controls[count++].ptr = &scaling;
    controls[count].id = V4L2_CID_STATELESS_HEVC_SLICE_PARAMS;
    controls[count].size = static_cast<uint32_t>(sizeof(v4l2_ctrl_hevc_slice_params) * slices.size());
    controls[count++].ptr = slices.data();

    // DECODE_MODE and START_CODE are device-level controls: they are left at
    // the driver's defaults (frame-based, Annex B) instead of being set per
    // request, which the kernel rejects.
    try {
        device.set_ext_controls(surface.request_fd, std::span(controls, count));
    } catch (std::runtime_error& e) {
        fprintf(stderr, "[h265] controls failed: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    return VA_STATUS_SUCCESS;
}

std::set<VAProfile> H265Context::supported_profiles(const V4L2M2MDevice& device)
{
    std::set<VAProfile> profiles;
    if (!device.format_supported(device.output_buf_type, V4L2_PIX_FMT_HEVC_SLICE))
        return profiles;

    profiles.insert(VAProfileHEVCMain);
    // rkvdec 10-bit output is packed NV15; the driver exposes it as P010 and
    // unpacks it in vaGetImage/vaDeriveImage.
    profiles.insert(VAProfileHEVCMain10);
    return profiles;
}
