/*
 * av1_enc_case.h - hardware case for AV1 encode (#8), included by
 * test_api.c. Encodes 10 frames at 320x240 with a key frame every 5, checks
 * the packets (OBU temporal units, key flags at 0 and 5, "12 00" start, no
 * IVF "DKIF", sequence header on key frames) and decodes them again through
 * the AV1 decoder (10 frames at 320x240); see av1_enc_rt.h.
 *
 * Needs an NVENC and an NVDEC with AV1 support (Orin; Xavier has neither).
 * Outside strict mode (stubs, JetPack 5 builds, no device) it is skipped;
 * with NVMPI_TEST_STRICT=1 every shortfall is a failure. When
 * NVMPI_HAVE_AV1_ENC is 0 the case always skips. Under the mock
 * (ctest api_mock) the same case runs against the mock encoder and decoder.
 */
#ifndef NVMPI_TEST_AV1_ENC_CASE_H
#define NVMPI_TEST_AV1_ENC_CASE_H

#if NVMPI_HAVE_AV1_ENC
#include "av1_obu.h"
#include "av1_enc_rt.h"
#endif

static int test_encoder_av1_roundtrip(void)
{
#if !NVMPI_HAVE_AV1_ENC
    /* Headers without the AV1 encode controls (or NVMPI_FORCE_NO_AV1[_ENC]):
     * unsupported by construction, so skip even in strict mode. The mock
     * suite asserts the NULL create. */
    return 77;
#else
    return av1_enc_dec_roundtrip(!strict_mode);
#endif
}

/* #146: AV1 + P010 must be refused by create (NULL), with no device
 * access: on L4T R36.4 the Orin encoder accepts the format but fails the
 * first frame (NvEncNvVicConvertSurfaceFormat failed, BlockSide error 0x4).
 * A NULL from create also covers builds without the AV1 encode controls;
 * both outcomes are the same contract, so no skip is needed. */
static int test_encoder_av1_p010_refused(void)
{
    nvEncParam ep;
    nvmpictx *ctx;

    memset(&ep, 0, sizeof(ep));
    ep.codingType = NV_VIDEO_CodingAV1;
    ep.width = 320;
    ep.height = 240;
    ep.bitrate = 2000000;
    ep.fps_n = 30;
    ep.fps_d = 1;
    ep.capture_num = 4;
    ep.hw_preset_type = 1;
    ep.pixFormat = NV_PIX_P010;
    ep.wait_timeout = 2000;
    ctx = nvmpi_create_encoder(&ep);
    if (ctx) {
        nvmpi_encoder_close(ctx);
        fprintf(stderr, "AV1 + P010 create returned a context (must be refused)\n");
        return 1;
    }
    return 0;
}

#endif /* NVMPI_TEST_AV1_ENC_CASE_H */
