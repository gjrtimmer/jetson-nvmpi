/*
 * nvmpi_enc_input.cpp — encoder frame submission, memory-copy path (layer 1).
 *
 * Implements the frame-copy helper and nvmpi_encoder_put_frame() of
 * the public C API in include/nvmpi.h on top of NVIDIA's V4L2 NvVideoEncoder
 * sample class. Consumed by the FFmpeg wrapper in
 * ffmpeg/dev/common/libavcodec/nvmpi_enc.c.
 *
 * V4L2 encoder model (M2M device, two queues — note the naming is from the
 * device's point of view, mirrored relative to the decoder):
 *   - OUTPUT plane  = input side: raw YUV frames go in.
 *   - CAPTURE plane = output side: encoded bitstream comes out.
 *
 * Data flow / threading:
 *   user thread:      nvmpi_encoder_put_frame() (or the DMA-BUF variant,
 *                     nvmpi_encoder_put_frame_fd()) copies or maps the
 *                     caller's frame into an OUTPUT-plane buffer and queues
 *                     it (blocks in dqBuffer once all buffers are in flight).
 *   DQ thread:        NvVideoEncoder's capture-plane dequeue thread invokes
 *                     encoder_capture_plane_dq_callback() (nvmpi_enc_output.cpp)
 *                     per encoded buffer.
 *
 * Companion files:
 *   nvmpi_enc_internal.h  — shared includes, defines, nvmpi_enc_ctx struct,
 *                           and forward declarations.
 *   nvmpi_enc_input_fd.cpp — nvmpi_encoder_put_frame_fd() (DMA-BUF path).
 *   nvmpi_enc_init.cpp    — nvmpi_create_encoder() (device open, format
 *                           negotiation, plane/stream setup).
 *   nvmpi_enc_output.cpp  — encoder_capture_plane_dq_callback and
 *                           setup_output_dmabuf (OUTPUT-plane DMA path).
 */
#include "nvmpi_enc_internal.h"

//Bytes per row actually copied for one plane. bytesperpixel comes from the
//negotiated NvBuffer format: 1 for 8-bit Y/U/V, 2 for 8-bit interleaved
//CbCr and for 16-bit P010 Y, 4 for P010 interleaved CbCr (two 16-bit
//samples). Kept in size_t: uint32 * uint32 could wrap for hostile sizes.
static inline size_t enc_row_bytes(const NvBuffer::NvBufferPlane &plane)
{
	return (size_t)plane.fmt.bytesperpixel * (size_t)plane.fmt.width;
}

//Check, before anything is copied, that the caller's frame can fill the
//OUTPUT-plane buffer without reading or writing out of bounds. Runs on
//untrusted caller input (linesize/payload come from the application).
//Source side: every plane needs a payload pointer and a linesize of at
//least one copied row (the copy reads row_bytes per row, so a smaller
//stride would read into the next row's tail, or past the plane for the
//last row). The source plane size is not passed in nvFrame reliably
//(payload_size may be 0), so it cannot be bounds-checked beyond that.
//Destination side: a copied row must fit the negotiated stride and the
//whole plane (stride * height) must fit the buffer length reported by the
//driver (length 0 = not reported, e.g. a not-yet-mapped buffer: skipped).
//The CPU mapping (plane.data) is checked in copyFrameToNvBuf, not here: in
//the DMA OUTPUT-plane variant it only exists after the buffer was mapped.
//Returns 0 when valid, -1 (after logging) otherwise.
static int enc_check_frame(const nvFrame *frame, const NvBuffer &buffer)
{
	for (uint32_t i = 0; i < buffer.n_planes && i < 3; i++)
	{
		const NvBuffer::NvBufferPlane &plane = buffer.planes[i];
		size_t row = enc_row_bytes(plane);

		if (!frame->payload[i])
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "put_frame: plane %u has no payload", i);
			return -1;
		}
		if (frame->linesize[i] < row)
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "put_frame: plane %u linesize %u < %zu bytes per row "
			          "(wrong pixel format for this encoder?)", i, frame->linesize[i], row);
			return -1;
		}
		if (row > plane.fmt.stride ||
		    (plane.length && (uint64_t)plane.fmt.stride * plane.fmt.height > plane.length))
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "put_frame: plane %u exceeds the OUTPUT buffer", i);
			return -1;
		}
	}
	return 0;
}

