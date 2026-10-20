/*
 * nvmpi_dec_jpeg_fd_util.cpp — helpers of the DMA-BUF JPEG decode mode:
 * JPEG header screening and per-buffer plane geometry (NvUtils builds).
 */
#include "nvmpi_dec_jpeg_internal.h"
#include "nvmpi_jpeg_sof.h"

/* Screen the JPEG header: accept only 3-component baseline (SOF0/SOF1).
 * Logs the reason and returns false otherwise. Untrusted input; the scan
 * itself is bounds-checked (nvmpi_jpeg_sof.cpp). */
bool jpegFdCheckSof(const unsigned char *d, unsigned long size)
{
	unsigned nf = 0;
	switch (nvmpi_jpeg_scan_sof(d, size, &nf, NULL, NULL)) {
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
/* Fill the geometry from the buffer's own plane params (never pitch*height). */
bool jpegFdFillGeometry(nvPixFormat fmt, nvmpi_frame_buffer *fb,
                         uint32_t w, uint32_t h, nvmpi_fd_frame *fr)
{
	const NvBufSurfaceParams &sp = fb->dst_dma_surface->surfaceList[0];
	const NvBufSurfacePlaneParams &pp = sp.planeParams;
	uint32_t want = (fmt == NV_PIX_NV12) ? 2 : 3;

	if (pp.num_planes != want || sp.dataSize == 0) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "unexpected plane count %u", pp.num_planes);
		return false;
	}
	memset(fr, 0, sizeof(*fr));
	for (uint32_t i = 0; i < want; i++) {
		/* A pitch below the row size or an offset outside the buffer would
		 * make the consumer read out of bounds. */
		if (pp.pitch[i] < pp.width[i] * pp.bytesPerPix[i] || pp.offset[i] >= sp.dataSize) {
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "implausible plane %u geometry", i);
			return false;
		}
		fr->pitch[i] = pp.pitch[i];
		fr->offset[i] = pp.offset[i];
	}
	fr->struct_size = sizeof(*fr);
	fr->fd = fb->dst_dma_fd;
	fr->width = w;
	fr->height = h;
	fr->format = fmt;
	fr->num_planes = want;
	fr->size = sp.dataSize;
	fr->flags = NVMPI_FD_FRAME_FULL_RANGE;
	return true;
}
#endif
