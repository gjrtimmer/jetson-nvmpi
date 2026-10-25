/*
 * nvmpi_query.cpp — nvmpi_query_codec(): can this process use a codec? (#159)
 *
 * Answers without creating a session, in two steps:
 *   1. build table  (no device access): what the create calls refuse on
 *      purpose in this build, on any module -> NVMPI_CODEC_UNSUPPORTED_BUILD.
 *   2. hardware probe, cached per direction for the process: open the device
 *      node the create call uses and list its coded formats with
 *      VIDIOC_ENUM_FMT (video), or create and delete one NvJPEG codec (JPEG)
 *      -> NVMPI_CODEC_SUPPORTED or NVMPI_CODEC_UNSUPPORTED_HW.
 *
 * The table mirrors nvmpi_create_decoder / nvmpi_create_encoder / the JPEG
 * create calls (nvmpi_dec_codec.cpp, nvmpi_enc_init.cpp, nvmpi_enc_av1.cpp,
 * nvmpi_enc_params.cpp). A change there needs the same change here; the mock
 * test checks both directions (every SUPPORTED creates, every BUILD is
 * refused by create).
 *
 * This file deliberately includes neither nvmpi_dec_internal.h nor
 * nvmpi_enc_internal.h: both define their own struct nvmpictx.
 */
#include "nvmpi.h"
#include "nvmpi_log.h"
#include "NvJpegDecoder.h"
#include "NvJpegEncoder.h"

#include <linux/videodev2.h>
#include "v4l2_nv_extensions.h"
#include <libv4l2.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <mutex>

/* Upper bound on the formats read from one device: a misbehaving driver that
 * never answers EINVAL must not loop forever or overrun the cache. */
#define QUERY_MAX_FMTS 64

/* Result of one cached video probe. */
struct vid_probe {
	bool done;                         /* false until a conclusive probe */
	unsigned int n;                    /* valid entries in fourcc[]      */
	uint32_t fourcc[QUERY_MAX_FMTS];   /* coded formats the node offers  */
};

/* g_mu guards every cache below. Held for the whole probe so a device is
 * opened at most once per process even with concurrent first calls; nothing
 * in the probe calls back into libnvmpi, so there is no lock-order issue. */
static std::mutex g_mu;
static vid_probe g_vid[2];             /* [0] decode, [1] encode          */
static int g_jpeg[2] = { -1, -1 };     /* [dir]: -1 unknown, else the result */

/* Build table: NVMPI_CODEC_SUPPORTED when the build can, else
 * NVMPI_CODEC_UNSUPPORTED_BUILD. Arguments are already range-checked. */
