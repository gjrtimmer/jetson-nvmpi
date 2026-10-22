/*
 * nvmpi.h — public C API of libnvmpi (layer 1 of jetson-ffmpeg).
 *
 * libnvmpi wraps NVIDIA's Jetson V4L2/NvBuffer multimedia API behind a small,
 * plain-C encode/decode interface. This header is the ONLY contract between
 * the two layers of the project:
 *   - implemented by src/nvmpi_dec_api.cpp (and its siblings nvmpi_dec_capture.cpp,
 *     nvmpi_dec_planes.cpp, nvmpi_dec_internal.h) and src/nvmpi_enc.cpp (built into
 *     libnvmpi.so, installed system-wide), and
 *   - consumed by the FFmpeg integration layer
 *     (ffmpeg/dev/common/libavcodec/nvmpi_{enc,dec}.c), which is patched
 *     into an FFmpeg source tree and compiled as part of FFmpeg.
 *
 * Usage pattern (both directions are simple put/get loops):
 *   decoder: nvmpi_create_decoder -> put_packet/get_frame ... -> close
 *   encoder: nvmpi_create_encoder -> put_frame/get_packet ... -> close
 */
#ifndef __NVMPI_H__
#define __NVMPI_H__
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

//Maximum size of the encoded buffers on the capture plane in bytes
//Also used by the FFmpeg wrapper as the allocation size of each pooled
//packet buffer, so a single encoded frame must never exceed this.
//10 MiB leaves headroom for 4K high-bitrate I-frames; frames larger than
//this are dropped with an error instead of overflowing the packet buffer.
#define NVMPI_ENC_CHUNK_SIZE 10*1024*1024

/* Encoder cannot take the frame now: no free OUTPUT-plane buffer
 * (non-blocking mode only), or every empty packet is already owed an access
 * unit (both modes, issue #145). The frame was NOT queued. Caller must drain get_packet (returning each packet with
 * qEmptyPacket) and retry the same frame. Matches Linux EAGAIN. */
#define NVMPI_ERR_EAGAIN (-11)

/* Invalid argument: NULL pointer, struct_size too small, or the handle was not
 * created by the API family the function belongs to. Matches Linux EINVAL. */
#define NVMPI_ERR_INVAL (-22)

/* The decoded frame does not fit the caller's buffers (nvmpi_decoder_get_frame2,
 * nvmpi_decoder_get_frame, nvmpi_jpeg_decoder_get_frame). Nothing was written
 * into the caller's planes. See nvmpi_decoder_get_frame2 for the recovery. */
#define NVMPI_ERR_FRAME_SIZE (-90)

//Opaque context handle. Note: the decoder and encoder each define their own
//(different) struct nvmpictx internally; a handle must only be passed back
//to the API family (decoder_* or encoder_*) that created it.
typedef struct nvmpictx nvmpictx;

//Raw (uncompressed) pixel layouts. Used for decoder output and encoder
//input. NV_PIX_P010 is 10-bit 4:2:0 semi-planar (matches FFmpeg P010LE:
//16-bit little-endian samples, 10 significant bits in the high bits, one
//Y plane plus one interleaved CbCr plane). Decode: HEVC Main10 only and
//requires the NvUtils buffer API. Encode: HEVC only (implies Main10; H.264
//and AV1 reject it at create time). AV1 10-bit was probed on L4T R36.4
//(Orin NX): create succeeds but the first frame fails in the driver
//(NvEncNvVicConvertSurfaceFormat failed, VENC BlockSide error 0x4).
typedef enum {
	NV_PIX_NV12,
	NV_PIX_YUV420,
	NV_PIX_P010
}nvPixFormat;

//Compressed bitstream codec selector. Decode supports all entries (AV1 only
//where the L4T headers define V4L2_PIX_FMT_AV1, i.e. JetPack 6; create returns
//NULL otherwise); encode supports H.264, HEVC and AV1 (AV1 encode: 8-bit 4:2:0 only, JetPack 6 headers; create returns
//NULL otherwise; one OBU temporal unit per packet, see https://github.com/gjrtimmer/jetson-ffmpeg/wiki/Development-Guide).
typedef enum {
	NV_VIDEO_CodingUnused,
	NV_VIDEO_CodingH264,             /**< H.264 */
	NV_VIDEO_CodingMPEG4,              /**< MPEG-4 */
	NV_VIDEO_CodingMPEG2,              /**< MPEG-2 */
	NV_VIDEO_CodingVP8,                /**< VP8 */
	NV_VIDEO_CodingVP9,                /**< VP9 */
	NV_VIDEO_CodingHEVC,               /**< H.265/HEVC */
	NV_VIDEO_CodingMJPEG,              /**< MJPEG (via NvJPEGDecoder, not V4L2) */
	NV_VIDEO_CodingAV1,                /**< AV1 (appended last to keep ABI) */
} nvCodingType;

//Simple width/height pair (used for the optional decoder resize target).
typedef struct _NVSIZE{
	unsigned int width;
	unsigned int height;
}nvSize;

//nvEncParam.profile values for HEVC (FFmpeg FF_PROFILE_HEVC_MAIN / _MAIN_10).
//Any other value selects Main. Not an ABI change: the field keeps its type.
#define NVMPI_HEVC_PROFILE_MAIN   1
#define NVMPI_HEVC_PROFILE_MAIN10 2

