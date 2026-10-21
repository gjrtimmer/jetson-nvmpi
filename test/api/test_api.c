/*
 * test_api.c — libnvmpi public C API smoke test.
 *
 * Links libnvmpi.so directly (no FFmpeg) and exercises the create/close
 * lifecycle for both decoder and encoder. This catches linkage issues,
 * init-without-data crashes, and close-path segfaults at the lowest level.
 *
 * Each test runs in a forked child process so a crash in one test does not
 * kill the harness. Outcomes:
 *   PASS  — child exited 0
 *   SKIP  — child exited 77 (e.g. create returned NULL on stubs)
 *   CRASH — child killed by signal (e.g. SIGSEGV on stubs close path)
 *   FAIL  — child exited non-zero
 *
 * On stubs builds, create_decoder/create_encoder may succeed (the stub V4L2
 * symbols are resolved) but close may crash because the DQ thread runs
 * against fake ioctl results. CRASH is reported cleanly — it means the code
 * path needs attention but does not block the rest of the harness.
 *
 * Environment switches (default off, stubs behaviour unchanged):
 *   NVMPI_TEST_STRICT=1  — crashes count as failures (use on real hardware,
 *                          where a crash is a genuine bug, not a stub gap)
 *   NVMPI_TEST_VERBOSE=1 — keep child stderr so abort()/driver messages show
 *
 * Build: cmake -DBUILD_TESTING=ON .. && make test_api
 * Run:   ./test_api  (on Jetson or stubs build)
 *
 * Issue: gjrtimmer/jetson-ffmpeg#27
 */
#include "leak_check.h"
#include <nvmpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

/* CMake defines NVMPI_HAVE_AV1 (also in av1_fixture.h); assume yes otherwise. */
#ifndef NVMPI_HAVE_AV1
#define NVMPI_HAVE_AV1 1
#endif
/* Same for AV1 encode (#8): CMake passes 0/1. */
#ifndef NVMPI_HAVE_AV1_ENC
#define NVMPI_HAVE_AV1_ENC 1
#endif

static int tests_run = 0;
static int tests_passed = 0;
static int tests_skipped = 0;
static int tests_crashed = 0;
static int strict_mode = 0;   /* NVMPI_TEST_STRICT: crash == failure */
static int verbose_mode = 0;  /* NVMPI_TEST_VERBOSE: keep child stderr */

/* True when the env var is set to anything other than empty or "0". */
static int env_flag(const char *name)
{
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

/*
 * Iteration count for the rapid create/close loops. NVMPI_TEST_ITER overrides
 * the per-test default (weekly schedule sets a high value for soak coverage).
 * Input is untrusted env text: parse strictly and clamp so a typo or huge
 * value cannot turn the loop into an endless run or overflow the counter.
 */
static int iter_count(int def)
{
    const char *v = getenv("NVMPI_TEST_ITER");
    if (!v || !*v) return def;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (*end != '\0' || n < 1 || n > 10000) return def;
    return (int)n;
}

/*
 * Run a test function in a forked child process. The child calls fn()
 * and _exit()s with its return value. The parent waitpid()s and interprets
 * the result. Returns 0 for PASS, 77 for SKIP, 1 for FAIL/CRASH.
 */
static int run_isolated(const char *name, int (*fn)(void))
{
    printf("  %-40s ", name);
    fflush(stdout);
    tests_run++;

    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL (fork)\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: run the test, exit with its return code.
         * Redirect stderr to /dev/null so V4L2 noise from stubs
         * does not clutter the harness output. Skipped in verbose mode:
         * on real hardware the abort reason is exactly what we need. */
        if (!verbose_mode)
            (void)!freopen("/dev/null", "w", stderr);
        int r = fn();
        nvmpi_test_leak_check();
        _exit(r);
    }

    /* Parent: wait for the child and interpret the result. */
    int status;
    waitpid(pid, &status, 0);

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        tests_crashed++;
        printf("CRASH (signal %d: %s)\n", sig, strsignal(sig));
        return 1;
    }

    int code = WEXITSTATUS(status);
    if (code == 0)  { tests_passed++;  printf("PASS\n");  return 0; }
    if (code == 77) { tests_skipped++; printf("SKIP\n");  return 0; }
    printf("FAIL (exit %d)\n", code);
    return 1;
}

