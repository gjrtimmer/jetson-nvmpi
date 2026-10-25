/*
 * jpeg_enc_fd_case.h - hardware cases for nvmpi_jpeg_encoder_put_frame_fd
 * (#158, #5), included by test_api.c after jpeg_fd_case.h (uses strict_mode
 * and the DECFD2_HAVE_SURF / weak NvBufSurfaceFromFd of dec_fd2_case.h).
 *
 * Needs the NVJPG encoder AND decoder (round trip through the real engine).
 * The mock encoder writes no pixels, so the mock build skips; so do modules
 * without NVJPG encode (create returns NULL, e.g. Orin Nano) and stub runs.
 *
 *   nv12:      an NV12 surface (640x360, 642x358: padded pitch) goes through
 *              VIC. The JPEG is decoded again and compared with the JPEG the
 *              CPU path (put_frame) makes of the same pixels.
 *   yuv420:    a create_jpeg_decoder_fmt(YUV420) surface goes in as it is
 *              (direct when the decoder's surface is exactly the picture).
 *   dup:       a dup'd fd is refused with NVMPI_ERR_INVAL.
 *
 * Judged by the failure mode, not by an absolute number: a wrong plane
 * offset/pitch or a lost chroma plane makes the fd result differ from the
 * CPU result by tens of dB, while the same pixels through the same encoder
 * agree. So the fd result must stay within JEF_AGREE_MSE of the CPU result and
 * must not be worse than it against the source by more than JEF_LOSS_X. The
 * mean squared errors are printed for the record (PSNR = 10*log10(65025/MSE)).
 */
#ifndef NVMPI_TEST_JPEG_ENC_FD_CASE_H
#define NVMPI_TEST_JPEG_ENC_FD_CASE_H

#include "jpeg_mock_data.h"

/* PSNR limits as mean squared error (no libm needed): 30 dB is MSE 65, 3 dB
 * is a factor 2. */
#define JEF_AGREE_MSE 65.0    /* fd JPEG vs CPU JPEG, decoded              */
#define JEF_LOSS_X 2.0        /* fd vs source may be this much worse than  */
#define JEF_LOSS_MIN 1.0      /* the CPU path (MSE), at least this          */
#define JEF_FAIL(what) do { fprintf(stderr, "jpeg_enc_fd FAIL %s:%d: %s\n", __func__, __LINE__, (what)); return 1; } while (0)

#if defined(NVMPI_MOCK_JPEG) || !defined(DECFD2_HAVE_SURF)
static int test_jpeg_enc_fd_nv12(void) { return 77; }
static int test_jpeg_enc_fd_yuv420(void) { return 77; }
static int test_jpeg_enc_fd_dup(void) { return 77; }
#else

/* A planar 4:2:0 picture; planes malloc'd, strides in st[]. */
struct jef_img {
    unsigned char *p[3];
    unsigned st[3], w, h;
};

static void jef_free(struct jef_img *im)
{
    for (int i = 0; i < 3; i++) { free(im->p[i]); im->p[i] = NULL; }
}

static int jef_alloc(struct jef_img *im, unsigned w, unsigned h)
{
    memset(im, 0, sizeof(*im));
    im->w = w; im->h = h;
    im->st[0] = (w + 63) & ~63u;
    im->st[1] = im->st[2] = im->st[0] / 2;
    for (int i = 0; i < 3; i++) {
        im->p[i] = calloc((size_t)im->st[i], i ? (h + 1) / 2 : h);
        if (!im->p[i]) { jef_free(im); return -1; }
    }
    return 0;
}

/* Smooth gradients plus a little texture: compresses like a photo, and any
 * shift or swap of planes is a large error. */
static void jef_pattern(struct jef_img *im)
{
    for (unsigned y = 0; y < im->h; y++)
        for (unsigned x = 0; x < im->w; x++)
            im->p[0][(size_t)y * im->st[0] + x] =
                (unsigned char)(32 + x * 150 / im->w + y * 50 / im->h + ((x ^ y) & 7));
    for (unsigned y = 0; y < (im->h + 1) / 2; y++)
        for (unsigned x = 0; x < (im->w + 1) / 2; x++) {
            im->p[1][(size_t)y * im->st[1] + x] = (unsigned char)(100 + x * 60 / im->w * 2);
            im->p[2][(size_t)y * im->st[2] + x] = (unsigned char)(160 - y * 60 / im->h * 2);
        }
}

