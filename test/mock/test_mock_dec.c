/*
 * test_mock_dec.c - decoder model of the mock (mock_dec.c), issue #99,
 * driven through the V4L2 API exactly as NvVideoDecoder does. Part of the
 * test_mock executable; a failure here is a mock bug, not a libnvmpi bug.
 *
 * Covers: first-input RESOLUTION_CHANGE and its size sources (SPS with
 * cropping, env override, default, garbage), one gray NV12 frame per VCL
 * access unit with timestamp passthrough, in-order back-pressure, EOS
 * semantics (LAST flag), mid-stream size change, flush, and the blocking
 * paths (STREAMOFF / interrupt / close wake waiters; DQBUF wait is bounded).
 */
#include "mock_dec_harness.h"

#include <poll.h>
#include <stdlib.h>

#define POLL_CID_DEVICE (V4L2_CID_MPEG_BASE + 550)
#define POLL_CID_INTR   (V4L2_CID_MPEG_BASE + 551)

static uint8_t au[2048];

#define start dh_start

static unsigned drain_frames(struct dh *d, uint32_t *last_flags)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];
    unsigned n = 0;

    while (dh_dq(d, CAP, &b, pl) == 0) {
        n++;
        if (last_flags)
            *last_flags = b.flags;
        CHECK(dh_cap_q(d, b.index) == 0);
    }
    return n;
}

static void test_frames_and_sizes(void)
{
    struct dh d;
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];
    unsigned i, frames = 0;
    uint64_t t0;
    size_t n;

    CHECK(start(&d, 1280, 720, 6, O_NONBLOCK) == 0);
    CHECK(d.w == 1280 && d.h == 720);
    /* First IDR (+ SPS/PPS) -> exactly one frame, pts carried through. */
    t0 = dh_now_ms();
    while (dh_dq(&d, CAP, &b, pl) != 0 && dh_now_ms() - t0 < 2000)
        dh_ms(1);
    CHECK(b.timestamp.tv_sec == 0 && b.timestamp.tv_usec == 1000);
    CHECK(pl[0].bytesused == 1280 * 720 && pl[1].bytesused == 1280 * 720 / 2);
    CHECK(!(b.flags & V4L2_BUF_FLAG_LAST) && b.sequence == 0);
    CHECK(dh_is_gray(&d, b.index));
    CHECK(dh_cap_q(&d, b.index) == 0);
    frames = 1;

    /* 20 P slices (one frame each, drained as we go) + a parameter-set-only
     * AU (completes, but makes no frame). */
    for (i = 0; i < 20; i++) {
        unsigned got = 0;

        n = fx_pslice(au);
        CHECK(dh_put(&d, au, n, 2000 + i) == 0);
        if (i == 7)
            CHECK(dh_put(&d, au, fx_sps(au, 1280, 720), 0) == 0);
        t0 = dh_now_ms();
        while (!got && dh_now_ms() - t0 < 2000) {
            got = drain_frames(&d, NULL);
            if (!got)
                dh_ms(1);
        }
        frames += got;
    }
    CHECK(frames == 21);                 /* the SPS-only AU made no frame */
    CHECK(drain_frames(&d, NULL) == 0);
    dh_close(&d);
}

/* Size sources: SPS (incl. cropping 1080p), env override, garbage, default. */
static void size_case(uint32_t w, uint32_t h, uint32_t ew, uint32_t eh)
{
    struct dh d;
    struct v4l2_crop c;
    char v[16];

    if (ew) {
        snprintf(v, sizeof v, "%u", ew);
        setenv("NVMPI_MOCK_WIDTH", v, 1);
        snprintf(v, sizeof v, "%u", eh);
        setenv("NVMPI_MOCK_HEIGHT", v, 1);
    }
    CHECK(start(&d, w, h, 2, O_NONBLOCK) == 0);
    CHECK(d.w == (ew ? ew : w) && d.h == (ew ? eh : h));
    memset(&c, 0, sizeof(c));
    c.type = CAP;
    CHECK(v4l2_ioctl(d.fd, VIDIOC_G_CROP, &c) == 0);
    CHECK(c.c.width == d.w && c.c.height == d.h);
    dh_close(&d);
    unsetenv("NVMPI_MOCK_WIDTH");
    unsetenv("NVMPI_MOCK_HEIGHT");
}

