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

Returned by the encoder submit calls when the frame cannot be taken now: no
OUTPUT-plane buffer is free (non-blocking), or (both modes) the pool has no spare packet for
the frame's encoded output (see `nvmpi_encoder_put_frame`). The frame was not
queued. Drain output packets with `nvmpi_encoder_get_packet`, return each with
`nvmpi_encoder_qEmptyPacket`, and retry the same frame. The value matches Linux
`EAGAIN`. The JPEG decoder in DMA-BUF mode also returns it from
`nvmpi_jpeg_decoder_put_packet` when its pool is empty.

### `NVMPI_ERR_FRAME_SIZE`

`nvmpi.h` -- `#define NVMPI_ERR_FRAME_SIZE (-90)`

The decoded frame does not fit the caller's planes (`nvmpi_decoder_get_frame2`,
`nvmpi_decoder_get_frame`, `nvmpi_jpeg_decoder_get_frame`). Nothing was written
into the caller's memory. Pinned by a static assert.

### `NVMPI_ERR_INVAL`

`nvmpi.h` -- `#define NVMPI_ERR_INVAL (-22)`

NULL argument, `struct_size` too small, or a handle of the wrong kind
(`nvmpi_jpeg_decoder_get_frame_fd`, `nvmpi_decoder_get_frame_fd2`). Matches Linux `EINVAL`. The value is part
of the frozen 4.1.0 ABI.

## Enums

### `nvPixFormat`

`nvmpi.h:43-47` -- raw (uncompressed) pixel layouts, used for decoder output and
encoder input.

| Value | Meaning |
| --- | --- |
| `NV_PIX_NV12` | 8-bit 4:2:0 semi-planar: Y plane plus interleaved UV (2 planes). |
| `NV_PIX_YUV420` | 8-bit 4:2:0 planar: separate Y, U, V planes (3 planes). |
| `NV_PIX_P010` | 10-bit 4:2:0 semi-planar (matches FFmpeg `P010LE`). Decode: HEVC Main10 only, requires the NvUtils buffer API (JetPack 5 and later). Encode: HEVC only, implies Main10 (see `nvEncParam`); H.264 and AV1 reject it. AV1 10-bit was tried (L4T R36.4 on Orin NX: create succeeds, but the first frame fails in the driver (`NvEncNvVicConvertSurfaceFormat failed`, `VENC: NvVideoEncBlockReportError ... BlockSide error 0x4`), so it is refused up front.) |

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
| `NV_VIDEO_CodingAV1` | AV1 | yes, JetPack 6 only (needs `V4L2_PIX_FMT_AV1`) | no |

