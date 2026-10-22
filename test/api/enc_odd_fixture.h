/*
 * enc_odd_fixture.h - odd-size encoder input for the chroma cases (#154):
 * a 641x357 4:2:0 picture (321x179 chroma samples, one more column and row
 * than the floored 320x178) as NV12 or planar YUV420, with a sentinel in the
 * last chroma column and the last chroma row. Shared by the hardware case
 * (enc_odd_case.h) and the encoder mock suite (enc_mock_oddchroma.h).
 * Header-only; needs <nvmpi.h>, <stdio.h>, <stdlib.h>, <string.h> first.
 *
 * Each plane is its own exact-size malloc (no slack), so under ASan a read
 * past what the frame declares is a heap-buffer-overflow.
 */
#ifndef NVMPI_TEST_ENC_ODD_FIXTURE_H
#define NVMPI_TEST_ENC_ODD_FIXTURE_H

#define OC_W 641u
#define OC_H 357u
#define OC_CW ((OC_W + 1) / 2)            /* 321 chroma columns            */
#define OC_CH ((OC_H + 1) / 2)            /* 179 chroma rows               */

/* How the caller sizes and declares its chroma planes. */
enum oc_mode {
    OC_FULL,       /* ceil rows allocated, payload_size = their exact size  */
    OC_ZERO,       /* ceil rows allocated, payload_size left 0 (legacy)     */
    OC_FLOORED,    /* old wrapper: floored rows allocated and declared      */
    OC_UNDER       /* one byte below the floored need declared: refused    */
};

/* Smooth interior, distinctive last column / last row (lossy-codec safe). */
static unsigned char oc_u(unsigned x, unsigned y)
{
    return (x == OC_CW - 1 || y == OC_CH - 1) ? 220 : 90 + (x / 32 + y / 32) % 40;
}

static unsigned char oc_v(unsigned x, unsigned y)
{
    return (x == OC_CW - 1 || y == OC_CH - 1) ? 36 : 160 - (x / 32 + y / 32) % 40;
}

static unsigned char oc_luma(unsigned x, unsigned y)
{
    return (unsigned char)(16 + (x / 4 + y / 4) % 200);
}

struct oc_pic {
    nvFrame f;
    unsigned char *mem[3];
};

static void oc_free(struct oc_pic *p)
{
    for (int i = 0; i < 3; i++) {
        free(p->mem[i]);
        p->mem[i] = NULL;
    }
}

/* Build the frame for pix (NV_PIX_NV12 or NV_PIX_YUV420) in the given mode.
 * Floored modes allocate (and declare) only the floored rows, so the last
 * chroma row is not in the caller's memory at all. Returns 0, or -1 on
 * allocation failure. */
static int oc_build(struct oc_pic *p, nvPixFormat pix, enum oc_mode mode, long pts)
{
    int nv = pix == NV_PIX_NV12;
    unsigned crow = nv ? 2 * OC_CW : OC_CW;            /* chroma row bytes */
    unsigned rows = (mode == OC_FLOORED || mode == OC_UNDER) ? OC_CH - 1 : OC_CH;
    unsigned np = nv ? 2 : 3, x, y;
    size_t sz[3];

    memset(p, 0, sizeof(*p));
    sz[0] = (size_t)OC_W * OC_H;
    sz[1] = (size_t)crow * rows;
    sz[2] = nv ? 0 : sz[1];
    for (unsigned i = 0; i < np; i++) {
        p->mem[i] = calloc(1, sz[i]);
        if (!p->mem[i])
            return -1;
        p->f.payload[i] = p->mem[i];
        p->f.linesize[i] = i ? crow : OC_W;
        p->f.payload_size[i] = mode == OC_ZERO ? 0 : sz[i];
    }
    if (mode == OC_UNDER)
        p->f.payload_size[1] -= 2;   /* floored need = rows*linesize - 1 */
    for (y = 0; y < OC_H; y++)
        for (x = 0; x < OC_W; x++)
            p->mem[0][(size_t)y * OC_W + x] = oc_luma(x, y);
    for (y = 0; y < rows; y++)
        for (x = 0; x < OC_CW; x++) {
            if (nv) {
                p->mem[1][(size_t)y * crow + 2 * x] = oc_u(x, y);
                p->mem[1][(size_t)y * crow + 2 * x + 1] = oc_v(x, y);
            } else {
                p->mem[1][(size_t)y * crow + x] = oc_u(x, y);
                p->mem[2][(size_t)y * crow + x] = oc_v(x, y);
            }
        }
    p->f.type = pix;
    p->f.width = OC_W;
    p->f.height = OC_H;
    p->f.timestamp = pts;
    return 0;
}

/* Result of comparing a coded picture's chroma with the source. */
struct oc_cmp {
    double psnr_u, psnr_v;      /* over all chroma samples                  */
    double edge_diff;           /* mean |diff| of last column + last row    */
};

/* log10 without libm (the test binaries do not link it): scale x into
 * [1, 10), then ln(m) = 2 * atanh((m - 1) / (m + 1)) as a series. */
static double oc_log10(double x)
{
    double e = 0, t, t2, sum = 0, term;

    while (x >= 10.0) { x /= 10.0; e += 1.0; }
    while (x < 1.0) { x *= 10.0; e -= 1.0; }
    t = (x - 1.0) / (x + 1.0);
    t2 = t * t;
    term = t;
    for (int k = 1; k < 40; k += 2) {
        sum += term / k;
        term *= t2;
    }
    return e + 2.0 * sum / 2.302585092994046;
}

static double oc_abs(double d)
{
    return d < 0 ? -d : d;
}

static double oc_psnr(double sse, unsigned n)
{
    double mse = sse / n;

    return mse < 1e-9 ? 99.0 : 10.0 * oc_log10(255.0 * 255.0 / mse);
}

/* u/v: the coded chroma as two 321x179 planes (row stride OC_CW). */
static void oc_compare(const unsigned char *u, const unsigned char *v, struct oc_cmp *c)
{
    double su = 0, sv = 0, ed = 0;
    unsigned n = 0;

    for (unsigned y = 0; y < OC_CH; y++)
        for (unsigned x = 0; x < OC_CW; x++) {
            double du = (double)u[y * OC_CW + x] - oc_u(x, y);
            double dv = (double)v[y * OC_CW + x] - oc_v(x, y);

            su += du * du;
            sv += dv * dv;
            if (x == OC_CW - 1 || y == OC_CH - 1) {
                ed += oc_abs(du) + oc_abs(dv);
                n += 2;
            }
        }
    c->psnr_u = oc_psnr(su, OC_CW * OC_CH);
    c->psnr_v = oc_psnr(sv, OC_CW * OC_CH);
    c->edge_diff = ed / n;
}

#endif /* NVMPI_TEST_ENC_ODD_FIXTURE_H */
