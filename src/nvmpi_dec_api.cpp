/*
 * nvmpi_dec_api.cpp — decoder runtime API (libnvmpi).
 *
 * Implements the packet-submission, frame-retrieval, and control surface
 * (flush, close) of the public C API in include/nvmpi.h on top of NVIDIA's
 * V4L2 NvVideoDecoder sample class.
 *
 * This file owns:
 *   - nvmpi_decoder_put_packet()   — feed one compressed packet.
 *   - copyNvBufToFrame()           — copy a decoded DMA buffer into the
 *                                    caller's nvFrame planes.
 *   - nvmpi_decoder_get_frame()    — pop + copy the next decoded frame.
 *   - nvmpi_frame_ref / frame_fd_release_impl() — release callback plumbing
 *                                    for the zero-copy fd path.
 *   - nvmpi_decoder_get_frame_fd() — zero-copy variant (borrowed DMA fd).
 *
 * Companion files:
 *   nvmpi_dec_internal.h  — shared includes, defines, nvmpi_dec_ctx struct,
 *                           and forward declarations.
 *   nvmpi_dec_init.cpp    — nvmpi_create_decoder() and the frame-pool /
 *                           VIC-transform-parameter setup it depends on.
 *   nvmpi_dec_capture.cpp — capture-thread loop and resolution-change
 *                           handling.
 *   nvmpi_dec_planes.cpp  — CAPTURE-plane allocation/teardown helpers.
 *   nvmpi_dec_close.cpp   — nvmpi_decoder_flush() and nvmpi_decoder_close().
 */
#include "nvmpi_dec_internal.h"

//Public API: feed one compressed packet to the decoder OUTPUT plane.
//Buffer acquisition strategy: first use each of the plane's buffers once
//(by index), afterwards block in dqBuffer(-1 = infinite) until the decoder
//releases one. The packet payload is memcpy'd, so the caller keeps
//ownership of it. pts (microseconds) is carried in the V4L2 timestamp.
//A zero-sized payload is the EOS marker and flips ctx->eos.
//Returns 0 on success, -1 on dequeue failure, -2 on queue failure,
//-3 when the packet exceeds chunk_size (packet dropped, decoder usable).
int nvmpi_decoder_put_packet(nvmpictx* h,nvPacket* packet)
{
	nvmpi_dec_ctx* ctx = static_cast<nvmpi_dec_ctx*>(h);
	int ret;
	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[MAX_PLANES];
	NvBuffer *nvBuffer;

	//reject packets larger than the V4L2 input buffers before dequeuing
	//anything — copying would overflow the plane buffer. Distinct return
	//code: callers must treat this as invalid input data, not as a
	//transient/hardware failure (the FFmpeg wrapper maps it accordingly).
	if (packet->payload_size > ctx->chunk_size)
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "input packet (%zu bytes) exceeds chunk_size (%u); dropping. Increase the chunk_size option.", packet->payload_size, ctx->chunk_size);
		return -3;
	}

	memset(&v4l2_buf, 0, sizeof(v4l2_buf));
	memset(planes, 0, sizeof(planes));

	v4l2_buf.m.planes = planes;

	if (ctx->index < (int)ctx->dec->output_plane.getNumBuffers())
	{
		nvBuffer = ctx->dec->output_plane.getNthBuffer(ctx->index);
		v4l2_buf.index = ctx->index;
		ctx->index++;
	}
	else
	{
		ret = ctx->dec->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, -1);
		if (ret < 0)
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error DQing buffer at output plane");
			return -1;
		}
	}

	memcpy(nvBuffer->planes[0].data,packet->payload,packet->payload_size);
	nvBuffer->planes[0].bytesused=packet->payload_size;
	v4l2_buf.m.planes[0].bytesused = nvBuffer->planes[0].bytesused;

	v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
	v4l2_buf.timestamp.tv_sec = packet->pts / 1000000;
	v4l2_buf.timestamp.tv_usec = packet->pts % 1000000;

	ret = ctx->dec->output_plane.qBuffer(v4l2_buf, NULL);
	if (ret < 0)
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error Qing buffer at output plane");
		ctx->index--;
		return -2;
	}

	/* Count BEFORE the EOS store below: the capture thread's race guard
	 * (nvmpi_dec_capture.cpp) reads this after seeing eos. */
	ctx->packets_put.fetch_add(1, std::memory_order_release);

	if (v4l2_buf.m.planes[0].bytesused == 0)
	{
		ctx->eos.store(true);
		NVMPI_LOG(NVMPI_LOG_INFO, "input EOS detected");
	}

	return 0;
}

