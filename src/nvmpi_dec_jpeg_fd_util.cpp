/*
 * nvmpi_dec_jpeg_fd_util.cpp — helpers of the DMA-BUF JPEG decode mode:
 * JPEG header screening and per-buffer plane geometry (NvUtils builds).
 */
#include "nvmpi_dec_jpeg_internal.h"
#include "nvmpi_jpeg_sof.h"
#include "nvmpi_fd_geometry.h"

/* Screen the JPEG header: accept only 3-component baseline (SOF0/SOF1).
 * Logs the reason and returns false otherwise. Untrusted input; the scan
 * itself is bounds-checked (nvmpi_jpeg_sof.cpp). */
bool jpegFdCheckSof(const unsigned char *d, unsigned long size,
                    unsigned *width, unsigned *height)
{
	unsigned nf = 0;
	switch (nvmpi_jpeg_scan_sof(d, size, &nf, width, height)) {
	case NVMPI_SOF_OK:
		return true;
	case NVMPI_SOF_PROGRESSIVE:
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "progressive JPEG not supported by hardware decoder (SOF2)");
		break;
	case NVMPI_SOF_UNSUPPORTED:
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "unsupported JPEG process (not SOF0/SOF1)");
		break;
	case NVMPI_SOF_COMPONENTS:
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "%u-component JPEG not supported in DMA-BUF mode (grayscale/CMYK rejected)", nf);
		break;
	default:
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "no SOF marker found (truncated or malformed JPEG)");
		break;
	}
	return false;
}

#ifdef WITH_NVUTILS
/* Plane layout comes from the shared fill (nvmpi_fd_geometry.cpp); the JPEG
 * path adds its displayed size and the full-range flag (JFIF is full range). */
bool jpegFdFillGeometry(nvPixFormat fmt, nvmpi_frame_buffer *fb,
                         uint32_t w, uint32_t h, nvmpi_fd_frame *fr)
{
	if (!nvmpi_fd_fill_geometry(fmt, fb, fr))
		return false;
	fr->width = w;
	fr->height = h;
	fr->flags = NVMPI_FD_FRAME_FULL_RANGE;
	return true;
}
#endif
