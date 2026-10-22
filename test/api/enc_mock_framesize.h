/*
 * enc_mock_framesize.h - put_frame plane size cases of test_api_enc_mock.c
 * (issue #150). Included from enc_mock_cases.h after enc_mock_util.h.
 *
 * Each plane is its own exact-size malloc (no slack), so under ASan any read
 * past payload_size[p] is a heap-buffer-overflow. Contract (include/nvmpi.h):
 * a declared payload_size[p] smaller than what the copy reads is refused with
 * NVMPI_ERR_INVAL before anything is read or queued; 0 = not declared (legacy),
 * accepted.
 */
#ifndef NVMPI_TEST_ENC_MOCK_FRAMESIZE_H
#define NVMPI_TEST_ENC_MOCK_FRAMESIZE_H

/* Exact-size YUV420 planes for a w x h picture (h rows luma, h/2 chroma). */
struct fs_planes {
    unsigned char *p[3];
    size_t sz[3];
};

static void fs_alloc(struct fs_planes *a, unsigned w, unsigned h)
{
    a->sz[0] = (size_t)w * h;
    a->sz[1] = a->sz[2] = (size_t)(w / 2) * (h / 2);
    for (int i = 0; i < 3; i++) {
        a->p[i] = malloc(a->sz[i]);
        REQUIRE(a->p[i]);
        memset(a->p[i], 0x80, a->sz[i]);
    }
}

static void fs_free(struct fs_planes *a)
{
    for (int i = 0; i < 3; i++)
        free(a->p[i]);
}

/* Frame over the planes; declare=0 leaves payload_size at 0 (legacy). */
static void fs_frame(nvFrame *f, const struct fs_planes *a, unsigned w,
                     int declare, long pts)
{
    memset(f, 0, sizeof(*f));
    for (int i = 0; i < 3; i++) {
        f->payload[i] = a->p[i];
        f->payload_size[i] = declare ? a->sz[i] : 0;
    }
    f->linesize[0] = w;
    f->linesize[1] = f->linesize[2] = w / 2;
    f->type = NV_PIX_YUV420;
    f->width = EW;
    f->height = EH;
    f->timestamp = pts;
}

/* Frames smaller than the encoder's EW x EH, planes allocated exactly and
 * payload_size declared: refused, repeatedly (more refusals than OUTPUT
 * buffers proves no slot is consumed), then a correct-size frame encodes. */
static int put_frame_size_refused(void)
{
    static const unsigned dim[][2] = { { EW, EH / 2 }, { EW / 2, EH },
                                       { EW / 2, EH / 2 } };
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvmpictx *ctx = e_create(&c);
    struct fs_planes small, full;
    struct epkt p;
    nvFrame f;

    REQUIRE(ctx);
    for (size_t k = 0; k < sizeof(dim) / sizeof(dim[0]); k++) {
        fs_alloc(&small, dim[k][0], dim[k][1]);
        fs_frame(&f, &small, dim[k][0], 1, 1);
        for (int i = 0; i < 4; i++)
            REQUIRE(nvmpi_encoder_put_frame(ctx, &f) == NVMPI_ERR_INVAL);
        fs_free(&small);
    }
    /* only the luma plane too small, chroma exact */
    fs_alloc(&full, EW, EH);
    fs_alloc(&small, EW, EH / 2);
    fs_frame(&f, &full, EW, 1, 1);
    f.payload[0] = small.p[0];
    f.payload_size[0] = small.sz[0];
    REQUIRE(nvmpi_encoder_put_frame(ctx, &f) == NVMPI_ERR_INVAL);
    fs_free(&small);
    /* correct size, still encodes (more frames than OUTPUT buffers) */
    for (int i = 0; i < 6; i++) {
        fs_frame(&f, &full, EW, 1, 500 + i);
        REQUIRE(nvmpi_encoder_put_frame(ctx, &f) == 0);
        REQUIRE(e_get(ctx, 0, 1, &p) == 0 && p.pts == 500ul + i);
    }
    fs_free(&full);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* payload_size == 0 (not declared): unchanged, accepted and encoded. Also a
 * per-plane mix (luma declared exact, chroma 0). */
static int put_frame_size_legacy(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvmpictx *ctx = e_create(&c);
    struct fs_planes full;
    struct epkt p;
    nvFrame f;

    REQUIRE(ctx);
    fs_alloc(&full, EW, EH);
    for (int i = 0; i < 6; i++) {
        fs_frame(&f, &full, EW, 0, 700 + i);
        if (i & 1)
            f.payload_size[0] = full.sz[0];
        REQUIRE(nvmpi_encoder_put_frame(ctx, &f) == 0);
        REQUIRE(e_get(ctx, 0, 1, &p) == 0 && p.pts == 700ul + i);
    }
    fs_free(&full);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

#endif /* NVMPI_TEST_ENC_MOCK_FRAMESIZE_H */
