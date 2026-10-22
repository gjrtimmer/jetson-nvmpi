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

#include "../mock/nal_scan.h"
#include "enc_odd_fixture.h"

#define ODD_EDGE_MAX 24.0                 /* mean |diff| of last col + row */
#define ODD_POOL 4

#ifndef NVMPI_MOCK_JPEG                   /* hardware only, see above */

/* Every failure prints the probe line with what is known, then a reason. */
#define ODD_FAIL(name, why, extra) \
    do { \
        printf("odd_chroma probe: fmt=%s FAIL reason=%s %s\n", (name), (why), (extra)); \
        fflush(stdout); \
    } while (0)

/* Encode the odd-size frame, then EOS, and collect EVERY packet up to EOS
 * into one Annex-B stream (*au, malloc'd): the hardware encoder may deliver
 * SPS/PPS and the IDR slice as separate packets. Counts packets in *npk.
 * 0 ok, 77 skip (encoder create failed), 1 fail (reason printed). */
static int odd_encode(nvPixFormat pix, const char *name, unsigned char **au, size_t *au_n, int *npk)
{
    nvEncParam ep;
    nvmpictx *enc;
    nvPacket pool[ODD_POOL], *g = NULL;
    struct oc_pic p;
    size_t cap = 0;
    int rc = 1, i, tries, r;

    *au = NULL;
    *au_n = 0;
    *npk = 0;
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
        if (!pool[i].payload) {
            ODD_FAIL(name, "alloc_pool", "");
            goto out;
        }
        nvmpi_encoder_qEmptyPacket(enc, &pool[i]);
    }
    if (oc_build(&p, pix, OC_FULL, 1000) != 0) {
        ODD_FAIL(name, "alloc_frame", "");
        goto out;
    }
    r = nvmpi_encoder_put_frame(enc, &p.f);
    oc_free(&p);
    if (r != 0) {
        char x[48];

        snprintf(x, sizeof(x), "put_frame=%d", r);
        ODD_FAIL(name, "put_frame", x);
        goto out;
    }
    r = nvmpi_encoder_put_frame(enc, NULL);              /* EOS */
    if (r != 0) {
        char x[48];

        snprintf(x, sizeof(x), "put_eos=%d", r);
        ODD_FAIL(name, "put_eos", x);
        goto out;
    }
    for (tries = 0; tries < 200; tries++) {              /* drain to EOS */
        r = nvmpi_encoder_get_packet(enc, &g, true);
        if (r == -2)
            break;
        if (r != 0)
            continue;
        if (*au_n + g->payload_size > cap) {
            unsigned char *n;

            cap = (*au_n + g->payload_size) * 2;
            n = realloc(*au, cap);
            if (!n) {
                ODD_FAIL(name, "alloc_au", "");
                goto out;
            }
            *au = n;
        }
        memcpy(*au + *au_n, g->payload, g->payload_size);
        *au_n += g->payload_size;
        (*npk)++;
        nvmpi_encoder_qEmptyPacket(enc, g);
    }
    if (!*au_n) {
        ODD_FAIL(name, "no_packet_from_encoder", "");
        goto out;
    }
    rc = 0;
out:
    {
        nvPacket *tmp;

        while (nvmpi_encoder_dqEmptyPacket(enc, &tmp) == 0)
            ;
    }
    if (nvmpi_encoder_close(enc) != 0 && rc == 0) {
        ODD_FAIL(name, "encoder_close", "");
        rc = 1;
    }
    for (i = 0; i < ODD_POOL; i++)
        free(pool[i].payload);
    if (rc) {
        free(*au);
        *au = NULL;
    }
    return rc;
}

/* Decode the stream to NV12 and return its chroma (top-left 321x179) as two
 * OC_CW x OC_CH planes. The decoder may report the coded size (padded) or the
 * cropped one: anything >= 641x357 is compared on its top-left corner. EOS is
 * sent after the data so a decoder that holds frames releases them.
 * 0 ok, 77 skip (decoder unavailable), 1 fail (reason printed). */
