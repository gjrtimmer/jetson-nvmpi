/*
 * mock_enc_bits.h - bit writer and NAL framing shared by the encoder model's
 * access unit builders (mock_enc_nal.c, mock_enc_hevc.c). Header-only and
 * allocation free; every write is bounds-checked against the caller's buffer.
 */
#ifndef NVMPI_MOCK_ENC_BITS_H
#define NVMPI_MOCK_ENC_BITS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* MSB-first bit writer over a zeroed buffer. A write past cap is dropped and
 * flagged in over, so a caller only needs one check (bw_bytes() == 0). */
struct bw { uint8_t *b; size_t cap; size_t bits; bool over; };

static inline void put_bits(struct bw *w, uint32_t v, unsigned n)
{
    while (n--) {
        if ((w->bits >> 3) >= w->cap) {
            w->over = true;
            return;
        }
        if (v >> n & 1)
            w->b[w->bits >> 3] |= (uint8_t)(0x80 >> (w->bits & 7));
        w->bits++;
    }
}

static inline void put_ue(struct bw *w, uint32_t v)   /* Exp-Golomb ue(v)  */
{
    unsigned len = 0;

    while (((uint64_t)v + 1) >> (len + 1))
        len++;
    put_bits(w, 0, len);
    put_bits(w, v + 1, len + 1);
}

static inline void put_se(struct bw *w, int32_t v)    /* Exp-Golomb se(v)  */
{
    put_ue(w, v > 0 ? 2 * (uint32_t)v - 1 : 2 * (uint32_t)(-(int64_t)v));
}

/* Append n whole bytes. Byte aligned (the common case: slice data follows
 * byte_alignment()) this is one memcpy; otherwise bit by bit. Bounds as for
 * put_bits: what does not fit is dropped and flagged in over. */
static inline void put_bytes(struct bw *w, const uint8_t *p, size_t n)
{
    size_t i;

    if (w->over)
        return;
    if (!(w->bits & 7)) {
        size_t at = w->bits >> 3;

        if (at > w->cap || n > w->cap - at) {
            w->over = true;
            return;
        }
        memcpy(w->b + at, p, n);       /* buffer is zeroed: plain copy    */
        w->bits += n * 8;
        return;
    }
    for (i = 0; i < n && !w->over; i++)
        put_bits(w, p[i], 8);
}

/* rbsp_trailing_bits / byte_alignment: a 1 bit, then zeros to the byte end. */
static inline void put_trailing(struct bw *w)
{
    put_bits(w, 1, 1);
    /* !over: a dropped write does not advance bits, so stop instead of
     * spinning when the buffer is full. */
    while ((w->bits & 7) && !w->over)
        put_bits(w, 0, 1);
}

/* Whole bytes written; 0 when the buffer overflowed. */
static inline size_t bw_bytes(const struct bw *w)
{
    return w->over ? 0 : (w->bits + 7) / 8;
}

/* Emit "00 00 00 01 <hdr...>" plus n payload bytes into out, applying
 * emulation prevention (00 00 0x, x <= 3, becomes 00 00 03 0x) so no
 * payload can fake a start code. Returns the bytes written, 0 when they do
 * not fit in room. */
static inline size_t put_nal(uint8_t *out, size_t room, const uint8_t *hdr,
                             size_t hlen, const uint8_t *body, size_t n)
{
    size_t i, o = 4 + hlen;
    unsigned zeros = 0;

    if (o > room)
        return 0;
    memcpy(out, "\0\0\0\1", 4);
    memcpy(out + 4, hdr, hlen);
    for (i = 0; i < n; i++) {
        if (zeros >= 2 && body[i] <= 3) {
            if (o >= room)
                return 0;
            out[o++] = 3;
            zeros = 0;
        }
        if (o >= room)
            return 0;
        out[o++] = body[i];
        zeros = body[i] == 0 ? zeros + 1 : 0;
    }
    return o;
}

#endif /* NVMPI_MOCK_ENC_BITS_H */
