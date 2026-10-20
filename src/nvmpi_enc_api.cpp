/*
 * nvmpi_enc_api.cpp — encoder packet/control API of libnvmpi (layer 1).
 *
 * Implements the packet-pool operations, packet retrieval, and control
 * surface (force_idr, set_bitrate, flush, close) of the public C API in
 * include/nvmpi.h on top of NVIDIA's V4L2 NvVideoEncoder sample class.
 * Consumed by the FFmpeg wrapper in ffmpeg/dev/common/libavcodec/nvmpi_enc.c.
 *
 * V4L2 encoder model (M2M device, two queues — note the naming is from the
 * device's point of view, mirrored relative to the decoder):
 *   - OUTPUT plane  = input side: raw YUV frames go in.
 *   - CAPTURE plane = output side: encoded bitstream comes out.
 *
 * Data flow / threading:
 *   DQ thread:        NvVideoEncoder's capture-plane dequeue thread invokes
 *                     encoder_capture_plane_dq_callback() per encoded
 *                     buffer; the callback copies the bitstream into an
 *                     "empty" nvPacket from pktPool and publishes it as
 *                     "filled", then re-queues the V4L2 buffer.
 *   user thread:      nvmpi_encoder_get_packet() pops filled packets;
 *                     the caller returns them via qEmptyPacket().
 *
 * The nvPacket pool itself is filled by the caller (the FFmpeg wrapper
 * allocates packets backed by AVPacket buffers) — libnvmpi never allocates
 * or frees packet memory.
 *
 * Companion files:
 *   nvmpi_enc_internal.h  — shared includes, defines, nvmpi_enc_ctx struct,
 *                           and forward declarations.
 *   nvmpi_enc_init.cpp    — nvmpi_create_encoder() (device open, format
 *                           negotiation, plane/stream setup).
 *   nvmpi_enc_input.cpp   — copyFrameToNvBuf(), nvmpi_encoder_put_frame().
 *   nvmpi_enc_input_fd.cpp — nvmpi_encoder_put_frame_fd() (DMA-BUF).
 *   nvmpi_enc_output.cpp  — encoder_capture_plane_dq_callback and
 *                           setup_output_dmabuf (OUTPUT-plane DMA path).
 */
#include "nvmpi_enc_internal.h"

//Public API: take an empty packet out of the pool (e.g. to free or replace
//it). Returns -1 when the empty queue is exhausted AND the DQ thread holds no
//packet in flight. If the queue is empty only because the DQ callback has
//taken a packet and not yet published it, waits for that publish (one AU
//memcpy; capped at 1 s as a backstop). The caller assumes ownership of the
//returned packet. After -1 the DQ thread cannot take another packet (the
//queue is empty), so draining empty-then-filled misses none.
int nvmpi_encoder_dqEmptyPacket(nvmpictx* h,nvPacket** packet)
{
	nvmpi_enc_ctx* ctx = static_cast<nvmpi_enc_ctx*>(h);
	nvPacket* pkt = ctx->pktPool->dqEmptyBufSettled(std::chrono::milliseconds(1000));
	if(!pkt) return -1;
	*packet = pkt;
	return 0;
}

//Public API: donate an empty, caller-allocated packet to the pool so the
//capture DQ callback can fill it. The pool stores only the pointer; the
//caller remains responsible for the packet's eventual deallocation.
void nvmpi_encoder_qEmptyPacket(nvmpictx* h,nvPacket* packet)
{
	nvmpi_enc_ctx* ctx = static_cast<nvmpi_enc_ctx*>(h);
	ctx->pktPool->qEmptyBuf(packet);
	return;
}

