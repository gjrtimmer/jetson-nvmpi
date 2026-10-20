cmake_minimum_required(VERSION 3.10)
project(nvmpi VERSION 4.2.1 DESCRIPTION "nvidia multimedia api")

# ---------------------------------------------------------------------------
# Cross-compile sysroot
# ---------------------------------------------------------------------------
# Prefix for all default Jetson/CUDA paths. Empty on native builds.
# Precedence: explicit -DSYSROOT > env BITBAKE_SYSROOT (Yocto) > env TARGET_ROOTFS.
# NOT inherited from CMAKE_SYSROOT — when a toolchain file sets CMAKE_SYSROOT,
# CMake auto-prepends it to find_library/find_file PATHS; inheriting would
# cause double-prefixing (sysroot/sysroot/usr/lib/...). Users with toolchain
# files should pass explicit -DJETSON_MULTIMEDIA_API_DIR=... instead.
set(SYSROOT "" CACHE PATH "Cross-compile sysroot prefix for Jetson libraries and headers.")
if(NOT SYSROOT)
	if(DEFINED ENV{BITBAKE_SYSROOT})
		set(SYSROOT "$ENV{BITBAKE_SYSROOT}")
	elseif(DEFINED ENV{TARGET_ROOTFS})
		set(SYSROOT "$ENV{TARGET_ROOTFS}")
	endif()
endif()
if(SYSROOT)
	message(STATUS "Cross-compile sysroot: ${SYSROOT}")
endif()

# ---------------------------------------------------------------------------
# Path defaults (sysroot-prefixed when cross-compiling)
# ---------------------------------------------------------------------------
set(JETSON_MULTIMEDIA_API_DIR_DEFAULT "${SYSROOT}/usr/src/jetson_multimedia_api")
# JP6 moved tegra libs from tegra/ to nvidia/
set(JETSON_MULTIMEDIA_LIB_DIR_JP5 "${SYSROOT}/usr/lib/aarch64-linux-gnu/tegra")
set(JETSON_MULTIMEDIA_LIB_DIR_JP6 "${SYSROOT}/usr/lib/aarch64-linux-gnu/nvidia")
set(CUDA_INCLUDE_DIR_DEFAULT "${SYSROOT}/usr/local/cuda/include")
set(CUDA_LIB_DIR_DEFAULT "${SYSROOT}/usr/local/cuda/lib64")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(WITH_STUBS "Build against stubs instead of real Jetson libraries." OFF)
option(NVMPI_MOCK "Build the behavioural Jetson mock libs (mock/) into <build>/mock and run test_api against them. Implies WITH_STUBS (link-time only; the mock replaces the stubs at run time)." OFF)
set(NVMPI_SANITIZER "" CACHE STRING "Sanitizer build for the mock tests: address (ASan+UBSan+LSan) or thread (TSan). Empty = off.")
set_property(CACHE NVMPI_SANITIZER PROPERTY STRINGS "" address thread)
option(WITH_VENDORED_NVV4L2 "Use vendored NvV4l2ElementPlane.cpp with pthread_join guard." ON)

if(NVMPI_MOCK)
	# The mock only replaces the libs at run time; linking still needs the
	# stub sonames, and no real Jetson libs exist on the mock runner.
	set(WITH_STUBS ON CACHE BOOL "Build against stubs instead of real Jetson libraries." FORCE)
	# Mock mode: no build-tree RPATH/RUNPATH at all. Libraries are found only
	# through LD_LIBRARY_PATH (set by the ctest entries), so a stale RPATH to
	# stubs/ can never shadow build/mock (DT_RPATH would beat LD_LIBRARY_PATH,
	# DT_RUNPATH would not; neither is present this way).
	set(CMAKE_SKIP_BUILD_RPATH ON)
	message(STATUS "NVMPI_MOCK=ON: mock libs in ${CMAKE_BINARY_DIR}/mock, stubs link-only")
endif()

