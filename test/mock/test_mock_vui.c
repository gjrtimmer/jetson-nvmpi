/*
 * test_mock_vui.c - frame rate, VUI timing_info and the AU size bound of the
 * mock encoder (#116, mock/mock_enc*.c) at driver level. The SPS of the
 * first access unit is parsed back with the minimal readers of bit_read.h.
 * Part of the test_mock executable.
 */
#include "bit_read.h"
#include "mock_enc_harness.h"

static uint32_t g_fps_n, g_fps_d;        /* S_PARM sent by set_fps (0 = none) */
static int g_vui;                        /* INSERT_VUI value set by set_fps   */

/* What NvVideoEncoder issues after setFormat: INSERT_VUI, then S_PARM on the
 * OUTPUT queue (numerator = fps_d, denominator = fps_n), before REQBUFS. */
static void set_fps(int fd)
{
    struct v4l2_streamparm sp;

    if (g_vui)
        CHECK(ctrl(fd, VIDIOC_S_EXT_CTRLS, CID_INSERT_VUI, 1) == 0);
    if (!g_fps_n)
        return;
    memset(&sp, 0, sizeof(sp));
    sp.type = OUT;
    sp.parm.output.timeperframe.numerator = g_fps_d;
    sp.parm.output.timeperframe.denominator = g_fps_n;
    CHECK(v4l2_ioctl(fd, VIDIOC_S_PARM, &sp) == 0);
}

/* Read ahead to the end of the H.264 SPS and parse the VUI. Returns -1 when
 * no VUI, else 0 with the timing fields filled (all-zero when no timing). */
static int h264_vui(const uint8_t *rbsp, size_t n, uint32_t *tick,
                    uint32_t *scale, unsigned *fixed)
{
    struct br r = { rbsp, n, 0 };
    unsigned i;

    CHECK(get_bits(&r, 8) == 66);             /* profile baseline           */
    (void)get_bits(&r, 16);                   /* constraints, level         */
    CHECK(get_ue(&r) == 0 && get_ue(&r) == 0);
    CHECK(get_ue(&r) == 2 && get_ue(&r) == 1);
    CHECK(get_bits(&r, 1) == 0);
    (void)get_ue(&r);
    (void)get_ue(&r);
    CHECK(get_bits(&r, 2) == 3);              /* frame_mbs_only, direct 8x8 */
    if (get_bits(&r, 1))                      /* frame_cropping_flag        */
        for (i = 0; i < 4; i++)
            (void)get_ue(&r);
    if (!get_bits(&r, 1))                     /* vui_parameters_present     */
        return -1;
    CHECK(get_bits(&r, 4) == 0);              /* aspect, overscan, signal,
                                               * chroma loc: all absent     */
    CHECK(get_bits(&r, 1) == 1);              /* timing_info_present_flag   */
    *tick = get_bits(&r, 32);
    *scale = get_bits(&r, 32);
    *fixed = get_bits(&r, 1);
    CHECK(get_bits(&r, 4) == 0);              /* hrd x2, pic_struct, restr. */
    CHECK(r.pos <= n * 8 - 1);                /* the stop bit follows       */
    return 0;
}

static int hevc_vui(const uint8_t *rbsp, size_t n, uint32_t *tick,
                    uint32_t *scale)
{
    struct br r = { rbsp, n, 0 };
    unsigned i;

    (void)get_bits(&r, 8 + 96);               /* ids/flags + profile_tier_level */
    CHECK(get_ue(&r) == 0 && get_ue(&r) == 1);
    (void)get_ue(&r);
    (void)get_ue(&r);
    if (get_bits(&r, 1))                      /* conformance_window_flag    */
        for (i = 0; i < 4; i++)
            (void)get_ue(&r);
    for (i = 0; i < 3; i++)                   /* bit depths, poc lsb        */
        (void)get_ue(&r);
    (void)get_bits(&r, 1);                    /* sub_layer_ordering_info    */
    for (i = 0; i < 3 + 4 + 2; i++)           /* dpb, cb/tb sizes, depths   */
        (void)get_ue(&r);
    if (get_bits(&r, 4) & 1) {                /* scaling, amp, sao, pcm     */
        (void)get_bits(&r, 8);                /* PCM bit depths (#118)      */
        (void)get_ue(&r);                     /* PCM CB sizes               */
        (void)get_ue(&r);
        (void)get_bits(&r, 1);                /* pcm_loop_filter_disabled   */
    }
    (void)get_ue(&r);                         /* num_short_term_ref_pic_sets */
    (void)get_bits(&r, 3);                    /* long term, tmvp, strong    */
    if (!get_bits(&r, 1))                     /* vui_parameters_present     */
        return -1;
    CHECK(get_bits(&r, 8) == 0);              /* aspect .. default window   */
    CHECK(get_bits(&r, 1) == 1);              /* vui_timing_info_present    */
    *tick = get_bits(&r, 32);
    *scale = get_bits(&r, 32);
    CHECK(get_bits(&r, 3) == 0);              /* poc prop., hrd, restriction */
    CHECK(get_bits(&r, 1) == 0 && get_bits(&r, 1) == 1);   /* extension, stop */
    return 0;
}

