/*
 * mock_enc_nal.c - Annex-B access unit dispatcher of the encoder model
 * (#100, #117): picks the builder from the CAPTURE pixelformat.
 *
 *   H.264  SPS(7) PPS(8) [IDR slice(5) | non-IDR slice(1)]   mock_enc_h264.c
 *          decodable: I_PCM key frames carrying the input, P_Skip frames
 *   HEVC   VPS(32) SPS(33) PPS(34) [IDR_W_RADL(19) | CRA(21) | TRAIL_R(1)]
 *          decodable (#118): PCM CUs for key frames carrying the input,
 *          skipped CUs for P frames (mock_enc_hevc.c, mock_enc_hevc_slice.c,
 *          mock_enc_cabac.c)
 *
 * The parameter sets hold the real picture size and VUI timing, which is
 * what a consumer of the mock encoder (libnvmpi, the FFmpeg wrapper's
 * extradata extraction, muxers, the tests) looks at.
 */
#include "mock_enc.h"

#include <linux/videodev2.h>

#define PIX_H265 v4l2_fourcc('H', '2', '6', '5')   /* NVIDIA fourcc (mock_dec.c) */

size_t mock_enc_build_au(const struct mock_enc_au *au, uint8_t *out, size_t cap,
                         uint8_t *rbsp, size_t rbsp_cap)
{
    size_t o = 0, n;

    if (au->width < 2 || au->height < 2 || au->width > MOCK_ENC_MAX_DIM ||
        au->height > MOCK_ENC_MAX_DIM)
        return 0;
    if (au->pixfmt == V4L2_PIX_FMT_H264)
        return mock_enc_h264_au(au, out, cap, rbsp, rbsp_cap);
    if (au->pixfmt == PIX_H265 || au->pixfmt == V4L2_PIX_FMT_HEVC) {
        if (au->headers) {
            if (!(o = mock_enc_hevc_params(out, cap, au->width, au->height,
                                              au->tpf_num, au->tpf_den)))
                return 0;
        }
        n = mock_enc_hevc_slice(au, out + o, cap - o, rbsp, rbsp_cap);
        return n ? o + n : 0;
    }
    return 0;
}
