/*
 * mock_dec.c - decoder model of the V4L2 mock (issue #99).
 *
 * Plugs into the queue machinery of mock_v4l2_*.c through struct mock_ops
 * and behaves like a Tegra stateful decoder as seen by libnvmpi:
 *
 *   OUTPUT (bitstream)   Buffers are consumed IN ORDER. One that holds a VCL
 *                        NAL is held (stays QUEUED, so the application sees
 *                        back-pressure) until a CAPTURE buffer is available,
 *                        then completes together with one gray NV12 frame.
 *                        A buffer without VCL (SPS/PPS/SEI only) completes
 *                        at once. An empty buffer (bytesused 0) is the EOS
 *                        marker.
 *   RESOLUTION_CHANGE    Pushed when the first input arrives (stream size
 *                        from NVMPI_MOCK_WIDTH/HEIGHT, else the H.264 SPS,
 *                        else 320x240) and again when a later SPS changes
 *                        the size. Frames of the new size are held until the
 *                        application tore the CAPTURE queue down (STREAMOFF)
 *                        and set it up again, like the real driver.
 *   CAPTURE (frames)     DMABUF/MMAP buffers are filled with gray (0x80).
 *   EOS                  Frames already decoded are delivered first; the
 *                        newest undelivered one is flagged V4L2_BUF_FLAG_LAST.
 *                        Afterwards DQBUF on the empty CAPTURE queue reports
 *                        EAGAIN with V4L2_BUF_FLAG_LAST set in the buffer
 *                        (what NvV4l2ElementPlane::dqBuffer and libnvmpi's
 *                        capture loop test for). No bogus empty frame is
 *                        ever produced. Input after EOS is dropped until the
 *                        OUTPUT queue is streamed off (flush).
 *   STREAMOFF            OUTPUT: full flush (stream state and pending events
 *                        are reset). CAPTURE: completes a resolution change.
 *                        Blocked DQBUF/DEVICE_POLL waiters are woken (core);
 *                        a woken DQBUF returns EAGAIN (see dec_on_open).
 *
 * Environment (read at open): NVMPI_MOCK_WIDTH + NVMPI_MOCK_HEIGHT force the
 * stream size (both, even, 16..8192); NVMPI_MOCK_DQBUF_WAIT_MS bounds a
 * blocking DQBUF (default 10 ms, 0 = unbounded) - libnvmpi's capture loop is
 * written for a DQBUF that returns EAGAIN, and an unbounded wait would stop
 * it from ever seeing a resolution-change event.
 *
 * Locking: every hook runs with ctx->mu held (see mock_v4l2.h); nothing here
 * blocks except DEVICE_POLL, which waits on ctx->cv (releasing ctx->mu).
 */
#define _GNU_SOURCE
#include "mock_v4l2.h"
#include "mock_dec.h"
#include "mock_env.h"
#include "mock_log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* NVIDIA's V4L2_EVENT_RESOLUTION_CHANGE is the mainline SOURCE_CHANGE (5);
 * the NVIDIA header is not needed to build the mock. */
#define MOCK_EVENT_RES_CHANGE V4L2_EVENT_SOURCE_CHANGE
/* NVIDIA fourcc for HEVC (v4l2_nv_extensions.h); mainline uses 'HEVC'. */
#define MOCK_PIX_FMT_H265 v4l2_fourcc('H', '2', '6', '5')

#define DEC_DEFAULT_W   320u
#define DEC_DEFAULT_H   240u
#define DEC_DQ_WAIT_MS  10u        /* default bound on a blocking DQBUF     */
#define DEC_MAX_FILL    (1u << 28) /* never touch a surface bigger than this */


