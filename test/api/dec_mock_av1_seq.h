/*
 * dec_mock_av1_seq.h - AV1 decode without an in-band sequence header (#147),
 * cases of test_api_dec_mock.c. Included once from test_api_dec_mock.c after
 * dec_mock_latency.h and dec_mock_av1.h (create helpers, put, REQUIRE, lat_*,
 * the AV1 fixture); each case runs in the forked case child under alarm().
 *
 * Patterns (packet shapes from av1_split.h):
 *   a  noseq_first  TU without a sequence header (TD + key frame) before the
 *                   first TU that has one; the stream must start at the
 *                   full TU and later header-less TUs must decode.
 *   b  seq_only     sequence-header-only packet: no frame by itself, the
 *                   next frame TU decodes.
 *   c  td_only      temporal-delimiter-only packets (before and mid-stream)
 *                   are ignored.
 *   d  never_seq    a stream that never sends a header: no frame, no hang,
 *                   close() (also after an EOS marker) stays within bound.
 *
 * MODEL ASSUMPTION: the mock decoder (mock_dec.c) models "no sequence header
 * yet" as NVDEC most plausibly behaves: no RESOLUTION_CHANGE and no frame
 * until a sequence header OBU arrived, input consumed and dropped, TD-only
 * ignored. The real driver may differ (error, garbage, hold); the test_api
 * case av1_noseq_case.h records what the Orin does. What these cases pin is
 * libnvmpi's side: no crash or hang, put_packet never blocks, frames flow
 * once a header plus key frame arrive, and close() latency.
 *
 * Negative checks poll non-blocking after a short settle instead of waiting
 * out wait_timeout: the model is deterministic, so a late frame can only mean
 * a bug, and the case stays fast under sanitizers.
 */
#ifndef NVMPI_TEST_DEC_MOCK_AV1_SEQ_H
#define NVMPI_TEST_DEC_MOCK_AV1_SEQ_H

#include "av1_obu.h"
#include "av1_split.h"

/* close() after data was fed but no stream event ever came: the capture
 * thread's pre-event slice (50 ms) plus the race-guard dqEvent (500 ms,
 * nvmpi_dec_capture.cpp) is the DESIGNED latency (~0.6 s), so the
 * no-data bound (400 ms) does not apply. 2 s catches a stuck join. */
#define AV1S_CLOSE_MAX_MS 2000
#define AV1S_SETTLE_MS 50

static nvmpictx *av1s_create(void)
{
    nvDecParam p;

    memset(&p, 0, sizeof(p));
    p.codingType = NV_VIDEO_CodingAV1;
    p.pixFormat = NV_PIX_NV12;
    p.frame_pool_size = 4;
    p.wait_timeout = 1000;
    return nvmpi_create_decoder(&p);
}

static int av1s_put(nvmpictx *c, enum av1_pkt k, unsigned long pts)
{
    return put(c, av1_pkt_buf[k], av1_pkt_len[k], pts);
}

/* 0 = a frame was ready (released at once), nonzero = none. Non-blocking. */
static int av1s_poll(nvmpictx *c)
{
    int fd, w, h, pitch, ret;
    int64_t ts;
    nvmpi_frame_release_cb rel = NULL;
    void *opaque = NULL;

    ret = nvmpi_decoder_get_frame_fd(c, &fd, &w, &h, &pitch, &ts, &rel,
                                     &opaque, false);
    if (ret == 0 && rel)
        rel(opaque);
    return ret;
}

/* Put packet k: it must be accepted and must yield no frame. */
static int av1s_expect_none(nvmpictx *c, enum av1_pkt k, unsigned long pts)
{
    if (av1s_put(c, k, pts) != 0)
        return 1;
    usleep(AV1S_SETTLE_MS * 1000);
    return av1s_poll(c) == 0;          /* a frame here is the failure       */
}

/* Put packet k: it must yield one 320x240 frame (blocking get). */
static int av1s_expect_frame(nvmpictx *c, enum av1_pkt k, unsigned long pts)
{
    int w = 0, h = 0;

    if (av1s_put(c, k, pts) != 0)
        return 1;
    return take_fd(c, &w, &h) != 0 || w != AV1_FX_W || h != AV1_FX_H;
}

