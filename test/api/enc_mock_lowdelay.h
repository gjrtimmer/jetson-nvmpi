/*
 * enc_mock_lowdelay.h - blocking get_packet(wait=true) cases of
 * test_api_enc_mock.c (issue #103), kept in their own header so the main
 * file stays small. Included from enc_mock_cases.h, after enc_mock_util.h;
 * runs inside the forked case child like every other case.
 *
 * Contract under test (include/nvmpi.h): get_packet(wait=true) returns -1
 * at once when no accepted frame is still in flight, and otherwise waits
 * for the in-flight AUs. A low-delay caller (put, get, get...) must not
 * pay wait_timeout on the get that finds nothing. e_create() sets
 * wait_timeout to 1000 ms, so the old behaviour (N * 1 s) is unmistakable
 * against the 50 ms per-frame budget.
 */
#ifndef NVMPI_TEST_ENC_MOCK_LOWDELAY_H
#define NVMPI_TEST_ENC_MOCK_LOWDELAY_H

#define LD_FRAMES 20
#define LD_PTS0 500
#define LD_BUDGET_MS 50                  /* per-frame ceiling, bug = 1000  */

/* Encoder (wait_timeout 1000 ms from e_create). */
static nvmpictx *ld_create(int nonblocking)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, nonblocking, 0 };
    nvmpictx *ctx = e_create(&c);

    REQUIRE(ctx);
    return ctx;
}

/* Low-delay loop: put, get (one packet), get (nothing owed -> -1, fast). */
static int lowdelay_loop(void)
{
    nvmpictx *ctx = ld_create(0);
    struct epkt p;
    uint64_t t0 = e_now_ms();
    int i;

    for (i = 0; i < LD_FRAMES; i++) {
        REQUIRE(e_put(ctx, LD_PTS0 + i) == 0);
        REQUIRE(e_get(ctx, 0, 1, &p) == 0 && p.pts == (unsigned long)(LD_PTS0 + i));
        REQUIRE(e_get(ctx, 0, 1, &p) == -1);     /* nothing in flight */
    }
    fprintf(stderr, "lowdelay_loop: %d frames in %llu ms\n", LD_FRAMES,
            (unsigned long long)(e_now_ms() - t0));
    REQUIRE(e_now_ms() - t0 < (uint64_t)LD_FRAMES * LD_BUDGET_MS);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* Nothing ever submitted: the wait must not wait. */
static int lowdelay_idle(void)
{
    nvmpictx *ctx = ld_create(0);
    struct epkt p;
    uint64_t t0 = e_now_ms();

    REQUIRE(e_get(ctx, 0, 1, &p) == -1);
    REQUIRE(e_now_ms() - t0 < LD_BUDGET_MS);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* Non-blocking encoder with NVMPI_MOCK_ENC_LATENCY=2 (the model keeps two
 * OUTPUT buffers back). The AU of each frame arrives asynchronously on the
 * DQ thread, so right after put_frame the wait has pending > 0 and an empty
 * queue: it must wait for the AU (never a premature -1), then -1 at once
 * for the get that follows. */
static int lowdelay_latency(void)
{
    nvmpictx *ctx;
    struct epkt p;
    uint64_t t0;
    int i, r;

    setenv("NVMPI_MOCK_ENC_LATENCY", "2", 1);
    ctx = ld_create(1);
    t0 = e_now_ms();
    for (i = 0; i < LD_FRAMES; i++) {
        REQUIRE(e_put(ctx, LD_PTS0 + i) == 0);
        REQUIRE(e_get(ctx, 0, 1, &p) == 0 && p.pts == (unsigned long)(LD_PTS0 + i));
        REQUIRE(e_get(ctx, 0, 1, &p) == -1);
    }
    REQUIRE(e_now_ms() - t0 < (uint64_t)LD_FRAMES * LD_BUDGET_MS);
    REQUIRE(nvmpi_encoder_put_frame(ctx, NULL) == 0);
    while ((r = e_get(ctx, 0, 1, &p)) == 0)
        ;
    REQUIRE(r == -2);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* Flush: put_frame(NULL) then get_packet(true) gives the tail, then -2. */
static int lowdelay_flush(void)
{
    nvmpictx *ctx = ld_create(0);
    struct epkt p;
    int i, got = 0, r;

    for (i = 0; i < 4; i++)
        REQUIRE(e_put(ctx, LD_PTS0 + i) == 0);
    REQUIRE(nvmpi_encoder_put_frame(ctx, NULL) == 0);
    while ((r = e_get(ctx, 0, 1, &p)) == 0) {
        REQUIRE(p.pts == (unsigned long)(LD_PTS0 + got));
        got++;
    }
    REQUIRE(r == -2);
    REQUIRE(got == 4);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

#endif /* NVMPI_TEST_ENC_MOCK_LOWDELAY_H */
