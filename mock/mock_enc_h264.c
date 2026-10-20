/*
 * mock_enc_h264.c - decodable H.264 access units of the encoder model
 * (#117): Constrained Baseline, CAVLC, one slice per picture.
 *
 *   key frame  every macroblock is I_PCM (mb_type ue(25), pcm alignment, 256
 *              Y + 64 Cb + 64 Cr raw samples read from the OUTPUT picture,
 *              mock_enc_pic.c), so a decoder reproduces the input exactly.
 *   P frame    a single mb_skip_run over the whole picture: the decoder
 *              copies the previous picture (zero motion), no residual.
 *
 * Stream parameters: SPS profile 66 level 4.0, POC type 2 (output order =
 * decode order, no POC syntax in the slice header), log2_max_frame_num 4
 * (frame_num = frames since the IDR mod 16), one reference frame (sliding
 * window), PPS with deblocking_filter_control_present_flag 1 so each slice
 * can set disable_deblocking_filter_idc 1 (PCM edges stay untouched).
 *
 * Buffers: the slice RBSP is built in the caller's rbsp scratch (the bit
 * writer ORs into it, so the used prefix is zeroed first because the scratch
 * is reused across AUs); the NAL units are then written, with emulation
 * prevention, into out. The two never alias. Everything is bounds-checked;
 * the builder allocates nothing.
 */
#include "mock_enc.h"
#include "mock_enc_bits.h"

#include <string.h>

#define MB_BYTES 384u                  /* 256 Y + 2 * 64 chroma samples     */

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

/* PPS: CAVLC, one slice group, qp 26, deblocking_filter_control_present_flag
 * 1 (the slice header then carries disable_deblocking_filter_idc). */
static size_t h264_pps(uint8_t *out, size_t room)
{
    static const uint8_t hdr = 0x68, body[3] = { 0xce, 0x3c, 0x80 };

    return put_nal(out, room, &hdr, 1, body, sizeof(body));
}

/* slice_header() of a baseline picture. A key frame is an I slice (type 7,
 * all slices of the picture are I), else P (type 5). The slice is always
 * the first of the picture, so first_mb_in_slice is 0. */
static void slice_header(struct bw *bw, const struct mock_enc_au *au)
{
    put_ue(bw, 0);                     /* first_mb_in_slice                */
    put_ue(bw, au->key ? 7 : 5);       /* slice_type                       */
    put_ue(bw, 0);                     /* pic_parameter_set_id             */
    put_bits(bw, au->idr ? 0 : au->poc & 15, 4);   /* frame_num, 4 bits    */
    if (au->idr)
        put_ue(bw, au->seq & 0xffff);  /* idr_pic_id: differs between
                                        * consecutive IDRs (seq counts up) */
    if (!au->key) {
        put_bits(bw, 0, 1);            /* num_ref_idx_active_override_flag */
        put_bits(bw, 0, 1);            /* ref_pic_list_modification_flag_l0 */
    }
    if (au->idr) {                     /* dec_ref_pic_marking()            */
        put_bits(bw, 0, 1);            /* no_output_of_prior_pics_flag     */
        put_bits(bw, 0, 1);            /* long_term_reference_flag         */
    } else {
        put_bits(bw, 0, 1);            /* adaptive_ref_pic_marking_mode    */
    }
    put_se(bw, 0);                     /* slice_qp_delta                   */
    put_ue(bw, 1);                     /* disable_deblocking_filter_idc 1  */
}

/* slice_data() of an I slice: every macroblock I_PCM. The MB's samples are
 * gathered row by row from the input picture (edge replicated for padding
 * macroblocks), then written as one byte-aligned run. */
static void pcm_data(struct bw *bw, const struct mock_enc_pic *pic,
                     uint32_t mbw, uint32_t nmb)
{
    uint8_t mb[MB_BYTES];
    uint32_t m, r;

    for (m = 0; m < nmb && !bw->over; m++) {
        uint32_t x = m % mbw * 16, y = m / mbw * 16;

        for (r = 0; r < 16; r++)
            mock_enc_pic_row(pic, 0, x, y + r, 16, mb + r * 16);
        for (r = 0; r < 8; r++) {
            mock_enc_pic_row(pic, 1, x / 2, y / 2 + r, 8, mb + 256 + r * 8);
            mock_enc_pic_row(pic, 2, x / 2, y / 2 + r, 8, mb + 320 + r * 8);
        }
        put_ue(bw, 25);                /* mb_type I_PCM                    */
        put_align0(bw);                /* pcm_alignment_zero_bits          */
        put_bytes(bw, mb, MB_BYTES);   /* pcm_sample_luma, _chroma         */
    }
}

size_t mock_enc_h264_au(const struct mock_enc_au *au, uint8_t *out, size_t cap,
                        uint8_t *rbsp, size_t rbsp_cap)
{
    uint32_t mbw = (au->width + 15) / 16, mbh = (au->height + 15) / 16;
    uint32_t nmb = mbw * mbh;          /* <= 512^2 (MOCK_ENC_MAX_DIM)       */
    /* Bytes of rbsp this picture can use (see MOCK_ENC_MB_MAX in
     * mock_enc.h); a P slice is just a header and one ue(nmb). */
    size_t want = (au->key ? (size_t)nmb * MOCK_ENC_MB_MAX : 0) +
                  MOCK_ENC_AU_HEADROOM;
    struct bw bw = { rbsp, want, 0, false };
    const uint8_t hdr = au->idr ? 0x65 : 0x41;     /* ref_idc 3/2, type 5/1 */
    size_t o = 0, n;

    if (want > rbsp_cap || !au->pic)
        return 0;
    /* put_bits ORs into the buffer and the scratch is reused across AUs:
     * clear exactly the region this AU may touch. */
    memset(rbsp, 0, want);
    slice_header(&bw, au);
    if (au->key)
        pcm_data(&bw, au->pic, mbw, nmb);
    else
        put_ue(&bw, nmb);              /* mb_skip_run: every macroblock    */
    put_trailing(&bw);                 /* rbsp_slice_trailing_bits         */
    if (!bw_bytes(&bw))
        return 0;

    if (au->headers) {
        if (!(n = h264_sps(out, cap, au->width, au->height, au->tpf_num,
                           au->tpf_den)))
            return 0;
        o = n;
        if (!(n = h264_pps(out + o, cap - o)))
            return 0;
        o += n;
    }
    n = put_nal(out + o, cap - o, &hdr, 1, rbsp, bw_bytes(&bw));
    return n ? o + n : 0;
}
