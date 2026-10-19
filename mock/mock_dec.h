/*
 * mock_dec.h - internal interface between the decoder model (mock_dec.c) and
 * its bitstream inspector (mock_dec_nal.c).
 */
#ifndef NVMPI_MOCK_DEC_H
#define NVMPI_MOCK_DEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mock_ctx;

#define MOCK_DEC_MIN_DIM 16u
#define MOCK_DEC_MAX_DIM 8192u        /* same bound as the nvbufsurface mock */

/* What the model needs to know about one OUTPUT (bitstream) buffer. */
struct mock_scan {
    bool     has_vcl;                 /* at least one slice/VCL NAL          */
    bool     has_dims;                /* an SPS was parsed successfully      */
    uint32_t width, height;           /* displayed size from the SPS         */
};

/* Per-device decoder state (mock_dec.c), also used by the control ioctl
 * handler (mock_dec_ctl.c). ctx->model points at one; ctx->mu guards it. */
struct dec_model {
    bool announced;                /* a RESOLUTION_CHANGE was pushed        */
    uint32_t width, height;        /* current stream size                   */
    bool reconfig;                 /* size changed, CAPTURE not yet re-armed */
    bool eos_seen;                 /* EOS marker processed                  */
    bool eos_last;                 /* empty CAPTURE DQBUF reports LAST      */
    bool poll_int;                 /* SET_POLL_INTERRUPT state              */
    uint32_t off_epoch;            /* ++ on every STREAMOFF (wakes polls)   */
    int last_idx;                  /* newest frame buffer, -1 none          */
    bool env_dims;                 /* NVMPI_MOCK_WIDTH/HEIGHT given         */
    uint32_t env_w, env_h;
};

/* mock_dec_ctl.c: controls the decoder model answers itself: G_CTRL
 * MIN_BUFFERS_FOR_CAPTURE, S_EXT_CTRLS DEVICE_POLL / SET_POLL_INTERRUPT.
 * Same contract as mock_ops.on_ioctl (1 = handled). ctx->mu held. */
int mock_dec_ioctl(struct mock_ctx *ctx, unsigned long req, void *arg, int *ret);

/* Inspect a buffer of Annex-B data. The buffer is UNTRUSTED input: every read
 * is bounds-checked against n and nothing is allocated. Never fails; fields
 * stay false/0 when nothing usable is found.
 *   mock_scan_h264  VCL = NAL types 1..5; SPS (type 7) parsed for size
 *   mock_scan_h265  VCL = NAL types 0..31; size not parsed (env/default)
 * Both require at least one start code (00 00 01), as libnvmpi's input mode
 * 0 feeds complete Annex-B access units. */
void mock_scan_h264(const uint8_t *p, size_t n, struct mock_scan *out);
void mock_scan_h265(const uint8_t *p, size_t n, struct mock_scan *out);

#endif /* NVMPI_MOCK_DEC_H */