static void dec_on_open(struct mock_ctx *ctx)
{
    /* Allocated here; freed in dec_on_close() (single deallocation point). */
    struct dec_model *m = calloc(1, sizeof(*m));
    uint32_t w = 0, h = 0, wait = DEC_DQ_WAIT_MS;

    if (!m)
        return;                    /* ctx->model NULL: model stays inert    */
    m->last_idx = -1;
    if (mock_env_uint("NVMPI_MOCK_WIDTH", MOCK_DEC_MIN_DIM, MOCK_DEC_MAX_DIM, &w) &&
        mock_env_uint("NVMPI_MOCK_HEIGHT", MOCK_DEC_MIN_DIM, MOCK_DEC_MAX_DIM, &h) &&
        !(w & 1) && !(h & 1)) {
        m->env_dims = true;
        m->env_w = w;
        m->env_h = h;
    }
    (void)mock_env_uint("NVMPI_MOCK_DQBUF_WAIT_MS", 0, 60000, &wait);
    ctx->dq_wait_ms = wait;
    /* A DQBUF released by STREAMOFF reports EAGAIN, not EINVAL. libnvmpi's
     * flush()/close() stream the CAPTURE queue off while its capture thread
     * may sit in DQBUF, and NvV4l2ElementPlane::dqBuffer turns any other
     * errno into a STICKY plane error (is_in_error) that kills every later
     * decode after the flush. On hardware DQBUF returns promptly, so that
     * straddle is a ~microsecond window; the mock would otherwise open it
     * for the whole wait. The real driver stays the authority (hw-decoder-
     * flush guards this on the Orin). */
    ctx->dq_wake_eagain = true;
    ctx->model = m;
}

static void dec_on_close(struct mock_ctx *ctx)
{
    if (ctx->model) {
        memset(ctx->model, 0, sizeof(struct dec_model));
        free(ctx->model);          /* single deallocation point             */
        ctx->model = NULL;
    }
}

/* Fill every byte of a buffer's backing memfd with 0x80: Y = 128 and
 * CbCr = 128 is mid-gray in NV12 whatever the plane layout. -1 on failure. */
static int fill_gray(int fd)
{
    struct stat st;
    void *p;

    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || (uint64_t)st.st_size > DEC_MAX_FILL)
        return -1;
    p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        return -1;
    memset(p, 0x80, (size_t)st.st_size);
    munmap(p, (size_t)st.st_size);
    return 0;
}

/* Announce stream size (w,h): program the CAPTURE format/crop and push the
 * RESOLUTION_CHANGE event. A change while CAPTURE streams must be completed
 * by the application (STREAMOFF) before new-size frames flow. */
static void announce(struct mock_ctx *ctx, struct dec_model *m, uint32_t w,
                     uint32_t h)
{
    struct mock_queue *cap = &ctx->q[MOCK_Q_CAPTURE];
    struct v4l2_pix_format_mplane *pix = &cap->fmt.fmt.pix_mp;

    m->reconfig = m->announced && cap->streaming;
    m->announced = true;
    m->width = w;
    m->height = h;
    memset(pix, 0, sizeof(*pix));
    pix->width = w;
    pix->height = h;
    pix->pixelformat = V4L2_PIX_FMT_NV12M;
    pix->field = V4L2_FIELD_NONE;
    pix->colorspace = V4L2_COLORSPACE_SMPTE170M;
    pix->num_planes = 2;
    pix->plane_fmt[0].sizeimage = w * h;
    pix->plane_fmt[0].bytesperline = w;
    pix->plane_fmt[1].sizeimage = w * h / 2;
    pix->plane_fmt[1].bytesperline = w;
    cap->crop.left = cap->crop.top = 0;
    cap->crop.width = w;
    cap->crop.height = h;
    MOCK_TRACE("decoder: stream %ux%u%s", w, h, m->reconfig ? " (change)" : "");
    mock_push_event(ctx, MOCK_EVENT_RES_CHANGE, 0);
}

/* Complete OUTPUT buffer oi (consumed by the decoder). */
static void finish_out(struct mock_ctx *ctx, int oi, uint32_t flags)
{
    (void)mock_complete_buf(ctx, MOCK_Q_OUTPUT, (uint32_t)oi, NULL, flags);
}

