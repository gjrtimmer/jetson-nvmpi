/*
 * test_mock_h264.c - the decodable H.264 stream of the mock encoder (#117,
 * mock/mock_enc_h264.c + mock_enc_pic.c) at driver level. Frames with a known
 * pattern go in through the OUTPUT plane (MMAP and DMABUF, NV12M and YUV420M);
 * the slices that come out are parsed back: I slices must be all I_PCM with
 * the input's samples, P slices one skip run over the picture, frame_num must
 * count up from the IDR. Part of the test_mock executable; a real decoder
 * run on the same streams is the local ffmpeg validation of #117.
 */
#include "bit_read.h"
#include "mock_enc_harness.h"
#include "pic_fixture.h"

#define P010M v4l2_fourcc('P', 'M', '1', '0')

/* Parse one slice NAL of the AU. want_f < 0: only check that PCM samples are
 * valid (>= 1), not their value (fallback pattern). Returns 0 when the NAL is
 * absent. */
static int check_slice(const uint8_t *au, uint32_t bytes, uint32_t w,
                       uint32_t h, int want_f, unsigned fn, bool key,
                       bool idr)
{
    uint32_t mbw = (w + 15) / 16, nmb = mbw * ((h + 15) / 16), m, i;
    uint8_t *rb = malloc(bytes + 16);
    size_t n;
    struct br r;

    if (!rb)
        return 0;
    n = nal_rbsp(au, bytes, 0, idr ? 5 : 1, rb, bytes + 16);
    r = (struct br){ rb, n, 0 };
    CHECK(n > 0);
    CHECK(get_ue(&r) == 0);                       /* first_mb_in_slice      */
    CHECK(get_ue(&r) == (key ? 7u : 5u));         /* slice_type I / P       */
    CHECK(get_ue(&r) == 0);                       /* pps id                 */
    CHECK(get_bits(&r, 4) == fn);                 /* frame_num              */
    if (idr)
        (void)get_ue(&r);                         /* idr_pic_id             */
    if (!key)
        CHECK(get_bits(&r, 2) == 0);              /* override, reordering   */
    CHECK(get_bits(&r, idr ? 2 : 1) == 0);        /* ref pic marking        */
    CHECK(get_ue(&r) == 0);                       /* slice_qp_delta         */
    CHECK(get_ue(&r) == 1);                       /* deblocking idc 1       */
    if (!key) {
        CHECK(get_ue(&r) == nmb);                 /* mb_skip_run: all MBs   */
    } else {
        for (m = 0; m < nmb && r.pos < n * 8; m++) {
            uint8_t exp[384];
            uint32_t x = m % mbw * 16, y = m / mbw * 16, cw = (w + 1) / 2,
                     ch = (h + 1) / 2, yy, xx;

            CHECK(get_ue(&r) == 25);              /* mb_type I_PCM          */
            r.pos = (r.pos + 7) & ~(size_t)7;     /* pcm_alignment_zero     */
            if (r.pos / 8 + 384 > n)
                break;
            for (i = 0; i < 384; i++) {           /* expected, edge clamped */
                if (i < 256) {
                    xx = x + i % 16 < w ? x + i % 16 : w - 1;
                    yy = y + i / 16 < h ? y + i / 16 : h - 1;
                    exp[i] = smp_y(xx, yy, (uint32_t)want_f);
                } else {
                    unsigned c = i < 320 ? 1 : 2, k = (i - 256) % 64;

                    xx = x / 2 + k % 8 < cw ? x / 2 + k % 8 : cw - 1;
                    yy = y / 2 + k / 8 < ch ? y / 2 + k / 8 : ch - 1;
                    exp[i] = smp_c(c, xx, yy, (uint32_t)want_f);
                }
            }
            if (want_f >= 0)
                CHECK(memcmp(rb + r.pos / 8, exp, 384) == 0);
            else
                for (i = 0; i < 384; i++)
                    CHECK(rb[r.pos / 8 + i] != 0);
            r.pos += 384 * 8;
        }
        CHECK(m == nmb);
    }
    CHECK(get_bits(&r, 1) == 1);                  /* rbsp_stop_one_bit      */
    CHECK(((r.pos + 7) / 8) == n);                /* nothing after it       */
    free(rb);
    return 1;
}

/* Encode `frames` pictures and check each AU. gop/idr: 0 = none. */
static void run(uint32_t fmt, uint32_t w, uint32_t h, unsigned frames,
                void (*pre)(int), unsigned gop, unsigned idr_iv)
{
    struct eh e;
    unsigned ty[8], cnt, i, since = 0;
    uint32_t bytes = 0, idx = 0;
    int fl, slot;

    g_outfmt = fmt;
    CHECK(eh_open(&e, H264, w, h, pre) == 0);
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
        CHECK(check_slice(e.cap[idx], bytes, w, h, (int)i,
                          idr ? 0 : (since - 1) & 15, key, idr));
        CHECK(qcap(&e, idx) == 0);
    }
    eh_close(&e);
    g_outfmt = V4L2_PIX_FMT_NV12M;
}

static void gop3(int fd)
{
    CHECK(ctrl(fd, VIDIOC_S_EXT_CTRLS, V4L2_CID_MPEG_VIDEO_GOP_SIZE, 3) == 0);
    CHECK(ctrl(fd, VIDIOC_S_EXT_CTRLS, CID_IDR_INTERVAL, 6) == 0);
}

/* Formats the model cannot read, and a dma-buf it cannot map: the AU stays
 * valid PCM, filled from the fallback pattern. */
