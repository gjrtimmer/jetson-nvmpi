/*
 * mock_enc_nal.c - canned Annex-B access units of the encoder model (#100).
 *
 * The slice data is NOT decodable video (#110). The NALs carry the right
 * types, the H.264 SPS and the HEVC VPS/SPS/PPS (mock_enc_hevc.c, real
 * syntax, #108) hold the real picture size and the HEVC slice starts with a
 * matching real slice header, which is what a consumer of the mock encoder
 * (libnvmpi, the FFmpeg wrapper's extradata extraction, muxers, the tests,
 * and the decoder model's own scanner) looks at:
 *
 *   H.264  SPS(7) PPS(8) [IDR slice(5) | non-IDR slice(1)]
 *   HEVC   VPS(32) SPS(33) PPS(34) [IDR_W_RADL(19) | CRA(21) | TRAIL_R(1)]
 *
 * Slice payload bytes never contain 00 (so no start-code emulation) and
 * vary with the frame counter so identical frames are distinguishable.
 * Everything is written with explicit bounds checks against the caller's
 * capacity; the builder allocates nothing.
 */
#include "mock_enc.h"
#include "mock_enc_bits.h"

#include <linux/videodev2.h>
#include <string.h>

#define PIX_H265 v4l2_fourcc('H', '2', '6', '5')   /* NVIDIA fourcc (mock_dec.c) */

#define SLICE_IDR_LEN MOCK_ENC_SLICE_MAX
#define SLICE_P_LEN   32u

/* Slice body: deterministic, never 00 (so never a start code). */
static size_t slice_body(uint8_t *dst, size_t n, uint32_t seq)
{
    size_t i;

    for (i = 0; i < n; i++)
        dst[i] = (uint8_t)(0x11 + ((i + seq) % 0xe0));
    return n;
}

/* H.264 SPS: baseline, level 4.0, picture size w x h (rounded up to whole
 * macroblocks and cropped back), same layout as the decoder tests' fixture. */
static size_t h264_sps(uint8_t *out, size_t room, uint32_t w, uint32_t h,
                       uint32_t tpf_num, uint32_t tpf_den)
{
    uint8_t rbsp[48] = { 0 };
    struct bw bw = { rbsp, sizeof(rbsp), 0, false };
    uint32_t cw = (w + 15) / 16 * 16, ch = (h + 15) / 16 * 16;
    const uint8_t hdr = 0x67;

    put_bits(&bw, 66, 8);              /* profile_idc: baseline            */
    put_bits(&bw, 0xc0, 8);            /* constraint_set0/1                */
    put_bits(&bw, 40, 8);              /* level_idc 4.0                    */
    put_ue(&bw, 0);                    /* seq_parameter_set_id             */
    put_ue(&bw, 0);                    /* log2_max_frame_num_minus4        */
    put_ue(&bw, 2);                    /* pic_order_cnt_type 2             */
    put_ue(&bw, 1);                    /* max_num_ref_frames               */
    put_bits(&bw, 0, 1);               /* gaps_in_frame_num_allowed        */
    put_ue(&bw, cw / 16 - 1);
    put_ue(&bw, ch / 16 - 1);
    put_bits(&bw, 1, 1);               /* frame_mbs_only_flag              */
    put_bits(&bw, 1, 1);               /* direct_8x8_inference_flag        */
    if (cw != w || ch != h) {
        put_bits(&bw, 1, 1);           /* frame_cropping_flag              */
        put_ue(&bw, 0);
        put_ue(&bw, (cw - w) / 2);
        put_ue(&bw, 0);
        put_ue(&bw, (ch - h) / 2);
    } else {
        put_bits(&bw, 0, 1);
    }
    if (tpf_num && tpf_den) {
        /* vui_parameters() with timing_info only (E.1.1). H.264 counts
         * one tick per FIELD, so time_scale is 2 * fps: a decoder reads
         * the frame rate as time_scale / (2 * num_units_in_tick). */
        put_bits(&bw, 1, 1);           /* vui_parameters_present_flag      */
        put_bits(&bw, 0, 1);           /* aspect_ratio_info_present_flag   */
        put_bits(&bw, 0, 1);           /* overscan_info_present_flag       */
        put_bits(&bw, 0, 1);           /* video_signal_type_present_flag   */
        put_bits(&bw, 0, 1);           /* chroma_loc_info_present_flag     */
        put_bits(&bw, 1, 1);           /* timing_info_present_flag         */
        put_bits(&bw, tpf_num, 32);    /* num_units_in_tick                */
        put_bits(&bw, 2 * tpf_den, 32);/* time_scale (S_PARM caps den)     */
        put_bits(&bw, 1, 1);           /* fixed_frame_rate_flag            */
        put_bits(&bw, 0, 1);           /* nal_hrd_parameters_present_flag  */
        put_bits(&bw, 0, 1);           /* vcl_hrd_parameters_present_flag  */
        put_bits(&bw, 0, 1);           /* pic_struct_present_flag          */
        put_bits(&bw, 0, 1);           /* bitstream_restriction_flag       */
    } else {
        put_bits(&bw, 0, 1);           /* vui_parameters_present_flag      */
    }
    put_trailing(&bw);                 /* rbsp_stop_one_bit + alignment    */
    return bw_bytes(&bw) ? put_nal(out, room, &hdr, 1, rbsp, bw_bytes(&bw)) : 0;
}

size_t mock_enc_build_au(const struct mock_enc_au *au, uint8_t *out, size_t cap)
{
    uint8_t body[SLICE_IDR_LEN], hdr;
    size_t o = 0, n, blen = au->idr ? SLICE_IDR_LEN : SLICE_P_LEN;
    const uint8_t pps264 = 0x68, pps_body[3] = { 0xce, 0x3c, 0x80 };

    if (au->width < 2 || au->height < 2 || au->width > MOCK_ENC_MAX_DIM ||
        au->height > MOCK_ENC_MAX_DIM)
        return 0;
    (void)slice_body(body, blen, au->seq);

    if (au->pixfmt == V4L2_PIX_FMT_H264) {
        if (au->headers) {
            if (!(n = h264_sps(out, cap, au->width, au->height,
                                     au->tpf_num, au->tpf_den)))
                return 0;
            o = n;
            if (!(n = put_nal(out + o, cap - o, &pps264, 1, pps_body, 3)))
                return 0;
            o += n;
        }
        hdr = au->idr ? 0x65 : 0x41;     /* ref_idc 3/2, type 5 / 1     */
        if (!(n = put_nal(out + o, cap - o, &hdr, 1, body, blen)))
            return 0;
        return o + n;
    }
    if (au->pixfmt == PIX_H265 || au->pixfmt == V4L2_PIX_FMT_HEVC) {
        if (au->headers) {
            if (!(o = mock_enc_hevc_params(out, cap, au->width, au->height,
                                              au->tpf_num, au->tpf_den)))
                return 0;
        }
        /* IDR_W_RADL 19, CRA 21 (periodic I frame), TRAIL_R 1. */
        n = mock_enc_hevc_slice(out + o, cap - o,
                                au->idr ? 19 : au->key ? 21 : 1, au->poc,
                                body, blen);
        if (!n)
            return 0;
        return o + n;
    }
    return 0;
}
