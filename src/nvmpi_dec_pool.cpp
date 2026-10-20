/*
 * nvmpi_dec_pool.cpp — decoder destination frame pool (libnvmpi).
 *
 * Moved out of nvmpi_dec_init.cpp (issue #105) to keep that file small:
 *   - deinitFramePool()/initFramePool() — free/allocate the pitch-linear
 *     destination DMA buffers (VIC output).
 */
#include "nvmpi_dec_internal.h"

//Destroy all frame buffers tracked by allocatedFrameBufs and drain both
//pool queues. Uses the vector instead of the queues so that buffers
//temporarily outside both queues (e.g. held by a concurrent get_frame
//call during resolution change) are still freed.
void nvmpi_dec_ctx::deinitFramePool()
{
	while(framePool->dqEmptyBuf()) {}
	while(framePool->dqFilledBuf()) {}

	for (auto* fb : allocatedFrameBufs)
	{
		fb->destroy();
		delete fb;
	}
	allocatedFrameBufs.clear();
}

//Allocate frame_pool_size pitch-linear DMA buffers at the output resolution
//and user-requested pixel format, and seed the pool's "empty" queue with
//them. These are the destination buffers of the VIC transform — distinct
//from the block-linear CAPTURE-plane buffers the decoder writes into.
void nvmpi_dec_ctx::initFramePool()
{
	/* Validate pool size — zero or negative would allocate nothing, causing
	 * downstream dqEmptyBuf() to block forever or return null. */
	if (frame_pool_size <= 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "initFramePool: frame_pool_size=%d is invalid, skipping allocation", frame_pool_size);
		return;
	}

	/* If the pool is already populated (e.g. initFramePool called without a
	 * prior deinitFramePool), tear down the existing buffers first to prevent
	 * leaking DMA buffer FDs. deinitFramePool is safe on an empty pool. */
	if (!allocatedFrameBufs.empty()) {
		NVMPI_LOG(NVMPI_LOG_WARN, "initFramePool: pool already allocated (%zu buffers), deinitializing first to prevent DMA fd leak", allocatedFrameBufs.size());
		deinitFramePool();
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

	for(int i=0;i<frame_pool_size;i++)
	{
		nvmpi_frame_buffer* fb = new nvmpi_frame_buffer();
		if(!fb->alloc(input_params)) { delete fb; break; }
		allocatedFrameBufs.push_back(fb);
		framePool->qEmptyBuf(fb);
	}
	return;
}