static int build_support(nvCodingType type, int is_encoder, nvPixFormat pix)
{
	if (type == NV_VIDEO_CodingMJPEG) {
		/* JPEG codes 8-bit 4:2:0 only (NV12 / YUV420 rasters). */
		return pix == NV_PIX_P010 ? NVMPI_CODEC_UNSUPPORTED_BUILD
		                          : NVMPI_CODEC_SUPPORTED;
	}
	if (is_encoder) {
		switch (type) {
		case NV_VIDEO_CodingH264:
		case NV_VIDEO_CodingHEVC:
			break;
		case NV_VIDEO_CodingAV1:
			/* AV1 encode needs the R36 encode controls (JetPack 6 headers). */
#if !(defined(NVMPI_HAVE_AV1_ENC) && NVMPI_HAVE_AV1_ENC)
			return NVMPI_CODEC_UNSUPPORTED_BUILD;
#endif
			break;
		default:
			/* MPEG-2/4, VP8/9: decode only. */
			return NVMPI_CODEC_UNSUPPORTED_BUILD;
		}
		/* P010 (Main10) encode is HEVC only; create refuses H.264 and AV1. */
		if (pix == NV_PIX_P010 && type != NV_VIDEO_CodingHEVC)
			return NVMPI_CODEC_UNSUPPORTED_BUILD;
		return NVMPI_CODEC_SUPPORTED;
	}
	switch (type) {
	case NV_VIDEO_CodingH264:
	case NV_VIDEO_CodingHEVC:
	case NV_VIDEO_CodingMPEG4:
	case NV_VIDEO_CodingMPEG2:
	case NV_VIDEO_CodingVP8:
	case NV_VIDEO_CodingVP9:
		break;
	case NV_VIDEO_CodingAV1:
		/* Same gate as nvmpi_dec_codec_pixfmt(): R36 headers define the
		 * fourcc, NVMPI_NO_AV1 is the test-only override. */
#if !(defined(V4L2_PIX_FMT_AV1) && !defined(NVMPI_NO_AV1))
		return NVMPI_CODEC_UNSUPPORTED_BUILD;
#endif
		break;
	default:
		return NVMPI_CODEC_UNSUPPORTED_BUILD;
	}
	if (pix == NV_PIX_P010) {
		/* 10-bit output is HEVC Main10 and needs the NvBufSurface 10-bit
		 * formats, which only the NvUtils API has. */
#ifndef WITH_NVUTILS
		return NVMPI_CODEC_UNSUPPORTED_BUILD;
#endif
		if (type != NV_VIDEO_CodingHEVC)
			return NVMPI_CODEC_UNSUPPORTED_BUILD;
	}
	return NVMPI_CODEC_SUPPORTED;
}

/* Coded-plane fourcc of a codec the build table accepted (0: none). */
static uint32_t coded_fourcc(nvCodingType type)
{
	switch (type) {
	case NV_VIDEO_CodingH264:  return V4L2_PIX_FMT_H264;
	case NV_VIDEO_CodingHEVC:  return V4L2_PIX_FMT_H265;
	case NV_VIDEO_CodingMPEG4: return V4L2_PIX_FMT_MPEG4;
	case NV_VIDEO_CodingMPEG2: return V4L2_PIX_FMT_MPEG2;
	case NV_VIDEO_CodingVP8:   return V4L2_PIX_FMT_VP8;
	case NV_VIDEO_CodingVP9:   return V4L2_PIX_FMT_VP9;
#ifdef V4L2_PIX_FMT_AV1
	case NV_VIDEO_CodingAV1:   return V4L2_PIX_FMT_AV1;
#endif
	default:                   return 0;
	}
}

/* The node create would open: same candidates, same order (see
 * nvmpi_dec_init.cpp / nvmpi_enc_init.cpp). NULL when neither exists. */
static const char *pick_node(int is_encoder)
{
	if (is_encoder) {
		if (access("/dev/nvhost-msenc", F_OK) == 0) return "/dev/nvhost-msenc";
		if (access("/dev/v4l2-nvenc", F_OK) == 0)   return "/dev/v4l2-nvenc";
	} else {
		if (access("/dev/nvhost-nvdec", F_OK) == 0) return "/dev/nvhost-nvdec";
		if (access("/dev/v4l2-nvdec", F_OK) == 0)   return "/dev/v4l2-nvdec";
	}
	return NULL;
}

/* Probe one video direction into *p. Returns 0 when the answer is final (p
 * filled, p->done set) or -1 when inconclusive (nothing cached). g_mu held. */
