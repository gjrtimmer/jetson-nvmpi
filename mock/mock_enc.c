/*
 * mock_enc.c - encoder model of the V4L2 mock (issue #100).
 *
 * Plugs into the queue machinery of mock_v4l2_*.c through struct mock_ops and
 * behaves like a Tegra stateful encoder as seen by libnvmpi (note the plane
 * naming is the device's: OUTPUT takes raw frames, CAPTURE emits bitstream):
 *
 *   OUTPUT (raw)     Buffers are consumed IN ORDER, one per frame, each
 *                    needing a free CAPTURE buffer (back-pressure: without
 *                    one the frame stays QUEUED). A consumed buffer is
 *                    returned to the application at once, or - with
 *                    NVMPI_MOCK_ENC_LATENCY=N - only after N more frames were
 *                    queued, which keeps put_frame() starved of OUTPUT
 *                    buffers so the EAGAIN path can be driven. Returned
 *                    OUTPUT buffers report bytesused 0 (EOS after the buffer
 *                    pool wrapped relies on that; see enc_release()).
 *   CAPTURE (AUs)    One Annex-B access unit per frame (H.264 or HEVC from
 *                    the CAPTURE pixelformat, picture size from the OUTPUT
 *                    format; mock_enc_nal.c), the input timestamp copied
 *                    through, KEYFRAME/PFRAME flags set. H.264 is decodable
 *                    (#117): key frames are I_PCM macroblocks carrying the
 *                    OUTPUT picture (mock_enc_pic.c), P frames are all
 *                    skip; HEVC likewise with PCM CUs and CABAC (#118). The
 *                    slice RBSP is built in a heap scratch buffer sized from
 *                    the picture (enc_scratch_size) and the NAL units are
 *                    written straight into the mapped CAPTURE buffer; an AU
 *                    that does not fit its length, or a picture above
 *                    MOCK_ENC_MAX_PIXELS, completes with V4L2_BUF_FLAG_ERROR.
 *                    AV1 emits one OBU temporal unit instead (#8, mock_enc_av1.c).
 *   GOP              Frame 0, a forced IDR (FORCE_FRAME_TYPE), every
 *                    IDR_INTERVAL frames: IDR. Every GOP_SIZE frames: key
 *                    frame (periodic I). SPS/PPS (+VPS) lead the first IDR
 *                    always and later IDRs only with INSERT_SPS_PPS_AT_IDR
 *                    (the extradata extraction path reads the first AU).
 *                    INSERT_VUI adds timing_info (frame rate from S_PARM on
 *                    the OUTPUT queue, default 30) to every emitted SPS.
 *   EOS              An OUTPUT buffer with bytesused 0 returns every held
 *                    buffer, then yields an empty CAPTURE buffer (bytesused 0,
 *                    V4L2_BUF_FLAG_LAST) - libnvmpi's capture callback treats
 *                    that as end of stream. Input after EOS is dropped until
 *                    the OUTPUT queue is streamed off.
 *   STREAMOFF        OUTPUT: stream state is reset (flush). Blocked DQBUF
 *                    callers are woken by the core (EINVAL: the plane's DQ
 *                    thread ends, as on the device).
 *
 * Environment (read at open): NVMPI_MOCK_ENC_LATENCY (0..32, default 0),
 * NVMPI_MOCK_DQBUF_WAIT_MS (bounds a blocking DQBUF; default unbounded).
 *
 * Locking: every hook runs with ctx->mu held (see mock_v4l2.h); the model
 * never blocks.
 */
#define _GNU_SOURCE
#include "mock_v4l2.h"
#include "mock_enc.h"
#include "mock_env.h"
#include "mock_log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* NVIDIA fourcc (v4l2_nv_extensions.h): 10-bit 4:2:0, not in mainline. */
#define MOCK_PIX_FMT_P010M v4l2_fourcc('P', 'M', '1', '0')

static void enc_on_open(struct mock_ctx *ctx)
{
    /* Allocated here; freed in enc_on_close() (single deallocation point). */
    struct enc_model *m = calloc(1, sizeof(*m));
    uint32_t v = 0;

    if (!m)
        return;                    /* ctx->model NULL: model stays inert    */
    if (mock_env_uint("NVMPI_MOCK_ENC_LATENCY", 0, MOCK_ENC_MAX_LATENCY, &v))
        m->latency = v;
    ctx->dq_wait_ms = 0;
    if (mock_env_uint("NVMPI_MOCK_DQBUF_WAIT_MS", 0, 60000, &v))
        ctx->dq_wait_ms = v;
    ctx->model = m;
}

