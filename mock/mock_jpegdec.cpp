/*
 * mock_jpegdec.cpp - behavioural mock of the NvJPEGDecoder sample class
 * (issue #128). Compiled INTO libnvmpi in NVMPI_MOCK builds in place of the
 * NVIDIA sample NvJpegDecoder.cpp, which needs the Tegra libjpeg and the
 * NVJPG engine; on a mock runner it can create but never decode.
 *
 * It implements exactly the interface libnvmpi calls: createJPEGDecoder,
 * decodeToFd, the destructor, plus decodeToBuffer so the class links
 * (decodeToBuffer fails: libnvmpi never uses it). disableMjpegDecode is
 * deliberately absent: libnvmpi never calls it and the R35 (JP5) header
 * does not declare it, so defining it breaks the JP5 build.
 *
 * decodeToFd models what libnvjpeg does, because libnvmpi's lifetime rules
 * (nvmpi_dec_jpeg_fd.cpp) exist for these properties:
 *   - size and validity come from nvmpi_jpeg_scan_sof(): progressive, gray,
 *     CMYK and malformed input fail; only 3-component baseline decodes;
 *   - the reported size is the picture size padded UP to a multiple of
 *     NVMPI_MOCK_JPEG_PAD (default 16, 0/1 = exact), so the displayed-size
 *     handling (crop by VIC, #4) is exercised;
 *   - ONE block-linear I420 surface per decoder is REUSED by every decode
 *     of the same padded size and destroyed and reallocated when the size
 *     changes. The returned fd belongs to the decoder: a caller that hands
 *     it out sees it overwritten (or closed) by a later decode;
 *   - the surface holds the deterministic mock_jpeg_pattern(), not the
 *     JPEG's pixels (no entropy decoding is modelled).
 * The surface comes from the mock NvBufSurfaceAllocate, so NvBufSurfaceFromFd
 * accepts the fd and NvBufSurfTransform (mock) can read it.
 *
 * Thread-safety: like the real class, one thread per decoder.
 */
#include "NvJpegDecoder.h"
#include "nvbufsurface.h"
#include "mock_jpegdec.h"
#include "mock_env.h"
#include "../src/nvmpi_jpeg_sof.h"

#include <linux/videodev2.h>
#include <cstdio>
#include <cstring>
#include <new>

/* Same bound as the NVJPG engine / mock surfaces (16384). */
#define MOCK_JPEG_MAX_DIM 16384u

/* The sample class has no member for the output surface and its header is
 * not ours to change, so the state hangs off cinfo.client_data (unused: the
 * mock never calls libjpeg). Allocated in the constructor; freed in the
 * destructor, the single deallocation point. */
struct mock_jpeg_state {
	NvBufSurface *surf{nullptr};   /* reused output; destroyed on resize/dtor */
	uint32_t w{0}, h{0};           /* padded size of surf */
};

static mock_jpeg_state *state_of(struct jpeg_decompress_struct &c)
{
	return static_cast<mock_jpeg_state *>(c.client_data);
}

static unsigned pad_of_env()
{
	uint32_t v = 0;
	if (!mock_env_uint(MOCK_JPEG_PAD_ENV, 0, MOCK_JPEG_PAD_MAX, &v))
		return MOCK_JPEG_PAD_DEFAULT;
	return v;
}

/* Fill every sample of the surface (padding included) with the pattern. */
static int fill_pattern(NvBufSurface *s)
{
	NvBufSurfaceParams *p = &s->surfaceList[0];
	for (uint32_t pl = 0; pl < p->planeParams.num_planes; pl++) {
		if (NvBufSurfaceMap(s, 0, (int)pl, NVBUF_MAP_READ_WRITE) != 0)
			return -1;
		unsigned char *base = static_cast<unsigned char *>(p->mappedAddr.addr[pl]);
		for (uint32_t y = 0; y < p->planeParams.height[pl]; y++)
			for (uint32_t x = 0; x < p->planeParams.width[pl]; x++)
				base[(size_t)y * p->planeParams.pitch[pl] + x] =
					mock_jpeg_pattern(pl, x, y);
		NvBufSurfaceUnMap(s, 0, (int)pl);
	}
	return 0;
}

