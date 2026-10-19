/*
 * mock_v4l2.h - internal types of the behavioural libnvv4l2 mock.
 *
 * The mock replaces NVIDIA's libnvv4l2.so (soname libv4l2.so.0), which the
 * vendored/NVIDIA NvV4l2Element* sample classes call through v4l2_open /
 * v4l2_ioctl / v4l2_mmap / v4l2_close. It models a V4L2 memory-to-memory
 * device: every open() yields an independent context with an OUTPUT queue
 * (bitstream for a decoder, raw frames for an encoder) and a CAPTURE queue.
 *
 * Scope of the skeleton (issue #98): the queue/buffer/event machinery and
 * the thread-safe blocking semantics. What the device DOES with queued
 * buffers (decode / encode data model) is plugged in through struct mock_ops
 * (#99 decoder, #100 encoder); with no ops registered buffers are accepted
 * and never completed, exactly like a device that has not produced output.
 *
 * Thread-safety contract
 *   g_lock (core.c)  guards the context list and ctx->refs.
 *   ctx->mu          guards EVERYTHING else in a context (queues, events,
 *                    flags). ctx->cv is signalled on any state change a
 *                    waiter may care about (buffer done, event, STREAMOFF,
 *                    close).
 *   Lock order: g_lock -> ctx->mu. Hooks run with ctx->mu held and must not
 *   call back into v4l2_* for the same context.
 */
#ifndef NVMPI_MOCK_V4L2_H
#define NVMPI_MOCK_V4L2_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <linux/videodev2.h>

#define MOCK_MAX_BUFS   32           /* per queue; Tegra uses <= 32        */
#define MOCK_MAX_PLANES VIDEO_MAX_PLANES
#define MOCK_MAX_EVENTS 16           /* pending V4L2 events per context    */
#define MOCK_PAGE       4096u        /* mmap cookie granularity            */

enum { MOCK_Q_OUTPUT = 0, MOCK_Q_CAPTURE = 1, MOCK_NQ = 2 };

enum mock_buf_state { MB_DEQUEUED = 0, MB_QUEUED, MB_DONE };

/* FIFO of buffer indexes (ring). Not synchronised: ctx->mu protects it. */
struct mock_fifo {
    uint8_t  idx[MOCK_MAX_BUFS];
    uint32_t head;
    uint32_t count;
};

struct mock_buf {
    enum mock_buf_state state;
    uint32_t n_planes;
    uint32_t length[MOCK_MAX_PLANES];     /* plane capacity in bytes       */
    uint32_t bytesused[MOCK_MAX_PLANES];  /* set by QBUF or by the model   */
    int      memfd;                       /* MMAP backing, ONE per buffer
                                           * (-1 otherwise); plane p lives
                                           * at plane_off[p]                */
    uint32_t plane_off[MOCK_MAX_PLANES];  /* page-aligned offsets in memfd */
    int      dmabuf_fd[MOCK_MAX_PLANES];  /* DMABUF: fd given at QBUF      */
    uint32_t flags;                       /* V4L2_BUF_FLAG_* reported at DQ */
    uint32_t sequence;
    uintptr_t userptr[MOCK_MAX_PLANES];   /* USERPTR: caller memory given
                                           * at QBUF (never owned/freed)    */
    struct timeval ts;                    /* QBUF timestamp, copied to the
                                           * DQ'd buffer by the model       */
};

struct mock_queue {
    uint32_t type;                        /* V4L2_BUF_TYPE_*_MPLANE        */
    uint32_t memory;                      /* V4L2_MEMORY_*, 0 = no REQBUFS */
    uint32_t n_bufs;
    uint32_t n_planes;                    /* fixed at REQBUFS              */
    bool     streaming;
    uint32_t sequence;                    /* next DQ sequence number       */
    struct v4l2_format fmt;
    struct v4l2_rect   crop;
    struct mock_buf    bufs[MOCK_MAX_BUFS];
    struct mock_fifo   queued;            /* QBUF'd, not yet processed     */
    struct mock_fifo   done;              /* ready for DQBUF               */
};

struct mock_ctx;

/*
 * Behaviour plug-in. All members optional (NULL = default accept).
 * Called with ctx->mu held. A model typically moves buffers from
 * q->queued to q->done with mock_complete_buf().
 */
struct mock_ops {
    void (*on_open)(struct mock_ctx *ctx);
    void (*on_close)(struct mock_ctx *ctx);
    /* After a buffer was queued on queue qi. Non-zero return = -errno. */
    int  (*on_qbuf)(struct mock_ctx *ctx, int qi, uint32_t index);
    int  (*on_streamon)(struct mock_ctx *ctx, int qi);
    void (*on_streamoff)(struct mock_ctx *ctx, int qi);
    /* DQBUF found queue qi empty. Fill b->flags if needed (e.g. LAST).
     * Non-zero return = report EAGAIN now instead of waiting. */
    int  (*on_dq_empty)(struct mock_ctx *ctx, int qi, struct v4l2_buffer *b);
    /* Return 1 when the ioctl was handled (result in *ret / errno). */
    int  (*on_ioctl)(struct mock_ctx *ctx, unsigned long req, void *arg,
                     int *ret);
};

