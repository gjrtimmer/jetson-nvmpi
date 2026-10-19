/*
 * mock_nvbufsurface_ops.c - per-surface operations of the nvbufsurface mock:
 * Map/UnMap, Sync*, MemSet, Copy, NvBufSurface2Raw and the EGL stubs.
 * Split from mock_nvbufsurface.c (allocation + fd registry) to keep each file
 * under 400 LOC. Surface contents are the caller's to synchronise, as with
 * the real library; the memfd is coherent with every mapping, so the Sync
 * calls only validate their arguments.
 */
#define _GNU_SOURCE
#include "mock_nvbuf_util.h"
#include "mock_log.h"

#include <stdlib.h>
#include <unistd.h>

#define MOCK_EXPORT __attribute__((visibility("default")))

/* Common index/plane validation; plane == -1 means "all planes". */
static int check_idx(NvBufSurface *s, int index, int plane, int *first,
                     int *last)
{
    NvBufSurfaceParams *sp;

    if (!s || index < 0 || (uint32_t)index >= s->batchSize)
        return -1;
    sp = &s->surfaceList[index];
    if (plane < -1 || plane >= (int)sp->planeParams.num_planes)
        return -1;
    *first = plane < 0 ? 0 : plane;
    *last = plane < 0 ? (int)sp->planeParams.num_planes - 1 : plane;
    return 0;
}

MOCK_EXPORT int NvBufSurfaceMap(NvBufSurface *surf, int index, int plane,
                                NvBufSurfaceMemMapFlags type)
{
    int pl, first, last, prot = PROT_READ | PROT_WRITE;
    NvBufSurfaceParams *sp;

    if (check_idx(surf, index, plane, &first, &last) < 0)
        return -1;
    sp = &surf->surfaceList[index];
    if (type == NVBUF_MAP_READ)
        prot = PROT_READ;
    else if (type == NVBUF_MAP_WRITE)
        prot = PROT_WRITE;
    for (pl = first; pl <= last; pl++) {
        void *m;

        if (sp->mappedAddr.addr[pl])
            continue;                   /* already mapped: idempotent   */
        m = mmap(NULL, sp->planeParams.psize[pl], prot, MAP_SHARED,
                 (int)sp->bufferDesc, sp->planeParams.offset[pl]);
        if (m == MAP_FAILED)
            return -1;
        sp->mappedAddr.addr[pl] = m;    /* unmapped by UnMap or Destroy */
    }
    return 0;
}

MOCK_EXPORT int NvBufSurfaceUnMap(NvBufSurface *surf, int index, int plane)
{
    int pl, first, last;
    NvBufSurfaceParams *sp;

    if (check_idx(surf, index, plane, &first, &last) < 0)
        return -1;
    sp = &surf->surfaceList[index];
    for (pl = first; pl <= last; pl++)
        if (sp->mappedAddr.addr[pl]) {
            munmap(sp->mappedAddr.addr[pl], sp->planeParams.psize[pl]);
            sp->mappedAddr.addr[pl] = NULL;
        }
    return 0;
}

/* The memfd is coherent with every mapping: syncs only validate. */
MOCK_EXPORT int NvBufSurfaceSyncForCpu(NvBufSurface *surf, int index, int plane)
{
    int f, l;
    return check_idx(surf, index, plane, &f, &l);
}

MOCK_EXPORT int NvBufSurfaceSyncForDevice(NvBufSurface *surf, int index,
                                          int plane)
{
    int f, l;
    return check_idx(surf, index, plane, &f, &l);
}

MOCK_EXPORT int NvBufSurfaceMemSet(NvBufSurface *surf, int index, int plane,
                                   uint8_t value)
{
    int pl, first, last;
    NvBufSurfaceParams *sp;

    if (check_idx(surf, index, plane, &first, &last) < 0)
        return -1;
    sp = &surf->surfaceList[index];
    for (pl = first; pl <= last; pl++) {
        int tmp;
        void *m = mock_plane_acquire(sp, (uint32_t)pl, &tmp);

        if (!m)
            return -1;
        memset(m, value, sp->planeParams.psize[pl]);
        mock_plane_release(sp, (uint32_t)pl, m, tmp);
    }
    return 0;
}

MOCK_EXPORT int NvBufSurfaceCopy(NvBufSurface *src, NvBufSurface *dst)
{
    uint32_t i;

    if (!src || !dst || src->batchSize != dst->batchSize)
        return -1;
    for (i = 0; i < src->batchSize; i++)
        if (mock_copy_params(&src->surfaceList[i], &dst->surfaceList[i]) < 0)
            return -1;
    return 0;
}

/* Copy one plane into a tightly packed caller buffer (outwidth x outheight
 * pixels, clipped to the plane). */
MOCK_EXPORT int NvBufSurface2Raw(NvBufSurface *surf, unsigned int index,
                                 unsigned int plane, unsigned int outwidth,
                                 unsigned int outheight, unsigned char *ptr)
{
    int f, l, tmp;
    uint32_t y, w, h, bpp;
    NvBufSurfaceParams *sp;
    unsigned char *m;

    if (!ptr || check_idx(surf, (int)index, (int)plane, &f, &l) < 0)
        return -1;
    sp = &surf->surfaceList[index];
    bpp = sp->planeParams.bytesPerPix[plane];
    w = outwidth < sp->planeParams.width[plane] ? outwidth
                                                : sp->planeParams.width[plane];
    h = outheight < sp->planeParams.height[plane]
        ? outheight : sp->planeParams.height[plane];
    m = mock_plane_acquire(sp, plane, &tmp);
    if (!m)
        return -1;
    for (y = 0; y < h; y++)
        memcpy(ptr + (size_t)y * outwidth * bpp,
               m + (size_t)y * sp->planeParams.pitch[plane], (size_t)w * bpp);
    mock_plane_release(sp, plane, m, tmp);
    return 0;
}

/* No EGL on the mock host. */
MOCK_EXPORT int NvBufSurfaceMapEglImage(NvBufSurface *surf, int index)
{
    (void)surf; (void)index;
    return -1;
}

MOCK_EXPORT int NvBufSurfaceUnMapEglImage(NvBufSurface *surf, int index)
{
    (void)surf; (void)index;
    return -1;
}
