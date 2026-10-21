/*
 * av1_p010_case.h - hardware case for AV1 encode from P010 input (#146),
 * included by test_api.c. Encodes 10 frames at 320x240 as AV1, parses the
 * sequence header of the first packet (seq_profile 0, high_bitdepth 1) and
 * decodes the packets through the AV1 decoder with P010 output; see
 * av1_p010_rt.h. The case prints "av1_p010 probe: ..." so the pipeline log
 * records how the Orin driver behaved.
 *
 * It was not known whether the Orin's AV1 encoder takes a P010M OUTPUT
 * plane. In strict mode (NVMPI_TEST_STRICT=1, the Orin MR job) a refused
 * create is a FAILURE on purpose, with the probe line in the log: the run
 * tells us whether to keep this path or switch to a "refused cleanly" case.
 * Outside strict mode (stubs, no device) it skips. Under the mock
 * (ctest api_mock) only the encode half runs: the mock decoder has no 10-bit
 * model; the encoder suite asserts the sequence header.
 */
#ifndef NVMPI_TEST_AV1_P010_CASE_H
#define NVMPI_TEST_AV1_P010_CASE_H

#if NVMPI_HAVE_AV1_ENC
#include "av1_obu.h"
#include "av1_p010_rt.h"
#endif

static int test_encoder_av1_p010_roundtrip(void)
{
#if !NVMPI_HAVE_AV1_ENC
    return 77;      /* headers without the AV1 encode controls: unsupported */
#elif defined(NVMPI_MOCK_JPEG)
    return av1_p010_run(!strict_mode, 0);
#else
    return av1_p010_run(!strict_mode, 1);
#endif
}

#endif /* NVMPI_TEST_AV1_P010_CASE_H */
