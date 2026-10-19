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
#include "mock_faults.h"

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

/* Free a surface struct whose fd the application already close()d (EXPBUF
 * fds are closed by NvBuffer teardown, not via NvBufSurfaceDestroy). Never
 * touches the fd. Called with g_lock held, only for batch-1 surfaces. */
static void surface_drop_mem(NvBufSurface *s)
{
    uint32_t pl;
    NvBufSurfaceParams *sp = &s->surfaceList[0];

    for (pl = 0; pl < sp->planeParams.num_planes; pl++)
        if (sp->mappedAddr.addr[pl])
            munmap(sp->mappedAddr.addr[pl], sp->planeParams.psize[pl]);
    free(s->surfaceList);
    memset(s, 0, sizeof(*s));
    free(s);
}

/* Drop registry entries whose fd is no longer open (the app closed it).
 * Run on every registration so stale entries cannot pile up or alias a
 * recycled fd number. Caveat: if the application re-opens the SAME memfd
 * under the recycled number (e.g. dup() of a still-open sibling), the entry
 * is indistinguishable; the mock cannot see close(). g_lock held. */
static void reg_sweep_locked(void)
{
    size_t i = 0;

    while (i < g_reg_n) {
        if (fcntl(g_reg[i].fd, F_GETFD) >= 0) {
            i++;
            continue;
        }
        if (g_reg[i].surf->batchSize == 1)
            surface_drop_mem(g_reg[i].surf);
        g_reg[i] = g_reg[--g_reg_n];
    }
}

static int reg_add(int fd, NvBufSurface *surf, uint32_t index)
{
    int rc = 0;

    pthread_mutex_lock(&g_lock);
    reg_sweep_locked();
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

    /* Injected allocation failure (NVMPI_MOCK_FAULT=alloc@N): fails like an
     * exhausted allocator, before anything is allocated. */
    if (mock_fault_hit(MFP_ALLOC)) {
        if (out)
            *out = NULL;
        return -1;
    }
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

/*
 * Mock-only entry point (NOT part of the NVIDIA API), called by the V4L2
 * mock's EXPBUF through dlsym: wrap an exported buffer fd as a registered
 * batch-1 surface so NvBufSurfaceFromFd/Map/Sync work on it. `fd` is a
 * dup of the V4L2 buffer's memfd that the application will close; planes
 * are described by byte size and offset inside that memfd. The surface is
 * freed by NvBufSurfaceDestroy or, after the app closes the fd, by the
 * registry sweep. Returns 0 or -1 (fd untouched on failure).
 */
MOCK_EXPORT int mock_nvbuf_register_export(int fd, uint32_t n_planes,
                                           const uint32_t *sizes,
                                           const uint32_t *offsets)
{
    NvBufSurface *s;
    NvBufSurfaceParams *sp;
    uint32_t pl;

    if (fd < 0 || !sizes || !offsets || n_planes == 0 ||
        n_planes > NVBUF_MAX_PLANES)
        return -1;
    /* Allocated here; freed by NvBufSurfaceDestroy or reg_sweep_locked. */
    s = calloc(1, sizeof(*s));
    if (!s)
        return -1;
    s->surfaceList = calloc(1, sizeof(*s->surfaceList));
    if (!s->surfaceList) {
        free(s);
        return -1;
    }
    s->batchSize = s->numFilled = 1;
    sp = &s->surfaceList[0];
    sp->planeParams.num_planes = n_planes;
    for (pl = 0; pl < n_planes; pl++) {
        sp->planeParams.width[pl] = sizes[pl];   /* bytes: opaque plane */
        sp->planeParams.height[pl] = 1;
        sp->planeParams.pitch[pl] = sizes[pl];
        sp->planeParams.bytesPerPix[pl] = 1;
        sp->planeParams.offset[pl] = offsets[pl];
        sp->planeParams.psize[pl] = mock_align_up(sizes[pl], MOCK_NVBUF_PAGE);
    }
    sp->bufferDesc = (uint64_t)fd;
    if (reg_add(fd, s, 0) < 0) {
        sp->bufferDesc = 0;                  /* caller keeps the fd */
        free(s->surfaceList);
        free(s);
        return -1;
    }
    return 0;
}
