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
//  4. Drain the frame pool (move filled frames back to the empty queue).
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

	nvmpi_frame_buffer* fb;
	while ((fb = ctx->framePool->dqFilledBuf()))
		ctx->framePool->qEmptyBuf(fb);

	ctx->eos.store(false);
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
//  6. Destroy frame pool (VIC destination buffers) — safe because all
//     planes are stopped and no transforms can be pending.
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

	/* Signal DRM_PRIME release callbacks that the pool is gone BEFORE
	 * destroying buffers — a callback firing between deinitFramePool()
	 * and the store(false) would pass the pool_alive guard and call
	 * qEmptyBuf on a destroyed buffer.  Must be BEFORE deinitFramePool
	 * AND before delete so late-arriving callbacks (from FFmpeg 8.1+
	 * scheduler cleanup order) skip the qEmptyBuf call entirely. */
	if (ctx->pool_alive)
		ctx->pool_alive->store(false);
	ctx->deinitFramePool();
	delete ctx->framePool;
	ctx->framePool = nullptr;

	delete ctx->dec;
	ctx->dec = nullptr;

	delete ctx;

	tm.total();                        /* ctx is gone; tm lives on the stack */
	return 0;
}
