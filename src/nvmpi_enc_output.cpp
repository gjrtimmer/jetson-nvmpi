#include "nvmpi_enc_internal.h"
#include "nvmpi_alloc.h"

//Callback run on NvVideoEncoder's capture-plane DQ thread for every
//dequeued (encoded) buffer. Contract: return true to keep the thread
//running, false to stop it (EOS or fatal error).
//Steps: detect the zero-byte EOS buffer; take an "empty" nvPacket from the
//pool (marked HELD), fetch encode metadata (keyframe flag) and memcpy the
//bitstream into the packet, then publish it "filled" for
//nvmpi_encoder_get_packet(); release the hold; finally re-queue the V4L2
//buffer so the encoder can reuse it. If the pool is empty the encoded data
//is DROPPED (error printed, ctx->aus_dropped counted). Non-blocking mode
//cannot reach that: put_frame admits a frame only while the pool holds a
//packet for every AU still owed (aus_pending, issue #125). Blocking mode
//relies on the caller's pool size (FFmpeg packet_pool_size) as before.
//Every non-EOS buffer consumes exactly one aus_pending count, on every
//path below, after releaseHold() (see enc_can_admit for the ordering).
//Held window (issue #121): between the take and the publish the packet is
//in neither pool queue. The hold lets nvmpi_encoder_dqEmptyPacket() (the
//teardown drain) wait for it instead of closing while it is in flight.
bool encoder_capture_plane_dq_callback(struct v4l2_buffer *v4l2_buf, NvBuffer * buffer, NvBuffer * shared_buffer __attribute__((unused)), void *arg)
{
	nvmpi_enc_ctx *ctx = (nvmpi_enc_ctx*)arg;

	if (v4l2_buf == NULL)
	{
		NVMPI_LOG(NVMPI_LOG_ERROR, "error while dequeing buffer from output plane");
		return false;
	}

	if (buffer->planes[0].bytesused == 0)
	{
		ctx->capPlaneGotEOS.store(true, std::memory_order_release);
		NVMPI_LOG(NVMPI_LOG_DEBUG, "got 0-size buffer in capture (EOS signal)");
		return false;
	}

	//nvPacket.payload --> AVPacket->data
	//nvPacket.privData --> AVPacket
	//Allocated by the caller (FFmpeg wrapper), never by libnvmpi. Held until
	//releaseHold() below; m_emptyBuf guards the held count. The metadata
	//ioctl runs after the take so the whole in-flight window is covered.
	nvPacket* pkt = ctx->pktPool->dqEmptyBufHold();

	v4l2_ctrl_videoenc_outputbuf_metadata enc_metadata;
	ctx->enc->getMetadata(v4l2_buf->index, enc_metadata);

	if(!pkt)
	{
		//Last resort, unreachable with a well-behaved non-blocking caller
		//(admission in put_frame). Reached in blocking mode with a pool
		//smaller than the frames in flight, or if the caller removed packets
		//(dqEmptyPacket) outside teardown. The capture buffer is re-queued
		//below like every other one; nothing is leaked.
		ctx->aus_dropped.fetch_add(1, std::memory_order_relaxed);
		NVMPI_LOG(NVMPI_LOG_ERROR, "encoder packet pool empty; encoded frame dropped (%u so far); caller must return packets with qEmptyPacket after get_packet; there will be artifacts in output video",
			ctx->aus_dropped.load(std::memory_order_relaxed));
		ctx->aus_pending.fetch_sub(1, std::memory_order_seq_cst);
	}
	else if (buffer->planes[0].bytesused > NVMPI_ENC_CHUNK_SIZE)
	{
		//pool packet buffers are NVMPI_ENC_CHUNK_SIZE bytes; copying a
		//larger encoded frame would overflow them
		NVMPI_LOG(NVMPI_LOG_ERROR, "encoded frame (%u bytes) exceeds packet buffer (%u); frame dropped; there will be artifacts in output video",
			buffer->planes[0].bytesused, (unsigned)(NVMPI_ENC_CHUNK_SIZE));
		ctx->pktPool->qEmptyBuf(pkt);
		//publish-then-release: the packet is back in the empty queue first
		ctx->pktPool->releaseHold();
		ctx->aus_pending.fetch_sub(1, std::memory_order_seq_cst);
	}
	else
	{
		pkt->pts = (v4l2_buf->timestamp.tv_usec % 1000000) + (v4l2_buf->timestamp.tv_sec * 1000000UL);
		//AV_PKT_FLAG_KEY 0x0001. if current packet is keyframe then enc_metadata.KeyFrame should be 0x1, so it should be OK to just assign value
		pkt->flags = enc_metadata.KeyFrame;
		pkt->payload_size = buffer->planes[0].bytesused;
		memcpy(pkt->payload, buffer->planes[0].data, pkt->payload_size);

		//publish first, release after: a settled drain that sees held == 0
		//must find the packet in the filled queue
		ctx->pktPool->qFilledBuf(pkt);
		ctx->pktPool->releaseHold();
		//after the release: the freed hold and the lower count must never
		//be observed the other way round (over-admission)
		ctx->aus_pending.fetch_sub(1, std::memory_order_seq_cst);
	}

	if (ctx->enc->capture_plane.qBuffer(*v4l2_buf, NULL) < 0)
	{
		//TODO error handling
		ERROR_MSG("Error while Qing buffer at capture plane");
		return false;
	}

	return true;
}

