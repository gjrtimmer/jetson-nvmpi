/*
 * enc_p010_case.h - hardware case for HEVC Main10 encode from P010 input
 * (#126), included by test_api.c. Encodes 10 frames at 320x240, checks the
 * SPS of the first access unit (profile_idc 2, bit depth 10) and decodes the
 * packets again through the HEVC decoder with P010 output (see
 * enc_p010_rt.h).
 *
 * Needs an NVENC with Main10 support and, for the decode half, the NvUtils
 * buffer API (JetPack 5+). Outside strict mode (stubs, no device, no 10-bit
 * support) it is skipped; with NVMPI_TEST_STRICT=1 every shortfall is a
 * failure. Under the mock (ctest api_mock) only the encode half runs: the
 * mock decoder has no 10-bit model, the encoder suite asserts the stream.
 */
#ifndef NVMPI_TEST_ENC_P010_CASE_H
#define NVMPI_TEST_ENC_P010_CASE_H

#include "enc_p010_rt.h"

static int test_encoder_hevc_p010_roundtrip(void)
{
#ifdef NVMPI_MOCK_JPEG
    return p010_enc_run(!strict_mode, 0);
#else
    return p010_enc_run(!strict_mode, 1);
#endif
}

#endif /* NVMPI_TEST_ENC_P010_CASE_H */
