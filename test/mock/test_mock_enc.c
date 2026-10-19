/*
 * test_mock_enc.c - the encoder model of the mock (mock/mock_enc*.c, issue
 * #100) at driver level, without libnvmpi. Part of the test_mock executable.
 *
 * Checks the access units, key frame/IDR/header placement, metadata,
 * timestamps, EOS and flush handling, back-pressure, latency, wake-ups and
 * the open fault against the contract in mock_enc.c. How libnvmpi copes
 * with the model is tested in test_api_enc_mock.c.
 */
#include "mock_enc_harness.h"

static void set_gop(int fd)
{
    CHECK(ctrl(fd, VIDIOC_S_EXT_CTRLS, V4L2_CID_MPEG_VIDEO_GOP_SIZE, 3) == 0);
    CHECK(ctrl(fd, VIDIOC_S_EXT_CTRLS, CID_IDR_INTERVAL, 6) == 0);
}

static void set_gop_hdr(int fd)
{
    set_gop(fd);
    CHECK(ctrl(fd, VIDIOC_S_EXT_CTRLS, CID_SPS_PPS_AT_IDR, 1) == 0);
}

/* Encode strlen(key) frames one at a time (put, get, recycle) and compare
 * each AU with the expectation strings: '1' = key frame / IDR NAL / SPS+PPS
 * present at that frame. HEVC additionally needs the VPS. */
static void check_stream(uint32_t codec, void (*pre)(int), const char *key,
                         const char *idr, const char *hdr)
{
    struct eh e;
    unsigned ty[8], cnt, hv = codec == H265;
    unsigned idr_t = hv ? 19 : 5, sps_t = hv ? 33 : 7, pps_t = hv ? 34 : 8;
    uint32_t bytes = 0, idx = 0;
    long pts;
    size_t i, n = strlen(key);
    int fl;

    CHECK(eh_open(&e, codec, 320, 240, pre) == 0);
    for (i = 0; i < n; i++) {
        CHECK(eh_frame(&e, 100 + (long)i) == 0);
        fl = eh_get(&e, ty, 8, &cnt, &bytes, &pts, &idx);
        CHECK(fl >= 0 && bytes > 0 && pts == 100 + (long)i);
        if (fl < 0)
            break;
        CHECK(!!(fl & V4L2_BUF_FLAG_KEYFRAME) == (key[i] == '1'));
        CHECK(!!(fl & V4L2_BUF_FLAG_PFRAME) == (key[i] != '1'));
        CHECK(nal_has(e.cap[idx], bytes, (int)hv, idr_t) == (idr[i] == '1'));
        CHECK(nal_has(e.cap[idx], bytes, (int)hv, sps_t) == (hdr[i] == '1'));
        CHECK(nal_has(e.cap[idx], bytes, (int)hv, pps_t) == (hdr[i] == '1'));
        CHECK(!hv || nal_has(e.cap[idx], bytes, 1, 32) == (hdr[i] == '1'));
        /* Metadata KeyFrame agrees with the flag, valid while DQ'd. */
        {
            struct { uint8_t key, eof; uint16_t qp; uint8_t pad[512]; } md;
            struct video_metadata vm = { 0, 0, &md, 0, idx };
            struct v4l2_ext_control c;
            struct v4l2_ext_controls cs;

            memset(&md, 0xee, sizeof(md));
            memset(&c, 0, sizeof(c));
            memset(&cs, 0, sizeof(cs));
            c.id = CID_ENC_METADATA;
            c.string = (char *)&vm;
            cs.count = 1;
            cs.controls = &c;
            CHECK(v4l2_ioctl(e.fd, VIDIOC_G_EXT_CTRLS, &cs) == 0);
            CHECK(md.key == (key[i] == '1'));
        }
        CHECK(qcap(&e, idx) == 0);
    }
    eh_close(&e);
}

static void test_streams(void)
{
    /* No GOP controls: only frame 0 is key/IDR/headers. */
    check_stream(H264, NULL, "1000", "1000", "1000");
    check_stream(H265, NULL, "1000", "1000", "1000");
    /* GOP 3, IDR every 6: key at 0,3,6,9; IDR at 0,6; headers only first. */
    check_stream(H264, set_gop, "10010010010", "10000010000", "10000000000");
    check_stream(H265, set_gop, "10010010010", "10000010000", "10000000000");
    /* Same with SPS/PPS at every IDR. */
    check_stream(H264, set_gop_hdr, "10010010010", "10000010000", "10000010000");
    check_stream(H265, set_gop_hdr, "10010010010", "10000010000", "10000010000");
}

