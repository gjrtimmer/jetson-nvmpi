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
 *   AV1 (#147)           MODEL OF THE ORIN (R36.4, measured by the test_api
 *                        hardware case, MR !58): until a sequence header
 *                        OBU arrived, AV1 data is HELD - its OUTPUT buffer
 *                        is never returned - and the session is poisoned:
 *                        every later buffer queues behind it, so no event,
 *                        no frame, even when a header follows. put_packet
 *                        then blocks once the OUTPUT buffers are used up.
 *                        libnvmpi must drop header-less data itself
 *                        (nvmpi_dec_av1.cpp). Cleared by an OUTPUT
 *                        streamoff (flush/close). After a header, buffers
 *                        with no picture OBU (TD/header only) complete
 *                        without a frame and the state is sticky.
 *   Reorder delay        NVMPI_MOCK_DEC_REORDER=N (default 0) models the DPB
 *                        depth of a real decoder: a VCL AU only produces a
 *                        frame once N frames are held back, so the last N
 *                        frames of a stream exist only inside the decoder
 *                        until EOS drains them (the loss of #161
 *                        when the capture thread stops before the LAST
 *                        buffer). Frames come out in input order.
 *   Colorimetry          NVMPI_MOCK_DEC_COLORSPACE=601|709|2020 (default 601) and
 *                        NVMPI_MOCK_DEC_FULL_RANGE=0|1 set the CAPTURE format's
 *                        colorspace/quantization, as the real decoder reports
 *                        the stream's VUI (#163).
 *   EOS                  Frames already decoded are delivered first; the
 *                        newest undelivered one is flagged V4L2_BUF_FLAG_LAST.
 *                        With a reorder delay the EOS marker stays QUEUED
 *                        until every held frame was produced (it needs free
 *                        CAPTURE buffers, like any frame), and LAST goes on
 *                        the final one.
 *                        Afterwards DQBUF on the empty CAPTURE queue reports
 *                        EAGAIN with V4L2_BUF_FLAG_LAST set in the buffer
 *                        (what NvV4l2ElementPlane::dqBuffer and libnvmpi's
 *                        capture loop test for). No bogus empty frame is
 *                        ever produced. Input after EOS is dropped until the
 *                        OUTPUT queue is streamed off (flush).
 *   STREAMOFF            OUTPUT: full flush (stream state and pending events
 *                        are reset). CAPTURE: completes a resolution change.
 *                        After an OUTPUT flush the next input announces the
 *                        stream again with NVMPI_MOCK_DEC_REANNOUNCE=1; the
 *                        default 0 follows the stateful decoder spec (an
 *                        event only on a format change; to be confirmed on
 *                        the Orin): the CAPTURE format is kept, so a
 *                        client that tore the CAPTURE queue down must re-arm
 *                        it itself (#162). A later different SPS size still
 *                        announces.
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
#include <unistd.h>

/* NVIDIA's V4L2_EVENT_RESOLUTION_CHANGE is the mainline SOURCE_CHANGE (5);
 * the NVIDIA header is not needed to build the mock. */
/* NVIDIA fourcc for HEVC (v4l2_nv_extensions.h); mainline uses 'HEVC'. */
#define MOCK_PIX_FMT_H265 v4l2_fourcc('H', '2', '6', '5')
/* AV1: NVIDIA fourcc ('AV10', V4L2_PIX_FMT_AV1 in its headers) and mainline's
 * 'AV01'; both are accepted so the model follows whichever header built it. */
#define MOCK_PIX_FMT_AV1_NV v4l2_fourcc('A', 'V', '1', '0')
#define MOCK_PIX_FMT_AV1    v4l2_fourcc('A', 'V', '0', '1')

#define DEC_DEFAULT_W   320u
#define DEC_DEFAULT_H   240u
#define DEC_DQ_WAIT_MS  10u        /* default bound on a blocking DQBUF     */


static void dec_on_open(struct mock_ctx *ctx)
{
    /* Allocated here; freed in dec_on_close() (single deallocation point). */
    struct dec_model *m = calloc(1, sizeof(*m));
    uint32_t w = 0, h = 0, wait = DEC_DQ_WAIT_MS, reannounce = 0;

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
    (void)mock_env_uint("NVMPI_MOCK_DEC_REORDER", 0, MOCK_DEC_REORDER_MAX,
                        &m->reorder);
    (void)mock_env_uint("NVMPI_MOCK_DEC_REANNOUNCE", 0, 1, &reannounce);
    {
        uint32_t cs = 601, fr = 0;

        (void)mock_env_uint("NVMPI_MOCK_DEC_COLORSPACE", 601, 2020, &cs);
        (void)mock_env_uint("NVMPI_MOCK_DEC_FULL_RANGE", 0, 1, &fr);
        m->colorspace = cs;
        m->full_range = fr != 0;
    }
    m->reannounce = reannounce != 0;
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

/* Complete OUTPUT buffer oi (consumed by the decoder). */
static void finish_out(struct mock_ctx *ctx, int oi, uint32_t flags)
{
    (void)mock_complete_buf(ctx, MOCK_Q_OUTPUT, (uint32_t)oi, NULL, flags);
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

/* Process the EOS marker (empty OUTPUT buffer oi). Returns false while held
 * frames still wait for a CAPTURE buffer (the marker stays queued and is
 * retried when the application queues one); true once consumed. */
static bool step_eos(struct mock_ctx *ctx, struct dec_model *m, int oi)
{
    struct mock_queue *cap = &ctx->q[MOCK_Q_CAPTURE];
    bool drained = false;
    int ci;

    if (!m->eos_seen) {
        /* Drain the reorder FIFO: every held frame is produced in order and
         * the final one becomes the "last" buffer. */
        while (m->held_n > 0) {
            if (m->reconfig || !cap->streaming ||
                (ci = mock_peek_queued(ctx, MOCK_Q_CAPTURE)) < 0) {
                MOCK_TRACE("decoder: EOS waits, %u frame(s) held", m->held_n);
                return false;
            }
            mock_dec_produce(ctx, m, ci, mock_dec_held_pop(m));
            drained = true;
            if (m->held_n == 0)
                cap->bufs[ci].flags |= V4L2_BUF_FLAG_LAST;
        }
        /* Nothing was held: the newest frame, if the application has not
         * taken it yet, becomes the "last" buffer. */
        if (!drained && m->last_idx >= 0 &&
            cap->bufs[m->last_idx].state == MB_DONE)
            cap->bufs[m->last_idx].flags |= V4L2_BUF_FLAG_LAST;
        m->eos_seen = m->eos_last = true;
    }
    finish_out(ctx, oi, 0);
    return true;
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
    bool av1 = pixfmt == MOCK_PIX_FMT_AV1_NV || pixfmt == MOCK_PIX_FMT_AV1;

    if (n == 0)
        return step_eos(ctx, m, oi);
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
    else if (data && av1)
        mock_scan_av1(data, n, &sc);
    else if (data)
        sc.has_vcl = true;             /* other codecs: every buffer = frame */
    if (map)
        munmap(map, n);

    if (av1) {
        /* Orin behaviour (#147): header-less data before the first sequence
         * header is held and poisons the session (see the file header). The
         * buffer stays QUEUED, so everything behind it waits too. */
        m->seq_seen = m->seq_seen || sc.has_seq;
        if (!m->seq_seen) {
            MOCK_TRACE("decoder: hold AV1 data before the sequence header");
            return false;
        }
    }

    if (m->env_dims) {
        if (!m->announced)
            mock_dec_announce(ctx, m, m->env_w, m->env_h);
    } else if (!m->announced) {
        mock_dec_announce(ctx, m, sc.has_dims ? sc.width : DEC_DEFAULT_W,
                 sc.has_dims ? sc.height : DEC_DEFAULT_H);
    } else if (sc.has_dims && (sc.width != m->width || sc.height != m->height)) {
        mock_dec_announce(ctx, m, sc.width, sc.height);
    }

    if (!sc.has_vcl) {                 /* parameter sets only: no frame     */
        finish_out(ctx, oi, 0);
        return true;
    }
    if (m->held_n < m->reorder) {      /* still filling the reorder delay   */
        mock_dec_held_push(m, mb->ts_ns);
        finish_out(ctx, oi, 0);
        return true;
    }
    if (m->reconfig || !cap->streaming ||
        (ci = mock_peek_queued(ctx, MOCK_Q_CAPTURE)) < 0) {
        MOCK_TRACE("decoder: hold AU (reconfig=%d capture streaming=%d)",
                   m->reconfig, cap->streaming);
        return false;                  /* hold: no destination buffer yet   */
    }
    if (m->reorder) {                  /* the frame shown now is an older one */
        uint64_t ts = mock_dec_held_pop(m);

        mock_dec_held_push(m, mb->ts_ns);
        mock_dec_produce(ctx, m, ci, ts);
    } else {
        mock_dec_produce(ctx, m, ci, mb->ts_ns);
    }
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
        m->reconfig = m->eos_seen = m->eos_last = false;
        if (m->reannounce)             /* else keep the stream format       */
            m->announced = false;
        m->seq_seen = false;           /* AV1: next stream sends its own header */
        m->held_n = m->held_head = 0;  /* reorder FIFO is flushed too       */
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
