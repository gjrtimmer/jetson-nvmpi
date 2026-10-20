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
 * slice (~55 ms on the Orin, ~75 ms on the mock). 400 ms is the bound:
 * ASan with parallel ctests on a loaded runner reached 249 ms (#132), and
 * the regression can never come in under 500 ms (the race-guard wait
 * alone), so the check still catches it. */
#define CLOSE_NO_DATA_MAX_MS 400
#define CLOSE_NO_DATA_CYCLES 5

static long lat_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Time only nvmpi_decoder_close(): create() is a cold open that stretches
 * under CPU contention (630 ms seen with 7 parallel ASan ctests on 8 vCPU)
 * and says nothing about shutdown latency. */
static long lat_close_ms(nvmpictx *c)
{
    long t0 = lat_now_ms();

    if (nvmpi_decoder_close(c) != 0)
        return -1;
    return lat_now_ms() - t0;
}

/* Assert on the fastest cycle: a load spike can slow one cycle, but a
 * regression puts every cycle at >= 500 ms (race-guard wait alone), so the
 * minimum stays a faithful detector. All times are printed on failure. */
static int lat_check_min(const long *ms, int n)
{
    long min = ms[0];
    int i;

    for (i = 0; i < n; i++) {
        fprintf(stderr, "    cycle %d close %ld ms\n", i, ms[i]);
        if (ms[i] < 0)
            return 1;
        if (ms[i] < min)
            min = ms[i];
    }
    fprintf(stderr, "    min close %ld ms (bound %d)\n", min,
            CLOSE_NO_DATA_MAX_MS);
    return min < CLOSE_NO_DATA_MAX_MS ? 0 : 1;
}

/* Rapid create->close, nothing ever queued: the capture thread is still in
 * its pre-event wait when close() sets eos (the hardware test_api shows
 * 1.0 / 1.5 / 1.0 s before the fix). */
static int teardown_no_data_latency(void)
{
    long ms[CLOSE_NO_DATA_CYCLES];
    int i;

    for (i = 0; i < CLOSE_NO_DATA_CYCLES; i++) {
        nvmpictx *c = create(0, 0, 4);

        REQUIRE(c);
        ms[i] = lat_close_ms(c);
    }
    return lat_check_min(ms, CLOSE_NO_DATA_CYCLES);
}

/* Same with a container hint: the frame pool is pre-allocated at create
 * time, which must not reintroduce a wait on close. */
static int teardown_no_data_hint_latency(void)
{
    long ms[CLOSE_NO_DATA_CYCLES];
    int i;

    for (i = 0; i < CLOSE_NO_DATA_CYCLES; i++) {
        nvmpictx *c = create(640, 360, 4);

        REQUIRE(c);
        ms[i] = lat_close_ms(c);
    }
    return lat_check_min(ms, CLOSE_NO_DATA_CYCLES);
}

#endif /* NVMPI_TEST_DEC_MOCK_LATENCY_H */
