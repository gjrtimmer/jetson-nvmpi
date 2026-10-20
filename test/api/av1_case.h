/*
 * av1_case.h - hardware case for AV1 decode (#7), included by test_api.c.
 *
 * Creates an AV1 decoder, feeds the embedded 320x240 key-frame temporal unit
 * and waits for one frame. Needs a real NVDEC with AV1 support (Orin; Xavier
 * has none). Outside strict mode (stubs, JetPack 5 builds, no device) it is
 * skipped; with NVMPI_TEST_STRICT=1 every shortfall is a failure. When
 * NVMPI_HAVE_AV1 is 0 the case always skips.
 *
 * The mock covers AV1 separately in test_api_dec_mock.c (it only counts
 * buffers; real bitstream validation is what this case is for).
 */
#ifndef NVMPI_TEST_AV1_CASE_H
#define NVMPI_TEST_AV1_CASE_H

#include "av1_fixture.h"

#define AV1_FAIL(what) do { fprintf(stderr, "av1 FAIL %s:%d: %s\n", __func__, __LINE__, (what)); return 1; } while (0)

static int test_decoder_av1_frame(void)
{
    nvDecParam param;
    nvPacket pkt;
    nvmpictx *ctx;
    nvmpi_frame_release_cb rel = NULL;
    void *opaque = NULL;
    int fd = -1, w = 0, h = 0, pitch = 0, ret;
    int64_t ts = 0;

#if !NVMPI_HAVE_AV1
    /* Headers without V4L2_PIX_FMT_AV1 (or NVMPI_FORCE_NO_AV1): AV1 is
     * unsupported by construction, so skip even in strict mode. The mock
     * suite asserts the NULL create. */
    return 77;
#endif
    dec_param_init(&param, NV_VIDEO_CodingAV1);
    param.pixFormat = NV_PIX_NV12;
    param.frame_pool_size = 4;
    param.wait_timeout = 2000;   /* ms; the first frame also pays NVDEC start-up */
    ctx = nvmpi_create_decoder(&param);
    if (!ctx) return strict_mode ? 1 : 77;

    memset(&pkt, 0, sizeof(pkt));
    pkt.payload = (unsigned char *)av1_test_tu;
    pkt.payload_size = av1_test_tu_len;
    pkt.pts = 1;
    if (nvmpi_decoder_put_packet(ctx, &pkt) < 0) {
        nvmpi_decoder_close(ctx);
        AV1_FAIL("put_packet");
    }

    /* Zero-copy path (what the FFmpeg wrapper uses): it reports the size
     * without needing a caller buffer sized for the stream. */
    ret = nvmpi_decoder_get_frame_fd(ctx, &fd, &w, &h, &pitch, &ts, &rel,
                                     &opaque, true);
    if (ret != 0) {
        nvmpi_decoder_close(ctx);
        AV1_FAIL("no frame decoded");
    }
    if (rel)
        rel(opaque);             /* hand the frame back before close */
    if (w != AV1_FX_W || h != AV1_FX_H) {
        fprintf(stderr, "av1: got %dx%d, want %dx%d\n", w, h, AV1_FX_W, AV1_FX_H);
        nvmpi_decoder_close(ctx);
        AV1_FAIL("frame size");
    }
    return nvmpi_decoder_close(ctx) == 0 ? 0 : 1;
}

#endif /* NVMPI_TEST_AV1_CASE_H */
