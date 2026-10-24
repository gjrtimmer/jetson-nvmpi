/*
 * dec_mock_flush.h - flush after the first resolution event (#162), cases
 * of test_api_dec_mock.c. Included once after the helpers (create(), put(),
 * take(), REQUIRE, fx_*).
 *
 * A stateful decoder raises RESOLUTION_CHANGE only when the stream format
 * changes. libnvmpi's flush() tears the CAPTURE plane down, so a resume at
 * the SAME resolution (seek to another IDR) gets no new event: the CAPTURE
 * queue must be re-armed by flush itself, or the capture thread waits for
 * an event that never comes and put_packet blocks once the OUTPUT buffers
 * are used up. The mock follows the spec with NVMPI_MOCK_DEC_REANNOUNCE=0
 * (no new event after an OUTPUT flush); =1 keeps the old model that
 * re-announces, which must keep working.
 *
 * Every case counts frames per round; a missing frame fails fast (the take
 * times out) and a blocked put_packet is caught by the case alarm.
 */
#ifndef NVMPI_TEST_DEC_MOCK_FLUSH_H
#define NVMPI_TEST_DEC_MOCK_FLUSH_H

#define FL_W 320u
#define FL_H 240u
#define FL_N 6                           /* access units per round         */
#define FL_ROUNDS 3                      /* flushes in a row               */
#define FL_ALARM_S 20                    /* bound: a hang fails the case   */

/* One round: IDR (with or without SPS/PPS) + FL_N-1 P frames in lockstep;
 * returns the frames received. */
static int fl_round(nvmpictx *c, int headers, int round)
{
    uint8_t au[4096];
    int i, got = 0, gray;

    for (i = 0; i < FL_N; i++) {
        size_t len = i ? fx_pslice(au)
                       : headers ? fx_idr(au, FL_W, FL_H)
                                 : fx_nal(au, 0x65, 24);   /* bare IDR */
        long pts = 1000L * (round * FL_N + i + 1);

        REQUIRE(put(c, au, len, (unsigned long)pts) == 0);
        gray = 1;
        if (take(c, FL_W, FL_H, pts, &gray) == 0) {
            got++;
            REQUIRE(gray);
        }
    }
    return got;
}

/* First round with headers (the event), then FL_ROUNDS flushes, each
 * followed by a round that resumes from an IDR. `hdr_mask` bit r selects
 * headers for the resume after flush r. */
static int fl_case(const char *reannounce, unsigned hdr_mask)
{
    nvmpictx *c;
    int r, got;

    setenv("NVMPI_MOCK_DEC_REANNOUNCE", reannounce, 1);
    alarm(FL_ALARM_S);
    c = create(0, 0, 4);
    REQUIRE(c);
    got = fl_round(c, 1, 0);
    REQUIRE(got == FL_N);
    for (r = 1; r <= FL_ROUNDS; r++) {
        REQUIRE(nvmpi_decoder_flush(c) == 0);
        got = fl_round(c, (int)(hdr_mask >> (r - 1) & 1), r);
        fprintf(stderr, "flush %d: %d of %d frames\n", r, got, FL_N);
        REQUIRE(got == FL_N);
    }
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

static int flush_event_hdr(void) { return fl_case("0", 7); }
static int flush_event_nohdr(void) { return fl_case("0", 0); }
static int flush_event_mixed(void) { return fl_case("0", 2); }
static int flush_event_reannounce(void) { return fl_case("1", 7); }

/* A real size change after a flush is still handled by the main loop. */
static int flush_event_then_change(void)
{
    uint8_t au[4096];
    nvmpictx *c;
    int w = 0, h = 0, got;

    setenv("NVMPI_MOCK_DEC_REANNOUNCE", "0", 1);
    alarm(FL_ALARM_S);
    c = create(0, 0, 4);
    REQUIRE(c);
    REQUIRE(fl_round(c, 1, 0) == FL_N);
    REQUIRE(nvmpi_decoder_flush(c) == 0);
    REQUIRE(fl_round(c, 0, 1) == FL_N);          /* same size: no event   */
    REQUIRE(put(c, au, fx_idr(au, 640, 360), 99000) == 0);
    for (got = 0; got < 1 && take_fd(c, &w, &h) == 0;)
        got++;
    REQUIRE(got == 1 && w == 640 && h == 360);   /* new size delivered    */
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* Flush right after the first event, nothing taken, is still prompt. */
static int flush_event_prompt(void)
{
    uint8_t au[4096];
    nvmpictx *c;
    long t0;

    setenv("NVMPI_MOCK_DEC_REANNOUNCE", "0", 1);
    alarm(FL_ALARM_S);
    c = create(0, 0, 4);
    REQUIRE(c);
    REQUIRE(put(c, au, fx_idr(au, FL_W, FL_H), 1000) == 0);
    usleep(100 * 1000);
    t0 = fdb_ms();
    REQUIRE(nvmpi_decoder_flush(c) == 0);
    REQUIRE(fdb_ms() - t0 < 2000);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

#endif /* NVMPI_TEST_DEC_MOCK_FLUSH_H */
