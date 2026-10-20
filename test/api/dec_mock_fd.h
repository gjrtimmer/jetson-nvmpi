/*
 * dec_mock_fd.h - zero-copy (get_frame_fd) lifetime cases of
 * test_api_dec_mock.c (issue #137): release after close, resolution change
 * with borrowed frames, descriptor accounting, flush with borrows, and an
 * allocation failure during a resolution change. Included once from
 * test_api_dec_mock.c after the helpers (create, put, REQUIRE, fx_*).
 *
 * The mock surfaces are memfd-backed, and NvBufSurfaceFromFd is as strict as
 * the driver (unknown / destroyed fd -> error), so "fd still valid and the
 * gray pattern is still readable" proves a borrowed buffer was not freed.
 */
#ifndef NVMPI_TEST_DEC_MOCK_FD_H
#define NVMPI_TEST_DEC_MOCK_FD_H

#include <nvbufsurface.h>

#include <dirent.h>
#include <time.h>

/* New frames after a resolution change must not wait for borrows (the old
 * code waited up to 1 s); bound includes a loaded ASan runner. */
#define FDB_NEW_FRAME_MAX_MS 300
#define FDB_CLOSE_MAX_MS 3000

struct fdb {                             /* one borrowed frame            */
    int fd, w, h, pitch;
    int64_t ts;
    nvmpi_frame_release_cb rel;
    void *op;
};

static long fdb_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int fdb_nfds(void)
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

static int fdb_take(nvmpictx *c, struct fdb *b)
{
    memset(b, 0, sizeof(*b));
    return nvmpi_decoder_get_frame_fd(c, &b->fd, &b->w, &b->h, &b->pitch,
                                      &b->ts, &b->rel, &b->op, true);
}

/* Exactly once per borrow; a second call is a no-op. */
static void fdb_give(struct fdb *b)
{
    if (b->rel)
        b->rel(b->op);
    b->rel = NULL;
}

/* Feed one AU (IDR when w != 0, else a P slice) and borrow its frame. */
static int fdb_step(nvmpictx *c, struct fdb *b, unsigned w, unsigned h,
                    unsigned long pts)
{
    uint8_t au[4096];
    size_t len = w ? fx_idr(au, w, h) : fx_pslice(au);

    if (put(c, au, len, pts) != 0)
        return -2;
    return fdb_take(c, b);
}

/* The borrowed fd is registered, has the geometry the borrow reported and
 * still carries the mock's gray luma at the corners of the visible window.
 * 0 = intact. */
static int fdb_check(const struct fdb *b)
{
    NvBufSurface *s = NULL;
    unsigned char *m;
    int r, bad = 0;

    if (b->fd < 0 || NvBufSurfaceFromFd(b->fd, (void **)&s) < 0 || !s) {
        fprintf(stderr, "fdb_check: fd %d is not a live surface\n", b->fd);
        return 1;
    }
    if (s->surfaceList[0].planeParams.pitch[0] != (unsigned)b->pitch ||
        s->surfaceList[0].width < (unsigned)b->w ||
        s->surfaceList[0].height < (unsigned)b->h) {
        fprintf(stderr, "fdb_check: surface geometry != borrow %dx%d/%d\n",
                b->w, b->h, b->pitch);
        return 2;
    }
    if (NvBufSurfaceMap(s, 0, 0, NVBUF_MAP_READ) != 0)
        return 3;
    m = (unsigned char *)s->surfaceList[0].mappedAddr.addr[0];
    for (r = 0; r < b->h; r++)
        if (m[(size_t)r * b->pitch] != 0x80 ||
            m[(size_t)r * b->pitch + b->w - 1] != 0x80)
            bad = 1;
    NvBufSurfaceUnMap(s, 0, 0);
    return bad ? 4 : 0;
}

/* Borrow n frames (first one an IDR of w x h), all checked. */
static void fdb_hold(nvmpictx *c, struct fdb *b, int n, unsigned w,
                     unsigned h, unsigned long pts0)
{
    int i;

    for (i = 0; i < n; i++) {
        REQUIRE(fdb_step(c, &b[i], i == 0 ? w : 0, h, pts0 + i) == 0);
        REQUIRE(b[i].w == (int)w && b[i].h == (int)h && b[i].pitch >= (int)w);
        REQUIRE(fdb_check(&b[i]) == 0);
    }
}

/* Close with 3 frames borrowed: close must not wait, the fds must stay valid
 * and intact until released, and releasing after close must be safe (LSan /
 * ASan prove the buffers are freed exactly once, by the last release). */
static int release_after_close(void)
{
    struct fdb b[3];
    int i, base = fdb_nfds();
    nvmpictx *c = create(0, 0, 4);
    long t0;

    REQUIRE(c);
    fdb_hold(c, b, 3, 320, 240, 1);
    t0 = fdb_ms();
    REQUIRE(nvmpi_decoder_close(c) == 0);
    REQUIRE(fdb_ms() - t0 < FDB_CLOSE_MAX_MS);
    for (i = 0; i < 3; i++)
        REQUIRE(fdb_check(&b[i]) == 0);  /* alive after close              */
    for (i = 2; i >= 0; i--)             /* any order, any time            */
        fdb_give(&b[i]);
    REQUIRE(fdb_nfds() == base);
    return 0;
}

/* Descriptor accounting: with K frames borrowed, exactly K descriptors
 * survive close (one memfd per borrowed surface); the generation, and so all
 * of them, is freed by the last release. */