//Encoder creation parameters, filled in by the caller (the FFmpeg wrapper
//maps AVCodecContext fields / AVOptions onto these) and consumed once by
//nvmpi_create_encoder(). Numeric profile/level values follow FFmpeg's
//FF_PROFILE_H264_*/level_idc conventions and are translated to the
//corresponding V4L2 enums inside the encoder.
typedef struct _NVENCPARAM{
	unsigned int width;
	unsigned int height;
	unsigned int profile;          //H.264: FFmpeg-style profile id (66/77/100). HEVC:
	                               //NVMPI_HEVC_PROFILE_MAIN (1, default) or _MAIN10 (2);
	                               //Main10 needs pixFormat NV_PIX_P010 and P010 implies it
	unsigned int level;            //H.264 level_idc style value (e.g. 41 = 4.1)
	unsigned int bitrate;          //target bitrate in bit/s
	unsigned int peak_bitrate;     //peak bitrate for VBR mode (0 = derive from bitrate)
	char enableLossless;           //non-zero: constant QP 0 / High 4:4:4 (H.264 only)
	char mode_vbr;                 //non-zero: VBR rate control instead of CBR
	char insert_spspps_idr;        //non-zero: repeat SPS/PPS at every IDR frame
	char insert_vui;               //non-zero: embed VUI timing_info (fps) in the bitstream
	nvPixFormat pixFormat;         //raw input layout: NV_PIX_YUV420 (default), NV_PIX_NV12, or
	                               //NV_PIX_P010 (HEVC only, 10-bit Main10; create returns NULL
	                               //for H.264/AV1, or when profile is Main10 without P010)
	unsigned int iframe_interval;  //I-frame period in frames
	unsigned int idr_interval;     //IDR period in frames
	unsigned int fps_n;            //framerate numerator
	unsigned int fps_d;            //framerate denominator
	int capture_num;               //number of V4L2 buffers per plane (output & capture)
	unsigned int max_b_frames;     //number of B frames between references (H.264 only)
	unsigned int refs;             //number of reference frames (0 = encoder default)
	unsigned int qmax;             //max quantizer (0 = unset; applied only with qmin)
	unsigned int qmin;             //min quantizer (0 = unset; applied only with qmax)
	unsigned int hw_preset_type;   //speed/quality preset: 1=ultrafast .. 4=slow
	unsigned int vbv_buffer_size; //virtual buffer size of the encoder
	nvCodingType codingType;       //NV_VIDEO_CodingH264 or NV_VIDEO_CodingHEVC
	int max_perf;                  //non-zero: lift NVENC clock governor (max clocks)
	unsigned int poc_type;         //H.264 picture order count type (0=default, 2=low-latency)
	unsigned int wait_timeout;     //blocking-wait timeout in ms (0 = default 500ms)
	char enable_cabac;             //non-zero: enable CABAC entropy coding (H.264 only; default CAVLC)
	char insert_aud;               //non-zero: insert Access Unit Delimiter NALs
	int use_dmabuf;                //non-zero: encoder OUTPUT plane uses V4L2_MEMORY_DMABUF,
	                               //enabling zero-copy frame input via nvmpi_encoder_put_frame_fd()
	int nonblocking;               //non-zero: put_frame/put_frame_fd return NVMPI_ERR_EAGAIN
	                               //instead of blocking when no OUTPUT-plane buffer is available.
	                               //Default 0 = blocking (backward compatible).
	                               //Non-blocking mode never loses an encoded frame: see
	                               //nvmpi_encoder_put_frame().
} nvEncParam;

//Decoder creation parameters, consumed once by nvmpi_create_decoder().
typedef struct _NVDECPARAM{
	int frame_pool_size;     //number of decoded frames buffered before the user must read
	nvCodingType codingType; //input bitstream codec
	nvPixFormat pixFormat;   //requested output pixel layout
	nvSize resized;          //optional hw scaling target; {0,0} keeps stream resolution
	unsigned int chunk_size; //bytes per compressed-input V4L2 buffer; 0 = default (10 MiB).
	                         //One input packet (access unit) must fit in one chunk.
	int max_perf;            //non-zero: lift NVDEC clock governor (max clocks)
	int disable_dpb;         //non-zero: skip decoded-picture-buffer reordering (low-latency)
	int wait_timeout;        //blocking wait timeout in ms (0 = use default 500ms)
	unsigned int width;      //stream width from container headers (0 = unknown/use resolution-change events)
	unsigned int height;     //stream height from container headers (0 = unknown/use resolution-change events)
} nvDecParam;

//pts contract (video decoder and encoder): any 64-bit value given as
//nvPacket.pts to nvmpi_decoder_put_packet, or as the frame timestamp to
//nvmpi_encoder_put_frame / _put_frame_fd, comes back bit for bit from
//get_frame / get_frame_fd / get_frame_fd2, or from get_packet. That includes
//AV_NOPTS_VALUE (INT64_MIN), other negative values and values near
//INT64_MAX, read as int64 (nvPacket.pts is unsigned long, same bits). The
//Tegra kernel keeps the V4L2 timestamp as u64 ns, which is exact only for
//0..9223372036854775 us, so libnvmpi maps values outside that range to
//reserved in-range sentinels and restores them on output. It tracks 256
//distinct out-of-range values at a time (a repeated value reuses its slot);
//a frame still in flight after 256 newer distinct out-of-range puts comes
//back with a newer value. The callers' pipelines hold far fewer frames.
//Compressed packet exchanged across the API boundary.
//Decoder direction: caller owns 'payload' (it is memcpy'd into a V4L2
//buffer inside put_packet, so it may be freed on return).
//Encoder direction: packets are pooled. The wrapper pre-allocates them
//(payload typically NVMPI_ENC_CHUNK_SIZE bytes), queues them as "empty"
//via qEmptyPacket, and receives them back "filled" from get_packet.
typedef struct _NVPACKET{
	unsigned long flags;        //encoder sets keyframe flag (matches AV_PKT_FLAG_KEY)
	unsigned long payload_size; //valid bytes in payload
	unsigned char *payload;     //bitstream data
	unsigned long  pts;         //presentation timestamp in microseconds, any 64-bit value (see pts contract)
	//NVMPI_pkt pointer. used by encoder
	void* privData;
} nvPacket;

