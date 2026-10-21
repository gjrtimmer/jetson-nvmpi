/*
 * nvmpi_av1_obu.h - bounds-checked AV1 OBU scan shared by libnvmpi
 * (nvmpi_decoder_put_packet, #147) and the decoder mock (mock_dec_nal.c), so
 * both judge a packet identically. Header-only, plain C (static inline), no
 * allocation.
 *
 * Why it exists: NVDEC on the Orin (R36.4) HOLDS the OUTPUT buffer of AV1
 * data that precedes the first sequence header and never returns it; after
 * the OUTPUT buffers are used up put_packet blocks forever and the session
 * produces no frames even once a header arrives. libnvmpi therefore drops
 * such data before it reaches the driver.
 *
 * The packet is UNTRUSTED input: every read is checked against n.
 */
#ifndef NVMPI_AV1_OBU_H
#define NVMPI_AV1_OBU_H

#include <stddef.h>
#include <stdint.h>

#define NVMPI_AV1_OBU_SEQ_HDR 1u      /* OBU_SEQUENCE_HEADER                 */

/* Scan one packet (low-overhead format, OBUs normally carry a size field).
 * *has_seq = a sequence header OBU is present; *has_pic = a frame header,
 * tile group or frame OBU is present. A malformed OBU ends the walk: what
 * was found before it counts, nothing after it does. Both outputs are
 * always written. */
static inline void nvmpi_av1_scan(const uint8_t *p, size_t n, int *has_seq,
                                  int *has_pic)
{
    size_t o = 0;

    *has_seq = *has_pic = 0;
    while (p && o < n) {
        uint8_t h = p[o++];
        unsigned type = (h >> 3) & 0x0f, sh = 0;
        size_t sz = 0;

        if (h & 0x80)                 /* forbidden bit set: not an OBU      */
            return;
        if (h & 0x04) {               /* extension byte (temporal/spatial id) */
            if (o >= n)
                return;
            o++;
        }
        if (h & 0x02) {               /* obu_size leb128, 4 bytes at most   */
            for (;;) {
                uint8_t b;

                if (o >= n || sh > 21)
                    return;
                b = p[o++];
                sz |= (size_t)(b & 0x7f) << sh;
                sh += 7;
                if (!(b & 0x80))
                    break;
            }
        } else {
            sz = n - o;               /* no size field: OBU runs to the end */
        }
        if (sz > n - o)               /* payload past the buffer: truncated */
            return;
        if (type == NVMPI_AV1_OBU_SEQ_HDR)
            *has_seq = 1;
        else if (type == 3 || type == 4 || type == 6)
            *has_pic = 1;             /* frame header / tile group / frame  */
        o += sz;
    }
}

#endif /* NVMPI_AV1_OBU_H */
