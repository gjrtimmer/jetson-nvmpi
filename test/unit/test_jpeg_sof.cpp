/*
 * test_jpeg_sof.cpp — host-only unit test of the JPEG SOF scanner used by the
 * DMA-BUF JPEG decode mode (src/nvmpi_jpeg_sof.cpp). No hardware, no libnvmpi.
 */
#include "nvmpi_jpeg_sof.h"
#include "../api/jpeg_fd_data.h"   /* 33x17 baseline 4:2:0 fixture */
#include "jpeg_sof_fixtures.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef std::vector<unsigned char> Buf;

static Buf mk(const unsigned char *p, size_t n) { return Buf(p, p + n); }

/* Index of the first FF <m> pair, or -1. */
static long find_marker(const Buf &b, unsigned char m)
{
    for (size_t i = 0; i + 1 < b.size(); i++)
        if (b[i] == 0xFF && b[i + 1] == m) return (long)i;
    return -1;
}

static nvmpi_jpeg_sof_result scan(const Buf &b, unsigned *c = NULL, unsigned *w = NULL, unsigned *h = NULL)
{
    return nvmpi_jpeg_scan_sof(b.empty() ? NULL : &b[0], b.size(), c, w, h);
}

int main(void)
{
    Buf base = mk(jpeg_fd_test_jpg, jpeg_fd_test_jpg_len);

    /* The fixture: 3 components, 33x17 (width high byte is 0, the byte the
     * original off-by-two bug read as Nf). */
    unsigned c = 99, w = 0, h = 0;
    CHECK(scan(base, &c, &w, &h) == NVMPI_SOF_OK);
    CHECK(c == 3 && w == 33 && h == 17);

    /* Progressive and grayscale are rejected with their own reasons. */
    CHECK(scan(mk(sof_prog_jpg, sof_prog_jpg_len)) == NVMPI_SOF_PROGRESSIVE);
    c = 99;
    CHECK(scan(mk(sof_gray_jpg, sof_gray_jpg_len), &c) == NVMPI_SOF_COMPONENTS);
    CHECK(c == 1);

    /* Fill bytes (extra 0xFF) before markers are legal. */
    {
        Buf b = base;
        long i = find_marker(b, 0xDB);
        CHECK(i > 0);
        b.insert(b.begin() + i, 3, 0xFF);
        long s = find_marker(b, 0xC0);
        CHECK(s > 0);
        b.insert(b.begin() + s, 5, 0xFF);
        CHECK(scan(b, &c, &w, &h) == NVMPI_SOF_OK && c == 3 && w == 33 && h == 17);
    }

    /* DHT (C4), JPG (C8) and DAC (CC) are not SOFs: a stream whose only
     * C4 segment precedes SOF must still find the real SOF (the fixture has
     * DHT before SOS but after SOF0; build DHT-first by moving nothing and
     * checking C4 is skipped when it comes first). */
    {
        Buf b;
        const unsigned char soi[] = {0xFF, 0xD8, 0xFF, 0xC4, 0x00, 0x03, 0x00};
        b.assign(soi, soi + sizeof(soi));
        long s = find_marker(base, 0xC0);
        b.insert(b.end(), base.begin() + s, base.end());
        CHECK(scan(b) == NVMPI_SOF_OK);
    }

    /* Truncation anywhere never over-reads and never reports OK before the
     * SOF's Nf byte is present. ASan catches an over-read. */
    {
        long s = find_marker(base, 0xC0);
        for (size_t n = 0; n < (size_t)s + 2 + 8; n++) {
            Buf b(base.begin(), base.begin() + n);
            CHECK(scan(b) == NVMPI_SOF_MALFORMED);
        }
        Buf b(base.begin(), base.begin() + s + 2 + 8);   /* just Nf present */
        CHECK(scan(b) == NVMPI_SOF_OK);
    }

    /* Degenerate input. */
    CHECK(scan(Buf()) == NVMPI_SOF_MALFORMED);
    CHECK(nvmpi_jpeg_scan_sof(NULL, 10, NULL, NULL, NULL) == NVMPI_SOF_MALFORMED);
    {
        const unsigned char junk[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x01, 0xFF};   /* len < 2 */
        CHECK(scan(mk(junk, sizeof(junk))) == NVMPI_SOF_MALFORMED);
    }

    std::printf("test_jpeg_sof: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