static int av1s_close(nvmpictx *c)
{
    long ms = lat_close_ms(c);

    fprintf(stderr, "    close %ld ms (bound %d)\n", ms, AV1S_CLOSE_MAX_MS);
    return ms < 0 || ms >= AV1S_CLOSE_MAX_MS;
}

#if NVMPI_HAVE_AV1
#define AV1S_BEGIN(c)                                                     \
    nvmpictx *c;                                                          \
    REQUIRE(av1_pkts_init() == 0);                                        \
    c = av1s_create();                                                    \
    REQUIRE(c)
#else
/* No AV1 fourcc: create returns NULL by design (dec_mock_av1.h asserts it). */
#define AV1S_BEGIN(c) return 0
#endif

static int av1s_noseq_first(void)
{
    AV1S_BEGIN(c);
    /* Two header-less key-frame TUs: accepted, no event, no frame. */
    REQUIRE(av1s_expect_none(c, AV1_PKT_NOSEQ, 10) == 0);
    REQUIRE(av1s_expect_none(c, AV1_PKT_NOSEQ, 11) == 0);
    /* The first TU with a sequence header starts the stream. */
    REQUIRE(av1s_expect_frame(c, AV1_PKT_FULL, 12) == 0);
    /* Container-style stream: the header is not repeated, frames go on. */
    REQUIRE(av1s_expect_frame(c, AV1_PKT_NOSEQ, 13) == 0);
    REQUIRE(av1s_expect_frame(c, AV1_PKT_NOSEQ, 14) == 0);
    REQUIRE(av1s_close(c) == 0);
    return 0;
}

static int av1s_seq_only(void)
{
    AV1S_BEGIN(c);
    REQUIRE(av1s_expect_none(c, AV1_PKT_SEQ, 20) == 0);    /* header alone  */
    REQUIRE(av1s_expect_frame(c, AV1_PKT_NOSEQ, 21) == 0); /* then picture  */
    REQUIRE(av1s_expect_none(c, AV1_PKT_SEQ, 22) == 0);    /* repeated hdr  */
    REQUIRE(av1s_expect_frame(c, AV1_PKT_NOSEQ, 23) == 0);
    REQUIRE(av1s_close(c) == 0);
    return 0;
}

static int av1s_td_only(void)
{
    AV1S_BEGIN(c);
    REQUIRE(av1s_expect_none(c, AV1_PKT_TD, 30) == 0);
    REQUIRE(av1s_expect_none(c, AV1_PKT_TD, 31) == 0);
    REQUIRE(av1s_expect_frame(c, AV1_PKT_FULL, 32) == 0);
    REQUIRE(av1s_expect_none(c, AV1_PKT_TD, 33) == 0);     /* mid-stream    */
    REQUIRE(av1s_expect_frame(c, AV1_PKT_NOSEQ, 34) == 0);
    REQUIRE(av1s_close(c) == 0);
    return 0;
}

/* Never a sequence header: headless TUs plus TD-only, then close; and the
 * same ended by an EOS marker (empty payload). More packets than the 10
 * OUTPUT buffers would prove put_packet never blocks on a decoder that
 * drops them; 12 covers that. */
static int av1s_never_seq(void)
{
    uint8_t eos = 0;
    int i;

    {
        AV1S_BEGIN(c);
        for (i = 0; i < 12; i++)
            REQUIRE(av1s_expect_none(c, i % 4 ? AV1_PKT_NOSEQ : AV1_PKT_TD,
                                     40 + i) == 0);
        REQUIRE(av1s_close(c) == 0);
    }
    {
        AV1S_BEGIN(c);
        REQUIRE(av1s_expect_none(c, AV1_PKT_NOSEQ, 60) == 0);
        REQUIRE(put(c, &eos, 0, 0) == 0);          /* EOS marker            */
        REQUIRE(av1s_poll(c) != 0);                /* bounded, no frame     */
        REQUIRE(av1s_close(c) == 0);
    }
    return 0;
}

#endif /* NVMPI_TEST_DEC_MOCK_AV1_SEQ_H */
