/*
 * enc_odd_case.h - hardware case for the chroma of odd frame sizes (#154),
 * included by test_api.c. Encodes 10 identical 641x357 frames (321x179
 * chroma) as H.264 with a sentinel in the last chroma column and row
 * (enc_odd_fixture.h), feeds the packets one by one to the libnvmpi decoder
 * (as enc_p010_rt.h does; a single-frame stream is not decoded by NVDEC), and
 * compares the chroma of the first decoded frame (the stream is the
 * even-rounded 642x358, 4:2:0 cannot signal odd sizes; the encoder pads the
 * last luma column/row and takes the chroma as given, #154):
 *
 *   odd_chroma probe: fmt=NV12 psnr_u=.. psnr_v=.. lastcol=ok|bad
 *
 * lastcol is "ok" when at most ODD_EDGE_LOST_MAX of the last-column/last-row
 * samples stay at their interior neighbour's value (moved less than a quarter
 * of the way to the source sentinel): an encoder that ignores the edge
 * replicates the interior there, while lossy coding only blurs a one-sample
 * sentinel (edge_diff is printed too). Strict
 * mode (NVMPI_TEST_STRICT=1) fails on "bad"; outside it the probe line is the
 * result. The encoder output of YUV420 vs NV12 input is compared the same way.
 * The mock cannot run this (its decoder does not reproduce pixels): the
 * encoder mock suite covers the same input in enc_mock_oddchroma.h.
 */
#ifndef NVMPI_TEST_ENC_ODD_CASE_H
#define NVMPI_TEST_ENC_ODD_CASE_H

#include "../mock/nal_scan.h"
#include "enc_odd_fixture.h"

#define ODD_EDGE_LOST_MAX 0.10            /* share of edge samples lost    */
#define ODD_POOL 8
#define ODD_FRAMES 10

#ifndef NVMPI_MOCK_JPEG                   /* hardware only, see above */

/* Every failure prints the probe line with what is known, then a reason. */
#define ODD_FAIL(name, why, extra) \
    do { \
        printf("odd_chroma probe: fmt=%s FAIL reason=%s %s\n", (name), (why), (extra)); \
        fflush(stdout); \
    } while (0)

/* put_frame_fd feed of the DMA-BUF variant (#156), enc_odd_fd_case.h:
 * allocate + fill the odd-size surface, submit it (so odd_encode serves both
 * the memory and the fd input). */
static int odd_fd_surface(int *fd);
static int odd_fd_feed(nvmpictx *enc, int fd, long pts);
static int odd_fd_full_chroma;          /* set by odd_fd_surface: ceil chroma held */

/* The coded stream: one packet per frame, kept apart so they can be fed to
 * the decoder one by one the way enc_p010_rt.h does. */
struct odd_stream {
    unsigned char *d[ODD_FRAMES];
    size_t n[ODD_FRAMES];
    int count;
};

static void odd_stream_free(struct odd_stream *s)
{
    for (int i = 0; i < ODD_FRAMES; i++) {
        free(s->d[i]);
        s->d[i] = NULL;
    }
    s->count = 0;
}

/* via_fd: the frames go in through put_frame_fd (use_dmabuf=1) from one
 * odd-size NV12 surface instead of put_frame (#156).
 * Encode ODD_FRAMES identical-pattern frames, one packet awaited per frame
 * (a single-frame stream is not enough for the hardware decoder, which holds
 * its only frame, so the stream mirrors the working hevc round trips), then
 * EOS and drain. 0 ok, 77 skip (encoder create failed), 1 fail (printed). */
