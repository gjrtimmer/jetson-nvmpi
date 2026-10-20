/*
 * case_log.h - run one mock API test case in a forked child and capture its
 * stderr (issue #120). Shared by test_api_dec_mock.c / test_api_enc_mock.c.
 *
 * Why: NVMPI_LOG writes to stderr. With NVMPI_TEST_VERBOSE set (CI does) the
 * errors that fault injection provokes on purpose ("[libnvmpi][E]: ...")
 * used to interleave with the results and look like real failures. The child
 * stderr now always goes to a temp file; the parent classifies it after the
 * child exits:
 *   [libnvmpi][E] in a fault case   expected: counted, shown only if verbose
 *   [libnvmpi][E] elsewhere         strict: case FAILS, line shown
 *                                   lenient: shown only if verbose
 *   other [libnvmpi][x] lines       shown only if verbose
 *   everything else                 ALWAYS shown verbatim (REQUIRE failures,
 *                                   ASan/UBSan/TSan reports, crashes): a
 *                                   sanitizer report must never be hidden.
 */
#ifndef NVMPI_TEST_CASE_LOG_H
#define NVMPI_TEST_CASE_LOG_H

#include "leak_check.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CASE_LOG_TAG   "[libnvmpi]["
#define CASE_LOG_ERROR "[libnvmpi][E]"

/* Count expected/unexpected libnvmpi errors in the (rewound) capture file. */
static inline void case_log_scan(FILE *f, int is_fault, size_t *expected,
                                 size_t *unexpected)
{
    char *line = NULL;
    size_t cap = 0;

    *expected = *unexpected = 0;
    rewind(f);
    /* getline(): no fixed buffer, so a long line is never split in two and
     * mis-classified. */
    while (getline(&line, &cap, f) >= 0) {
        if (!strstr(line, CASE_LOG_ERROR))
            continue;
        if (is_fault)
            (*expected)++;
        else
            (*unexpected)++;
    }
    free(line);
}

/* Replay the capture file; see the table at the top for what is shown. */
static inline void case_log_dump(FILE *f, int is_fault, int strict)
{
    int verbose = getenv("NVMPI_TEST_VERBOSE") != NULL;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    rewind(f);
    while ((n = getline(&line, &cap, f)) >= 0) {
        const char *tag = NULL;
        int always = 0;                  /* shown even when not verbose   */

        if (strstr(line, CASE_LOG_ERROR)) {
            tag = is_fault ? "[expected] "
                           : (strict ? "[unexpected] " : "[libnvmpi] ");
            always = !is_fault && strict;
        } else if (strstr(line, CASE_LOG_TAG)) {
            tag = "[libnvmpi] ";
        }
        /* Untagged text (REQUIRE, sanitizers) is always printed raw. */
        if (tag && !verbose && !always)
            continue;
        if (tag)
            printf("    %s", tag);
        fputs(line, stdout);
        if (n > 0 && line[n - 1] != '\n')
            putchar('\n');               /* partial last line (crash)     */
    }
    free(line);
}

/*
 * Sharding (issue #130): NVMPI_TEST_SHARD="k/n" makes a suite run only the
 * cases whose index i satisfies i % n == k, so ctest can run n shards of one
 * binary side by side. Unset: never skip. A malformed value aborts instead of
 * silently running everything (or nothing), which would hide missing cases.
 * Env var, not argv, so the optional argv[1] name filter keeps working.
 */
static inline int case_shard_skip(size_t i)
{
    const char *v = getenv("NVMPI_TEST_SHARD");
    char *end = NULL;
    long k, n;

    if (!v)
        return 0;
    k = strtol(v, &end, 10);
    if (end == v || *end != '/' || k < 0)
        goto bad;
    v = end + 1;
    n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < 1 || k >= n)
        goto bad;
    return (long)(i % (size_t)n) != k;
bad:
    fprintf(stderr, "NVMPI_TEST_SHARD must be \"k/n\" with 0 <= k < n\n");
    exit(2);
}

static inline uint64_t case_log_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/*
 * Fork body(arg) with NVMPI_MOCK_FAULT=fault (if non-NULL), alarm() timeout
 * and stderr captured; print the result line. Returns 0 pass, 1 fail.
 * strict: a [libnvmpi][E] outside a fault case fails the case.
 */
static inline int case_log_run(const char *name, const char *fault,
                               int strict, int show_ms, int timeout_s,
                               int (*body)(void *), void *arg)
{
    FILE *f;
    pid_t pid;
    int st, bad = 0;
    size_t exp, unexp;
    uint64_t t0 = case_log_now_ms();

    printf("  %-24s ", name);
    /* Flush before fork: unflushed stdio would be emitted twice. */
    fflush(stdout);
    fflush(stderr);
    f = tmpfile();                       /* removed on fclose / exit      */
    if (!f) {
        printf("FAIL (tmpfile: %s)\n", strerror(errno));
        return 1;
    }
    pid = fork();
    if (pid < 0) {
        printf("FAIL (fork: %s)\n", strerror(errno));
        fclose(f);
        return 1;
    }
    if (pid == 0) {
        alarm(timeout_s);                /* a hang dies with SIGALRM      */
        if (fault)
            setenv("NVMPI_MOCK_FAULT", fault, 1);
        /* Child stderr -> capture file (shared offset, parent rewinds). */
        if (dup2(fileno(f), STDERR_FILENO) < 0)
            _exit(125);
        st = body(arg);
        fflush(stdout);
        nvmpi_test_leak_check();
        _exit(st & 0xff);
    }
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    fflush(stdout);
    case_log_scan(f, fault != NULL, &exp, &unexp);
    if (WIFSIGNALED(st)) {
        printf("FAIL (signal %d: %s)\n", WTERMSIG(st), strsignal(WTERMSIG(st)));
        bad = 1;
    } else if (WEXITSTATUS(st)) {
        printf("FAIL (exit %d)\n", WEXITSTATUS(st));
        bad = 1;
    } else if (strict && unexp) {
        printf("FAIL (unexpected libnvmpi error)\n");
        bad = 1;
    } else {
        printf("PASS");
        if (exp)
            printf(" (%zu expected libnvmpi errors)", exp);
        if (show_ms)
            printf(" (%llu ms)",
                   (unsigned long long)(case_log_now_ms() - t0));
        printf("\n");
    }
    /* Captured output follows the result line; also after a crash. */
    case_log_dump(f, fault != NULL, strict);
    fclose(f);
    return bad;
}

#endif