//Raw planar frame exchanged across the API boundary. The payload pointers
//always reference caller-owned memory: the decoder memcpy's decoded data
//into them (get_frame) and the encoder memcpy's out of them (put_frame),
//so no plane buffer ever changes ownership.
typedef struct _NVFRAME{
	unsigned long flags;
	unsigned long payload_size[3]; //per-plane byte size: IN capacity of payload[p]
	                               //(get_frame2, see there), OUT bytes the frame needs
	unsigned char *payload[3];     //per-plane data pointers (caller-owned)
	unsigned int linesize[3];      //per-plane stride in bytes
	nvPixFormat type;
	unsigned int width;
	unsigned int height;
	time_t timestamp;              //microseconds, any 64-bit value (see pts contract)
}nvFrame;

//Release callback for borrowed DMA-BUF fds returned by get_frame_fd.
//The caller MUST invoke this callback (passing the opaque pointer) when
//done with the fd — it returns the underlying frame buffer to the
//decoder's internal pool so it can be reused for future frames. If the
//caller needs the fd to outlive the callback, it must dup() the fd
//before calling back. Failure to invoke the callback starves the pool
//and stalls decoding.
typedef void (*nvmpi_frame_release_cb)(void *opaque);

//One decoded frame handed out as a borrowed DMA-BUF fd by
//nvmpi_jpeg_decoder_get_frame_fd or nvmpi_decoder_get_frame_fd2. Geometry is per plane and comes from the
//allocator, so never derive a plane address as pitch*height: use offset[].
//width/height are the DISPLAYED size of the JPEG (from its SOF header, e.g.
//33x17); for the video decoder they are the output size of the frame's pool
//generation (crop, or resize target). The planes behind fd may be larger (padding/alignment): always use
//pitch[]/offset[] (the real surface layout) and size (total bytes of the
//buffer), never width/height, to address pixels.
//Caller sets struct_size = sizeof(nvmpi_fd_frame) before the call; fields
//are only ever appended, so an older caller keeps working against a newer
//library (the library fills at most struct_size bytes).
typedef struct {
	uint32_t struct_size;      //IN: sizeof(nvmpi_fd_frame) of the caller
	int fd;                    //borrowed pitch-linear dmabuf fd (see ownership)
	uint32_t width, height;    //displayed JPEG size (planes may be larger)
	nvPixFormat format;        //NV_PIX_NV12 or NV_PIX_P010 (2 planes), NV_PIX_YUV420 (3)
	uint32_t num_planes;       //2 for NV12/P010, 3 for YUV420
	uint32_t pitch[3];         //bytes per row of each plane (>= row bytes)
	uint32_t offset[3];        //byte offset of each plane inside the fd
	uint64_t size;             //total size of the buffer behind fd in bytes
	int64_t timestamp;         //pts copied from nvPacket.pts of the input
	uint32_t flags;            //bit0 (NVMPI_FD_FRAME_FULL_RANGE): full range;
	                           //0 = limited (all video decoder frames)
} nvmpi_fd_frame;

#define NVMPI_FD_FRAME_FULL_RANGE 0x1u
//Smallest struct_size the library accepts (the first released layout).
#define NVMPI_FD_FRAME_V1_SIZE (offsetof(nvmpi_fd_frame, flags) + sizeof(uint32_t))

