/*
 * av1_split.h - cut the AV1 fixture temporal unit into the packet shapes of
 * issue #147 (no in-band sequence header). Header-only, allocation free;
 * shared by the mock case (dec_mock_av1_seq.h) and the hardware case
 * (av1_noseq_case.h). Needs av1_fixture.h and av1_obu.h included first.
 *
 * The fixture is TD + sequence header + frame OBU (low-overhead format, every
 * OBU has a size field). The walker below is bounds-checked: the bytes are a
 * compiled-in constant here, but the same code must stay safe if the fixture
 * is ever regenerated into something unexpected (it then fails the setup
 * instead of reading past the array).
 */
#ifndef NVMPI_TEST_AV1_SPLIT_H
#define NVMPI_TEST_AV1_SPLIT_H

#include <string.h>

/* Byte range of one OBU (header + size field + payload) inside the unit. */
struct av1_span {
    size_t off, len;                   /* len 0 = not present               */
};

struct av1_parts {
    struct av1_span td, seq, frame;    /* first OBU of each kind            */
};

/* Packet shapes. Bit values double as the build mask. */
#define AV1_P_TD    1u
#define AV1_P_SEQ   2u
#define AV1_P_FRAME 4u

enum av1_pkt {
    AV1_PKT_TD,        /* temporal delimiter only                           */
    AV1_PKT_SEQ,       /* sequence header only                              */
    AV1_PKT_NOSEQ,     /* TD + key frame, sequence header stripped          */
    AV1_PKT_FULL,      /* the fixture as is (TD + sequence header + frame)  */
    AV1_PKT_N
};

#define AV1_PKT_CAP 2048u              /* fixture is ~1.2 KB                */

static uint8_t av1_pkt_buf[AV1_PKT_N][AV1_PKT_CAP];
static size_t av1_pkt_len[AV1_PKT_N];

/* Locate the first TD / sequence header / frame OBU. Returns 1 when all three
 * exist and every OBU fits the buffer, else 0. */
static inline int av1_split_tu(const uint8_t *p, size_t n, struct av1_parts *pt)
{
    size_t o = 0;

    memset(pt, 0, sizeof(*pt));
    while (o < n) {
        size_t start = o, sz = 0;
        unsigned type, sh = 0;
        uint8_t h = p[o++];
        struct av1_span *sp = NULL;

        /* Same layout rule as av1_parse_tu(): no extension, size field set */
        if ((h & 0x80) || (h & 0x04) || !(h & 0x02))
            return 0;
        type = (h >> 3) & 0x0f;
        for (;;) {                     /* leb128, at most 4 bytes           */
            uint8_t b;

            if (o >= n || sh > 21)
                return 0;
            b = p[o++];
            sz |= (size_t)(b & 0x7f) << sh;
            sh += 7;
            if (!(b & 0x80))
                break;
        }
        if (sz > n - o)                /* payload past the end: truncated   */
            return 0;
        o += sz;
        sp = type == 2 ? &pt->td : type == 1 ? &pt->seq :
             type == 6 ? &pt->frame : NULL;
        if (sp && !sp->len) {
            sp->off = start;
            sp->len = o - start;
        }
    }
    return pt->td.len && pt->seq.len && pt->frame.len;
}

/* Concatenate the selected OBUs (TD, SEQ, FRAME order) into dst. Returns the
 * length, 0 when it does not fit. */
static inline size_t av1_pkt_make(uint8_t *dst, size_t cap, const uint8_t *tu,
                                  const struct av1_parts *pt, unsigned mask)
{
    const struct av1_span *sp[3] = { &pt->td, &pt->seq, &pt->frame };
    size_t len = 0;
    unsigned i;

    for (i = 0; i < 3; i++) {
        if (!(mask & (1u << i)))
            continue;
        if (sp[i]->len > cap - len)
            return 0;
        memcpy(dst + len, tu + sp[i]->off, sp[i]->len);
        len += sp[i]->len;
    }
    return len;
}

/* Build all four packets from the fixture and check their shape with the
 * independent OBU parser. Returns 0 on success. */
static int av1_pkts_init(void)
{
    static const unsigned mask[AV1_PKT_N] = {
        [AV1_PKT_TD] = AV1_P_TD, [AV1_PKT_SEQ] = AV1_P_SEQ,
        [AV1_PKT_NOSEQ] = AV1_P_TD | AV1_P_FRAME,
        [AV1_PKT_FULL] = AV1_P_TD | AV1_P_SEQ | AV1_P_FRAME,
    };
    struct av1_parts pt;
    struct av1_tu t;
    int i;

    if (!av1_split_tu(av1_test_tu, av1_test_tu_len, &pt))
        return 1;
    for (i = 0; i < AV1_PKT_N; i++) {
        av1_pkt_len[i] = av1_pkt_make(av1_pkt_buf[i], AV1_PKT_CAP,
                                      av1_test_tu, &pt, mask[i]);
        if (!av1_pkt_len[i])
            return 1;
    }
    /* Shapes: the stripped unit must still be a valid shown key frame, so a
     * decoder that lacks only the header is the sole variable. */
    av1_parse_tu(av1_pkt_buf[AV1_PKT_TD], av1_pkt_len[AV1_PKT_TD], &t);
    if (!t.ok || t.n_td != 1 || t.n_seq || t.n_frame)
        return 1;
    av1_parse_tu(av1_pkt_buf[AV1_PKT_SEQ], av1_pkt_len[AV1_PKT_SEQ], &t);
    if (!t.ok || t.n_seq != 1 || t.n_td || t.n_frame)
        return 1;
    av1_parse_tu(av1_pkt_buf[AV1_PKT_NOSEQ], av1_pkt_len[AV1_PKT_NOSEQ], &t);
    if (!t.ok || t.n_td != 1 || t.n_seq || t.n_frame != 1 || !t.key)
        return 1;
    av1_parse_tu(av1_pkt_buf[AV1_PKT_FULL], av1_pkt_len[AV1_PKT_FULL], &t);
    if (!t.ok || t.n_td != 1 || t.n_seq != 1 || t.n_frame != 1 || !t.key ||
        av1_pkt_len[AV1_PKT_FULL] != av1_test_tu_len)
        return 1;
    return 0;
}

#endif /* NVMPI_TEST_AV1_SPLIT_H */
