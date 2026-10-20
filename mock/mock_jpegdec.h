/*
 * mock_jpegdec.h - contract between the NvJPEGDecoder mock (mock_jpegdec.cpp,
 * compiled into libnvmpi in mock builds) and the tests that check its pixels.
 * Plain C so the C test suites can include it.
 */
#ifndef NVMPI_MOCK_JPEGDEC_H
#define NVMPI_MOCK_JPEGDEC_H

/* The decoder reports (and allocates) the picture size rounded UP to a
 * multiple of this value, like the Orin's NVJPG does (it hands back an
 * aligned surface). 0 or 1 = exact size. Read on every decode. */
#define MOCK_JPEG_PAD_ENV     "NVMPI_MOCK_JPEG_PAD"
#define MOCK_JPEG_PAD_DEFAULT 16u
#define MOCK_JPEG_PAD_MAX     256u

/* Sample of the decoded I420 surface at (x, y) of plane `plane` (0 = Y,
 * 1 = U, 2 = V), in that plane's own pixel coordinates of the PADDED
 * decoder surface. Distinct per plane and linear in x and y, so a scaled
 * or shifted copy (instead of a crop) differs at almost every sample. */
static inline unsigned char mock_jpeg_pattern(unsigned plane, unsigned x,
                                              unsigned y)
{
    return (unsigned char)((x * 5u + y * 11u + plane * 71u + 29u) & 0xffu);
}

#endif /* NVMPI_MOCK_JPEGDEC_H */
