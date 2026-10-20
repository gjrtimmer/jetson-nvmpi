/*
 * jpeg_mock_util.h - helpers of test_api_jpeg_mock.c: borrow bookkeeping, fd
 * counting and the geometry/pixel check of one borrowed JPEG frame against
 * the NvJPEGDecoder mock (mock/mock_jpegdec.cpp, issue #128).
 */
#ifndef NVMPI_TEST_JPEG_MOCK_UTIL_H
#define NVMPI_TEST_JPEG_MOCK_UTIL_H

#include "case_log.h"
#include <nvmpi.h>
#include <nvbufsurface.h>
#include "../../mock/mock_jpegdec.h"
#include "jpeg_fd_data.h"
#include "jpeg_mock_data.h"

#include <dirent.h>

/* Child of case_log_run: leave with a line-derived code (see case_log.h). */
#define REQUIRE(c)                                                        \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "REQUIRE failed %s:%d: %s\n", __FILE__,       \
                    __LINE__, #c);                                        \
            _exit(10 + (__LINE__ % 200));                                 \
        }                                                                 \
    } while (0)

/* Fault spec that never fires: passed to a case only to mark its libnvmpi
 * error lines as expected (see case_log.h). */
#define NEVER_FIRES "alloc@999999"

struct img {
    const unsigned char *d;
    unsigned long n;
    unsigned w, h;                       /* displayed size from the SOF   */
};

static struct img img_a(void)            /* 33x17 (decoder pads to 48x32) */
{
    struct img i = { jpeg_fd_test_jpg, jpeg_fd_test_jpg_len, 33, 17 };
    return i;
}

static struct img img_b(void)            /* 64x48 (already aligned)       */
{
    struct img i = { jpeg_mock_64x48_jpg, jpeg_mock_64x48_jpg_len, 64, 48 };
    return i;
}

static void set_pad(int pad)
{
    char v[16];

    snprintf(v, sizeof(v), "%d", pad);
    setenv(MOCK_JPEG_PAD_ENV, v, 1);
}

/* One borrowed frame: the frame plus what its release needs. */
struct borrow {
    nvmpi_fd_frame f;
    nvmpi_frame_release_cb rel;
    void *op;
};

static int put(nvmpictx *c, const struct img *i, unsigned long pts)
{
    nvPacket p;

    memset(&p, 0, sizeof(p));
    p.payload = (unsigned char *)i->d;
    p.payload_size = i->n;
    p.pts = pts;
    return nvmpi_jpeg_decoder_put_packet(c, &p);
}

static int take(nvmpictx *c, struct borrow *b, bool wait)
{
    memset(b, 0, sizeof(*b));
    b->f.struct_size = sizeof(b->f);
    return nvmpi_jpeg_decoder_get_frame_fd(c, &b->f, &b->rel, &b->op, wait);
}

/* Exactly once per borrow: a second call on the same struct is a no-op. */
static void give(struct borrow *b)
{
    if (b->rel)
        b->rel(b->op);
    b->rel = NULL;
}

/* Open descriptors of this process, plus a constant (".", ".." and the
 * DIR's own fd): only differences between two calls mean anything. */
static int nfds(void)
{
    DIR *d = opendir("/proc/self/fd");
    int n = 0;

    if (!d)
        return -1;
    while (readdir(d))
        n++;
    closedir(d);
    return n;
}

/* Row bytes the plane needs for a w x h picture (chroma rounds up). */
static unsigned row_bytes(nvPixFormat fmt, unsigned pl, unsigned w)
{
    if (pl == 0)
        return w;
    return fmt == NV_PIX_NV12 ? (w + 1) / 2 * 2 : (w + 1) / 2;
}

/* Compare the visible window of the surface behind f with the mock pattern.
 * Chroma is compared over w/2 x h/2: the mock allocator rounds chroma planes
 * DOWN for odd sizes. Returns 0 on a match. */
static int check_pixels(const nvmpi_fd_frame *f, NvBufSurface *s)
{
    NvBufSurfaceParams *p = &s->surfaceList[0];
    unsigned pl, x, y, k, bad = 0;

    for (pl = 0; pl < f->num_planes; pl++) {
        unsigned w = pl ? f->width / 2 : f->width;
        unsigned h = pl ? f->height / 2 : f->height;
        unsigned char *m;

        if (NvBufSurfaceMap(s, 0, (int)pl, NVBUF_MAP_READ) != 0)
            return 1;
        m = (unsigned char *)p->mappedAddr.addr[pl];
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                for (k = 0; k < (f->format == NV_PIX_NV12 && pl ? 2u : 1u); k++) {
                    unsigned char want = mock_jpeg_pattern(
                        (f->format == NV_PIX_NV12 && pl) ? 1 + k : pl, x, y);
                    unsigned char got = (f->format == NV_PIX_NV12 && pl)
                        ? m[(size_t)y * f->pitch[pl] + x * 2 + k]
                        : m[(size_t)y * f->pitch[pl] + x];

                    if (got != want && bad++ < 3)
                        fprintf(stderr, "pixel plane %u (%u,%u,%u): got %u "
                                "want %u\n", pl, x, y, k, got, want);
                }
        NvBufSurfaceUnMap(s, 0, (int)pl);
    }
    return bad != 0;
}

/* Geometry of one borrowed frame against the expected displayed size, the
 * registration of its fd, and optionally its pixels. 0 = ok. */
static int check_frame(const nvmpi_fd_frame *f, nvPixFormat fmt, unsigned w,
                       unsigned h, int pixels)
{
    unsigned want_planes = fmt == NV_PIX_NV12 ? 2 : 3, pl;
    NvBufSurface *s = NULL;

    if (f->width != w || f->height != h || f->format != fmt ||
        f->num_planes != want_planes || f->fd < 0 || f->size == 0 ||
        !(f->flags & NVMPI_FD_FRAME_FULL_RANGE)) {
        fprintf(stderr, "frame: %ux%u fmt %d planes %u fd %d size %llu "
                "flags %x; want %ux%u fmt %d planes %u\n", f->width, f->height,
                (int)f->format, f->num_planes, f->fd,
                (unsigned long long)f->size, f->flags, w, h, (int)fmt,
                want_planes);
        return 1;
    }
    for (pl = 0; pl < want_planes; pl++) {
        unsigned rows = pl ? (h + 1) / 2 : h;

        if (f->pitch[pl] < row_bytes(fmt, pl, w) ||
            f->offset[pl] >= f->size ||
            (pl && f->offset[pl] <= f->offset[pl - 1]) ||
            (uint64_t)f->offset[pl] + (uint64_t)f->pitch[pl] * rows > f->size) {
            fprintf(stderr, "plane %u: pitch %u offset %u size %llu\n", pl,
                    f->pitch[pl], f->offset[pl], (unsigned long long)f->size);
            return 2;
        }
    }
    /* The fd must be the registered original (a dup'd fd fails here) and its
     * plane layout must be the one reported in the frame. */
    if (NvBufSurfaceFromFd(f->fd, (void **)&s) < 0 || !s ||
        s->surfaceList[0].planeParams.num_planes != want_planes) {
        fprintf(stderr, "NvBufSurfaceFromFd(%d) failed\n", f->fd);
        return 3;
    }
    for (pl = 0; pl < want_planes; pl++)
        if (s->surfaceList[0].planeParams.pitch[pl] != f->pitch[pl] ||
            s->surfaceList[0].planeParams.offset[pl] != f->offset[pl]) {
            fprintf(stderr, "plane %u layout differs from the surface\n", pl);
            return 4;
        }
    return pixels ? check_pixels(f, s) : 0;
}

#endif
