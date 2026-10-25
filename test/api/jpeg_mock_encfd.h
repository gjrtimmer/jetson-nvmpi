/*
 * jpeg_mock_encfd.h - nvmpi_jpeg_encoder_put_frame_fd cases of
 * test_api_jpeg_mock.c (#158, #5). Needs jpeg_mock_util.h and
 * jpeg_mock_enc.h first.
 *
 * The mock NvJPEGEncoder encodes no pixels but records the surface of its
 * last encodeFromFd call (mock/mock_jpegenc.h): that tells the direct path
 * (the caller's own fd) from the VIC path (the encoder's YUV420 input
 * buffer) and shows the size that was encoded. A packet is the 4-byte
 * SOI/EOI stand-in whose pts is the descriptor's timestamp.
 *
 *   jpegfd_direct      YUV420 surface of exactly the picture: caller's fd
 *   jpegfd_vic         NV12 surface, and padded / larger YUV420 surfaces
 *   jpegfd_odd         641x357 from both formats
 *   jpegfd_dec_fd2     frames of the mock video decoder (get_frame_fd2)
 *   jpegfd_bad_fd      dup'd, foreign, closed, -1 fd: INVAL, packet kept
 *   jpegfd_bad_desc    every descriptor field wrong: INVAL, packet kept
 *   jpegfd_eos         NULL frame = EOS, then -1; NULL ctx
 *
 * The interleaved and descriptor-count cases are in jpeg_mock_encfd_mix.h.
 */
#ifndef NVMPI_TEST_JPEG_MOCK_ENCFD_H
#define NVMPI_TEST_JPEG_MOCK_ENCFD_H

#include "../mock/h264_fixture.h"
#include <fcntl.h>
#include "../../mock/mock_jpegenc.h"

struct efs {                             /* one registered test surface   */
    NvBufSurface *s;
    int fd;
};

/* Surface of pixel format pf (NV_PIX_NV12 or NV_PIX_YUV420) at w x h. */
static void ef_new(struct efs *e, int pf, unsigned w, unsigned h)
{
    NvBufSurfaceCreateParams p;

    memset(&p, 0, sizeof(p));
    p.width = w;
    p.height = h;
    p.colorFormat = pf == NV_PIX_NV12 ? NVBUF_COLOR_FORMAT_NV12
                                      : NVBUF_COLOR_FORMAT_YUV420;
    p.layout = NVBUF_LAYOUT_PITCH;
    p.memType = NVBUF_MEM_SURFACE_ARRAY;
    e->s = NULL;
    REQUIRE(NvBufSurfaceCreate(&e->s, 1, &p) == 0 && e->s);
    e->fd = (int)e->s->surfaceList[0].bufferDesc;
}

static void ef_del(struct efs *e)
{
    REQUIRE(NvBufSurfaceDestroy(e->s) == 0);
}

/* The descriptor of surface e for a w x h picture, layout from the library. */
static void ef_desc(nvmpi_fd_frame *f, const struct efs *e, unsigned w,
                    unsigned h, long pts)
{
    memset(f, 0, sizeof(*f));
    f->struct_size = sizeof(*f);
    REQUIRE(nvmpi_surface_get_layout(e->fd, f) == 0);
    f->width = w;
    f->height = h;
    f->timestamp = pts;
}

static struct mock_jpegenc_last ef_last(void)
{
    struct mock_jpegenc_last l;

    nvmpi_mock_jpegenc_last(&l);
    return l;
}

/* The staged packet is the stand-in with the given pts. */
static int ef_packet(nvmpictx *c, unsigned long pts)
{
    unsigned char buf[16];
    nvPacket p;

    memset(&p, 0, sizeof(p));
    p.payload = buf;
    if (nvmpi_jpeg_encoder_get_packet(c, &p) != 0 || p.payload_size != 4 ||
        p.pts != pts)
        return 0;
    return buf[0] == 0xff && buf[1] == 0xd8 && buf[2] == 0xff && buf[3] == 0xd9;
}

/* Put d, expect success and a packet; what the engine saw: direct = the
 * caller's fd, else the encoder's own YUV420 buffer of ew x eh. */
static int ef_ok(nvmpictx *c, const nvmpi_fd_frame *d, int direct,
                 unsigned ew, unsigned eh)
{
    struct mock_jpegenc_last b = ef_last(), a;

    if (nvmpi_jpeg_encoder_put_frame_fd(c, d) != 0)
        return 0;
    a = ef_last();
    if (a.calls != b.calls + 1 || a.color_format != NVBUF_COLOR_FORMAT_YUV420 ||
        a.width != ew || a.height != eh)
        return 0;
    if (direct ? a.fd != d->fd : a.fd == d->fd)
        return 0;
    return ef_packet(c, (unsigned long)d->timestamp);
}

