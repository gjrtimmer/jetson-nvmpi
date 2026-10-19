/*
 * mock_enc_nal.c - canned Annex-B access units of the encoder model (#100).
 *
 * The NALs are NOT decodable video. They carry the right NAL types and the
 * H.264 SPS holds the real picture size, which is all a consumer of the
 * mock encoder (libnvmpi, the FFmpeg wrapper's extradata extraction, the
 * tests, and the decoder model's own scanner) looks at:
 *
 *   H.264  SPS(7) PPS(8) [IDR slice(5) | non-IDR slice(1)]
 *   HEVC   VPS(32) SPS(33) PPS(34) [IDR_W_RADL(19) | CRA(21) | TRAIL_R(1)]
 *
 * Slice payload bytes never contain 00 00 (no start-code emulation) and
 * vary with the frame counter so identical frames are distinguishable.
 * Everything is written with explicit bounds checks against the caller's
 * capacity; the builder allocates nothing.
 */
#include "mock_enc.h"

#include <linux/videodev2.h>
#include <string.h>

#define PIX_H265 v4l2_fourcc('H', '2', '6', '5')   /* NVIDIA fourcc (mock_dec.c) */

#define SLICE_IDR_LEN 96u
#define SLICE_P_LEN   32u

struct bw { uint8_t *b; size_t bits; };            /* MSB-first bit writer   */

static void put_bits(struct bw *w, uint32_t v, unsigned n)
{
    while (n--) {
        if (v >> n & 1)
            w->b[w->bits >> 3] |= (uint8_t)(0x80 >> (w->bits & 7));
        w->bits++;
    }
}

static void put_ue(struct bw *w, uint32_t v)       /* Exp-Golomb             */
{
    unsigned len = 0;

    while (((uint64_t)v + 1) >> (len + 1))
        len++;
    put_bits(w, 0, len);
    put_bits(w, v + 1, len + 1);
}

/* Emit "00 00 00 01 <hdr...>" plus n payload bytes into out; returns the
 * number of bytes written or 0 when they would not fit in room. */
static size_t put_nal(uint8_t *out, size_t room, const uint8_t *hdr,
                      size_t hlen, const uint8_t *body, size_t n)
{
    if (4 + hlen + n > room)
        return 0;
    memcpy(out, "\0\0\0\1", 4);
    memcpy(out + 4, hdr, hlen);
    if (n)
        memcpy(out + 4 + hlen, body, n);
    return 4 + hlen + n;
}

/* Slice body: deterministic, never 00 (so never a start code). */
static size_t slice_body(uint8_t *dst, size_t n, uint32_t seq)
{
    size_t i;

    for (i = 0; i < n; i++)
        dst[i] = (uint8_t)(0x11 + ((i + seq) % 0xe0));
    return n;
}

/* H.264 SPS: baseline, level 4.0, picture size w x h (rounded up to whole
 * macroblocks and cropped back), same layout as the decoder tests' fixture. */
static size_t h264_sps(uint8_t *out, size_t room, uint32_t w, uint32_t h)
{
    uint8_t rbsp[32] = { 0 }, esc[40];
    struct bw bw = { rbsp, 0 };
    uint32_t cw = (w + 15) / 16 * 16, ch = (h + 15) / 16 * 16;
    size_t i, n, o = 0;
    unsigned zeros = 0;
    const uint8_t hdr = 0x67;

    put_bits(&bw, 66, 8);              /* profile_idc: baseline            */
    put_bits(&bw, 0xc0, 8);            /* constraint_set0/1                */
    put_bits(&bw, 40, 8);              /* level_idc 4.0                    */
    put_ue(&bw, 0);                    /* seq_parameter_set_id             */
    put_ue(&bw, 0);                    /* log2_max_frame_num_minus4        */
    put_ue(&bw, 2);                    /* pic_order_cnt_type 2             */
    put_ue(&bw, 1);                    /* max_num_ref_frames               */
    put_bits(&bw, 0, 1);               /* gaps_in_frame_num_allowed        */
    put_ue(&bw, cw / 16 - 1);
    put_ue(&bw, ch / 16 - 1);
    put_bits(&bw, 1, 1);               /* frame_mbs_only_flag              */
    put_bits(&bw, 1, 1);               /* direct_8x8_inference_flag        */
    if (cw != w || ch != h) {
        put_bits(&bw, 1, 1);           /* frame_cropping_flag              */
        put_ue(&bw, 0);
        put_ue(&bw, (cw - w) / 2);
        put_ue(&bw, 0);
        put_ue(&bw, (ch - h) / 2);
    } else {
        put_bits(&bw, 0, 1);
    }
    put_bits(&bw, 1, 1);               /* rbsp_stop_one_bit                */
    n = (bw.bits + 7) / 8;
    /* Emulation prevention (00 00 0x -> 00 00 03 0x), bounded by esc[]. */
    for (i = 0; i < n && o < sizeof(esc) - 1; i++) {
        if (zeros >= 2 && rbsp[i] <= 3) {
            esc[o++] = 3;
            zeros = 0;
        }
        esc[o++] = rbsp[i];
        zeros = rbsp[i] == 0 ? zeros + 1 : 0;
    }
    return put_nal(out, room, &hdr, 1, esc, o);
}

