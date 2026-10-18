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

/*
 * Copy (and nearest-neighbour scale when sizes differ) all planes of
 * src -> dst. Both must have the same plane count and per-plane bytes per
 * pixel (no colour conversion in the skeleton: that belongs to the decoder
 * model, #99). Returns 0 or -1. Callers serialise concurrent use.
 */
static inline int mock_copy_params(const NvBufSurfaceParams *s,
                                   const NvBufSurfaceParams *d)
{
    uint32_t pl, y, x, n = s->planeParams.num_planes;
    int rc = 0;

    if (n == 0 || n != d->planeParams.num_planes)
        return -1;
    for (pl = 0; pl < n && !rc; pl++) {
        uint32_t bpp = s->planeParams.bytesPerPix[pl];
        uint32_t sw = s->planeParams.width[pl], sh = s->planeParams.height[pl];
        uint32_t dw = d->planeParams.width[pl], dh = d->planeParams.height[pl];
        int st, dt;
        unsigned char *sm, *dm;

        if (bpp == 0 || bpp != d->planeParams.bytesPerPix[pl] ||
            sw == 0 || sh == 0 || dw == 0 || dh == 0)
            return -1;
        sm = mock_plane_acquire(s, pl, &st);
        dm = sm ? mock_plane_acquire(d, pl, &dt) : NULL;
        if (!sm || !dm) {
            if (sm)
                mock_plane_release(s, pl, sm, st);
            return -1;
        }
        for (y = 0; y < dh; y++) {
            const unsigned char *srow = sm +
                (uint64_t)(y * sh / dh) * s->planeParams.pitch[pl];
            unsigned char *drow = dm + (uint64_t)y * d->planeParams.pitch[pl];

            if (sw == dw) {
                memcpy(drow, srow, (size_t)dw * bpp);
                continue;
            }
            for (x = 0; x < dw; x++)
                memcpy(drow + (size_t)x * bpp,
                       srow + (size_t)(x * sw / dw) * bpp, bpp);
        }
        mock_plane_release(s, pl, sm, st);
        mock_plane_release(d, pl, dm, dt);
    }
    return rc;
}

#endif /* NVMPI_MOCK_NVBUF_UTIL_H */
