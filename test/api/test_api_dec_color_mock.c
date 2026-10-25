/*
 * test_api_dec_color_mock.c - decoder output colour tag against the mock
 * model (issue #163). Mock-only; the hardware counterpart is dec_color_case.h.
 *
 * The decoder's CAPTURE surfaces carry the stream's colorimetry and range
 * (the mock reports them via NVMPI_MOCK_DEC_COLORSPACE / _FULL_RANGE). The
 * pitch-linear destination must carry the same tag, because the real VIC
 * runs a matrix/range conversion when the tags differ and the output is then
 * no longer bit-exact. The mock transform fails such a copy when
 * NVMPI_MOCK_XFORM_STRICT_COLOR=1, so a frame only arrives when the tags
 * match; the cases also read the tag of the delivered surface and the
 * FULL_RANGE flag of get_frame_fd2.
 */
#include "case_log.h"
#include <nvmpi.h>
#include <nvbufsurface.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../mock/h264_fixture.h"

#define CASE_TIMEOUT_S 30
#define COLOR_W 320
#define COLOR_H 240
#define COLOR_FRAMES 3

#define REQUIRE(c)                                                        \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "REQUIRE failed %s:%d: %s\n", __FILE__,       \
                    __LINE__, #c);                                        \
            _exit(10 + (__LINE__ % 200));                                 \
        }                                                                 \
    } while (0)

struct ccase {
    const char *name;
    const char *space;                   /* NVMPI_MOCK_DEC_COLORSPACE      */
    int full;                            /* NVMPI_MOCK_DEC_FULL_RANGE      */
    nvPixFormat pf;
    int hint;                            /* container size hint (pre-allocated pool) */
    int want_nv12, want_i420;            /* expected NVBUF_COLOR_FORMAT_*  */
};

static nvmpictx *create(nvPixFormat pf, int hint)
{
    nvDecParam p;

    memset(&p, 0, sizeof(p));
    p.codingType = NV_VIDEO_CodingH264;
    p.pixFormat = pf;
    p.frame_pool_size = 4;
    p.wait_timeout = 1000;
    if (hint) {
        p.width = COLOR_W;
        p.height = COLOR_H;
    }
    return nvmpi_create_decoder(&p);
}

static int put(nvmpictx *c, const uint8_t *au, size_t n, unsigned long pts)
{
    nvPacket pkt;

    memset(&pkt, 0, sizeof(pkt));
    pkt.payload = (unsigned char *)au;
    pkt.payload_size = n;
    pkt.pts = pts;
    return nvmpi_decoder_put_packet(c, &pkt);
}

static int color_case(void *arg)
{
    const struct ccase *t = arg;
    const int want = t->pf == NV_PIX_NV12 ? t->want_nv12 : t->want_i420;
    uint8_t au[4096];
    nvmpictx *c;
    int i;

    setenv("NVMPI_MOCK_DEC_COLORSPACE", t->space, 1);
    setenv("NVMPI_MOCK_DEC_FULL_RANGE", t->full ? "1" : "0", 1);
    setenv("NVMPI_MOCK_XFORM_STRICT_COLOR", "1", 1);
    c = create(t->pf, t->hint);
    REQUIRE(c);
    for (i = 0; i < COLOR_FRAMES; i++) {
        nvmpi_fd_frame fr;
        nvmpi_frame_release_cb rel = NULL;
        void *op = NULL;
        NvBufSurface *s = NULL;

        REQUIRE(put(c, au, i ? fx_pslice(au) : fx_idr(au, COLOR_W, COLOR_H),
                    1 + i) == 0);
        memset(&fr, 0, sizeof(fr));
        fr.struct_size = sizeof(fr);
        REQUIRE(nvmpi_decoder_get_frame_fd2(c, &fr, &rel, &op, true) == 0);
        REQUIRE(fr.format == t->pf);     /* callers still see NV12 / YUV420 */
        REQUIRE(NvBufSurfaceFromFd(fr.fd, (void **)&s) == 0 && s);
        REQUIRE((int)s->surfaceList[0].colorFormat == want);
        REQUIRE(((fr.flags & NVMPI_FD_FRAME_FULL_RANGE) != 0) == (t->full != 0));
        if (rel)
            rel(op);
    }
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

#define N12(x) NVBUF_COLOR_FORMAT_NV12##x
#define I420(x) NVBUF_COLOR_FORMAT_YUV420##x
#define PAIR(x) N12(x), I420(x)
#define CASES(sfx, pf)                                                              \
    { "bt601_limited" sfx, "601", 0, pf, 0, PAIR() },                               \
    { "bt601_full" sfx, "601", 1, pf, 0, PAIR(_ER) },                               \
    { "bt709_limited" sfx, "709", 0, pf, 0, PAIR(_709) },                           \
    { "bt709_full" sfx, "709", 1, pf, 0, PAIR(_709_ER) },                           \
    { "bt2020" sfx, "2020", 0, pf, 0, PAIR(_2020) }

static const struct ccase k_cases[] = {
    CASES("_nv12", NV_PIX_NV12),
    CASES("_yuv420", NV_PIX_YUV420),
    /* pool pre-allocated from container hints (BT.601 limited) must be
     * rebuilt when the stream turns out to be BT.709 / full range */
    { "hint_bt709_nv12", "709", 0, NV_PIX_NV12, 1, PAIR(_709) },
    { "hint_full_yuv420", "601", 1, NV_PIX_YUV420, 1, PAIR(_ER) },
};

int main(int argc, char **argv)
{
    size_t i;
    int fails = 0, ran = 0;

    printf("=== libnvmpi decoder colour tag vs mock model ===\n");
    for (i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        if (argc > 1 && strstr(k_cases[i].name, argv[1]) == NULL)
            continue;
        fails += case_log_run(k_cases[i].name, NULL, 1, 0, CASE_TIMEOUT_S,
                              color_case, (void *)&k_cases[i]);
        ran++;
    }
    printf("\n%d cases, %d failed\n", ran, fails);
    return fails ? 1 : 0;
}
