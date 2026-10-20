/*
 * test_mock_hevc_slice.c - the decodable HEVC slices of the mock encoder
 * (#118, mock/mock_enc_hevc_slice.c + mock_enc_cabac.c) at driver level.
 * Frames with a known pattern go in through the OUTPUT plane; every slice
 * that comes out is decoded with a small CABAC decoder written here from the
 * decoding process of H.265 9.3.4.3 (it shares only the state tables with the
 * encoder): slice header, then per CTB the split flag, per 8x8 CU part_mode +
 * pcm_flag + PCM samples (I slices, compared with the input) or cu_skip_flag
 * (P slices), end_of_slice_segment_flag, and the stop bit. Part of the
 * test_mock executable; a real decoder run on the same streams is the local
 * ffmpeg validation of #118.
 */
#include "bit_read.h"
#include "mock_enc_cabac_tab.h"
#include "mock_enc_harness.h"
#include "pic_fixture.h"

struct cdec { struct br *r; uint32_t range, off; };
struct cctx { unsigned state, mps; };

/* 9.3.2.2 context initialisation, decoder side of the same formula. */
static void cctx_init(struct cctx *c, int init_value, int qp)
{
    int m = (init_value >> 4) * 5 - 45, n = ((init_value & 15) << 3) - 16;
    int pre = ((m * qp) >> 4) + n;

    pre = pre < 1 ? 1 : pre > 126 ? 126 : pre;
    c->mps = pre > 63;
    c->state = c->mps ? pre - 64 : 63 - pre;
}

/* 9.3.2.5 / 9.3.4.3.1: (re)initialise the engine at the reader position. */
static void cdec_start(struct cdec *d)
{
    d->range = 510;
    d->off = get_bits(d->r, 9);
}

static void renorm(struct cdec *d)
{
    while (d->range < 256) {
        d->range <<= 1;
        d->off = d->off << 1 | get_bits(d->r, 1);
    }
}

static unsigned cdec_bin(struct cdec *d, struct cctx *c)   /* 9.3.4.3.2 */
{
    uint32_t lps = cabac_lps_tab[c->state][(d->range >> 6) & 3];
    unsigned bin;

    d->range -= lps;
    if (d->off >= d->range) {                   /* LPS                      */
        bin = !c->mps;
        d->off -= d->range;
        d->range = lps;
        if (c->state == 0)
            c->mps = !c->mps;
        c->state = cabac_trans_lps[c->state];
    } else {
        bin = c->mps;
        if (c->state < 62)
            c->state++;
    }
    renorm(d);
    return bin;
}

static unsigned cdec_term(struct cdec *d)       /* 9.3.4.3.5, no renorm on 1 */
{
    d->range -= 2;
    if (d->off >= d->range)
        return 1;
    renorm(d);
    return 0;
}

/* Decode slice NAL of `type` in the AU. want_f < 0: PCM samples are not
 * compared. poc is the slice_pic_order_cnt_lsb expected in non-IDR slices. */