/* Mean squared error of plane pl of a vs b (same size). */
static double jef_mse(const struct jef_img *a, const struct jef_img *b, int pl)
{
    unsigned w = pl ? (a->w + 1) / 2 : a->w, h = pl ? (a->h + 1) / 2 : a->h;
    double se = 0;

    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            double d = (double)a->p[pl][(size_t)y * a->st[pl] + x] -
                       (double)b->p[pl][(size_t)y * b->st[pl] + x];
            se += d * d;
        }
    return se / ((double)w * h);
}

/* Fetch the staged JPEG of ctx into a fresh malloc'd buffer. */
static int jef_packet(nvmpictx *c, unsigned char **jpg, unsigned long *n, unsigned w, unsigned h)
{
    nvPacket pk;

    *jpg = malloc((size_t)w * h * 2 + 65536);
    if (!*jpg) return -1;
    memset(&pk, 0, sizeof(pk));
    pk.payload = *jpg;
    if (nvmpi_jpeg_encoder_get_packet(c, &pk) != 0 || pk.payload_size < 4) return -1;
    *n = pk.payload_size;
    return 0;
}

/* The CPU path: put_frame of the planar picture. */
static int jef_enc_cpu(nvmpictx *c, const struct jef_img *im, unsigned char **jpg, unsigned long *n)
{
    nvFrame f;

    memset(&f, 0, sizeof(f));
    for (int i = 0; i < 3; i++) {
        f.payload[i] = im->p[i];
        f.linesize[i] = im->st[i];
        f.payload_size[i] = (size_t)im->st[i] * (i ? (im->h + 1) / 2 : im->h);
    }
    f.type = NV_PIX_YUV420; f.width = im->w; f.height = im->h;
    if (nvmpi_jpeg_encoder_put_frame(c, &f) != 0) return -1;
    return jef_packet(c, jpg, n, im->w, im->h);
}

/* Decode a JPEG with the copy-out decoder into im (allocated here). */
static int jef_decode(const unsigned char *jpg, unsigned long n, struct jef_img *im, unsigned w, unsigned h)
{
    nvmpictx *d = nvmpi_create_jpeg_decoder();
    nvPacket pk;
    nvFrame f;
    int ok = 0;

    if (!d || jef_alloc(im, w, h) != 0) { if (d) nvmpi_jpeg_decoder_close(d); return -1; }
    memset(&pk, 0, sizeof(pk));
    pk.payload = (unsigned char *)jpg; pk.payload_size = n;
    memset(&f, 0, sizeof(f));
    for (int i = 0; i < 3; i++) {
        f.payload[i] = im->p[i];
        f.linesize[i] = im->st[i];
        f.payload_size[i] = (size_t)im->st[i] * (i ? (h + 1) / 2 : h);
    }
    ok = nvmpi_jpeg_decoder_put_packet(d, &pk) == 0 &&
         nvmpi_jpeg_decoder_get_frame(d, &f, true) == 0 && f.width == w && f.height == h;
    nvmpi_jpeg_decoder_close(d);
    if (!ok) jef_free(im);
    return ok ? 0 : -1;
}