static int odd_decode(const unsigned char *au, size_t au_n, const char *name, const char *info,
                      unsigned char *u, unsigned char *v, unsigned *dw, unsigned *dh)
{
    enum { PITCH = 1024, ROWS = 512 };
    nvDecParam dp;
    nvmpictx *dec;
    nvFrame f;
    nvPacket in;
    unsigned char *y = NULL, *uv = NULL;
    char x[160];
    int rc = 1, r = -1, tries;

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
    if (!y || !uv) {
        ODD_FAIL(name, "alloc_decode_buffers", info);
        goto out;
    }
    memset(&in, 0, sizeof(in));
    in.payload = (unsigned char *)au;
    in.payload_size = au_n;
    in.pts = 1000;
    r = nvmpi_decoder_put_packet(dec, &in);
    if (r < 0) {
        snprintf(x, sizeof(x), "%s put_packet=%d", info, r);
        ODD_FAIL(name, "decoder_put_packet", x);
        goto out;
    }
    memset(&in, 0, sizeof(in));
    in.payload = (unsigned char *)au;                    /* size 0 = EOS */
    nvmpi_decoder_put_packet(dec, &in);
    for (tries = 0; tries < 3; tries++) {
        memset(&f, 0, sizeof(f));
        f.payload[0] = y;
        f.payload[1] = uv;
        f.linesize[0] = f.linesize[1] = PITCH;
        f.payload_size[0] = f.payload_size[1] = (unsigned long)PITCH * ROWS;
        r = nvmpi_decoder_get_frame2(dec, &f, true);
        if (r == 0)
            break;
        snprintf(x, sizeof(x), "%s get_frame2=%d try=%d w=%u h=%u", info, r, tries, f.width, f.height);
        fprintf(stderr, "odd_chroma: %s\n", x);
        if (r != NVMPI_ERR_FRAME_SIZE)
            break;
    }
    if (r != 0) {
        snprintf(x, sizeof(x), "%s get_frame2=%d frames=0 w=%u h=%u", info, r, f.width, f.height);
        ODD_FAIL(name, r == NVMPI_ERR_FRAME_SIZE ? "decoder_frame_size" : "decoder_no_frame", x);
        goto out;
    }
    *dw = f.width;
    *dh = f.height;
    if (f.width < OC_W || f.height < OC_H) {
        snprintf(x, sizeof(x), "%s frames=1 w=%u h=%u", info, f.width, f.height);
        ODD_FAIL(name, "decoded_size_smaller_than_source", x);
        goto out;
    }
    for (unsigned rr = 0; rr < OC_CH; rr++)
        for (unsigned xx = 0; xx < OC_CW; xx++) {
            u[rr * OC_CW + xx] = uv[(size_t)rr * PITCH + 2 * xx];
            v[rr * OC_CW + xx] = uv[(size_t)rr * PITCH + 2 * xx + 1];
        }
    rc = 0;
out:
    free(y);
    free(uv);
    if (nvmpi_decoder_close(dec) != 0 && rc == 0) {
        ODD_FAIL(name, "decoder_close", info);
        rc = 1;
    }
    return rc;
}

static int odd_chroma_run(nvPixFormat pix, const char *name)
{
    static unsigned char u[OC_CW * OC_CH], v[OC_CW * OC_CH];
    unsigned char *au = NULL;
    size_t au_n = 0;
    unsigned dw = 0, dh = 0;
    struct oc_cmp c;
    char info[96];
    int ok, npk = 0, r = odd_encode(pix, name, &au, &au_n, &npk);

    if (r == 0) {
        snprintf(info, sizeof(info), "au_bytes=%zu packets=%d sps=%d pps=%d idr=%d", au_n, npk,
                 nal_has(au, au_n, 0, 7), nal_has(au, au_n, 0, 8), nal_has(au, au_n, 0, 5));
        r = odd_decode(au, au_n, name, info, u, v, &dw, &dh);
    }
    free(au);
    if (r == 77) {
        ODD_FAIL(name, "encoder_or_decoder_unavailable", strict_mode ? "(strict)" : "(skipped)");
        return strict_mode ? 1 : 77;
    }
    if (r)
        return r;
    oc_compare(u, v, &c);
    ok = c.edge_diff <= ODD_EDGE_MAX;
    printf("odd_chroma probe: fmt=%s psnr_u=%.1f psnr_v=%.1f lastcol=%s edge_diff=%.1f dec=%ux%u %s\n",
           name, c.psnr_u, c.psnr_v, ok ? "ok" : "bad", c.edge_diff, dw, dh, info);
    return (!ok && strict_mode) ? 1 : 0;
}

#endif /* !NVMPI_MOCK_JPEG */

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
