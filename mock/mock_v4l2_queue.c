/*
 * mock_v4l2_queue.c - OUTPUT/CAPTURE buffer queues of the V4L2 mock:
 * REQBUFS, QUERYBUF, QBUF, DQBUF and buffer completion. EXPBUF, STREAMON/OFF
 * and the mmap cookie lookup live in mock_v4l2_stream.c.
 *
 * State machine per buffer:  DEQUEUED --QBUF--> QUEUED --model--> DONE
 *                            DONE --DQBUF--> DEQUEUED
 *                            STREAMOFF / REQBUFS(0): any state -> DEQUEUED
 * Every function here takes ctx->mu itself (callers hold only a ctx ref).
 */
#define _GNU_SOURCE
#include "mock_v4l2.h"
#include "mock_log.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int mock_qidx(uint32_t type)
{
    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
        return MOCK_Q_OUTPUT;
    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
        return MOCK_Q_CAPTURE;
    return -1;
}

static void fifo_push(struct mock_fifo *f, uint32_t idx)
{
    f->idx[(f->head + f->count) % MOCK_MAX_BUFS] = (uint8_t)idx;
    f->count++;
}

static uint32_t fifo_pop(struct mock_fifo *f)
{
    uint32_t idx = f->idx[f->head];

    f->head = (f->head + 1) % MOCK_MAX_BUFS;
    f->count--;
    return idx;
}

/* Remove idx from a fifo wherever it sits (used by the model on early drop). */
static void fifo_remove(struct mock_fifo *f, uint32_t idx)
{
    uint32_t i, n = f->count;

    for (i = 0; i < n; i++) {
        uint32_t v = fifo_pop(f);

        if (v != idx)
            fifo_push(f, v);
    }
}

/* Close MMAP backings and reset every buffer to a pristine DEQUEUED slot.
 * Single deallocation point for memfds; fds are set to -1 after close. */
void mock_queue_free_bufs(struct mock_queue *q)
{
    uint32_t b, p;

    for (b = 0; b < MOCK_MAX_BUFS; b++) {
        for (p = 0; p < MOCK_MAX_PLANES; p++) {
            if (q->bufs[b].memfd[p] >= 0)
                close(q->bufs[b].memfd[p]);
            q->bufs[b].memfd[p] = -1;
            q->bufs[b].dmabuf_fd[p] = -1;   /* caller-owned, never closed */
            q->bufs[b].length[p] = 0;
            q->bufs[b].bytesused[p] = 0;
        }
        q->bufs[b].state = MB_DEQUEUED;
        q->bufs[b].n_planes = 0;
        q->bufs[b].flags = 0;
    }
    memset(&q->queued, 0, sizeof(q->queued));
    memset(&q->done, 0, sizeof(q->done));
    q->n_bufs = 0;
    q->memory = 0;
}

int mock_do_reqbufs(struct mock_ctx *ctx, struct v4l2_requestbuffers *rb)
{
    int qi = mock_qidx(rb->type), ret = 0;
    struct mock_queue *q;
    uint32_t b, p, count;

    if (qi < 0 || (rb->memory != V4L2_MEMORY_MMAP &&
                   rb->memory != V4L2_MEMORY_USERPTR &&
                   rb->memory != V4L2_MEMORY_DMABUF)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&ctx->mu);
    q = &ctx->q[qi];
    if (q->streaming) {                  /* the driver refuses while ON */
        ret = EBUSY;
        goto out;
    }
    mock_queue_free_bufs(q);
    count = rb->count > MOCK_MAX_BUFS ? MOCK_MAX_BUFS : rb->count;
    if (count > 0) {
        q->memory = rb->memory;
        q->n_bufs = count;
        q->n_planes = q->fmt.fmt.pix_mp.num_planes
            ? q->fmt.fmt.pix_mp.num_planes : 1;
        for (b = 0; b < count; b++) {
            q->bufs[b].n_planes = q->n_planes;
            for (p = 0; p < q->n_planes; p++) {
                uint32_t len = q->fmt.fmt.pix_mp.plane_fmt[p].sizeimage;

                q->bufs[b].length[p] = len;
                if (rb->memory != V4L2_MEMORY_MMAP)
                    continue;
                /* memfd gives a real, mappable, fd-backed buffer so EXPBUF
                 * can hand out dma-buf-like fds. Freed in free_bufs. */
                if (len == 0) { ret = EINVAL; goto fail; }
                q->bufs[b].memfd[p] = memfd_create("mock-v4l2", MFD_CLOEXEC);
                if (q->bufs[b].memfd[p] < 0 ||
                    ftruncate(q->bufs[b].memfd[p], len) < 0) {
                    ret = errno ? errno : ENOMEM;
                    goto fail;
                }
            }
        }
    }
    rb->count = count;
    goto out;
fail:
    mock_queue_free_bufs(q);             /* roll back partial allocation */
out:
    pthread_cond_broadcast(&ctx->cv);
    mock_update_ready(ctx);
    pthread_mutex_unlock(&ctx->mu);
    if (ret) {
        errno = ret;
        return -1;
    }
    return 0;
}

/* Fill planes[] of the user's v4l2_buffer from a mock buffer. mu held. */
static void fill_buffer(struct v4l2_buffer *b, const struct mock_queue *q,
                        int qi, uint32_t idx)
{
    const struct mock_buf *mb = &q->bufs[idx];
    uint32_t p;

    b->index = idx;
    b->type = q->type;
    b->memory = q->memory;
    b->flags = mb->flags | (mb->state == MB_QUEUED ? V4L2_BUF_FLAG_QUEUED : 0)
             | (mb->state == MB_DONE ? V4L2_BUF_FLAG_DONE : 0);
    b->sequence = mb->sequence;
    b->length = mb->n_planes;
    if (!b->m.planes)
        return;
    for (p = 0; p < mb->n_planes; p++) {
        b->m.planes[p].length = mb->length[p];
        b->m.planes[p].bytesused = mb->bytesused[p];
        if (q->memory == V4L2_MEMORY_MMAP)
            b->m.planes[p].m.mem_offset = mock_cookie_of(qi, idx, p);
    }
}

