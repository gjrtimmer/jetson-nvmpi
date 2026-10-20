/*
 * jpeg_mock_misc.h - the rejected-input, fault and legacy cases of
 * test_api_jpeg_mock.c (split out to keep each file under 400 lines).
 * Needs jpeg_mock_util.h first.
 */
#ifndef NVMPI_TEST_JPEG_MOCK_MISC_H
#define NVMPI_TEST_JPEG_MOCK_MISC_H

#include "../unit/jpeg_sof_fixtures.h"

/* Progressive, gray, truncated and garbage input fail without producing a
 * frame, and the same decoder then decodes a good image. */
static int reject_inputs(int fmt, int b_, int c_)
{
    struct img a = img_a();
    struct img bad[4] = {
        { sof_prog_jpg, sof_prog_jpg_len, 33, 17 },
        { sof_gray_jpg, sof_gray_jpg_len, 33, 17 },
        { jpeg_fd_test_jpg, 40, 33, 17 },            /* cut before the SOF */
        { (const unsigned char *)"not a jpeg at all", 17, 0, 0 },
    };
    struct borrow b;
    nvmpictx *c;
    int i;

    (void)b_; (void)c_;
    c = nvmpi_create_jpeg_decoder_fmt((nvPixFormat)fmt, 2);
    REQUIRE(c);
    for (i = 0; i < 4; i++) {
        REQUIRE(put(c, &bad[i], 1) == -1);
        REQUIRE(take(c, &b, false) == -1);
    }
    REQUIRE(put(c, &a, 5) == 0 && take(c, &b, true) == 0);
    REQUIRE(check_frame(&b.f, (nvPixFormat)fmt, a.w, a.h, 0) == 0);
    give(&b);
    return nvmpi_jpeg_decoder_close(c) == 0 ? 0 : 1;
}

/* A failed allocation (call 1 = decoder surface, 2.. = pool buffers, chosen
 * by the case's NVMPI_MOCK_FAULT) fails the put, leaks nothing, and the
 * next put recovers. */
static int fault_alloc(int fmt, int which, int c_)
{
    struct img a = img_a();
    struct borrow b;
    int base = nfds();
    nvmpictx *c;

    (void)c_;
    (void)which;                         /* the case's fault string selects it */
    set_pad(16);
    c = nvmpi_create_jpeg_decoder_fmt((nvPixFormat)fmt, 3);
    REQUIRE(c);
    REQUIRE(put(c, &a, 1) == -1);                    /* fault fires here  */
    REQUIRE(take(c, &b, false) == -1);
    REQUIRE(put(c, &a, 2) == 0 && take(c, &b, true) == 0);
    REQUIRE(check_frame(&b.f, (nvPixFormat)fmt, a.w, a.h, 0) == 0);
    give(&b);
    REQUIRE(nvmpi_jpeg_decoder_close(c) == 0);
    REQUIRE(nfds() == base);
    return 0;
}

/* The copy-out decoder (no fd mode) decodes through the same mock. Exact
 * size (pad 0): the legacy path reports the decoder's size, see the report. */
static int legacy_decode(int b_, int c_, int d_)
{
    struct img a = img_a();
    static unsigned char y[256 * 64], u[256 * 64], v[256 * 64];
    nvFrame f;
    nvPacket p;
    nvmpictx *c;
    unsigned x, yy, bad = 0;

    (void)b_; (void)c_; (void)d_;
    set_pad(0);
    c = nvmpi_create_jpeg_decoder();
    REQUIRE(c);
    memset(&p, 0, sizeof(p));
    p.payload = (unsigned char *)a.d;
    p.payload_size = a.n;
    p.pts = 9;
    REQUIRE(nvmpi_jpeg_decoder_put_packet(c, &p) == 0);
    memset(&f, 0, sizeof(f));
    f.payload[0] = y; f.payload[1] = u; f.payload[2] = v;
    f.linesize[0] = f.linesize[1] = f.linesize[2] = 256;
    REQUIRE(nvmpi_jpeg_decoder_get_frame(c, &f, true) == 0);
    REQUIRE(f.width == a.w && f.height == a.h && f.timestamp == 9);
    for (yy = 0; yy < a.h; yy++)
        for (x = 0; x < a.w; x++)
            bad += y[yy * 256 + x] != mock_jpeg_pattern(0, x, yy);
    for (yy = 0; yy < a.h / 2; yy++)
        for (x = 0; x < a.w / 2; x++)
            bad += u[yy * 256 + x] != mock_jpeg_pattern(1, x, yy) ||
                   v[yy * 256 + x] != mock_jpeg_pattern(2, x, yy);
    REQUIRE(bad == 0);
    {
        nvPacket pp = p;                             /* progressive: -1   */
        pp.payload = (unsigned char *)sof_prog_jpg;
        pp.payload_size = sof_prog_jpg_len;
        REQUIRE(nvmpi_jpeg_decoder_put_packet(c, &pp) == -1);
    }
    return nvmpi_jpeg_decoder_close(c) == 0 ? 0 : 1;
}

#endif
