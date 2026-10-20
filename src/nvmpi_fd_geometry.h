/*
 * nvmpi_fd_geometry.h — plane geometry of a pitch-linear pool buffer, shared
 * by the decoder fd2 path and the JPEG fd path (nvmpi_fd_geometry.cpp).
 */
#pragma once

#include "nvmpi.h"
#include "nvmpi_frame_buffer.hpp"

/* Fill the layout part of *fr from the buffer's own allocator data: fd,
 * format, num_planes, pitch[], offset[], size (struct_size = sizeof). The
 * rest (width, height, timestamp, flags) is zeroed for the caller to set.
 * Returns false (after logging) for an unexpected plane count or an
 * implausible layout. NV12/P010 need 2 planes, YUV420 needs 3. */
bool nvmpi_fd_fill_geometry(nvPixFormat fmt, nvmpi_frame_buffer *fb,
                            nvmpi_fd_frame *fr);
