/*
 * nvmpi_alloc.h — failure-safe batch allocation of NvBufSurface DMA buffers
 * (NvUtils API, JetPack 5+).
 *
 * NVIDIA's sample helper NvBufSurf::NvAllocate() ignores the result of
 * NvBufSurfaceAllocate(): when the allocator is exhausted it dereferences
 * the NULL surface and crashes the whole process (NvBufSurface.cpp, loop
 * body), instead of returning the error it was handed. libnvmpi must turn an
 * out-of-memory condition into an error return, so decoder buffer allocation
 * goes through nvmpi_alloc_surfaces(), which does the same work but checks
 * every call and rolls back the buffers already allocated in the batch.
 *
 * Allocated here; each fd is freed by NvBufferDestroy (NvBufSurf::NvDestroy)
 * by the caller, exactly like the buffers NvAllocate returns.
 */
#ifndef NVMPI_ALLOC_H
#define NVMPI_ALLOC_H

#include "nvUtils2NvBuf.h"
#include <string.h>

#ifdef WITH_NVUTILS
/* Allocate numBuffers surfaces described by p; fd[i] receives the dma-buf fd.
 * Returns 0 on success. On failure returns -1 with every fd of this call
 * closed and set to -1 (all-or-nothing, like NvAllocate's contract). */
static inline int nvmpi_alloc_surfaces(NvBufSurf::NvCommonAllocateParams *p,
				       uint32_t numBuffers, int *fd)
{
	NvBufSurfaceAllocateParams in;
	uint32_t i, j;

	if (numBuffers < 1 || !p || !fd)
		return -1;
	memset(&in, 0, sizeof(in));
	in.params.width = p->width;
	in.params.height = p->height;
	in.params.memType = p->memType;
	in.params.layout = p->layout;
	in.params.colorFormat = p->colorFormat;
	in.memtag = p->memtag;
	in.chromaSubsampling.chromaLocHoriz = p->chromaSubsampling.chromaLocHoriz;
	in.chromaSubsampling.chromaLocVert = p->chromaSubsampling.chromaLocVert;

	for (i = 0; i < numBuffers; i++) {
		NvBufSurface *s = NULL;

		/* The sample helper does not check s: a failed allocation
		 * (ret < 0, s possibly NULL) would be dereferenced. */
		if (NvBufSurfaceAllocate(&s, 1, &in) < 0 || !s ||
		    s->surfaceList == NULL) {
			for (j = 0; j < i; j++) {	/* roll back the batch */
				(void)NvBufSurf::NvDestroy(fd[j]);
				fd[j] = -1;
			}
			return -1;
		}
		fd[i] = (int)s->surfaceList[0].bufferDesc;
		s->numFilled = 1;
	}
	return 0;
}
#endif /* WITH_NVUTILS */

#endif /* NVMPI_ALLOC_H */
