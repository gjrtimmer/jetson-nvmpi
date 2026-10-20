/*
 * mock_enc_hevc.c - HEVC parameter sets and slice headers of the encoder
 * model (#108).
 *
 * VPS/SPS/PPS are REAL, spec-conformant syntax (H.265 7.3.2): Main profile,
 * 8-bit 4:2:0, the encoder's actual picture size, so FFmpeg can build hvcC
 * (mp4/mkv) and RTP sprop-* from them. The slice NAL starts with a real
 * slice_segment_header() matching those sets; the slice DATA after it is
 * still canned and not decodable (#110).
 *
 * Coding tools are minimal: CTB 16, min CB 8, no AMP/SAO/PCM/scaling lists,
 * no long-term refs, no temporal MVP, no tiles. VUI is absent unless the
 * INSERT_VUI control is set (#116): then timing_info only.
 */
#include "mock_enc.h"
#include "mock_enc_bits.h"

#define PS_MAX 64u          /* rbsp scratch; the largest set is ~25 bytes  */
#define HDR_MAX 24u         /* slice header scratch                        */

/* Level from luma picture size (H.265 Table A.8, MaxLumaPs). Returns
 * general_level_idc = 30 * level. */
static unsigned hevc_level_idc(uint32_t w, uint32_t h)
{
    uint64_t ps = (uint64_t)w * h;

    if (ps <= 36864)   return 30;      /* 1   */
    if (ps <= 122880)  return 60;      /* 2   */
    if (ps <= 245760)  return 63;      /* 2.1 */
    if (ps <= 552960)  return 90;      /* 3   */
    if (ps <= 983040)  return 93;      /* 3.1 */
    if (ps <= 2228224) return 120;     /* 4   */
    if (ps <= 8912896) return 153;     /* 5.1 */
    return 186;                        /* 6.2 */
}

/* profile_tier_level(1, 0): Main, tier 0, progressive frame-only stream. */
static void put_ptl(struct bw *bw, unsigned level_idc)
{
    put_bits(bw, 0, 2);                /* general_profile_space            */
    put_bits(bw, 0, 1);                /* general_tier_flag                */
    put_bits(bw, 1, 5);                /* general_profile_idc: Main        */
    put_bits(bw, 0x60000000u, 32);     /* compat flags 1 (Main) and 2 (Main10) */
    put_bits(bw, 1, 1);                /* progressive_source_flag          */
    put_bits(bw, 0, 1);                /* interlaced_source_flag           */
    put_bits(bw, 0, 1);                /* non_packed_constraint_flag       */
    put_bits(bw, 1, 1);                /* frame_only_constraint_flag       */
    put_bits(bw, 0, 32);               /* 43 reserved zero bits ...        */
    put_bits(bw, 0, 11);
    put_bits(bw, 0, 1);                /* ... + general_inbld/reserved bit */
    put_bits(bw, level_idc, 8);        /* general_level_idc                */
}

static size_t hevc_vps(uint8_t *out, size_t room, unsigned lvl)
{
    uint8_t rbsp[PS_MAX] = { 0 };
    struct bw bw = { rbsp, sizeof(rbsp), 0, false };
    static const uint8_t hdr[2] = { 32 << 1, 0x01 };   /* type 32, tid 0   */

    put_bits(&bw, 0, 4);               /* vps_video_parameter_set_id       */
    put_bits(&bw, 3, 2);               /* base_layer_internal/available    */
    put_bits(&bw, 0, 6);               /* vps_max_layers_minus1            */
    put_bits(&bw, 0, 3);               /* vps_max_sub_layers_minus1        */
    put_bits(&bw, 1, 1);               /* vps_temporal_id_nesting_flag     */
    put_bits(&bw, 0xffff, 16);         /* vps_reserved_0xffff_16bits       */
    put_ptl(&bw, lvl);
    put_bits(&bw, 1, 1);               /* sub_layer_ordering_info_present  */
    put_ue(&bw, 1);                    /* max_dec_pic_buffering_minus1     */
    put_ue(&bw, 0);                    /* max_num_reorder_pics             */
    put_ue(&bw, 0);                    /* max_latency_increase_plus1       */
    put_bits(&bw, 0, 6);               /* vps_max_layer_id                 */
    put_ue(&bw, 0);                    /* vps_num_layer_sets_minus1        */
    put_bits(&bw, 0, 1);               /* vps_timing_info_present_flag     */
    put_bits(&bw, 0, 1);               /* vps_extension_flag               */
    put_trailing(&bw);
    return bw_bytes(&bw) ? put_nal(out, room, hdr, 2, rbsp, bw_bytes(&bw)) : 0;
}

