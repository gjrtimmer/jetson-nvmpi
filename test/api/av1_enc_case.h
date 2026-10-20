/*
 * av1_enc_case.h - hardware case for AV1 encode (#8), included by
 * test_api.c. Encodes 10 frames at 320x240 with a key frame every 5, checks
 * the packets (OBU temporal units, key flags at 0 and 5, "12 00" start, no
 * IVF "DKIF", sequence header on key frames) and decodes them again through
 * the AV1 decoder (10 frames at 320x240); see av1_enc_rt.h.
 *
 * Needs an NVENC and an NVDEC with AV1 support (Orin; Xavier has neither).
 * Outside strict mode (stubs, JetPack 5 builds, no device) it is skipped;
 * with NVMPI_TEST_STRICT=1 every shortfall is a failure. When
 * NVMPI_HAVE_AV1_ENC is 0 the case always skips. Under the mock
 * (ctest api_mock) the same case runs against the mock encoder and decoder.
 */
#ifndef NVMPI_TEST_AV1_ENC_CASE_H
#define NVMPI_TEST_AV1_ENC_CASE_H

#if NVMPI_HAVE_AV1_ENC
#include "av1_obu.h"
#include "av1_enc_rt.h"
#endif

static int test_encoder_av1_roundtrip(void)
{
#if !NVMPI_HAVE_AV1_ENC
    /* Headers without the AV1 encode controls (or NVMPI_FORCE_NO_AV1[_ENC]):
     * unsupported by construction, so skip even in strict mode. The mock
     * suite asserts the NULL create. */
    return 77;
#else
    return av1_enc_dec_roundtrip(!strict_mode);
#endif
}

#endif /* NVMPI_TEST_AV1_ENC_CASE_H */