/* Produce one gray frame into capture buffer ci for output buffer mb. */
static void produce(struct mock_ctx *ctx, struct dec_model *m, int ci,
                    const struct mock_buf *mb)
{
    struct mock_queue *cap = &ctx->q[MOCK_Q_CAPTURE];
    struct mock_buf *cb = &cap->bufs[ci];
    uint32_t used[MOCK_MAX_PLANES] = { 0 }, flags = 0;
    uint64_t luma = (uint64_t)m->width * m->height;
    int fd = cap->memory == V4L2_MEMORY_MMAP ? cb->memfd :
             cap->memory == V4L2_MEMORY_DMABUF ? cb->dmabuf_fd[0] : -1;

    if (cb->n_planes >= 2) {
        used[0] = (uint32_t)luma;
        used[1] = (uint32_t)(luma / 2);
    } else {
        used[0] = (uint32_t)(luma * 3 / 2);
    }
    if (fill_gray(fd) < 0)
        flags |= V4L2_BUF_FLAG_ERROR;  /* USERPTR or a foreign fd: no data */
    cb->ts = mb->ts;                   /* timestamp copy (pts passthrough)  */
    (void)mock_complete_buf(ctx, MOCK_Q_CAPTURE, (uint32_t)ci, used, flags);
    m->last_idx = ci;
}

/* View of an OUTPUT buffer's bytes. Returns NULL when not readable. *map is
 * set when the view must be munmap'ed by the caller. */
static const uint8_t *out_bytes(const struct mock_buf *mb, uint32_t memory,
                                size_t n, void **map)
{
    void *m;

    *map = NULL;
    if (memory == V4L2_MEMORY_USERPTR)
        return (const uint8_t *)mb->userptr[0];
    if (memory == V4L2_MEMORY_MMAP && mb->memfd >= 0) {
        m = mmap(NULL, n, PROT_READ, MAP_SHARED, mb->memfd, mb->plane_off[0]);
        if (m == MAP_FAILED)
            return NULL;
        *map = m;                      /* caller munmap()s                  */
        return (const uint8_t *)m;
    }
    return NULL;                       /* DMABUF bitstream: not modelled    */
}

/* Process the EOS marker (empty OUTPUT buffer oi). Always consumes it. */
static void step_eos(struct mock_ctx *ctx, struct dec_model *m, int oi)
{
    struct mock_queue *cap = &ctx->q[MOCK_Q_CAPTURE];

    if (!m->eos_seen) {
        /* The newest frame, if the application has not taken it yet,
         * becomes the "last" buffer. */
        if (m->last_idx >= 0 && cap->bufs[m->last_idx].state == MB_DONE)
            cap->bufs[m->last_idx].flags |= V4L2_BUF_FLAG_LAST;
        m->eos_seen = m->eos_last = true;
    }
    finish_out(ctx, oi, 0);
}

/* Process OUTPUT buffer oi. Returns false when it must wait (back-pressure
 * or a pending reconfiguration); true when consumed. */