static void test_fallback(void)
{
    struct eh e;
    unsigned ty[8], cnt, k;
    uint32_t bytes = 0, idx = 0, fm[2] = { P010M, V4L2_PIX_FMT_YUV444M };
    int fl, slot;

    for (k = 0; k < 2; k++) {
        g_outfmt = fm[k];
        CHECK(eh_open(&e, H264, 64, 48, NULL) == 0);
        CHECK(eh_slot(&e, &slot) == 0 && eh_put(&e, (unsigned)slot, 4096, 0) == 0);
        fl = eh_get(&e, ty, 8, &cnt, &bytes, NULL, &idx);
        CHECK(fl >= 0 && !(fl & V4L2_BUF_FLAG_ERROR));
        CHECK(check_slice(e.cap[idx], bytes, 64, 48, -1, 0, true, true));
        eh_close(&e);
    }
    g_outfmt = V4L2_PIX_FMT_NV12M;
    g_outmem = V4L2_MEMORY_DMABUF;       /* /dev/null: not a memfd         */
    CHECK(eh_open(&e, H264, 64, 48, NULL) == 0);
    e.dmafd[0] = open("/dev/null", O_RDWR);
    CHECK(e.dmafd[0] >= 0 && eh_slot(&e, &slot) == 0 &&
          eh_put(&e, (unsigned)slot, 4096, 0) == 0);
    fl = eh_get(&e, ty, 8, &cnt, &bytes, NULL, &idx);
    CHECK(fl >= 0 && !(fl & V4L2_BUF_FLAG_ERROR));
    CHECK(check_slice(e.cap[idx], bytes, 64, 48, -1, 0, true, true));
    eh_close(&e);
    close(e.dmafd[0]);
    g_outmem = V4L2_MEMORY_MMAP;
}

/* DMABUF OUTPUT: planes of an NvBufSurface fd (pitch 256-aligned, which
 * differs from the V4L2 bytesperline) must be read at the surface layout. */
static void test_dmabuf(uint32_t fmt, uint32_t w, uint32_t h)
{
    NvBufSurfaceCreateParams cp;
    NvBufSurface *s[EH_N] = { NULL };
    struct eh e;
    unsigned ty[8], cnt, i, p;
    uint32_t bytes = 0, idx = 0;
    int fl, slot;

    memset(&cp, 0, sizeof(cp));
    cp.width = w;
    cp.height = h;
    cp.colorFormat = fmt == V4L2_PIX_FMT_NV12M ? NVBUF_COLOR_FORMAT_NV12
                                               : NVBUF_COLOR_FORMAT_YUV420;
    cp.layout = NVBUF_LAYOUT_PITCH;
    cp.memType = NVBUF_MEM_SURFACE_ARRAY;
    g_outfmt = fmt;
    g_outmem = V4L2_MEMORY_DMABUF;
    CHECK(eh_open(&e, H264, w, h, NULL) == 0);
    for (i = 0; i < EH_N; i++) {
        CHECK(NvBufSurfaceCreate(&s[i], 1, &cp) == 0 && s[i]);
        CHECK(s[i] && NvBufSurfaceMap(s[i], 0, -1, NVBUF_MAP_READ_WRITE) == 0);
        e.dmafd[i] = s[i] ? (int)s[i]->surfaceList[0].bufferDesc : -1;
    }
    for (i = 0; i < 3; i++) {
        struct raw r;
        NvBufSurfaceParams *sp;

        if (eh_slot(&e, &slot) || !s[slot])
            break;
        sp = &s[slot]->surfaceList[0];
        memset(&r, 0, sizeof(r));
        for (p = 0; p < 3; p++) {
            r.p[p] = p < sp->planeParams.num_planes ? sp->mappedAddr.addr[p] : NULL;
            r.stride[p] = sp->planeParams.pitch[p];
        }
        fill(&r, fmt, w, h, i);
        CHECK(eh_put(&e, (unsigned)slot, 4096, i) == 0);
        fl = eh_get(&e, ty, 8, &cnt, &bytes, NULL, &idx);
        CHECK(fl >= 0 && !(fl & V4L2_BUF_FLAG_ERROR));
        if (fl < 0)
            break;
        CHECK(check_slice(e.cap[idx], bytes, w, h, (int)i, i & 15, i == 0,
                          i == 0));
        CHECK(qcap(&e, idx) == 0);
    }
    eh_close(&e);
    for (i = 0; i < EH_N; i++)
        if (s[i])
            CHECK(NvBufSurfaceDestroy(s[i]) == 0);
    g_outfmt = V4L2_PIX_FMT_NV12M;
    g_outmem = V4L2_MEMORY_MMAP;
}

void test_encoder_h264(void)
{
    run(V4L2_PIX_FMT_NV12M, 320, 240, 14, gop3, 3, 6);
    run(V4L2_PIX_FMT_YUV420M, 320, 240, 8, gop3, 3, 6);
    run(V4L2_PIX_FMT_NV12M, 642, 362, 18, NULL, 0, 0);   /* frame_num wraps  */
    run(V4L2_PIX_FMT_YUV420M, 642, 362, 8, gop3, 3, 6);  /* cropped size     */
    run(V4L2_PIX_FMT_NV12M, 34, 18, 4, NULL, 0, 0);      /* partial MBs      */
    run(V4L2_PIX_FMT_NV12M, 1920, 1080, 2, NULL, 0, 0);
    test_fallback();
    test_dmabuf(V4L2_PIX_FMT_NV12M, 320, 240);
    test_dmabuf(V4L2_PIX_FMT_YUV420M, 642, 362);
}
