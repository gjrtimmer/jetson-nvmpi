/*
 * enc_mock_drain.h - FFmpeg-wrapper-shaped encode/EOS-drain loop (issue #111
 * family). Mirrors nvmpi_enc_runtime.c: optional throw-away extradata encoder
 * first (+global_header), then the real encoder; per frame put_frame then
 * get_packet until none is ready; EOS = put_frame(NULL), then get_packet
 * (blocking flag as the wrapper sets it while flushing) until -2. Counts
 * delivered packets and the "dropped" log lines so a loss is attributed to
 * the pool-empty path or to a drain race. Iterations come from
 * NVMPI_DRAIN_ITERS (default 5); NVMPI_DRAIN_JITTER_US adds random reader
 * stalls to emulate a loaded host. Runs in the forked case child.
 */
#ifndef NVMPI_TEST_ENC_MOCK_DRAIN_H
#define NVMPI_TEST_ENC_MOCK_DRAIN_H

#define DR_FRAMES 30
#define DR_POOL 10                       /* FFmpeg packet_pool_size default */

/* Extradata encoder like +global_header: blocking, one frame, get, EOS,
 * drain to -2, close. */
static void dr_extradata(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvmpictx *ctx = e_create(&c);
    struct epkt p;
    int r, n = 0;

    REQUIRE(ctx);
    REQUIRE(e_put(ctx, 1000) == 0);
    while ((r = e_get(ctx, 0, 0, &p)) != 0)
        REQUIRE(r == -1);
    REQUIRE(nvmpi_encoder_put_frame(ctx, NULL) == 0);
    while ((r = e_get(ctx, 0, 0, &p)) != -2)
        n += r == 0;
    REQUIRE(e_destroy(ctx, NULL) == 0);
}

static void dr_jitter(unsigned max_us)
{
    if (max_us)
        usleep((unsigned)rand() % max_us);
}

/* One stream; returns delivered packets, *drops = pool-empty log lines. */
static int dr_stream(int nonblock, int extradata, unsigned jitter_us)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, nonblock, 0 };
    int i, r, got = 0;
    nvmpictx *ctx;
    struct epkt p;
    nvPacket *pk;

    if (extradata)
        dr_extradata();
    ctx = e_create(&c);
    REQUIRE(ctx);
    for (i = DR_POOL; i < E_NPKT; i++) {     /* pool like FFmpeg default */
        REQUIRE(nvmpi_encoder_dqEmptyPacket(ctx, &pk) == 0);
        free(pk->payload);
        free(pk);
    }
    for (i = 0; i < DR_FRAMES; i++) {
        while ((r = e_put(ctx, 100 + i)) == NVMPI_ERR_EAGAIN) {
            while (e_get(ctx, 0, 1, &p) == 0)   /* send_eagain: wait=1 */
                got++;
        }
        REQUIRE(r == 0);
        dr_jitter(jitter_us);
        while (e_get(ctx, 0, 0, &p) == 0)
            got++;
    }
    while (nvmpi_encoder_put_frame(ctx, NULL) == NVMPI_ERR_EAGAIN)
        while (e_get(ctx, 0, 0, &p) == 0)
            got++;
    dr_jitter(jitter_us);
    for (;;) {                               /* flushing: wait=1 */
        r = e_get(ctx, 0, 1, &p);
        if (r == 0)
            got++;
        else if (r == -2)
            break;
    }
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return got;
}

static int dr_count_drops(FILE *cap)
{
    char buf[4096];
    size_t n;
    int drops = 0;

    rewind(cap);
    while ((n = fread(buf, 1, sizeof(buf) - 1, cap)) > 0) {
        char *s = buf;

        buf[n] = '\0';
        while ((s = strstr(s, "pool empty")) != NULL) {
            drops++;
            s++;
        }
    }
    return drops;
}

static int dr_run(int nonblock, int extradata)
{
    const char *e = getenv("NVMPI_DRAIN_ITERS");
    const char *j = getenv("NVMPI_DRAIN_JITTER_US");
    int iters = e ? atoi(e) : 5, k, lost = 0, drop_runs = 0;
    unsigned jitter = j ? (unsigned)atoi(j) : 0;
    int saved;
    FILE *cap;

    fflush(stderr);
    cap = tmpfile();
    REQUIRE(cap);
    saved = dup(2);
    REQUIRE(saved >= 0 && dup2(fileno(cap), 2) >= 0);
    for (k = 0; k < iters; k++) {
        int got, drops;

        rewind(cap);
        REQUIRE(ftruncate(fileno(cap), 0) == 0);
        got = dr_stream(nonblock, extradata, jitter);
        fflush(stderr);
        drops = dr_count_drops(cap);
        if (got != DR_FRAMES) {
            lost++;
            dprintf(saved, "iter %d: %d/%d packets, pool-empty drops %d\n",
                    k, got, DR_FRAMES, drops);
        }
        drop_runs += drops != 0;
    }
    dup2(saved, 2);
    close(saved);
    fclose(cap);
    dprintf(2, "drain nb=%d extradata=%d: %d iters, %d lossy, %d with drops\n",
            nonblock, extradata, iters, lost, drop_runs);
    return lost != 0 || drop_runs != 0;
}

static int drain_wrapper_blocking(void) { return dr_run(0, 1); }
static int drain_wrapper_nonblock(void) { return dr_run(1, 1); }

#endif /* NVMPI_TEST_ENC_MOCK_DRAIN_H */
