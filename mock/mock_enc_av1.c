/*
 * mock_enc_av1.c - AV1 temporal units of the encoder model (#8).
 *
 * One temporal unit per frame in the low-overhead OBU format (every OBU has
 * obu_has_size_field = 1, no IVF framing - the real encoder is told
 * HEADERS_WITH_FRAME = 0):
 *
 *   temporal delimiter   12 00                         always first
 *   sequence header      0a <size> ...                 when au->headers
 *   OBU_FRAME            32 <size> <frame header> <tile group>
 *
 * The sequence header is real: it carries the OUTPUT picture size and a
 * fixed tool set (profile 0, 8-bit 4:2:0, no order hints, CDEF and loop
 * restoration off, so the frame header needs no tool-specific syntax). The
 * frame header follows the spec for that sequence: a KEY_FRAME when au->key,
 * else an INTER_FRAME, single tile, base_q_idx 100, no filters. The tile
 * data is a fixed filler: a parser accepts the stream (OBU types, sizes,
 * header bit layout), a decoder does not - decodability is proven on
 * hardware only. Nothing here reads the input picture.
 *
 * Bounds: every write goes through the checked bit writer or the room test
 * in put_obu(); the builder allocates nothing and returns 0 on overflow.
 */
#include "mock_enc.h"
#include "mock_enc_bits.h"

#include <string.h>

#define OBU_SEQ_HDR 1u
#define OBU_TD      2u
#define OBU_FRAME   6u
#define TILE_BYTES  32u                /* filler tile data per frame        */
#define HDR_MAX     64u                /* sequence / frame header bytes     */

/* ceil(log2(v)) for v >= 1 (spec tile_log2(1, v)). */
static unsigned tile_log2(uint32_t v)
{
    unsigned k = 0;

    while ((1u << k) < v)
        k++;
    return k;
}

/* OBU: header byte (type, has_size_field), leb128 size, payload. Payloads
 * stay below 16384 bytes here, so the size takes one or two bytes. Returns
 * the bytes written, 0 when they do not fit in room. */
static size_t put_obu(uint8_t *out, size_t room, unsigned type,
                      const uint8_t *pl, size_t n)
{
    size_t o = 0;

    if (n >= 16384u || room < 3 + n)
        return 0;
    out[o++] = (uint8_t)(type << 3 | 0x02);
    if (n < 128u) {
        out[o++] = (uint8_t)n;
    } else {
        out[o++] = (uint8_t)((n & 0x7f) | 0x80);
        out[o++] = (uint8_t)(n >> 7);
    }
    if (n)                             /* TD has no payload (pl NULL)      */
        memcpy(out + o, pl, n);
    return o + n;
}

/* sequence_header_obu() payload; returns bytes, 0 on overflow. */
static size_t seq_hdr(uint8_t *b, size_t cap, uint32_t w, uint32_t h)
{
    struct bw bw = { b, cap, 0, false };

    put_bits(&bw, 0, 3);               /* seq_profile 0 (Main)             */
    put_bits(&bw, 0, 1);               /* still_picture                    */
    put_bits(&bw, 0, 1);               /* reduced_still_picture_header     */
    put_bits(&bw, 0, 1);               /* timing_info_present_flag         */
    put_bits(&bw, 0, 1);               /* initial_display_delay_present    */
    put_bits(&bw, 0, 5);               /* operating_points_cnt_minus_1     */
    put_bits(&bw, 0, 12);              /* operating_point_idc[0]           */
    put_bits(&bw, 8, 5);               /* seq_level_idx[0]: level 4.0      */
    put_bits(&bw, 0, 1);               /* seq_tier[0] (level > 3.3)        */
    put_bits(&bw, 15, 4);              /* frame_width_bits_minus_1  (16)   */
    put_bits(&bw, 15, 4);              /* frame_height_bits_minus_1 (16)   */
    put_bits(&bw, w - 1, 16);          /* max_frame_width_minus_1          */
    put_bits(&bw, h - 1, 16);          /* max_frame_height_minus_1         */
    put_bits(&bw, 0, 1);               /* frame_id_numbers_present_flag    */
    put_bits(&bw, 0, 1);               /* use_128x128_superblock (64x64)   */
    put_bits(&bw, 0, 1);               /* enable_filter_intra              */
    put_bits(&bw, 0, 1);               /* enable_intra_edge_filter         */
    put_bits(&bw, 0, 1);               /* enable_interintra_compound       */
    put_bits(&bw, 0, 1);               /* enable_masked_compound           */
    put_bits(&bw, 0, 1);               /* enable_warped_motion             */
    put_bits(&bw, 0, 1);               /* enable_dual_filter               */
    put_bits(&bw, 0, 1);               /* enable_order_hint (0: no jnt_comp,
                                        * ref_frame_mvs, order_hint_bits)  */
    put_bits(&bw, 1, 1);               /* seq_choose_screen_content_tools  */
    put_bits(&bw, 1, 1);               /* seq_choose_integer_mv            */
    put_bits(&bw, 0, 1);               /* enable_superres                  */
    put_bits(&bw, 0, 1);               /* enable_cdef                      */
    put_bits(&bw, 0, 1);               /* enable_restoration               */
    put_bits(&bw, 0, 1);               /* color: high_bitdepth (8-bit)     */
    put_bits(&bw, 0, 1);               /* mono_chrome                      */
    put_bits(&bw, 0, 1);               /* color_description_present_flag   */
    put_bits(&bw, 0, 1);               /* color_range                      */
    put_bits(&bw, 0, 2);               /* chroma_sample_position           */
    put_bits(&bw, 0, 1);               /* separate_uv_delta_q              */
    put_bits(&bw, 0, 1);               /* film_grain_params_present        */
    put_trailing(&bw);
    return bw_bytes(&bw);
}

