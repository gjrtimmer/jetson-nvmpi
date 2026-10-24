/*
 * surf_layout_case.h - cases for nvmpi_surface_get_layout (#157), included
 * by test_api.c after dec_fd2_case.h (reuses DECFD2_HAVE_SURF and the decfd_*
 * helpers). Runs on the mock (api_mock) and on the Orin (test:api:hw).
 *
 *   alloc:   640x360, 642x358 and 641x357 from nvmpi_surface_alloc and
 *            _for_enc; the layout must satisfy the bounds a consumer relies
 *            on (pitch[0] >= w, offset[1] >= pitch[0]*h, size covers the UV
 *            plane). Mock only: 642 wide gives pitch 768 and the numbers
 *            equal NvBufSurfaceFromFd's planeParams.
 *   invalid: dup'd, -1, closed and non-surface fds, NULL frame and the
 *            struct_size handshake edges; nothing is written on error.
 *   decoder: the fd of a get_frame_fd2 frame reports the same layout fd2 did.
 *
 * Outside strict mode a missing device or allocation is a skip, with
 * NVMPI_TEST_STRICT=1 every shortfall is a failure.
 */
#ifndef NVMPI_TEST_SURF_LAYOUT_CASE_H
#define NVMPI_TEST_SURF_LAYOUT_CASE_H

#include <sys/syscall.h>

#define SL_FAIL(what) do { fprintf(stderr, "surf_layout FAIL %s:%d: %s\n", __func__, __LINE__, (what)); return 1; } while (0)

static const unsigned sl_w[3] = { 640, 642, 641 };
static const unsigned sl_h[3] = { 360, 358, 357 };

/* 0 = the layout of an NV12 w x h surface satisfies the consumer bounds. */
static int sl_check_bounds(const nvmpi_fd_frame *fr, int fd, unsigned w, unsigned h)
{
    uint64_t uv_end;

    if (fr->fd != fd || fr->format != NV_PIX_NV12 || fr->num_planes != 2)
        return 1;
    if (fr->timestamp != 0 || fr->flags != 0 || fr->struct_size != sizeof(*fr))
        return 2;
    if (fr->width < w || fr->height < h)
        return 3;
    if (fr->pitch[0] < w || fr->offset[0] != 0 ||
        fr->offset[1] < (uint64_t)fr->pitch[0] * h)
        return 4;
    uv_end = (uint64_t)fr->offset[1] + (uint64_t)fr->pitch[1] * ((h + 1) / 2);
    if (fr->size < uv_end)
        return 5;
    return 0;
}

#ifdef NVMPI_MOCK_JPEG
/* Mock only: pitch is rounded to 256 and the numbers are the allocator's. */
static int sl_check_mock(const nvmpi_fd_frame *fr, int fd, unsigned w, unsigned h)
{
    if (fr->width != w || fr->height != h)
        return 1;
    if (fr->pitch[0] != ((w + 255) & ~255u))      /* 642 -> 768 */
        return 2;
#ifdef DECFD2_HAVE_SURF
    if (NvBufSurfaceFromFd) {                     /* weak, see jpeg_fd_case.h */
        NvBufSurface *s = NULL;
        const NvBufSurfaceParams *sp;

        if (NvBufSurfaceFromFd(fd, (void **)&s) != 0 || !s)
            return 3;
        sp = &s->surfaceList[0];
        if (sp->planeParams.num_planes != fr->num_planes ||
            sp->planeParams.width[0] != fr->width ||
            sp->planeParams.height[0] != fr->height ||
            sp->planeParams.pitch[0] != fr->pitch[0] ||
            sp->planeParams.pitch[1] != fr->pitch[1] ||
            sp->planeParams.offset[1] != fr->offset[1] ||
            (sp->dataSize && sp->dataSize != fr->size))
            return 4;
    }
#endif
    return 0;
}
#endif

static int test_surface_layout_alloc(void)
{
    unsigned i;
    int kind;

    for (kind = 0; kind < 2; kind++) {
        for (i = 0; i < 3; i++) {
            int fd = -1, rc;
            nvmpi_fd_frame fr;

            rc = kind ? nvmpi_surface_alloc_for_enc(sl_w[i], sl_h[i], &fd)
                      : nvmpi_surface_alloc(sl_w[i], sl_h[i], &fd);
            if (rc != 0 || fd < 0)
                return strict_mode ? 1 : 77;
            memset(&fr, 0, sizeof(fr));
            fr.struct_size = sizeof(fr);
            rc = nvmpi_surface_get_layout(fd, &fr);
            if (rc != 0)
                SL_FAIL("get_layout failed on a fresh surface");
            fprintf(stderr, "surf_layout %s %ux%u: pitch %u/%u offset1 %u size %llu\n",
                    kind ? "enc" : "vic", sl_w[i], sl_h[i], fr.pitch[0],
                    fr.pitch[1], fr.offset[1], (unsigned long long)fr.size);
            if (sl_check_bounds(&fr, fd, sl_w[i], sl_h[i]) != 0)
                SL_FAIL("layout bounds");
#ifdef NVMPI_MOCK_JPEG
            if (sl_check_mock(&fr, fd, sl_w[i], sl_h[i]) != 0)
                SL_FAIL("mock layout (pitch rounding / allocator equality)");
#endif
            /* the getter takes no reference: destroy still works once */
            if (nvmpi_surface_destroy(fd) != 0)
                SL_FAIL("destroy after get_layout");
        }
    }
    return 0;
}