static void test_size_sources(void)
{
    struct dh d;
    size_t n, cut;
    unsigned i;

    size_case(1920, 1080, 0, 0);         /* 1088 coded, cropped to 1080  */
    size_case(176, 144, 0, 0);
    size_case(4096, 2160, 0, 0);
    size_case(1280, 720, 640, 360);      /* env wins over the SPS        */
    setenv("NVMPI_MOCK_WIDTH", "641", 1);  /* odd: ignored */
    setenv("NVMPI_MOCK_HEIGHT", "360", 1);
    size_case(320, 240, 0, 0);
    unsetenv("NVMPI_MOCK_WIDTH");
    unsetenv("NVMPI_MOCK_HEIGHT");

    /* No SPS at all -> 320x240 default. */
    CHECK(dh_open(&d, O_NONBLOCK, V4L2_PIX_FMT_H264) == 0);
    CHECK(dh_put(&d, au, fx_pslice(au), 1) == 0);
    CHECK(dh_wait_event(&d, 2000) == 0 && dh_arm(&d, 2) == 0);
    CHECK(d.w == 320 && d.h == 240);
    dh_close(&d);

    /* Truncated SPS at every length, then pseudo-random bytes with start
     * codes sprinkled in: never a crash, always a (default or parsed) size. */
    for (cut = 5; cut < 20; cut++) {
        CHECK(dh_open(&d, O_NONBLOCK, V4L2_PIX_FMT_H264) == 0);
        n = fx_sps(au, 640, 360);
        CHECK(dh_put(&d, au, cut < n ? cut : n, 1) == 0);
        CHECK(dh_wait_event(&d, 2000) == 0 && dh_arm(&d, 2) == 0);
        CHECK(d.w >= 16 && d.h >= 16);
        dh_close(&d);
    }
    CHECK(start(&d, 320, 240, 3, O_NONBLOCK) == 0);
    srand(1234);
    for (i = 0; i < 300; i++) {
        size_t len = 1 + (size_t)(rand() % 200), k;

        for (k = 0; k < len; k++)
            au[k] = (uint8_t)rand();
        if (i % 3 == 0 && len > 8)
            memcpy(au + len / 2, "\0\0\1\x67\x42", 5);
        CHECK(dh_put(&d, au, len, i) == 0);
        if (dh_wait_event(&d, 0) == 0) {  /* a garbage SPS parsed: re-arm */
            dh_disarm(&d);
            CHECK(dh_arm(&d, 3) == 0);
        }
        (void)drain_frames(&d, NULL);
    }
    dh_close(&d);
}

static void test_backpressure_and_eos(void)
{
    struct dh d;
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];
    uint32_t fl = 0;
    unsigned i, got = 0;

    /* Only 2 capture buffers: the 3rd and 4th AU stay queued (no output
     * completion) until a capture buffer comes back. */
    CHECK(start(&d, 640, 360, 2, O_NONBLOCK) == 0);
    for (i = 0; i < 3; i++)
        CHECK(dh_put(&d, au, fx_pslice(au), 10 + i) == 0);
    dh_ms(50);
    CHECK(dh_dq(&d, CAP, &b, pl) == 0);          /* frame 0 (the IDR)    */
    CHECK(dh_dq(&d, CAP, &b, pl) == 0);          /* frame 1              */
    CHECK(dh_dq(&d, CAP, &b, pl) != 0 && errno == EAGAIN);
    CHECK(dh_cap_q(&d, 0) == 0 && dh_cap_q(&d, 1) == 0);
    dh_ms(50);
    CHECK(drain_frames(&d, NULL) == 2);          /* frames 2 and 3       */

    /* EOS with an undelivered frame: that frame carries LAST. */
    CHECK(dh_put(&d, au, fx_pslice(au), 20) == 0);
    CHECK(dh_put(&d, au, 0, 0) == 0);            /* EOS marker           */
    dh_ms(50);
    CHECK(drain_frames(&d, &fl) == 1 && (fl & V4L2_BUF_FLAG_LAST));
    /* Then: empty queue reports EAGAIN with LAST set in the buffer. */
    CHECK(dh_dq(&d, CAP, &b, pl) != 0 && errno == EAGAIN);
    CHECK(b.flags & V4L2_BUF_FLAG_LAST);
    /* Input after EOS is dropped (no frame) until a flush. */
    CHECK(dh_put(&d, au, fx_pslice(au), 30) == 0);
    dh_ms(30);
    CHECK(drain_frames(&d, NULL) == 0);
    dh_close(&d);

    /* EOS after every frame was already delivered: EAGAIN + LAST only.
     * This device also re-announces after the flush at the end (#162). */
    setenv("NVMPI_MOCK_DEC_REANNOUNCE", "1", 1);
    CHECK(start(&d, 640, 360, 4, O_NONBLOCK) == 0);
    dh_ms(30);
    CHECK(drain_frames(&d, &fl) == 1 && !(fl & V4L2_BUF_FLAG_LAST));
    CHECK(dh_put(&d, au, 0, 0) == 0);
    dh_ms(30);
    CHECK(dh_dq(&d, CAP, &b, pl) != 0 && errno == EAGAIN);
    CHECK(b.flags & V4L2_BUF_FLAG_LAST);

    /* Flush (OUTPUT STREAMOFF/ON) re-arms the stream: a new event. */
    {
        unsigned type = OUT;

        CHECK(v4l2_ioctl(d.fd, VIDIOC_STREAMOFF, &type) == 0);
        CHECK(v4l2_ioctl(d.fd, VIDIOC_STREAMON, &type) == 0);
        d.out_used = 0;
    }
    CHECK(dh_put(&d, au, fx_idr(au, 640, 360), 40) == 0);
    CHECK(dh_wait_event(&d, 2000) == 0);
    dh_disarm(&d);                               /* CAPTURE streams: re-arm */
    CHECK(dh_arm(&d, 3) == 0);
    dh_ms(30);
    for (i = 0; i < 40 && !got; i++, dh_ms(5))
        got = drain_frames(&d, NULL);
    CHECK(got == 1);
    dh_close(&d);
    unsetenv("NVMPI_MOCK_DEC_REANNOUNCE");
}

