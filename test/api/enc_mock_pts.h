/*
 * enc_mock_pts.h - encoder pts round-trip cases of test_api_enc_mock.c
 * (issue #148). Included from enc_mock_cases.h, after enc_mock_util.h.
 */
#ifndef NVMPI_TEST_ENC_MOCK_PTS_H
#define NVMPI_TEST_ENC_MOCK_PTS_H

#include "pts_values.h"

static void pts_report(int64_t want, unsigned long got)
{
    fprintf(stderr, "pts %lld came back as %lld\n", (long long)want,
            (long long)got);
}

/* put_frame -> get_packet in lockstep: the packet pts is the frame
 * timestamp, bit for bit. */
static int pts_roundtrip_enc(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvmpictx *ctx = e_create(&c);
    struct epkt p;
    int i;

    REQUIRE(ctx);
    for (i = 0; i < K_PTS_N; i++) {
        REQUIRE(e_put(ctx, (long)k_pts[i]) == 0);
        REQUIRE(e_get(ctx, 0, 1, &p) == 0);
        if (p.pts != (unsigned long)k_pts[i])
            pts_report(k_pts[i], p.pts);
        REQUIRE(p.pts == (unsigned long)k_pts[i]);
    }
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* Same through put_frame_fd. */
static int pts_roundtrip_enc_fd(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 1 };
    nvmpictx *ctx = e_create(&c);
    struct epkt p;
    int fd = -1, i;

    REQUIRE(ctx);
    REQUIRE(nvmpi_surface_alloc_for_enc(EW, EH, &fd) == 0 && fd >= 0);
    for (i = 0; i < K_PTS_N; i++) {
        REQUIRE(nvmpi_encoder_put_frame_fd(ctx, fd, EW, EH, EW,
                                           k_pts[i]) == 0);
        REQUIRE(e_get(ctx, 0, 1, &p) == 0);
        if (p.pts != (unsigned long)k_pts[i])
            pts_report(k_pts[i], p.pts);
        REQUIRE(p.pts == (unsigned long)k_pts[i]);
    }
    REQUIRE(nvmpi_surface_destroy(fd) == 0);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* Many distinct out-of-range values: the side table recycles its slots. */
#define PTS_LONG_N 1000
static int pts_roundtrip_enc_long(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvmpictx *ctx = e_create(&c);
    struct epkt p;
    int i;

    REQUIRE(ctx);
    for (i = 0; i < PTS_LONG_N; i++) {
        int64_t pts = i % 2 ? INT64_MIN + i : INT64_MAX - i;

        REQUIRE(e_put(ctx, (long)pts) == 0);
        REQUIRE(e_get(ctx, 0, 1, &p) == 0);
        REQUIRE(p.pts == (unsigned long)pts);
    }
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

#endif /* NVMPI_TEST_ENC_MOCK_PTS_H */
