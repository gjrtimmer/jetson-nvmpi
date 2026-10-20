/*
 * bit_read.h - minimal readers for the encoder tests: an MSB-first bit reader
 * with Exp-Golomb ue(v), and extraction of a NAL's RBSP (emulation
 * prevention removed) from an Annex-B access unit. Header-only; reads past
 * the end yield zeros, so a short payload fails a CHECK instead of crashing.
 */
#ifndef NVMPI_TEST_BIT_READ_H
#define NVMPI_TEST_BIT_READ_H

#include <stddef.h>
#include <stdint.h>

struct br { const uint8_t *b; size_t n, pos; };   /* MSB-first bit reader */

static uint32_t get_bits(struct br *r, unsigned n)
{
    uint32_t v = 0;

    while (n--) {
        size_t byte = r->pos >> 3;

        v = v << 1 | (byte < r->n ? (r->b[byte] >> (7 - (r->pos & 7)) & 1u) : 0u);
        r->pos++;
    }
    return v;
}

static uint32_t get_ue(struct br *r)
{
    unsigned z = 0;

    while (z < 31 && !get_bits(r, 1))
        z++;
    return (1u << z) - 1 + get_bits(r, z);
}

/* Payload of the first NAL of `type` in the AU, emulation prevention bytes
 * removed, NAL header skipped (2 bytes HEVC, 1 byte H.264); returns the
 * length, 0 when absent. The NAL ends at the next 00 00 00/01, so an
 * unescaped look-alike would truncate it and fail the parse. */
static size_t nal_rbsp(const uint8_t *p, size_t n, int hevc, unsigned type,
                       uint8_t *out, size_t max)
{
    size_t i, s, e, o = 0;
    unsigned zeros = 0;

    for (s = 0; s + 4 < n; s++)
        if (!p[s] && !p[s + 1] && p[s + 2] == 1 &&
            (hevc ? (p[s + 3] >> 1) & 0x3fu : p[s + 3] & 0x1fu) == type)
            break;
    if (s + 4 >= n)
        return 0;
    s += hevc ? 5 : 4;                        /* start code + NAL header    */
    for (e = s; e + 2 < n && !(!p[e] && !p[e + 1] && p[e + 2] <= 1); e++)
        ;
    e = e + 2 < n ? e : n;
    for (i = s; i < e && o < max; i++) {
        if (zeros >= 2 && p[i] == 3) {        /* emulation prevention byte  */
            zeros = 0;
            continue;
        }
        zeros = p[i] == 0 ? zeros + 1 : 0;
        out[o++] = p[i];
    }
    return o;
}

#endif /* NVMPI_TEST_BIT_READ_H */