/* One stream: parse the first AU. fps 0 = leave the default (30/1). */
static void check_vui(uint32_t codec, uint32_t w, uint32_t h, int vui,
                      uint32_t fn, uint32_t fd)
{
    struct eh e;
    unsigned ty[8], cnt, fixed = 0, hv = codec == H265;
    uint32_t bytes = 0, idx = 0, tick = 0, scale = 0;
    uint8_t rbsp[96];
    size_t n;
    int fl, v;

    g_vui = vui;
    g_fps_n = fn;
    g_fps_d = fd;
    CHECK(eh_open(&e, codec, w, h, set_fps) == 0);
    CHECK(eh_frame(&e, 1) == 0);
    fl = eh_get(&e, ty, 8, &cnt, &bytes, NULL, &idx);
    CHECK(fl >= 0 && !(fl & V4L2_BUF_FLAG_ERROR) && bytes > 0);
    n = nal_rbsp(e.cap[idx], bytes, (int)hv, hv ? 33 : 7, rbsp, sizeof(rbsp));
    CHECK(n > 0);
    v = hv ? hevc_vui(rbsp, n, &tick, &scale)
           : h264_vui(rbsp, n, &tick, &scale, &fixed);
    CHECK((v == 0) == !!vui);
    if (vui && v == 0) {
        uint32_t en = fn ? fn : 30, ed = fn ? fd : 1;

        CHECK(tick == ed);                    /* num_units_in_tick = fps_d  */
        CHECK(scale == (hv ? en : 2 * en));   /* H.264 counts fields        */
        CHECK(hv || fixed == 1);
    }
    CHECK(qcap(&e, idx) == 0);
    eh_close(&e);
    g_vui = 0;
    g_fps_n = g_fps_d = 0;
}

static void test_vui(void)
{
    uint32_t c[2] = { H264, H265 };
    unsigned i;

    for (i = 0; i < 2; i++) {
        check_vui(c[i], 320, 240, 0, 25, 1);         /* off: no VUI          */
        check_vui(c[i], 320, 240, 0, 0, 0);
        check_vui(c[i], 320, 240, 1, 25, 1);
        check_vui(c[i], 320, 240, 1, 30000, 1001);
        check_vui(c[i], 320, 240, 1, 0, 0);          /* no S_PARM: 30/1      */
        check_vui(c[i], 1920, 1080, 1, 25, 1);       /* cropped H.264 SPS    */
        check_vui(c[i], 642, 362, 1, 30000, 1001);   /* odd size, cropping   */
    }
}

static void test_parm(void)
{
    struct eh e;
    struct v4l2_streamparm sp;

    CHECK(eh_open(&e, H264, 320, 240, NULL) == 0);
    memset(&sp, 0, sizeof(sp));
    sp.type = OUT;
    CHECK(v4l2_ioctl(e.fd, VIDIOC_G_PARM, &sp) == 0);
    CHECK(sp.parm.output.timeperframe.numerator == 1 &&
          sp.parm.output.timeperframe.denominator == 30);   /* default 30 fps */
    sp.parm.output.timeperframe.numerator = 1001;
    sp.parm.output.timeperframe.denominator = 30000;
    CHECK(v4l2_ioctl(e.fd, VIDIOC_S_PARM, &sp) == 0);
    memset(&sp, 0, sizeof(sp));
    sp.type = OUT;
    CHECK(v4l2_ioctl(e.fd, VIDIOC_G_PARM, &sp) == 0);
    CHECK(sp.parm.output.timeperframe.numerator == 1001 &&
          sp.parm.output.timeperframe.denominator == 30000);
    /* Zero numerator or denominator: "driver picks", the value is kept. */
    memset(&sp, 0, sizeof(sp));
    sp.type = OUT;
    CHECK(v4l2_ioctl(e.fd, VIDIOC_S_PARM, &sp) == 0);
    CHECK(v4l2_ioctl(e.fd, VIDIOC_G_PARM, &sp) == 0);
    CHECK(sp.parm.output.timeperframe.denominator == 30000);
    sp.type = 0;                         /* not an MPLANE type             */
    CHECK(v4l2_ioctl(e.fd, VIDIOC_G_PARM, &sp) != 0 && errno == EINVAL);
    eh_close(&e);
}

/* One frame at w x h with CAPTURE sizeimage capsz: returns the DQ flags. */
static int one_frame(uint32_t w, uint32_t h, uint32_t capsz, uint32_t *bytes)
{
    struct eh e;
    unsigned ty[8], cnt;
    uint32_t idx = 0;
    int fl;

    g_capsz = capsz;
    CHECK(eh_open(&e, H264, w, h, NULL) == 0);
    CHECK(eh_frame(&e, 1) == 0);
    fl = eh_get(&e, ty, 8, &cnt, bytes, NULL, &idx);
    CHECK(qcap(&e, idx) == 0);
    eh_close(&e);
    g_capsz = EH_CAPSZ;
    return fl;
}

static void test_size_bound(void)
{
    uint32_t b = 0;
    int fl;

    /* 1080p builds its AU through the heap scratch buffer (3.1 MB). */
    fl = one_frame(1920, 1080, EH_CAPSZ, &b);
    CHECK(fl >= 0 && !(fl & V4L2_BUF_FLAG_ERROR) && b > 64);
    /* Just inside / outside the 6.5 MP bound. */
    fl = one_frame(2540, 2540, EH_CAPSZ, &b);
    CHECK(fl >= 0 && !(fl & V4L2_BUF_FLAG_ERROR) && b > 64);
    fl = one_frame(2700, 2700, EH_CAPSZ, &b);
    CHECK(fl >= 0 && (fl & V4L2_BUF_FLAG_ERROR) && b == 1);
    fl = one_frame(8192, 8192, EH_CAPSZ, &b);
    CHECK(fl >= 0 && (fl & V4L2_BUF_FLAG_ERROR) && b == 1);
    /* An AU larger than the CAPTURE buffer is flagged, never truncated. */
    fl = one_frame(320, 240, 64, &b);
    CHECK(fl >= 0 && (fl & V4L2_BUF_FLAG_ERROR) && b == 1);
}

void test_encoder_vui(void)
{
    test_parm();
    test_vui();
    test_size_bound();
}
