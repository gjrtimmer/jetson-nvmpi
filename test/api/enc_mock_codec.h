/*
 * enc_mock_codec.h - unsupported-codec create case of test_api_enc_mock.c
 * (issue #134). Included from enc_mock_cases.h, after enc_mock_util.h; runs
 * inside the forked case child like every other case.
 *
 * Contract under test (include/nvmpi.h): nvmpi_create_encoder() returns NULL
 * for any codingType other than H.264/HEVC, checked before any device is
 * opened. The case runs with NVMPI_MOCK_FAULT=open@1+ (every open fails), so
 * the logged errors count as expected, and it reads its own stderr capture
 * back: the unsupported-codec error must be there, an open failure must not
 * (a create that reached the device would log it).
 */
#ifndef NVMPI_TEST_ENC_MOCK_CODEC_H
#define NVMPI_TEST_ENC_MOCK_CODEC_H

#include <fcntl.h>

static int create_unsupported_codec(void)
{
    static const nvCodingType bad[] = {
        NV_VIDEO_CodingMPEG2, NV_VIDEO_CodingVP9, NV_VIDEO_CodingMJPEG,
        NV_VIDEO_CodingUnused,
    };
    char buf[4096];
    ssize_t n;
    int fd;
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct ecfg c = { bad[i], 0, 0, 0, 0, 0 };

        REQUIRE(e_create(&c) == NULL);
    }
    /* Fresh open of the capture file (own offset) to read what was logged. */
    fd = open("/proc/self/fd/2", O_RDONLY);
    REQUIRE(fd >= 0);
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    REQUIRE(n > 0);
    buf[n] = '\0';
    REQUIRE(strstr(buf, "Unsupported encoder codingType") != NULL);
    REQUIRE(strstr(buf, "Encoder factory") == NULL);
    REQUIRE(strstr(buf, "Could not create encoder") == NULL);
    return 0;
}

#endif /* NVMPI_TEST_ENC_MOCK_CODEC_H */
