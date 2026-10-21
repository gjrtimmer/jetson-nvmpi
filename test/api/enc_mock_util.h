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
/* libnvmpi copies a whole AU into the packet payload and its contract is
 * NVMPI_ENC_CHUNK_SIZE bytes (nvmpi.h). A mock key frame is I_PCM (#117,
 * ~115 KB at 320x240), so a smaller buffer overflows. */
#define E_PKT_SZ NVMPI_ENC_CHUNK_SIZE
#define E_KEY 1ul                         /* AV_PKT_FLAG_KEY               */

/* determinism: encoder create/destroy dominates a run (threads, pools), and
 * TSan/ASan multiply that cost (#142: 12-15 s of the 30 s case alarm in CI
 * with 40 runs). Release builds keep the 40 runs that catch rare races; a
 * sanitized build does 10 - it still compares 10 full encodes byte for byte,
 * and the sanitizer itself is what looks for the races. */
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define NVMPI_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define NVMPI_TEST_SANITIZED 1
#endif
#endif
#ifdef NVMPI_TEST_SANITIZED
#define DET_RUNS 10
#else
#define DET_RUNS 40
#endif

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

/* Create an encoder and fill its packet pool (npkt / E_NPKT packets, owned by the
 * caller exactly like the FFmpeg wrapper owns them). NULL when creation
 * fails. Pool packets are released by e_destroy(). */
static inline nvmpictx *e_create_n(const struct ecfg *c, int npkt)
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
    for (i = 0; i < npkt; i++) {
        nvPacket *pk = calloc(1, sizeof(*pk));

        REQUIRE(pk);
        pk->payload = calloc(1, E_PKT_SZ);
        REQUIRE(pk->payload);
        nvmpi_encoder_qEmptyPacket(ctx, pk);
    }
    return ctx;
}

static inline nvmpictx *e_create(const struct ecfg *c)
{
    return e_create_n(c, E_NPKT);
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

/* Deadline for a put/get/EOS drain loop (#136): a wedged EOS path then fails
 * with its state, well before the case alarm, instead of dying as a bare
 * SIGALRM. Zero-init; the clock starts on the first call. Returns 1 to go on,
 * 0 (after printing the state) once EOS_LOOP_MS has passed without EOS. */
#define EOS_LOOP_MS 10000

struct eos_watch { struct timespec t0; long iters; };

static inline int eos_watch(struct eos_watch *w, int got, int eos, int last)
{
    struct timespec now;
    long ms;

    if (w->iters++ == 0)
        clock_gettime(CLOCK_MONOTONIC, &w->t0);
    if (eos)
        return 1;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (now.tv_sec - w->t0.tv_sec) * 1000 +
         (now.tv_nsec - w->t0.tv_nsec) / 1000000;
    if (ms < EOS_LOOP_MS)
        return 1;
    fprintf(stderr, "EOS drain stuck %ld ms: packets=%d eos_seen=%d "
            "iterations=%ld last_get_packet=%d\n", ms, got, eos, w->iters,
            last);
    return 0;
}

#endif /* NVMPI_TEST_ENC_MOCK_UTIL_H */
