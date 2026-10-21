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
//  5. Reset EOS flag and output-plane buffer index.
//  6. STREAMON output plane.
//  7. Restart the capture thread — it will wait for the next
//     resolution-change event (triggered when the caller re-primes
//     extradata / SPS / PPS), then reinitialize the capture plane
//     and frame pool via respondToResolutionEvent().
//
//The caller MUST re-prime extradata after this call so the hardware
//decoder can reconfigure its capture plane (same path as initial setup).
int nvmpi_decoder_flush(nvmpictx* h)
{
	nvmpi_dec_ctx* ctx = static_cast<nvmpi_dec_ctx*>(h);
	ctx->eos.store(true);
	ctx->dec->capture_plane.setStreamStatus(false);
	if (ctx->dec_capture_loop.joinable())
		ctx->dec_capture_loop.join();

	ctx->dec->output_plane.setStreamStatus(false);

	/* Filled-but-undelivered frames go back to the empty queue. The
	 * generation stays current (flush keeps the pool); each slot still owns
	 * its generation reference while queued, so nothing changes hands. */
	dec_slot* slot;
	while ((slot = ctx->framePool->dqFilledBuf()))
		ctx->framePool->qEmptyBuf(slot);

	ctx->eos.store(false);
	/* AV1: the restarted stream must send its own sequence header; until it
	 * does, data is dropped again instead of poisoning the driver (#147). */
	ctx->av1_seq_seen = false;
	ctx->av1_drop_logged = false;
	/* Clear shutdown so the pool blocks again after flush/restart. */
	ctx->framePool->reset();
	ctx->index = 0;

	ctx->dec->output_plane.setStreamStatus(true);

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
	ctx->retireFramePool();
	delete ctx->framePool;   //allocated in nvmpi_create_decoder; both queues drained by retire
	ctx->framePool = nullptr;

	delete ctx->dec;
	ctx->dec = nullptr;

	delete ctx;

	tm.total();                        /* ctx is gone; tm lives on the stack */
	return 0;
}
