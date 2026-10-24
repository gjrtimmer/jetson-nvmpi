/*
 * nvmpi_dec_resevent.cpp - capture-plane (re)arming on a V4L2
 * RESOLUTION_CHANGE event. Called only by the capture thread
 * (nvmpi_dec_capture.cpp), split out of it to keep both files small.
 */
#include "nvmpi_dec_internal.h"

void respondToResolutionEvent(v4l2_format &format, v4l2_crop &crop,nvmpi_dec_ctx* ctx)
{
	int ret=0;

    /* Get capture plane format from the decoder.
       This may change after resolution change event.
       Refer ioctl VIDIOC_G_FMT */
	ret = ctx->dec->capture_plane.getFormat(format);
	TEST_ERROR(ret < 0, "Error: Could not get format from decoder capture plane", ret);

    /* Get the display resolution from the decoder.
       Refer ioctl VIDIOC_G_CROP */
	ret = ctx->dec->capture_plane.getCrop(crop);
	TEST_ERROR(ret < 0, "Error: Could not get crop from decoder capture plane", ret);

	//output resolution = resize target if requested, else stream resolution
	ctx->coded_width = crop.c.width;
	ctx->coded_height = crop.c.height;
	ctx->output_width = ctx->resized.width ? ctx->resized.width : crop.c.width;
	ctx->output_height = ctx->resized.height ? ctx->resized.height : crop.c.height;

	//init/reinit DecoderCapturePlane
#ifndef WITH_NVUTILS
	bool hadCapture = ctx->numberCaptureBuffers > 0;
#endif
	ctx->deinitDecoderCapturePlane();
	ctx->initDecoderCapturePlane(format);

	/* override default seesion. Without overriding session we wil
	   get seg. fault if decoding in forked process*/
#ifdef WITH_NVUTILS
	ctx->session.compute_mode = NvBufSurfTransformCompute_VIC;
	ctx->session.gpu_id = 0;
	ctx->session.cuda_stream = 0;
	NvBufSurfTransformSetSessionParams(&(ctx->session));
#else
	if (hadCapture)
		NvBufferSessionDestroy(ctx->session);
	ctx->session = NvBufferSessionCreate();
#endif

	/* Skip the rebuild only if the CURRENT generation already has the new
	 * output size (e.g. pre-allocated from container hints that turned out
	 * accurate): saves one alloc/free cycle and first-frame latency. The
	 * comparison must be against the generation's own size: output_width/
	 * height were just overwritten above, so comparing them with the same
	 * expression was always true and a mid-stream resolution change never
	 * rebuilt the pool (#137). A generation without buffers (allocation
	 * failed earlier) is rebuilt too. */
	if (ctx->gen && !ctx->gen->slots.empty() &&
	    ctx->gen->w == ctx->output_width && ctx->gen->h == ctx->output_height)
	{
		NVMPI_LOG(NVMPI_LOG_DEBUG, "frame pool already allocated at %ux%u, skipping rebuild",
		          ctx->output_width, ctx->output_height);
	}
	else
	{
		/* Retire the old generation without waiting: unborrowed buffers die
		 * now, borrowed ones when released (their cookies keep the
		 * generation alive), then allocate at the new size. */
		ctx->retireFramePool();
		ctx->initFramePool();
	}

	//reset buffer transformation params based on new resolution data
	ctx->updateBufferTransformParams();

	return;
}