//FROZEN ABI (4.1.0): the layout of nvmpi_fd_frame and the value of
//NVMPI_ERR_INVAL are copied byte for byte by consumers (jetson-ffmpeg's
//dynlink header). Fields are only ever appended after `flags`; existing
//offsets never change. The asserts below pin the LP64 (aarch64) layout.
#if defined(__cplusplus)
#define NVMPI_STATIC_ASSERT(c, m) static_assert(c, m)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define NVMPI_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
#if defined(NVMPI_STATIC_ASSERT) && defined(__LP64__)
NVMPI_STATIC_ASSERT(sizeof(nvPixFormat) == 4, "nvPixFormat ABI");
NVMPI_STATIC_ASSERT(NVMPI_ERR_INVAL == -22, "NVMPI_ERR_INVAL ABI");
NVMPI_STATIC_ASSERT(NVMPI_ERR_FRAME_SIZE == -90, "NVMPI_ERR_FRAME_SIZE ABI");
NVMPI_STATIC_ASSERT(sizeof(nvmpi_fd_frame) == 72, "nvmpi_fd_frame size ABI");
NVMPI_STATIC_ASSERT(offsetof(nvmpi_fd_frame, struct_size) == 0, "struct_size ABI");
NVMPI_STATIC_ASSERT(offsetof(nvmpi_fd_frame, fd) == 4, "fd ABI");
NVMPI_STATIC_ASSERT(offsetof(nvmpi_fd_frame, width) == 8, "width ABI");
NVMPI_STATIC_ASSERT(offsetof(nvmpi_fd_frame, height) == 12, "height ABI");
NVMPI_STATIC_ASSERT(offsetof(nvmpi_fd_frame, format) == 16, "format ABI");
NVMPI_STATIC_ASSERT(offsetof(nvmpi_fd_frame, num_planes) == 20, "num_planes ABI");
NVMPI_STATIC_ASSERT(offsetof(nvmpi_fd_frame, pitch) == 24, "pitch ABI");
NVMPI_STATIC_ASSERT(offsetof(nvmpi_fd_frame, offset) == 36, "offset ABI");
NVMPI_STATIC_ASSERT(offsetof(nvmpi_fd_frame, size) == 48, "size ABI");
NVMPI_STATIC_ASSERT(offsetof(nvmpi_fd_frame, timestamp) == 56, "timestamp ABI");
NVMPI_STATIC_ASSERT(offsetof(nvmpi_fd_frame, flags) == 64, "flags ABI");
NVMPI_STATIC_ASSERT(NVMPI_FD_FRAME_V1_SIZE == 68, "V1 size ABI");
#endif

