/*
 * nvmpi_enc_jpeg.cpp — hardware MJPEG/JPEG encoder via NvJPEGEncoder.
 *
 * This is a synchronous, per-frame encode path using the Tegra NVJPG engine.
 * It does NOT use V4L2 M2M — there is no DQ thread, no OUTPUT/CAPTURE plane
 * negotiation, and no packet pool.
 *
 * Lifecycle:
 *   nvmpi_create_jpeg_encoder()         — create NvJPEGEncoder
 *   nvmpi_jpeg_encoder_put_frame()      — copy YUV420 → DMA buf → encodeFromFd
 *   nvmpi_jpeg_encoder_get_packet()     — retrieve staged JPEG output
 *   nvmpi_jpeg_encoder_close()          — destroy everything
 *
 * The encoder is simpler than the decoder: no frame pool, no VIC transform
 * (the buffer side lives in nvmpi_enc_jpeg_buf.cpp).
 * The caller's YUV420 frame is memcpy'd into a pitch-linear DMA buffer
 * (which encodeFromFd expects), then the NVJPG engine produces baseline JPEG.
 *
 * Hardware constraint: Orin Nano has no NVJPG encode capability. The
 * NvJPEGEncoder::createJPEGEncoder() call returns NULL on such modules,
 * which we propagate as a clean creation failure.
 *
 * Allocated by nvmpi_create_jpeg_encoder(); freed by nvmpi_jpeg_encoder_close().
 */
#include "nvmpi_enc_jpeg_internal.h"
#include "nvmpi_frame_fit.h"

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * Create a JPEG encoder context.
 *
 * Opens the Tegra NVJPG encode engine via NvJPEGEncoder::createJPEGEncoder().
 * No V4L2 device is used — JPEG encode is backed by libnvjpeg (patched
 * libjpeg-8b with TEGRA_ACCELERATE).
 *
 * The input DMA buffer is NOT allocated here — it is lazily allocated on
 * the first put_frame when the frame dimensions become known.
 *
 * Returns NULL on NvJPEGEncoder creation failure. This is the expected
 * path on Orin Nano which has no NVJPG encode capability (NVIDIA-confirmed:
 * Keylost#43). The FFmpeg wrapper reports a clear error with fallback
 * suggestion when this returns NULL.
 *
 * quality: libjpeg quality 1-100. Clamped to [1, 100].
 *
 * Allocated here; freed by nvmpi_jpeg_encoder_close().
 */
nvmpictx* nvmpi_create_jpeg_encoder(int quality)
{
	nvmpictx_jpegenc *ctx = new nvmpictx_jpegenc();
	memset(ctx->plane_pitch, 0, sizeof(ctx->plane_pitch));
	memset(ctx->plane_height, 0, sizeof(ctx->plane_height));
	memset(ctx->plane_linedata, 0, sizeof(ctx->plane_linedata));

	/* Clamp quality to valid libjpeg range. */
	if (quality < 1) quality = 1;
	if (quality > 100) quality = 100;
	ctx->quality = quality;

	/* Allocated here; freed in nvmpi_jpeg_encoder_close() via delete.
	 * Returns NULL on Orin Nano (no NVJPG encode engine). */
	ctx->jpegenc = NvJPEGEncoder::createJPEGEncoder("jpegenc");
	if (!ctx->jpegenc) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "NvJPEGEncoder creation failed. This module may not have NVJPG encode capability (e.g. Orin Nano). Use software mjpeg encoder as fallback.");
		delete ctx;
		return NULL;
	}

	/* Cast to opaque C API handle (nvmpictx* from nvmpi.h). */
	return reinterpret_cast<nvmpictx*>(ctx);
}

/*
 * Encode one YUV420 frame to JPEG synchronously.
 *
 * Flow:
 *   1. Validate input; NULL frame signals EOS.
 *   2. On resolution change: (re)allocate input DMA buffer.
 *   3. Copy caller's nvFrame planes into the DMA buffer.
 *   4. Call encodeFromFd() — NVJPG engine produces baseline JPEG.
 *   5. Stage the JPEG output for retrieval via get_packet().
 *
 * Returns 0 on success, NVMPI_ERR_INVAL when a plane is too small for the
 * encoder's frame size (#151), -1 on other errors.
 */