//Public API: fetch the next encoded packet.
//While encoding: non-blocking, -1 when nothing is ready (wrapper maps this
//When wait=false: non-blocking, returns -1 immediately if no packet ready
//(maps to EAGAIN). When wait=true: blocks up to wait_timeout_ms using the
//pool's CV-based tiered wait. While flushing: poll every 1ms until either
//a packet arrives (0) or the DQ callback saw the EOS buffer (-2).
//On success the caller holds the packet until re-queueing it via
//nvmpi_encoder_qEmptyPacket().
int nvmpi_encoder_get_packet(nvmpictx* h, nvPacket** packet, bool wait)
{
	nvmpi_enc_ctx* ctx = static_cast<nvmpi_enc_ctx*>(h);
	nvPacket* pkt;

	if (wait)
		pkt = ctx->pktPool->dqFilledBuf(
			std::chrono::milliseconds(ctx->wait_timeout_ms));
	else
		pkt = ctx->pktPool->dqFilledBuf();

	if(!pkt)
	{
		if(!ctx->flushing.load(std::memory_order_acquire)) return -1;
		/* Read the EOS flag BEFORE polling the filled queue (#111). The DQ
		 * thread publishes every packet (qFilledBuf) before it stores the
		 * EOS flag (release), so once this acquire load sees EOS, all
		 * packets are already queued and a following empty dq means the
		 * stream is really drained. The old order (poll, then load) let
		 * the DQ thread publish the last packets and set EOS in between,
		 * and the tail of the stream was lost (-2 with packets queued). */
		for(;;)
		{
			bool eos = ctx->capPlaneGotEOS.load(std::memory_order_acquire);
			pkt = ctx->pktPool->dqFilledBuf();
			if(pkt || eos) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if(!pkt) return -2; //if got eos
	}

	*packet = pkt;
	return 0;
}

//Public API: force the next encoded frame to be an IDR (keyframe on demand).
//Calls NvVideoEncoder::forceIDR() which sets the V4L2 control
//V4L2_CID_MPEG_MFC51_VIDEO_FORCE_FRAME_TYPE. Safe to call from the user
//thread while the DQ thread is running — the V4L2 ioctl is thread-safe.
//Returns 0 on success, -1 on ioctl failure.
int nvmpi_encoder_force_idr(nvmpictx* h)
{
	nvmpi_enc_ctx* ctx = static_cast<nvmpi_enc_ctx*>(h);
	int ret = ctx->enc->forceIDR();
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not force IDR frame");
		return -1;
	}
	return 0;
}

//Public API: change the encoder target bitrate mid-stream.
//Calls NvVideoEncoder::setBitrate() which updates V4L2_CID_MPEG_VIDEO_BITRATE.
//The new bitrate takes effect from the next encoded frame. Also updates
//ctx->bitrate so the internal state stays consistent. For VBR mode the caller
//should also adjust peak_bitrate externally if needed.
//Returns 0 on success, -1 on ioctl failure.
int nvmpi_encoder_set_bitrate(nvmpictx* h, unsigned int bitrate)
{
	nvmpi_enc_ctx* ctx = static_cast<nvmpi_enc_ctx*>(h);
	int ret = ctx->enc->setBitrate(bitrate);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not set bitrate to %u", bitrate);
		return -1;
	}
	/* Keep internal state in sync; freed in nvmpi_encoder_close(). */
	ctx->bitrate = bitrate;
	return 0;
}

