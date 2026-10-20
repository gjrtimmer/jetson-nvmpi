/*
 * nvmpi_dec_fd.cpp — decoder zero-copy output: borrow and release (libnvmpi).
 *
 * Owns:
 *   - dec_gen_release()           — give a slot back (requeue or drop).
 *   - nvmpi_decoder_get_frame_fd() — borrow the next frame as a DMA-BUF fd.
 *
 * Lifetime model (#137, same as the JPEG fd path): the borrow cookie owns one
 * reference to the slot's pool generation. close and resolution change only
 * retire the generation (nvmpi_dec_pool.cpp); a borrowed buffer is destroyed
 * by ~dec_gen when the LAST borrow is released, on whatever thread that is,
 * possibly long after nvmpi_decoder_close. close never waits for borrows and
 * there is no check-then-requeue race: the "still current?" decision and the
 * requeue happen together under dec_gen::mu.
 */
#include "nvmpi_dec_internal.h"
#include <new>

//Heap cookie of one borrow (the release `opaque`). Holds the generation
//reference that the slot owned while queued (ownership invariant, see
//dec_slot). Freed by dec_fd_release().
struct dec_fd_borrow {
	std::shared_ptr<dec_gen> gen;
	dec_slot *slot;
};

//Give a slot back. `ref` is the generation reference the caller held for the
//slot (slot->self moved out, or the borrow cookie's).
//Steps: move the ref into a local; lock gen->mu; if still current, hand the
//ref back to the slot (the queued state owns it) and requeue as empty; unlock;
//the local dies outside the lock — if the generation was retired it is
//possibly the last reference and ~dec_gen destroys the buffers here.
//Lock order: gen->mu -> pool m_emptyBuf (qEmptyBuf), never the reverse.
//Why `current` is checked under mu: retire flips it under the same mu before
//the context deletes the pool, so `pool` is valid whenever it is true.
void dec_gen_release(std::shared_ptr<dec_gen> &&ref, dec_slot *slot)
{
	std::shared_ptr<dec_gen> local = std::move(ref);
	if (!local || !slot)
		return;
	dec_gen *g = local.get();   //valid: `local` (or slot->self) keeps it alive past unlock
	{
		std::lock_guard<std::mutex> l(g->mu);
		if (g->current)
		{
			slot->self = std::move(local);   //queued slots own a ref again
			g->pool->qEmptyBuf(slot);
		}
	}
	//`local` (if still set) is dropped here, outside the lock
}

//Release callback for nvmpi_decoder_get_frame_fd. Any thread, before or
//after close, exactly once per borrow.
/* Allocated in: nvmpi_decoder_get_frame_fd (below) */
/* Freed in: this function (single free point) */
static void dec_fd_release(void *opaque)
{
	dec_fd_borrow *b = static_cast<dec_fd_borrow*>(opaque);
	if (!b) return;

	std::shared_ptr<dec_gen> g = std::move(b->gen);
	dec_slot *slot = b->slot;
	delete b;
	dec_gen_release(std::move(g), slot);
}

//Public API: zero-copy variant of nvmpi_decoder_get_frame.
//Returns the next decoded frame as a borrowed DMA-BUF fd instead of
//copying pixel data. The fd points to a pitch-linear NV12/YUV420
//buffer that was VIC-transformed from the decoder's block-linear
//capture plane — the same buffer that get_frame would memcpy from.
//
//The caller receives metadata (dimensions, pitch, timestamp) and a
//release callback+opaque pair. The fd is valid only until the release
//callback is invoked; if the caller needs it longer (e.g. to pass to
//an encoder), it must dup() the fd before calling back.
//
//Ownership: the frame buffer is borrowed from its pool generation. Invoking
//the release callback returns it to the empty queue (or, if the generation
//was retired by a resolution change or close, frees it). Failure to release
//starves the pool and stalls decoding. width/height/pitch describe the
//slot's own generation, so a frame decoded before a resolution change keeps
//its original geometry.
int nvmpi_decoder_get_frame_fd(nvmpictx* h,
	int *dmabuf_fd, int *width, int *height, int *pitch,
	int64_t *timestamp,
	nvmpi_frame_release_cb *release, void **opaque, bool wait)
{
	nvmpi_dec_ctx* ctx = static_cast<nvmpi_dec_ctx*>(h);
	dec_slot* slot;

	/* Dequeue a filled frame — same path as get_frame. The slot is ours
	 * now, together with the generation reference in slot->self. */
	if (wait)
		slot = ctx->framePool->dqFilledBuf(
			std::chrono::milliseconds(ctx->wait_timeout_ms));
	else
		slot = ctx->framePool->dqFilledBuf();

	if (!slot) return -1;

	/* Allocate the cookie first so a failure can still hand the slot back
	 * with its reference intact. Allocated here; freed in dec_fd_release() */
	dec_fd_borrow *b = new (std::nothrow) dec_fd_borrow();
	if (!b) {
		dec_gen_release(std::move(slot->self), slot);
		return -1;
	}
	b->gen = std::move(slot->self);   //reference moves slot -> cookie
	b->slot = slot;

	/* Expose the underlying DMA-BUF fd and the generation's geometry.
	 * linesize[0] is the Y plane pitch; for NV12 (2-plane) this is also
	 * the UV plane pitch (interleaved U/V at half height). */
	const dec_gen *g = b->gen.get();
	*dmabuf_fd = slot->fb->dst_dma_fd;
	*width = (int)g->w;
	*height = (int)g->h;
	*pitch = (int)g->linesize[0];
	*timestamp = (int64_t)slot->fb->timestamp;

	*release = dec_fd_release;
	*opaque = b;

	return 0;
}