/* Spec model (#162, NVMPI_MOCK_DEC_REANNOUNCE=0): a flush resumed at the same size raises NO new event
 * and an armed CAPTURE queue keeps producing; a different size still does. */
static void test_flush_no_reannounce(void)
{
    struct dh d;
    unsigned type = OUT;
    int got = 0, i;

    setenv("NVMPI_MOCK_DEC_REANNOUNCE", "0", 1);
    CHECK(start(&d, 640, 360, 4, O_NONBLOCK) == 0);
    dh_ms(30);
    CHECK(drain_frames(&d, NULL) == 1);
    CHECK(v4l2_ioctl(d.fd, VIDIOC_STREAMOFF, &type) == 0);
    CHECK(v4l2_ioctl(d.fd, VIDIOC_STREAMON, &type) == 0);
    d.out_used = 0;
    CHECK(dh_put(&d, au, fx_idr(au, 640, 360), 60) == 0);
    CHECK(dh_wait_event(&d, 100) != 0);          /* no new event          */
    for (i = 0; i < 40 && !got; i++, dh_ms(5))
        got = drain_frames(&d, NULL);
    CHECK(got == 1);                             /* armed queue: frame    */
    CHECK(dh_put(&d, au, fx_idr(au, 320, 240), 61) == 0);
    CHECK(dh_wait_event(&d, 2000) == 0);         /* real change: event    */
    dh_close(&d);
    unsetenv("NVMPI_MOCK_DEC_REANNOUNCE");
}

static void test_midstream_change(void)
{
    struct dh d;
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];

    CHECK(start(&d, 640, 360, 4, O_NONBLOCK) == 0);
    dh_ms(30);
    CHECK(drain_frames(&d, NULL) == 1);
    CHECK(dh_put(&d, au, fx_idr(au, 320, 240), 50) == 0);
    CHECK(dh_wait_event(&d, 2000) == 0);         /* second event         */
    dh_ms(30);
    /* Held until the application re-arms CAPTURE (no old-size frame). */
    CHECK(dh_dq(&d, CAP, &b, pl) != 0 && errno == EAGAIN);
    dh_disarm(&d);
    CHECK(dh_arm(&d, 3) == 0 && d.w == 320 && d.h == 240);
    dh_ms(30);
    CHECK(dh_dq(&d, CAP, &b, pl) == 0 && pl[0].bytesused == 320 * 240);
    CHECK(b.timestamp.tv_usec == 50);
    dh_close(&d);
}

struct waiter { struct dh *d; int ret, err; uint16_t resp; };

static void *dq_waiter(void *arg)
{
    struct waiter *w = arg;
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];

    w->ret = dh_dq(w->d, CAP, &b, pl);
    w->err = errno;
    return NULL;
}