#define RUN_TEST(fn) run_isolated(#fn, fn)

/* Helper: populate a minimal nvDecParam for the given codec type. */
static void dec_param_init(nvDecParam *p, nvCodingType codec)
{
    memset(p, 0, sizeof(*p));
    p->codingType = codec;
}

/* Helper: populate a minimal nvEncParam for H.264. */
static void enc_param_init(nvEncParam *p)
{
    memset(p, 0, sizeof(*p));
    p->codingType = NV_VIDEO_CodingH264;
    p->width = 1280;
    p->height = 720;
    p->bitrate = 2000000;
    p->fps_n = 30;
    p->fps_d = 1;
    p->iframe_interval = 30;
    p->idr_interval = 60;
    p->capture_num = 4;
    p->insert_spspps_idr = 1;
    p->pixFormat = NV_PIX_YUV420;
}

/*
 * Decoder: create and immediately close without feeding any data.
 * Exercises nvmpi_create_decoder → nvmpi_decoder_close init/teardown.
 */
static int test_decoder_create_close(void)
{
    nvDecParam param;
    dec_param_init(&param, NV_VIDEO_CodingH264);

    nvmpictx *ctx = nvmpi_create_decoder(&param);
    if (!ctx) return 77;

    int ret = nvmpi_decoder_close(ctx);
    return (ret == 0) ? 0 : 1;
}

/*
 * Encoder: create and immediately close without feeding any data.
 * Exercises nvmpi_create_encoder → nvmpi_encoder_close init/teardown.
 */
static int test_encoder_create_close(void)
{
    nvEncParam param;
    enc_param_init(&param);

    nvmpictx *ctx = nvmpi_create_encoder(&param);
    if (!ctx) return 77;

    int ret = nvmpi_encoder_close(ctx);
    return (ret == 0) ? 0 : 1;
}

/*
 * Decoder: create/close in a tight loop (default 5, NVMPI_TEST_ITER
 * overrides) — leak/crash detector. Each decoder create costs ~1s on real
 * hardware, so the default is small; the leak/crash assertion (every create
 * must succeed after a close) is unchanged. If the V4L2 device is not properly released, later iterations will fail
 * to open or the system will run out of DMA-buf file descriptors.
 */
static int test_decoder_rapid_lifecycle(void)
{
    nvDecParam param;
    dec_param_init(&param, NV_VIDEO_CodingH264);

    for (int i = 0, n = iter_count(5); i < n; i++) {
        nvmpictx *ctx = nvmpi_create_decoder(&param);
        if (!ctx) return 77;
        nvmpi_decoder_close(ctx);
    }
    return 0;
}

/*
 * Encoder: create/close in a tight loop (default 10, NVMPI_TEST_ITER
 * overrides) — leak/crash detector. Same rationale as the decoder variant: DQ thread start/stop + V4L2
 * device open/close must not leak handles.
 */
static int test_encoder_rapid_lifecycle(void)
{
    nvEncParam param;
    enc_param_init(&param);

    for (int i = 0, n = iter_count(10); i < n; i++) {
        nvmpictx *ctx = nvmpi_create_encoder(&param);
        if (!ctx) return 77;
        nvmpi_encoder_close(ctx);
    }
    return 0;
}

/*
 * Decoder: every supported codec type creates without crash.
 * Verifies the codingType → V4L2 format mapping covers all enum values.
 */