NvJPEGDecoder::NvJPEGDecoder(const char *comp_name)
	: NvElement(comp_name, valid_fields)
{
	memset(&cinfo, 0, sizeof(cinfo));
	memset(&jerr, 0, sizeof(jerr));
	/* Freed in ~NvJPEGDecoder(). nothrow: a NULL state fails decodeToFd. */
	cinfo.client_data = new (std::nothrow) mock_jpeg_state();
}

NvJPEGDecoder::~NvJPEGDecoder()
{
	mock_jpeg_state *st = state_of(cinfo);
	if (st && st->surf)
		NvBufSurfaceDestroy(st->surf);   /* closes the fd handed out */
	delete st;
	cinfo.client_data = nullptr;
}

NvJPEGDecoder *NvJPEGDecoder::createJPEGDecoder(const char *comp_name)
{
	/* Freed by the caller (libnvmpi deletes it in close). */
	NvJPEGDecoder *d = new (std::nothrow) NvJPEGDecoder(comp_name);
	if (d && !d->cinfo.client_data) {
		delete d;
		return nullptr;
	}
	return d;
}

int NvJPEGDecoder::decodeToFd(int &fd, unsigned char *in_buf,
                              unsigned long in_buf_size, uint32_t &pixfmt,
                              uint32_t &width, uint32_t &height)
{
	mock_jpeg_state *st = state_of(cinfo);
	unsigned comps = 0, w = 0, h = 0;

	if (!st || !in_buf)
		return -1;
	/* Untrusted bytes: the scanner never reads outside [0, size). Anything
	 * but 3-component baseline is a decode failure, like the NVJPG engine. */
	if (nvmpi_jpeg_scan_sof(in_buf, in_buf_size, &comps, &w, &h) != NVMPI_SOF_OK ||
	    w == 0 || h == 0 || w > MOCK_JPEG_MAX_DIM || h > MOCK_JPEG_MAX_DIM)
		return -1;

	unsigned pad = pad_of_env();
	uint32_t pw = w, ph = h;
	if (pad > 1) {
		pw = (w + pad - 1) / pad * pad;
		ph = (h + pad - 1) / pad * pad;
	}

	if (!st->surf || pw != st->w || ph != st->h) {
		/* Resize: the previous output (and its fd) dies here, as in
		 * libnvjpeg when the picture size changes. */
		if (st->surf)
			NvBufSurfaceDestroy(st->surf);
		st->surf = nullptr;
		NvBufSurfaceAllocateParams ap;
		memset(&ap, 0, sizeof(ap));
		ap.params.width = pw;
		ap.params.height = ph;
		ap.params.colorFormat = NVBUF_COLOR_FORMAT_YUV420;
		ap.params.layout = NVBUF_LAYOUT_BLOCK_LINEAR;
		ap.params.memType = NVBUF_MEM_SURFACE_ARRAY;
		if (NvBufSurfaceAllocate(&st->surf, 1, &ap) != 0 || !st->surf) {
			st->surf = nullptr;
			return -1;
		}
		st->w = pw;
		st->h = ph;
	}
	if (fill_pattern(st->surf) != 0)
		return -1;

	fd = (int)st->surf->surfaceList[0].bufferDesc;
	pixfmt = V4L2_PIX_FMT_YUV420M;
	width = pw;
	height = ph;
	return 0;
}

/* libnvmpi never decodes to a software buffer; fail like the stub did. */
int NvJPEGDecoder::decodeToBuffer(NvBuffer ** /* buffer */,
                                  unsigned char * /* in_buf */,
                                  unsigned long /* in_buf_size */,
                                  uint32_t * /* pixfmt */,
                                  uint32_t * /* width */,
                                  uint32_t * /* height */)
{
	return -1;
}
