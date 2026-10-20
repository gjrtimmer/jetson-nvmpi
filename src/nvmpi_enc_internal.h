#pragma once

#include "nvmpi_ctx_base.h"
#include "NvVideoEncoder.h"
#include "nvUtils2NvBuf.h"
#include "NVMPI_bufPool.hpp"
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <malloc.h>
#include <vector>
#include "nvmpi_log.h"
#include <thread>
#include <unistd.h>
#include <memory>
#include <atomic>
#include <chrono>

#define MAX_BUFFERS 32
//Error-check macro for encoder init: logs to stderr and jumps to the
//function's cleanup label on failure. Every call site must be inside a
//function that has a `cleanup:` label and an `int ret` in scope.
#define TEST_ERROR(condition, message, errorCode)         \
	if (condition) {                                     \
		NVMPI_LOG(NVMPI_LOG_ERROR, "%s (code=%d)",       \
		          message, (int)(errorCode));             \
		goto cleanup;                                    \
	}

//Compile-time choice of OUTPUT-plane (raw input) memory type:
//MMAP = driver-allocated buffers mapped into userspace (current default),
//DMA  = self-allocated NvBufSurface dmabufs (alternate path, NvUtils only).
#define OUTPLANE_MEMTYPE_MMAP 0
#define OUTPLANE_MEMTYPE_DMA 1
#define OUTPLANE_MEMTYPE OUTPLANE_MEMTYPE_MMAP

using namespace std;

//Encoder context behind the opaque nvmpictx* handle of the public API
//(a different struct than the decoder's nvmpi_dec_ctx in nvmpi_dec_internal.h — the
//two are never mixed). Most fields mirror nvEncParam after translation to
//V4L2 enums; shared between the user thread and the capture DQ thread.
struct nvmpi_enc_ctx : nvmpictx
{
	uint32_t index;                //next OUTPUT-plane buffer index until all used once
	uint32_t width;
	uint32_t height;
	uint32_t profile;              //V4L2 H.264/H.265 profile enum value
	uint32_t bitrate;              //target bitrate (bit/s)
	uint32_t peak_bitrate;         //VBR peak bitrate (bit/s)
	uint32_t raw_pixfmt;           //V4L2 fourcc of the raw input frames
	uint32_t encoder_pixfmt;       //V4L2 fourcc of the compressed output (H264/H265)
	uint32_t iframe_interval;
	uint32_t idr_interval;
	uint32_t fps_n;
	uint32_t fps_d;
	uint32_t qmax;
	uint32_t qmin;
	uint32_t num_b_frames;
	uint32_t num_reference_frames;
	uint32_t vbv_buffer_size; //virtual buffer size of the encoder
	uint32_t packets_num;          //V4L2 buffer count per plane (param->capture_num)

	bool insert_sps_pps_at_idr;
	bool insert_vui;               //embed VUI timing_info (fps) in the bitstream
	bool insert_aud;               //insert Access Unit Delimiter NALs
	bool enable_cabac;             //enable CABAC entropy coding (H.264 only)
	bool max_perf;                 //enable max performance mode
	uint32_t poc_type;             //H.264 picture order count type (0=default, 2=low-latency)
	bool enable_extended_colorformat;
	bool enableLossless;           //constant QP 0 + High 4:4:4 profile (H.264)
	bool blocking_mode;            //true (default): put_frame blocks waiting for OUTPUT-plane
	                               //buffers. false: put_frame returns NVMPI_ERR_EAGAIN instead.
	bool dmabuf_external{false};   //true: OUTPUT plane uses V4L2_MEMORY_DMABUF from
	                               //external DMA-BUF fds (zero-copy encoder input via
	                               //nvmpi_encoder_put_frame_fd). Set at creation time
	                               //when nvEncParam.use_dmabuf is non-zero. When false
	                               //(default), OUTPUT plane uses MMAP and put_frame copies.
	/* Set to true in nvmpi_create_encoder() after startDQThread succeeds;
	 * checked in nvmpi_encoder_close() to avoid calling stopDQThread on
	 * an un-started thread (pthread_join on uninitialized thread = UB).
	 * Also used by the cleanup path when nvmpi_create_encoder fails
	 * after STREAMON but before the DQ thread is started. */
	bool dq_thread_started{false};

	/* Cross-thread flags — atomic to avoid data races between the user
	 * thread (put_frame/get_packet) and the capture-plane DQ callback.
	 * capPlaneGotEOS: written by DQ callback (nvmpi_enc_output.cpp),
	 *   read by user thread in nvmpi_encoder_get_packet().
	 * flushing: written by user thread in nvmpi_encoder_put_frame(),
	 *   read by user thread in get_packet; combined with capPlaneGotEOS
	 *   so both must use acquire/release ordering for coherence. */
	std::atomic<bool> capPlaneGotEOS{false};
	std::atomic<bool> flushing{false};

	/* Teardown flag (issue #122). Set by the user thread BEFORE it
	 * STREAMOFFs the CAPTURE plane in close/flush, read by the DQ callback.
	 * The vendored NvV4l2ElementPlane DQ thread hands the callback a NULL
	 * buffer when its blocking dqBuffer() fails; STREAMOFF makes that call
	 * fail (EINVAL), which is the expected way out of a teardown and not an
	 * error. The callback uses this flag to tell the two apart. Cleared
	 * only by nvmpi_encoder_flush() before it restarts the DQ thread. */
	std::atomic<bool> dq_stopping{false};

