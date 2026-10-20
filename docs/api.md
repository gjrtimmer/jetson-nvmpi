# libnvmpi C API reference

The complete public API of libnvmpi lives in one header,
[`include/nvmpi.h`](../include/nvmpi.h). It is plain C (`extern "C"` guarded)
and is the only installed header. Line references below (`nvmpi.h:N`) point at
the declaration in that header.

Contents:

- [Concepts](#concepts)
- [Constants](#constants)
- [Enums](#enums)
- [Structures](#structures)
- [Video decoder](#video-decoder)
- [Video encoder](#video-encoder)
- [JPEG decoder](#jpeg-decoder)
- [JPEG encoder](#jpeg-encoder)
- [DMA-BUF zero-copy path](#dma-buf-zero-copy-path)
- [Surfaces](#surfaces)
- [VIC transform](#vic-transform)
- [Threading summary](#threading-summary)
- [Return code cheat sheet](#return-code-cheat-sheet)

## Concepts

- **Context (`nvmpictx*`).** An opaque handle returned by a `create` function.
  Decoder and encoder each define their own internal struct behind the same
  typedef, so a handle must only be passed back to the API family that created
  it (video decoder, video encoder, JPEG decoder or JPEG encoder). `nvmpi.h:35-38`
- **Put/get model.** Both directions are simple loops. Decode:
  `nvmpi_create_decoder` -> `put_packet` / `get_frame` ... -> `close`.
  Encode: `nvmpi_create_encoder` -> `put_frame` / `get_packet` ... -> `close`.
  `nvmpi.h:14-16`
- **Parameter structs are read once.** `nvDecParam` and `nvEncParam` are
  consumed by the `create` call and not retained. Always zero-initialise them
  (`memset(&p, 0, sizeof p)` or `= {0}`) and then set what you need: a zero
  field means "default / unset" for most members.
- **Memory ownership.** Pixel and bitstream memory passed through `nvFrame` and
  `nvPacket` is caller-owned and copied (see the structures below), with two
  exceptions: the encoder packet pool (you donate packets, libnvmpi fills and
  returns them) and borrowed DMA-BUF fds (you must release them).
- **Threads.** Each video context is single-user-thread. libnvmpi starts its
  own internal thread per video context (decoder capture thread, encoder
  dequeue thread). See [Threading summary](#threading-summary) and
  [internals.md](internals.md).
- **Hardware.** All calls other than the JPEG and VIC ones open V4L2 codec
  devices exposed by the Jetson driver; they only work on a real Jetson. On a
  stubs build the library links but is not functional (see [build.md](build.md)).

## Constants

### `NVMPI_ENC_CHUNK_SIZE`

`nvmpi.h:29` -- `#define NVMPI_ENC_CHUNK_SIZE 10*1024*1024`

Maximum size in bytes of one encoded buffer on the encoder capture plane. A
caller that pre-allocates pooled packet buffers should allocate this many bytes
per packet: an encoded frame larger than this is dropped with an error log
instead of overflowing the packet buffer. The macro is not parenthesised, so
wrap it yourself when using it in an expression: `(NVMPI_ENC_CHUNK_SIZE)`.

### `NVMPI_ERR_EAGAIN`

`nvmpi.h:33` -- `#define NVMPI_ERR_EAGAIN (-11)`

Returned by the non-blocking encoder submit calls when no OUTPUT-plane buffer
is free. Drain output packets with `nvmpi_encoder_get_packet` and retry. The
value matches Linux `EAGAIN`.

## Enums

### `nvPixFormat`

`nvmpi.h:43-47` -- raw (uncompressed) pixel layouts, used for decoder output and
encoder input.

| Value | Meaning |
| --- | --- |
| `NV_PIX_NV12` | 8-bit 4:2:0 semi-planar: Y plane plus interleaved UV (2 planes). |
| `NV_PIX_YUV420` | 8-bit 4:2:0 planar: separate Y, U, V planes (3 planes). |
| `NV_PIX_P010` | 10-bit 4:2:0 semi-planar (matches FFmpeg `P010LE`). Decode only, HEVC Main10 only, requires the NvUtils buffer API (JetPack 5 and later). |

Note: `NV_PIX_NV12` is enum value 0, and the encoder selects the NV12 input
format whenever `pixFormat == NV_PIX_NV12`, so a zero-initialised `nvEncParam`
selects NV12 input. The header comment on `nvEncParam::pixFormat`
(`nvmpi.h:84`) calls `NV_PIX_YUV420` the default; set the field explicitly.

### `nvCodingType`

`nvmpi.h:51-60` -- compressed bitstream codec selector.

| Value | Codec | Video decode | Video encode |
| --- | --- | :---: | :---: |
| `NV_VIDEO_CodingUnused` | none | no | no |
| `NV_VIDEO_CodingH264` | H.264 / AVC | yes | yes |
| `NV_VIDEO_CodingMPEG4` | MPEG-4 Part 2 | yes | no |
| `NV_VIDEO_CodingMPEG2` | MPEG-2 | yes | no |
| `NV_VIDEO_CodingVP8` | VP8 | yes | no |
| `NV_VIDEO_CodingVP9` | VP9 | yes | no |
| `NV_VIDEO_CodingHEVC` | H.265 / HEVC | yes | yes |
| `NV_VIDEO_CodingMJPEG` | MJPEG | via the [JPEG decoder](#jpeg-decoder) only | via the [JPEG encoder](#jpeg-encoder) only |

`nvmpi_create_decoder` maps `NV_VIDEO_CodingH264`, `HEVC`, `MPEG4`, `MPEG2`,
`VP8` and `VP9` to the matching V4L2 format; any other value (including
`NV_VIDEO_CodingMJPEG` and `NV_VIDEO_CodingUnused`) falls through to H.264
(`src/nvmpi_dec_init.cpp`, switch on `codingType`). Use the dedicated JPEG API
for MJPEG. The video encoder supports only `NV_VIDEO_CodingH264` and
`NV_VIDEO_CodingHEVC`.

## Structures

### `nvSize`

`nvmpi.h:63-66` -- width/height pair (`unsigned int width, height`), used for
the decoder resize target.

### `nvEncParam`

`nvmpi.h:73-107` -- encoder creation parameters, consumed once by
`nvmpi_create_encoder()`. Numeric profile/level values follow FFmpeg's
`FF_PROFILE_H264_*` / `level_idc` conventions and are translated to V4L2 enums
internally.

| Field | Type | Meaning |
| --- | --- | --- |
| `width`, `height` | `unsigned int` | Frame size in pixels (required). |
| `profile` | `unsigned int` | FFmpeg-style H.264 profile id (66 baseline, 77 main, 100 high). |
| `level` | `unsigned int` | H.264 `level_idc` style value (41 = level 4.1). |
| `bitrate` | `unsigned int` | Target bitrate in bit/s. |
| `peak_bitrate` | `unsigned int` | Peak bitrate for VBR; 0 derives it from `bitrate`. |
| `enableLossless` | `char` | Non-zero: constant QP 0 / High 4:4:4 (H.264 only). |
| `mode_vbr` | `char` | Non-zero: VBR rate control instead of CBR. |
| `insert_spspps_idr` | `char` | Non-zero (the code tests `== 1`): repeat SPS/PPS at every IDR frame. |
| `insert_vui` | `char` | Non-zero: embed VUI timing info (fps) in the bitstream. |
| `pixFormat` | `nvPixFormat` | Raw input layout: `NV_PIX_YUV420` or `NV_PIX_NV12`. |
| `iframe_interval` | `unsigned int` | I-frame period in frames. |
| `idr_interval` | `unsigned int` | IDR period in frames. |
| `fps_n`, `fps_d` | `unsigned int` | Framerate numerator / denominator. |
| `capture_num` | `int` | Number of V4L2 buffers per plane (OUTPUT and CAPTURE). Must be greater than 0; the API test uses 4. |
| `max_b_frames` | `unsigned int` | B-frames between references (H.264 only). |
| `refs` | `unsigned int` | Number of reference frames; 0 = encoder default. |
| `qmax`, `qmin` | `unsigned int` | Quantizer bounds; 0 = unset, applied only as a pair. |
| `hw_preset_type` | `unsigned int` | Speed/quality preset, 1 = ultrafast ... 4 = slow. |
| `vbv_buffer_size` | `unsigned int` | Virtual buffer size of the encoder. |
| `codingType` | `nvCodingType` | `NV_VIDEO_CodingH264` or `NV_VIDEO_CodingHEVC`. |
| `max_perf` | `int` | Non-zero: lift the NVENC clock governor (max clocks). |
| `poc_type` | `unsigned int` | H.264 picture order count type (0 default, 2 low-latency). |
| `wait_timeout` | `unsigned int` | Blocking-wait timeout in ms. 0 = 500 ms; non-zero values are clamped to 100..5000 ms (`src/nvmpi_enc_init.cpp:78-82`). |
| `enable_cabac` | `char` | Non-zero: CABAC entropy coding (H.264 only; default CAVLC). |
| `insert_aud` | `char` | Non-zero: insert Access Unit Delimiter NAL units. |
| `use_dmabuf` | `int` | Non-zero: OUTPUT plane uses `V4L2_MEMORY_DMABUF`, enabling `nvmpi_encoder_put_frame_fd()`. |
| `nonblocking` | `int` | Non-zero: submit calls return `NVMPI_ERR_EAGAIN` instead of blocking. 0 (default) = blocking. |

### `nvDecParam`

`nvmpi.h:110-122` -- decoder creation parameters, consumed once by
`nvmpi_create_decoder()`.

| Field | Type | Meaning |
| --- | --- | --- |
| `frame_pool_size` | `int` | Number of decoded frames buffered before the caller must read. Must be greater than 0: the decoder context's internal initial value of 12 is overwritten with this field, and a value of 0 or less logs an error and skips frame-pool allocation (`src/nvmpi_dec_init.cpp:142`, `:299`). |
| `codingType` | `nvCodingType` | Input bitstream codec (see [`nvCodingType`](#nvcodingtype)). |
| `pixFormat` | `nvPixFormat` | Requested output pixel layout. |
| `resized` | `nvSize` | Optional hardware scaling target (VIC); `{0, 0}` keeps the stream resolution. |
| `chunk_size` | `unsigned int` | Bytes per compressed-input V4L2 buffer. 0 = default (10,000,000 bytes); accepted range 65536..67108864, out-of-range values log a warning and use the default. One input packet must fit in one chunk. |
| `max_perf` | `int` | Non-zero: lift the NVDEC clock governor. This field is copied as-is, so 0 means off. |
| `disable_dpb` | `int` | Non-zero: skip decoded-picture-buffer reordering (low latency, no B-frame reordering). |
| `wait_timeout` | `int` | Blocking wait timeout in ms. 0 = 500 ms; accepted range 50..5000, out-of-range values log a warning and use 500 ms. |
| `width`, `height` | `unsigned int` | Stream size from container headers; 0 = unknown, rely on V4L2 resolution-change events. |

### `nvPacket`

`nvmpi.h:130-137` -- one compressed packet.

| Field | Type | Meaning |
| --- | --- | --- |
| `flags` | `unsigned long` | Encoder sets the keyframe flag (value matches `AV_PKT_FLAG_KEY`). |
| `payload_size` | `unsigned long` | Valid bytes in `payload`. |
| `payload` | `unsigned char*` | Bitstream data. |
| `pts` | `unsigned long` | Presentation timestamp in microseconds. |
| `privData` | `void*` | Encoder pool back-pointer to the caller's own packet object (the FFmpeg layer stores its `AVPacket` here; see `src/nvmpi_enc_output.cpp`). |

Ownership:

- **Decoder direction:** the caller owns `payload`. It is `memcpy`'d into a V4L2
  buffer inside `put_packet`, so it may be freed on return.
- **Encoder direction:** packets are pooled. The caller allocates them (payload
  typically `NVMPI_ENC_CHUNK_SIZE` bytes), hands them over with
  `nvmpi_encoder_qEmptyPacket`, and receives them back filled from
  `nvmpi_encoder_get_packet`. libnvmpi never allocates or frees packet memory.

### `nvFrame`

`nvmpi.h:143-152` -- one raw planar frame.

| Field | Type | Meaning |
| --- | --- | --- |
| `flags` | `unsigned long` | Frame flags. |
| `payload_size[3]` | `unsigned long` | Per-plane byte size. |
| `payload[3]` | `unsigned char*` | Per-plane data pointers (caller-owned). |
| `linesize[3]` | `unsigned int` | Per-plane stride in bytes. |
| `type` | `nvPixFormat` | Pixel layout. |
| `width`, `height` | `unsigned int` | Frame size. |
| `timestamp` | `time_t` | Microseconds, carried through the V4L2 buffer timestamp. |

Payload pointers always reference caller-owned memory. The decoder copies into
them (`get_frame`), the encoder copies out of them (`put_frame`); ownership
never changes. `get_frame` fills the planes and `timestamp`; it does not fill
`width`/`height`, so take the geometry from your stream (or `nvDecParam`).

### `nvmpi_frame_release_cb`

`nvmpi.h:161` -- `typedef void (*nvmpi_frame_release_cb)(void *opaque);`

Release callback for borrowed DMA-BUF fds returned by
[`nvmpi_decoder_get_frame_fd`](#nvmpi_decoder_get_frame_fd). Call it exactly
once per frame, passing the opaque pointer you received. It returns the frame
buffer to the decoder pool and frees the small bookkeeping object behind
`opaque`. Not calling it starves the pool and stalls decoding.

## Video decoder

Backed by the V4L2 decoder device (NVDEC). The decoder runs an internal capture
thread that dequeues decoded buffers, converts them with VIC from the decoder's
block-linear layout into pitch-linear buffers (and scales if `resized` is set),
and publishes them to a frame pool.

### `nvmpi_create_decoder`

`nvmpi.h:175` -- `nvmpictx* nvmpi_create_decoder(nvDecParam* param);`

Opens the V4L2 decoder, sets up the compressed-input (OUTPUT) plane and spawns
the capture thread. `param` is read once. The decoder device is looked up as
`/dev/nvhost-nvdec` or `/dev/v4l2-nvdec`. Returns a handle, or `NULL` when:

- `pixFormat` is `NV_PIX_P010` and the build has no NvUtils buffer API
  (legacy `nvbuf_utils`, pre-JetPack 5); 10-bit output cannot be honoured.
- Neither device node exists, or opening it fails with `ENODEV`, `ENXIO` or
  `EACCES`, or with `EBUSY` on all 3 attempts (100 ms apart).
- The decoder could not be created after 3 attempts.
- Event subscription or OUTPUT-plane setup fails (format, frame input mode,
  DPB, max-perf, buffer setup, STREAMON).

### `nvmpi_decoder_put_packet`

`nvmpi.h:183` -- `int nvmpi_decoder_put_packet(nvmpictx* ctx, nvPacket* packet);`

Feeds one compressed packet (Annex-B for H.264/HEVC). May block waiting for a
free V4L2 OUTPUT-plane buffer once all buffers are in flight. A packet with
`payload_size == 0` signals end of stream (starts flushing). `pts` is carried
in microseconds.

| Return | Meaning |
| --- | --- |
| `0` | Success. |
| `-1` | Dequeue failure on the OUTPUT plane. |
| `-2` | Queue failure on the OUTPUT plane. |
| `-3` | Packet larger than `chunk_size`: invalid input, the packet is dropped, the decoder stays usable and smaller packets continue to be accepted. |

Blocking: yes (once all input buffers are in flight). Threading: user thread
only.

### `nvmpi_decoder_get_frame`

`nvmpi.h:194` -- `int nvmpi_decoder_get_frame(nvmpictx* ctx, nvFrame* frame, bool wait);`

Copies one decoded frame into `frame->payload[]`. The planes must already be
allocated with sufficient size and `linesize[]` set; the copy is line by line,
so your stride may differ from the hardware's. The number of planes written is
2 for `NV_PIX_NV12` / `NV_PIX_P010` and 3 for `NV_PIX_YUV420`.

| `wait` | Behaviour |
| --- | --- |
| `false` | Non-blocking; returns `-1` immediately when no frame is ready. |
| `true` | Blocks up to `wait_timeout` ms (default 500); returns `-1` on timeout or shutdown. |

| Return | Meaning |
| --- | --- |
| `0` | Success. |
| `-1` | No frame available (non-blocking), or timeout / shutdown (blocking). |
| other non-zero | The result of the failed CPU buffer-map call (`NvBufSurfaceMap` on NvUtils, `NvBufferMemMap` on the legacy API), passed through unchanged. The pool buffer is still recycled and `frame->timestamp` is set, but the planes are not valid. |

Threading: user thread only.

### `nvmpi_decoder_flush`

`nvmpi.h:200` -- `int nvmpi_decoder_flush(nvmpictx* ctx);`

Resets the pipeline for a seek or stream restart: stops and joins the capture
thread, drains in-flight frames back to the pool, restarts the thread. The
caller **must** re-prime extradata (SPS/PPS) afterwards so the hardware
decoder can reconfigure its capture plane. Always returns 0. Threading: user
thread only; joins the capture thread internally.

### `nvmpi_decoder_close`

`nvmpi.h:205` -- `int nvmpi_decoder_close(nvmpictx* ctx);`

Stops the capture thread, frees all DMA buffers and pools and destroys the
context; the handle is invalid afterwards. Returns `0`, or
`-1` for a `NULL` ctx (nothing is done). Frames borrowed through `get_frame_fd` and
released after close are handled safely: the release callback checks a shared
"pool alive" flag and skips the pool access. Threading: user thread only.

## Video encoder

Backed by the V4L2 encoder device (NVENC). The V4L2 naming is mirrored relative
to the decoder: raw frames go into the OUTPUT plane, the bitstream comes out of
the CAPTURE plane. NVIDIA's dequeue thread copies each encoded buffer into a
pooled `nvPacket`.

### `nvmpi_create_encoder`

`nvmpi.h:258` -- `nvmpictx* nvmpi_create_encoder(nvEncParam* param);`

Opens the V4L2 encoder, programs profile, level and rate control from `param`,
sets up both planes and starts the capture-plane dequeue thread. Returns a
handle, or `NULL` when no encoder device node exists, the device probe fails
(`ENODEV`, `ENXIO`, `EACCES`, or `EBUSY` on all 3 attempts), the encoder cannot
be created after 3 attempts, or a critical format, plane or stream setup step
fails (the half-built context is released; in the rare case the dequeue thread
cannot be joined it is leaked and `NULL` is still returned).

The caller must still fill the packet pool with `nvmpi_encoder_qEmptyPacket`
before encoded output can be delivered: if no empty packet is available when a
buffer is encoded, that encoded frame is dropped with a warning.

### `nvmpi_encoder_put_frame`

`nvmpi.h:269` -- `int nvmpi_encoder_put_frame(nvmpictx* ctx, nvFrame* frame);`

Copies the frame planes into a V4L2 OUTPUT-plane buffer and queues it. In
blocking mode (default) it blocks waiting for a free buffer. With
`nonblocking=1` it returns `NVMPI_ERR_EAGAIN` when none is free. `frame == NULL`
sends EOS and puts the encoder into flushing mode.

| Return | Meaning |
| --- | --- |
| `0` | Success. |
| `NVMPI_ERR_EAGAIN` (-11) | Non-blocking mode and no OUTPUT buffer is free. |
| `-2` | Already flushing (EOS was sent). |
| `-1` | Encoder is in an error state, or the blocking dequeue failed. |
| other negative | The error code from queueing the buffer on the OUTPUT plane. |

Threading: user thread only.

### `nvmpi_encoder_get_packet`

`nvmpi.h:277` -- `int nvmpi_encoder_get_packet(nvmpictx* ctx, nvPacket** packet, bool wait);`

Dequeues one encoded packet (the pointer is one of the packets you donated).

| `wait` | Flushing | Behaviour |
| --- | --- | --- |
| `false` | no | Non-blocking; `-1` immediately if no packet is ready. |
| `true` | no | Blocks up to `wait_timeout` ms (default 500); `-1` on timeout or shutdown. |
| either | yes (after `put_frame(NULL)`) | Polls until a packet arrives (`0`) or the end-of-stream buffer is seen (`-2`). |

On success the caller owns the packet until it hands it back with
`nvmpi_encoder_qEmptyPacket`. `packet->flags` carries the keyframe flag, `pts`
the timestamp in microseconds. A frame larger than `NVMPI_ENC_CHUNK_SIZE` is
dropped with an error log.

### `nvmpi_encoder_dqEmptyPacket`

`nvmpi.h:281` -- `int nvmpi_encoder_dqEmptyPacket(nvmpictx* ctx, nvPacket** packet);`

Removes one packet from the empty pool; the caller takes ownership. Returns `-1`
when the pool is exhausted and the encoder is not filling a packet. It may
block briefly (capped at 1 s, normally one packet copy) while the encoder
finishes a packet it already took from the pool. Use it to drain and free the
pool before closing: drain this queue until `-1` **before** the filled queue
(`get_packet`), because the in-flight packet ends up filled, and do not call
`nvmpi_encoder_qEmptyPacket` during the drain.

### `nvmpi_encoder_qEmptyPacket`

`nvmpi.h:286` -- `void nvmpi_encoder_qEmptyPacket(nvmpictx* ctx, nvPacket* packet);`

Hands a caller-allocated packet (payload buffer included) to the pool; the
dequeue thread fills it later. The pool stores only the pointer: the caller
remains responsible for eventually freeing the packet. Also use it to give back
packets obtained from `get_packet`.

### `nvmpi_encoder_force_idr`

`nvmpi.h:291` -- `int nvmpi_encoder_force_idr(nvmpictx* ctx);`

Forces an IDR frame for the next queued input (sets
`V4L2_CID_MPEG_MFC51_VIDEO_FORCE_FRAME_TYPE`). May be called at any time while
the encoder runs. Returns `0`, or `-1` on ioctl failure.

### `nvmpi_encoder_set_bitrate`

`nvmpi.h:296` -- `int nvmpi_encoder_set_bitrate(nvmpictx* ctx, unsigned int bitrate);`

Changes the target bitrate mid-stream (updates `V4L2_CID_MPEG_VIDEO_BITRATE`);
takes effect from the next encoded frame. For VBR, `peak_bitrate` is not
adjusted. Returns `0`, or `-1` on failure.

### `nvmpi_encoder_flush`

`nvmpi.h:303` -- `int nvmpi_encoder_flush(nvmpictx* ctx);`

Mid-stream reset: stops the dequeue thread, STREAMOFF on both planes, moves
pending encoded packets back to the empty pool, clears flushing/EOS state,
STREAMON, re-queues capture buffers and restarts the thread. Encoding can
resume immediately. Returns `0`, or a negative value if a STREAMON, queue or
thread restart step fails. The caller should force the next frame to IDR if it
needs a clean random-access point.

### `nvmpi_encoder_close`

`nvmpi.h:310` -- `int nvmpi_encoder_close(nvmpictx* ctx);`

Stops the dequeue thread and frees the context. Packets still held in the
pools are **not** returned to you: drain the pool first with
`nvmpi_encoder_dqEmptyPacket` until `-1`, then `get_packet` until empty, and
free them. Anything left
is discarded with a warning (the memory behind those packets leaks, because
libnvmpi does not own it).

Returns `0`. Returns `-1` for a `NULL` ctx, or if the dequeue thread did not
exit within its bounded join (in that case the context is intentionally
leaked to avoid a use-after-free). Closing an encoder that never received a
frame is supported: close queues an EOS itself so the dequeue thread can exit
(see [internals.md](internals.md#encoder-teardown)).

## JPEG decoder

Synchronous decode on the Tegra NVJPG engine through NVIDIA's `NvJPEGDecoder`.
No V4L2 device and no background thread. Baseline JPEG only; progressive JPEG
is rejected. Output is YUV420.

### `nvmpi_create_jpeg_decoder`

`nvmpi.h:212` -- `nvmpictx* nvmpi_create_jpeg_decoder(void);`

No parameters: the decoder configures itself per frame from the JPEG header.
Returns `NULL` on failure.

### `nvmpi_jpeg_decoder_put_packet`

`nvmpi.h:218` -- `int nvmpi_jpeg_decoder_put_packet(nvmpictx* ctx, nvPacket* packet);`

Decodes one JPEG frame synchronously. `payload_size == 0` signals EOS (no
decode; sets the internal EOS flag). The decoded frame is queued internally.
Returns `0`, or `-1` on decode error or unsupported format (progressive).

### `nvmpi_jpeg_decoder_get_frame`

`nvmpi.h:222` -- `int nvmpi_jpeg_decoder_get_frame(nvmpictx* ctx, nvFrame* frame, bool wait);`

Retrieves one decoded frame into caller-allocated planes. With `wait == true`
it blocks up to 500 ms. Returns `0`, or `-1` when no frame is available
(timeout or EOS).

### `nvmpi_jpeg_decoder_close`

`nvmpi.h:225` -- `int nvmpi_jpeg_decoder_close(nvmpictx* ctx);`

Destroys the context and frees all DMA buffers; the handle is invalid
afterwards.

## JPEG encoder

Synchronous encode on the NVJPG engine through `NvJPEGEncoder`. No V4L2 device,
no dequeue thread, no rate control: quality-based, one frame at a time.

### `nvmpi_create_jpeg_encoder`

`nvmpi.h:235` -- `nvmpictx* nvmpi_create_jpeg_encoder(int quality);`

`quality` is libjpeg style, 1 to 100. The header documents an out-of-range
value as clamped with 85 as the default; pass a value in range to be explicit.
Returns `NULL` on failure. On modules without NVJPG encode capability (for
example Orin Nano) creation returns `NULL`: fall back to a software JPEG
encoder there.

### `nvmpi_jpeg_encoder_put_frame`

`nvmpi.h:240` -- `int nvmpi_jpeg_encoder_put_frame(nvmpictx* ctx, nvFrame* frame);`

Encodes one YUV420 frame. `frame == NULL` signals EOS. The encoded JPEG is
staged internally. Returns `0`, or `-1` on error.

### `nvmpi_jpeg_encoder_get_packet`

`nvmpi.h:246` -- `int nvmpi_jpeg_encoder_get_packet(nvmpictx* ctx, nvPacket* packet);`

Copies the encoded JPEG into `packet->payload`, which the caller must
pre-allocate with enough capacity (note: this is a plain copy into a caller
buffer, not the video encoder's packet pool). `packet->flags` is set to 1
(keyframe) for every frame.

| Return | Meaning |
| --- | --- |
| `0` | Success. |
| `-1` | No packet ready. |
| `-2` | EOS and no packet. |

### `nvmpi_jpeg_encoder_close`

`nvmpi.h:249` -- `int nvmpi_jpeg_encoder_close(nvmpictx* ctx);`

Destroys the context and frees all resources.

## DMA-BUF zero-copy path

These calls exchange frames as DMA-BUF file descriptors instead of copying
pixels through caller memory. Buffers are pitch-linear NV12.

### `nvmpi_decoder_get_frame_fd`

`nvmpi.h:329-332`

```c
int nvmpi_decoder_get_frame_fd(nvmpictx* ctx,
    int *dmabuf_fd, int *width, int *height, int *pitch,
    int64_t *timestamp,
    nvmpi_frame_release_cb *release, void **opaque, bool wait);
```

Zero-copy alternative to `nvmpi_decoder_get_frame`. The fd points at a
pitch-linear NV12/YUV420 buffer already VIC-transformed from the decoder's
block-linear capture plane. You receive the borrowed fd (not `dup`'d), the
buffer geometry, a timestamp in microseconds, and a `release` callback plus
`opaque` cookie.

- The fd is valid only until you call `release(opaque)`. Call it exactly once.
- If the fd must outlive the callback (for example as encoder input), `dup()`
  it first.
- Not releasing starves the decoder's frame pool and stalls decoding.
- `wait` behaves as in `nvmpi_decoder_get_frame`.

Returns `0` on success, `-1` on no frame / timeout (also if the small
bookkeeping allocation fails).

### `nvmpi_encoder_put_frame_fd`

`nvmpi.h:342-344`

```c
int nvmpi_encoder_put_frame_fd(nvmpictx* ctx,
    int dmabuf_fd, int width, int height, int pitch,
    int64_t timestamp);
```

Zero-copy alternative to `nvmpi_encoder_put_frame`. The fd must reference an
NV12 DMA-BUF surface with pitch-linear layout and matching dimensions. The
encoder reads it directly through `V4L2_MEMORY_DMABUF`. Requires
`nvEncParam::use_dmabuf = 1` at creation time; otherwise the call fails.
`dmabuf_fd == -1` signals EOS. Returns `0`, `NVMPI_ERR_EAGAIN` (non-blocking,
no buffer), or a negative value on error or if already flushing.

For fds you allocate yourself and pass to the encoder, use
[`nvmpi_surface_alloc_for_enc`](#nvmpi_surface_alloc_for_enc).

## Surfaces

Helpers to allocate and free pitch-linear NV12 DMA-BUF surfaces. Both
allocators reject zero dimensions and sizes above 8192 in either direction
(`src/nvmpi_surface.cpp`); the dimension limit is not stated in the header.

### `nvmpi_surface_alloc`

`nvmpi.h:352-353` -- `int nvmpi_surface_alloc(unsigned int width, unsigned int height, int *dmabuf_fd);`

Allocates a surface tagged for VIC transform (`NvBufSurfaceTag_VIDEO_CONVERT`).
Use it for surfaces that are VIC source or destination, not for surfaces passed
straight to the encoder's DMABUF plane. Returns `0` and writes the fd, or `-1`
and sets `*dmabuf_fd` to `-1`.

### `nvmpi_surface_alloc_for_enc`

`nvmpi.h:363-364` -- `int nvmpi_surface_alloc_for_enc(unsigned int width, unsigned int height, int *dmabuf_fd);`

Allocates a surface tagged for the encoder (`NvBufSurfaceTag_VIDEO_ENC`) so it
is registered in the encoder's NvMap domain. Use it for surfaces passed to
`nvmpi_encoder_put_frame_fd`. VIC transforms also work on these surfaces.
Returns `0` or `-1`.

### `nvmpi_surface_destroy`

`nvmpi.h:369` -- `int nvmpi_surface_destroy(int dmabuf_fd);`

Destroys a surface from either allocator. Safe with `dmabuf_fd == -1` (no-op).
Returns `0`, or `-1` if the underlying destroy fails.

### `nvmpi_surface_copy_from_dmabuf`

`nvmpi.h:376-378`

```c
int nvmpi_surface_copy_from_dmabuf(int dst_fd, int src_fd,
    unsigned int width, unsigned int height, unsigned int src_pitch);
```

Copies NV12 data from a DMA-BUF fd that is not registered with NvBufSurface
(for example one `dup`'d from a DRM_PRIME decoder output) into a registered
surface, using `mmap`, a CPU copy and DMA-BUF sync. `dst_fd` must come from
`nvmpi_surface_alloc` at matching dimensions; `src_pitch` is the source row
stride in bytes. The header does not document return values beyond success
being 0; treat non-zero as failure.

## VIC transform

The VIC (Video Image Compositor) engine does scaling, block-linear to
pitch-linear conversion and colour conversion in hardware, with no CPU copies.

### `nvmpi_vic_ctx`

`nvmpi.h:386` -- opaque context typedef. **A context must not be shared across
threads**: the VIC session is thread-local, so each thread needs its own
context.

### `nvmpi_vic_create`

`nvmpi.h:392` -- `nvmpi_vic_ctx *nvmpi_vic_create(void);`

Binds the VIC engine for the calling thread (`NvBufSurfTransformSetSessionParams`
on NvUtils, `NvBufferSessionCreate` on the legacy API). Returns `NULL` on
failure.

### `nvmpi_vic_transform`

`nvmpi.h:400-403`

```c
int nvmpi_vic_transform(nvmpi_vic_ctx *ctx,
    int src_fd, int dst_fd,
    unsigned int src_w, unsigned int src_h,
    unsigned int dst_w, unsigned int dst_h);
```

Scales and converts between two NV12 DMA-BUF surfaces (from
`nvmpi_surface_alloc` or decoder output). Returns `0`, or `-1` on error. All
transform calls in the library, including the decoder's own, are serialised by
one process-wide mutex (see [internals.md](internals.md#nvbufsurftransform-mutex-rule)),
so concurrent callers do not run in parallel.

### `nvmpi_vic_close`

`nvmpi.h:407` -- `void nvmpi_vic_close(nvmpi_vic_ctx *ctx);`

Destroys the context and releases the VIC session. Safe with `NULL`.

## Threading summary

| Object | Rule |
| --- | --- |
| Video decoder context | One user thread calls `put_packet`, `get_frame`, `get_frame_fd`, `flush`, `close`. libnvmpi's capture thread runs concurrently and talks to the user thread through a thread-safe pool. |
| Video encoder context | One user thread calls `put_frame*`, `get_packet`, `*EmptyPacket`, `force_idr`, `set_bitrate`, `flush`, `close`. The dequeue thread fills packets concurrently. |
| JPEG contexts | Single thread; everything is synchronous. |
| VIC context | One context per thread. |
| `nvmpi_frame_release_cb` | May be invoked from a different thread than the one that fetched the frame (the pool and counters it touches are thread-safe). |
| Independent contexts | Separate contexts may be used from separate threads. Hardware access is shared, so heavy parallel use is bounded by the engines. |

Blocking behaviour at a glance:

| Call | Blocks when |
| --- | --- |
| `nvmpi_decoder_put_packet` | All OUTPUT buffers are in flight. |
| `nvmpi_decoder_get_frame` / `_fd` | `wait == true`, up to `wait_timeout`. |
| `nvmpi_encoder_put_frame` / `_fd` | Blocking mode and no free OUTPUT buffer. Non-blocking mode returns `NVMPI_ERR_EAGAIN` instead. |
| `nvmpi_encoder_get_packet` | `wait == true` (up to `wait_timeout`), or while flushing. |

## Return code cheat sheet

| Function family | Success | Typical failure |
| --- | --- | --- |
| `create_*` | non-NULL handle | `NULL` |
| `nvmpi_decoder_put_packet` | `0` | `-1` dequeue, `-2` queue, `-3` oversize packet |
| `nvmpi_decoder_get_frame`, `_fd` | `0` | `-1` no frame / timeout |
| `nvmpi_encoder_put_frame`, `_fd` | `0` | `NVMPI_ERR_EAGAIN` (-11) non-blocking, other negative on error |
| `nvmpi_encoder_get_packet` | `0` | `-1` no packet / timeout, `-2` EOS while flushing |
| `nvmpi_encoder_dqEmptyPacket` | `0` | `-1` pool empty |
| `nvmpi_encoder_force_idr`, `_set_bitrate` | `0` | `-1` |
| `*_flush`, `*_close` | `0` | see the per-function notes (close can return `-1`) |
| `nvmpi_jpeg_encoder_get_packet` | `0` | `-1` none ready, `-2` EOS |
| `nvmpi_surface_*`, `nvmpi_vic_transform` | `0` | `-1` |

## Minimal usage

Complete decode and encode snippets are in the [README](../README.md#quick-api-example).
