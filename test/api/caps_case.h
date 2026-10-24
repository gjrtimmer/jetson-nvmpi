/*
 * caps_case.h - nvmpi_get_caps() cases (#166), included by test_api.c.
 *
 * The bits are fixed by this build: the #161 EOS drain is compiled into every
 * variant, so NVMPI_CAP_DEC_EOS_DRAIN must be set (on the mock too), and the
 * value must not change between calls. Needs no device.
 */
#ifndef NVMPI_TEST_CAPS_CASE_H
#define NVMPI_TEST_CAPS_CASE_H

static int test_get_caps_drain_bit(void)
{
    unsigned int first = nvmpi_get_caps();
    int i;

    if (!(first & NVMPI_CAP_DEC_EOS_DRAIN)) {
        fprintf(stderr, "caps FAIL: NVMPI_CAP_DEC_EOS_DRAIN not set (0x%x)\n", first);
        return 1;
    }
    for (i = 0; i < 100; i++) {
        if (nvmpi_get_caps() != first) {
            fprintf(stderr, "caps FAIL: value changed between calls\n");
            return 1;
        }
    }
    return 0;
}

#endif /* NVMPI_TEST_CAPS_CASE_H */
