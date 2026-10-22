#pragma once

#include "nvmpi_ctx_base.h"
#include "NvVideoDecoder.h"
#include "nvUtils2NvBuf.h"
#include "NVMPI_bufPool.hpp"
#include "nvmpi_frame_buffer.hpp"
#include "nvmpi_fd_geometry.h"
#include "nvmpi_pts.h"
#include <vector>
#include "nvmpi_log.h"
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>

//default max size (bytes) of one compressed input chunk on the OUTPUT
//plane; 4 MB proved too small for 4K high-bitrate I-frames. Overridable
//per-context via nvDecParam.chunk_size.
#define CHUNK_SIZE_DEFAULT 10000000
//upper bound for CAPTURE-plane DMA buffers (sizes the fd/surface arrays)
#define MAX_BUFFERS 32

//Error-check macro for decoder capture/plane paths: logs the error with the
//actual error code (previously the errorCode parameter was silently ignored).
//These void-returning capture-thread functions cannot propagate errors to the
//caller, so logging is the best available response. The two call sites in
//nvmpi_create_decoder (disableDPB, setMaxPerfMode) use inline error handling
//with proper return-on-failure instead.
#define TEST_ERROR(condition, message, errorCode)             \
	if (condition) {                                         \
		NVMPI_LOG(NVMPI_LOG_ERROR, "%s (code=%d)",           \
		          message, (int)(errorCode));                 \
	}

using namespace std;

struct dec_gen;

//One frame-pool entry: a destination DMA buffer plus its owner reference.
//Ownership invariant: whoever holds a slot holds a reference to its
//generation. While the slot is queued in the pool or owned by the capture
//thread that reference is `self`; while it is borrowed (get_frame_fd) it is
//moved into the borrow cookie. `self` is therefore a deliberate cycle
//(gen -> slots -> self -> gen) that retire breaks for every queued slot.
struct dec_slot {
	nvmpi_frame_buffer *fb{nullptr};    //destination buffer; owned by the generation
	int idx{0};                         //position in dec_gen::slots
	std::shared_ptr<dec_gen> self;      //see invariant above; empty while borrowed
};

//One pool generation: buffers of a single output size plus the geometry
//cache read by every consumer of those buffers. Held by shared_ptr (same
//model as the JPEG fd path, jpeg_pool_gen). A resolution change or close
//retires the generation; borrowed buffers are destroyed by ~dec_gen when the
//last borrow is released (any thread, possibly after close). No waiting.
//
//Lock order (never invert): dec_gen::mu -> one pool queue lock
//(m_emptyBuf or m_filledBuf, never both). Pool code never takes
//dec_gen::mu and the capture thread's hot path (dqEmptyBuf / qFilledBuf)
//does not either. nvmpi_transform_mutex is never held together with mu.
struct dec_gen {
	std::mutex mu;                      //guards `current` and the decision to requeue
	bool current{true};                 //false once retired; written under mu only
	//pool the slots circulate through. Valid ONLY while `current` (the
	//context deletes the pool after retiring, which flips `current` under mu).
	NVMPI_bufPool<dec_slot*> *pool{nullptr};
	std::vector<dec_slot> slots;        //reserved up front: slot addresses stay stable
	unsigned int w{0}, h{0};            //output size of this generation
	//geometry of the pitch-linear buffers (filled once, before any slot is
	//queued; read-only afterwards, so lock-free reads are safe)
	unsigned int num_planes{0};
	unsigned int linesize[MAX_NUM_PLANES]{};     //stride (pitch) of each plane in bytes
	unsigned int height[MAX_NUM_PLANES]{};       //number of lines in each plane
	unsigned int linedatasize[MAX_NUM_PLANES]{}; //usable data size for 1 line
	//per-plane layout for get_frame_fd2 (pitch/offset/size/num_planes/format/
	//width/height; fd and timestamp are per frame). Filled once from slot 0
	//with the cache above; valid only when fd_geom_ok.
	nvmpi_fd_frame fd_geom{};
	bool fd_geom_ok{false};
	~dec_gen();
};

//Give a slot back after the consumer is done with it. Takes over the
//generation reference held for the slot; requeues the slot as empty if the
//generation is still current, else just drops the reference (which frees the
//buffers if it was the last). Any thread, before or after close.
//Defined in nvmpi_dec_fd.cpp.
void dec_gen_release(std::shared_ptr<dec_gen> &&ref, dec_slot *slot);

//Decoder context behind the opaque nvmpictx* handle of the public API.
//Created by nvmpi_create_decoder(), owned by the caller via
//nvmpi_decoder_close(). Shared between the user thread and the internal
//capture thread; cross-thread handoff happens through framePool, and the
//'eos' flag is the (best-effort) shutdown signal.
struct nvmpi_dec_ctx : nvmpictx
{
	NvVideoDecoder *dec{nullptr};       //NVIDIA V4L2 decoder device wrapper
	std::atomic<bool> eos{false};       //set on EOS or fatal error; stops capture loop
	                                    //(atomic: read by user thread, written by capture thread)
	int index{0};                       //next OUTPUT-plane buffer index until all are used once
	/* Packets successfully queued to the OUTPUT plane since create (incl.
	 * the zero-sized EOS marker). Written by the user thread in
	 * put_packet AFTER qBuffer succeeds and BEFORE eos is set; read by the
	 * capture thread after it observed eos, so the release/acquire pair
	 * orders the counter before the eos flag. Never reset (flush keeps it):
	 * the capture thread's race guard keys on "was ever fed". */
	std::atomic<uint32_t> packets_put{0};
	nvmpi_pts_map pts_map;              //int64 pts <-> V4L2 timeval (put thread -> capture thread)
	unsigned int coded_width{0};        //stream resolution reported by the decoder (crop)
	unsigned int coded_height{0};
	unsigned int output_width{0};       //resolution delivered to the user (resized or coded)
	unsigned int output_height{0};
	nvSize resized{0, 0};               //requested hw downscale target; {0,0} = no resize
	unsigned int hint_width{0};         //container-reported stream width (0 = unknown)
	unsigned int hint_height{0};        //container-reported stream height (0 = unknown)

