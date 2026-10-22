/*
 * enc_odd_case.h - hardware case for the chroma of odd frame sizes (#154),
 * included by test_api.c. Encodes one 641x357 frame (321x179 chroma) as
 * H.264 with a sentinel in the last chroma column and row (enc_odd_fixture.h),
 * decodes the access unit with the libnvmpi decoder and compares the chroma:
 *
 *   odd_chroma probe: fmt=NV12 psnr_u=.. psnr_v=.. lastcol=ok|bad
 *
 * lastcol is "ok" when the mean absolute difference over the last column and
 * the last row stays below ODD_EDGE_MAX (lossy coding, so not exact). Strict
 * mode (NVMPI_TEST_STRICT=1) fails on "bad"; outside it the probe line is the
 * result. The encoder output of YUV420 vs NV12 input is compared the same way.
 * The mock cannot run this (its decoder does not reproduce pixels): the
 * encoder mock suite covers the same input in enc_mock_oddchroma.h.
 */
#ifndef NVMPI_TEST_ENC_ODD_CASE_H
#define NVMPI_TEST_ENC_ODD_CASE_H

#include "../mock/bit_read.h"
#include "enc_odd_fixture.h"

#define ODD_EDGE_MAX 24.0                 /* mean |diff| of last col + row */
#define ODD_POOL 4

/* Encode the odd-size frame, return its access unit in *au (malloc'd).
 * 0 ok, 77 skip (encoder create failed), 1 fail. */
static int odd_encode(nvPixFormat pix, unsigned char **au, size_t *au_n)
{
    nvEncParam ep;
    nvmpictx *enc;
    nvPacket pool[ODD_POOL], *g = NULL, *tmp;
    struct oc_pic p;
    int rc = 1, i, tries;

    memset(pool, 0, sizeof(pool));
    memset(&ep, 0, sizeof(ep));
    ep.width = OC_W;
    ep.height = OC_H;
    ep.level = 41;
    ep.bitrate = 8000000;
    ep.pixFormat = pix;
    ep.iframe_interval = 5;
    ep.idr_interval = 5;
    ep.insert_spspps_idr = 1;
    ep.fps_n = 30;
    ep.fps_d = 1;
    ep.capture_num = 4;
    ep.hw_preset_type = 1;
    ep.codingType = NV_VIDEO_CodingH264;
    ep.wait_timeout = 2000;
    enc = nvmpi_create_encoder(&ep);
    if (!enc)
        return 77;
    for (i = 0; i < ODD_POOL; i++) {
        pool[i].payload = calloc(1, NVMPI_ENC_CHUNK_SIZE);
        if (!pool[i].payload)
            goto out;
        nvmpi_encoder_qEmptyPacket(enc, &pool[i]);
    }
    if (oc_build(&p, pix, OC_FULL, 1000) != 0)
        goto out2;
    if (nvmpi_encoder_put_frame(enc, &p.f) != 0)
        goto out3;
    for (tries = 0; tries < 50 && nvmpi_encoder_get_packet(enc, &g, true) != 0; tries++)
        ;
    if (!g)
        goto out3;
    *au = malloc(g->payload_size);
    if (!*au)
        goto out3;
    memcpy(*au, g->payload, g->payload_size);
    *au_n = g->payload_size;
    nvmpi_encoder_qEmptyPacket(enc, g);
    rc = 0;
out3:
    oc_free(&p);
out2:
    nvmpi_encoder_put_frame(enc, NULL);                 /* EOS */
    for (tries = 0; tries < 1000; tries++) {            /* drain to EOS */
        int gr = nvmpi_encoder_get_packet(enc, &tmp, false);

        if (gr == -2)
            break;
        if (gr == 0)
            nvmpi_encoder_qEmptyPacket(enc, tmp);
    }
out:
    while (nvmpi_encoder_dqEmptyPacket(enc, &tmp) == 0)
        ;
    if (nvmpi_encoder_close(enc) != 0)
        rc = 1;
    for (i = 0; i < ODD_POOL; i++)
        free(pool[i].payload);
    return rc;
}

/* Decode the AU to NV12 and return its chroma as two OC_CW x OC_CH planes.
 * 0 ok, 77 skip (decoder unavailable), 1 fail. */
static int odd_decode(const unsigned char *au, size_t au_n, unsigned char *u, unsigned char *v)
{
    enum { PITCH = 1024, ROWS = 512 };
    nvDecParam dp;
    nvmpictx *dec;
    nvFrame f;
    nvPacket in;
    unsigned char *y = NULL, *uv = NULL;
    int rc = 1;

    memset(&dp, 0, sizeof(dp));
    dp.codingType = NV_VIDEO_CodingH264;
    dp.pixFormat = NV_PIX_NV12;
    dp.frame_pool_size = 4;
    dp.wait_timeout = 2000;
    dec = nvmpi_create_decoder(&dp);
    if (!dec)
        return 77;
    y = calloc(PITCH, ROWS);
    uv = calloc(PITCH, ROWS);
    if (!y || !uv)
        goto out;
    memset(&in, 0, sizeof(in));
    in.payload = (unsigned char *)au;
    in.payload_size = au_n;
    in.pts = 1000;
    if (nvmpi_decoder_put_packet(dec, &in) < 0)
        goto out;
    memset(&f, 0, sizeof(f));
    f.payload[0] = y;
    f.payload[1] = uv;
    f.linesize[0] = f.linesize[1] = PITCH;
    f.payload_size[0] = f.payload_size[1] = (unsigned long)PITCH * ROWS;
    if (nvmpi_decoder_get_frame2(dec, &f, true) != 0)
        goto out;
    if (f.width != OC_W || f.height != OC_H) {
        fprintf(stderr, "odd_chroma: decoded size %ux%u\n", f.width, f.height);
        goto out;
    }
    for (unsigned r = 0; r < OC_CH; r++)
        for (unsigned x = 0; x < OC_CW; x++) {
            u[r * OC_CW + x] = uv[(size_t)r * PITCH + 2 * x];
            v[r * OC_CW + x] = uv[(size_t)r * PITCH + 2 * x + 1];
        }
    rc = 0;
out:
    free(y);
    free(uv);
    if (nvmpi_decoder_close(dec) != 0)
        rc = 1;
    return rc;
}

static int odd_chroma_run(nvPixFormat pix, const char *name)
{
    static unsigned char u[OC_CW * OC_CH], v[OC_CW * OC_CH];
    unsigned char *au = NULL;
    size_t au_n = 0;
    struct oc_cmp c;
    int ok, r = odd_encode(pix, &au, &au_n);

    if (r == 0)
        r = odd_decode(au, au_n, u, v);
    free(au);
    if (r == 77)
        return strict_mode ? 1 : 77;
    if (r)
        return r;
    oc_compare(u, v, &c);
    ok = c.edge_diff <= ODD_EDGE_MAX;
    printf("odd_chroma probe: fmt=%s psnr_u=%.1f psnr_v=%.1f lastcol=%s\n", name,
           c.psnr_u, c.psnr_v, ok ? "ok" : "bad");
    return (!ok && strict_mode) ? 1 : 0;
}

static int test_encoder_odd_chroma(void)
{
#ifdef NVMPI_MOCK_JPEG
    return 77;                    /* mock decoder: no pixels, see header */
#else
    int a = odd_chroma_run(NV_PIX_NV12, "NV12");
    int b = odd_chroma_run(NV_PIX_YUV420, "YUV420");

    if (a == 1 || b == 1)
        return 1;
    return (a == 77 && b == 77) ? 77 : 0;
#endif
}

#endif /* NVMPI_TEST_ENC_ODD_CASE_H */
