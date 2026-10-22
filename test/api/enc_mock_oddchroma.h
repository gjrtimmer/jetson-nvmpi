/*
 * enc_mock_oddchroma.h - odd-size chroma cases of test_api_enc_mock.c (#154).
 * Included from enc_mock_cases.h after enc_mock_util.h.
 *
 * A 641x357 frame (321x179 chroma) goes in through put_frame; the mock's
 * H.264 key frame is I_PCM, so the chroma that reached the OUTPUT plane is
 * read back out of the access unit (enc_odd_pcm.h) and compared sample by
 * sample. Old behaviour: the last chroma column and row never arrive.
 * Covered: the full geometry (NV12 and YUV420, payload declared exactly or
 * left 0), a caller that declares only the floored chroma rows (older
 * wrappers: accepted, copied as before), an undersized payload (refused), and
 * a driver that floors its chroma plane (the library must not overflow it).
 */
#ifndef NVMPI_TEST_ENC_MOCK_ODDCHROMA_H
#define NVMPI_TEST_ENC_MOCK_ODDCHROMA_H

#include "../mock/bit_read.h"
#include "enc_odd_fixture.h"
#include "enc_odd_pcm.h"

/* Chroma planes of the first (IDR) access unit. */
struct oc_chroma { unsigned char u[OC_CW * OC_CH], v[OC_CW * OC_CH]; };

/* Encode one frame and read its chroma back. Returns the put_frame result
 * when it is not 0; else 0 with *out filled (exits through REQUIRE when the
 * packet does not arrive or does not parse). */
static int oc_encode(nvmpictx *ctx, struct oc_pic *p, struct oc_chroma *out)
{
    nvPacket *pk = NULL;
    int r = nvmpi_encoder_put_frame(ctx, &p->f);

    if (r)
        return r;
    REQUIRE(nvmpi_encoder_get_packet(ctx, &pk, true) == 0);
    REQUIRE(oc_pcm_chroma(pk->payload, pk->payload_size, out->u, out->v));
    nvmpi_encoder_qEmptyPacket(ctx, pk);
    return 0;
}

/* Samples [0, cw) x [0, rows) of the coded chroma equal the source. */
static int oc_equal(const struct oc_chroma *c, unsigned cw, unsigned rows)
{
    for (unsigned y = 0; y < rows; y++)
        for (unsigned x = 0; x < cw; x++)
            if (c->u[y * OC_CW + x] != oc_u(x, y) ||
                c->v[y * OC_CW + x] != oc_v(x, y))
                return 0;
    return 1;
}

static int oc_run(nvPixFormat pix, enum oc_mode mode)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvmpictx *ctx = e_create_sz(&c, E_NPKT, OC_W, OC_H, pix);
    static struct oc_chroma out;
    struct oc_pic p;

    REQUIRE(ctx);
    REQUIRE(oc_build(&p, pix, mode, 1) == 0);
    REQUIRE(oc_encode(ctx, &p, &out) == 0);
    if (mode == OC_FLOORED) {
        /* floored payload: copied as before, the last column/row stay out */
        REQUIRE(oc_equal(&out, OC_CW - 1, OC_CH - 1));
    } else {
        REQUIRE(oc_equal(&out, OC_CW, OC_CH));
    }
    oc_free(&p);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

static int odd_chroma_nv12(void)    { return oc_run(NV_PIX_NV12, OC_FULL); }
static int odd_chroma_yuv420(void)  { return oc_run(NV_PIX_YUV420, OC_FULL); }
static int odd_chroma_nv12_zero(void)   { return oc_run(NV_PIX_NV12, OC_ZERO); }
static int odd_chroma_yuv420_zero(void) { return oc_run(NV_PIX_YUV420, OC_ZERO); }
static int odd_chroma_nv12_floored(void)   { return oc_run(NV_PIX_NV12, OC_FLOORED); }
static int odd_chroma_yuv420_floored(void) { return oc_run(NV_PIX_YUV420, OC_FLOORED); }

/* Declared chroma below even the floored need: refused (repeatedly, so no
 * OUTPUT slot is consumed), a good frame then encodes. */
static int odd_chroma_under(void)
{
    struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
    nvmpictx *ctx = e_create_sz(&c, E_NPKT, OC_W, OC_H, NV_PIX_YUV420);
    static struct oc_chroma out;
    struct oc_pic bad, good;

    REQUIRE(ctx);
    REQUIRE(oc_build(&bad, NV_PIX_YUV420, OC_UNDER, 1) == 0);
    for (int i = 0; i < 6; i++)
        REQUIRE(nvmpi_encoder_put_frame(ctx, &bad.f) == NVMPI_ERR_INVAL);
    oc_free(&bad);
    REQUIRE(oc_build(&good, NV_PIX_YUV420, OC_FULL, 2) == 0);
    REQUIRE(oc_encode(ctx, &good, &out) == 0);
    REQUIRE(oc_equal(&out, OC_CW, OC_CH));
    oc_free(&good);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* A driver whose OUTPUT chroma plane is floored cannot take the last column
 * and row: the library must fall back to the floored copy (no overflow, ASan
 * watches), the frame still encodes with the rest of the chroma intact. */
static int odd_chroma_driver_floored(void)
{
    static const nvPixFormat fmt[2] = { NV_PIX_NV12, NV_PIX_YUV420 };

    setenv("NVMPI_MOCK_ENC_FLOOR_CHROMA", "1", 1);
    for (int k = 0; k < 2; k++) {
        struct ecfg c = { NV_VIDEO_CodingH264, 0, 0, 0, 0, 0 };
        nvmpictx *ctx = e_create_sz(&c, E_NPKT, OC_W, OC_H, fmt[k]);
        static struct oc_chroma out;
        struct oc_pic p;

        REQUIRE(ctx);
        REQUIRE(oc_build(&p, fmt[k], OC_FULL, 1) == 0);
        REQUIRE(oc_encode(ctx, &p, &out) == 0);
        REQUIRE(oc_equal(&out, OC_CW - 1, OC_CH - 1));
        oc_free(&p);
        REQUIRE(e_destroy(ctx, NULL) == 0);
    }
    return 0;
}

#endif /* NVMPI_TEST_ENC_MOCK_ODDCHROMA_H */
