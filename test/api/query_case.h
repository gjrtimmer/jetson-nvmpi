/*
 * query_case.h - nvmpi_query_codec() cases (#159), included by test_api.c.
 *
 * Device-free cases (argument checks, the build table) run everywhere. The
 * hardware case runs on the real module (NVMPI_TEST_STRICT=1) and on the mock
 * (default Orin NX matrix); on stubs it is skipped. It only spot-checks the
 * query against a handful of creates, one per codec, never in a loop: the
 * Tegra driver crashes on rapid open/close cycles. The full matrix, the
 * probe cache and the fault cases are mock-only (query_mock_case.h).
 *
 * Expectations that depend on the build use the same macros the library is
 * built with (NVMPI_HAVE_AV1, NVMPI_HAVE_AV1_ENC), so a JetPack 5 or a
 * NVMPI_FORCE_NO_AV1 build expects UNSUPPORTED_BUILD for AV1.
 */
#ifndef NVMPI_TEST_QUERY_CASE_H
#define NVMPI_TEST_QUERY_CASE_H

#define QC_BUILD NVMPI_CODEC_UNSUPPORTED_BUILD
#define QC_HW    NVMPI_CODEC_UNSUPPORTED_HW
#define QC_OK    NVMPI_CODEC_SUPPORTED
/* What the build table says about AV1 when the hardware is not asked. */
#define QC_AV1_DEC_OK (NVMPI_HAVE_AV1 ? QC_OK : QC_BUILD)
#define QC_AV1_ENC_OK (NVMPI_HAVE_AV1_ENC ? QC_OK : QC_BUILD)

/* Compare one answer; prints and returns 1 on a mismatch. */
static int qc_check(nvCodingType t, int enc, nvPixFormat p, int want)
{
    int got = nvmpi_query_codec(t, enc, p);

    if (got == want)
        return 0;
    fprintf(stderr, "query FAIL: codec %d enc %d pix %d: got %d, want %d\n",
            (int)t, enc, (int)p, got, want);
    return 1;
}

/* 1 when the matching create call returns a handle (closed again at once). */
static int qc_create_ok(nvCodingType t, int enc, nvPixFormat p)
{
    nvmpictx *ctx;

    if (t == NV_VIDEO_CodingMJPEG) {
        if (enc) {
            ctx = nvmpi_create_jpeg_encoder(90);
            if (ctx)
                nvmpi_jpeg_encoder_close(ctx);
        } else {
            ctx = nvmpi_create_jpeg_decoder_fmt(p, 0);
            if (ctx)
                nvmpi_jpeg_decoder_close(ctx);
        }
        return ctx != NULL;
    }
    if (enc) {
        nvEncParam ep;

        enc_param_init(&ep);
        ep.codingType = t;
        ep.pixFormat = p;
        ctx = nvmpi_create_encoder(&ep);
        if (ctx)
            nvmpi_encoder_close(ctx);
    } else {
        nvDecParam dp;

        dec_param_init(&dp, t);
        dp.pixFormat = p;
        ctx = nvmpi_create_decoder(&dp);
        if (ctx)
            nvmpi_decoder_close(ctx);
    }
    return ctx != NULL;
}

/* Bad arguments give NVMPI_ERR_INVAL before anything else. */
static int test_query_invalid_args(void)
{
    int bad = 0;

    bad += qc_check(NV_VIDEO_CodingUnused, 0, NV_PIX_NV12, NVMPI_ERR_INVAL);
    bad += qc_check(NV_VIDEO_CodingUnused, 1, NV_PIX_NV12, NVMPI_ERR_INVAL);
    bad += qc_check((nvCodingType)(NV_VIDEO_CodingAV1 + 1), 0, NV_PIX_NV12, NVMPI_ERR_INVAL);
    bad += qc_check((nvCodingType)99, 1, NV_PIX_NV12, NVMPI_ERR_INVAL);
    bad += qc_check((nvCodingType)-1, 0, NV_PIX_NV12, NVMPI_ERR_INVAL);
    bad += qc_check(NV_VIDEO_CodingH264, 2, NV_PIX_NV12, NVMPI_ERR_INVAL);
    bad += qc_check(NV_VIDEO_CodingH264, -1, NV_PIX_NV12, NVMPI_ERR_INVAL);
    bad += qc_check(NV_VIDEO_CodingH264, 0, (nvPixFormat)3, NVMPI_ERR_INVAL);
    bad += qc_check(NV_VIDEO_CodingH264, 1, (nvPixFormat)-1, NVMPI_ERR_INVAL);
    bad += qc_check(NV_VIDEO_CodingMJPEG, 1, (nvPixFormat)77, NVMPI_ERR_INVAL);
    return bad ? 1 : 0;
}

/* Answers the build table gives on its own: they must come back without any
 * device (this also runs on stubs, where no node exists). */