static void test_force_idr(void)
{
    struct eh e;
    unsigned ty[8], cnt;
    uint32_t bytes = 0, idx = 0;
    int i, fl;

    CHECK(eh_open(&e, H264, 320, 240, NULL) == 0);
    for (i = 0; i < 4; i++) {
        if (i == 2)                      /* IDR on the next queued frame   */
            CHECK(ctrl(e.fd, VIDIOC_S_EXT_CTRLS,
                       V4L2_CID_MPEG_MFC51_VIDEO_FORCE_FRAME_TYPE, 0) == 0);
        CHECK(eh_frame(&e, i) == 0);
        fl = eh_get(&e, ty, 8, &cnt, &bytes, NULL, &idx);
        CHECK(fl >= 0);
        CHECK(!!(fl & V4L2_BUF_FLAG_KEYFRAME) == (i == 0 || i == 2));
        CHECK(nal_has(e.cap[idx], bytes, 0, 5) == (i == 0 || i == 2));
        CHECK(qcap(&e, idx) == 0);
    }
    eh_close(&e);
}

static void test_eos_flush(void)
{
    struct eh e;
    unsigned ty[8], cnt, t = OUT;
    uint32_t bytes = 0, idx = 0;
    int i, fl;

    g_oflags = O_NONBLOCK;               /* every result is ready at once  */
    CHECK(eh_open(&e, H264, 320, 240, NULL) == 0);
    for (i = 0; i < 6; i++) {            /* wraps the 4 slots              */
        CHECK(eh_frame(&e, i) == 0);
        CHECK(eh_get(&e, ty, 8, &cnt, &bytes, NULL, &idx) >= 0);
        CHECK(qcap(&e, idx) == 0);
    }
    /* EOS marker in a RECLAIMED slot: empty LAST AU, marker returned. */
    CHECK(eh_slot(&e, &i) == 0 && eh_put(&e, (unsigned)i, 0, 0) == 0);
    fl = eh_get(&e, ty, 8, &cnt, &bytes, NULL, &idx);
    CHECK(fl >= 0 && bytes == 0 && (fl & V4L2_BUF_FLAG_LAST) && cnt == 0);
    CHECK(qcap(&e, idx) == 0);
    /* Input after EOS is dropped: slot comes back, no AU appears. */
    CHECK(eh_slot(&e, &i) == 0 && eh_put(&e, (unsigned)i, 4096, 7) == 0);
    {                                    /* no AU: the eventfd goes idle  */
        struct pollfd pf = { e.fd, POLLIN, 0 };

        while (dq_raw(&e, OUT, NULL) == 0)
            ;
        CHECK(poll(&pf, 1, 20) == 0);
    }
    /* Flush = OUTPUT STREAMOFF/STREAMON: a fresh stream starts with an IDR
     * carrying SPS/PPS again. */
    CHECK(v4l2_ioctl(e.fd, VIDIOC_STREAMOFF, &t) == 0);
    CHECK(v4l2_ioctl(e.fd, VIDIOC_STREAMON, &t) == 0);
    e.out_used = 0;
    CHECK(eh_frame(&e, 50) == 0);
    fl = eh_get(&e, ty, 8, &cnt, &bytes, NULL, &idx);
    CHECK(fl >= 0 && (fl & V4L2_BUF_FLAG_KEYFRAME) && nal_has(e.cap[idx], bytes, 0, 7));
    eh_close(&e);
    g_oflags = 0;
}

/* Without a free CAPTURE buffer the frame stays queued (its OUTPUT buffer
 * is not returned); queueing a CAPTURE buffer releases it. Nothing is lost. */
static void test_backpressure(void)
{
    struct eh e;
    unsigned ty[8], cnt, i;
    uint32_t bytes = 0, idx = 0;
    long pts;

    g_oflags = O_NONBLOCK;
    CHECK(eh_open(&e, H264, 320, 240, NULL) == 0);
    for (i = 0; i < EH_N; i++) {         /* occupy every CAPTURE buffer    */
        CHECK(eh_frame(&e, (long)i) == 0);
        ms(5);
    }
    for (i = 0; i < EH_N; i++)           /* all OUTPUT slots came back     */
        CHECK(dq_raw(&e, OUT, NULL) == 0);
    CHECK(eh_put(&e, 0, 4096, 9) == 0);  /* the 5th frame: no CAPTURE free */
    ms(20);
    CHECK(dq_raw(&e, OUT, NULL) != 0 && errno == EAGAIN);   /* held        */
    for (i = 0; i < EH_N; i++) {         /* free a CAPTURE buffer          */
        CHECK(eh_get(&e, ty, 8, &cnt, &bytes, &pts, &idx) >= 0 && pts == (long)i);
        if (i == 0)
            CHECK(qcap(&e, idx) == 0);   /* releases the held frame        */
    }
    ms(5);
    CHECK(eh_get(&e, ty, 8, &cnt, &bytes, &pts, &idx) >= 0 && pts == 9);
    CHECK(dq_raw(&e, OUT, NULL) == 0);   /* its OUTPUT buffer is back now  */
    eh_close(&e);
    g_oflags = 0;
}

