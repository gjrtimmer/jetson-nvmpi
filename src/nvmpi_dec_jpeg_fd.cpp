/*
 * nvmpi_dec_jpeg_fd.cpp — JPEG decoder, zero-copy DMA-BUF output mode.
 *
 * Flow per packet (one thread calls put_packet/get_frame_fd):
 *   take an empty pool buffer -> decodeToFd (libnvjpeg block-linear output,
 *   owned by libnvjpeg and REUSED by its next decode, so never handed out)
 *   -> NvBufSurfTransform (VIC) into the pool buffer -> queue as filled.
 *
 * Lifetime: the pool is a "generation" (jpeg_pool_gen) held by shared_ptr.
 * The context, every queued frame and every borrowed frame own one reference.
 * Resolution change or close only drops the context's reference; the surfaces
 * are destroyed by the generation destructor when the last reference goes,
 * i.e. possibly inside a release callback on a foreign thread, possibly long
 * after close. There is no timeout and no check-then-requeue race.
 *
 * Locks (never held together, never held across a driver call except
 * nvmpi_transform_mutex around NvBufSurfTransform):
 *   jpeg_pool_gen::mu       guards `empty` and `current`.
 *   jpeg_fd_state::fmu      guards the filled queue and `closed`.
 *
 * Needs the NvUtils/NvBufSurface API; legacy nvbuf_utils builds reject
 * creation (NULL) and report the mode as unavailable.
 */
#include "nvmpi_dec_jpeg_internal.h"
#include "nvmpi_alloc.h"
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <new>

#define JPEG_FD_DEFAULT_POOL 8
#define JPEG_FD_MAX_POOL 64

/* Defined in nvmpi_vic.cpp — serializes NvBufSurfTransform calls. */
extern pthread_mutex_t nvmpi_transform_mutex;

#ifdef WITH_NVUTILS

/* One pool generation: buffers of a single size. */
struct jpeg_pool_gen {
	std::mutex mu;                           /* guards empty, current */
	std::vector<nvmpi_frame_buffer*> bufs;   /* fixed after allocation */
	std::vector<int> empty;                  /* indices free for decode */
	bool current{true};                      /* false once superseded/closed */
	~jpeg_pool_gen();
};

/* Last reference gone: nobody can touch the buffers any more. Single point
 * where pool surfaces are destroyed. */
jpeg_pool_gen::~jpeg_pool_gen()
{
	for (size_t i = 0; i < bufs.size(); i++) {
		if (bufs[i]) {
			bufs[i]->destroy();   /* NvBufSurf::NvDestroy, fd = -1 */
			delete bufs[i];
			bufs[i] = nullptr;
		}
	}
	bufs.clear();
}

/* A decoded frame waiting for get_frame_fd. Holds a generation reference. */
struct jpeg_fd_filled {
	std::shared_ptr<jpeg_pool_gen> gen;
	int idx;
	nvmpi_fd_frame fr;
};

struct jpeg_fd_state {
	nvPixFormat fmt{NV_PIX_NV12};
	int pool_size{JPEG_FD_DEFAULT_POOL};
	/* The context's own reference; touched only by the API thread. */
	std::shared_ptr<jpeg_pool_gen> gen;
	uint32_t width{0}, height{0};
	std::mutex fmu;                          /* guards filled, closed */
	std::condition_variable fcv;
	std::deque<jpeg_fd_filled> filled;
	bool closed{false};
};

/* Heap cookie of one borrow (the release `opaque`). Freed by the release. */
struct jpeg_fd_borrow {
	std::shared_ptr<jpeg_pool_gen> gen;
	int idx;
};

static bool popEmpty(const std::shared_ptr<jpeg_pool_gen> &g, int *idx)
{
	std::lock_guard<std::mutex> l(g->mu);
	if (g->empty.empty())
		return false;
	*idx = g->empty.back();
	g->empty.pop_back();
	return true;
}

/* Return a buffer to the free list only while the generation is current;
 * a superseded generation is not reused, its buffers die with it. */