//Public API: stop the encoder and free the context.
//Mid-stream flush: reset the V4L2 encoder pipeline so encoding can continue
//after a seek or stream restart without closing/reopening the codec context.
//
//Sequence (mirrors the decoder flush in nvmpi_dec_api.cpp):
//  1. Stop the DQ thread — prevents callbacks during STREAMOFF.
//  2. STREAMOFF both planes — tears down V4L2 queues, returns all buffers.
//  3. Drain the packet pool — discard any pending encoded packets by moving
//     them from the filled queue back to the empty queue.
//  4. Reset flushing/EOS atomics — re-enable put_frame/get_packet.
//  5. STREAMON both planes — restart the V4L2 pipeline.
//  6. Re-queue capture plane buffers — the encoder needs empty buffers to
//     write encoded output into.
//  7. Restart the DQ thread — resume capture-plane processing.
//  8. Reset the output-plane buffer index — next put_frame starts from 0.
//
//The caller (FFmpeg wrapper) is responsible for resetting its own
//encoder_flushing flag and forcing the next frame to IDR if needed.
int nvmpi_encoder_flush(nvmpictx* h)
{
	nvmpi_enc_ctx* ctx = static_cast<nvmpi_enc_ctx*>(h);
	int ret;

	/* 1. Stop DQ thread — must complete before STREAMOFF to avoid
	 *    callbacks on torn-down queues. DQ thread runs in both blocking
	 *    and non-blocking modes (always started since non-blocking support). */
	if (ctx->dq_thread_started) {
		ctx->enc->capture_plane.stopDQThread();
		ctx->enc->capture_plane.waitForDQThread(1000);
		ctx->dq_thread_started = false;
	}

	/* 2. STREAMOFF both planes — returns all queued buffers. */
	ctx->enc->capture_plane.setStreamStatus(false);
	ctx->enc->output_plane.setStreamStatus(false);

	/* 3. Drain packet pool: move filled packets back to empty queue.
	 *    The pool must be reset() first to clear any shutdown state
	 *    from a prior EOS drain, then drained. */
	ctx->pktPool->reset();
	{
		nvPacket* pkt;
		while ((pkt = ctx->pktPool->dqFilledBuf()))
			ctx->pktPool->qEmptyBuf(pkt);
	}

	/* 4. Reset EOS/flushing atomics so put_frame/get_packet work again.
	 *    STREAMOFF discarded every frame in flight, so no AU is owed. The
	 *    DQ thread is stopped: nothing decrements concurrently. */
	ctx->aus_pending.store(0, std::memory_order_seq_cst);
	ctx->flushing.store(false, std::memory_order_release);
	ctx->capPlaneGotEOS.store(false, std::memory_order_release);

	/* 5. STREAMON both planes — restart the V4L2 pipeline. */
	ret = ctx->enc->output_plane.setStreamStatus(true);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error in output plane streamon after flush");
		return ret;
	}

	ret = ctx->enc->capture_plane.setStreamStatus(true);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error in capture plane streamon after flush");
		return ret;
	}

	/* 6. Re-queue all capture plane buffers so the encoder has
	 *    somewhere to write encoded output. */
	for (uint32_t i = 0; i < ctx->enc->capture_plane.getNumBuffers(); i++) {
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];
		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;

		ret = ctx->enc->capture_plane.qBuffer(v4l2_buf, NULL);
		if (ret < 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR,
				  "Error while queueing buffer at capture plane after flush (buf %u)", i);
			return ret;
		}
	}

	/* 7. Restart DQ thread — resume capture-plane processing.
	 *    DQ thread runs in both blocking and non-blocking modes. */
	ctx->enc->capture_plane.setDQThreadCallback(encoder_capture_plane_dq_callback);
	ret = ctx->enc->capture_plane.startDQThread(ctx);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Could not restart DQ thread after flush");
		return ret;
	}
	ctx->dq_thread_started = true;

	/* 8. Reset output-plane buffer index — next put_frame starts
	 *    from buffer 0 (first N frames use getNthBuffer directly). */
	ctx->index = 0;

	return 0;
}

//Stops and joins the capture-plane DQ thread (no-op if never started).
//Why EOS first: stopDQThread() only sets a flag, and on Tegra neither it nor
//STREAMOFF unblocks a dqBuffer()/poll() that is waiting for encoded data
//(CI evidence: job 192719 still timed out the join). With no frame ever
//queued the thread blocks forever. The only reliable wake-up is what NVIDIA's
//01_video_encode sample does: queue an OUTPUT buffer with bytesused == 0; the
//encoder then returns a zero-size CAPTURE buffer, the DQ callback sets
//capPlaneGotEOS and returns false, and the thread exits by itself.
//Returns 0 when the thread is gone, -1 if the join timed out (thread may
//still touch ctx->enc: caller must NOT free it).
int nvmpi_enc_stop_dq_thread(nvmpi_enc_ctx* ctx)
{
	if (!ctx->enc || !ctx->dq_thread_started)
		return 0;

	/* 1. Send EOS unless the caller already did (flushing set by
	 *    put_frame(NULL)/put_frame_fd(-1)) or the thread already saw it.
	 *    Only when an OUTPUT slot is still unused (ctx->index < numBuffers):
	 *    the put_frame path then takes the slot directly and cannot block in
	 *    dqBuffer(-1) on a stuck encoder. If every slot was used, frames are
	 *    in flight and the thread is not idle-blocked. */
	if (!ctx->flushing.load(std::memory_order_acquire) &&
	    !ctx->capPlaneGotEOS.load(std::memory_order_acquire) &&
	    ctx->index < ctx->enc->output_plane.getNumBuffers()) {
		int eos_ret = ctx->dmabuf_external ?
			nvmpi_encoder_put_frame_fd(ctx, -1, 0, 0, 0, 0) :
			nvmpi_encoder_put_frame(ctx, NULL);
		if (eos_ret < 0)
			NVMPI_LOG(NVMPI_LOG_WARN, "could not queue EOS at close (code=%d)", eos_ret);
	}

	/* 2. Bounded wait (<= 1s) for the DQ callback to see the EOS buffer.
	 *    Timeout is not fatal: the join below is the authority. */
	for (int i = 0; i < 100 && ctx->flushing.load(std::memory_order_acquire) &&
	     !ctx->capPlaneGotEOS.load(std::memory_order_acquire); i++)
		usleep(10000);

	/* 3. Ask the thread to stop (flag only), 4. STREAMOFF CAPTURE as a
	 *    best effort wake-up, 5. join: 0 = joined, non-zero = timed out. */
	ctx->enc->capture_plane.stopDQThread();
	ctx->enc->capture_plane.setStreamStatus(false);
	if (ctx->enc->capture_plane.waitForDQThread(1000) != 0)
		return -1;

	ctx->dq_thread_started = false;
	return 0;
}