/* vui_parameters() with only timing_info set (E.2.1): every optional block
 * absent, tick = tpf_num, scale = tpf_den, so the frame rate is
 * scale / tick (HEVC counts one tick per frame, unlike H.264's field ticks). */
static void put_vui(struct bw *bw, uint32_t tick, uint32_t scale)
{
    put_bits(bw, 0, 1);                /* aspect_ratio_info_present_flag   */
    put_bits(bw, 0, 1);                /* overscan_info_present_flag       */
    put_bits(bw, 0, 1);                /* video_signal_type_present_flag   */
    put_bits(bw, 0, 1);                /* chroma_loc_info_present_flag     */
    put_bits(bw, 0, 1);                /* neutral_chroma_indication_flag   */
    put_bits(bw, 0, 1);                /* field_seq_flag                   */
    put_bits(bw, 0, 1);                /* frame_field_info_present_flag    */
    put_bits(bw, 0, 1);                /* default_display_window_flag      */
    put_bits(bw, 1, 1);                /* vui_timing_info_present_flag     */
    put_bits(bw, tick, 32);            /* vui_num_units_in_tick            */
    put_bits(bw, scale, 32);           /* vui_time_scale                   */
    put_bits(bw, 0, 1);                /* vui_poc_proportional_to_timing   */
    put_bits(bw, 0, 1);                /* vui_hrd_parameters_present_flag  */
    put_bits(bw, 0, 1);                /* bitstream_restriction_flag       */
}

static size_t hevc_sps(uint8_t *out, size_t room, uint32_t w, uint32_t h,
                       unsigned lvl, uint32_t tick, uint32_t scale)
{
    uint8_t rbsp[PS_MAX] = { 0 };
    struct bw bw = { rbsp, sizeof(rbsp), 0, false };
    static const uint8_t hdr[2] = { 33 << 1, 0x01 };   /* type 33, tid 0   */
    /* MinCbSizeY is 8: code a size rounded up to it and crop back with the
     * conformance window (offsets are in chroma units, SubWidthC = 2). */
    uint32_t pw = (w + 7) & ~7u, ph = (h + 7) & ~7u;

    put_bits(&bw, 0, 4);               /* sps_video_parameter_set_id       */
    put_bits(&bw, 0, 3);               /* sps_max_sub_layers_minus1        */
    put_bits(&bw, 1, 1);               /* sps_temporal_id_nesting_flag     */
    put_ptl(&bw, lvl);
    put_ue(&bw, 0);                    /* sps_seq_parameter_set_id         */
    put_ue(&bw, 1);                    /* chroma_format_idc: 4:2:0         */
    put_ue(&bw, pw);                   /* pic_width_in_luma_samples        */
    put_ue(&bw, ph);                   /* pic_height_in_luma_samples       */
    if (pw != w || ph != h) {
        put_bits(&bw, 1, 1);           /* conformance_window_flag          */
        put_ue(&bw, 0);                /* left                             */
        put_ue(&bw, (pw - w) / 2);     /* right                            */
        put_ue(&bw, 0);                /* top                              */
        put_ue(&bw, (ph - h) / 2);     /* bottom                           */
    } else {
        put_bits(&bw, 0, 1);
    }
    put_ue(&bw, 0);                    /* bit_depth_luma_minus8            */
    put_ue(&bw, 0);                    /* bit_depth_chroma_minus8          */
    put_ue(&bw, 4);                    /* log2_max_poc_lsb_minus4 (8 bits) */
    put_bits(&bw, 1, 1);               /* sub_layer_ordering_info_present  */
    put_ue(&bw, 1);                    /* max_dec_pic_buffering_minus1     */
    put_ue(&bw, 0);                    /* max_num_reorder_pics             */
    put_ue(&bw, 0);                    /* max_latency_increase_plus1       */
    put_ue(&bw, 0);                    /* log2_min_luma_cb_minus3: 8x8     */
    put_ue(&bw, 1);                    /* log2_diff_max_min_cb: CTB 16     */
    put_ue(&bw, 0);                    /* log2_min_luma_tb_minus2: 4x4     */
    put_ue(&bw, 2);                    /* log2_diff_max_min_tb: 16x16      */
    put_ue(&bw, 0);                    /* max_transform_hierarchy_depth_inter */
    put_ue(&bw, 0);                    /* max_transform_hierarchy_depth_intra */
    put_bits(&bw, 0, 1);               /* scaling_list_enabled_flag        */
    put_bits(&bw, 0, 1);               /* amp_enabled_flag                 */
    put_bits(&bw, 0, 1);               /* sample_adaptive_offset_enabled   */
    put_bits(&bw, 0, 1);               /* pcm_enabled_flag                 */
    put_ue(&bw, 0);                    /* num_short_term_ref_pic_sets      */
    put_bits(&bw, 0, 1);               /* long_term_ref_pics_present_flag  */
    put_bits(&bw, 0, 1);               /* sps_temporal_mvp_enabled_flag    */
    put_bits(&bw, 0, 1);               /* strong_intra_smoothing_enabled   */
    put_bits(&bw, scale && tick, 1);   /* vui_parameters_present_flag      */
    if (scale && tick)
        put_vui(&bw, tick, scale);
    put_bits(&bw, 0, 1);               /* sps_extension_present_flag       */
    put_trailing(&bw);
    return bw_bytes(&bw) ? put_nal(out, room, hdr, 2, rbsp, bw_bytes(&bw)) : 0;
}

