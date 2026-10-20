/*
 * mock_enc_pic.c - the raw input picture of the encoder model (#117).
 *
 * The H.264 key frames of the model are I_PCM (mock_enc_h264.c), so they can
 * carry the frame the application queued on the OUTPUT plane and a real
 * decoder reproduces it byte for byte. This file maps that OUTPUT buffer and
 * serves its samples to the builder:
 *
 *   MMAP     the buffer's memfd (planes at their page-aligned offsets, rows
 *            at the bytesperline the mock reported from S_FMT).
 *   DMABUF   the dma-buf fd of plane 0. It is an NvBufSurface memfd (the
 *            only kind the mock hands out, mock_nvbufsurface.c), so the
 *            planes sit at the surface layout: pitch aligned to
 *            MOCK_NVBUF_PITCH_ALIGN, plane size rounded to a page.
 *   formats  NV12M and YUV420M (8-bit 4:2:0) are read. Anything else (P010M,
 *            YUV444M, USERPTR, a foreign fd) cannot be turned into the
 *            8-bit 4:2:0 PCM samples, so the picture falls back to a
 *            deterministic pattern of (x, y, frame). That keeps the AU
 *            valid and decodable and only loses pixel identity, so a
 *            fallback is traced (MOCK_TRACE), not an error: a real encoder
 *            would have encoded such a frame fine, failing it would be less
 *            faithful.
 *
 * Every sample access is bounds-checked against the plane length and the row
 * stride; a sample outside the mapping is replaced by the pattern. Samples
 * beyond the picture (macroblock padding) repeat the nearest edge sample.
 * Lifecycle: mock_enc_pic_map() maps, mock_enc_pic_unmap() unmaps; the
 * mapping lives for one frame (encode_frame()), under ctx->mu.
 */
#define _GNU_SOURCE
#include "mock_v4l2.h"
#include "mock_enc.h"
#include "mock_log.h"
#include "mock_nvbuf_util.h"

#include <sys/mman.h>
#include <sys/stat.h>

enum { PIC_NV12 = 1, PIC_I420 = 2 };

/* Sample value when the input is unavailable: never 0 (PCM samples are
 * clamped to >= 1 so no 00 00 0x can arise inside them). */
static uint8_t pattern(unsigned comp, uint32_t x, uint32_t y, uint32_t seq)
{
    uint32_t v = comp == 0 ? x + y + seq : 2 * x + 3 * y + 5 * seq + 40 * comp;

    return (uint8_t)(1 + v % 254);
}

/* Byte layout of the planes inside the mapping. Returns the planes needed
 * (0 = format not readable); fills the per-plane offset/stride/length. */
static unsigned pic_layout(const struct mock_queue *q, const struct mock_buf *b,
                           uint32_t w, uint32_t h, uint32_t off[3],
                           uint32_t stride[3], uint32_t len[3])
{
    const struct v4l2_pix_format_mplane *pix = &q->fmt.fmt.pix_mp;
    unsigned fmt = pix->pixelformat == V4L2_PIX_FMT_NV12M ? PIC_NV12 :
                   pix->pixelformat == V4L2_PIX_FMT_YUV420M ? PIC_I420 : 0;
    unsigned n = fmt == PIC_NV12 ? 2 : 3, p;

    if (!fmt || b->n_planes < n || w == 0 || h == 0)
        return 0;
    if (q->memory == V4L2_MEMORY_MMAP) {
        for (p = 0; p < n; p++) {
            off[p] = b->plane_off[p];
            stride[p] = pix->plane_fmt[p].bytesperline;
            len[p] = b->length[p];
        }
        return fmt;
    }
    /* DMABUF: NvBufSurface layout (see layout_surface() in
     * mock_nvbufsurface.c): planes back to back, page-rounded. */
    {
        uint64_t at = 0;

        for (p = 0; p < n; p++) {
            uint32_t pw = p && fmt == PIC_NV12 ? (w / 2) * 2 : p ? w / 2 : w;
            uint32_t ph = p ? h / 2 : h;

            stride[p] = mock_align_up(pw, MOCK_NVBUF_PITCH_ALIGN);
            len[p] = stride[p] * ph;
            if (at > UINT32_MAX)
                return 0;
            off[p] = (uint32_t)at;
            at += mock_align_up(len[p], MOCK_NVBUF_PAGE);
        }
    }
    return fmt;
}