static int fd_count(void)
{
    struct fdb b[3];
    int i, base = fdb_nfds();
    nvmpictx *c = create(0, 0, 4);

    REQUIRE(c);
    fdb_hold(c, b, 3, 320, 240, 1);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    REQUIRE(fdb_nfds() == base + 3);
    fdb_give(&b[0]);                     /* the generation dies with its   */
    REQUIRE(fdb_nfds() == base + 3);     /* LAST borrow, not before        */
    fdb_give(&b[1]);
    fdb_give(&b[2]);
    REQUIRE(fdb_nfds() == base);
    for (i = 0; i < 3; i++)
        REQUIRE(b[i].rel == NULL);
    return 0;
}

/* Resolution change with 2 frames borrowed at 320x240: the pool is rebuilt
 * at 640x360 without waiting for them (the pre-#137 code never rebuilt it:
 * the skip check compared a value with itself, so the new frames were
 * reported 640 wide over a pitch of 320), the old buffers stay intact, and
 * releasing them later neither requeues them into the new pool nor leaks. */
static int res_change_borrowed(void)
{
    struct fdb old[2], nw[4];
    int i, base = fdb_nfds();
    nvmpictx *c = create(0, 0, 4);
    long t0;

    REQUIRE(c);
    fdb_hold(c, old, 2, 320, 240, 1);
    t0 = fdb_ms();
    REQUIRE(fdb_step(c, &nw[0], 640, 360, 10) == 0);
    REQUIRE(fdb_ms() - t0 < FDB_NEW_FRAME_MAX_MS);
    REQUIRE(nw[0].w == 640 && nw[0].h == 360);
    REQUIRE(nw[0].pitch >= 640);         /* pool really rebuilt           */
    REQUIRE(fdb_check(&nw[0]) == 0);
    for (i = 0; i < 2; i++)              /* old gen untouched             */
        REQUIRE(fdb_check(&old[i]) == 0);
    for (i = 1; i < 4; i++) {            /* the new pool holds 4 buffers  */
        REQUIRE(fdb_step(c, &nw[i], 0, 360, 10 + i) == 0);
        REQUIRE(nw[i].w == 640 && nw[i].pitch >= 640);
    }
    for (i = 0; i < 2; i++)
        fdb_give(&old[i]);               /* retired gen: freed, not reused */
    for (i = 0; i < 4; i++)
        fdb_give(&nw[i]);
    for (i = 0; i < 6; i++) {            /* all 4 new buffers recycle     */
        REQUIRE(fdb_step(c, &nw[0], 0, 360, 20 + i) == 0);
        REQUIRE(nw[0].w == 640 && fdb_check(&nw[0]) == 0);
        fdb_give(&nw[0]);
    }
    REQUIRE(nvmpi_decoder_close(c) == 0);
    REQUIRE(fdb_nfds() == base);
    return 0;
}

/* Flush with 2 frames borrowed: the generation stays current, the borrows
 * stay valid across the flush, and releasing them afterwards returns them to
 * the pool (decoding continues, nothing leaks at close). */
static int flush_borrowed(void)
{
    struct fdb b[2], n;
    int i, base = fdb_nfds();
    nvmpictx *c = create(0, 0, 4);

    REQUIRE(c);
    fdb_hold(c, b, 2, 320, 240, 1);
    REQUIRE(nvmpi_decoder_flush(c) == 0);
    for (i = 0; i < 2; i++)
        REQUIRE(fdb_check(&b[i]) == 0);
    for (i = 0; i < 2; i++) {            /* new IDR, 2 free buffers left  */
        REQUIRE(fdb_step(c, &n, i == 0 ? 320 : 0, 240, 50 + i) == 0);
        fdb_give(&n);
    }
    fdb_give(&b[0]);                     /* borrowed before the flush     */
    fdb_give(&b[1]);
    for (i = 0; i < 6; i++) {            /* all 4 buffers circulate       */
        REQUIRE(fdb_step(c, &n, 0, 240, 60 + i) == 0);
        REQUIRE(fdb_check(&n) == 0);
        fdb_give(&n);
    }
    REQUIRE(nvmpi_decoder_close(c) == 0);
    REQUIRE(fdb_nfds() == base);
    return 0;
}

/* Allocation failure while the new generation is built during a resolution
 * change, with frames borrowed: whatever the new pool ends up (partial or
 * empty), the old borrows stay intact, close succeeds and the late releases
 * are clean. The fault number is chosen in k_cases to land in the rebuild
 * (allocator calls are counted per surface; see the comment there). */
static int res_change_alloc_fault(void)
{
    struct fdb old[2], nw;
    int i, base = fdb_nfds();
    nvmpictx *c = create(0, 0, 4);
    int got;

    REQUIRE(c);
    fdb_hold(c, old, 2, 320, 240, 1);
    got = fdb_step(c, &nw, 640, 360, 10);
    printf("    new frame after faulted rebuild: %s\n", got == 0 ? "yes" : "no");
    if (got == 0) {
        REQUIRE(nw.w == 640 && nw.pitch >= 640);
        fdb_give(&nw);
    }
    for (i = 0; i < 2; i++)
        REQUIRE(fdb_check(&old[i]) == 0);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    for (i = 0; i < 2; i++)
        REQUIRE(fdb_check(&old[i]) == 0);
    fdb_give(&old[0]);
    fdb_give(&old[1]);
    REQUIRE(fdb_nfds() == base);
    return 0;
}

#endif /* NVMPI_TEST_DEC_MOCK_FD_H */
