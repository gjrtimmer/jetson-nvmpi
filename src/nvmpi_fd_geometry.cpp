/*
 * nvmpi_fd_geometry.cpp — plane geometry for DMA-BUF frame hand-out
 * (libnvmpi). Single source of pitch[]/offset[]/size for nvmpi_fd_frame,
 * used by the decoder (get_frame_fd2) and the JPEG decoder (fd mode).
 */
#include "nvmpi_fd_geometry.h"
#include "nvmpi_log.h"
#include "nvUtils2NvBuf.h"
#include <string.h>

bool nvmpi_fd_fill_geometry(nvPixFormat fmt, nvmpi_frame_buffer *fb,
                            nvmpi_fd_frame *fr)
{
	/* NV12 and P010 are semi-planar (Y + interleaved UV); YUV420 is 3 planes */
	const uint32_t want = (fmt == NV_PIX_YUV420) ? 3 : 2;
	uint32_t num_planes, pitch[3], offset[3];
	uint64_t total;

#ifdef WITH_NVUTILS
	const NvBufSurfaceParams &sp = fb->dst_dma_surface->surfaceList[0];
	const NvBufSurfacePlaneParams &pp = sp.planeParams;
	num_planes = pp.num_planes;
	/* dataSize can be 0 on some surface types: fall back to the end of the
	 * last plane (offset + psize). */
	total = sp.dataSize;
	if (total == 0 && num_planes >= 1 && num_planes <= 3)
		total = (uint64_t)pp.offset[num_planes - 1] + pp.psize[num_planes - 1];
#else
	NvBufferParams pp;
	int ret = NvBufferGetParams(fb->dst_dma_fd, &pp);
	if (ret < 0) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "fd", "NvBufferGetParams failed (code=%d)", ret);
		return false;
	}
	num_planes = pp.num_planes;
	total = pp.memsize;
	if (total == 0 && num_planes >= 1 && num_planes <= 3)
		total = (uint64_t)pp.offset[num_planes - 1] + pp.psize[num_planes - 1];
#endif
	if (num_planes != want || total == 0) {
		NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "fd", "unexpected surface: planes %u (want %u) size %llu",
		              num_planes, want, (unsigned long long)total);
		return false;
	}
	for (uint32_t i = 0; i < want; i++) {
		pitch[i] = pp.pitch[i];
		offset[i] = pp.offset[i];
		/* A zero pitch, a pitch below the row size (NvUtils only: the legacy
		 * params lack bytesPerPix) or an offset outside the buffer would make
		 * the consumer read out of bounds. */
		bool bad = pitch[i] == 0 || offset[i] >= total;
#ifdef WITH_NVUTILS
		bad = bad || pitch[i] < pp.width[i] * pp.bytesPerPix[i];
#endif
		if (bad) {
			NVMPI_LOG_SUB(NVMPI_LOG_ERROR, "fd", "implausible plane %u geometry", i);
			return false;
		}
	}

	memset(fr, 0, sizeof(*fr));
	for (uint32_t i = 0; i < want; i++) {
		fr->pitch[i] = pitch[i];
		fr->offset[i] = offset[i];
	}
	fr->struct_size = sizeof(*fr);
	fr->fd = fb->dst_dma_fd;
	fr->format = fmt;
	fr->num_planes = want;
	fr->size = total;
	return true;
}
