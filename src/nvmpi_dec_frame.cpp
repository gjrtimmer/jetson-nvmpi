/*
 * nvmpi_dec_frame.cpp — decoder copy-out path: get_frame / get_frame2 (#149).
 *
 * Owns:
 *   - dec_next_slot() / dec_drop_pending() — the held-frame ("pending") slot
 *     shared by every consumer entry point.
 *   - nvmpi_decoder_get_frame()  — copy, hardened: refuses a frame that does
 *     not fit the caller's buffers.
 *   - nvmpi_decoder_get_frame2() — copy with an explicit caller capacity;
 *     reports the frame size and keeps a frame that did not fit.
 *
 * Threat model: the decoded size comes from the (untrusted) stream and may
 * grow mid-stream; the destination is caller memory of unknown size. The
 * fit check (nvmpi_frame_fit.h) runs BEFORE the first byte is copied.
 */
#include "nvmpi_dec_internal.h"
#include "nvmpi_frame_fit.h"

/* Held-frame rule (single consumer thread, see nvmpi.h): `pending` is only
 * touched by the thread that calls the get_frame* functions, and by flush /
 * close after that consumer is done. No lock is needed or taken. */

//Next frame: the held one first (so a refused frame is delivered next, in
//order, before anything newer), else the pool. The returned slot is owned by
//the caller, generation reference in slot->self.
dec_slot* dec_next_slot(nvmpi_dec_ctx* ctx, bool wait)
{
	if (ctx->pending)
	{
		dec_slot *s = ctx->pending;
		ctx->pending = nullptr;
		return s;
	}
	if (wait)
		return ctx->framePool->dqFilledBuf(
			std::chrono::milliseconds(ctx->wait_timeout_ms));
	return ctx->framePool->dqFilledBuf();
}

//Give the held frame back: requeue as empty while its generation is current,
//else drop the reference (maybe freeing the buffer). Single release point of
//a pending slot (flush, close); the slot's reference moves into the call.
void dec_drop_pending(nvmpi_dec_ctx* ctx)
{
	dec_slot *s = ctx->pending;
	ctx->pending = nullptr;
	if (s)
		dec_gen_release(std::move(s->self), s);
}

//View of a generation's cached geometry for the fit helpers. Read-only, the
//cache is immutable once a slot is queued.
static nvmpi_frame_geom dec_geom(const dec_gen *g)
{
	nvmpi_frame_geom gm;
	gm.width = g->w;
	gm.height = g->h;
	gm.num_planes = g->num_planes < MAX_NUM_PLANES ? g->num_planes : MAX_NUM_PLANES;
	gm.plane_height = g->height;
	gm.plane_linedata = g->linedatasize;
	return gm;
}

//Planes of the format the caller asked for at create time (the format is
//fixed for the life of the decoder, generations only change the size).
static unsigned int dec_format_planes(const nvmpi_dec_ctx *ctx)
{
	return ctx->out_pixfmt == NV_PIX_YUV420 ? 3 : 2;
}

//Legacy get_frame without payload_size: decide from an assumed size whether
//the caller's buffers can hold this generation. Assumed size, in order: the
//resize target, the container size from create, the first generation this
//path delivered (recorded here). Returns 0 or NVMPI_ERR_FRAME_SIZE.
static int dec_legacy_fit(nvmpi_dec_ctx *ctx, const nvFrame *frame,
	const dec_gen *g, const nvmpi_frame_geom *gm)
{
	/* Pointer + stride sanity: a NULL plane or a stride below one row
	 * would write outside whatever the caller allocated. */
	if (nvmpi_frame_check_ptrs(frame, gm) != 0)
		return NVMPI_ERR_FRAME_SIZE;

	unsigned int bw = ctx->resized.width ? ctx->resized.width : ctx->hint_width;
	unsigned int bh = ctx->resized.height ? ctx->resized.height : ctx->hint_height;
	if (!bw || !bh)
	{
		/* Neither promised: the first delivered size is what the caller
		 * sized its buffers for (consumer thread only, no lock). */
		if (!ctx->base_w || !ctx->base_h)
		{
			ctx->base_w = g->w;
			ctx->base_h = g->h;
		}
		bw = ctx->base_w;
		bh = ctx->base_h;
	}

	/* Bytes per sample of the create-time format (P010 = 2). A generation
	 * with wider samples than the buffers were sized for is refused even if
	 * the pixel size is not larger. */
	unsigned int bpp = ctx->out_pixfmt == NV_PIX_P010 ? 2 : 1;
	size_t row0;
	if (__builtin_mul_overflow((size_t)bpp, (size_t)g->w, &row0) ||
	    (gm->num_planes && g->linedatasize[0] > row0))
		return NVMPI_ERR_FRAME_SIZE;

	return (g->w <= bw && g->h <= bh) ? 0 : NVMPI_ERR_FRAME_SIZE;
}

