# Behavioural mock of the Jetson userspace libraries (issue #98).
#
# Builds drop-in replacements carrying the REAL sonames into <build>/mock:
#   libnvv4l2.so (+ libv4l2.so.0 symlink)   V4L2 M2M device mock
#   libnvbufsurface.so.1.0.0 (+ .so)        NvBufSurface mock (memfd)
#   libnvbufsurftransform.so.1.0.0 (+ .so)  NvBufSurfTransform mock (memcpy)
# Included from the top-level CMakeLists.txt only when NVMPI_MOCK=ON.
set(MOCK_OUT "${CMAKE_BINARY_DIR}/mock")

set(MOCK_INCLUDE_DIRS ${JETSON_MULTIMEDIA_API_INCLUDE_DIR} ${CUDA_INCLUDE_DIR})

# Hidden by default; only the MOCK_EXPORT'ed API leaks, like the real libs.
# (each source defines _GNU_SOURCE itself for memfd_create / eventfd)
set(MOCK_C_FLAGS -fvisibility=hidden -O2 -g)

function(nvmpi_mock_lib target outname soname)
	add_library(${target} SHARED ${ARGN})
	target_include_directories(${target} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR} ${MOCK_INCLUDE_DIRS})
	target_compile_options(${target} PRIVATE ${MOCK_C_FLAGS})
	# Mock .so files must resolve every symbol on their own (fail the build,
	# not the test run) and carry exactly the soname the consumer NEEDs.
	target_link_libraries(${target} PRIVATE Threads::Threads ${CMAKE_DL_LIBS}
		-Wl,--no-undefined -Wl,-soname,${soname})
	set_target_properties(${target} PROPERTIES
		OUTPUT_NAME ${outname}
		NO_SONAME ON                       # we pass -soname ourselves
		LIBRARY_OUTPUT_DIRECTORY "${MOCK_OUT}")
endfunction()

# libnvv4l2: soname libv4l2.so.0 (same as stubs/ and the NVIDIA library).
nvmpi_mock_lib(nvv4l2_mock nvv4l2 libv4l2.so.0
	mock_v4l2_core.c mock_v4l2_queue.c mock_v4l2_stream.c
	mock_v4l2_fmt.c mock_v4l2_models.c mock_faults.c
	mock_dec.c mock_dec_ctl.c mock_dec_nal.c
	mock_enc.c mock_enc_ctl.c mock_enc_nal.c mock_enc_h264.c mock_enc_pic.c
	mock_enc_hevc.c mock_enc_hevc_slice.c mock_enc_cabac.c mock_enc_av1.c)
add_custom_command(TARGET nvv4l2_mock POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E create_symlink libnvv4l2.so
		"${MOCK_OUT}/libv4l2.so.0")

# libnvbufsurface / libnvbufsurftransform: the filename IS the soname
# (libX.so.1.0.0) and libX.so is a symlink, mirroring the NVIDIA packaging.
set(nvbufsurface_SRCS mock_nvbufsurface.c mock_nvbufsurface_ops.c mock_faults.c)
set(nvbufsurftransform_SRCS mock_nvbufsurftransform.c)
foreach(lib nvbufsurface nvbufsurftransform)
	nvmpi_mock_lib(${lib}_mock ${lib} lib${lib}.so.1.0.0 ${${lib}_SRCS})
	set_target_properties(${lib}_mock PROPERTIES SUFFIX ".so.1.0.0")
	add_custom_command(TARGET ${lib}_mock POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E create_symlink lib${lib}.so.1.0.0
			"${MOCK_OUT}/lib${lib}.so")
endforeach()

# Mock self-test: exercises the mock libs directly (no libnvmpi), so a mock
# bug is distinguishable from a libnvmpi bug. Part of #98's gate.
if(BUILD_TESTING)
	add_executable(test_mock
		${CMAKE_SOURCE_DIR}/test/mock/test_mock.c
		${CMAKE_SOURCE_DIR}/test/mock/test_mock_dec.c
		${CMAKE_SOURCE_DIR}/test/mock/test_mock_fault.c
		${CMAKE_SOURCE_DIR}/test/mock/test_mock_enc.c
		${CMAKE_SOURCE_DIR}/test/mock/test_mock_hevc.c
		${CMAKE_SOURCE_DIR}/test/mock/test_mock_hevc_slice.c
		${CMAKE_SOURCE_DIR}/test/mock/test_mock_vui.c
		${CMAKE_SOURCE_DIR}/test/mock/test_mock_h264.c
		${CMAKE_SOURCE_DIR}/test/mock/test_mock_xform.c)
	# mock/: the CABAC tables, shared with the test's decoder.
	target_include_directories(test_mock PRIVATE ${MOCK_INCLUDE_DIRS}
		${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(test_mock PRIVATE
		nvv4l2_mock nvbufsurface_mock nvbufsurftransform_mock Threads::Threads)
endif()
