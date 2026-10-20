/*
 * enc_mock_rc.h - VBR peak bitrate case of test_api_enc_mock.c (issue #141).
 * Included from enc_mock_cases.h, after enc_mock_util.h; runs inside the
 * forked case child like every other case.
 *
 * Contract under test (include/nvmpi.h, docs/api.md): in VBR mode the encoder
 * programs nvEncParam.peak_bitrate as the peak control; 0 derives it from the
 * bitrate (1.2x), and a peak below the bitrate is auto-corrected the same way.
 * The value the device received is read back from the mock through
 * mock_enc_last_peak_bitrate() (mock/mock_enc_ctl.c).
 */
#ifndef NVMPI_TEST_ENC_MOCK_RC_H
#define NVMPI_TEST_ENC_MOCK_RC_H

#include <dlfcn.h>

#define RC_BITRATE 2000000u

/* Create a VBR H.264 encoder with the given peak_bitrate and return the peak
 * the mock device received (-1 on any failure). */
static long vbr_peak_for(unsigned peak)
{
    typedef int (*peak_fn)(uint32_t *);
    peak_fn get = (peak_fn)dlsym(dlopen(NULL, RTLD_NOW),
                                "mock_enc_last_peak_bitrate");
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvEncParam p;
    nvmpictx *ctx;
    uint32_t got = 0;

    REQUIRE(get);
    memset(&p, 0, sizeof(p));
    p.width = EW;
    p.height = EH;
    p.profile = 77;
    p.level = 41;
    p.bitrate = RC_BITRATE;
    p.peak_bitrate = peak;
    p.mode_vbr = 1;
    p.pixFormat = NV_PIX_YUV420;
    p.fps_n = 30;
    p.fps_d = 1;
    p.capture_num = 4;
    p.hw_preset_type = 1;
    p.codingType = c.codec;
    p.wait_timeout = 1000;
    ctx = nvmpi_create_encoder(&p);
    REQUIRE(ctx);
    REQUIRE(get(&got) == 0);
    e_destroy(ctx, NULL);
    return (long)got;
}

static int vbr_peak_bitrate(void)
{
    /* explicit peak above the bitrate is passed through unchanged */
    REQUIRE(vbr_peak_for(3 * RC_BITRATE) == 3 * (long)RC_BITRATE);
    /* 0 = derive 1.2x bitrate (init: peak < bitrate branch) */
    REQUIRE(vbr_peak_for(0) == (long)(uint32_t)(1.2f * RC_BITRATE));
    /* below the bitrate: auto-corrected to 1.2x bitrate */
    REQUIRE(vbr_peak_for(RC_BITRATE / 2) == (long)(uint32_t)(1.2f * RC_BITRATE));
    /* equal to the bitrate is valid (not below): kept as is */
    REQUIRE(vbr_peak_for(RC_BITRATE) == (long)RC_BITRATE);
    return 0;
}

#endif /* NVMPI_TEST_ENC_MOCK_RC_H */