if(NVMPI_SANITIZER)
	if(NOT NVMPI_MOCK)
		message(FATAL_ERROR "NVMPI_SANITIZER needs NVMPI_MOCK=ON (stub-linked builds are not runnable)")
	endif()
	if(NVMPI_SANITIZER STREQUAL "address")
		set(NVMPI_SAN_FLAGS "-fsanitize=address,undefined -fno-sanitize-recover=undefined")
	elseif(NVMPI_SANITIZER STREQUAL "thread")
		set(NVMPI_SAN_FLAGS "-fsanitize=thread")
		# GCC < 12 libtsan misses pthread_cond_clockwait (see the header).
		if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
			string(APPEND CMAKE_CXX_FLAGS " -include ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tsan_compat.h")
		endif()
	else()
		message(FATAL_ERROR "NVMPI_SANITIZER must be address or thread")
	endif()
	# Compile AND link flags: libnvmpi, the mock libs and the tests all carry
	# the runtime, so the instrumented code and the mock see each other.
	string(APPEND NVMPI_SAN_FLAGS " -fno-omit-frame-pointer -g")
	string(APPEND CMAKE_C_FLAGS " ${NVMPI_SAN_FLAGS}")
	string(APPEND CMAKE_CXX_FLAGS " ${NVMPI_SAN_FLAGS}")
	string(APPEND CMAKE_EXE_LINKER_FLAGS " ${NVMPI_SAN_FLAGS}")
	string(APPEND CMAKE_SHARED_LINKER_FLAGS " ${NVMPI_SAN_FLAGS}")
	message(STATUS "NVMPI_SANITIZER=${NVMPI_SANITIZER}")
endif()

# Path options — CACHE PATH, not option() (option() is boolean-only)
set(JETSON_MULTIMEDIA_API_DIR "" CACHE PATH
	"Jetson Multimedia API headers and sources directory.")
set(JETSON_MULTIMEDIA_LIB_DIR "" CACHE PATH
	"Jetson Multimedia libraries directory.")
# .deb packaging knobs (see the CPack section at the end of this file)
set(NVMPI_DEB_L4T_MAJOR "" CACHE STRING
	"L4T major pinned by libnvmpi1 Depends (36 = JP6, 35 = JP5). Empty = auto-detect.")
set(NVMPI_DEB_VERSION "" CACHE STRING
	"Full .deb version, e.g. 4.0.0-1~jp6. Empty = <project version>-1.")
set(CUDA_INCLUDE_DIR "" CACHE PATH "CUDA headers directory.")
set(CUDA_LIB_DIR "" CACHE PATH "CUDA libraries directory.")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED on)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Resolve paths: explicit -D > probed JP6/JP5 > default
# ---------------------------------------------------------------------------
if(NOT JETSON_MULTIMEDIA_API_DIR)
	set(JETSON_MULTIMEDIA_API_DIR "${JETSON_MULTIMEDIA_API_DIR_DEFAULT}")
endif()

if(WITH_STUBS AND NOT JETSON_MULTIMEDIA_LIB_DIR)
	set(JETSON_MULTIMEDIA_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/stubs")
elseif(NOT JETSON_MULTIMEDIA_LIB_DIR)
	if(EXISTS "${JETSON_MULTIMEDIA_LIB_DIR_JP6}")
		set(JETSON_MULTIMEDIA_LIB_DIR "${JETSON_MULTIMEDIA_LIB_DIR_JP6}")
		set(NVMPI_L4T_MAJOR_DETECTED 36)
		message(STATUS "JetPack 6 detected (nvidia lib dir)")
	elseif(EXISTS "${JETSON_MULTIMEDIA_LIB_DIR_JP5}")
		set(JETSON_MULTIMEDIA_LIB_DIR "${JETSON_MULTIMEDIA_LIB_DIR_JP5}")
		set(NVMPI_L4T_MAJOR_DETECTED 35)
		message(STATUS "JetPack 4/5 detected (tegra lib dir)")
	else()
		set(JETSON_MULTIMEDIA_LIB_DIR "${JETSON_MULTIMEDIA_LIB_DIR_JP5}")
	endif()
endif()

if(NOT CUDA_INCLUDE_DIR)
	set(CUDA_INCLUDE_DIR "${CUDA_INCLUDE_DIR_DEFAULT}")
endif()

if(NOT CUDA_LIB_DIR)
	set(CUDA_LIB_DIR "${CUDA_LIB_DIR_DEFAULT}")
endif()

set(OPT_FLAGS "-Ofast -march=native")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OPT_FLAGS}")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OPT_FLAGS}")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-value -fPIC")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-value -fPIC")

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-rpath-link=${JETSON_MULTIMEDIA_LIB_DIR}")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-rpath-link=${CUDA_LIB_DIR}")
if(NVMPI_MOCK)
	# Without a build RPATH, ld can no longer find libnvmpi.so's NEEDED stub
	# sonames when linking test_api; point it there (link time only).
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath-link=${JETSON_MULTIMEDIA_LIB_DIR}")
endif()

