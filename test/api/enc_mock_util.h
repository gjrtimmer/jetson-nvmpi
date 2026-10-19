/*
 * enc_mock_util.h - helpers of the encoder mock tests (test_api_enc_mock.c):
 * encoder creation with a packet pool, raw frame submission, packet
 * inspection. Header-only. Everything is driven through the public API,
 * the way the FFmpeg wrapper drives it.
 */
#ifndef NVMPI_TEST_ENC_MOCK_UTIL_H
#define NVMPI_TEST_ENC_MOCK_UTIL_H

#include <nvmpi.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../mock/nal_scan.h"

#define EW 320u                           /* test picture size             */
#define EH 240u
#define E_NPKT 16                         /* packets in the pool           */
#define E_PKT_SZ (64u * 1024u)            /* payload bytes per packet      */
#define E_KEY 1ul                         /* AV_PKT_FLAG_KEY               */

#define REQUIRE(c)                                                        \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "REQUIRE failed %s:%d: %s\n", __FILE__,       \
                    __LINE__, #c);                                        \
            _exit(10 + (__LINE__ % 200));                                 \
        }                                                                 \
    } while (0)

struct ecfg {
    nvCodingType codec;
    unsigned gop, idr;                    /* iframe_interval, idr_interval */
    int hdr_at_idr;                       /* insert_spspps_idr             */
    int nonblocking;
    int dmabuf;                           /* use_dmabuf                    */
};

static inline uint64_t e_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Create an encoder and fill its packet pool (E_NPKT packets, owned by the
 * caller exactly like the FFmpeg wrapper owns them). NULL when creation
 * fails. Pool packets are released by e_destroy(). */
static inline nvmpictx *e_create(const struct ecfg *c)
{
    nvEncParam p;
    nvmpictx *ctx;
    int i;

    memset(&p, 0, sizeof(p));
    p.width = EW;
    p.height = EH;
    p.profile = 77;
    p.level = 41;
    p.bitrate = 2000000;
    p.pixFormat = c->dmabuf ? NV_PIX_NV12 : NV_PIX_YUV420; /* NV12 surfaces */
    p.iframe_interval = c->gop;
    p.idr_interval = c->idr;
    p.insert_spspps_idr = (char)c->hdr_at_idr;
    p.fps_n = 30;
    p.fps_d = 1;
    p.capture_num = 4;
    p.hw_preset_type = 1;
    p.codingType = c->codec;
    p.wait_timeout = 1000;
    p.nonblocking = c->nonblocking;
    p.use_dmabuf = c->dmabuf;
    ctx = nvmpi_create_encoder(&p);
    if (!ctx)
        return NULL;
    for (i = 0; i < E_NPKT; i++) {
        nvPacket *pk = calloc(1, sizeof(*pk));

        REQUIRE(pk);
        pk->payload = calloc(1, E_PKT_SZ);
        REQUIRE(pk->payload);
        nvmpi_encoder_qEmptyPacket(ctx, pk);
    }
    return ctx;
}

/* Pop every pool packet, free it, close the encoder. Returns the close
 * result and the time close() took (ms) in *took when given. */
static inline int e_destroy(nvmpictx *ctx, uint64_t *took)
{
    nvPacket *pk;
    uint64_t t0;
    int r;

    while (nvmpi_encoder_dqEmptyPacket(ctx, &pk) == 0) {
        free(pk->payload);
        free(pk);
    }
    while (nvmpi_encoder_get_packet(ctx, &pk, false) == 0) {
        free(pk->payload);
        free(pk);
    }
    t0 = e_now_ms();
    r = nvmpi_encoder_close(ctx);
    if (took)
        *took = e_now_ms() - t0;
    return r;
}

/* Submit one YUV420 frame (mid-gray) with timestamp pts. Returns the
 * put_frame result (0, NVMPI_ERR_EAGAIN, <0). */
static inline int e_put(nvmpictx *ctx, long pts)
{
    static unsigned char y[EW * EH], u[EW * EH / 4], v[EW * EH / 4];
    nvFrame f;

    memset(y, 0x80, sizeof(y));
    memset(u, 0x80, sizeof(u));
    memset(v, 0x80, sizeof(v));
    memset(&f, 0, sizeof(f));
    f.payload[0] = y;
    f.payload[1] = u;
    f.payload[2] = v;
    f.linesize[0] = EW;
    f.linesize[1] = f.linesize[2] = EW / 2;
    f.payload_size[0] = sizeof(y);
    f.payload_size[1] = f.payload_size[2] = sizeof(u);
    f.type = NV_PIX_YUV420;
    f.width = EW;
    f.height = EH;
    f.timestamp = pts;
    return nvmpi_encoder_put_frame(ctx, &f);
}

/* One encoded packet's facts. */
struct epkt {
    unsigned long flags, pts, size;
    int has_idr, has_sps, has_pps, has_vps;
    uint64_t hash;                        /* FNV-1a of the payload bytes   */
};

/* get_packet(wait) and return the packet to the pool; 0 on a packet,
 * otherwise the get_packet result (-1 none, -2 EOS). */
static inline int e_get(nvmpictx *ctx, int hevc, int wait, struct epkt *o)
{
    nvPacket *pk = NULL;
    int r = nvmpi_encoder_get_packet(ctx, &pk, wait != 0);

    if (r)
        return r;
    memset(o, 0, sizeof(*o));
    o->flags = pk->flags;
    o->pts = pk->pts;
    o->size = pk->payload_size;
    o->hash = 1469598103934665603ull;
    for (size_t k = 0; k < pk->payload_size; k++)
        o->hash = (o->hash ^ pk->payload[k]) * 1099511628211ull;
    o->has_idr = nal_has(pk->payload, pk->payload_size, hevc, hevc ? 19 : 5);
    o->has_sps = nal_has(pk->payload, pk->payload_size, hevc, hevc ? 33 : 7);
    o->has_pps = nal_has(pk->payload, pk->payload_size, hevc, hevc ? 34 : 8);
    o->has_vps = hevc && nal_has(pk->payload, pk->payload_size, 1, 32);
    nvmpi_encoder_qEmptyPacket(ctx, pk);
    return 0;
}

#endif /* NVMPI_TEST_ENC_MOCK_UTIL_H */