/* Raw frame layout of the OUTPUT plane, decided by the "driver": per plane a
 * bytesperline (aligned to 32 like the hardware's pitch, so stride != width
 * is exercised) and sizeimage = bytesperline * plane height. NvV4l2Element
 * reads both back after S_FMT; a zero stride would make every frame empty.
 * Subsampled planes round UP for an odd picture size ((w + 1) / 2): a 4:2:0
 * picture has that many chroma samples and the driver sizes the plane for
 * them (#154). */
/* Bytes the RBSP scratch buffer needs for a w x h picture: a PCM key frame
 * (#117, #118) takes MOCK_ENC_MB_MAX bytes per 16x16 unit of the picture
 * rounded up to whole units, plus header room (derivation in mock_enc.h). 0 = refuse (above the pixel bound; also keeps the product
 * far from size_t overflow). */
static size_t enc_scratch_size(uint32_t w, uint32_t h)
{
    uint64_t px = (uint64_t)w * h;
    uint64_t nmb = (uint64_t)((w + 15) / 16) * ((h + 15) / 16);

    if (px > MOCK_ENC_MAX_PIXELS)
        return 0;
    return (size_t)(nmb * MOCK_ENC_MB_MAX) + MOCK_ENC_AU_HEADROOM;
}

/* (Re)size the AU scratch buffer for the OUTPUT picture. The buffer is
 * only replaced when the size changes, never while an AU is being built
 * (every hook runs under ctx->mu). A failed calloc leaves scratch_cap 0, so
 * frames error out instead of dereferencing NULL. */
static void enc_set_scratch(struct enc_model *m, uint32_t w, uint32_t h)
{
    size_t want = enc_scratch_size(w, h);

    if (want == m->scratch_cap)
        return;
    free(m->scratch);              /* old buffer: single owner is the model */
    m->scratch = NULL;
    m->scratch_cap = 0;
    if (!want)
        return;
    /* Allocated here; freed in enc_on_close() or on the next resize.
     * Zeroed so stale heap never reaches a CAPTURE buffer by accident. */
    m->scratch = calloc(1, want);
    if (m->scratch)
        m->scratch_cap = want;
}

static void enc_on_fmt(struct mock_ctx *ctx, int qi, struct v4l2_format *f)
{
    struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
    /* Per plane: width divisor, height divisor, bytes per sample (an
     * interleaved CbCr sample is 2 bytes, 4 for 16-bit P010). */
    static const struct { uint32_t n, wd[3], hd[3], bpp[3]; uint32_t fcc; } k[] = {
        { 2, { 1, 2, 0 }, { 1, 2, 0 }, { 1, 2, 0 }, V4L2_PIX_FMT_NV12M },
        { 3, { 1, 2, 2 }, { 1, 2, 2 }, { 1, 1, 1 }, V4L2_PIX_FMT_YUV420M },
        { 2, { 1, 2, 0 }, { 1, 2, 0 }, { 2, 4, 0 }, MOCK_PIX_FMT_P010M },
        { 3, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, V4L2_PIX_FMT_YUV444M },
    };
    size_t i;
    uint32_t p;

    if (qi != MOCK_Q_OUTPUT || pix->width == 0 || pix->height == 0 ||
        pix->width > MOCK_ENC_MAX_DIM || pix->height > MOCK_ENC_MAX_DIM)
        return;
    if (ctx->model)
        enc_set_scratch(ctx->model, pix->width, pix->height);
    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (k[i].fcc != pix->pixelformat)
            continue;
        pix->num_planes = (uint8_t)k[i].n;
        for (p = 0; p < k[i].n; p++) {
            uint32_t w = (pix->width + k[i].wd[p] - 1) / k[i].wd[p];
            uint32_t h = (pix->height + k[i].hd[p] - 1) / k[i].hd[p];
            uint32_t stride = (w * k[i].bpp[p] + 31u) & ~31u;

            pix->plane_fmt[p].bytesperline = stride;
            pix->plane_fmt[p].sizeimage = stride * h;
        }
        return;
    }
}

static void enc_on_close(struct mock_ctx *ctx)
{
    if (ctx->model) {
        struct enc_model *m = ctx->model;

        if (m->scratch) {          /* AU bytes: zero before release     */
            memset(m->scratch, 0, m->scratch_cap);
            free(m->scratch);      /* single deallocation point         */
        }
        memset(m, 0, sizeof(*m));
        free(m);          /* single deallocation point             */
        ctx->model = NULL;
    }
}