static int check_hevc_slice(const uint8_t *au, uint32_t bytes, uint32_t w,
                            uint32_t h, int want_f, unsigned poc, bool key,
                            bool idr)
{
    unsigned type = idr ? 19 : key ? 21 : 1, k;
    uint32_t pw = (w + 7) & ~7u, ph = (h + 7) & ~7u, cx, cy, z, i;
    uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    uint8_t *rb = malloc(bytes + 16);
    size_t n;
    struct br r;
    struct cdec d;
    struct cctx split[3], skip[3], part;
    static const int isplit[2][3] = { { 139, 141, 157 }, { 107, 139, 126 } };
    static const int iskip[3] = { 197, 185, 201 };

    if (!rb)
        return 0;
    n = nal_rbsp(au, bytes, 1, type, rb, bytes + 16);
    r = (struct br){ rb, n, 0 };
    CHECK(n > 0);
    CHECK(get_bits(&r, 1) == 1);                  /* first_slice_segment    */
    if (idr || type == 21)
        CHECK(get_bits(&r, 1) == 0);              /* no_output_of_prior     */
    CHECK(get_ue(&r) == 0);                       /* pps id                 */
    CHECK(get_ue(&r) == (key ? 2u : 1u));         /* slice_type I / P       */
    if (!idr) {
        CHECK(get_bits(&r, 8) == (poc & 0xff));   /* slice_pic_order_cnt    */
        CHECK(get_bits(&r, 1) == 0);              /* st_rps_sps_flag        */
        if (key) {
            CHECK(get_ue(&r) == 0 && get_ue(&r) == 0);   /* empty RPS       */
        } else {
            CHECK(get_ue(&r) == 1 && get_ue(&r) == 0);   /* one negative    */
            CHECK(get_ue(&r) == 0 && get_bits(&r, 1) == 1); /* prev, used   */
        }
    }
    if (!key) {
        CHECK(get_bits(&r, 1) == 0);              /* ref idx override       */
        CHECK(get_ue(&r) == 4);                   /* MaxNumMergeCand 1      */
    }
    CHECK(get_ue(&r) == 0);                       /* se(0): slice_qp_delta  */
    CHECK(get_bits(&r, 1) == 1);                  /* byte_alignment         */
    while (r.pos & 7)
        CHECK(get_bits(&r, 1) == 0);

    for (k = 0; k < 3; k++) {
        cctx_init(&split[k], isplit[!key][k], 26);
        cctx_init(&skip[k], iskip[k], 26);
    }
    cctx_init(&part, 184, 26);
    d.r = &r;
    cdec_start(&d);
    for (cy = 0; cy < (ph + 15) / 16 && r.pos < n * 8; cy++)
        for (cx = 0; cx < (pw + 15) / 16; cx++) {
            if (cx * 16 + 16 <= pw && cy * 16 + 16 <= ph)
                CHECK(cdec_bin(&d, &split[(cx > 0) + (cy > 0)]) == 1);
            for (z = 0; z < 4; z++) {
                uint32_t x = cx * 16 + (z & 1) * 8, y = cy * 16 + (z >> 1) * 8;
                uint8_t exp[96];

                if (x >= pw || y >= ph)
                    continue;
                if (!key) {
                    CHECK(cdec_bin(&d, &skip[(x > 0) + (y > 0)]) == 1);
                    continue;
                }
                CHECK(cdec_bin(&d, &part) == 1);  /* part_mode 2Nx2N        */
                CHECK(cdec_term(&d) == 1);        /* pcm_flag               */
                r.pos = (r.pos + 7) & ~(size_t)7; /* pcm_alignment_zero_bit */
                if (r.pos / 8 + 96 > n) {         /* truncated slice        */
                    CHECK(0);
                    free(rb);
                    return 0;
                }
                for (i = 0; i < 96; i++) {        /* edge replicated        */
                    uint32_t j = i % 16, xx, yy;

                    if (i < 64) {
                        xx = x + i % 8 < w ? x + i % 8 : w - 1;
                        yy = y + i / 8 < h ? y + i / 8 : h - 1;
                        exp[i] = smp_y(xx, yy, (uint32_t)want_f);
                    } else {
                        xx = x / 2 + j % 4 < cw ? x / 2 + j % 4 : cw - 1;
                        yy = y / 2 + j / 4 < ch ? y / 2 + j / 4 : ch - 1;
                        exp[i] = smp_c(i < 80 ? 1 : 2, xx, yy, (uint32_t)want_f);
                    }
                }
                if (want_f >= 0)
                    CHECK(memcmp(rb + r.pos / 8, exp, 96) == 0);
                r.pos += 96 * 8;
                cdec_start(&d);
            }
            /* end_of_slice_segment_flag: 1 exactly after the last CTB. */
            CHECK(cdec_term(&d) == (cy == (ph + 15) / 16 - 1 &&
                                    cx == (pw + 15) / 16 - 1));
        }
    /* The flush's last bit is the stop bit; only zero bits follow it. */
    CHECK(r.pos > 0 && (rb[(r.pos - 1) >> 3] >> (7 - ((r.pos - 1) & 7)) & 1));
    while (r.pos & 7)
        CHECK(get_bits(&r, 1) == 0);
    CHECK(r.pos / 8 == n);
    free(rb);
    return 1;
}

static void gop3(int fd)
{
    CHECK(ctrl(fd, VIDIOC_S_EXT_CTRLS, V4L2_CID_MPEG_VIDEO_GOP_SIZE, 3) == 0);
    CHECK(ctrl(fd, VIDIOC_S_EXT_CTRLS, CID_IDR_INTERVAL, 6) == 0);
}

/* Encode `frames` pictures and decode each AU: IDR at 0 and every idr_iv,
 * CRA every gop. P slices carry no samples; they must be all skip. */
static void run(uint32_t fmt, uint32_t w, uint32_t h, unsigned frames,
                void (*pre)(int), unsigned gop, unsigned idr_iv)
{
    struct eh e;
    unsigned ty[8], cnt, i, since = 0;
    uint32_t bytes = 0, idx = 0;
    int fl, slot;

    g_outfmt = fmt;
    CHECK(eh_open(&e, H265, w, h, pre) == 0);
    for (i = 0; i < frames; i++) {
        struct raw r = { { NULL }, { e.ostride[0], e.ostride[1], e.ostride[2] } };
        bool idr = i == 0 || (idr_iv && since >= idr_iv);
        bool key = idr || (gop && i % gop == 0);
        unsigned p;

        if (eh_slot(&e, &slot))
            break;
        for (p = 0; p < 3; p++)
            r.p[p] = e.raw[slot][p];
        fill(&r, fmt, w, h, i);
        CHECK(eh_put(&e, (unsigned)slot, 4096, i) == 0);
        fl = eh_get(&e, ty, 8, &cnt, &bytes, NULL, &idx);
        CHECK(fl >= 0 && !(fl & V4L2_BUF_FLAG_ERROR));
        if (fl < 0)
            break;
        since = idr ? 1 : since + 1;
        CHECK(check_hevc_slice(e.cap[idx], bytes, w, h, (int)i, since - 1, key,
                               idr));
        CHECK(qcap(&e, idx) == 0);
    }
    eh_close(&e);
    g_outfmt = V4L2_PIX_FMT_NV12M;
}

void test_encoder_hevc_slices(void)
{
    run(V4L2_PIX_FMT_NV12M, 320, 240, 14, gop3, 3, 6);
    run(V4L2_PIX_FMT_YUV420M, 320, 240, 8, gop3, 3, 6);
    run(V4L2_PIX_FMT_NV12M, 642, 362, 8, gop3, 3, 6);    /* cropped size   */
    run(V4L2_PIX_FMT_YUV420M, 40, 24, 4, gop3, 3, 6);    /* partial CTBs   */
    run(V4L2_PIX_FMT_NV12M, 1920, 1080, 2, NULL, 0, 0);
}
