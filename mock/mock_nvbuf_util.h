/*
 * mock_nvbuf_util.h - layout and copy helpers shared by the nvbufsurface and
 * nvbufsurftransform mocks (static inline: each .so carries its own copy,
 * because the two libraries must not depend on each other's symbols).
 *
 * Mock surface model: one memfd per surface (NvBufSurfaceParams.bufferDesc is
 * that fd, like the real dma-buf fd). Planes are laid out back to back,
 * each starting on a page boundary so it can be mmap()ed on its own at
 * planeParams.offset[]. Layout is always pitch-linear: BLOCK_LINEAR requests
 * are accepted but stored linearly (the mock models data flow, not tiling).
 */
#ifndef NVMPI_MOCK_NVBUF_UTIL_H
#define NVMPI_MOCK_NVBUF_UTIL_H

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <nvbufsurface.h>

#define MOCK_NVBUF_PAGE  4096u
#define MOCK_NVBUF_PITCH_ALIGN 256u         /* VIC/NvBuffer pitch alignment */
#define MOCK_NVBUF_MAX_DIM 16384u           /* sanity bound (fuzzed sizes)  */

static inline uint32_t mock_align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) / a * a;
}

/* Per-plane geometry of a colour format: width/height divisors relative to
 * the surface and bytes per pixel of that plane (chroma pairs count as one
 * pixel of 2 bytes, matching the real planeParams.bytesPerPix). */
struct mock_plane_fmt {
    uint32_t n;
    uint32_t wdiv[3], hdiv[3], bpp[3];
};

