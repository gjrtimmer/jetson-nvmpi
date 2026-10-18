/*
 * mock_nvbufsurface.c - behavioural mock of libnvbufsurface.so.1.0.0.
 *
 * Surfaces are memfd-backed (see mock_nvbuf_util.h). The fd stored in
 * surfaceList[i].bufferDesc is the surface's dma-buf stand-in.
 *
 * NvBufSurfaceFromFd is deliberately STRICT, like the real driver: it only
 * knows fds returned by Allocate/Create. A dup()'d fd, an EXPBUF fd or any
 * foreign fd fails, which reproduces the Tegra pitfall that downstream
 * filters must copy into a freshly allocated (registered) surface before
 * calling NvBufSurfTransform. Changing that would hide the very bug class
 * the mock exists to catch in CI.
 *
 * Lifecycle: Allocate/Create -> Destroy (single deallocation point). Destroy
 * unmaps leftover mappings, unregisters the fd BEFORE closing it (so a
 * reused fd number can never hit a stale entry), closes it and frees.
 *
 * Thread-safety: g_lock guards the fd registry only. Surface contents are
 * the caller's to synchronise, as with the real library.
 */
#define _GNU_SOURCE
#include "mock_nvbuf_util.h"
#include "mock_log.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#define MOCK_EXPORT __attribute__((visibility("default")))
#define MOCK_MAX_BATCH 64u

/* fd -> (surface, batch index). Grown on demand; guarded by g_lock. */
struct reg_ent { int fd; NvBufSurface *surf; uint32_t index; };
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct reg_ent *g_reg;
static size_t g_reg_n, g_reg_cap;

static int reg_add(int fd, NvBufSurface *surf, uint32_t index)
{
    int rc = 0;

    pthread_mutex_lock(&g_lock);
    if (g_reg_n == g_reg_cap) {
        size_t cap = g_reg_cap ? g_reg_cap * 2 : 16;
        /* Freed never: process-lifetime registry, bounded by live surfaces. */
        struct reg_ent *n = realloc(g_reg, cap * sizeof(*n));

        if (!n) {
            rc = -1;
            goto out;
        }
        g_reg = n;
        g_reg_cap = cap;
    }
    g_reg[g_reg_n++] = (struct reg_ent){fd, surf, index};
out:
    pthread_mutex_unlock(&g_lock);
    return rc;
}

static void reg_del(int fd)
{
    size_t i;

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < g_reg_n; i++)
        if (g_reg[i].fd == fd) {
            g_reg[i] = g_reg[--g_reg_n];
            break;
        }
    pthread_mutex_unlock(&g_lock);
}

/* Fill planeParams for one surface entry; returns the total byte size or 0
 * for an unsupported format / bad size. */
static uint32_t layout_surface(NvBufSurfaceParams *sp, uint32_t w, uint32_t h,
                               int cf)
{
    struct mock_plane_fmt f;
    uint32_t pl, off = 0;

    if (mock_fmt_info(cf, &f) < 0)
        return 0;
    sp->planeParams.num_planes = f.n;
    for (pl = 0; pl < f.n; pl++) {
        uint32_t pw = w / f.wdiv[pl], ph = h / f.hdiv[pl];
        uint32_t pitch = mock_align_up(pw * f.bpp[pl], MOCK_NVBUF_PITCH_ALIGN);

        sp->planeParams.width[pl] = pw;
        sp->planeParams.height[pl] = ph;
        sp->planeParams.pitch[pl] = pitch;
        sp->planeParams.bytesPerPix[pl] = f.bpp[pl];
        sp->planeParams.offset[pl] = off;
        sp->planeParams.psize[pl] = mock_align_up(pitch * ph, MOCK_NVBUF_PAGE);
        off += sp->planeParams.psize[pl];
    }
    return off;
}

