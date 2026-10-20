/*
 * mock_enc_cabac.c - CABAC arithmetic encoder (see mock_enc_cabac.h).
 *
 * The engine is the informative encoder of H.265 9.3.5 (PutBit with
 * outstanding bits, RenormE, EncodeDecision, EncodeFlush), the mirror of
 * the decoder in 9.3.4.3. Cross-checks against FFmpeg: rangeTabLps and
 * transIdxLps equal the LPS range / mlps state blocks of
 * libavcodec/cabac.c ff_h264_cabac_tables for every state, the context init
 * formula and the init values used by mock_enc_hevc_slice.c are those of
 * libavcodec/hevc/cabac.c (cabac_init_state, init_values[]), and FFmpeg
 * decodes the produced streams with -err_detect explode.
 */
#include "mock_enc_cabac.h"
#include "mock_enc_cabac_tab.h"

void cabac_ctx_init(struct cabac_ctx *c, unsigned init_value, int slice_qp)
{
    int m = (int)(init_value >> 4) * 5 - 45;
    int n = (int)((init_value & 15) << 3) - 16;
    int qp = slice_qp < 0 ? 0 : slice_qp > 51 ? 51 : slice_qp;
    int pre = ((m * qp) >> 4) + n;           /* arithmetic shift per spec */

    pre = pre < 1 ? 1 : pre > 126 ? 126 : pre;
    c->mps = pre > 63;
    c->state = (uint8_t)(c->mps ? pre - 64 : 63 - pre);
}

void cabac_start(struct cabac *c, struct bw *w)
{
    c->w = w;
    c->low = 0;
    c->range = 510;                /* ivlCurrRange init, 9.3.4.3.1         */
    c->outstanding = 0;
    c->first = true;
}

/* PutBit: the very first bit of an engine run is never written (the 10 bit
 * low register starts one bit ahead); a decided bit is followed by the
 * outstanding bits, each the inverse of it. */
static void put_bit(struct cabac *c, unsigned b)
{
    if (c->first)
        c->first = false;
    else
        put_bits(c->w, b, 1);
    for (; c->outstanding; c->outstanding--)
        put_bits(c->w, !b, 1);
}

/* RenormE: shift until range >= 256, resolving carries through the
 * outstanding-bit counter when low is in the undecided band 256..511. */
static void renorm(struct cabac *c)
{
    while (c->range < 256) {
        if (c->low < 256) {
            put_bit(c, 0);
        } else if (c->low >= 512) {
            c->low -= 512;
            put_bit(c, 1);
        } else {
            c->low -= 256;
            c->outstanding++;
        }
        c->range <<= 1;
        c->low <<= 1;
    }
}

void cabac_encode(struct cabac *c, struct cabac_ctx *x, unsigned bin)
{
    uint32_t lps = cabac_lps_tab[x->state][(c->range >> 6) & 3];

    c->range -= lps;
    if (bin != x->mps) {           /* LPS path: take the upper sub-range   */
        c->low += c->range;
        c->range = lps;
        if (x->state == 0)
            x->mps = !x->mps;
        x->state = cabac_trans_lps[x->state];
    } else if (x->state < 62) {
        x->state++;                /* MPS path: transIdxMps                */
    }
    renorm(c);
}

/* EncodeFlush: range 2, renorm (7 shifts), then the 10 bit low is written
 * out; the final '|1' is the stop bit described at cabac_terminate(). */
static void flush(struct cabac *c)
{
    c->range = 2;
    renorm(c);
    put_bit(c, (c->low >> 9) & 1);
    put_bits(c->w, ((c->low >> 7) & 3) | 1, 2);
}

void cabac_terminate(struct cabac *c, unsigned bin)
{
    c->range -= 2;
    if (bin) {
        c->low += c->range;
        flush(c);
    } else {
        renorm(c);
    }
}