static size_t hevc_pps(uint8_t *out, size_t room)
{
    uint8_t rbsp[PS_MAX] = { 0 };
    struct bw bw = { rbsp, sizeof(rbsp), 0, false };
    static const uint8_t hdr[2] = { 34 << 1, 0x01 };   /* type 34, tid 0   */

    put_ue(&bw, 0);                    /* pps_pic_parameter_set_id         */
    put_ue(&bw, 0);                    /* pps_seq_parameter_set_id         */
    put_bits(&bw, 0, 1);               /* dependent_slice_segments_enabled */
    put_bits(&bw, 0, 1);               /* output_flag_present_flag         */
    put_bits(&bw, 0, 3);               /* num_extra_slice_header_bits      */
    put_bits(&bw, 0, 1);               /* sign_data_hiding_enabled_flag    */
    put_bits(&bw, 0, 1);               /* cabac_init_present_flag          */
    put_ue(&bw, 0);                    /* num_ref_idx_l0_default_active_minus1 */
    put_ue(&bw, 0);                    /* num_ref_idx_l1_default_active_minus1 */
    put_se(&bw, 0);                    /* init_qp_minus26                  */
    put_bits(&bw, 0, 1);               /* constrained_intra_pred_flag      */
    put_bits(&bw, 0, 1);               /* transform_skip_enabled_flag      */
    put_bits(&bw, 0, 1);               /* cu_qp_delta_enabled_flag         */
    put_se(&bw, 0);                    /* pps_cb_qp_offset                 */
    put_se(&bw, 0);                    /* pps_cr_qp_offset                 */
    put_bits(&bw, 0, 1);               /* pps_slice_chroma_qp_offsets_present */
    put_bits(&bw, 0, 1);               /* weighted_pred_flag               */
    put_bits(&bw, 0, 1);               /* weighted_bipred_flag             */
    put_bits(&bw, 0, 1);               /* transquant_bypass_enabled_flag   */
    put_bits(&bw, 0, 1);               /* tiles_enabled_flag               */
    put_bits(&bw, 0, 1);               /* entropy_coding_sync_enabled_flag */
    put_bits(&bw, 0, 1);               /* pps_loop_filter_across_slices    */
    put_bits(&bw, 0, 1);               /* deblocking_filter_control_present */
    put_bits(&bw, 0, 1);               /* pps_scaling_list_data_present    */
    put_bits(&bw, 0, 1);               /* lists_modification_present_flag  */
    put_ue(&bw, 0);                    /* log2_parallel_merge_level_minus2 */
    put_bits(&bw, 0, 1);               /* slice_segment_header_extension   */
    put_bits(&bw, 0, 1);               /* pps_extension_present_flag       */
    put_trailing(&bw);
    return bw_bytes(&bw) ? put_nal(out, room, hdr, 2, rbsp, bw_bytes(&bw)) : 0;
}

