/*
 * mock_dec_nal.c - minimal Annex-B inspector for the decoder model: finds
 * VCL NAL units and extracts the picture size from an H.264 SPS.
 *
 * The input is attacker-controlled in spirit (a fuzzed or truncated stream
 * must not crash the mock and thereby mask a libnvmpi bug), so:
 *   - every access is checked against the buffer end,
 *   - the SPS is copied (emulation prevention removed) into a fixed 64-byte
 *     RBSP buffer, never into memory sized by the stream,
 *   - Exp-Golomb codes are capped at 32 bits,
 *   - computed sizes are range checked with 64-bit arithmetic.
 * Only what the model needs is parsed. SPS with a scaling matrix are not
 * parsed (the size then falls back to env/default).
 */
#include "mock_dec.h"

#include <string.h>

#define SPS_RBSP_MAX 64               /* enough for any SPS up to the size
                                       * fields we need; longer is clipped  */

struct bits {
    const uint8_t *p;
    size_t nbits, pos;
    bool err;                         /* sticky: read past the end          */
};

static uint32_t get_bits(struct bits *b, unsigned n)
{
    uint32_t v = 0;

    while (n--) {
        if (b->pos >= b->nbits) {
            b->err = true;
            return 0;
        }
        v = (v << 1) | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u);
        b->pos++;
    }
    return v;
}

/* Unsigned Exp-Golomb; > 32 leading zeros is malformed. */
static uint32_t get_ue(struct bits *b)
{
    unsigned zeros = 0;

    while (!b->err && get_bits(b, 1) == 0) {
        if (++zeros > 31) {
            b->err = true;
            return 0;
        }
    }
    if (b->err)
        return 0;
    return (uint32_t)(((uint64_t)1 << zeros) - 1 + get_bits(b, zeros));
}

/* Signed Exp-Golomb: only the bit consumption matters here. */
static void skip_se(struct bits *b) { (void)get_ue(b); }

