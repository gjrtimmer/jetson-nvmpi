/*
 * nvmpi_enc_av1.cpp — AV1-specific parts of nvmpi_create_encoder() (#8).
 *
 * AV1 encode reuses the generic encoder path (nvmpi_enc_init.cpp): same
 * planes, rate control, GOP and packet flow. Only these things differ, and
 * they live here so the H.264/HEVC code stays free of AV1 branches:
 *   - the CAPTURE fourcc, available only when the L4T headers carry the AV1
 *     encode controls (CMake probe NVMPI_HAVE_AV1_ENC);
 *   - request validation (8-bit 4:2:0 only);
 *   - nvEncParam fields that make no sense for AV1 are neutralized;
 *   - the AV1 headers-with-frame control (no IVF wrapper).
 *
 * Output contract (no libnvmpi API change): one low-overhead-format OBU
 * temporal unit per packet (temporal delimiter, sequence header on key
 * frames, frame OBU). libnvmpi does no av1C work; consumers take the
 * sequence header OBU from the first packet.
 */
#include "nvmpi_enc_internal.h"

/* CAPTURE-plane fourcc for AV1, or 0 when this build cannot encode AV1.
 * NVMPI_HAVE_AV1_ENC is set by CMake only when the Multimedia API headers
 * define both V4L2_PIX_FMT_AV1 and V4L2_CID_MPEG_VIDEOENC_AV1_HEADERS_WITH_FRAME
 * (R36/JetPack 6; R35/JetPack 5 headers lack the encode controls). The
 * caller turns 0 into a clean NULL from nvmpi_create_encoder(), the same as
 * for any other unsupported codec. */
uint32_t nvmpi_enc_av1_pixfmt(void)
{
#if defined(NVMPI_HAVE_AV1_ENC) && NVMPI_HAVE_AV1_ENC
	return V4L2_PIX_FMT_AV1;
#else
	return 0;
#endif
}

/* Reject requests the AV1 encoder cannot honour, before any device is
 * opened or anything allocated. Returns true when the request is valid.
 * Only 8-bit 4:2:0 input (NV12M / YUV420M) is supported: a 10-bit request
 * would otherwise be silently encoded as 8-bit by the generic raw-format
 * switch, which hides a caller bug. */
bool nvmpi_enc_av1_check(const nvEncParam* param)
{
	if (param->pixFormat == NV_PIX_P010) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "AV1 encode supports 8-bit 4:2:0 input only (P010 rejected)");
		return false;
	}
	return true;
}

/* Neutralize the ctx fields that carry H.264/HEVC-only settings, after
 * nvmpi_enc_translate_params() filled them. Not forwarded for AV1:
 *   - profile/level: NvVideoEncoder::setProfile() returns -1 for AV1; the
 *     hardware picks Main and the level itself. The profile is pinned to
 *     the HEVC "Main" enum so the generic raw-format switch stays 8-bit.
 *   - lossless, B-frames, POC type, VUI, AUD, CABAC: no AV1 equivalent.
 *   - qmin/qmax: the H.264 QP scale differs from AV1 qindex and the
 *     mapping is unverified, so rate control alone drives quality.
 * Sequence headers must accompany every key frame (a consumer can join at
 * any IDR), so SPS/PPS insertion is always requested. */
void nvmpi_enc_av1_translate(nvmpi_enc_ctx* ctx)
{
	ctx->profile = V4L2_MPEG_VIDEO_H265_PROFILE_MAIN;
	ctx->enableLossless = false;
	ctx->num_b_frames = (uint32_t) -1;   /* "unset": the B-frame control is skipped */
	ctx->poc_type = 0;
	ctx->insert_vui = false;
	ctx->insert_aud = false;
	ctx->enable_cabac = false;
	ctx->qmin = 0;
	ctx->qmax = 0;
	ctx->insert_sps_pps_at_idr = true;
}

/* Program the AV1-only controls. Must run after both plane formats are set
 * and before setupPlane() (V4L2 REQBUFS), which is where the driver reads
 * it. HEADERS_WITH_FRAME = 0 selects raw OBU output; the default on some
 * driver versions is IVF framing (a "DKIF" file header plus a per-frame
 * header), which no packet consumer expects. Returns 0, or -1 after
 * logging (the caller cleans up). */
int nvmpi_enc_av1_apply_ctrls(nvmpi_enc_ctx* ctx)
{
#if defined(NVMPI_HAVE_AV1_ENC) && NVMPI_HAVE_AV1_ENC
	struct v4l2_ext_control control;
	struct v4l2_ext_controls ctrls;

	memset(&control, 0, sizeof(control));
	memset(&ctrls, 0, sizeof(ctrls));
	ctrls.count = 1;
	ctrls.controls = &control;
	ctrls.ctrl_class = V4L2_CTRL_CLASS_MPEG;
	control.id = V4L2_CID_MPEG_VIDEOENC_AV1_HEADERS_WITH_FRAME;
	control.value = 0;

	if (ctx->enc->setExtControls(ctrls) < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not disable AV1 IVF headers");
		return -1;
	}
#else
	(void)ctx;
#endif
	return 0;
}
