/*
 * test_api_dec_mock.c - libnvmpi decoder against the mock decoder model
 * (mock/mock_dec.c, issue #99). MOCK-ONLY: it needs the mock libs (data
 * path, fault injection) and is registered for the mock build only. The
 * hardware suites (test_api, jetson-ffmpeg hw-*) are unchanged and remain
 * the source of truth for driver behaviour.
 *
 * Drives the public API the way the FFmpeg wrapper does (one thread doing
 * put_packet / get_frame) through src/nvmpi_dec_*.cpp, the vendored
 * NvV4l2ElementPlane and the real NVIDIA sample classes:
 *   decode_*        N packets in lockstep: frame count, pts, gray content
 *   resolution      mid-stream size change (zero-copy path reports the size)
 *   teardown_*      close with packets in flight / frames not drained
 *   flush_*         flush while the capture thread sits in DQBUF
 *   fault_*         every injected fault ends in a clean error path
 *
 * Each case runs in a forked child under alarm(): a hang or crash fails the
 * case, not the run. Exit status of the child: 0 pass, anything else fail.
 * Run under ASan/valgrind to prove the teardown paths leak nothing.
 */
#include "case_log.h"
#include <nvmpi.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../mock/h264_fixture.h"

#define CASE_TIMEOUT_S 30

#define REQUIRE(c)                                                        \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "REQUIRE failed %s:%d: %s\n", __FILE__,       \
                    __LINE__, #c);                                        \
            _exit(10 + (__LINE__ % 200));                                 \
        }                                                                 \
    } while (0)

/* ---- helpers ----------------------------------------------------------- */

static nvmpictx *create(unsigned w, unsigned h, int pool)
{
    nvDecParam p;

    memset(&p, 0, sizeof(p));
    p.codingType = NV_VIDEO_CodingH264;
    p.pixFormat = NV_PIX_NV12;
    p.frame_pool_size = pool;
    p.wait_timeout = 1000;
    p.width = w;
    p.height = h;
    return nvmpi_create_decoder(&p);
}

static int put(nvmpictx *c, const uint8_t *au, size_t n, unsigned long pts)
{
    nvPacket pkt;

    memset(&pkt, 0, sizeof(pkt));
    pkt.payload = (unsigned char *)au;
    pkt.payload_size = n;
    pkt.pts = pts;
    return nvmpi_decoder_put_packet(c, &pkt);
}

/* Copy-out path: 0 on a frame, checks gray luma and the timestamp. */
static int take(nvmpictx *c, unsigned w, unsigned h, long pts, int *gray_ok)
{
    static unsigned char y[4096 * 2304], uv[4096 * 2304 / 2];
    nvFrame f;
    unsigned r;
    int ret;

    memset(&f, 0, sizeof(f));
    f.payload[0] = y;
    f.payload[1] = uv;
    f.linesize[0] = f.linesize[1] = w;
    ret = nvmpi_decoder_get_frame(c, &f, true);
    if (ret == 0 && gray_ok) {
        *gray_ok = (long)f.timestamp == pts;
        for (r = 0; r < h && *gray_ok; r++)
            if (y[(size_t)r * w] != 0x80 || y[(size_t)r * w + w - 1] != 0x80)
                *gray_ok = 0;
    }
    return ret;
}

/* Zero-copy path: reports the frame size; releases the frame at once. */
static int take_fd(nvmpictx *c, int *w, int *h)
{
    int fd, pitch;
    int64_t ts;
    nvmpi_frame_release_cb rel = NULL;
    void *opaque = NULL;
    int ret = nvmpi_decoder_get_frame_fd(c, &fd, w, h, &pitch, &ts, &rel,
                                         &opaque, true);

    if (ret == 0 && rel)
        rel(opaque);
    return ret;
}

/* Feed an IDR then n P frames in lockstep; returns the frames received. */
static int run_stream(nvmpictx *c, unsigned w, unsigned h, int n, int *put_err)
{
    uint8_t au[4096];
    int i, got = 0, gray;

    *put_err = 0;
    for (i = 0; i < n; i++) {
        size_t len = i == 0 ? fx_idr(au, w, h) : fx_pslice(au);

        if (put(c, au, len, 1000UL * (i + 1)) < 0)
            (*put_err)++;
        gray = 1;
        if (take(c, w, h, 1000L * (i + 1), &gray) == 0) {
            got++;
            REQUIRE(gray);               /* data + pts passthrough */
        }
    }
    return got;
}

/* ---- cases ------------------------------------------------------------- */

