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

/* An AU is built in a heap scratch buffer sized from the picture (see
 * enc_scratch_size() in mock_enc.c): a raw-sample (PCM) key frame needs about
 * 1.5 bytes per pixel (#110) plus headers. Pictures above MOCK_ENC_MAX_PIXELS
 * would need more than the 10 MiB CAPTURE buffer libnvmpi requests
 * (NVMPI_ENC_CHUNK_SIZE), so they are refused: every frame completes with
 * V4L2_BUF_FLAG_ERROR instead of being truncated. */
#define MOCK_ENC_MAX_PIXELS 6900000u
#define MOCK_ENC_AU_HEADROOM 4096u    /* SPS/PPS/VPS + slice header bytes  */
#define MOCK_ENC_MAX_DIM 8192u        /* same bound as the decoder model   */
#define MOCK_ENC_SLICE_MAX 96u        /* canned slice data bytes (IDR)     */
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
    bool     insert_vui;           /* INSERT_VUI: SPS carries timing_info    */
    uint8_t *scratch;              /* AU build buffer, calloc'd in on_fmt,
                                    * freed in enc_on_close/on_fmt resize    */
    size_t   scratch_cap;          /* bytes in scratch; 0 = picture refused  */
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
    uint32_t poc;                  /* frames since the last IDR (HEVC POC)   */
    uint32_t tpf_num, tpf_den;     /* timeperframe; VUI emitted when both
                                    * are non-zero (INSERT_VUI on)           */
};

/* Render the access unit as Annex-B into out (cap bytes). Returns the size,
 * 0 when cap is too small or the format is not H.264/HEVC. */
size_t mock_enc_build_au(const struct mock_enc_au *au, uint8_t *out, size_t cap);

/* mock_enc_hevc.c: real HEVC VPS+SPS+PPS for a w x h picture, and one slice
 * NAL (nal_type 19/21/1) with a real slice header followed by blen canned
 * body bytes. Both return the bytes written, 0 when room is too small.
 * tpf_num/tpf_den (timeperframe) non-zero add VUI timing_info to the SPS:
 * vui_num_units_in_tick = tpf_num, vui_time_scale = tpf_den. */
size_t mock_enc_hevc_params(uint8_t *out, size_t room, uint32_t w, uint32_t h,
                            uint32_t tpf_num, uint32_t tpf_den);
size_t mock_enc_hevc_slice(uint8_t *out, size_t room, unsigned nal_type,
                           uint32_t poc, const uint8_t *body, size_t blen);

/* mock_enc_ctl.c: GOP/IDR/header controls and the metadata query. Same
 * contract as mock_ops.on_ioctl (1 = handled). ctx->mu held. */
int mock_enc_ioctl(struct mock_ctx *ctx, unsigned long req, void *arg, int *ret);

#endif /* NVMPI_MOCK_ENC_H */