static void *poll_waiter(void *arg)
{
    struct waiter *w = arg;
    struct v4l2_ext_control c;
    struct v4l2_ext_controls ec;
    struct { uint16_t req, resp; } dp = { POLLIN | POLLPRI, 0 };

    memset(&c, 0, sizeof(c));
    memset(&ec, 0, sizeof(ec));
    c.id = POLL_CID_DEVICE;
    c.string = (char *)&dp;
    ec.count = 1;
    ec.controls = &c;
    w->ret = v4l2_ioctl(w->d->fd, VIDIOC_S_EXT_CTRLS, &ec);
    w->resp = dp.resp;
    return NULL;
}

static void poll_intr(struct dh *d, int on)
{
    struct v4l2_ext_control c;
    struct v4l2_ext_controls ec;

    memset(&c, 0, sizeof(c));
    memset(&ec, 0, sizeof(ec));
    c.id = POLL_CID_INTR;
    c.value = on;
    ec.count = 1;
    ec.controls = &c;
    CHECK(v4l2_ioctl(d->fd, VIDIOC_S_EXT_CTRLS, &ec) == 0);
}

static void test_blocking_paths(void)
{
    struct dh d;
    struct waiter w = { &d, 0, 0, 0 };
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];
    pthread_t t;
    unsigned type = CAP;
    uint64_t t0;

    /* Default: a blocking DQBUF gives up with EAGAIN (bounded wait). */
    CHECK(start(&d, 320, 240, 2, 0) == 0);
    CHECK(dh_dq(&d, CAP, &b, pl) == 0);
    t0 = dh_now_ms();
    CHECK(dh_dq(&d, CAP, &b, pl) != 0 && errno == EAGAIN);
    CHECK(dh_now_ms() - t0 < 1000);
    dh_close(&d);

    /* Unbounded wait: STREAMOFF wakes the blocked DQBUF. The decoder model
     * answers EAGAIN (a later DQBUF on the stopped queue gets EINVAL). */
    setenv("NVMPI_MOCK_DQBUF_WAIT_MS", "0", 1);
    CHECK(start(&d, 320, 240, 2, 0) == 0);
    CHECK(dh_dq(&d, CAP, &b, pl) == 0);  /* the IDR's frame (does not block) */
    CHECK(pthread_create(&t, NULL, dq_waiter, &w) == 0);
    dh_ms(100);
    CHECK(v4l2_ioctl(d.fd, VIDIOC_STREAMOFF, &type) == 0);
    pthread_join(t, NULL);
    CHECK(w.ret < 0 && w.err == EAGAIN);
    CHECK(dh_dq(&d, CAP, &b, pl) != 0 && errno == EINVAL);
    dh_close(&d);

    /* ... and close() wakes it (EBADF). */
    CHECK(start(&d, 320, 240, 2, 0) == 0);
    CHECK(dh_dq(&d, CAP, &b, pl) == 0);
    CHECK(pthread_create(&t, NULL, dq_waiter, &w) == 0);
    dh_ms(100);
    CHECK(v4l2_close(d.fd) == 0);
    pthread_join(t, NULL);
    CHECK(w.ret < 0 && w.err == EBADF);
    d.fd = -1;
    d.ncap = 0;                          /* surfaces freed with the process */

    /* DEVICE_POLL: ready immediately for a pending frame; wakes on the
     * poll interrupt and on STREAMOFF instead of hanging. */
    CHECK(start(&d, 320, 240, 2, O_NONBLOCK) == 0);
    dh_ms(30);
    w.ret = -1;
    poll_waiter(&w);
    CHECK(w.ret == 0 && (w.resp & POLLIN));
    (void)drain_frames(&d, NULL);
    CHECK(pthread_create(&t, NULL, poll_waiter, &w) == 0);
    dh_ms(100);
    poll_intr(&d, 1);
    pthread_join(t, NULL);
    CHECK(w.ret == 0 && w.resp == 0);
    poll_intr(&d, 0);
    CHECK(pthread_create(&t, NULL, poll_waiter, &w) == 0);
    dh_ms(100);
    CHECK(v4l2_ioctl(d.fd, VIDIOC_STREAMOFF, &type) == 0);
    pthread_join(t, NULL);
    CHECK(w.ret == 0 && w.resp == 0);
    dh_close(&d);
    unsetenv("NVMPI_MOCK_DQBUF_WAIT_MS");
}

void test_decoder_model(void)
{
    test_frames_and_sizes();
    test_size_sources();
    test_backpressure_and_eos();
    test_flush_no_reannounce();
    test_midstream_change();
    test_blocking_paths();
}
