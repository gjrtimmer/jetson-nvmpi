/*
 * dec_mock_copysize.h - get_frame / get_frame2 against caller buffers that
 * are too small (issue #149). Cases of test_api_dec_mock.c, included after
 * dec_mock_reschange.h (reuses its 320x240 -> 640x360 burst and helpers).
 *
 * Every plane is its own exact-size malloc with linesize == row, so under
 * ASan a copy of the larger frame is a heap-buffer-overflow (that is what
 * get_frame did before the fix). Cases that refuse frames log libnvmpi
 * errors on purpose: k_cases marks them as fault cases ("dqbuf@9999" never
 * fires, it only relaxes the strict-log check, like the av1 case).
 */
#ifndef NVMPI_TEST_DEC_MOCK_COPYSIZE_H
#define NVMPI_TEST_DEC_MOCK_COPYSIZE_H

#define CS_PAT 0xA5     /* fill byte: proves a refused call wrote nothing */

/* NV12 planes of one size, each an exact-size allocation. */
struct cs_buf {
    unsigned char *p[2];
    size_t cap[2];
    unsigned w, h;
};

static void cs_alloc(struct cs_buf *b, unsigned w, unsigned h)
{
    int i;

    b->w = w;
    b->h = h;
    b->cap[0] = (size_t)w * h;
    b->cap[1] = (size_t)w * (h / 2);
    for (i = 0; i < 2; i++) {
        b->p[i] = malloc(b->cap[i]);
        REQUIRE(b->p[i]);
        memset(b->p[i], CS_PAT, b->cap[i]);
    }
}

static void cs_fill(const struct cs_buf *b)
{
    memset(b->p[0], CS_PAT, b->cap[0]);
    memset(b->p[1], CS_PAT, b->cap[1]);
}

static void cs_free(struct cs_buf *b)
{
    free(b->p[0]);
    free(b->p[1]);
    b->p[0] = b->p[1] = NULL;
}

static int cs_untouched(const struct cs_buf *b)
{
    size_t i;
    int pl;

    for (pl = 0; pl < 2; pl++)
        for (i = 0; i < b->cap[pl]; i++)
            if (b->p[pl][i] != CS_PAT)
                return 0;
    return 1;
}

/* nvFrame over b: linesize == row (w), capacity given when `caps`. */
static void cs_frame(nvFrame *f, const struct cs_buf *b, int caps)
{
    memset(f, 0, sizeof(*f));
    f->payload[0] = b->p[0];
    f->payload[1] = b->p[1];
    f->linesize[0] = f->linesize[1] = b->w;
    if (caps) {
        f->payload_size[0] = b->cap[0];
        f->payload_size[1] = b->cap[1];
    }
}

/* (a) Legacy get_frame, buffers sized for the first size (create hint), no
 * payload_size. Old-size frames copy; every new-size frame is refused with
 * NVMPI_ERR_FRAME_SIZE and dropped; the buffers never see a byte past their
 * size (ASan). Pre-fix: heap-buffer-overflow on the first new-size frame. */
