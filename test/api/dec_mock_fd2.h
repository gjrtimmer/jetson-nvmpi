/*
 * dec_mock_fd2.h - nvmpi_decoder_get_frame_fd2 cases of test_api_dec_mock.c
 * (issue #138): per-plane geometry against the surface's own planeParams
 * (offset[1] == psize[0], page aligned, NOT pitch*height), YUV420 and P010,
 * struct_size handling, equality with the v1 call, borrow lifetime across
 * close. Included once from test_api_dec_mock.c after dec_mock_fd.h (reuses
 * its fdb_* helpers and fd_snap.h).
 */
#ifndef NVMPI_TEST_DEC_MOCK_FD2_H
#define NVMPI_TEST_DEC_MOCK_FD2_H

#define FD2_W 640
#define FD2_H 360

static nvmpictx *fd2_create(nvPixFormat pf)
{
    nvDecParam p;

    memset(&p, 0, sizeof(p));
    p.codingType = NV_VIDEO_CodingH264;
    p.pixFormat = pf;
    p.frame_pool_size = 4;
    p.wait_timeout = 1000;
    return nvmpi_create_decoder(&p);
}

struct fd2b {                            /* one frame borrowed via fd2    */
    nvmpi_fd_frame fr;
    nvmpi_frame_release_cb rel;
    void *op;
};

static int fd2_take(nvmpictx *c, struct fd2b *b)
{
    memset(b, 0, sizeof(*b));
    b->fr.struct_size = sizeof(b->fr);
    return nvmpi_decoder_get_frame_fd2(c, &b->fr, &b->rel, &b->op, true);
}

static void fd2_give(struct fd2b *b)
{
    if (b->rel)
        b->rel(b->op);
    b->rel = NULL;
}

/* Feed one AU (IDR when w != 0, else a P slice) and borrow its frame. */
static int fd2_step(nvmpictx *c, struct fd2b *b, unsigned w, unsigned h,
                    unsigned long pts)
{
    uint8_t au[4096];
    size_t len = w ? fx_idr(au, w, h) : fx_pslice(au);

    if (put(c, au, len, pts) != 0)
        return -2;
    return fd2_take(c, b);
}

/* The returned layout equals the surface behind fd, plane by plane. */
static int fd2_check_surface(const nvmpi_fd_frame *f, unsigned planes)
{
    NvBufSurface *s = NULL;
    const NvBufSurfaceParams *sp;
    unsigned i;

    if (f->fd < 0 || NvBufSurfaceFromFd(f->fd, (void **)&s) < 0 || !s) {
        fprintf(stderr, "fd2: fd %d is not a live surface\n", f->fd);
        return 1;
    }
    sp = &s->surfaceList[0];
    if (sp->planeParams.num_planes != planes || f->num_planes != planes ||
        f->size != sp->dataSize) {
        fprintf(stderr, "fd2: planes %u/%u size %llu/%llu\n", f->num_planes,
                sp->planeParams.num_planes, (unsigned long long)f->size,
                (unsigned long long)sp->dataSize);
        return 2;
    }
    for (i = 0; i < planes; i++)
        if (f->pitch[i] != sp->planeParams.pitch[i] ||
            f->offset[i] != sp->planeParams.offset[i]) {
            fprintf(stderr, "fd2: plane %u pitch %u/%u offset %u/%u\n", i,
                    f->pitch[i], sp->planeParams.pitch[i], f->offset[i],
                    sp->planeParams.offset[i]);
            return 3;
        }
    /* the point of fd2: plane 1 starts at the page-aligned plane 0 size,
     * not at pitch*height of plane 0 */
    if (f->offset[1] != sp->planeParams.psize[0] ||
        f->offset[1] == f->pitch[0] * FD2_H) {
        fprintf(stderr, "fd2: offset[1] %u psize[0] %u pitch*h %u\n",
                f->offset[1], sp->planeParams.psize[0], f->pitch[0] * FD2_H);
        return 4;
    }
    if (planes == 3 &&
        f->offset[2] != sp->planeParams.psize[0] + sp->planeParams.psize[1])
        return 5;
    return 0;
}

