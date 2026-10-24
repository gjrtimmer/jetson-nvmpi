/*
 * dec_mock_eos.h - decoder EOS drain cases of test_api_dec_mock.c
 * (#161). Included once from test_api_dec_mock.c after the
 * helpers (create(), put(), REQUIRE, fx_*).
 *
 * A real decoder keeps the last frames (reorder / DPB depth) until the EOS
 * marker flushes them out. The mock models that with
 * NVMPI_MOCK_DEC_REORDER=N: the last N frames are only produced after EOS.
 * Before the fix the capture thread stopped at the EOS marker and those
 * frames were lost; every case here counts the frames after EOS and
 * requires all of them. Each case sets the variable in its forked child
 * before create() (the mock reads it when the device is opened).
 */
#ifndef NVMPI_TEST_DEC_MOCK_EOS_H
#define NVMPI_TEST_DEC_MOCK_EOS_H

#define EOS_REORDER "3"
#define EOS_N 30                         /* access units per stream        */
#define EOS_W 320u
#define EOS_H 240u
#define EOS_CLOSE_MAX_MS 3000            /* close during a drain: prompt   */

/* pts of AU i: 1000 * (i + 1), strictly increasing like decode order. */
#define EOS_PTS(i) (1000L * ((i) + 1))

static nvmpictx *eos_create(nvCodingType codec, int pool)
{
    nvDecParam p;

    memset(&p, 0, sizeof(p));
    p.codingType = codec;
    p.pixFormat = NV_PIX_NV12;
    p.frame_pool_size = pool;
    p.wait_timeout = 1000;
    return nvmpi_create_decoder(&p);
}

/* HEVC access units for the mock: it only reads NAL types (IDR_W_RADL = 19
 * for the first AU, TRAIL_R = 1 after), so a header plus filler is enough.
 * Two-byte NAL header: type << 1, then layer 0 / temporal id 1. */
static size_t eos_hevc_au(uint8_t *out, int idr)
{
    size_t i;

    memcpy(out, "\0\0\0\1", 4);
    out[4] = (uint8_t)((idr ? 19 : 1) << 1);
    out[5] = 1;
    for (i = 0; i < 24; i++)
        out[6 + i] = (uint8_t)(0x11 + i);       /* never 00 00 */
    return 30;
}

static size_t eos_au(uint8_t *out, nvCodingType codec, int i)
{
    if (codec == NV_VIDEO_CodingHEVC)
        return eos_hevc_au(out, i == 0);
    return i == 0 ? fx_idr(out, EOS_W, EOS_H) : fx_pslice(out);
}

/* One frame through the copy path: 0 and its pts, else -1. */
static int eos_take(nvmpictx *c, bool wait, long *pts)
{
    static unsigned char y[EOS_W * EOS_H], uv[EOS_W * EOS_H / 2];
    nvFrame f;

    memset(&f, 0, sizeof(f));
    f.payload[0] = y;
    f.payload[1] = uv;
    f.linesize[0] = f.linesize[1] = EOS_W;
    if (nvmpi_decoder_get_frame(c, &f, wait) != 0)
        return -1;
    *pts = (long)f.timestamp;
    return 0;
}

/* Same through the zero-copy path; the frame is released at once. */
static int eos_take_fd(nvmpictx *c, bool wait, long *pts)
{
    int fd, w, h, pitch;
    int64_t ts = 0;
    nvmpi_frame_release_cb rel = NULL;
    void *opaque = NULL;

    if (nvmpi_decoder_get_frame_fd(c, &fd, &w, &h, &pitch, &ts, &rel, &opaque,
                                   wait) != 0)
        return -1;
    if (rel)
        rel(opaque);
    *pts = (long)ts;
    return 0;
}

/* Feed n AUs (non-blocking take after each, like a pipelined caller), send
 * EOS, then take until the end of the stream. Returns the frames received;
 * *ordered is 1 when every pts matched EOS_PTS(index) (no gap, no repeat). */
