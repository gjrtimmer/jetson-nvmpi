/*
 * mock_dec_harness.h - a minimal V4L2 decoder client for the mock tests: the
 * same call sequence NvVideoDecoder + libnvmpi issue (USERPTR bitstream
 * buffers, subscribe to RESOLUTION_CHANGE, DMABUF NV12 capture buffers
 * backed by NvBufSurface), reduced to what a driver-level test needs.
 * Header-only; include after mock_test.h.
 */
#ifndef NVMPI_TEST_MOCK_DEC_HARNESS_H
#define NVMPI_TEST_MOCK_DEC_HARNESS_H

#include "mock_test.h"
#include "h264_fixture.h"
#include <time.h>

#define DEC_NODE "/dev/v4l2-nvdec"
#define DH_NOUT 4
#define DH_NCAP 8

struct dh {
    int fd;
    uint8_t store[DH_NOUT][4096];      /* USERPTR bitstream memory          */
    unsigned out_used;                 /* output buffers handed out so far  */
    NvBufSurface *surf[DH_NCAP];       /* capture buffers (DMABUF)          */
    unsigned ncap;
    uint32_t w, h;                     /* from G_FMT after the event        */
};

static inline void dh_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

static inline uint64_t dh_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static inline int dh_set_fmt(int fd, uint32_t type, uint32_t pixfmt, uint32_t w,
                             uint32_t h, uint32_t nplanes, uint32_t size)
{
    struct v4l2_format f;

    memset(&f, 0, sizeof(f));
    f.type = type;
    f.fmt.pix_mp.pixelformat = pixfmt;
    f.fmt.pix_mp.width = w;
    f.fmt.pix_mp.height = h;
    f.fmt.pix_mp.num_planes = (uint8_t)nplanes;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = size;
    return v4l2_ioctl(fd, VIDIOC_S_FMT, &f);
}

static inline int dh_reqbufs(int fd, uint32_t type, uint32_t mem, uint32_t n)
{
    struct v4l2_requestbuffers rb;

    memset(&rb, 0, sizeof(rb));
    rb.type = type;
    rb.memory = mem;
    rb.count = n;
    return v4l2_ioctl(fd, VIDIOC_REQBUFS, &rb);
}

/* Open the decoder like NvVideoDecoder: bitstream plane streaming. */
static inline int dh_open(struct dh *d, int oflags, uint32_t codec)
{
    struct v4l2_event_subscription sub;
    unsigned type = OUT;

    memset(d, 0, sizeof(*d));
    d->fd = v4l2_open(DEC_NODE, O_RDWR | oflags);
    if (d->fd < 0)
        return -1;
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_SOURCE_CHANGE;       /* RESOLUTION_CHANGE (= 5) */
    if (v4l2_ioctl(d->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) ||
        dh_set_fmt(d->fd, OUT, codec, 0, 0, 1, sizeof(d->store[0])) ||
        dh_reqbufs(d->fd, OUT, V4L2_MEMORY_USERPTR, DH_NOUT) != 0 ||
        v4l2_ioctl(d->fd, VIDIOC_STREAMON, &type))
        return -1;
    return 0;
}

/* Queue output buffer idx with n bytes (n == 0 is the EOS marker). */
static inline int dh_qout(struct dh *d, uint32_t idx, const uint8_t *au,
                          size_t n, uint64_t pts_us)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];

    memcpy(d->store[idx], au, n);
    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.type = OUT;
    b.memory = V4L2_MEMORY_USERPTR;
    b.index = idx;
    b.length = 1;
    b.m.planes = pl;
    pl[0].m.userptr = (unsigned long)d->store[idx];
    pl[0].bytesused = (uint32_t)n;
    b.timestamp.tv_sec = (time_t)(pts_us / 1000000);
    b.timestamp.tv_usec = (suseconds_t)(pts_us % 1000000);
    return v4l2_ioctl(d->fd, VIDIOC_QBUF, &b);
}

static inline int dh_dq(struct dh *d, uint32_t type, struct v4l2_buffer *b,
                        struct v4l2_plane *pl)
{
    memset(b, 0, sizeof(*b));
    memset(pl, 0, sizeof(*pl) * VIDEO_MAX_PLANES);
    b->type = type;
    b->memory = type == OUT ? V4L2_MEMORY_USERPTR : V4L2_MEMORY_DMABUF;
    b->length = type == OUT ? 1 : 2;
    b->m.planes = pl;
    return v4l2_ioctl(d->fd, VIDIOC_DQBUF, b);
}

/* Feed one AU like libnvmpi: a fresh buffer while any is unused, else
 * recycle a completed one (waits up to 2 s). Returns the QBUF result. */
static inline int dh_put(struct dh *d, const uint8_t *au, size_t n, uint64_t pts)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];
    uint32_t idx;
    uint64_t t0 = dh_now_ms();

    if (d->out_used < DH_NOUT) {
        idx = d->out_used++;
    } else {
        while (dh_dq(d, OUT, &b, pl) != 0) {
            if (errno != EAGAIN || dh_now_ms() - t0 > 2000)
                return -1;
            dh_ms(1);
        }
        idx = b.index;
    }
    return dh_qout(d, idx, au, n, pts);
}