/* Judge the fd JPEG against the CPU JPEG of the same source. */
static int jef_judge(const char *tag, const struct jef_img *src, const unsigned char *ja, unsigned long na, const unsigned char *jb, unsigned long nb)
{
    struct jef_img da, db;
    int bad = 0;

    if (jef_decode(ja, na, &da, src->w, src->h) != 0) JEF_FAIL("decode of the CPU-path JPEG");
    if (jef_decode(jb, nb, &db, src->w, src->h) != 0) { jef_free(&da); JEF_FAIL("decode of the fd-path JPEG"); }
    for (int pl = 0; pl < 3; pl++) {
        double ba = jef_mse(&db, &da, pl), bs = jef_mse(&db, src, pl), as = jef_mse(&da, src, pl);

        fprintf(stderr, "    %s plane %d MSE: fd-vs-cpu %.2f, fd-vs-src %.2f, cpu-vs-src %.2f\n", tag, pl, ba, bs, as);
        {
            unsigned pw = pl ? (src->w + 1) / 2 : src->w, ph = pl ? (src->h + 1) / 2 : src->h;
            fprintf(stderr, "      dbg plane %d row %u samples x=0,w/4,w/2,3w/4,w-1 (src/fd/cpu):", pl, ph / 2);
            for (unsigned k = 0; k < 5; k++) {
                size_t o = (size_t)(ph / 2) * src->st[pl] + (k == 4 ? pw - 1 : k * pw / 4);
                fprintf(stderr, " %u/%u/%u", src->p[pl][o], db.p[pl][o], da.p[pl][o]);
            }
            fprintf(stderr, "\n");
            if (pl == 1) {
                fprintf(stderr, "      dbg plane 1 column-band MSE fd-vs-src (8 bands):");
                for (unsigned b = 0; b < 8; b++) {
                    double se = 0; unsigned n = 0;
                    for (unsigned y = 0; y < ph; y++)
                        for (unsigned x = b * pw / 8; x < (b + 1) * pw / 8; x++, n++) {
                            double d = (double)db.p[1][(size_t)y * src->st[1] + x] - src->p[1][(size_t)y * src->st[1] + x];
                            se += d * d;
                        }
                    fprintf(stderr, " %.1f", se / n);
                }
                fprintf(stderr, "\n      dbg plane 1 row-band MSE fd-vs-src (8 bands):");
                for (unsigned b = 0; b < 8; b++) {
                    double se = 0; unsigned n = 0;
                    for (unsigned y = b * ph / 8; y < (b + 1) * ph / 8; y++)
                        for (unsigned x = 0; x < pw; x++, n++) {
                            double d = (double)db.p[1][(size_t)y * src->st[1] + x] - src->p[1][(size_t)y * src->st[1] + x];
                            se += d * d;
                        }
                    fprintf(stderr, " %.1f", se / n);
                }
                fprintf(stderr, "\n      dbg plane 1 right part (src/fd):");
                for (unsigned x = pw > 90 ? pw - 90 : 0; x < pw; x += 10) {
                    size_t o = (size_t)(ph / 2) * src->st[pl] + x;
                    fprintf(stderr, " %u/%u", src->p[pl][o], db.p[pl][o]);
                }
                fprintf(stderr, "\n");
            }
        }
        if (ba > JEF_AGREE_MSE || bs > as * JEF_LOSS_X + JEF_LOSS_MIN) bad = 1;
    }
    jef_free(&da); jef_free(&db);
    if (bad) JEF_FAIL("fd result differs from the CPU result (wrong plane address?)");
    return 0;
}

/* NvBufSurface colorFormat of fd (diagnostic print), -1 when unknown. */
static int jef_cf(int fd)
{
    NvBufSurface *s = NULL;

    if (!NvBufSurfaceFromFd || NvBufSurfaceFromFd(fd, (void **)&s) != 0 || !s)
        return -1;
    return (int)s->surfaceList[0].colorFormat;
}

/* Read the w x h picture of a 3-plane YUV420 surface back into im. */
static int jef_read_yuv420(int fd, struct jef_img *im, unsigned w, unsigned h)
{
    NvBufSurface *s = NULL;
    NvBufSurfaceParams *sp;

    if (jef_alloc(im, w, h) != 0) return -1;
    if (!NvBufSurfaceFromFd || NvBufSurfaceFromFd(fd, (void **)&s) != 0 || !s ||
        NvBufSurfaceMap(s, 0, -1, NVBUF_MAP_READ) != 0) { jef_free(im); return -1; }
    NvBufSurfaceSyncForCpu(s, 0, -1);
    sp = &s->surfaceList[0];
    for (int pl = 0; pl < 3; pl++)
        for (unsigned y = 0; y < (pl ? (h + 1) / 2 : h); y++)
            memcpy(im->p[pl] + (size_t)y * im->st[pl],
                   (unsigned char *)sp->mappedAddr.addr[pl] + (size_t)y * sp->planeParams.pitch[pl],
                   pl ? (w + 1) / 2 : w);
    NvBufSurfaceUnMap(s, 0, -1);
    return 0;
}