`nvmpi_create_decoder` maps `NV_VIDEO_CodingH264`, `HEVC`, `MPEG4`, `MPEG2`,
`VP8`, `VP9` and `AV1` to the matching V4L2 format
(`src/nvmpi_dec_codec.cpp`). Any other value (including
`NV_VIDEO_CodingMJPEG` and `NV_VIDEO_CodingUnused`) is logged and makes the
call return `NULL`; it no longer falls back to H.264. Use the dedicated JPEG
API for MJPEG. `NV_VIDEO_CodingAV1` was appended last, so existing enum values
are unchanged. AV1 needs `V4L2_PIX_FMT_AV1` in the L4T headers (JetPack 6);
a build without it returns `NULL` with "AV1 decode not supported by this L4T
build". Feed one temporal unit (low-overhead OBU format, starting with the
temporal delimiter) per `nvmpi_decoder_put_packet` call. AV1 data that
arrives before the first sequence header OBU is discarded (`put_packet`
returns 0, no frame, one INFO log line): NVDEC on the Orin holds such input
and never returns the buffer, which would block `put_packet`. A caller that
keeps the sequence header out of band (MP4/MKV `av1C`) must send the config
OBUs first, as a packet of its own or in front of the first temporal unit.
`nvmpi_decoder_flush` re-arms this gate, so a restarted stream must send its
header again. The video encoder
supports only `NV_VIDEO_CodingH264` and `NV_VIDEO_CodingHEVC`.

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
| `profile` | `unsigned int` | H.264: FFmpeg-style profile id (66 baseline, 77 main, 100 high). HEVC: `NVMPI_HEVC_PROFILE_MAIN` (1, default) or `NVMPI_HEVC_PROFILE_MAIN10` (2); Main10 requires `NV_PIX_P010`. |
| `level` | `unsigned int` | H.264 `level_idc` style value (41 = level 4.1). |
| `bitrate` | `unsigned int` | Target bitrate in bit/s. |
| `peak_bitrate` | `unsigned int` | Peak bitrate for VBR; 0 derives it from `bitrate`. |
| `enableLossless` | `char` | Non-zero: constant QP 0 / High 4:4:4 (H.264 only). |
| `mode_vbr` | `char` | Non-zero: VBR rate control instead of CBR. |
| `insert_spspps_idr` | `char` | Non-zero (the code tests `== 1`): repeat SPS/PPS at every IDR frame. |
| `insert_vui` | `char` | Non-zero: embed VUI timing info (fps) in the bitstream. |
| `pixFormat` | `nvPixFormat` | Raw input layout: `NV_PIX_YUV420`, `NV_PIX_NV12`, or `NV_PIX_P010` (HEVC only; 16-bit little-endian samples, Y plane + interleaved CbCr plane, `linesize` in bytes; implies Main10). `nvmpi_create_encoder()` returns NULL for P010 with H.264 or AV1, and for Main10 without P010. AV1 + P010 is refused before any device is opened: L4T R36.4 on Orin NX: create succeeds, but the first frame fails in the driver (`NvEncNvVicConvertSurfaceFormat failed`, `VENC: NvVideoEncBlockReportError ... BlockSide error 0x4`), so it is refused up front. |
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
| `wait_timeout` | `unsigned int` | Blocking-wait timeout in ms. 0 = 500 ms; non-zero values are clamped to 100..5000 ms (`src/nvmpi_enc_params.cpp`). |
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
| `pts` | `unsigned long` | Presentation timestamp in microseconds. Any 64-bit value round-trips unchanged (see [pts round trip](#pts-round-trip)). |
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
| `timestamp` | `time_t` | Microseconds. Any 64-bit value round-trips unchanged (see [pts round trip](#pts-round-trip)). |

Payload pointers always reference caller-owned memory. The decoder copies into
them (`get_frame`), the encoder copies out of them (`put_frame`); ownership
never changes. `get_frame` fills the planes and `timestamp`; it does not fill
`width`/`height`, so take the geometry from your stream (or `nvDecParam`).

### pts round trip

Any 64-bit pts passed to `nvmpi_decoder_put_packet` (`nvPacket.pts`) or to
`nvmpi_encoder_put_frame` / `nvmpi_encoder_put_frame_fd` (`nvFrame.timestamp`,
`timestamp`) is returned unchanged by `get_frame`, `get_frame_fd`,
`get_frame_fd2` and `get_packet`. This includes `AV_NOPTS_VALUE` (`INT64_MIN`),
other negative values and values up to `INT64_MAX`; values are read as `int64`
(`nvPacket.pts` is `unsigned long`, the same 64 bits).

Why this needs work: libnvmpi carries the pts in the V4L2 buffer timestamp, and
the Tegra kernel stores that as u64 nanoseconds, which holds a microsecond value
exactly only in `0 ..= 9223372036854775` (`INT64_MAX / 1000`, about 292 000
years). libnvmpi passes values in that range (minus a reserved top block of 256)
through unchanged and maps every other value to a reserved sentinel in the block,
keeping the original in a side table of 256 entries that restores it on output.
Normal pts never use the table.

Limits: the table tracks 256 distinct out-of-range values at a time. A repeated
value (a stream of `AV_NOPTS_VALUE`) reuses one entry. When the table is full the
oldest entry is recycled, so a frame still in flight after 256 newer distinct
out-of-range pts returns the newer value. A decoder or encoder holds far fewer
frames in flight.

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

**Buffer size.** The call does not know how big your planes are unless
`payload_size[]` says so. With `payload_size[p]` non-zero for every plane the
frame is copied only if it fits (rules under
[`nvmpi_decoder_get_frame2`](#nvmpi_decoder_get_frame2)). With `payload_size[]`
all zero you promise the planes hold the *expected size*: the resize target
(`nvDecParam.resized`) if set, else the container size (`nvDecParam.width` /
`height`), else the first frame this call delivered; `linesize[p]` must hold a
row, and samples may not be wider than `nvDecParam.pixFormat` implies. A
frame that is larger, has wider samples or a layout that cannot hold a row is
**not copied**: it is dropped and `NVMPI_ERR_FRAME_SIZE` is returned (logged,
rate limited). Smaller frames are copied as before. This call cannot keep the
frame; use `get_frame2` to learn the new size and retry.

| `wait` | Behaviour |
| --- | --- |
| `false` | Non-blocking; returns `-1` immediately when no frame is ready. |
| `true` | Blocks up to `wait_timeout` ms (default 500); returns `-1` on timeout or shutdown. |

| Return | Meaning |
| --- | --- |
| `0` | Success. |
| `-1` | No frame available (non-blocking), or timeout / shutdown (blocking). |
| `NVMPI_ERR_FRAME_SIZE` | The frame did not fit your buffers; dropped, nothing copied. |
| other non-zero | The result of the failed CPU buffer-map call (`NvBufSurfaceMap` on NvUtils, `NvBufferMemMap` on the legacy API), passed through unchanged. The pool buffer is still recycled and `frame->timestamp` is set, but the planes are not valid. |

Threading: user thread only.

### `nvmpi_decoder_get_frame2`

`nvmpi.h` -- `int nvmpi_decoder_get_frame2(nvmpictx* ctx, nvFrame* frame, bool wait);`

`get_frame` with a buffer capacity, for streams whose size can change. Set,
for every plane of the format (2 for NV12/P010, 3 for YUV420): `payload[p]`
non-NULL, `payload_size[p]` = bytes available at `payload[p]` (non-zero) and
`linesize[p]` = the stride you want. A NULL payload or zero capacity returns
`NVMPI_ERR_INVAL` and takes no frame.

A plane fits when `linesize[p]` >= bytes of one row and
`(rows - 1) * linesize[p] + row_bytes <= payload_size[p]` (checked with
overflow guards). All planes must fit or nothing is copied.

| Return | Meaning |
| --- | --- |
| `0` | Copied. `width`, `height`, `type`, `timestamp` are set; `payload_size[p]` = `rows * row_bytes` (packed size). |
| `NVMPI_ERR_FRAME_SIZE` | Nothing copied, no byte of yours written. `width` / `height` / `type` are the frame's, `payload_size[p]` = `rows * row_bytes`. Reallocate (stride >= row bytes) and call again: the **same** frame comes back next, with its timestamp. |
| `NVMPI_ERR_INVAL` | NULL argument, NULL plane or zero capacity. |
| `-1` | No frame (non-blocking), timeout or shutdown. |

The refused frame is held by the decoder until a call succeeds, `flush`
(discards it) or `close`; while held, no newer frame is taken. It occupies one
pool slot meanwhile. `get_frame`, `get_frame_fd` and `get_frame_fd2` deliver a
held frame first. Threading: every `get_frame*` call, `flush` and `close` must
come from one thread at a time (the held frame is not locked).

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
`-1` for a `NULL` ctx (nothing is done). Close never waits for frames borrowed
through `get_frame_fd`: they stay valid until released and are freed by the
last release, which is safe after close and from any thread. Threading: user
thread only.

## Video encoder

Backed by the V4L2 encoder device (NVENC). The V4L2 naming is mirrored relative
to the decoder: raw frames go into the OUTPUT plane, the bitstream comes out of
the CAPTURE plane. NVIDIA's dequeue thread copies each encoded buffer into a
pooled `nvPacket`.

### `nvmpi_create_encoder`

`nvmpi.h:258` -- `nvmpictx* nvmpi_create_encoder(nvEncParam* param);`

Opens the V4L2 encoder, programs profile, level and rate control from `param`,
sets up both planes and starts the capture-plane dequeue thread. Returns a
handle, or `NULL` when `param->codingType` is not `NV_VIDEO_CodingH264` or
`NV_VIDEO_CodingHEVC` (checked first, before any device is opened; logs
"Unsupported encoder codingType N"), when no encoder device node exists, the device probe fails
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

Frame size check (#150): per plane the copy reads `(rows-1)*linesize + row
bytes` from `payload[p]`, with rows and row bytes taken from the encoder's
configured size. When `payload_size[p]` is non-zero it is the size of that
allocation, and a frame that needs more (smaller or cropped buffer) is refused
with `NVMPI_ERR_INVAL`: it is not queued, no OUTPUT buffer is used and the
caller's memory is not read past `payload_size[p]`. `payload_size[p] == 0`
means "not declared": the frame is accepted and the caller guarantees the plane
is large enough. Set `payload_size[]` for every plane you pass. Planes the
format does not use (the third plane of NV12/P010) are ignored.

Back-pressure in both modes (no silent drops): besides the OUTPUT-buffer check,
a frame is refused with `NVMPI_ERR_EAGAIN` (and not queued) while the frames
already in flight would outnumber the pool's empty packets. Each accepted frame
therefore has an empty packet reserved for its encoded output, and the capture
callback never has to discard an access unit. After `EAGAIN` call
`nvmpi_encoder_get_packet` until it reports nothing ready, return every packet
with `nvmpi_encoder_qEmptyPacket`, then retry the same frame. EOS is never
refused for lack of packets. The pool must hold more packets than the encoder
holds frames back (B-frames, reordering); a pool smaller than that produces no
output until EOS. Blocking mode returns `EAGAIN` for the packet condition too
(it still blocks only for a free OUTPUT buffer). It cannot wait for pool
capacity instead: packets free only when the caller reads and returns them,
which the same thread cannot do while blocked in `put_frame`. A blocking caller
that falls more than the pool size behind therefore gets an explicit `EAGAIN`
rather than losing access units (issue #145). Callers that read after every
put, or bound the frames in flight by the pool size, never see it.

| Return | Meaning |
| --- | --- |
| `0` | Success. |
| `NVMPI_ERR_EAGAIN` (-11) | Non-blocking mode and no OUTPUT buffer is free, or (either mode) no empty packet is spare for the frame's output. The frame was not queued. |
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
| `true` | no | `-1` immediately when no accepted frame is still in flight; otherwise waits up to `wait_timeout` ms (default 500) for the in-flight access units; `-1` on timeout or shutdown. |
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

Buffer size: with `payload_size[]` set for every plane the image is copied
only if it fits (rule under
[`nvmpi_decoder_get_frame2`](#nvmpi_decoder_get_frame2)); without it the
planes are assumed to hold the first image this call delivered, with
`linesize[p]` >= bytes per row. An image that does not fit is dropped and
nothing is copied: `width` / `height` / `type` report its size and
`NVMPI_ERR_FRAME_SIZE` is returned. Reallocate and re-submit the image. Smaller
images are copied as before.

### `nvmpi_jpeg_decoder_close`

`nvmpi.h:225` -- `int nvmpi_jpeg_decoder_close(nvmpictx* ctx);`

Destroys the context and frees all DMA buffers; the handle is invalid
afterwards.

### JPEG decoder, DMA-BUF output (`nvmpi_create_jpeg_decoder_fmt`, `nvmpi_jpeg_decoder_get_frame_fd`)

`nvmpictx* nvmpi_create_jpeg_decoder_fmt(nvPixFormat out, int pool_size);`

`int nvmpi_jpeg_decoder_get_frame_fd(nvmpictx* ctx, nvmpi_fd_frame* frame, nvmpi_frame_release_cb *release, void **opaque, bool wait);`

Zero-copy alternative to `nvmpi_jpeg_decoder_get_frame`. `out` is `NV_PIX_NV12`
or `NV_PIX_YUV420`; `pool_size` is 1..64 (0 = 8). Anything else, or a failed
`NvJPEGDecoder` creation, returns `NULL`. Needs the NvUtils buffer API (legacy
`nvbuf_utils` builds return `NULL`). Use only `put_packet`,
`get_frame_fd` and `close` on the handle; the copying `get_frame` returns -1.
The legacy `nvmpi_create_jpeg_decoder` is unchanged.

Per packet: libnvjpeg decodes to its own block-linear buffer, the VIC converts
it into a pool buffer (pitch-linear, full range, 4:2:2 and 4:4:4 included),
and the pool buffer is queued. The libnvjpeg buffer is reused by its next
decode, so it is never handed out. Grayscale, CMYK and progressive JPEGs are
rejected (`put_packet` returns -1). `put_packet` returns `NVMPI_ERR_EAGAIN`
before decoding, with the packet not consumed, when every pool buffer is
borrowed or queued; release frames and retry. A change of picture size starts
a new pool; frames of the old size stay valid.

`nvmpi_fd_frame` (frozen ABI, fields only appended): `struct_size` (set to
`sizeof` before the call, minimum `NVMPI_FD_FRAME_V1_SIZE`), `fd`, `width`,
`height`, `format`, `num_planes` (2 or 3), `pitch[3]`, `offset[3]`, `size`,
`timestamp` (the packet `pts`), `flags` (bit0 full range). Plane addresses are
`offset[i]`, never `pitch*height`. `width`/`height` are the displayed JPEG
size from the SOF header; the planes may be larger, so address pixels only
through `pitch[]`/`offset[]` and `size` (total buffer bytes).

Ownership and threads:

- The fd belongs to the pool and is not dup'd (`NvBufSurfaceFromFd` fails on
  dup'd fds). Do not `close()` it. `dup()` it before releasing to keep it.
- Call the release callback exactly once per frame, from any thread, before or
  after `close`. After `close` every borrowed fd stays valid until its release;
  `close` never blocks or fails because of borrows (each pool generation is
  reference counted; the surfaces are destroyed by the last release).
- `put_packet` and `get_frame_fd` run on one thread at a time.
- Pool fds are `FD_CLOEXEC`. A forked child must not use the context.
- Returns 0; -1 no frame (non-blocking), 500 ms timeout or closed;
  `NVMPI_ERR_INVAL` for bad arguments.

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

Frame size check (#151): per plane the copy reads `(rows-1)*linesize + row
bytes` from `payload[p]` (rows and row bytes of the YUV420 frame of
`width` x `height`). A `linesize` below the row bytes, or a non-zero
`payload_size[p]` smaller than that span, is refused with `NVMPI_ERR_INVAL`
before anything is read or encoded. `payload_size[p] == 0` means "not
declared": the frame is accepted and the caller guarantees the plane is large
enough. Set `payload_size[]` for all three planes.

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
- The release may come from any thread, before or after `nvmpi_decoder_close`.
  After close, or after a mid-stream resolution change, a borrowed buffer
  stays valid until its release (a resolution change rebuilds the pool at the
  new size without waiting); the buffers of that pool generation are freed
  when its last borrow is released.
- `width`, `height` and `pitch` describe the borrowed buffer itself, so a
  frame decoded before a resolution change keeps its original geometry.
- `wait` behaves as in `nvmpi_decoder_get_frame`.

Returns `0` on success, `-1` on no frame / timeout (also if the small
bookkeeping allocation fails).

### `nvmpi_decoder_get_frame_fd2`

```c
int nvmpi_decoder_get_frame_fd2(nvmpictx* ctx, nvmpi_fd_frame* frame,
    nvmpi_frame_release_cb *release, void **opaque, bool wait);
```

Same borrow as `nvmpi_decoder_get_frame_fd` (one shared internal path and
release callback), but returns the per-plane geometry in an `nvmpi_fd_frame`
(see the JPEG decoder section for the struct). Set `frame->struct_size` to
`sizeof(nvmpi_fd_frame)` (minimum `NVMPI_FD_FRAME_V1_SIZE`); the library fills
`min(struct_size, sizeof)` bytes and writes that size back.

| Field | Value |
| --- | --- |
| `fd` | Borrowed pitch-linear dmabuf fd, not `dup`'d. |
| `width`, `height` | Output size of the frame's pool generation (crop or resize target); the planes may be larger. |
| `format` | The decoder output format: `NV_PIX_NV12` / `NV_PIX_P010` have 2 planes, `NV_PIX_YUV420` has 3. |
| `num_planes`, `pitch[]`, `offset[]`, `size` | From the allocator's plane parameters (`dataSize`, falling back to the end of the last plane). `offset[1]` is the real plane start (page aligned), never `pitch*height`. |
| `timestamp` | As in the copying path, microseconds. |
| `flags` | 0: the VIC destination surfaces are limited range. |

Ownership, release callback and the close and resolution-change guarantees are
those of `nvmpi_decoder_get_frame_fd`. The v1 call is unchanged, and its
`pitch` equals `pitch[0]` here.

Returns `0` on success, `-1` on no frame, timeout or shutdown (also if the
bookkeeping allocation fails), `NVMPI_ERR_INVAL` for NULL arguments or a
too-small `struct_size` (no frame is consumed).

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
`dmabuf_fd == -1` signals EOS. Returns `0`, `NVMPI_ERR_EAGAIN` (no spare packet in
either mode, or non-blocking and no buffer; same back-pressure rule as `nvmpi_encoder_put_frame`), or a negative value on error or if already flushing.

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
| `nvmpi_encoder_put_frame` / `_fd` | Blocking mode and no free OUTPUT buffer. Non-blocking mode returns `NVMPI_ERR_EAGAIN` instead. Neither mode waits for packet-pool capacity: that returns `NVMPI_ERR_EAGAIN` in both. |
| `nvmpi_encoder_get_packet` | `wait == true` (up to `wait_timeout`), or while flushing. |

## Return code cheat sheet

| Function family | Success | Typical failure |
| --- | --- | --- |
| `create_*` | non-NULL handle | `NULL` |
| `nvmpi_decoder_put_packet` | `0` | `-1` dequeue, `-2` queue, `-3` oversize packet |
| `nvmpi_decoder_get_frame`, `_fd` | `0` | `-1` no frame / timeout |
| `nvmpi_encoder_put_frame`, `_fd` | `0` | `NVMPI_ERR_EAGAIN` (-11) non-blocking or no spare packet, other negative on error |
| `nvmpi_encoder_get_packet` | `0` | `-1` no packet / timeout, `-2` EOS while flushing |
| `nvmpi_encoder_dqEmptyPacket` | `0` | `-1` pool empty |
| `nvmpi_encoder_force_idr`, `_set_bitrate` | `0` | `-1` |
| `*_flush`, `*_close` | `0` | see the per-function notes (close can return `-1`) |
| `nvmpi_jpeg_decoder_get_frame_fd` | `0` | `-1` no frame / timeout / closed, `NVMPI_ERR_INVAL` |
| `nvmpi_jpeg_decoder_put_packet` (fd mode) | `0` | `-1` error, `NVMPI_ERR_EAGAIN` pool empty |
| `nvmpi_jpeg_encoder_get_packet` | `0` | `-1` none ready, `-2` EOS |
| `nvmpi_surface_*`, `nvmpi_vic_transform` | `0` | `-1` |

## Minimal usage

Complete decode and encode snippets are in the [README](../README.md#quick-api-example).
