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
        } else if (type == 6) {
            /* show_existing_frame (1), frame_type (2): 0 = KEY_FRAME */
            if (!t->n_frame && sz > 0)
                t->key = av1_bits(p + o, sz, 0, 3) == 0;
            t->n_frame++;
        }
        o += sz;
    }
}

#endif /* NVMPI_TEST_AV1_OBU_H */
