/*
 * enc_p010_rt.h - HEVC Main10 encode from P010 input (#126), shared by the
 * hardware case in test_api.c (enc_p010_case.h) and the encoder mock suite
 * (enc_mock_p010.h). Header-only; needs <nvmpi.h>, <stdio.h>, <stdlib.h>,
 * <string.h> included first.
 *
 * p010_enc_run() encodes P010_FRAMES 320x240 P010 frames as HEVC, checks
 * that the first access unit carries an SPS that says Main10 and 10-bit
 * (general_profile_idc 2, bit_depth_luma/chroma 10), and optionally decodes
 * every packet back through the HEVC decoder (P010 output) expecting one
 * 320x240 frame per packet. Return: 0 pass, 77 skipped (create failed and
 * the caller says that is acceptable), 1 fail; every failure names the check
 * on stderr.
 */
#ifndef NVMPI_TEST_ENC_P010_RT_H
#define NVMPI_TEST_ENC_P010_RT_H

#include "../mock/bit_read.h"

#define P010_W 320u
#define P010_H 240u
#define P010_FRAMES 10
#define P010_POOL 8
#define P010_FAIL(m) do { fprintf(stderr, "p010_rt FAIL %s:%d: %s\n", __func__, __LINE__, (m)); rc = 1; goto out; } while (0)

struct p010_sps { unsigned profile_idc, bit_luma, bit_chroma; };

/* Fields of the first SPS in an Annex-B access unit (H.265 7.3.2.2): the
 * profile_tier_level is 96 bits for sps_max_sub_layers_minus1 == 0, which
 * is what both the mock and the Tegra encoder write for a single layer.
 * Returns 1 when an SPS was found and parsed. */
static int p010_parse_sps(const uint8_t *p, size_t n, struct p010_sps *o)
{
    uint8_t rb[128];
    size_t len = nal_rbsp(p, n, 1, 33, rb, sizeof(rb));
    struct br r = { rb, len, 0 };

    if (len < 16)
        return 0;
    get_bits(&r, 8);                            /* vps id, sub layers, nesting */
    get_bits(&r, 3);                            /* profile_space, tier         */
    o->profile_idc = get_bits(&r, 5);
    r.pos += 88;                                /* flags + constraints + level */
    get_ue(&r);                                 /* sps id                      */
    if (get_ue(&r) == 3)
        get_bits(&r, 1);                        /* separate_colour_plane       */
    get_ue(&r);                                 /* width                       */
    get_ue(&r);                                 /* height                      */
    if (get_bits(&r, 1)) {                      /* conformance_window          */
        get_ue(&r); get_ue(&r); get_ue(&r); get_ue(&r);
    }
    o->bit_luma = get_ue(&r) + 8;
    o->bit_chroma = get_ue(&r) + 8;
    return 1;
}

static void p010_fill_param(nvEncParam *p, nvCodingType codec, nvPixFormat pix,
                            unsigned profile)
{
    memset(p, 0, sizeof(*p));
    p->width = P010_W;
    p->height = P010_H;
    p->profile = profile;
    p->level = 41;
    p->bitrate = 2000000;
    p->pixFormat = pix;
    p->iframe_interval = 5;
    p->idr_interval = 5;
    p->fps_n = 30;
    p->fps_d = 1;
    p->capture_num = 4;
    p->hw_preset_type = 1;
    p->codingType = codec;
    p->wait_timeout = 2000;
}

/* P010LE frame: 16-bit samples with the 10 significant bits in the high
 * bits (value << 6). y: W*H samples, uv: W/2 interleaved CbCr pairs per row,
 * H/2 rows (= W*H/2 samples). linesize is in BYTES: 2 per luma sample, 4 per
 * CbCr pair, i.e. 2 * W for both planes. */
static void p010_frame(nvFrame *f, uint16_t *y, uint16_t *uv, int n)
{
    unsigned i;

    for (i = 0; i < P010_W * P010_H; i++)
        y[i] = (uint16_t)(((64u + (i % P010_W + (unsigned)n * 8) % 800) & 0x3ff) << 6);
    for (i = 0; i < P010_W * P010_H / 2; i++)
        uv[i] = (uint16_t)(512u << 6);
    memset(f, 0, sizeof(*f));
    f->payload[0] = (unsigned char *)y;
    f->payload[1] = (unsigned char *)uv;
    f->linesize[0] = f->linesize[1] = 2 * P010_W;
    f->payload_size[0] = sizeof(uint16_t) * P010_W * P010_H;
    f->payload_size[1] = sizeof(uint16_t) * P010_W * P010_H / 2;
    f->type = NV_PIX_P010;
    f->width = P010_W;
    f->height = P010_H;
    f->timestamp = 1000 + n;
}