/* NV12 surface of the picture: Y rows and interleaved CbCr rows by pitch. */
static int jef_fill_nv12(int fd, const struct jef_img *im)
{
    NvBufSurface *s = NULL;
    NvBufSurfaceParams *sp;

    if (!NvBufSurfaceFromFd || NvBufSurfaceFromFd(fd, (void **)&s) != 0 || !s ||
        NvBufSurfaceMap(s, 0, -1, NVBUF_MAP_READ_WRITE) != 0)
        return -1;
    sp = &s->surfaceList[0];
    for (unsigned y = 0; y < im->h; y++)
        memcpy((unsigned char *)sp->mappedAddr.addr[0] + (size_t)y * sp->planeParams.pitch[0],
               im->p[0] + (size_t)y * im->st[0], im->w);
    for (unsigned y = 0; y < (im->h + 1) / 2; y++)
        for (unsigned x = 0; x < (im->w + 1) / 2; x++) {
            unsigned char *q = (unsigned char *)sp->mappedAddr.addr[1] + (size_t)y * sp->planeParams.pitch[1] + 2 * x;
            q[0] = im->p[1][(size_t)y * im->st[1] + x];
            q[1] = im->p[2][(size_t)y * im->st[2] + x];
        }
    NvBufSurfaceSyncForDevice(s, 0, -1);
    NvBufSurfaceUnMap(s, 0, -1);
    return 0;
}

static nvmpictx *jef_create(void)
{
    /* Stub runs have no NVJPG engine; a module without NVJPG encode makes
     * the create fail (Orin Nano): both skip. */
    if (!strict_mode && access("/dev/nvhost-nvjpg", F_OK) != 0)
        return NULL;
    return nvmpi_create_jpeg_encoder(85);
}

static int jef_nv12_one(nvmpictx *c, unsigned w, unsigned h)
{
    struct jef_img src;
    nvmpi_fd_frame d;
    unsigned char *ja = NULL, *jb = NULL;
    unsigned long na = 0, nb = 0;
    int fd = -1, r = 1;

    if (jef_alloc(&src, w, h) != 0) JEF_FAIL("alloc");
    jef_pattern(&src);
    if (nvmpi_surface_alloc(w, h, &fd) != 0 || fd < 0) { jef_free(&src); JEF_FAIL("nvmpi_surface_alloc"); }
    memset(&d, 0, sizeof(d));
    d.struct_size = sizeof(d);
    if (jef_fill_nv12(fd, &src) != 0 || nvmpi_surface_get_layout(fd, &d) != 0) goto out;
    fprintf(stderr, "    nv12 %ux%u: surface %ux%u pitch %u cf %d\n", w, h, d.width, d.height, d.pitch[0], jef_cf(fd));
    d.width = w; d.height = h; d.timestamp = 3;
    if (jef_enc_cpu(c, &src, &ja, &na) != 0) goto out;
    if (nvmpi_jpeg_encoder_put_frame_fd(c, &d) != 0 || jef_packet(c, &jb, &nb, w, h) != 0) goto out;
    r = jef_judge("nv12", &src, ja, na, jb, nb);
out:
    free(ja); free(jb); jef_free(&src);
    nvmpi_surface_destroy(fd);
    if (r) fprintf(stderr, "jpeg_enc_fd: nv12 %ux%u failed\n", w, h);
    return r;
}

static int test_jpeg_enc_fd_nv12(void)
{
    nvmpictx *c = jef_create();

    if (!c) return 77;
    if (jef_nv12_one(c, 640, 360) != 0 || jef_nv12_one(c, 642, 358) != 0) return 1;
    return nvmpi_jpeg_encoder_close(c) == 0 ? 0 : 1;
}