/* Stream state of a fresh stream (open, OUTPUT STREAMOFF). The controls
 * (gop, idr_interval, headers_at_idr, latency) are device configuration and
 * survive a flush. */
static void reset_stream(struct enc_model *m)
{
    m->force_idr = m->eos_seen = false;
    m->frames = m->since_idr = m->since_key = m->held = 0;
}

/* Return consumed OUTPUT buffers (oldest first = FIFO head) until at most
 * `keep` stay held. bytesused is reported as 0: a raw-frame buffer that came
 * back from the device is empty, and libnvmpi's MMAP path re-queues the
 * plane's last bytesused for the EOS marker - a stale non-zero value there
 * would make EOS look like another frame. */
static void enc_release(struct mock_ctx *ctx, struct enc_model *m, uint32_t keep)
{
    struct mock_queue *out = &ctx->q[MOCK_Q_OUTPUT];
    static const uint32_t zero[MOCK_MAX_PLANES];

    while (m->held > keep && out->queued.count > 0) {
        (void)mock_complete_buf(ctx, MOCK_Q_OUTPUT,
                                (uint32_t)mock_peek_queued(ctx, MOCK_Q_OUTPUT),
                                zero, 0);
        m->held--;
    }
}

/* Render one AU into CAPTURE buffer ci. Returns bytes written; 0 = failure
 * (picture refused, AU larger than the buffer, CAPTURE not MMAP, mapping
 * failed, unsupported format): the caller flags the buffer with ERROR. The
 * RBSP lives in m->scratch, the NAL units go straight into the CAPTURE
 * mapping (separate memory, no aliasing); a failed AU may leave partial
 * bytes there, but it completes flagged ERROR with bytesused 1. */
static size_t write_au(struct mock_ctx *ctx, struct enc_model *m, int ci,
                       const struct mock_enc_au *au)
{
    struct mock_queue *cap = &ctx->q[MOCK_Q_CAPTURE];
    struct mock_buf *cb = &cap->bufs[ci];
    size_t n;
    void *p;

    if (!m->scratch || cap->memory != V4L2_MEMORY_MMAP || cb->memfd < 0 ||
        cb->length[0] == 0) {
        MOCK_ERR("encoder: no AU buffer (picture refused or CAPTURE not MMAP)");
        return 0;
    }
    /* The plane offset inside the memfd is page aligned (queue.c). Mapped
     * here; unmapped below (single point), whatever the build result. */
    p = mmap(NULL, cb->length[0], PROT_WRITE, MAP_SHARED, cb->memfd,
             (off_t)cb->plane_off[0]);
    if (p == MAP_FAILED)
        return 0;
    n = mock_enc_build_au(au, p, cb->length[0], m->scratch, m->scratch_cap);
    munmap(p, cb->length[0]);
    if (!n)
        MOCK_ERR("encoder: AU does not fit (capture buffer %u, scratch %zu)",
                 cb->length[0], m->scratch_cap);
    return n;
}

/* Encode the OUTPUT buffer at FIFO position `held` (the first unconsumed)
 * into CAPTURE buffer ci. */
