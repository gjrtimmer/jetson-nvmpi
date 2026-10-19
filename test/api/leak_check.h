/*
 * Forked test children leave through _exit(), which skips the atexit hook
 * that runs LeakSanitizer. Call nvmpi_test_leak_check() right before _exit()
 * so an ASan build still reports (and fails on) leaks from the child; it is
 * a no-op in every other build.
 */
#ifndef NVMPI_TEST_LEAK_CHECK_H
#define NVMPI_TEST_LEAK_CHECK_H

#if defined(__SANITIZE_ADDRESS__)
#define NVMPI_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NVMPI_TEST_ASAN 1
#endif
#endif

#ifdef NVMPI_TEST_ASAN
#include <sanitizer/lsan_interface.h>
/* Terminates the process with the sanitizer exit code if leaks are found. */
static inline void nvmpi_test_leak_check(void) { __lsan_do_leak_check(); }
#else
static inline void nvmpi_test_leak_check(void) {}
#endif

#endif
