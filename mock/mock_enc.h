/*
 * mock_enc.h - internal interface of the encoder model: model state shared
 * by mock_enc.c (queue behaviour), mock_enc_ctl.c (control ioctls) and the
 * Annex-B access unit builder mock_enc_nal.c.
 */
#ifndef NVMPI_MOCK_ENC_H
#define NVMPI_MOCK_ENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mock_ctx;

/* Largest access unit the builder emits: SPS+PPS(+VPS)+one slice. Far below
 * any CAPTURE buffer libnvmpi requests (NVMPI_ENC_CHUNK_SIZE = 10 MiB). */
#define MOCK_ENC_AU_MAX 1024u
#define MOCK_ENC_MAX_DIM 8192u        /* same bound as the decoder model   */
#define MOCK_ENC_MAX_LATENCY 32u      /* NVMPI_MOCK_ENC_LATENCY bound      */

/* Per-device encoder state. ctx->model points at one; ctx->mu guards it. */
struct enc_model {
    uint32_t gop;                  /* GOP_SIZE: key frame period, 0 = none   */
    uint32_t idr_interval;         /* IDR_INTERVAL: IDR period, 0 = first only */
    bool     headers_at_idr;       /* INSERT_SPS_PPS_AT_IDR                  */
    bool     force_idr;            /* FORCE_FRAME_TYPE pending for next frame */
    bool     eos_seen;             /* EOS marker processed; input dropped    */
    uint32_t frames;               /* frames encoded since the last flush    */
    uint32_t since_idr;            /* frames since the last IDR (this incl.) */
    uint32_t since_key;            /* frames since the last key frame        */
    uint32_t latency;              /* OUTPUT buffers kept before returning   */
    uint32_t held;                 /* consumed OUTPUT buffers not returned   */
    uint32_t seq;                  /* AU payload counter (content varies)    */
    uint8_t  key[32];              /* KeyFrame flag per CAPTURE buffer       */
};

/* What one access unit looks like (the model decides, the builder renders). */
struct mock_enc_au {
    uint32_t pixfmt;               /* V4L2 fourcc of the CAPTURE format      */
    uint32_t width, height;        /* coded size (OUTPUT format)             */
    bool     idr;                  /* IDR slice (else P, or I when key)      */
    bool     key;                  /* key frame (IDR, or periodic I)         */
    bool     headers;              /* prepend SPS+PPS (+VPS for HEVC)        */
    uint32_t seq;                  /* varies slice bytes frame to frame      */
};

/* Render the access unit as Annex-B into out (cap bytes). Returns the size,
 * 0 when cap is too small or the format is not H.264/HEVC. */
size_t mock_enc_build_au(const struct mock_enc_au *au, uint8_t *out, size_t cap);

/* mock_enc_ctl.c: GOP/IDR/header controls and the metadata query. Same
 * contract as mock_ops.on_ioctl (1 = handled). ctx->mu held. */
int mock_enc_ioctl(struct mock_ctx *ctx, unsigned long req, void *arg, int *ret);

#endif /* NVMPI_MOCK_ENC_H */
