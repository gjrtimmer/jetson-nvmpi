/*
 * nvmpi_dec_pool.cpp — decoder destination frame pool (libnvmpi).
 *
 * The pool is a refcounted "generation" (dec_gen, see nvmpi_dec_internal.h):
 *   - initFramePool()   — allocate a generation of pitch-linear destination
 *                         DMA buffers (VIC output) and queue it as empty.
 *   - retireFramePool() — retire the current generation (resolution change,
 *                         close): unborrowed buffers die now, borrowed ones
 *                         when their last borrow is released.
 *   - ~dec_gen()        — the single place pool surfaces are destroyed.
 *   - cacheGeometry()   — plane pitch/height cache of one generation
 *                         (moved here from nvmpi_dec_init.cpp, #137).
 */
#include "nvmpi_dec_internal.h"

//Last reference gone: no queued slot, no borrow, no context points at this
//generation any more, so nobody can touch the buffers. Runs on whichever
//thread dropped the last reference (capture thread, close caller, or a
//foreign release-callback thread) and never under dec_gen::mu.
//Single deallocation point for every frame buffer of a generation that
//still owns one (retire already destroyed the unborrowed ones).
dec_gen::~dec_gen()
{
	for (auto &s : slots)
	{
		if (s.fb)
		{
			s.fb->destroy();   //NvBufSurf::NvDestroy, fd = -1 (idempotent)
			delete s.fb;       //allocated in initFramePool
			s.fb = nullptr;
		}
	}
	slots.clear();
}

//Query the actual plane geometry (count, pitch, height, bytes-per-line) of
//one freshly allocated dst_dma frame buffer and cache it in the generation.
//These values drive the memcpy in copyNvBufToFrame(); the allocator may
//choose pitches different from width, so they must be read back, not
//assumed. Called before any slot of `g` is queued, so no locking is needed;
//afterwards the cache is read-only.
static void cacheGeometry(dec_gen &g)
{
	/* initFramePool() may have allocated no buffer at all (allocator
	 * exhausted); there is no geometry to cache then and num_planes stays 0.
	 * The next resolution change rebuilds the pool. */
	if (g.slots.empty()) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "frame pool is empty (allocation failed)");
		g.num_planes = 0;
		return;
	}
	nvmpi_frame_buffer *fb = g.slots[0].fb;
#ifdef WITH_NVUTILS
	NvBufSurfacePlaneParams parm;
	NvBufSurfaceParams dst_dma_surface_params;
	dst_dma_surface_params = fb->dst_dma_surface->surfaceList[0];
	parm = dst_dma_surface_params.planeParams;
#else
	NvBufferParams parm;
	int ret = NvBufferGetParams(fb->dst_dma_fd, &parm);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Failed to get dst dma buf params (code=%d)", ret);
		g.num_planes = 0;
		return;
	}
#endif

	g.num_planes = parm.num_planes;
	for(unsigned int i=0; i<g.num_planes; i++)
	{
		g.linesize[i] = parm.pitch[i];
		g.height[i] = parm.height[i];
#ifdef WITH_NVUTILS
		g.linedatasize[i] = parm.width[i] * parm.bytesPerPix[i]; //valid only for nvutils
#else
		//legacy API lacks bytesPerPix: the interleaved UV plane of NV12
		//variants carries 2 bytes per pixel, everything else 1.
		if(i == 1 && (parm.pixel_format == NvBufferColorFormat_NV12 ||
				parm.pixel_format == NvBufferColorFormat_NV16 ||
				parm.pixel_format == NvBufferColorFormat_NV24 ||
				parm.pixel_format == NvBufferColorFormat_NV12_ER ||
				parm.pixel_format == NvBufferColorFormat_NV12_709 ||
				parm.pixel_format == NvBufferColorFormat_NV12_709_ER ||
				parm.pixel_format == NvBufferColorFormat_NV12_2020))
		{
			g.linedatasize[i] = parm.width[i] * 2;
		}
		else
		{
			g.linedatasize[i] = parm.width[i];
		}
#endif
	}
}

