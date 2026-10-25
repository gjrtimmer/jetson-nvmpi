/*
 * query_mock_case.h - nvmpi_query_codec() against the mock (#159), included
 * by test_api.c in mock builds only. Needs query_case.h first.
 *
 * Every case runs in its own forked child (RUN_TEST), and the probe cache is
 * per process, so each case starts with an empty cache and sets its own mock
 * environment: NVMPI_MOCK_CODECS (the ENUM_FMT matrix), NVMPI_MOCK_FAULT
 * (open@N: node gone, enumfmt@N: listing fails) and NVMPI_MOCK_NO_JPEGENC.
 * Together they produce every return value: 0, 1, 2, NVMPI_ERR_INVAL and -1.
 */
#ifndef NVMPI_TEST_QUERY_MOCK_CASE_H
#define NVMPI_TEST_QUERY_MOCK_CASE_H

/* Set (or clear, with NULL) the three mock switches for this child. */
static void qm_env(const char *codecs, const char *fault, int no_jpegenc)
{
    if (codecs) setenv("NVMPI_MOCK_CODECS", codecs, 1); else unsetenv("NVMPI_MOCK_CODECS");
    if (fault) setenv("NVMPI_MOCK_FAULT", fault, 1); else unsetenv("NVMPI_MOCK_FAULT");
    if (no_jpegenc) setenv("NVMPI_MOCK_NO_JPEGENC", "1", 1); else unsetenv("NVMPI_MOCK_NO_JPEGENC");
}

/* NvUtils builds (nvbufsurface.h present) have P010 decode. */
#if defined(__has_include)
#if __has_include("nvbufsurface.h")
#define QM_P010_DEC QC_OK
#endif
#endif
#ifndef QM_P010_DEC
#define QM_P010_DEC QC_BUILD
#endif

/* Default matrix = Orin NX: everything the build allows is SUPPORTED. */
static int test_query_mock_default_matrix(void)
{
    static const nvCodingType dec[] = {
        NV_VIDEO_CodingH264, NV_VIDEO_CodingHEVC, NV_VIDEO_CodingMPEG2,
        NV_VIDEO_CodingMPEG4, NV_VIDEO_CodingVP8, NV_VIDEO_CodingVP9,
    };
    int bad = 0;
    size_t i;

    qm_env(NULL, NULL, 0);
    for (i = 0; i < sizeof(dec) / sizeof(dec[0]); i++) {
        bad += qc_check(dec[i], 0, NV_PIX_NV12, QC_OK);
        bad += qc_check(dec[i], 0, NV_PIX_YUV420, QC_OK);
    }
    bad += qc_check(NV_VIDEO_CodingAV1, 0, NV_PIX_NV12, QC_AV1_DEC_OK);
    bad += qc_check(NV_VIDEO_CodingH264, 1, NV_PIX_NV12, QC_OK);
    bad += qc_check(NV_VIDEO_CodingHEVC, 1, NV_PIX_YUV420, QC_OK);
    bad += qc_check(NV_VIDEO_CodingAV1, 1, NV_PIX_NV12, QC_AV1_ENC_OK);
    bad += qc_check(NV_VIDEO_CodingHEVC, 1, NV_PIX_P010, QC_OK);
    bad += qc_check(NV_VIDEO_CodingHEVC, 0, NV_PIX_P010, QM_P010_DEC);
    bad += qc_check(NV_VIDEO_CodingMJPEG, 1, NV_PIX_YUV420, QC_OK);
    bad += qc_check(NV_VIDEO_CodingMJPEG, 1, NV_PIX_NV12, QC_OK);
    bad += qc_check(NV_VIDEO_CodingMJPEG, 0, NV_PIX_NV12, QC_OK);
    bad += qc_check(NV_VIDEO_CodingMJPEG, 0, NV_PIX_YUV420, QC_OK);
    return bad ? 1 : 0;
}

/* A restricted matrix: what the node does not list is UNSUPPORTED_HW, as long
 * as the build could do it (BUILD answers do not depend on the node). */