static int copysize_legacy_drop(void)
{
    nvmpictx *c = create(RC_OLD_W, RC_OLD_H, 4);
    struct cs_buf b;
    nvFrame f;
    int old_got = 0, refused = 0, r;

    REQUIRE(c);
    cs_alloc(&b, RC_OLD_W, RC_OLD_H);
    REQUIRE(rc_put_burst(c) == 0);
    while (refused < RC_NEW_AUS) {
        cs_frame(&f, &b, 0);
        r = nvmpi_decoder_get_frame(c, &f, true);
        if (r == 0) {
            REQUIRE(refused == 0);           /* old size only before the change */
            old_got++;
        } else {
            REQUIRE(r == NVMPI_ERR_FRAME_SIZE);
            refused++;
        }
    }
    fprintf(stderr, "    old copied %d, new refused %d\n", old_got, refused);
    REQUIRE(nvmpi_decoder_get_frame(c, &f, false) == -1);   /* nothing held */
    cs_free(&b);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* Legacy get_frame with a declared capacity: same fit rule as get_frame2. */
static int copysize_legacy_caps(void)
{
    nvmpictx *c = create(0, 0, 4);
    struct cs_buf b;
    nvFrame f;
    uint8_t au[4096];

    REQUIRE(c);
    cs_alloc(&b, 320, 240);
    b.cap[1] -= 1;                           /* UV plane one byte short */
    REQUIRE(put(c, au, fx_idr(au, 320, 240), 7) == 0);
    cs_frame(&f, &b, 1);
    REQUIRE(nvmpi_decoder_get_frame(c, &f, true) == NVMPI_ERR_FRAME_SIZE);
    REQUIRE(cs_untouched(&b));
    cs_free(&b);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* (b) get_frame2: the first new-size frame is refused with its size, the
 * caller reallocates and the SAME frame (timestamp 100) is delivered, then
 * the rest in order: all RC_NEW_AUS new-size frames arrive. */
static int copysize_get_frame2_realloc(void)
{
    nvmpictx *c = create(RC_OLD_W, RC_OLD_H, 4);
    struct cs_buf b;
    nvFrame f;
    int new_got = 0, refused = 0, r;

    REQUIRE(c);
    cs_alloc(&b, RC_OLD_W, RC_OLD_H);
    REQUIRE(rc_put_burst(c) == 0);
    while (new_got < RC_NEW_AUS) {
        cs_fill(&b);                         /* earlier copies overwrote it */
        cs_frame(&f, &b, 1);
        r = nvmpi_decoder_get_frame2(c, &f, true);
        REQUIRE(r == 0 || r == NVMPI_ERR_FRAME_SIZE);
        if (r == 0) {
            if (f.width == RC_OLD_W) {       /* old frame, before the change */
                REQUIRE(new_got == 0 && f.height == RC_OLD_H);
                REQUIRE(f.payload_size[0] == b.cap[0] &&
                        f.payload_size[1] == b.cap[1]);
                continue;
            }
            REQUIRE(f.width == RC_NEW_W && f.height == RC_NEW_H);
            REQUIRE((long)f.timestamp == 100 + new_got);   /* none lost */
            new_got++;
            continue;
        }
        refused++;
        REQUIRE(refused == 1);               /* only before the realloc */
        REQUIRE(f.width == RC_NEW_W && f.height == RC_NEW_H);
        REQUIRE(f.payload_size[0] == (size_t)RC_NEW_W * RC_NEW_H);
        REQUIRE(f.payload_size[1] == (size_t)RC_NEW_W * (RC_NEW_H / 2));
        REQUIRE(cs_untouched(&b));           /* refusal wrote nothing */
        cs_free(&b);
        cs_alloc(&b, f.width, f.height);     /* realloc to the reported size */
    }
    REQUIRE(refused == 1);
    cs_free(&b);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* get_frame2 argument checks consume no frame; a stride below one row is a
 * refusal (frame kept), not a copy. */
static int copysize_args_and_stride(void)
{
    nvmpictx *c = create(0, 0, 4);
    struct cs_buf b;
    nvFrame f;
    uint8_t au[4096];

    REQUIRE(c);
    cs_alloc(&b, 320, 240);
    REQUIRE(put(c, au, fx_idr(au, 320, 240), 9) == 0);

    REQUIRE(nvmpi_decoder_get_frame2(NULL, &f, false) == NVMPI_ERR_INVAL);
    REQUIRE(nvmpi_decoder_get_frame2(c, NULL, false) == NVMPI_ERR_INVAL);
    cs_frame(&f, &b, 0);                     /* no capacity */
    REQUIRE(nvmpi_decoder_get_frame2(c, &f, true) == NVMPI_ERR_INVAL);
    cs_frame(&f, &b, 1);
    f.payload_size[1] = 0;                   /* one plane without capacity */
    REQUIRE(nvmpi_decoder_get_frame2(c, &f, true) == NVMPI_ERR_INVAL);
    cs_frame(&f, &b, 1);
    f.payload[1] = NULL;
    REQUIRE(nvmpi_decoder_get_frame2(c, &f, true) == NVMPI_ERR_INVAL);

    cs_frame(&f, &b, 1);                     /* stride 319 < 320 bytes/row */
    f.linesize[0] = 319;
    REQUIRE(nvmpi_decoder_get_frame2(c, &f, true) == NVMPI_ERR_FRAME_SIZE);
    REQUIRE(f.width == 320 && f.height == 240);
    REQUIRE(cs_untouched(&b));

    cs_frame(&f, &b, 1);                     /* the frame was never lost */
    REQUIRE(nvmpi_decoder_get_frame2(c, &f, true) == 0);
    REQUIRE((long)f.timestamp == 9);
    REQUIRE(nvmpi_decoder_get_frame2(c, &f, false) == -1);
    cs_free(&b);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* Legacy get_frame with linesize < row: refused and dropped, nothing written. */
static int copysize_legacy_stride(void)
{
    nvmpictx *c = create(0, 0, 4);
    struct cs_buf b;
    nvFrame f;
    uint8_t au[4096];

    REQUIRE(c);
    cs_alloc(&b, 320, 240);
    REQUIRE(put(c, au, fx_idr(au, 320, 240), 3) == 0);
    cs_frame(&f, &b, 0);
    f.linesize[0] = 200;
    REQUIRE(nvmpi_decoder_get_frame(c, &f, true) == NVMPI_ERR_FRAME_SIZE);
    REQUIRE(cs_untouched(&b));
    REQUIRE(nvmpi_decoder_get_frame(c, &f, false) == -1);   /* dropped */
    cs_free(&b);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* Hold a refused new-size frame: the next get_frame_fd delivers it first. */
static int copysize_pending_fd(void)
{
    nvmpictx *c = create(RC_OLD_W, RC_OLD_H, 4);
    struct cs_buf b;
    nvFrame f;
    int w = 0, h = 0, r;

    REQUIRE(c);
    cs_alloc(&b, RC_OLD_W, RC_OLD_H);
    REQUIRE(rc_put_burst(c) == 0);
    do {
        cs_frame(&f, &b, 1);
        r = nvmpi_decoder_get_frame2(c, &f, true);
        REQUIRE(r == 0 || r == NVMPI_ERR_FRAME_SIZE);
    } while (r == 0);
    REQUIRE(take_fd(c, &w, &h) == 0);        /* the held frame, no copy */
    REQUIRE(w == RC_NEW_W && h == RC_NEW_H);
    cs_free(&b);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

/* Refused frame still held at close: no leak (ASan) and no race (TSan). */
static int copysize_pending_close(void)
{
    nvmpictx *c = create(RC_OLD_W, RC_OLD_H, 4);
    struct cs_buf b;
    nvFrame f;
    int r;

    REQUIRE(c);
    cs_alloc(&b, RC_OLD_W, RC_OLD_H);
    REQUIRE(rc_put_burst(c) == 0);
    do {
        cs_frame(&f, &b, 1);
        r = nvmpi_decoder_get_frame2(c, &f, true);
        REQUIRE(r == 0 || r == NVMPI_ERR_FRAME_SIZE);
    } while (r == 0);
    cs_free(&b);
    REQUIRE(nvmpi_decoder_close(c) == 0);    /* pending released here */
    return 0;
}

/* Refused frame still held at flush: discarded, decoding continues. */
static int copysize_pending_flush(void)
{
    nvmpictx *c = create(RC_OLD_W, RC_OLD_H, 4);
    struct cs_buf b;
    nvFrame f;
    uint8_t au[4096];
    int r;

    REQUIRE(c);
    cs_alloc(&b, RC_OLD_W, RC_OLD_H);
    REQUIRE(rc_put_burst(c) == 0);
    do {
        cs_frame(&f, &b, 1);
        r = nvmpi_decoder_get_frame2(c, &f, true);
        REQUIRE(r == 0 || r == NVMPI_ERR_FRAME_SIZE);
    } while (r == 0);
    REQUIRE(nvmpi_decoder_flush(c) == 0);
    REQUIRE(put(c, au, fx_idr(au, RC_OLD_W, RC_OLD_H), 500) == 0);
    cs_frame(&f, &b, 1);
    REQUIRE(nvmpi_decoder_get_frame2(c, &f, true) == 0);
    REQUIRE((long)f.timestamp == 500 && f.width == RC_OLD_W);
    cs_free(&b);
    REQUIRE(nvmpi_decoder_close(c) == 0);
    return 0;
}

#endif /* NVMPI_TEST_DEC_MOCK_COPYSIZE_H */
