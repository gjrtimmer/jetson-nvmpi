# Build and install

How to install a released libnvmpi, build it from source (on a Jetson or
off-Jetson against stubs), cross-compile it, package it, and consume it from
your own project. For the API itself see [api.md](api.md).

Contents:

- [Prerequisites](#prerequisites)
- [Install a release package](#install-a-release-package)
- [Build from source](#build-from-source)
- [scripts/build.sh](#scriptsbuildsh)
- [CMake options](#cmake-options)
- [Buffer API auto-detection](#buffer-api-auto-detection)
- [Stubs (off-Jetson builds)](#stubs-off-jetson-builds)
- [Cross-compilation](#cross-compilation)
- [Vendored NvV4l2ElementPlane](#vendored-nvv4l2elementplane)
- [Packaging](#packaging)
- [Using libnvmpi from your project](#using-libnvmpi-from-your-project)
- [Verifying the installation](#verifying-the-installation)
- [Troubleshooting](#troubleshooting)

## Prerequisites

- An NVIDIA Jetson with JetPack installed (provides the Jetson Multimedia API
  and the tegra userspace libraries), or a cross/stubs setup (below).
- CMake 3.10 or newer, GCC/G++ with C++11 support, Git. (`cmake_minimum_required`
  is 3.10; the C++ standard is set to 11.)
- Libjpeg: Jetson ships `libjpeg.so.8` without the `-dev` symlink; the build
  searches for both `jpeg` and the `libjpeg.so.8` soname.

Default locations used by the build (all overridable, see
[CMake options](#cmake-options)):

| What | Path |
| --- | --- |
| Multimedia API headers and sample classes | `/usr/src/jetson_multimedia_api` |
| Tegra libraries, JetPack 5 (L4T r35) | `/usr/lib/aarch64-linux-gnu/tegra` |
| Tegra libraries, JetPack 6 (L4T r36) | `/usr/lib/aarch64-linux-gnu/nvidia` |
| CUDA headers / libraries | `/usr/local/cuda/include`, `/usr/local/cuda/lib64` |

## Install a release package

Each tagged release publishes one archive per JetPack generation (name pattern
`jetson-nvmpi_<version>_l4t<tag>_aarch64.tar.gz`, for example
`jetson-nvmpi_3.10.0_l4tr36.4.0_aarch64.tar.gz`). See [ci.md](ci.md) for where
they are published. Pick the archive that matches your JetPack (r35 for
JetPack 5, r36 for JetPack 6).

Archive layout (a single top-level directory):

```text
jetson-nvmpi_<version>_l4t<tag>_aarch64/
├── include/
│   └── nvmpi.h
└── lib/
    ├── libnvmpi.so            (symlink, required: FFmpeg dlopens this name)
    ├── libnvmpi.so.1          (symlink, SONAME)
    ├── libnvmpi.so.<version>  (the library)
    └── pkgconfig/
        └── nvmpi.pc           (prefix rewritten to /usr/local)
```

Install into `/usr/local` and refresh the linker cache:

```bash
sudo tar xzf jetson-nvmpi_<version>_l4t<tag>_aarch64.tar.gz --strip-components=1 -C /usr/local
sudo ldconfig
```

The archives never contain stub libraries; they are built against the real
tegra libraries. The static library `libnvmpi.a` is not part of the archive.

## Build from source

On a Jetson (native build). Either the helper script:

```bash
git clone <your clone URL of jetson-nvmpi>
cd jetson-nvmpi
./scripts/build.sh --install
```

or raw CMake:

```bash
mkdir build && cd build
cmake ..
make -j"$(nproc)"
sudo make install
sudo ldconfig
```

`make install` (default prefix `/usr/local`) installs:

| File | Destination |
| --- | --- |
| `libnvmpi.so` (+ `.so.1`, `.so.<version>`) | `<prefix>/<libdir>` |
| `libnvmpi.a` | `<prefix>/<libdir>` |
| `nvmpi.h` | `<prefix>/include` |
| `nvmpi.pc` | `<prefix>/share/pkgconfig` and `<prefix>/<libdir>/pkgconfig` |

`<libdir>` comes from CMake's `GNUInstallDirs`; on a `/usr/local` prefix that is
`lib`. The shared library is built with `VERSION` equal to the CMake project
version and `SOVERSION` 1.

With custom paths:

```bash
cmake .. \
  -DJETSON_MULTIMEDIA_API_DIR=/path/to/jetson_multimedia_api \
  -DJETSON_MULTIMEDIA_LIB_DIR=/path/to/tegra/libs \
  -DCUDA_INCLUDE_DIR=/usr/local/cuda/include \
  -DCUDA_LIB_DIR=/usr/local/cuda/lib64
```

## scripts/build.sh

`scripts/build.sh` wraps the CMake build. By default it builds against the real
Jetson libraries; if the Multimedia API directory or the tegra library
directory is missing (off-Jetson, CI) it automatically falls back to the stubs
so the library still compiles (but is **not runnable**). Safe to run from any
working directory.

```bash
scripts/build.sh [options]
```

| Option | Description |
| --- | --- |
| `--stubs` | Force linking against `stubs/` (`-DWITH_STUBS=ON`). |
| `--no-stubs` | Force the real Jetson libraries (`-DWITH_STUBS=OFF`); configuration fails if they are missing. |
| `--install` | Run `cmake --install` (uses `sudo` when not root), then `ldconfig`. |
| `--clean` | Remove the build directory before configuring. |
| `--package` | Build a `.deb` with CPack after building (see [Packaging](#packaging)). |
| `--build-dir DIR` | Build directory (default: `<repo>/build`). |
| `--prefix DIR` | `CMAKE_INSTALL_PREFIX` (default: `/usr/local`). |
| `--build-type TYPE` | `CMAKE_BUILD_TYPE` (default: `Release`). |
| `-j N` / `-jN` | Parallel jobs (default: `nproc`). |
| `-h`, `--help` | Show usage. |

Environment variables `JETSON_MULTIMEDIA_API_DIR` and
`JETSON_MULTIMEDIA_LIB_DIR` override the paths used for the stubs
auto-detection (defaults: `/usr/src/jetson_multimedia_api`, and the `nvidia`
directory if it exists, otherwise `tegra`).

Examples:

```bash
scripts/build.sh                        # libnvmpi only (native, or stubs off-Jetson)
scripts/build.sh --install              # build and install system-wide
scripts/build.sh --stubs --clean        # fresh stubs build (CI / non-Jetson)
scripts/build.sh --prefix ~/_nvmpi --install   # install into a custom prefix
scripts/build.sh --package              # also produce a .deb
```

To start over, use `scripts/build.sh --clean` rather than deleting the build
directory by hand: the directory may be shared with a concurrent build, and
`--clean` tolerates root-owned leftovers from an earlier `--install`.

## CMake options

| Option | Default | Description |
| --- | --- | --- |
| `WITH_STUBS` | `OFF` | Link against the stub libraries in `stubs/` instead of the real NVIDIA libraries. |
| `WITH_VENDORED_NVV4L2` | `ON` | Use the vendored `NvV4l2ElementPlane.cpp` with a `pthread_join` guard (see below). |
| `JETSON_MULTIMEDIA_API_DIR` | `<SYSROOT>/usr/src/jetson_multimedia_api` | Multimedia API headers and sample classes. |
| `JETSON_MULTIMEDIA_LIB_DIR` | auto: `<SYSROOT>/usr/lib/aarch64-linux-gnu/nvidia` (JetPack 6) if it exists, else `.../tegra` (JetPack 5); `stubs/` when `WITH_STUBS=ON` | Directory holding `libnvv4l2`, `libnvjpeg`, `libnvbufsurface`, ... |
| `CUDA_INCLUDE_DIR` | `<SYSROOT>/usr/local/cuda/include` | CUDA headers. |
| `CUDA_LIB_DIR` | `<SYSROOT>/usr/local/cuda/lib64` | CUDA libraries. |
| `SYSROOT` | empty | Cross-compile sysroot prefix for all default paths above. If unset, the environment variables `BITBAKE_SYSROOT` then `TARGET_ROOTFS` are used. Deliberately not inherited from `CMAKE_SYSROOT` (that would double-prefix). |
| `BUILD_TESTING` | `ON` (CTest default) | Builds `test_bufpool` and `test_api`. See [Testing](../README.md#testing). |
| `CMAKE_BUILD_TYPE` | `Release` | Set to Release when empty. |

The library targets are `nvmpi` (shared) and `nvmpi_static` (static, output name
`libnvmpi.a`). Compile flags include `-Wall -Wextra -fPIC`; NVIDIA's sample
classes are compiled with two extra warning suppressions because they are not
`-Wextra` clean, while `src/` keeps the full warning set.

The log level is a compile-time threshold (`include/nvmpi_log.h`): the default
is `NVMPI_LOG_WARN`; define `NVMPI_LOG_LEVEL` (for example
`-DNVMPI_LOG_LEVEL=NVMPI_LOG_DEBUG` through `CMAKE_CXX_FLAGS`) to change it.
Messages go to stderr as `[libnvmpi][W]: ...`.

## Buffer API auto-detection

NVIDIA ships two buffer-management APIs. CMake probes the Multimedia API
include directory: if `nvbufsurface.h` exists it defines `WITH_NVUTILS` and
links `libnvbufsurface` and `libnvbufsurftransform`; otherwise it links the
legacy `libnvbuf_utils`. The configure log prints either `Using NvUtils API.`
or `Using NvBufUtils API.`

| | Legacy `nvbuf_utils` | NvUtils / NvBufSurface |
| --- | --- | --- |
| Header | `nvbuf_utils.h` | `nvbufsurface.h` |
| Detection | `nvbufsurface.h` absent | `nvbufsurface.h` present |
| CMake define | none | `-DWITH_NVUTILS` (automatic) |
| JetPack | 4.x and earlier | 5.x and 6.x |
| P010 (10-bit) decode | not available | available |

The log line shows JetPack generation detection too: `JetPack 6 detected (nvidia
lib dir)` or `JetPack 4/5 detected (tegra lib dir)`. You never choose the buffer
API by hand. A compile-time shim (`include/nvUtils2NvBuf.h`) maps legacy names
to NvUtils equivalents so the sources stay API-agnostic; see
[internals.md](internals.md#buffer-api-shim-and-version-guards).

## Stubs (off-Jetson builds)

For CI, containers and development on a non-Jetson host, build against the
stub shared objects in `stubs/`:

```bash
scripts/build.sh --stubs
# equivalent raw CMake:
cmake -DWITH_STUBS=ON -DJETSON_MULTIMEDIA_API_DIR=/path/to/jetson_multimedia_api ..
make
```

`stubs/` contains minimal aarch64 ELF shared objects that carry the real
sonames (`libnvv4l2`, `libnvbufsurface`, `libnvbufsurftransform`, `libnvbuf_utils`,
`libnvjpeg`, `libv4l2`) plus `jpeg_tegra_stubs.c` (no-op implementations of
the two Tegra-only libjpeg symbols the NVIDIA JPEG sample classes call).
They only satisfy the linker. A stubs build:

- is not functional at runtime (no hardware, no drivers),
- must never be shipped (the release job refuses to package one),
- still needs the Jetson Multimedia API headers and sample classes
  (`JETSON_MULTIMEDIA_API_DIR`), which are not vendored here apart from one file.

Because the stubs carry the real sonames, a stub-linked binary can run on a
real Jetson if the real libraries are placed ahead of the stubs on the library
path; the project's own hardware test does exactly that (see [ci.md](ci.md)).

## Cross-compilation

For an x86_64 host targeting a Jetson root filesystem, pass a sysroot:

```bash
cmake -B build-cross \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake \
  -DSYSROOT=/path/to/jetson-rootfs \
  .
cmake --build build-cross -j"$(nproc)"
```

`cmake/toolchain-aarch64.cmake` is an example toolchain file: it sets
`CMAKE_SYSTEM_NAME Linux`, `CMAKE_SYSTEM_PROCESSOR aarch64` and the compilers
`aarch64-linux-gnu-gcc` / `aarch64-linux-gnu-g++`. Adjust it for your toolchain.
`SYSROOT` prefixes the Multimedia API, library and CUDA default paths so
`find_library` and the header probe resolve inside the sysroot. Yocto/BitBake
builds can set `BITBAKE_SYSROOT` instead (or `TARGET_ROOTFS`); both are picked up
when `SYSROOT` is not given. If your toolchain file already sets `CMAKE_SYSROOT`,
do not also rely on inheritance: pass explicit `-DJETSON_MULTIMEDIA_API_DIR=...`
paths.

Add `-DWITH_STUBS=ON` when you only need a compile check without a real
rootfs.

## Vendored NvV4l2ElementPlane

NVIDIA's sample class `NvV4l2ElementPlane.cpp` (from
`${JETSON_MULTIMEDIA_API_DIR}/samples/common/classes`) has an unguarded
`pthread_join` in `stopDQThread()` and `waitForDQThread()`. On glibc 2.34 and
newer (Ubuntu 22.04, JetPack 6) calling `pthread_join` on a zero or
uninitialised `pthread_t` segfaults instead of returning `ESRCH`, which would
crash every decoder and encoder teardown path.

libnvmpi vendors a corrected copy at `vendor/mmapi/NvV4l2ElementPlane.cpp` with
`if (dq_thread)` guards before both `pthread_join` calls. It is the default
(`WITH_VENDORED_NVV4L2=ON`) and safe on all glibc versions. Set
`-DWITH_VENDORED_NVV4L2=OFF` only if your JetPack's copy is already patched.

## Packaging

### Release archive (`scripts/package.sh`)

Turns an installed prefix into the distributable tarball (this is what CI
publishes). Safe to run from any directory.

```bash
scripts/build.sh --no-stubs --clean --install --prefix /tmp/pfx
scripts/package.sh --prefix /tmp/pfx --l4t r36.4.0
# -> dist/jetson-nvmpi_<version>_l4tr36.4.0_aarch64.tar.gz
```

| Option | Description |
| --- | --- |
| `--prefix DIR` | Install prefix produced by `scripts/build.sh --install` (required). |
| `--version VER` | Package version (default: the project `VERSION` in `CMakeLists.txt`). |
| `--l4t TAG` | L4T tag in the file name (default: `$L4T_TAG`, else `r36.4.0`). |
| `--arch ARCH` | Architecture label (default: `aarch64`). |
| `--outdir DIR` | Output directory (default: `<repo>/dist`). |
| `-h`, `--help` | Show usage. |

The script copies `libnvmpi.so*` (versioned file and both symlinks), the
installed `*.h` headers and `nvmpi.pc` (with `prefix`, `exec_prefix` and
`libdir` rewritten to `/usr/local` and `lib`). It fails if the unversioned
`libnvmpi.so` is missing, because FFmpeg `dlopen`s exactly that name. The
version is overridable: CI uses `<ver>` for tags and `<ver>-<short-sha>` for
main snapshots.

### Debian package (CPack)

```bash
scripts/build.sh --package
# or: cmake --build build --target package
```

CPack generates a `.deb` named `libnvmpi_<version>_arm64.deb` in the build
directory (`CPACK_GENERATOR DEB`, package `libnvmpi`, section `libs`,
architecture `arm64`, `DEB-DEFAULT` file naming). This path is not exercised by
the project's CI, which publishes the tarballs; the install paths inside the
`.deb` were not verified for this document.

## Using libnvmpi from your project

Header and shared library after install:

```bash
pkg-config --cflags --libs nvmpi
# -I/usr/local/include -L/usr/local/lib -lnvmpi
gcc app.c $(pkg-config --cflags --libs nvmpi) -o app
```

`nvmpi.pc` content comes from `nvmpi.pc.in`:

```text
prefix=<install prefix>
exec_prefix=<install prefix>
libdir=${exec_prefix}/<libdir>
includedir=${prefix}/include

Name: nvmpi
Description: nvidia multimedia api
Version: <project version>

Requires:
Libs: -L${libdir} -lnvmpi
Cflags: -I${includedir}
```

CMake consumers can link by name (`target_link_libraries(app nvmpi)` after
`find_library`/`pkg_check_modules`); the project does not export a CMake
package config.

FFmpeg integration builds do not link libnvmpi: the FFmpeg codec layer loads
`libnvmpi.so` at run time with `dlopen`, so the unversioned `libnvmpi.so` must
be resolvable on the library path of the machine that runs FFmpeg (see
[jetson-ffmpeg](https://github.com/gjrtimmer/jetson-ffmpeg)).

### Static library and pkg-config (known limitation)

`libnvmpi.a` is built and installed, but it is only an archive of libnvmpi's own
objects. `nvmpi.pc` has no `Libs.private`, so
`pkg-config --static --libs nvmpi` does not list libnvmpi's own dependencies
(the NVIDIA libraries, libjpeg and pthreads). Linking statically therefore
requires adding those libraries by hand. This is a known gap tracked as issue
#12 on the project's GitLab; the shared library is the supported way to
consume libnvmpi.

## Verifying the installation

```bash
pkg-config --libs nvmpi
# expected: -lnvmpi (plus -L<libdir> when not in a default path)

ldconfig -p | grep nvmpi
# expected: libnvmpi.so.1 (libc6,AArch64) => /usr/local/lib/libnvmpi.so.1
```

On a Jetson, run the API smoke test from a `-DBUILD_TESTING=ON` build
(`build/test_api`; see [Testing](../README.md#testing)).

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `Package nvmpi was not found in the pkg-config search path` | The install prefix is not on pkg-config's path. `sudo ldconfig`, then `export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH` (persist it in your shell profile if needed). |
| CMake reports `NOTFOUND` for `nvv4l2`, `nvjpeg`, `nvbufsurface` or `nvbufsurftransform` | The Jetson libraries are not where the build looks. Check `ls /usr/lib/aarch64-linux-gnu/nvidia /usr/lib/aarch64-linux-gnu/tegra` and `ls /usr/src/jetson_multimedia_api/include/nvbufsurface.h`, or pass `-DJETSON_MULTIMEDIA_LIB_DIR=...` / `-DJETSON_MULTIMEDIA_API_DIR=...`. Inside a container the libraries are usually bind-mounted from the host; make sure the mounts exist. |
| Configure log says `Using NvBufUtils API.` on JetPack 5 or 6 | `nvbufsurface.h` was not found under `<JETSON_MULTIMEDIA_API_DIR>/include`; fix the path. |
| Segfault on teardown with glibc 2.34 or newer (Ubuntu 22.04, JetPack 6) | The unguarded `pthread_join` in NVIDIA's sample class. Keep `WITH_VENDORED_NVV4L2=ON` (the default). No `linux-libc-dev` downgrade is needed. |
| Library loads but hardware calls fail or `create_*` returns `NULL` | You are probably running a stubs build, or the process cannot see the real tegra libraries and device nodes (for example a container without the NVIDIA runtime). A stubs build is link-only. |
| Slow encode or decode | Make sure the Jetson power mode is appropriate (`sudo nvpmodel -m 0`, optionally `sudo jetson_clocks`) and set `max_perf` in `nvEncParam` / `nvDecParam`. Neither field is on by default through the C API. |
| Encode above 4K fails | A hardware limit of the NVENC engine, not a software setting. Downscale first (see the [VIC API](api.md#vic-transform)) or use a software encoder. |
| Encoder or hardware JPEG missing on Orin Nano | The Orin Nano module has no NVENC and no NVJPG encode engine; decode (NVDEC) and VIC are present. `nvmpi_create_jpeg_encoder` returns `NULL` there. |