	/* Encoder back-pressure (issue #125). aus_pending = frames accepted by
	 * put_frame whose AU the DQ callback has not yet consumed (published,
	 * or dropped on an error path). Incremented by the user thread just
	 * before the OUTPUT buffer is queued; decremented by the DQ callback
	 * after releaseHold(). Signed: it is only a counter, never an index.
	 * Non-blocking put_frame admits a frame only while
	 *   aus_pending < pktPool->emptyAvailable()
	 * so every in-flight AU owns a packet and the callback never finds the
	 * pool empty. Reset by nvmpi_encoder_flush() (STREAMOFF discards the
	 * frames in flight). aus_dropped counts last-resort drops (contract
	 * violation by the caller); it must stay 0. */
	std::atomic<int> aus_pending{0};
	std::atomic<unsigned> aus_dropped{0};

	/* Blocking-wait ceiling for nvmpi_encoder_get_packet() when wait=true.
	 * Mirrors the decoder's wait_timeout_ms (default 500ms). */
	unsigned int wait_timeout_ms{500};

	enum v4l2_mpeg_video_bitrate_mode ratecontrol; //CBR or VBR
	/* V4L2 level enum — uint32_t so it can hold either
	 * v4l2_mpeg_video_h264_level or v4l2_mpeg_video_hevc_level. */
	uint32_t level;
	enum v4l2_enc_hw_preset_type hw_preset_type;   //speed/quality preset

	/* NVIDIA V4L2 encoder device wrapper. unique_ptr guarantees cleanup
	 * on all exit paths, preventing leaks from partial init failures.
	 * Allocated in nvmpi_create_encoder(); released in nvmpi_encoder_close()
	 * via reset() after the DQ thread is joined. */
	std::unique_ptr<NvVideoEncoder> enc;
	//producer/consumer pool of caller-allocated packets: DQ thread fills,
	//user thread consumes and recycles
	NVMPI_bufPool<nvPacket*>* pktPool;
	int *output_plane_fd; //array to store dmabuf fd's
};

//Non-blocking admission test, user thread only. True when one more frame
//may be queued without risking an AU that finds no packet.
//Order matters: aus_pending is read BEFORE the pool capacity. The DQ
//callback publishes, releases the hold, and only then decrements
//aus_pending, so a reader that sees the new (lower) pending also sees the
//new capacity; the reverse read order could pair a stale-high capacity with
//a fresh-low pending and over-admit. The races left only under-admit (a
//spurious EAGAIN the caller retries after draining).
static inline bool enc_can_admit(nvmpi_enc_ctx* ctx)
{
	int pending = ctx->aus_pending.load(std::memory_order_seq_cst);
	size_t avail = ctx->pktPool->emptyAvailable();
	return pending < 0 || (size_t)pending < avail;
}

//Blocking get_packet wait for the not-flushing case (issue #103). Waits up
//to wait_timeout_ms for a packet, but only while an AU is still in flight:
//with nothing owed it returns NULL at once instead of burning the whole
//timeout (a low-delay caller polls after every delivered AU).
//Ordering: aus_pending is read BEFORE the filled queue is polled. The DQ
//callback does qFilledBuf -> releaseHold -> aus_pending--, so a reader that
//sees pending <= 0 is guaranteed that every published packet is already in
//the queue, and the poll that follows cannot miss one. The reverse order
//(poll, then load) could poll empty, let the callback publish and
//decrement, then see 0 and report "nothing in flight" with a packet queued.
//Slice: the callback's oversize/empty-pool drop paths decrement
//aus_pending without notifying cv_filledBuf, so a long cv wait could sleep
//through "nothing left"; waiting in <=5 ms slices re-reads the counter.
//Runs on the user thread only; takes no lock of its own (the pool locks
//m_filledBuf internally, never while this reads the atomic).
static inline nvPacket* enc_wait_packet(nvmpi_enc_ctx* ctx)
{
	using Clock = std::chrono::steady_clock;
	const auto slice = std::chrono::milliseconds(5);
	const auto deadline = Clock::now() +
		std::chrono::milliseconds(ctx->wait_timeout_ms);

	for(;;)
	{
		int pending = ctx->aus_pending.load(std::memory_order_seq_cst);
		nvPacket* pkt = ctx->pktPool->dqFilledBuf();
		if(pkt) return pkt;
		auto now = Clock::now();
		if(pending <= 0 || ctx->flushing.load(std::memory_order_acquire) ||
		   ctx->pktPool->isShutdown() || now >= deadline)
			return NULL;
		auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
		pkt = ctx->pktPool->dqFilledBuf(remaining < slice ? remaining : slice);
		if(pkt) return pkt;
	}
}

//Forward declarations for functions defined across the split encoder files.
//encoder_capture_plane_dq_callback: defined in nvmpi_enc_output.cpp; registered
//with NvVideoEncoder's capture-plane DQ thread in nvmpi_create_encoder()
//(nvmpi_enc_init.cpp) and nvmpi_encoder_flush() (nvmpi_enc_api.cpp).
bool encoder_capture_plane_dq_callback(struct v4l2_buffer *v4l2_buf, NvBuffer *buffer, NvBuffer *shared_buffer, void *arg);
//copyFrameToNvBuf: defined in nvmpi_enc_input.cpp; copies a caller raw frame
//into a V4L2 OUTPUT-plane NvBuffer (used by nvmpi_encoder_put_frame()).
int copyFrameToNvBuf(nvFrame* frame, NvBuffer& buffer);
//nvmpi_enc_stop_dq_thread: defined in nvmpi_enc_api.cpp; stops and joins the
//capture DQ thread safely (see definition). Returns 0 ok, -1 join timed out.
int nvmpi_enc_stop_dq_thread(nvmpi_enc_ctx* ctx);
