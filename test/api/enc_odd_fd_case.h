/*
 * enc_odd_fd_case.h - hardware case for odd frame sizes through
 * put_frame_fd (#156), the DMA-BUF twin of enc_odd_case.h (#154), included
 * at its end. The same 641x357 picture and sentinel (enc_odd_fixture.h) goes
 * in as an NV12 surface from nvmpi_surface_alloc_for_enc; odd_encode /
 * odd_decode / oc_compare then judge the decoded chroma exactly like the
 * memory case, by the failure mode (edge_lost, not a PSNR threshold):
 *
 *   odd_chroma probe: fmt=NV12-fd psnr_u=.. lastcol=ok|bad edge_lost=.. ...
 *
 * First the surface geometry the allocator really produced (planeParams
 * width/height/pitch per plane) is printed: if the NvBufSurface holds only
 * floored chroma (320x178) the surface has no last-column/row sentinel, the
 * library replicates it (the same samples the hardware would substitute), and
 * the edge cannot be judged: the case then only proves encode + decode work
 * and the size is right. NV12 only: YUV420M + DMABUF crashes the driver.
 * Strict mode fails on "bad". The mock cannot run it (no decoded pixels);
 * enc_mock_oddfd.h covers the same input there.
 */
#ifndef NVMPI_TEST_ENC_ODD_FD_CASE_H
#define NVMPI_TEST_ENC_ODD_FD_CASE_H

#ifndef NVMPI_MOCK_JPEG                   /* hardware only, see above */

#if defined(__has_include)
#if __has_include(<nvbufsurface.h>)
#include <nvbufsurface.h>
#define ODDFD_HAVE_SURF 1
#endif
#endif

/* A build that must run this case (the deb hw job) defines NVMPI_REQUIRE_SURF:
 * a missing header is then a build error, never a case that cannot run. */
#if defined(NVMPI_REQUIRE_SURF) && !defined(ODDFD_HAVE_SURF)
#error "NVMPI_REQUIRE_SURF: nvbufsurface.h not found, add -I<jetson_multimedia_api>/include"
#endif

#ifdef ODDFD_HAVE_SURF

/* Allocate the 641x357 NV12 surface, print its real plane geometry and fill
 * it with the oc_* pattern over what each plane holds. 0 ok, -1 failure. */
static int odd_fd_surface(int *fd)
{
    NvBufSurface *s = NULL;
    NvBufSurfaceParams *sp;
    unsigned x, y, cw, ch;

    *fd = -1;
    if (nvmpi_surface_alloc_for_enc(OC_W, OC_H, fd) != 0 || *fd < 0 ||
        NvBufSurfaceFromFd(*fd, (void **)&s) != 0 || !s)
        return -1;
    sp = &s->surfaceList[0];
    printf("odd_chroma surface: %ux%u planes=%u fmt=%d", sp->width, sp->height,
           sp->planeParams.num_planes, (int)sp->colorFormat);
    for (unsigned p = 0; p < sp->planeParams.num_planes; p++)
        printf(" p%u=%ux%u/pitch%u/bpp%u", p, sp->planeParams.width[p], sp->planeParams.height[p],
               sp->planeParams.pitch[p], sp->planeParams.bytesPerPix[p]);
    printf("\n");
    fflush(stdout);
    if (sp->planeParams.num_planes < 2 || sp->planeParams.width[0] < OC_W ||
        sp->planeParams.height[0] < OC_H || sp->planeParams.bytesPerPix[1] != 2 ||
        NvBufSurfaceMap(s, 0, -1, NVBUF_MAP_READ_WRITE) != 0)
        return -1;
    for (y = 0; y < OC_H; y++)
        for (x = 0; x < OC_W; x++)
            ((unsigned char *)sp->mappedAddr.addr[0])[(size_t)y * sp->planeParams.pitch[0] + x] = oc_luma(x, y);
    cw = sp->planeParams.width[1];
    ch = sp->planeParams.height[1];
    for (y = 0; y < ch; y++)
        for (x = 0; x < cw; x++) {
            unsigned char *p = (unsigned char *)sp->mappedAddr.addr[1] +
                               (size_t)y * sp->planeParams.pitch[1] + 2 * x;

            p[0] = oc_u(x, y);
            p[1] = oc_v(x, y);
        }
    NvBufSurfaceSyncForDevice(s, 0, -1);
    NvBufSurfaceUnMap(s, 0, -1);
    odd_fd_full_chroma = cw >= OC_CW && ch >= OC_CH;
    return 0;
}

#else  /* no nvbufsurface.h (legacy nvbuf_utils): no surface to fill, SKIP */
static int odd_fd_surface(int *fd)
{
    *fd = -1;
    return 77;
}
#endif

static int odd_fd_feed(nvmpictx *enc, int fd, long pts)
{
    return nvmpi_encoder_put_frame_fd(enc, fd, OC_W, OC_H, OC_W, pts);
}

#endif /* !NVMPI_MOCK_JPEG */

static int test_encoder_odd_chroma_fd(void)
{
#ifdef NVMPI_MOCK_JPEG
    return 77;                    /* mock decoder: no pixels, see header */
#else
    setenv("NVMPI_ENC_TRACE", "1", 1);
    return odd_chroma_run(NV_PIX_NV12, "NV12-fd", 1);
#endif
}

#endif /* NVMPI_TEST_ENC_ODD_FD_CASE_H */