//Retire the current generation. Callers: the capture thread on a resolution
//change, and close after the capture thread was joined and the planes were
//deinitialised — so no thread of ours holds a slot taken from the empty
//queue. A user thread may hold a slot it dequeued from the filled queue or
//borrowed; those are not in the queues, are not touched here, and are freed
//by their release (current == false => no requeue => ~dec_gen).
//
//Under gen->mu: current = false, then both queues are drained (lock order
//gen->mu -> queue lock, see dec_gen). Every drained slot has its buffer
//destroyed immediately and its self reference moved into `dropped`, which
//breaks the gen <-> slot cycle. The references are released AFTER the lock is
//dropped (ctx->gen still holds one here, so they are never the last, but the
//rule "never free under gen->mu" stays uniform). Queued frames of the old
//size are discarded, as before. No waiting.
void nvmpi_dec_ctx::retireFramePool()
{
	if (!gen)
		return;

	std::vector<std::shared_ptr<dec_gen>> dropped;
	{
		std::lock_guard<std::mutex> l(gen->mu);
		gen->current = false;   //from now on releases drop instead of requeue

		for (int q = 0; q < 2; q++)
		{
			dec_slot *s;
			while ((s = q ? framePool->dqFilledBuf() : framePool->dqEmptyBuf()))
			{
				if (s->fb)
				{
					s->fb->destroy();   //unborrowed: nobody else can reference it
					delete s->fb;       //allocated in initFramePool; single free for this slot
					s->fb = nullptr;
				}
				dropped.push_back(std::move(s->self));
			}
		}
	}
	gen.reset();   //context ref; maybe the last if nothing is borrowed
	//`dropped` dies here, outside the lock
}

//Allocate a generation of frame_pool_size pitch-linear DMA buffers at the
//output resolution and user-requested pixel format, cache its geometry and
//seed the pool's "empty" queue with its slots. These are the destination
//buffers of the VIC transform — distinct from the block-linear
//CAPTURE-plane buffers the decoder writes into. The previous generation, if
//any, must be retired by the caller first (done defensively here).
void nvmpi_dec_ctx::initFramePool()
{
	/* Validate pool size — zero or negative would allocate nothing, causing
	 * downstream dqEmptyBuf() to block forever or return null. */
	if (frame_pool_size <= 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "initFramePool: frame_pool_size=%d is invalid, skipping allocation", frame_pool_size);
		return;
	}

	/* A live generation here means the caller skipped retireFramePool();
	 * retire it so its DMA fds cannot leak. Safe on an empty context. */
	if (gen) {
		NVMPI_LOG(NVMPI_LOG_WARN, "initFramePool: generation still live (%zu buffers), retiring first to prevent DMA fd leak", gen->slots.size());
		retireFramePool();
	}

	//VIC destination format: NV12 or planar YUV420 (8-bit), or NV12_10LE
	//(P010, 10-bit). The 10-bit branch is NvUtils-only — out_pixfmt can
	//never be NV_PIX_P010 on legacy builds (rejected at decoder init), but
	//the constant must still be compiled out there.
	NvBufferColorFormat cFmt = out_pixfmt==NV_PIX_NV12?NvBufferColorFormat_NV12: NvBufferColorFormat_YUV420;
#ifdef WITH_NVUTILS
	if(out_pixfmt==NV_PIX_P010) cFmt = NvBufferColorFormat_NV12_10LE;
#endif

	NvBufferCreateParams input_params;
	memset(&input_params, 0, sizeof(input_params));
	/* Create PitchLinear output buffer for transform. */
	input_params.width = output_width;
	input_params.height = output_height;
	input_params.layout = NvBufferLayout_Pitch;
	input_params.colorFormat = cFmt;
#ifdef WITH_NVUTILS
	input_params.memType = NVBUF_MEM_SURFACE_ARRAY;
	input_params.memtag = NvBufSurfaceTag_VIDEO_CONVERT;
#else
	input_params.payloadType = NvBufferPayload_SurfArray;
	input_params.nvbuf_tag = NvBufferTag_VIDEO_DEC;
#endif

	/* Allocated here; owned by the generation. Freed by retireFramePool()
	 * (unborrowed) or ~dec_gen() (the rest), via the slots' fb pointers. */
	std::shared_ptr<dec_gen> g = std::make_shared<dec_gen>();
	g->pool = framePool;
	g->w = output_width;
	g->h = output_height;
	//reserve so &slots[i] stays valid: the slots are queued by address
	g->slots.reserve((size_t)frame_pool_size);

	for(int i=0;i<frame_pool_size;i++)
	{
		nvmpi_frame_buffer* fb = new nvmpi_frame_buffer();
		if(!fb->alloc(input_params)) { delete fb; break; }
		g->slots.emplace_back();
		g->slots.back().fb = fb;
		g->slots.back().idx = i;
	}

	cacheGeometry(*g);
	//fd2 layout: all slots share one allocation recipe, so slot 0 stands
	//for the generation. Output size is the crop/resize target, not the
	//padded plane size. flags stay 0: VIC destination surfaces are limited
	//range (no full-range request is made on the transform).
	if (!g->slots.empty() &&
	    nvmpi_fd_fill_geometry(out_pixfmt, g->slots[0].fb, &g->fd_geom))
	{
		g->fd_geom.width = g->w;
		g->fd_geom.height = g->h;
		g->fd_geom_ok = true;
	}

	//Publish: from here the generation is live. Each queued slot carries a
	//reference (`self`); retire or a release hands it back.
	gen = g;
	for (auto &s : g->slots)
	{
		s.self = g;
		framePool->qEmptyBuf(&s);
	}
}
