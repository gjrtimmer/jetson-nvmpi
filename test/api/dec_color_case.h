/*
 * dec_color_case.h - hardware case for the decoder output colorimetry
 * (#163), included by test_api.c.
 *
 * Four H.264 clips (BT.601 / BT.709, limited / full range, signalled in the
 * VUI; dec_color_fixture.h) are decoded through the copy path as NV12 and as
 * planar YUV420 and compared with a software decode of the same stream. NVDEC
 * is bit-exact, so the pictures must match. The bug was a VIC destination
 * tagged BT.601 limited: any other clip came out matrix- and range-converted,
 * tens of luma levels off on every row. Failure mode, not an absolute number:
 *   - pass: every compared sample (full rows) and every block mean within
 *     DECCOL_TOL levels of the software decode (exact on the Orin; 1 leaves
 *     room for a rounding difference of the copy);
 *   - fail: anything larger. A pass on bt601_limited with a fail on the other
 *     clips points at the colour tag; a fail on bt601_limited too points at
 *     the transform filter (see #163).
 * Max difference and PSNR per plane are printed for information.
 * Skipped on stubs (no frame) and on the mock (it only produces gray; the
 * tag is checked by test_api_dec_color_mock.c).
 */
#ifndef NVMPI_TEST_DEC_COLOR_CASE_H
#define NVMPI_TEST_DEC_COLOR_CASE_H

#include "dec_color_fixture.h"
#include "../mock/nal_scan.h"

#include <math.h>

#define DECCOL_TOL 1

struct deccol_clip {
    const char *name;
    const unsigned char *es;
    size_t es_len;
    const unsigned char *ymean, *umean, *vmean, *yrow, *urow, *vrow;
};

#define DECCOL_CLIP(n) {                                                      \
    #n, deccol_##n##_es, sizeof(deccol_##n##_es), deccol_##n##_ymean,         \
    deccol_##n##_umean, deccol_##n##_vmean, deccol_##n##_yrow,                \
    deccol_##n##_urow, deccol_##n##_vrow }

static const struct deccol_clip deccol_clips[] = {
    DECCOL_CLIP(bt601_limited), DECCOL_CLIP(bt709_limited),
    DECCOL_CLIP(bt601_full), DECCOL_CLIP(bt709_full),
};

/* Decoded frame as planes (NV12 is split into U and V). */
struct deccol_pic {
    unsigned char y[DECCOL_W * DECCOL_H];
    unsigned char u[DECCOL_W * DECCOL_H / 4];
    unsigned char v[DECCOL_W * DECCOL_H / 4];
};

/* One frame through the copy path; 0 on success. */
static int deccol_take(nvmpictx *ctx, nvPixFormat pf, struct deccol_pic *pic)
{
    static unsigned char uv[DECCOL_W * DECCOL_H / 2];
    nvFrame f;
    unsigned i;

    memset(&f, 0, sizeof(f));
    f.payload[0] = pic->y;
    f.linesize[0] = DECCOL_W;
    if (pf == NV_PIX_NV12) {
        f.payload[1] = uv;
        f.linesize[1] = DECCOL_W;
    } else {
        f.payload[1] = pic->u;
        f.payload[2] = pic->v;
        f.linesize[1] = f.linesize[2] = DECCOL_W / 2;
    }
    if (nvmpi_decoder_get_frame(ctx, &f, true) != 0)
        return -1;
    if (pf == NV_PIX_NV12)
        for (i = 0; i < DECCOL_W * DECCOL_H / 4; i++) {
            pic->u[i] = uv[2 * i];
            pic->v[i] = uv[2 * i + 1];
        }
    return 0;
}

/* Mean of each b x b block, rounded like the generator. */
static void deccol_means(const unsigned char *p, unsigned w, unsigned h,
                         unsigned b, unsigned char *out)
{
    unsigned bx, by, x, y;

    for (by = 0; by < h / b; by++)
        for (bx = 0; bx < w / b; bx++) {
            unsigned s = 0;

            for (y = 0; y < b; y++)
                for (x = 0; x < b; x++)
                    s += p[(by * b + y) * w + bx * b + x];
            out[by * (w / b) + bx] = (unsigned char)((s + b * b / 2) / (b * b));
        }
}

/* Largest absolute difference and PSNR (255 peak) of n samples. */
static unsigned deccol_diff(const unsigned char *a, const unsigned char *b,
                            size_t n, double *psnr)
{
    unsigned mx = 0;
    double se = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        int d = (int)a[i] - (int)b[i];
        unsigned ad = (unsigned)(d < 0 ? -d : d);

        if (ad > mx)
            mx = ad;
        se += (double)d * d;
    }
    *psnr = se == 0 ? INFINITY : 10 * log10(255.0 * 255.0 * (double)n / se);
    return mx;
}

/* Compare plane `pl` (0 Y, 1 U, 2 V) of one frame: sampled rows in full and
 * the block means. Returns the larger of the two max differences. */