/* One format: geometry of the first frame and of a recycled one. */
static int fd2_geom_one(nvPixFormat pf, unsigned planes, unsigned bpp,
                        int optional)
{
    struct fd2b b;
    nvmpictx *c = fd2_create(pf);
    unsigned cw = pf == NV_PIX_YUV420 ? FD2_W / 2 : FD2_W;
    int i;

    if (!c && optional) {                /* format not decodable by the mock */
        printf("    format %d: create refused, not covered\n", (int)pf);
        return 0;
    }
    REQUIRE(c);
    for (i = 0; i < 6; i++) {            /* > pool: buffers recycle        */
        int r = fd2_step(c, &b, i == 0 ? FD2_W : 0, FD2_H, 7 + i);

        if (r != 0 && optional && i == 0) {
            nvmpi_decoder_close(c);      /* mock models 8-bit 4:2:0 only   */
            printf("    format %d: no frame from the mock, not covered\n", (int)pf);
            return 0;
        }
        REQUIRE(r == 0);
        REQUIRE(b.fr.struct_size == sizeof(b.fr));
        REQUIRE(b.fr.width == FD2_W && b.fr.height == FD2_H);
        REQUIRE(b.fr.format == pf && b.fr.flags == 0);
        REQUIRE(b.fr.timestamp == 7 + i);
        REQUIRE(b.fr.offset[0] == 0);
        REQUIRE(b.fr.pitch[0] >= FD2_W * bpp && b.fr.pitch[1] >= cw * bpp);
        REQUIRE(fd2_check_surface(&b.fr, planes) == 0);
        fd2_give(&b);
    }
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

static int fd2_geometry(void)
{
    struct fd_snap base;

    fds_take(&base);
    REQUIRE(fd2_geom_one(NV_PIX_NV12, 2, 1, 0) == 0);
    REQUIRE(fd2_geom_one(NV_PIX_YUV420, 3, 1, 0) == 0);
    REQUIRE(fd2_geom_one(NV_PIX_P010, 2, 2, 1) == 0);
    REQUIRE(fds_check(&base, 0, "released") == 0);
    return 0;
}

/* struct_size handling: V1 size fills exactly V1 bytes, too small or NULL is
 * NVMPI_ERR_INVAL, and a refused call consumes no frame. */
static int fd2_args(void)
{
    union { nvmpi_fd_frame f; unsigned char raw[sizeof(nvmpi_fd_frame) + 8]; } u;
    nvmpi_fd_frame fr;
    nvmpi_frame_release_cb rel = NULL;
    void *op = NULL;
    unsigned i;
    nvmpictx *c = fd2_create(NV_PIX_NV12);

    REQUIRE(c);
    memset(&fr, 0, sizeof(fr));
    fr.struct_size = sizeof(fr);
    REQUIRE(nvmpi_decoder_get_frame_fd2(NULL, &fr, &rel, &op, false) ==
            NVMPI_ERR_INVAL);
    REQUIRE(nvmpi_decoder_get_frame_fd2(c, NULL, &rel, &op, false) ==
            NVMPI_ERR_INVAL);
    REQUIRE(nvmpi_decoder_get_frame_fd2(c, &fr, NULL, &op, false) ==
            NVMPI_ERR_INVAL);
    REQUIRE(nvmpi_decoder_get_frame_fd2(c, &fr, &rel, NULL, false) ==
            NVMPI_ERR_INVAL);
    fr.struct_size = 0;
    REQUIRE(nvmpi_decoder_get_frame_fd2(c, &fr, &rel, &op, false) ==
            NVMPI_ERR_INVAL);
    fr.struct_size = NVMPI_FD_FRAME_V1_SIZE - 1;
    REQUIRE(nvmpi_decoder_get_frame_fd2(c, &fr, &rel, &op, false) ==
            NVMPI_ERR_INVAL);
    /* valid call, no frame decoded yet: -1, not INVAL */
    fr.struct_size = sizeof(fr);
    REQUIRE(nvmpi_decoder_get_frame_fd2(c, &fr, &rel, &op, false) == -1);

    /* feed a frame; the refused calls above must not have eaten anything */
    {
        uint8_t au[4096];
        size_t len = fx_idr(au, FD2_W, FD2_H);

        REQUIRE(put(c, au, len, 1) == 0);
    }
    memset(&u, 0xAB, sizeof(u));
    u.f.struct_size = NVMPI_FD_FRAME_V1_SIZE;
    REQUIRE(nvmpi_decoder_get_frame_fd2(c, &u.f, &rel, &op, true) == 0);
    REQUIRE(u.f.struct_size == NVMPI_FD_FRAME_V1_SIZE);
    REQUIRE(u.f.width == FD2_W && u.f.height == FD2_H && u.f.flags == 0);
    REQUIRE(fd2_check_surface(&u.f, 2) == 0);
    for (i = NVMPI_FD_FRAME_V1_SIZE; i < sizeof(u.raw); i++)
        REQUIRE(u.raw[i] == 0xAB);       /* nothing written past V1_SIZE   */
    rel(op);

    /* a larger struct_size (newer caller) is clamped to sizeof and written
     * back as that size */
    memset(&u, 0xAB, sizeof(u));
    u.f.struct_size = sizeof(u.raw);
    {
        uint8_t au[4096];
        size_t len = fx_pslice(au);

        REQUIRE(put(c, au, len, 2) == 0);
    }
    REQUIRE(nvmpi_decoder_get_frame_fd2(c, &u.f, &rel, &op, true) == 0);
    REQUIRE(u.f.struct_size == sizeof(nvmpi_fd_frame));
    for (i = sizeof(nvmpi_fd_frame); i < sizeof(u.raw); i++)
        REQUIRE(u.raw[i] == 0xAB);
    rel(op);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* v1 and v2 describe the same stream identically (pitch == pitch[0]) and
 * share one pool: alternating them recycles the same 4 buffers. */
static int fd2_v1_equal(void)
{
    struct fdb a;
    struct fd2b b;
    struct fd_snap base;
    nvmpictx *c;
    int i;

    fds_take(&base);
    c = fd2_create(NV_PIX_NV12);
    REQUIRE(c);
    for (i = 0; i < 6; i++) {
        REQUIRE(fdb_step(c, &a, i == 0 ? FD2_W : 0, FD2_H, 10 + i) == 0);
        REQUIRE(fd2_step(c, &b, 0, FD2_H, 20 + i) == 0);
        REQUIRE(a.w == (int)b.fr.width && a.h == (int)b.fr.height);
        REQUIRE(a.pitch == (int)b.fr.pitch[0]);
        REQUIRE(a.ts == 10 + i && b.fr.timestamp == 20 + i);
        fdb_give(&a);
        fd2_give(&b);
    }
    REQUIRE(nvmpi_decoder_close(c) == 0);
    REQUIRE(fds_check(&base, 0, "released") == 0);
    return 0;
}

/* Borrow two frames via fd2, close, check them (layout still equals the live
 * surface), release after close: no leak (ASan/LSan + descriptor count). */
static int fd2_after_close(void)
{
    struct fd2b b[2];
    struct fd_snap base;
    nvmpictx *c;
    int i;

    fds_take(&base);
    c = fd2_create(NV_PIX_NV12);
    REQUIRE(c);
    for (i = 0; i < 2; i++)
        REQUIRE(fd2_step(c, &b[i], i == 0 ? FD2_W : 0, FD2_H, 1 + i) == 0);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    REQUIRE(fds_check(&base, 2, "after close") == 0);
    for (i = 0; i < 2; i++)
        REQUIRE(fd2_check_surface(&b[i].fr, 2) == 0);
    fd2_give(&b[1]);
    fd2_give(&b[0]);
    REQUIRE(fds_check(&base, 0, "released") == 0);
    return 0;
}

#endif /* NVMPI_TEST_DEC_MOCK_FD2_H */
