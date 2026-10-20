/*
 * mock_enc_harness.h - a minimal V4L2 encoder client for the mock tests: the
 * call sequence NvVideoEncoder + libnvmpi issue (MMAP planes on both queues,
 * S_EXT_CTRLS before REQBUFS, mapped CAPTURE buffers), reduced to what a
 * driver-level test needs. Header-only; used by test_mock_enc.c only.
 */
#ifndef NVMPI_TEST_MOCK_ENC_HARNESS_H
#define NVMPI_TEST_MOCK_ENC_HARNESS_H

#include "mock_test.h"
#include "nal_scan.h"

#include <poll.h>
#include <stdlib.h>
#include <time.h>

#define ENC_NODE "/dev/v4l2-nvenc"
#define EH_N     4                       /* buffers per queue              */
#define EH_CAPSZ (10u << 20)             /* NVMPI_ENC_CHUNK_SIZE: a PCM key
                                          * frame (#117) is ~1.5 bytes/pixel */
#define H264 V4L2_PIX_FMT_H264
#define H265 v4l2_fourcc('H', '2', '6', '5')

/* ABI twins of the NVIDIA ids/structs used here (see mock_enc_ctl.c). */
#define CID_IDR_INTERVAL    (V4L2_CID_MPEG_BASE + 514)
#define CID_SPS_PPS_AT_IDR  (V4L2_CID_MPEG_BASE + 535)
#define CID_ENC_METADATA    (V4L2_CID_MPEG_BASE + 536)
#define CID_INSERT_VUI      (V4L2_CID_MPEG_BASE + 546)
struct video_metadata { void *a, *b, *enc, *mv; uint32_t index; };

static int g_oflags;                     /* extra v4l2_open flags (O_NONBLOCK) */
static uint32_t g_capsz = EH_CAPSZ;      /* CAPTURE sizeimage asked for      */
static uint32_t g_outfmt = V4L2_PIX_FMT_NV12M;   /* OUTPUT pixelformat       */
static uint32_t g_outmem = V4L2_MEMORY_MMAP;     /* OUTPUT memory type       */

struct eh {
    int fd;
    uint8_t *cap[EH_N];                  /* CAPTURE mappings               */
    int dmafd[EH_N];                     /* DMABUF: dma-buf per OUTPUT slot,
                                          * set by the test before eh_put  */
    uint8_t *raw[EH_N][3];               /* OUTPUT plane mappings (rw)     */
    uint32_t caplen;                     /* CAPTURE mapping length         */
    unsigned onp;                        /* OUTPUT planes of g_outfmt      */
    uint32_t ostride[3], olen[3];        /* OUTPUT bytesperline / sizeimage */
    unsigned out_used;
    unsigned hevc;
};

static inline void ms(unsigned n)
{
    struct timespec ts = { n / 1000, (long)(n % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

static inline int rb(int fd, uint32_t type, unsigned n)
{
    struct v4l2_requestbuffers r;

    memset(&r, 0, sizeof(r));
    r.type = type;
    r.memory = type == OUT ? g_outmem : V4L2_MEMORY_MMAP;
    r.count = n;
    return v4l2_ioctl(fd, VIDIOC_REQBUFS, &r);
}

static inline int ctrl(int fd, unsigned long req, uint32_t id, int32_t val)
{
    struct v4l2_ext_control c;
    struct v4l2_ext_controls cs;

    memset(&c, 0, sizeof(c));
    memset(&cs, 0, sizeof(cs));
    c.id = id;
    c.value = val;
    cs.count = 1;
    cs.controls = &c;
    return v4l2_ioctl(fd, req, &cs);
}

static inline int qcap(struct eh *e, unsigned i)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];

    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.type = CAP;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = i;
    b.length = 1;
    b.m.planes = pl;
    return v4l2_ioctl(e->fd, VIDIOC_QBUF, &b);
}

/* Open + format both planes + MMAP buffers + STREAMON; controls are set by
 * the caller via `pre` (before REQBUFS, like NvVideoEncoder) when given. */
static inline int eh_open(struct eh *e, uint32_t codec, uint32_t w, uint32_t h,
                   void (*pre)(int fd))
{
    struct v4l2_format f;
    unsigned i, t;

    memset(e, 0, sizeof(*e));
    e->hevc = codec == H265;
    e->fd = v4l2_open(ENC_NODE, O_RDWR | g_oflags);
    if (e->fd < 0)
        return -1;
    memset(&f, 0, sizeof(f));
    f.type = CAP;
    f.fmt.pix_mp.pixelformat = codec;
    f.fmt.pix_mp.width = w;
    f.fmt.pix_mp.height = h;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = g_capsz;
    if (v4l2_ioctl(e->fd, VIDIOC_S_FMT, &f))
        return -1;
    memset(&f, 0, sizeof(f));
    f.type = OUT;
    f.fmt.pix_mp.pixelformat = g_outfmt;
    f.fmt.pix_mp.width = w;
    f.fmt.pix_mp.height = h;
    f.fmt.pix_mp.num_planes = g_outfmt == V4L2_PIX_FMT_YUV420M ? 3 : 2;
    if (v4l2_ioctl(e->fd, VIDIOC_S_FMT, &f))
        return -1;
    e->onp = f.fmt.pix_mp.num_planes;    /* the driver decides the layout   */
    for (i = 0; i < e->onp && i < 3; i++) {
        e->ostride[i] = f.fmt.pix_mp.plane_fmt[i].bytesperline;
        e->olen[i] = f.fmt.pix_mp.plane_fmt[i].sizeimage;
    }
    if (pre)
        pre(e->fd);
    if (rb(e->fd, OUT, EH_N) || rb(e->fd, CAP, EH_N))
        return -1;
    for (i = 0; i < EH_N; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane pl[VIDEO_MAX_PLANES];

        memset(&b, 0, sizeof(b));
        memset(pl, 0, sizeof(pl));
        b.type = CAP;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = 1;
        b.m.planes = pl;
        if (v4l2_ioctl(e->fd, VIDIOC_QUERYBUF, &b))
            return -1;
        /* Map like NvBuffer::map(): the EXPBUF fd at mem_offset. */
        {
            struct v4l2_exportbuffer eb;

            memset(&eb, 0, sizeof(eb));
            eb.type = CAP;
            eb.index = i;
            if (v4l2_ioctl(e->fd, VIDIOC_EXPBUF, &eb))
                return -1;
            e->caplen = pl[0].length;
            e->cap[i] = mmap(NULL, pl[0].length, PROT_READ, MAP_SHARED, eb.fd,
                             (off_t)pl[0].m.mem_offset);
            close(eb.fd);                /* the mapping outlives the fd     */
        }
        if (e->cap[i] == MAP_FAILED)
            return -1;
    }
    for (i = 0; i < EH_N && g_outmem == V4L2_MEMORY_MMAP; i++) {
        /* map the raw planes, like CAPTURE */
        struct v4l2_buffer b;
        struct v4l2_plane pl[VIDEO_MAX_PLANES];
        struct v4l2_exportbuffer eb;
        unsigned p;

        memset(&b, 0, sizeof(b));
        memset(pl, 0, sizeof(pl));
        b.type = OUT;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = e->onp;
        b.m.planes = pl;
        if (v4l2_ioctl(e->fd, VIDIOC_QUERYBUF, &b))
            return -1;
        memset(&eb, 0, sizeof(eb));
        eb.type = OUT;
        eb.index = i;
        if (v4l2_ioctl(e->fd, VIDIOC_EXPBUF, &eb))
            return -1;
        for (p = 0; p < e->onp && p < 3; p++) {
            e->raw[i][p] = mmap(NULL, pl[p].length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, eb.fd, (off_t)pl[p].m.mem_offset);
            if (e->raw[i][p] == MAP_FAILED)
                return -1;
        }
        close(eb.fd);
    }
    t = OUT;
    if (v4l2_ioctl(e->fd, VIDIOC_STREAMON, &t))
        return -1;
    t = CAP;
    if (v4l2_ioctl(e->fd, VIDIOC_STREAMON, &t))
        return -1;
    for (i = 0; i < EH_N; i++)
        if (qcap(e, i))
            return -1;
    return 0;
}

static inline void eh_close(struct eh *e)
{
    unsigned i, p;

    (void)v4l2_close(e->fd);             /* mappings outlive the memfd     */
    for (i = 0; i < EH_N; i++) {         /* unmap what eh_open mapped      */
        if (e->cap[i] && e->cap[i] != MAP_FAILED)
            munmap(e->cap[i], e->caplen);
        for (p = 0; p < e->onp && p < 3; p++)
            if (e->raw[i][p] && e->raw[i][p] != MAP_FAILED)
                munmap(e->raw[i][p], e->olen[p]);
    }
}

/* Queue a raw frame (bytesused != 0) or the EOS marker (n == 0). */
static inline int eh_put(struct eh *e, unsigned idx, uint32_t used, long pts)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];

    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.type = OUT;
    b.memory = g_outmem;
    b.index = idx;
    b.length = e->onp;
    b.m.planes = pl;
    pl[0].m.fd = e->dmafd[idx];          /* DMABUF: one fd covers all planes */
    pl[0].bytesused = used;
    pl[1].bytesused = used / 2;
    pl[2].bytesused = used / 2;
    b.timestamp.tv_usec = pts;
    return v4l2_ioctl(e->fd, VIDIOC_QBUF, &b);
}