/* NVMPI_MOCK_ENC_LATENCY=2: the first two consumed OUTPUT buffers are not
 * returned until later frames (or EOS) push them out. */
static void test_latency(void)
{
    struct eh e;
    unsigned ty[8], cnt, i;
    uint32_t bytes = 0, idx = 0;

    setenv("NVMPI_MOCK_ENC_LATENCY", "2", 1);
    g_oflags = O_NONBLOCK;
    CHECK(eh_open(&e, H264, 320, 240, NULL) == 0);
    for (i = 0; i < 2; i++) {
        CHECK(eh_frame(&e, (long)i) == 0);
        CHECK(eh_get(&e, ty, 8, &cnt, &bytes, NULL, &idx) >= 0);
        CHECK(qcap(&e, idx) == 0);
    }
    CHECK(dq_raw(&e, OUT, NULL) != 0 && errno == EAGAIN);   /* held        */
    CHECK(eh_frame(&e, 2) == 0);
    CHECK(eh_get(&e, ty, 8, &cnt, &bytes, NULL, &idx) >= 0);
    CHECK(dq_raw(&e, OUT, NULL) == 0);    /* frame 0's buffer pushed out   */
    CHECK(dq_raw(&e, OUT, NULL) != 0 && errno == EAGAIN);
    CHECK(eh_put(&e, 3, 0, 0) == 0);      /* EOS releases everything       */
    ms(5);
    CHECK(dq_raw(&e, OUT, NULL) == 0 && dq_raw(&e, OUT, NULL) == 0 &&
          dq_raw(&e, OUT, NULL) == 0);
    eh_close(&e);
    g_oflags = 0;
    unsetenv("NVMPI_MOCK_ENC_LATENCY");
}

static void *cap_waiter(void *arg)
{
    struct eh *e = arg;

    return (void *)(intptr_t)(dq_raw(e, CAP, NULL) == 0 ? 0 : errno);
}

/* A blocked CAPTURE DQBUF is released by STREAMOFF (EINVAL, like the device:
 * libnvmpi's DQ thread ends on it) and by close (EBADF). */
static void test_wakeups(void)
{
    struct eh e;
    pthread_t th;
    void *rv;
    unsigned t = CAP;

    CHECK(eh_open(&e, H264, 320, 240, NULL) == 0);
    CHECK(pthread_create(&th, NULL, cap_waiter, &e) == 0);
    ms(30);
    CHECK(v4l2_ioctl(e.fd, VIDIOC_STREAMOFF, &t) == 0);
    CHECK(pthread_join(th, &rv) == 0 && (intptr_t)rv == EINVAL);
    eh_close(&e);

    CHECK(eh_open(&e, H264, 320, 240, NULL) == 0);
    CHECK(pthread_create(&th, NULL, cap_waiter, &e) == 0);
    ms(30);
    /* close while a DQBUF is blocked: the thread must return EBADF. */
    CHECK(v4l2_close(e.fd) == 0);
    CHECK(pthread_join(th, &rv) == 0 && (intptr_t)rv == EBADF);
}

/* Also nodes of the older driver name get the model; open@N fails the open. */
static void test_nodes_and_open_fault(void)
{
    int fd = v4l2_open("/dev/nvhost-msenc", O_RDWR);
    struct v4l2_ext_controls none;

    /* The encoder model rejects an empty control list; a plain node would
     * accept it - so this tells the model apart. */
    memset(&none, 0, sizeof(none));
    CHECK(fd >= 0 && v4l2_ioctl(fd, VIDIOC_S_EXT_CTRLS, &none) != 0 &&
          errno == EINVAL);
    if (fd >= 0)
        v4l2_close(fd);
    setenv("NVMPI_MOCK_FAULT", "open@2", 1);
    fd = v4l2_open(ENC_NODE, O_RDWR);    /* 1st call counted (new spec)    */
    CHECK(fd >= 0);
    CHECK(v4l2_open(ENC_NODE, O_RDWR) < 0 && errno == ENODEV);   /* 2nd    */
    {
        int fd2 = v4l2_open(ENC_NODE, O_RDWR);                  /* 3rd ok  */

        CHECK(fd2 >= 0);
        if (fd2 >= 0)
            v4l2_close(fd2);
    }
    if (fd >= 0)
        v4l2_close(fd);
    unsetenv("NVMPI_MOCK_FAULT");
}

void test_encoder_model(void)
{
    test_streams();
    test_force_idr();
    test_eos_flush();
    test_backpressure();
    test_latency();
    test_wakeups();
    test_nodes_and_open_fault();
}
