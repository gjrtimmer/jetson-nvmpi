/*
 * mock_dec_frame.c - frame side of the decoder model (mock_dec.c): stream
 * announcement (CAPTURE format + RESOLUTION_CHANGE event), gray frame
 * production and the reorder-delay FIFO of held frame timestamps.
 *
 * Every function runs with ctx->mu held (see mock_v4l2.h).
 */
#define _GNU_SOURCE
#include "mock_v4l2.h"
#include "mock_dec.h"
#include "mock_log.h"

#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* NVIDIA's V4L2_EVENT_RESOLUTION_CHANGE is the mainline SOURCE_CHANGE (5);
 * the NVIDIA header is not needed to build the mock. */
#define MOCK_EVENT_RES_CHANGE V4L2_EVENT_SOURCE_CHANGE
#define DEC_MAX_FILL    (1u << 28) /* never touch a surface bigger than this */

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
void mock_dec_announce(struct mock_ctx *ctx, struct dec_model *m, uint32_t w,
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

/* Produce one gray frame into capture buffer ci carrying timestamp ts_ns
 * (the OUTPUT buffer's own, or the oldest held one with a reorder delay). */
void mock_dec_produce(struct mock_ctx *ctx, struct dec_model *m, int ci,
                    uint64_t ts_ns)
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
    cb->ts_ns = ts_ns;                    /* timestamp copy (pts passthrough)  */
    (void)mock_complete_buf(ctx, MOCK_Q_CAPTURE, (uint32_t)ci, used, flags);
    m->last_idx = ci;
}

/* Hold back / release frame timestamps (reorder delay). The FIFO never
 * holds more than reorder (<= MOCK_DEC_REORDER_MAX) entries. */
void mock_dec_held_push(struct dec_model *m, uint64_t ts)
{
    m->held_ts[(m->held_head + m->held_n) % MOCK_DEC_REORDER_MAX] = ts;
    m->held_n++;
}

uint64_t mock_dec_held_pop(struct dec_model *m)
{
    uint64_t ts = m->held_ts[m->held_head];

    m->held_head = (m->held_head + 1) % MOCK_DEC_REORDER_MAX;
    m->held_n--;
    return ts;
}

