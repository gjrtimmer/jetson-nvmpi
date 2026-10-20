/*
 * mock_enc_hevc_slice.c - slice_segment_data() of the decodable HEVC access
 * units of the encoder model (#118), written after the slice header of
 * mock_enc_hevc.c. One slice per picture, CTB 16 (SPS), every CTB split
 * down to 8x8 CUs (MinCbSizeY 8):
 *
 *   I slice   (IDR, CRA) every CU is 2Nx2N intra PCM: part_mode, pcm_flag
 *             (terminate bin), pcm_alignment_zero_bits, 64 Y + 2 x 16
 *             chroma raw samples read from the OUTPUT picture
 *             (mock_enc_pic.c), then the arithmetic engine restarts
 *             (9.3.2.5). A decoder reproduces the input exactly (the SPS
 *             disables the PCM loop filter).
 *   P slice   (TRAIL_R) every CU is cu_skip_flag 1. merge_idx is not coded
 *             because the slice header sets MaxNumMergeCand to 1, and the
 *             only candidate is the zero motion vector on the previous
 *             picture: the decoder copies it, no residual.
 *
 * end_of_slice_segment_flag follows every CTB; the last one flushes the
 * engine and the stop bit comes out of that flush.
 */
#include "mock_enc.h"
#include "mock_enc_cabac.h"

#define CU_Y 64u                       /* 8x8 luma samples of one PCM CU    */
#define CU_C 16u                       /* 4x4 samples per chroma component  */
#define SLICE_QP 26                    /* 26 + init_qp_minus26 + qp_delta 0 */

/* initValue per initType (9.3.2.2 tables; libavcodec hevc/cabac.c
 * init_values[]): I slice = initType 0, P slice (cabac_init_flag
 * 0) = initType 1. */
static const uint8_t init_split[2][3] = { { 139, 141, 157 }, { 107, 139, 126 } };
static const uint8_t init_skip[3] = { 197, 185, 201 };   /* initType 1      */
#define INIT_PART_MODE 184u                              /* initType 0      */

/* The 8x8 CU at (x, y): luma rows, then Cb, then Cr, as pcm_sample() orders
 * them. Positions past the picture (padding) repeat the edge sample. */
static void pcm_cu(struct bw *bw, const struct mock_enc_pic *pic, uint32_t x,
                   uint32_t y)
{
    uint8_t s[CU_Y + 2 * CU_C];
    uint32_t r;

    for (r = 0; r < 8; r++)
        mock_enc_pic_row(pic, 0, x, y + r, 8, s + r * 8);
    for (r = 0; r < 4; r++) {
        mock_enc_pic_row(pic, 1, x / 2, y / 2 + r, 4, s + CU_Y + r * 4);
        mock_enc_pic_row(pic, 2, x / 2, y / 2 + r, 4, s + CU_Y + CU_C + r * 4);
    }
    put_bytes(bw, s, sizeof(s));
}

void mock_enc_hevc_slice_data(struct bw *bw, const struct mock_enc_au *au)
{
    uint32_t pw = (au->width + 7) & ~7u, ph = (au->height + 7) & ~7u;
    uint32_t ctbw = (pw + 15) / 16, ctbh = (ph + 15) / 16;
    uint32_t cx, cy, z;
    bool intra = au->key;              /* I slice: IDR or CRA               */
    struct cabac cb;
    struct cabac_ctx split[3], skip[3], part;
    unsigned k;

    for (k = 0; k < 3; k++) {
        cabac_ctx_init(&split[k], init_split[!intra][k], SLICE_QP);
        cabac_ctx_init(&skip[k], init_skip[k], SLICE_QP);
    }
    cabac_ctx_init(&part, INIT_PART_MODE, SLICE_QP);
    cabac_start(&cb, bw);

    for (cy = 0; cy < ctbh && !bw->over; cy++) {
        for (cx = 0; cx < ctbw; cx++) {
            /* split_cu_flag is coded only when the whole CTB is inside the
             * picture, else inferred 1. ctxInc counts the available
             * neighbours deeper than this level: all CUs are depth 1, and
             * left / above exist exactly when cx / cy > 0 (one slice). */
            if (cx * 16 + 16 <= pw && cy * 16 + 16 <= ph)
                cabac_encode(&cb, &split[(cx > 0) + (cy > 0)], 1);
            for (z = 0; z < 4; z++) {
                uint32_t x = cx * 16 + (z & 1) * 8, y = cy * 16 + (z >> 1) * 8;

                if (x >= pw || y >= ph)    /* CU outside the picture: absent */
                    continue;
                if (intra) {
                    cabac_encode(&cb, &part, 1);   /* part_mode 2Nx2N       */
                    cabac_terminate(&cb, 1);       /* pcm_flag 1: flush     */
                    put_align0(bw);                /* pcm_alignment_zero_bit */
                    pcm_cu(bw, au->pic, x, y);
                    cabac_start(&cb, bw);          /* 9.3.2.5               */
                } else {
                    /* cu_skip_flag ctxInc: left / above available and
                     * skipped (every CU is), in z-scan order they are
                     * already coded when inside the picture. */
                    cabac_encode(&cb, &skip[(x > 0) + (y > 0)], 1);
                }
            }
            /* end_of_slice_segment_flag, 1 after the last CTB. */
            cabac_terminate(&cb, cy == ctbh - 1 && cx == ctbw - 1);
        }
    }
    put_align0(bw);                    /* zero bits after the stop bit      */
}
