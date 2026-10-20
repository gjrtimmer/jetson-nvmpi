/*
 * jpeg_fd_case.h — hardware cases for the JPEG DMA-BUF output API
 * (nvmpi_create_jpeg_decoder_fmt / nvmpi_jpeg_decoder_get_frame_fd).
 *
 * Included by test_api.c (uses its strict_mode). The cases need a real NVJPG
 * engine: on stub/mock builds there is no /dev/nvhost-nvjpg and the cases
 * report SKIP (exit 77). With
 * NVMPI_TEST_STRICT=1 (real hardware) they always run and a NULL create is
 * a failure.
 * The mock has no NvJPEGDecoder model; that is tracked separately.
 */
#ifndef NVMPI_TEST_JPEG_FD_CASE_H
#define NVMPI_TEST_JPEG_FD_CASE_H

#include "jpeg_fd_data.h"
#include <dirent.h>

#define JFD_W 33
#define JFD_H 17

/* Resolved from libnvbufsurface at run time (a dependency of libnvmpi).
 * Weak: NULL on builds without the NvUtils API, then the registration
 * check is skipped. */
extern int NvBufSurfaceFromFd(int fd, void **buf) __attribute__((weak));

#define JFD_FAIL(n, what) do { fprintf(stderr, "jpeg_fd FAIL (exit %d) %s:%d: %s\n", (n), __func__, __LINE__, what); return (n); } while (0)

static int jfd_open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    while (readdir(d)) n++;
    closedir(d);
    return n;
}

static int jfd_put(nvmpictx *ctx, unsigned long pts)
{
    nvPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.payload = (unsigned char *)jpeg_fd_test_jpg;
    pkt.payload_size = jpeg_fd_test_jpg_len;
    pkt.pts = pts;
    return nvmpi_jpeg_decoder_put_packet(ctx, &pkt);
}

/* Print one frame plus the expectation (diagnosing blind hardware runs). */
static void jfd_dump(const char *why, const nvmpi_fd_frame *f, nvPixFormat fmt)
{
    fprintf(stderr, "jpeg_fd FAIL: %s\n"
            "  got:  struct_size=%u fd=%d %ux%u format=%d planes=%u size=%llu flags=0x%x ts=%lld\n"
            "        pitch=[%u,%u,%u] offset=[%u,%u,%u]\n"
            "  want: %ux%u format=%d planes=%u fd>=0 size>0 flags&1 pitch[i]>=row bytes "
            "offset[i]<size, offsets increasing\n",
            why, f->struct_size, f->fd, f->width, f->height, (int)f->format,
            f->num_planes, (unsigned long long)f->size, f->flags, (long long)f->timestamp,
            f->pitch[0], f->pitch[1], f->pitch[2], f->offset[0], f->offset[1], f->offset[2],
            JFD_W, JFD_H, (int)fmt, fmt == NV_PIX_NV12 ? 2u : 3u);
}

/* Geometry + registration checks of one borrowed frame; 0 = ok. Width and
 * height are the DISPLAYED size; planes may be larger (pitch/offset rule). */
static int jfd_check(const nvmpi_fd_frame *f, nvPixFormat fmt)
{
    unsigned want = (fmt == NV_PIX_NV12) ? 2 : 3;
    if (f->width != JFD_W || f->height != JFD_H) { jfd_dump("width/height", f, fmt); return 1; }
    if (f->format != fmt) { jfd_dump("format", f, fmt); return 1; }
    if (f->num_planes != want) { jfd_dump("num_planes", f, fmt); return 2; }
    if (f->fd < 0) { jfd_dump("fd", f, fmt); return 2; }
    if (f->size == 0) { jfd_dump("size", f, fmt); return 2; }
    if (!(f->flags & NVMPI_FD_FRAME_FULL_RANGE)) { jfd_dump("flags full-range", f, fmt); return 3; }
    for (unsigned i = 0; i < want; i++) {
        unsigned w = (i == 0) ? JFD_W : (fmt == NV_PIX_NV12 ? (JFD_W + 1) : (JFD_W + 1) / 2);
        if (f->pitch[i] < w) { jfd_dump("pitch below row bytes", f, fmt); return 4; }
        if (f->offset[i] >= f->size) { jfd_dump("offset outside size", f, fmt); return 5; }
        if (i && f->offset[i] <= f->offset[i - 1]) { jfd_dump("offsets not increasing", f, fmt); return 6; }
    }
    if (NvBufSurfaceFromFd) {
        void *surf = NULL;
        /* Fails for dup'd fds: proves the fd is the registered original. */
        if (NvBufSurfaceFromFd(f->fd, &surf) < 0 || !surf) {
            jfd_dump("NvBufSurfaceFromFd (fd not registered)", f, fmt);
            return 7;
        }
    }
    return 0;
}

/* The mock build links the real NvJPEGDecoder sample class against a system
 * libjpeg, so create succeeds there but decoding cannot work. Without the
 * NVJPG engine node (and not strict) the cases are skipped up front. */
static nvmpictx *jfd_create(nvPixFormat fmt, int pool)
{
    if (!strict_mode && access("/dev/nvhost-nvjpg", F_OK) != 0)
        return NULL;
    return nvmpi_create_jpeg_decoder_fmt(fmt, pool);
}

