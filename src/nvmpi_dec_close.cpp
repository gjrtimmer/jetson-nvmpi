/*
 * nvmpi_dec_close.cpp — decoder flush and close (libnvmpi).
 *
 * Moved out of nvmpi_dec_api.cpp (issue #105) to keep that file under the
 * size limit. Owns:
 *   - nvmpi_decoder_flush() — reset the pipeline for seek/restart.
 *   - nvmpi_decoder_close() — full teardown.
 */
#include "nvmpi_dec_internal.h"
#include "nvmpi_timing.h"

//Public API: shut the decoder down and free everything.
//
//Reset the decoder pipeline without destroying it (seek / stream restart).
//
//Sequence:
//  1. Signal EOS and STREAMOFF capture — stops the capture thread.
//  2. Join the capture thread.
//  3. STREAMOFF output plane — aborts any in-flight V4L2 buffers.
//  4. Move filled frames back to the empty queue (generation unchanged).
//  5. Reset EOS flags (eos, input_eos) and output-plane buffer index.
//  6. STREAMON output plane.
//  7. If the CAPTURE plane was configured: re-queue all its buffers and
//     STREAMON it (#162). A stateful decoder only raises a resolution event
//     when the format changes, so a resume at the same size never gets one.
//  8. Restart the capture thread. With the plane re-armed it goes straight
//     to the capture loop (a later real resolution change arrives there as
//     an event); without one (no event seen yet) it waits for the first
//     resolution-change event as at create.
//
//The caller should re-prime extradata (SPS / PPS) after this call: the
//decoder needs them to parse the stream again.
int nvmpi_decoder_flush(nvmpictx* h)
{
	nvmpi_dec_ctx* ctx = static_cast<nvmpi_dec_ctx*>(h);
	{
		/* eos and STREAMOFF under the re-queue mutex, see its declaration. */
		std::lock_guard<std::mutex> g(ctx->capture_requeue_mu);
		ctx->eos.store(true);
		ctx->dec->capture_plane.setStreamStatus(false);
	}
	if (ctx->dec_capture_loop.joinable())
		ctx->dec_capture_loop.join();
	/* The thread may have restarted the plane (resolution event handled
	 * while we stopped it): stop it again so every buffer is ours. */
	if (ctx->dec->capture_plane.getStreamStatus())
		ctx->dec->capture_plane.setStreamStatus(false);

	ctx->dec->output_plane.setStreamStatus(false);

	/* Filled-but-undelivered frames go back to the empty queue. The
	 * generation stays current (flush keeps the pool); each slot still owns
	 * its generation reference while queued, so nothing changes hands. */
	/* A frame get_frame2 kept pending is discarded with the rest (the capture
	 * thread is joined; single-consumer rule). Requeued while the generation
	 * is current, dropped otherwise. */
	dec_drop_pending(ctx);

	dec_slot* slot;
	while ((slot = ctx->framePool->dqFilledBuf()))
		ctx->framePool->qEmptyBuf(slot);

	ctx->eos.store(false);
	ctx->input_eos.store(false);        /* capture thread joined: no reader */
	/* AV1: the restarted stream must send its own sequence header; until it
	 * does, data is dropped again instead of poisoning the driver (#147). */
	ctx->av1_seq_seen = false;
	ctx->av1_drop_logged = false;
	/* Clear shutdown so the pool blocks again after flush/restart. */
	ctx->framePool->reset();
	ctx->index = 0;

	ctx->dec->output_plane.setStreamStatus(true);

	/* Thread joined: this thread owns the CAPTURE plane. A configured plane
	 * (buffers allocated) was STREAMOFF'd above, which returned every
	 * buffer to us: queue them again and start it, so frames flow after
	 * the resume without a resolution event. If re-arming fails, fall back
	 * to the event wait (the plane is rebuilt when an event arrives). */
	ctx->capture_armed = false;
	if (ctx->numberCaptureBuffers > 0)
	{
		if (ctx->dec->capture_plane.setStreamStatus(true) == 0 &&
		    ctx->requeueDecoderCapturePlane() == 0)
			ctx->capture_armed = true;
		else
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "flush: could not re-arm the capture plane");
			ctx->dec->capture_plane.setStreamStatus(false);
		}
	}

	ctx->dec_capture_loop = std::thread(dec_capture_loop_fcn, ctx);
	pthread_setname_np(ctx->dec_capture_loop.native_handle(), "dec_capture");

	return 0;
}

//Teardown order (each step depends on the previous):
//  1. Signal EOS and STREAMOFF capture plane — unblocks the capture thread.
//  2. Join the capture thread — guarantees no transforms are in flight.
//  3. Destroy the VIC transform session (legacy path only).
//  4. Stop the output (input-side) plane and release its V4L2 buffers.
//  5. Release CAPTURE-plane DMA buffers (decoder output side).
//  6. Retire the frame-pool generation (VIC destination buffers): safe
//     because all planes are stopped and no transforms can be pending.
//     Borrowed buffers (get_frame_fd) outlive close until released.
//  7. Free the pool container, decoder device, and context.
//
//The handle is invalid after this call.
int nvmpi_decoder_close(nvmpictx* h)
{
	nvmpi_dec_ctx* ctx = static_cast<nvmpi_dec_ctx*>(h);
	if (!ctx)
		return -1;

	NvmpiTimer tm("dec_close");        /* NVMPI_TIMING=1 only; no-op otherwise */
	ctx->eos.store(true);
	/* Unblock any get_frame() caller blocked in dqFilledBuf(timeout)
	 * before STREAMOFF — otherwise the consumer hangs until its timeout
	 * expires while we wait for the capture thread to join. */
	ctx->framePool->shutdown();
	ctx->dec->capture_plane.setStreamStatus(false);
	if (ctx->dec_capture_loop.joinable())
	{
		ctx->dec_capture_loop.join();
	}
	tm.phase("eos_to_join");           /* capture-thread exit latency (#105) */

#ifndef WITH_NVUTILS
	if (ctx->numberCaptureBuffers > 0)
		NvBufferSessionDestroy(ctx->session);
#endif

	ctx->dec->output_plane.setStreamStatus(false);
	ctx->dec->output_plane.deinitPlane();

	ctx->deinitDecoderCapturePlane();

	/* Retire the pool generation after the join (no capture thread holds a
	 * slot) and the plane teardown: unborrowed buffers are destroyed now;
	 * buffers still borrowed via get_frame_fd stay valid and are destroyed
	 * by the last release, possibly after this function returned. Never
	 * waits. Flipping `current` under gen->mu happens BEFORE the pool is
	 * deleted, so a concurrent release can never requeue into a dead pool. */
	/* A held (pending) frame is not in either queue, so retire would not see
	 * it: give it back first (pool still alive, queues drained below). */
	dec_drop_pending(ctx);
	ctx->retireFramePool();
	delete ctx->framePool;   //allocated in nvmpi_create_decoder; both queues drained by retire
	ctx->framePool = nullptr;

	delete ctx->dec;
	ctx->dec = nullptr;

	delete ctx;

	tm.total();                        /* ctx is gone; tm lives on the stack */
	return 0;
}
