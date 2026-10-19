/*
 * test_mock.c - self-test of the behavioural Jetson mock (mock/), issue #98.
 *
 * Links the mock libs directly (no libnvmpi) so a failure here is a mock bug,
 * not a libnvmpi bug. Covers the skeleton contract: M2M queue state machine,
 * blocking DQBUF woken by STREAMOFF and by close, mmap/EXPBUF backing, event
 * queue, and NvBufSurface alloc/map/strict-FromFd/transform.
 *
 * Runs on any Linux host (no Tegra). Exit 0 = all pass. A global alarm turns
 * a hang (the failure mode of a broken wake-up) into a clean failure.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <nvbufsurface.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* libnvv4l2 API (libv4l2.h is not part of the build image). */
int v4l2_open(const char *file, int oflag, ...);
int v4l2_close(int fd);
int v4l2_ioctl(int fd, unsigned long int request, ...);
void *v4l2_mmap(void *start, size_t length, int prot, int flags, int fd,
                int64_t offset);
/* Transform entry point, declared locally: avoids CUDA headers. */
int NvBufSurfTransform(NvBufSurface *src, NvBufSurface *dst, void *params);

static int fails;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s (errno %d)\n", __FILE__,      \
                    __LINE__, #c, errno);                                 \
            fails++;                                                      \
        }                                                                 \
    } while (0)

#define OUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE

static int set_fmt(int fd, uint32_t type, uint32_t size)
{
    struct v4l2_format f;

    memset(&f, 0, sizeof(f));
    f.type = type;
    f.fmt.pix_mp.width = 64;
    f.fmt.pix_mp.height = 64;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = size;
    return v4l2_ioctl(fd, VIDIOC_S_FMT, &f);
}

static int reqbufs(int fd, uint32_t type, uint32_t mem, uint32_t n)
{
    struct v4l2_requestbuffers rb;

    memset(&rb, 0, sizeof(rb));
    rb.type = type;
    rb.memory = mem;
    rb.count = n;
    return v4l2_ioctl(fd, VIDIOC_REQBUFS, &rb) ? -1 : (int)rb.count;
}

static int qbuf(int fd, uint32_t type, uint32_t mem, uint32_t idx)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];

    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.type = type;
    b.memory = mem;
    b.index = idx;
    b.length = 1;
    b.m.planes = pl;
    return v4l2_ioctl(fd, VIDIOC_QBUF, &b);
}

static int dqbuf(int fd, uint32_t type, uint32_t mem)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];

    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.type = type;
    b.memory = mem;
    b.length = 1;
    b.m.planes = pl;
    return v4l2_ioctl(fd, VIDIOC_DQBUF, &b);
}

