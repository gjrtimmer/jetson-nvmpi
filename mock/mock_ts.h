/*
 * mock_ts.h - V4L2 buffer timestamp conversions of the mock, faithful to the
 * Linux kernel / Tegra driver (issue #148).
 */
#ifndef MOCK_TS_H
#define MOCK_TS_H

#include <stdint.h>
#include <sys/time.h>

/* Like the Linux kernel (v4l2_timeval_to_ns): the QBUF timeval is stored as
 * u64 nanoseconds with unsigned wrap-around and no range check. */
static inline uint64_t mock_timeval_to_ns(const struct timeval *tv)
{
    return (uint64_t)tv->tv_sec * 1000000000ULL
         + (uint64_t)tv->tv_usec * 1000ULL;
}

/* Like the kernel (ns_to_kernel_old_timeval): the stored value is read back
 * as signed s64 ns, split with floor semantics, tv_usec = tv_nsec / 1000.
 * Anything >= 2^63 ns therefore comes back as a negative time. */
static inline struct timeval mock_ns_to_timeval(uint64_t ns)
{
    int64_t n = (int64_t)ns;
    int64_t sec = n / 1000000000LL;
    int64_t rem = n % 1000000000LL;
    struct timeval tv;

    if (rem < 0) {
        sec--;
        rem += 1000000000LL;
    }
    tv.tv_sec = (time_t)sec;
    tv.tv_usec = (suseconds_t)(rem / 1000);
    return tv;
}

#endif /* MOCK_TS_H */
