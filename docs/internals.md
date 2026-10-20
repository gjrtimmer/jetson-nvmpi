# libnvmpi internals

Developer documentation: how libnvmpi is organised, how the decoder and
encoder pipelines work, which threads exist, the teardown rules, and the
Tegra driver pitfalls the code is written around. Public API semantics are in
[api.md](api.md).

Contents:

- [Source layout](#source-layout)
- [Naming and file rules](#naming-and-file-rules)
- [V4L2 model](#v4l2-model)
- [Decoder pipeline](#decoder-pipeline)
- [Encoder pipeline](#encoder-pipeline)
- [JPEG paths](#jpeg-paths)
- [Buffer pool](#buffer-pool)
- [Frame buffers](#frame-buffers)
- [Threads and synchronisation](#threads-and-synchronisation)
- [Encoder teardown](#encoder-teardown)
- [Decoder teardown and flush](#decoder-teardown-and-flush)
- [NvBufSurfTransform mutex rule](#nvbufsurftransform-mutex-rule)
- [Tegra V4L2 driver pitfalls](#tegra-v4l2-driver-pitfalls)
- [Buffer API shim and version guards](#buffer-api-shim-and-version-guards)
- [Resource lifecycle contracts](#resource-lifecycle-contracts)
- [Coding guidelines](#coding-guidelines)

## Source layout

```text
include/
  nvmpi.h                  public C API (the only installed header)
  NVMPI_bufPool.hpp        thread-safe two-queue buffer pool (header-only template)
  nvmpi_frame_buffer.hpp   one DMA frame buffer
  nvmpi_log.h              NVMPI_LOG / NVMPI_LOG_SUB logging macros
  nvUtils2NvBuf.h          compile-time shim: legacy nvbuf_utils names -> NvBufSurface
src/
  nvmpi_dec_init.cpp       nvmpi_create_decoder, VIC parameters
  nvmpi_dec_codec.cpp      codingType to V4L2 fourcc mapping
  nvmpi_dec_pool.cpp       destination frame-pool generations (alloc, retire)
  nvmpi_dec_api.cpp        put_packet, get_frame
  nvmpi_dec_fd.cpp         get_frame_fd, get_frame_fd2, borrow/release
  nvmpi_fd_geometry.cpp    plane geometry of nvmpi_fd_frame (decoder + JPEG)
  nvmpi_dec_close.cpp      flush, close
  nvmpi_dec_capture.cpp    capture thread loop, resolution-change handling
  nvmpi_dec_planes.cpp     CAPTURE-plane alloc/teardown, colour format selection
  nvmpi_dec_internal.h     decoder context struct, macros
  nvmpi_timing.h           NVMPI_TIMING phase timer (create/close latency)
  nvmpi_enc_init.cpp       nvmpi_create_encoder (device open, format, planes)
  nvmpi_enc_input.cpp      put_frame (memory copy), frame copy helper
  nvmpi_enc_input_fd.cpp   put_frame_fd (DMA-BUF)
  nvmpi_enc_api.cpp        packet pool ops, get_packet, force_idr, set_bitrate, flush, close
  nvmpi_enc_output.cpp     capture-plane DQ callback, OUTPUT-plane DMA setup
  nvmpi_enc_internal.h     encoder context struct, macros
  nvmpi_dec_jpeg*.{cpp,h}  JPEG decoder (NvJPEGDecoder, synchronous)
  nvmpi_enc_jpeg*.{cpp,h}  JPEG encoder (NvJPEGEncoder, synchronous)
  nvmpi_surface.cpp        surface alloc/destroy/copy (DMA-BUF)
  nvmpi_vic.cpp            standalone VIC transform API + global transform mutex
  nvmpi_frame_buffer.cpp   frame buffer alloc/destroy
stubs/                     link-only stub libraries (see build.md)
vendor/mmapi/              vendored NvV4l2ElementPlane.cpp with pthread_join guard
```

NVIDIA's sample classes (`NvVideoDecoder`, `NvVideoEncoder`, `NvBuffer`,
`NvV4l2Element`, `NvJpegDecoder`, `NvJpegEncoder`, ...) are not vendored: the
build compiles them straight from the host's
`${JETSON_MULTIMEDIA_API_DIR}/samples/common/classes` (except
`NvV4l2ElementPlane.cpp`, see [build.md](build.md#vendored-nvv4l2elementplane)).

## Naming and file rules

- Source files follow `nvmpi_{dec,enc}_{concern}.cpp`, with `_internal` appended
  for private headers (for example `nvmpi_dec_jpeg_internal.h`). Do not use
  `nvmpi_{codec}{dec,enc}` or other orderings.
- Internal headers (`*_internal.h`) hold the context struct and forward
  declarations. They live in `src/` and are not installed.
- `include/nvmpi.h` is the only installed header, so internal refactors cannot
  break the ABI. Never add public signatures anywhere else.
- Each `.cpp` must be independently compilable; no circular includes.
- `CMakeLists.txt` lists sources explicitly (no globs).
- Split a file when it grows past roughly 500 lines or gains a new logical
  concern (a new thread, a new V4L2 device interaction). When adding a feature:
  find the concern it touches, edit that file, and only create a new
  `nvmpi_{codec}_{concern}.cpp` (listed in `CMakeLists.txt`) if it fits none.

## V4L2 model

Both codecs are V4L2 memory-to-memory devices with two queues. The naming is
from the device's point of view, so it is mirrored between the two directions:

| | OUTPUT plane (host to device) | CAPTURE plane (device to host) |
| --- | --- | --- |
| Decoder | compressed bitstream (USERPTR buffers sized `chunk_size`) | decoded frames (block-linear) |
| Encoder | raw YUV frames | encoded bitstream |

libnvmpi drives them through NVIDIA's `NvVideoDecoder` / `NvVideoEncoder`
sample classes. The decoder device node is `/dev/nvhost-nvdec` or
`/dev/v4l2-nvdec`; creation retries the factory up to three times.

## Decoder pipeline

```text
user thread                                  capture thread
-----------                                  --------------
nvmpi_create_decoder
  open device, subscribe to
  V4L2_EVENT_RESOLUTION_CHANGE,
  configure OUTPUT plane, STREAMON,
  spawn capture thread  ------------------>  wait for events
nvmpi_decoder_put_packet                      on RESOLUTION_CHANGE:
  memcpy payload -> OUTPUT buffer,              rebuild CAPTURE plane
  pts -> V4L2 timestamp, qBuffer                allocate frame pool (pitch-linear)
                                              loop:
                                                dqBuffer (CAPTURE, block-linear)
                                                VIC transform -> pool buffer
                                                  (scale if `resized`)
                                                timestamp copied to the buffer
                                                qFilledBuf(buffer)
nvmpi_decoder_get_frame
  dqFilledBuf -> copy planes to caller
  (or get_frame_fd: hand out the fd)
  recycle buffer to the empty queue
```

Details worth knowing:

- **Lazy CAPTURE setup.** Nothing about the output geometry is known at create
  time. The first V4L2 resolution-change event (fired once the decoder has
  parsed the stream headers) triggers `respondToResolutionEvent()` on the
  capture thread, which queries format and crop, rebuilds the CAPTURE plane and
  (re)allocates the destination frame pool with `frame_pool_size` buffers at
  the output resolution (the `resized` target if set, else the stream size).
  The same path handles mid-stream resolution changes. `nvDecParam.width/height`
  are hints that let the pool be sized earlier.
- **Two buffer sets.** The decoder writes block-linear CAPTURE buffers; VIC
  converts into a separate pitch-linear pool (NV12, planar YUV420, or
  NV12 10-bit for P010 on NvUtils). Only the pool is ever seen by callers.
- **Colour.** The decoder-reported colourspace and quantisation range select the
  matching NV12 colorimetry variant (BT.601/709/2020, standard or extended
  range) so VIC interprets colours correctly (`nvmpi_dec_planes.cpp`).
- **Input bound.** `put_packet` rejects a packet larger than `chunk_size`
  (`-3`) before it dequeues anything, since copying would overflow the V4L2
  plane buffer. Input buffers are first handed out by index, then recycled with
  a blocking `dqBuffer`.
- **EOS.** A zero-size packet is queued like any other, and flips the context's
  atomic `eos` flag.
- **Pool generations.** The destination pool is a refcounted generation
  (`dec_gen`, the same model as the JPEG fd path): buffers of one output size,
  the plane geometry of those buffers, and a `dec_slot` per buffer. Whoever
  holds a slot holds a reference to its generation (the slot's own `self`
  while queued or owned by the capture thread, the borrow cookie while
  borrowed). A resolution change (capture thread) or close (after the join)
  retires the generation: it is marked not-current, both queues are drained,
  unborrowed buffers are destroyed at once, and the context drops its
  reference. Borrowed buffers are destroyed by the last release, on any thread,
  possibly after close. Nothing waits.
- **Rebuild check.** On a resolution event the new output size is compared with
  the current generation's own size; the pool is rebuilt only if it differs
  (or there is no usable generation). Frames queued at the old size are
  dropped.
- **Release.** Release takes the generation reference, then under the
  generation's mutex requeues the slot only if the generation is still
  current; the reference is dropped outside the lock. Lock order: generation
  mutex, then one pool queue lock; never the reverse.
- **Zero-copy.** `get_frame_fd` returns the pool buffer's fd plus a release
  callback; geometry comes from the slot's generation, so frames from before a
  resolution change keep their size.

## Encoder pipeline

```text
user thread                                  DQ thread (NVIDIA capture-plane thread)
-----------                                  ---------------------------------------
nvmpi_create_encoder
  open device, set format/profile/level/
  rate control/GOP, set up planes,
  STREAMON, start DQ thread  -------------->  waits on CAPTURE plane
nvmpi_encoder_qEmptyPacket(pkt) xN  (caller donates packets)
nvmpi_encoder_put_frame / _fd
  copy (or map dmabuf) into an OUTPUT
  buffer, qBuffer (blocks, or EAGAIN if
  nonblocking)                                encoder_capture_plane_dq_callback:
                                                zero-size buffer? -> EOS, stop thread
                                                read keyframe metadata
                                                take an empty packet, copy bitstream,
                                                  set pts/flags, publish as filled
                                                re-queue the CAPTURE buffer
nvmpi_encoder_get_packet
  take a filled packet (caller owns it)
nvmpi_encoder_qEmptyPacket(pkt)  (give it back)
```

Details:

- **Packets are caller memory.** The pool stores pointers only. If the callback
  finds the empty queue exhausted, it logs a warning and drops that encoded
  frame; a frame bigger than `NVMPI_ENC_CHUNK_SIZE` is dropped with an error.
  Size the pool for the encoder's in-flight depth.
- **Input paths.** Default is `V4L2_MEMORY_MMAP` with a line-by-line copy
  (caller stride differs from the hardware stride). With `use_dmabuf`, the
  OUTPUT plane is set up for `V4L2_MEMORY_DMABUF` and
  `nvmpi_encoder_put_frame_fd` maps the caller's fd. The DQ thread is always
  started, in blocking and non-blocking mode alike.
- **Keyframe flag.** Taken from V4L2 encode metadata rather than flagging every
  packet.
- **Blocking vs non-blocking submit.** Both walk the OUTPUT buffers by index
  first; once all are used they dequeue one. In non-blocking mode a failed
  dequeue becomes `NVMPI_ERR_EAGAIN`.
- **Flushing state.** `put_frame(NULL)` (or `put_frame_fd(-1)`) sets the
  atomic `flushing` flag and queues the zero-size EOS buffer. While flushing,
  `get_packet` polls every 1 ms until a packet arrives or `capPlaneGotEOS` is
  set (`-2`).
- **Rate control and tuning.** Applied once at create time from `nvEncParam`;
  `force_idr` and `set_bitrate` are runtime V4L2 controls and may be called
  while the DQ thread runs.

## JPEG paths

The JPEG decoder and encoder use `NvJpegDecoder::decodeToFd()` and
`NvJpegEncoder::encodeFromFd()` on the Tegra NVJPG engine. They are
fundamentally different from the V4L2 codecs:

| | JPEG decoder | JPEG encoder |
| --- | --- | --- |
| Threads | none, decode is synchronous in `put_packet` | none, encode is synchronous in `put_frame` |
| Context | its own struct (not the video `nvmpictx`) to avoid ODR collisions | its own struct |
| Format | baseline DCT only; progressive (SOF2) is detected by a marker scan and rejected before the hardware call | quality based (libjpeg 1 to 100), no rate control |
| Buffers | `decodeToFd` yields block-linear output, converted by VIC to pitch-linear YUV420 | the input frame is copied into a DMA buffer and encoded from there |
| Dimensions | taken from each decoded frame, no resolution events | from the frame |
| Hardware | NVJPG | NVJPG encode (absent on Orin Nano: create returns `NULL`) |

Files: `nvmpi_dec_jpeg*.cpp/.h`, `nvmpi_enc_jpeg*.cpp/.h`. The JPEG decoder's VIC
conversion goes through the same global transform mutex as everything else.

## Buffer pool

`NVMPI_bufPool<T>` (`include/NVMPI_bufPool.hpp`) is a header-only
producer/consumer pool used for decoder frame buffers and encoder packets. It
has two independent FIFO queues, "empty" and "filled", each with its own mutex
so a producer and consumer do not contend on the same lock.

| API | Notes |
| --- | --- |
| `qEmptyBuf` / `dqEmptyBuf` | Return / take an empty item (non-blocking `dq`, plus a timed variant). |
| `qFilledBuf` / `dqFilledBuf` | Publish / take a filled item. `dqFilledBuf(timeout)` blocks on a condition variable. |
| `dqEmptyBufHold` / `releaseHold` | Encoder DQ thread: take an empty item and count it held until it is published (`qFilledBuf`) or returned; `releaseHold` comes after. |
| `dqEmptyBufSettled(timeout)` | Teardown drain: like `dqEmptyBuf`, but waits while the queue is empty and an item is held; `NULL` once nothing is queued or held. |
| `shutdown()` | Sets a flag and wakes all blocked consumers; they return `NULL` immediately. |
| `reset()` | Clears the shutdown flag for flush and restart cycles. |

Rules and facts:

- `m_emptyBuf` guards the empty queue and its condition variable;
  `m_filledBuf` guards the filled queue and its condition variable.
  **Never hold both at once** (deadlock). `m_shutdown` is atomic, written under
  `m_filledBuf` so the wake-up is visible to waiters.
- Blocking waits wake every 100 ms at most to re-check `m_shutdown`, so even a
  missed notification costs at most 100 ms.
- The blocking wait ceiling is the context's `wait_timeout` (default 500 ms;
  decoder range 50 to 5000 ms, encoder clamps to 100 to 5000 ms).
- `reset()` must be called between `shutdown()` and the next blocking dequeue,
  otherwise the next capture thread's frames would be ignored (flush does this).
- The pool never owns its items: deleting it does not free what is inside.
  Callers drain both queues first. The destructor warns if items remain.

## Frame buffers

`nvmpi_frame_buffer` (`include/nvmpi_frame_buffer.hpp`,
`src/nvmpi_frame_buffer.cpp`) wraps one hardware DMA frame buffer: the dmabuf
fd (`-1` when unallocated), on NvUtils also the `NvBufSurface*` resolved from the
fd (a view, not separately owned), and the timestamp in microseconds. `alloc()`
takes a creation-parameter struct and leaves the object unallocated on failure;
`destroy()` is idempotent (checks `fd >= 0` and resets the fields). Instances
circulate through `NVMPI_bufPool<nvmpi_frame_buffer*>` between the capture
thread and the user thread.

## Threads and synchronisation

Decoder threads:

- **User thread**: the public API calls (one thread per context).
- **Capture thread** (`dec_capture_loop_fcn`, named `dec_capture`): lives for the
  lifetime of the context (restarted by flush). Dequeues decoded buffers, VIC
  transforms them, publishes frames. On every exit path (normal EOS, event or
  dequeue error, external EOS) it calls the pool's `shutdown()` so no consumer
  can block forever.

Encoder threads:

- **User thread**: `put_frame*`, `get_packet`, packet pool ops, controls.
- **DQ thread**: NVIDIA's capture-plane dequeue thread
  (`startDQThread` / `stopDQThread` / `waitForDQThread`), calling
  `encoder_capture_plane_dq_callback`.

| Primitive | Where | Protects |
| --- | --- | --- |
| `m_filledBuf` mutex + `cv_filledBuf` | `NVMPI_bufPool` | filled queue and blocking waits |
| `m_emptyBuf` mutex + `cv_emptyBuf` | `NVMPI_bufPool` | empty queue |
| `m_shutdown` (`std::atomic<bool>`) | `NVMPI_bufPool` | shutdown signal to blocked consumers |
| `eos` (`std::atomic<bool>`) | decoder context | end of stream between user and capture thread |
| `flushing`, `capPlaneGotEOS` (`std::atomic<bool>`) | encoder context | flushing state, EOS seen by the DQ thread |
| `nvmpi_transform_mutex` (`pthread_mutex_t`) | `nvmpi_vic.cpp` | every `NvBufSurfTransform` call |

Deadlock prevention in short: no function holds more than one pool mutex;
timed waits re-check shutdown every 100 ms; the capture thread always shuts
the pool down before exiting; close sets `eos`, calls `shutdown()`, stops the
stream, and only then joins.

## Encoder teardown

`nvmpi_encoder_close` enforces the thread-before-resource rule: the DQ thread
must be gone before any plane, fd or the encoder object is touched. The
sequence is:

1. Shut the packet pool down, which wakes any thread blocked in
   `get_packet(wait=true)` (needed for early termination such as `-t`).
2. `nvmpi_enc_stop_dq_thread`:
   1. **Send EOS first**, unless the caller already did or the thread already saw
      it, and only if an OUTPUT slot is still unused (so the submit path takes
      the slot directly and cannot block on a stuck encoder).
   2. Wait up to about 1 s for the DQ callback to observe the EOS buffer.
   3. `stopDQThread()` (flag only), best-effort `STREAMOFF` on CAPTURE, then
      `waitForDQThread(1000)`.
   4. If the join times out, close returns `-1` and **leaks the context on
      purpose**: destroying the encoder under a running thread would be a
      use-after-free.
3. `STREAMOFF` the OUTPUT plane, unmap and destroy self-allocated OUTPUT-plane
   dmabufs, free the fd array, release the `NvVideoEncoder`.
4. Safety-net drain of undrained packets (logged as a warning), then free the
   pool and the context.

**Why EOS before the join.** `stopDQThread()` only sets a flag. On Tegra,
neither it nor `STREAMOFF` reliably unblocks a `dqBuffer()` / `poll()` that is
waiting for encoded data. If the encoder never received a frame, the thread
blocks forever and the join times out. The only reliable wake-up is the one
NVIDIA's own `01_video_encode` sample uses: queue an OUTPUT buffer with
`bytesused == 0`; the encoder returns a zero-size CAPTURE buffer, the callback
sets `capPlaneGotEOS` and returns `false`, and the thread exits on its own.
This fixed a segfault when closing an encoder that never encoded a frame.
The regression guard is the create/close test in `test/api/test_api.c`
(strict mode on the Jetson runner).

**Caller-side drain and the held packet.** Before `close`, the wrapper drains
the pool while the DQ thread may still be running. The callback takes a packet
from the empty queue and publishes it to the filled queue a moment later; in
between it sits in neither queue. A plain non-blocking drain could miss it and
close would then discard it (leak). The callback therefore takes packets with
`dqEmptyBufHold()` and calls `releaseHold()` after publishing, and
`nvmpi_encoder_dqEmptyPacket` uses `dqEmptyBufSettled(1000 ms)`: once it
returns `-1` nothing is queued or in flight, so draining empty, then filled,
misses nothing. The wait ends on the release notification (one packet copy);
the 1 s is only a backstop.

`nvmpi_encoder_flush` uses the same ordering for its own stop and restart (stop
the DQ thread, STREAMOFF both planes, drain, STREAMON, re-queue CAPTURE
buffers, restart the thread).

## Decoder teardown and flush

`nvmpi_decoder_close` order (each step depends on the previous):

1. Set `eos` and shut the frame pool down (unblocks a `get_frame` caller that is
   waiting; otherwise it would sit until its timeout while close waits).
2. `STREAMOFF` CAPTURE and join the capture thread (guarantees no transform is
   in flight).
3. Legacy API only: destroy the VIC session.
4. Stop the OUTPUT plane and release its buffers; release the CAPTURE-plane
   DMA buffers.
5. Clear the shared "pool alive" flag **before** destroying the pool, so late
   release callbacks (which can arrive after close in some consumers) skip the
   pool access.
6. Destroy the frame pool and its buffers, delete the pool, the decoder device
   and the context.

`nvmpi_decoder_flush` stops and joins the capture thread, stops the OUTPUT
plane, moves filled frames back to the empty queue, `reset()`s the pool,
restarts the OUTPUT plane and spawns a new capture thread. That thread waits
for the next resolution-change event, which only fires after the caller primes
the decoder again with SPS/PPS, hence the "re-prime after flush" rule in the API.

### Close latency and `NVMPI_TIMING`

The join in step 2 waits for the capture thread, so close latency is bounded by
what that thread is blocked in. Before the first resolution-change event it
polls `dqEvent` in 50 ms slices and re-checks `eos` between slices, so a
create-then-close without data returns in about one slice (50-75 ms). It used
to wait out a 500 ms `dqEvent` plus the 500 ms race guard below, 0.5-1.5 s per
cycle (issue #105).

The race guard (an extra 500 ms `dqEvent` when the loop ended on `eos` without
an event) exists because FFmpeg 7.0+ may queue the EOS marker before the driver
emits the event for re-primed extradata. It runs only when a packet was ever
queued to the OUTPUT plane (`packets_put`, never reset by flush): without data
the driver cannot emit an event, so there is nothing to wait for. Teardown after
data is unchanged.

Setting `NVMPI_TIMING=1` in the environment prints per-phase timings to stderr
(`[libnvmpi][timing]: <scope> phase=<name> ms=<n>`), independent of the log
level: every `nvmpi_create_decoder` phase (`dec_create`), the capture thread's
wait for the first event or `eos` (`dec_capture`), and `nvmpi_decoder_close`
from `eos` set to join return and in total (`dec_close`). The value is read
once; unset, it costs one cached-bool branch per phase. The `test:api:hw` CI
job enables it.

## NvBufSurfTransform mutex rule

NVIDIA documents the transform session parameters as thread-local. In practice,
**concurrent `NvBufSurfTransform` calls from two threads deadlock the Tegra
driver** (for example the decoder capture thread and a filter thread in the
consuming application).

The fix is a single process-wide `pthread_mutex_t nvmpi_transform_mutex`,
defined in `src/nvmpi_vic.cpp` and wrapped around **every**
`NvBufSurfTransform` call: in `nvmpi_dec_capture.cpp`, `nvmpi_dec_jpeg.cpp` and
`nvmpi_vic.cpp`. **Any new code that calls `NvBufSurfTransform` must lock this
mutex.** The VIC context also deliberately uses VIC compute (not GPU compute):
the GPU path needs an initialised CUDA context and aborted on JetPack 6 when it
was missing.

## Tegra V4L2 driver pitfalls

Verified on Orin; the code is written around each of these.

- **`DevicePoll()` hangs after EOS.** The driver does not unblock
  `V4L2_CID_MPEG_VIDEO_DEVICE_POLL` after the final capture buffer is drained
  post-EOS. NVIDIA's `00_video_decode` sample works around it with a dedicated
  poll thread, a semaphore and `SetPollInterrupt()`, which is too invasive for
  libnvmpi. Do not use `DevicePoll()` in the decoder capture loop: use
  non-blocking `dqBuffer` plus a short sleep.
- **`NvV4l2Element::fd` is protected.** The V4L2 file descriptor cannot be
  reached from outside the class hierarchy, so a plain `poll()` on the decoder
  or encoder fd is not possible.
- **`NvBufSurfTransform` deadlocks on concurrent calls.** See the mutex rule above.
- **`NvBufSurfaceFromFd` fails on `dup`'d fds.** `NvBufSurface` keeps an internal
  table mapping fd to metadata. `dup()` creates a new fd that is not
  registered, so `NvBufSurfaceFromFd(dup_fd)` fails. A DRM_PRIME-style handoff
  that `dup`s the decoder fd must therefore copy the frame into a freshly
  allocated, registered surface (`nvmpi_surface_copy_from_dmabuf`) before
  calling `NvBufSurfTransform`.
- **Encoder DQ thread blocks with no input.** `stopDQThread()` and `STREAMOFF`
  do not wake it; see [Encoder teardown](#encoder-teardown).
- **Device access collisions crash instead of returning `EBUSY`.** The driver
  segfaults when two processes fight for the same NVDEC/NVENC engine. Do not run
  hardware tests concurrently with other hardware workloads on the same device.
- **Rapid open/close cycles are stochastically unstable** (notably HEVC, around
  100+ iterations). Treat crashes in such stress loops with care: they cannot
  always be told apart from code defects. The API smoke test uses 50 cycles.
- **`-stream_loop -1` style seeks with hardware decoders** (an FFmpeg-level
  behaviour that triggers a seek and decoder re-init) can crash some driver
  versions; prefer re-invoking the program in a loop for soak tests.
- **glibc 2.34 `pthread_join` on a zero `pthread_t`.** Handled by the vendored
  plane class, see [build.md](build.md#vendored-nvv4l2elementplane).

## Buffer API shim and version guards

NVIDIA ships two buffer APIs: legacy `nvbuf_utils` (JetPack 4 and earlier) and
NvUtils / `NvBufSurface` (JetPack 5 and later). `CMakeLists.txt` probes for
`nvbufsurface.h` and defines `WITH_NVUTILS` when present (see
[build.md](build.md#buffer-api-auto-detection)).

`include/nvUtils2NvBuf.h` maps the legacy `NvBuffer*` names onto their
`NvBufSurf*` equivalents when `WITH_NVUTILS` is set, so most of `src/` is
written once against the legacy spelling. It only covers 1:1 renames: calls with
genuinely different signatures (transform, memory map and sync, surface
destroy details) still need explicit `#ifdef WITH_NVUTILS` branches at the call
sites. Guidelines:

- Every `#ifdef WITH_NVUTILS` block carries a comment saying which API
  difference it handles and which JetPack generations it affects.
- Features that exist only on NvUtils are gated at init: P010 (10-bit) decode is
  rejected on legacy builds, and the 10-bit colour constants are compiled out.
- The legacy path is supported by the code but is not known to be exercised by
  the project's CI: which API a CI build uses depends on the headers in its
  builder image, and the configure log line (`Using NvUtils API.` or
  `Using NvBufUtils API.`) tells you which one a given build took.
- The JetPack 5 build in CI exists to catch V4L2 header constants and symbols
  that exist on JetPack 6 (kernel 5.15 and later) but not on JetPack 5
  (kernel 5.10).

## Resource lifecycle contracts

| Resource | Allocated | Freed | Rule |
| --- | --- | --- | --- |
| Decoder context | `nvmpi_create_decoder` | `nvmpi_decoder_close` | Join the capture thread first. |
| Encoder context | `nvmpi_create_encoder` | `nvmpi_encoder_close` | Stop the DQ thread first (leak instead of use-after-free if the join fails). |
| `NvVideoDecoder` / `NvVideoEncoder` | create functions | deleted in close | After `STREAMOFF` and thread join. |
| Frame pool generation (`dec_gen`) | `initFramePool` (create or capture thread) | `retireFramePool` (unborrowed buffers), `~dec_gen` (the rest) | Retire after the capture thread is joined or on its own thread; borrowed buffers outlive close until released. |
| DMA frame buffer | `nvmpi_frame_buffer::alloc` | `destroy()` | Idempotent; fd reset to `-1`. |
| `dec_fd_borrow` (zero-copy release cookie) | `nvmpi_decoder_get_frame_fd` | the release callback | Freed exactly once by the callback; owns the generation reference. |
| Encoder OUTPUT-plane fds | DMA / dmabuf mode | per fd in close | Only in DMA modes. |
| Packet memory | caller | caller | libnvmpi never allocates or frees packet payloads. |
| `nvmpi_vic_ctx` | `nvmpi_vic_create` | `nvmpi_vic_close` | One context per thread. |
| Surfaces | `nvmpi_surface_alloc*` | `nvmpi_surface_destroy` | Caller owns the fd. |

## Coding guidelines

The codebase is security-sensitive: media bytes entering `put_packet` or
`put_frame` are untrusted input. Contributors are expected to follow these
rules.

- **Single-exit cleanup.** Every resource has one deallocation path. In C use
  the `goto cleanup;` pattern; in C++ prefer RAII or destructor-guaranteed
  cleanup. No early return that bypasses cleanup.
- **Hygiene.** Zero-initialise frame, packet and state buffers on allocation.
  After every free, destroy or close: set pointers to `NULL` and file
  descriptors to `-1`. Do not leave dangling references in pools: free each
  item as it is dequeued, never delete a pool that still holds items.
- **Defensive parsing.** Validate dimensions, packet sizes and offsets against
  sane and hardware limits before allocating or indexing. Check arithmetic on
  bitstream-derived lengths for overflow. Never write more than the negotiated
  V4L2 `length` into an OUTPUT-plane buffer. On a resolution change, fully
  tear down and reallocate the CAPTURE plane and never reuse stale
  dimensions or fd arrays. Bounds-check the read pointer against the packet
  end on every byte access when scanning NAL units or start codes.
- **Comments.** These files carry extensive comments by design. Explain the
  "why": every allocation names its deallocation site, every defensive check
  says what it prevents, every ioctl and STREAMON/STREAMOFF transition
  documents the expected device state, and every version-gated block says which
  API change it handles. Do not restate what the code literally does.
- **Style.** Match existing style (tab-indented C++ in `src/`). Prefer
  `static inline` helpers over macros when type safety matters. Avoid
  exceptions, RTTI and heavy abstractions on hot paths unless they clearly
  improve safety at no measurable cost.
- **Compatibility.** New code must compile cleanly on both buffer-API
  generations (JetPack 5 and 6 builds in CI) and must not break the public ABI.
- **Logging.** Use `NVMPI_LOG` / `NVMPI_LOG_SUB` from `include/nvmpi_log.h`; do
  not print with bare `fprintf`.