static int jpegfd_direct(int a_, int b_, int c_)
{
    nvmpictx *c = nvmpi_create_jpeg_encoder(85);
    struct efs e;
    nvmpi_fd_frame d;

    (void)a_; (void)b_; (void)c_;
    REQUIRE(c);
    ef_new(&e, NV_PIX_YUV420, 640, 360);
    ef_desc(&d, &e, 640, 360, 77);
    REQUIRE(ef_ok(c, &d, 1, 640, 360));
    ef_del(&e);
    return nvmpi_jpeg_encoder_close(c) == 0 ? 0 : 1;
}

/* a = source format (NV12 or YUV420), b/c = allocated size, encoded size is
 * 640 x 356 when the surface is larger (padded), else the surface. */
static int jpegfd_vic(int a, int b, int c_)
{
    nvmpictx *c = nvmpi_create_jpeg_encoder(85);
    unsigned ew = b > 640 ? 640 : (unsigned)b, eh = c_ > 358 ? 356 : (unsigned)c_;
    struct efs e;
    nvmpi_fd_frame d;
    int i;

    REQUIRE(c);
    ef_new(&e, a, (unsigned)b, (unsigned)c_);
    for (i = 0; i < 3; i++) {            /* repeat: VIC context reused    */
        ef_desc(&d, &e, ew, eh, 10 + i);
        REQUIRE(ef_ok(c, &d, 0, ew, eh));
    }
    ef_del(&e);
    return nvmpi_jpeg_encoder_close(c) == 0 ? 0 : 1;
}

/* 641 x 357: either path, the engine must see that size. */
static int jpegfd_odd(int a, int b_, int c_)
{
    nvmpictx *c = nvmpi_create_jpeg_encoder(85);
    struct efs e;
    nvmpi_fd_frame d;

    (void)b_; (void)c_;
    REQUIRE(c);
    ef_new(&e, a, 641, 357);
    ef_desc(&d, &e, 641, 357, 3);
    REQUIRE(nvmpi_jpeg_encoder_put_frame_fd(c, &d) == 0);
    REQUIRE(ef_last().width == 641 && ef_last().height == 357);
    REQUIRE(ef_packet(c, 3));
    ef_del(&e);
    return nvmpi_jpeg_encoder_close(c) == 0 ? 0 : 1;
}

/* Frames of the mock video decoder: encode, the borrowed fd stays valid and
 * registered, release afterwards. a = decoder output format. */
static int jpegfd_dec_fd2(int a, int b_, int c_)
{
    nvmpictx *dec, *c = nvmpi_create_jpeg_encoder(85);
    nvmpi_frame_release_cb rel = NULL;
    nvmpi_fd_frame fr;
    nvDecParam p;
    nvPacket pk;
    uint8_t au[4096];
    NvBufSurface *s = NULL;
    void *op = NULL;

    (void)b_; (void)c_;
    REQUIRE(c);
    memset(&p, 0, sizeof(p));
    p.codingType = NV_VIDEO_CodingH264;
    p.pixFormat = (nvPixFormat)a;
    p.frame_pool_size = 4;
    p.wait_timeout = 1000;
    dec = nvmpi_create_decoder(&p);
    REQUIRE(dec);
    memset(&pk, 0, sizeof(pk));
    pk.payload = au;
    pk.payload_size = fx_idr(au, 640, 360);
    pk.pts = 9;
    REQUIRE(nvmpi_decoder_put_packet(dec, &pk) == 0);
    memset(&fr, 0, sizeof(fr));
    fr.struct_size = sizeof(fr);
    REQUIRE(nvmpi_decoder_get_frame_fd2(dec, &fr, &rel, &op, true) == 0);
    REQUIRE(fr.timestamp == 9);
    REQUIRE(nvmpi_jpeg_encoder_put_frame_fd(c, &fr) == 0);
    REQUIRE(ef_packet(c, 9));
    REQUIRE(NvBufSurfaceFromFd(fr.fd, (void **)&s) == 0 && s);  /* still ours */
    REQUIRE(nvmpi_jpeg_encoder_put_frame_fd(c, &fr) == 0);      /* reusable   */
    REQUIRE(ef_packet(c, 9));
    rel(op);
    REQUIRE(nvmpi_decoder_close(dec) == 0);
    return nvmpi_jpeg_encoder_close(c) == 0 ? 0 : 1;
}

/* Stage a packet (pts 5) that every refused call below must leave alone. */
static nvmpictx *ef_staged(struct efs *e, int pf)
{
    nvmpictx *c = nvmpi_create_jpeg_encoder(85);
    nvmpi_fd_frame d;

    REQUIRE(c);
    ef_new(e, pf, 640, 360);
    ef_desc(&d, e, 640, 360, 5);
    REQUIRE(nvmpi_jpeg_encoder_put_frame_fd(c, &d) == 0);
    return c;
}

/* The refused call changed nothing: no encode, the packet of pts 5 is
 * still there (once). */
static int ef_kept(nvmpictx *c, unsigned calls)
{
    return ef_last().calls == calls && ef_packet(c, 5);
}

