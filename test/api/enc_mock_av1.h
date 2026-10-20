/*
 * enc_mock_av1.h - AV1 encode cases of test_api_enc_mock.c (issue #8).
 * Included from enc_mock_cases.h, after enc_mock_util.h; runs inside the
 * forked case child like every other case.
 *
 * Contract under test (include/nvmpi.h, src/nvmpi_enc_av1.cpp): AV1 encode
 * emits one low-overhead OBU temporal unit per packet (temporal delimiter,
 * sequence header on IDR frames only, frame OBU), key flag on the key frames,
 * the sequence header always inserted at IDR (even with insert_spspps_idr 0),
 * and 10-bit input rejected. The mock stream is parseable but not decodable
 * (mock/mock_enc_av1.c); the case walks the OBUs with av1_obu.h. Builds whose
 * headers lack the AV1 encode controls (NVMPI_HAVE_AV1_ENC 0) must return
 * NULL from create instead; that branch is what these cases check there.
 */
#ifndef NVMPI_TEST_ENC_MOCK_AV1_H
#define NVMPI_TEST_ENC_MOCK_AV1_H

#include <fcntl.h>

/* CMake passes 0/1 (CMakeLists.txt); assume the feature when built another way. */
#ifndef NVMPI_HAVE_AV1_ENC
#define NVMPI_HAVE_AV1_ENC 1
#endif

#if NVMPI_HAVE_AV1_ENC

#include "av1_obu.h"
#include "av1_enc_rt.h"

/* get_packet(wait) -> parsed temporal unit; also returns flags/pts/first
 * bytes. 0 on a packet, else the get_packet result (-1 none, -2 EOS). */
struct av1_pk {
    struct av1_tu t;
    unsigned long flags, pts, size;
    int ivf;                             /* payload starts with "DKIF"     */
};

static int av1_get(nvmpictx *ctx, int wait, struct av1_pk *o)
{
    nvPacket *pk = NULL;
    int r = nvmpi_encoder_get_packet(ctx, &pk, wait != 0);

    if (r)
        return r;
    av1_parse_tu(pk->payload, pk->payload_size, &o->t);
    o->flags = pk->flags;
    o->pts = pk->pts;
    o->size = pk->payload_size;
    o->ivf = pk->payload_size >= 4 && memcmp(pk->payload, "DKIF", 4) == 0;
    nvmpi_encoder_qEmptyPacket(ctx, pk);
    return 0;
}

/* Checks every packet property of one temporal unit; frame i of a stream
 * with key frames every `gop` and IDR (headers) every `idr`. */
static void av1_check(const struct av1_pk *p, int i, int gop, int idr, long pts)
{
    REQUIRE(p->pts == (unsigned long)pts && p->size > 0);
    REQUIRE(p->t.ok && p->t.first_td && !p->ivf);
    REQUIRE(p->t.n_td == 1 && p->t.n_frame == 1);
    REQUIRE(!!(p->flags & E_KEY) == (i % gop == 0));
    REQUIRE(p->t.key == (i % gop == 0));
    REQUIRE(p->t.n_seq == (i % idr == 0 ? 1u : 0u));
    if (p->t.n_seq)
        REQUIRE(p->t.w == EW && p->t.h == EH);
}

/* 30 frames, key frames at 0/10/20, sequence header at IDR 0 and 20 only,
 * insert_spspps_idr deliberately 0 (AV1 always requests headers at IDR);
 * then EOS drains cleanly. */
