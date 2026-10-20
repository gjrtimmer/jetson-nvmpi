/*
 * dec_mock_latency.h - create->close latency cases of test_api_dec_mock.c
 * (issue #105), kept in their own header so the main file stays small.
 * Included once from test_api_dec_mock.c after the helpers (create(),
 * REQUIRE); each case runs inside the forked case child.
 *
 * The mock answers an empty VIDIOC_DQEVENT with EAGAIN, so the real
 * NvV4l2Element::dqEvent honours its timeout exactly like the driver: a
 * capture thread waiting for the first event blocks for the whole slice,
 * and close() has to wait for that slice. That makes the elapsed time here
 * a faithful measure of the shutdown latency, not just of mock overhead.
 */
#ifndef NVMPI_TEST_DEC_MOCK_LATENCY_H
#define NVMPI_TEST_DEC_MOCK_LATENCY_H

#include <time.h>

/* Bound for create->close with no data. The pre-fix latency was 0.5-1.5 s
 * (500 ms dqEvent slice + 500 ms race-guard wait); the fix needs one 50 ms
 * slice. 200 ms leaves 4x headroom for sanitizer builds on a loaded runner
 * and is still far below the 500 ms quantum a regression would show. */
#define CLOSE_NO_DATA_MAX_MS 200
#define CLOSE_NO_DATA_CYCLES 5

static long lat_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Rapid create->close, nothing ever queued: the capture thread is still in
 * its pre-event wait when close() sets eos. Every cycle must stay under the
 * bound (the hardware test_api shows 1.0 / 1.5 / 1.0 s before the fix). */
static int teardown_no_data_latency(void)
{
    int i;

    for (i = 0; i < CLOSE_NO_DATA_CYCLES; i++) {
        long t0 = lat_now_ms();
        nvmpictx *c = create(0, 0, 4);
        long ms;

        REQUIRE(c);
        REQUIRE(nvmpi_decoder_close(c) == 0);
        ms = lat_now_ms() - t0;
        fprintf(stderr, "    cycle %d create->close %ld ms\n", i, ms);
        REQUIRE(ms < CLOSE_NO_DATA_MAX_MS);
    }
    return 0;
}

/* Same with a container hint: the frame pool is pre-allocated at create
 * time, which must not reintroduce a wait on close. */
static int teardown_no_data_hint_latency(void)
{
    long t0 = lat_now_ms();
    nvmpictx *c = create(640, 360, 4);
    long ms;

    REQUIRE(c);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    ms = lat_now_ms() - t0;
    fprintf(stderr, "    create->close %ld ms\n", ms);
    REQUIRE(ms < CLOSE_NO_DATA_MAX_MS);
    return 0;
}

#endif /* NVMPI_TEST_DEC_MOCK_LATENCY_H */