static int jfd_one(nvPixFormat fmt)
{
    nvmpictx *ctx = jfd_create(fmt, 4);
    if (!ctx) return strict_mode ? 1 : 77;
    nvmpi_fd_frame f;
    nvmpi_frame_release_cb rel = NULL;
    void *op = NULL;
    memset(&f, 0, sizeof(f));
    f.struct_size = sizeof(f);
    if (jfd_put(ctx, 42) != 0) JFD_FAIL(1, "jfd_put(ctx, 42) != 0");
    if (nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, true) != 0) JFD_FAIL(1, "nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, true) != 0");
    if (jfd_check(&f, fmt) != 0) return 1;
    if (f.timestamp != 42) { fprintf(stderr, "jpeg_fd FAIL: timestamp %lld want 42\n", (long long)f.timestamp); return 1; }
    rel(op);
    /* Nothing left: -1, and bad arguments are INVAL. */
    if (nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, false) != -1) JFD_FAIL(1, "nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, false) != -1");
    if (nvmpi_jpeg_decoder_get_frame_fd(ctx, NULL, &rel, &op, false) != NVMPI_ERR_INVAL) JFD_FAIL(1, "nvmpi_jpeg_decoder_get_frame_fd(ctx, NULL, &rel, &op, false) != NVMPI_ERR_INVAL");
    return nvmpi_jpeg_decoder_close(ctx) == 0 ? 0 : 1;
}

static int test_jpeg_fd_nv12(void) { return jfd_one(NV_PIX_NV12); }
static int test_jpeg_fd_yuv420(void) { return jfd_one(NV_PIX_YUV420); }

/* Borrow, close, use the fd (still registered), release; then the reverse
 * order (release, close). close must succeed with a borrow outstanding. */
static int test_jpeg_fd_close_order(void)
{
    nvmpictx *ctx = jfd_create(NV_PIX_NV12, 0);
    if (!ctx) return strict_mode ? 1 : 77;
    nvmpi_fd_frame f;
    nvmpi_frame_release_cb rel = NULL;
    void *op = NULL;
    memset(&f, 0, sizeof(f));
    f.struct_size = sizeof(f);
    if (jfd_put(ctx, 1) != 0) JFD_FAIL(1, "jfd_put(ctx, 1) != 0");
    if (nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, true) != 0) JFD_FAIL(1, "nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, true) != 0");
    if (nvmpi_jpeg_decoder_close(ctx) != 0) JFD_FAIL(1, "nvmpi_jpeg_decoder_close(ctx) != 0");
    if (jfd_check(&f, NV_PIX_NV12) != 0) JFD_FAIL(2, "jfd_check after close");   /* valid after close */
    rel(op);

    ctx = jfd_create(NV_PIX_YUV420, 2);
    if (!ctx) JFD_FAIL(1, "!ctx");
    f.struct_size = sizeof(f);
    if (jfd_put(ctx, 2) != 0) JFD_FAIL(1, "jfd_put(ctx, 2) != 0");
    if (nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, true) != 0) JFD_FAIL(1, "nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, true) != 0");
    rel(op);
    return nvmpi_jpeg_decoder_close(ctx) == 0 ? 0 : 1;
}

/* 200 frames through a pool of 3: pool exhaustion gives EAGAIN (never a
 * crash) and the process fd count stays flat (no fd leak per frame). */
static int test_jpeg_fd_loop_fd_count(void)
{
    nvmpictx *ctx = jfd_create(NV_PIX_NV12, 3);
    if (!ctx) return strict_mode ? 1 : 77;
    nvmpi_fd_frame f;
    nvmpi_frame_release_cb rel = NULL;
    void *op = NULL;
    int base = -1;
    for (int i = 0; i < 200; i++) {
        memset(&f, 0, sizeof(f));
        f.struct_size = sizeof(f);
        if (jfd_put(ctx, (unsigned long)i) != 0) JFD_FAIL(1, "jfd_put(ctx, (unsigned long)i) != 0");
        if (nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, true) != 0) JFD_FAIL(1, "nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, true) != 0");
        if (jfd_check(&f, NV_PIX_NV12) != 0) JFD_FAIL(2, "jfd_check in loop");
        rel(op);
        if (i == 4) base = jfd_open_fds();   /* after warm-up */
    }
    int end = jfd_open_fds();
    if (base < 0 || end != base) JFD_FAIL(3, "base < 0 || end != base");

    /* Hold all 3 borrows: the 4th put must report EAGAIN, not consume. */
    void *ops[3]; nvmpi_frame_release_cb rels[3];
    for (int i = 0; i < 3; i++) {
        f.struct_size = sizeof(f);
        if (jfd_put(ctx, 1000 + i) != 0) JFD_FAIL(1, "jfd_put(ctx, 1000 + i) != 0");
        if (nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rels[i], &ops[i], true) != 0) JFD_FAIL(1, "nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rels[i], &ops[i], true) != 0");
    }
    int eagain = jfd_put(ctx, 2000) == NVMPI_ERR_EAGAIN;
    for (int i = 0; i < 3; i++) rels[i](ops[i]);
    if (!eagain) JFD_FAIL(4, "!eagain");
    return nvmpi_jpeg_decoder_close(ctx) == 0 ? 0 : 1;
}

#endif
