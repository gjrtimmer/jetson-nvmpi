/*
 * mock_env.h - environment parsing shared by the device models.
 */
#ifndef NVMPI_MOCK_ENV_H
#define NVMPI_MOCK_ENV_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* Parse a decimal env var in [lo, hi]; false when unset/invalid (the value
 * is untrusted input: digits only, whole string, bounded). */
static inline bool mock_env_uint(const char *name, uint32_t lo, uint32_t hi,
                                 uint32_t *out)
{
    const char *s = getenv(name);
    char *end = NULL;
    unsigned long v;

    if (!s || *s < '0' || *s > '9')
        return false;
    v = strtoul(s, &end, 10);
    if (*end != '\0' || v < lo || v > hi)
        return false;
    *out = (uint32_t)v;
    return true;
}

#endif /* NVMPI_MOCK_ENV_H */
