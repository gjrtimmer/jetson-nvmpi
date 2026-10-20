/*
 * pic_fixture.h - known test pictures for the encoder tests: sample values
 * that are distinct, never 0 and differ per component and frame, and fill()
 * to write one into raw planes as NV12 or I420. Shared by test_mock_h264.c
 * and test_mock_hevc_slice.c. Header-only.
 */
#ifndef NVMPI_TEST_PIC_FIXTURE_H
#define NVMPI_TEST_PIC_FIXTURE_H

#include "mock_enc_harness.h"

/* Test input: distinct, never 0, different per component and frame. */
static uint8_t smp_y(uint32_t x, uint32_t y, uint32_t f)
{
    return (uint8_t)(1 + (x * 7 + y * 3 + f) % 254);
}

static uint8_t smp_c(unsigned c, uint32_t x, uint32_t y, uint32_t f)
{
    return (uint8_t)(1 + (x * (c == 1 ? 5 : 13) + y * (c == 1 ? 11 : 2) +
                          f * c + 17 * c * c) % 254);
}

/* Where the raw planes of a slot live: MMAP mappings, or a mapped surface. */
struct raw { uint8_t *p[3]; uint32_t stride[3]; };

/* Write frame f (w x h, even) into the planes as NV12 or I420. */
static void fill(const struct raw *r, uint32_t fmt, uint32_t w, uint32_t h,
                 uint32_t f)
{
    uint32_t x, y;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            r->p[0][y * r->stride[0] + x] = smp_y(x, y, f);
    for (y = 0; y < h / 2; y++)
        for (x = 0; x < w / 2; x++) {
            if (fmt == V4L2_PIX_FMT_NV12M) {
                r->p[1][y * r->stride[1] + 2 * x] = smp_c(1, x, y, f);
                r->p[1][y * r->stride[1] + 2 * x + 1] = smp_c(2, x, y, f);
            } else {
                r->p[1][y * r->stride[1] + x] = smp_c(1, x, y, f);
                r->p[2][y * r->stride[2] + x] = smp_c(2, x, y, f);
            }
        }
}

#endif /* NVMPI_TEST_PIC_FIXTURE_H */
