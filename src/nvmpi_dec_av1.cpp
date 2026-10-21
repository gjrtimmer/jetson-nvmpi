/*
 * nvmpi_dec_av1.cpp — AV1 input gate of the decoder (libnvmpi, #147).
 *
 * NVDEC on the Orin (R36.4) holds the OUTPUT buffer of AV1 data that comes
 * before the first sequence header and never returns it. After the OUTPUT
 * buffers are used up put_packet blocks forever in dqBuffer(-1), and the
 * session stays dead even when a header follows. Streams can legitimately
 * start that way (header kept out of band in av1C and stripped from the
 * packets, or decoding that starts mid-stream), so the library drops the
 * data itself until a sequence header has been seen.
 */
#include "nvmpi_dec_internal.h"
#include "nvmpi_av1_obu.h"

//Called by put_packet for every non-empty packet of an AV1 session, user
//thread only. Returns true when the packet must be dropped (consumed, not
//queued to the driver). The packet that carries the first sequence header is
//queued normally and opens the gate until flush (nvmpi_dec_close.cpp) closes
//it again. The payload is untrusted: nvmpi_av1_scan bounds-checks every
//length, and a malformed packet simply has no sequence header (dropped).
bool nvmpi_dec_av1_drop(nvmpi_dec_ctx* ctx, const nvPacket* packet)
{
	int has_seq, has_pic;

	if (!ctx->is_av1 || ctx->av1_seq_seen)
		return false;

	nvmpi_av1_scan(packet->payload, packet->payload_size, &has_seq, &has_pic);
	if (has_seq)
	{
		ctx->av1_seq_seen = true;
		return false;
	}

	ctx->av1_dropped++;
	if (!ctx->av1_drop_logged)
	{
		//once per stream start: a per-packet line would flood the log
		ctx->av1_drop_logged = true;
		NVMPI_LOG(NVMPI_LOG_INFO, "AV1: dropping input until the first sequence header (callers with out-of-band headers must send the config OBUs first)");
	}
	return true;
}