/* HEVC parameter sets: fixed bodies, size-independent (the decoder model
 * does not parse HEVC sizes either). Two-byte NAL headers: type << 1, tid+1. */
static size_t hevc_params(uint8_t *out, size_t room)
{
    static const uint8_t vps_h[2] = { 0x40, 0x01 }, vps_b[] = {
        0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x01, 0x90, 0x01, 0x5d, 0xac, 0x59 };
    static const uint8_t sps_h[2] = { 0x42, 0x01 }, sps_b[] = {
        0x01, 0x01, 0x01, 0x60, 0x01, 0x90, 0x01, 0x03, 0xa0, 0x08, 0x58,
        0xa1, 0x5a, 0x59, 0x25, 0x13, 0xff };
    static const uint8_t pps_h[2] = { 0x44, 0x01 }, pps_b[] = {
        0xc1, 0x72, 0xb4, 0x62, 0x40 };
    size_t o = 0, n;

    if (!(n = put_nal(out, room, vps_h, 2, vps_b, sizeof(vps_b))))
        return 0;
    o += n;
    if (!(n = put_nal(out + o, room - o, sps_h, 2, sps_b, sizeof(sps_b))))
        return 0;
    o += n;
    if (!(n = put_nal(out + o, room - o, pps_h, 2, pps_b, sizeof(pps_b))))
        return 0;
    return o + n;
}

size_t mock_enc_build_au(const struct mock_enc_au *au, uint8_t *out, size_t cap)
{
    uint8_t body[SLICE_IDR_LEN], hdr[2];
    size_t o = 0, n, blen = au->idr ? SLICE_IDR_LEN : SLICE_P_LEN;
    const uint8_t pps264 = 0x68, pps_body[3] = { 0xce, 0x3c, 0x80 };

    if (au->width < 2 || au->height < 2 || au->width > MOCK_ENC_MAX_DIM ||
        au->height > MOCK_ENC_MAX_DIM)
        return 0;
    (void)slice_body(body, blen, au->seq);

    if (au->pixfmt == V4L2_PIX_FMT_H264) {
        if (au->headers) {
            if (!(n = h264_sps(out, cap, au->width, au->height)))
                return 0;
            o = n;
            if (!(n = put_nal(out + o, cap - o, &pps264, 1, pps_body, 3)))
                return 0;
            o += n;
        }
        hdr[0] = au->idr ? 0x65 : 0x41;     /* ref_idc 3/2, type 5 / 1     */
        if (!(n = put_nal(out + o, cap - o, hdr, 1, body, blen)))
            return 0;
        return o + n;
    }
    if (au->pixfmt == PIX_H265 || au->pixfmt == V4L2_PIX_FMT_HEVC) {
        if (au->headers) {
            if (!(o = hevc_params(out, cap)))
                return 0;
        }
        /* IDR_W_RADL 19, CRA 21 (periodic I frame), TRAIL_R 1. */
        hdr[0] = (uint8_t)((au->idr ? 19 : au->key ? 21 : 1) << 1);
        hdr[1] = 0x01;
        if (!(n = put_nal(out + o, cap - o, hdr, 2, body, blen)))
            return 0;
        return o + n;
    }
    return 0;
}
