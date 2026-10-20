/*
 * enc_mock_backpressure.h - non-blocking encoder back-pressure cases of
 * test_api_enc_mock.c (issue #125), kept in their own header so the main
 * file stays small. Included from enc_mock_cases.h, after enc_mock_util.h
 * and the case helpers; runs inside the forked case child like every other
 * case.
 *
 * Contract under test (include/nvmpi.h): in non-blocking mode put_frame
 * answers NVMPI_ERR_EAGAIN instead of accepting a frame whose access unit
 * could find no empty packet; the caller drains get_packet (returning each
 * packet with qEmptyPacket) and retries. Every accepted frame therefore
 * comes out as exactly one packet: nothing is dropped.
 */
#ifndef NVMPI_TEST_ENC_MOCK_BACKPRESSURE_H
#define NVMPI_TEST_ENC_MOCK_BACKPRESSURE_H

#define BP_FRAMES 24
#define BP_PTS0 100
#define BP_WAIT_MS 5000                  /* generous: a stall fails, not hangs */

/* Encoder with a pool of only `pool` packets (e_create seeds E_NPKT; the
 * surplus is taken back and freed before any frame is queued). */
static nvmpictx *bp_create(int pool)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 1, 0 };
    nvmpictx *ctx = e_create(&c);
    nvPacket *pk;
    int i;

    REQUIRE(ctx);
    for (i = pool; i < E_NPKT; i++) {
        REQUIRE(nvmpi_encoder_dqEmptyPacket(ctx, &pk) == 0);
        free(pk->payload);
        free(pk);
    }
    return ctx;
}

/* put_frame; on EAGAIN give the DQ thread a moment and ask once more. A
 * single EAGAIN can be a transient (the callback released a packet but not
 * yet its AU count, admission is then conservative); after the settle time
 * the answer is the real one. */
static int bp_put_settled(nvmpictx *ctx, long pts)
{
    int r = e_put(ctx, pts);

    if (r == NVMPI_ERR_EAGAIN) {
        usleep(30 * 1000);
        r = e_put(ctx, pts);
    }
    return r;
}

/* Read every ready packet, checking the pts order; returns how many. */
static int bp_drain(nvmpictx *ctx, int already)
{
    struct epkt p;
    int n = 0;

    while (e_get(ctx, 0, 0, &p) == 0) {
        REQUIRE(p.pts == (unsigned long)(BP_PTS0 + already + n) && p.size > 0);
        n++;
    }
    return n;
}

/* While stderr is captured a failed check must still reach the real one. */
static int bp_saved = -1;
#define BPR(c)                                                            \
    do {                                                                  \
        if (!(c)) {                                                       \
            dup2(bp_saved, 2);                                            \
            REQUIRE(c);                                                   \
        }                                                                 \
    } while (0)

/* Run the stream: BP_FRAMES frames, the caller reads only when put_frame
 * says EAGAIN (a slow reader), then EOS and a full drain. Returns the
 * number of "dropped" log lines seen on stderr (libnvmpi prints one per
 * lost AU). */
static int bp_stream(int pool, int latency)
{
    int accepted = 0, delivered = 0, eagain = 0, i, eos = 0;
    uint64_t t0;
    struct epkt p;
    nvmpictx *ctx;
    char buf[8192];
    FILE *cap;
    int saved, drops = 0;
    size_t n;

    fflush(stderr);                      /* capture libnvmpi's log lines   */
    cap = tmpfile();
    REQUIRE(cap);
    bp_saved = saved = dup(2);
    BPR(saved >= 0 && dup2(fileno(cap), 2) >= 0);

    if (latency) {
        char v[8];
        snprintf(v, sizeof(v), "%d", latency);
        setenv("NVMPI_MOCK_ENC_LATENCY", v, 1);
    }
    ctx = bp_create(pool);

    /* Phase A: no reader. A settled encoder accepts exactly `pool` frames
     * with latency 0 (each AU owns one packet), then says EAGAIN. */
    if (!latency) {
        while (accepted < BP_FRAMES &&
               bp_put_settled(ctx, BP_PTS0 + accepted) == 0)
            accepted++;      /* bounded: no back-pressure = no end */
        usleep(50 * 1000);
        delivered += bp_drain(ctx, delivered);
        dprintf(bp_saved, "phase A: accepted %d, delivered %d (pool %d)\n",
                accepted, delivered, pool);
        BPR(accepted == pool);
        BPR(delivered == accepted);
        eagain++;
    }

    /* Phase B: the slow reader. */
    t0 = e_now_ms();
    for (i = accepted; i < BP_FRAMES; i++) {
        int r;

        while ((r = e_put(ctx, BP_PTS0 + i)) != 0) {
            BPR(r == NVMPI_ERR_EAGAIN);
            eagain++;
            delivered += bp_drain(ctx, delivered);
            usleep(1000);
            BPR(e_now_ms() - t0 < BP_WAIT_MS);
        }
        accepted++;
    }
    BPR(eagain > 0);

    /* EOS is never refused for lack of packets; EAGAIN only if no OUTPUT
     * buffer is free, in which case drain and retry. */
    while (nvmpi_encoder_put_frame(ctx, NULL) != 0) {
        delivered += bp_drain(ctx, delivered);
        usleep(1000);
        BPR(e_now_ms() - t0 < BP_WAIT_MS);
    }
    while (!eos) {
        int r = e_get(ctx, 0, 0, &p);

        if (r == 0) {
            BPR(p.pts == (unsigned long)(BP_PTS0 + delivered));
            delivered++;
        } else {
            eos = r == -2;
        }
        BPR(e_now_ms() - t0 < BP_WAIT_MS);
    }
    BPR(e_destroy(ctx, NULL) == 0);

    dprintf(bp_saved, "stream: accepted %d delivered %d eagain %d\n", accepted,
            delivered, eagain);
    fflush(stderr);
    rewind(cap);                         /* count lost-AU log lines        */
    while ((n = fread(buf, 1, sizeof(buf) - 1, cap)) > 0) {
        char *s = buf;

        buf[n] = '\0';
        while ((s = strstr(s, "dropped")) != NULL) {
            drops++;
            s++;
        }
    }
    dup2(saved, 2);
    dprintf(2, "accepted %d delivered %d eagain %d drop-lines %d\n",
            accepted, delivered, eagain, drops);
    close(saved);
    fclose(cap);
    BPR(accepted == BP_FRAMES);
    BPR(delivered == accepted);      /* a frame in = a packet out      */
    BPR(drops == 0);
    return drops;
}

/* Pool of 2, encoder keeps up: the reader is slower than the writer. */
static int nonblock_backpressure(void)
{
    return bp_stream(2, 0) != 0;
}

/* Pool of 3 against an encoder that holds 2 frames back (mock latency): the
 * AUs arrive late, the pool covers the delay plus one. Nothing is lost and
 * the EOS flush delivers the held-back tail. */
static int nonblock_backpressure_latency(void)
{
    return bp_stream(3, 2) != 0;
}

#endif /* NVMPI_TEST_ENC_MOCK_BACKPRESSURE_H */
