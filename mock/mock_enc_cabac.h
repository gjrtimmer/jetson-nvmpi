/*
 * mock_enc_cabac.h - minimal CABAC encoder for the HEVC slice data of the
 * encoder model (#118). Only what the mock's syntax needs: context coded
 * bins, the terminate bin (end_of_slice_segment_flag, pcm_flag) with its
 * flush, and re-initialisation after PCM samples. Output goes through the
 * bounds-checked bit writer of mock_enc_bits.h: an overflow is flagged in
 * the writer (bw_bytes() == 0), the engine itself never fails.
 */
#ifndef NVMPI_MOCK_ENC_CABAC_H
#define NVMPI_MOCK_ENC_CABAC_H

#include "mock_enc_bits.h"

/* One adaptive context: probability state index and most probable symbol. */
struct cabac_ctx { uint8_t state, mps; };

struct cabac {
    struct bw *w;                  /* sink; owned by the caller            */
    uint32_t low, range;           /* ivlLow (10 bits + carry), ivlCurrRange */
    unsigned outstanding;          /* bitsOutstanding (carry propagation)  */
    bool     first;                /* firstBitFlag: first PutBit dropped   */
};

/* 9.3.2.2: initialise a context from its initValue at SliceQpY. */
void cabac_ctx_init(struct cabac_ctx *c, unsigned init_value, int slice_qp);
/* (Re)start the arithmetic engine on w: slice data start, and again after
 * the PCM samples (9.3.2.5). Contexts keep their state. */
void cabac_start(struct cabac *c, struct bw *w);
/* Encode one context coded bin. */
void cabac_encode(struct cabac *c, struct cabac_ctx *x, unsigned bin);
/* Encode a terminate bin. bin 1 flushes the engine (the last bit written is
 * the stop bit '1': rbsp_stop_one_bit for end_of_slice_segment_flag, the
 * codeword end before pcm_alignment_zero_bits for pcm_flag). The caller
 * then aligns the writer with zero bits. */
void cabac_terminate(struct cabac *c, unsigned bin);

#endif /* NVMPI_MOCK_ENC_CABAC_H */