/* Wait for a RESOLUTION_CHANGE event; 0 on success. */
static inline int dh_wait_event(struct dh *d, unsigned timeout_ms)
{
    struct v4l2_event ev;
    uint64_t t0 = dh_now_ms();

    while (dh_now_ms() - t0 <= timeout_ms) {
        memset(&ev, 0, sizeof(ev));
        if (v4l2_ioctl(d->fd, VIDIOC_DQEVENT, &ev) == 0)
            return ev.type == V4L2_EVENT_SOURCE_CHANGE ? 0 : -1;
        dh_ms(1);
    }
    return -1;
}

static inline int dh_cap_q(struct dh *d, uint32_t idx)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];

    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.type = CAP;
    b.memory = V4L2_MEMORY_DMABUF;
    b.index = idx;
    b.length = 2;
    b.m.planes = pl;
    pl[0].m.fd = (int)d->surf[idx]->surfaceList[0].bufferDesc;
    return v4l2_ioctl(d->fd, VIDIOC_QBUF, &b);
}

/* Set up the CAPTURE queue after the event, as respondToResolutionEvent
 * does: G_FMT, allocate n NV12 surfaces, REQBUFS(DMABUF), STREAMON, QBUF. */
static inline int dh_arm(struct dh *d, unsigned n)
{
    struct v4l2_format f;
    NvBufSurfaceCreateParams cp;
    unsigned i, type = CAP;

    memset(&f, 0, sizeof(f));
    f.type = CAP;
    if (n > DH_NCAP || v4l2_ioctl(d->fd, VIDIOC_G_FMT, &f))
        return -1;
    d->w = f.fmt.pix_mp.width;
    d->h = f.fmt.pix_mp.height;
    if (f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12M ||
        f.fmt.pix_mp.num_planes != 2 ||
        v4l2_ioctl(d->fd, VIDIOC_S_FMT, &f))
        return -1;
    memset(&cp, 0, sizeof(cp));
    cp.width = d->w;
    cp.height = d->h;
    cp.colorFormat = NVBUF_COLOR_FORMAT_NV12;
    cp.layout = NVBUF_LAYOUT_PITCH;
    cp.memType = NVBUF_MEM_SURFACE_ARRAY;
    for (i = 0; i < n; i++)
        if (NvBufSurfaceCreate(&d->surf[i], 1, &cp) != 0)
            return -1;
    d->ncap = n;
    if (dh_reqbufs(d->fd, CAP, V4L2_MEMORY_DMABUF, n) ||
        v4l2_ioctl(d->fd, VIDIOC_STREAMON, &type))
        return -1;
    for (i = 0; i < n; i++)
        if (dh_cap_q(d, i))
            return -1;
    return 0;
}

/* Tear the CAPTURE queue down (STREAMOFF, REQBUFS 0, free surfaces). */
static inline void dh_disarm(struct dh *d)
{
    unsigned i, type = CAP;

    (void)v4l2_ioctl(d->fd, VIDIOC_STREAMOFF, &type);
    (void)dh_reqbufs(d->fd, CAP, V4L2_MEMORY_DMABUF, 0);
    for (i = 0; i < d->ncap; i++) {
        (void)NvBufSurfaceDestroy(d->surf[i]);
        d->surf[i] = NULL;
    }
    d->ncap = 0;
}

static inline void dh_close(struct dh *d)
{
    dh_disarm(d);
    (void)v4l2_close(d->fd);
    d->fd = -1;
}

/* Open, feed one IDR of size w x h, wait for the event, arm ncap capture
 * buffers: the state libnvmpi reaches after its first resolution change. */
static inline int dh_start(struct dh *d, uint32_t w, uint32_t h, unsigned ncap,
                           int oflags)
{
    uint8_t buf[1024];
    size_t n = fx_idr(buf, w, h);

    if (dh_open(d, oflags, V4L2_PIX_FMT_H264) || dh_put(d, buf, n, 1000))
        return -1;
    if (dh_wait_event(d, 2000) || dh_arm(d, ncap))
        return -1;
    return 0;
}

/* Is the whole luma plane of capture buffer idx mid-gray (0x80)? */
static inline int dh_is_gray(struct dh *d, uint32_t idx)
{
    NvBufSurface *s = d->surf[idx];
    NvBufSurfaceParams *sp = &s->surfaceList[0];
    uint32_t y, x;
    int ok = 1;

    if (NvBufSurfaceMap(s, 0, 0, NVBUF_MAP_READ) != 0)
        return 0;
    for (y = 0; y < sp->planeParams.height[0] && ok; y++)
        for (x = 0; x < sp->planeParams.width[0]; x++)
            if (((uint8_t *)sp->mappedAddr.addr[0])
                    [y * sp->planeParams.pitch[0] + x] != 0x80) {
                ok = 0;
                break;
            }
    (void)NvBufSurfaceUnMap(s, 0, 0);
    return ok;
}

#endif /* NVMPI_TEST_MOCK_DEC_HARNESS_H */
