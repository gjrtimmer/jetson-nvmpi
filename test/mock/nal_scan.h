/*
 * nal_scan.h - list the NAL unit types of an Annex-B buffer (header-only).
 * Used by the encoder tests to check what the mock encoder emitted. The
 * buffer is checked, not trusted: every read is bounds-checked and nothing
 * is allocated. Start codes are 00 00 01 (a leading zero byte is part of
 * the 4-byte form and is skipped naturally).
 */
#ifndef NVMPI_TEST_NAL_SCAN_H
#define NVMPI_TEST_NAL_SCAN_H

#include <stddef.h>
#include <stdint.h>

/* Write up to max NAL types (H.264: header & 0x1f; HEVC: header >> 1 & 0x3f)
 * into out; returns the number found. */
static inline unsigned nal_types(const uint8_t *p, size_t n, int hevc,
                                 unsigned *out, unsigned max)
{
    size_t i;
    unsigned cnt = 0;

    for (i = 0; i + 3 < n && cnt < max; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            uint8_t h = p[i + 3];

            out[cnt++] = hevc ? (unsigned)((h >> 1) & 0x3f) : (unsigned)(h & 0x1f);
            i += 3;
        }
    }
    return cnt;
}

/* 1 when the AU holds a NAL of type t. */
static inline int nal_has(const uint8_t *p, size_t n, int hevc, unsigned t)
{
    unsigned ty[16], c = nal_types(p, n, hevc, ty, 16), i;

    for (i = 0; i < c; i++)
        if (ty[i] == t)
            return 1;
    return 0;
}

#endif /* NVMPI_TEST_NAL_SCAN_H */
