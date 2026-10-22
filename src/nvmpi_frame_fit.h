/*
 * nvmpi_frame_fit.h - does a decoded frame fit the caller's nvFrame? (#149)
 *
 * Shared by the V4L2 decoder (nvmpi_dec_api.cpp, nvmpi_dec_frame.cpp) and the
 * JPEG decoder (nvmpi_dec_jpeg.cpp). Header-only, no libnvmpi state.
 *
 * Why: the copy paths write g->height[p] rows of g->linedatasize[p] bytes into
 * frame->payload[p], stepping by frame->linesize[p]. The sizes come from the
 * decoded stream (untrusted, can grow mid-stream), the destination is caller
 * memory the library knows nothing about. Every copy must be proven to fit
 * BEFORE the first byte is written.
 */
#ifndef NVMPI_FRAME_FIT_H
#define NVMPI_FRAME_FIT_H

#include <nvmpi.h>
#include <stddef.h>

//Layout of one decoded frame (the generation's cached geometry).
struct nvmpi_frame_geom {
	unsigned int width, height;                //frame size in pixels
	unsigned int num_planes;                   //1..3
	const unsigned int *plane_height;          //lines per plane
	const unsigned int *plane_linedata;        //valid bytes per line
};

//1 when the caller declared a capacity for every plane of the layout
//(payload_size[p] != 0 for p < num_planes), else 0.
static inline int nvmpi_frame_has_caps(const nvFrame *f, unsigned int n)
{
	for (unsigned int p = 0; p < n && p < 3; p++)
		if (f->payload_size[p] == 0)
			return 0;
	return n != 0;
}

//Bytes the copy touches in plane p when the destination stride is `stride`:
//(rows-1)*stride + one row of data (the last row has no trailing stride).
//Returns 0 on arithmetic overflow (a size_t that wrapped would pass the
//capacity check and then under-allocate the bound). rows == 0 needs 0 bytes.
static inline int nvmpi_plane_need(unsigned int rows, unsigned int stride,
	unsigned int linedata, size_t *need)
{
	size_t span;

	if (rows == 0) {
		*need = 0;
		return 1;
	}
	if (__builtin_mul_overflow((size_t)(rows - 1), (size_t)stride, &span) ||
	    __builtin_add_overflow(span, (size_t)linedata, need))
		return 0;
	return 1;
}

//Pointer and stride sanity common to every copy: each plane needs a
//destination pointer and a stride that holds one row of data (a smaller
//stride would make consecutive rows overlap and, for the last row, still
//write linedata bytes). Returns NVMPI_ERR_FRAME_SIZE or 0.
static inline int nvmpi_frame_check_ptrs(const nvFrame *f,
	const nvmpi_frame_geom *g)
{
	for (unsigned int p = 0; p < g->num_planes && p < 3; p++)
		if (!f->payload[p] || f->linesize[p] < g->plane_linedata[p])
			return NVMPI_ERR_FRAME_SIZE;
	return 0;
}

//Capacity check with the caller's payload_size[]: returns 0 when every plane
//fits, NVMPI_ERR_FRAME_SIZE otherwise. Nothing is written.
static inline int nvmpi_frame_check_caps(const nvFrame *f,
	const nvmpi_frame_geom *g)
{
	if (nvmpi_frame_check_ptrs(f, g) != 0)
		return NVMPI_ERR_FRAME_SIZE;
	for (unsigned int p = 0; p < g->num_planes && p < 3; p++) {
		size_t need;

		if (!nvmpi_plane_need(g->plane_height[p], f->linesize[p],
		                      g->plane_linedata[p], &need) ||
		    need > f->payload_size[p])
			return NVMPI_ERR_FRAME_SIZE;
	}
	return 0;
}

//Report the generation's size back to the caller (after a copy, or after a
//refusal so the caller can reallocate): width/height, format and the
//capacity the frame needs per plane (height * bytes per line, i.e. a packed
//frame; a caller using a wider stride adds the difference itself).
static inline void nvmpi_frame_report(nvFrame *f, const nvmpi_frame_geom *g,
	nvPixFormat type)
{
	f->width = g->width;
	f->height = g->height;
	f->type = type;
	for (unsigned int p = 0; p < g->num_planes && p < 3; p++)
		f->payload_size[p] =
			(unsigned long)g->plane_height[p] * g->plane_linedata[p];
}

#endif /* NVMPI_FRAME_FIT_H */
