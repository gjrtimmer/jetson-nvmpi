/*
 * dec_mock_reschange.h - resolution change while the frame pool is full
 * (issue #143), cases of test_api_dec_mock.c. Included once from
 * test_api_dec_mock.c after the helpers (create, put, take_fd, REQUIRE, fx_*).
 *
 * Scenario (single caller thread, like jetson-ffmpeg's nvmpi_decode): the
 * caller queues a burst of packets and only then drains. The old-size frames
 * fill the pool, the capture thread sits in back-pressure holding one more
 * old-size CAPTURE buffer, and the decoder (mock: like the driver) announces
 * the new size and holds every new-size AU until the CAPTURE queue is torn
 * down and re-armed. Pre-fix the capture thread never looked at the event
 * while back-pressured, so OUTPUT never freed and put_packet blocked forever
 * in output_plane.dqBuffer(-1). Each case runs under the case alarm: a
 * hang is a failure.
 *
 * Packet budget (mock: 10 OUTPUT buffers, used by index for the first 10
 * puts, afterwards put_packet must dequeue a consumed one):
 *   old frames  = RC_OLD_AUS: fill the pool + the held buffer, so the
 *                 capture thread is back-pressured when the event arrives;
 *   new AUs     = RC_NEW_AUS: held by the decoder until the change is
 *                 serviced; together with the old ones they exceed the 10
 *                 OUTPUT buffers, so a put in the middle of the new burst
 *                 can only return once the change was serviced.
 * Old-size frames that were never drained are discarded at the change (the
 * pool generation is retired, #137) unless the consumer wins the race for
 * them; the new-size frames must all arrive.
 */
#ifndef NVMPI_TEST_DEC_MOCK_RESCHANGE_H
#define NVMPI_TEST_DEC_MOCK_RESCHANGE_H

#define RC_OLD_W 320
#define RC_OLD_H 240
#define RC_NEW_W 640
#define RC_NEW_H 360
#define RC_OLD_AUS 6
#define RC_NEW_AUS 11
#define RC_SETTLE_MS 400   /* capture thread reaches back-pressure */

/* Queue RC_OLD_AUS old-size AUs, pause, then RC_NEW_AUS new-size ones,
 * draining nothing. All puts must succeed (they hang pre-fix). */
static int rc_put_burst(nvmpictx *c)
{
    uint8_t au[4096];
    int i;

    REQUIRE(put(c, au, fx_idr(au, RC_OLD_W, RC_OLD_H), 1) == 0);
    for (i = 1; i < RC_OLD_AUS; i++)
        REQUIRE(put(c, au, fx_pslice(au), 1 + i) == 0);
    /* Let the capture thread run into back-pressure first: without the
     * pause it services the event before it has dequeued any old frame and
     * the pool never fills (the case would pass vacuously). 4 pool slots
     * take 4 frames, a 5th CAPTURE buffer is held, a 6th stays queued. */
    usleep(RC_SETTLE_MS * 1000);
    REQUIRE(put(c, au, fx_idr(au, RC_NEW_W, RC_NEW_H), 100) == 0);
    for (i = 1; i < RC_NEW_AUS; i++)
        REQUIRE(put(c, au, fx_pslice(au), 100 + i) == 0);
    return 0;
}

/* Drain after the puts returned: every new-size frame must arrive. Old-size
 * frames may precede them but are not required: the change unblocks the
 * caller (CAPTURE STREAMOFF) a moment before the old pool generation is
 * retired, so a fast consumer can still take a not yet discarded old-size
 * frame. An old-size frame after a new-size one would be a bug. */
static int rc_drain_new(nvmpictx *c)
{
    int new_got = 0, w = 0, h = 0;

    while (new_got < RC_NEW_AUS && take_fd(c, &w, &h) == 0) {
        if (w == RC_NEW_W && h == RC_NEW_H) {
            new_got++;
            continue;
        }
        REQUIRE(new_got == 0 && w == RC_OLD_W && h == RC_OLD_H);
    }
    fprintf(stderr, "    new-size frames %d/%d\n", new_got, RC_NEW_AUS);
    return new_got == RC_NEW_AUS ? 0 : 1;
}

/* Default pool (4): the capture thread is back-pressured at the change. */
static int res_change_backpressure(void)
{
    nvmpictx *c = create(0, 0, 4);

    REQUIRE(c);
    REQUIRE(rc_put_burst(c) == 0);
    REQUIRE(rc_drain_new(c) == 0);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* Smallest pool: the held buffer plus one slot are all the old size has. */
static int res_change_backpressure_pool1(void)
{
    nvmpictx *c = create(0, 0, 1);

    REQUIRE(c);
    REQUIRE(rc_put_burst(c) == 0);
    REQUIRE(rc_drain_new(c) == 0);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* Same burst, then close() without ever draining: teardown must not wait
 * for the consumer (thread-before-resource order stays intact). */
static int res_change_backpressure_close(void)
{
    nvmpictx *c = create(0, 0, 4);

    REQUIRE(c);
    REQUIRE(rc_put_burst(c) == 0);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

#endif /* NVMPI_TEST_DEC_MOCK_RESCHANGE_H */
