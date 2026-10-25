/*
 * jpeg_mock_encfd_mix.h - the put_frame / put_frame_fd interleaving and the
 * descriptor-count cases of test_api_jpeg_mock.c (#158). Needs
 * jpeg_mock_encfd.h first (helpers).
 *
 *   jpegfd_mixed       put_frame and put_frame_fd interleaved, size changes
 *   jpegfd_fd_stable   descriptors flat, caller's fd usable after each call
 */
#ifndef NVMPI_TEST_JPEG_MOCK_ENCFD_MIX_H
#define NVMPI_TEST_JPEG_MOCK_ENCFD_MIX_H

/* put_frame (CPU planes) of w x h; w and h even. */
static int ef_cpu(nvmpictx *c, unsigned w, unsigned h, long pts)
{
    struct je_planes pl;
    nvFrame f;
    int ok;

    je_alloc(&pl, w, h);
    memset(&f, 0, sizeof(f));
    for (int i = 0; i < 3; i++) {
        f.payload[i] = pl.p[i];
        f.payload_size[i] = pl.sz[i];
    }
    f.linesize[0] = w;
    f.linesize[1] = f.linesize[2] = w / 2;
    f.type = NV_PIX_YUV420;
    f.width = w;
    f.height = h;
    f.timestamp = pts;
    ok = nvmpi_jpeg_encoder_put_frame(c, &f) == 0 && ef_packet(c, (unsigned long)pts);
    je_free(&pl);
    return ok;
}

/* put_frame and put_frame_fd on one context across size changes, VIC and
 * direct. The engine must always see the size just encoded. */
static int jpegfd_mixed(int a_, int b_, int c_)
{
    nvmpictx *c = nvmpi_create_jpeg_encoder(85);
    struct efs n640, y128, n128, n321;
    nvmpi_fd_frame d;

    (void)a_; (void)b_; (void)c_;
    REQUIRE(c);
    ef_new(&n640, NV_PIX_NV12, 640, 360);
    ef_new(&y128, NV_PIX_YUV420, 128, 96);
    ef_new(&n128, NV_PIX_NV12, 128, 96);
    ef_new(&n321, NV_PIX_NV12, 321, 181);
    REQUIRE(ef_cpu(c, 64, 48, 1));
    ef_desc(&d, &n640, 640, 360, 2);
    REQUIRE(ef_ok(c, &d, 0, 640, 360));
    REQUIRE(ef_cpu(c, 128, 96, 3));                  /* realloc (put_frame) */
    REQUIRE(ef_last().width == 128 && ef_last().height == 96);
    REQUIRE(ef_last().sof_width == 128 && ef_last().sof_height == 96);
    ef_desc(&d, &y128, 128, 96, 4);
    REQUIRE(ef_ok(c, &d, 1, 128, 96));               /* direct             */
    ef_desc(&d, &n128, 128, 96, 5);
    REQUIRE(ef_ok(c, &d, 0, 128, 96));               /* VIC, same size     */
    REQUIRE(ef_cpu(c, 64, 48, 6));
    ef_desc(&d, &n321, 321, 181, 7);
    REQUIRE(ef_ok(c, &d, 0, 321, 181));              /* odd, realloc (fd)  */
    REQUIRE(ef_cpu(c, 64, 48, 8));
    ef_desc(&d, &y128, 128, 96, 9);
    REQUIRE(ef_ok(c, &d, 1, 128, 96));
    REQUIRE(ef_cpu(c, 128, 96, 10));                 /* after direct only  */
    ef_del(&n640); ef_del(&y128); ef_del(&n128); ef_del(&n321);
    return nvmpi_jpeg_encoder_close(c) == 0 ? 0 : 1;
}

/* 60 encodes over both paths: descriptor count flat, the caller's fd is
 * still its own (registered) after every call, and close frees the VIC
 * context and the input buffer. */
static int jpegfd_fd_stable(int a_, int b_, int c_)
{
    struct efs n, y;
    nvmpi_fd_frame d;
    NvBufSurface *s;
    int base, i;
    nvmpictx *c;

    (void)a_; (void)b_; (void)c_;
    ef_new(&n, NV_PIX_NV12, 640, 360);
    ef_new(&y, NV_PIX_YUV420, 640, 360);
    base = nfds();
    c = nvmpi_create_jpeg_encoder(85);
    REQUIRE(c);
    for (i = 0; i < 60; i++) {
        struct efs *e = (i & 1) ? &y : &n;

        ef_desc(&d, e, 640, 360, i);
        REQUIRE(ef_ok(c, &d, (i & 1) != 0, 640, 360));
        s = NULL;
        REQUIRE(NvBufSurfaceFromFd(e->fd, (void **)&s) == 0 && s == e->s);
    }
    REQUIRE(nvmpi_jpeg_encoder_close(c) == 0);
    REQUIRE(nfds() == base);
    ef_del(&n);
    ef_del(&y);
    return 0;
}

#endif /* NVMPI_TEST_JPEG_MOCK_ENCFD_MIX_H */
