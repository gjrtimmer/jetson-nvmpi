/*
 * av1_p010_rt.h - AV1 encode from P010 input (#146), shared by the hardware
 * case in test_api.c (av1_p010_case.h) and the encoder mock suite
 * (enc_mock_av1.h). Header-only; needs <nvmpi.h>, <stdio.h>, <stdlib.h>,
 * <string.h> and av1_obu.h included first.
 *
 * Whether the Orin's AV1 encoder takes a P010M OUTPUT plane was unknown when
 * this was written, so the run reports what the driver did instead of
 * assuming: it always prints one probe line to stderr (CI job log):
 *
 *   av1_p010 probe: create=<ok|fail> seq_profile=X high_bitdepth=Y decode=<ok|fail|skip|unsupported>
 *
 * Steps: create an AV1 encoder with NV_PIX_P010 (fail -> create=fail), encode
 * AV1P_FRAMES 320x240 frames (key frame every AV1P_GOP), parse the sequence
 * header of the first packet (av1_seq_color: seq_profile, high_bitdepth,
 * twelve_bit; "-" when the header cannot be parsed) and require seq_profile 0
 * with high_bitdepth 1 (BitDepth 10). With decode != 0 every packet then goes
 * through the AV1 decoder with P010 output: decode=unsupported when the
 * decoder cannot be created (the encoder result stands), decode=fail when it
 * is created but a packet does not decode (failure), decode=skip when not
 * asked (the mock decoder has no 10-bit model).
 *
 * Return: 0 pass, 77 skipped (create failed and skip_ok), 1 fail. Every
 * failure names the check on stderr.
 */
#ifndef NVMPI_TEST_AV1_P010_RT_H
#define NVMPI_TEST_AV1_P010_RT_H

#include "enc_p010_rt.h"

#define AV1P_FRAMES 10
#define AV1P_GOP 5
#define AV1P_POOL 8
#define AV1P_FAIL(m) do { fprintf(stderr, "av1_p010 FAIL %s:%d: %s\n", __func__, __LINE__, (m)); rc = 1; goto out; } while (0)

static int av1_p010_run(int skip_ok, int decode)
{
    static uint16_t y[P010_W * P010_H], uv[P010_W * P010_H / 2];
    static unsigned char *store[AV1P_FRAMES];
    static unsigned long store_n[AV1P_FRAMES];
    const char *dec_res = "skip";
    char prof[16] = "-", hbd[16] = "-";
    nvEncParam ep;
    nvDecParam dp;
    nvmpictx *enc = NULL, *dec = NULL;
    nvPacket *pk, pool[AV1P_POOL];
    nvFrame f;
    int rc = 0, i, tries, eos = 0, created = 0;

    memset(pool, 0, sizeof(pool));
    p010_fill_param(&ep, NV_VIDEO_CodingAV1, NV_PIX_P010, 0);
    ep.iframe_interval = AV1P_GOP;
    ep.idr_interval = AV1P_GOP;
    /* A driver that refuses the format must surface here as NULL, never as
     * a crash (nvmpi_create_encoder cleans up and logs the cause). */
    enc = nvmpi_create_encoder(&ep);
    if (!enc) {
        rc = skip_ok ? 77 : 1;
        goto out;
    }
    created = 1;
    for (i = 0; i < AV1P_POOL; i++) {
        pool[i].payload = calloc(1, NVMPI_ENC_CHUNK_SIZE);
        if (!pool[i].payload)
            AV1P_FAIL("alloc");
        nvmpi_encoder_qEmptyPacket(enc, &pool[i]);
    }
    for (i = 0; i < AV1P_FRAMES; i++) {
        struct av1_tu t;
        nvPacket *g = NULL;

        p010_frame(&f, y, uv, i);
        if (nvmpi_encoder_put_frame(enc, &f) != 0)
            AV1P_FAIL("put_frame");
        for (tries = 0; tries < 50 && nvmpi_encoder_get_packet(enc, &g, true) != 0; tries++)
            ;
        if (!g)
            AV1P_FAIL("no packet");
        av1_parse_tu(g->payload, g->payload_size, &t);
        if (!t.ok || !t.first_td || t.n_frame < 1)
            AV1P_FAIL("packet is not a low-overhead OBU temporal unit");
        if (!!(g->flags & 1ul) != (i % AV1P_GOP == 0))
            AV1P_FAIL("key frame flag");
        if (i == 0) {
            if (t.color_ok) {
                snprintf(prof, sizeof(prof), "%u", t.profile);
                snprintf(hbd, sizeof(hbd), "%u", t.high_bd);
            }
            if (!t.n_seq || !t.color_ok)
                AV1P_FAIL("first packet has no parseable sequence header");
            if (t.profile != 0 || !t.high_bd || t.twelve_bit)
                AV1P_FAIL("sequence header is not 10-bit (seq_profile 0, high_bitdepth 1)");
            if (t.w && (t.w != P010_W || t.h != P010_H))
                AV1P_FAIL("sequence header size");
        }
        store[i] = malloc(g->payload_size);
        if (!store[i])
            AV1P_FAIL("alloc");
        memcpy(store[i], g->payload, g->payload_size);
        store_n[i] = g->payload_size;
        nvmpi_encoder_qEmptyPacket(enc, g);
    }
    if (nvmpi_encoder_put_frame(enc, NULL) != 0)       /* EOS */
        AV1P_FAIL("EOS");
    while (!eos) {                                     /* drain to EOS */
        int r = nvmpi_encoder_get_packet(enc, &pk, false);

        if (r == 0)
            AV1P_FAIL("packet after the last frame");
        eos = r == -2;
    }
    if (!decode)
        goto out;

    memset(&dp, 0, sizeof(dp));
    dp.codingType = NV_VIDEO_CodingAV1;
    dp.pixFormat = NV_PIX_P010;
    dp.frame_pool_size = 4;
    dp.wait_timeout = 2000;
    dec = nvmpi_create_decoder(&dp);
    if (!dec) {
        dec_res = "unsupported";      /* no 10-bit AV1 decode: encode stands */
        goto out;
    }
    dec_res = "fail";
    for (i = 0; i < AV1P_FRAMES; i++) {
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
            AV1P_FAIL("decoder put_packet");
        if (nvmpi_decoder_get_frame_fd(dec, &fd, &w, &h, &pitch, &ts, &rel,
                                       &opaque, true) != 0)
            AV1P_FAIL("no decoded frame");
        if (rel)
            rel(opaque);
        if (w != (int)P010_W || h != (int)P010_H)
            AV1P_FAIL("decoded size");
    }
    dec_res = "ok";
out:
    /* One line, always: the MR job log records what the driver did. */
    fprintf(stderr, "av1_p010 probe: create=%s seq_profile=%s high_bitdepth=%s decode=%s\n",
            created ? "ok" : "fail", prof, hbd, dec_res);
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
    for (i = 0; i < AV1P_POOL; i++)
        free(pool[i].payload);
    for (i = 0; i < AV1P_FRAMES; i++) {
        free(store[i]);
        store[i] = NULL;
    }
    return rc;
}

#endif /* NVMPI_TEST_AV1_P010_RT_H */
