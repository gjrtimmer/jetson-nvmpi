/*
 * enc_mock_teardown.h - teardown_held_packet case of test_api_enc_mock.c
 * (issue #121), kept in its own header so the main file stays small.
 * Included from enc_mock_cases.h, after enc_mock_util.h and the case
 * helpers; runs inside the forked case child like every other case.
 */
#ifndef NVMPI_TEST_ENC_MOCK_TEARDOWN_H
#define NVMPI_TEST_ENC_MOCK_TEARDOWN_H

#define HELD_DELAY_US "300000"           /* DQ callback stalled for 300 ms */
#define HELD_DRAIN_MAX_MS 900            /* settle ends on release, not on
                                          * the 1 s backstop               */

/* The drain of e_destroy() while the DQ callback holds a packet: the mock
 * stalls the callback inside its metadata query (NVMPI_MOCK_CTRL_DELAY_US),
 * i.e. after it took a packet from the pool and before it published it. A
 * drain that ignores the held packet frees E_NPKT - 1 and close() discards
 * (leaks) the last one. Every packet must come back: empty queue first (the
 * order the FFmpeg wrapper uses), then the filled one. */
static int teardown_held_packet(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvmpictx *ctx;
    nvPacket *pk;
    uint64_t t0;
    int freed = 0;

    setenv("NVMPI_MOCK_CTRL_DELAY_US", HELD_DELAY_US, 1);
    ctx = e_create(&c);
    REQUIRE(ctx);
    REQUIRE(e_put(ctx, 1) == 0);
    usleep(30 * 1000);                   /* callback is now inside the stall */
    t0 = e_now_ms();
    while (nvmpi_encoder_dqEmptyPacket(ctx, &pk) == 0) {
        free(pk->payload);
        free(pk);
        freed++;
    }
    while (nvmpi_encoder_get_packet(ctx, &pk, false) == 0) {
        free(pk->payload);
        free(pk);
        freed++;
    }
    fprintf(stderr, "drain took %llu ms, freed %d\n",
            (unsigned long long)(e_now_ms() - t0), freed);
    REQUIRE(e_now_ms() - t0 < HELD_DRAIN_MAX_MS);
    REQUIRE(freed == E_NPKT);            /* none left for close() to discard */
    REQUIRE(nvmpi_encoder_close(ctx) == 0);
    return 0;
}

/* #131: drain the pool while one AU is held by the stalled callback and a
 * second one is queued behind it. The callback finds the pool empty for the
 * second AU only AFTER the drain started, so the drop is deterministic: it
 * must be silent (strict mode fails on the ERROR log). */
static int teardown_pool_empty_drop(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvmpictx *ctx;
    nvPacket *pk;
    int freed = 0;

    setenv("NVMPI_MOCK_CTRL_DELAY_US", HELD_DELAY_US, 1);
    ctx = e_create(&c);
    REQUIRE(ctx);
    REQUIRE(e_put(ctx, 1) == 0);
    REQUIRE(e_put(ctx, 2) == 0);
    usleep(30 * 1000);                   /* callback stalled on AU 1       */
    while (nvmpi_encoder_dqEmptyPacket(ctx, &pk) == 0) {
        free(pk->payload);
        free(pk);
        freed++;
    }
    usleep(700 * 1000);                  /* AU 2 hits the empty pool       */
    while (nvmpi_encoder_get_packet(ctx, &pk, false) == 0) {
        free(pk->payload);
        free(pk);
        freed++;
    }
    REQUIRE(freed == E_NPKT);            /* AU 1 published, AU 2 discarded */
    REQUIRE(nvmpi_encoder_close(ctx) == 0);
    return 0;
}

#endif /* NVMPI_TEST_ENC_MOCK_TEARDOWN_H */