static int test_query_mock_hw_matrix(void)
{
    int bad = 0;

    qm_env("dec:h264,hevc;enc:h264", NULL, 0);
    bad += qc_check(NV_VIDEO_CodingH264, 0, NV_PIX_NV12, QC_OK);
    bad += qc_check(NV_VIDEO_CodingHEVC, 0, NV_PIX_NV12, QC_OK);
    bad += qc_check(NV_VIDEO_CodingVP9, 0, NV_PIX_NV12, QC_HW);
    bad += qc_check(NV_VIDEO_CodingMPEG2, 0, NV_PIX_NV12, QC_HW);
    bad += qc_check(NV_VIDEO_CodingAV1, 0, NV_PIX_NV12, NVMPI_HAVE_AV1 ? QC_HW : QC_BUILD);
    bad += qc_check(NV_VIDEO_CodingH264, 1, NV_PIX_NV12, QC_OK);
    bad += qc_check(NV_VIDEO_CodingHEVC, 1, NV_PIX_NV12, QC_HW);
    bad += qc_check(NV_VIDEO_CodingAV1, 1, NV_PIX_NV12, NVMPI_HAVE_AV1_ENC ? QC_HW : QC_BUILD);
    bad += qc_check(NV_VIDEO_CodingVP9, 1, NV_PIX_NV12, QC_BUILD);
    return bad ? 1 : 0;
}

/* The node is gone (open fails ENODEV): HW, cached per direction. The first
 * open is the decode probe; encode opens a second time and works. */
static int test_query_mock_open_fault(void)
{
    int bad = 0;

    qm_env(NULL, "open@1", 0);
    bad += qc_check(NV_VIDEO_CodingH264, 0, NV_PIX_NV12, QC_HW);
    bad += qc_check(NV_VIDEO_CodingHEVC, 0, NV_PIX_NV12, QC_HW);   /* cached, no reopen */
    bad += qc_check(NV_VIDEO_CodingH264, 1, NV_PIX_NV12, QC_OK);
    return bad ? 1 : 0;
}

/* No NVJPG encoder (Orin Nano): JPEG encode is HW, JPEG decode unaffected,
 * and the video codecs do not care. */
static int test_query_mock_jpeg_hw(void)
{
    int bad = 0;

    qm_env(NULL, NULL, 1);
    bad += qc_check(NV_VIDEO_CodingMJPEG, 1, NV_PIX_YUV420, QC_HW);
    bad += qc_check(NV_VIDEO_CodingMJPEG, 1, NV_PIX_NV12, QC_HW);   /* cached */
    bad += qc_check(NV_VIDEO_CodingMJPEG, 0, NV_PIX_NV12, QC_OK);
    bad += qc_check(NV_VIDEO_CodingH264, 1, NV_PIX_NV12, QC_OK);
    if (qc_create_ok(NV_VIDEO_CodingMJPEG, 1, NV_PIX_YUV420)) {
        fprintf(stderr, "query FAIL: JPEG encode HW but create succeeded\n");
        bad++;
    }
    return bad ? 1 : 0;
}

/* -1: the probe learned nothing, so nothing is cached and the next call asks
 * again. The fault is a failing ENUM_FMT at the first call, at a later call,
 * and a node that lists no coded format at all (encode, after decode is
 * cached). */
static int test_query_mock_inconclusive(void)
{
    int bad = 0;

    qm_env(NULL, "enumfmt@1", 0);
    bad += qc_check(NV_VIDEO_CodingH264, 0, NV_PIX_NV12, -1);
    bad += qc_check(NV_VIDEO_CodingH264, 0, NV_PIX_NV12, QC_OK);   /* retried, now conclusive */
    qm_env(NULL, "enumfmt@3", 0);                                 /* counters restart: encode */
    bad += qc_check(NV_VIDEO_CodingH264, 1, NV_PIX_NV12, -1);
    qm_env("enc:", NULL, 0);                                      /* empty list: index 0 EINVAL */
    bad += qc_check(NV_VIDEO_CodingH264, 1, NV_PIX_NV12, -1);
    qm_env(NULL, NULL, 0);
    bad += qc_check(NV_VIDEO_CodingH264, 1, NV_PIX_NV12, QC_OK);
    bad += qc_check(NV_VIDEO_CodingVP8, 0, NV_PIX_NV12, QC_OK);        /* decode: cached since step 1 */
    return bad ? 1 : 0;
}

/* After the first conclusive answer a direction is never opened again: with
 * every later open failing and the matrix emptied, decode still answers from
 * the cache while the not yet probed encode direction sees the dead node. */
