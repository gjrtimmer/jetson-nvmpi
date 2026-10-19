/*
 * test_mock_fault.c - NVMPI_MOCK_FAULT / NVMPI_MOCK_DELAY_US of the mock
 * (mock_faults.c), issue #99. Part of the test_mock executable.
 *
 * Checks the contract of mock_faults.h: what counts as a call per point,
 * one-shot versus sticky, defensive parsing, counter reset on a changed
 * spec, and the delay. How libnvmpi copes with each fault is tested in
 * test_api_dec_mock.c.
 */
#include "mock_dec_harness.h"

#include <stdlib.h>

static void fault(const char *spec)
{
    if (spec)
        setenv("NVMPI_MOCK_FAULT", spec, 1);
    else
        unsetenv("NVMPI_MOCK_FAULT");
}

/* Queue n P slices so n frames are ready, then count DQBUF outcomes. */
static void test_dqbuf(void)
{
    struct dh d;
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];
    uint8_t au[64];
    unsigned i;

    fault(NULL);
    CHECK(dh_start(&d, 320, 240, 6, O_NONBLOCK) == 0);
    for (i = 0; i < 3; i++)
        CHECK(dh_put(&d, au, fx_pslice(au), 10 + i) == 0);
    dh_ms(30);                           /* 4 frames ready (IDR + 3 P)   */

    fault("dqbuf@2");                    /* the 2nd successful DQ fails  */
    CHECK(dh_dq(&d, CAP, &b, pl) == 0);
    CHECK(dh_dq(&d, CAP, &b, pl) != 0 && errno == EIO);
    CHECK(dh_dq(&d, CAP, &b, pl) == 0 && b.timestamp.tv_usec == 10);
    /* The failed buffer stayed ready: nothing was lost. */
    CHECK(dh_dq(&d, CAP, &b, pl) == 0 && b.sequence == 2);

    CHECK(dh_cap_q(&d, 0) == 0 && dh_cap_q(&d, 1) == 0);
    CHECK(dh_put(&d, au, fx_pslice(au), 20) == 0);
    CHECK(dh_put(&d, au, fx_pslice(au), 21) == 0);
    dh_ms(30);
    fault("dqbuf@2+");                   /* sticky: 2nd and every later  */
    CHECK(dh_dq(&d, CAP, &b, pl) == 0);
    CHECK(dh_dq(&d, CAP, &b, pl) != 0 && errno == EIO);
    CHECK(dh_dq(&d, CAP, &b, pl) != 0 && errno == EIO);
    dh_close(&d);
}