static int av1_stream(void)
{
    struct ecfg c = { NV_VIDEO_CodingAV1, 10, 20, 0, 0, 0 };
    nvmpictx *ctx = e_create(&c);
    struct av1_pk p;
    int i, eos = 0;

    REQUIRE(ctx);
    for (i = 0; i < 30; i++) {
        REQUIRE(e_put(ctx, 1000 + i) == 0);
        REQUIRE(av1_get(ctx, 1, &p) == 0);
        av1_check(&p, i, 10, 20, 1000 + i);
    }
    REQUIRE(nvmpi_encoder_put_frame(ctx, NULL) == 0);       /* EOS */
    while (!eos) {
        int r = av1_get(ctx, 0, &p);

        REQUIRE(r == -2 || r == -1);
        eos = r == -2;
    }
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* nvmpi_encoder_force_idr(): the next frame is a key frame with a sequence
 * header again (periodic IDR is off). */
static int av1_force_idr(void)
{
    struct ecfg c = { NV_VIDEO_CodingAV1, 0, 0, 0, 0, 0 };
    nvmpictx *ctx = e_create(&c);
    struct av1_pk p;
    int i;

    REQUIRE(ctx);
    for (i = 0; i < 4; i++) {
        REQUIRE(e_put(ctx, 10 + i) == 0);
        REQUIRE(av1_get(ctx, 1, &p) == 0);
        REQUIRE(!!(p.flags & E_KEY) == (i == 0) && p.t.n_seq == (i == 0));
    }
    REQUIRE(nvmpi_encoder_force_idr(ctx) == 0);
    REQUIRE(e_put(ctx, 99) == 0);
    REQUIRE(av1_get(ctx, 1, &p) == 0);
    REQUIRE((p.flags & E_KEY) && p.t.key && p.t.n_seq == 1 && p.pts == 99);
    REQUIRE(e_destroy(ctx, NULL) == 0);
    return 0;
}

/* Mock encode -> mock decode: one decoded 320x240 frame per packet. The mock
 * decoder takes the stream size from the environment (no AV1 parser). */
static int av1_roundtrip(void)
{
    setenv("NVMPI_MOCK_WIDTH", "320", 1);
    setenv("NVMPI_MOCK_HEIGHT", "240", 1);
    return av1_enc_dec_roundtrip(0);
}

/* Create gate for a build with the AV1 encode controls: 10-bit input is
 * refused before any device is opened. Run with open@1+ (every open fails),
 * so the logged error is expected and any open failure in the log would mean
 * create reached the device. */
static int av1_create_gate(void)
{
    char buf[4096];
    nvEncParam p;
    ssize_t n;
    int fd;

    memset(&p, 0, sizeof(p));
    p.codingType = NV_VIDEO_CodingAV1;
    p.width = EW;
    p.height = EH;
    p.bitrate = 2000000;
    p.fps_n = 30;
    p.fps_d = 1;
    p.capture_num = 4;
    p.pixFormat = NV_PIX_P010;
    REQUIRE(nvmpi_create_encoder(&p) == NULL);
    fd = open("/proc/self/fd/2", O_RDONLY);
    REQUIRE(fd >= 0);
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    REQUIRE(n > 0);
    buf[n] = '\0';
    REQUIRE(strstr(buf, "P010 rejected") != NULL);
    REQUIRE(strstr(buf, "Encoder factory") == NULL);
    REQUIRE(strstr(buf, "Could not create encoder") == NULL);
    return 0;
}

#else  /* !NVMPI_HAVE_AV1_ENC: headers without the AV1 encode controls */

/* Without the gate AV1 encode must fail cleanly: NULL, the "not supported"
 * error, no device access (open@1+). Same stance as the decoder (#7). */
static int av1_create_gate(void)
{
    char buf[4096];
    struct ecfg c = { NV_VIDEO_CodingAV1, 0, 0, 0, 0, 0 };
    ssize_t n;
    int fd;

    REQUIRE(e_create(&c) == NULL);
    fd = open("/proc/self/fd/2", O_RDONLY);
    REQUIRE(fd >= 0);
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    REQUIRE(n > 0);
    buf[n] = '\0';
    REQUIRE(strstr(buf, "AV1 encode not supported") != NULL);
    REQUIRE(strstr(buf, "Encoder factory") == NULL);
    REQUIRE(strstr(buf, "Could not create encoder") == NULL);
    return 0;
}

#endif /* NVMPI_HAVE_AV1_ENC */

#endif /* NVMPI_TEST_ENC_MOCK_AV1_H */