find_library(LIB_V4L2 nvv4l2 PATHS "${JETSON_MULTIMEDIA_LIB_DIR}")
find_library(LIB_NVJPEG nvjpeg PATHS "${JETSON_MULTIMEDIA_LIB_DIR}")
find_package (Threads)

# NvJpegDecoder.cpp calls standard libjpeg functions (jpeg_read_raw_data etc.).
# Jetson ships libjpeg.so.8 but not the -dev symlink; find_library needs the
# unversioned name or NAMES with the soname. Stubs fallback: link -ljpeg
# against the stub libjpeg.so we ship.
find_library(LIB_JPEG NAMES jpeg PATHS "${JETSON_MULTIMEDIA_LIB_DIR}" "/usr/lib/aarch64-linux-gnu")
if(NOT LIB_JPEG)
	find_file(LIB_JPEG_SONAME "libjpeg.so.8" PATHS "/usr/lib/aarch64-linux-gnu" "/usr/lib")
	if(LIB_JPEG_SONAME)
		set(LIB_JPEG "${LIB_JPEG_SONAME}")
	else()
		set(LIB_JPEG "jpeg")
		message(STATUS "libjpeg not found — will link -ljpeg (stubs or system).")
	endif()
endif()

set(NVMPI_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(NVMPI_PUBLIC_HEADER "${NVMPI_INCLUDE_DIR}/nvmpi.h")
set(JETSON_MULTIMEDIA_API_INCLUDE_DIR "${JETSON_MULTIMEDIA_API_DIR}/include")
set(JETSON_MULTIMEDIA_API_SRC_DIR "${JETSON_MULTIMEDIA_API_DIR}/samples/common/classes")

set(NVMPI_INCLUDE_DIRS
	${NVMPI_INCLUDE_DIR}
	${CUDA_INCLUDE_DIR}
	${JETSON_MULTIMEDIA_API_INCLUDE_DIR}
	${JETSON_MULTIMEDIA_API_INCLUDE_DIR}/libjpeg-8b)

if(WITH_VENDORED_NVV4L2)
    set(NVV4L2_ELEMENT_PLANE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/mmapi/NvV4l2ElementPlane.cpp")
    message(STATUS "Using vendored NvV4l2ElementPlane.cpp (pthread_join guard for glibc>=2.34)")
else()
    set(NVV4L2_ELEMENT_PLANE_SRC "${JETSON_MULTIMEDIA_API_SRC_DIR}/NvV4l2ElementPlane.cpp")
    message(STATUS "Using system NvV4l2ElementPlane.cpp")
endif()

set(JETSON_MULTIMEDIA_API_SRC
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvBuffer.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvElement.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvElementProfiler.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvLogging.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvV4l2Element.cpp
    ${NVV4L2_ELEMENT_PLANE_SRC}
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvVideoDecoder.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvVideoEncoder.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvJpegDecoder.cpp
    ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvJpegEncoder.cpp)

set(NVMPI_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_init.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_api.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_capture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_planes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_fd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_close.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_enc_init.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_enc_input.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_enc_input_fd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_enc_api.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_enc_output.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_frame_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_jpeg.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_jpeg_ctx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_jpeg_fd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_dec_jpeg_fd_util.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_jpeg_sof.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_enc_jpeg.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_surface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvmpi_vic.cpp)
    
set(NVMPI_DEP_LIBS ${CMAKE_THREAD_LIBS_INIT} ${LIB_V4L2} ${LIB_NVJPEG} ${LIB_JPEG})

#if NvUtils API is present prefer it to nvbuf_utils 
if(EXISTS "${JETSON_MULTIMEDIA_API_INCLUDE_DIR}/nvbufsurface.h")
	add_definitions(-DWITH_NVUTILS)
	find_library(LIB_NVBUFSURFACE nvbufsurface PATHS "${JETSON_MULTIMEDIA_LIB_DIR}")
	find_library(LIB_NVBUFSURFTRANSFORM nvbufsurftransform PATHS "${JETSON_MULTIMEDIA_LIB_DIR}")
	set(JETSON_MULTIMEDIA_API_SRC ${JETSON_MULTIMEDIA_API_SRC}
		${JETSON_MULTIMEDIA_API_SRC_DIR}/NvBufSurface.cpp)
	set(NVMPI_DEP_LIBS ${NVMPI_DEP_LIBS} ${LIB_NVBUFSURFACE} ${LIB_NVBUFSURFTRANSFORM})
	message(STATUS "Using NvUtils API.")
else ()
	find_library(LIB_NVBUF nvbuf_utils PATHS "${JETSON_MULTIMEDIA_LIB_DIR}")
	set(NVMPI_DEP_LIBS ${NVMPI_DEP_LIBS} ${LIB_NVBUF})
	message(STATUS "Using NvBufUtils API.")
endif()

if(NVMPI_MOCK)
	# The NVIDIA NvJpegDecoder sample needs the Tegra libjpeg and the NVJPG
	# engine, so in mock builds it can create but never decode. Swap in the
	# behavioural model (mock/mock_jpegdec.cpp, issue #128).
	list(REMOVE_ITEM JETSON_MULTIMEDIA_API_SRC ${JETSON_MULTIMEDIA_API_SRC_DIR}/NvJpegDecoder.cpp)
	set(NVMPI_SRC ${NVMPI_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/mock/mock_jpegdec.cpp)
endif()
set(NVMPI_SRC ${NVMPI_SRC} ${JETSON_MULTIMEDIA_API_SRC})

# AV1 decode needs V4L2_PIX_FMT_AV1 from the L4T headers (R36 has it; R35 is
# unverified). Probe the include dirs the library really uses; the library
# gates on the macro itself, the tests get NVMPI_HAVE_AV1 so they can expect
# either behaviour. NVMPI_FORCE_NO_AV1 is test-only: it makes the library
# treat the fourcc as missing so ctest exercises the no-AV1 path on an R36 box.
option(NVMPI_FORCE_NO_AV1 "Test only: build as if V4L2_PIX_FMT_AV1 were missing (AV1 decode create returns NULL)." OFF)
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${NVMPI_INCLUDE_DIRS})
check_cxx_source_compiles("
#include <linux/videodev2.h>
#include \"v4l2_nv_extensions.h\"
#ifndef V4L2_PIX_FMT_AV1
#error no AV1 fourcc
#endif
int main() { return 0; }" NVMPI_HEADERS_HAVE_AV1)
unset(CMAKE_REQUIRED_INCLUDES)
if(NVMPI_HEADERS_HAVE_AV1 AND NOT NVMPI_FORCE_NO_AV1)
	set(NVMPI_HAVE_AV1 1)
else()
	set(NVMPI_HAVE_AV1 0)
endif()
if(NVMPI_FORCE_NO_AV1)
	add_definitions(-DNVMPI_NO_AV1)
endif()
message(STATUS "AV1 decode: NVMPI_HAVE_AV1=${NVMPI_HAVE_AV1} (headers define V4L2_PIX_FMT_AV1: ${NVMPI_HEADERS_HAVE_AV1}, NVMPI_FORCE_NO_AV1=${NVMPI_FORCE_NO_AV1})")

# Tegra's custom libjpeg.so.8 exports jpeg_set_hardware_acceleration_parameters_{enc,dec}
# which NvJpegEncoder.cpp / NvJpegDecoder.cpp call. System libjpeg lacks these symbols;
# in stubs mode, compile no-op implementations so the linker is satisfied.
if(WITH_STUBS)
	set(NVMPI_SRC ${NVMPI_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/stubs/jpeg_tegra_stubs.c)
endif()

#NVIDIA's sample classes are external (host-provided) and not -Wextra clean:
#they trigger benign -Wmissing-field-initializers/-Wunused-parameter noise.
#Suppress those two warnings for these files only; src/ keeps full -Wall -Wextra.
set_source_files_properties(${JETSON_MULTIMEDIA_API_SRC} PROPERTIES
	COMPILE_OPTIONS "-Wno-missing-field-initializers;-Wno-unused-parameter")

add_library(nvmpi SHARED ${NVMPI_SRC})
add_library(nvmpi_static STATIC ${NVMPI_SRC})

set_target_properties(nvmpi_static PROPERTIES OUTPUT_NAME nvmpi)
set_target_properties(nvmpi PROPERTIES VERSION ${PROJECT_VERSION})
set_target_properties(nvmpi PROPERTIES SOVERSION 1)
set_target_properties(nvmpi nvmpi_static PROPERTIES PUBLIC_HEADER ${NVMPI_PUBLIC_HEADER})
set_target_properties(nvmpi PROPERTIES LINK_FLAGS "-Wl,--no-as-needed")
target_link_libraries(nvmpi PRIVATE ${NVMPI_DEP_LIBS})
target_include_directories(nvmpi PRIVATE ${NVMPI_INCLUDE_DIRS})
target_include_directories(nvmpi_static PRIVATE ${NVMPI_INCLUDE_DIRS})
include(GNUInstallDirs)
configure_file(nvmpi.pc.in nvmpi.pc @ONLY)
# Install components: "runtime" (libnvmpi1 deb) and "dev" (libnvmpi-dev deb).
# A plain `cmake --install` without --component installs both.
# NAMELINK_COMPONENT moves the unversioned libnvmpi.so symlink (the FFmpeg
# dlopen name and the link-time name) into dev; the versioned files stay runtime.
install(TARGETS nvmpi
	    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	        COMPONENT runtime
	        NAMELINK_COMPONENT dev
	    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev
	    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev)
install(TARGETS nvmpi_static
	    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev
	    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev)
# pkg-config file: libdir copy only (the former share/pkgconfig duplicate is gone)
install(FILES ${CMAKE_BINARY_DIR}/nvmpi.pc
	    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig COMPONENT dev)
# DEP-5 copyright file, one per package (Debian policy 12.5)
install(FILES packaging/deb/copyright
	    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libnvmpi1 COMPONENT runtime)
install(FILES packaging/deb/copyright
	    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libnvmpi-dev COMPONENT dev)
# changelog.Debian.gz (Debian policy 12.7; lintian: no-changelog). Generated
# at configure time so the entry carries the deb version; gzip -n keeps the
# archive reproducible for a given date.
find_program(NVMPI_GZIP gzip)
if(NVMPI_GZIP)
	set(NVMPI_DEB_MAINTAINER "G.J.R. Timmer <6072255+gjrtimmer@users.noreply.github.com>")
	string(TIMESTAMP NVMPI_DEB_DATE "%a, %d %b %Y %H:%M:%S +0000" UTC)
	configure_file(packaging/deb/changelog.in ${CMAKE_BINARY_DIR}/changelog.Debian @ONLY)
	execute_process(COMMAND ${NVMPI_GZIP} -9nc ${CMAKE_BINARY_DIR}/changelog.Debian
		OUTPUT_FILE ${CMAKE_BINARY_DIR}/changelog.Debian.gz RESULT_VARIABLE NVMPI_GZ_RC)
	if(NOT NVMPI_GZ_RC EQUAL 0)
		message(FATAL_ERROR "gzip of changelog.Debian failed")
	endif()
	install(FILES ${CMAKE_BINARY_DIR}/changelog.Debian.gz
		    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libnvmpi1 COMPONENT runtime)
	install(FILES ${CMAKE_BINARY_DIR}/changelog.Debian.gz
		    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libnvmpi-dev COMPONENT dev)
endif()

find_package(Threads REQUIRED)
include(CTest)
if(NVMPI_MOCK)
  # After CTest so mock/CMakeLists.txt sees BUILD_TESTING; the libs build even
  # with testing off.
  add_subdirectory(mock)
endif()
if(BUILD_TESTING)
  add_executable(test_bufpool test/unit/test_bufpool.cpp)
  target_include_directories(test_bufpool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(test_bufpool PRIVATE Threads::Threads)
  add_test(NAME bufpool COMMAND test_bufpool)

  # Host-only parser test: compiles the scanner directly, no libnvmpi/hardware.
  add_executable(test_jpeg_sof test/unit/test_jpeg_sof.cpp src/nvmpi_jpeg_sof.cpp)
  target_include_directories(test_jpeg_sof PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME jpeg_sof COMMAND test_jpeg_sof)

  add_executable(test_api test/api/test_api.c)
  target_include_directories(test_api PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(test_api PRIVATE nvmpi Threads::Threads)
  target_compile_definitions(test_api PRIVATE NVMPI_HAVE_AV1=${NVMPI_HAVE_AV1})
  if(NVMPI_MOCK)
    # The JPEG fd cases run (instead of skipping on the missing NVJPG node).
    target_compile_definitions(test_api PRIVATE NVMPI_MOCK_JPEG=1)
    # LD_LIBRARY_PATH order: mock libs first (real sonames), then the build
    # dir (libnvmpi.so), then stubs/ only for libs the mock does not provide
    # (libnvjpeg.so). Never register the stub-run "api" test in mock mode.
    set(NVMPI_MOCK_LD "${CMAKE_BINARY_DIR}/mock:${CMAKE_BINARY_DIR}:${CMAKE_CURRENT_SOURCE_DIR}/stubs")
    add_test(NAME mock_selftest COMMAND test_mock)
    add_test(NAME api_mock COMMAND test_api)
    # Decoder data path + fault matrix (needs the mock model; mock-only).
    add_executable(test_api_dec_mock test/api/test_api_dec_mock.c)
    # nvbufsurface.h: the zero-copy cases check the surface behind each borrowed fd (#137).
    target_include_directories(test_api_dec_mock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${JETSON_MULTIMEDIA_API_INCLUDE_DIR} ${CUDA_INCLUDE_DIR})
    target_link_libraries(test_api_dec_mock PRIVATE nvmpi ${LIB_NVBUFSURFACE} Threads::Threads)
    target_compile_definitions(test_api_dec_mock PRIVATE NVMPI_HAVE_AV1=${NVMPI_HAVE_AV1})
    # Decoder suite in 2 shards (NVMPI_TEST_SHARD=k/n, see case_log.h).
    set(NVMPI_DEC_SHARDS 2)
    math(EXPR NVMPI_DEC_LAST "${NVMPI_DEC_SHARDS} - 1")
    foreach(k RANGE ${NVMPI_DEC_LAST})
      add_test(NAME api_mock_dec_${k} COMMAND test_api_dec_mock)
      set_tests_properties(api_mock_dec_${k} PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${NVMPI_MOCK_LD};NVMPI_TEST_SHARD=${k}/${NVMPI_DEC_SHARDS}"
        TIMEOUT 300)
    endforeach()
    # JPEG DMA-BUF output against the NvJPEGDecoder model (mock-only, #128).
    add_executable(test_api_jpeg_mock test/api/test_api_jpeg_mock.c)
    # nvbufsurface.h: the test maps the pool surface behind each borrowed fd.
    target_include_directories(test_api_jpeg_mock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${JETSON_MULTIMEDIA_API_INCLUDE_DIR} ${CUDA_INCLUDE_DIR})
    target_link_libraries(test_api_jpeg_mock PRIVATE nvmpi ${LIB_NVBUFSURFACE} Threads::Threads)
    add_test(NAME api_mock_jpeg COMMAND test_api_jpeg_mock)
    set_tests_properties(api_mock_jpeg PROPERTIES
      ENVIRONMENT "LD_LIBRARY_PATH=${NVMPI_MOCK_LD}"
      TIMEOUT 300)
    # Encoder data path, teardown and fault matrix (mock-only).
    add_executable(test_api_enc_mock test/api/test_api_enc_mock.c)
    target_include_directories(test_api_enc_mock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_api_enc_mock PRIVATE nvmpi Threads::Threads)
    # Encoder suite in 3 shards (NVMPI_TEST_SHARD=k/n, see case_log.h): the
    # ASan job is bound by its longest ctest, so one big suite is split.
    set(NVMPI_ENC_SHARDS 3)
    math(EXPR NVMPI_ENC_LAST "${NVMPI_ENC_SHARDS} - 1")
    foreach(k RANGE ${NVMPI_ENC_LAST})
      add_test(NAME api_mock_enc_${k} COMMAND test_api_enc_mock)
      set_tests_properties(api_mock_enc_${k} PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${NVMPI_MOCK_LD};NVMPI_TEST_SHARD=${k}/${NVMPI_ENC_SHARDS}"
        TIMEOUT 300)
    endforeach()
    set_tests_properties(mock_selftest PROPERTIES
      ENVIRONMENT "LD_LIBRARY_PATH=${NVMPI_MOCK_LD}"
      TIMEOUT 300)
    # test_api's AV1 hardware case checks a 320x240 frame; the mock has no AV1
    # header parser, so it takes the stream size from the environment.
    set_tests_properties(api_mock PROPERTIES
      ENVIRONMENT "LD_LIBRARY_PATH=${NVMPI_MOCK_LD};NVMPI_MOCK_WIDTH=320;NVMPI_MOCK_HEIGHT=240"
      TIMEOUT 300)
  else()
    add_test(NAME api COMMAND test_api)
  endif()
endif()

# ---------------------------------------------------------------------------
# CPack .deb packaging: two packages, libnvmpi1 (runtime) + libnvmpi-dev.
#   cmake -S . -B build-deb -DCMAKE_INSTALL_PREFIX=/usr \
#         -DCMAKE_INSTALL_LIBDIR=lib/aarch64-linux-gnu -DNVMPI_DEB_VERSION=4.0.0-1~jp6
#   cmake --build build-deb && (cd build-deb && cpack -G DEB)
# scripts/package-deb.sh wraps this. Dependencies are listed by hand: the NVIDIA
# libs are not resolvable by dpkg-shlibdeps, so SHLIBDEPS stays OFF.
# ---------------------------------------------------------------------------
if(NOT NVMPI_DEB_L4T_MAJOR)
	if(NVMPI_L4T_MAJOR_DETECTED)
		set(NVMPI_DEB_L4T_MAJOR "${NVMPI_L4T_MAJOR_DETECTED}")
	else()
		set(NVMPI_DEB_L4T_MAJOR 36)
	endif()
endif()
math(EXPR NVMPI_DEB_L4T_NEXT "${NVMPI_DEB_L4T_MAJOR} + 1")
if(NOT NVMPI_DEB_VERSION)
	set(NVMPI_DEB_VERSION "${PROJECT_VERSION}-1")
endif()
if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
	message(WARNING "CMAKE_INSTALL_PREFIX is '${CMAKE_INSTALL_PREFIX}', not /usr: "
		"nvmpi.pc in the .deb will carry the wrong prefix. Configure with -DCMAKE_INSTALL_PREFIX=/usr to package.")
endif()

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "libnvmpi")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_CONTACT "G.J.R. Timmer <6072255+gjrtimmer@users.noreply.github.com>")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL runtime dev)
set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS OFF)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_VERSION "${NVMPI_DEB_VERSION}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/gjrtimmer/jetson-nvmpi")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
# triggers must keep their 0644 mode (lintian), so do not force 0755 on control files
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)
# Strip the shipped library (lintian: unstripped-binary-or-object)
set(CPACK_STRIP_FILES TRUE)
# shlibs control file so packages linking libnvmpi.so.1 get a libnvmpi1 dependency
configure_file(packaging/deb/shlibs.in ${CMAKE_BINARY_DIR}/shlibs @ONLY)