	int numberCaptureBuffers{0};        //actual CAPTURE-plane DMA buffer count (min + 5)

	//dmabuf fds of the CAPTURE-plane buffers the decoder writes into
	int dmaBufferFileDescriptor[MAX_BUFFERS];

#ifdef WITH_NVUTILS
	//NvBufSurface views of the fds above (needed by NvBufSurfTransform)
	NvBufSurface *dmaBufferSurface[MAX_BUFFERS];
	//VIC transform session config (compute device selection etc.)
	NvBufSurfTransformConfigParams session;
#else
	NvBufferSession session;
#endif
	//cached parameters for the per-frame VIC transform (crop rects, filter)
	NvBufferTransformParams transform_params;
	NvBufferRect src_rect, dest_rect;

	nvPixFormat out_pixfmt;             //user-requested output layout (NV12/YUV420)
	unsigned int decoder_pixfmt{0};     //V4L2 fourcc of the compressed input
	/* AV1 input gate (nvmpi_dec_av1.cpp, #147). User thread only: put_packet
	 * and flush are the same caller thread, so plain fields suffice. */
	bool is_av1{false};                 //codingType is AV1
	bool av1_seq_seen{false};           //a sequence header was queued; cleared by flush
	bool av1_drop_logged{false};        //INFO line printed for this stream start
	uint32_t av1_dropped{0};            //packets dropped before the first header
	std::thread dec_capture_loop;       //runs dec_capture_loop_fcn()

	int frame_pool_size{12};            //number of nvmpi_frame_buffer's to allocate
	uint32_t chunk_size{CHUNK_SIZE_DEFAULT}; //bytes per compressed-input OUTPUT-plane buffer
	bool max_perf{true};                //lift NVDEC clock governor for max throughput
	bool disable_dpb{false};            //skip DPB reordering (low-latency, B-frame-free only)
	int wait_timeout_ms{500};           //blocking dequeue ceiling (ms); set via AVOption
	//producer/consumer pool of generation slots: capture thread fills,
	//user thread consumes. Owned by the context; deleted in close AFTER the
	//generation is retired (a current generation points at it).
	NVMPI_bufPool<dec_slot*>* framePool{nullptr};
	/* The context's own reference to the CURRENT pool generation (see
	 * dec_gen above). Touched by one thread at a time only: create (before
	 * the capture thread starts), the capture thread (resolution change),
	 * close (after the join). Never read by the user/release threads, which
	 * reach their generation through the slot / borrow cookie. */
	std::shared_ptr<dec_gen> gen;

	/* Consumer-side state of the copy path (#149). Touched ONLY by the
	 * thread calling get_frame/get_frame2/get_frame_fd/get_frame_fd2 and, for
	 * the release, by flush/close (documented single-consumer rule in
	 * nvmpi.h): no lock. */
	/* A frame get_frame2 refused for lack of capacity and keeps for the next
	 * call. It is a slot owned by the consumer (not in either queue), so it
	 * still holds its generation reference in slot->self. Released by
	 * nvmpi_decoder_flush (discarded) and nvmpi_decoder_close. */
	dec_slot *pending{nullptr};
	/* Size the legacy get_frame (no payload_size) assumes the caller's
	 * buffers hold when neither a resize target nor a container size exists:
	 * the first generation it delivered. 0 = not recorded yet. */
	unsigned int base_w{0}, base_h{0};
	uint32_t size_drops{0};             //frames refused by the legacy get_frame (log rate limit)

	//retire the current generation (no-op without one): see nvmpi_dec_pool.cpp
	void retireFramePool();
	//allocate a new generation of frame_pool_size buffers at output_width x
	//output_height and queue its slots as empty
	void initFramePool();
	//refresh src/dst rects and filter settings for the per-frame transform
	void updateBufferTransformParams();

	//allocate CAPTURE-plane DMA buffers, REQBUFS/STREAMON, enqueue them all
	void initDecoderCapturePlane(v4l2_format &format);
	/* deinitPlane unmaps the buffers and calls REQBUFS with count 0 */
	void deinitDecoderCapturePlane();
};

//Forward declarations
//codingType -> V4L2 compressed fourcc; 0 (after logging) when unsupported
unsigned int nvmpi_dec_codec_pixfmt(nvCodingType codec);
NvBufferColorFormat getNvColorFormatFromV4l2Format(v4l2_format &format, bool want_10bit);
//true: drop this AV1 packet (no sequence header seen yet); see nvmpi_dec_av1.cpp
bool nvmpi_dec_av1_drop(nvmpi_dec_ctx* ctx, const nvPacket* packet);
void dec_capture_loop_fcn(void *arg);
void respondToResolutionEvent(v4l2_format &format, v4l2_crop &crop, nvmpi_dec_ctx* ctx);
int copyNvBufToFrame(const dec_gen* g, nvmpi_frame_buffer *nvmpiBuf, nvFrame* frame);
//Next frame for any consumer: the held (pending) one first, else the pool
//(wait = block up to wait_timeout_ms). nullptr when none. The slot is the
//caller's, with its generation reference in slot->self.
dec_slot* dec_next_slot(nvmpi_dec_ctx* ctx, bool wait);
//Give the held frame back (flush, close). Safe when none is held.
void dec_drop_pending(nvmpi_dec_ctx* ctx);
