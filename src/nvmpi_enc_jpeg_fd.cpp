/*
 * nvmpi_enc_jpeg_fd.cpp — zero-copy input of the JPEG encoder: encode a
 * registered DMA-BUF surface (#158, #5) instead of copying caller planes.
 *
 * Two paths, neither copies pixels on the CPU:
 *   direct — a YUV420 surface of exactly width x height goes straight to
 *            encodeFromFd (the same call put_frame ends in);
 *   VIC    — everything else (NV12, padded or larger surfaces) goes through
 *            one nvmpi_vic_transform into the encoder's own YUV420 input
 *            buffer, which then goes to encodeFromFd.
 *
 * The fd is borrowed for the synchronous call only: never closed, dup'd or
 * kept. Allocated here: the lazily created VIC context; freed in
 * nvmpi_jpeg_encoder_close() (nvmpi_enc_jpeg.cpp).
 */
#include "nvmpi_enc_jpeg_internal.h"

#ifdef WITH_NVUTILS

/* Max picture size VIC and the surface allocators accept (same bound as
 * nvmpi_vic_transform and nvmpi_surface_get_layout). */
#define JPEGFD_MAX_DIM 8192

/*
 * Validate the descriptor against the surface behind frame->fd. Everything
 * is checked before the encoder state is touched, so a refused frame leaves
 * the staged packet and the input buffer as they were. On success *lay holds
 * the surface layout. Returns 0 or NVMPI_ERR_INVAL.
 */
static int jpegfdValidate(const nvmpi_fd_frame *frame, nvmpi_fd_frame *lay)
{
	if (frame->struct_size < NVMPI_FD_FRAME_V1_SIZE || frame->fd < 0) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "put_frame_fd: bad struct_size %u or fd %d", frame->struct_size, frame->fd);
		return NVMPI_ERR_INVAL;
	}

	/* Bound the size before it reaches VIC or the output buffer estimate. */
	if (frame->width == 0 || frame->height == 0 ||
	    frame->width > JPEGFD_MAX_DIM || frame->height > JPEGFD_MAX_DIM) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "put_frame_fd: size %ux%u out of range (max %u)", frame->width, frame->height, (unsigned)JPEGFD_MAX_DIM);
		return NVMPI_ERR_INVAL;
	}

	/* Fails for dup'd or foreign fds (NvBufSurfaceFromFd only knows fds the
	 * allocator issued): VIC needs a registered surface too, so these are
	 * refused rather than copied. Also rejects formats other than NV12 and
	 * YUV420 (and their colorimetry variants). */
	memset(lay, 0, sizeof(*lay));
	lay->struct_size = sizeof(*lay);
	if (nvmpi_surface_get_layout(frame->fd, lay) != 0) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "put_frame_fd: fd %d is not a registered NV12/YUV420 surface", frame->fd);
		return NVMPI_ERR_INVAL;
	}

	if ((frame->format != NV_PIX_NV12 && frame->format != NV_PIX_YUV420) ||
	    frame->format != lay->format || frame->num_planes != lay->num_planes) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "put_frame_fd: format %d / planes %u do not match the surface (%d / %u)", (int)frame->format, frame->num_planes, (int)lay->format, lay->num_planes);
		return NVMPI_ERR_INVAL;
	}

	/* A wrong pitch/offset would make the engine read at the wrong place:
	 * the descriptor must be the surface's own layout, plane by plane. */
	for (unsigned int p = 0; p < lay->num_planes; p++) {
		if (frame->pitch[p] != lay->pitch[p] || frame->offset[p] != lay->offset[p]) {
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "put_frame_fd: plane %u pitch %u / offset %u differ from the surface (%u / %u)", p, frame->pitch[p], frame->offset[p], lay->pitch[p], lay->offset[p]);
			return NVMPI_ERR_INVAL;
		}
	}

	/* The picture must fit inside plane 0 of the surface. */
	if (frame->width > lay->width || frame->height > lay->height) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "put_frame_fd: %ux%u exceeds the %ux%u surface", frame->width, frame->height, lay->width, lay->height);
		return NVMPI_ERR_INVAL;
	}
	return 0;
}

