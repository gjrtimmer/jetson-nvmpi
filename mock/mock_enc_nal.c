/*
 * mock_enc_nal.c - Annex-B access unit dispatcher of the encoder model
 * (#100, #117): picks the builder from the CAPTURE pixelformat.
 *
 *   H.264  SPS(7) PPS(8) [IDR slice(5) | non-IDR slice(1)]   mock_enc_h264.c
 *          decodable: I_PCM key frames carrying the input, P_Skip frames
 *   HEVC   VPS(32) SPS(33) PPS(34) [IDR_W_RADL(19) | CRA(21) | TRAIL_R(1)]
 *          real parameter sets and slice header, canned slice body, NOT
 *          decodable yet (mock_enc_hevc.c, #108, #118)
 *
 * The parameter sets hold the real picture size and VUI timing, which is
 * what a consumer of the mock encoder (libnvmpi, the FFmpeg wrapper's
 * extradata extraction, muxers, the tests) looks at.
 */
#include "mock_enc.h"

#include <linux/videodev2.h>

#define PIX_H265 v4l2_fourcc('H', '2', '6', '5')   /* NVIDIA fourcc (mock_dec.c) */

/* HEVC canned slice body: deterministic, never 00 (so never a start code). */
static void slice_body(uint8_t *dst, size_t n, uint32_t seq)
{
    size_t i;

    for (i = 0; i < n; i++)
        dst[i] = (uint8_t)(0x11 + ((i + seq) % 0xe0));
}

size_t mock_enc_build_au(const struct mock_enc_au *au, uint8_t *out, size_t cap,
                         uint8_t *rbsp, size_t rbsp_cap)
{
    uint8_t body[MOCK_ENC_SLICE_MAX];
    size_t o = 0, n, blen = au->idr ? MOCK_ENC_SLICE_MAX : 32u;

    if (au->width < 2 || au->height < 2 || au->width > MOCK_ENC_MAX_DIM ||
        au->height > MOCK_ENC_MAX_DIM)
        return 0;
    if (au->pixfmt == V4L2_PIX_FMT_H264)
        return mock_enc_h264_au(au, out, cap, rbsp, rbsp_cap);
    if (au->pixfmt == PIX_H265 || au->pixfmt == V4L2_PIX_FMT_HEVC) {
        slice_body(body, blen, au->seq);
        if (au->headers) {
            if (!(o = mock_enc_hevc_params(out, cap, au->width, au->height,
                                              au->tpf_num, au->tpf_den)))
                return 0;
        }
        /* IDR_W_RADL 19, CRA 21 (periodic I frame), TRAIL_R 1. */
        n = mock_enc_hevc_slice(out + o, cap - o,
                                au->idr ? 19 : au->key ? 21 : 1, au->poc,
                                body, blen);
        return n ? o + n : 0;
    }
    return 0;
}
