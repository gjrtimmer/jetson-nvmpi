/*
 * nvmpi_dec_jpeg_fd_util.cpp — helpers of the DMA-BUF JPEG decode mode:
 * JPEG header screening and per-buffer plane geometry (NvUtils builds).
 */
#include "nvmpi_dec_jpeg_internal.h"


/* Marker walk: accept only 3-component baseline (SOF0/SOF1). Returns true
 * when ok; logs the reason otherwise. Untrusted input: every read is bounds
 * checked against size. */
bool jpegFdCheckSof(const unsigned char *d, unsigned long size)
{
	unsigned long pos = 0;
	while (pos + 1 < size) {
		if (d[pos] != 0xFF) { pos++; continue; }
		while (pos + 1 < size && d[pos + 1] == 0xFF) pos++;
		if (pos + 1 >= size) break;
		unsigned char m = d[pos + 1];
		pos += 2;
		if (m == 0xC2) {
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "progressive JPEG not supported by hardware decoder (SOF2)");
			return false;
		}
		if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
			if (m > 0xC1) {
				NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "unsupported JPEG process (SOF%u)", m - 0xC0);
				return false;
			}
			if (pos + 6 >= size) break;
			unsigned nf = d[pos + 5];   /* len(2) P(1) Y(2) X(2) then Nf */
			if (nf != 3) {
				NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "%u-component JPEG not supported in DMA-BUF mode (grayscale/CMYK rejected)", nf);
				return false;
			}
			return true;
		}
		if (m == 0xDA) break;
		if (m != 0x00 && m != 0x01 && !(m >= 0xD0 && m <= 0xD9)) {
			if (pos + 1 >= size) break;
			unsigned len = (d[pos] << 8) | d[pos + 1];
			if (len < 2) break;
			pos += len;
		}
	}
	NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "jpeg", "no SOF marker found");
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