//Stops/joins the capture DQ thread, releases self-allocated OUTPUT-plane
//dmabufs (DMA mode only), then destroys the device and the (by now empty)
//packet pool. Packets still inside the pool are NOT freed here — the
//FFmpeg wrapper drains and frees them first (nvmpienc_deinitPktPool):
//dqEmptyPacket until -1 (it waits out a packet the DQ thread is still
//filling), THEN get_packet until empty. That order matters: the in-flight
//packet lands in the filled queue, so the filled drain comes second. The
//caller must not qEmptyPacket during this drain.
int nvmpi_encoder_close(nvmpictx* h)
{
	nvmpi_enc_ctx* ctx = static_cast<nvmpi_enc_ctx*>(h);
	if (!ctx)
		return -1;

	/* Shutdown the packet pool to unblock any thread waiting in
	 * dqFilledBuf(timeout) — prevents hangs on early termination
	 * (e.g. ffmpeg -t) when blocking wait is active. */
	if (ctx->pktPool)
		ctx->pktPool->shutdown();

	/* Thread-before-resource rule: the capture DQ thread must be gone
	 * before any plane, fd or the encoder is touched. If the join times
	 * out the thread is still running against ctx->enc; destroying it
	 * would be a use-after-free (SIGSEGV), so leak ctx and report the
	 * failure instead. */
	if (nvmpi_enc_stop_dq_thread(ctx) < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR,
			  "nvmpi_encoder_close: DQ thread did not exit; leaking encoder "
			  "context to avoid use-after-free");
		return -1;
	}

	/* STREAMOFF the OUTPUT plane too (CAPTURE was stopped above). Both
	 * are idempotent no-ops when already off. */
	if (ctx->enc)
		ctx->enc->output_plane.setStreamStatus(false);

	/* Cleanup internal DMABUF surfaces. Applies to both compile-time
	 * DMA mode (OUTPLANE_MEMTYPE_DMA) and runtime external DMABUF mode
	 * (dmabuf_external). Check fd validity (-1 = unallocated slot). */
	if (ctx->enc && ctx->output_plane_fd) {
		for (uint32_t i = 0; i < ctx->enc->output_plane.getNumBuffers(); i++) {
			if (ctx->output_plane_fd[i] >= 0) {
				ctx->enc->output_plane.unmapOutputBuffers(i,
					ctx->output_plane_fd[i]);
				NvBufferDestroy(ctx->output_plane_fd[i]);
				ctx->output_plane_fd[i] = -1;
			}
		}
	}
	delete[] ctx->output_plane_fd;
	ctx->output_plane_fd = nullptr;

	/* unique_ptr: release the NvVideoEncoder after the DQ thread is
	 * joined and all V4L2 buffers are unmapped. */
	ctx->enc.reset();

	/* Safety-net drain: discard any packets still in the pool queues.
	 * The normal path is the FFmpeg wrapper calling nvmpienc_deinitPktPool()
	 * BEFORE this function, which drains and frees every nvPacket properly
	 * (the DQ thread is already joined here, so nothing is in flight).
	 * If the caller forgot to drain, these packets leak (libnvmpi cannot
	 * call av_packet_free — no libavcodec linkage). The drain empties the
	 * queues so the NVMPI_bufPool destructor does not warn, and logs the
	 * count so the leak is diagnosable. */
	if (ctx->pktPool) {
		int leaked = 0;
		nvPacket* discard;
		while ((discard = ctx->pktPool->dqEmptyBuf()) != NULL)
			leaked++;
		while ((discard = ctx->pktPool->dqFilledBuf()) != NULL)
			leaked++;
		if (leaked > 0)
			NVMPI_LOG(NVMPI_LOG_WARN,
				  "nvmpi_encoder_close: discarded %d undrained packets "
				  "(caller should drain before close)", leaked);
	}
	delete ctx->pktPool;
	delete ctx;
	return 0;
}
