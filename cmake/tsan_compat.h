/*
 * Force-included into C++ translation units of TSan builds (NVMPI_SANITIZER=
 * thread) only. libstdc++ in GCC < 12 maps condition_variable::wait_for to
 * pthread_cond_clockwait, which GCC 11's libtsan does not intercept: TSan then
 * never sees the mutex released and re-acquired inside the wait and reports
 * bogus "double lock" and data races on every NVMPI_bufPool queue. Turn the
 * clockwait path off so wait_for uses pthread_cond_timedwait (intercepted).
 * Test-build only; the shipped library is unaffected.
 */
#include <bits/c++config.h>
#undef _GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT
