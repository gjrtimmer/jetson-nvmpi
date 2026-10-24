#include "nvmpi_dec_internal.h"
#include <pthread.h>
#include "nvmpi_timing.h"

/* Slice length of the pre-event dqEvent polls: bounds how long close()/flush()
 * wait for a capture thread that is still waiting for the first event.
 * NvV4l2Element::dqEvent counts its timeout in usleep(1000) iterations, not
 * in wall-clock time, so under CPU load a slice lasts several times its
 * nominal length (#165): keep it short. */
#define EVENT_POLL_MS 10

/* Race guard budget (wall-clock), see the guard in dec_capture_loop_fcn. */
#define RACE_GUARD_MS 500

/* Drain safety net (#161): after the caller's EOS the capture thread waits
 * for the driver's LAST buffer. If no capture buffer arrives for
 * DRAIN_IDLE_FACTOR x wait_timeout_ms (at least DRAIN_IDLE_MIN_MS), a driver
 * that never flags LAST must not hang the stream: the drain is declared done
 * and a WARN is logged. A healthy drain delivers its buffers within
 * milliseconds, so this only bounds the failure case. */
#define DRAIN_IDLE_FACTOR 4
#define DRAIN_IDLE_MIN_MS 1000

/* Defined in nvmpi_vic.cpp — serializes NvBufSurfTransform calls to
 * prevent driver deadlock between capture thread and filter thread. */
extern pthread_mutex_t nvmpi_transform_mutex;

