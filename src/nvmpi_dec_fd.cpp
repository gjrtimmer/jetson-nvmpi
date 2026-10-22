/*
 * nvmpi_dec_fd.cpp — decoder zero-copy output: borrow and release (libnvmpi).
 *
 * Owns:
 *   - dec_gen_release()           — give a slot back (requeue or drop).
 *   - nvmpi_decoder_get_frame_fd()  — borrow the next frame as a DMA-BUF fd.
 *   - nvmpi_decoder_get_frame_fd2() — same borrow, with per-plane geometry.
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

//Take the next filled slot and turn it into a borrow: the shared front half
//of get_frame_fd and get_frame_fd2. On success *out_slot / *out_b describe
//the borrow (generation reference moved slot -> cookie); on failure nothing
//is held and -1 is returned (no frame, timeout or shutdown).
static int dec_take_slot(nvmpi_dec_ctx *ctx, bool wait,
	dec_slot **out_slot, dec_fd_borrow **out_b)
{
	/* A frame get_frame2 kept pending comes first, else dequeue a filled
	 * one — same path as get_frame. The slot is ours now, together with the
	 * generation reference in slot->self. */
	dec_slot* slot = dec_next_slot(ctx, wait);

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
	*out_slot = slot;
	*out_b = b;
	return 0;
}

//Public API: zero-copy variant of nvmpi_decoder_get_frame.
//Returns the next decoded frame as a borrowed DMA-BUF fd instead of
//copying pixel data. See include/nvmpi.h for the contract; the fd stays
//valid until the release callback runs, even after nvmpi_decoder_close.
//width/height/pitch describe the slot's own generation, so a frame decoded
//before a resolution change keeps its original geometry.
int nvmpi_decoder_get_frame_fd(nvmpictx* h,
	int *dmabuf_fd, int *width, int *height, int *pitch,
	int64_t *timestamp,
	nvmpi_frame_release_cb *release, void **opaque, bool wait)
{
	dec_slot* slot;
	dec_fd_borrow *b;
	if (dec_take_slot(static_cast<nvmpi_dec_ctx*>(h), wait, &slot, &b) != 0)
		return -1;

	/* linesize[0] is the Y plane pitch; for NV12 (2-plane) this is also
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

//Public API: get_frame_fd with full per-plane geometry (nvmpi_fd_frame).
//Validation first, so a bad call never consumes a frame. Fills
//min(struct_size, sizeof) bytes like the JPEG API and writes the filled size
//back. Same borrow/release as v1 (dec_take_slot, dec_fd_release).
int nvmpi_decoder_get_frame_fd2(nvmpictx* h, nvmpi_fd_frame *frame,
	nvmpi_frame_release_cb *release, void **opaque, bool wait)
{
	if (!h || !frame || !release || !opaque ||
	    frame->struct_size < NVMPI_FD_FRAME_V1_SIZE)
		return NVMPI_ERR_INVAL;

	dec_slot* slot;
	dec_fd_borrow *b;
	if (dec_take_slot(static_cast<nvmpi_dec_ctx*>(h), wait, &slot, &b) != 0)
		return -1;

	const dec_gen *g = b->gen.get();
	if (!g->fd_geom_ok) {
		/* Geometry could not be read for this generation (logged at pool
		 * init): hand the frame back rather than expose a guessed layout. */
		dec_fd_release(b);
		return -1;
	}

	/* Cached layout + the two per-frame fields (fd, timestamp). */
	nvmpi_fd_frame fr = g->fd_geom;
	fr.fd = slot->fb->dst_dma_fd;
	fr.timestamp = (int64_t)slot->fb->timestamp;
	size_t n = frame->struct_size < sizeof(fr) ? frame->struct_size : sizeof(fr);
	fr.struct_size = (uint32_t)n;
	memcpy(frame, &fr, n);

	*release = dec_fd_release;
	*opaque = b;
	return 0;
}
