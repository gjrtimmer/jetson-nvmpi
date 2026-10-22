/*
 * enc_odd_pcm.h - read the picture back out of a mock-encoder H.264 IDR
 * access unit (#154). The mock's key frames are I_PCM (mock/mock_enc_h264.c),
 * so the samples that reached the OUTPUT plane are in the bitstream verbatim.
 * Needs "../mock/bit_read.h" and enc_odd_fixture.h first. Header-only.
 */
#ifndef NVMPI_TEST_ENC_ODD_PCM_H
#define NVMPI_TEST_ENC_ODD_PCM_H

/* The encoder codes the even-rounded picture of an odd size (4:2:0 cannot
 * signal odd sizes): 642x358, luma stride OC_YW. */
#define OC_YW ((OC_W + 1) & ~1u)
#define OC_YH ((OC_H + 1) & ~1u)

/* Extract the planes of the IDR slice: y (OC_YW x OC_YH, stride OC_YW), u/v
 * (OC_CW x OC_CH each, stride OC_CW). Returns 1 on success, 0 when the slice
 * does not parse. */
static int oc_pcm_planes(const uint8_t *au, size_t n, unsigned char *y,
                         unsigned char *u, unsigned char *v)
{
    unsigned mbw = (OC_W + 15) / 16, nmb = mbw * ((OC_H + 15) / 16), m, r, c;
    uint8_t *rb = malloc(n + 16);
    struct br b;
    size_t len;
    int ok = 0;

    if (!rb)
        return 0;
    len = nal_rbsp(au, n, 0, 5, rb, n + 16);
    b = (struct br){ rb, len, 0 };
    get_ue(&b);                                   /* first_mb_in_slice     */
    if (get_ue(&b) != 7)                          /* slice_type I          */
        goto out;
    get_ue(&b);                                   /* pps id                */
    get_bits(&b, 4);                              /* frame_num             */
    get_ue(&b);                                   /* idr_pic_id            */
    get_bits(&b, 2);                              /* ref pic marking       */
    get_ue(&b);                                   /* slice_qp_delta        */
    get_ue(&b);                                   /* deblocking idc        */
    for (m = 0; m < nmb; m++) {
        const uint8_t *mb;

        if (get_ue(&b) != 25)                     /* mb_type I_PCM         */
            goto out;
        b.pos = (b.pos + 7) & ~(size_t)7;         /* pcm alignment         */
        if (b.pos / 8 + 384 > len)
            goto out;
        mb = rb + b.pos / 8;
        for (r = 0; r < 16; r++)
            for (c = 0; c < 16; c++) {
                unsigned x = m % mbw * 16 + c, yy = m / mbw * 16 + r;

                if (x < OC_YW && yy < OC_YH)
                    y[yy * OC_YW + x] = mb[r * 16 + c];
            }
        for (r = 0; r < 8; r++)
            for (c = 0; c < 8; c++) {
                unsigned x = m % mbw * 8 + c, yc = m / mbw * 8 + r;

                if (x < OC_CW && yc < OC_CH) {     /* rest is edge padding  */
                    u[yc * OC_CW + x] = mb[256 + r * 8 + c];
                    v[yc * OC_CW + x] = mb[320 + r * 8 + c];
                }
            }
        b.pos += 384 * 8;
    }
    ok = 1;
out:
    free(rb);
    return ok;
}

#endif /* NVMPI_TEST_ENC_ODD_PCM_H */
