/*
 * mock_enc_ctl.c - control ioctls of the encoder model (split from
 * mock_enc.c): the controls that change what the model emits, and the
 * per-buffer output metadata query. Every other control is accepted and
 * ignored by the generic handler in mock_v4l2_core.c (return 0 = "not
 * handled here"), like a driver that stores bitrate/profile/level values
 * the mock has no use for.
 *
 * Control ids are the ones NvVideoEncoder issues through VIDIOC_S_EXT_CTRLS /
 * VIDIOC_G_EXT_CTRLS (NvVideoEncoder.cpp, v4l2_nv_extensions.h):
 *   GOP_SIZE                       setIFrameInterval()  key frame period
 *   IDR_INTERVAL        BASE+514   setIDRInterval()
 *   FORCE_FRAME_TYPE (MFC51)       forceIDR(): next queued frame is an IDR
 *   INSERT_SPS_PPS_AT_IDR BASE+535 setInsertSpsPpsAtIdrEnabled()
 *   METADATA            BASE+536   getMetadata(): KeyFrame of a CAPTURE buffer
 *   INSERT_VUI          BASE+546   setInsertVuiEnabled(): SPS gets VUI
 *                                  timing_info from the S_PARM frame rate
 *   BITRATE_PEAK                   setPeakBitrate(): recorded for tests only
 *                                  (mock_enc_last_peak_bitrate())
 *   H265_PROFILE        BASE+513   setProfile() on an HEVC encoder: Main10
 *                                  makes the SPS 10-bit/profile_idc 2; the
 *                                  value is also recorded for tests only
 *                                  (mock_enc_last_h265_profile())
 *
 * Runs with ctx->mu held (mock_ops.on_ioctl contract); nothing blocks, except
 * the opt-in NVMPI_MOCK_CTRL_DELAY_US sleep in the metadata query, which drops
 * the lock for the sleep.
 */
#define _GNU_SOURCE
#include "mock_v4l2.h"
#include "mock_enc.h"
#include "mock_faults.h"

#include <errno.h>
#include <stdatomic.h>

#define MOCK_EXPORT __attribute__((visibility("default")))

#define MOCK_CID_IDR_INTERVAL      (V4L2_CID_MPEG_BASE + 514)
#define MOCK_CID_SPS_PPS_AT_IDR    (V4L2_CID_MPEG_BASE + 535)
#define MOCK_CID_ENC_METADATA      (V4L2_CID_MPEG_BASE + 536)
#define MOCK_CID_INSERT_VUI        (V4L2_CID_MPEG_BASE + 546)
#define MOCK_CID_H265_PROFILE      (V4L2_CID_MPEG_BASE + 513)
#define MOCK_H265_PROFILE_MAIN10   1   /* V4L2_MPEG_VIDEO_H265_PROFILE_MAIN10 */

/* Last BITRATE_PEAK value programmed on any encoder in this process; test
 * read-back only (the model has no use for it). peak_seen = 0 until set. */
static _Atomic uint32_t peak_value;
static atomic_int peak_seen;

/* Test hook (found with dlsym(RTLD_DEFAULT), like the other mock exports):
 * stores the last peak bitrate in *out and returns 0, or -1 if none was set. */
MOCK_EXPORT int mock_enc_last_peak_bitrate(uint32_t *out)
{
    if (!out || !atomic_load(&peak_seen))
        return -1;
    *out = atomic_load(&peak_value);
    return 0;
}

/* Last H265_PROFILE value programmed on any encoder in this process (test
 * read-back, like the peak bitrate). profile_seen = 0 until set. */
static _Atomic uint32_t profile_value;
static atomic_int profile_seen;

/* Test hook (weak extern in the tests): last HEVC profile control value in
 * *out and 0, or -1 if none was set. */
MOCK_EXPORT int mock_enc_last_h265_profile(uint32_t *out)
{
    if (!out || !atomic_load(&profile_seen))
        return -1;
    *out = atomic_load(&profile_value);
    return 0;
}

