/*
 * nvmpi_enc_jpeg_buf.cpp — buffer side of the JPEG encoder: the input DMA
 * buffer, the CPU copy of a caller frame into it, and the shared
 * encode-and-stage step used by put_frame and put_frame_fd.
 *
 * Allocated by allocInputBuffer(); freed by freeInputBuffer().
 */
#include "nvmpi_enc_jpeg_internal.h"

/* ------------------------------------------------------------------ */
/* Input DMA buffer management                                         */
/* ------------------------------------------------------------------ */

/*
 * Allocate a pitch-linear YUV420 DMA buffer at width×height.
 * This is the input surface for encodeFromFd. A single buffer suffices
 * because encoding is synchronous — we fill it, encode, then reuse.
 *
 * Allocated here; freed by freeInputBuffer().
 */
bool nvmpictx_jpegenc::allocInputBuffer(uint32_t w, uint32_t h)
{
	/* Free any existing buffer first (resolution change path). */
	freeInputBuffer();

	NvBufferCreateParams params;
	memset(&params, 0, sizeof(params));
	params.width = w;
	params.height = h;
	params.layout = NvBufferLayout_Pitch;
	/* encodeFromFd supports YUV420 and NV12; we use YUV420 (planar). */
	params.colorFormat = NvBufferColorFormat_YUV420;
#ifdef WITH_NVUTILS
	params.memType = NVBUF_MEM_SURFACE_ARRAY;
	params.memtag = NvBufSurfaceTag_VIDEO_CONVERT;
#else
	params.payloadType = NvBufferPayload_SurfArray;
	params.nvbuf_tag = NvBufferTag_VIDEO_ENC;
#endif

	if (!input_buf.alloc(params)) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "failed to allocate input DMA buffer %ux%u", w, h);
		return false;
	}

	input_buf_allocated = true;
	width = w;
	height = h;

	updatePlaneParams();
	return true;
}

/*
 * Free the input DMA buffer. Safe to call when not allocated.
 * Allocated by allocInputBuffer(); freed here.
 */
void nvmpictx_jpegenc::freeInputBuffer()
{
	if (input_buf_allocated) {
		input_buf.destroy();
		input_buf_allocated = false;
	}
	num_planes = 0;
	memset(plane_pitch, 0, sizeof(plane_pitch));
	memset(plane_height, 0, sizeof(plane_height));
	memset(plane_linedata, 0, sizeof(plane_linedata));
}

/*
 * Query actual plane geometry from the allocated input buffer and cache it.
 * These values drive the memcpy from the caller's nvFrame into the DMA
 * buffer in put_frame.
 */
void nvmpictx_jpegenc::updatePlaneParams()
{
	if (!input_buf_allocated) return;

#ifdef WITH_NVUTILS
	NvBufSurfacePlaneParams parm;
	NvBufSurfaceParams surf_params = input_buf.dst_dma_surface->surfaceList[0];
	parm = surf_params.planeParams;
#else
	NvBufferParams parm;
	int ret = NvBufferGetParams(input_buf.dst_dma_fd, &parm);
	if (ret < 0) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "NvBufferGetParams failed");
		return;
	}
#endif

	num_planes = parm.num_planes;
	for (unsigned int i = 0; i < num_planes; i++)
	{
		plane_pitch[i] = parm.pitch[i];
		plane_height[i] = parm.height[i];
#ifdef WITH_NVUTILS
		plane_linedata[i] = parm.width[i] * parm.bytesPerPix[i];
#else
		/* Legacy API: YUV420 planar — all planes are 1 byte per pixel. */
		plane_linedata[i] = parm.width[i];
#endif
	}
}

/* ------------------------------------------------------------------ */
/* Copy caller's nvFrame into the DMA input buffer.                    */
/* Reverse of the decoder's jpegCopyToFrame — writes into DMA instead  */
/* of reading from it.                                                 */
/* ------------------------------------------------------------------ */

int jpegCopyFromFrame(nvmpictx_jpegenc *ctx, const nvFrame *frame)
{
	int ret;

	for (unsigned int plane = 0; plane < ctx->num_planes; plane++)
	{
		char *dataDst;
#ifdef WITH_NVUTILS
		NvBufSurface *nvbuf_surf = ctx->input_buf.dst_dma_surface;
		ret = NvBufSurfaceMap(nvbuf_surf, 0, plane, NVBUF_MAP_READ_WRITE);
		if (ret != 0) {
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "NvBufSurfaceMap failed plane=%u", plane);
			return ret;
		}
		dataDst = (char *)nvbuf_surf->surfaceList[0].mappedAddr.addr[plane];
#else
		void *pdst_data;
		ret = NvBufferMemMap(ctx->input_buf.dst_dma_fd, plane,
		                     NvBufferMem_Read_Write, &pdst_data);
		if (ret != 0) {
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "NvBufferMemMap failed plane=%u", plane);
			return ret;
		}
		dataDst = (char *)pdst_data;