static int test_query_build_table(void)
{
    int bad = 0;

    /* Decode-only codecs cannot encode. */
    bad += qc_check(NV_VIDEO_CodingVP9, 1, NV_PIX_NV12, QC_BUILD);
    bad += qc_check(NV_VIDEO_CodingVP8, 1, NV_PIX_YUV420, QC_BUILD);
    bad += qc_check(NV_VIDEO_CodingMPEG2, 1, NV_PIX_NV12, QC_BUILD);
    bad += qc_check(NV_VIDEO_CodingMPEG4, 1, NV_PIX_NV12, QC_BUILD);
    /* P010 encode is HEVC only (create refuses H.264 and AV1). */
    bad += qc_check(NV_VIDEO_CodingH264, 1, NV_PIX_P010, QC_BUILD);
    bad += qc_check(NV_VIDEO_CodingAV1, 1, NV_PIX_P010, QC_BUILD);
    /* P010 decode is HEVC Main10 only. */
    bad += qc_check(NV_VIDEO_CodingH264, 0, NV_PIX_P010, QC_BUILD);
    bad += qc_check(NV_VIDEO_CodingVP9, 0, NV_PIX_P010, QC_BUILD);
    /* JPEG codes 8-bit 4:2:0 only. */
    bad += qc_check(NV_VIDEO_CodingMJPEG, 0, NV_PIX_P010, QC_BUILD);
    bad += qc_check(NV_VIDEO_CodingMJPEG, 1, NV_PIX_P010, QC_BUILD);
    /* AV1 follows the headers this build was made with. */
    if (!NVMPI_HAVE_AV1) {
        bad += qc_check(NV_VIDEO_CodingAV1, 0, NV_PIX_NV12, QC_BUILD);
        bad += qc_check(NV_VIDEO_CodingAV1, 0, NV_PIX_YUV420, QC_BUILD);
    }
    if (!NVMPI_HAVE_AV1_ENC) {
        bad += qc_check(NV_VIDEO_CodingAV1, 1, NV_PIX_NV12, QC_BUILD);
        bad += qc_check(NV_VIDEO_CodingAV1, 1, NV_PIX_YUV420, QC_BUILD);
    }
    return bad ? 1 : 0;
}

/* Real module (strict) or mock default matrix: the query agrees with what
 * create does for a few codecs, and decode lists every codec. */
static int test_query_hw_vs_create(void)
{
    static const nvCodingType dec[] = {
        NV_VIDEO_CodingH264, NV_VIDEO_CodingHEVC, NV_VIDEO_CodingMPEG2,
        NV_VIDEO_CodingMPEG4, NV_VIDEO_CodingVP8, NV_VIDEO_CodingVP9,
    };
    const char *variant = getenv("JETSON_VARIANT");
    /* The Orin Nano has no NVENC: its encoders are UNSUPPORTED_HW. */
    int nano = variant && strstr(variant, "nano");
    int bad = 0;
    size_t i;

#ifndef NVMPI_MOCK_JPEG
    if (!strict_mode)
        return 77;                  /* stubs: no device to ask */
#endif
    for (i = 0; i < sizeof(dec) / sizeof(dec[0]); i++)
        bad += qc_check(dec[i], 0, NV_PIX_NV12, QC_OK);
    bad += qc_check(NV_VIDEO_CodingAV1, 0, NV_PIX_NV12, QC_AV1_DEC_OK);
    bad += qc_check(NV_VIDEO_CodingH264, 1, NV_PIX_NV12, nano ? QC_HW : QC_OK);
    bad += qc_check(NV_VIDEO_CodingHEVC, 1, NV_PIX_YUV420, nano ? QC_HW : QC_OK);
    bad += qc_check(NV_VIDEO_CodingAV1, 1, NV_PIX_NV12,
                    nano ? (NVMPI_HAVE_AV1_ENC ? QC_HW : QC_BUILD) : QC_AV1_ENC_OK);
    /* Spot-check against create: one per direction and codec family. */
    if (!qc_create_ok(NV_VIDEO_CodingH264, 0, NV_PIX_NV12)) {
        fprintf(stderr, "query FAIL: H.264 decode SUPPORTED but create failed\n");
        bad++;
    }
    if (NVMPI_HAVE_AV1 && !qc_create_ok(NV_VIDEO_CodingAV1, 0, NV_PIX_NV12)) {
        fprintf(stderr, "query FAIL: AV1 decode SUPPORTED but create failed\n");
        bad++;
    }
    if (qc_create_ok(NV_VIDEO_CodingH264, 1, NV_PIX_NV12) == nano) {
        fprintf(stderr, "query FAIL: H.264 encode answer and create disagree (nano=%d)\n", nano);
        bad++;
    }
    return bad ? 1 : 0;
}

#endif /* NVMPI_TEST_QUERY_CASE_H */