static void pushEmpty(const std::shared_ptr<jpeg_pool_gen> &g, int idx)
{
	std::lock_guard<std::mutex> l(g->mu);
	if (g->current)
		g->empty.push_back(idx);
}

static void retireGen(std::shared_ptr<jpeg_pool_gen> &g)
{
	if (!g)
		return;
	{
		std::lock_guard<std::mutex> l(g->mu);
		g->current = false;
	}
	g.reset();   /* maybe last ref -> destroys surfaces, outside the lock */
}

/* Release callback handed to the caller; any thread, before or after close. */
static void jpegFdRelease(void *opaque)
{
	jpeg_fd_borrow *b = static_cast<jpeg_fd_borrow*>(opaque);
	if (!b)
		return;
	std::shared_ptr<jpeg_pool_gen> g = std::move(b->gen);
	int idx = b->idx;
	delete b;   /* allocated in get_frame_fd; single free point */
	if (g)
		pushEmpty(g, idx);
	/* g goes out of scope here, outside any lock: possibly the last ref. */
}

/* Allocate a pool generation of ctx->fd->pool_size NV12/YUV420 pitch-linear
 * buffers. Surface pixels are fully written by VIC before first exposure, so
 * no zero-fill pass is done (mapping DMA memory just to memset it would
 * only cost time). Returns NULL on any failure with nothing leaked. */
static std::shared_ptr<jpeg_pool_gen> allocGen(jpeg_fd_state *st, uint32_t w, uint32_t h)
{
	std::shared_ptr<jpeg_pool_gen> g(new (std::nothrow) jpeg_pool_gen());
	if (!g)
		return g;

	NvBufferCreateParams p;
	memset(&p, 0, sizeof(p));
	p.width = w;
	p.height = h;
	p.layout = NvBufferLayout_Pitch;
	/* JPEG is full range: use the _ER formats so VIC does not squeeze it. */
	p.colorFormat = (st->fmt == NV_PIX_NV12) ? NVBUF_COLOR_FORMAT_NV12_ER
	                                         : NVBUF_COLOR_FORMAT_YUV420_ER;
	p.memType = NVBUF_MEM_SURFACE_ARRAY;
	p.memtag = NvBufSurfaceTag_VIDEO_CONVERT;

	for (int i = 0; i < st->pool_size; i++) {
		/* Allocated here; freed by ~jpeg_pool_gen(). */
		nvmpi_frame_buffer *fb = new (std::nothrow) nvmpi_frame_buffer();
		if (!fb || !fb->alloc(p)) {
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "fd pool: buffer %d/%d alloc failed", i, st->pool_size);
			delete fb;
			return std::shared_ptr<jpeg_pool_gen>();   /* ~gen frees earlier ones */
		}
		g->bufs.push_back(fb);
		/* The allocator does not set close-on-exec; do it so an exec'd
		 * child does not inherit the pool. */
		if (fcntl(fb->dst_dma_fd, F_SETFD, FD_CLOEXEC) != 0) {
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "fd pool: FD_CLOEXEC failed");
			return std::shared_ptr<jpeg_pool_gen>();
		}
		g->empty.push_back(i);
	}
	return g;
}