static int eos_run(nvmpictx *c, nvCodingType codec, int fd_path, int n,
                   int *ordered)
{
    uint8_t au[4096], eos = 0;
    int i, got = 0;
    long pts;

    *ordered = 1;
    for (i = 0; i < n; i++) {
        REQUIRE(put(c, au, eos_au(au, codec, i), (unsigned long)EOS_PTS(i)) == 0);
        while ((fd_path ? eos_take_fd(c, false, &pts)
                        : eos_take(c, false, &pts)) == 0) {
            if (pts != EOS_PTS(got))
                *ordered = 0;
            got++;
        }
    }
    REQUIRE(put(c, &eos, 0, 0) == 0);            /* EOS marker             */
    while ((fd_path ? eos_take_fd(c, true, &pts)
                    : eos_take(c, true, &pts)) == 0) {
        if (pts != EOS_PTS(got))
            *ordered = 0;
        got++;
    }
    return got;
}

static int eos_case(nvCodingType codec, int fd_path)
{
    nvmpictx *c;
    int got, ordered;

    setenv("NVMPI_MOCK_DEC_REORDER", EOS_REORDER, 1);
    c = eos_create(codec, 4);
    REQUIRE(c);
    got = eos_run(c, codec, fd_path, EOS_N, &ordered);
    fprintf(stderr, "eos drain: %d of %d frames, ordered=%d\n", got, EOS_N,
            ordered);
    REQUIRE(got == EOS_N);                       /* none lost at EOS       */
    REQUIRE(ordered);
    REQUIRE(eos_take(c, true, &(long){ 0 }) != 0);   /* then end of stream */
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

static int eos_drain_reorder(void) { return eos_case(NV_VIDEO_CodingH264, 0); }
static int eos_drain_fd(void) { return eos_case(NV_VIDEO_CodingH264, 1); }
static int eos_drain_hevc(void) { return eos_case(NV_VIDEO_CodingHEVC, 0); }

/* A stream shorter than the reorder delay: every frame is held at EOS. */
static int eos_drain_short(void)
{
    nvmpictx *c;
    int got, ordered;

    setenv("NVMPI_MOCK_DEC_REORDER", EOS_REORDER, 1);
    c = eos_create(NV_VIDEO_CodingH264, 4);
    REQUIRE(c);
    got = eos_run(c, NV_VIDEO_CodingH264, 0, 2, &ordered);
    REQUIRE(got == 2 && ordered);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* No reorder delay (default): EOS still ends the stream with all frames. */
static int eos_drain_noreorder(void)
{
    nvmpictx *c;
    int got, ordered;

    unsetenv("NVMPI_MOCK_DEC_REORDER");
    c = eos_create(NV_VIDEO_CodingH264, 4);
    REQUIRE(c);
    got = eos_run(c, NV_VIDEO_CodingH264, 0, EOS_N, &ordered);
    REQUIRE(got == EOS_N && ordered);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* Flush after a completed drain clears the end-of-input state: the next
 * stream decodes and drains completely again. */
static int eos_drain_after_flush(void)
{
    nvmpictx *c;
    int got, ordered;

    setenv("NVMPI_MOCK_DEC_REORDER", EOS_REORDER, 1);
    c = eos_create(NV_VIDEO_CodingH264, 4);
    REQUIRE(c);
    got = eos_run(c, NV_VIDEO_CodingH264, 0, 10, &ordered);
    REQUIRE(got == 10 && ordered);
    REQUIRE(nvmpi_decoder_flush(c) == 0);
    got = eos_run(c, NV_VIDEO_CodingH264, 0, 10, &ordered);
    REQUIRE(got == 10 && ordered);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* Close right after EOS, frames still held in the decoder and none taken:
 * close sets shutdown and joins the capture thread promptly (no waiting for
 * a LAST buffer that may never be consumed) and leaks nothing. */
static int eos_close_during_drain(void)
{
    uint8_t au[4096], eos = 0;
    nvmpictx *c;
    int i;
    long t0;

    setenv("NVMPI_MOCK_DEC_REORDER", EOS_REORDER, 1);
    c = eos_create(NV_VIDEO_CodingH264, 4);
    REQUIRE(c);
    for (i = 0; i < 8; i++)
        REQUIRE(put(c, au, eos_au(au, NV_VIDEO_CodingH264, i),
                    (unsigned long)EOS_PTS(i)) == 0);
    REQUIRE(put(c, &eos, 0, 0) == 0);
    t0 = fdb_ms();
    REQUIRE(nvmpi_decoder_close(c) == 0);
    REQUIRE(fdb_ms() - t0 < EOS_CLOSE_MAX_MS);
    return 0;
}

#endif /* NVMPI_TEST_DEC_MOCK_EOS_H */
