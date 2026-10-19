/*
 * mock_v4l2_core.c - exported libnvv4l2 API (v4l2_open/close/ioctl/mmap/...)
 * of the behavioural mock, plus the context table and ioctl dispatcher.
 *
 * Built as libnvv4l2.so with SONAME libv4l2.so.0, the same soname the stub
 * and the real NVIDIA library carry, so the dynamic linker resolves the NEEDED
 * entry of libnvmpi.so to this mock when LD_LIBRARY_PATH points at build/mock.
 */
#define _GNU_SOURCE
#include "mock_v4l2.h"
#include "mock_log.h"
#include "mock_faults.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MOCK_EXPORT __attribute__((visibility("default")))

/* Exported by libnvv4l2; libv4l2 users may set it. Unused by the mock. */
MOCK_EXPORT FILE *v4l2_log_file = NULL;

/* g_lock guards g_ctx_list and every ctx->refs; g_refs_cv signals "a ref
 * dropped" to v4l2_close waiting for in-flight calls to drain. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_refs_cv = PTHREAD_COND_INITIALIZER;
static struct mock_ctx *g_ctx_list;

/* Look up the context for fd and take a reference so v4l2_close cannot free
 * it under a concurrent ioctl (e.g. the DQ thread blocked in DQBUF). */
struct mock_ctx *mock_ctx_get(int fd)
{
    struct mock_ctx *c;

    pthread_mutex_lock(&g_lock);
    for (c = g_ctx_list; c; c = c->next)
        if (c->fd == fd)
            break;
    if (c)
        c->refs++;
    pthread_mutex_unlock(&g_lock);
    if (!c)
        errno = EBADF;
    return c;
}

void mock_ctx_put(struct mock_ctx *ctx)
{
    pthread_mutex_lock(&g_lock);
    ctx->refs--;
    if (ctx->refs == 0)
        pthread_cond_broadcast(&g_refs_cv);
    pthread_mutex_unlock(&g_lock);
}

/* Keep the eventfd readable exactly while a DQBUF or DQEVENT would succeed,
 * so callers polling the fd see level-triggered readiness like the driver.
 * ctx->mu held. */
void mock_update_ready(struct mock_ctx *ctx)
{
    bool ready = ctx->ev_count > 0;
    uint64_t one = 1, drain;
    int i;

    for (i = 0; i < MOCK_NQ; i++)
        if (ctx->q[i].done.count > 0)
            ready = true;
    if (ready && !ctx->signalled) {
        if (write(ctx->fd, &one, sizeof(one)) == sizeof(one))
            ctx->signalled = true;
    } else if (!ready && ctx->signalled) {
        if (read(ctx->fd, &drain, sizeof(drain)) == sizeof(drain))
            ctx->signalled = false;
    }
}

MOCK_EXPORT int v4l2_open(const char *file, int oflag, ...)
{
    struct mock_ctx *ctx;
    int i;

    /* Only device nodes: the real library opens /dev/v4l2-nvdec,
     * /dev/nvhost-nvdec, ... Anything else is "no such device". */
    if (!file || strncmp(file, "/dev/", 5) != 0) {
        errno = ENOENT;
        return -1;
    }

    /* NVMPI_MOCK_FAULT=open@N: the device is "gone" (driver not loaded). */
    if (mock_fault_hit(MFP_OPEN)) {
        errno = ENODEV;
        return -1;
    }

    /* Allocated here; freed in v4l2_close() (single deallocation point). */
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        errno = ENOMEM;
        return -1;
    }
    /* eventfd is the user-visible fd: unique per open, pollable, closable. */
    ctx->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ctx->fd < 0) {
        free(ctx);
        return -1;
    }
    ctx->nonblock = (oflag & O_NONBLOCK) != 0;
    snprintf(ctx->node, sizeof(ctx->node), "%s", file);
    pthread_mutex_init(&ctx->mu, NULL);
    {   /* CLOCK_MONOTONIC so timed DQBUF waits ignore wall-clock jumps */
        pthread_condattr_t ca;

        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&ctx->cv, &ca);
        pthread_condattr_destroy(&ca);
    }
    for (i = 0; i < MOCK_NQ; i++) {
        int p, b;

        ctx->q[i].type = i == MOCK_Q_OUTPUT
            ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
            : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ctx->q[i].fmt.type = ctx->q[i].type;
        ctx->q[i].n_planes = 1;
        for (b = 0; b < MOCK_MAX_BUFS; b++) {
            ctx->q[i].bufs[b].memfd = -1;
            for (p = 0; p < MOCK_MAX_PLANES; p++)
                ctx->q[i].bufs[b].dmabuf_fd[p] = -1;
        }
    }
    ctx->ops = mock_model_for_node(file);

    pthread_mutex_lock(&g_lock);
    ctx->next = g_ctx_list;
    g_ctx_list = ctx;
    pthread_mutex_unlock(&g_lock);

    if (ctx->ops && ctx->ops->on_open) {
        pthread_mutex_lock(&ctx->mu);
        ctx->ops->on_open(ctx);
        pthread_mutex_unlock(&ctx->mu);
    }
    MOCK_TRACE("open %s -> fd %d%s", file, ctx->fd,
               ctx->nonblock ? " (nonblock)" : "");
    return ctx->fd;
}