int jpegFdPutPacket(nvmpictx_jpeg *ctx, nvPacket *packet)
{
	jpeg_fd_state *st = ctx->fd;
	int idx = -1;
	bool have = false;

	/* Displayed picture size from the SOF header: decodeToFd reports the
	 * decoder's (possibly aligned/padded) size, which must not leak into
	 * nvmpi_fd_frame.width/height. */
	unsigned sofw = 0, sofh = 0;
	if (!jpegFdCheckSof(packet->payload, packet->payload_size, &sofw, &sofh))
		return -1;
	if (sofw == 0 || sofh == 0 || sofw > NVJPEG_MAX_DIM || sofh > NVJPEG_MAX_DIM) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "JPEG dimensions %ux%u out of range", sofw, sofh);
		return -1;
	}

	/* Reserve the output buffer BEFORE decoding so EAGAIN never costs a
	 * decode. The very first frame has no pool yet (size unknown). */
	if (st->gen) {
		have = popEmpty(st->gen, &idx);
		if (!have)
			return NVMPI_ERR_EAGAIN;
	}

	int decode_fd = -1;
	uint32_t pixfmt = 0, w = 0, h = 0;
	/* decode_fd belongs to libnvjpeg and is overwritten by its next decode:
	 * used only as VIC source here, never exposed, never destroyed. */
	int ret = ctx->jpegdec->decodeToFd(decode_fd, packet->payload,
	                                   packet->payload_size, pixfmt, w, h);
	if (ret != 0) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "decodeToFd failed");
		goto fail;
	}
	if (w == 0 || h == 0 || w > NVJPEG_MAX_DIM || h > NVJPEG_MAX_DIM) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "decoded dimensions %ux%u out of range", w, h);
		goto fail;
	}
	/* The decoder output must cover the displayed size; it may be larger
	 * (padding). From here on w/h are the DISPLAYED size: pool surfaces,
	 * VIC crop rect and the reported frame size. */
	if (w < sofw || h < sofh) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "decoded size %ux%u smaller than SOF %ux%u", w, h, sofw, sofh);
		goto fail;
	}
	w = sofw;
	h = sofh;

	if (!st->gen || w != st->width || h != st->height) {
		/* New generation; the reserved buffer (if any) goes back first so
		 * the old generation is consistent for its borrowers. */
		if (have)
			pushEmpty(st->gen, idx);
		have = false;
		retireGen(st->gen);
		st->gen = allocGen(st, w, h);
		if (!st->gen)
			return -1;
		st->width = w;
		st->height = h;
		have = popEmpty(st->gen, &idx);
		if (!have)
			return -1;
	}

	{
		nvmpi_frame_buffer *fb = st->gen->bufs[idx];
		NvBufSurface *src = NULL;
		/* The libnvjpeg fd is registered, so FromFd works on it. */
		if (NvBufSurfaceFromFd(decode_fd, (void**)&src) < 0 || !src) {
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "NvBufSurfaceFromFd failed");
			goto fail;
		}
		NvBufSurfTransformRect sr, dr;
		memset(&sr, 0, sizeof(sr));
		sr.width = w; sr.height = h;
		dr = sr;
		NvBufSurfTransformParams tp;
		memset(&tp, 0, sizeof(tp));
		/* CROP flags: crop the padded decode to w x h, don't scale it */
		tp.transform_flag = NVBUFSURF_TRANSFORM_FILTER | NVBUFSURF_TRANSFORM_CROP_SRC | NVBUFSURF_TRANSFORM_CROP_DST;
		tp.transform_filter = NvBufSurfTransformInter_Nearest;
		tp.src_rect = &sr;
		tp.dst_rect = &dr;
		NvBufSurfTransformConfigParams sess;
		memset(&sess, 0, sizeof(sess));

		/* Global lock: concurrent NvBufSurfTransform calls deadlock the
		 * Tegra driver (see nvmpi_vic.cpp). Held for the transform only. */
		pthread_mutex_lock(&nvmpi_transform_mutex);
		NvBufSurfTransformSetSessionParams(&sess);
		ret = NvBufSurfTransform(src, fb->dst_dma_surface, &tp);
		pthread_mutex_unlock(&nvmpi_transform_mutex);
		if (ret != 0) {
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "VIC transform failed");
			goto fail;
		}

		jpeg_fd_filled f;
		f.idx = idx;
		if (!jpegFdFillGeometry(st->fmt, fb, w, h, &f.fr))
			goto fail;
		f.fr.timestamp = (int64_t)packet->pts;
		f.gen = st->gen;   /* the queued frame owns a reference */
		{
			std::lock_guard<std::mutex> l(st->fmu);
			st->filled.push_back(f);
		}
		st->fcv.notify_one();
	}
	return 0;

fail:
	if (have)
		pushEmpty(st->gen, idx);
	return -1;
}

