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

/* Split an Annex-B stream into access units (hardware test fixtures, #161).
 * An AU starts at the first non-VCL NAL (SPS/PPS/VPS/SEI/AUD) that follows a
 * VCL NAL, or at a VCL NAL whose first-slice flag is set (H.264
 * first_mb_in_slice == 0 and HEVC first_slice_segment_in_pic_flag are both
 * the top bit of the byte after the NAL header). VCL: H.264 types 1..5,
 * HEVC 0..31. Offsets point at the start code of each AU's first NAL
 * (a 4-byte start code's leading zero belongs to the AU). Returns the number
 * of AUs (at most max); off[i]/len[i] describe AU i. Bounds-checked,
 * allocation free. */
static inline unsigned nal_split_aus(const uint8_t *p, size_t n, int hevc,
                                     size_t *off, size_t *len, unsigned max)
{
    size_t i, start = 0;
    unsigned cnt = 0;
    int seen_vcl = 0;

    for (i = 0; i + 3 < n; i++) {
        unsigned t;
        size_t nal = i, hdr = hevc ? 2 : 1;
        int vcl, first;

        if (!(p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1))
            continue;
        if (i > 0 && p[i - 1] == 0)
            nal = i - 1;                 /* 4-byte start code */
        t = hevc ? (unsigned)((p[i + 3] >> 1) & 0x3f) : (unsigned)(p[i + 3] & 0x1f);
        vcl = hevc ? t <= 31 : (t >= 1 && t <= 5);
        first = vcl && i + 3 + hdr < n && (p[i + 3 + hdr] & 0x80);
        if (seen_vcl && (!vcl || first)) {
            if (cnt >= max)
                return cnt;
            off[cnt] = start;
            len[cnt++] = nal - start;
            start = nal;
            seen_vcl = 0;
        }
        if (vcl)
            seen_vcl = 1;
        i += 2;
    }
    if (seen_vcl && cnt < max) {
        off[cnt] = start;
        len[cnt++] = n - start;
    }
    return cnt;
}

#endif /* NVMPI_TEST_NAL_SCAN_H */