/* A YUV420 pool surface of the fd decoder goes in as it is. The same JPEG
 * decoded by the copy-out decoder is the source of the CPU-path encode. */
static int test_jpeg_enc_fd_yuv420(void)
{
    nvmpictx *c = jef_create(), *fdd = c ? jfd_create(NV_PIX_YUV420, 2) : NULL;
    nvmpi_frame_release_cb rel = NULL;
    nvmpi_fd_frame f;
    struct jef_img src;
    unsigned char *ja = NULL, *jb = NULL;
    unsigned long na = 0, nb = 0;
    nvPacket pk;
    void *op = NULL;
    int r = 1;

    memset(&src, 0, sizeof(src));
    if (!c || !fdd) return c ? (nvmpi_jpeg_encoder_close(c), 77) : 77;
    memset(&pk, 0, sizeof(pk));
    pk.payload = (unsigned char *)jpeg_mock_64x48_jpg; pk.payload_size = jpeg_mock_64x48_jpg_len; pk.pts = 4;
    memset(&f, 0, sizeof(f));
    f.struct_size = sizeof(f);
    if (nvmpi_jpeg_decoder_put_packet(fdd, &pk) != 0 ||
        nvmpi_jpeg_decoder_get_frame_fd(fdd, &f, &rel, &op, true) != 0) JEF_FAIL("fd decode");
    fprintf(stderr, "    yuv420 %ux%u: size %llu pitch %u cf %d (direct when the surface is exactly the picture)\n",
            f.width, f.height, (unsigned long long)f.size, f.pitch[0], jef_cf(f.fd));
    if (jef_decode(jpeg_mock_64x48_jpg, jpeg_mock_64x48_jpg_len, &src, f.width, f.height) != 0) goto out;
    {
        struct jef_img sf;
        if (jef_read_yuv420(f.fd, &sf, f.width, f.height) == 0) {
            for (int pl = 0; pl < 3; pl++)
                fprintf(stderr, "    dbg surface-vs-decoded plane %d MSE %.2f\n", pl, jef_mse(&sf, &src, pl));
            jef_free(&sf);
        }
    }
    if (jef_enc_cpu(c, &src, &ja, &na) != 0) goto out;
    if (nvmpi_jpeg_encoder_put_frame_fd(c, &f) != 0 || jef_packet(c, &jb, &nb, f.width, f.height) != 0) goto out;
    r = jef_judge("yuv420", &src, ja, na, jb, nb);
out:
    jef_free(&src);
    free(ja); free(jb);
    rel(op);
    nvmpi_jpeg_decoder_close(fdd);
    nvmpi_jpeg_encoder_close(c);
    return r;
}

/* A dup'd fd is not in the NvBufSurface table: refused, nothing encoded. */
static int test_jpeg_enc_fd_dup(void)
{
    nvmpictx *c = jef_create();
    nvmpi_fd_frame d;
    int fd = -1, dupfd, r;

    if (!c) return 77;
    if (nvmpi_surface_alloc(640, 360, &fd) != 0 || fd < 0) JEF_FAIL("nvmpi_surface_alloc");
    memset(&d, 0, sizeof(d));
    d.struct_size = sizeof(d);
    if (nvmpi_surface_get_layout(fd, &d) != 0) JEF_FAIL("layout");
    d.width = 640; d.height = 360;
    dupfd = dup(fd);
    d.fd = dupfd;
    r = nvmpi_jpeg_encoder_put_frame_fd(c, &d);
    close(dupfd);
    d.fd = fd;                           /* the original is accepted */
    if (r != NVMPI_ERR_INVAL) JEF_FAIL("dup'd fd not refused");
    if (nvmpi_jpeg_encoder_put_frame_fd(c, &d) != 0) JEF_FAIL("original fd refused");
    nvmpi_surface_destroy(fd);
    return nvmpi_jpeg_encoder_close(c) == 0 ? 0 : 1;
}

#endif /* mock / DECFD2_HAVE_SURF */

#endif