int nvmpi_jpeg_decoder_get_frame_fd(nvmpictx* handle, nvmpi_fd_frame* frame,
	nvmpi_frame_release_cb *release, void **opaque, bool wait)
{
	if (!handle || !frame || !release || !opaque ||
	    frame->struct_size < NVMPI_FD_FRAME_V1_SIZE)
		return NVMPI_ERR_INVAL;
	nvmpictx_jpeg *ctx = reinterpret_cast<nvmpictx_jpeg*>(handle);
	jpeg_fd_state *st = ctx->fd;
	if (!st)
		return NVMPI_ERR_INVAL;

	jpeg_fd_filled f;
	{
		std::unique_lock<std::mutex> l(st->fmu);
		if (wait && st->filled.empty() && !st->closed)
			st->fcv.wait_for(l, std::chrono::milliseconds(500));
		if (st->filled.empty())
			return -1;
		f = st->filled.front();
		st->filled.pop_front();
	}

	/* Allocated here; freed by jpegFdRelease(). */
	jpeg_fd_borrow *b = new (std::nothrow) jpeg_fd_borrow();
	if (!b) {
		pushEmpty(f.gen, f.idx);
		return -1;
	}
	b->gen = std::move(f.gen);
	b->idx = f.idx;

	size_t n = frame->struct_size < sizeof(f.fr) ? frame->struct_size : sizeof(f.fr);
	f.fr.struct_size = (uint32_t)n;
	memcpy(frame, &f.fr, n);
	*release = jpegFdRelease;
	*opaque = b;
	return 0;
}

void jpegFdClose(nvmpictx_jpeg *ctx)
{
	jpeg_fd_state *st = ctx->fd;
	if (!st)
		return;
	std::deque<jpeg_fd_filled> drop;
	{
		std::lock_guard<std::mutex> l(st->fmu);
		st->closed = true;
		drop.swap(st->filled);
	}
	st->fcv.notify_all();
	drop.clear();            /* queued frames drop their references */
	retireGen(st->gen);      /* context's reference; borrows keep theirs */
	delete st;               /* allocated in create_jpeg_decoder_fmt */
	ctx->fd = nullptr;
}

nvmpictx* nvmpi_create_jpeg_decoder_fmt(nvPixFormat out, int pool_size)
{
	if (out != NV_PIX_NV12 && out != NV_PIX_YUV420)
		return NULL;
	if (pool_size == 0)
		pool_size = JPEG_FD_DEFAULT_POOL;
	if (pool_size < 1 || pool_size > JPEG_FD_MAX_POOL)
		return NULL;

	/* Allocated here; freed by nvmpi_jpeg_decoder_close(). */
	nvmpictx_jpeg *ctx = new (std::nothrow) nvmpictx_jpeg();
	if (!ctx)
		return NULL;
	ctx->out_pixfmt = out;
	ctx->fd = new (std::nothrow) jpeg_fd_state();
	/* Allocated here; freed by nvmpi_jpeg_decoder_close(). */
	ctx->jpegdec = ctx->fd ? NvJPEGDecoder::createJPEGDecoder("jpegdec") : nullptr;
	if (!ctx->jpegdec) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "NvJPEGDecoder creation failed");
		delete ctx->fd;
		delete ctx;
		return NULL;
	}
	ctx->fd->fmt = out;
	ctx->fd->pool_size = pool_size;
	return reinterpret_cast<nvmpictx*>(ctx);
}

#else /* !WITH_NVUTILS */

struct jpeg_fd_state {};

int jpegFdPutPacket(nvmpictx_jpeg *, nvPacket *) { return -1; }
void jpegFdClose(nvmpictx_jpeg *) {}

nvmpictx* nvmpi_create_jpeg_decoder_fmt(nvPixFormat, int)
{
	NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "DMA-BUF JPEG output needs the NvUtils buffer API");
	return NULL;
}

int nvmpi_jpeg_decoder_get_frame_fd(nvmpictx*, nvmpi_fd_frame*,
	nvmpi_frame_release_cb*, void**, bool)
{
	return NVMPI_ERR_INVAL;
}

#endif