size_t mock_enc_hevc_params(uint8_t *out, size_t room, uint32_t w, uint32_t h,
                            uint32_t tpf_num, uint32_t tpf_den)
{
    unsigned lvl = hevc_level_idc(w, h);
    size_t o = 0, n;

    if (!(n = hevc_vps(out, room, lvl)))
        return 0;
    o += n;
    if (!(n = hevc_sps(out + o, room - o, w, h, lvl, tpf_num, tpf_den)))
        return 0;
    o += n;
    if (!(n = hevc_pps(out + o, room - o)))
        return 0;
    return o + n;
}

/* One slice NAL: slice_segment_header() for the sets above, byte alignment,
 * then the canned body. nal_type: 19 IDR_W_RADL, 21 CRA, 1 TRAIL_R. */
size_t mock_enc_hevc_slice(uint8_t *out, size_t room, unsigned nal_type,
                           uint32_t poc, const uint8_t *body, size_t blen)
{
    uint8_t rbsp[HDR_MAX + MOCK_ENC_SLICE_MAX] = { 0 };
    struct bw bw = { rbsp, sizeof(rbsp), 0, false };
    uint8_t hdr[2] = { (uint8_t)(nal_type << 1), 0x01 };
    bool irap = nal_type >= 16 && nal_type <= 23, idr = nal_type == 19;

    if (blen > MOCK_ENC_SLICE_MAX)
        return 0;
    put_bits(&bw, 1, 1);               /* first_slice_segment_in_pic_flag  */
    if (irap)
        put_bits(&bw, 0, 1);           /* no_output_of_prior_pics_flag     */
    put_ue(&bw, 0);                    /* slice_pic_parameter_set_id       */
    put_ue(&bw, nal_type == 1 ? 1 : 2);/* slice_type: P (1) or I (2)       */
    if (!idr) {
        put_bits(&bw, poc & 0xff, 8);  /* slice_pic_order_cnt_lsb          */
        put_bits(&bw, 0, 1);           /* short_term_ref_pic_set_sps_flag  */
        if (nal_type == 1) {
            put_ue(&bw, 1);            /* num_negative_pics                */
            put_ue(&bw, 0);            /* num_positive_pics                */
            put_ue(&bw, 0);            /* delta_poc_s0_minus1: previous pic */
            put_bits(&bw, 1, 1);       /* used_by_curr_pic_s0_flag         */
        } else {
            put_ue(&bw, 0);            /* num_negative_pics                */
            put_ue(&bw, 0);            /* num_positive_pics                */
        }
    }
    if (nal_type == 1) {
        put_bits(&bw, 0, 1);           /* num_ref_idx_active_override_flag */
        put_ue(&bw, 0);                /* five_minus_max_num_merge_cand    */
    }
    put_se(&bw, 0);                    /* slice_qp_delta                   */
    put_trailing(&bw);                 /* byte_alignment()                 */
    /* Header larger than HDR_MAX cannot happen with the fields above; the
     * check keeps the body from being appended to a runaway header. */
    if (bw.over || (bw.bits >> 3) > HDR_MAX)
        return 0;
    put_bytes(&bw, body, blen);        /* byte aligned: one memcpy         */
    return bw_bytes(&bw) ? put_nal(out, room, hdr, 2, rbsp, bw_bytes(&bw)) : 0;
}
