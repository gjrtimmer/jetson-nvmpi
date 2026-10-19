/*
 * mock_test.h - shared pieces of the mock self-test executable (test_mock):
 * the CHECK macro, the libnvv4l2 prototypes (libv4l2.h is not part of the
 * build image) and the entry points of the test groups.
 */
#ifndef NVMPI_TEST_MOCK_TEST_H
#define NVMPI_TEST_MOCK_TEST_H

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <nvbufsurface.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int v4l2_open(const char *file, int oflag, ...);
int v4l2_close(int fd);
int v4l2_ioctl(int fd, unsigned long int request, ...);
void *v4l2_mmap(void *start, size_t length, int prot, int flags, int fd,
                int64_t offset);
/* Declared locally: avoids the CUDA headers. */
int NvBufSurfTransform(NvBufSurface *src, NvBufSurface *dst, void *params);

extern int mock_fails;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s (errno %d)\n", __FILE__,      \
                    __LINE__, #c, errno);                                 \
            mock_fails++;                                                 \
        }                                                                 \
    } while (0)

#define OUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE

/* test_mock_dec.c: decoder model. test_mock_fault.c: fault/delay injection. */
void test_decoder_model(void);
void test_fault_injection(void);
/* test_mock_enc.c: encoder model. */
void test_encoder_model(void);

#endif /* NVMPI_TEST_MOCK_TEST_H */