static int odd_encode(nvPixFormat pix, const char *name, struct odd_stream *st, int via_fd)
{
    nvEncParam ep;
    nvmpictx *enc;
    nvPacket pool[ODD_POOL], *g, *tmp;
    struct oc_pic p;
    char x[48];
    int rc = 1, i, tries, r, fd = -1;

    memset(st, 0, sizeof(*st));
    memset(pool, 0, sizeof(pool));
    memset(&ep, 0, sizeof(ep));
    ep.width = OC_W;
    ep.height = OC_H;
    ep.level = 41;
    ep.bitrate = 8000000;
    ep.pixFormat = pix;
    ep.iframe_interval = 5;
    ep.idr_interval = 5;
    ep.insert_spspps_idr = 1;
    ep.fps_n = 30;
    ep.fps_d = 1;
    ep.capture_num = 4;
    ep.hw_preset_type = 1;
    ep.codingType = NV_VIDEO_CodingH264;
    ep.wait_timeout = 2000;
    ep.use_dmabuf = via_fd;
    enc = nvmpi_create_encoder(&ep);
    if (!enc)
        return 77;
    for (i = 0; i < ODD_POOL; i++) {
        pool[i].payload = calloc(1, NVMPI_ENC_CHUNK_SIZE);
        if (!pool[i].payload) {
            ODD_FAIL(name, "alloc_pool", "");
            goto out;
        }
        nvmpi_encoder_qEmptyPacket(enc, &pool[i]);
    }
    if (via_fd) {
        r = odd_fd_surface(&fd);
        if (r == 77) {            /* no nvbufsurface.h: cannot run, not a failure */
            rc = 77;
            goto out;
        }
        if (r != 0) {
            ODD_FAIL(name, "alloc_surface", "");
            goto out;
        }
    }
    for (i = 0; i < ODD_FRAMES; i++) {
        g = NULL;
        if (via_fd) {
            r = odd_fd_feed(enc, fd, 1000 + i);
        } else {
            if (oc_build(&p, pix, OC_FULL, 1000 + i) != 0) {
                ODD_FAIL(name, "alloc_frame", "");
                goto out;
            }
            r = nvmpi_encoder_put_frame(enc, &p.f);
            oc_free(&p);
        }
        if (r != 0) {
            snprintf(x, sizeof(x), "frame=%d put_frame%s=%d", i, via_fd ? "_fd" : "", r);
            ODD_FAIL(name, "put_frame", x);
            goto out;
        }
        for (tries = 0; tries < 50 && nvmpi_encoder_get_packet(enc, &g, true) != 0; tries++)
            ;
        if (!g) {
            snprintf(x, sizeof(x), "frame=%d", i);
            ODD_FAIL(name, "no_packet_from_encoder", x);
            goto out;
        }
        st->d[i] = malloc(g->payload_size);
        if (!st->d[i]) {
            ODD_FAIL(name, "alloc_au", "");
            goto out;
        }
        memcpy(st->d[i], g->payload, g->payload_size);
        st->n[i] = g->payload_size;
        st->count = i + 1;
        nvmpi_encoder_qEmptyPacket(enc, g);
    }
    if (nvmpi_encoder_put_frame(enc, NULL) != 0) {         /* EOS */
        ODD_FAIL(name, "put_eos", "");
        goto out;
    }
    for (tries = 0; tries < 1000; tries++) {                /* drain to EOS */
        r = nvmpi_encoder_get_packet(enc, &tmp, false);
        if (r == -2)
            break;
        if (r == 0)
            nvmpi_encoder_qEmptyPacket(enc, tmp);
    }
    rc = 0;
out:
    while (nvmpi_encoder_dqEmptyPacket(enc, &tmp) == 0)
        ;
    if (nvmpi_encoder_close(enc) != 0 && rc == 0) {
        ODD_FAIL(name, "encoder_close", "");
        rc = 1;
    }
    if (fd >= 0)
        nvmpi_surface_destroy(fd);
    for (i = 0; i < ODD_POOL; i++)
        free(pool[i].payload);
    if (rc)
        odd_stream_free(st);
    return rc;
}

/* Decode the stream to NV12: every packet is put, then a frame is awaited
 * (a lagging decoder just yields none yet), then EOS and a drain. The chroma
 * (top-left 321x179) of the FIRST decoded frame goes to u/v; *frames counts
 * every decoded frame. The decoder may report the coded (padded) or cropped
 * size: anything >= 641x357 is compared on its top-left corner.
 * 0 ok, 77 skip (decoder unavailable), 1 fail (reason printed). */
