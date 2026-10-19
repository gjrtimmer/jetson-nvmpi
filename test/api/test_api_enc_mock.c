/*
 * test_api_enc_mock.c - libnvmpi encoder against the mock encoder model
 * (mock/mock_enc.c, issue #100). MOCK-ONLY: it needs the mock libs (data
 * path, fault injection) and is registered for the mock build only. The
 * hardware suites (test_api, jetson-ffmpeg hw-*) are unchanged and remain
 * the source of truth for driver behaviour.
 *
 * Drives the public API the way the FFmpeg wrapper does, through
 * src/nvmpi_enc_*.cpp, the vendored NvV4l2ElementPlane and the real NVIDIA
 * sample classes:
 *   blocking_*      N frames in lockstep: packet count, pts, key/IDR flags,
 *                   SPS/PPS(/VPS) placement, EOS drain (H.264 and HEVC)
 *   header_*        the extradata path: headers lead the first AU only,
 *                   or every IDR with insert_spspps_idr
 *   force_idr       nvmpi_encoder_force_idr() -> IDR on the next frame
 *   nonblock_*      EAGAIN when OUTPUT buffers are held, plain flow otherwise
 *   flush_*         flush restarts the stream (IDR + headers again)
 *   teardown_*      close with frames in flight / after EOS / without data
 *                   (must not wait out timeouts)
 *   fault_*         every injected fault ends in a clean error path
 *
 * Each case runs in a forked child under alarm(): a hang or crash fails the
 * case, not the run. Exit status of the child: 0 pass, anything else fail.
 * Run under ASan/valgrind to prove the teardown paths leak nothing.
 */
#include "leak_check.h"
#include <signal.h>
#include <sys/wait.h>

#include "enc_mock_util.h"

#define CASE_TIMEOUT_S 30
#define CLOSE_MAX_MS 2500                /* a close() that waits a timeout */

/* ---- cases ------------------------------------------------------------- */

/* N frames, one at a time. Key frames at 0,10,20; IDR (with headers) at 0
 * and 20 only when hdr_at_idr, else headers on the first AU only. */