/* Dup'd, foreign, closed and -1 fds are refused, not copied or closed. */
static int jpegfd_bad_fd(int a, int b_, int c_)
{
    struct efs e;
    nvmpictx *c = ef_staged(&e, a);
    unsigned calls = ef_last().calls;
    int fds[4], i;
    nvmpi_fd_frame d;

    (void)b_; (void)c_;
    fds[0] = dup(e.fd);
    fds[1] = open("/dev/null", O_RDONLY);
    fds[2] = dup(e.fd);
    REQUIRE(fds[0] >= 0 && fds[1] >= 0 && fds[2] >= 0);
    close(fds[2]);                       /* closed: a stale number        */
    fds[3] = -1;
    for (i = 0; i < 4; i++) {
        ef_desc(&d, &e, 640, 360, 6);
        d.fd = fds[i];
        REQUIRE(nvmpi_jpeg_encoder_put_frame_fd(c, &d) == NVMPI_ERR_INVAL);
    }
    REQUIRE(fcntl(fds[0], F_GETFD) >= 0 && fcntl(fds[1], F_GETFD) >= 0);
    close(fds[0]);                       /* the library never closed them */
    close(fds[1]);
    REQUIRE(ef_kept(c, calls));
    ef_del(&e);
    return nvmpi_jpeg_encoder_close(c) == 0 ? 0 : 1;
}

/* Apply mutation i to a valid 640x360 descriptor; 0 when i is past the end
 * or does not apply to this format (np = its plane count). */
static int ef_mut(nvmpi_fd_frame *f, int i)
{
    unsigned np = f->num_planes, last = np - 1;
    nvPixFormat other = f->format == NV_PIX_NV12 ? NV_PIX_YUV420 : NV_PIX_NV12;

    switch (i) {
    case 0: f->struct_size = 0; break;
    case 1: f->struct_size = NVMPI_FD_FRAME_V1_SIZE - 1; break;
    case 2: f->width = 0; break;
    case 3: f->height = 0; break;
    case 4: f->width = 8193; break;
    case 5: f->height = 8193; break;
    case 6: f->width = 641; break;       /* wider than plane 0            */
    case 7: f->height = 361; break;
    case 8: f->format = NV_PIX_P010; break;
    case 9: f->format = other; break;    /* other 8-bit format            */
    case 10: f->num_planes = np + 1; break;
    case 11: f->num_planes = np - 1; break;
    case 12: f->num_planes = 0; break;
    case 13: f->pitch[0] += 1; break;
    case 14: f->pitch[1] += 1; break;
    case 15: f->pitch[last] += 16; break;
    case 16: f->offset[0] = 1; break;
    case 17: f->offset[1] += 1; break;
    case 18: f->offset[last] += 4096; break;
    default: return 0;
    }
    return 1;
}

/* Every wrong field is INVAL, nothing is encoded, the staged packet stays. */
static int jpegfd_bad_desc(int a, int b_, int c_)
{
    struct efs e;
    nvmpictx *c = ef_staged(&e, a);
    unsigned calls = ef_last().calls;
    nvmpi_fd_frame d;
    int i;

    (void)b_; (void)c_;
    for (i = 0; ; i++) {
        ef_desc(&d, &e, 640, 360, 6);
        if (!ef_mut(&d, i))
            break;
        REQUIRE(nvmpi_jpeg_encoder_put_frame_fd(c, &d) == NVMPI_ERR_INVAL);
    }
    REQUIRE(i == 19);
    REQUIRE(ef_kept(c, calls));
    ef_desc(&d, &e, 640, 360, 8);        /* and the encoder still works   */
    REQUIRE(nvmpi_jpeg_encoder_put_frame_fd(c, &d) == 0 && ef_packet(c, 8));
    ef_del(&e);
    return nvmpi_jpeg_encoder_close(c) == 0 ? 0 : 1;
}

static int jpegfd_eos(int a, int b_, int c_)
{
    struct efs e;
    nvmpictx *c = ef_staged(&e, a);
    unsigned calls = ef_last().calls;
    nvmpi_fd_frame d;
    nvPacket p;

    (void)b_; (void)c_;
    REQUIRE(nvmpi_jpeg_encoder_put_frame_fd(NULL, NULL) == -1);
    ef_desc(&d, &e, 640, 360, 6);
    REQUIRE(nvmpi_jpeg_encoder_put_frame_fd(NULL, &d) == -1);
    REQUIRE(nvmpi_jpeg_encoder_put_frame_fd(c, NULL) == 0);       /* EOS  */
    REQUIRE(nvmpi_jpeg_encoder_put_frame_fd(c, &d) == -1);        /* after */
    REQUIRE(ef_last().calls == calls);
    REQUIRE(ef_packet(c, 5));            /* the staged one is still served */
    memset(&p, 0, sizeof(p));
    REQUIRE(nvmpi_jpeg_encoder_get_packet(c, &p) == -2);
    ef_del(&e);
    return nvmpi_jpeg_encoder_close(c) == 0 ? 0 : 1;
}

#endif /* NVMPI_TEST_JPEG_MOCK_ENCFD_H */