//Public API: fetch the next decoded frame (see include/nvmpi.h).
//wait=false: non-blocking, -1 when none ready. wait=true: blocks up to
//wait_timeout_ms. On success the data is copied into the caller's planes,
//the timestamp is set and the slot goes back to the pool (or its buffer is
//freed if a resolution change retired the generation meanwhile). The slot's
//generation reference keeps buffer and geometry valid for the whole copy.
//A frame that does not fit is dropped (slot released, nothing written) and
//NVMPI_ERR_FRAME_SIZE returned: this legacy call has no way to hold it.
int nvmpi_decoder_get_frame(nvmpictx* h, nvFrame* frame, bool wait)
{
	nvmpi_dec_ctx* ctx = static_cast<nvmpi_dec_ctx*>(h);
	if (!ctx || !frame)
		return NVMPI_ERR_INVAL;

	dec_slot* slot = dec_next_slot(ctx, wait);
	if (!slot) return -1;

	const dec_gen *g = slot->self.get();
	const nvmpi_frame_geom gm = dec_geom(g);
	int ret;

	/* Declared capacity: exact check. Otherwise the assumed-size check. */
	if (nvmpi_frame_has_caps(frame, gm.num_planes))
		ret = nvmpi_frame_check_caps(frame, &gm);
	else
		ret = dec_legacy_fit(ctx, frame, g, &gm);

	if (ret == 0)
	{
		ret = copyNvBufToFrame(g, slot->fb, frame);
		/* Set even when the map failed (documented, unchanged since 4.x). */
		frame->timestamp = slot->fb->timestamp;
	}
	else
	{
		/* Log the first refusal and then every power of two, so a stream
		 * that keeps sending oversized frames cannot flood the log. */
		uint32_t n = ++ctx->size_drops;
		if ((n & (n - 1)) == 0)
			NVMPI_LOG(NVMPI_LOG_ERROR, "get_frame: %ux%u frame does not fit the "
			          "caller's buffers, dropped (%u so far); use "
			          "nvmpi_decoder_get_frame2", g->w, g->h, n);
	}
	/* Hand the reference back with the slot: requeue under gen->mu if the
	 * generation is still current, else drop (maybe freeing the buffer). */
	dec_gen_release(std::move(slot->self), slot);

	return ret;
}

//Public API: get_frame with the caller's capacity (payload_size[p]).
//Arguments are validated BEFORE a frame is taken, so a bad call consumes
//nothing. On NVMPI_ERR_FRAME_SIZE the slot is parked in ctx->pending (still
//owning its generation reference) and the next get_frame* call delivers it;
//the caller learns the size from frame->width/height/payload_size.
int nvmpi_decoder_get_frame2(nvmpictx* h, nvFrame* frame, bool wait)
{
	nvmpi_dec_ctx* ctx = static_cast<nvmpi_dec_ctx*>(h);
	if (!ctx || !frame)
		return NVMPI_ERR_INVAL;

	/* Every plane of the format needs a pointer and a capacity; a zero
	 * capacity would otherwise mean "unknown" and defeat the check. */
	unsigned int np = dec_format_planes(ctx);
	for (unsigned int p = 0; p < np; p++)
		if (!frame->payload[p] || frame->payload_size[p] == 0)
			return NVMPI_ERR_INVAL;

	dec_slot* slot = dec_next_slot(ctx, wait);
	if (!slot) return -1;

	const dec_gen *g = slot->self.get();
	const nvmpi_frame_geom gm = dec_geom(g);

	int ret = nvmpi_frame_check_caps(frame, &gm);
	if (ret != 0)
	{
		/* Nothing was copied. Report the size, keep the frame for the retry. */
		nvmpi_frame_report(frame, &gm, ctx->out_pixfmt);
		ctx->pending = slot;
		return NVMPI_ERR_FRAME_SIZE;
	}

	ret = copyNvBufToFrame(g, slot->fb, frame);
	if (ret == 0)
	{
		nvmpi_frame_report(frame, &gm, ctx->out_pixfmt);
		frame->timestamp = slot->fb->timestamp;
	}
	dec_gen_release(std::move(slot->self), slot);
	return ret;
}