static unsigned deccol_plane(const struct deccol_clip *c, unsigned fr,
                             unsigned pl, const struct deccol_pic *pic,
                             const char *tag)
{
    static const unsigned yrows[] = DECCOL_YROWS;
    unsigned char mean[DECCOL_BLOCKS], row[3 * DECCOL_W];
    const unsigned char *plane = pl == 0 ? pic->y : pl == 1 ? pic->u : pic->v;
    const unsigned w = pl ? DECCOL_W / 2 : DECCOL_W;
    const unsigned h = pl ? DECCOL_H / 2 : DECCOL_H;
    const unsigned nrows = pl ? 1 : 3;
    const unsigned char *rmean = pl == 0 ? c->ymean : pl == 1 ? c->umean : c->vmean;
    const unsigned char *rrow = pl == 0 ? c->yrow : pl == 1 ? c->urow : c->vrow;
    double p_mean, p_row;
    unsigned i, d_mean, d_row;

    deccol_means(plane, w, h, pl ? 8 : 16, mean);
    for (i = 0; i < nrows; i++)
        memcpy(row + i * w, plane + (size_t)(pl ? DECCOL_CROW : yrows[i]) * w, w);
    d_mean = deccol_diff(mean, rmean + (size_t)fr * DECCOL_BLOCKS, DECCOL_BLOCKS, &p_mean);
    d_row = deccol_diff(row, rrow + (size_t)fr * nrows * w, (size_t)nrows * w, &p_row);
    fprintf(stderr, "dec_color[%s %s] frame %u %c: rows max %u psnr %.1f, block means max %u psnr %.1f\n",
            c->name, tag, fr, "YUV"[pl], d_row, p_row, d_mean, p_mean);
    return d_row > d_mean ? d_row : d_mean;
}

/* Decode one clip as pf. 0 pass, 1 fail, 77 no decode path. */
static int deccol_run(const struct deccol_clip *c, nvPixFormat pf)
{
    static struct deccol_pic pic;
    const char *tag = pf == NV_PIX_NV12 ? "nv12" : "yuv420";
    size_t off[DECCOL_FRAMES + 1], len[DECCOL_FRAMES + 1];
    unsigned n_au, i, pl, worst = 0;
    int got = 0, bad = 0;
    nvDecParam param;
    nvPacket pkt;
    nvmpictx *ctx;

    n_au = nal_split_aus(c->es, c->es_len, 0, off, len, DECCOL_FRAMES + 1);
    if (n_au != DECCOL_FRAMES) {
        fprintf(stderr, "dec_color FAIL %s: fixture splits into %u AUs\n", c->name, n_au);
        return 1;
    }
    dec_param_init(&param, NV_VIDEO_CodingH264);
    param.pixFormat = pf;
    param.frame_pool_size = 4;
    param.wait_timeout = 2000;           /* ms; the first frame pays NVDEC start-up */
    ctx = nvmpi_create_decoder(&param);
    if (!ctx)
        return strict_mode ? 1 : 77;

    for (i = 0; i < n_au; i++) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.payload = (unsigned char *)c->es + off[i];
        pkt.payload_size = len[i];
        pkt.pts = 1 + i;
        if (nvmpi_decoder_put_packet(ctx, &pkt) < 0) {
            nvmpi_decoder_close(ctx);
            fprintf(stderr, "dec_color FAIL %s %s: put_packet\n", c->name, tag);
            return 1;
        }
    }
    memset(&pkt, 0, sizeof(pkt));        /* EOS marker: drains the decoder */
    if (nvmpi_decoder_put_packet(ctx, &pkt) < 0) {
        nvmpi_decoder_close(ctx);
        fprintf(stderr, "dec_color FAIL %s %s: EOS put_packet\n", c->name, tag);
        return 1;
    }
    while (got < DECCOL_FRAMES && deccol_take(ctx, pf, &pic) == 0) {
        for (pl = 0; pl < 3; pl++) {
            unsigned d = deccol_plane(c, (unsigned)got, pl, &pic, tag);

            if (d > worst)
                worst = d;
        }
        got++;
    }
    if (nvmpi_decoder_close(ctx) != 0)
        bad = 1;
    if (got == 0 && !strict_mode)
        return 77;                       /* no decode path here */
    if (got != DECCOL_FRAMES || bad || worst > DECCOL_TOL) {
        fprintf(stderr, "dec_color FAIL %s %s: %d/%d frames, max diff %u (tolerance %d)\n",
                c->name, tag, got, DECCOL_FRAMES, worst, DECCOL_TOL);
        return 1;
    }
    fprintf(stderr, "dec_color %s %s: ok (max diff %u)\n", c->name, tag, worst);
    return 0;
}

/* All clips x {NV12, YUV420}; every combination runs, one failure fails the case. */
static int test_decoder_color_exact(void)
{
    unsigned i, f;
    int rc, fail = 0, skip = 0;

#ifdef NVMPI_MOCK_JPEG
    return 77;                           /* the mock decodes gray only */
#endif
    for (i = 0; i < sizeof(deccol_clips) / sizeof(deccol_clips[0]); i++)
        for (f = 0; f < 2; f++) {
            rc = deccol_run(&deccol_clips[i], f ? NV_PIX_YUV420 : NV_PIX_NV12);
            if (rc == 77)
                skip = 1;
            else if (rc)
                fail = 1;
        }
    return fail ? 1 : skip ? 77 : 0;
}

#endif /* NVMPI_TEST_DEC_COLOR_CASE_H */