/* 0 = get_layout returned NVMPI_ERR_INVAL and left *fr byte for byte alone. */
static int sl_expect_inval(int fd, nvmpi_fd_frame *fr)
{
    nvmpi_fd_frame before = *fr;

    if (nvmpi_surface_get_layout(fd, fr) != NVMPI_ERR_INVAL)
        return 1;
    return memcmp(&before, fr, sizeof(before)) ? 2 : 0;
}

static int test_surface_layout_invalid(void)
{
    nvmpi_fd_frame fr;
    int fd = -1, dupfd, memfd;
    /* oversize caller struct: sizeof + 16 bytes of room */
    unsigned char big[sizeof(nvmpi_fd_frame) + 16];
    nvmpi_fd_frame *bf = (nvmpi_fd_frame *)big;

    if (nvmpi_surface_alloc(640, 360, &fd) != 0 || fd < 0)
        return strict_mode ? 1 : 77;

    memset(&fr, 0xA5, sizeof(fr));
    fr.struct_size = sizeof(fr);
    if (nvmpi_surface_get_layout(fd, NULL) != NVMPI_ERR_INVAL)
        SL_FAIL("NULL frame");
    if (sl_expect_inval(-1, &fr))
        SL_FAIL("fd -1");

    /* dup'd fd: same dmabuf, not in the allocator's table */
    dupfd = dup(fd);
    if (dupfd < 0 || sl_expect_inval(dupfd, &fr))
        SL_FAIL("dup'd fd");
    close(dupfd);

    /* a plain fd that is no surface */
    memfd = (int)syscall(SYS_memfd_create, "sl", 0);
    if (memfd < 0 || sl_expect_inval(memfd, &fr))
        SL_FAIL("memfd");
    close(memfd);

    /* struct_size handshake: below V1 refused, exactly V1 fills V1 bytes */
    fr.struct_size = NVMPI_FD_FRAME_V1_SIZE - 1;
    if (sl_expect_inval(fd, &fr))
        SL_FAIL("struct_size below V1");
    memset(&fr, 0xA5, sizeof(fr));
    fr.struct_size = NVMPI_FD_FRAME_V1_SIZE;
    if (nvmpi_surface_get_layout(fd, &fr) != 0)
        SL_FAIL("struct_size == V1");
    if (fr.struct_size != NVMPI_FD_FRAME_V1_SIZE || fr.fd != fd || fr.num_planes != 2)
        SL_FAIL("V1 fill");
    for (size_t i = NVMPI_FD_FRAME_V1_SIZE; i < sizeof(fr); i++)
        if (((unsigned char *)&fr)[i] != 0xA5)
            SL_FAIL("wrote past a V1 struct_size");

    /* larger caller struct: library fills sizeof and writes sizeof back */
    memset(big, 0xA5, sizeof(big));
    bf->struct_size = sizeof(big);
    if (nvmpi_surface_get_layout(fd, bf) != 0)
        SL_FAIL("oversize struct_size");
    if (bf->struct_size != sizeof(nvmpi_fd_frame) || bf->fd != fd)
        SL_FAIL("oversize struct_size not written back as sizeof");
    for (size_t i = sizeof(nvmpi_fd_frame); i < sizeof(big); i++)
        if (big[i] != 0xA5)
            SL_FAIL("wrote past sizeof(nvmpi_fd_frame)");

    /* closed (destroyed) surface */
    if (nvmpi_surface_destroy(fd) != 0)
        SL_FAIL("destroy");
    memset(&fr, 0xA5, sizeof(fr));
    fr.struct_size = sizeof(fr);
    if (sl_expect_inval(fd, &fr))
        SL_FAIL("closed fd");
    return 0;
}

static int test_surface_layout_decoder(void)
{
    nvmpi_fd_frame fr, lay;
    nvmpi_frame_release_cb rel = NULL;
    void *op = NULL;
    nvmpictx *ctx = decfd_create();
    nvPacket pkt;
    unsigned i;
    int rc;

    if (!ctx) return strict_mode ? 1 : 77;
    memset(&pkt, 0, sizeof(pkt));
    pkt.payload = (unsigned char *)decfd_a;
    pkt.payload_size = decfd_a_len;
    pkt.pts = 1;
    if (nvmpi_decoder_put_packet(ctx, &pkt) < 0)
        SL_FAIL("put_packet");
    memset(&fr, 0, sizeof(fr));
    fr.struct_size = sizeof(fr);
    rc = nvmpi_decoder_get_frame_fd2(ctx, &fr, &rel, &op, true);
    if (rc != 0) {
        nvmpi_decoder_close(ctx);
        return strict_mode ? 1 : 77;
    }
    memset(&lay, 0, sizeof(lay));
    lay.struct_size = sizeof(lay);
    if (nvmpi_surface_get_layout(fr.fd, &lay) != 0)
        SL_FAIL("get_layout on a decoder fd");
    /* width/height differ by design (stream size vs allocated plane 0) */
    if (lay.fd != fr.fd || lay.format != fr.format || lay.num_planes != fr.num_planes ||
        lay.size != fr.size)
        SL_FAIL("header differs from fd2");
    for (i = 0; i < fr.num_planes; i++)
        if (lay.pitch[i] != fr.pitch[i] || lay.offset[i] != fr.offset[i])
            SL_FAIL("plane geometry differs from fd2");
    if (lay.width < fr.width || lay.height < fr.height)
        SL_FAIL("allocated size below the frame size");
    rel(op);
    return nvmpi_decoder_close(ctx) == 0 ? 0 : 1;
}

#endif /* NVMPI_TEST_SURF_LAYOUT_CASE_H */
