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
#include "nvmpi_frame_fit.h"

//Copied geometry of one plane: rows of row-bytes each.
struct enc_geom {
	size_t row;
	uint32_t rows;
};

//Geometry the frame has in plane i (#154). NvBuffer derives the chroma
//plane size by flooring (width/2 x height/2), but a 4:2:0 picture of odd
//size has ceil((w+1)/2) x ceil((h+1)/2) chroma samples: with the floored
//geometry the last chroma column and the last chroma row of an odd-sized
//frame are never copied. The picture size is taken from the luma plane
//(plane 0 is the encoder's configured size); a chroma plane is the odd-size
//case exactly when 2 * its floored size + 1 == the luma size, then it grows
//by one. Even sizes and non-subsampled planes are unchanged. Bytes per
//sample (bytesperpixel): 1 for 8-bit Y/U/V, 2 for interleaved 8-bit CbCr and
//16-bit P010 Y, 4 for P010 CbCr pairs. size_t: uint32 * uint32 could wrap.
static void enc_geoms(const NvBuffer &buffer, uint32_t i, enc_geom *flo, enc_geom *cei)
{
	const NvBuffer::NvBufferPlane &plane = buffer.planes[i];
	uint32_t w = plane.fmt.width, h = plane.fmt.height;
	size_t bpp = plane.fmt.bytesperpixel;

	flo->row = bpp * (size_t)w;
	flo->rows = h;
	if (i > 0)
	{
		if ((uint64_t)w * 2 + 1 == buffer.planes[0].fmt.width)
			w++;
		if ((uint64_t)h * 2 + 1 == buffer.planes[0].fmt.height)
			h++;
	}
	cei->row = bpp * (size_t)w;
	cei->rows = h;
}

//Check, before anything is copied, that the caller's frame can fill the
//OUTPUT-plane buffer without reading or writing out of bounds, and decide
//the geometry to copy per plane (g, may be NULL). Runs on untrusted caller
//input (linesize/payload come from the application).
//Source side: every plane needs a payload pointer and a linesize of at
//least one copied row (the copy reads row_bytes per row, so a smaller
//stride would read into the next row's tail, or past the plane for the
//last row). Source capacity (#150): when payload_size[i] is non-zero it is
//the caller's allocation size of plane i, and the copy reads
//(rows-1)*linesize + row bytes from it; a larger need (frame smaller than
//the encoder's configured size, cropped or stale buffer) would read past the
//caller's buffer, so it is refused. payload_size[i] == 0 means "not
//declared" (legacy callers): accepted, the caller guarantees the size.
//Destination side: a copied row must fit the negotiated stride and the
//whole plane (stride * rows) must fit the buffer length reported by the
//driver (length 0 = not reported, e.g. a not-yet-mapped buffer: skipped).
//The CPU mapping (plane.data) is checked in copyFrameToNvBuf, not here: in
//the DMA OUTPUT-plane variant it only exists after the buffer was mapped.
//Ceil vs floored chroma (#154), per plane: the full (ceil) geometry is used
//when the caller's frame holds it (linesize >= ceil row bytes, and
//payload_size covers it or is 0) and the negotiated plane can hold it (ceil
//row <= stride, ceil rows * stride <= length, length reported). Older
//callers size the chroma payload with floored rows (linesize*height/2):
//their declared payload_size covers only the floored geometry, which is then
//copied exactly as before (no refusal). A payload_size that covers not even
//that is refused. With payload_size 0 the linesize decides: a linesize below
//the ceil row bytes means the floored geometry; the caller guarantees the
//rows then. A plane that cannot hold the ceil geometry falls back to the
//floored one (logged once) instead of overflowing.
//Returns 0 when valid, -1 (after logging) otherwise.
static int enc_check_frame(const nvFrame *frame, const NvBuffer &buffer, enc_geom g[3])
{
	static std::atomic<bool> warned(false);

	for (uint32_t i = 0; i < buffer.n_planes && i < 3; i++)
	{
		const NvBuffer::NvBufferPlane &plane = buffer.planes[i];
		enc_geom flo, cei, use;
		size_t need;

		enc_geoms(buffer, i, &flo, &cei);
		if (!frame->payload[i])
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "put_frame: plane %u has no payload", i);
			return -1;
		}
		if (frame->linesize[i] < flo.row)
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "put_frame: plane %u linesize %u < %zu bytes per row "
			          "(wrong pixel format for this encoder?)", i, frame->linesize[i], flo.row);
			return -1;
		}
		use = flo;
		if (cei.row != flo.row || cei.rows != flo.rows)
		{
			bool src_ok = frame->linesize[i] >= cei.row &&
			              nvmpi_plane_need(cei.rows, frame->linesize[i], (unsigned int)cei.row, &need) &&
			              (!frame->payload_size[i] || need <= frame->payload_size[i]);
			bool dst_ok = cei.row <= plane.fmt.stride && plane.length &&
			              (uint64_t)plane.fmt.stride * cei.rows <= plane.length;

			if (src_ok && dst_ok)
				use = cei;
			else if (src_ok && !warned.exchange(true))
				NVMPI_LOG(NVMPI_LOG_WARN, "put_frame: OUTPUT plane %u cannot hold the %ux%u chroma "
				          "of an odd-sized frame, last chroma column/row is dropped", i,
				          (unsigned)(cei.row / plane.fmt.bytesperpixel), cei.rows);
		}
		if (!nvmpi_plane_need(use.rows, frame->linesize[i], (unsigned int)use.row, &need) ||
		    (frame->payload_size[i] && need > frame->payload_size[i]))
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "put_frame: plane %u needs %zu bytes but payload_size is %lu",
			          i, need, frame->payload_size[i]);
			return -1;
		}
		if (use.row > plane.fmt.stride ||
		    (plane.length && (uint64_t)plane.fmt.stride * use.rows > plane.length))
		{
			NVMPI_LOG(NVMPI_LOG_ERROR, "put_frame: plane %u exceeds the OUTPUT buffer", i);
			return -1;
		}
		if (g)
			g[i] = use;
	}
	return 0;
}

