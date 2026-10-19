/*
 * h264_fixture.h - tiny synthetic H.264 Annex-B access units for the mock
 * decoder tests (header-only, no dependencies).
 *
 * The NAL payloads are NOT decodable video: the mock decoder only looks at
 * NAL types and at the SPS picture size, so a valid SPS plus slice NAL
 * headers with a few payload bytes are enough. Used by the driver-level
 * tests (test_mock_dec.c) and the libnvmpi-level tests (test_api_dec_mock.c).
 */
#ifndef NVMPI_TEST_H264_FIXTURE_H
#define NVMPI_TEST_H264_FIXTURE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct fx_bw {                         /* MSB-first bit writer             */
    uint8_t *b;
    size_t bits;
};

static inline void fx_put(struct fx_bw *w, uint32_t v, unsigned n)
{
    while (n--) {
        if (v >> n & 1)
            w->b[w->bits >> 3] |= (uint8_t)(0x80 >> (w->bits & 7));
        w->bits++;
    }
}

static inline void fx_ue(struct fx_bw *w, uint32_t v)   /* Exp-Golomb */
{
    unsigned len = 0;

    while (((uint64_t)v + 1) >> (len + 1))
        len++;
    fx_put(w, 0, len);
    fx_put(w, v + 1, len + 1);
}

/* Copy src to dst inserting emulation-prevention bytes; returns new length. */
static inline size_t fx_escape(uint8_t *dst, const uint8_t *src, size_t n)
{
    size_t i, o = 0;
    unsigned zeros = 0;

    for (i = 0; i < n; i++) {
        if (zeros >= 2 && src[i] <= 3) {
            dst[o++] = 3;
            zeros = 0;
        }
        dst[o++] = src[i];
        zeros = src[i] == 0 ? zeros + 1 : 0;
    }
    return o;
}

/* Append a baseline-profile SPS NAL (with 4-byte start code) for a w x h
 * picture (both even); the coded size is rounded up to 16 and cropped back.
 * Returns the number of bytes written. */
static inline size_t fx_sps(uint8_t *out, uint32_t w, uint32_t h)
{
    uint8_t rbsp[32] = { 0 };
    struct fx_bw bw = { rbsp, 0 };
    uint32_t cw = (w + 15) / 16 * 16, ch = (h + 15) / 16 * 16;
    size_t n;

    fx_put(&bw, 66, 8);                /* profile_idc: baseline            */
    fx_put(&bw, 0xc0, 8);              /* constraint flags                 */
    fx_put(&bw, 40, 8);                /* level_idc                        */
    fx_ue(&bw, 0);                     /* sps id                           */
    fx_ue(&bw, 0);                     /* log2_max_frame_num_minus4        */
    fx_ue(&bw, 2);                     /* pic_order_cnt_type 2             */
    fx_ue(&bw, 1);                     /* max_num_ref_frames               */
    fx_put(&bw, 0, 1);                 /* gaps                             */
    fx_ue(&bw, cw / 16 - 1);
    fx_ue(&bw, ch / 16 - 1);
    fx_put(&bw, 1, 1);                 /* frame_mbs_only                   */
    fx_put(&bw, 1, 1);                 /* direct_8x8_inference             */
    if (cw != w || ch != h) {
        fx_put(&bw, 1, 1);             /* frame_cropping_flag              */
        fx_ue(&bw, 0);
        fx_ue(&bw, (cw - w) / 2);      /* right  (4:2:0 unit = 2 px)       */
        fx_ue(&bw, 0);
        fx_ue(&bw, (ch - h) / 2);      /* bottom                           */
    } else {
        fx_put(&bw, 0, 1);
    }
    fx_put(&bw, 1, 1);                 /* rbsp stop bit                    */
    n = (bw.bits + 7) / 8;
    memcpy(out, "\0\0\0\1\x67", 5);
    return 5 + fx_escape(out + 5, rbsp, n);
}

static inline size_t fx_nal(uint8_t *out, uint8_t hdr, size_t payload)
{
    size_t i;

    memcpy(out, "\0\0\0\1", 4);
    out[4] = hdr;
    for (i = 0; i < payload; i++)
        out[5 + i] = (uint8_t)(0x11 + i);   /* never 00 00 */
    return 5 + payload;
}

/* IDR access unit: SPS + PPS + IDR slice. */
static inline size_t fx_idr(uint8_t *out, uint32_t w, uint32_t h)
{
    size_t n = fx_sps(out, w, h);

    n += fx_nal(out + n, 0x68, 3);     /* PPS */
    return n + fx_nal(out + n, 0x65, 24);
}

/* Non-IDR (P) access unit: one slice. */
static inline size_t fx_pslice(uint8_t *out)
{
    return fx_nal(out, 0x41, 24);
}

#endif /* NVMPI_TEST_H264_FIXTURE_H */
