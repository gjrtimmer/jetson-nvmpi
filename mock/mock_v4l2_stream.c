/*
 * mock_v4l2_stream.c - EXPBUF, STREAMON/OFF and the v4l2_mmap cookie lookup
 * of the V4L2 mock (split from mock_v4l2_queue.c to stay under 400 LOC).
 * Each function takes ctx->mu itself; callers hold only a ctx reference.
 */
#define _GNU_SOURCE
#include "mock_v4l2.h"
#include "mock_log.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

int mock_do_expbuf(struct mock_ctx *ctx, struct v4l2_exportbuffer *e)
{
    int qi = mock_qidx(e->type), ret = 0, fd = -1;
    struct mock_queue *q;

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
        /* dup of the memfd: caller owns it (closes it), like a dma-buf fd.
         * It is NOT registered with the nvbufsurface mock (strict FromFd). */
        fd = fcntl(q->bufs[e->index].memfd[e->plane], F_DUPFD_CLOEXEC, 3);
        if (fd < 0)
            ret = errno;
        else
            e->fd = fd;
    }
    pthread_mutex_unlock(&ctx->mu);
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

/* v4l2_mmap helper: decode the cookie back to (queue, buffer, plane). */
int mock_mmap_lookup(struct mock_ctx *ctx, int64_t offset, size_t length,
                     int *memfd_out)
{
    int ret = 0;
    int64_t c;
    uint32_t plane, idx, qi;
    struct mock_queue *q;

    if (offset <= 0 || offset % MOCK_PAGE) {
        errno = EINVAL;
        return -1;
    }
    c = offset / MOCK_PAGE - 1;
    plane = (uint32_t)(c % MOCK_MAX_PLANES);
    idx = (uint32_t)((c / MOCK_MAX_PLANES) % MOCK_MAX_BUFS);
    qi = (uint32_t)(c / MOCK_MAX_PLANES / MOCK_MAX_BUFS);
    if (qi >= MOCK_NQ) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&ctx->mu);
    q = &ctx->q[qi];
    if (q->memory != V4L2_MEMORY_MMAP || idx >= q->n_bufs ||
        plane >= q->bufs[idx].n_planes || q->bufs[idx].memfd[plane] < 0 ||
        length > q->bufs[idx].length[plane])
        ret = EINVAL;
    else
        *memfd_out = q->bufs[idx].memfd[plane];
    pthread_mutex_unlock(&ctx->mu);
    if (ret) {
        errno = ret;
        return -1;
    }
    return 0;
}
