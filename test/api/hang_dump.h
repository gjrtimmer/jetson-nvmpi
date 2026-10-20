/*
 * hang_dump.h - print every thread's stack when a forked mock case hangs
 * (issue #136). Used by case_log_run(); the child calls case_hang_arm().
 *
 * Why in-process and not "gdb -batch -p <pid>" from the parent: the target is
 * a sibling of gdb, so Yama ptrace_scope=1 and the default container seccomp
 * profile both refuse the attach, and the CI mock images do not ship gdb. A
 * watchdog thread in the child needs nothing but glibc:
 *   - at the deadline it lists /proc/self/task and sends SIGUSR2 to each
 *     other thread, one at a time;
 *   - the handler (backtrace + backtrace_symbols_fd + write only, which is
 *     async-signal-safe once libgcc is preloaded by case_hang_arm) prints that
 *     thread's stack to stderr, which is the captured case log;
 *   - then the watchdog raises SIGALRM with the default action, so the case
 *     still dies "signal 14" exactly as before and stays a FAIL.
 * The plain alarm() stays armed a few seconds later as a backstop should the
 * watchdog itself wedge. Frames show "binary(sym+off) [addr]"; internal
 * (static) functions have no dynsym, so resolve the +off with addr2line -e.
 * Output avoids every pattern in scripts/check-test-log.sh.
 */
#ifndef NVMPI_TEST_HANG_DUMP_H
#define NVMPI_TEST_HANG_DUMP_H

#include <dirent.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define HANG_BACKSTOP_S 5               /* alarm() after the watchdog     */
#define HANG_DUMP_WAIT_US 1000000       /* max wait for one thread's dump */

static atomic_int hang_dump_done;       /* set by the SIGUSR2 handler     */
static int hang_deadline_s;

static void hang_write(const char *s)
{
    ssize_t r = write(STDERR_FILENO, s, strlen(s));

    (void)r;
}

/* Async-signal-safe: runs on the thread being dumped. */
static void hang_usr2(int sig)
{
    void *bt[64];
    int n;

    (void)sig;
    n = backtrace(bt, 64);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    atomic_store(&hang_dump_done, 1);
}

/* Watchdog thread: plain code is fine here, only the handler must be safe. */
static void hang_dump_all(void)
{
    char hdr[96];
    long self = syscall(SYS_gettid);
    struct dirent *e;
    DIR *d = opendir("/proc/self/task");

    hang_write("\n=== case hang: thread stacks (issue #136) ===\n");
    while (d && (e = readdir(d))) {
        long tid = strtol(e->d_name, NULL, 10);
        int w;

        if (tid <= 0 || tid == self)     /* "." / ".." / this watchdog    */
            continue;
        snprintf(hdr, sizeof hdr, "--- thread %ld ---\n", tid);
        hang_write(hdr);
        atomic_store(&hang_dump_done, 0);
        if (syscall(SYS_tgkill, getpid(), tid, SIGUSR2) < 0)
            continue;
        for (w = 0; w < HANG_DUMP_WAIT_US / 1000 &&
                    !atomic_load(&hang_dump_done); w++)
            usleep(1000);
    }
    if (d)
        closedir(d);
    hang_write("=== end thread stacks ===\n");
}

static void *hang_watchdog(void *arg)
{
    (void)arg;
    sleep((unsigned)hang_deadline_s);
    hang_dump_all();
    /* Default action: the parent sees WTERMSIG == SIGALRM, a FAIL, as with
     * the bare alarm(). */
    signal(SIGALRM, SIG_DFL);
    kill(getpid(), SIGALRM);
    for (;;)
        pause();
    return NULL;
}

/* Call in the forked child, before running the body. */
static inline void hang_arm(int timeout_s)
{
    struct sigaction sa;
    void *warm[4];
    pthread_t t;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = hang_usr2;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    /* First backtrace() loads libgcc (malloc/dlopen): do it now, not in the
     * signal handler. */
    (void)backtrace(warm, 4);
    alarm((unsigned)timeout_s + HANG_BACKSTOP_S);
    hang_deadline_s = timeout_s;
    if (pthread_create(&t, NULL, hang_watchdog, NULL) == 0)
        pthread_detach(t);
}

#endif
