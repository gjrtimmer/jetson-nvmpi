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

/* Geometry + registration checks of one borrowed frame; 0 = ok. */
static int jfd_check(const nvmpi_fd_frame *f, nvPixFormat fmt)
{
    unsigned want = (fmt == NV_PIX_NV12) ? 2 : 3;
    if (f->width != JFD_W || f->height != JFD_H || f->format != fmt) return 1;
    if (f->num_planes != want || f->fd < 0 || f->size == 0) return 2;
    if (!(f->flags & NVMPI_FD_FRAME_FULL_RANGE)) return 3;
    for (unsigned i = 0; i < want; i++) {
        unsigned w = (i == 0) ? JFD_W : (fmt == NV_PIX_NV12 ? (JFD_W + 1) : (JFD_W + 1) / 2);
        if (f->pitch[i] < w) return 4;
        if (f->offset[i] >= f->size) return 5;
        if (i && f->offset[i] <= f->offset[i - 1]) return 6;
    }
    if (NvBufSurfaceFromFd) {
        void *surf = NULL;
        /* Fails for dup'd fds: proves the fd is the registered original. */
        if (NvBufSurfaceFromFd(f->fd, &surf) < 0 || !surf) return 7;
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
    if (jfd_put(ctx, 42) != 0) return 1;
    if (nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, true) != 0) return 1;
    if (jfd_check(&f, fmt) != 0 || f.timestamp != 42) return 1;
    rel(op);
    /* Nothing left: -1, and bad arguments are INVAL. */
    if (nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, false) != -1) return 1;
    if (nvmpi_jpeg_decoder_get_frame_fd(ctx, NULL, &rel, &op, false) != NVMPI_ERR_INVAL) return 1;
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
    if (jfd_put(ctx, 1) != 0) return 1;
    if (nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, true) != 0) return 1;
    if (nvmpi_jpeg_decoder_close(ctx) != 0) return 1;
    if (jfd_check(&f, NV_PIX_NV12) != 0) return 2;   /* valid after close */
    rel(op);

    ctx = jfd_create(NV_PIX_YUV420, 2);
    if (!ctx) return 1;
    f.struct_size = sizeof(f);
    if (jfd_put(ctx, 2) != 0) return 1;
    if (nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, true) != 0) return 1;
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
        if (jfd_put(ctx, (unsigned long)i) != 0) return 1;
        if (nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rel, &op, true) != 0) return 1;
        if (jfd_check(&f, NV_PIX_NV12) != 0) return 2;
        rel(op);
        if (i == 4) base = jfd_open_fds();   /* after warm-up */
    }
    int end = jfd_open_fds();
    if (base < 0 || end != base) return 3;

    /* Hold all 3 borrows: the 4th put must report EAGAIN, not consume. */
    void *ops[3]; nvmpi_frame_release_cb rels[3];
    for (int i = 0; i < 3; i++) {
        f.struct_size = sizeof(f);
        if (jfd_put(ctx, 1000 + i) != 0) return 1;
        if (nvmpi_jpeg_decoder_get_frame_fd(ctx, &f, &rels[i], &ops[i], true) != 0) return 1;
    }
    int eagain = jfd_put(ctx, 2000) == NVMPI_ERR_EAGAIN;
    for (int i = 0; i < 3; i++) rels[i](ops[i]);
    if (!eagain) return 4;
    return nvmpi_jpeg_decoder_close(ctx) == 0 ? 0 : 1;
}

#endif