static int probe_video(int is_encoder, vid_probe *p)
{
	const char *node = pick_node(is_encoder);
	int fd, rc = 0;
	/* Decode lists its codecs on the compressed OUTPUT plane, encode on the
	 * compressed CAPTURE plane. */
	uint32_t buf_type = is_encoder ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
	                               : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	p->n = 0;
	if (!node) {
		/* No node at all: this module has no such engine. */
		p->done = true;
		return 0;
	}
	/* Opened here; closed below on every path (single close point). */
	fd = v4l2_open(node, O_RDWR);
	if (fd < 0) {
		int e = errno;

		if (e == ENOENT || e == ENODEV || e == ENXIO) {
			/* Node present but driver not loaded or hardware absent: the
			 * same errnos create treats as permanent. */
			p->done = true;
			return 0;
		}
		/* EACCES, EBUSY, ...: the module may well have the codec, we just
		 * cannot see it now. Do not cache. */
		NVMPI_LOG(NVMPI_LOG_WARN, "query: cannot open %s (errno=%d: %s)", node, e, strerror(e));
		return -1;
	}
	for (unsigned int idx = 0; ; idx++) {
		struct v4l2_fmtdesc d;

		if (idx >= QUERY_MAX_FMTS) {
			/* Bound reached without EINVAL: treat the list as complete
			 * rather than loop on a broken driver. */
			break;
		}
		memset(&d, 0, sizeof(d));
		d.index = idx;
		d.type = buf_type;
		if (v4l2_ioctl(fd, VIDIOC_ENUM_FMT, &d) < 0) {
			/* EINVAL past index 0 is the normal end of the list. At index 0
			 * (or any other error) we learned nothing: inconclusive. */
			if (!(errno == EINVAL && idx > 0)) {
				NVMPI_LOG(NVMPI_LOG_WARN, "query: ENUM_FMT on %s failed at index %u (errno=%d: %s)", node, idx, errno, strerror(errno));
				rc = -1;
			}
			break;
		}
		p->fourcc[p->n++] = d.pixelformat;
	}
	v4l2_close(fd);                 /* the only close of fd */
	if (rc == 0)
		p->done = true;
	else
		p->n = 0;
	return rc;
}

/* Probe the NvJPEG engine of one direction: one create + delete. Result is
 * NVMPI_CODEC_SUPPORTED or NVMPI_CODEC_UNSUPPORTED_HW (NULL create: Orin Nano
 * has no NVJPG encoder). Always final. g_mu held. */
static int probe_jpeg(int is_encoder)
{
	if (is_encoder) {
		/* Created and deleted here, never handed out. */
		NvJPEGEncoder *e = NvJPEGEncoder::createJPEGEncoder("jpegenc_q");
		if (!e)
			return NVMPI_CODEC_UNSUPPORTED_HW;
		delete e;
	} else {
		NvJPEGDecoder *d = NvJPEGDecoder::createJPEGDecoder("jpegdec_q");
		if (!d)
			return NVMPI_CODEC_UNSUPPORTED_HW;
		delete d;
	}
	return NVMPI_CODEC_SUPPORTED;
}

extern "C" int nvmpi_query_codec(nvCodingType type, int is_encoder, nvPixFormat pix_fmt)
{
	int b;

	/* Arguments come from a dlopen caller: range-check as ints before the
	 * enum values are used for anything. */
	if ((int)type < (int)NV_VIDEO_CodingH264 || (int)type > (int)NV_VIDEO_CodingAV1)
		return NVMPI_ERR_INVAL;
	if (is_encoder != 0 && is_encoder != 1)
		return NVMPI_ERR_INVAL;
	if ((int)pix_fmt < (int)NV_PIX_NV12 || (int)pix_fmt > (int)NV_PIX_P010)
		return NVMPI_ERR_INVAL;

	b = build_support(type, is_encoder, pix_fmt);
	if (b != NVMPI_CODEC_SUPPORTED)
		return b;                   /* no device access for BUILD answers */

	std::lock_guard<std::mutex> lk(g_mu);
	if (type == NV_VIDEO_CodingMJPEG) {
		if (g_jpeg[is_encoder] < 0)
			g_jpeg[is_encoder] = probe_jpeg(is_encoder);
		return g_jpeg[is_encoder];
	}
	vid_probe *p = &g_vid[is_encoder];
	if (!p->done && probe_video(is_encoder, p) < 0)
		return -1;
	uint32_t want = coded_fourcc(type);
	for (unsigned int i = 0; want && i < p->n; i++)
		if (p->fourcc[i] == want)
			return NVMPI_CODEC_SUPPORTED;
	return NVMPI_CODEC_UNSUPPORTED_HW;
}
