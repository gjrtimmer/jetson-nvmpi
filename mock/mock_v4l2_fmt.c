/*
 * mock_v4l2_fmt.c - format, crop/selection, stream-parm and event ioctls of
 * the V4L2 mock. Everything here is "accepting" state keeping: the values
 * the caller sets are stored and read back, with sensible defaults, so the
 * NvV4l2Element* sample classes see a cooperative driver. Format
 * negotiation that depends on codec/pixel-format knowledge (clamping sizes,
 * rejecting unsupported fourccs) belongs to the decoder/encoder models.
 */
#define _GNU_SOURCE
#include "mock_v4l2.h"

#include <errno.h>
#include <string.h>
#include <time.h>

/* Default plane size when the caller leaves sizeimage 0: 4:2:0 frame size,
 * or 1 MiB for a not-yet-sized bitstream plane. */
static uint32_t default_sizeimage(const struct v4l2_pix_format_mplane *pix)
{
    uint64_t sz = (uint64_t)pix->width * pix->height * 3 / 2;

    if (sz == 0 || sz > (64u << 20))
        sz = 1u << 20;
    return (uint32_t)sz;
}

static void normalise_fmt(struct v4l2_format *f)
{
    struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
    uint32_t p;

    if (pix->num_planes == 0 || pix->num_planes > MOCK_MAX_PLANES)
        pix->num_planes = 1;
    for (p = 0; p < pix->num_planes; p++)
        if (pix->plane_fmt[p].sizeimage == 0)
            pix->plane_fmt[p].sizeimage = default_sizeimage(pix);
}

int mock_do_fmt(struct mock_ctx *ctx, unsigned long req, struct v4l2_format *f)
{
    int qi = mock_qidx(f->type), ret = 0;
    struct mock_queue *q;

    if (qi < 0) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&ctx->mu);
    q = &ctx->q[qi];
    if (req == VIDIOC_G_FMT) {
        *f = q->fmt;
    } else if (req == VIDIOC_TRY_FMT) {
        normalise_fmt(f);
        if (ctx->ops && ctx->ops->on_fmt)
            ctx->ops->on_fmt(ctx, qi, f);
    } else if (q->streaming) {
        ret = EBUSY;                     /* S_FMT while streaming */
    } else {
        normalise_fmt(f);
        if (ctx->ops && ctx->ops->on_fmt)
            ctx->ops->on_fmt(ctx, qi, f);
        q->fmt = *f;
    }
    pthread_mutex_unlock(&ctx->mu);
    if (ret) {
        errno = ret;
        return -1;
    }
    return 0;
}

/* The visible rectangle defaults to the full coded size, which is what the
 * decoder reports through G_CROP after a resolution change. */
static struct v4l2_rect *crop_rect(struct mock_queue *q)
{
    if (q->crop.width == 0 || q->crop.height == 0) {
        q->crop.left = q->crop.top = 0;
        q->crop.width = q->fmt.fmt.pix_mp.width;
        q->crop.height = q->fmt.fmt.pix_mp.height;
    }
    return &q->crop;
}

int mock_do_crop(struct mock_ctx *ctx, unsigned long req, void *arg)
{
    struct v4l2_crop *c = arg;
    struct v4l2_selection *s = arg;
    struct v4l2_streamparm *sp = arg;
    int qi;

    pthread_mutex_lock(&ctx->mu);
    switch (req) {
    case VIDIOC_G_CROP:
    case VIDIOC_S_CROP:
        qi = mock_qidx(c->type);
        if (qi >= 0) {
            if (req == VIDIOC_S_CROP)
                ctx->q[qi].crop = c->c;
            else
                c->c = *crop_rect(&ctx->q[qi]);
        }
        break;
    case VIDIOC_G_SELECTION:
    case VIDIOC_S_SELECTION:
        qi = mock_qidx(s->type);
        if (qi >= 0) {
            if (req == VIDIOC_S_SELECTION)
                ctx->q[qi].crop = s->r;
            else
                s->r = *crop_rect(&ctx->q[qi]);
        }
        break;
    default:                             /* S_PARM / G_PARM: echo back */
        qi = mock_qidx(sp->type);
        if (req == VIDIOC_G_PARM && qi >= 0)
            memset(&sp->parm, 0, sizeof(sp->parm));
        break;
    }
    pthread_mutex_unlock(&ctx->mu);
    if (qi < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static bool subscribed(const struct mock_ctx *ctx, uint32_t type)
{
    uint32_t i;

    for (i = 0; i < ctx->ev_nsub; i++)
        if (ctx->ev_sub[i] == type || ctx->ev_sub[i] == V4L2_EVENT_ALL)
            return true;
    return false;
}

void mock_push_event(struct mock_ctx *ctx, uint32_t type, uint32_t u32)
{
    struct v4l2_event *e;
    struct timespec ts;

    if (!subscribed(ctx, type))
        return;                          /* like the driver: not delivered */
    if (ctx->ev_count == MOCK_MAX_EVENTS) {   /* drop oldest on overflow  */
        ctx->ev_head = (ctx->ev_head + 1) % MOCK_MAX_EVENTS;
        ctx->ev_count--;
    }
    e = &ctx->events[(ctx->ev_head + ctx->ev_count) % MOCK_MAX_EVENTS];
    memset(e, 0, sizeof(*e));
    e->type = type;
    memcpy(e->u.data, &u32, sizeof(u32));  /* src_change.changes lives here */
    e->sequence = ctx->ev_seq++;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    e->timestamp = ts;
    ctx->ev_count++;
    pthread_cond_broadcast(&ctx->cv);
    mock_update_ready(ctx);
}

int mock_do_event(struct mock_ctx *ctx, unsigned long req, void *arg)
{
    int ret = 0;

    pthread_mutex_lock(&ctx->mu);
    if (req == VIDIOC_SUBSCRIBE_EVENT) {
        struct v4l2_event_subscription *sub = arg;

        if (!subscribed(ctx, sub->type)) {
            if (ctx->ev_nsub == MOCK_MAX_EVENTS)
                ret = ENOMEM;
            else
                ctx->ev_sub[ctx->ev_nsub++] = sub->type;
        }
    } else if (req == VIDIOC_UNSUBSCRIBE_EVENT) {
        struct v4l2_event_subscription *sub = arg;
        uint32_t i, n = 0;

        for (i = 0; i < ctx->ev_nsub; i++)
            if (sub->type != V4L2_EVENT_ALL && ctx->ev_sub[i] != sub->type)
                ctx->ev_sub[n++] = ctx->ev_sub[i];
        ctx->ev_nsub = n;
    } else {                             /* VIDIOC_DQEVENT */
        if (ctx->ev_count == 0) {
            /* The Tegra driver reports "no event" as EAGAIN, which
             * NvV4l2Element::dqEvent retries until its timeout. */
            ret = EAGAIN;
        } else {
            *(struct v4l2_event *)arg = ctx->events[ctx->ev_head];
            ctx->ev_head = (ctx->ev_head + 1) % MOCK_MAX_EVENTS;
            ctx->ev_count--;
            ((struct v4l2_event *)arg)->pending = ctx->ev_count;
            mock_update_ready(ctx);
        }
    }
    pthread_mutex_unlock(&ctx->mu);
    if (ret) {
        errno = ret;
        return -1;
    }
    return 0;
}