static void test_other_points(void)
{
    struct dh d;
    struct v4l2_requestbuffers rb;
    struct v4l2_exportbuffer eb;
    NvBufSurface *s = NULL;
    NvBufSurfaceCreateParams cp;
    uint8_t au[64];
    unsigned type = OUT;
    int fd;

    /* reqbufs: only count > 0 fails; REQBUFS(0) is infallible. */
    fault("reqbufs@1+");
    fd = v4l2_open(DEC_NODE, O_RDWR | O_NONBLOCK);
    CHECK(fd >= 0);
    CHECK(dh_set_fmt(fd, OUT, V4L2_PIX_FMT_H264, 0, 0, 1, 4096) == 0);
    CHECK(dh_reqbufs(fd, OUT, V4L2_MEMORY_USERPTR, 4) != 0 && errno == ENOMEM);
    CHECK(dh_reqbufs(fd, OUT, V4L2_MEMORY_USERPTR, 0) == 0);
    CHECK(v4l2_ioctl(fd, VIDIOC_STREAMON, &type) != 0);   /* no buffers   */
    fault("reqbufs@2");                  /* spec changed: counter reset  */
    CHECK(dh_reqbufs(fd, OUT, V4L2_MEMORY_USERPTR, 4) == 0);
    CHECK(dh_reqbufs(fd, OUT, V4L2_MEMORY_USERPTR, 4) != 0 && errno == ENOMEM);
    CHECK(dh_reqbufs(fd, OUT, V4L2_MEMORY_USERPTR, 4) == 0);
    memset(&rb, 0, sizeof(rb));
    CHECK(v4l2_close(fd) == 0);

    /* streamon: one-shot, the retry succeeds. */
    fault("streamon@1");
    fd = v4l2_open(DEC_NODE, O_RDWR | O_NONBLOCK);
    CHECK(dh_set_fmt(fd, OUT, V4L2_PIX_FMT_H264, 0, 0, 1, 4096) == 0);
    CHECK(dh_reqbufs(fd, OUT, V4L2_MEMORY_USERPTR, 2) == 0);
    CHECK(v4l2_ioctl(fd, VIDIOC_STREAMON, &type) != 0 && errno == EIO);
    CHECK(v4l2_ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    CHECK(v4l2_close(fd) == 0);

    /* qbuf: the buffer stays with the application and can be re-queued. */
    fault("qbuf@1");
    CHECK(dh_open(&d, O_NONBLOCK, V4L2_PIX_FMT_H264) == 0);
    CHECK(dh_qout(&d, 0, au, fx_pslice(au), 1) != 0 && errno == EIO);
    CHECK(dh_qout(&d, 0, au, fx_pslice(au), 1) == 0);
    dh_close(&d);

    /* expbuf on an MMAP queue of a plain (model-less) node. */
    fault("expbuf@1");
    fd = v4l2_open("/dev/v4l2-mock-raw", O_RDWR | O_NONBLOCK);
    CHECK(dh_set_fmt(fd, CAP, V4L2_PIX_FMT_NV12M, 64, 64, 1, 8192) == 0);
    CHECK(dh_reqbufs(fd, CAP, V4L2_MEMORY_MMAP, 2) == 0);
    memset(&eb, 0, sizeof(eb));
    eb.type = CAP;
    CHECK(v4l2_ioctl(fd, VIDIOC_EXPBUF, &eb) != 0 && errno == ENOMEM);
    CHECK(v4l2_ioctl(fd, VIDIOC_EXPBUF, &eb) == 0);
    close(eb.fd);
    CHECK(v4l2_close(fd) == 0);

    /* alloc (libnvbufsurface): 2nd allocation fails, 1st and 3rd succeed. */
    fault("alloc@2");
    memset(&cp, 0, sizeof(cp));
    cp.width = cp.height = 64;
    cp.colorFormat = NVBUF_COLOR_FORMAT_NV12;
    cp.layout = NVBUF_LAYOUT_PITCH;
    cp.memType = NVBUF_MEM_SURFACE_ARRAY;
    CHECK(NvBufSurfaceCreate(&s, 1, &cp) == 0 && s);
    CHECK(NvBufSurfaceDestroy(s) == 0);
    s = (NvBufSurface *)1;
    CHECK(NvBufSurfaceCreate(&s, 1, &cp) != 0 && s == NULL);
    CHECK(NvBufSurfaceCreate(&s, 1, &cp) == 0 && s);
    CHECK(NvBufSurfaceDestroy(s) == 0);
    fault(NULL);
}

/* Malformed specs are ignored (and reported), they never fire or crash. */
static void test_spec_parsing(void)
{
    static const char *const bad[] = {
        "dqbuf@0", "dqbuf@x", "dqbuf@-1", "dqbuf@", "@3", "foo@1", "dqbuf",
        "dqbuf@2extra", "dqbuf@99999999999", ",,,", " dqbuf@1",
        "dqbuf@1;qbuf@1",
    };
    char longspec[400];
    struct dh d;
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];
    unsigned i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fault(bad[i]);
        CHECK(dh_start(&d, 320, 240, 2, O_NONBLOCK) == 0);
        dh_ms(10);
        CHECK(dh_dq(&d, CAP, &b, pl) == 0);          /* no fault fired   */
        dh_close(&d);
    }
    memset(longspec, 'a', sizeof(longspec) - 1);
    longspec[sizeof(longspec) - 1] = '\0';
    fault(longspec);
    CHECK(dh_start(&d, 320, 240, 2, O_NONBLOCK) == 0);
    dh_close(&d);
    /* A bad token does not poison a good one next to it. */
    fault("bogus@1,reqbufs@1");
    CHECK(dh_open(&d, O_NONBLOCK, V4L2_PIX_FMT_H264) != 0);
    (void)v4l2_close(d.fd);
    fault(NULL);
}

static void test_delay(void)
{
    struct dh d;
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];
    uint64_t t0;

    fault(NULL);
    CHECK(dh_start(&d, 320, 240, 2, O_NONBLOCK) == 0);
    dh_ms(20);
    setenv("NVMPI_MOCK_DELAY_US", "60000", 1);
    t0 = dh_now_ms();
    CHECK(dh_dq(&d, CAP, &b, pl) == 0);
    CHECK(dh_now_ms() - t0 >= 55);
    CHECK(dh_cap_q(&d, b.index) == 0);
    setenv("NVMPI_MOCK_DELAY_US", "abc", 1);   /* invalid: no delay      */
    dh_ms(20);
    t0 = dh_now_ms();
    (void)dh_dq(&d, CAP, &b, pl);
    CHECK(dh_now_ms() - t0 < 50);
    unsetenv("NVMPI_MOCK_DELAY_US");
    dh_close(&d);
}

void test_fault_injection(void)
{
    test_dqbuf();
    test_other_points();
    test_spec_parsing();
    test_delay();
}