static int stream(nvCodingType codec, int hdr_at_idr)
{
    struct ecfg c = { codec, 10, 20, hdr_at_idr, 0, 0 };
    int hevc = codec == NV_VIDEO_CodingHEVC, i, eos = 0;
    nvmpictx *ctx = e_create(&c);
    struct epkt p;

    REQUIRE(ctx);
    for (i = 0; i < 30; i++) {
        REQUIRE(e_put(ctx, 1000 + i) == 0);
        REQUIRE(e_get(ctx, hevc, 1, &p) == 0);
        REQUIRE(p.pts == 1000ul + i && p.size > 0);
        REQUIRE(!!(p.flags & E_KEY) == (i % 10 == 0));
        REQUIRE(p.has_idr == (i % 20 == 0));
        REQUIRE(p.has_sps == (i == 0 || (hdr_at_idr && i % 20 == 0)));
        REQUIRE(p.has_pps == p.has_sps && p.has_vps == (hevc && p.has_sps));
    }
    REQUIRE(nvmpi_encoder_put_frame(ctx, NULL) == 0);       /* EOS */
    while (!eos) {                       /* drain: only the EOS result left */
        int r = e_get(ctx, hevc, 0, &p);   /* no wait: EOS spin is inside */

        REQUIRE(r == -2 || r == -1);
        eos = r == -2;
    }
    REQUIRE(nvmpi_encoder_put_frame(ctx, &(nvFrame){ 0 }) == -2); /* flushing */
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

static int blocking_h264(void) { return stream(NV_VIDEO_CodingH264, 0); }
static int blocking_hevc(void) { return stream(NV_VIDEO_CodingHEVC, 0); }
static int header_idr_h264(void) { return stream(NV_VIDEO_CodingH264, 1); }
static int header_idr_hevc(void) { return stream(NV_VIDEO_CodingHEVC, 1); }

/* Put all frames first (blocking put recycles OUTPUT buffers), then drain:
 * every frame yields exactly one packet, in order. */
static int pipelined(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvmpictx *ctx = e_create(&c);
    struct epkt p;
    int i;

    REQUIRE(ctx);
    for (i = 0; i < 12; i++)
        REQUIRE(e_put(ctx, 10 + i) == 0);
    for (i = 0; i < 12; i++) {
        REQUIRE(e_get(ctx, 0, 1, &p) == 0 && p.pts == 10ul + i);
        REQUIRE(!!(p.flags & E_KEY) == (i == 0));
    }
    REQUIRE(e_get(ctx, 0, 0, &p) == -1);                  /* nothing more */
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* Determinism (#111): the same input must give byte-identical access units
 * in every run. Encodes DET_FRAMES frames the way the FFmpeg wrapper does
 * (put, opportunistic get, EOS, drain), folds every packet's size, pts,
 * flags and payload into one hash and compares it with the first run's.
 * The first AU must carry SPS+PPS (the extradata scan reads it), every
 * packet must be well-formed Annex-B (see check_au), and none may be lost. */
#define DET_FRAMES 30
#define DET_RUNS 40

static int det_run(uint64_t *hash)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 10, 20, 0, 0, 0 };
    nvmpictx *ctx = e_create(&c);
    struct epkt p;
    int i, got = 0, r, eos = 0;

    REQUIRE(ctx);
    *hash = 1469598103934665603ull;
#define FOLD(p_) do { \
        *hash = (*hash ^ (p_).hash) * 1099511628211ull; \
        *hash = (*hash ^ (p_).size ^ ((p_).pts << 20) ^ ((p_).flags << 40)) * 1099511628211ull; \
        if (got == 0) REQUIRE((p_).has_sps && (p_).has_pps && (p_).has_idr); \
        got++; } while (0)
    for (i = 0; i < DET_FRAMES; i++) {
        REQUIRE(e_put(ctx, 500 + i) == 0);
        while ((r = e_get(ctx, 0, 0, &p)) == 0)
            FOLD(p);
    }
    REQUIRE(nvmpi_encoder_put_frame(ctx, NULL) == 0);       /* EOS */
    while (!eos) {
        r = e_get(ctx, 0, 0, &p);      /* no wait: EOS spin is inside */
        if (r == 0)
            FOLD(p);
        else
            eos = r == -2;
    }
#undef FOLD
    REQUIRE(got == DET_FRAMES);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

static int determinism(void)
{
    uint64_t ref = 0, h;
    int i;

    for (i = 0; i < DET_RUNS; i++) {
        REQUIRE(det_run(&h) == 0);
        if (i == 0)
            ref = h;
        else if (h != ref) {
            fprintf(stderr, "run %d hash %016llx != %016llx\n", i,
                    (unsigned long long)h, (unsigned long long)ref);
            return 1;
        }
    }
    return 0;
}

static int force_idr(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvmpictx *ctx = e_create(&c);
    struct epkt p;
    int i;

    REQUIRE(ctx);
    for (i = 0; i < 6; i++) {
        if (i == 3)
            REQUIRE(nvmpi_encoder_force_idr(ctx) == 0);
        REQUIRE(e_put(ctx, i) == 0 && e_get(ctx, 0, 1, &p) == 0);
        REQUIRE(p.has_idr == (i == 0 || i == 3));
        REQUIRE(!!(p.flags & E_KEY) == (i == 0 || i == 3));
    }
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* Non-blocking mode, normal flow: put never reports EAGAIN while the
 * encoder keeps up, and the packets match. */
static int nonblock_flow(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 1, 0 };
    nvmpictx *ctx = e_create(&c);
    struct epkt p;
    int i;

    REQUIRE(ctx);
    for (i = 0; i < 20; i++) {
        REQUIRE(e_put(ctx, 5 + i) == 0);
        REQUIRE(e_get(ctx, 0, 1, &p) == 0 && p.pts == 5ul + i);
    }
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* EAGAIN path: the model keeps 8 consumed OUTPUT buffers (NVMPI_MOCK_ENC_
 * LATENCY) and bounds the blocking DQBUF at 20 ms, so after the 4 OUTPUT
 * buffers are in flight put_frame must answer NVMPI_ERR_EAGAIN instead of
 * blocking - and keep doing so until the application drains packets; the
 * packets themselves were produced already. */
static int nonblock_eagain(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 1, 0 };
    nvmpictx *ctx;
    struct epkt p;
    uint64_t t0;
    int i, got = 0;

    setenv("NVMPI_MOCK_ENC_LATENCY", "8", 1);
    setenv("NVMPI_MOCK_DQBUF_WAIT_MS", "20", 1);
    ctx = e_create(&c);
    REQUIRE(ctx);
    for (i = 0; i < 4; i++)              /* the 4 OUTPUT buffers           */
        REQUIRE(e_put(ctx, i) == 0);
    t0 = e_now_ms();
    for (i = 0; i < 3; i++)              /* no free buffer: bounded EAGAIN  */
        REQUIRE(e_put(ctx, 99) == NVMPI_ERR_EAGAIN);
    REQUIRE(e_now_ms() - t0 < 2000);
    usleep(50 * 1000);
    while (e_get(ctx, 0, 0, &p) == 0)
        got++;
    REQUIRE(got == 4);                   /* all 4 frames were encoded       */
    REQUIRE(e_put(ctx, 99) == NVMPI_ERR_EAGAIN);   /* still starved        */
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* Flush restarts the stream: the first frame after it is an IDR with
 * headers again, and pts/packets flow as before. */
static int flush_restart(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvmpictx *ctx = e_create(&c);
    struct epkt p;
    int i;

    REQUIRE(ctx);
    for (i = 0; i < 5; i++)
        REQUIRE(e_put(ctx, i) == 0 && e_get(ctx, 0, 1, &p) == 0);
    REQUIRE(nvmpi_encoder_put_frame(ctx, NULL) == 0);
    usleep(50 * 1000);                   /* DQ thread sees the EOS buffer  */
    REQUIRE(nvmpi_encoder_flush(ctx) == 0);
    for (i = 0; i < 5; i++) {
        REQUIRE(e_put(ctx, 100 + i) == 0);
        REQUIRE(e_get(ctx, 0, 1, &p) == 0 && p.pts == 100ul + i);
        REQUIRE(p.has_idr == (i == 0) && p.has_sps == (i == 0));
    }
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* Close paths: none may wait out the DQ-thread/EOS timeouts. */
static int teardown(int frames, int drain, int eos)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvmpictx *ctx = e_create(&c);
    struct epkt p;
    uint64_t took = 0;
    int i;

    REQUIRE(ctx);
    for (i = 0; i < frames; i++)
        REQUIRE(e_put(ctx, i) == 0);
    for (i = 0; drain && i < frames; i++)
        REQUIRE(e_get(ctx, 0, 1, &p) == 0);
    if (eos)
        REQUIRE(nvmpi_encoder_put_frame(ctx, NULL) == 0);
    REQUIRE(e_destroy(ctx, &took) == 0);
    fprintf(stderr, "close took %llu ms\n", (unsigned long long)took);
    REQUIRE(took < CLOSE_MAX_MS);
    return 0;
}

static int teardown_in_flight(void) { return teardown(4, 0, 0); }
static int teardown_wrapped(void) { return teardown(11, 0, 0); }
static int teardown_after_eos(void) { return teardown(6, 1, 1); }
static int teardown_no_data(void) { return teardown(0, 0, 0); }
static int teardown_all_slots(void) { return teardown(4, 1, 0); }

/* put_frame_fd (DMABUF OUTPUT plane): lazily allocates the internal surface
 * through nvmpi_alloc_surfaces (the fix of #100); packets still flow. */
static int dmabuf_flow(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 1 };
    nvmpictx *ctx = e_create(&c);
    struct epkt p;
    int fd = -1, i;

    REQUIRE(ctx);
    REQUIRE(nvmpi_surface_alloc_for_enc(EW, EH, &fd) == 0 && fd >= 0);
    for (i = 0; i < 6; i++) {
        REQUIRE(nvmpi_encoder_put_frame_fd(ctx, fd, EW, EH, EW, 7 + i) == 0);
        REQUIRE(e_get(ctx, 0, 1, &p) == 0 && p.pts == 7ul + i);
    }
    REQUIRE(nvmpi_surface_destroy(fd) == 0);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* ---- fault matrix ------------------------------------------------------ */

/* One fault case: run a short stream, never hang or crash, close cleanly.
 * `expect`: 1 = the fault must be visible (no create, put error or lost
 * packets); 0 = libnvmpi must recover completely (all packets arrive). */
static int fault_run(int expect)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvmpictx *ctx = e_create(&c);
    struct epkt p;
    int i, put_err = 0, got = 0, visible;
    uint64_t took = 0;

    if (!ctx) {
        printf("    create failed (clean error path)\n");
        return expect ? 0 : 1;
    }
    for (i = 0; i < 8; i++) {
        if (e_put(ctx, i) != 0)
            put_err++;
        usleep(10 * 1000);               /* the model answers at once      */
        if (e_get(ctx, 0, 0, &p) == 0)   /* no wait: a lost packet is not
                                          * worth a 1 s timeout            */
            got++;
    }
    visible = put_err > 0 || got < 8;
    printf("    packets=%d put_err=%d\n", got, put_err);
    (void)nvmpi_encoder_put_frame(ctx, NULL);
    REQUIRE(e_destroy(ctx, &took) == 0 || expect);
    REQUIRE(took < CLOSE_MAX_MS * 2);
    return visible == expect ? 0 : 1;
}

/* alloc point: public surface allocation (nvmpi_surface.cpp) - the old
 * NvAllocate call dereferenced NULL when the allocator failed. */
static int fault_surface_alloc(void)
{
    int fd = 5;

    REQUIRE(nvmpi_surface_alloc(EW, EH, &fd) != 0);      /* alloc@1: clean  */
    REQUIRE(fd == -1);
    REQUIRE(nvmpi_surface_alloc_for_enc(EW, EH, &fd) == 0 && fd >= 0);
    REQUIRE(nvmpi_surface_destroy(fd) == 0);
    return 0;
}

/* alloc@2: the encoder's lazy internal surface in put_frame_fd. */
static int fault_dmabuf_alloc(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 1 };
    nvmpictx *ctx = e_create(&c);
    int fd = -1;

    REQUIRE(ctx);
    REQUIRE(nvmpi_surface_alloc_for_enc(EW, EH, &fd) == 0);   /* alloc #1  */
    REQUIRE(nvmpi_encoder_put_frame_fd(ctx, fd, EW, EH, EW, 1) < 0); /* #2 */
    REQUIRE(nvmpi_surface_destroy(fd) == 0);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

struct tcase {
    const char *name;
    const char *fault;                   /* NVMPI_MOCK_FAULT or NULL       */
    int (*fn)(void);
    int expect;                          /* fault cases: fault visible     */
};

static int fault_case(const struct tcase *t) { return fault_run(t->expect); }

#include "enc_mock_cases.h"

static int run_case(const struct tcase *t)
{
    pid_t pid;
    int st;
    uint64_t t0 = e_now_ms();

    printf("  %-24s ", t->name);
    fflush(stdout);
    pid = fork();
    if (pid < 0)
        return 1;
    if (pid == 0) {
        alarm(CASE_TIMEOUT_S);           /* a hang dies with SIGALRM      */
        if (t->fault)
            setenv("NVMPI_MOCK_FAULT", t->fault, 1);
        if (!getenv("NVMPI_TEST_VERBOSE"))
            (void)!freopen("/dev/null", "w", stderr);
        st = t->fn ? t->fn() : fault_case(t);
        fflush(stdout);
        nvmpi_test_leak_check();
        _exit(st & 0xff);
    }
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) {
        printf("FAIL (signal %d: %s)\n", WTERMSIG(st), strsignal(WTERMSIG(st)));
        return 1;
    }
    if (WEXITSTATUS(st)) {
        printf("FAIL (exit %d)\n", WEXITSTATUS(st));
        return 1;
    }
    printf("PASS (%llu ms)\n", (unsigned long long)(e_now_ms() - t0));
    return 0;
}

int main(int argc, char **argv)
{
    size_t i;
    int fails = 0, ran = 0;

    printf("=== libnvmpi encoder vs mock model ===\n");
    for (i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        if (argc > 1 && strstr(k_cases[i].name, argv[1]) == NULL)
            continue;                    /* optional name filter           */
        fails += run_case(&k_cases[i]);
        ran++;
    }
    printf("\n%d cases, %d failed\n", ran, fails);
    return fails ? 1 : 0;
}
