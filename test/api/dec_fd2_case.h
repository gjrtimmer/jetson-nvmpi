/*
 * dec_fd2_case.h - hardware case for nvmpi_decoder_get_frame_fd2 (#138),
 * included by test_api.c after dec_fd_case.h (reuses its decfd_* helpers and
 * the embedded 320x240 H.264 picture).
 *
 * One frame is borrowed through fd2 and the returned layout is compared with
 * what the NVIDIA allocator reports for the very same fd (NvBufSurfaceFromFd
 * planeParams: pitch, offset, total size); a second frame is borrowed through
 * v1 and its pitch must equal pitch[0] of v2. Where the NvUtils headers are
 * absent (stub / legacy builds) only the allocator comparison is skipped.
 * Needs a real NVDEC or the mock; outside strict mode a missing device or
 * frame is a skip, with NVMPI_TEST_STRICT=1 every shortfall is a failure.
 */
#ifndef NVMPI_TEST_DEC_FD2_CASE_H
#define NVMPI_TEST_DEC_FD2_CASE_H

#if defined(__has_include)
#if __has_include(<nvbufsurface.h>)
#include <nvbufsurface.h>
#define DECFD2_HAVE_SURF 1
#endif
#endif

/* NVMPI_REQUIRE_SURF (the deb hw job): the allocator comparison must run,
 * so a missing header is a build error, not a silently skipped check. */
#if defined(NVMPI_REQUIRE_SURF) && !defined(DECFD2_HAVE_SURF)
#error "NVMPI_REQUIRE_SURF: nvbufsurface.h not found, add -I<jetson_multimedia_api>/include"
#endif

#define DECFD2_FAIL(what) do { fprintf(stderr, "dec_fd2 FAIL %s:%d: %s\n", __func__, __LINE__, (what)); return 1; } while (0)

#ifdef DECFD2_HAVE_SURF
/* 0 = the layout equals the allocator's view of fr->fd (or no NvUtils). */
static int decfd2_vs_allocator(const nvmpi_fd_frame *fr)
{
    NvBufSurface *s = NULL;
    const NvBufSurfaceParams *sp;
    unsigned i;

    if (!NvBufSurfaceFromFd)             /* weak, see jpeg_fd_case.h */
        return 0;
    if (NvBufSurfaceFromFd(fr->fd, (void **)&s) < 0 || !s)
        return 1;
    sp = &s->surfaceList[0];
    if (sp->planeParams.num_planes != fr->num_planes)
        return 2;
    if (sp->dataSize && sp->dataSize != fr->size)
        return 3;
    for (i = 0; i < fr->num_planes; i++)
        if (sp->planeParams.pitch[i] != fr->pitch[i] ||
            sp->planeParams.offset[i] != fr->offset[i])
            return 4;
    return 0;
}
#endif

static int test_decoder_fd2_geometry(void)
{
    nvmpi_fd_frame fr;
    nvmpi_frame_release_cb rel = NULL, rel1 = NULL;
    void *op = NULL, *op1 = NULL;
    struct decfd_b b1;
    nvmpictx *ctx = decfd_create();
    nvPacket pkt;
    int rc;

    if (!ctx) return strict_mode ? 1 : 77;
    memset(&pkt, 0, sizeof(pkt));
    pkt.payload = (unsigned char *)decfd_a;
    pkt.payload_size = decfd_a_len;
    pkt.pts = 1;
    if (nvmpi_decoder_put_packet(ctx, &pkt) < 0)
        DECFD2_FAIL("put_packet");
    memset(&fr, 0, sizeof(fr));
    fr.struct_size = sizeof(fr);
    rc = nvmpi_decoder_get_frame_fd2(ctx, &fr, &rel, &op, true);
    if (rc != 0) {
        nvmpi_decoder_close(ctx);
        if (strict_mode) DECFD2_FAIL("no frame decoded");
        return 77;
    }
    if (fr.struct_size != sizeof(fr) || fr.fd < 0 ||
        fr.width != DECFD_A_W || fr.height != DECFD_A_H ||
        fr.format != NV_PIX_NV12 || fr.num_planes != 2 ||
        fr.flags != 0 || fr.timestamp != 1)
        DECFD2_FAIL("frame header fields");
    if (fr.pitch[0] < DECFD_A_W || fr.pitch[1] < DECFD_A_W ||
        fr.offset[0] != 0 || fr.offset[1] == 0 || fr.offset[1] >= fr.size)
        DECFD2_FAIL("plane geometry");
#ifdef DECFD2_HAVE_SURF
    if (decfd2_vs_allocator(&fr) != 0)
        DECFD2_FAIL("fd2 layout differs from NvBufSurfaceFromFd planeParams");
#endif

    /* v1 on the next frame: its pitch is plane 0 of the same generation */
    if (decfd_step(ctx, decfd_a, decfd_a_len, 2, &b1) != 0)
        DECFD2_FAIL("no second frame (v1)");
    rel1 = b1.rel; op1 = b1.op;
    if ((unsigned)b1.pitch != fr.pitch[0] || b1.w != (int)fr.width ||
        b1.h != (int)fr.height)
        DECFD2_FAIL("v1 pitch/size differ from v2");

    /* borrows outlive close (#137), layout still readable afterwards */
    if (nvmpi_decoder_close(ctx) != 0)
        DECFD2_FAIL("close");
#ifdef DECFD2_HAVE_SURF
    if (decfd2_vs_allocator(&fr) != 0)
        DECFD2_FAIL("fd2 frame lost at close");
#endif
    rel(op);
    rel1(op1);
    return 0;
}

#endif /* NVMPI_TEST_DEC_FD2_CASE_H */