# Per-package fields. Old single package "libnvmpi" (< 4.0.0) owned all these files.
set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "libnvmpi1")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "libs")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_MULTIARCH "same")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS
	"nvidia-l4t-multimedia (>= ${NVMPI_DEB_L4T_MAJOR}), nvidia-l4t-multimedia (<< ${NVMPI_DEB_L4T_NEXT}), libc6, libstdc++6, libjpeg-turbo8")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_REPLACES "libnvmpi (<< 4.0.0)")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_BREAKS "libnvmpi (<< 4.0.0)")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/triggers;${CMAKE_BINARY_DIR}/shlibs")
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION
	"NVIDIA Jetson hardware video codec library (runtime)
 libnvmpi wraps the L4T Multimedia API (V4L2 NVDEC/NVENC, NvBufSurface and
 VIC) behind a small C API. It is used by the jetson-ffmpeg *_nvmpi codecs.")

set(CPACK_DEBIAN_DEV_PACKAGE_NAME "libnvmpi-dev")
set(CPACK_DEBIAN_DEV_PACKAGE_SECTION "libdevel")
set(CPACK_DEBIAN_DEV_PACKAGE_MULTIARCH "same")
set(CPACK_DEBIAN_DEV_PACKAGE_DEPENDS "libnvmpi1 (= ${NVMPI_DEB_VERSION})")
set(CPACK_DEBIAN_DEV_PACKAGE_REPLACES "libnvmpi (<< 4.0.0)")
set(CPACK_DEBIAN_DEV_PACKAGE_BREAKS "libnvmpi (<< 4.0.0)")
set(CPACK_COMPONENT_DEV_DESCRIPTION
	"NVIDIA Jetson hardware video codec library (development files)
 Header files, the static library, the libnvmpi.so development symlink and
 the pkg-config file needed to build against libnvmpi.")
include(CPack)