static int test_query_mock_cache(void)
{
    int bad = 0;

    qm_env(NULL, NULL, 0);
    bad += qc_check(NV_VIDEO_CodingH264, 0, NV_PIX_NV12, QC_OK);
    qm_env("dec:;enc:", "open@1+", 0);
    bad += qc_check(NV_VIDEO_CodingHEVC, 0, NV_PIX_NV12, QC_OK);
    bad += qc_check(NV_VIDEO_CodingVP9, 0, NV_PIX_YUV420, QC_OK);
    bad += qc_check(NV_VIDEO_CodingH264, 1, NV_PIX_NV12, QC_HW);
    bad += qc_check(NV_VIDEO_CodingHEVC, 1, NV_PIX_NV12, QC_HW);
    return bad ? 1 : 0;
}

/* BUILD answers never touch a device: with every open failing (which would
 * read as HW) they still come back as BUILD. */
static int test_query_mock_build_no_device(void)
{
    int bad = 0;

    qm_env("dec:;enc:", "open@1+,enumfmt@1+", 0);
    bad += qc_check(NV_VIDEO_CodingVP9, 1, NV_PIX_NV12, QC_BUILD);
    bad += qc_check(NV_VIDEO_CodingH264, 1, NV_PIX_P010, QC_BUILD);
    bad += qc_check(NV_VIDEO_CodingAV1, 1, NV_PIX_P010, QC_BUILD);
    bad += qc_check(NV_VIDEO_CodingH264, 0, NV_PIX_P010, QC_BUILD);
    bad += qc_check(NV_VIDEO_CodingMJPEG, 1, NV_PIX_P010, QC_BUILD);
    bad += qc_check(NV_VIDEO_CodingMJPEG, 0, NV_PIX_P010, QC_BUILD);
    if (!NVMPI_HAVE_AV1)
        bad += qc_check(NV_VIDEO_CodingAV1, 0, NV_PIX_NV12, QC_BUILD);
    if (!NVMPI_HAVE_AV1_ENC)
        bad += qc_check(NV_VIDEO_CodingAV1, 1, NV_PIX_NV12, QC_BUILD);
    /* ... while a video answer does see the dead node. */
    bad += qc_check(NV_VIDEO_CodingH264, 0, NV_PIX_NV12, QC_HW);
    return bad ? 1 : 0;
}

/* Query and create agree over the whole argument space (default matrix):
 * every SUPPORTED creates, every BUILD is refused by create. Two answers have
 * no create-side refusal and are skipped: decode P010 for non-HEVC codecs
 * (create takes the request, the contract is HEVC Main10 only) and JPEG
 * encode at P010 (nvmpi_create_jpeg_encoder has no pixel format argument). */
static int test_query_mock_consistency(void)
{
    int bad = 0, t, enc, p;

    qm_env(NULL, NULL, 0);
    for (t = NV_VIDEO_CodingH264; t <= NV_VIDEO_CodingAV1; t++) {
        for (enc = 0; enc <= 1; enc++) {
            for (p = NV_PIX_NV12; p <= NV_PIX_P010; p++) {
                int r = nvmpi_query_codec((nvCodingType)t, enc, (nvPixFormat)p);
                int skip_build = (!enc && p == NV_PIX_P010 && t != NV_VIDEO_CodingHEVC &&
                                  t != NV_VIDEO_CodingMJPEG) ||
                                 (enc && p == NV_PIX_P010 && t == NV_VIDEO_CodingMJPEG);

                if (r == QC_OK && !qc_create_ok((nvCodingType)t, enc, (nvPixFormat)p)) {
                    fprintf(stderr, "query FAIL: SUPPORTED but create failed: codec %d enc %d pix %d\n", t, enc, p);
                    bad++;
                } else if (r == QC_BUILD && !skip_build &&
                           qc_create_ok((nvCodingType)t, enc, (nvPixFormat)p)) {
                    fprintf(stderr, "query FAIL: BUILD but create succeeded: codec %d enc %d pix %d\n", t, enc, p);
                    bad++;
                } else if (r != QC_OK && r != QC_BUILD) {
                    fprintf(stderr, "query FAIL: unexpected %d on the default matrix: codec %d enc %d pix %d\n", r, t, enc, p);
                    bad++;
                }
            }
        }
    }
    return bad ? 1 : 0;
}

#endif /* NVMPI_TEST_QUERY_MOCK_CASE_H */