#endif

		const char *dataSrc = (const char *)frame->payload[plane];
		unsigned int srcStride = frame->linesize[plane];
		unsigned int dstStride = ctx->plane_pitch[plane];
		/* Copy only the valid data width — avoid writing past the frame's
		 * actual data if the DMA buffer pitch is wider. */
		unsigned int copySz = ctx->plane_linedata[plane];

		/* Bounds check: do not read beyond what the caller provided. */
		if (srcStride < copySz)
			copySz = srcStride;

		for (unsigned int i = 0; i < ctx->plane_height[plane]; i++)
		{
			memcpy(dataDst, dataSrc, copySz);
			dataDst += dstStride;
			dataSrc += srcStride;
		}

		/* Sync the written data to hardware before encodeFromFd reads it. */
#ifdef WITH_NVUTILS
		NvBufSurfaceSyncForDevice(nvbuf_surf, 0, plane);
		NvBufSurfaceUnMap(nvbuf_surf, 0, plane);
#else
		NvBufferMemSyncForDevice(ctx->input_buf.dst_dma_fd, plane, &pdst_data);
		NvBufferMemUnMap(ctx->input_buf.dst_dma_fd, plane, &pdst_data);
#endif
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Staged output and the shared encode step                            */
/* ------------------------------------------------------------------ */

/*
 * Drop an un-consumed staged packet.
 * Zero out before freeing — encoded frame data.
 */
void jpegDropStaged(nvmpictx_jpegenc *ctx)
{
	if (ctx->out_buf) {
		memset(ctx->out_buf, 0, ctx->out_buf_size);
		free(ctx->out_buf);
		ctx->out_buf = nullptr;
	}
	ctx->out_buf_size = 0;
	ctx->packet_ready = false;
}

/*
 * Encode the YUV420/NV12 surface behind fd (a w x h picture) with the NVJPG
 * engine and stage the result for get_packet(). Any previously staged packet
 * must already be dropped. Returns 0 on success, -1 on failure (nothing
 * staged).
 */
int jpegEncodeStage(nvmpictx_jpegenc *ctx, int fd, uint32_t w, uint32_t h,
                    unsigned long pts)
{
	/* Pre-allocate output buffer for encodeFromFd.
	 * The Tegra-patched libjpeg passes out_buf_size to
	 * jpeg_set_hardware_acceleration_parameters_enc() as the HW DMA output
	 * buffer size.  When out_buf_size is 0 the HW path allocates a zero-size
	 * buffer and segfaults when the encoder writes into it (observed on
	 * Orin at ≥640x480).  Pre-allocating a worst-case buffer avoids this;
	 * libjpeg will reallocate if the estimate is too small.
	 *
	 * Worst-case JPEG: uncompressed YUV420 = w*h*1.5, plus JPEG headers.
	 * We use w*h*2 + 65536 as a safe upper bound.
	 *
	 * Integer overflow protection: width/height validated against
	 * NVJPEG_ENC_MAX_DIM (16384) by the callers, so the product fits uint64. */
	unsigned long jpeg_alloc = (unsigned long)w * h * 2 + 65536;
	unsigned char *jpeg_buf = (unsigned char *)malloc(jpeg_alloc);
	if (!jpeg_buf) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "failed to allocate JPEG output buffer (%lu bytes)", jpeg_alloc);
		return -1;
	}
	/* Zero-initialize — prevent information leak if encode partially fills
	 * the buffer and the caller reads beyond the valid JPEG data. */
	memset(jpeg_buf, 0, jpeg_alloc);
	unsigned long jpeg_size = jpeg_alloc;

	/* encodeFromFd: the Tegra NVJPG engine encodes from the DMA fd.
	 * out_buf/out_buf_size are in/out — libjpeg may reallocate if the
	 * pre-allocated buffer is too small.  Caller must free() the final
	 * *out_buf pointer (which may differ from the original allocation).
	 *
	 * Parameters:
	 *   fd          — DMA buffer fd containing YUV420 frame
	 *   color_space — JCS_YCbCr (YUV420, the only supported format)
	 *   out_buf     — pointer to output buffer (pre-allocated, may be reallocated)
	 *   out_buf_size — in: buffer capacity; out: actual JPEG size
	 *   quality     — libjpeg quality 1-100
	 */
	int ret = ctx->jpegenc->encodeFromFd(fd,
	                                     JCS_YCbCr,
	                                     &jpeg_buf, jpeg_size,
	                                     ctx->quality);
	if (ret < 0) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "encodeFromFd failed");
		if (jpeg_buf) {
			/* Zero out before freeing — may contain partial frame data. */
			memset(jpeg_buf, 0, jpeg_size);
			free(jpeg_buf);
		}
		return -1;
	}

	/* Stage output for get_packet. */
	ctx->out_buf = jpeg_buf;
	ctx->out_buf_size = jpeg_size;
	ctx->out_pts = pts;
	ctx->packet_ready = true;

	return 0;
}
