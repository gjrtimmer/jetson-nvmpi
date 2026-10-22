/*
 * jpeg_mock_enc.h - JPEG encoder put_frame plane size cases of
 * test_api_jpeg_mock.c (issue #151). Needs jpeg_mock_util.h first.
 *
 * Each plane is its own exact-size malloc (no slack), so under ASan any read
 * past the plane is a heap-buffer-overflow. Contract (include/nvmpi.h): a
 * declared payload_size[p] smaller than what the copy reads, or a linesize
 * below the row bytes, is refused with NVMPI_ERR_INVAL before anything is
 * read or encoded; payload_size[p] == 0 = not declared (legacy), accepted.
 *
 * The mock NvJPEGEncoder (mock/mock_jpegenc.cpp) encodes no pixels: an
 * accepted frame yields a 4-byte SOI/EOI stand-in packet.
 */
#ifndef NVMPI_TEST_JPEG_MOCK_ENC_H
#define NVMPI_TEST_JPEG_MOCK_ENC_H

#define JW 64
#define JH 48

struct je_planes {
    unsigned char *p[3];
    size_t sz[3];
};

/* Exact-size I420 planes of a w x h picture. */
static void je_alloc(struct je_planes *a, unsigned w, unsigned h)
{
    a->sz[0] = (size_t)w * h;
    a->sz[1] = a->sz[2] = (size_t)(w / 2) * (h / 2);
    for (int i = 0; i < 3; i++) {
        a->p[i] = malloc(a->sz[i]);
        REQUIRE(a->p[i]);
        memset(a->p[i], 0x80, a->sz[i]);
    }
}

static void je_free(struct je_planes *a)
{
    for (int i = 0; i < 3; i++)
        free(a->p[i]);
}

/* JW x JH frame over the planes; declare=0 leaves payload_size at 0. */
static void je_frame(nvFrame *f, const struct je_planes *a, unsigned stride,
                     int declare)
{
    memset(f, 0, sizeof(*f));
    for (int i = 0; i < 3; i++) {
        f->payload[i] = a->p[i];
        f->payload_size[i] = declare ? a->sz[i] : 0;
    }
    f->linesize[0] = stride;
    f->linesize[1] = f->linesize[2] = stride / 2;
    f->type = NV_PIX_YUV420;
    f->width = JW;
    f->height = JH;
}

/* put_frame must be accepted and yield the stand-in packet (SOI ... EOI). */
static int je_accepted(nvmpictx *c, nvFrame *f)
{
    unsigned char buf[64];
    nvPacket p;

    if (nvmpi_jpeg_encoder_put_frame(c, f) != 0)
        return 0;
    memset(&p, 0, sizeof(p));
    p.payload = buf;
    if (nvmpi_jpeg_encoder_get_packet(c, &p) != 0 || p.payload_size != 4)
        return 0;
    return buf[0] == 0xff && buf[1] == 0xd8 && buf[2] == 0xff && buf[3] == 0xd9;
}

/* Smaller planes with payload_size declared (whole frame, then one plane at a
 * time) are refused, repeatedly; a correct-size frame still encodes. */
static int jpeg_enc_size_refused(int a_, int b_, int c_)
{
    static const unsigned dim[][2] = { { JW, JH / 2 }, { JW / 2, JH },
                                       { JW / 2, JH / 2 } };
    nvmpictx *c = nvmpi_create_jpeg_encoder(85);
    struct je_planes small, full;
    nvFrame f;

    (void)a_; (void)b_; (void)c_;
    REQUIRE(c);
    for (size_t k = 0; k < sizeof(dim) / sizeof(dim[0]); k++) {
        je_alloc(&small, dim[k][0], dim[k][1]);
        je_frame(&f, &small, dim[k][0], 1);
        for (int i = 0; i < 3; i++)
            REQUIRE(nvmpi_jpeg_encoder_put_frame(c, &f) == NVMPI_ERR_INVAL);
        je_free(&small);
    }
    je_alloc(&full, JW, JH);
    for (int pl = 0; pl < 3; pl++) {         /* one plane one byte short  */
        je_frame(&f, &full, JW, 1);
        f.payload_size[pl] -= 1;
        REQUIRE(nvmpi_jpeg_encoder_put_frame(c, &f) == NVMPI_ERR_INVAL);
    }
    je_frame(&f, &full, JW, 1);              /* exact: encodes            */
    REQUIRE(je_accepted(c, &f));
    je_free(&full);
    return nvmpi_jpeg_encoder_close(c) == 0 ? 0 : 1;
}

/* payload_size == 0 (not declared) is unchanged: accepted, also a
 * per-plane mix (luma declared exact, chroma 0). */
static int jpeg_enc_size_legacy(int a_, int b_, int c_)
{
    nvmpictx *c = nvmpi_create_jpeg_encoder(85);
    struct je_planes full;
    nvFrame f;

    (void)a_; (void)b_; (void)c_;
    REQUIRE(c);
    je_alloc(&full, JW, JH);
    for (int i = 0; i < 4; i++) {
        je_frame(&f, &full, JW, 0);
        if (i & 1)
            f.payload_size[0] = full.sz[0];
        REQUIRE(je_accepted(c, &f));
    }
    je_free(&full);
    return nvmpi_jpeg_encoder_close(c) == 0 ? 0 : 1;
}

/* linesize below the row bytes is refused with or without payload_size
 * (the planes are large enough, only the stride is wrong). */
static int jpeg_enc_linesize_short(int a_, int b_, int c_)
{
    nvmpictx *c = nvmpi_create_jpeg_encoder(85);
    struct je_planes full;
    nvFrame f;

    (void)a_; (void)b_; (void)c_;
    REQUIRE(c);
    je_alloc(&full, JW, JH);
    for (int declare = 0; declare < 2; declare++)
        for (int pl = 0; pl < 3; pl++) {
            je_frame(&f, &full, JW, declare);
            f.linesize[pl] = (pl ? JW / 2 : JW) - 1;
            REQUIRE(nvmpi_jpeg_encoder_put_frame(c, &f) == NVMPI_ERR_INVAL);
        }
    je_free(&full);
    return nvmpi_jpeg_encoder_close(c) == 0 ? 0 : 1;
}

#endif /* NVMPI_TEST_JPEG_MOCK_ENC_H */