/* Queue state machine, mmap/EXPBUF backing, events, eventfd readiness. */
static void test_queue_basics(void)
{
    struct v4l2_capability cap;
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];
    struct v4l2_exportbuffer eb;
    struct v4l2_event ev;
    struct v4l2_event_subscription sub;
    struct pollfd pfd;
    unsigned type = OUT;
    unsigned char *m, *m2;
    int fd = v4l2_open("/dev/v4l2-mock-raw", O_RDWR | O_NONBLOCK);

    CHECK(fd >= 0);
    CHECK(v4l2_open("/not/a/dev", O_RDWR) < 0 && errno == ENOENT);
    CHECK(v4l2_ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0);
    CHECK(cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE);
    CHECK(set_fmt(fd, OUT, 4096) == 0 && set_fmt(fd, CAP, 0) == 0);
    CHECK(reqbufs(fd, OUT, V4L2_MEMORY_MMAP, 4) == 4);

    /* QUERYBUF -> mmap -> write; EXPBUF fd sees the same bytes. */
    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.type = OUT;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = 1;
    b.length = 1;
    b.m.planes = pl;
    CHECK(v4l2_ioctl(fd, VIDIOC_QUERYBUF, &b) == 0);
    CHECK(pl[0].length == 4096 && pl[0].m.mem_offset != 0);
    m = v4l2_mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                  pl[0].m.mem_offset);
    CHECK(m != MAP_FAILED);
    memset(&eb, 0, sizeof(eb));
    eb.type = OUT;
    eb.index = 1;
    eb.plane = 0;
    CHECK(v4l2_ioctl(fd, VIDIOC_EXPBUF, &eb) == 0 && eb.fd >= 0);
    if (eb.fd >= 0) {                    /* EXPBUF fd is a registered surface */
        NvBufSurface *es = NULL;
        int d = dup(eb.fd);

        CHECK(NvBufSurfaceFromFd(eb.fd, (void **)&es) == 0 && es);
        CHECK(es && es->surfaceList[0].planeParams.num_planes == 1);
        CHECK(NvBufSurfaceFromFd(d, (void **)&es) != 0);   /* strict */
        close(d);
    }
    if (m != MAP_FAILED && eb.fd >= 0) {
        m[0] = 0xA5;
        m2 = mmap(NULL, 4096, PROT_READ, MAP_SHARED, eb.fd, 0);
        CHECK(m2 != MAP_FAILED && m2[0] == 0xA5);
        munmap(m2, 4096);
        munmap(m, 4096);
        close(eb.fd);
    }
    CHECK(v4l2_mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 12345) ==
          MAP_FAILED);

    /* QBUF validation: bad index, double QBUF. */
    CHECK(qbuf(fd, OUT, V4L2_MEMORY_MMAP, 99) < 0 && errno == EINVAL);
    CHECK(qbuf(fd, OUT, V4L2_MEMORY_MMAP, 0) == 0);
    CHECK(qbuf(fd, OUT, V4L2_MEMORY_MMAP, 0) < 0 && errno == EINVAL);
    CHECK(qbuf(fd, OUT, V4L2_MEMORY_DMABUF, 2) < 0 && errno == EINVAL);

    /* DQBUF: not streaming -> EINVAL; streaming+empty+nonblock -> EAGAIN. */
    CHECK(dqbuf(fd, OUT, V4L2_MEMORY_MMAP) < 0 && errno == EINVAL);
    CHECK(v4l2_ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    CHECK(dqbuf(fd, OUT, V4L2_MEMORY_MMAP) < 0 && errno == EAGAIN);
    CHECK(reqbufs(fd, OUT, V4L2_MEMORY_MMAP, 2) < 0 && errno == EBUSY);

    /* STREAMOFF returns buffers to the app: QBUF of index 0 works again. */
    CHECK(v4l2_ioctl(fd, VIDIOC_STREAMOFF, &type) == 0);
    CHECK(qbuf(fd, OUT, V4L2_MEMORY_MMAP, 0) == 0);
    CHECK(reqbufs(fd, OUT, V4L2_MEMORY_MMAP, 0) == 0);

    /* Events: unsubscribed none, DQEVENT empty -> EAGAIN; eventfd idle. */
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    CHECK(v4l2_ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub) == 0);
    CHECK(v4l2_ioctl(fd, VIDIOC_DQEVENT, &ev) < 0 && errno == EAGAIN);
    pfd.fd = fd;
    pfd.events = POLLIN;
    CHECK(poll(&pfd, 1, 0) == 0);        /* nothing ready */
    CHECK(v4l2_close(fd) == 0);
    CHECK(v4l2_close(fd) < 0 && errno == EBADF);
}

struct waiter {
    int fd;
    int ret;
    int err;
};

static void *dq_waiter(void *arg)
{
    struct waiter *w = arg;

    w->ret = dqbuf(w->fd, CAP, V4L2_MEMORY_MMAP);
    w->err = errno;
    return NULL;
}

