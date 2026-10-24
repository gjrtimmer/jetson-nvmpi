/*
 * dec_flush_case.h - hardware case for flush after the first resolution
 * event (#162), included by test_api.c.
 *
 * Decode part of a GOP, so the capture plane is configured, then flush and
 * resume from an IDR of the same stream (same resolution, a seek). Three
 * flushes in a row: resume (1) with the IDR's SPS/PPS, (2) without them (the
 * parameter-set NALs are cut out of the access unit), (3) with them again.
 * Each resumed GOP (8 access units) is followed by the EOS marker and must
 * deliver all 8 frames, each pts exactly once, within the decoder wait
 * timeout. The decoder is expected to raise no new resolution event for a
 * resume at the same size (V4L2 stateful decoder spec): flush re-arms the
 * capture plane itself. Before the fix the frames stop after the flush and
 * put_packet blocks once the OUTPUT buffers are used up; an alarm turns that
 * into a failure instead of a hang.
 *
 * Informational: run with NVMPI_TIMING=1 (and NVMPI_TEST_VERBOSE=1 for the
 * child's stderr) and look for "phase=resolution_event_in_loop": present
 * means the driver announced the stream again after a flush, absent means it
 * stayed silent. Both must pass.
 *
 * Needs a real NVDEC (or the mock). Outside strict mode a missing device or
 * a stream that yields no frame at all is a skip; with NVMPI_TEST_STRICT=1
 * every shortfall is a failure.
 */
#ifndef NVMPI_TEST_DEC_FLUSH_CASE_H
#define NVMPI_TEST_DEC_FLUSH_CASE_H

#include "dec_flush_fixture.h"
#include "../mock/nal_scan.h"

#define DECFLUSH_FAIL(what) do { fprintf(stderr, "dec_flush FAIL %s:%d: %s\n", __func__, __LINE__, (what)); return 1; } while (0)
#define DECFLUSH_ALARM_S 120             /* the whole case; a hang fails it */
#define DECFLUSH_FLUSHES 3

/* One frame through the copy path (320x240 NV12): 0 and its pts, else -1. */
static int decflush_take(nvmpictx *ctx, bool wait, long *pts)
{
    static unsigned char y[DECFLUSH_W * DECFLUSH_H], uv[DECFLUSH_W * DECFLUSH_H / 2];
    nvFrame f;

    memset(&f, 0, sizeof(f));
    f.payload[0] = y;
    f.payload[1] = uv;
    f.linesize[0] = f.linesize[1] = DECFLUSH_W;
    if (nvmpi_decoder_get_frame(ctx, &f, wait) != 0)
        return -1;
    *pts = (long)f.timestamp;
    return 0;
}

/* Is the NAL at nal[0..n) (start code included) a parameter set (H.264
 * SPS/PPS, HEVC VPS/SPS/PPS)? */
static int decflush_is_ps(const unsigned char *nal, size_t n, int hevc)
{
    size_t h = nal[2] == 1 ? 3 : 4;      /* 3- or 4-byte start code */
    unsigned t;

    if (n <= h)
        return 0;
    t = hevc ? (nal[h] >> 1) & 0x3f : nal[h] & 0x1f;
    return hevc ? (t >= 32 && t <= 34) : (t == 7 || t == 8);
}

/* Copy an access unit without its parameter-set NALs. Returns the new
 * length. */
static size_t decflush_strip(unsigned char *dst, const unsigned char *au,
                             size_t n, int hevc)
{
    size_t i, start = 0, o = 0;
    int have = 0;

    for (i = 0; i <= n; i++) {
        int sc = i + 3 <= n && au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1;
        size_t nal = (sc && i > 0 && au[i - 1] == 0) ? i - 1 : i;

        if (!sc && i < n)
            continue;
        if (have && !decflush_is_ps(au + start, nal - start, hevc)) {
            memcpy(dst + o, au + start, nal - start);
            o += nal - start;
        }
        start = nal;
        have = i < n;
    }
    return o;
}

static int decflush_put(nvmpictx *ctx, const unsigned char *p, size_t n, long pts)
{
    nvPacket pkt;

    memset(&pkt, 0, sizeof(pkt));
    pkt.payload = (unsigned char *)p;
    pkt.payload_size = n;
    pkt.pts = (unsigned long)pts;
    return nvmpi_decoder_put_packet(ctx, &pkt);
}

/* Feed GOP `gop` (optionally without parameter sets), EOS, and count the
 * frames until the end of the stream. pts = base + au index + 1. Returns 0
 * when all DECFLUSH_GOP frames came back once each. */
