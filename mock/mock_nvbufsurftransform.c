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
 * Not modelled yet (decoder/encoder data models, #99/#100): crop rectangles,
 * flips, any other colour conversion (10-bit, 4:4:4, RGB), composite/blend,
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

MOCK_EXPORT int NvBufSurfTransform(NvBufSurface *src, NvBufSurface *dst,
                                   void *params)
{
    uint32_t n, i;
    int rc = XFORM_OK;

    (void)params;                      /* rects/flags: not modelled yet */
    if (!src || !dst || !src->surfaceList || !dst->surfaceList)
        return XFORM_BAD;
    n = src->numFilled < dst->batchSize ? src->numFilled : dst->batchSize;
    pthread_mutex_lock(&g_xform_lock);
    for (i = 0; i < n && rc == XFORM_OK; i++)
        if (mock_copy_params(&src->surfaceList[i], &dst->surfaceList[i]) < 0)
            rc = XFORM_UNSUPP;
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
