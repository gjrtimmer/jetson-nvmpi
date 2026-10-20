/*
 * fd_snap.h - snapshot of this process's open descriptors with their
 * /proc/self/fd targets (#137). A count alone ("fds 11 -> 48") cannot tell a
 * leaked buffer from a descriptor an NVIDIA library opens lazily on first
 * use; the targets can (dmabuf:, /dev/nvmap, anon_inode:..., /dev/v4l2-...,
 * memfd: in the mock). Header-only, shared by test_api.c and the mock suites.
 */
#ifndef NVMPI_TEST_FD_SNAP_H
#define NVMPI_TEST_FD_SNAP_H

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FDS_MAX 1024                     /* descriptors above are not tracked */
#define FDS_TGT 96

struct fd_snap {
    char t[FDS_MAX][FDS_TGT];            /* target per fd, "" = not open   */
    int n;                               /* open descriptors               */
};

/* Walks fd numbers instead of opendir(): no descriptor of its own. */
static void fds_take(struct fd_snap *s)
{
    int fd;

    memset(s, 0, sizeof(*s));
    for (fd = 0; fd < FDS_MAX; fd++) {
        char path[32];
        ssize_t r;

        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        r = readlink(path, s->t[fd], FDS_TGT - 1);
        if (r < 0) {
            s->t[fd][0] = '\0';
            continue;
        }
        s->t[fd][r] = '\0';
        s->n++;
    }
}

/* Compare against `base`; `expect_new` descriptors may be open beyond it
 * (borrowed buffers that legitimately outlive close). Silent when the net
 * number of new descriptors equals expect_new and nothing was retargeted;
 * otherwise prints every difference as "fd N: <target>" (opened, gone,
 * retargeted) plus a summary under `label`. Returns the difference count that
 * is NOT explained by expect_new (0 = ok). */
static int fds_check(const struct fd_snap *base, int expect_new,
                     const char *label)
{
    struct fd_snap now;
    int fd, opened = 0, closed = 0, retarget = 0;

    fds_take(&now);
    for (fd = 0; fd < FDS_MAX; fd++) {
        if (strcmp(base->t[fd], now.t[fd]) == 0)
            continue;
        if (now.t[fd][0] && !base->t[fd][0])
            opened++;
        else if (!now.t[fd][0])
            closed++;
        else
            retarget++;
    }
    if (opened - closed == expect_new && !retarget)
        return 0;
    for (fd = 0; fd < FDS_MAX; fd++) {
        if (strcmp(base->t[fd], now.t[fd]) == 0)
            continue;
        if (now.t[fd][0] && !base->t[fd][0])
            fprintf(stderr, "  [%s] new fd %d -> %s\n", label, fd, now.t[fd]);
        else if (!now.t[fd][0])
            fprintf(stderr, "  [%s] gone fd %d (was %s)\n", label, fd,
                    base->t[fd]);
        else
            fprintf(stderr, "  [%s] fd %d retargeted %s -> %s\n", label, fd,
                    base->t[fd], now.t[fd]);
    }
    fprintf(stderr, "  [%s] fds %d -> %d (%d new, %d gone, %d retargeted, "
            "%d expected)\n", label, base->n, now.n, opened, closed, retarget,
            expect_new);
    return opened - closed - expect_new ? opened - closed - expect_new
                                         : retarget;
}

#endif /* NVMPI_TEST_FD_SNAP_H */
