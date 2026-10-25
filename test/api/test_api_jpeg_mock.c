/*
 * test_api_jpeg_mock.c - the JPEG DMA-BUF output API (issue #4) against the
 * NvJPEGDecoder model (mock/mock_jpegdec.cpp, issue #128). MOCK-ONLY: it
 * needs the mock libs. The hardware cases in test_api.c (jpeg_fd_case.h)
 * stay the source of truth for the NVJPG engine itself.
 *
 *   geometry_*      displayed size, pitch/offset/size, fd registration
 *   pixels_*        the VIC-converted pool surface holds the mock pattern,
 *                   cropped to the displayed size (padded decoder output)
 *   release_*       borrowed fds survive close; release after close, from
 *                   another thread, queued frames dropped at close
 *   resolution_*    size change while frames are borrowed; reverse release
 *   eagain_*        full pool: EAGAIN, nothing consumed
 *   fd_*            descriptor count stays flat (200 frames, size churn)
 *   reject_*        progressive/gray/garbage fail, decoder stays usable
 *   fault_*         allocation failures end in a clean error path
 *   legacy_*        the copy-out (non-fd) JPEG decoder still works
 *   enc_*           JPEG encoder put_frame plane size checks (#151)
 *   encfd_*         JPEG encoder put_frame_fd (#158, jpeg_mock_encfd.h)
 *
 * Each case runs in a forked child under alarm() with its stderr captured
 * (case_log.h): an unexpected [libnvmpi][..][E] line fails the case, a
 * sanitizer report is always shown. Cases that fail on purpose pass a
 * never-firing fault spec so their errors count as expected.
 */
#include "jpeg_mock_util.h"
#include <pthread.h>

#define CASE_TIMEOUT_S 60

/* ---- geometry / pixels ------------------------------------------------- */

/* a = nvPixFormat, b = decoder pad, c = also check the pixels. */
static int geom(int fmt, int pad, int pixels)
{
    struct img a = img_a();
    struct borrow b;
    nvmpictx *c;

    set_pad(pad);
    c = nvmpi_create_jpeg_decoder_fmt((nvPixFormat)fmt, 4);
    REQUIRE(c);
    REQUIRE(put(c, &a, 42) == 0);
    REQUIRE(take(c, &b, true) == 0);
    REQUIRE(b.f.timestamp == 42);
    REQUIRE(check_frame(&b.f, (nvPixFormat)fmt, a.w, a.h, pixels) == 0);
    give(&b);
    REQUIRE(take(c, &b, false) == -1);               /* nothing left      */
    REQUIRE(nvmpi_jpeg_decoder_close(c) == 0);
    return 0;
}

/* ---- lifetime ---------------------------------------------------------- */

/* Borrow, close, use the fd and its pixels, release; then release first and
 * close second. Every descriptor is gone after the last release. */
static int release_after_close(int fmt, int unused_b, int unused_c)
{
    struct img a = img_a();
    struct borrow b;
    int base = nfds(), i;
    nvmpictx *c;

    (void)unused_b; (void)unused_c;
    set_pad(0);
    for (i = 0; i < 2; i++) {
        c = nvmpi_create_jpeg_decoder_fmt((nvPixFormat)fmt, 2);
        REQUIRE(c);
        REQUIRE(put(c, &a, 1) == 0);
        REQUIRE(take(c, &b, true) == 0);
        if (i == 0) {
            REQUIRE(nvmpi_jpeg_decoder_close(c) == 0);   /* borrow open   */
            REQUIRE(check_frame(&b.f, (nvPixFormat)fmt, a.w, a.h, 1) == 0);
            give(&b);
        } else {
            give(&b);
            REQUIRE(nvmpi_jpeg_decoder_close(c) == 0);
        }
        REQUIRE(nfds() == base);         /* pool destroyed with last ref  */
    }
    return 0;
}

/* Frames decoded but never borrowed are dropped by close: no fd survives. */
static int release_queued_at_close(int fmt, int b_, int c_)
{
    struct img a = img_a();
    int base = nfds();
    nvmpictx *c;

    (void)b_; (void)c_;
    c = nvmpi_create_jpeg_decoder_fmt((nvPixFormat)fmt, 4);
    REQUIRE(c);
    REQUIRE(put(c, &a, 1) == 0 && put(c, &a, 2) == 0 && put(c, &a, 3) == 0);
    REQUIRE(nvmpi_jpeg_decoder_close(c) == 0);
    REQUIRE(nfds() == base);
    return 0;
}