static int odd_decode(const struct odd_stream *st, const char *name, const char *info,
                      unsigned char *u, unsigned char *v, unsigned *dw, unsigned *dh,
                      int *frames)
{
    enum { PITCH = 1024, ROWS = 512 };
    nvDecParam dp;
    nvmpictx *dec;
    nvFrame f;
    nvPacket in;
    unsigned char *y = NULL, *uv = NULL;
    char x[160];
    int rc = 1, r, i, drain;

    *frames = 0;
    memset(&dp, 0, sizeof(dp));
    dp.codingType = NV_VIDEO_CodingH264;
    dp.pixFormat = NV_PIX_NV12;
    dp.frame_pool_size = 4;
    dp.wait_timeout = 1000;
    dec = nvmpi_create_decoder(&dp);
    if (!dec)
        return 77;
    y = calloc(PITCH, ROWS);
    uv = calloc(PITCH, ROWS);
    if (!y || !uv) {
        ODD_FAIL(name, "alloc_decode_buffers", info);
        goto out;
    }
    /* i < count: one packet each; i == count: EOS (size 0), then drain */
    for (i = 0; i <= st->count; i++) {
        memset(&in, 0, sizeof(in));
        in.payload = st->d[i < st->count ? i : 0];
        in.payload_size = i < st->count ? st->n[i] : 0;
        in.pts = 1000 + i;
        r = nvmpi_decoder_put_packet(dec, &in);
        if (r < 0 && i < st->count) {
            snprintf(x, sizeof(x), "%s packet=%d put_packet=%d frames=%d", info, i, r, *frames);
            ODD_FAIL(name, "decoder_put_packet", x);
            goto out;
        }
        for (drain = 0; drain < (i == st->count ? ODD_FRAMES : 1); drain++) {
            memset(&f, 0, sizeof(f));
            f.payload[0] = y;
            f.payload[1] = uv;
            f.linesize[0] = f.linesize[1] = PITCH;
            f.payload_size[0] = f.payload_size[1] = (unsigned long)PITCH * ROWS;
            r = nvmpi_decoder_get_frame2(dec, &f, true);
            if (r != 0) {
                if (r != -1)
                    fprintf(stderr, "odd_chroma: packet=%d get_frame2=%d w=%u h=%u\n", i, r, f.width, f.height);
                break;
            }
            if (++*frames == 1) {
                *dw = f.width;
                *dh = f.height;
                if (f.width < OC_W || f.height < OC_H) {
                    snprintf(x, sizeof(x), "%s frames=1 w=%u h=%u", info, f.width, f.height);
                    ODD_FAIL(name, "decoded_size_smaller_than_source", x);
                    goto out;
                }
                for (unsigned rr = 0; rr < OC_CH; rr++)
                    for (unsigned xx = 0; xx < OC_CW; xx++) {
                        u[rr * OC_CW + xx] = uv[(size_t)rr * PITCH + 2 * xx];
                        v[rr * OC_CW + xx] = uv[(size_t)rr * PITCH + 2 * xx + 1];
                    }
            }
        }
    }
    if (*frames == 0) {
        snprintf(x, sizeof(x), "%s packets_fed=%d frames=0 w=0 h=0", info, st->count);
        ODD_FAIL(name, "decoder_no_frame", x);
        goto out;
    }
    rc = 0;
out:
    free(y);
    free(uv);
    if (nvmpi_decoder_close(dec) != 0 && rc == 0) {
        ODD_FAIL(name, "decoder_close", info);
        rc = 1;
    }
    return rc;
}

/* Source vs decoded chroma around the last column and row (U then V): is the
 * edge a constant, a stale value or interpolated? */