struct mock_ctx {
    int  fd;                              /* eventfd handed to the caller  */
    bool nonblock;                        /* O_NONBLOCK at open            */
    bool closing;                         /* set by v4l2_close             */
    bool signalled;                       /* eventfd currently readable    */
    uint32_t dq_wait_ms;                  /* blocking DQBUF gives up with
                                           * EAGAIN after this long; 0 =
                                           * wait until woken (default)     */
    bool dq_wake_eagain;                  /* a DQBUF that was blocked when
                                           * its queue was streamed off
                                           * returns EAGAIN, not EINVAL
                                           * (see mock_dec.c for why)       */
    int  refs;                            /* in-flight calls (g_lock)      */
    char node[64];                        /* device node path as opened    */
    const struct mock_ops *ops;
    void *model;                          /* private data of the model     */

    pthread_mutex_t mu;
    pthread_cond_t  cv;
    struct mock_queue q[MOCK_NQ];
    struct v4l2_event events[MOCK_MAX_EVENTS];
    uint32_t ev_head, ev_count;
    uint32_t ev_sub[MOCK_MAX_EVENTS];     /* subscribed event types        */
    uint32_t ev_nsub;
    uint32_t ev_seq;

    struct mock_ctx *next;                /* context list (g_lock)         */
};

/* mmap offset cookie: page-aligned, unique per (queue, buffer, plane) and
 * never 0 so a zero offset is always rejected. */
static inline uint32_t mock_cookie_of(int qi, uint32_t idx, uint32_t plane)
{
    return ((((uint32_t)qi * MOCK_MAX_BUFS + idx) * MOCK_MAX_PLANES) +
            plane + 1) * MOCK_PAGE;
}

/* ---- core.c ---------------------------------------------------------- */
struct mock_ctx *mock_ctx_get(int fd);    /* takes a ref; NULL+EBADF       */
void mock_ctx_put(struct mock_ctx *ctx);
/* Re-sync the eventfd with "any done buffer or pending event". mu held.   */
void mock_update_ready(struct mock_ctx *ctx);

/* ---- queue.c --------------------------------------------------------- */
int  mock_qidx(uint32_t type);            /* -1 when not an MPLANE type    */
void mock_queue_free_bufs(struct mock_queue *q);
int  mock_do_reqbufs(struct mock_ctx *ctx, struct v4l2_requestbuffers *rb);
int  mock_do_querybuf(struct mock_ctx *ctx, struct v4l2_buffer *b);
int  mock_do_qbuf(struct mock_ctx *ctx, struct v4l2_buffer *b);
int  mock_do_dqbuf(struct mock_ctx *ctx, struct v4l2_buffer *b);
int  mock_do_expbuf(struct mock_ctx *ctx, struct v4l2_exportbuffer *e);
int  mock_do_stream(struct mock_ctx *ctx, uint32_t type, bool on);
/* Model API: index of the oldest QBUF'd buffer on queue qi, or -1. mu held. */
int  mock_peek_queued(struct mock_ctx *ctx, int qi);
/* Model API: complete a QBUF'd buffer (queued -> done) and wake waiters.  */
int  mock_complete_buf(struct mock_ctx *ctx, int qi, uint32_t index,
                       const uint32_t *bytesused, uint32_t flags);
/* mmap cookie -> backing memfd and the plane's offset inside it; -1 + errno
 * on a bad cookie. Caller holds a ctx ref (takes ctx->mu itself). */
int  mock_mmap_lookup(struct mock_ctx *ctx, int64_t offset, size_t length,
                      int *memfd_out, int64_t *off_out);

/* ---- fmt.c ----------------------------------------------------------- */
int  mock_do_fmt(struct mock_ctx *ctx, unsigned long req, struct v4l2_format *f);
int  mock_do_crop(struct mock_ctx *ctx, unsigned long req, void *arg);
int  mock_do_event(struct mock_ctx *ctx, unsigned long req, void *arg);
/* Model API: queue an event (e.g. V4L2_EVENT_SOURCE_CHANGE); dropped when
 * not subscribed. mu held. */
void mock_push_event(struct mock_ctx *ctx, uint32_t type, uint32_t u32);

/* ---- models.c: which behaviour a device node gets ---------------------- */
const struct mock_ops *mock_model_for_node(const char *node);
/* mock_dec.c: the decoder model (nvdec nodes). */
extern const struct mock_ops mock_dec_ops;

#endif /* NVMPI_MOCK_V4L2_H */