struct rel_arg { struct borrow *b; int n; };

static void *rel_thread(void *p)
{
    struct rel_arg *r = p;
    int i;

    for (i = 0; i < r->n; i++)
        give(&r->b[i]);
    return NULL;
}

/* Release comes from a foreign thread, after close (TSan: release vs the
 * context teardown on the main thread). */
static int release_other_thread(int fmt, int b_, int c_)
{
    struct img a = img_a();
    struct borrow b[3];
    struct rel_arg ra = { b, 3 };
    int base = nfds(), i;
    pthread_t t;
    nvmpictx *c;

    (void)b_; (void)c_;
    c = nvmpi_create_jpeg_decoder_fmt((nvPixFormat)fmt, 3);
    REQUIRE(c);
    for (i = 0; i < 3; i++) {
        REQUIRE(put(c, &a, (unsigned long)i) == 0);
        REQUIRE(take(c, &b[i], true) == 0);
    }
    REQUIRE(nvmpi_jpeg_decoder_close(c) == 0);
    REQUIRE(pthread_create(&t, NULL, rel_thread, &ra) == 0);
    REQUIRE(pthread_join(t, NULL) == 0);
    REQUIRE(nfds() == base);
    return 0;
}

/* 33x17, borrow; 64x48, borrow; both stay valid and keep their pixels while
 * the decoder moved on; release in reverse order; then back to 33x17. */
static int resolution_borrowed(int fmt, int pad, int c_)
{
    struct img a = img_a(), bi = img_b();
    struct borrow fa, fb, fc;
    int base = nfds();
    nvmpictx *c;

    (void)c_;
    set_pad(pad);
    c = nvmpi_create_jpeg_decoder_fmt((nvPixFormat)fmt, 3);
    REQUIRE(c);
    REQUIRE(put(c, &a, 1) == 0 && take(c, &fa, true) == 0);
    REQUIRE(put(c, &bi, 2) == 0 && take(c, &fb, true) == 0);   /* new gen */
    REQUIRE(fa.f.fd != fb.f.fd);
    REQUIRE(check_frame(&fa.f, (nvPixFormat)fmt, a.w, a.h, 1) == 0);
    REQUIRE(check_frame(&fb.f, (nvPixFormat)fmt, bi.w, bi.h, 1) == 0);
    give(&fb);                                       /* reverse order     */
    REQUIRE(check_frame(&fa.f, (nvPixFormat)fmt, a.w, a.h, 1) == 0);
    REQUIRE(put(c, &a, 3) == 0 && take(c, &fc, true) == 0);    /* new gen */
    REQUIRE(check_frame(&fc.f, (nvPixFormat)fmt, a.w, a.h, 1) == 0);
    REQUIRE(nvmpi_jpeg_decoder_close(c) == 0);
    give(&fa);
    REQUIRE(check_frame(&fc.f, (nvPixFormat)fmt, a.w, a.h, 1) == 0);
    give(&fc);
    REQUIRE(nfds() == base);
    return 0;
}

/* ---- pool exhaustion / fd stability ------------------------------------ */

/* Pool of 3: three queued frames fill it (EAGAIN, nothing consumed, no
 * extra frame appears); one release frees exactly one slot. */
