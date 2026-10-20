/*
 * test_mock_xform.c - NvBufSurfTransform colour conversion of the mock:
 * 8-bit 4:2:0 NV12 <-> I420, byte-exact, round trip and combined with
 * scaling. Surfaces are pitch-padded (pitch 256 > width) so a row-stride
 * bug cannot pass by accident.
 */
#include "mock_test.h"

static NvBufSurface *mk(int cf, uint32_t w, uint32_t h)
{
    NvBufSurfaceCreateParams cp;
    NvBufSurface *s = NULL;

    memset(&cp, 0, sizeof(cp));
    cp.width = w;
    cp.height = h;
    cp.colorFormat = cf;
    cp.layout = NVBUF_LAYOUT_PITCH;
    cp.memType = NVBUF_MEM_SURFACE_ARRAY;
    CHECK(NvBufSurfaceCreate(&s, 1, &cp) == 0 && s);
    return s;
}

/* Deterministic, distinct byte per (plane, component, x, y). */
static unsigned char pat(unsigned pl, unsigned k, unsigned x, unsigned y)
{
    return (unsigned char)(pl * 77 + k * 41 + x * 5 + y * 13 + 1);
}

/* Fill the NV12 surface (UV pairs interleaved, k = 0 U / 1 V). */
static void fill_nv12(NvBufSurface *s)
{
    NvBufSurfaceParams *p = &s->surfaceList[0];
    unsigned x, y, k;

    CHECK(NvBufSurfaceMap(s, 0, -1, NVBUF_MAP_READ_WRITE) == 0);
    for (y = 0; y < p->planeParams.height[0]; y++)
        for (x = 0; x < p->planeParams.width[0]; x++)
            ((unsigned char *)p->mappedAddr.addr[0])
                [y * p->planeParams.pitch[0] + x] = pat(0, 0, x, y);
    for (y = 0; y < p->planeParams.height[1]; y++)
        for (x = 0; x < p->planeParams.width[1]; x++)
            for (k = 0; k < 2; k++)
                ((unsigned char *)p->mappedAddr.addr[1])
                    [y * p->planeParams.pitch[1] + x * 2 + k] =
                        pat(1, k, x, y);
}

/* Expected chroma/luma sample of src grid (sw x sh) at dst (x, y) of a
 * dw x dh plane: nearest neighbour, same rule as the mock. */
#define SRC_X(x, sw, dw) ((x) * (sw) / (dw))

/* Compare an I420 surface against the pattern sampled from a source of
 * src_w x src_h luma (chroma half). */
static void check_i420(NvBufSurface *s, unsigned src_w, unsigned src_h)
{
    NvBufSurfaceParams *p = &s->surfaceList[0];
    unsigned dw = p->planeParams.width[0], dh = p->planeParams.height[0];
    unsigned cw = p->planeParams.width[1], ch = p->planeParams.height[1];
    unsigned x, y, k, bad = 0;

    CHECK(NvBufSurfaceMap(s, 0, -1, NVBUF_MAP_READ) == 0);
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++)
            bad += ((unsigned char *)p->mappedAddr.addr[0])
                       [y * p->planeParams.pitch[0] + x] !=
                   pat(0, 0, SRC_X(x, src_w, dw), SRC_X(y, src_h, dh));
    for (k = 0; k < 2; k++)
        for (y = 0; y < ch; y++)
            for (x = 0; x < cw; x++)
                bad += ((unsigned char *)p->mappedAddr.addr[1 + k])
                           [y * p->planeParams.pitch[1 + k] + x] !=
                       pat(1, k, SRC_X(x, src_w / 2, cw),
                           SRC_X(y, src_h / 2, ch));
    CHECK(bad == 0);
}

/* Compare an NV12 surface's valid area against another NV12 surface. */
static void check_nv12_equal(NvBufSurface *a, NvBufSurface *b)
{
    NvBufSurfaceParams *pa = &a->surfaceList[0], *pb = &b->surfaceList[0];
    unsigned pl, y, bad = 0;

    CHECK(NvBufSurfaceMap(a, 0, -1, NVBUF_MAP_READ) == 0);
    CHECK(NvBufSurfaceMap(b, 0, -1, NVBUF_MAP_READ) == 0);
    for (pl = 0; pl < 2; pl++)
        for (y = 0; y < pa->planeParams.height[pl]; y++)
            bad += memcmp((unsigned char *)pa->mappedAddr.addr[pl] +
                              y * pa->planeParams.pitch[pl],
                          (unsigned char *)pb->mappedAddr.addr[pl] +
                              y * pb->planeParams.pitch[pl],
                          pa->planeParams.width[pl] *
                              pa->planeParams.bytesPerPix[pl]) != 0;
    CHECK(bad == 0);
}

