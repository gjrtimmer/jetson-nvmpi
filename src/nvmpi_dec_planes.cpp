#include "nvmpi_dec_internal.h"
#include "nvmpi_alloc.h"

//Map the colorspace/quantization reported by the decoder on its CAPTURE
//plane to the matching NvBuffer NV12 color format variant (BT.601/709/2020,
//standard vs extended luma range). Used when allocating the CAPTURE-plane
//DMA buffers so the subsequent VIC transform interprets colors correctly.
NvBufferColorFormat getNvColorFormatFromV4l2Format(v4l2_format &format, bool want_10bit)
{
#ifdef WITH_NVUTILS
	//10-bit (P010) output: pick the matching NV12_10LE colorimetry variant.
	//These constants exist only in the NvUtils API, which is why P010 is
	//gated on WITH_NVUTILS at the decoder init layer.
	if (want_10bit)
	{
		switch (format.fmt.pix_mp.colorspace)
		{
			case V4L2_COLORSPACE_REC709:
				return (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
					? NvBufferColorFormat_NV12_10LE_709 : NvBufferColorFormat_NV12_10LE_709_ER;
			case V4L2_COLORSPACE_BT2020:
				return NvBufferColorFormat_NV12_10LE_2020;
			case V4L2_COLORSPACE_SMPTE170M:
			default:
				return (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
					? NvBufferColorFormat_NV12_10LE : NvBufferColorFormat_NV12_10LE_ER;
		}
	}
#else
	(void)want_10bit;
#endif
	NvBufferColorFormat ret_cf = NvBufferColorFormat_NV12;
	switch (format.fmt.pix_mp.colorspace)
	{
		case V4L2_COLORSPACE_SMPTE170M:
			if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
			{
				// "Decoder colorspace ITU-R BT.601 with standard range luma (16-235)"
				ret_cf = NvBufferColorFormat_NV12;
			}
			else
			{
				//"Decoder colorspace ITU-R BT.601 with extended range luma (0-255)";
				ret_cf = NvBufferColorFormat_NV12_ER;
			}
			break;
		case V4L2_COLORSPACE_REC709:
			if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
			{
				//"Decoder colorspace ITU-R BT.709 with standard range luma (16-235)";
				ret_cf = NvBufferColorFormat_NV12_709;
			}
			else
			{
				//"Decoder colorspace ITU-R BT.709 with extended range luma (0-255)";
				ret_cf = NvBufferColorFormat_NV12_709_ER;
			}
			break;
		case V4L2_COLORSPACE_BT2020:
			{
				//"Decoder colorspace ITU-R BT.2020";
				ret_cf = NvBufferColorFormat_NV12_2020;
			}
			break;
		default:
			if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
			{
				//"Decoder colorspace ITU-R BT.601 with standard range luma (16-235)";
				ret_cf = NvBufferColorFormat_NV12;
			}
			else
			{
				//"Decoder colorspace ITU-R BT.601 with extended range luma (0-255)";
				ret_cf = NvBufferColorFormat_NV12_ER;
			}
			break;
	}
	return ret_cf;
}


//Colorimetry (BT.601/709/2020) and luma range of an NV12-family colour format
//(8-bit and, on NvUtils, 10-bit variants); false for anything else.
static bool cf_decompose(NvBufferColorFormat cf, int *matrix, bool *er)
{
	*er = false;
	switch (cf)
	{
		case NvBufferColorFormat_NV12:           *matrix = 601;  return true;
		case NvBufferColorFormat_NV12_ER:        *matrix = 601;  *er = true; return true;
		case NvBufferColorFormat_NV12_709:       *matrix = 709;  return true;
		case NvBufferColorFormat_NV12_709_ER:    *matrix = 709;  *er = true; return true;
		case NvBufferColorFormat_NV12_2020:      *matrix = 2020; return true;
#ifdef WITH_NVUTILS
		case NvBufferColorFormat_NV12_10LE:      *matrix = 601;  return true;
		case NvBufferColorFormat_NV12_10LE_ER:   *matrix = 601;  *er = true; return true;
		case NvBufferColorFormat_NV12_10LE_709:  *matrix = 709;  return true;
		case NvBufferColorFormat_NV12_10LE_709_ER: *matrix = 709; *er = true; return true;
		case NvBufferColorFormat_NV12_10LE_2020: *matrix = 2020; return true;
#endif
		default: return false;
	}
}

bool nvmpi_dec_cf_full_range(NvBufferColorFormat cf)
{
	int m;
	bool er;
	return cf_decompose(cf, &m, &er) && er;
}

//Pick the destination format of the VIC copy: layout from the requested
//output (NV12, planar YUV420, P010), colorimetry and range from the CAPTURE
//surface. A destination that differs in either makes NvBufSurfTransform
//run a matrix/range conversion instead of a copy (#163). BT.2020 has no
//extended-range variant. An unknown capture format falls back to BT.601
//limited, the previous behaviour.
NvBufferColorFormat nvmpi_dec_dest_format(NvBufferColorFormat capture_cf, nvPixFormat out)
{
	int m = 601;
	bool er = false;
	if (!cf_decompose(capture_cf, &m, &er))
	{
		m = 601;
		er = false;
	}
#ifdef WITH_NVUTILS
	if (out == NV_PIX_P010)
		return m == 709 ? (er ? NvBufferColorFormat_NV12_10LE_709_ER : NvBufferColorFormat_NV12_10LE_709)
		     : m == 2020 ? NvBufferColorFormat_NV12_10LE_2020
		     : (er ? NvBufferColorFormat_NV12_10LE_ER : NvBufferColorFormat_NV12_10LE);
#endif
	if (out == NV_PIX_NV12)
		return m == 709 ? (er ? NvBufferColorFormat_NV12_709_ER : NvBufferColorFormat_NV12_709)
		     : m == 2020 ? NvBufferColorFormat_NV12_2020
		     : (er ? NvBufferColorFormat_NV12_ER : NvBufferColorFormat_NV12);
	return m == 709 ? (er ? NvBufferColorFormat_YUV420_709_ER : NvBufferColorFormat_YUV420_709)
	     : m == 2020 ? NvBufferColorFormat_YUV420_2020
	     : (er ? NvBufferColorFormat_YUV420_ER : NvBufferColorFormat_YUV420);
}

//(Re)initialize the decoder CAPTURE plane after a resolution-change event:
//set the negotiated plane format, allocate numberCaptureBuffers block-linear
//NV12 DMA buffers sized to the coded resolution, REQBUFS them as
//V4L2_MEMORY_DMABUF, start streaming and enqueue every buffer so the
//decoder can start writing. Called from the capture thread only.
void nvmpi_dec_ctx::initDecoderCapturePlane(v4l2_format &format)
{
	int ret=0;
	int32_t minimumDecoderCaptureBuffers;
	NvBufferCreateParams cParams;
	memset(&cParams, 0, sizeof(cParams));

	ret=dec->setCapturePlaneFormat(format.fmt.pix_mp.pixelformat,format.fmt.pix_mp.width,format.fmt.pix_mp.height);
	TEST_ERROR(ret < 0, "Error in setting decoder capture plane format", ret);

	dec->getMinimumCapturePlaneBuffers(minimumDecoderCaptureBuffers);
	TEST_ERROR(ret < 0, "Error while getting value of minimum capture plane buffers",ret);

	/* Request (min + extra) buffers, export and map buffers. */
	numberCaptureBuffers = minimumDecoderCaptureBuffers + 5;

	//Block-linear layout matches what the hw decoder writes natively; the
	//buffers are converted to pitch-linear later by the VIC transform.
	cParams.colorFormat = getNvColorFormatFromV4l2Format(format, out_pixfmt == NV_PIX_P010);
	//remembered for the destination pool (same colorimetry/range, #163)
	capture_cf = cParams.colorFormat;
	cParams.width = coded_width;
	cParams.height = coded_height;
	cParams.layout = NvBufferLayout_BlockLinear;
#ifdef WITH_NVUTILS
	cParams.memType = NVBUF_MEM_SURFACE_ARRAY;
	cParams.memtag = NvBufSurfaceTag_VIDEO_DEC;

	//NvUtils path: allocate all buffers in one call, then resolve the
	//NvBufSurface view of each fd for use with NvBufSurfTransform.
	ret = nvmpi_alloc_surfaces(&cParams, numberCaptureBuffers, dmaBufferFileDescriptor);
	if (ret < 0) {
		NVMPI_LOG(NVMPI_LOG_ERROR, "Failed to create capture-plane buffers (code=%d)", ret);
		/* nvmpi_alloc_surfaces is all-or-nothing — no partial cleanup needed. */
		numberCaptureBuffers = 0;
		return;
	}
	for (int index = 0; index < numberCaptureBuffers; index++)
	{
		ret = NvBufSurfaceFromFd(dmaBufferFileDescriptor[index], (void**)(&(dmaBufferSurface[index])));
		if (ret < 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR,
				  "Failed to get surface for buffer %d (code=%d); "
				  "cleaning up %d allocated fds", index, ret, numberCaptureBuffers);
			/* Destroy all batch-allocated fds — the batch allocation succeeded so
			 * all numberCaptureBuffers fds are valid. */
			for (int j = 0; j < numberCaptureBuffers; j++) {
				NvBufferDestroy(dmaBufferFileDescriptor[j]);
				dmaBufferFileDescriptor[j] = -1;
			}
			numberCaptureBuffers = 0;
			return;
		}
	}
#else
	//legacy nvbuf_utils path: allocate the buffers one by one
	cParams.payloadType = NvBufferPayload_SurfArray;
	cParams.nvbuf_tag = NvBufferTag_VIDEO_DEC;

	for (int index = 0; index < numberCaptureBuffers; index++)
	{
		ret = NvBufferCreateEx(&dmaBufferFileDescriptor[index], &cParams);
		if (ret < 0) {
			NVMPI_LOG(NVMPI_LOG_ERROR,
				  "Failed to create buffer %d/%d (code=%d); "
				  "cleaning up %d allocated fds", index, numberCaptureBuffers, ret, index);
			/* Destroy fds allocated so far (0..index-1). */
			for (int j = 0; j < index; j++) {
				NvBufferDestroy(dmaBufferFileDescriptor[j]);
				dmaBufferFileDescriptor[j] = -1;
			}
			numberCaptureBuffers = 0;
			return;
		}
	}
#endif

    /* Request buffers on decoder capture plane. Refer ioctl VIDIOC_REQBUFS */
	dec->capture_plane.reqbufs(V4L2_MEMORY_DMABUF, numberCaptureBuffers);
	TEST_ERROR(ret < 0, "Error in decoder capture plane streamon", ret);

    /* Decoder capture plane STREAMON. Refer ioctl VIDIOC_STREAMON */
	dec->capture_plane.setStreamStatus(true);
	TEST_ERROR(ret < 0, "Error in decoder capture plane streamon", ret);

	/* Enqueue all the empty decoder capture plane buffers. */
	requeueDecoderCapturePlane();
}

//Enqueue every CAPTURE-plane buffer to the decoder (all of them are empty:
//a STREAMOFF returns the queued ones to us). Used after the plane setup and
//by flush(), which re-arms the plane without a new resolution event (#162).
//Capture thread or, with the thread joined, the user thread. 0 or -1.
int nvmpi_dec_ctx::requeueDecoderCapturePlane()
{
	int ret = 0;

	for (uint32_t i = 0; i < dec->capture_plane.getNumBuffers(); i++)
	{
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];

		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, sizeof(planes));

		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;
		v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		v4l2_buf.memory = V4L2_MEMORY_DMABUF;
		v4l2_buf.m.planes[0].m.fd = dmaBufferFileDescriptor[i];

		ret = dec->capture_plane.qBuffer(v4l2_buf, NULL);
		if (ret < 0) {
			const int q_errno = errno;
			/* Partial init is worse than no init — remaining buffers
			 * never reach the driver queue, leading to silent capture
			 * starvation.  Return early so the caller can handle the
			 * error (or fail-fast on the next decode call). */
			NVMPI_LOG(NVMPI_LOG_ERROR,
			          "capture plane qBuffer failed at buffer %d/%d (errno=%d)",
			          i, dec->capture_plane.getNumBuffers(), q_errno);
			return -1;
		}
	}

	return 0;
}

//Tear down the CAPTURE plane: STREAMOFF, unmap/release the V4L2 buffers
//(deinitPlane issues REQBUFS with count 0) and destroy our DMA buffers.
//Called on resolution change (before re-init) and from nvmpi_decoder_close.
void nvmpi_dec_ctx::deinitDecoderCapturePlane()
{
	if (numberCaptureBuffers == 0)
		return;

	int ret = 0;
	dec->capture_plane.setStreamStatus(false);
	// bypass deinitPlane() — it calls waitForDQThread() which touches MMAPI
	// DQ thread state the decoder never uses (we use std::thread instead);
	// for DMABUF the only work deinitPlane() does beyond that is reqbufs(0)
	dec->capture_plane.reqbufs(V4L2_MEMORY_DMABUF, 0);
	for (int index = 0; index < numberCaptureBuffers; index++)
	{
		if (dmaBufferFileDescriptor[index] >= 0)
		{
			ret = NvBufferDestroy(dmaBufferFileDescriptor[index]);
			TEST_ERROR(ret < 0, "Failed to Destroy NvBuffer", ret);
			dmaBufferFileDescriptor[index] = -1;
		}
	}
	numberCaptureBuffers = 0;
	return;
}