#ifdef __cplusplus
extern "C" {
#endif

	//Create a decoder context: opens the V4L2 decoder device, sets up the
	//bitstream (OUTPUT) plane and spawns the internal capture thread.
	//Returns an opaque handle; param is read once and not retained.
	//Returns NULL on failure: no decoder device node, device probe error
	//(ENODEV/ENXIO/EACCES, or EBUSY on all 3 attempts), decoder creation
	//failed after 3 attempts, or any OUTPUT-plane setup/ioctl error. Also
	//returns NULL for nvDecParam.pixFormat == NV_PIX_P010 in builds without
	//the NvUtils buffer API (legacy nvbuf_utils, pre-JetPack 5).
	nvmpictx* nvmpi_create_decoder(nvDecParam* param);

	//Feed one compressed packet (Annex-B for H.264/HEVC). May block waiting
	//for a free V4L2 OUTPUT-plane buffer once all buffers are in flight.
	//Decoded frames must be drained (get_frame / get_frame_fd) by the
	//caller or another thread: with the frame pool full the decoder stalls
	//and this call waits. A mid-stream resolution change is serviced even
	//while the pool is full (frames of the old size not yet taken are
	//discarded), so it never leaves put_packet waiting on its own caller.
	//A packet with payload_size==0 signals end-of-stream (starts flushing).
	//Returns 0 on success, -1 on dequeue failure, -2 on queue failure,
	//-3 when the packet exceeds chunk_size (invalid input — the packet is
	//dropped; smaller packets continue to be accepted).
	//AV1: data before the first sequence header OBU is discarded (returns 0,
	//no frame): NVDEC would hold it and block this call. A caller that keeps
	//the sequence header out of band (MP4/MKV av1C) must send the config
	//OBUs first. A flush re-arms the gate; the EOS marker always passes.
	int nvmpi_decoder_put_packet(nvmpictx* ctx, nvPacket* packet);

	//Retrieve one decoded frame by copying it into the caller-provided
	//frame->payload planes (which must already be allocated with matching
	//linesize). When wait is false: non-blocking, returns -1 immediately
	//if no frame is ready. When wait is true: blocks up to wait_timeout
	//milliseconds (configurable via nvDecParam.wait_timeout, default
	//500ms). Returns 0 on success, -1 when no frame is available (timeout
	//or shutdown), or the non-zero result of the failed CPU buffer-map call
	//(NvBufSurfaceMap / NvBufferMemMap) if the frame could not be copied;
	//the buffer is still recycled to the pool in that case.
	//
	//Buffer size (#149): this call does not know how large the caller's
	//planes are unless payload_size[] says so. If payload_size[p] is non-zero
	//for every plane of the format, the frame is copied only when it fits
	//exactly as nvmpi_decoder_get_frame2 defines. If payload_size[] is not
	//set (all zero), the caller promises the planes hold a frame of the
	//"expected size": the resize target (nvDecParam.resized) if one was set,
	//else the container size (nvDecParam.width/height) if given, else the
	//first frame this call delivered; with linesize >= the row bytes and the
	//pixel format of nvDecParam.pixFormat. A frame that is larger, has a
	//wider sample size, or has a linesize/payload that cannot hold a row is
	//NOT copied: it is dropped and NVMPI_ERR_FRAME_SIZE is returned (smaller
	//frames are copied as before). Use nvmpi_decoder_get_frame2 to learn the
	//new size and keep the frame.
	int nvmpi_decoder_get_frame(nvmpictx* ctx, nvFrame* frame,bool wait);

	//get_frame with a buffer capacity. IN per plane p of the format (2 for
	//NV12/P010, 3 for YUV420): payload[p] non-NULL, payload_size[p] = bytes
	//available at payload[p] (non-zero), linesize[p] = the stride you want.
	//A NULL payload or zero payload_size gives NVMPI_ERR_INVAL (frame stays
	//queued). The frame fits when, for every plane, linesize[p] >= the bytes
	//of one row and (rows-1)*linesize[p] + row bytes <= payload_size[p].
	//
	//Returns 0: copied; width/height/type/timestamp are set and
	//payload_size[p] = rows*row bytes (the packed size of the frame).
	//NVMPI_ERR_FRAME_SIZE: nothing was copied and no caller byte was
	//written. width/height/type are the frame's size and payload_size[p] =
	//rows*row bytes; reallocate (use linesize >= row bytes) and call again
	//with the same wait: the SAME frame is delivered next, with its
	//timestamp. The frame stays held by the decoder until a call succeeds,
	//the decoder is flushed (it is discarded) or closed; while it is held,
	//no new frame is taken from the pool. -1: no frame (timeout/shutdown).
	//Threads: all get_frame/get_frame2/get_frame_fd/get_frame_fd2 calls (and
	//flush/close) must come from one thread at a time; the held frame is not
	//locked. get_frame_fd/_fd2 also deliver a held frame first.
	int nvmpi_decoder_get_frame2(nvmpictx* ctx, nvFrame* frame, bool wait);

	//Reset the decoder pipeline for seek / stream restart: stops the
	//capture thread, drains in-flight frames, and restarts. The caller
	//MUST re-prime extradata (SPS/PPS) after this call so the hardware
	//decoder can reconfigure its capture plane. Always returns 0.
	int nvmpi_decoder_flush(nvmpictx* ctx);

	//Stop the capture thread, free all DMA buffers/pools and destroy ctx.
	//The handle is invalid afterwards. Returns 0, or -1 if ctx is NULL
	//(nothing is done).
	int nvmpi_decoder_close(nvmpictx* ctx);

	//--- JPEG decoder (synchronous, NvJPEGDecoder-backed, no V4L2) ---

	//Create a JPEG decoder context backed by the Tegra NVJPG engine.
	//No V4L2 device, no capture thread — decoding is synchronous per frame.
	//Returns NULL on failure (NvJPEGDecoder creation error).
	nvmpictx* nvmpi_create_jpeg_decoder(void);

	//Decode one JPEG frame. payload_size==0 signals EOS (no decode, sets
	//internal eos flag). Returns 0 on success, -1 on decode error or
	//unsupported JPEG format (progressive). The decoded frame is queued
	//internally for retrieval via nvmpi_jpeg_decoder_get_frame().
	int nvmpi_jpeg_decoder_put_packet(nvmpictx* ctx, nvPacket* packet);

	//Retrieve one decoded frame. When wait is true: blocks up to 500ms
	//for a frame. Returns -1 when no frame is available (timeout or EOS).
	//Buffer size (#149): with payload_size[] set for every plane, the image
	//is copied only if it fits (same rule as nvmpi_decoder_get_frame2);
	//without it the planes are assumed to hold the first image this call
	//delivered (linesize >= row bytes). An image that does not fit is
	//dropped, nothing is copied, width/height/type report its size and
	//NVMPI_ERR_FRAME_SIZE is returned (reallocate, re-submit the image).
	int nvmpi_jpeg_decoder_get_frame(nvmpictx* ctx, nvFrame* frame, bool wait);

	//Destroy the JPEG decoder context and free all DMA buffers.
	int nvmpi_jpeg_decoder_close(nvmpictx* ctx);

	//--- JPEG decoder, zero-copy DMA-BUF output ---

	//Create a JPEG decoder whose frames are retrieved as DMA-BUF fds with
	//nvmpi_jpeg_decoder_get_frame_fd instead of being copied.
	//  out:       NV_PIX_NV12 or NV_PIX_YUV420 (anything else -> NULL).
	//  pool_size: number of pitch-linear output buffers, 1..64; 0 selects
	//             the default of 8. Out of range -> NULL.
	//The pool is allocated on the first decoded frame and re-created when
	//the picture size changes. 4:2:0, 4:2:2 and 4:4:4 baseline JPEGs are
	//converted by the VIC; grayscale and progressive JPEGs are rejected by
	//put_packet (-1). Requires the NvUtils/NvBufSurface API; builds against
	//the legacy nvbuf_utils API return NULL.
	//Use ONLY the nvmpi_jpeg_decoder_{put_packet,get_frame_fd,close}
	//functions with the returned handle; nvmpi_jpeg_decoder_get_frame
	//(copying) returns -1 on it. Returns NULL on invalid arguments or if
	//NvJPEGDecoder creation fails. The legacy nvmpi_create_jpeg_decoder
	//is unchanged.
	//
	//In this mode nvmpi_jpeg_decoder_put_packet additionally returns
	//NVMPI_ERR_EAGAIN (the packet was NOT consumed) when every pool buffer
	//is still borrowed or queued: return frames via their release callback
	//(or drain get_frame_fd) and retry the same packet. Other results are
	//as documented above: 0 ok, -1 error.
	nvmpictx* nvmpi_create_jpeg_decoder_fmt(nvPixFormat out, int pool_size);

	//Retrieve one decoded frame as a borrowed DMA-BUF fd.
	//  frame:   OUT, caller sets frame->struct_size first.
	//  release: OUT, callback the caller MUST call exactly once with *opaque
	//           when it is done with frame->fd.
	//  opaque:  OUT, cookie for the callback.
	//  wait:    false = non-blocking; true = block up to 500 ms.
	//Ownership: the fd belongs to libnvmpi's pool and is NOT dup'd; the
	//caller must not close() it. It stays valid until the release callback
	//ran. To keep it longer, dup() it BEFORE releasing (a dup'd fd is not
	//known to NvBufSurfaceFromFd; only the original fd is). Decoder-internal
	//buffers are never exposed, so the fd is never overwritten by a later
	//decode while borrowed.
	//Threads: put_packet and get_frame_fd must be called from one thread at
	//a time. The release callback may be called from any thread, at any
	//time, including after nvmpi_jpeg_decoder_close(): the pool memory then
	//lives until the last borrowed frame is released (no timeout, no wait).
//Guarantee: after nvmpi_jpeg_decoder_close() every borrowed fd/surface stays
//fully valid (usable, e.g. by NvBufSurfaceFromFd or as encoder input) until
//its release callback runs; the destruction is deferred to that last
//release. close() never fails or blocks because of outstanding borrows.
	//Release exactly once, never touch frame->fd afterwards.
	//Pool fds are created close-on-exec (FD_CLOEXEC). A forked child must
	//not use the context or a borrowed fd for anything but exec/_exit.
	//Returns 0 on success; -1 no frame available (non-blocking), timeout
	//or EOS/closed; NVMPI_ERR_INVAL for NULL ctx/frame/release/opaque, a
	//struct_size below NVMPI_FD_FRAME_V1_SIZE, or a handle that was not
	//created by nvmpi_create_jpeg_decoder_fmt.
	int nvmpi_jpeg_decoder_get_frame_fd(nvmpictx* ctx, nvmpi_fd_frame* frame,
		nvmpi_frame_release_cb *release, void **opaque, bool wait);

	//--- JPEG encoder (synchronous, NvJPEGEncoder-backed, no V4L2) ---

	//Create a JPEG encoder context backed by the Tegra NVJPG engine.
	//No V4L2 device, no DQ thread — encoding is synchronous per frame.
	//quality: libjpeg quality 1-100 (clamped). Default 85 if out of range.
	//Returns NULL on failure. On modules without NVJPG encode capability
	//(e.g. Orin Nano), createJPEGEncoder returns NULL — the FFmpeg wrapper
	//reports a clear error suggesting -c:v mjpeg software fallback.
	nvmpictx* nvmpi_create_jpeg_encoder(int quality);

	//Encode one YUV420 frame to JPEG. frame==NULL signals EOS.
	//Returns 0 on success, -1 on error. The encoded JPEG is staged
	//internally for retrieval via nvmpi_jpeg_encoder_get_packet().
	int nvmpi_jpeg_encoder_put_frame(nvmpictx* ctx, nvFrame* frame);

	//Retrieve the encoded JPEG packet. Copies JPEG data into
	//packet->payload (caller must pre-allocate). Returns 0 on success,
	//-1 when no packet is ready, -2 on EOS with no packet.
	//packet->flags is set to 1 (keyframe) for every JPEG frame.
	int nvmpi_jpeg_encoder_get_packet(nvmpictx* ctx, nvPacket* packet);

	//Destroy the JPEG encoder context and free all resources.
	int nvmpi_jpeg_encoder_close(nvmpictx* ctx);

	//Create an encoder context: opens the V4L2 encoder device, programs
	//profile/level/rate-control from param, sets up both planes and starts
	//the capture-plane dequeue thread. Returns opaque handle, or NULL on
	//failure (no encoder device node, device probe error, creation failed
	//after 3 attempts, or a critical format/plane/stream setup error), or
	//when param->codingType is not H.264 or HEVC (checked first, before any
	//device is opened).
	//NOTE: the caller must still fill the packet pool (qEmptyPacket) before
	//encoded output can be delivered.
	nvmpictx* nvmpi_create_encoder(nvEncParam* param);
	//add frame to encoder
	//Copies the frame planes into a V4L2 OUTPUT-plane buffer and queues it.
	//In blocking mode (default): blocks waiting for a free buffer.
	//In non-blocking mode (nonblocking=1): returns NVMPI_ERR_EAGAIN when no
	//OUTPUT-plane buffer is available — caller should drain encoded packets
	//via get_packet and retry. frame==NULL sends EOS and puts the encoder
	//into flushing mode (never refused for lack of packets).
	//Back-pressure, blocking AND non-blocking mode: a frame is also refused
	//(NVMPI_ERR_EAGAIN, not queued) while the frames in flight would outnumber the empty
	//packets of the pool, so every accepted frame has a packet waiting for
	//its encoded output and nothing is dropped. The caller must read
	//packets (get_packet) and return them (qEmptyPacket), then retry the
	//same frame. Blocking mode returns EAGAIN here too instead of waiting,
	//because only the caller can free packets and it cannot while blocked in
	//this call; a caller that ignored it used to lose AUs silently. The pool must hold more packets than the encoder delays
	//frames (B-frames/reordering), or no output appears until EOS.
	//Returns 0 on success, NVMPI_ERR_EAGAIN when no packet is spare (both
	//modes) or, non-blocking, no OUTPUT buffer is available, -2 if already flushing, -1 if
	//the encoder is in error or the blocking dequeue failed, or the negative
	//queue (qBuffer) error code.
	int nvmpi_encoder_put_frame(nvmpictx* ctx, nvFrame* frame);
	//get filled packet from encoder
	//When wait=false: non-blocking, returns -1 immediately if no packet ready.
	//When wait=true: returns -1 immediately when no accepted frame is still
	//in flight; otherwise waits up to wait_timeout_ms for the in-flight AUs.
	//Returns -1 on timeout or shutdown.
	//While flushing: blocks until a packet arrives or EOS (-2 = EOS).
	//On success the caller owns the packet until it re-queues it with
	//nvmpi_encoder_qEmptyPacket().
	int nvmpi_encoder_get_packet(nvmpictx* ctx, nvPacket** packet, bool wait);
	//get empty packet with allocated buffer from encoder packet pool
	//Returns -1 when the empty pool is exhausted and the encoder is not
	//filling a packet. May block briefly (capped at 1 s, normally one
	//packet copy) while the encoder finishes a packet it already took.
	//Used by the wrapper to drain/teardown the pool: drain this queue until
	//-1 BEFORE get_packet(), since the in-flight packet ends up filled, and
	//do not call nvmpi_encoder_qEmptyPacket() during the drain.
	int nvmpi_encoder_dqEmptyPacket(nvmpictx* ctx, nvPacket** packet);
	//add empty packet with allocated buffer  to  encoder packet pool
	//Hands a caller-allocated packet (payload buffer included) to the pool;
	//the encoder's capture thread fills it later. The pool only stores the
	//pointer — the caller remains responsible for eventually freeing it.
	void nvmpi_encoder_qEmptyPacket(nvmpictx* ctx, nvPacket* packet);
	//Force the encoder to produce an IDR frame for the next queued input.
	//Calls NvVideoEncoder::forceIDR() which sets
	//V4L2_CID_MPEG_MFC51_VIDEO_FORCE_FRAME_TYPE. May be called at any time
	//while the encoder is running. Returns 0 on success, -1 on error.
	int nvmpi_encoder_force_idr(nvmpictx* ctx);
	//Change the encoder target bitrate mid-stream (adaptive bitrate).
	//Calls NvVideoEncoder::setBitrate() which updates
	//V4L2_CID_MPEG_VIDEO_BITRATE. Takes effect from the next encoded frame.
	//Returns 0 on success, -1 on error.
	int nvmpi_encoder_set_bitrate(nvmpictx* ctx, unsigned int bitrate);
	//flush encoder (mid-stream reset)
	//Stops DQ thread, STREAMOFF both planes, drains packet pool, resets
	//flushing/EOS state, STREAMON, re-queues capture buffers, restarts DQ
	//thread. Encoding can resume immediately after this call.
	//Returns 0, or a negative code if a STREAMON, capture-buffer queue or
	//DQ thread restart step fails.
	int nvmpi_encoder_flush(nvmpictx* ctx);
	//close encoder
	//Stops the dequeue thread and frees the context. Packets still held in
	//the pools are NOT freed here — drain them first: dqEmptyPacket until
	//-1, then get_packet until empty (see FFmpeg wrapper).
	//Returns 0, or -1 if ctx is NULL or the capture DQ thread did not exit
	//within its bounded join; in the latter case ctx is deliberately leaked
	//to avoid a use-after-free.
	int nvmpi_encoder_close(nvmpictx* ctx);

	//--- DMA-BUF fd passthrough (zero-copy path) ---

	//Retrieve one decoded frame as a borrowed DMA-BUF fd instead of copying
	//pixel data into caller-owned memory (zero-copy alternative to
	//nvmpi_decoder_get_frame). The fd points to a pitch-linear NV12/YUV420
	//buffer that has already been VIC-transformed from the decoder's
	//block-linear capture plane. The caller receives:
	//  *dmabuf_fd  — the DMA-BUF file descriptor (borrowed, not dup'd)
	//  *width, *height, *pitch — buffer geometry
	//  *timestamp  — presentation timestamp in microseconds
	//  *release    — callback the caller MUST invoke when done with the fd
	//  *opaque     — cookie to pass to the release callback
	//The fd is valid only until the release callback is invoked. If the
	//caller needs it longer (e.g. for encoder input), it must dup() first.
	//When wait is false: non-blocking, returns -1 when no frame is ready.
	//When wait is true: blocks up to wait_timeout_ms (default 500ms).
	//Returns 0 on success, -1 on no frame / timeout.
	int nvmpi_decoder_get_frame_fd(nvmpictx* ctx,
		int *dmabuf_fd, int *width, int *height, int *pitch,
		int64_t *timestamp,
		nvmpi_frame_release_cb *release, void **opaque, bool wait);

	//Same borrow as nvmpi_decoder_get_frame_fd, but returns the full
	//per-plane geometry in an nvmpi_fd_frame (P010 and YUV420 included).
	//Caller sets frame->struct_size = sizeof(nvmpi_fd_frame) (at least
	//NVMPI_FD_FRAME_V1_SIZE); the library fills min(struct_size, sizeof) bytes
	//and writes the filled size back to struct_size.
	//  fd          borrowed pitch-linear dmabuf fd, NOT dup'd (NvBufSurfaceFromFd
	//              fails on dup'd fds); never close() it
	//  width/height output size of the frame's pool generation (the crop or
	//              resize target, not the padded plane size)
	//  format      the decoder's output format (NV_PIX_NV12/P010: 2 planes,
	//              NV_PIX_YUV420: 3 planes)
	//  pitch[]/offset[]/size  the real surface layout from the allocator:
	//              plane i starts at offset[i] (page aligned, NOT pitch*height)
	//  timestamp   as in the copying path, microseconds
	//  flags       0: the VIC destination surfaces are limited range
	//Ownership is the one of nvmpi_decoder_get_frame_fd: the fd is valid until
	//*release(*opaque) runs, which must happen exactly once, from any thread,
	//also after nvmpi_decoder_close or a resolution change (the buffer then
	//stays fully valid until that last release; close never blocks on
	//borrows). dup() the fd to keep it longer. Frames decoded before a
	//resolution change keep their original geometry.
	//wait: as in nvmpi_decoder_get_frame_fd.
	//Returns 0 on success; -1 when no frame is ready (non-blocking), on
	//timeout or shutdown; NVMPI_ERR_INVAL for NULL arguments or a struct_size
	//below NVMPI_FD_FRAME_V1_SIZE (no frame is consumed then).
	int nvmpi_decoder_get_frame_fd2(nvmpictx* ctx, nvmpi_fd_frame* frame,
		nvmpi_frame_release_cb *release, void **opaque, bool wait);

	//Submit one raw frame to the encoder via DMA-BUF fd (zero-copy
	//alternative to nvmpi_encoder_put_frame). The fd must reference an
	//NV12 DMA-BUF surface with pitch-linear layout and matching
	//dimensions. The encoder accesses the buffer directly via
	//V4L2_MEMORY_DMABUF — no CPU memcpy. Requires use_dmabuf=1 in
	//nvEncParam at encoder creation time. dmabuf_fd==−1 signals EOS.
	//Returns 0 on success, NVMPI_ERR_EAGAIN when no packet is spare (both
	//modes) or, non-blocking, no buffer available, negative on error or if
	//already flushing. Same back-pressure rule as nvmpi_encoder_put_frame.
	int nvmpi_encoder_put_frame_fd(nvmpictx* ctx,
		int dmabuf_fd, int width, int height, int pitch,
		int64_t timestamp);

	//Allocate a pitch-linear NV12 DMA-BUF surface tagged for VIC
	//transform (NvBufSurfaceTag_VIDEO_CONVERT). Use this for surfaces
	//that are VIC source/destination but NOT passed directly to the
	//encoder's V4L2 DMABUF output plane.
	//Returns 0 on success and writes the dmabuf fd to *dmabuf_fd.
	//On failure returns -1 and *dmabuf_fd is set to -1.
	int nvmpi_surface_alloc(unsigned int width, unsigned int height,
		int *dmabuf_fd);

	//Allocate a pitch-linear NV12 DMA-BUF surface tagged for the
	//encoder (NvBufSurfaceTag_VIDEO_ENC). Use this for surfaces that
	//will be passed to the encoder's V4L2 DMABUF output plane via
	//nvmpi_encoder_put_frame_fd. The VIDEO_ENC memtag registers the
	//buffer in the encoder's NvMap domain so NvMMLite can access it
	//for internal format conversion.
	//VIC hardware transform also works with VIDEO_ENC tagged surfaces.
	//Returns 0 on success, -1 on failure.
	int nvmpi_surface_alloc_for_enc(unsigned int width, unsigned int height,
		int *dmabuf_fd);

	//Destroy a surface previously allocated with nvmpi_surface_alloc
	//or nvmpi_surface_alloc_for_enc. Safe to call with dmabuf_fd == -1
	//(no-op). Returns 0 on success, -1 on NvBufferDestroy failure.
	int nvmpi_surface_destroy(int dmabuf_fd);

	//Copy NV12 frame data from an unregistered DMA-BUF fd (e.g. dup'd
	//from decoder DRM_PRIME output) into a registered NvBufSurface fd.
	//Uses mmap + CPU copy + DMA-BUF sync. dst_fd must have been
	//allocated via nvmpi_surface_alloc at matching dimensions.
	//src_pitch = source row stride in bytes (from DRM descriptor).
	int nvmpi_surface_copy_from_dmabuf(int dst_fd, int src_fd,
		unsigned int width, unsigned int height,
		unsigned int src_pitch);

	//--- VIC hardware transform (standalone scale/CSC) ---

	//Opaque VIC context handle. Created by nvmpi_vic_create(), destroyed
	//by nvmpi_vic_close(). Holds the VIC session binding and cached
	//transform parameters. Must not be shared across threads — each
	//thread needs its own context (VIC session is thread-local).
	typedef struct nvmpi_vic_ctx nvmpi_vic_ctx;

	//Create a VIC transform context: binds the VIC engine for the
	//calling thread via NvBufSurfTransformSetSessionParams (NvUtils) or
	//NvBufferSessionCreate (legacy). Returns NULL on failure.
	//Allocated here; freed in nvmpi_vic_close().
	nvmpi_vic_ctx *nvmpi_vic_create(void);

	//Perform a VIC hardware scale/CSC transform between two DMA-BUF
	//surfaces. Both fds must reference valid NV12 DMA-BUF buffers
	//(e.g. from nvmpi_surface_alloc or decoder output). The VIC engine
	//handles scaling, block-linear ↔ pitch-linear conversion, and
	//color-space conversion in hardware with zero CPU copies.
	//Returns 0 on success, -1 on error.
	int nvmpi_vic_transform(nvmpi_vic_ctx *ctx,
		int src_fd, int dst_fd,
		unsigned int src_w, unsigned int src_h,
		unsigned int dst_w, unsigned int dst_h);

	//Destroy a VIC context and release the VIC session. Safe to call
	//with NULL (no-op). Freed here; allocated in nvmpi_vic_create().
	void nvmpi_vic_close(nvmpi_vic_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /*__NVMPI_H__*/
