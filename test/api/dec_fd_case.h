/*
 * dec_fd_case.h - hardware case for the decoder zero-copy lifetime (#137),
 * included by test_api.c.
 *
 * Two embedded H.264 pictures (320x240, then 640x360) are fed to one decoder
 * while two 320x240 frames are borrowed through nvmpi_decoder_get_frame_fd:
 *   - the resolution change must rebuild the frame pool at the new size
 *     (the new frame reports 640x360 over a pitch >= 640) without waiting
 *     for, or destroying, the borrowed buffers;
 *   - close with borrows outstanding must not wait, and the borrowed fds
 *     must stay valid until released (release after close);
 *   - after the last release no descriptor is left behind.
 * Validity of a borrowed dmabuf is probed with fcntl + lseek(SEEK_END) (the
 * dmabuf size), which needs no NVIDIA headers. Needs a real NVDEC (or the
 * mock); outside strict mode a missing device or frame is a skip, with
 * NVMPI_TEST_STRICT=1 every shortfall is a failure.
 */
#ifndef NVMPI_TEST_DEC_FD_CASE_H
#define NVMPI_TEST_DEC_FD_CASE_H

#include "dec_fd_fixture.h"
#include "fd_snap.h"

#include <fcntl.h>
#include <time.h>

#define DECFD_FAIL(what) do { fprintf(stderr, "dec_fd FAIL %s:%d: %s\n", __func__, __LINE__, (what)); return 1; } while (0)
/* close() with 2 borrows must take about as long as without (tens of ms to
 * a second on hardware); a wait on borrows would show up as >= several s. */
#define DECFD_CLOSE_MAX_MS 3000

struct decfd_b {                         /* one borrowed frame            */
    int fd, w, h, pitch;
    int64_t ts;
    nvmpi_frame_release_cb rel;
    void *op;
};

static long decfd_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* fd is still open and still a buffer big enough for pitch x height. */
static int decfd_alive(const struct decfd_b *b)
{
    off_t sz;

    if (fcntl(b->fd, F_GETFD) < 0)
        return 0;
    sz = lseek(b->fd, 0, SEEK_END);
    return sz >= (off_t)b->pitch * b->h;
}

static void decfd_give(struct decfd_b *b)
{
    if (b->rel)
        b->rel(b->op);
    b->rel = NULL;
}

/* Feed one unit and borrow its frame; 0 = got one. */
static int decfd_step(nvmpictx *ctx, const unsigned char *au, size_t n,
                      unsigned long pts, struct decfd_b *b)
{
    nvPacket pkt;

    memset(&pkt, 0, sizeof(pkt));
    pkt.payload = (unsigned char *)au;
    pkt.payload_size = n;
    pkt.pts = pts;
    if (nvmpi_decoder_put_packet(ctx, &pkt) < 0)
        return -2;
    memset(b, 0, sizeof(*b));
    return nvmpi_decoder_get_frame_fd(ctx, &b->fd, &b->w, &b->h, &b->pitch,
                                      &b->ts, &b->rel, &b->op, true);
}

/* Decoder for these cases: pool of 4, no reordering, generous first wait. */
static nvmpictx *decfd_create(void)
{
    nvDecParam param;

    dec_param_init(&param, NV_VIDEO_CodingH264);
    param.pixFormat = NV_PIX_NV12;
    param.frame_pool_size = 4;
    param.disable_dpb = 1;       /* no reordering: one picture in, one out */
    param.wait_timeout = 2000;   /* ms; the first frame pays NVDEC start-up */
    return nvmpi_create_decoder(&param);
}

/* Warm-up: create -> decode one frame of EACH stream -> close. NVIDIA
 * libraries open descriptors lazily on first use (nvmap, VIC/NvBufSurface
 * sessions, libv4l2 plugin, ...) and keep them for the process lifetime, so
 * the descriptor baseline is taken only after this cycle. 0 ok, 77 skip. */
static int decfd_warm(void)
{
    struct decfd_b b;
    nvmpictx *ctx = decfd_create();

    if (!ctx) return strict_mode ? 1 : 77;
    if (decfd_step(ctx, decfd_a, decfd_a_len, 1, &b) != 0) {
        nvmpi_decoder_close(ctx);
        return strict_mode ? 1 : 77;
    }
    decfd_give(&b);
    if (decfd_step(ctx, decfd_b, decfd_b_len, 2, &b) == 0)
        decfd_give(&b);
    return nvmpi_decoder_close(ctx) == 0 ? 0 : 1;
}

