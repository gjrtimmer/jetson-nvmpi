/*
 * av1_obu.h - checked OBU walker for the AV1 encode cases (#8). Header-only,
 * allocation free; the buffer is untrusted, every read is bounds-checked.
 *
 * Understands the low-overhead format libnvmpi promises: every OBU has
 * obu_has_size_field = 1, no extension, no IVF framing. It records which OBU
 * types a temporal unit holds, whether the first OBU is the temporal
 * delimiter, the picture size of a sequence header (width/height are the
 * first fields after the fixed 38-bit prefix when the sequence uses 16-bit
 * size fields, which both the mock and the hardware encoder emit for sizes
 * that fit; see seq_dims()) and whether the frame header is a KEY_FRAME.
 * av1_seq_color() (#146) walks the whole sequence header (timing info,
 * decoder model, operating points, tool flags) to the color_config and
 * reports seq_profile, high_bitdepth and twelve_bit, i.e. the stream bit
 * depth: 8 (high_bitdepth 0), 10 (1) or 12 (profile 2 with twelve_bit).
 */
#ifndef NVMPI_TEST_AV1_OBU_H
#define NVMPI_TEST_AV1_OBU_H

#include <stddef.h>
#include <stdint.h>

struct av1_tu {
    int ok;                    /* every OBU parsed and fits the buffer      */
    int first_td;              /* first OBU is a temporal delimiter         */
    unsigned n_td, n_seq, n_frame;
    int key;                   /* first frame OBU is a shown KEY_FRAME      */
    unsigned w, h;             /* from the sequence header (0 = none)       */
    int color_ok;              /* av1_seq_color() parsed the header         */
    unsigned profile, high_bd, twelve_bit;  /* color_config (valid if color_ok) */
};

static inline unsigned av1_bits(const uint8_t *p, size_t n, size_t at, unsigned c)
{
    unsigned v = 0, i;

    for (i = 0; i < c; i++, at++)
        v = v << 1 | (at >> 3 < n ? (unsigned)(p[at >> 3] >> (7 - (at & 7)) & 1) : 0u);
    return v;
}

/* Sequence header payload -> picture size. Fixed layout up to the size
 * fields: 3+1+1+1+1 bits, operating_points_cnt_minus_1 (5, must be 0),
 * idc (12), level (5) + tier (1 when level > 7), then the two 4-bit
 * *_bits_minus_1 fields. Returns 0 when the layout is not understood. */
static inline int av1_seq_dims(const uint8_t *p, size_t n, unsigned *w, unsigned *h)
{
    size_t at = 7;
    unsigned level, wb, hb;

    /* still_picture, reduced_still_picture_header, timing_info_present and
     * initial_display_delay_present must be 0: otherwise the layout below
     * does not apply (a hardware sequence header may carry timing info). */
    if (n < 10 || av1_bits(p, n, 3, 4) != 0 || av1_bits(p, n, at, 5) != 0)
        return 0;
    at += 5 + 12;
    level = av1_bits(p, n, at, 5);
    at += 5 + (level > 7 ? 1 : 0);
    wb = av1_bits(p, n, at, 4) + 1;
    hb = av1_bits(p, n, at + 4, 4) + 1;
    at += 8;
    if (wb > 16 || hb > 16)
        return 0;
    *w = av1_bits(p, n, at, wb) + 1;
    *h = av1_bits(p, n, at + wb, hb) + 1;
    return 1;
}

/* Cursor over a bit string with an overrun flag: reads past the end return
 * 0 and set bad, so a truncated or hostile header can never read outside the
 * buffer (av1_bits() itself is already bounds-safe). */
struct av1_rd {
    const uint8_t *p;
    size_t n, at;
    int bad;
};

static inline unsigned av1_rd_u(struct av1_rd *r, unsigned c)
{
    unsigned v;

    if (r->at + c > r->n * 8) {
        r->bad = 1;
        r->at = r->n * 8;
        return 0;
    }
    v = av1_bits(r->p, r->n, r->at, c > 32 ? 32 : c);
    r->at += c;
    return v;
}

/* Sequence header payload -> seq_profile / high_bitdepth / twelve_bit.
 * Follows sequence_header_obu() of the AV1 spec field by field. Returns 1
 * when color_config was reached without overrun, else 0 (outputs untouched).
 * A reduced still-picture header has no operating points; it is parsed too. */
