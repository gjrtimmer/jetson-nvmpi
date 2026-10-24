/*
 * dec_eos_case.h - hardware case for the decoder EOS drain (#161), included
 * by test_api.c.
 *
 * A 24-frame stream with B-frames (reorder depth 2, dec_eos_fixture.h) is
 * fed access unit by access unit, then the zero-sized EOS packet is sent.
 * The decoder keeps the last frames until EOS flushes them, so a capture
 * thread that stops at the EOS marker (the bug) delivers fewer than 24
 * (H.264 296/300, HEVC 298/300 on a 300-frame clip). Expected on the Orin:
 *   - every one of the 24 frames comes back, each presentation index exactly
 *     once (pts 1..24);
 *   - in ascending order (hardware outputs in display order; the mock keeps
 *     decode order, so the order is only checked off the mock);
 *   - then get_frame reports the end of the stream (-1) without a hang.
 * Needs a real NVDEC (or the mock). Outside strict mode a missing device or
 * a stream that yields no frame at all is a skip; with NVMPI_TEST_STRICT=1
 * every shortfall is a failure.
 */
#ifndef NVMPI_TEST_DEC_EOS_CASE_H
#define NVMPI_TEST_DEC_EOS_CASE_H

#include "dec_eos_fixture.h"
#include "../mock/nal_scan.h"

#define DECEOS_FAIL(what) do { fprintf(stderr, "dec_eos FAIL %s:%d: %s\n", __func__, __LINE__, (what)); return 1; } while (0)

/* One frame through the copy path (320x240 NV12): 0 and its pts, else -1. */
static int deceos_take(nvmpictx *ctx, bool wait, int *pts)
{
    static unsigned char y[DECEOS_W * DECEOS_H], uv[DECEOS_W * DECEOS_H / 2];
    nvFrame f;

    memset(&f, 0, sizeof(f));
    f.payload[0] = y;
    f.payload[1] = uv;
    f.linesize[0] = f.linesize[1] = DECEOS_W;
    if (nvmpi_decoder_get_frame(ctx, &f, wait) != 0)
        return -1;
    *pts = (int)f.timestamp;
    return 0;
}

/* Record one received pts; 0 ok, 1 when out of range or a repeat. */
static int deceos_note(int pts, int *seen, int *last, int *ordered, int *got)
{
    if (pts < 1 || pts > DECEOS_N || seen[pts - 1])
        return 1;
    seen[pts - 1] = 1;
    if (pts < *last)
        *ordered = 0;
    *last = pts;
    (*got)++;
    return 0;
}

static int deceos_run(nvCodingType codec, const unsigned char *es, size_t es_len,
                      const int *pts_tab)
{
    size_t off[DECEOS_N + 1], len[DECEOS_N + 1];
    int seen[DECEOS_N] = { 0 };
    int got = 0, last = 0, ordered = 1, pts, i;
    nvDecParam param;
    nvPacket pkt;
    nvmpictx *ctx;
    unsigned n_au;

    n_au = nal_split_aus(es, es_len, codec == NV_VIDEO_CodingHEVC, off, len,
                         DECEOS_N + 1);
    if (n_au != DECEOS_N)
        DECEOS_FAIL("fixture does not split into the expected access units");

#ifdef NVMPI_MOCK_JPEG
    /* The mock holds back 2 frames like the stream's reorder depth. */
    setenv("NVMPI_MOCK_DEC_REORDER", "2", 1);
#endif
    dec_param_init(&param, codec);
    param.pixFormat = NV_PIX_NV12;
    param.frame_pool_size = 4;
    param.wait_timeout = 2000;   /* ms; the first frame pays NVDEC start-up */
    ctx = nvmpi_create_decoder(&param);
    if (!ctx) return strict_mode ? 1 : 77;

    for (i = 0; i < DECEOS_N; i++) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.payload = (unsigned char *)es + off[i];
        pkt.payload_size = len[i];
        pkt.pts = (unsigned long)pts_tab[i];
        if (nvmpi_decoder_put_packet(ctx, &pkt) < 0) {
            nvmpi_decoder_close(ctx);
            DECEOS_FAIL("put_packet");
        }
        /* Pipelined caller: take what is ready, never wait. */
        while (deceos_take(ctx, false, &pts) == 0)
            if (deceos_note(pts, seen, &last, &ordered, &got)) {
                nvmpi_decoder_close(ctx);
                DECEOS_FAIL("unexpected or repeated pts");
            }
    }

    memset(&pkt, 0, sizeof(pkt));        /* EOS marker: payload_size 0 */
    if (nvmpi_decoder_put_packet(ctx, &pkt) < 0) {
        nvmpi_decoder_close(ctx);
        DECEOS_FAIL("EOS put_packet");
    }
    while (deceos_take(ctx, true, &pts) == 0)
        if (deceos_note(pts, seen, &last, &ordered, &got)) {
            nvmpi_decoder_close(ctx);
            DECEOS_FAIL("unexpected or repeated pts after EOS");
        }

    fprintf(stderr, "dec_eos[%s]: %d of %d frames, ordered=%d\n",
            codec == NV_VIDEO_CodingHEVC ? "hevc" : "h264", got, DECEOS_N,
            ordered);
    if (got == 0 && !strict_mode) {
        nvmpi_decoder_close(ctx);
        return 77;                       /* no decode path here */
    }
    if (nvmpi_decoder_close(ctx) != 0)
        DECEOS_FAIL("close");
    if (got != DECEOS_N)
        DECEOS_FAIL("frames lost at EOS (decoder not drained)");
#ifndef NVMPI_MOCK_JPEG
    if (!ordered)
        DECEOS_FAIL("frames out of display order");
#endif
    return 0;
}

static int test_decoder_eos_drain_h264(void)
{
    return deceos_run(NV_VIDEO_CodingH264, deceos_h264, sizeof(deceos_h264),
                      deceos_pts_h264);
}

static int test_decoder_eos_drain_hevc(void)
{
    return deceos_run(NV_VIDEO_CodingHEVC, deceos_h265, sizeof(deceos_h265),
                      deceos_pts_h265);
}

#endif /* NVMPI_TEST_DEC_EOS_CASE_H */