/* Reclaim a used OUTPUT buffer index (or the next unused one). */
static inline int eh_slot(struct eh *e, int *idx)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];

    if (e->out_used < EH_N) {
        *idx = (int)e->out_used++;
        return 0;
    }
    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.type = OUT;
    b.memory = g_outmem;
    b.length = e->onp;
    b.m.planes = pl;
    if (v4l2_ioctl(e->fd, VIDIOC_DQBUF, &b))
        return -1;
    CHECK(pl[0].bytesused == 0);         /* returned buffers report empty  */
    *idx = (int)b.index;
    return 0;
}

static inline int eh_frame(struct eh *e, long pts)
{
    int i;

    return eh_slot(e, &i) ? -1 : eh_put(e, (unsigned)i, 4096, pts);
}

/* DQ one AU: fills types; returns flags, or -1 on error. *bytes = payload. */
static inline int eh_get(struct eh *e, unsigned *ty, unsigned max, unsigned *cnt,
                  uint32_t *bytes, long *pts, uint32_t *idx)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];

    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.type = CAP;
    b.memory = V4L2_MEMORY_MMAP;
    b.length = 1;
    b.m.planes = pl;
    if (v4l2_ioctl(e->fd, VIDIOC_DQBUF, &b))
        return -1;
    *bytes = pl[0].bytesused;
    *cnt = nal_types(e->cap[b.index], *bytes, (int)e->hevc, ty, max);
    if (pts)
        *pts = b.timestamp.tv_usec;
    if (idx)
        *idx = b.index;
    return (int)b.flags;
}

/* Raw DQBUF of queue `type`; returns 0 or -1 with errno. */
static inline int dq_raw(struct eh *e, uint32_t type, uint32_t *used)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[VIDEO_MAX_PLANES];
    int r;

    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.type = type;
    b.memory = type == OUT ? g_outmem : V4L2_MEMORY_MMAP;
    b.length = type == OUT ? e->onp : 1;
    b.m.planes = pl;
    r = v4l2_ioctl(e->fd, VIDIOC_DQBUF, &b);
    if (!r && used)
        *used = pl[0].bytesused;
    return r;
}


#endif /* NVMPI_TEST_MOCK_ENC_HARNESS_H */