static void odd_edges(const char *name, const unsigned char *u, const unsigned char *v)
{
    static const unsigned xs[4] = { OC_CW - 4, OC_CW - 3, OC_CW - 2, OC_CW - 1 };
    static const unsigned ys[4] = { OC_CH - 4, OC_CH - 3, OC_CH - 2, OC_CH - 1 };

    for (int pl = 0; pl < 2; pl++) {
        const unsigned char *d = pl ? v : u;
        char c[64] = "", r[64] = "";

        for (int k = 0; k < 4; k++) {
            snprintf(c + strlen(c), sizeof(c) - strlen(c), " %u/%u", pl ? oc_v(xs[k], 10) : oc_u(xs[k], 10),
                     d[10 * OC_CW + xs[k]]);
            snprintf(r + strlen(r), sizeof(r) - strlen(r), " %u/%u", pl ? oc_v(10, ys[k]) : oc_u(10, ys[k]),
                     d[ys[k] * OC_CW + 10]);
        }
        printf("odd_chroma edge: fmt=%s plane=%c row10_cols%u-%u(src/dec)=%s col10_rows%u-%u(src/dec)=%s\n",
               name, pl ? 'V' : 'U', xs[0], xs[3], c, ys[0], ys[3], r);
    }
    fflush(stdout);
}

static int odd_chroma_run(nvPixFormat pix, const char *name, int via_fd)
{
    static unsigned char u[OC_CW * OC_CH], v[OC_CW * OC_CH];
    struct odd_stream st;
    unsigned dw = 0, dh = 0;
    struct oc_cmp c;
    char info[96];
    int ok, frames = 0, r = odd_encode(pix, name, &st, via_fd);

    if (r == 0) {
        snprintf(info, sizeof(info), "packets=%d au0_bytes=%zu sps=%d pps=%d idr=%d", st.count,
                 st.n[0], nal_has(st.d[0], st.n[0], 0, 7), nal_has(st.d[0], st.n[0], 0, 8),
                 nal_has(st.d[0], st.n[0], 0, 5));
        r = odd_decode(&st, name, info, u, v, &dw, &dh, &frames);
        odd_stream_free(&st);
    }
    if (r == 77) {
        ODD_FAIL(name, "encoder_or_decoder_unavailable", strict_mode ? "(strict)" : "(skipped)");
        return strict_mode ? 1 : 77;
    }
    if (r)
        return r;
    oc_compare(u, v, &c);
    odd_edges(name, u, v);
    /* An fd source whose surface holds only the floored chroma (printed by
     * odd_fd_surface) has no sentinel to lose: the replicated edge equals the
     * hardware's substitution, so the edge cannot be judged then. */
    ok = c.edge_lost <= ODD_EDGE_LOST_MAX || (via_fd && !odd_fd_full_chroma);
    printf("odd_chroma probe: fmt=%s psnr_u=%.1f psnr_v=%.1f lastcol=%s edge_lost=%.2f edge_diff=%.1f dec=%ux%u frames=%d %s\n",
           name, c.psnr_u, c.psnr_v, ok ? "ok" : "bad", c.edge_lost, c.edge_diff, dw, dh, frames, info);
    fflush(stdout);        /* the case child _exit()s: unflushed output is lost */
    return (!ok && strict_mode) ? 1 : 0;
}

#endif /* !NVMPI_MOCK_JPEG */

static int test_encoder_odd_chroma(void)
{
#ifdef NVMPI_MOCK_JPEG
    return 77;                    /* mock decoder: no pixels, see header */
#else
    int a, b;

    setenv("NVMPI_ENC_TRACE", "1", 1);        /* libnvmpi prints the copy decision */
    a = odd_chroma_run(NV_PIX_NV12, "NV12", 0);
    b = odd_chroma_run(NV_PIX_YUV420, "YUV420", 0);

    if (a == 1 || b == 1)
        return 1;
    return (a == 77 && b == 77) ? 77 : 0;
#endif
}

#include "enc_odd_fd_case.h"

#endif /* NVMPI_TEST_ENC_ODD_CASE_H */