static void encode_frame(struct mock_ctx *ctx, struct enc_model *m, int oi, int ci)
{
    struct mock_queue *out = &ctx->q[MOCK_Q_OUTPUT];
    struct mock_queue *cap = &ctx->q[MOCK_Q_CAPTURE];
    struct v4l2_pix_format_mplane *raw = &out->fmt.fmt.pix_mp;
    struct mock_enc_au au;
    struct mock_enc_pic pic;
    uint32_t used[MOCK_MAX_PLANES] = { 0 }, flags;
    bool idr = m->frames == 0 || m->force_idr ||
               (m->idr_interval && m->since_idr >= m->idr_interval);
    bool key = idr || (m->gop && m->since_key >= m->gop);
    size_t n;

    memset(&au, 0, sizeof(au));
    au.pixfmt = cap->fmt.fmt.pix_mp.pixelformat;
    au.width = raw->width;
    au.height = raw->height;
    au.idr = idr;
    au.key = key;
    au.headers = idr && (m->frames == 0 || m->headers_at_idr);
    au.main10 = m->main10;
    au.seq = m->seq++;
    au.poc = idr ? 0 : m->since_idr;     /* since_idr counts the last IDR */
    if (m->insert_vui) {                 /* timeperframe from S_PARM      */
        au.tpf_num = out->tpf.numerator;
        au.tpf_den = out->tpf.denominator;
    }
    /* The OUTPUT frame supplies the PCM samples of key frames. Mapped here;
     * unmapped right after the AU is written. Unreadable input becomes a pattern, never fails. */
    mock_enc_pic_map(out, oi, au.seq, &pic);
    au.pic = &pic;
    n = write_au(ctx, m, ci, &au);
    mock_enc_pic_unmap(&pic);
    flags = n ? 0 : V4L2_BUF_FLAG_ERROR;
    flags |= key ? V4L2_BUF_FLAG_KEYFRAME : V4L2_BUF_FLAG_PFRAME;
    used[0] = n ? (uint32_t)n : 1;       /* never 0: 0 means EOS to libnvmpi */
    cap->bufs[ci].ts_ns = out->bufs[oi].ts_ns; /* timestamp copy (pts passthrough) */
    if (ci < (int)sizeof(m->key))
        m->key[ci] = key;
    (void)mock_complete_buf(ctx, MOCK_Q_CAPTURE, (uint32_t)ci, used, flags);

    m->force_idr = false;
    m->since_idr = idr ? 1 : m->since_idr + 1;
    m->since_key = key ? 1 : m->since_key + 1;
    m->frames++;
    MOCK_TRACE("encoder: frame %u -> capture %d (%zu bytes%s%s)", m->frames,
               ci, n, idr ? ", IDR" : key ? ", key" : "", au.headers ? ", hdr" : "");
}

/* EOS marker consumed: flush everything, emit the empty LAST buffer. */
static void encode_eos(struct mock_ctx *ctx, struct enc_model *m, int ci)
{
    uint32_t used[MOCK_MAX_PLANES] = { 0 };

    m->eos_seen = true;
    (void)mock_complete_buf(ctx, MOCK_Q_CAPTURE, (uint32_t)ci, used,
                            V4L2_BUF_FLAG_LAST);
    m->held++;                         /* the marker itself is consumed too */
    enc_release(ctx, m, 0);
}

/* Run the encoder as far as the queues allow. */
static void enc_run(struct mock_ctx *ctx)
{
    struct enc_model *m = ctx->model;
    struct mock_queue *out = &ctx->q[MOCK_Q_OUTPUT];
    struct mock_queue *cap = &ctx->q[MOCK_Q_CAPTURE];

    if (!m)
        return;
    while (out->streaming && out->queued.count > m->held) {
        int oi = out->queued.idx[(out->queued.head + m->held) % MOCK_MAX_BUFS];
        int ci;
        bool eos = out->bufs[oi].bytesused[0] == 0;

        if (m->eos_seen && !eos) {     /* stopped after EOS: drop input     */
            m->held++;
            enc_release(ctx, m, m->latency);
            continue;
        }
        if (eos && m->eos_seen) {      /* repeated marker: nothing to emit  */
            m->held++;
            enc_release(ctx, m, 0);
            continue;
        }
        if (!cap->streaming || (ci = mock_peek_queued(ctx, MOCK_Q_CAPTURE)) < 0) {
            MOCK_TRACE("encoder: hold frame (capture streaming=%d)",
                       cap->streaming);
            break;                     /* hold: no destination buffer yet   */
        }
        if (eos) {
            encode_eos(ctx, m, ci);
            continue;
        }
        encode_frame(ctx, m, oi, ci);
        m->held++;
        enc_release(ctx, m, m->latency);
    }
}

static int enc_on_qbuf(struct mock_ctx *ctx, int qi, uint32_t index)
{
    (void)qi;
    (void)index;
    enc_run(ctx);
    return 0;
}

static int enc_on_streamon(struct mock_ctx *ctx, int qi)
{
    (void)qi;
    enc_run(ctx);
    return 0;
}

static void enc_on_streamoff(struct mock_ctx *ctx, int qi)
{
    struct enc_model *m = ctx->model;

    if (m && qi == MOCK_Q_OUTPUT)      /* flush: the core already returned
                                        * every OUTPUT buffer to the caller */
        reset_stream(m);
}

const struct mock_ops mock_enc_ops = {
    .on_open = enc_on_open,
    .on_close = enc_on_close,
    .on_qbuf = enc_on_qbuf,
    .on_streamon = enc_on_streamon,
    .on_streamoff = enc_on_streamoff,
    .on_fmt = enc_on_fmt,
    .on_ioctl = mock_enc_ioctl,
};