void mock_enc_pic_map(const struct mock_queue *out, int oi, uint32_t seq,
                      struct mock_enc_pic *pic)
{
    const struct mock_buf *b = &out->bufs[oi];
    const struct v4l2_pix_format_mplane *pix = &out->fmt.fmt.pix_mp;
    uint32_t off[3] = { 0 }, stride[3] = { 0 }, len[3] = { 0 };
    uint64_t total = 0;
    unsigned fmt, n, p;
    int fd = -1;
    struct stat st;
    uint8_t *base;

    memset(pic, 0, sizeof(*pic));      /* no planes = pattern only          */
    pic->w = pix->width;
    pic->h = pix->height;
    pic->seq = seq;
    if (out->memory == V4L2_MEMORY_MMAP)
        fd = b->memfd;
    else if (out->memory == V4L2_MEMORY_DMABUF)
        fd = b->dmabuf_fd[0];
    fmt = pic_layout(out, b, pic->w, pic->h, off, stride, len);
    if (fd < 0 || !fmt) {
        MOCK_TRACE("encoder: input not readable (memory %u, format 0x%x): pattern",
                   out->memory, pix->pixelformat);
        return;
    }
    n = fmt == PIC_NV12 ? 2 : 3;
    for (p = 0; p < n; p++)            /* mapping spans the last plane's end */
        if (off[p] + (uint64_t)len[p] > total)
            total = off[p] + (uint64_t)len[p];
    /* A foreign or too small fd must not be mapped past its end (SIGBUS). */
    if (total == 0 || total > SIZE_MAX || fstat(fd, &st) < 0 ||
        !S_ISREG(st.st_mode) || (uint64_t)st.st_size < total) {
        MOCK_TRACE("encoder: input fd unusable (%llu bytes needed): pattern",
                   (unsigned long long)total);
        return;
    }
    base = mmap(NULL, (size_t)total, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        MOCK_TRACE("encoder: mmap of the input failed: pattern");
        return;
    }
    /* Mapped here; unmapped in mock_enc_pic_unmap() (single point). */
    pic->map = base;
    pic->map_len = (size_t)total;
    pic->layout = fmt;
    for (p = 0; p < n; p++) {
        pic->plane[p] = base + off[p];
        pic->stride[p] = stride[p];
        pic->len[p] = len[p];
    }
}

void mock_enc_pic_unmap(struct mock_enc_pic *pic)
{
    if (pic->map)
        munmap(pic->map, pic->map_len);    /* single unmap point            */
    memset(pic, 0, sizeof(*pic));
}

/* One sample of component comp (0 Y, 1 U, 2 V) at (x, y) in that
 * component's own coordinates. *ok = 0 when it is not inside the mapping. */
static uint8_t pic_sample(const struct mock_enc_pic *p, unsigned comp,
                          uint32_t x, uint32_t y, int *ok)
{
    bool nv = p->layout == PIC_NV12 && comp;
    unsigned pl = nv ? 1 : comp;
    uint64_t col = nv ? (uint64_t)x * 2 + (comp - 1) : x;
    uint64_t at = (uint64_t)y * p->stride[pl] + col;

    /* Both bounds: the column must stay inside the row (else it would read
     * the next row's pixels) and the byte inside the plane. */
    *ok = p->plane[pl] && col < p->stride[pl] && at < p->len[pl];
    return *ok ? p->plane[pl][at] : 0;
}

void mock_enc_pic_row(const struct mock_enc_pic *p, unsigned comp, uint32_t x0,
                      uint32_t y, unsigned n, uint8_t *dst)
{
    uint32_t cw = comp ? (p->w + 1) / 2 : p->w;
    uint32_t ch = comp ? (p->h + 1) / 2 : p->h;
    uint32_t sy = y < ch ? y : ch - 1;     /* padding rows repeat the edge  */
    unsigned i;

    for (i = 0; i < n; i++) {
        uint32_t sx = x0 + i < cw ? x0 + i : cw - 1;
        int ok;
        uint8_t v = pic_sample(p, comp, sx, sy, &ok);

        if (!ok)
            v = pattern(comp, sx, sy, p->seq);
        dst[i] = v ? v : 1;                /* PCM samples are never 0       */
    }
}