//Copy one filled DMA frame buffer into the caller's nvFrame planes.
//Per plane: map the dmabuf for CPU access, sync caches (device -> CPU),
//then copy line by line because the source pitch (hw-chosen) and the
//destination linesize (caller-chosen) usually differ; only
//frame_linedatasize valid bytes per line are copied. Unmaps when done.
//frame->payload[] must point at sufficiently large caller-owned memory.
int copyNvBufToFrame(nvmpi_dec_ctx* ctx, nvmpi_frame_buffer *nvmpiBuf, nvFrame* frame)
{
	int ret;
	char *dataDst;
	char *dataSrc;

	for(unsigned int plane=0; plane<ctx->num_planes; plane++)
	{
#ifdef WITH_NVUTILS
		NvBufSurface *nvbuf_surf = nvmpiBuf->dst_dma_surface;
		ret = NvBufSurfaceMap(nvbuf_surf, 0, plane, NVBUF_MAP_READ_WRITE);
		NvBufSurfaceSyncForCpu (nvbuf_surf, 0, plane);
		dataSrc = (char *)nvbuf_surf->surfaceList[0].mappedAddr.addr[plane];
#else
		int dmabuf_fd = nvmpiBuf->dst_dma_fd;
		void *psrc_data;
		ret = NvBufferMemMap(dmabuf_fd, plane, NvBufferMem_Read_Write, &psrc_data);
		NvBufferMemSyncForCpu(dmabuf_fd, plane, &psrc_data);
		dataSrc = (char *)psrc_data;
#endif
		if(ret != 0)
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "NvBufferMap failed");
			return ret;
		}

		dataDst = (char *)frame->payload[plane];
		unsigned int &dstFrameLineSize = frame->linesize[plane];
		unsigned int &srcFrameLineSize = ctx->frame_linesize[plane];
		unsigned int &copySz = ctx->frame_linedatasize[plane];

		for (unsigned int i = 0; i < ctx->frame_height[plane]; i++)
		{
			memcpy(dataDst, dataSrc, copySz);
			dataDst += dstFrameLineSize;
			dataSrc += srcFrameLineSize;
		}

#ifdef WITH_NVUTILS
		NvBufSurfaceUnMap(nvbuf_surf, 0, plane);
#else
		NvBufferMemUnMap(dmabuf_fd, plane, &psrc_data);
#endif
	}
    return 0;
}

//Public API: fetch the next decoded frame.
//Non-blocking: returns -1 immediately when no filled frame is queued (the
//When wait=false: non-blocking, returns -1 immediately if no frame ready.
//When wait=true: blocks up to wait_timeout_ms using the pool's CV-based
//tiered wait. Returns -1 on timeout or shutdown.
//On success the frame data is copied into the caller's buffers, the pts
//is set, and the pool buffer is immediately recycled to the "empty" queue.
int nvmpi_decoder_get_frame(nvmpictx* h, nvFrame* frame, bool wait)
{
	nvmpi_dec_ctx* ctx = static_cast<nvmpi_dec_ctx*>(h);
	int ret;
	nvmpi_frame_buffer* fb;

	if (wait)
		fb = ctx->framePool->dqFilledBuf(
			std::chrono::milliseconds(ctx->wait_timeout_ms));
	else
		fb = ctx->framePool->dqFilledBuf();

	if (!fb) return -1;

	/* Mark buffer as checked out so respondToResolutionEvent knows
	 * to wait before destroying the pool (prevents use-after-free
	 * if a resolution change fires while we're copying). */
	ctx->frames_checked_out->fetch_add(1, std::memory_order_relaxed);
	ret = copyNvBufToFrame(ctx, fb, frame);
	frame->timestamp = fb->timestamp;
	ctx->framePool->qEmptyBuf(fb);
	ctx->frames_checked_out->fetch_sub(1, std::memory_order_relaxed);

	return ret;
}

