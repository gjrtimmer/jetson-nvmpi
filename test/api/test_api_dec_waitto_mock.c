/*
 * test_api_dec_waitto_mock.c - nvDecParam.wait_timeout clamping (#164).
 * MOCK-ONLY. 1..49 -> 50 ms, above 5000 -> 5000 ms, 0 or negative -> the
 * 500 ms default. The effective value is read back from the WARN line the
 * clamp logs (the child's stderr is the capture file, see case_log.h); only
 * the 10 ms case also measures a real wait, so no case sleeps for seconds.
 */
#include "case_log.h"
#include <nvmpi.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#define CASE_TIMEOUT_S 30

#define REQUIRE(c)                                                        \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "REQUIRE failed %s:%d: %s\n", __FILE__,       \
                    __LINE__, #c);                                        \
            _exit(10 + (__LINE__ % 200));                                 \
        }                                                                 \
    } while (0)

/* Create a decoder with wait_timeout wt; *log gets what create logged. */
static nvmpictx *create_wt(int wt, char *log, size_t cap)
{
    nvDecParam p;
    nvmpictx *c;
    off_t start;
    ssize_t n;

    fflush(stderr);
    start = lseek(STDERR_FILENO, 0, SEEK_CUR);   /* shared with the parent  */
    memset(&p, 0, sizeof(p));
    p.codingType = NV_VIDEO_CodingH264;
    p.pixFormat = NV_PIX_NV12;
    p.wait_timeout = wt;
    c = nvmpi_create_decoder(&p);
    fflush(stderr);
    n = pread(STDERR_FILENO, log, cap - 1, start < 0 ? 0 : start);
    log[n < 0 ? 0 : n] = '\0';
    return c;
}

/* get_frame(wait=true) with nothing queued: -1 after the timeout; ms taken. */
static uint64_t empty_wait_ms(nvmpictx *c)
{
    static unsigned char y[320 * 240], uv[320 * 240 / 2];
    nvFrame f;
    uint64_t t0 = case_log_now_ms();

    memset(&f, 0, sizeof(f));
    f.payload[0] = y;
    f.payload[1] = uv;
    f.linesize[0] = f.linesize[1] = 320;
    REQUIRE(nvmpi_decoder_get_frame(c, &f, true) == -1);
    return case_log_now_ms() - t0;
}

static int below_min(void)
{
    char log[2048];
    nvmpictx *c = create_wt(10, log, sizeof(log));
    uint64_t ms;

    REQUIRE(c);
    REQUIRE(strstr(log, "wait_timeout 10 ms clamped to 50 ms"));
    ms = empty_wait_ms(c);
    /* generous bounds: sanitizer builds are slow, never 500 ms though */
    REQUIRE(ms >= 40 && ms < 300);
    nvmpi_decoder_close(c);
    return 0;
}

static int above_max(void)
{
    char log[2048];
    nvmpictx *c = create_wt(9000, log, sizeof(log));

    REQUIRE(c);
    REQUIRE(strstr(log, "wait_timeout 9000 ms clamped to 5000 ms"));
    nvmpi_decoder_close(c);
    return 0;
}

static int negative_default(void)
{
    char log[2048];
    nvmpictx *c = create_wt(-7, log, sizeof(log));

    REQUIRE(c);
    REQUIRE(strstr(log, "wait_timeout -7 ms clamped to 500 ms"));
    nvmpi_decoder_close(c);
    return 0;
}

static int zero_default(void)
{
    char log[2048];
    nvmpictx *c = create_wt(0, log, sizeof(log));

    REQUIRE(c);
    REQUIRE(!strstr(log, "clamped"));            /* 0 is the default, silent */
    nvmpi_decoder_close(c);
    return 0;
}

static int in_range(void)
{
    char log[2048];
    nvmpictx *c = create_wt(50, log, sizeof(log));

    REQUIRE(c);
    REQUIRE(!strstr(log, "clamped"));
    nvmpi_decoder_close(c);
    return 0;
}

static int run_body(void *arg)
{
    return ((int (*)(void))arg)();
}

int main(void)
{
    static const struct { const char *name; int (*fn)(void); } k[] = {
        { "wait_timeout_below_min", below_min },
        { "wait_timeout_above_max", above_max },
        { "wait_timeout_negative", negative_default },
        { "wait_timeout_zero", zero_default },
        { "wait_timeout_in_range", in_range },
    };
    size_t i;
    int fails = 0;

    printf("=== libnvmpi decoder wait_timeout clamp ===\n");
    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        fails += case_log_run(k[i].name, NULL, 1, 0, CASE_TIMEOUT_S,
                              run_body, (void *)k[i].fn);
    printf("\n%zu cases, %d failed\n", sizeof(k) / sizeof(k[0]), fails);
    return fails ? 1 : 0;
}
