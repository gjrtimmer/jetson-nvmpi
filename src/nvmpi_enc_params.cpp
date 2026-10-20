/*
 * nvmpi_enc_params.cpp — encoder parameter translation and codec controls.
 *
 * Pure refactor split out of nvmpi_enc_init.cpp (#140): translates the
 * caller's nvEncParam into nvmpi_enc_ctx fields and V4L2 enums (profile,
 * level and HW-preset maps), and programs the H.264/HEVC-specific V4L2
 * controls on an already-created NvVideoEncoder. nvmpi_create_encoder()
 * (nvmpi_enc_init.cpp) calls these at the points in its setup sequence where
 * the code originally lived; the order of V4L2 control calls is unchanged.
 */
#include "nvmpi_enc_internal.h"

/* Like TEST_ERROR but for helpers: log, then return -1 so the caller can
 * "goto cleanup" (TEST_ERROR's goto cannot cross function boundaries). */
#define ENC_CTRL_CHECK(condition, message, errorCode)    \
	if (condition) {                                     \
		NVMPI_LOG(NVMPI_LOG_ERROR, "%s (code=%d)",       \
		          message, (int)(errorCode));             \
		return -1;                                       \
	}

//Copy/translate the scalar nvEncParam settings into ctx. No allocation and
//no device access; pktPool, output_plane_fd and encoder_pixfmt stay with the
//caller (nvmpi_create_encoder).
void nvmpi_enc_translate_params(nvmpi_enc_ctx* ctx, const nvEncParam* param)
{
	ctx->index=0;
	ctx->width=param->width;
	ctx->height=param->height;
	ctx->enableLossless=false;
	ctx->bitrate=param->bitrate;
	ctx->peak_bitrate=param->peak_bitrate;
	ctx->ratecontrol = V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;
	ctx->idr_interval = param->idr_interval;
	ctx->fps_n = param->fps_n;
	ctx->fps_d = param->fps_d;
	ctx->iframe_interval = param->iframe_interval;
	ctx->enable_extended_colorformat=false;
	ctx->packets_num=param->capture_num;
	ctx->qmax=param->qmax;
	ctx->qmin=param->qmin;
	ctx->num_b_frames=param->max_b_frames;
	ctx->num_reference_frames=param->refs;
	ctx->insert_sps_pps_at_idr=(param->insert_spspps_idr==1)?true:false;
	ctx->insert_vui=(param->insert_vui!=0)?true:false;
	ctx->insert_aud=(param->insert_aud!=0)?true:false;
	ctx->enable_cabac=(param->enable_cabac!=0)?true:false;
	ctx->capPlaneGotEOS.store(false, std::memory_order_relaxed);
	ctx->flushing.store(false, std::memory_order_relaxed);
	/* Non-blocking mode: put_frame returns NVMPI_ERR_EAGAIN instead of
	 * blocking when no OUTPUT-plane buffer is available. Default (0) =
	 * blocking, preserving backward compatibility with existing callers. */
	ctx->blocking_mode = !param->nonblocking;
	ctx->dmabuf_external = (param->use_dmabuf != 0);
	ctx->max_perf = param->max_perf;
	ctx->poc_type = param->poc_type;
	ctx->vbv_buffer_size = param->vbv_buffer_size;

	/* Blocking-wait timeout: 0 = default (500ms), otherwise clamp to
	 * [100, 5000]ms — same range as the decoder (issue #10). */
	if (param->wait_timeout > 0)
		ctx->wait_timeout_ms = std::max(100u, std::min(param->wait_timeout, 5000u));
	else
		ctx->wait_timeout_ms = 500;

	//Profile mapping: the caller passes FFmpeg-style H.264 profile ids
	//(numeric values of FF_PROFILE_H264_*); translate to V4L2 enums.
	//Unknown/unset values fall back to Main.
	switch(param->profile)
	{
		case 77://FF_PROFILE_H264_MAIN
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
			break;
		case 66://FF_PROFILE_H264_BASELINE
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
			break;
		case 100://FF_PROFILE_H264_HIGH
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
			break;

		default:
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
			break;
	}

	//Level mapping: level_idc style values (10*level, e.g. 41 = 4.1) to
	//V4L2 enums. Both H.264 and HEVC share the same level_idc convention
	//from the FFmpeg wrapper; the correct V4L2 enum family is selected by
	//codingType. Default: 5.1 (also for the "auto"/0 option value).
	if (param->codingType == NV_VIDEO_CodingH264)
	{
		switch(param->level)
		{
			case 10: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_0; break;
			case 11: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_1; break;
			case 12: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_2; break;
			case 13: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_3; break;
			case 20: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_0; break;
			case 21: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_1; break;
			case 22: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_2; break;
			case 30: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_0; break;
			case 31: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_1; break;
			case 32: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_2; break;
			case 40: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_0; break;
			case 41: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_1; break;
			case 42: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_2; break;
			case 50: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_0; break;
			case 51: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_1; break;
			/* H.264 levels 5.2–6.2: added in linux/v4l2-controls.h from kernel
			 * 5.17+. JetPack 5.x (kernel 5.10) only defines up to 5.1; guard
			 * so builds succeed on older headers — falls through to default. */
#ifdef V4L2_MPEG_VIDEO_H264_LEVEL_5_2
			case 52: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_2; break;
#endif
#ifdef V4L2_MPEG_VIDEO_H264_LEVEL_6_0
			case 60: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_6_0; break;
#endif
#ifdef V4L2_MPEG_VIDEO_H264_LEVEL_6_1
			case 61: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_6_1; break;
#endif
#ifdef V4L2_MPEG_VIDEO_H264_LEVEL_6_2
			case 62: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_6_2; break;
#endif
			default: ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_1; break;
		}
	}
	else if (param->codingType == NV_VIDEO_CodingHEVC)
	{
		/* HEVC uses the same level_idc convention (10*level) from the
		 * FFmpeg wrapper. Tegra's V4L2 extensions define per-tier enums
		 * (v4l2_mpeg_video_h265_level in v4l2_nv_extensions.h); default
		 * to Main Tier since FFmpeg's -level does not distinguish tiers. */
		switch(param->level)
		{
			case 10: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_1_0_MAIN_TIER; break;
			case 20: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_2_0_MAIN_TIER; break;
			case 21: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_2_1_MAIN_TIER; break;
			case 30: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_3_0_MAIN_TIER; break;
			case 31: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_3_1_MAIN_TIER; break;
			case 40: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_4_0_MAIN_TIER; break;
			case 41: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_4_1_MAIN_TIER; break;
			case 50: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_5_0_MAIN_TIER; break;
			case 51: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_5_1_MAIN_TIER; break;
			/* HEVC levels 5.2–6.2: NVIDIA extension in v4l2_nv_extensions.h.
			 * Present on JetPack 6+ (Orin); may be absent on older JetPack
			 * or non-Jetson builds — guard for portability. */
#ifdef V4L2_MPEG_VIDEO_H265_LEVEL_5_2_MAIN_TIER
			case 52: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_5_2_MAIN_TIER; break;
#endif
#ifdef V4L2_MPEG_VIDEO_H265_LEVEL_6_0_MAIN_TIER
			case 60: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_6_0_MAIN_TIER; break;
#endif
#ifdef V4L2_MPEG_VIDEO_H265_LEVEL_6_1_MAIN_TIER
			case 61: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_6_1_MAIN_TIER; break;
#endif
#ifdef V4L2_MPEG_VIDEO_H265_LEVEL_6_2_MAIN_TIER
			case 62: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_6_2_MAIN_TIER; break;
#endif
			default: ctx->level=V4L2_MPEG_VIDEO_H265_LEVEL_5_1_MAIN_TIER; break;
		}
	}

	//HW preset mapping: 1=ultrafast .. 4=slow (default medium); trades
	//encoding speed against quality inside the hw encoder.
	switch(param->hw_preset_type)
	{
		case 1:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_ULTRAFAST;
			break;
		case 2:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_FAST;
			break;
		case 3:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_MEDIUM;
			break;
		case 4:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_SLOW;
			break;
		default:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_MEDIUM;
			break;
	}

	if(param->enableLossless)
		ctx->enableLossless=true;

	//rate control: CBR by default, VBR when requested
	if(param->mode_vbr)
		ctx->ratecontrol=V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
}