/* uncompressed_header() + byte_alignment() of the sequence above. */
static size_t frame_hdr(uint8_t *b, size_t cap, uint32_t w, uint32_t h, bool key)
{
    struct bw bw = { b, cap, 0, false };
    /* MiCols/MiRows in 4x4 units, 64x64 superblocks (16 MI per side). */
    uint32_t sbc = (2 * ((w + 7) >> 3) + 15) >> 4;
    uint32_t sbr = (2 * ((h + 7) >> 3) + 15) >> 4;
    unsigned i;

    put_bits(&bw, 0, 1);               /* show_existing_frame              */
    put_bits(&bw, key ? 0 : 1, 2);     /* frame_type KEY_FRAME / INTER     */
    put_bits(&bw, 1, 1);               /* show_frame                       */
    if (!key)
        put_bits(&bw, 0, 1);           /* error_resilient_mode (forced 1
                                        * and not coded on a shown key)    */
    put_bits(&bw, 0, 1);               /* disable_cdf_update               */
    put_bits(&bw, 0, 1);               /* allow_screen_content_tools       */
    put_bits(&bw, 0, 1);               /* frame_size_override_flag         */
    if (!key) {
        put_bits(&bw, 7, 3);           /* primary_ref_frame: NONE          */
        put_bits(&bw, 0x01, 8);        /* refresh_frame_flags              */
        for (i = 0; i < 7; i++)
            put_bits(&bw, 0, 3);       /* ref_frame_idx[i]                 */
    }
    put_bits(&bw, 0, 1);               /* render_and_frame_size_different  */
    if (!key) {
        put_bits(&bw, 0, 1);           /* allow_high_precision_mv          */
        put_bits(&bw, 1, 1);           /* is_filter_switchable             */
        put_bits(&bw, 0, 1);           /* is_motion_mode_switchable        */
    }
    put_bits(&bw, 0, 1);               /* disable_frame_end_update_cdf     */
    /* tile_info(): uniform spacing, one tile - a 0 "increment" bit ends
     * each column / row loop when more than one superblock is possible. */
    put_bits(&bw, 1, 1);               /* uniform_tile_spacing_flag        */
    if (tile_log2(sbc < 64 ? sbc : 64) > 0)
        put_bits(&bw, 0, 1);           /* increment_tile_cols_log2         */
    if (tile_log2(sbr < 64 ? sbr : 64) > 0)
        put_bits(&bw, 0, 1);           /* increment_tile_rows_log2         */
    put_bits(&bw, 100, 8);             /* base_q_idx                       */
    put_bits(&bw, 0, 3);               /* DeltaQ{Y,U}Dc / DeltaQUAc coded  */
    put_bits(&bw, 0, 1);               /* using_qmatrix                    */
    put_bits(&bw, 0, 1);               /* segmentation_enabled             */
    put_bits(&bw, 0, 1);               /* delta_q_present                  */
    put_bits(&bw, 0, 6);               /* loop_filter_level[0]             */
    put_bits(&bw, 0, 6);               /* loop_filter_level[1]             */
    put_bits(&bw, 0, 3);               /* loop_filter_sharpness            */
    put_bits(&bw, 0, 1);               /* loop_filter_delta_enabled        */
    put_bits(&bw, 0, 1);               /* tx_mode_select                   */
    if (!key)
        put_bits(&bw, 0, 1);           /* reference_select                 */
    put_bits(&bw, 0, 1);               /* reduced_tx_set                   */
    if (!key)
        for (i = 0; i < 7; i++)
            put_bits(&bw, 0, 1);       /* is_global[ref]                   */
    put_align0(&bw);                   /* byte_alignment()                 */
    return bw_bytes(&bw);
}

size_t mock_enc_av1_au(const struct mock_enc_au *au, uint8_t *out, size_t cap)
{
    uint8_t hdr[HDR_MAX] = { 0 }, fr[HDR_MAX + TILE_BYTES];
    size_t o = 0, n;

    if (!au->width || !au->height || au->width > 65536u || au->height > 65536u)
        return 0;
    if (!(n = put_obu(out, cap, OBU_TD, NULL, 0)))
        return 0;
    o += n;
    if (au->headers) {
        n = seq_hdr(hdr, sizeof(hdr), au->width, au->height);
        if (!n || !(n = put_obu(out + o, cap - o, OBU_SEQ_HDR, hdr, n)))
            return 0;
        o += n;
    }
    memset(hdr, 0, sizeof(hdr));
    n = frame_hdr(hdr, sizeof(hdr), au->width, au->height, au->key);
    if (!n)
        return 0;
    memcpy(fr, hdr, n);
    memset(fr + n, 0xA5, TILE_BYTES);  /* tile data: fixed filler          */
    if (!(n = put_obu(out + o, cap - o, OBU_FRAME, fr, n + TILE_BYTES)))
        return 0;
    return o + n;
}
