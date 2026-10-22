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
 *
 * nvmpi_decoder_get_frame() / _get_frame2() live in nvmpi_dec_frame.cpp.
 *
 * Companion files:
 *   nvmpi_dec_internal.h  — shared includes, defines, nvmpi_dec_ctx struct,
 *                           and forward declarations.
 *   nvmpi_dec_fd.cpp      — nvmpi_decoder_get_frame_fd(), borrow/release.
 *   nvmpi_dec_pool.cpp    — frame-pool generations.
 *   nvmpi_dec_init.cpp    — nvmpi_create_decoder() and the
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
//AV1: packets before the first sequence header are dropped and report 0.
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

	//AV1 before the first sequence header: consume without queueing. The
	//Orin driver would hold the buffer forever and block this call (#147).
	//A zero-sized payload is the EOS marker and always goes through.
	if (packet->payload_size != 0 && nvmpi_dec_av1_drop(ctx, packet))
		return 0;

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
	//total round trip for any 64-bit pts (AV_NOPTS_VALUE, negatives, huge):
	//the kernel keeps the timeval as u64 ns, see nvmpi_pts.h
	ctx->pts_map.encode((int64_t)packet->pts, &v4l2_buf.timestamp);

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
//The callers (nvmpi_dec_frame.cpp) have proven before this call that every
//plane fits the caller's memory (nvmpi_frame_fit.h); it does not check.
int copyNvBufToFrame(const dec_gen* g, nvmpi_frame_buffer *nvmpiBuf, nvFrame* frame)
{
	int ret;
	char *dataDst;
	char *dataSrc;

	for(unsigned int plane=0; plane<g->num_planes; plane++)
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
		unsigned int srcFrameLineSize = g->linesize[plane];
		unsigned int copySz = g->linedatasize[plane];

		for (unsigned int i = 0; i < g->height[plane]; i++)
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