//Copy the caller's raw frame planes into a V4L2 OUTPUT-plane NvBuffer.
//Line-by-line memcpy because the caller's linesize and the hw buffer's
//stride generally differ; sets bytesused per plane so the driver knows the
//payload extent. The source frame is untouched (caller keeps ownership).
//Plane geometry is bytesperpixel * width per row, so P010M (2 and 4 bytes
//per sample/pair) copies 16-bit samples unchanged; chroma of odd sizes
//rounds up (see enc_check_frame, #154). The frame is validated
//first (enc_check_frame), so nothing is written on a bad frame; returns -1
//then, 0 on success.
int copyFrameToNvBuf(nvFrame* frame, NvBuffer& buffer)
{
	uint32_t i, j;
	char *dataDst;
	char *dataSrc;
	enc_geom g[3];
	if (enc_check_frame(frame, buffer, g) < 0)
		return -1;
	for (i = 0; i < buffer.n_planes; i++)
	{
		NvBuffer::NvBufferPlane &plane = buffer.planes[i];
		unsigned int &frameLineSize = frame->linesize[i];
		size_t copySz = g[i].row;
		dataDst = (char *) plane.data;
		dataSrc = (char *) frame->payload[i];
		if (!dataDst)
			return -1;       /* buffer not CPU-mapped: nothing to copy into */
		plane.bytesused = 0;
		for (j = 0; j < g[i].rows; j++)
		{
			memcpy(dataDst, dataSrc, copySz);
			dataDst += plane.fmt.stride;
			dataSrc += frameLineSize;
		}
		plane.bytesused = plane.fmt.stride * g[i].rows;
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
	if(frame && enc_check_frame(frame, *ctx->enc->output_plane.getNthBuffer(0), NULL) < 0)
		return NVMPI_ERR_INVAL;

	/* Back-pressure (issues #125, #145), both modes: refuse a frame whose AU
	 * could find no packet. Checked before any OUTPUT buffer is taken, so
	 * EAGAIN leaves the encoder untouched. Blocking mode cannot wait here
	 * instead: packets free only when the caller reads (get_packet) and
	 * returns them (qEmptyPacket), which this thread cannot do while blocked,
	 * so a wait would deadlock a single-threaded caller. EOS (frame==NULL)
	 * yields no AU and is never refused here. */
	if(frame && !enc_can_admit(ctx))
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
		//total round trip for any 64-bit timestamp, see nvmpi_pts.h
		ctx->pts_map.encode((int64_t)frame->timestamp, &v4l2_buf.timestamp);
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