//Alternate OUTPUT-plane setup used only when OUTPLANE_MEMTYPE_DMA is
//selected: REQBUFS as V4L2_MEMORY_DMABUF and self-allocate one pitch-linear
//YUV420 NvBufSurface per buffer, storing the fds in ctx->output_plane_fd
//(freed in nvmpi_encoder_close). NvUtils-only code path.
#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_DMA)
int setup_output_dmabuf(nvmpi_enc_ctx *ctx, uint32_t num_buffers )
{
    int ret=0;
    NvBufSurf::NvCommonAllocateParams cParams;
    int fd;
    ret = ctx->enc->output_plane.reqbufs(V4L2_MEMORY_DMABUF,num_buffers);
    if(ret)
    {
        NVMPI_LOG(NVMPI_LOG_ERROR, "reqbufs failed for output plane V4L2_MEMORY_DMABUF");
        return ret;
    }
    for (uint32_t i = 0; i < ctx->enc->output_plane.getNumBuffers(); i++)
    {
        cParams.width = ctx->width;
        cParams.height = ctx->height;
        cParams.layout = NVBUF_LAYOUT_PITCH;

        /*
        switch (ctx->cs)
        {
            case V4L2_COLORSPACE_REC709:
                cParams.colorFormat = ctx->enable_extended_colorformat ?
                    NVBUF_COLOR_FORMAT_YUV420_709_ER : NVBUF_COLOR_FORMAT_YUV420_709;
                break;
            case V4L2_COLORSPACE_SMPTE170M:
            default:
                cParams.colorFormat = ctx->enable_extended_colorformat ?
                    NVBUF_COLOR_FORMAT_YUV420_ER : NVBUF_COLOR_FORMAT_YUV420;
        }
        if (ctx->is_semiplanar)
        {
            cParams.colorFormat = NVBUF_COLOR_FORMAT_NV12;
        }
        if (ctx->encoder_pixfmt == V4L2_PIX_FMT_H264)
        {
            if (ctx->enableLossless)
            {
                if (ctx->is_semiplanar)
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_NV24;
                else
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_YUV444;
            }
        }


        else if (ctx->encoder_pixfmt == V4L2_PIX_FMT_H265)
        {
            if (ctx->chroma_format_idc == 3)
            {
                if (ctx->is_semiplanar)
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_NV24;
                else
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_YUV444;

                if (ctx->bit_depth == 10)
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_NV24_10LE;
            }
            if (ctx->profile == V4L2_MPEG_VIDEO_H265_PROFILE_MAIN10 && (ctx->bit_depth == 10))
            {
                cParams.colorFormat = NVBUF_COLOR_FORMAT_NV12_10LE;
            }
        }
        */

        cParams.colorFormat = NVBUF_COLOR_FORMAT_YUV420;
        cParams.memtag = NvBufSurfaceTag_VIDEO_ENC;
        cParams.memType = NVBUF_MEM_SURFACE_ARRAY;
        // Create output plane fd for DMABUF io-mode
        /* Checked allocation (NvAllocate dereferences NULL on failure). */
        ret = nvmpi_alloc_surfaces(&cParams, 1, &fd);
        if(ret < 0)
        {
            NVMPI_LOG(NVMPI_LOG_ERROR,
                  "failed to create NvBuffer at index %u/%u; "
                  "rolling back %u already-allocated fds",
                  i, ctx->enc->output_plane.getNumBuffers(), i);
            /* Rollback: destroy fds allocated in previous iterations
             * to prevent DMA-BUF / CMA leak on partial init failure. */
            for (uint32_t j = 0; j < i; j++) {
                if (ctx->output_plane_fd[j] >= 0) {
                    NvBufferDestroy(ctx->output_plane_fd[j]);
                    ctx->output_plane_fd[j] = -1;
                }
            }
            return ret;
        }
        ctx->output_plane_fd[i]=fd;
    }
    return ret;
}
#endif