MOCK_EXPORT int v4l2_close(int fd)
{
    struct mock_ctx *ctx, **pp;
    int i;

    /* Unlink first: from here no new lookup can succeed, and the fd number
     * cannot be reused by another open() before we are done with it. */
    pthread_mutex_lock(&g_lock);
    for (pp = &g_ctx_list; *pp && (*pp)->fd != fd; pp = &(*pp)->next)
        ;
    ctx = *pp;
    if (!ctx) {
        pthread_mutex_unlock(&g_lock);
        errno = EBADF;
        return -1;
    }
    *pp = ctx->next;
    pthread_mutex_unlock(&g_lock);

    /* Wake blocked DQBUF callers; they see closing and return EBADF. */
    pthread_mutex_lock(&ctx->mu);
    ctx->closing = true;
    pthread_cond_broadcast(&ctx->cv);
    pthread_mutex_unlock(&ctx->mu);

    /* Drain in-flight calls before touching anything they use. */
    pthread_mutex_lock(&g_lock);
    while (ctx->refs > 0)
        pthread_cond_wait(&g_refs_cv, &g_lock);
    pthread_mutex_unlock(&g_lock);

    if (ctx->ops && ctx->ops->on_close) {
        pthread_mutex_lock(&ctx->mu);
        ctx->ops->on_close(ctx);
        pthread_mutex_unlock(&ctx->mu);
    }
    for (i = 0; i < MOCK_NQ; i++)
        mock_queue_free_bufs(&ctx->q[i]);
    MOCK_TRACE("close fd %d", fd);
    close(ctx->fd);                       /* the eventfd */
    pthread_cond_destroy(&ctx->cv);
    pthread_mutex_destroy(&ctx->mu);
    memset(ctx, 0, sizeof(*ctx));         /* scrub before free (policy)  */
    free(ctx);                            /* single deallocation point   */
    return 0;
}

/* VIDIOC_QUERYCAP: advertise a multi-planar M2M streaming device. */
static int do_querycap(struct v4l2_capability *cap)
{
    memset(cap, 0, sizeof(*cap));
    snprintf((char *)cap->driver, sizeof(cap->driver), "nvmpi-mock");
    snprintf((char *)cap->card, sizeof(cap->card), "nvmpi mock M2M");
    snprintf((char *)cap->bus_info, sizeof(cap->bus_info), "platform:mock");
    cap->version = 0x00060100;
    cap->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
    cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
    return 0;
}

