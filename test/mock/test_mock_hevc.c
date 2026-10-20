/*
 * test_mock_hevc.c - the HEVC parameter sets of the mock encoder (#108,
 * mock/mock_enc_hevc.c). The first access unit of an HEVC stream is read
 * back through the driver interface and VPS/SPS/PPS are parsed with a
 * minimal reader: profile, level, coded size, conformance window, PCM
 * fields, plus the first bytes of the slice (the slice data is checked in
 * test_mock_hevc_slice.c). Part of the test_mock executable.
 */
#include "bit_read.h"
#include "mock_enc_harness.h"

static void check_ptl(struct br *r, unsigned level_idc)
{
    CHECK(get_bits(r, 2) == 0);               /* profile_space              */
    CHECK(get_bits(r, 1) == 0);               /* tier                       */
    CHECK(get_bits(r, 5) == 1);               /* profile_idc: Main          */
    CHECK(get_bits(r, 32) == 0x60000000u);    /* compat flags 1 and 2       */
    CHECK(get_bits(r, 4) == 9);               /* progressive, frame only    */
    (void)get_bits(r, 32);
    (void)get_bits(r, 12);                    /* 44 reserved bits           */
    CHECK(get_bits(r, 8) == level_idc);
}

static void check_size(uint32_t w, uint32_t h, unsigned level_idc)
{
    struct eh e;
    unsigned ty[8], cnt;
    uint32_t bytes = 0, idx = 0, pw = (w + 7) & ~7u, ph = (h + 7) & ~7u;
    uint8_t v[64], s[64], p[64];
    const uint8_t *au;
    struct br r;
    size_t vn, sn, pn, i;

    CHECK(eh_open(&e, H265, w, h, NULL) == 0);
    CHECK(eh_frame(&e, 1) == 0);
    CHECK(eh_get(&e, ty, 8, &cnt, &bytes, NULL, &idx) >= 0 && bytes > 0);
    au = e.cap[idx];
    vn = nal_rbsp(au, bytes, 1, 32, v, sizeof(v));
    sn = nal_rbsp(au, bytes, 1, 33, s, sizeof(s));
    pn = nal_rbsp(au, bytes, 1, 34, p, sizeof(p));
    CHECK(vn && sn && pn);

    r = (struct br){ v, vn, 0 };              /* VPS                        */
    CHECK(get_bits(&r, 4) == 0);
    CHECK(get_bits(&r, 2) == 3);              /* base layer internal/avail  */
    CHECK(get_bits(&r, 6) == 0 && get_bits(&r, 3) == 0);
    CHECK(get_bits(&r, 1) == 1 && get_bits(&r, 16) == 0xffff);
    check_ptl(&r, level_idc);

    r = (struct br){ s, sn, 0 };              /* SPS                        */
    CHECK(get_bits(&r, 4) == 0 && get_bits(&r, 3) == 0 && get_bits(&r, 1) == 1);
    check_ptl(&r, level_idc);
    CHECK(get_ue(&r) == 0);                   /* sps id                     */
    CHECK(get_ue(&r) == 1);                   /* 4:2:0                      */
    CHECK(get_ue(&r) == pw);
    CHECK(get_ue(&r) == ph);
    if (pw != w || ph != h) {
        CHECK(get_bits(&r, 1) == 1);          /* conformance window         */
        CHECK(get_ue(&r) == 0);
        CHECK(get_ue(&r) == (pw - w) / 2);    /* right, chroma units        */
        CHECK(get_ue(&r) == 0);
        CHECK(get_ue(&r) == (ph - h) / 2);    /* bottom                     */
    } else {
        CHECK(get_bits(&r, 1) == 0);
    }
    CHECK(get_ue(&r) == 0 && get_ue(&r) == 0);   /* 8-bit luma and chroma   */
    CHECK(get_ue(&r) == 4);                   /* log2_max_poc_lsb_minus4    */
    CHECK(get_bits(&r, 1) == 1 && get_ue(&r) == 1 && get_ue(&r) == 0 &&
          get_ue(&r) == 0);                   /* sub-layer ordering         */
    CHECK(get_ue(&r) == 0 && get_ue(&r) == 1);   /* CB 8..16 (CTB 16)       */
    CHECK(get_ue(&r) == 0 && get_ue(&r) == 2);   /* TB 4..16                */
    CHECK(get_ue(&r) == 0 && get_ue(&r) == 0);   /* hierarchy depths        */
    CHECK(get_bits(&r, 3) == 0);              /* no scaling list, AMP, SAO  */
    CHECK(get_bits(&r, 1) == 1);              /* pcm_enabled_flag           */
    CHECK(get_bits(&r, 4) == 7 && get_bits(&r, 4) == 7);   /* 8-bit PCM     */
    CHECK(get_ue(&r) == 0 && get_ue(&r) == 0);   /* PCM CB size 8 only      */
    CHECK(get_bits(&r, 1) == 1);              /* pcm_loop_filter_disabled   */
    CHECK(get_ue(&r) == 0);                   /* num_short_term_ref_pic_sets */
    CHECK(r.pos < sn * 8);                    /* no read past the payload   */

    r = (struct br){ p, pn, 0 };              /* PPS                        */
    CHECK(get_ue(&r) == 0 && get_ue(&r) == 0);   /* pps id, sps id          */

    /* IDR_W_RADL slice: first_slice_segment_in_pic_flag 1,
     * no_output_of_prior_pics_flag 0, slice_pic_parameter_set_id ue(0). */
    for (i = 0; i + 4 < bytes; i++)
        if (!au[i] && !au[i + 1] && au[i + 2] == 1 && (au[i + 3] >> 1) == 19)
            break;
    CHECK(i + 6 < bytes && (au[i + 5] & 0xe0) == 0xa0);
    CHECK(qcap(&e, idx) == 0);
    eh_close(&e);
}

void test_encoder_hevc_params(void)
{
    check_size(320, 240, 60);                 /* level 2, no window         */
    check_size(642, 362, 63);                 /* window 3 / 3, level 2.1    */
    check_size(1920, 1080, 120);              /* level 4                    */
    /* Level 5.1; 4K (8.3 MP) is above MOCK_ENC_MAX_PIXELS and errors (#116). */
    check_size(2560, 1440, 153);
}
