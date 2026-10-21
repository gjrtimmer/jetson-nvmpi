/*
 * pts_values.h - pts values shared by the decoder and encoder round-trip
 * cases (issue #148). The Tegra kernel stores the V4L2 buffer timestamp as
 * u64 nanoseconds, so a microsecond pts only survives it unchanged inside
 * [0, INT64_MAX / 1000]; the mock now converts exactly like the kernel.
 * Every value below must still come back unchanged from the library.
 */
#ifndef NVMPI_TEST_PTS_VALUES_H
#define NVMPI_TEST_PTS_VALUES_H

#include <stdint.h>

#define PTS_NS_MAX 9223372036854775LL    /* INT64_MAX / 1000: last safe us */

static const int64_t k_pts[] = {
    INT64_MIN,                           /* AV_NOPTS_VALUE                  */
    -1, 0, 1, 33333, 66667, 1000000, 1000001,
    INT64_MIN,                           /* a second NOPTS packet           */
    INT64_MIN + 1, -1000000, -1000001, -42,
    PTS_NS_MAX - 1, PTS_NS_MAX, PTS_NS_MAX + 1, /* edge of the u64 ns range */
    INT64_MAX / 2, INT64_MAX - 1, INT64_MAX,
    100, 200, 300,                       /* normal increasing again         */
};
#define K_PTS_N ((int)(sizeof(k_pts) / sizeof(k_pts[0])))

#endif /* NVMPI_TEST_PTS_VALUES_H */