static int do_ioctl(struct mock_ctx *ctx, unsigned long req, void *arg)
{
    int ret = 0, handled = 0;

    /* A model may take over any ioctl (e.g. DEVICE_POLL controls). */
    if (ctx->ops && ctx->ops->on_ioctl) {
        pthread_mutex_lock(&ctx->mu);
        handled = ctx->ops->on_ioctl(ctx, req, arg, &ret);
        pthread_mutex_unlock(&ctx->mu);
        if (handled)
            return ret;
    }
    switch (req) {
    case VIDIOC_QUERYCAP:       return do_querycap(arg);
    case VIDIOC_REQBUFS:        return mock_do_reqbufs(ctx, arg);
    case VIDIOC_QUERYBUF:       return mock_do_querybuf(ctx, arg);
    case VIDIOC_QBUF:           return mock_do_qbuf(ctx, arg);
    case VIDIOC_DQBUF:          return mock_do_dqbuf(ctx, arg);
    case VIDIOC_EXPBUF:         return mock_do_expbuf(ctx, arg);
    case VIDIOC_STREAMON:
        return mock_do_stream(ctx, *(uint32_t *)arg, true);
    case VIDIOC_STREAMOFF:
        return mock_do_stream(ctx, *(uint32_t *)arg, false);
    case VIDIOC_S_FMT:
    case VIDIOC_G_FMT:
    case VIDIOC_TRY_FMT:        return mock_do_fmt(ctx, req, arg);
    case VIDIOC_G_CROP:
    case VIDIOC_S_CROP:
    case VIDIOC_S_SELECTION:
    case VIDIOC_G_SELECTION:
    case VIDIOC_S_PARM:
    case VIDIOC_G_PARM:         return mock_do_crop(ctx, req, arg);
    case VIDIOC_SUBSCRIBE_EVENT:
    case VIDIOC_UNSUBSCRIBE_EVENT:
    case VIDIOC_DQEVENT:        return mock_do_event(ctx, req, arg);
    /* Controls: accepted and ignored in the skeleton. The Tegra-specific
     * DEVICE_POLL / SET_POLL_INTERRUPT semantics need the data model
     * (#99/#100) and are handled there via mock_ops.on_ioctl. */
    case VIDIOC_S_EXT_CTRLS:
    case VIDIOC_G_EXT_CTRLS:
    case VIDIOC_TRY_EXT_CTRLS:
    case VIDIOC_S_CTRL:
    case VIDIOC_G_CTRL:         return 0;
    default:
        MOCK_TRACE("ioctl 0x%lx: not supported", req);
        errno = ENOTTY;
        return -1;
    }
}

MOCK_EXPORT int v4l2_ioctl(int fd, unsigned long int request, ...)
{
    struct mock_ctx *ctx;
    va_list ap;
    void *arg;
    int ret;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    ctx = mock_ctx_get(fd);
    if (!ctx)
        return -1;
    /* All requests take an argument; refuse NULL early instead of faulting
     * like a driver would (EFAULT). */
    if (!arg) {
        mock_ctx_put(ctx);
        errno = EFAULT;
        return -1;
    }
    ret = do_ioctl(ctx, request, arg);
    mock_ctx_put(ctx);
    return ret;
}

/* MMAP buffers are mapped through their EXPBUF fd at the QUERYBUF mem_offset
 * (what NvBuffer::map() does; the offset is the plane's position in the
 * buffer's memfd, 0 for plane 0, so it cannot identify a buffer on the
 * device fd). Nothing in libnvmpi or the sample classes uses v4l2_mmap. */
MOCK_EXPORT void *v4l2_mmap(void *start, size_t length, int prot, int flags,
                            int fd, int64_t offset)
{
    (void)start; (void)length; (void)prot; (void)flags; (void)fd;
    (void)offset;
    errno = ENODEV;
    return MAP_FAILED;
}

MOCK_EXPORT int v4l2_munmap(void *start, size_t length)
{
    return munmap(start, length);
}

/* Rarely used libv4l2 entry points: exported so the symbol set matches the
 * stub/real library, but not part of the M2M model. */
MOCK_EXPORT int v4l2_dup(int fd)               { (void)fd; errno = ENOSYS; return -1; }
MOCK_EXPORT int v4l2_fd_open(int fd, int flags){ (void)fd; (void)flags; errno = ENOSYS; return -1; }
MOCK_EXPORT ssize_t v4l2_read(int fd, void *b, size_t n)
{ (void)fd; (void)b; (void)n; errno = ENOSYS; return -1; }
MOCK_EXPORT ssize_t v4l2_write(int fd, const void *b, size_t n)
{ (void)fd; (void)b; (void)n; errno = ENOSYS; return -1; }
MOCK_EXPORT int v4l2_get_control(int fd, int cid)
{ (void)fd; (void)cid; return 0; }
MOCK_EXPORT int v4l2_set_control(int fd, int cid, int value)
{ (void)fd; (void)cid; (void)value; return 0; }
