/*
 * av1_noseq_case.h - hardware case for AV1 decode of streams without an
 * in-band sequence header (#147), included by test_api.c after av1_case.h.
 *
 * Feeds four packet patterns to a real NVDEC at 320x240 (the av1_case.h
 * fixture, cut into shapes by av1_split.h) and PRINTS what the driver does,
 * one line per pattern, so the MR job log records the behaviour:
 *
 *   av1_noseq probe: pattern=a frames=N err=X close_ms=Y pre=P post=Q
 *
 *   pattern  packets (| = from here on the stream is well formed)
 *   a        TD+frame (no seq hdr) | full TU, TD+frame
 *   b        seq hdr only          | TD+frame
 *   c        TD only               | full TU
 *   d        12 packets (TD+frame, TD+frame, TD only, repeating; never a
 *            header), then the EOS marker. 12 > the 10 OUTPUT buffers: if
 *            NVDEC HELD header-less input instead of consuming it,
 *            put_packet would block in output_plane.dqBuffer(-1)
 *   frames   total frames received      err    first put_packet error (0 none)
 *   pre      frames received for the header-less part (informational)
 *   post     frames received once a header + key frame were fed
 *   close_ms nvmpi_decoder_close() latency
 *
 * The frame counts are INFORMATIONAL: until hardware data exists we do not
 * know whether NVDEC errors, waits or emits garbage for these inputs. The
 * case fails only on what libnvmpi must always guarantee: no crash (the
 * harness sees the signal), no hang, a clean close() that returns 0 within
 * the bound. Hang detection: every blocking step (put, get, close) arms
 * alarm(NOSEQ_STEP_S); the SIGALRM handler prints which step of which
 * pattern blocked (plus a progress line is printed before each put) and
 * exits 1, so a held OUTPUT buffer shows as a clear FAIL with the index of
 * the blocked put_packet, not an anonymous crash. Needs NVDEC with AV1 (Orin);
 * skipped outside strict mode when create fails, like test_decoder_av1_frame.
 * Against the mock (api_mock ctest) the same code runs on the mock decoder
 * model documented in mock/mock_dec.c.
 */
#ifndef NVMPI_TEST_AV1_NOSEQ_CASE_H
#define NVMPI_TEST_AV1_NOSEQ_CASE_H

#include "av1_obu.h"
#include "av1_split.h"

#include <time.h>

#define NOSEQ_STEP_S 30          /* one put/get/close: a hang fails the case */
#define NOSEQ_CLOSE_MAX_MS 5000  /* close() bound (designed ~0.6 s + driver) */
#define NOSEQ_MAX_PKTS 12

struct noseq_pattern {
    char id;
    enum av1_pkt pkt[NOSEQ_MAX_PKTS];
    int n;                       /* packets fed                            */
    int npre;                    /* leading packets of the header-less part */
    int eos;                     /* send the EOS marker after the packets  */
    int wait_ms;                 /* decoder wait_timeout (pre polls block) */
};

static const struct noseq_pattern noseq_patterns[] = {
    { 'a', { AV1_PKT_NOSEQ, AV1_PKT_FULL, AV1_PKT_NOSEQ }, 3, 1, 0, 2000 },
    { 'b', { AV1_PKT_SEQ, AV1_PKT_NOSEQ }, 2, 1, 0, 2000 },
    { 'c', { AV1_PKT_TD, AV1_PKT_FULL }, 2, 1, 0, 2000 },
    /* d: shorter wait, 12 blocking polls at 2 s would only slow the job */
    { 'd', { AV1_PKT_NOSEQ, AV1_PKT_NOSEQ, AV1_PKT_TD, AV1_PKT_NOSEQ,
             AV1_PKT_NOSEQ, AV1_PKT_TD, AV1_PKT_NOSEQ, AV1_PKT_NOSEQ,
             AV1_PKT_TD, AV1_PKT_NOSEQ, AV1_PKT_NOSEQ, AV1_PKT_TD },
      12, 12, 1, 300 },
};

/* Progress for the SIGALRM handler. Written by the main flow, read in the
 * handler: volatile, plain ints (single thread, handler only reads). */
static volatile char noseq_pat;
static volatile int noseq_idx;           /* put index, -1 outside a put    */
static const char *volatile noseq_stage; /* "put", "get", "eos", "close"   */

/* A blocked step: say which one, then fail. Only async-signal-safe calls
 * (write, _exit); the index is formatted by hand. */
static void noseq_on_alarm(int sig)
{
    char b[96];
    size_t n = 0;
    int idx = noseq_idx;
    const char *p = "\nav1_noseq probe: pattern=";
    const char *st = noseq_stage;

    (void)sig;
    while (*p)
        b[n++] = *p++;
    b[n++] = noseq_pat;
    p = " BLOCKED stage=";
    while (*p)
        b[n++] = *p++;
    while (st && *st && n < 70)
        b[n++] = *st++;
    p = " put_index=";
    while (*p)
        b[n++] = *p++;
    if (idx >= 10)
        b[n++] = (char)('0' + idx / 10 % 10);
    b[n++] = (char)('0' + (idx < 0 ? 0 : idx % 10));
    b[n++] = '\n';
    (void)!write(1, b, n);
    _exit(1);
}