/* ABI twins of the NVIDIA structs (v4l2_nv_extensions.h): the metadata
 * pointer block of v4l2_ctrl_video_metadata, and the first fields of
 * v4l2_ctrl_videoenc_outputbuf_metadata (the caller's struct is much
 * larger; only these are written, the rest is left as the caller had it). */
struct mock_video_metadata {
    void *dec_header_err, *dec_meta, *enc_meta, *enc_mv_meta;
    uint32_t buffer_index;
};
struct mock_enc_meta_head {
    uint8_t  key_frame, end_of_frame;
    uint16_t avg_qp;
};

/* Metadata of CAPTURE buffer idx: valid from its DQBUF until it is queued
 * again (the model only rewrites key[idx] when it fills the buffer). */
static int get_metadata(struct mock_ctx *ctx, struct enc_model *m, char *str)
{
    struct mock_video_metadata *vm = (struct mock_video_metadata *)(void *)str;
    struct mock_enc_meta_head *h;

    /* The pointer and the index come straight from the caller: refuse a
     * missing struct and an index outside the negotiated CAPTURE queue. */
    if (!vm || !vm->enc_meta ||
        vm->buffer_index >= ctx->q[MOCK_Q_CAPTURE].n_bufs ||
        vm->buffer_index >= sizeof(m->key))
        return EINVAL;
    h = vm->enc_meta;
    h->key_frame = m->key[vm->buffer_index];
    h->end_of_frame = 1;
    h->avg_qp = 26;
    return 0;
}

int mock_enc_ioctl(struct mock_ctx *ctx, unsigned long req, void *arg, int *ret)
{
    struct enc_model *m = ctx->model;
    struct v4l2_ext_controls *ec = arg;
    uint32_t i;
    int err = 0;

    if (!m || (req != VIDIOC_S_EXT_CTRLS && req != VIDIOC_G_EXT_CTRLS))
        return 0;
    /* count and the controls pointer are caller data: bound them. */
    if (ec->count == 0 || ec->count > 16 || !ec->controls) {
        errno = EINVAL;
        *ret = -1;
        return 1;
    }
    for (i = 0; i < ec->count && !err; i++) {
        struct v4l2_ext_control *k = &ec->controls[i];

        if (req == VIDIOC_G_EXT_CTRLS) {
            if (k->id == MOCK_CID_ENC_METADATA) {
                /* Test hook: stretch the DQ callback's take-to-publish
                 * window. Sleep without ctx->mu so the model keeps running;
                 * the caller is the DQ thread, which owns the device until
                 * it is joined, so ctx/m stay valid across the unlock. */
                pthread_mutex_unlock(&ctx->mu);
                mock_fault_ctrl_delay();
                pthread_mutex_lock(&ctx->mu);
                err = k->string ? get_metadata(ctx, m, k->string) : EFAULT;
            }
            continue;
        }
        switch (k->id) {
        case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
            m->gop = (uint32_t)k->value;
            break;
        case MOCK_CID_IDR_INTERVAL:
            m->idr_interval = (uint32_t)k->value;
            break;
        case MOCK_CID_SPS_PPS_AT_IDR:
            m->headers_at_idr = k->value != 0;
            break;
        case MOCK_CID_INSERT_VUI:
            m->insert_vui = k->value != 0;
            break;
        case V4L2_CID_MPEG_VIDEO_BITRATE_PEAK:
            atomic_store(&peak_value, (uint32_t)k->value);
            atomic_store(&peak_seen, 1);
            break;
        case MOCK_CID_H265_PROFILE:
            m->main10 = (uint32_t)k->value == MOCK_H265_PROFILE_MAIN10;
            atomic_store(&profile_value, (uint32_t)k->value);
            atomic_store(&profile_seen, 1);
            break;
        case V4L2_CID_MPEG_MFC51_VIDEO_FORCE_FRAME_TYPE:
            m->force_idr = true;         /* consumed by the next frame     */
            break;
        default:
            break;                       /* accepted, no effect            */
        }
    }
    if (err) {
        errno = err;
        *ret = -1;
        return 1;
    }
    return 0;                            /* generic handler reports success */
}
