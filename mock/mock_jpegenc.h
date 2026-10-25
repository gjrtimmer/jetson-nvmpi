/*
 * mock_jpegenc.h - contract between the NvJPEGEncoder mock (mock_jpegenc.cpp,
 * compiled into libnvmpi in mock builds) and the tests. Plain C so the C test
 * suites can include it.
 *
 * The mock encodes no pixels. It records what the last encodeFromFd call was
 * handed, so a test can tell whether put_frame_fd encoded the caller's
 * surface (direct path) or the encoder's own input buffer (VIC path).
 */
#ifndef NVMPI_MOCK_JPEGENC_H
#define NVMPI_MOCK_JPEGENC_H

#include <stdint.h>

struct mock_jpegenc_last {
    unsigned calls;         /* encodeFromFd calls so far (successful or not) */
    int fd;                 /* fd of the last call, -1 before the first      */
    uint32_t color_format;  /* NvBufSurfaceColorFormat of that surface       */
    uint32_t width, height; /* its plane 0 size (planeParams)                */
    uint32_t sof_width, sof_height; /* picture size the JPEG would carry     */
};

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot of the last encodeFromFd call (all zero / fd -1 before any). */
void nvmpi_mock_jpegenc_last(struct mock_jpegenc_last *out);

#ifdef __cplusplus
}
#endif

#endif /* NVMPI_MOCK_JPEGENC_H */
