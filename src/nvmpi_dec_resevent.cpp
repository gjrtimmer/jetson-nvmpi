/*
 * nvmpi_dec_resevent.cpp - capture-plane (re)arming on a V4L2
 * RESOLUTION_CHANGE event. Called only by the capture thread
 * (nvmpi_dec_capture.cpp), split out of it to keep both files small; also
 * the capture thread's wait for the first event (dec_wait_first_event).
 */
#include "nvmpi_dec_internal.h"
#include "nvmpi_timing.h"

/* Slice length of the pre-event dqEvent polls: bounds how long close()/flush()
 * wait for a capture thread that is still waiting for the first event.
 * NvV4l2Element::dqEvent counts its timeout in usleep(1000) iterations, not
 * in wall-clock time, so under CPU load a slice lasts several times its
 * nominal length (#165): keep it short. */
#define EVENT_POLL_MS 10

/* Race guard budget (wall-clock), see the guard in dec_capture_loop_fcn. */
#define RACE_GUARD_MS 500

//Capture thread start: wait for the first RESOLUTION_CHANGE event (see
//below), then give the race guard its extra budget. Returns true when the
//event arrived; false when eos / input EOS ended the wait first.
bool dec_wait_first_event(nvmpi_dec_ctx* ctx, NvmpiTimer& tm)
{
    NvVideoDecoder *dec = ctx->dec;
    struct v4l2_event v4l2Event;
    memset(&v4l2Event, 0, sizeof(v4l2Event));
    int ret;
    bool got_resolution_change = false;

    /* Need to wait for the first Resolution change event, so that
       the decoder knows the stream resolution and can allocate appropriate
       buffers when we call REQBUFS.

       Poll in short slices (EVENT_POLL_MS) instead of one long dqEvent:
       close()/flush() set eos and then join this thread, so the join waits
       for the slice in flight. With the former 500 ms slice a
       create->close without data took 0.5-1.5 s (#105). The loop still
       runs until the event arrives or eos is set; only the eos latency
       changes. */
    do
    {
        /* Refer ioctl VIDIOC_DQEVENT */
        ret = dec->dqEvent(v4l2Event, EVENT_POLL_MS);
        if (ret < 0)
        {
            if (errno == EAGAIN)
            {
                continue;
            }
            else
            {
               ERROR_MSG("Error in dequeueing decoder event");
               ctx->eos.store(true);
            }
        }
        if (v4l2Event.type == V4L2_EVENT_RESOLUTION_CHANGE)
            got_resolution_change = true;
    }
    while (!got_resolution_change && !ctx->eos.load() && !ctx->input_eos.load());
    tm.phase(got_resolution_change ? "pre_event_wait(event)" : "pre_event_wait(eos)");

    /* Race guard: after flush/seek, FFmpeg's threaded decoder model
     * (libavcodec >= 61 / FFmpeg 7.0+) may queue all remaining packets
     * including the zero-sized EOS marker before the V4L2 decoder has
     * emitted the resolution-change event triggered by re-primed
     * extradata. put_packet sets ctx->input_eos on the zero-sized marker,
     * causing the loop above to exit before the event arrives.
     *
     * Without a retry the capture plane stays uninitialized: no REQBUFS,
     * no STREAMON, no DMA buffers. The Tegra V4L2 driver then segfaults
     * when decoded frames have no destination — it does not gracefully
     * handle the "capture plane never set up" state.
     *
     * One additional 500 ms dqEvent covers the gap with margin; the
     * extradata-to-event latency is normally single-digit ms. For
     * nvmpi_decoder_close() the extra wait is within join tolerance.
     *
     * Origin: commit 55d5f7f (fix for the FFmpeg 7.0+ threaded model).
     * Only needed when data was fed: the decoder can emit the event only
     * after it parsed a queued packet. With packets_put == 0 (create ->
     * close, nothing ever queued) no event can be in flight, the capture
     * plane has no frames to receive, and the wait only delayed close()
     * by 500 ms (#105). packets_put is never reset by flush, so every
     * stream that was fed keeps the guard exactly as before. */
    if (!got_resolution_change && (ctx->eos.load() || ctx->input_eos.load()) &&
        ctx->packets_put.load(std::memory_order_acquire) > 0) {
        /* Wait in EVENT_POLL_MS slices against a wall-clock deadline and
         * leave as soon as ctx->eos is set. One dqEvent(500) cannot be
         * interrupted and its usleep(1000) loop stretches under CPU load: a
         * close() right after input EOS then waited ~5 s for the join (#165).
         * input_eos alone must not end the wait (that is what the guard is
         * for); only eos, set by close()/flush()/errors, does. */
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(RACE_GUARD_MS);
        while (!got_resolution_change && !ctx->eos.load() &&
               std::chrono::steady_clock::now() < deadline) {
            ret = dec->dqEvent(v4l2Event, EVENT_POLL_MS);
            if (ret == 0 && v4l2Event.type == V4L2_EVENT_RESOLUTION_CHANGE)
                got_resolution_change = true;
        }
        tm.phase("race_guard");
    }

    return got_resolution_change;
}

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
