/*
 * mock_enc.h - internal interface of the encoder model: model state shared
 * by mock_enc.c (queue behaviour), mock_enc_ctl.c (control ioctls), the
 * Annex-B access unit builders (mock_enc_nal.c dispatcher, mock_enc_h264.c,
 * mock_enc_hevc.c) and the input picture reader mock_enc_pic.c.
 */
#ifndef NVMPI_MOCK_ENC_H
#define NVMPI_MOCK_ENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mock_ctx;
struct mock_queue;
struct mock_enc_pic;

/* H.264 key frames are I_PCM (#117): per macroblock a 9-bit mb_type
 * ue(25), up to 7 alignment bits and 384 sample bytes (256 Y + 2 x 64
 * chroma). With the macroblock starting byte aligned the header is <= 2
 * bytes, so MOCK_ENC_MB_MAX bytes bound one macroblock's RBSP.
 *
 * The RBSP is built in a heap scratch buffer sized from the padded picture
 * (see enc_scratch_size() in mock_enc.c): nmb * MOCK_ENC_MB_MAX plus
 * MOCK_ENC_AU_HEADROOM, which covers the slice header (<= 16 bytes), the
 * first macroblock's unaligned start and the trailing byte. The NAL itself
 * (emulation prevention applied) goes straight into the mapped CAPTURE
 * buffer and is bounded by its length. PCM samples are >= 1, so only the
 * header bytes can need emulation prevention: the scratch bound is the RBSP
 * bound and the output exceeds it by a few bytes at most. Pictures above
 * MOCK_ENC_MAX_PIXELS would need more than the 10 MiB CAPTURE buffer
 * libnvmpi requests (NVMPI_ENC_CHUNK_SIZE), so they are refused: every frame
 * completes with V4L2_BUF_FLAG_ERROR instead of being truncated (a picture
 * just below the bound can still miss the buffer by its macroblock padding;
 * the builder then refuses it the same way). */
#define MOCK_ENC_MAX_PIXELS 6900000u
#define MOCK_ENC_MB_MAX 386u          /* RBSP bytes of one I_PCM macroblock */
#define MOCK_ENC_AU_HEADROOM 4096u    /* slice header + alignment slack     */
#define MOCK_ENC_MAX_DIM 8192u        /* same bound as the decoder model   */
#define MOCK_ENC_SLICE_MAX 96u        /* canned HEVC slice body bytes      */
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
    const struct mock_enc_pic *pic;/* input picture (H.264 PCM samples)      */
};

/* mock_enc_pic.c: the OUTPUT buffer of the frame being encoded. Unreadable
 * input (other formats, USERPTR, a foreign fd) is a deterministic pattern. */
struct mock_enc_pic {
    uint32_t w, h;                 /* picture size                           */
    uint32_t seq;                  /* pattern seed (frame counter)           */
    unsigned layout;               /* 0 pattern, else NV12 / I420 (pic.c)    */
    const uint8_t *plane[3];       /* plane starts inside map (NULL = none)  */
    uint32_t stride[3], len[3];    /* row pitch and bytes of each plane      */
    void *map;                     /* the mapping (munmap in unmap)          */
    size_t map_len;
};

/* Map OUTPUT buffer oi of queue `out` read-only (always initialises pic). */
void mock_enc_pic_map(const struct mock_queue *out, int oi, uint32_t seq,
                      struct mock_enc_pic *pic);
void mock_enc_pic_unmap(struct mock_enc_pic *pic);
/* n samples of component comp (0 Y, 1 U, 2 V) starting at (x0, y) in that
 * component's coordinates; positions past the picture repeat the edge;
 * values are clamped to >= 1. */
void mock_enc_pic_row(const struct mock_enc_pic *pic, unsigned comp, uint32_t x0,
                      uint32_t y, unsigned n, uint8_t *dst);

/* Render the access unit as Annex-B into out (cap bytes); rbsp (rbsp_cap
 * bytes, any content) is scratch for the H.264 slice RBSP and never aliases
 * out. Returns the size, 0 when out or rbsp is too small, the picture is
 * invalid or the format is not H.264/HEVC. out may hold partial bytes then. */
size_t mock_enc_build_au(const struct mock_enc_au *au, uint8_t *out, size_t cap,
                         uint8_t *rbsp, size_t rbsp_cap);

/* mock_enc_h264.c: SPS (+VUI), PPS and the CAVLC slice of one H.264 AU. */
size_t mock_enc_h264_au(const struct mock_enc_au *au, uint8_t *out, size_t cap,
                        uint8_t *rbsp, size_t rbsp_cap);

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