static int p010_enc_run(int skip_ok, int decode)
{
    static uint16_t y[P010_W * P010_H], uv[P010_W * P010_H / 2];
    static unsigned char *store[P010_FRAMES];
    static unsigned long store_n[P010_FRAMES];
    nvEncParam ep;
    nvDecParam dp;
    nvmpictx *enc = NULL, *dec = NULL;
    nvPacket *pk, pool[P010_POOL];
    nvFrame f;
    int rc = 0, i, tries, eos = 0;

    memset(pool, 0, sizeof(pool));
    p010_fill_param(&ep, NV_VIDEO_CodingHEVC, NV_PIX_P010, NVMPI_HEVC_PROFILE_MAIN10);
    enc = nvmpi_create_encoder(&ep);
    if (!enc)
        return skip_ok ? 77 : 1;
    for (i = 0; i < P010_POOL; i++) {
        pool[i].payload = calloc(1, NVMPI_ENC_CHUNK_SIZE);
        if (!pool[i].payload)
            P010_FAIL("alloc");
        nvmpi_encoder_qEmptyPacket(enc, &pool[i]);
    }
    for (i = 0; i < P010_FRAMES; i++) {
        nvPacket *g = NULL;

        p010_frame(&f, y, uv, i);
        if (nvmpi_encoder_put_frame(enc, &f) != 0)
            P010_FAIL("put_frame");
        for (tries = 0; tries < 50 && nvmpi_encoder_get_packet(enc, &g, true) != 0; tries++)
            ;
        if (!g)
            P010_FAIL("no packet");
        if (i == 0) {
            struct p010_sps s = { 0, 0, 0 };

            if (!p010_parse_sps(g->payload, g->payload_size, &s))
                P010_FAIL("first packet has no SPS");
            fprintf(stderr, "p010_rt SPS profile_idc=%u bit_depth=%u/%u\n",
                    s.profile_idc, s.bit_luma, s.bit_chroma);
            if (s.profile_idc != 2)
                P010_FAIL("SPS general_profile_idc is not Main10 (2)");
            if (s.bit_luma != 10 || s.bit_chroma != 10)
                P010_FAIL("SPS bit depth is not 10");
        }
        store[i] = malloc(g->payload_size);
        if (!store[i])
            P010_FAIL("alloc");
        memcpy(store[i], g->payload, g->payload_size);
        store_n[i] = g->payload_size;
        nvmpi_encoder_qEmptyPacket(enc, g);
    }
    if (nvmpi_encoder_put_frame(enc, NULL) != 0)       /* EOS */
        P010_FAIL("EOS");
    while (!eos) {                                     /* drain to EOS */
        int r = nvmpi_encoder_get_packet(enc, &pk, false);

        if (r == 0)
            P010_FAIL("packet after the last frame");
        eos = r == -2;
    }
    if (!decode)
        goto out;

    memset(&dp, 0, sizeof(dp));
    dp.codingType = NV_VIDEO_CodingHEVC;
    dp.pixFormat = NV_PIX_P010;
    dp.frame_pool_size = 4;
    dp.wait_timeout = 2000;
    dec = nvmpi_create_decoder(&dp);
    if (!dec) {
        if (skip_ok)
            rc = 77;                    /* no 10-bit decode on this build  */
        else
            fprintf(stderr, "p010_rt FAIL %s:%d: decoder create\n", __func__, __LINE__), rc = 1;
        goto out;
    }
    for (i = 0; i < P010_FRAMES; i++) {
        nvmpi_frame_release_cb rel = NULL;
        void *opaque = NULL;
        int fd = -1, w = 0, h = 0, pitch = 0;
        int64_t ts = 0;
        nvPacket in;

        memset(&in, 0, sizeof(in));
        in.payload = store[i];
        in.payload_size = store_n[i];
        in.pts = 1000 + i;
        if (nvmpi_decoder_put_packet(dec, &in) < 0)
            P010_FAIL("decoder put_packet");
        if (nvmpi_decoder_get_frame_fd(dec, &fd, &w, &h, &pitch, &ts, &rel,
                                       &opaque, true) != 0)
            P010_FAIL("no decoded frame");
        if (rel)
            rel(opaque);
        if (w != (int)P010_W || h != (int)P010_H)
            P010_FAIL("decoded size");
    }
out:
    if (dec && nvmpi_decoder_close(dec) != 0 && !rc)
        rc = 1;
    if (enc) {                          /* drain pool, then close */
        while (nvmpi_encoder_dqEmptyPacket(enc, &pk) == 0)
            ;
        while (nvmpi_encoder_get_packet(enc, &pk, false) == 0)
            ;
        if (nvmpi_encoder_close(enc) != 0 && !rc)
            rc = 1;
    }
    for (i = 0; i < P010_POOL; i++)
        free(pool[i].payload);
    for (i = 0; i < P010_FRAMES; i++) {
        free(store[i]);
        store[i] = NULL;
    }
    return rc;
}

#endif /* NVMPI_TEST_ENC_P010_RT_H */