//Wrapper that pairs a frame buffer with its owning pool for the
//release callback. Allocated on the heap per get_frame_fd call;
//freed inside frame_fd_release_impl.
struct nvmpi_frame_ref {
	nvmpi_frame_buffer *fb;
	NVMPI_bufPool<nvmpi_frame_buffer*> *pool;
	/* Shared flag from nvmpi_dec_ctx::pool_alive — stays valid after decoder
	 * close because the shared_ptr ref-count keeps it alive. Checked
	 * before accessing pool to avoid use-after-free when FFmpeg's
	 * scheduler drops frame refs after codec close (FFmpeg 8.1+). */
	std::shared_ptr<std::atomic<bool>> pool_alive;
	/* Shared counter from nvmpi_dec_ctx::frames_checked_out — decremented
	 * on release so respondToResolutionEvent knows when all frames
	 * have been returned before destroying the pool. */
	std::shared_ptr<std::atomic<int>> checked_out_count;
};

//Release callback for nvmpi_decoder_get_frame_fd.
//Returns the borrowed frame buffer to the pool's empty queue so it can
//be reused by the capture thread for future decoded frames, then frees
//the nvmpi_frame_ref wrapper.
/* Allocated in: nvmpi_decoder_get_frame_fd (below) */
/* Freed in: this function (via delete) */
static void frame_fd_release_impl(void *opaque)
{
	nvmpi_frame_ref *ref = static_cast<nvmpi_frame_ref*>(opaque);
	if (!ref) return;

	/* Return the frame buffer to the pool's empty queue so the
	 * capture thread can reuse it for future decoded frames.
	 *
	 * Guard: check pool_alive before accessing pool. When FFmpeg's
	 * scheduler closes the codec before all frame refs are released
	 * (FFmpeg 8.1+ changed shutdown order), the pool is already deleted.
	 * The shared_ptr keeps the atomic<bool> alive past decoder close,
	 * so this check is safe even after nvmpi_decoder_close returns. */
	if (ref->pool && ref->fb &&
	    ref->pool_alive && ref->pool_alive->load())
		ref->pool->qEmptyBuf(ref->fb);

	/* Decrement checked-out count — allows respondToResolutionEvent
	 * to proceed with pool teardown once all frames are returned. */
	if (ref->checked_out_count)
		ref->checked_out_count->fetch_sub(1, std::memory_order_release);

	delete ref;
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
//Ownership: the frame buffer is borrowed from the pool. Invoking the
//release callback returns it to the empty queue. Failure to release
//starves the pool and stalls decoding.
int nvmpi_decoder_get_frame_fd(nvmpictx* h,
	int *dmabuf_fd, int *width, int *height, int *pitch,
	int64_t *timestamp,
	nvmpi_frame_release_cb *release, void **opaque, bool wait)
{
	nvmpi_dec_ctx* ctx = static_cast<nvmpi_dec_ctx*>(h);
	nvmpi_frame_buffer* fb;

	/* Dequeue a filled frame buffer — same path as get_frame. */
	if (wait)
		fb = ctx->framePool->dqFilledBuf(
			std::chrono::milliseconds(ctx->wait_timeout_ms));
	else
		fb = ctx->framePool->dqFilledBuf();

	if (!fb) return -1;

	/* Expose the underlying DMA-BUF fd and buffer geometry.
	 * frame_linesize[0] is the Y plane pitch; for NV12 (2-plane) this
	 * is also the UV plane pitch (interleaved U/V at half height). */
	*dmabuf_fd = fb->dst_dma_fd;
	*width = (int)ctx->output_width;
	*height = (int)ctx->output_height;
	*pitch = (int)ctx->frame_linesize[0];
	*timestamp = (int64_t)fb->timestamp;

	/* Mark buffer as checked out — respondToResolutionEvent waits for
	 * this counter to reach 0 before destroying the pool. Increment
	 * BEFORE allocating the ref so the counter is accurate even if
	 * the ref allocation below fails (cleaned up in error path). */
	ctx->frames_checked_out->fetch_add(1, std::memory_order_acquire);

	/* Allocate a ref wrapper so the release callback can return this
	 * specific buffer to this specific pool. The wrapper is freed
	 * inside frame_fd_release_impl. */
	/* Allocated here; freed in frame_fd_release_impl() */
	nvmpi_frame_ref *ref = new (std::nothrow) nvmpi_frame_ref();
	if (!ref) {
		ctx->frames_checked_out->fetch_sub(1, std::memory_order_release);
		ctx->framePool->qEmptyBuf(fb);
		return -1;
	}
	ref->fb = fb;
	ref->pool = ctx->framePool;
	ref->pool_alive = ctx->pool_alive;
	ref->checked_out_count = ctx->frames_checked_out;

	*release = frame_fd_release_impl;
	*opaque = ref;

	return 0;
}
