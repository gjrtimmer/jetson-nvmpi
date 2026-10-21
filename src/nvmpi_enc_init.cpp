/*
 * nvmpi_enc_init.cpp — encoder creation/initialization (layer 1).
 *
 * Implements nvmpi_create_encoder() from the public C API in include/nvmpi.h
 * on top of NVIDIA's V4L2 NvVideoEncoder sample class. Consumed by the
 * FFmpeg wrapper in ffmpeg/dev/common/libavcodec/nvmpi_enc.c.
 *
 * V4L2 encoder model (M2M device, two queues — note the naming is from the
 * device's point of view, mirrored relative to the decoder):
 *   - OUTPUT plane  = input side: raw YUV frames go in.
 *   - CAPTURE plane = output side: encoded bitstream comes out.
 *
 * This file owns the device-open, format-negotiation, and plane/stream
 * setup sequence. Frame submission (nvmpi_encoder_put_frame[_fd]) lives in
 * nvmpi_enc_input.cpp / nvmpi_enc_input_fd.cpp; packet retrieval and control API (get_packet,
 * force_idr, set_bitrate, flush, close) live in nvmpi_enc_api.cpp.
 *
 * Companion files:
 *   nvmpi_enc_internal.h  — shared includes, defines, nvmpi_enc_ctx struct,
 *                           and forward declarations.
 *   nvmpi_enc_params.cpp  — nvEncParam -> ctx translation (profile/level/
 *                           preset maps) and H.264/HEVC control programming.
 *   nvmpi_enc_av1.cpp     — AV1 fourcc gate, request checks and controls (#8).
 *   nvmpi_enc_output.cpp  — encoder_capture_plane_dq_callback and
 *                           setup_output_dmabuf (OUTPUT-plane DMA path).
 */
#include "nvmpi_enc_internal.h"

