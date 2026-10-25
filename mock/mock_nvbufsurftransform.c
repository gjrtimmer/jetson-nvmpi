/*
 * mock_nvbufsurftransform.c - behavioural mock of libnvbufsurftransform.so.
 *
 * NvBufSurfTransform is a memcpy (nearest-neighbour scale when the sizes
 * differ) of every plane, serialised by one process-wide mutex. The mutex is
 * deliberate: on real Tegra, concurrent transforms deadlock the driver, and
 * libnvmpi serialises them itself (nvmpi_vic.cpp). Serialising here keeps the
 * mock data-race free; it does not try to reproduce the deadlock.
 *
 * Colour conversion is limited to 8-bit 4:2:0 NV12 <-> I420 (chroma
 * (de)interleave, combinable with scaling; see mock_convert_420). Other
 * format pairs must have matching plane layouts, else XFORM_UNSUPP.
 *
 * Crop: like the real VIC, src_rect / dst_rect are honoured ONLY when the
 * matching transform_flag bit (CROP_SRC / CROP_DST) is set; otherwise the
 * whole surface is used and the rect is ignored. A caller that fills a rect
 * but forgets the flag gets a scaled picture, not a crop (issue #128).
 *
 * Resampling filter: only Nearest (filter 0) is an exact copy at scale 1:1.
 * Every other kernel (Bilinear ... Algo3/Smart) is not the identity on
 * hardware: at 1:1 it bends the chroma planes in the outer columns of the
 * window (measured on Orin: an NV12 -> I420 640x360 copy with Smart came out
 * up to 12 levels off in the outer 40 chroma columns). The mock models this
 * as a blend of the first and last chroma column of the destination window
 * with its inward neighbour (see mock_edge_bend), so a caller that copies
 * with a resampling filter fails a byte-exact check (#158).
 *
 * Strict colour mode: with NVMPI_MOCK_XFORM_STRICT_COLOR=1 a transform whose
 * source and destination carry different colorimetry/range tags fails with
 * XFORM_UNSUPP (and logs both formats). The real VIC converts matrix/range
 * in that case instead of copying, so a decoder that wants bit-exact output
 * must tag its destination like the CAPTURE surface (#163). Off by default:
 * the mock itself never does colour maths.
 *
 * Not modelled yet (decoder/encoder data models, #99/#100): flips, any other colour conversion (10-bit, 4:4:4, RGB), composite/blend,
 * async sync objects.
 *
 * The CUDA/transform headers are not included: only the NvBufSurface layout
 * is needed, and the unsupported entry points take opaque pointers (the C
 * ABI only sees pointers/ints), so the mock does not depend on CUDA headers.
 */
#define _GNU_SOURCE
#include "mock_nvbuf_util.h"
#include "mock_log.h"

#include <pthread.h>

#define MOCK_EXPORT __attribute__((visibility("default")))

/* ABI twin of NvBufSurfTransformConfigParams {compute_mode, gpu_id, stream}.
 * Session params are thread-local in the real library; so they are here. */
struct mock_cfg {
    int   compute_mode;
    int   gpu_id;
    void *cuda_stream;
};
static __thread struct mock_cfg t_cfg;

/* ABI twins of NvBufSurfTransformRect {top, left, width, height} and of
 * NvBufSurfTransformParams {flag, flip, filter, src_rect, dst_rect}. */
struct mock_xrect {
    uint32_t top, left, width, height;
};
struct mock_xparams {
    uint32_t flag;
    int      flip;
    int      filter;
    struct mock_xrect *src_rect;
    struct mock_xrect *dst_rect;
};
#define XFORM_CROP_SRC 1u               /* NVBUFSURF_TRANSFORM_CROP_SRC */
#define XFORM_CROP_DST 2u               /* NVBUFSURF_TRANSFORM_CROP_DST */

#define XFORM_FILTER_NEAREST 0          /* NvBufSurfTransformInter_Nearest */

/* Serialises the memcpy engine (see file comment). */
static pthread_mutex_t g_xform_lock = PTHREAD_MUTEX_INITIALIZER;

/* Return codes are NvBufSurfTransform_Error values: Success = 0, errors < 0.
 * Plain ints keep this file independent of the enum's spelling. */
#define XFORM_OK       0
#define XFORM_BAD      (-1)
#define XFORM_UNSUPP   (-3)

MOCK_EXPORT int NvBufSurfTransformSetSessionParams(void *config)
{
    if (!config)
        return XFORM_BAD;
    memcpy(&t_cfg, config, sizeof(t_cfg));
    return XFORM_OK;
}

MOCK_EXPORT int NvBufSurfTransformGetSessionParams(void *config)
{
    if (!config)
        return XFORM_BAD;
    memcpy(config, &t_cfg, sizeof(t_cfg));
    return XFORM_OK;
}

/* Model of a resampling kernel run at scale 1:1: blend the first and last
 * column of each chroma plane window of dst with the inward neighbour. */