/* Unwind a partially built surface (also used by Destroy). */
static void surface_free(NvBufSurface *s)
{
    uint32_t i, pl;

    if (!s)
        return;
    for (i = 0; s->surfaceList && i < s->batchSize; i++) {
        NvBufSurfaceParams *sp = &s->surfaceList[i];

        for (pl = 0; pl < sp->planeParams.num_planes; pl++)
            if (sp->mappedAddr.addr[pl]) {
                munmap(sp->mappedAddr.addr[pl], sp->planeParams.psize[pl]);
                sp->mappedAddr.addr[pl] = NULL;
            }
        if (sp->bufferDesc) {
            reg_del((int)sp->bufferDesc);       /* unregister before close */
            close((int)sp->bufferDesc);
            sp->bufferDesc = 0;
        }
    }
    free(s->surfaceList);
    memset(s, 0, sizeof(*s));                   /* scrub before free       */
    free(s);
}

static int alloc_surfaces(NvBufSurface **out, uint32_t batch,
                          const NvBufSurfaceCreateParams *cp)
{
    NvBufSurface *s;
    uint32_t i, total;

    if (!out || !cp || batch == 0 || batch > MOCK_MAX_BATCH ||
        cp->width == 0 || cp->height == 0 ||
        cp->width > MOCK_NVBUF_MAX_DIM || cp->height > MOCK_NVBUF_MAX_DIM)
        return -1;
    *out = NULL;
    /* Allocated here; freed by NvBufSurfaceDestroy -> surface_free(). */
    s = calloc(1, sizeof(*s));
    if (!s)
        return -1;
    s->surfaceList = calloc(batch, sizeof(*s->surfaceList));
    if (!s->surfaceList) {
        free(s);
        return -1;
    }
    s->batchSize = batch;
    s->numFilled = batch;
    s->gpuId = cp->gpuId;
    s->memType = cp->memType;
    s->isContiguous = cp->isContiguous;
    for (i = 0; i < batch; i++) {
        NvBufSurfaceParams *sp = &s->surfaceList[i];
        int fd;

        sp->width = cp->width;
        sp->height = cp->height;
        sp->colorFormat = cp->colorFormat;
        sp->layout = cp->layout;        /* stored linearly, see util.h */
        total = layout_surface(sp, cp->width, cp->height, cp->colorFormat);
        if (total == 0) {
            MOCK_ERR("NvBufSurfaceAllocate: unsupported format %d",
                     (int)cp->colorFormat);
            goto fail;
        }
        sp->pitch = sp->planeParams.pitch[0];
        sp->dataSize = total;
        fd = memfd_create("mock-nvbuf", MFD_CLOEXEC);
        if (fd < 0)
            goto fail;
        if (ftruncate(fd, total) < 0) {
            close(fd);
            goto fail;
        }
        sp->bufferDesc = (uint64_t)fd;
        if (reg_add(fd, s, i) < 0)
            goto fail;
    }
    *out = s;
    return 0;
fail:
    surface_free(s);
    return -1;
}

MOCK_EXPORT int NvBufSurfaceCreate(NvBufSurface **surf, uint32_t batchSize,
                                   NvBufSurfaceCreateParams *params)
{
    return alloc_surfaces(surf, batchSize, params);
}

MOCK_EXPORT int NvBufSurfaceAllocate(NvBufSurface **surf, uint32_t batchSize,
                                     NvBufSurfaceAllocateParams *params)
{
    /* memtag / pitch-padding hints do not change the mock layout. */
    return params ? alloc_surfaces(surf, batchSize, &params->params) : -1;
}

MOCK_EXPORT int NvBufSurfaceDestroy(NvBufSurface *surf)
{
    if (!surf)
        return -1;
    surface_free(surf);
    return 0;
}

/* Strict fd lookup: only fds issued by Allocate/Create (never dup()s). */
MOCK_EXPORT int NvBufSurfaceFromFd(int dmabuf_fd, void **buffer)
{
    size_t i;
    int rc = -1;

    if (!buffer || dmabuf_fd < 0)
        return -1;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < g_reg_n; i++)
        if (g_reg[i].fd == dmabuf_fd) {
            *buffer = g_reg[i].surf;
            rc = 0;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    if (rc)
        MOCK_TRACE("NvBufSurfaceFromFd(%d): unknown fd", dmabuf_fd);
    return rc;
}

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