//Copy the caller's raw frame planes into a V4L2 OUTPUT-plane NvBuffer.
//Line-by-line memcpy because the caller's linesize and the hw buffer's
//stride generally differ; sets bytesused per plane so the driver knows the
//payload extent. The source frame is untouched (caller keeps ownership).
//Plane geometry is bytesperpixel * width per row, so P010M (2 and 4 bytes
//per sample/pair) copies 16-bit samples unchanged. The frame is validated
//first (enc_check_frame), so nothing is written on a bad frame; returns -1
//then, 0 on success.
int copyFrameToNvBuf(nvFrame* frame, NvBuffer& buffer)
{
	uint32_t i, j;
	char *dataDst;
	char *dataSrc;
	if (enc_check_frame(frame, buffer) < 0)
		return -1;
	for (i = 0; i < buffer.n_planes; i++)
	{
		NvBuffer::NvBufferPlane &plane = buffer.planes[i];
		unsigned int &frameLineSize = frame->linesize[i];
		size_t copySz = enc_row_bytes(plane);
		dataDst = (char *) plane.data;
		dataSrc = (char *) frame->payload[i];
		if (!dataDst)
			return -1;       /* buffer not CPU-mapped: nothing to copy into */
		plane.bytesused = 0;
		for (j = 0; j < plane.fmt.height; j++)
		{
			memcpy(dataDst, dataSrc, copySz);
			dataDst += plane.fmt.stride;
			dataSrc += frameLineSize;
		}
		plane.bytesused = plane.fmt.stride * plane.fmt.height;
	}
	return 0;
}

