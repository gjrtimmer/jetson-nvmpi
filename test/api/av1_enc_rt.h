/*
 * av1_enc_rt.h - AV1 encode -> decode round trip (#8), shared by the
 * hardware case in test_api.c (av1_enc_case.h) and the encoder mock suite
 * (enc_mock_av1.h). Header-only; needs <nvmpi.h>, <stdio.h>, <stdlib.h>,
 * <string.h> and av1_obu.h included first.
 *
 * Encodes AV1_RT_FRAMES 320x240 frames (NVDEC rejects 64x64 AV1, so the
 * size matters) with a key frame every AV1_RT_GOP frames, checks every
 * packet's structure, then feeds each packet to the AV1 decoder and expects
 * one 320x240 frame back per packet. Return: 0 pass, 77 skipped (create
 * failed and the caller says that is acceptable), 1 fail. Every failure
 * names the check on stderr.
 */
#ifndef NVMPI_TEST_AV1_ENC_RT_H
#define NVMPI_TEST_AV1_ENC_RT_H

#define AV1_RT_W 320u
#define AV1_RT_H 240u
#define AV1_RT_FRAMES 10
#define AV1_RT_GOP 5
#define AV1_RT_FAIL(m) do { fprintf(stderr, "av1_rt FAIL %s:%d: %s\n", __func__, __LINE__, (m)); rc = 1; goto out; } while (0)

static void av1_rt_frame(nvFrame *f, unsigned char *y, unsigned char *u,
                         unsigned char *v, int n)
{
    unsigned i;

    /* Moving gradient: not a flat frame, so the encoder has work to do. */
    for (i = 0; i < AV1_RT_W * AV1_RT_H; i++)
        y[i] = (unsigned char)(16 + (i % AV1_RT_W + (unsigned)n * 8) % 200);
    memset(u, 128, AV1_RT_W * AV1_RT_H / 4);
    memset(v, 128, AV1_RT_W * AV1_RT_H / 4);
    memset(f, 0, sizeof(*f));
    f->payload[0] = y;
    f->payload[1] = u;
    f->payload[2] = v;
    f->linesize[0] = AV1_RT_W;
    f->linesize[1] = f->linesize[2] = AV1_RT_W / 2;
    f->payload_size[0] = AV1_RT_W * AV1_RT_H;
    f->payload_size[1] = f->payload_size[2] = AV1_RT_W * AV1_RT_H / 4;
    f->type = NV_PIX_YUV420;
    f->width = AV1_RT_W;
    f->height = AV1_RT_H;
    f->timestamp = 1000 + n;
}

/* skip_ok: 77 instead of failure when the encoder or decoder cannot be
 * created (stubs, no device, headers without AV1). */
static int av1_enc_dec_roundtrip(int skip_ok)
{
    static unsigned char y[AV1_RT_W * AV1_RT_H], u[AV1_RT_W * AV1_RT_H / 4],
                         v[AV1_RT_W * AV1_RT_H / 4];
    static unsigned char *store[AV1_RT_FRAMES];
    static unsigned long store_n[AV1_RT_FRAMES];
    nvEncParam ep;
    nvDecParam dp;
    nvmpictx *enc = NULL, *dec = NULL;
    nvPacket *pk;
    nvFrame f;
    int rc = 0, i, got = 0, eos = 0, tries;
    nvPacket pool[8];

    memset(pool, 0, sizeof(pool));
    memset(&ep, 0, sizeof(ep));
    ep.codingType = NV_VIDEO_CodingAV1;
    ep.width = AV1_RT_W;
    ep.height = AV1_RT_H;
    ep.bitrate = 2000000;
    ep.fps_n = 30;
    ep.fps_d = 1;
    ep.iframe_interval = AV1_RT_GOP;
    ep.idr_interval = AV1_RT_GOP;
    ep.capture_num = 4;
    ep.hw_preset_type = 1;
    ep.pixFormat = NV_PIX_YUV420;
    ep.wait_timeout = 2000;
    enc = nvmpi_create_encoder(&ep);
    if (!enc)
        return skip_ok ? 77 : 1;
    for (i = 0; i < 8; i++) {
        pool[i].payload = calloc(1, NVMPI_ENC_CHUNK_SIZE);
        if (!pool[i].payload)
            AV1_RT_FAIL("alloc");
        nvmpi_encoder_qEmptyPacket(enc, &pool[i]);
    }

    for (i = 0; i < AV1_RT_FRAMES; i++) {
        struct av1_tu t;
        nvPacket *g = NULL;

        av1_rt_frame(&f, y, u, v, i);
        if (nvmpi_encoder_put_frame(enc, &f) != 0)
            AV1_RT_FAIL("put_frame");
        for (tries = 0; tries < 50 && nvmpi_encoder_get_packet(enc, &g, true) != 0; tries++)
            ;
        if (!g)
            AV1_RT_FAIL("no packet");
        av1_parse_tu(g->payload, g->payload_size, &t);
        if (!t.ok || !t.first_td || t.n_frame < 1)
            AV1_RT_FAIL("packet is not a low-overhead OBU temporal unit");
        if (g->payload_size >= 4 && !memcmp(g->payload, "DKIF", 4))
            AV1_RT_FAIL("IVF header in packet");
        if (!!(g->flags & 1ul) != (i % AV1_RT_GOP == 0))
            AV1_RT_FAIL("key frame flag");
        if ((t.n_seq > 0) != (i % AV1_RT_GOP == 0))
            AV1_RT_FAIL("sequence header only on key frames");
        if (t.w && (t.w != AV1_RT_W || t.h != AV1_RT_H))
            AV1_RT_FAIL("sequence header size");
        store[i] = malloc(g->payload_size);
        if (!store[i])
            AV1_RT_FAIL("alloc");
        memcpy(store[i], g->payload, g->payload_size);
        store_n[i] = g->payload_size;
        nvmpi_encoder_qEmptyPacket(enc, g);
    }
    if (nvmpi_encoder_put_frame(enc, NULL) != 0)       /* EOS */
        AV1_RT_FAIL("EOS");
    while (!eos) {                                     /* drain to EOS */
        int r = nvmpi_encoder_get_packet(enc, &pk, false);

        if (r == 0)
            AV1_RT_FAIL("packet after the last frame");
        eos = r == -2;
    }

    memset(&dp, 0, sizeof(dp));
    dp.codingType = NV_VIDEO_CodingAV1;
    dp.pixFormat = NV_PIX_NV12;
    dp.frame_pool_size = 4;
    dp.wait_timeout = 2000;
    dec = nvmpi_create_decoder(&dp);
    if (!dec)
        AV1_RT_FAIL("decoder create");
    for (i = 0; i < AV1_RT_FRAMES; i++) {
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
            AV1_RT_FAIL("decoder put_packet");
        if (nvmpi_decoder_get_frame_fd(dec, &fd, &w, &h, &pitch, &ts, &rel,
                                       &opaque, true) != 0)
            AV1_RT_FAIL("no decoded frame");
        if (rel)
            rel(opaque);
        if (w != (int)AV1_RT_W || h != (int)AV1_RT_H)
            AV1_RT_FAIL("decoded size");
        got++;
    }
    if (got != AV1_RT_FRAMES)
        AV1_RT_FAIL("frame count");
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
    for (i = 0; i < 8; i++)
        free(pool[i].payload);
    for (i = 0; i < AV1_RT_FRAMES; i++)
        free(store[i]);
    return rc;
}

#endif /* NVMPI_TEST_AV1_ENC_RT_H */