//Reference frames, B-frames, profile and level controls (H.264/HEVC).
//Returns 0, or -1 after logging when a control call fails (caller cleans up).
int nvmpi_enc_apply_gop_profile_ctrls(nvmpi_enc_ctx* ctx, const nvEncParam* param)
{
	int ret;

	if(ctx->num_reference_frames)
	{
		ret = ctx->enc->setNumReferenceFrames(ctx->num_reference_frames);
		ENC_CTRL_CHECK(ret < 0, "Could not set num reference frames", ret);
	}

	if(ctx->num_b_frames != (uint32_t) -1 && param->codingType == NV_VIDEO_CodingH264)
	{
		ret = ctx->enc->setNumBFrames(ctx->num_b_frames);
		ENC_CTRL_CHECK(ret < 0, "Could not set number of B Frames", ret);
	}


	if(param->codingType == NV_VIDEO_CodingH264 || param->codingType == NV_VIDEO_CodingHEVC)
	{
		ret = ctx->enc->setProfile(ctx->profile);
		ENC_CTRL_CHECK(ret < 0, "Could not set encoder profile", ret);
	}

	if(param->codingType == NV_VIDEO_CodingH264 || param->codingType == NV_VIDEO_CodingHEVC)
	{
		ret = ctx->enc->setLevel(ctx->level);
		ENC_CTRL_CHECK(ret < 0, "Could not set encoder level", ret);
	}

	return 0;
}

