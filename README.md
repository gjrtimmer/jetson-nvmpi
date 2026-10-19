# jetson-nvmpi (libnvmpi)

Hardware-accelerated video and JPEG encode/decode on NVIDIA Jetson through a
small, plain-C library.

Every Jetson module has dedicated silicon for video codecs (NVDEC, NVENC), image
scaling (VIC) and JPEG (NVJPG), but NVIDIA exposes it through a V4L2 and
NvBuffer multimedia API that is awkward to use. **libnvmpi** wraps that API
behind a handful of `create` / `put` / `get` / `close` functions in a single
header, `nvmpi.h`.

It is used two ways:

- **From FFmpeg**, through the `*_nvmpi` codecs of
  [jetson-ffmpeg](https://github.com/gjrtimmer/jetson-ffmpeg). You install
  libnvmpi once; FFmpeg loads it at run time.
- **Directly from C or C++**, for applications that want the hardware engines
  without FFmpeg.

## Contents

- [Relation to jetson-ffmpeg](#relation-to-jetson-ffmpeg)
- [Features and supported codecs](#features-and-supported-codecs)
- [Supported hardware and JetPack](#supported-hardware-and-jetpack)
- [Install from a release package](#install-from-a-release-package)
- [Build from source](#build-from-source)
- [Quick API example](#quick-api-example)
- [Testing](#testing)
- [CI and releases](#ci-and-releases)
- [Known limitations and driver pitfalls](#known-limitations-and-driver-pitfalls)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [License](#license)

## Relation to jetson-ffmpeg

libnvmpi started inside the [jetson-ffmpeg](https://github.com/gjrtimmer/jetson-ffmpeg)
repository, which had two layers:

1. **libnvmpi** (this repository): a standalone shared library with a C API.
2. **The FFmpeg integration**: codec sources and a patcher that add `h264_nvmpi`,
   `hevc_nvmpi` and friends to an FFmpeg tree.

libnvmpi has been split out into this repository with its history and tags
preserved (tags `v2.0.0` to `v3.10.0` carry over). jetson-ffmpeg keeps the FFmpeg
patches, the patcher and the hardware test suites. The FFmpeg layer does not
link against libnvmpi: it loads `libnvmpi.so` at run time with `dlopen` when the
first nvmpi codec is initialised, so FFmpeg starts and its software codecs work
even without libnvmpi installed, and prints a clear error if you try to use an
nvmpi codec without it. For that reason the unversioned `libnvmpi.so` name must
be resolvable on the target system. New libnvmpi fixes and issues belong here.

## Features and supported codecs

| Codec | Decode | Encode | Engine / path |
| --- | :---: | :---: | --- |
| H.264 / AVC | yes | yes | NVDEC / NVENC (V4L2) |
| H.265 / HEVC | yes | yes | NVDEC / NVENC (V4L2) |
| MPEG-2 | yes | no | NVDEC (V4L2) |
| MPEG-4 Part 2 | yes | no | NVDEC (V4L2) |
| VP8 | yes | no | NVDEC (V4L2), availability varies by module |
| VP9 | yes | no | NVDEC (V4L2) |
| JPEG / MJPEG | yes (baseline only) | yes | NVJPG, synchronous, no V4L2 |

Highlights:

- **Video decoder**: put/get loop with a frame pool, hardware scaling to a target
  size through VIC, 8-bit NV12 or planar YUV420 output, 10-bit P010 output for
  HEVC Main10 (NvUtils buffer API only), optional low-latency mode (DPB
  reordering off), max-performance mode, flush for seeking.
- **Video encoder**: H.264 and HEVC with CBR/VBR, profile and level, GOP control,
  B-frames and reference frames (H.264), CABAC, AUD and SPS/PPS repetition,
  lossless H.264, runtime `force_idr` and `set_bitrate`, flush, and a pooled
  packet interface.
- **Zero-copy**: decode to a DMA-BUF fd (`nvmpi_decoder_get_frame_fd`) and feed an
  encoder from a DMA-BUF fd (`nvmpi_encoder_put_frame_fd`), with surface
  allocation helpers.
- **VIC**: standalone hardware scale and colour conversion between DMA-BUF
  surfaces (`nvmpi_vic_*`).
- **Non-blocking mode** for the encoder (`NVMPI_ERR_EAGAIN` instead of blocking),
  and configurable blocking-wait timeouts on both sides.
- **Both NVIDIA buffer APIs**: the legacy `nvbuf_utils` and the newer
  NvUtils/NvBufSurface are auto-detected at build time.
- **Safe teardown**: a vendored fix for the glibc 2.34 `pthread_join` crash in
  NVIDIA's sample class, ordered thread-before-resource shutdown, and an encoder
  close path that works even if no frame was ever submitted.

Performance note (measured through the FFmpeg integration with
max-performance mode on versus off): 720p H.264 decode about 46 fps to about
290 fps (roughly 6x), 4K HEVC decode about 28 fps to about 64 fps (roughly 2.3x).
Set `max_perf` in `nvDecParam` / `nvEncParam` to lift the engine clock governor.
Through the C API it is off unless you set it.

## Supported hardware and JetPack

libnvmpi targets the Jetson families that ship the NVIDIA Jetson Multimedia API:

| JetPack | L4T | Buffer API used | Platforms | Status in this project |
| --- | --- | --- | --- | --- |
| 6.x | r36 | NvUtils (NvBufSurface) | AGX Orin, Orin NX, Orin Nano | Tested in CI on Orin NX (JetPack 6, L4T r36.4) |
| 5.x | r35 | NvUtils (NvBufSurface) | AGX Xavier, Xavier NX, AGX Orin, Orin NX (5.1.x), Orin Nano (5.1.x) | Compile-checked in CI; not hardware tested |
| 4.x and earlier | r32 and earlier | legacy `nvbuf_utils` | TX1, TX2, Nano, AGX Xavier, Xavier NX | Supported by the code; not exercised by CI. jetson-ffmpeg's v2.x series is the line for FFmpeg 4.2/4.4 on these. |

The first JetPack with the multimedia API libnvmpi is built on is JetPack 3.2
(L4T r28); earlier releases and the TK1 cannot be supported. Platforms that
never received a given JetPack release are simply not listed for it. Anything
not marked tested is expected to work but is untested.

The build picks the right library directory automatically
(`/usr/lib/aarch64-linux-gnu/nvidia` on JetPack 6, `.../tegra` on JetPack 5) and
the right buffer API by probing for `nvbufsurface.h`; see
[docs/build.md](docs/build.md#buffer-api-auto-detection).

Hardware limits that no software can change:

- NVENC has a maximum encode resolution (4K class); downscale first for larger frames.
- **Orin Nano has no NVENC and no NVJPG encoder**: video encode and
  `nvmpi_create_jpeg_encoder` are unavailable; decode (NVDEC) and VIC work.
- Align width and height to multiples of 16 when you can; unaligned sizes show
  up as padding or a green edge.
- VP8 hardware decode availability varies by module.

## Install from a release package

Each tagged release publishes one archive per JetPack generation:
`jetson-nvmpi_<version>_l4t<tag>_aarch64.tar.gz` (for example
`..._l4tr36.4.0_...` for JetPack 6 and `..._l4tr35.4.1_...` for JetPack 5). They
are attached to the GitLab release and stored in the project's S3 bucket under
`releases/<version>/jp5` and `releases/<version>/jp6` (see
[docs/ci.md](docs/ci.md#published-artifacts-and-layout)). Choose the archive
that matches your JetPack.

Layout:

```text
jetson-nvmpi_<version>_l4t<tag>_aarch64/
├── include/nvmpi.h
└── lib/
    ├── libnvmpi.so  ->  libnvmpi.so.1  ->  libnvmpi.so.<version>
    └── pkgconfig/nvmpi.pc
```

Install into `/usr/local`:

```bash
sudo tar xzf jetson-nvmpi_<version>_l4t<tag>_aarch64.tar.gz --strip-components=1 -C /usr/local
sudo ldconfig
pkg-config --libs nvmpi        # expect: -lnvmpi
ldconfig -p | grep nvmpi       # expect: libnvmpi.so.1 => /usr/local/lib/libnvmpi.so.1
```

If `pkg-config` cannot find it, add the prefix to its path:
`export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH`.

The archives are built against the real tegra libraries; stub builds are never
packaged.

## Build from source

Prerequisites: a Jetson with JetPack installed (Multimedia API headers in
`/usr/src/jetson_multimedia_api`), CMake 3.10 or newer, a C++11 compiler, Git.

```bash
git clone <your clone URL of jetson-nvmpi>
cd jetson-nvmpi
./scripts/build.sh --install        # build, install to /usr/local, run ldconfig
```

Raw CMake equivalent:

```bash
mkdir build && cd build
cmake ..
make -j"$(nproc)"
sudo make install
sudo ldconfig
```

`scripts/build.sh` flags:

| Flag | Meaning |
| --- | --- |
| `--stubs` / `--no-stubs` | Force the link-only stub libraries / the real Jetson libraries (default: auto-detect). |
| `--install` | Install (uses `sudo` if needed) and run `ldconfig`. |
| `--clean` | Remove the build directory first. |
| `--package` | Also build a `.deb` with CPack. |
| `--build-dir DIR`, `--prefix DIR`, `--build-type TYPE` | Build directory (default `build`), install prefix (default `/usr/local`), build type (default `Release`). |
| `-j N` | Parallel jobs (default `nproc`). |

Off-Jetson (CI, containers, development): `scripts/build.sh --stubs` builds
against stub libraries in `stubs/`. The result compiles and links but is **not
runnable**. Cross-compilation uses `cmake/toolchain-aarch64.cmake` plus
`-DSYSROOT=...`.

CMake options in short: `WITH_STUBS` (default OFF), `WITH_VENDORED_NVV4L2`
(default ON), `JETSON_MULTIMEDIA_API_DIR`, `JETSON_MULTIMEDIA_LIB_DIR`,
`CUDA_INCLUDE_DIR`, `CUDA_LIB_DIR`, `SYSROOT`, `BUILD_TESTING`. Details, the
cross-compile recipe, packaging and the static-library note are in
[docs/build.md](docs/build.md).

## Quick API example

Both directions are put/get loops. The full reference, with return codes,
ownership and threading rules, is [docs/api.md](docs/api.md). Always
zero-initialise the parameter structs. Error handling is abbreviated here.

### Decode H.264 to YUV420

```c
#include <nvmpi.h>
#include <stdlib.h>
#include <string.h>

enum { W = 1920, H = 1080 };

/* Output planes (YUV420: Y is WxH, U and V are W/2 x H/2). The decoder copies
 * into caller-owned memory and does not fill width/height: they come from you. */
static void frame_alloc(nvFrame *f)
{
    memset(f, 0, sizeof(*f));
    f->type = NV_PIX_YUV420;
    f->width = W;
    f->height = H;
    f->linesize[0] = W;     f->payload_size[0] = (unsigned long)W * H;
    f->linesize[1] = W / 2; f->payload_size[1] = (unsigned long)(W / 2) * (H / 2);
    f->linesize[2] = W / 2; f->payload_size[2] = f->payload_size[1];
    for (int i = 0; i < 3; i++)
        f->payload[i] = malloc(f->payload_size[i]);
}

int decode(const unsigned char *annexb, unsigned long size, unsigned long pts_us)
{
    nvDecParam dp;
    memset(&dp, 0, sizeof(dp));
    dp.codingType = NV_VIDEO_CodingH264;
    dp.pixFormat = NV_PIX_YUV420;
    dp.frame_pool_size = 12;        /* must be > 0 */
    dp.max_perf = 1;
    dp.width = W;                   /* optional hints from the container */
    dp.height = H;

    nvmpictx *dec = nvmpi_create_decoder(&dp);
    if (!dec)
        return -1;                  /* no device, or hardware busy */

    nvFrame frame;
    frame_alloc(&frame);

    nvPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.payload = (unsigned char *)annexb;  /* copied inside put_packet */
    pkt.payload_size = size;
    pkt.pts = pts_us;               /* microseconds */
    if (nvmpi_decoder_put_packet(dec, &pkt) == 0) {
        while (nvmpi_decoder_get_frame(dec, &frame, false) == 0) {
            /* frame.payload[0..2] hold the picture, frame.timestamp the pts */
        }
    }

    pkt.payload_size = 0;           /* zero-size packet = end of stream */
    nvmpi_decoder_put_packet(dec, &pkt);
    while (nvmpi_decoder_get_frame(dec, &frame, true) == 0) {
        /* drain remaining frames; -1 means timeout or shutdown */
    }

    nvmpi_decoder_close(dec);       /* handle invalid afterwards */
    for (int i = 0; i < 3; i++)
        free(frame.payload[i]);
    return 0;
}
```

After `nvmpi_decoder_flush()` (seek) you must feed the stream's SPS/PPS again.
For zero-copy output use `nvmpi_decoder_get_frame_fd` and call the returned
release callback for every frame.

### Encode YUV420 to H.264

```c
#include <nvmpi.h>
#include <stdlib.h>
#include <string.h>

enum { W = 1920, H = 1080, NPKT = 8 };

int encode(nvFrame *frames, int nframes)   /* frames: YUV420, caller-filled */
{
    nvEncParam ep;
    memset(&ep, 0, sizeof(ep));
    ep.codingType = NV_VIDEO_CodingH264;
    ep.width = W;
    ep.height = H;
    ep.bitrate = 4000000;           /* bit/s */
    ep.fps_n = 30;
    ep.fps_d = 1;
    ep.iframe_interval = 30;
    ep.idr_interval = 60;
    ep.capture_num = 4;             /* V4L2 buffers per plane, must be > 0 */
    ep.insert_spspps_idr = 1;
    ep.pixFormat = NV_PIX_YUV420;
    ep.max_perf = 1;

    nvmpictx *enc = nvmpi_create_encoder(&ep);
    if (!enc)
        return -1;

    /* Donate empty packets: libnvmpi fills them, you own the memory. */
    for (int i = 0; i < NPKT; i++) {
        nvPacket *p = calloc(1, sizeof(*p));
        p->payload = malloc(NVMPI_ENC_CHUNK_SIZE);
        nvmpi_encoder_qEmptyPacket(enc, p);
    }

    nvPacket *out;
    for (int i = 0; i < nframes; i++) {
        nvmpi_encoder_put_frame(enc, &frames[i]);       /* copies the planes */
        while (nvmpi_encoder_get_packet(enc, &out, false) == 0) {
            /* out->payload / out->payload_size, out->pts, out->flags (keyframe) */
            nvmpi_encoder_qEmptyPacket(enc, out);       /* give it back */
        }
    }

    nvmpi_encoder_put_frame(enc, NULL);                 /* EOS, start flushing */
    while (nvmpi_encoder_get_packet(enc, &out, true) == 0)
        nvmpi_encoder_qEmptyPacket(enc, out);           /* returns -2 at EOS */

    /* Drain and free the pool yourself before closing. */
    while (nvmpi_encoder_dqEmptyPacket(enc, &out) == 0) {
        free(out->payload);
        free(out);
    }
    nvmpi_encoder_close(enc);
    return 0;
}
```

Compile either snippet with `gcc ... $(pkg-config --cflags --libs nvmpi)`. The
same flow works for HEVC (`NV_VIDEO_CodingHEVC`). JPEG encode/decode, DMA-BUF
and VIC usage are described in [docs/api.md](docs/api.md).

## Testing

| Test | Where it runs | What it checks |
| --- | --- | --- |
| `test_bufpool` (`test/unit/test_bufpool.cpp`) | any host | The `NVMPI_bufPool` blocking dequeue, timeout, shutdown, reset, notify and concurrent push/pop stress paths. 8 cases. |
| `test_api` (`test/api/test_api.c`) | real Jetson | Public-API smoke test, no FFmpeg: decoder and encoder create/close, 50x rapid create/close loops for each, decoder creation for every supported codec, HEVC encoder create/close. Catches linkage problems, init-without-data crashes and close-path segfaults. |

Build and run:

```bash
mkdir build && cd build
cmake -DBUILD_TESTING=ON ..          # BUILD_TESTING is ON by default via CTest
make -j"$(nproc)"
ctest --output-on-failure            # runs bufpool and api
./test_api                           # the API test directly
```

`test_api` runs every case in a forked child so one crash does not kill the
harness. Results: `PASS` (exit 0), `SKIP` (exit 77, for example when a `create`
returns `NULL`), `CRASH` (killed by a signal), `FAIL` (other non-zero exit).
Environment switches (both default off; a value of empty or `0` counts as off):

| Variable | Effect |
| --- | --- |
| `NVMPI_TEST_STRICT=1` | Crashes count as failures. Use on real hardware, where a crash is a genuine bug. Without it, crashes are reported but do not fail the run, which is the right behaviour on a stubs build where the stubs do not emulate the device. |
| `NVMPI_TEST_VERBOSE=1` | Keep the child's stderr so `abort()` and driver messages are visible. |

On a stubs build `create_*` may return `NULL` (cases `SKIP`) or the close path
may crash; neither means much. Run `test_api` on a Jetson, with nothing else
using NVDEC/NVENC, before trusting a result. The per-feature FFmpeg-level
hardware suites live in the jetson-ffmpeg repository.

## CI and releases

The GitLab pipeline:

```text
build (stubs: jp6, jp5; unit tests)  ->  test (test:api:hw on the Jetson runner;
        stub jp6/jp5, ASan, TSan on the mock, in parallel)
        -> dist (real libs, package: jp6, jp5)  ->  publish (S3)  ->  release
```

- Branch and merge request pipelines run build and test. Nothing is published.
- A pipeline on the default branch also packages and uploads a **snapshot** to
  S3 `snapshots/<short-sha>/jp5|jp6/`. Snapshot versions look like
  `<version>-<short-sha>`.
- A tag `vX.Y.Z` runs the same jobs, so a release is gated on the Jetson test of
  that exact commit. The **tag must equal the `VERSION` in `CMakeLists.txt`**.
  It then uploads to S3 `releases/<version>/jp5|jp6/`, the GitLab package
  registry, and creates the GitLab release with a changelog generated from
  Conventional Commits.
- Cutting a release: bump `project(nvmpi VERSION X.Y.Z ...)`, merge, tag
  `vX.Y.Z`, push the tag.

Everything is documented in [docs/ci.md](docs/ci.md): jobs, runner tags,
required CI/CD variable names, S3 layout and the manual downstream trigger of
jetson-ffmpeg.

## Known limitations and driver pitfalls

- **Static library**: `libnvmpi.a` is installed, but `nvmpi.pc` has no
  `Libs.private`, so `pkg-config --static` does not list the dependencies
  (issue #12 on the project's GitLab). The shared library is the supported way
  to consume libnvmpi.
- **Tegra V4L2 driver**: it does not unblock `DevicePoll()` after EOS, deadlocks
  on concurrent `NvBufSurfTransform` calls (libnvmpi serialises them with one
  global mutex), cannot resolve `dup`'d DMA-BUF fds through
  `NvBufSurfaceFromFd`, segfaults instead of returning `EBUSY` when two
  processes use the same engine, and is stochastically unstable under rapid
  open/close loops. Never run two hardware workloads against the same engine at
  once. Details in [docs/internals.md](docs/internals.md#tegra-v4l2-driver-pitfalls).
- **Encoder close with no frames** used to segfault because the dequeue thread
  could not be woken; close now queues an EOS first
  ([docs/internals.md](docs/internals.md#encoder-teardown)).
- **Encoder packets are caller memory**: drain and free the pool before
  `nvmpi_encoder_close()`. Packets left in the pool are discarded with a warning.
- **Encoded frames above 10 MiB** (`NVMPI_ENC_CHUNK_SIZE`) and **input packets
  above `chunk_size`** are dropped with an error, not truncated.
- **Hardware limits**: see [Supported hardware and JetPack](#supported-hardware-and-jetpack).

## Documentation

| Document | Contents |
| --- | --- |
| [docs/api.md](docs/api.md) | Every public function, struct and enum in `nvmpi.h`: parameters, return codes, ownership, blocking and threading. |
| [docs/build.md](docs/build.md) | Install from a release, build from source, CMake options, stubs, cross-compiling, packaging, pkg-config, troubleshooting. |
| [docs/internals.md](docs/internals.md) | Architecture, pipelines, threads, teardown order, Tegra driver pitfalls, buffer-API guards, coding rules. |
| [docs/ci.md](docs/ci.md) | Pipelines, runners, release flow, S3 layout, snapshots, variables. |

For the FFmpeg side (patches, supported FFmpeg versions, `*_nvmpi` options and
usage), see the [jetson-ffmpeg wiki](https://github.com/gjrtimmer/jetson-ffmpeg/wiki).

## Contributing

- **Issues**: open them on GitHub at
  [gjrtimmer/jetson-nvmpi](https://github.com/gjrtimmer/jetson-nvmpi/issues).
  For crashes include your Jetson module, JetPack / L4T version, and the
  `[libnvmpi]` log lines.
- **Merge requests** go to the project's GitLab (the GitHub repository is a push
  mirror). Branch from the default branch using `{type}/{NR}-{short-desc}`
  (`fix/`, `feat/`, `perf/`, `refactor/`, `ci/`, `docs/`; `NR` is the issue
  number).
- **Commits and MR titles** follow
  [Conventional Commits](https://www.conventionalcommits.org/)
  (`feat(nvmpi): ...`, `fix(nvmpi): ...`, `ci: ...`, `docs: ...`). The changelog
  is generated from them, so use the right type. Keep the subject imperative and
  at most 72 characters.
- **Code**: read [docs/internals.md](docs/internals.md#coding-guidelines) first
  (resource lifecycle, defensive parsing, comment policy, the transform mutex
  rule). New public API goes into `include/nvmpi.h` only. Every change that can be
  tested should ship with a test, and anything touching device code should pass
  `test_api` on a Jetson.

## License

MIT, see [LICENSE](LICENSE). Copyright (c) 2020 jiangwei, copyright (c) 2026
G.J.R. Timmer.