//Body of the decoder capture thread (started by nvmpi_create_decoder).
//Responsibilities:
//  1. block until the first resolution-change event so buffer sizes are
//     known, then set up the CAPTURE plane and frame pool;
//  2. loop: dequeue decoded CAPTURE buffers, VIC-transform each into an
//     "empty" nvmpi_frame_buffer from the pool (format convert + optional
//     scale), stamp the pts, publish it as "filled", and re-queue the V4L2
//     buffer to the decoder;
//  3. handle mid-stream resolution changes and EOS/error shutdown.
//EOS (#161): the caller's zero-sized packet sets ctx->input_eos only. The
//loop then keeps dequeuing, because frames held inside the decoder for
//reordering are delivered after the marker, until the driver flags the last
//capture buffer (V4L2_BUF_FLAG_LAST) or the idle safety net fires.
//Exits when ctx->eos is set (drain done, fatal error, or close()/flush()).
void dec_capture_loop_fcn(void *arg)
{
	nvmpi_dec_ctx* ctx=(nvmpi_dec_ctx*)arg;
	NvVideoDecoder *dec = ctx->dec;

	struct v4l2_format v4l2Format;
	struct v4l2_crop v4l2Crop;
	struct v4l2_event v4l2Event;
	memset(&v4l2Event, 0, sizeof(v4l2Event));
	int ret;
	dec_slot* slot = NULL;
	bool got_resolution_change = false;

    NvmpiTimer tm("dec_capture");      /* NVMPI_TIMING=1 only (#105) */

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

    /* Initialize capture plane if and only if we received the event.
     * Keyed on got_resolution_change (not eos): even when EOS arrived
     * early, the capture plane MUST be set up so the V4L2 decoder has
     * valid destination buffers for any frames it has already decoded. */
    if (got_resolution_change) {
        respondToResolutionEvent(v4l2Format, v4l2Crop, ctx);
    } else if (!ctx->eos.load()) {
        /* Input ended without any event: there is no capture plane, so
         * nothing can be drained. Stop here instead of entering the loop
         * below with an unarmed plane. */
        NVMPI_LOG(NVMPI_LOG_INFO, "input EOS before the first resolution event: nothing to drain");
        ctx->eos.store(true);
    }

    /* Drain idle bound, see DRAIN_IDLE_FACTOR. last_progress = last time a
     * capture buffer was dequeued or handled; only consulted after input EOS. */
    int drain_idle_ms = DRAIN_IDLE_FACTOR * ctx->wait_timeout_ms;
    if (drain_idle_ms < DRAIN_IDLE_MIN_MS)
        drain_idle_ms = DRAIN_IDLE_MIN_MS;
    std::chrono::steady_clock::time_point last_progress = std::chrono::steady_clock::now();

	while (!(ctx->eos.load() || dec->isInError()))
	{
		NvBuffer *dec_buffer;

		// Check for Resolution change again.
		ret = dec->dqEvent(v4l2Event, false);
		if (ret == 0)
		{
			switch (v4l2Event.type)
			{
				case V4L2_EVENT_RESOLUTION_CHANGE:
					respondToResolutionEvent(v4l2Format, v4l2Crop, ctx);
					continue;
			}
		}

		/* Decoder capture loop */
		while(!ctx->eos.load())
		{
			struct v4l2_buffer v4l2_buf;
			struct v4l2_plane planes[MAX_PLANES];
			/* Zero both every iteration: the drain decisions below read
			 * flags and bytesused, and stale or uninitialised values from
			 * the previous buffer (or the stack) must never look like a
			 * LAST / empty buffer. */
			memset(&v4l2_buf, 0, sizeof(v4l2_buf));
			memset(planes, 0, sizeof(planes));
			v4l2_buf.m.planes = planes;

			/* Dequeue a filled buffer.
			 * 0 retries: non-blocking. On EAGAIN we sleep and retry.
			 * The sleep interval (10 ms) is 10× longer than the original
			 * 1 ms, reducing the capture thread's poll rate from ~1 kHz
			 * to ~100 Hz and cutting its CPU overhead proportionally.
			 * This fixes the monotonic CPU rise reported in GitHub #24
			 * and upstream Keylost#41.
			 *
			 * DevicePoll() (kernel-blocking wait) was considered but
			 * rejected: the Tegra V4L2 driver does not unblock DevicePoll
			 * after the final EOS frame is dequeued, causing an indefinite
			 * hang once all capture buffers are drained. The NVIDIA sample
			 * works around this with a dedicated poll thread + semaphore
			 * coordination, but that is too invasive for this fix. */
			if (dec->capture_plane.dqBuffer(v4l2_buf, &dec_buffer, NULL, 0))
			{
				const int dq_errno = errno;
				if (dq_errno == EAGAIN)
				{
					/* Empty queue. The driver sets LAST in the buffer
					 * after the final frame was dequeued post-EOS. */
					if (v4l2_buf.flags & V4L2_BUF_FLAG_LAST)
					{
						if (ctx->input_eos.load())
							NVMPI_LOG(NVMPI_LOG_INFO, "capture plane drained (LAST)");
						else
							ERROR_MSG("Got EoS at capture plane");
						ctx->eos.store(true);
					}
					else if (ctx->input_eos.load() &&
					         std::chrono::steady_clock::now() - last_progress >
					         std::chrono::milliseconds(drain_idle_ms))
					{
						/* Driver never flagged LAST: do not hang. */
						NVMPI_LOG(NVMPI_LOG_WARN,
						          "no capture buffer %d ms after input EOS, ending the drain",
						          drain_idle_ms);
						ctx->eos.store(true);
					}
					usleep(10000);
				}
				else if (dq_errno == EPIPE && ctx->input_eos.load())
				{
					/* EPIPE = stream ended (some drivers answer this
					 * instead of EAGAIN+LAST once drained): expected. */
					NVMPI_LOG(NVMPI_LOG_INFO, "capture plane drained (EPIPE)");
					ctx->eos.store(true);
				}
				else
				{
					ERROR_MSG("Error while calling dequeue at capture plane");
					ctx->eos.store(true);
				}
				break;
			}

			last_progress = std::chrono::steady_clock::now();
			const bool is_last = (v4l2_buf.flags & V4L2_BUF_FLAG_LAST) != 0;
			if (is_last && v4l2_buf.m.planes[0].bytesused == 0)
			{
				/* LAST on an empty buffer: end marker without a picture.
				 * Nothing to transform or emit; the drain is complete. Not
				 * re-queued: close()/flush() tear the capture plane down. */
				NVMPI_LOG(NVMPI_LOG_INFO, "capture plane drained (empty LAST buffer)");
				ctx->eos.store(true);
				break;
			}

			//point the buffer at the DMA fd it was queued with
			dec_buffer->planes[0].fd = ctx->dmaBufferFileDescriptor[v4l2_buf.index];

			//grab a free destination slot from the pool (non-blocking). The slot
			//stays owned by this thread (its generation reference lives in
			//slot->self) until it is queued as filled.
			slot = ctx->framePool->dqEmptyBuf();

			if(slot)
			{
				/* hw transform: block-linear decoder output → pitch-linear
			 * dst buffer, converting format and scaling as configured.
			 * Lock the global transform mutex to prevent concurrent
			 * NvBufSurfTransform calls from the filter thread (VIC/GPU
			 * deadlock — see nvmpi_vic.cpp for rationale). */
#ifdef WITH_NVUTILS
				pthread_mutex_lock(&nvmpi_transform_mutex);
				NvBufSurfTransformSetSessionParams(&(ctx->session));
				ret = NvBufSurfTransform(ctx->dmaBufferSurface[v4l2_buf.index], slot->fb->dst_dma_surface, &(ctx->transform_params));
				pthread_mutex_unlock(&nvmpi_transform_mutex);
#else
				pthread_mutex_lock(&nvmpi_transform_mutex);
				ret = NvBufferTransform(dec_buffer->planes[0].fd, slot->fb->dst_dma_fd, &(ctx->transform_params));
				pthread_mutex_unlock(&nvmpi_transform_mutex);
#endif
				TEST_ERROR(ret==-1, "Transform failed",ret);
				//carry the pts through (V4L2 timeval -> original int64 microseconds)
				slot->fb->timestamp = (unsigned long long)ctx->pts_map.decode(v4l2_buf.timestamp);

				//hand the filled frame to the consumer (user thread)
				ctx->framePool->qFilledBuf(slot);
			}
			else
			{
				/* Backpressure: the frame pool is exhausted because the
				 * consumer (user thread calling nvmpi_decoder_get_frame)
				 * hasn't returned a buffer yet. Block on the pool's
				 * empty-queue CV (100 ms timeout) instead of the previous
				 * 500 µs busy-spin — eliminates ~2000 wakeups/s while
				 * back-pressured. The V4L2 capture buffer is held back
				 * meanwhile, eventually stalling the decoder hardware.
				 *
				 * Events are serviced here too (#143). A single-threaded
				 * caller only drains frames between put_packet calls, and
				 * put_packet blocks until the decoder consumes an OUTPUT
				 * buffer. The decoder holds every AU of a new stream size
				 * until the capture plane is torn down and re-armed
				 * (respondToResolutionEvent), which only this thread can
				 * do. Waiting for a free slot while ignoring the pending
				 * RESOLUTION_CHANGE deadlocked: caller in put_packet ->
				 * decoder waiting for us -> us waiting for the caller to
				 * drain. */
				bool res_change_handled = false;
				while(!ctx->eos.load())
				{
					slot = ctx->framePool->dqEmptyBuf(std::chrono::milliseconds(100));
					if(!slot)
					{
						/* No slot within the slice: poll for the event
						 * (0 ms, non-blocking). Only this thread dequeues
						 * events, so each is consumed exactly once. Other
						 * event types are ignored, as in the outer loop. */
						if (dec->dqEvent(v4l2Event, false) == 0 &&
						    v4l2Event.type == V4L2_EVENT_RESOLUTION_CHANGE)
						{
							/* The held old-size CAPTURE buffer is dropped
							 * without a transform and NOT re-queued:
							 * respondToResolutionEvent does STREAMOFF +
							 * REQBUFS(0) on the capture plane, the single
							 * release point for it. Old-size frames still
							 * waiting in the pool are discarded by
							 * retireFramePool() in the same call anyway
							 * (#137), so this adds no new kind of frame
							 * loss. The decoder is re-armed afterwards,
							 * which frees OUTPUT buffers for put_packet. */
							NVMPI_LOG(NVMPI_LOG_INFO,
							          "resolution change while back-pressured: dropping held CAPTURE buffer %d",
							          v4l2_buf.index);
							respondToResolutionEvent(v4l2Format, v4l2Crop, ctx);
							res_change_handled = true;
							break;
						}
						continue;
					}
					if(slot)
					{
#ifdef WITH_NVUTILS
						pthread_mutex_lock(&nvmpi_transform_mutex);
						NvBufSurfTransformSetSessionParams(&(ctx->session));
						ret = NvBufSurfTransform(ctx->dmaBufferSurface[v4l2_buf.index], slot->fb->dst_dma_surface, &(ctx->transform_params));
						pthread_mutex_unlock(&nvmpi_transform_mutex);
#else
						pthread_mutex_lock(&nvmpi_transform_mutex);
						ret = NvBufferTransform(dec_buffer->planes[0].fd, slot->fb->dst_dma_fd, &(ctx->transform_params));
						pthread_mutex_unlock(&nvmpi_transform_mutex);
#endif
						TEST_ERROR(ret==-1, "Transform failed",ret);
						slot->fb->timestamp = (unsigned long long)ctx->pts_map.decode(v4l2_buf.timestamp);

						ctx->framePool->qFilledBuf(slot);
						break;
					}
				}
				/* Change handled: the held buffer no longer exists in the
				 * rebuilt capture plane, skip the re-queue below and go
				 * back to the outer loop for the new stream. */
				if (res_change_handled)
					break;
			}

			/* LAST buffer with a picture: it was emitted above and is the
			 * final one. Finish the drain here (no re-queue); the pool
			 * shutdown below wakes the consumer once it took the rest. */
			if (is_last)
			{
				NVMPI_LOG(NVMPI_LOG_INFO, "capture plane drained (LAST buffer)");
				ctx->eos.store(true);
				break;
			}

			//return the DMA buffer to the decoder so it can be refilled
			v4l2_buf.m.planes[0].m.fd = ctx->dmaBufferFileDescriptor[v4l2_buf.index];
			if (dec->capture_plane.qBuffer(v4l2_buf, NULL) < 0)
			{
				/* qBuffer failure permanently removes this V4L2 buffer
				 * from the driver's circulation queue.  After enough
				 * failures the capture loop starves (no buffers to dequeue).
				 * Treat as fatal — set EOS to unblock the close path. */
				NVMPI_LOG(NVMPI_LOG_ERROR,
				          "capture loop: qBuffer failed for buffer %d, aborting",
				          v4l2_buf.index);
				ctx->eos.store(true);
				break;
			}
			/* Restart the idle clock: time spent back-pressured above is
			 * the consumer's, not a silent driver. */
			last_progress = std::chrono::steady_clock::now();
		}
	}
	/* Unblock any consumer waiting in dqFilledBuf(timeout) — all exit
	 * paths converge here, so one shutdown() call covers every case. */
	ctx->framePool->shutdown();
	return;
}
