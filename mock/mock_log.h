/*
 * mock_log.h - tiny stderr logger shared by the mock libraries.
 *
 * Silent unless NVMPI_MOCK_VERBOSE is set (anything but empty or "0"), so a
 * passing test run stays quiet while a failing one can be re-run with the
 * mock's own trace. Errors that indicate a mock misuse (bad index, double
 * QBUF, ...) are logged through MOCK_ERR and ALWAYS shown, because on real
 * hardware the same call would be a driver error the caller must handle.
 */
#ifndef NVMPI_MOCK_LOG_H
#define NVMPI_MOCK_LOG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read once per call; cheap enough for a test double and avoids init order
 * problems between the three mock libraries. */
static inline int mock_verbose(void)
{
    const char *v = getenv("NVMPI_MOCK_VERBOSE");
    return v && *v && strcmp(v, "0") != 0;
}

#define MOCK_TRACE(...)                                                 \
    do {                                                                \
        if (mock_verbose()) {                                           \
            fprintf(stderr, "[mock] " __VA_ARGS__);                     \
            fputc('\n', stderr);                                        \
        }                                                               \
    } while (0)

#define MOCK_ERR(...)                                                   \
    do {                                                                \
        fprintf(stderr, "[mock][E] " __VA_ARGS__);                      \
        fputc('\n', stderr);                                            \
    } while (0)

#endif /* NVMPI_MOCK_LOG_H */