/* NAL payload (after the 1-byte header) -> RBSP, dropping 00 00 03. */
static size_t unescape(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    size_t i, o = 0;
    unsigned zeros = 0;

    for (i = 0; i < n && o < cap; i++) {
        if (zeros >= 2 && src[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = src[i] == 0 ? zeros + 1 : 0;
        dst[o++] = src[i];
    }
    return o;
}

static bool high_profile(uint32_t idc)
{
    switch (idc) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86:
    case 118: case 128: case 138: case 139: case 134: case 135:
        return true;
    }
    return false;
}

/* Parse an SPS RBSP (no NAL header) for the displayed size. */
static bool parse_sps(const uint8_t *rbsp, size_t n, uint32_t *w, uint32_t *h)
{
    struct bits b = { rbsp, n * 8, 0, false };
    uint32_t profile, chroma = 1, poc_type, i, ncyc, frame_mbs_only;
    uint32_t wmbs, hmap, crop[4] = { 0, 0, 0, 0 };
    uint64_t W, H, cux, cuy;

    profile = get_bits(&b, 8);
    (void)get_bits(&b, 16);           /* constraint flags + level_idc      */
    (void)get_ue(&b);                 /* seq_parameter_set_id              */
    if (high_profile(profile)) {
        chroma = get_ue(&b);
        if (chroma == 3)
            (void)get_bits(&b, 1);    /* separate_colour_plane_flag        */
        (void)get_ue(&b);             /* bit_depth_luma_minus8             */
        (void)get_ue(&b);             /* bit_depth_chroma_minus8           */
        (void)get_bits(&b, 1);        /* qpprime_y_zero_transform_bypass   */
        if (get_bits(&b, 1))          /* seq_scaling_matrix_present_flag   */
            return false;             /* not modelled                      */
    }
    (void)get_ue(&b);                 /* log2_max_frame_num_minus4         */
    poc_type = get_ue(&b);
    if (poc_type == 0) {
        (void)get_ue(&b);             /* log2_max_pic_order_cnt_lsb_minus4 */
    } else if (poc_type == 1) {
        (void)get_bits(&b, 1);        /* delta_pic_order_always_zero_flag  */
        skip_se(&b);
        skip_se(&b);
        ncyc = get_ue(&b);
        if (ncyc > 255)
            return false;             /* spec limit; refuse the rest       */
        for (i = 0; i < ncyc && !b.err; i++)
            skip_se(&b);
    } else if (poc_type != 2) {
        return false;
    }
    (void)get_ue(&b);                 /* max_num_ref_frames                */
    (void)get_bits(&b, 1);            /* gaps_in_frame_num_allowed_flag    */
    wmbs = get_ue(&b);
    hmap = get_ue(&b);
    frame_mbs_only = get_bits(&b, 1);
    if (!frame_mbs_only)
        (void)get_bits(&b, 1);        /* mb_adaptive_frame_field_flag      */
    (void)get_bits(&b, 1);            /* direct_8x8_inference_flag         */
    if (get_bits(&b, 1))              /* frame_cropping_flag               */
        for (i = 0; i < 4; i++)
            crop[i] = get_ue(&b);
    if (b.err)
        return false;

    W = ((uint64_t)wmbs + 1) * 16;
    H = ((uint64_t)hmap + 1) * 16 * (frame_mbs_only ? 1 : 2);
    /* Crop units (H.264 7.4.2.1.1): 4:2:0 and 4:2:2 halve X, 4:2:0 halves
     * Y; field coding doubles the vertical unit. */
    cux = (chroma == 1 || chroma == 2) ? 2 : 1;
    cuy = (chroma == 1 ? 2 : 1) * (frame_mbs_only ? 1 : 2);
    if (W > MOCK_DEC_MAX_DIM * 2ull || H > MOCK_DEC_MAX_DIM * 2ull)
        return false;
    if ((uint64_t)(crop[0]) * cux + (uint64_t)(crop[1]) * cux >= W ||
        (uint64_t)(crop[2]) * cuy + (uint64_t)(crop[3]) * cuy >= H)
        return false;
    W -= ((uint64_t)crop[0] + crop[1]) * cux;
    H -= ((uint64_t)crop[2] + crop[3]) * cuy;
    if (W < MOCK_DEC_MIN_DIM || H < MOCK_DEC_MIN_DIM ||
        W > MOCK_DEC_MAX_DIM || H > MOCK_DEC_MAX_DIM)
        return false;
    *w = (uint32_t)W;
    *h = (uint32_t)H;
    return true;
}

/* Index of the first byte after the next 00 00 01 at or after `from`, or n
 * when none. `*sc` receives the start-code position (n when none). */
static size_t next_start(const uint8_t *p, size_t n, size_t from, size_t *sc)
{
    size_t i;

    for (i = from; i + 3 <= n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            *sc = i;
            return i + 3;
        }
    *sc = n;
    return n;
}

/* Shared walker: calls on_nal for each NAL (payload start, length incl.
 * header byte). */
typedef void (*nal_cb)(const uint8_t *nal, size_t len, struct mock_scan *s);

static void walk(const uint8_t *p, size_t n, struct mock_scan *s, nal_cb cb)
{
    size_t sc, pos, nxt_sc;

    memset(s, 0, sizeof(*s));
    if (!p || n < 4)
        return;
    pos = next_start(p, n, 0, &sc);
    while (pos < n) {
        size_t after = next_start(p, n, pos, &nxt_sc);
        size_t end = nxt_sc;          /* NAL ends where the next start code
                                       * begins (n for the last NAL)       */

        if (end > pos)
            cb(p + pos, end - pos, s);
        pos = after;
    }
}

static void h264_nal(const uint8_t *nal, size_t len, struct mock_scan *s)
{
    unsigned type = nal[0] & 0x1f;

    if (type >= 1 && type <= 5)
        s->has_vcl = true;
    if (type == 7 && !s->has_dims && len > 1) {
        uint8_t rbsp[SPS_RBSP_MAX];
        size_t m = unescape(nal + 1, len - 1, rbsp, sizeof(rbsp));

        s->has_dims = parse_sps(rbsp, m, &s->width, &s->height);
    }
}

static void h265_nal(const uint8_t *nal, size_t len, struct mock_scan *s)
{
    if (len >= 2 && ((nal[0] >> 1) & 0x3f) <= 31)
        s->has_vcl = true;
}

void mock_scan_h264(const uint8_t *p, size_t n, struct mock_scan *out)
{
    walk(p, n, out, h264_nal);
}

void mock_scan_h265(const uint8_t *p, size_t n, struct mock_scan *out)
{
    walk(p, n, out, h265_nal);
}

/* AV1 OBU walk (low-overhead format, one temporal unit per buffer, #147).
 * OBU types: 1 sequence header, 3 frame header, 4 tile group, 6 frame;
 * 2 (temporal delimiter), 5 (metadata), 8 (tile list), 15 (padding) carry no
 * picture. A malformed OBU ends the walk: what was found before it still
 * counts, nothing after it does (a truncated unit must not crash the mock).
 * The picture size is not parsed (env/default, like H.264 without an SPS). */
void mock_scan_av1(const uint8_t *p, size_t n, struct mock_scan *out)
{
    size_t o = 0;

    while (o < n) {
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
        if (type == 1)
            out->has_seq = true;
        else if (type == 3 || type == 4 || type == 6)
            out->has_vcl = true;
        o += sz;
    }
}