int nvmpi_jpeg_encoder_put_frame(nvmpictx *handle, nvFrame *frame)
{
	if (!handle)
		return -1;

	nvmpictx_jpegenc *ctx = reinterpret_cast<nvmpictx_jpegenc*>(handle);

	/* EOS marker: NULL frame signals end of stream. */
	if (!frame) {
		ctx->eos.store(true);
		return 0;
	}

	if (ctx->eos.load())
		return -1;

	/* Validate frame dimensions against hardware limits. */
	if (frame->width == 0 || frame->height == 0 ||
	    frame->width > NVJPEG_ENC_MAX_DIM || frame->height > NVJPEG_ENC_MAX_DIM) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "frame dimensions %ux%u out of range (max %u)", frame->width, frame->height, (unsigned)NVJPEG_ENC_MAX_DIM);
		return -1;
	}

	/* Validate that frame planes are non-NULL. */
	if (!frame->payload[0] || !frame->payload[1] || !frame->payload[2]) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "null frame plane pointer");
		return -1;
	}

	/* Drop any un-consumed previous output. The FFmpeg wrapper should
	 * always call get_packet between put_frame calls, but guard against
	 * misuse. */
	jpegDropStaged(ctx);

	/* ---- (Re)allocate input DMA buffer on resolution change ---- */
	if (frame->width != ctx->width || frame->height != ctx->height) {
		if (!ctx->allocInputBuffer(frame->width, frame->height)) {
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "input buffer allocation failed");
			return -1;
		}
	}

	/* ---- Caller plane check (#151) ----
	 * The copy reads (rows-1)*linesize + row bytes from every plane, rows and
	 * row bytes being the encoder's own geometry (frame size, not the caller's
	 * allocation). A linesize below the row length would overlap rows; a
	 * payload_size[p] smaller than the span would read past the caller's
	 * buffer. Refuse both before anything is copied or encoded.
	 * payload_size[p] == 0 means "not declared": the caller guarantees it. */
	for (unsigned int p = 0; p < ctx->num_planes && p < 3; p++) {
		size_t need;

		if (frame->linesize[p] < ctx->plane_linedata[p] ||
		    !nvmpi_plane_need(ctx->plane_height[p], frame->linesize[p],
		                      ctx->plane_linedata[p], &need) ||
		    (frame->payload_size[p] && need > frame->payload_size[p])) {
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "put_frame: plane %u linesize %u / payload_size %lu too small for %ux%u",
			              p, frame->linesize[p], frame->payload_size[p], ctx->width, ctx->height);
			return NVMPI_ERR_INVAL;
		}
	}

	/* ---- Copy caller's frame into DMA buffer ---- */
	int ret = jpegCopyFromFrame(ctx, frame);
	if (ret != 0) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "frame copy to DMA buffer failed");
		return -1;
	}

	/* ---- Hardware encode + stage output ---- */
	if (jpegEncodeStage(ctx, ctx->input_buf.dst_dma_fd, ctx->width, ctx->height,
	                    frame->timestamp) != 0)
		return -1;

	return 0;
}

/*
 * Retrieve the encoded JPEG packet.
 *
 * Copies the staged JPEG output into the caller's nvPacket and frees
 * the internal buffer. Returns 0 on success, -1 if no packet is ready,
 * -2 on EOS with no packet.
 */
int nvmpi_jpeg_encoder_get_packet(nvmpictx *handle, nvPacket *packet)
{
	if (!handle || !packet)
		return -1;

	nvmpictx_jpegenc *ctx = reinterpret_cast<nvmpictx_jpegenc*>(handle);

	if (!ctx->packet_ready) {
		if (ctx->eos.load())
			return -2;
		return -1;
	}

	/* Copy JPEG data to caller's packet buffer. */
	packet->payload_size = ctx->out_buf_size;
	if (packet->payload && ctx->out_buf_size > 0) {
		memcpy(packet->payload, ctx->out_buf, ctx->out_buf_size);
	}
	packet->pts = ctx->out_pts;
	/* JPEG frames are always keyframes. */
	packet->flags = 1;

	/* Free the internal JPEG buffer.
	 * Zero out before freeing — encoded frame data. */
	if (ctx->out_buf) {
		memset(ctx->out_buf, 0, ctx->out_buf_size);
		free(ctx->out_buf);
		ctx->out_buf = nullptr;
	}
	ctx->out_buf_size = 0;
	ctx->packet_ready = false;

	return 0;
}

/*
 * Destroy the JPEG encoder context and free all resources.
 *
 * Teardown order:
 *   1. Set EOS.
 *   2. Free any staged output buffer.
 *   3. Free input DMA buffer and the VIC context.
 *   4. Delete NvJPEGEncoder and context.
 *
 * The handle is invalid after this call.
 */
int nvmpi_jpeg_encoder_close(nvmpictx *handle)
{
	if (!handle)
		return -1;

	nvmpictx_jpegenc *ctx = reinterpret_cast<nvmpictx_jpegenc*>(handle);

	ctx->eos.store(true);

	/* Free staged output buffer (zeroed first, see jpegDropStaged). */
	jpegDropStaged(ctx);

	/* Free input DMA buffer.
	 * Allocated by allocInputBuffer(); freed here. */
	ctx->freeInputBuffer();

	/* Allocated by put_frame_fd() via nvmpi_vic_create(); freed here (NULL-safe). */
	nvmpi_vic_close(ctx->vic);
	ctx->vic = nullptr;

	/* Allocated by nvmpi_create_jpeg_encoder() via createJPEGEncoder();
	 * freed here. Destructor calls jpeg_destroy_compress. */
	if (ctx->jpegenc) {
		delete ctx->jpegenc;
		ctx->jpegenc = nullptr;
	}

	delete ctx;
	return 0;
}
