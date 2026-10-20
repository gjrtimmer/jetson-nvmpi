/*
 * nvmpi_jpeg_sof.cpp — JPEG start-of-frame scanner (see nvmpi_jpeg_sof.h).
 */
#include "nvmpi_jpeg_sof.h"

nvmpi_jpeg_sof_result nvmpi_jpeg_scan_sof(const unsigned char *d, unsigned long size,
                                          unsigned *comps, unsigned *width,
                                          unsigned *height)
{
	unsigned long pos = 0;
	if (!d)
		return NVMPI_SOF_MALFORMED;
	while (pos + 1 < size) {
		if (d[pos] != 0xFF) { pos++; continue; }
		/* Fill bytes: any number of 0xFF may precede a marker code. */
		while (pos + 1 < size && d[pos + 1] == 0xFF) pos++;
		if (pos + 1 >= size) break;
		unsigned char m = d[pos + 1];
		pos += 2;   /* pos now at the segment length field */

		/* SOFn: C0-CF except DHT (C4), JPG (C8), DAC (CC). */
		if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
			if (m == 0xC2)
				return NVMPI_SOF_PROGRESSIVE;
			if (m > 0xC1)
				return NVMPI_SOF_UNSUPPORTED;
			/* Lf(2) P(1) Y(2) X(2) Nf(1): Nf is the 8th byte (pos+7). */
			if (pos + 7 >= size)
				return NVMPI_SOF_MALFORMED;
			unsigned h = ((unsigned)d[pos + 3] << 8) | d[pos + 4];
			unsigned w = ((unsigned)d[pos + 5] << 8) | d[pos + 6];
			unsigned nf = d[pos + 7];
			if (comps) *comps = nf;
			if (width) *width = w;
			if (height) *height = h;
			return nf == 3 ? NVMPI_SOF_OK : NVMPI_SOF_COMPONENTS;
		}
		if (m == 0xDA)   /* SOS before any SOF */
			break;
		/* Standalone markers (TEM, RSTn, SOI, EOI) carry no length. */
		if (m != 0x00 && m != 0x01 && !(m >= 0xD0 && m <= 0xD9)) {
			if (pos + 1 >= size) break;
			unsigned len = ((unsigned)d[pos] << 8) | d[pos + 1];
			if (len < 2) break;
			pos += len;
		}
	}
	return NVMPI_SOF_MALFORMED;
}