//Public API: submit one raw frame for encoding (or EOS when frame==NULL).
//Buffer acquisition mirrors the decoder's put_packet: use each OUTPUT-plane
//buffer once by index, then block in dqBuffer(-1) until the encoder frees
//one. The frame is copied in, the pts (microseconds) goes into the V4L2
//timestamp, caches are synced for device access, and the buffer is queued.
//frame==NULL queues a zero-byte buffer = V4L2 EOS signal and sets
//ctx->flushing. Returns 0 on success, -2 if already flushing, negative on
//encoder error.
int nvmpi_encoder_put_frame(nvmpictx* h,nvFrame* frame)
{
	nvmpi_enc_ctx* ctx = static_cast<nvmpi_enc_ctx*>(h);
	if(ctx->flushing.load(std::memory_order_acquire)) return -2;

	int ret;
	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[MAX_PLANES];
	NvBuffer *nvBuffer;

	memset(&v4l2_buf, 0, sizeof(v4l2_buf));
	memset(planes, 0, sizeof(planes));

	v4l2_buf.m.planes = planes;

	if(ctx->enc->isInError())
		return -1;

	/* Validate the frame geometry BEFORE a buffer is taken: a rejected frame
	 * must not consume an OUTPUT slot (it would never be queued back). All
	 * OUTPUT buffers share one format, so buffer 0 stands for every slot. */
	if(frame && enc_check_frame(frame, *ctx->enc->output_plane.getNthBuffer(0)) < 0)
		return NVMPI_ERR_INVAL;

	/* Non-blocking back-pressure (issue #125): refuse a frame whose AU
	 * could find no packet. Checked before any OUTPUT buffer is taken, so
	 * EAGAIN leaves the encoder untouched. EOS (frame==NULL) yields no AU
	 * and is never refused here. */
	if(frame && !ctx->blocking_mode && !enc_can_admit(ctx))
		return NVMPI_ERR_EAGAIN;

	if(ctx->index < ctx->enc->output_plane.getNumBuffers())
	{
		nvBuffer=ctx->enc->output_plane.getNthBuffer(ctx->index);
		v4l2_buf.index = ctx->index;
		ctx->index++;

#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_DMA)
		v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		v4l2_buf.memory = V4L2_MEMORY_DMABUF;
		// Map output plane buffer for memory type DMABUF.
		ret = ctx->enc->output_plane.mapOutputBuffers(v4l2_buf, ctx->output_plane_fd[v4l2_buf.index]);
		if (ret < 0)
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error while mapping buffer at output plane");
		}
#endif
	}
	else
	{
		/* Reclaim a used OUTPUT-plane buffer. In blocking mode (default),
		 * dqBuffer waits indefinitely (-1) until the encoder frees one.
		 * In non-blocking mode, timeout=0 returns immediately — if no
		 * buffer is available, return NVMPI_ERR_EAGAIN so the caller can
		 * drain encoded packets and retry. */
		int dq_timeout = ctx->blocking_mode ? -1 : 0;
		ret = ctx->enc->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, dq_timeout);
		if (ret < 0)
		{
			if (!ctx->blocking_mode) {
				/* Non-blocking: no buffer available right now */
				return NVMPI_ERR_EAGAIN;
			}
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error DQing buffer at output plane");
			return -1;
		}
	}

	if(frame)
	{
		//normal frame: copy planes in and carry the pts on the buffer
		copyFrameToNvBuf(frame, *nvBuffer);
		v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
		v4l2_buf.timestamp.tv_usec = frame->timestamp % 1000000;
		v4l2_buf.timestamp.tv_sec = frame->timestamp / 1000000;
	}
	else
	{
		//send EOS and flush
		//(a buffer queued with bytesused==0 tells the encoder to drain)
		ctx->flushing.store(true, std::memory_order_release);
		v4l2_buf.m.planes[0].m.userptr = 0;
		v4l2_buf.m.planes[0].bytesused = v4l2_buf.m.planes[1].bytesused = v4l2_buf.m.planes[2].bytesused = 0;
	}

	//needed for V4L2_MEMORY_MMAP and V4L2_MEMORY_DMABUF
	//flush CPU caches so the hw encoder sees the bytes just written
	for (uint32_t j = 0 ; j < nvBuffer->n_planes; j++)
	{
#ifdef WITH_NVUTILS
		NvBufSurface *nvbuf_surf = 0;
		ret = NvBufSurfaceFromFd (nvBuffer->planes[j].fd, (void**)(&nvbuf_surf));
		if (ret < 0)
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error while NvBufSurfaceFromFd");
		}
		ret = NvBufSurfaceSyncForDevice (nvbuf_surf, 0, j);
		if (ret < 0)
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error while NvBufSurfaceSyncForDevice at output plane for V4L2_MEMORY_DMABUF");
		}
#else
		ret = NvBufferMemSyncForDevice (nvBuffer->planes[j].fd, j, (void **)&nvBuffer->planes[j].data);
		if (ret < 0)
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "Error while NvBufferMemSyncForDevice at output plane for V4L2_MEMORY_DMABUF");
		}
#endif
	}

	//for V4L2_MEMORY_DMABUF only
#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_DMA)
	for (uint32_t j = 0; j < nvBuffer->n_planes; j++)
	{
		v4l2_buf.m.planes[j].bytesused = nvBuffer->planes[j].bytesused;
	}
#endif

	/* This frame now owes one AU. Counted BEFORE the queue call so the DQ
	 * callback can never decrement first; rolled back if the queue fails. */
	if(frame)
		ctx->aus_pending.fetch_add(1, std::memory_order_seq_cst);
	ret = ctx->enc->output_plane.qBuffer(v4l2_buf, NULL);
	if (ret < 0) {
		if(frame)
			ctx->aus_pending.fetch_sub(1, std::memory_order_seq_cst);
		NVMPI_LOG(NVMPI_LOG_ERROR, "Error while queueing buffer at output plane (code=%d)", ret);
		return ret;
	}

	return 0;
}