static int test_decoder_all_codecs(void)
{
    nvCodingType codecs[] = {
        NV_VIDEO_CodingH264, NV_VIDEO_CodingHEVC,
        NV_VIDEO_CodingMPEG2, NV_VIDEO_CodingMPEG4,
        NV_VIDEO_CodingVP8, NV_VIDEO_CodingVP9,
#if NVMPI_HAVE_AV1   /* 0: L4T headers lack V4L2_PIX_FMT_AV1, create must return NULL */
        NV_VIDEO_CodingAV1,
#endif
    };
    int n = (int)(sizeof(codecs) / sizeof(codecs[0]));

    for (int i = 0; i < n; i++) {
        nvDecParam param;
        dec_param_init(&param, codecs[i]);

        nvmpictx *ctx = nvmpi_create_decoder(&param);
        /* AV1 is a feature of this library, not an optional device quirk:
         * on real hardware (strict) a NULL create is a failure. */
        if (!ctx) return (strict_mode && codecs[i] == NV_VIDEO_CodingAV1) ? 1 : 77;
        nvmpi_decoder_close(ctx);
    }
    return 0;
}

/*
 * Encoder: HEVC codec type creates without crash.
 * Verifies the codingType mapping for the second supported encode codec.
 */
static int test_encoder_hevc_create_close(void)
{
    nvEncParam param;
    enc_param_init(&param);
    param.codingType = NV_VIDEO_CodingHEVC;

    nvmpictx *ctx = nvmpi_create_encoder(&param);
    if (!ctx) return 77;

    int ret = nvmpi_encoder_close(ctx);
    return (ret == 0) ? 0 : 1;
}

#include "jpeg_fd_case.h"
#include "av1_case.h"
#include "av1_enc_case.h"
#include "enc_p010_case.h"
#include "dec_fd_case.h"
#include "dec_fd2_case.h"

int main(void)
{
    printf("=== libnvmpi API test ===\n");
    strict_mode = env_flag("NVMPI_TEST_STRICT");
    verbose_mode = env_flag("NVMPI_TEST_VERBOSE");

    int fail = 0;
    fail += RUN_TEST(test_decoder_create_close);
    fail += RUN_TEST(test_encoder_create_close);
    fail += RUN_TEST(test_decoder_rapid_lifecycle);
    fail += RUN_TEST(test_encoder_rapid_lifecycle);
    fail += RUN_TEST(test_decoder_all_codecs);
    fail += RUN_TEST(test_encoder_hevc_create_close);
    fail += RUN_TEST(test_decoder_av1_frame);
    fail += RUN_TEST(test_encoder_av1_roundtrip);
    /* AV1 + P010 is refused up front (#146). */
    fail += RUN_TEST(test_encoder_av1_p010_refused);
    /* HEVC Main10 from P010 input (#126). */
    fail += RUN_TEST(test_encoder_hevc_p010_roundtrip);
    /* 8-bit HEVC declares Main (#144). */
    fail += RUN_TEST(test_encoder_hevc_main8_roundtrip);
    /* Zero-copy lifetime: resolution change + release after close (#137). */
    fail += RUN_TEST(test_decoder_res_change_borrowed);
    /* get_frame_fd2 layout vs the allocator's planeParams (#138). */
    fail += RUN_TEST(test_decoder_fd2_geometry);
    /* JPEG DMA-BUF output: real NVJPG or the mock model, SKIP on stubs. */
    fail += RUN_TEST(test_jpeg_fd_nv12);
    fail += RUN_TEST(test_jpeg_fd_yuv420);
    fail += RUN_TEST(test_jpeg_fd_close_order);
    fail += RUN_TEST(test_jpeg_fd_loop_fd_count);

    printf("\n%d tests: %d passed, %d skipped",
           tests_run, tests_passed, tests_skipped);
    if (tests_crashed > 0)
        printf(", %d crashed", tests_crashed);
    if (fail > 0)
        printf(", %d failed", fail);
    printf("\n");

    /* On stubs builds, crashes are expected — the V4L2 stubs are link-only
     * and do not emulate device behavior. Report success as long as no test
     * returned a non-zero non-signal non-skip exit code. Crashes are logged
     * for visibility but do not fail the harness. In strict mode (real
     * hardware) a crash is a real defect, so it must fail the run; otherwise
     * a fully crashing binary would report green. */
    int real_failures = strict_mode ? fail : fail - tests_crashed;
    return (real_failures > 0) ? 1 : 0;
}