static int eagain_full_pool(int fmt, int b_, int c_)
{
    struct img a = img_a();
    struct borrow b[3], x;
    nvmpictx *c;
    int i;

    (void)b_; (void)c_;
    c = nvmpi_create_jpeg_decoder_fmt((nvPixFormat)fmt, 3);
    REQUIRE(c);
    for (i = 0; i < 3; i++)
        REQUIRE(put(c, &a, (unsigned long)i) == 0);  /* queued, unborrowed */
    for (i = 0; i < 5; i++)
        REQUIRE(put(c, &a, 99) == NVMPI_ERR_EAGAIN);
    for (i = 0; i < 3; i++) {
        REQUIRE(take(c, &b[i], true) == 0);
        REQUIRE(b[i].f.timestamp == i);              /* FIFO, no pts 99   */
    }
    REQUIRE(take(c, &x, false) == -1);               /* EAGAIN made none  */
    REQUIRE(put(c, &a, 99) == NVMPI_ERR_EAGAIN);     /* all three borrowed */
    give(&b[1]);
    REQUIRE(put(c, &a, 7) == 0);
    REQUIRE(put(c, &a, 8) == NVMPI_ERR_EAGAIN);
    REQUIRE(take(c, &x, true) == 0 && x.f.timestamp == 7);
    give(&x);
    give(&b[0]);
    give(&b[2]);
    return nvmpi_jpeg_decoder_close(c) == 0 ? 0 : 1;
}

/* 200 frames through a pool of 3: the fd count is flat after warm-up. */
static int fd_stable_200(int fmt, int b_, int c_)
{
    struct img a = img_a();
    struct borrow b;
    int base = nfds(), warm = -1, i;
    nvmpictx *c;

    (void)b_; (void)c_;
    c = nvmpi_create_jpeg_decoder_fmt((nvPixFormat)fmt, 3);
    REQUIRE(c);
    for (i = 0; i < 200; i++) {
        REQUIRE(put(c, &a, (unsigned long)i) == 0);
        REQUIRE(take(c, &b, true) == 0);
        REQUIRE(check_frame(&b.f, (nvPixFormat)fmt, a.w, a.h, 0) == 0);
        give(&b);
        if (i == 4)
            warm = nfds();
        REQUIRE(i < 5 || nfds() == warm);
    }
    REQUIRE(nvmpi_jpeg_decoder_close(c) == 0);
    REQUIRE(nfds() == base);
    return 0;
}

/* Alternating sizes: a new pool generation per frame; none may leak. */
static int fd_size_churn(int fmt, int b_, int c_)
{
    struct img im[2] = { img_a(), img_b() };
    struct borrow b;
    int base = nfds(), i;
    nvmpictx *c;

    (void)b_; (void)c_;
    c = nvmpi_create_jpeg_decoder_fmt((nvPixFormat)fmt, 2);
    REQUIRE(c);
    for (i = 0; i < 60; i++) {
        REQUIRE(put(c, &im[i & 1], (unsigned long)i) == 0);
        REQUIRE(take(c, &b, true) == 0);
        REQUIRE(check_frame(&b.f, (nvPixFormat)fmt, im[i & 1].w,
                            im[i & 1].h, 0) == 0);
        give(&b);
    }
    REQUIRE(nvmpi_jpeg_decoder_close(c) == 0);
    REQUIRE(nfds() == base);
    return 0;
}

#include "jpeg_mock_misc.h"
#include "jpeg_mock_enc.h"
#include "jpeg_mock_encfd.h"
#include "jpeg_mock_encfd_mix.h"

/* ---- table and runner --------------------------------------------------- */

struct tcase {
    const char *name;
    const char *fault;                   /* NVMPI_MOCK_FAULT / expected [E] */
    int (*fn)(int, int, int);
    int a, b, c;
};

#define NV12 NV_PIX_NV12
#define I420 NV_PIX_YUV420

