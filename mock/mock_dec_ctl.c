/*
 * mock_dec_ctl.c - control ioctls of the decoder model (split from
 * mock_dec.c): the minimum capture buffer count and NVIDIA's pseudo-poll
 * controls (V4L2_CID_MPEG_VIDEO_DEVICE_POLL / V4L2_CID_MPEG_SET_POLL_INTERRUPT
 * in v4l2_nv_extensions.h), which NvVideoDecoder::DevicePoll() and
 * SetPollInterrupt() issue through VIDIOC_S_EXT_CTRLS.
 *
 * Runs with ctx->mu held (mock_ops.on_ioctl contract); DEVICE_POLL waits on
 * ctx->cv, which releases ctx->mu while blocked.
 */
#define _GNU_SOURCE
#include "mock_v4l2.h"
#include "mock_dec.h"

#include <errno.h>
#include <poll.h>

/* v4l2_nv_extensions.h: V4L2_CID_MPEG_BASE + 550 / + 551. */
#define MOCK_CID_DEVICE_POLL     (V4L2_CID_MPEG_BASE + 550)
#define MOCK_CID_POLL_INTERRUPT  (V4L2_CID_MPEG_BASE + 551)
/* ABI twin of v4l2_ctrl_video_device_poll. */
struct mock_device_poll { uint16_t req_events, resp_events; };

#define DEC_MIN_CAPTURE 4          /* V4L2_CID_MIN_BUFFERS_FOR_CAPTURE      */

/* DEVICE_POLL: a pseudo poll() that blocks until a requested event is ready,
 * the poll interrupt is set, a queue is streamed off, or the device closes.
 * The real driver hangs after the final EOS DQBUF (jetson-ffmpeg wiki);
 * the mock returns instead so a model run cannot deadlock. */
static void device_poll(struct mock_ctx *ctx, struct dec_model *m,
                        struct mock_device_poll *dp)
{
    uint32_t epoch = m->off_epoch;
    uint16_t resp;

    for (;;) {
        resp = 0;
        if ((dp->req_events & POLLIN) &&
            (ctx->q[MOCK_Q_CAPTURE].done.count > 0 || m->eos_last))
            resp |= POLLIN;
        if ((dp->req_events & POLLOUT) && ctx->q[MOCK_Q_OUTPUT].done.count > 0)
            resp |= POLLOUT;
        if ((dp->req_events & POLLPRI) && ctx->ev_count > 0)
            resp |= POLLPRI;
        if (resp || m->poll_int || ctx->closing || epoch != m->off_epoch)
            break;
        pthread_cond_wait(&ctx->cv, &ctx->mu);
    }
    dp->resp_events = resp;
}

int mock_dec_ioctl(struct mock_ctx *ctx, unsigned long req, void *arg, int *ret)
{
    struct dec_model *m = ctx->model;
    struct v4l2_ext_controls *ec = arg;
    struct v4l2_control *c = arg;
    uint32_t i;

    if (!m)
        return 0;
    if (req == VIDIOC_G_CTRL && c->id == V4L2_CID_MIN_BUFFERS_FOR_CAPTURE) {
        c->value = DEC_MIN_CAPTURE;
        *ret = 0;
        return 1;
    }
    if (req != VIDIOC_S_EXT_CTRLS)
        return 0;
    if (ec->count == 0 || ec->count > 16 || !ec->controls) {
        errno = EINVAL;
        *ret = -1;
        return 1;
    }
    for (i = 0; i < ec->count; i++) {
        struct v4l2_ext_control *k = &ec->controls[i];

        if (k->id == MOCK_CID_POLL_INTERRUPT) {
            m->poll_int = k->value != 0;
            pthread_cond_broadcast(&ctx->cv);
        } else if (k->id == MOCK_CID_DEVICE_POLL) {
            if (!k->string) {
                errno = EFAULT;
                *ret = -1;
                return 1;
            }
            device_poll(ctx, m, (struct mock_device_poll *)(void *)k->string);
        }
    }
    *ret = 0;
    return 1;
}

