/*
 * nvmpi_jpeg_sof.h — dependency-free JPEG start-of-frame scanner.
 * Used by the DMA-BUF JPEG decode mode to screen input before decoding and
 * unit-tested on the host (test/unit/test_jpeg_sof.cpp).
 */
#pragma once

enum nvmpi_jpeg_sof_result {
	NVMPI_SOF_OK = 0,        /* SOF0/SOF1 with 3 components */
	NVMPI_SOF_PROGRESSIVE,   /* SOF2 */
	NVMPI_SOF_UNSUPPORTED,   /* any other SOFn (lossless, arithmetic, ...) */
	NVMPI_SOF_COMPONENTS,    /* baseline but not 3 components (gray, CMYK) */
	NVMPI_SOF_MALFORMED      /* no SOF before SOS, or truncated */
};

/* Walks the marker segments of d[0..size). Never reads outside the buffer.
 * On OK / COMPONENTS fills *comps, *width, *height (any may be NULL). */
nvmpi_jpeg_sof_result nvmpi_jpeg_scan_sof(const unsigned char *d, unsigned long size,
                                          unsigned *comps, unsigned *width,
                                          unsigned *height);