//POC type, SPS/PPS-at-IDR, VUI, CABAC and AUD controls. Must run after the
//plane formats are set and before STREAMON. Returns 0, or -1 after logging
//when a control call fails (caller cleans up).
int nvmpi_enc_apply_stream_ctrls(nvmpi_enc_ctx* ctx, const nvEncParam* param)
{
	int ret;

	if(ctx->poc_type)
	{
		ret = ctx->enc->setPocType(ctx->poc_type);
		ENC_CTRL_CHECK(ret < 0, "Error while setting encoder poc_type", ret);
	}

	if(ctx->insert_sps_pps_at_idr){
		ret = ctx->enc->setInsertSpsPpsAtIdrEnabled(true);
		ENC_CTRL_CHECK(ret < 0, "Could not set insertSPSPPSAtIDR", ret);
	}

	//VUI carries timing_info (fps) in the SPS so players/muxers report the
	//correct frame rate. Independent of SPS/PPS-at-IDR. Must be set after
	//setFormat (done above) and before STREAMON; the fps value comes from
	//setFrameRate below, which the firmware reads together with this flag.
	if(ctx->insert_vui){
		ret = ctx->enc->setInsertVuiEnabled(true);
		ENC_CTRL_CHECK(ret < 0, "Could not set insertVUI", ret);
	}

	/* CABAC entropy coding (H.264 only): CABAC achieves ~10-15% better
	 * compression than CAVLC at the cost of slightly higher decode
	 * complexity. setCABAC() wraps V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE;
	 * must be called after setFormat. Ignored for HEVC (always CABAC). */
	if (ctx->enable_cabac && param->codingType == NV_VIDEO_CodingH264) {
		ret = ctx->enc->setCABAC(true);
		ENC_CTRL_CHECK(ret < 0, "Could not enable CABAC", ret);
	}

	/* Access Unit Delimiter insertion: some transport streams and HLS
	 * workflows require AUD NALs. setInsertAudEnabled() wraps
	 * V4L2_CID_MPEG_VIDEO_H264_AUD_SAR_ENABLE. */
	if (ctx->insert_aud) {
		ret = ctx->enc->setInsertAudEnabled(true);
		ENC_CTRL_CHECK(ret < 0, "Could not enable AUD insertion", ret);
	}

	return 0;
}