static long noseq_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* One blocking poll (decoder wait_timeout) for a frame. 1 = got one. */
static int noseq_take(nvmpictx *ctx)
{
    int fd = -1, w = 0, h = 0, pitch = 0;
    int64_t ts = 0;
    nvmpi_frame_release_cb rel = NULL;
    void *opaque = NULL;
    int ret;

    noseq_stage = "get";
    alarm(NOSEQ_STEP_S);
    ret = nvmpi_decoder_get_frame_fd(ctx, &fd, &w, &h, &pitch, &ts, &rel,
                                         &opaque, true);
    alarm(0);
    if (ret == 0 && rel)
        rel(opaque);             /* hand the frame back before close       */
    return ret == 0;
}

/* Run one pattern on a fresh decoder. Returns 0 pass, 1 fail, 77 skip. */
static int noseq_run(const struct noseq_pattern *pt)
{
    nvDecParam param;
    nvPacket pkt;
    nvmpictx *ctx;
    uint8_t eos = 0;
    int i, err = 0, pre = 0, post = 0;
    long t0, close_ms;

    dec_param_init(&param, NV_VIDEO_CodingAV1);
    param.pixFormat = NV_PIX_NV12;
    param.frame_pool_size = 4;
    param.wait_timeout = pt->wait_ms;  /* ms; a first frame also pays NVDEC start-up */
    ctx = nvmpi_create_decoder(&param);
    if (!ctx)
        return strict_mode ? 1 : 77;

    noseq_pat = pt->id;
    for (i = 0; i < pt->n && !err; i++) {
        int got;

        memset(&pkt, 0, sizeof(pkt));
        pkt.payload = av1_pkt_buf[pt->pkt[i]];
        pkt.payload_size = av1_pkt_len[pt->pkt[i]];
        pkt.pts = 100 + i;
        /* Progress line before the put: if it blocks, the last line shows
         * which one (NVDEC holding OUTPUT buffers). */
        printf("\nav1_noseq put: pattern=%c index=%d", pt->id, i);
        fflush(stdout);
        noseq_idx = i;
        noseq_stage = "put";
        alarm(NOSEQ_STEP_S);
        err = nvmpi_decoder_put_packet(ctx, &pkt);
        alarm(0);
        noseq_idx = -1;
        if (err)
            break;               /* clean error: stop feeding, still close */
        got = noseq_take(ctx);
        if (i < pt->npre)
            pre += got;
        else
            post += got;
    }
    if (pt->eos && !err) {       /* zero-sized payload = EOS marker        */
        memset(&pkt, 0, sizeof(pkt));
        pkt.payload = &eos;
        noseq_stage = "eos";
        alarm(NOSEQ_STEP_S);
        err = nvmpi_decoder_put_packet(ctx, &pkt);
        alarm(0);
        pre += err ? 0 : noseq_take(ctx);
    }

    noseq_stage = "close";
    alarm(NOSEQ_STEP_S);
    t0 = noseq_now_ms();
    i = nvmpi_decoder_close(ctx);
    alarm(0);
    close_ms = noseq_now_ms() - t0;
    /* stdout, not stderr: stderr is /dev/null unless NVMPI_TEST_VERBOSE.
     * Leading newline: the harness printed the test name without one. */
    printf("\nav1_noseq probe: pattern=%c frames=%d err=%d close_ms=%ld pre=%d post=%d\n",
           pt->id, pre + post, err, close_ms, pre, post);
    fflush(stdout);
    if (i != 0 || close_ms >= NOSEQ_CLOSE_MAX_MS) {
        fprintf(stderr, "av1_noseq FAIL pattern=%c: close ret=%d in %ld ms (bound %d)\n",
                pt->id, i, close_ms, NOSEQ_CLOSE_MAX_MS);
        return 1;
    }
    return 0;
}

static int test_decoder_av1_no_seqhdr(void)
{
    size_t i;

#if !NVMPI_HAVE_AV1
    return 77;                   /* no AV1 fourcc: create returns NULL     */
#endif
    if (av1_pkts_init() != 0) {
        fprintf(stderr, "av1_noseq FAIL: fixture split\n");
        return 1;
    }
    signal(SIGALRM, noseq_on_alarm);  /* per-step alarms, see noseq_run   */
    for (i = 0; i < sizeof(noseq_patterns) / sizeof(noseq_patterns[0]); i++) {
        int r = noseq_run(&noseq_patterns[i]);

        if (r)
            return r;
    }
    return 0;
}

#endif /* NVMPI_TEST_AV1_NOSEQ_CASE_H */