static bool step(struct mock_ctx *ctx, struct dec_model *m, int oi)
{
    struct mock_queue *out = &ctx->q[MOCK_Q_OUTPUT];
    struct mock_queue *cap = &ctx->q[MOCK_Q_CAPTURE];
    const struct mock_buf *mb = &out->bufs[oi];
    uint32_t pixfmt = out->fmt.fmt.pix_mp.pixelformat;
    struct mock_scan sc;
    const uint8_t *data;
    void *map;
    size_t n = mb->bytesused[0];
    int ci;

    if (n == 0) {
        step_eos(ctx, m, oi);
        return true;
    }
    if (n > mb->length[0]) {           /* driver would reject; flag + drop  */
        finish_out(ctx, oi, V4L2_BUF_FLAG_ERROR);
        return true;
    }
    if (m->eos_seen) {                 /* stopped after EOS: drop input     */
        finish_out(ctx, oi, 0);
        return true;
    }

    data = out_bytes(mb, out->memory, n, &map);
    memset(&sc, 0, sizeof(sc));
    if (data && pixfmt == V4L2_PIX_FMT_H264)
        mock_scan_h264(data, n, &sc);
    else if (data && (pixfmt == MOCK_PIX_FMT_H265 || pixfmt == V4L2_PIX_FMT_HEVC))
        mock_scan_h265(data, n, &sc);
    else if (data)
        sc.has_vcl = true;             /* other codecs: every buffer = frame */
    if (map)
        munmap(map, n);

    if (m->env_dims) {
        if (!m->announced)
            announce(ctx, m, m->env_w, m->env_h);
    } else if (!m->announced) {
        announce(ctx, m, sc.has_dims ? sc.width : DEC_DEFAULT_W,
                 sc.has_dims ? sc.height : DEC_DEFAULT_H);
    } else if (sc.has_dims && (sc.width != m->width || sc.height != m->height)) {
        announce(ctx, m, sc.width, sc.height);
    }

    if (!sc.has_vcl) {                 /* parameter sets only: no frame     */
        finish_out(ctx, oi, 0);
        return true;
    }
    if (m->reconfig || !cap->streaming ||
        (ci = mock_peek_queued(ctx, MOCK_Q_CAPTURE)) < 0) {
        MOCK_TRACE("decoder: hold AU (reconfig=%d capture streaming=%d)",
                   m->reconfig, cap->streaming);
        return false;                  /* hold: no destination buffer yet   */
    }
    produce(ctx, m, ci, mb);
    MOCK_TRACE("decoder: frame -> capture buffer %d", ci);
    finish_out(ctx, oi, 0);
    return true;
}

/* Run the decoder as far as the queues allow. */
static void dec_run(struct mock_ctx *ctx)
{
    struct dec_model *m = ctx->model;
    int oi;

    if (!m)
        return;
    while (ctx->q[MOCK_Q_OUTPUT].streaming &&
           (oi = mock_peek_queued(ctx, MOCK_Q_OUTPUT)) >= 0 && step(ctx, m, oi))
        ;
}

static int dec_on_qbuf(struct mock_ctx *ctx, int qi, uint32_t index)
{
    (void)qi;
    (void)index;
    dec_run(ctx);
    return 0;
}

static int dec_on_streamon(struct mock_ctx *ctx, int qi)
{
    (void)qi;
    dec_run(ctx);
    return 0;
}

static void dec_on_streamoff(struct mock_ctx *ctx, int qi)
{
    struct dec_model *m = ctx->model;

    if (!m)
        return;
    m->off_epoch++;                    /* wakes DEVICE_POLL waiters         */
    m->last_idx = -1;
    if (qi == MOCK_Q_OUTPUT) {         /* flush: start over, drop events    */
        m->announced = m->reconfig = m->eos_seen = m->eos_last = false;
        ctx->ev_head = ctx->ev_count = 0;
    } else {                           /* CAPTURE torn down: re-armed next  */
        m->reconfig = false;
    }
    dec_run(ctx);
}

/* Empty CAPTURE queue after EOS: EAGAIN + LAST, never a wait. */
static int dec_on_dq_empty(struct mock_ctx *ctx, int qi, struct v4l2_buffer *b)
{
    struct dec_model *m = ctx->model;

    if (m && qi == MOCK_Q_CAPTURE && m->eos_last) {
        b->flags |= V4L2_BUF_FLAG_LAST;
        return 1;
    }
    return 0;
}

const struct mock_ops mock_dec_ops = {
    .on_open = dec_on_open,
    .on_close = dec_on_close,
    .on_qbuf = dec_on_qbuf,
    .on_streamon = dec_on_streamon,
    .on_streamoff = dec_on_streamoff,
    .on_dq_empty = dec_on_dq_empty,
    .on_ioctl = mock_dec_ioctl,
};
