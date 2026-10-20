/*
 * enc_mock_p010.h - P010 input / HEVC Main10 cases of test_api_enc_mock.c
 * (issue #126). Included from enc_mock_cases.h, after enc_mock_util.h; runs
 * inside the forked case child like every other case.
 *
 * Contract under test (include/nvmpi.h, docs/api.md):
 *   - HEVC + NV_PIX_P010 encodes, programs the Main10 profile control and
 *     the stream says Main10 / 10-bit (SPS parsed from the first packet);
 *   - HEVC with 8-bit input programs Main;
 *   - H.264 + P010 and AV1 + P010 are refused by create, and Main10 without
 *     P010 too, all before any device is opened (run with open@1+: every
 *     open fails, so any open error in the log means create got that far);
 *   - a P010 frame whose planes are too small is refused with
 *     NVMPI_ERR_INVAL and does not consume an OUTPUT buffer.
 * The programmed profile is read back from the mock through
 * mock_enc_last_h265_profile() (mock/mock_enc_ctl.c).
 */
#ifndef NVMPI_TEST_ENC_MOCK_P010_H
#define NVMPI_TEST_ENC_MOCK_P010_H

#include "enc_p010_rt.h"

/* Exported by the mock libnvv4l2; weak so the test links against the stubs
 * (NULL there). Same pattern as mock_enc_last_peak_bitrate in enc_mock_rc.h. */
extern int mock_enc_last_h265_profile(uint32_t *out) __attribute__((weak));

#define P010_HEVC_MAIN   0u      /* V4L2_MPEG_VIDEO_H265_PROFILE_MAIN   */
#define P010_HEVC_MAIN10 1u      /* V4L2_MPEG_VIDEO_H265_PROFILE_MAIN10 */

static int p010_hevc_main10(void)
{
    uint32_t prof = 99;

    REQUIRE(mock_enc_last_h265_profile);
    REQUIRE(p010_enc_run(0, 0) == 0);             /* SPS checked inside    */
    REQUIRE(mock_enc_last_h265_profile(&prof) == 0 && prof == P010_HEVC_MAIN10);
    return 0;
}

/* 8-bit HEVC stays Main (and a Main request with P010 is overridden). */
static int p010_hevc_main(void)
{
    struct ecfg c = { NV_VIDEO_CodingHEVC, 0, 0, 0, 0, 0 };
    nvmpictx *ctx = e_create(&c);
    uint32_t prof = 99;
    struct epkt p;
    nvPacket *pk;

    REQUIRE(mock_enc_last_h265_profile && ctx);
    REQUIRE(mock_enc_last_h265_profile(&prof) == 0 && prof == P010_HEVC_MAIN);
    REQUIRE(e_put(ctx, 1) == 0 && e_get(ctx, 1, 1, &p) == 0 && p.has_sps);
    REQUIRE(e_destroy(ctx, NULL) == 0);

    /* P010 with an explicit Main request: P010 wins, the stream is Main10. */
    {
        nvEncParam ep;
        nvmpictx *e2;

        p010_fill_param(&ep, NV_VIDEO_CodingHEVC, NV_PIX_P010, NVMPI_HEVC_PROFILE_MAIN);
        e2 = nvmpi_create_encoder(&ep);
        REQUIRE(e2);
        REQUIRE(mock_enc_last_h265_profile(&prof) == 0 && prof == P010_HEVC_MAIN10);
        while (nvmpi_encoder_dqEmptyPacket(e2, &pk) == 0)
            ;
        REQUIRE(nvmpi_encoder_close(e2) == 0);
    }
    return 0;
}

/* Create must fail before the device is touched (open@1+). Returns the
 * stderr capture so the caller can look for the message. */
static int p010_expect_reject(nvCodingType codec, nvPixFormat pix, unsigned prof,
                              const char *msg)
{
    char buf[4096];
    nvEncParam p;
    ssize_t n;
    int fd;

    p010_fill_param(&p, codec, pix, prof);
    REQUIRE(nvmpi_create_encoder(&p) == NULL);
    fd = open("/proc/self/fd/2", O_RDONLY);
    REQUIRE(fd >= 0);
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    REQUIRE(n > 0);
    buf[n] = '\0';
    REQUIRE(strstr(buf, msg) != NULL);
    REQUIRE(strstr(buf, "Encoder factory") == NULL);
    REQUIRE(strstr(buf, "Could not create encoder") == NULL);
    return 0;
}

static int p010_create_gate(void)
{
    REQUIRE(p010_expect_reject(NV_VIDEO_CodingH264, NV_PIX_P010, 77,
                               "P010 (10-bit) encode input is supported for HEVC only") == 0);
    REQUIRE(p010_expect_reject(NV_VIDEO_CodingHEVC, NV_PIX_NV12,
                               NVMPI_HEVC_PROFILE_MAIN10,
                               "HEVC Main10 requires NV_PIX_P010") == 0);
    return 0;
}

/* Frame bounds: P010 planes need 2 bytes per sample. A frame described with
 * 8-bit linesizes is refused up front (no OUTPUT slot consumed, so the 6
 * refusals below do not exhaust the 4-buffer queue), and a good frame still
 * encodes afterwards. */
static int p010_frame_bounds(void)
{
    static uint16_t y[P010_W * P010_H], uv[P010_W * P010_H / 2];
    nvEncParam ep;
    nvmpictx *ctx;
    nvPacket *pk;
    nvFrame f;
    struct epkt p;
    int i;

    p010_fill_param(&ep, NV_VIDEO_CodingHEVC, NV_PIX_P010, NVMPI_HEVC_PROFILE_MAIN10);
    ctx = nvmpi_create_encoder(&ep);
    REQUIRE(ctx);
    for (i = 0; i < E_NPKT; i++) {
        pk = calloc(1, sizeof(*pk));
        REQUIRE(pk && (pk->payload = calloc(1, E_PKT_SZ)));
        nvmpi_encoder_qEmptyPacket(ctx, pk);
    }
    p010_frame(&f, y, uv, 0);
    f.linesize[0] = P010_W;                       /* 8-bit stride: too small */
    for (i = 0; i < 6; i++)
        REQUIRE(nvmpi_encoder_put_frame(ctx, &f) == NVMPI_ERR_INVAL);
    p010_frame(&f, y, uv, 0);
    f.linesize[1] = P010_W;
    REQUIRE(nvmpi_encoder_put_frame(ctx, &f) == NVMPI_ERR_INVAL);
    p010_frame(&f, y, uv, 0);
    f.payload[1] = NULL;
    REQUIRE(nvmpi_encoder_put_frame(ctx, &f) == NVMPI_ERR_INVAL);
    for (i = 0; i < 6; i++) {                     /* more than the 4 buffers */
        p010_frame(&f, y, uv, i);
        REQUIRE(nvmpi_encoder_put_frame(ctx, &f) == 0);
        REQUIRE(e_get(ctx, 1, 1, &p) == 0 && p.pts == 1000ul + i);
    }
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

#endif /* NVMPI_TEST_ENC_MOCK_P010_H */