static inline int av1_seq_color(const uint8_t *p, size_t n, unsigned *profile,
                                unsigned *high_bd, unsigned *twelve_bit)
{
    struct av1_rd r = { p, n, 0, 0 };
    unsigned prof, reduced, i, ops, dm = 0, dly = 0, ord = 0, hb, tw = 0, v;
    unsigned idd = 0, fid, sct, imv;

    prof = av1_rd_u(&r, 3);
    av1_rd_u(&r, 1);                           /* still_picture            */
    reduced = av1_rd_u(&r, 1);
    if (reduced) {
        av1_rd_u(&r, 5);                       /* seq_level_idx[0]         */
    } else {
        if (av1_rd_u(&r, 1)) {                 /* timing_info_present_flag */
            av1_rd_u(&r, 32);                  /* num_units_in_display_tick */
            av1_rd_u(&r, 32);                  /* time_scale               */
            if (av1_rd_u(&r, 1)) {             /* equal_picture_interval   */
                for (v = 0; v < 32 && !av1_rd_u(&r, 1) && !r.bad; v++)
                    ;                          /* uvlc leadingZeros        */
                if (v < 32)
                    av1_rd_u(&r, v);           /* num_ticks_per_picture    */
            }
            if (av1_rd_u(&r, 1)) {             /* decoder_model_info_present */
                dm = av1_rd_u(&r, 5) + 1;      /* buffer_delay_length      */
                av1_rd_u(&r, 32);              /* num_units_in_decoding_tick */
                av1_rd_u(&r, 10);              /* removal / presentation len */
            }
        }
        idd = av1_rd_u(&r, 1);                 /* initial_display_delay_present */
        ops = av1_rd_u(&r, 5) + 1;
        for (i = 0; i < ops && !r.bad; i++) {
            av1_rd_u(&r, 12);                  /* operating_point_idc      */
            if (av1_rd_u(&r, 5) > 7)           /* seq_level_idx            */
                av1_rd_u(&r, 1);               /* seq_tier                 */
            if (dm && av1_rd_u(&r, 1)) {       /* decoder_model_present    */
                av1_rd_u(&r, dm);              /* decoder_buffer_delay     */
                av1_rd_u(&r, dm);              /* encoder_buffer_delay     */
                av1_rd_u(&r, 1);               /* low_delay_mode_flag      */
            }
            if (idd && av1_rd_u(&r, 1))        /* initial_display_delay_present_for_this_op */
                av1_rd_u(&r, 4);
        }
    }
    {
        unsigned wb = av1_rd_u(&r, 4) + 1, hbits = av1_rd_u(&r, 4) + 1;

        av1_rd_u(&r, wb);                      /* max_frame_width_minus_1  */
        av1_rd_u(&r, hbits);                   /* max_frame_height_minus_1 */
    }
    if (!reduced) {
        fid = av1_rd_u(&r, 1);                 /* frame_id_numbers_present */
        if (fid)
            av1_rd_u(&r, 7);                   /* delta / additional id len */
    }
    av1_rd_u(&r, 3);                           /* 128x128, filter_intra, edge */
    if (!reduced) {
        av1_rd_u(&r, 4);                       /* interintra .. dual_filter */
        if (av1_rd_u(&r, 1)) {                 /* enable_order_hint        */
            av1_rd_u(&r, 2);                   /* jnt_comp, ref_frame_mvs  */
            ord = 1;
        }
        sct = av1_rd_u(&r, 1) ? 2 : av1_rd_u(&r, 1);  /* seq_force_screen_content_tools */
        if (sct > 0) {
            imv = av1_rd_u(&r, 1);             /* seq_choose_integer_mv    */
            if (!imv)
                av1_rd_u(&r, 1);               /* seq_force_integer_mv     */
        }
        if (ord)
            av1_rd_u(&r, 3);                   /* order_hint_bits_minus_1  */
    }
    av1_rd_u(&r, 3);                           /* superres, cdef, restoration */
    hb = av1_rd_u(&r, 1);                      /* high_bitdepth            */
    if (prof == 2 && hb)
        tw = av1_rd_u(&r, 1);                  /* twelve_bit               */
    if (r.bad)
        return 0;
    *profile = prof;
    *high_bd = hb;
    *twelve_bit = tw;
    return 1;
}

static inline void av1_parse_tu(const uint8_t *p, size_t n, struct av1_tu *t)
{
    size_t o = 0;

    *t = (struct av1_tu){ 0 };
    t->ok = n > 0;
    while (o < n && t->ok) {
        unsigned type, sh = 0;
        size_t sz = 0;
        uint8_t h = p[o++];

        /* forbidden bit 0, no extension, has_size_field 1, reserved 0 */
        if ((h & 0x80) || (h & 0x04) || !(h & 0x02) || (h & 0x01)) {
            t->ok = 0;
            break;
        }
        type = (h >> 3) & 0x0f;
        for (;;) {                       /* leb128, at most 4 bytes here   */
            uint8_t b;

            if (o >= n || sh > 21) { t->ok = 0; break; }
            b = p[o++];
            sz |= (size_t)(b & 0x7f) << sh;
            sh += 7;
            if (!(b & 0x80))
                break;
        }
        if (!t->ok || sz > n - o) {
            t->ok = 0;
            break;
        }
        if (type == 2) {
            if (o == 1 + 1)
                t->first_td = 1;
            t->n_td++;
        } else if (type == 1) {
            t->n_seq++;
            if (!t->w)
                (void)av1_seq_dims(p + o, sz, &t->w, &t->h);
            if (!t->color_ok)
                t->color_ok = av1_seq_color(p + o, sz, &t->profile,
                                            &t->high_bd, &t->twelve_bit);
        } else if (type == 6 || type == 3) {
            /* OBU_FRAME, or OBU_FRAME_HEADER followed by OBU_TILE_GROUPs
             * (equally valid; an encoder may split them). Both start with
             * show_existing_frame (1), frame_type (2): 0 = KEY_FRAME */
            if (!t->n_frame && sz > 0)
                t->key = av1_bits(p + o, sz, 0, 3) == 0;
            t->n_frame++;
        }
        o += sz;
    }
}

#endif /* NVMPI_TEST_AV1_OBU_H */
