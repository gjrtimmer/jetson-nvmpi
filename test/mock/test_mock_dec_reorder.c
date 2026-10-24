/*
 * test_mock_dec_reorder.c - reorder delay of the decoder model
 * (NVMPI_MOCK_DEC_REORDER, #161), driven through the V4L2 API.
 * Part of the test_mock executable; a failure here is a mock bug.
 *
 * Covers: a VCL AU only yields a frame once N are held back (oldest first,
 * timestamps in input order); EOS stays queued until every held frame was
 * produced, LAST goes on the final one only, the empty queue then reports
 * EAGAIN + LAST; the drain waits for free CAPTURE buffers; default 0 keeps
 * the one-frame-per-AU behaviour.
 */
#include "mock_dec_harness.h"

#include <stdlib.h>

static uint8_t au[2048];

/* Dequeue one frame (waits up to 2 s); 0 on success. Reports pts (us) and
 * flags. The buffer is NOT re-queued: the caller decides. */
static int take(struct dh *d, uint64_t *pts_us, uint32_t *flags, uint32_t *idx)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];
    uint64_t t0 = dh_now_ms();

    while (dh_dq(d, CAP, &b, pl) != 0) {
        if (dh_now_ms() - t0 > 2000)
            return -1;
        dh_ms(1);
    }
    *pts_us = (uint64_t)b.timestamp.tv_sec * 1000000 + (uint64_t)b.timestamp.tv_usec;
    *flags = b.flags;
    *idx = b.index;
    return 0;
}

/* Is the CAPTURE queue empty right now (EAGAIN)? flags = what DQBUF left. */
static int empty(struct dh *d, uint32_t *flags)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];

    if (dh_dq(d, CAP, &b, pl) == 0)
        return 0;
    *flags = b.flags;
    return errno == EAGAIN;
}

/* n_p P slices with pts 2000 + first, 2001 + first, ... (us); the IDR that
 * dh_start feeds has pts 1000. */
static void feed(struct dh *d, unsigned first, unsigned n_p)
{
    unsigned i;

    for (i = first; i < first + n_p; i++) {
        CHECK(dh_put(d, au, fx_pslice(au), 2000 + i) == 0);
        dh_ms(5);
    }
}

static void test_reorder_fifo(void)
{
    struct dh d;
    uint64_t pts;
    uint32_t fl, idx, i;
    static const uint64_t want[6] = { 1000, 2000, 2001, 2002, 2003, 2004 };

    setenv("NVMPI_MOCK_DEC_REORDER", "3", 1);
    CHECK(dh_start(&d, 320, 240, 6, O_NONBLOCK) == 0);   /* 1 IDR: held   */
    dh_ms(20);
    CHECK(empty(&d, &fl) && !(fl & V4L2_BUF_FLAG_LAST));  /* nothing yet   */
    feed(&d, 0, 2);                                       /* 3 held        */
    dh_ms(20);
    CHECK(empty(&d, &fl));
    feed(&d, 2, 3);                                       /* 6 AUs: 3 out  */
    for (i = 0; i < 3; i++) {
        CHECK(take(&d, &pts, &fl, &idx) == 0);
        CHECK(pts == want[i] && !(fl & V4L2_BUF_FLAG_LAST));
        CHECK(dh_is_gray(&d, idx));
    }
    CHECK(empty(&d, &fl) && !(fl & V4L2_BUF_FLAG_LAST));  /* 3 still held  */
    CHECK(dh_put(&d, au, 0, 0) == 0);                     /* EOS drains    */
    for (i = 3; i < 6; i++) {
        CHECK(take(&d, &pts, &fl, &idx) == 0);
        CHECK(pts == want[i]);
        CHECK(((fl & V4L2_BUF_FLAG_LAST) != 0) == (i == 5)); /* LAST: final */
    }
    CHECK(empty(&d, &fl) && (fl & V4L2_BUF_FLAG_LAST));   /* drained       */
    dh_close(&d);
    unsetenv("NVMPI_MOCK_DEC_REORDER");
}

/* The drain needs CAPTURE buffers: with only 2 armed, the third held frame
 * is produced after the application re-queued one. */
static void test_reorder_drain_waits_for_buffers(void)
{
    struct dh d;
    uint64_t pts;
    uint32_t fl, idx, first, second;

    setenv("NVMPI_MOCK_DEC_REORDER", "3", 1);
    CHECK(dh_start(&d, 320, 240, 2, O_NONBLOCK) == 0);
    feed(&d, 0, 3);                                       /* 4 AUs: 1 out  */
    CHECK(take(&d, &pts, &fl, &idx) == 0 && pts == 1000);
    CHECK(dh_cap_q(&d, idx) == 0);                        /* 2 free again  */
    CHECK(dh_put(&d, au, 0, 0) == 0);                     /* EOS, 3 held   */
    CHECK(take(&d, &pts, &fl, &first) == 0 && pts == 2000);
    CHECK(!(fl & V4L2_BUF_FLAG_LAST));
    CHECK(take(&d, &pts, &fl, &second) == 0 && pts == 2001);
    CHECK(!(fl & V4L2_BUF_FLAG_LAST));
    dh_ms(20);
    CHECK(empty(&d, &fl) && !(fl & V4L2_BUF_FLAG_LAST));  /* 1 still held  */
    CHECK(dh_cap_q(&d, first) == 0);                      /* frees a buffer */
    CHECK(take(&d, &pts, &fl, &idx) == 0 && pts == 2002);
    CHECK(fl & V4L2_BUF_FLAG_LAST);
    dh_close(&d);
    unsetenv("NVMPI_MOCK_DEC_REORDER");
}

/* Default (0): one frame per AU at once, as before. */
static void test_reorder_default_off(void)
{
    struct dh d;
    uint64_t pts;
    uint32_t fl, idx;

    unsetenv("NVMPI_MOCK_DEC_REORDER");
    CHECK(dh_start(&d, 320, 240, 4, O_NONBLOCK) == 0);
    CHECK(take(&d, &pts, &fl, &idx) == 0 && pts == 1000);
    dh_close(&d);
}

void test_decoder_reorder(void)
{
    test_reorder_default_off();
    test_reorder_fifo();
    test_reorder_drain_waits_for_buffers();
}
