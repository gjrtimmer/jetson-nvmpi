/*
 * dec_mock_av1.h - AV1 case of test_api_dec_mock.c (#7). Included once from
 * test_api_dec_mock.c after the helpers; runs inside the forked case child.
 *
 * The mock treats every non-H.264/HEVC OUTPUT buffer as one frame and has no
 * AV1 header parser, so the stream size comes from NVMPI_MOCK_WIDTH/HEIGHT.
 * This proves the AV1 create path, the fourcc mapping (create must succeed
 * only because the codec switch resolved AV1) and the one-temporal-unit-per-
 * packet flow; it does not validate the bitstream (the hardware case does).
 */
#ifndef NVMPI_TEST_DEC_MOCK_AV1_H
#define NVMPI_TEST_DEC_MOCK_AV1_H

#include "av1_fixture.h"

static int decode_av1(void)
{
    nvDecParam p;
    nvmpictx *c;
    int i, w = 0, h = 0;

    setenv("NVMPI_MOCK_WIDTH", "64", 1);
    setenv("NVMPI_MOCK_HEIGHT", "64", 1);
    memset(&p, 0, sizeof(p));
    p.codingType = NV_VIDEO_CodingAV1;
    p.pixFormat = NV_PIX_NV12;
    p.frame_pool_size = 4;
    p.wait_timeout = 1000;
    c = nvmpi_create_decoder(&p);
#if !NVMPI_HAVE_AV1
    /* No V4L2_PIX_FMT_AV1 (R35 headers or NVMPI_FORCE_NO_AV1): create must
     * fail cleanly with "AV1 decode not supported by this L4T build". */
    REQUIRE(c == NULL);
    (void)i; (void)w; (void)h;
    return 0;
#endif
    REQUIRE(c);
    for (i = 0; i < 3; i++) {            /* one temporal unit = one frame */
        REQUIRE(put(c, av1_test_tu, av1_test_tu_len, 100 + i) == 0);
        REQUIRE(take_fd(c, &w, &h) == 0);
        REQUIRE(w == AV1_FX_W && h == AV1_FX_H);
    }
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* Codec types the decoder must refuse: no device open, NULL, no fallback to
 * H.264. The strict log check allows this case's expected errors. */
static int reject_codecs(void)
{
    /* No out-of-range enum value: loading one is UB and UBSan reports it. */
    static const int bad[] = { NV_VIDEO_CodingUnused, NV_VIDEO_CodingMJPEG };
    nvDecParam p;
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memset(&p, 0, sizeof(p));
        p.codingType = (nvCodingType)bad[i];
        p.pixFormat = NV_PIX_NV12;
        REQUIRE(nvmpi_create_decoder(&p) == NULL);
    }
    return 0;
}

/* Single table row (keeps the k_cases edit minimal). Registered with the
 * never-firing fault "dqbuf@9999" so reject_codecs' expected error logs do
 * not trip the strict log check; decode_av1 itself logs no errors. */
static int av1_case(void)
{
    return decode_av1() ? 1 : reject_codecs();
}

#endif /* NVMPI_TEST_DEC_MOCK_AV1_H */