static const struct tcase k_cases[] = {
    { "geometry_nv12_pad16", NULL, geom, NV12, 16, 0 },
    { "geometry_yuv420_pad16", NULL, geom, I420, 16, 0 },
    { "geometry_nv12_exact", NULL, geom, NV12, 0, 0 },
    { "geometry_yuv420_pad64", NULL, geom, I420, 64, 0 },
    { "pixels_nv12_exact", NULL, geom, NV12, 0, 1 },
    { "pixels_yuv420_exact", NULL, geom, I420, 0, 1 },
    { "pixels_nv12_pad16", NULL, geom, NV12, 16, 1 },
    { "pixels_yuv420_pad16", NULL, geom, I420, 16, 1 },
    { "release_after_close_nv12", NULL, release_after_close, NV12, 0, 0 },
    { "release_after_close_yuv420", NULL, release_after_close, I420, 0, 0 },
    { "release_queued_at_close", NULL, release_queued_at_close, NV12, 0, 0 },
    { "release_other_thread", NULL, release_other_thread, NV12, 0, 0 },
    { "resolution_borrowed_nv12", NULL, resolution_borrowed, NV12, 16, 0 },
    { "resolution_borrowed_yuv420", NULL, resolution_borrowed, I420, 0, 0 },
    { "eagain_full_pool", NULL, eagain_full_pool, NV12, 0, 0 },
    { "fd_stable_200_nv12", NULL, fd_stable_200, NV12, 0, 0 },
    { "fd_stable_200_yuv420", NULL, fd_stable_200, I420, 0, 0 },
    { "fd_size_churn", NULL, fd_size_churn, I420, 0, 0 },
    { "reject_inputs", NEVER_FIRES, reject_inputs, NV12, 0, 0 },
    { "fault_alloc_decoder", "alloc@1", fault_alloc, NV12, 1, 0 },
    { "fault_alloc_pool_first", "alloc@2", fault_alloc, NV12, 2, 0 },
    { "fault_alloc_pool_mid", "alloc@3", fault_alloc, I420, 3, 0 },
    { "legacy_decode", NEVER_FIRES, legacy_decode, 0, 0, 0 },
    { "legacy_size_guard", NEVER_FIRES, legacy_size_guard, 0, 0, 0 },
    { "enc_size_refused", NEVER_FIRES, jpeg_enc_size_refused, 0, 0, 0 },
    { "enc_size_legacy", NEVER_FIRES, jpeg_enc_size_legacy, 0, 0, 0 },
    { "enc_linesize_short", NEVER_FIRES, jpeg_enc_linesize_short, 0, 0, 0 },
    { "encfd_direct", NULL, jpegfd_direct, 0, 0, 0 },
    { "encfd_vic_nv12", NULL, jpegfd_vic, NV12, 640, 360 },
    { "encfd_vic_nv12_padded", NULL, jpegfd_vic, NV12, 642, 362 },
    { "encfd_vic_yuv420_padded", NULL, jpegfd_vic, I420, 642, 362 },
    { "encfd_vic_yuv420_larger", NULL, jpegfd_vic, I420, 704, 400 },
    { "encfd_odd_nv12", NULL, jpegfd_odd, NV12, 0, 0 },
    { "encfd_odd_yuv420", NULL, jpegfd_odd, I420, 0, 0 },
    { "encfd_dec_fd2_nv12", NULL, jpegfd_dec_fd2, NV12, 0, 0 },
    { "encfd_dec_fd2_yuv420", NULL, jpegfd_dec_fd2, I420, 0, 0 },
    { "encfd_bad_fd_nv12", NEVER_FIRES, jpegfd_bad_fd, NV12, 0, 0 },
    { "encfd_bad_fd_yuv420", NEVER_FIRES, jpegfd_bad_fd, I420, 0, 0 },
    { "encfd_bad_desc_nv12", NEVER_FIRES, jpegfd_bad_desc, NV12, 0, 0 },
    { "encfd_bad_desc_yuv420", NEVER_FIRES, jpegfd_bad_desc, I420, 0, 0 },
    { "encfd_eos", NULL, jpegfd_eos, I420, 0, 0 },
    { "encfd_mixed", NULL, jpegfd_mixed, 0, 0, 0 },
    { "encfd_fd_stable", NULL, jpegfd_fd_stable, 0, 0, 0 },
};

static int run_body(void *arg)
{
    const struct tcase *t = arg;

    return t->fn(t->a, t->b, t->c);
}

int main(int argc, char **argv)
{
    size_t i;
    int fails = 0, ran = 0;

    printf("=== libnvmpi JPEG fd output vs NvJPEGDecoder mock ===\n");
    for (i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        if (argc > 1 && strstr(k_cases[i].name, argv[1]) == NULL)
            continue;                    /* optional name filter           */
        fails += case_log_run(k_cases[i].name, k_cases[i].fault, 1, 0,
                              CASE_TIMEOUT_S, run_body, (void *)&k_cases[i]);
        ran++;
    }
    printf("\n%d cases, %d failed\n", ran, fails);
    return fails ? 1 : 0;
}
