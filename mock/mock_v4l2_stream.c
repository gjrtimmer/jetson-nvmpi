/*
 * mock_v4l2_stream.c - EXPBUF and STREAMON/OFF of the V4L2 mock (split from
 * mock_v4l2_queue.c to stay under 400 LOC).
 * Each function takes ctx->mu itself; callers hold only a ctx reference.
 */
#define _GNU_SOURCE
#include "mock_v4l2.h"
#include "mock_log.h"
#include "mock_faults.h"

#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

/* Optional hook into the nvbufsurface mock (resolved at run time, so the
 * V4L2 mock has no link-time dependency on it, and a real libnvbufsurface
 * simply lacks the symbol). It registers an EXPBUF fd as an NvBufSurface so
 * NvBufSurfaceFromFd works on it, like on the real Tegra driver, whose MMAP
 * buffers are NvBufSurface-backed. */
typedef int (*export_fn)(int fd, uint32_t n_planes, const uint32_t *sizes,
                         const uint32_t *offsets);

int mock_do_expbuf(struct mock_ctx *ctx, struct v4l2_exportbuffer *e)
{
    int qi = mock_qidx(e->type), ret = 0, fd = -1;
    struct mock_queue *q;
    uint32_t sizes[MOCK_MAX_PLANES], offs[MOCK_MAX_PLANES], n = 0, p;

    if (qi < 0) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&ctx->mu);
    q = &ctx->q[qi];
    if (q->memory != V4L2_MEMORY_MMAP || e->index >= q->n_bufs ||
        e->plane >= q->bufs[e->index].n_planes) {
        ret = EINVAL;
    } else {
        const struct mock_buf *mb = &q->bufs[e->index];

        n = mb->n_planes;
        for (p = 0; p < n; p++) {
            sizes[p] = mb->length[p];
            offs[p] = mb->plane_off[p];
        }
        if (mock_fault_hit(MFP_EXPBUF)) {     /* NVMPI_MOCK_FAULT=expbuf@N */
            ret = ENOMEM;
        } else {
            /* dup of the buffer memfd: the caller owns (closes) it, like a
             * dma-buf fd. One fd covers all planes. */
            fd = fcntl(mb->memfd, F_DUPFD_CLOEXEC, 3);
            if (fd < 0)
                ret = errno;
        }
    }
    pthread_mutex_unlock(&ctx->mu);
    if (!ret) {
        export_fn reg = (export_fn)dlsym(RTLD_DEFAULT,
                                         "mock_nvbuf_register_export");

        if (reg && reg(fd, n, sizes, offs) < 0) {
            close(fd);
            ret = ENOMEM;
        } else {
            e->fd = fd;
        }
    }
    if (ret) {
        errno = ret;
        return -1;
    }
    return 0;
}

/* STREAMON/STREAMOFF. STREAMOFF returns every buffer to the application
 * (all DEQUEUED) and wakes blocked DQBUF callers; it is idempotent. */
int mock_do_stream(struct mock_ctx *ctx, uint32_t type, bool on)
{
    int qi = mock_qidx(type), ret = 0;
    struct mock_queue *q;
    uint32_t b;

    if (qi < 0) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&ctx->mu);
    q = &ctx->q[qi];
    if (on) {
        if (q->n_bufs == 0)
            ret = EINVAL;                /* nothing to stream */
        else if (!q->streaming && mock_fault_hit(MFP_STREAMON))
            ret = EIO;                   /* NVMPI_MOCK_FAULT=streamon@N */
        else if (!q->streaming) {
            q->streaming = true;
            if (ctx->ops && ctx->ops->on_streamon)
                ret = -ctx->ops->on_streamon(ctx, qi);
            if (ret)
                q->streaming = false;
        }
    } else if (q->streaming) {
        q->streaming = false;
        for (b = 0; b < q->n_bufs; b++) {
            q->bufs[b].state = MB_DEQUEUED;
            q->bufs[b].flags = 0;
        }
        memset(&q->queued, 0, sizeof(q->queued));
        memset(&q->done, 0, sizeof(q->done));
        if (ctx->ops && ctx->ops->on_streamoff)
            ctx->ops->on_streamoff(ctx, qi);
    }
    pthread_cond_broadcast(&ctx->cv);    /* wake DQBUF waiters */
    mock_update_ready(ctx);
    pthread_mutex_unlock(&ctx->mu);
    if (ret) {
        errno = ret;
        return -1;
    }
    return 0;
}