static void mock_edge_bend(const NvBufSurfaceParams *d, const struct mock_rect *dr)
{
    uint32_t pl, y, c, win[4];

    for (pl = 1; pl < d->planeParams.num_planes; pl++) {
        uint32_t bpp = d->planeParams.bytesPerPix[pl];
        int tmp;
        unsigned char *m;

        if (!mock_plane_fits(d, pl) || mock_plane_win(d, pl, dr, win) < 0 ||
            win[2] < 2)
            continue;
        m = mock_plane_acquire(d, pl, &tmp);
        if (!m)
            continue;
        for (y = 0; y < win[3]; y++) {
            unsigned char *row = m + (uint64_t)(win[1] + y) * d->planeParams.pitch[pl] +
                                 (uint64_t)win[0] * bpp;
            unsigned char *last = row + (size_t)(win[2] - 1) * bpp;

            for (c = 0; c < bpp; c++) {
                row[c] = (unsigned char)((row[c] + row[bpp + c]) / 2);
                last[c] = (unsigned char)((last[c] + *(last - bpp + c)) / 2);
            }
        }
        mock_plane_release(d, pl, m, tmp);
    }
}

MOCK_EXPORT int NvBufSurfTransform(NvBufSurface *src, NvBufSurface *dst,
                                   void *params)
{
    uint32_t n, i;
    int rc = XFORM_OK, strict;

    const struct mock_xparams *tp = params;
    if (!src || !dst || !src->surfaceList || !dst->surfaceList)
        return XFORM_BAD;
    n = src->numFilled < dst->batchSize ? src->numFilled : dst->batchSize;
    strict = getenv("NVMPI_MOCK_XFORM_STRICT_COLOR") != NULL &&
             getenv("NVMPI_MOCK_XFORM_STRICT_COLOR")[0] == '1';
    pthread_mutex_lock(&g_xform_lock);
    for (i = 0; i < n && rc == XFORM_OK; i++) {
        int sc = mock_color_class((int)src->surfaceList[i].colorFormat);
        int dc = mock_color_class((int)dst->surfaceList[i].colorFormat);

        if (strict && sc >= 0 && dc >= 0 && sc != dc) {
            MOCK_ERR("NvBufSurfTransform: colour conversion (src format %d, "
                     "dst format %d)", (int)src->surfaceList[i].colorFormat,
                     (int)dst->surfaceList[i].colorFormat);
            rc = XFORM_UNSUPP;
            break;
        }
        /* Rect lists are indexed per batch entry, like the real API. */
        struct mock_rect sr, dr;
        const struct mock_rect *psr = NULL, *pdr = NULL;

        if (tp && (tp->flag & XFORM_CROP_SRC) && tp->src_rect) {
            sr = (struct mock_rect){tp->src_rect[i].left, tp->src_rect[i].top,
                                    tp->src_rect[i].width, tp->src_rect[i].height};
            psr = &sr;
        }
        if (tp && (tp->flag & XFORM_CROP_DST) && tp->dst_rect) {
            dr = (struct mock_rect){tp->dst_rect[i].left, tp->dst_rect[i].top,
                                    tp->dst_rect[i].width, tp->dst_rect[i].height};
            pdr = &dr;
        }
        if (mock_copy_rect(&src->surfaceList[i], psr, &dst->surfaceList[i],
                           pdr) < 0) {
            rc = XFORM_UNSUPP;
            continue;
        }
        /* Resampling kernel at scale 1:1 (see file comment). */
        if (tp && tp->filter != XFORM_FILTER_NEAREST &&
            (psr ? psr->w : src->surfaceList[i].width) ==
                (pdr ? pdr->w : dst->surfaceList[i].width) &&
            (psr ? psr->h : src->surfaceList[i].height) ==
                (pdr ? pdr->h : dst->surfaceList[i].height))
            mock_edge_bend(&dst->surfaceList[i], pdr);
    }
    pthread_mutex_unlock(&g_xform_lock);
    if (rc)
        MOCK_ERR("NvBufSurfTransform: unsupported conversion");
    return rc;
}

/* "Async" completes synchronously; the sync object is a dummy non-NULL. */
MOCK_EXPORT int NvBufSurfTransformAsync(NvBufSurface *src, NvBufSurface *dst,
                                        void *params, void **sync_obj)
{
    int rc = NvBufSurfTransform(src, dst, params);

    if (sync_obj)
        *sync_obj = NULL;
    return rc;
}

MOCK_EXPORT int NvBufSurfTransformSyncObjWait(void *sync_obj, unsigned int ms)
{
    (void)sync_obj; (void)ms;
    return XFORM_OK;
}

MOCK_EXPORT int NvBufSurfTransformSyncObjDestroy(void *sync_obj)
{
    (void)sync_obj;
    return XFORM_OK;
}

/* Composite / blend family: not modelled. */
MOCK_EXPORT int NvBufSurfTransformComposite(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; return XFORM_UNSUPP; }
MOCK_EXPORT int NvBufSurfTransformCompositeAsync(void *a, void *b, void *c,
                                                 void *d)
{ (void)a; (void)b; (void)c; (void)d; return XFORM_UNSUPP; }
MOCK_EXPORT int NvBufSurfTransformCompositeBlend(void *a, void *b, void *c,
                                                 void *d, void *e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return XFORM_UNSUPP; }
MOCK_EXPORT int NvBufSurfTransformMultiInputBufCompositeBlend(void *a, void *b,
                                                              void *c, void *d)
{ (void)a; (void)b; (void)c; (void)d; return XFORM_UNSUPP; }
MOCK_EXPORT int NvBufSurfTransformMultiInputBufCompositeBlendAsync(
    void *a, void *b, void *c, void *d, void *e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return XFORM_UNSUPP; }