/* One run: borrow 2 frames of stream A, then (change != 0) switch to stream B
 * or (control) feed A once more; close with the 2 old frames still borrowed;
 * release them. Without the change nothing is rebuilt, so a leak that shows
 * only with change != 0 belongs to the resolution-change path. Checks the
 * rebuild, the borrows' survival and the descriptor set against `base`.
 * 0 ok, 1 fail, 77 skip. */
static int decfd_run(int change, const struct fd_snap *base)
{
    nvmpictx *ctx = decfd_create();
    const unsigned char *nxt = change ? decfd_b : decfd_a;
    size_t nxt_len = change ? decfd_b_len : decfd_a_len;
    int nw_w = change ? DECFD_B_W : DECFD_A_W;
    int nw_h = change ? DECFD_B_H : DECFD_A_H;
    struct decfd_b old[2], nw;
    int i;
    long t0;

    if (!ctx) return strict_mode ? 1 : 77;

    for (i = 0; i < 2; i++) {    /* borrow two 320x240 frames */
        if (decfd_step(ctx, decfd_a, decfd_a_len, 1 + i, &old[i]) != 0) {
            nvmpi_decoder_close(ctx);
            if (i) decfd_give(&old[0]);
            if (strict_mode) DECFD_FAIL("no 320x240 frame decoded");
            return 77;
        }
        if (old[i].w != DECFD_A_W || old[i].h != DECFD_A_H ||
            old[i].pitch < DECFD_A_W) {
            fprintf(stderr, "dec_fd: got %dx%d pitch %d\n", old[i].w,
                    old[i].h, old[i].pitch);
            DECFD_FAIL("first frame size");
        }
    }

    /* Resolution change with both borrowed. The old code never rebuilt the
     * pool (the new frame was reported 640 wide over a 320 pitch). */
    t0 = decfd_ms();
    if (decfd_step(ctx, nxt, nxt_len, 10, &nw) != 0)
        DECFD_FAIL("no frame after the second unit");
    fprintf(stderr, "dec_fd[%s]: next frame after %ld ms\n",
            change ? "change" : "control", decfd_ms() - t0);
    if (nw.w != nw_w || nw.h != nw_h || nw.pitch < nw_w) {
        fprintf(stderr, "dec_fd: got %dx%d pitch %d\n", nw.w, nw.h, nw.pitch);
        DECFD_FAIL("new frame size / pitch (pool not rebuilt)");
    }
    for (i = 0; i < 2; i++)
        if (!decfd_alive(&old[i]))
            DECFD_FAIL("old borrowed buffer lost at the resolution change");
    decfd_give(&nw);

    /* Close with the old frames still borrowed: no wait, fds stay valid. */
    t0 = decfd_ms();
    if (nvmpi_decoder_close(ctx) != 0)
        DECFD_FAIL("close");
    if (decfd_ms() - t0 > DECFD_CLOSE_MAX_MS)
        DECFD_FAIL("close waited for borrowed frames");
    for (i = 0; i < 2; i++)
        if (!decfd_alive(&old[i]))
            DECFD_FAIL("borrowed buffer lost at close");
    decfd_give(&old[0]);
    decfd_give(&old[1]);
    return fds_check(base, 0, change ? "change" : "control") ? 1 : 0;
}

static int test_decoder_res_change_borrowed(void)
{
    struct fd_snap base;
    int rc, ctl, chg;

#ifdef NVMPI_MOCK_JPEG
    /* ctest forces the mock decoder to 320x240 for the AV1 case; here the
     * mock must read the size from each SPS, like the driver. This test runs
     * in its own forked child, so the other cases keep the forced size. */
    unsetenv("NVMPI_MOCK_WIDTH");
    unsetenv("NVMPI_MOCK_HEIGHT");
#endif
    rc = decfd_warm();
    if (rc) return rc;
    fds_take(&base);             /* baseline AFTER the warm-up cycle */

    /* Control first (no resolution change), then the real sequence; both
     * always run so one log shows which path, if any, leaves descriptors. */
    ctl = decfd_run(0, &base);
    if (ctl == 77) return 77;
    chg = decfd_run(1, &base);
    if (chg == 77) return 77;
    fprintf(stderr, "dec_fd: control(no change)=%s change=%s\n",
            ctl ? "FAIL" : "ok", chg ? "FAIL" : "ok");
    if (chg) DECFD_FAIL("resolution-change sequence (see messages above)");
    if (ctl) DECFD_FAIL("control sequence leaked descriptors (see above)");
    return 0;
}

#endif /* NVMPI_TEST_DEC_FD_CASE_H */