void test_transform_nv12_i420(void)
{
    NvBufSurface *nv = mk(NVBUF_COLOR_FORMAT_NV12, 64, 32);
    NvBufSurface *i4 = mk(NVBUF_COLOR_FORMAT_YUV420, 64, 32);
    NvBufSurface *back = mk(NVBUF_COLOR_FORMAT_NV12, 64, 32);
    NvBufSurface *small = mk(NVBUF_COLOR_FORMAT_YUV420, 32, 16);
    NvBufSurface *bad = mk(NVBUF_COLOR_FORMAT_NV12_10LE, 64, 32);

    if (!nv || !i4 || !back || !small || !bad)
        return;
    CHECK(nv->surfaceList[0].planeParams.pitch[0] > 64);   /* padded rows */
    fill_nv12(nv);

    /* NV12 -> I420 byte-exact, then I420 -> NV12 round trip. */
    CHECK(NvBufSurfTransform(nv, i4, NULL) == 0);
    check_i420(i4, 64, 32);
    CHECK(NvBufSurfTransform(i4, back, NULL) == 0);
    check_nv12_equal(nv, back);

    /* Convert + nearest-neighbour scale in one call: 64x32 -> 32x16. */
    CHECK(NvBufSurfTransform(nv, small, NULL) == 0);
    check_i420(small, 64, 32);

    /* Only 8-bit 4:2:0 is modelled: NV12_10LE <-> I420 is unsupported. */
    CHECK(NvBufSurfTransform(bad, i4, NULL) != 0);
    CHECK(NvBufSurfTransform(i4, bad, NULL) != 0);

    NvBufSurfaceDestroy(nv);
    NvBufSurfaceDestroy(i4);
    NvBufSurfaceDestroy(back);
    NvBufSurfaceDestroy(small);
    NvBufSurfaceDestroy(bad);
}

/* ABI twins of NvBufSurfTransformRect / NvBufSurfTransformParams (the mock
 * and this test stay independent of the transform and CUDA headers). */
struct xrect { uint32_t top, left, width, height; };
struct xparams {
    uint32_t flag;
    int flip, filter;
    struct xrect *src_rect, *dst_rect;
};

/* Count mismatches of dst against src pattern sampled at (x + ox, y + oy)
 * for luma and (x + ox / 2, y + oy / 2) for chroma, or, when scale is set,
 * at the nearest-neighbour position of a src_w x src_h source. */
static unsigned crop_bad(NvBufSurface *d, unsigned ox, unsigned oy,
                         unsigned scale, unsigned src_w, unsigned src_h)
{
    NvBufSurfaceParams *p = &d->surfaceList[0];
    unsigned dw = p->planeParams.width[0], dh = p->planeParams.height[0];
    unsigned x, y, k, bad = 0;
    unsigned char *m0, *m1;

    CHECK(NvBufSurfaceMap(d, 0, -1, NVBUF_MAP_READ) == 0);
    m0 = p->mappedAddr.addr[0];
    m1 = p->mappedAddr.addr[1];
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++) {
            unsigned sx = scale ? x * src_w / dw : x + ox;
            unsigned sy = scale ? y * src_h / dh : y + oy;

            bad += m0[y * p->planeParams.pitch[0] + x] != pat(0, 0, sx, sy);
        }
    for (y = 0; y < p->planeParams.height[1]; y++)
        for (x = 0; x < p->planeParams.width[1]; x++)
            for (k = 0; k < 2; k++) {
                unsigned sx = scale ? x * (src_w / 2) / p->planeParams.width[1]
                                    : x + ox / 2;
                unsigned sy = scale ? y * (src_h / 2) / p->planeParams.height[1]
                                    : y + oy / 2;

                bad += m1[y * p->planeParams.pitch[1] + x * 2 + k] !=
                       pat(1, k, sx, sy);
            }
    return bad;
}

/* Crop windows are honoured only with CROP_SRC / CROP_DST set (like the
 * VIC); a rect without its flag is ignored and the whole surface is scaled. */
void test_transform_crop(void)
{
    NvBufSurface *src = mk(NVBUF_COLOR_FORMAT_NV12, 48, 32);
    NvBufSurface *dst = mk(NVBUF_COLOR_FORMAT_NV12, 32, 16);
    struct xrect sr = { 4, 8, 32, 16 };         /* top, left, w, h */
    struct xrect dr = { 0, 0, 32, 16 };
    struct xparams tp;

    if (!src || !dst)
        return;
    fill_nv12(src);
    memset(&tp, 0, sizeof(tp));
    tp.src_rect = &sr;
    tp.dst_rect = &dr;

    tp.flag = 1;                                /* CROP_SRC */
    CHECK(NvBufSurfTransform(src, dst, &tp) == 0);
    CHECK(crop_bad(dst, 8, 4, 0, 0, 0) == 0);

    tp.flag = 0;                                /* rects set, flags not */
    CHECK(NvBufSurfTransform(src, dst, &tp) == 0);
    CHECK(crop_bad(dst, 0, 0, 1, 48, 32) == 0);

    tp.flag = 1;                                /* window outside src */
    sr.left = 40;
    CHECK(NvBufSurfTransform(src, dst, &tp) != 0);

    NvBufSurfaceDestroy(src);
    NvBufSurfaceDestroy(dst);
}