/* True when the surface can go to encodeFromFd as it is: YUV420, exactly
 * width x height, and chroma planes that cover the (rounded up) chroma size
 * of an odd picture. */
static bool jpegfdIsDirect(const nvmpi_fd_frame *frame, const nvmpi_fd_frame *lay)
{
	NvBufSurface *surf = NULL;

	if (getenv("NVMPI_EXP_DIRECT") && lay->format == NV_PIX_NV12 && lay->width == frame->width && lay->height == frame->height) /* EXPERIMENT */
		return true;
	if (lay->format != NV_PIX_YUV420 || lay->width != frame->width ||
	    lay->height != frame->height)
		return false;
	if (NvBufSurfaceFromFd(frame->fd, (void **)&surf) != 0 || !surf ||
	    !surf->surfaceList)
		return false;
	const NvBufSurfacePlaneParams &pp = surf->surfaceList[0].planeParams;
	return pp.width[1] >= (frame->width + 1) / 2 &&
	       pp.height[1] >= (frame->height + 1) / 2;
}

#endif /* WITH_NVUTILS */

/*
 * Encode one DMA-BUF frame to JPEG synchronously. frame == NULL signals EOS.
 *
 * Returns 0 on success, NVMPI_ERR_INVAL for a descriptor that does not match
 * the surface (nothing encoded, a staged packet is kept), -1 for a NULL ctx,
 * after EOS, on encode or VIC failure, and on legacy (non-NvUtils) builds.
 */
int nvmpi_jpeg_encoder_put_frame_fd(nvmpictx *handle, const nvmpi_fd_frame *frame)
{
	if (!handle)
		return -1;

	nvmpictx_jpegenc *ctx = reinterpret_cast<nvmpictx_jpegenc*>(handle);

	/* EOS marker: NULL frame signals end of stream. */
	if (!frame) {
		ctx->eos.store(true);
		return 0;
	}

	if (ctx->eos.load())
		return -1;

#ifndef WITH_NVUTILS
	/* The registered-surface lookup behind the validation is NvUtils only. */
	NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "put_frame_fd needs the NvUtils buffer API");
	return -1;
#else
	nvmpi_fd_frame lay;
	int ret = jpegfdValidate(frame, &lay);
	if (ret != 0)
		return ret;

	/* Validated: from here a staged packet of an earlier frame is replaced,
	 * as put_frame does. */
	jpegDropStaged(ctx);

	const uint32_t w = frame->width, h = frame->height;
	int enc_fd = frame->fd;

	if (!jpegfdIsDirect(frame, &lay)) {
		/* Allocated here on first use; freed in nvmpi_jpeg_encoder_close(). */
		if (!ctx->vic) {
			ctx->vic = nvmpi_vic_create();
			if (!ctx->vic) {
				NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "put_frame_fd: VIC context creation failed");
				return -1;
			}
		}

		/* (Re)allocate the YUV420 input buffer when the size changes, shared
		 * with put_frame so the two can be interleaved. */
		if (w != ctx->width || h != ctx->height) {
			if (!ctx->allocInputBuffer(w, h)) {
				NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "input buffer allocation failed");
				return -1;
			}
		}

		/* One hardware crop/convert into the input buffer: the source rect
		 * is the picture, so padding of the source is not encoded. */
		if (nvmpi_vic_transform(ctx->vic, frame->fd, ctx->input_buf.dst_dma_fd,
		                        w, h, w, h) != 0) {
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpegenc", "put_frame_fd: VIC transform failed");
			return -1;
		}
		enc_fd = ctx->input_buf.dst_dma_fd;
	}

	return jpegEncodeStage(ctx, enc_fd, w, h, (unsigned long)frame->timestamp) ? -1 : 0;
#endif
}
