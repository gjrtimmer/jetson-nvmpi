/*
 * nvmpi_dec_codec.cpp — decoder codingType to V4L2 fourcc mapping (libnvmpi).
 *
 * Kept apart from nvmpi_dec_init.cpp so that file stays under the size limit.
 * The mapping is the only per-codec decision in decoder creation: every codec
 * shares the same OUTPUT-plane setup (setFrameInputMode(0), USERPTR buffers,
 * optional disableDPB/max-perf). VP8, VP9 and AV1 carry one frame / temporal
 * unit per buffer; the driver needs no extra per-codec control for them.
 */
#include "nvmpi_dec_internal.h"

unsigned int nvmpi_dec_codec_pixfmt(nvCodingType codec)
{
	switch(codec)
	{
		case NV_VIDEO_CodingH264:
			return V4L2_PIX_FMT_H264;
		case NV_VIDEO_CodingHEVC:
			return V4L2_PIX_FMT_H265;
		case NV_VIDEO_CodingMPEG4:
			return V4L2_PIX_FMT_MPEG4;
		case NV_VIDEO_CodingMPEG2:
			return V4L2_PIX_FMT_MPEG2;
		case NV_VIDEO_CodingVP8:
			return V4L2_PIX_FMT_VP8;
		case NV_VIDEO_CodingVP9:
			return V4L2_PIX_FMT_VP9;
		case NV_VIDEO_CodingAV1:
#if defined(V4L2_PIX_FMT_AV1) && !defined(NVMPI_NO_AV1)
			/* V4L2_PIX_FMT_AV1 ('AV10') is defined by the R36 (JetPack 6)
			 * v4l2_nv_extensions.h; older L4T headers lack it, so JetPack 5
			 * builds take the #else path and creation fails cleanly.
			 * NVMPI_NO_AV1 (-DNVMPI_FORCE_NO_AV1=ON) forces that path for tests. */
			return V4L2_PIX_FMT_AV1;
#else
			NVMPI_LOG(NVMPI_LOG_ERROR, "AV1 decode not supported by this L4T build");
			return 0;
#endif
		default:
			/* MJPEG (separate JPEG API), Unused or garbage: the old fallback
			 * decoded these as H.264 and produced corrupt or empty output. */
			NVMPI_LOG(NVMPI_LOG_ERROR, "Unsupported decoder codingType %d", (int)codec);
			return 0;
	}
}
