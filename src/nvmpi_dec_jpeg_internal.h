/*
 * nvmpi_dec_jpeg_internal.h — JPEG decoder context (NvJPEGDecoder-backed).
 *
 * Unlike the V4L2 video decoder (nvmpi_dec_internal.h), JPEG decoding is
 * synchronous per-frame via NvJPEGDecoder::decodeToFd. There is no capture
 * thread, no V4L2 M2M device, no resolution-change events.
 *
 * The decoded DMA fd from decodeToFd is block-linear; a VIC transform
 * converts it to a pitch-linear nvmpi_frame_buffer, which is then queued
 * to the frame pool for retrieval via nvmpi_jpeg_decoder_get_frame().
 *
 * Allocated by nvmpi_create_jpeg_decoder(); freed by nvmpi_jpeg_decoder_close().
 */
#pragma once

#include "nvmpi.h"
#include "NvJpegDecoder.h"
#include "nvUtils2NvBuf.h"
#include "NVMPI_bufPool.hpp"
#include "nvmpi_frame_buffer.hpp"
#include <vector>
#include "nvmpi_log.h"
#include <atomic>
#include <cstring>

/* Maximum dimension the NVJPG engine supports (baseline JPEG). */
#define NVJPEG_MAX_DIM 16384

/* Default frame pool size for JPEG decoder. Smaller than video because
 * JPEG decode is synchronous — frames are produced one at a time. */
#define JPEG_FRAME_POOL_SIZE 4

/* JPEG SOF markers for format detection. */
#define JPEG_MARKER_SOF0 0xC0  /* Baseline DCT (supported) */
#define JPEG_MARKER_SOF2 0xC2  /* Progressive DCT (NOT supported by HW) */

/* Reuse the error-reporting macro from the V4L2 decoder. */
#define JPEG_ERROR(condition, message)    \
	if (condition) {                     \
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "%s", message); \
	}

/* Zero-copy (DMA-BUF) output state, defined in nvmpi_dec_jpeg_fd.cpp. */
struct jpeg_fd_state;

/*
 * JPEG decoder context.
 *
 * Named nvmpictx_jpeg (not nvmpictx) to avoid linker-level ODR collisions
 * with the V4L2 decoder's nvmpictx — both TUs are linked into libnvmpi.so
 * and identically-named member functions would produce duplicate symbols.
 *
 * The C API in nvmpi.h declares an opaque 'nvmpictx*'. The JPEG API
 * functions cast between nvmpictx* and nvmpictx_jpeg* at the boundary.
 */
struct nvmpictx_jpeg
{
	/* Allocated by nvmpi_create_jpeg_decoder(); freed in close(). */
	NvJPEGDecoder *jpegdec{nullptr};

	/* Set on EOS (zero-size packet) or fatal error. */
	std::atomic<bool> eos{false};

	/* Last decoded dimensions — used to detect resolution changes that
	 * require frame pool reallocation. */
	uint32_t last_width{0};
	uint32_t last_height{0};

	/* Size the copy-out get_frame assumes the caller's buffers hold when it
	 * gets no payload_size: the first image it delivered (#149). Touched by
	 * the get_frame thread only. 0 = not recorded yet. */
	uint32_t base_width{0};
	uint32_t base_height{0};

	/* Output pixel format — JPEG always produces YUV420 via VIC. */
	nvPixFormat out_pixfmt{NV_PIX_YUV420};

	/* Non-NULL in DMA-BUF mode (nvmpi_create_jpeg_decoder_fmt): the copy
	 * pool below is then unused. Allocated by the create function; freed
	 * by jpegFdClose() from nvmpi_jpeg_decoder_close(). */
	jpeg_fd_state *fd{nullptr};

	/* Frame pool: producer = put_packet (synchronous), consumer = get_frame. */
	NVMPI_bufPool<nvmpi_frame_buffer*>* framePool{nullptr};
	std::vector<nvmpi_frame_buffer*> allocatedFrameBufs;
	int frame_pool_size{JPEG_FRAME_POOL_SIZE};

	/* Cached plane geometry of the destination DMA buffers (filled by
	 * updateFrameSizeParams after pool allocation). Drives the memcpy
	 * in get_frame. */
	unsigned int num_planes{0};
	unsigned int frame_linesize[3];    /* pitch of each plane */
	unsigned int frame_height[3];      /* number of lines per plane */
	unsigned int frame_linedatasize[3]; /* valid data bytes per line */

	/* VIC transform parameters. */
#ifdef WITH_NVUTILS
	NvBufSurfTransformConfigParams session;
#else
	NvBufferSession session;
#endif
	NvBufferTransformParams transform_params;
	NvBufferRect src_rect, dest_rect;

	/* Allocate frame_pool_size pitch-linear DMA buffers at width×height
	 * and seed the pool's empty queue. */
	void initFramePool(uint32_t width, uint32_t height);

	/* Destroy all frame buffers and drain both pool queues. */
	void deinitFramePool();

	/* Read actual plane geometry from one allocated buffer. */
	void updateFrameSizeParams();

	/* Set up VIC transform rects for width×height pass-through. */
	void updateTransformParams(uint32_t width, uint32_t height);
};

/* Copy decoded pixels from a DMA buffer to the caller's nvFrame.
 * Same pattern as the V4L2 decoder's copyNvBufToFrame().
 * Defined in nvmpi_dec_jpeg_ctx.cpp; used by nvmpi_jpeg_decoder_get_frame()
 * in nvmpi_dec_jpeg.cpp. Not static because it is called from another TU. */
int jpegCopyToFrame(nvmpictx_jpeg* ctx, nvmpi_frame_buffer *buf, nvFrame* frame);

/* Scan a JPEG bitstream for SOF markers to detect progressive format.
 * Returns true if a progressive marker (SOF2, 0xFFC2) is found.
 * Defined in nvmpi_dec_jpeg_ctx.cpp; used by nvmpi_jpeg_decoder_put_packet()
 * in nvmpi_dec_jpeg.cpp. Not static because it is called from another TU. */
bool jpegIsProgressive(const unsigned char *data, unsigned long size);

/* DMA-BUF mode put_packet body (EOS and null checks already done by the
 * caller). Returns 0, -1 on error or NVMPI_ERR_EAGAIN. Defined in
 * nvmpi_dec_jpeg_fd.cpp. */
int jpegFdPutPacket(nvmpictx_jpeg *ctx, nvPacket *packet);

/* Release the context's references to the DMA-BUF pool and free ctx->fd.
 * Borrowed frames keep their pool generation alive. Defined in
 * nvmpi_dec_jpeg_fd.cpp. */
void jpegFdClose(nvmpictx_jpeg *ctx);

/* On success *width and *height receive the displayed size from the SOF header.
 * True if the JPEG is 3-component baseline/extended-sequential (SOF0/SOF1);
 * logs and returns false for progressive, grayscale, CMYK or malformed
 * input. Bounds-checked (untrusted bytes). Defined in nvmpi_dec_jpeg_sof.cpp. */
bool jpegFdCheckSof(const unsigned char *d, unsigned long size,
                    unsigned *width, unsigned *height);

/* Plane geometry of one pool buffer into *fr (pitch/offset from the
 * allocator's planeParams, validated). NvUtils builds only. Defined in
 * nvmpi_dec_jpeg_fd_util.cpp. */
#ifdef WITH_NVUTILS
bool jpegFdFillGeometry(nvPixFormat fmt, nvmpi_frame_buffer *fb,
                        uint32_t w, uint32_t h, nvmpi_fd_frame *fr);
#endif
