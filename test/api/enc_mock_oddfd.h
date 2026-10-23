/*
 * enc_mock_oddfd.h - odd-size put_frame_fd cases of test_api_enc_mock.c
 * (#156, the DMA-BUF twin of enc_mock_oddchroma.h / #154). Included from
 * enc_mock_cases.h after enc_mock_oddchroma.h.
 *
 * A 641x357 NV12 surface (nvmpi_surface_alloc_for_enc) goes in through
 * put_frame_fd. The mock's H.264 key frame is I_PCM, so the samples that
 * reached the OUTPUT plane are read back out of the access unit and judged
 * by what the bug produces: luma not padded to 642x358, chroma column/row
 * missing or substituted. The encoder runs at the even-rounded size, the
 * library stages the surface through an internal coded-size copy that pads.
 * The mock's NvBufSurface floors odd chroma (320x178), so the last chroma
 * column/row has to be replicated; a source that holds the full 321x179 must
 * arrive intact. An unregistered (dup'd) fd cannot be staged and is refused.
 */
#ifndef NVMPI_TEST_ENC_MOCK_ODDFD_H
#define NVMPI_TEST_ENC_MOCK_ODDFD_H

#include <nvbufsurface.h>

/* Fill the surface behind fd with the oc_* pattern at its own plane
 * geometry (luma OC_W x OC_H, chroma as many CbCr pairs/rows as the surface
 * has). Returns the chroma plane's {width,height} in *cw / *ch. */
static int ofd_fill(int fd, unsigned *cw, unsigned *ch)
{
    NvBufSurface *s = NULL;
    NvBufSurfaceParams *sp;
    unsigned x, y;

    REQUIRE(NvBufSurfaceFromFd(fd, (void **)&s) == 0 && s);
    sp = &s->surfaceList[0];
    REQUIRE(NvBufSurfaceMap(s, 0, -1, NVBUF_MAP_READ_WRITE) == 0);
    REQUIRE(sp->planeParams.width[0] == OC_W && sp->planeParams.height[0] == OC_H);
    for (y = 0; y < OC_H; y++)
        for (x = 0; x < OC_W; x++)
            ((unsigned char *)sp->mappedAddr.addr[0])[(size_t)y * sp->planeParams.pitch[0] + x] = oc_luma(x, y);
    *cw = sp->planeParams.width[1];
    *ch = sp->planeParams.height[1];
    for (y = 0; y < *ch; y++)
        for (x = 0; x < *cw; x++) {
            unsigned char *p = (unsigned char *)sp->mappedAddr.addr[1] +
                               (size_t)y * sp->planeParams.pitch[1] + 2 * x;

            p[0] = oc_u(x, y);
            p[1] = oc_v(x, y);
        }
    NvBufSurfaceSyncForDevice(s, 0, -1);
    NvBufSurfaceUnMap(s, 0, -1);
    return 0;
}

static nvmpictx *ofd_create(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 1 };

    return e_create_sz(&c, E_NPKT, OC_W, OC_H, NV_PIX_NV12);
}

/* Encode the surface; the coded planes must be the padded luma and, per what
 * the source surface holds, the full chroma or its replicated floored form. */
static int odd_chroma_fd(void)
{
    nvmpictx *ctx = ofd_create();
    static struct oc_planes out;
    unsigned cw, ch;
    nvPacket *pk = NULL;
    int fd = -1;

    REQUIRE(ctx);
    REQUIRE(nvmpi_surface_alloc_for_enc(OC_W, OC_H, &fd) == 0 && fd >= 0);
    REQUIRE(ofd_fill(fd, &cw, &ch) == 0);
    REQUIRE(nvmpi_encoder_put_frame_fd(ctx, fd, OC_W, OC_H, OC_W, 1) == 0);
    REQUIRE(nvmpi_encoder_get_packet(ctx, &pk, true) == 0);
    REQUIRE(oc_pcm_planes(pk->payload, pk->payload_size, out.y, out.u, out.v));
    nvmpi_encoder_qEmptyPacket(ctx, pk);
    REQUIRE(oc_luma_padded(&out));
    if (cw >= OC_CW && ch >= OC_CH)
        REQUIRE(oc_equal(&out, OC_CW, OC_CH));        /* full chroma arrives  */
    else {
        REQUIRE(cw == OC_CW - 1 && ch == OC_CH - 1);  /* the mock floors      */
        REQUIRE(oc_chroma_replicated(&out));          /* padded by copying    */
    }
    REQUIRE(nvmpi_surface_destroy(fd) == 0);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* A dup'd fd is not in the NvBufSurface table: no staging copy, and direct
 * pass-through would feed the encoder a 641x357 surface it reads as 642x358.
 * Refused repeatedly (no OUTPUT slot consumed), then the real fd encodes. */
static int odd_fd_unregistered(void)
{
    nvmpictx *ctx = ofd_create();
    unsigned cw, ch;
    nvPacket *pk = NULL;
    int fd = -1, dupfd, i;

    REQUIRE(ctx);
    REQUIRE(nvmpi_surface_alloc_for_enc(OC_W, OC_H, &fd) == 0 && fd >= 0);
    REQUIRE(ofd_fill(fd, &cw, &ch) == 0);
    dupfd = dup(fd);
    REQUIRE(dupfd >= 0);
    for (i = 0; i < 2 * E_NPKT; i++)
        REQUIRE(nvmpi_encoder_put_frame_fd(ctx, dupfd, OC_W, OC_H, OC_W, 1 + i) == NVMPI_ERR_INVAL);
    close(dupfd);
    REQUIRE(nvmpi_encoder_put_frame_fd(ctx, fd, OC_W, OC_H, OC_W, 99) == 0);
    REQUIRE(nvmpi_encoder_get_packet(ctx, &pk, true) == 0 && pk->pts == 99);
    nvmpi_encoder_qEmptyPacket(ctx, pk);
    REQUIRE(nvmpi_surface_destroy(fd) == 0);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

#endif /* NVMPI_TEST_ENC_MOCK_ODDFD_H */