//Public API: create and start an encoder.
//Copies/translates all nvEncParam settings into the context, then programs
//the V4L2 encoder: plane formats, bitrate & rate control, profile/level,
//presets, GOP structure, QP range, SPS/PPS insertion, framerate; sets up
//both planes, STREAMONs them, starts the capture DQ thread and pre-queues
//all empty CAPTURE buffers. Returns the new context (setup errors are only
//logged). The caller still must populate the packet pool afterwards.
nvmpictx* nvmpi_create_encoder(nvEncParam* param)
{
	int ret;
	log_level = LOG_LEVEL_INFO;
	/* Validate the codec before anything is allocated or any device is
	 * opened: only H.264, HEVC and AV1 are encodable, and an unset fourcc would
	 * otherwise reach the format negotiation (#134). */
	uint32_t pixfmt;
	if(param->codingType==NV_VIDEO_CodingH264)
		pixfmt=V4L2_PIX_FMT_H264;
	else if(param->codingType==NV_VIDEO_CodingHEVC)
		pixfmt=V4L2_PIX_FMT_H265;
	else if(param->codingType==NV_VIDEO_CodingAV1)
	{
		/* AV1 (#8): 0 = headers lack the AV1 encode controls (JetPack 5). */
		pixfmt=nvmpi_enc_av1_pixfmt();
		if(!pixfmt)
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "AV1 encode not supported by this L4T build");
			return NULL;
		}
		if(!nvmpi_enc_av1_check(param))
			return NULL;
	}
	else
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "Unsupported encoder codingType %d (H.264, HEVC and AV1 only)", (int)param->codingType);
		return NULL;
	}
	/* P010 / Main10 vs codec (P010: HEVC Main10 or AV1, #146). */
	if(!nvmpi_enc_check_input(param))
		return NULL;
	nvmpi_enc_ctx *ctx=new nvmpi_enc_ctx;
	ctx->encoder_pixfmt=pixfmt;
	/* Scalar nvEncParam -> ctx translation (profile/level/preset maps). */
	nvmpi_enc_translate_params(ctx, param);
	if(param->codingType==NV_VIDEO_CodingAV1)
		nvmpi_enc_av1_translate(ctx, param);	/* drop H.264/HEVC-only settings */
	ctx->pktPool = new NVMPI_bufPool<nvPacket*>();
	/* Always allocate the output-plane fd array: used by both compile-time
	 * DMA mode (OUTPLANE_MEMTYPE_DMA) and runtime external DMABUF mode
	 * (dmabuf_external). Initialized to -1 so cleanup knows which slots
	 * were filled. Freed in nvmpi_encoder_close(). */
	ctx->output_plane_fd = new int[ctx->packets_num];
	for (uint32_t i = 0; i < ctx->packets_num; i++)
		ctx->output_plane_fd[i] = -1;

	/* Open the V4L2 encoder device. Retry up to 3 times with 100ms
	 * backoff. Before each factory call, probe the device node ourselves
	 * to capture errno — MMAPI's NvV4l2Element discards it, making EBUSY
	 * indistinguishable from ENODEV at the factory level. See #37.
	 *
	 * Device paths from MMAPI source (NvVideoEncoder.cpp):
	 *   primary: /dev/nvhost-msenc   (nvhost driver, JetPack 4.x)
	 *   fallback: /dev/v4l2-nvenc    (V4L2 kernel driver, JetPack 5+) */
	for (int attempt = 0; attempt < 3; attempt++) {
		/* Probe: resolve the same device path MMAPI would use. */
		const char *dev_path = NULL;
		if (access("/dev/nvhost-msenc", F_OK) == 0)
			dev_path = "/dev/nvhost-msenc";
		else if (access("/dev/v4l2-nvenc", F_OK) == 0)
			dev_path = "/dev/v4l2-nvenc";

		if (dev_path) {
			int probe_fd = open(dev_path, O_RDWR);
			if (probe_fd < 0) {
				int saved_errno = errno;
				if (saved_errno == ENODEV || saved_errno == ENXIO) {
					/* Device node exists but driver not loaded or hw absent —
					 * permanent failure, no point retrying. */
					NVMPI_LOG(NVMPI_LOG_ERROR,
						  "Encoder device %s unavailable (errno=%d: %s)",
						  dev_path, saved_errno, strerror(saved_errno));
					goto cleanup;
				}
				if (saved_errno == EACCES) {
					NVMPI_LOG(NVMPI_LOG_ERROR,
						  "Encoder device %s: permission denied", dev_path);
					goto cleanup;
				}
				if (saved_errno == EBUSY) {
					NVMPI_LOG(NVMPI_LOG_WARN,
						  "Encoder device %s busy (EBUSY), retrying (%d/3)...",
						  dev_path, attempt + 1);
					usleep(100000);
					continue;
				}
				/* Other errno (EIO, etc.) — log and fall through to factory. */
				NVMPI_LOG(NVMPI_LOG_WARN,
					  "Encoder device probe failed (errno=%d: %s), trying factory...",
					  saved_errno, strerror(saved_errno));
			} else {
				/* Probe succeeded — device is accessible. Close the probe fd
				 * immediately and let the factory do the real open. */
				close(probe_fd);
			}
		} else {
			/* Neither device node exists — MMAPI factory will also fail. */
			NVMPI_LOG(NVMPI_LOG_ERROR,
				  "No encoder device node found (/dev/nvhost-msenc or /dev/v4l2-nvenc)");
			goto cleanup;
		}

		/* Always open encoder WITHOUT O_NONBLOCK — the flag applies to ALL
		 * V4L2 ioctls on the fd, including the CAPTURE-plane DQ thread's
		 * dqBuffer. O_NONBLOCK would prevent the DQ thread from blocking
		 * for encoded packets, causing a hang. Non-blocking behavior is
		 * implemented per-call via dqBuffer(timeout=0) on the OUTPUT
		 * plane only (see nvmpi_encoder_put_frame). */
		ctx->enc.reset(NvVideoEncoder::createVideoEncoder("enc0"));
		if (ctx->enc) break;
		if (attempt < 2) {
			NVMPI_LOG(NVMPI_LOG_WARN, "Encoder factory returned NULL, retrying (%d/3)...", attempt + 1);
			usleep(100000);
		}
	}
	if (!ctx->enc)
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not create encoder after 3 attempts");
		goto cleanup;
	}

	/* CRITICAL FORMAT SETUP — format negotiation must succeed for the
	 * encoder to function. Unlike parameter-tuning calls (setBitrate,
	 * setProfile, etc.) which may fall back to defaults, a format failure
	 * means the V4L2 device is in an unusable state. Abort to cleanup. */

	//CAPTURE plane carries the compressed bitstream; each buffer is sized
	//NVMPI_ENC_CHUNK_SIZE — the same constant the wrapper uses for packets
	ret = ctx->enc->setCapturePlaneFormat(ctx->encoder_pixfmt, ctx->width,ctx->height, NVMPI_ENC_CHUNK_SIZE);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not set capture plane format");
		goto cleanup;
	}

	//pick the raw input format. Priority: lossless (YUV444M, set below) >
	//HEVC Main10 / AV1 P010 (10-bit P010M) > requested 8-bit layout (NV12M vs YUV420M).
	//NV12 only affects the 8-bit default branch; it never overrides the
	//10-bit or lossless paths.
	switch (ctx->profile)
	{
		case V4L2_MPEG_VIDEO_H265_PROFILE_MAIN10:
			ctx->raw_pixfmt = V4L2_PIX_FMT_P010M;
			break;
		case V4L2_MPEG_VIDEO_H265_PROFILE_MAIN:
		default:
			ctx->raw_pixfmt = (param->pixFormat == NV_PIX_NV12) ? V4L2_PIX_FMT_NV12M : V4L2_PIX_FMT_YUV420M;
	}

	//lossless H.264 requires the High 4:4:4 Predictive profile and YUV444
	//input; otherwise use the raw format chosen above
	if (ctx->enableLossless && param->codingType == NV_VIDEO_CodingH264)
	{
		ctx->profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE;
		ret = ctx->enc->setOutputPlaneFormat(V4L2_PIX_FMT_YUV444M, ctx->width,ctx->height);
	}
	else
	{
		ret = ctx->enc->setOutputPlaneFormat(ctx->raw_pixfmt, ctx->width,ctx->height);
	}
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not set output plane format");
		goto cleanup;
	}

	ret = ctx->enc->setBitrate(ctx->bitrate);
	TEST_ERROR(ret < 0, "Could not set encoder bitrate", ret);

	if(ctx->vbv_buffer_size)
	{
		/* Set virtual buffer size value for encoder */
		ret = ctx->enc->setVirtualBufferSize(ctx->vbv_buffer_size);
		TEST_ERROR(ret < 0, "Could not set virtual buffer size", ret);
	}

	ret=ctx->enc->setHWPresetType(ctx->hw_preset_type);
	TEST_ERROR(ret < 0, "Could not set encoder HW Preset Type", ret);

	/* Reference frames, B-frames, profile, level (H.264/HEVC). */
	if (nvmpi_enc_apply_gop_profile_ctrls(ctx, param) < 0)
		goto cleanup;

	//Rate control: lossless mode pins QP to 0 (no rate control); otherwise
	//apply CBR/VBR, and for VBR also program a peak bitrate.
	if (ctx->enableLossless)
	{
		ret = ctx->enc->setConstantQp(0);
		TEST_ERROR(ret < 0, "Could not set encoder constant qp=0", ret);

	}
	else
	{
		ret = ctx->enc->setRateControlMode(ctx->ratecontrol);
		TEST_ERROR(ret < 0, "Could not set encoder rate control mode", ret);

		if (ctx->ratecontrol == V4L2_MPEG_VIDEO_BITRATE_MODE_VBR)
		{
			uint32_t peak_bitrate;
			/* peak must be >= target; derive 1.2x target if unset/invalid */
			if (ctx->peak_bitrate < ctx->bitrate) {
				peak_bitrate = 1.2f * ctx->bitrate;
				NVMPI_LOG(NVMPI_LOG_WARN, "peak_bitrate %u below bitrate %u, auto-corrected to %u",
				          ctx->peak_bitrate, ctx->bitrate, peak_bitrate);
			} else
				peak_bitrate = ctx->peak_bitrate;
			ret = ctx->enc->setPeakBitrate(peak_bitrate);
			TEST_ERROR(ret < 0, "Could not set encoder peak bitrate", ret);
		}
	}

	ret = ctx->enc->setIDRInterval(ctx->idr_interval);
	TEST_ERROR(ret < 0, "Could not set encoder IDR interval", ret);

	//same QP range applied to I, P and B frames
	if(ctx->qmax>0 ||ctx->qmin >0){
		ctx->enc->setQpRange(ctx->qmin, ctx->qmax, ctx->qmin,ctx->qmax, ctx->qmin, ctx->qmax);
	}
	ret = ctx->enc->setIFrameInterval(ctx->iframe_interval);
	TEST_ERROR(ret < 0, "Could not set encoder I-Frame interval", ret);

	if(ctx->max_perf)
	{
		ret = ctx->enc->setMaxPerfMode(1);
		TEST_ERROR(ret < 0, "Error while setting encoder to max perf", ret);
	}

	/* POC type, SPS/PPS at IDR, VUI, CABAC, AUD. */
	if (nvmpi_enc_apply_stream_ctrls(ctx, param) < 0)
		goto cleanup;
	/* AV1: raw OBU output instead of IVF, before the planes are set up. */
	if (param->codingType == NV_VIDEO_CodingAV1 && nvmpi_enc_av1_apply_ctrls(ctx) < 0)
		goto cleanup;

	ret = ctx->enc->setFrameRate(ctx->fps_n, ctx->fps_d);
	TEST_ERROR(ret < 0, "Could not set framerate", ret);

	/* CRITICAL PLANE/STREAM SETUP — if any of these fail the V4L2 device
	 * cannot operate; abort to cleanup instead of returning a broken ctx.
	 * Without this, the TEST_ERROR (log-only) macro allowed half-init
	 * encoders to reach the caller; under concurrent stress the resulting
	 * crashes leaked DMA memory and cascaded to all subsequent jobs. */

	//ret = ctx->enc->output_plane.setupPlane(V4L2_MEMORY_USERPTR, ctx->packets_num, false, true);
	if (ctx->dmabuf_external) {
		/* External DMA-BUF mode: OUTPUT plane uses V4L2_MEMORY_DMABUF.
		 * Internal NV12 VIDEO_ENC surfaces are allocated lazily in
		 * put_frame_fd on first use of each slot — this defers NVMM
		 * allocation until after any temporary encoder (extradata
		 * extraction) is destroyed, avoiding NVMM exhaustion on
		 * memory-constrained devices (Orin Nano). */
		ret = ctx->enc->output_plane.setupPlane(V4L2_MEMORY_DMABUF, ctx->packets_num, false, false);
	} else {
#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_MMAP)
		ret = ctx->enc->output_plane.setupPlane(V4L2_MEMORY_MMAP, ctx->packets_num, true, false);
#else
		ret = setup_output_dmabuf(ctx,ctx->packets_num); //V4L2_MEMORY_DMABUF
#endif
	}
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not setup output plane");
		goto cleanup;
	}

	ret = ctx->enc->capture_plane.setupPlane(V4L2_MEMORY_MMAP, ctx->packets_num, true, false);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not setup capture plane");
		goto cleanup;
	}

	ret = ctx->enc->subscribeEvent(V4L2_EVENT_EOS,0,0);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not subscribe EOS event");
		goto cleanup;
	}

	ret = ctx->enc->output_plane.setStreamStatus(true);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error in output plane streamon");
		goto cleanup;
	}

	ret = ctx->enc->capture_plane.setStreamStatus(true);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error in capture plane streamon");
		goto cleanup;
	}

	/* CAPTURE-plane DQ thread: always started regardless of blocking_mode.
	 * The DQ thread processes encoded output from the CAPTURE plane — needed
	 * in both blocking and non-blocking modes. Non-blocking only changes the
	 * OUTPUT-plane dqBuffer timeout (put_frame), not the capture path. */
	ctx->enc->capture_plane.setDQThreadCallback(encoder_capture_plane_dq_callback);
	ret = ctx->enc->capture_plane.startDQThread(ctx);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not start DQ thread");
		goto cleanup;
	}
	ctx->dq_thread_started = true;

	// Enqueue all the empty capture plane buffers
	for (uint32_t i = 0; i < ctx->enc->capture_plane.getNumBuffers(); i++){
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];
		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;

		ret = ctx->enc->capture_plane.qBuffer(v4l2_buf, NULL);
		if (ret < 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error while queueing buffer at capture plane (buf %u)", i);
			goto cleanup;
		}
	}

	return ctx;

	/* Cleanup on critical V4L2 setup failure. The DQ thread is only
	 * started after STREAMON; dq_thread_started gates the join.
	 * The NvVideoEncoder destructor (via unique_ptr reset) handles
	 * STREAMOFF + V4L2 buffer teardown. Freed here: enc (via reset),
	 * pktPool (allocated above), and ctx itself. */
cleanup:
	if (nvmpi_enc_stop_dq_thread(ctx) < 0) {
		/* Thread still alive and touching ctx->enc: freeing would be a
		 * use-after-free. Leak the half-built context and fail create. */
		NVMPI_LOG(NVMPI_LOG_ERROR, "DQ thread did not exit during create cleanup; leaking context");
		return NULL;
	}
	ctx->enc.reset();
	delete ctx->pktPool;
	/* output_plane_fd entries initialized to -1 on alloc; only destroy
	 * fds that were successfully allocated (fd >= 0). */
	if (ctx->output_plane_fd) {
		for (uint32_t i = 0; i < ctx->packets_num; i++) {
			if (ctx->output_plane_fd[i] >= 0) {
				NvBufferDestroy(ctx->output_plane_fd[i]);
				ctx->output_plane_fd[i] = -1;
			}
		}
		delete[] ctx->output_plane_fd;
	}
	delete ctx;
	return NULL;
}