/* A blocked DQBUF must wake on STREAMOFF (EINVAL) and on close (EBADF). */
static void test_blocking_wakeups(int close_wakes)
{
    struct waiter w;
    pthread_t t;
    unsigned type = CAP;
    int fd = v4l2_open("/dev/v4l2-mock-raw", O_RDWR);   /* blocking */

    CHECK(fd >= 0);
    CHECK(set_fmt(fd, CAP, 4096) == 0);
    CHECK(reqbufs(fd, CAP, V4L2_MEMORY_MMAP, 2) == 2);
    CHECK(v4l2_ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    w.fd = fd;
    w.ret = 0;
    w.err = 0;
    CHECK(pthread_create(&t, NULL, dq_waiter, &w) == 0);
    usleep(100 * 1000);                  /* let it block in DQBUF */
    if (close_wakes) {
        CHECK(v4l2_close(fd) == 0);
        pthread_join(t, NULL);
        CHECK(w.ret < 0 && w.err == EBADF);
    } else {
        CHECK(v4l2_ioctl(fd, VIDIOC_STREAMOFF, &type) == 0);
        pthread_join(t, NULL);
        CHECK(w.ret < 0 && w.err == EINVAL);
        CHECK(v4l2_close(fd) == 0);
    }
}

static void fill(unsigned char *p, size_t n, unsigned char seed)
{
    size_t i;

    for (i = 0; i < n; i++)
        p[i] = (unsigned char)(seed + i * 7);
}

/* NvBufSurface: layout, map, strict FromFd, transform copy + scale. */
static void test_nvbufsurface(void)
{
    NvBufSurfaceCreateParams cp;
    NvBufSurface *a = NULL, *b = NULL, *got = NULL;
    NvBufSurfaceParams *sp;
    int fd, dupfd;

    memset(&cp, 0, sizeof(cp));
    cp.width = 640;
    cp.height = 480;
    cp.colorFormat = NVBUF_COLOR_FORMAT_NV12;
    cp.layout = NVBUF_LAYOUT_PITCH;
    cp.memType = NVBUF_MEM_SURFACE_ARRAY;
    CHECK(NvBufSurfaceCreate(&a, 1, &cp) == 0 && a);
    CHECK(NvBufSurfaceCreate(&b, 1, &cp) == 0 && b);
    if (!a || !b)
        return;
    sp = &a->surfaceList[0];
    CHECK(sp->planeParams.num_planes == 2);
    CHECK(sp->planeParams.pitch[0] == 768);          /* 640 -> 256-aligned */
    CHECK(sp->planeParams.height[1] == 240);
    fd = (int)sp->bufferDesc;
    CHECK(fd > 2);

    /* Strict FromFd: the issued fd resolves, a dup()'d one must not. */
    CHECK(NvBufSurfaceFromFd(fd, (void **)&got) == 0 && got == a);
    dupfd = dup(fd);
    CHECK(dupfd >= 0 && NvBufSurfaceFromFd(dupfd, (void **)&got) != 0);
    close(dupfd);
    CHECK(NvBufSurfaceFromFd(-1, (void **)&got) != 0);

    /* Map, write a pattern, transform to same-size surface, compare. */
    CHECK(NvBufSurfaceMap(a, 0, -1, NVBUF_MAP_READ_WRITE) == 0);
    CHECK(sp->mappedAddr.addr[0] && sp->mappedAddr.addr[1]);
    fill(sp->mappedAddr.addr[0], sp->planeParams.psize[0], 3);
    fill(sp->mappedAddr.addr[1], sp->planeParams.psize[1], 9);
    CHECK(NvBufSurfaceSyncForDevice(a, 0, -1) == 0);
    CHECK(NvBufSurfTransform(a, b, NULL) == 0);
    CHECK(NvBufSurfaceMap(b, 0, 0, NVBUF_MAP_READ) == 0);
    CHECK(memcmp(sp->mappedAddr.addr[0], b->surfaceList[0].mappedAddr.addr[0],
                 640) == 0);
    CHECK(NvBufSurfaceUnMap(a, 0, -1) == 0 && NvBufSurfaceUnMap(b, 0, -1) == 0);
    CHECK(NvBufSurfaceMap(a, 7, 0, NVBUF_MAP_READ) != 0);  /* bad index */

    /* Scale down 640x480 -> 320x240 (same plane layout, different size). */
    {
        NvBufSurface *c = NULL;

        cp.width = 320;
        cp.height = 240;
        CHECK(NvBufSurfaceCreate(&c, 1, &cp) == 0 && c);
        CHECK(c && NvBufSurfTransform(a, c, NULL) == 0);
        CHECK(c && NvBufSurfaceDestroy(c) == 0);
    }

    CHECK(NvBufSurfaceDestroy(a) == 0);
    CHECK(NvBufSurfaceFromFd(fd, (void **)&got) != 0);    /* unregistered */
    CHECK(NvBufSurfaceDestroy(b) == 0);
    CHECK(NvBufSurfaceDestroy(NULL) != 0);
    cp.width = 0;
    CHECK(NvBufSurfaceCreate(&got, 1, &cp) != 0);
}

int main(void)
{
    alarm(60);                           /* a hang is a failure, not a stall */
    test_queue_basics();
    test_blocking_wakeups(0);
    test_blocking_wakeups(1);
    test_nvbufsurface();
    if (fails) {
        fprintf(stderr, "test_mock: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_mock: all checks passed\n");
    return 0;
}
