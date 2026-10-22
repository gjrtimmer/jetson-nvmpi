/*
 * mock_jpegenc.cpp - minimal mock of the NvJPEGEncoder sample class (#151).
 * Compiled INTO libnvmpi in NVMPI_MOCK builds in place of the NVIDIA sample
 * NvJpegEncoder.cpp, whose libjpeg (Tegra build) and NVJPG engine are absent
 * on a mock runner: the real class cannot even be created there.
 *
 * It implements what libnvmpi calls: createJPEGEncoder and encodeFromFd (plus
 * encodeFromBuffer, setCropRect and setScaledEncodeParams so the class
 * links). encodeFromFd checks the fd is a live NvBufSurface and returns a
 * 4-byte stand-in bitstream (SOI, EOI) in a malloc'd buffer, the way libjpeg
 * hands the output back: the caller frees it with free(). No pixels are
 * encoded.
 */
#include "NvJpegEncoder.h"
#include "nvbufsurface.h"

#include <cstdlib>
#include <cstring>
#include <new>

NvJPEGEncoder::NvJPEGEncoder(const char *comp_name)
	: NvElement(comp_name, valid_fields)
{
	memset(&cinfo, 0, sizeof(cinfo));
	memset(&jerr, 0, sizeof(jerr));
}

NvJPEGEncoder::~NvJPEGEncoder()
{
}

NvJPEGEncoder *NvJPEGEncoder::createJPEGEncoder(const char *comp_name)
{
	/* Freed by the caller (libnvmpi deletes it in close). */
	return new (std::nothrow) NvJPEGEncoder(comp_name);
}

int NvJPEGEncoder::encodeFromFd(int fd, J_COLOR_SPACE /* color_space */,
                                unsigned char **out_buf,
                                unsigned long &out_buf_size, int /* quality */)
{
	static const unsigned char k_jpeg[4] = { 0xff, 0xd8, 0xff, 0xd9 };
	NvBufSurface *s = nullptr;

	if (!out_buf || NvBufSurfaceFromFd(fd, (void **)&s) != 0 || !s)
		return -1;
	if (!*out_buf || out_buf_size < sizeof(k_jpeg)) {
		/* Like libjpeg: grow (or allocate) the caller's malloc'd buffer. */
		unsigned char *n = (unsigned char *)realloc(*out_buf, sizeof(k_jpeg));
		if (!n)
			return -1;
		*out_buf = n;
	}
	memcpy(*out_buf, k_jpeg, sizeof(k_jpeg));
	out_buf_size = sizeof(k_jpeg);
	return 0;
}

int NvJPEGEncoder::encodeFromBuffer(NvBuffer & /* buffer */,
                                    J_COLOR_SPACE /* color_space */,
                                    unsigned char ** /* out_buf */,
                                    unsigned long & /* out_buf_size */,
                                    int /* quality */)
{
	return -1;
}

void NvJPEGEncoder::setCropRect(uint32_t, uint32_t, uint32_t, uint32_t)
{
}

void NvJPEGEncoder::setScaledEncodeParams(uint32_t, uint32_t)
{
}