static int decode_n(unsigned hint_w, unsigned hint_h)
{
    uint8_t eos = 0;
    nvmpictx *c = create(hint_w, hint_h, 4);
    int put_err, got;

    REQUIRE(c);
    got = run_stream(c, 640, 360, 30, &put_err);
    REQUIRE(put_err == 0 && got == 30);          /* one frame per VCL AU */
    REQUIRE(take(c, 640, 360, 0, NULL) != 0);    /* nothing more pending */
    REQUIRE(put(c, &eos, 0, 0) == 0);            /* EOS marker           */
    REQUIRE(take(c, 640, 360, 0, NULL) != 0);    /* bounded, no hang     */
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

static int decode_plain(void) { return decode_n(0, 0); }
static int decode_hint(void) { return decode_n(640, 360); }

static int decode_batched(void)
{
    uint8_t au[4096];
    nvmpictx *c = create(0, 0, 6);
    int b, i, got = 0;

    REQUIRE(c);
    REQUIRE(put(c, au, fx_idr(au, 320, 240), 1) == 0);
    for (b = 0; b < 5; b++) {            /* 3 packets, then drain 3 */
        for (i = 0; i < 3; i++)
            REQUIRE(put(c, au, fx_pslice(au), 2 + b * 3 + i) == 0);
        for (i = 0; i < 3 + (b == 0); i++)
        {
            REQUIRE(take(c, 320, 240, 0, NULL) == 0);
            got++;
        }
    }
    REQUIRE(got == 16);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

static int resolution_change(void)
{
    uint8_t au[4096];
    nvmpictx *c = create(0, 0, 4);
    int i, w = 0, h = 0, n[2] = { 0, 0 };

    REQUIRE(c);
    for (i = 0; i < 6; i++) {            /* 3 at 320x240, then 3 at 640x360 */
        size_t len = i % 3 == 0 ? fx_idr(au, i ? 640 : 320, i ? 360 : 240)
                                : fx_pslice(au);

        REQUIRE(put(c, au, len, 100 + i) == 0);
        REQUIRE(take_fd(c, &w, &h) == 0);
        REQUIRE(w == (i < 3 ? 320 : 640) && h == (i < 3 ? 240 : 360));
        n[i / 3]++;
    }
    REQUIRE(n[0] == 3 && n[1] == 3);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

static int teardown_undrained(void)
{
    uint8_t au[4096];
    nvmpictx *c = create(0, 0, 4);
    int i;

    REQUIRE(c);
    REQUIRE(put(c, au, fx_idr(au, 640, 360), 1) == 0);
    for (i = 0; i < 11; i++)             /* decoder + pool stall behind us */
        REQUIRE(put(c, au, fx_pslice(au), 2 + i) == 0);
    usleep(200 * 1000);                  /* let the pipeline fill up       */
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

static int teardown_no_data(void)
{
    nvmpictx *c = create(0, 0, 4);

    REQUIRE(c);                          /* capture thread still waiting for
                                          * the first event                 */
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* Flush with the capture thread blocked in an UNBOUNDED DQBUF: only the
 * STREAMOFF in flush()/close() can release it. Then decode again. */
static int flush_blocked(void)
{
    uint8_t au[4096];
    nvmpictx *c;
    int put_err, got;

    setenv("NVMPI_MOCK_DQBUF_WAIT_MS", "0", 1);
    c = create(0, 0, 4);
    REQUIRE(c);
    got = run_stream(c, 320, 240, 4, &put_err);
    REQUIRE(got == 4 && put_err == 0);
    usleep(100 * 1000);                  /* capture thread now blocked    */
    REQUIRE(nvmpi_decoder_flush(c) == 0);
    got = run_stream(c, 320, 240, 4, &put_err);  /* new IDR, new event    */
    fprintf(stderr, "after flush: frames=%d put_err=%d\n", got, put_err);
    REQUIRE(got == 4 && put_err == 0);
    (void)au;
    usleep(100 * 1000);
    REQUIRE(nvmpi_decoder_close(c) == 0);    /* wakes the blocked thread  */
    return 0;
}

/* One fault case: run a short stream, never hang or crash, close cleanly.
 * `expect`: 1 = the fault must be visible (no create, put error or lost
 * frames); 0 = libnvmpi must recover completely (all frames arrive). */
static int fault_run(unsigned hint, int expect)
{
    nvmpictx *c = create(hint, hint ? 240 : 0, 4);
    int put_err = 0, got = 0, visible;

    if (!c) {
        printf("    create failed (clean error path)\n");
        return expect ? 0 : 1;
    }
    got = run_stream(c, 320, 240, 6, &put_err);
    visible = put_err > 0 || got < 6;
    printf("    frames=%d put_err=%d\n", got, put_err);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return visible == expect ? 0 : 1;
}

struct tcase {
    const char *name;
    const char *fault;                   /* NVMPI_MOCK_FAULT or NULL       */
    int (*fn)(void);
    unsigned hint;                       /* fault cases: container hint    */
    int expect;                          /* fault cases: fault visible     */
};

static int fault_case(const struct tcase *t)
{
    return fault_run(t->hint, t->expect);
}

#include "dec_mock_latency.h"
#include "dec_mock_av1.h"
#include "dec_mock_av1_seq.h"
#include "dec_mock_fd.h"
#include "dec_mock_fd2.h"
#include "dec_mock_reschange.h"

static const struct tcase k_cases[] = {
    { "decode_plain", NULL, decode_plain, 0, 0 },
    { "decode_hint", NULL, decode_hint, 0, 0 },
    { "decode_batched", NULL, decode_batched, 0, 0 },
    { "av1", "dqbuf@9999", av1_case, 0, 0 },   /* decode + reject; expected [E] logs */
    /* AV1 without an in-band sequence header (#147) */
    { "av1_noseq_first", NULL, av1s_noseq_first, 0, 0 },
    { "av1_seq_only", NULL, av1s_seq_only, 0, 0 },
    { "av1_td_only", NULL, av1s_td_only, 0, 0 },
    { "av1_never_seq", NULL, av1s_never_seq, 0, 0 },
    { "resolution_change", NULL, resolution_change, 0, 0 },
    { "teardown_undrained", NULL, teardown_undrained, 0, 0 },
    { "teardown_no_data", NULL, teardown_no_data, 0, 0 },
    { "teardown_no_data_latency", NULL, teardown_no_data_latency, 0, 0 },
    { "teardown_no_data_hint_latency", NULL, teardown_no_data_hint_latency, 0, 0 },
    { "flush_blocked", NULL, flush_blocked, 0, 0 },
    /* zero-copy borrow lifetime (#137) */
    { "release_after_close", NULL, release_after_close, 0, 0 },
    { "res_change_borrowed", NULL, res_change_borrowed, 0, 0 },
    { "fd_count", NULL, fd_count, 0, 0 },
    { "flush_borrowed", NULL, flush_borrowed, 0, 0 },
    /* get_frame_fd2 per-plane geometry (#138) */
    { "fd2_geometry", NULL, fd2_geometry, 0, 0 },
    { "fd2_args", NULL, fd2_args, 0, 0 },
    { "fd2_v1_equal", NULL, fd2_v1_equal, 0, 0 },
    { "fd2_after_close", NULL, fd2_after_close, 0, 0 },
    /* resolution change while the frame pool is full (#143) */
    { "res_change_backpressure", NULL, res_change_backpressure, 0, 0 },
    { "res_change_backpressure_pool1", NULL, res_change_backpressure_pool1, 0, 0 },
    { "res_change_backpressure_close", NULL, res_change_backpressure_close, 0, 0 },
    /* allocation failure during a resolution change with borrows held. The
     * allocator calls are counted per surface: 1-9 capture plane + 10-13
     * pool at the first event, 14-22 capture plane + 23-26 pool at the
     * change. @22 = capture plane fails, @23 = new pool empty, @24 = new
     * pool of 1 buffer. */
    { "res_change_alloc@22", "alloc@22", res_change_alloc_fault, 0, 1 },
    { "res_change_alloc@23", "alloc@23", res_change_alloc_fault, 0, 1 },
    { "res_change_alloc@24", "alloc@24", res_change_alloc_fault, 0, 1 },
    /* fault matrix: fn == NULL -> fault_case() */
    { "fault_none_control", "dqbuf@9999", NULL, 0, 0 },
    { "fault_dqbuf@1", "dqbuf@1", NULL, 0, 1 },
    { "fault_dqbuf@3", "dqbuf@3", NULL, 0, 1 },
    { "fault_dqbuf@2+", "dqbuf@2+", NULL, 0, 1 },
    { "fault_reqbufs@1", "reqbufs@1", NULL, 0, 1 },
    { "fault_reqbufs@2", "reqbufs@2", NULL, 0, 1 },
    { "fault_qbuf@1", "qbuf@1", NULL, 0, 1 },
    { "fault_qbuf@4+", "qbuf@4+", NULL, 0, 1 },
    { "fault_streamon@1", "streamon@1", NULL, 0, 1 },
    { "fault_streamon@2", "streamon@2", NULL, 0, 1 },
    { "fault_alloc@1", "alloc@1", NULL, 0, 1 },
    { "fault_alloc@3", "alloc@3", NULL, 0, 1 },
    { "fault_alloc@1_hint", "alloc@1", NULL, 320, 0 },   /* pool rebuilt */
    { "fault_alloc@2_hint", "alloc@2", NULL, 320, 0 },   /* pool rebuilt */
    { "fault_alloc@3+_hint", "alloc@3+", NULL, 320, 1 },
};

static int run_body(void *arg)
{
    const struct tcase *t = arg;

    return t->fn ? t->fn() : fault_case(t);
}

/* strict: the decoder logs no libnvmpi error outside the fault cases. */
static int run_case(const struct tcase *t)
{
    return case_log_run(t->name, t->fault, 1, 0, CASE_TIMEOUT_S, run_body,
                        (void *)t);
}

int main(int argc, char **argv)
{
    size_t i;
    int fails = 0, ran = 0;

    printf("=== libnvmpi decoder vs mock model ===\n");
    for (i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        if (argc > 1 && strstr(k_cases[i].name, argv[1]) == NULL)
            continue;                    /* optional name filter           */
        if (case_shard_skip(i)) continue;
        fails += run_case(&k_cases[i]);
        ran++;
    }
    printf("\n%d cases, %d failed\n", ran, fails);
    return fails ? 1 : 0;
}