int mock_do_querybuf(struct mock_ctx *ctx, struct v4l2_buffer *b)
{
    int qi = mock_qidx(b->type), ret = 0;

    if (qi < 0 || !b->m.planes) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&ctx->mu);
    if (b->index >= ctx->q[qi].n_bufs ||
        b->length < ctx->q[qi].bufs[b->index].n_planes)
        ret = EINVAL;
    else
        fill_buffer(b, &ctx->q[qi], qi, b->index);
    pthread_mutex_unlock(&ctx->mu);
    if (ret) {
        errno = ret;
        return -1;
    }
    return 0;
}

int mock_do_qbuf(struct mock_ctx *ctx, struct v4l2_buffer *b)
{
    int qi = mock_qidx(b->type), ret = 0;
    struct mock_queue *q;
    struct mock_buf *mb;
    uint32_t p;

    if (qi < 0 || !b->m.planes) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&ctx->mu);
    q = &ctx->q[qi];
    /* Validate like the driver: bounds, memory type, state, plane count.
     * A double QBUF is a caller bug the mock must surface, not absorb. */
    if (b->index >= q->n_bufs || b->memory != q->memory) {
        ret = EINVAL;
    } else if ((mb = &q->bufs[b->index])->state != MB_DEQUEUED ||
               b->length < mb->n_planes) {
        ret = EINVAL;
    } else {
        for (p = 0; p < mb->n_planes && !ret; p++) {
            if (q->memory == V4L2_MEMORY_DMABUF) {
                int fd = b->m.planes[p].m.fd;

                /* The real driver rejects a closed/invalid dma-buf fd. */
                if (fd < 0 || fcntl(fd, F_GETFD) < 0)
                    ret = EBADF;
                else
                    mb->dmabuf_fd[p] = fd;
            } else if (q->memory == V4L2_MEMORY_USERPTR &&
                       !b->m.planes[p].m.userptr) {
                ret = EINVAL;
            }
        }
        if (!ret) {
            for (p = 0; p < mb->n_planes; p++)
                mb->bytesused[p] = qi == MOCK_Q_OUTPUT
                    ? b->m.planes[p].bytesused : 0;
            mb->flags = 0;
            mb->state = MB_QUEUED;
            fifo_push(&q->queued, b->index);
            /* Model hook: may complete the buffer immediately. */
            if (ctx->ops && ctx->ops->on_qbuf)
                ret = -ctx->ops->on_qbuf(ctx, qi, b->index);
            if (ret) {                   /* undo so state stays coherent */
                fifo_remove(&q->queued, b->index);
                mb->state = MB_DEQUEUED;
            }
        }
    }
    pthread_cond_broadcast(&ctx->cv);
    mock_update_ready(ctx);
    pthread_mutex_unlock(&ctx->mu);
    if (ret) {
        errno = ret;
        return -1;
    }
    return 0;
}

int mock_complete_buf(struct mock_ctx *ctx, int qi, uint32_t index,
                      const uint32_t *bytesused, uint32_t flags)
{
    struct mock_queue *q = &ctx->q[qi];
    struct mock_buf *mb = &q->bufs[index];
    uint32_t p;

    if (index >= q->n_bufs || mb->state != MB_QUEUED)
        return -EINVAL;
    fifo_remove(&q->queued, index);
    for (p = 0; bytesused && p < mb->n_planes; p++)
        mb->bytesused[p] = bytesused[p];
    mb->flags = flags;
    mb->sequence = q->sequence++;
    mb->state = MB_DONE;
    fifo_push(&q->done, index);
    pthread_cond_broadcast(&ctx->cv);
    mock_update_ready(ctx);
    return 0;
}

/*
 * DQBUF. Honours O_NONBLOCK like the driver: EAGAIN when empty and
 * non-blocking; otherwise waits on ctx->cv. STREAMOFF and close wake the
 * waiter, which then fails (EINVAL when no longer streaming, EBADF when
 * closing) instead of hanging.
 */
int mock_do_dqbuf(struct mock_ctx *ctx, struct v4l2_buffer *b)
{
    int qi = mock_qidx(b->type), ret = 0;
    struct mock_queue *q;

    if (qi < 0 || !b->m.planes) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&ctx->mu);
    q = &ctx->q[qi];
    for (;;) {
        if (ctx->closing) { ret = EBADF; break; }
        if (!q->streaming) { ret = EINVAL; break; }
        if (q->done.count > 0) {
            uint32_t idx = fifo_pop(&q->done);

            q->bufs[idx].state = MB_DEQUEUED;
            fill_buffer(b, q, qi, idx);
            /* flags: DONE is not reported after DQ */
            b->flags &= ~(uint32_t)(V4L2_BUF_FLAG_DONE | V4L2_BUF_FLAG_QUEUED);
            break;
        }
        if (ctx->nonblock) { ret = EAGAIN; break; }
        pthread_cond_wait(&ctx->cv, &ctx->mu);
    }
    mock_update_ready(ctx);
    pthread_mutex_unlock(&ctx->mu);
    /* NvV4l2ElementPlane::dqBuffer busy-retries on EAGAIN; a short nap keeps
     * a polling DQ thread from pinning a CI core. Outside the lock. */
    if (ret == EAGAIN)
        usleep(200);
    if (ret) {
        errno = ret;
        return -1;
    }
    return 0;
}
