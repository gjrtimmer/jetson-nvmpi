/*
 * dec_mock_pts.h - decoder pts round-trip cases of test_api_dec_mock.c
 * (issue #148). Included from test_api_dec_mock.c after the helpers
 * (create(), put(), take(), REQUIRE); each case runs in the forked child.
 */
#ifndef NVMPI_TEST_DEC_MOCK_PTS_H
#define NVMPI_TEST_DEC_MOCK_PTS_H

#include "pts_values.h"

/* One packet per value, frame taken in lockstep: the copy-out path must
 * return the exact pts that went into put_packet. */
static int pts_roundtrip_copy(void)
{
    uint8_t au[4096];
    nvmpictx *c = create(0, 0, 4);
    int i;

    REQUIRE(c);
    for (i = 0; i < K_PTS_N; i++) {
        size_t len = i == 0 ? fx_idr(au, 320, 240) : fx_pslice(au);
        int gray = 1;

        REQUIRE(put(c, au, len, (unsigned long)k_pts[i]) == 0);
        REQUIRE(take(c, 320, 240, (long)k_pts[i], &gray) == 0);
        if (!gray)
            fprintf(stderr, "pts %lld did not round trip\n",
                    (long long)k_pts[i]);
        REQUIRE(gray);
    }
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* Same through the zero-copy path (get_frame_fd), several packets in
 * flight before the first frame is taken. */
static int pts_roundtrip_fd(void)
{
    uint8_t au[4096];
    nvmpictx *c = create(0, 0, 6);
    int i, b;

    REQUIRE(c);
    for (b = 0; b + 3 <= K_PTS_N; b += 3) {
        for (i = b; i < b + 3; i++) {
            size_t len = i == 0 ? fx_idr(au, 320, 240) : fx_pslice(au);

            REQUIRE(put(c, au, len, (unsigned long)k_pts[i]) == 0);
        }
        for (i = b; i < b + 3; i++) {
            int fd, w, h, pitch;
            int64_t ts = 0;
            nvmpi_frame_release_cb rel = NULL;
            void *opaque = NULL;

            REQUIRE(nvmpi_decoder_get_frame_fd(c, &fd, &w, &h, &pitch, &ts,
                                               &rel, &opaque, true) == 0);
            if (ts != k_pts[i])
                fprintf(stderr, "pts %lld came back as %lld\n",
                        (long long)k_pts[i], (long long)ts);
            REQUIRE(ts == k_pts[i]);
            if (rel)
                rel(opaque);
        }
    }
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* A long run of distinct out-of-range values (more than any side table
 * holds) in lockstep must stay exact: slots are recycled. */
#define PTS_LONG_N 1000
static int pts_roundtrip_long(void)
{
    uint8_t au[4096];
    nvmpictx *c = create(0, 0, 4);
    int i;

    REQUIRE(c);
    for (i = 0; i < PTS_LONG_N; i++) {
        size_t len = i == 0 ? fx_idr(au, 320, 240) : fx_pslice(au);
        int64_t pts = i % 2 ? INT64_MIN + i : INT64_MAX - i;
        int gray = 1;

        REQUIRE(put(c, au, len, (unsigned long)pts) == 0);
        REQUIRE(take(c, 320, 240, (long)pts, &gray) == 0);
        REQUIRE(gray);
    }
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

#endif /* NVMPI_TEST_DEC_MOCK_PTS_H */