/* Returns 0 and fills *f, or -1 for a format the mock does not model. */
static inline int mock_fmt_info(int cf, struct mock_plane_fmt *f)
{
    memset(f, 0, sizeof(*f));
    switch (cf) {
    case NVBUF_COLOR_FORMAT_NV12:
    case NVBUF_COLOR_FORMAT_NV12_ER:
    case NVBUF_COLOR_FORMAT_NV12_709:
    case NVBUF_COLOR_FORMAT_NV12_709_ER:
    case NVBUF_COLOR_FORMAT_NV12_2020:
        *f = (struct mock_plane_fmt){2, {1, 2}, {1, 2}, {1, 2}};
        return 0;
    case NVBUF_COLOR_FORMAT_NV12_10LE:
    case NVBUF_COLOR_FORMAT_NV12_10LE_ER:
    case NVBUF_COLOR_FORMAT_NV12_10LE_709:
    case NVBUF_COLOR_FORMAT_NV12_10LE_709_ER:
    case NVBUF_COLOR_FORMAT_NV12_10LE_2020:
        *f = (struct mock_plane_fmt){2, {1, 2}, {1, 2}, {2, 4}};
        return 0;
    case NVBUF_COLOR_FORMAT_YUV420:
    case NVBUF_COLOR_FORMAT_YUV420_ER:
    case NVBUF_COLOR_FORMAT_YUV420_709:
    case NVBUF_COLOR_FORMAT_YUV420_709_ER:
        *f = (struct mock_plane_fmt){3, {1, 2, 2}, {1, 2, 2}, {1, 1, 1}};
        return 0;
    case NVBUF_COLOR_FORMAT_NV24:
        *f = (struct mock_plane_fmt){2, {1, 1}, {1, 1}, {1, 2}};
        return 0;
    case NVBUF_COLOR_FORMAT_NV24_10LE:
        *f = (struct mock_plane_fmt){2, {1, 1}, {1, 1}, {2, 4}};
        return 0;
    case NVBUF_COLOR_FORMAT_YUV444:
        *f = (struct mock_plane_fmt){3, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
        return 0;
    case NVBUF_COLOR_FORMAT_BGRA:
        *f = (struct mock_plane_fmt){1, {1}, {1}, {4}};
        return 0;
    case NVBUF_COLOR_FORMAT_BGR:
    case NVBUF_COLOR_FORMAT_B8_G8_R8:
        *f = (struct mock_plane_fmt){1, {1}, {1}, {3}};
        return 0;
    default:
        return -1;
    }
}

/*
 * Map one plane of surface entry p read/write for the mock's own copy/set
 * helpers. Always a private temporary mapping (never the caller's
 * NvBufSurfaceMap one, which may be read-only); all mappings of the memfd
 * are coherent, so contents match. Release with mock_plane_release().
 * NULL on failure.
 */
static inline void *mock_plane_acquire(const NvBufSurfaceParams *p,
                                       uint32_t plane, int *tmp)
{
    void *m = mmap(NULL, p->planeParams.psize[plane], PROT_READ | PROT_WRITE,
                   MAP_SHARED, (int)p->bufferDesc, p->planeParams.offset[plane]);

    *tmp = 1;
    return m == MAP_FAILED ? NULL : m;
}

static inline void mock_plane_release(const NvBufSurfaceParams *p,
                                      uint32_t plane, void *m, int tmp)
{
    (void)tmp;                          /* always a temporary mapping */
    munmap(m, p->planeParams.psize[plane]);
}

/* A plane must lie inside its allocation: rows fit the pitch and the pitch
 * times the height fits psize. Guards the mapped-memory accesses below
 * against inconsistent (e.g. hand-built or fuzzed) planeParams. */
static inline int mock_plane_fits(const NvBufSurfaceParams *p, uint32_t pl)
{
    const NvBufSurfacePlaneParams *pp = &p->planeParams;

    return pp->width[pl] && pp->height[pl] && pp->bytesPerPix[pl] &&
           (uint64_t)pp->width[pl] * pp->bytesPerPix[pl] <= pp->pitch[pl] &&
           (uint64_t)pp->pitch[pl] * pp->height[pl] <= pp->psize[pl];
}

/* Rectangle in LUMA pixels (a crop window); NULL always means the whole
 * surface. Only NvBufSurfTransform with CROP_SRC / CROP_DST passes one. */
struct mock_rect {
    uint32_t x, y, w, h;
};

/* Window of plane pl, in that plane's pixels, covered by luma rect r (whole
 * plane when r is NULL). Chroma planes of 4:2:0 formats are half size, so
 * the rect is halved (rounded down, like the allocator's plane sizes).
 * Returns -1 for an empty window or one outside the plane: a crop that does
 * not fit is a caller error the real VIC rejects too. */
static inline int mock_plane_win(const NvBufSurfaceParams *p, uint32_t pl,
                                 const struct mock_rect *r, uint32_t win[4])
{
    uint32_t pw = p->planeParams.width[pl], ph = p->planeParams.height[pl];
    uint32_t dx = (pl > 0 && pw * 2 <= p->width + 1) ? 2 : 1;
    uint32_t dy = (pl > 0 && ph * 2 <= p->height + 1) ? 2 : 1;

    win[0] = r ? r->x / dx : 0;
    win[1] = r ? r->y / dy : 0;
    win[2] = r ? r->w / dx : pw;
    win[3] = r ? r->h / dy : ph;
    if (win[2] == 0 || win[3] == 0 ||
        (uint64_t)win[0] + win[2] > pw || (uint64_t)win[1] + win[3] > ph)
        return -1;
    return 0;
}

/* Copy window sr of plane spl of s into window dr of plane dpl of d,
 * nearest-neighbour scaled when the window sizes differ. Same bytes per
 * pixel required. Returns 0 or -1. */
static inline int mock_copy_plane(const NvBufSurfaceParams *s, uint32_t spl,
                                  const struct mock_rect *sr,
                                  const NvBufSurfaceParams *d, uint32_t dpl,
                                  const struct mock_rect *dr)
{
    uint32_t bpp = s->planeParams.bytesPerPix[spl];
    uint32_t sw[4], dw[4];
    uint32_t y, x;
    int st, dt;
    unsigned char *sm, *dm;

    if (bpp == 0 || bpp != d->planeParams.bytesPerPix[dpl] ||
        mock_plane_win(s, spl, sr, sw) < 0 ||
        mock_plane_win(d, dpl, dr, dw) < 0)
        return -1;
    sm = mock_plane_acquire(s, spl, &st);
    dm = sm ? mock_plane_acquire(d, dpl, &dt) : NULL;
    if (!sm || !dm) {
        if (sm)
            mock_plane_release(s, spl, sm, st);
        return -1;
    }
    for (y = 0; y < dw[3]; y++) {
        const unsigned char *srow = sm + (uint64_t)(sw[1] + y * sw[3] / dw[3]) *
            s->planeParams.pitch[spl] + (uint64_t)sw[0] * bpp;
        unsigned char *drow = dm + (uint64_t)(dw[1] + y) *
            d->planeParams.pitch[dpl] + (uint64_t)dw[0] * bpp;

        if (sw[2] == dw[2]) {
            memcpy(drow, srow, (size_t)dw[2] * bpp);
            continue;
        }
        for (x = 0; x < dw[2]; x++)
            memcpy(drow + (size_t)x * bpp,
                   srow + (size_t)(x * sw[2] / dw[2]) * bpp, bpp);
    }
    mock_plane_release(s, spl, sm, st);
    mock_plane_release(d, dpl, dm, dt);
    return 0;
}

/* 8-bit 4:2:0 semi-planar (NV12 and its colour-range variants: same bytes)
 * and planar (I420 family) share the luma plane and differ only in chroma
 * packing. The colour-range tags are metadata; no pixel maths is modelled. */
static inline int mock_is_nv12(int cf)
{
    return cf == NVBUF_COLOR_FORMAT_NV12 || cf == NVBUF_COLOR_FORMAT_NV12_ER ||
           cf == NVBUF_COLOR_FORMAT_NV12_709 ||
           cf == NVBUF_COLOR_FORMAT_NV12_709_ER ||
           cf == NVBUF_COLOR_FORMAT_NV12_2020;
}

static inline int mock_is_i420(int cf)
{
    return cf == NVBUF_COLOR_FORMAT_YUV420 ||
           cf == NVBUF_COLOR_FORMAT_YUV420_ER ||
           cf == NVBUF_COLOR_FORMAT_YUV420_709 ||
           cf == NVBUF_COLOR_FORMAT_YUV420_709_ER;
}

/*
 * NV12 <-> I420 in one pass (convert + nearest-neighbour scale): the luma
 * plane goes through mock_copy_plane; the interleaved UV plane is
 * deinterleaved into U/V (or the reverse), sampling the source chroma grid
 * at the scaled position. Returns 0 or -1 (inconsistent layout).
 */
static inline int mock_convert_420(const NvBufSurfaceParams *s,
                                   const struct mock_rect *sr,
                                   const NvBufSurfaceParams *d,
                                   const struct mock_rect *dr)
{
    int semi_src = mock_is_nv12(s->colorFormat);
    const NvBufSurfaceParams *sem = semi_src ? s : d;
    const NvBufSurfaceParams *pla = semi_src ? d : s;
    uint32_t sw[4], dw[4];              /* chroma windows: x, y, w, h */
    uint32_t pl, k, y, x;
    int mt, pt[2], rc = -1;
    unsigned char *mm, *pm[2];

    if (sem->planeParams.num_planes != 2 || pla->planeParams.num_planes != 3)
        return -1;
    for (pl = 0; pl < 3; pl++)
        if ((pl < 2 && !mock_plane_fits(sem, pl)) || !mock_plane_fits(pla, pl))
            return -1;
    /* Layout the model expects: Y 1 B/px, UV pairs 2 B/px, U and V 1 B/px,
     * and the two planar chroma planes identical in size. */
    if (sem->planeParams.bytesPerPix[0] != 1 ||
        sem->planeParams.bytesPerPix[1] != 2 ||
        pla->planeParams.bytesPerPix[0] != 1 ||
        pla->planeParams.bytesPerPix[1] != 1 ||
        pla->planeParams.bytesPerPix[2] != 1 ||
        pla->planeParams.width[1] != pla->planeParams.width[2] ||
        pla->planeParams.height[1] != pla->planeParams.height[2])
        return -1;
    if (mock_plane_win(s, 1, sr, sw) < 0 || mock_plane_win(d, 1, dr, dw) < 0 ||
        mock_copy_plane(s, 0, sr, d, 0, dr) < 0)
        return -1;

    mm = mock_plane_acquire(sem, 1, &mt);
    if (!mm)
        return -1;
    pm[0] = mock_plane_acquire(pla, 1, &pt[0]);
    pm[1] = pm[0] ? mock_plane_acquire(pla, 2, &pt[1]) : NULL;
    if (pm[0] && pm[1]) {
        for (y = 0; y < dw[3]; y++)
            for (x = 0; x < dw[2]; x++) {
                uint32_t sx = sw[0] + x * sw[2] / dw[2];
                uint32_t sy = sw[1] + y * sw[3] / dw[3];
                uint32_t ox = dw[0] + x, oy = dw[1] + y;

                for (k = 0; k < 2; k++) {
                    if (semi_src)
                        pm[k][(uint64_t)oy * pla->planeParams.pitch[1 + k] + ox] =
                            mm[(uint64_t)sy * sem->planeParams.pitch[1] +
                               (uint64_t)sx * 2 + k];
                    else
                        mm[(uint64_t)oy * sem->planeParams.pitch[1] +
                           (uint64_t)ox * 2 + k] =
                            pm[k][(uint64_t)sy * pla->planeParams.pitch[1 + k] +
                                  sx];
                }
            }
        rc = 0;
    }
    if (pm[1])
        mock_plane_release(pla, 2, pm[1], pt[1]);
    if (pm[0])
        mock_plane_release(pla, 1, pm[0], pt[0]);
    mock_plane_release(sem, 1, mm, mt);
    return rc;
}

/*
 * Copy (and nearest-neighbour scale when sizes differ) the luma rect sr of
 * src into the luma rect dr of dst (NULL = whole surface), all planes.
 * Same-layout pairs need the same plane count and per-plane bytes per
 * pixel; 8-bit NV12 <-> I420 is converted (mock_convert_420). Any other
 * cross-format pair is rejected. Returns 0 or -1. Callers serialise
 * concurrent use.
 */
static inline int mock_copy_rect(const NvBufSurfaceParams *s,
                                 const struct mock_rect *sr,
                                 const NvBufSurfaceParams *d,
                                 const struct mock_rect *dr)
{
    uint32_t pl, n = s->planeParams.num_planes;

    if ((mock_is_nv12(s->colorFormat) && mock_is_i420(d->colorFormat)) ||
        (mock_is_i420(s->colorFormat) && mock_is_nv12(d->colorFormat)))
        return mock_convert_420(s, sr, d, dr);
    if (n == 0 || n != d->planeParams.num_planes)
        return -1;
    for (pl = 0; pl < n; pl++)
        if (mock_copy_plane(s, pl, sr, d, pl, dr) < 0)
            return -1;
    return 0;
}

/* Whole-surface form of mock_copy_rect. */
static inline int mock_copy_params(const NvBufSurfaceParams *s,
                                   const NvBufSurfaceParams *d)
{
    return mock_copy_rect(s, NULL, d, NULL);
}

#endif /* NVMPI_MOCK_NVBUF_UTIL_H */