static int decflush_gop(nvmpictx *ctx, const unsigned char *es, const size_t *off,
                        const size_t *len, int gop, int hevc, int headers, long base)
{
    unsigned char buf[8192];
    int seen[DECFLUSH_GOP] = { 0 }, got = 0, i;
    long pts;

    for (i = 0; i < DECFLUSH_GOP; i++) {
        int au = gop * DECFLUSH_GOP + i;
        const unsigned char *p = es + off[au];
        size_t n = len[au];

        if (i == 0 && !headers) {
            if (n > sizeof(buf)) DECFLUSH_FAIL("fixture AU too large");
            n = decflush_strip(buf, p, n, hevc);
            p = buf;
        }
        if (decflush_put(ctx, p, n, base + au + 1) < 0)
            DECFLUSH_FAIL("put_packet after flush");
        while (decflush_take(ctx, false, &pts) == 0) {
            long k = pts - base - gop * DECFLUSH_GOP - 1;
            if (k < 0 || k >= DECFLUSH_GOP || seen[k]) DECFLUSH_FAIL("unexpected or repeated pts");
            seen[k] = 1;
            got++;
        }
    }
    if (decflush_put(ctx, es, 0, 0) < 0)         /* EOS marker */
        DECFLUSH_FAIL("EOS put_packet");
    while (decflush_take(ctx, true, &pts) == 0) {
        long k = pts - base - gop * DECFLUSH_GOP - 1;
        if (k < 0 || k >= DECFLUSH_GOP || seen[k]) DECFLUSH_FAIL("unexpected or repeated pts after EOS");
        seen[k] = 1;
        got++;
    }
    fprintf(stderr, "dec_flush: gop %d headers=%d: %d of %d frames\n", gop, headers,
            got, DECFLUSH_GOP);
    return got == DECFLUSH_GOP ? 0 : 1;
}

static int decflush_run(nvCodingType codec, const unsigned char *es, size_t es_len)
{
    const int hevc = codec == NV_VIDEO_CodingHEVC;
    size_t off[DECFLUSH_N + 1], len[DECFLUSH_N + 1];
    nvDecParam param;
    nvmpictx *ctx;
    long pts;
    int i, got = 0, rc;

    if (nal_split_aus(es, es_len, hevc, off, len, DECFLUSH_N + 1) != DECFLUSH_N)
        DECFLUSH_FAIL("fixture does not split into the expected access units");

#ifdef NVMPI_MOCK_JPEG
    setenv("NVMPI_MOCK_DEC_REORDER", "2", 1);    /* like the stream's DPB */
#endif
    alarm(DECFLUSH_ALARM_S);
    dec_param_init(&param, codec);
    param.pixFormat = NV_PIX_NV12;
    param.frame_pool_size = 4;
    param.wait_timeout = 2000;   /* ms; the first frame pays NVDEC start-up */
    ctx = nvmpi_create_decoder(&param);
    if (!ctx) return strict_mode ? 1 : 77;

    /* Part of GOP 0: configures the capture plane (the first event). */
    for (i = 0; i < 6; i++) {
        if (decflush_put(ctx, es + off[i], len[i], i + 1) < 0) {
            nvmpi_decoder_close(ctx);
            DECFLUSH_FAIL("put_packet before flush");
        }
        while (decflush_take(ctx, false, &pts) == 0)
            got++;
    }
    if (got == 0 && decflush_take(ctx, true, &pts) == 0)
        got++;
    if (got == 0) {
        nvmpi_decoder_close(ctx);
        return strict_mode ? 1 : 77;             /* no decode path here */
    }

    for (i = 1; i <= DECFLUSH_FLUSHES; i++) {
        /* resume GOP 1, 2, 0; parameter sets cut out on the second */
        if (nvmpi_decoder_flush(ctx) != 0) {
            nvmpi_decoder_close(ctx);
            DECFLUSH_FAIL("flush");
        }
        rc = decflush_gop(ctx, es, off, len, i % 3, hevc, i != 2, 1000L * i);
        if (rc) {
            nvmpi_decoder_close(ctx);
            DECFLUSH_FAIL("frames lost after flush");
        }
    }
    if (nvmpi_decoder_close(ctx) != 0)
        DECFLUSH_FAIL("close");
    return 0;
}

static int test_decoder_flush_after_event_h264(void)
{
    return decflush_run(NV_VIDEO_CodingH264, decflush_h264, sizeof(decflush_h264));
}

static int test_decoder_flush_after_event_hevc(void)
{
    return decflush_run(NV_VIDEO_CodingHEVC, decflush_h265, sizeof(decflush_h265));
}

#endif /* NVMPI_TEST_DEC_FLUSH_CASE_H */
