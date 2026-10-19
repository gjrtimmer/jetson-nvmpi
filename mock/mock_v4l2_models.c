/*
 * mock_v4l2_models.c - which behaviour a device node gets.
 *
 * Nodes whose path contains "nvdec" (/dev/nvhost-nvdec, /dev/v4l2-nvdec) get
 * the decoder model (mock_dec.c, #99). Every other node is a plain M2M device
 * that accepts buffers and never completes them (the queue skeleton, #98);
 * the encoder model (nvenc/msenc nodes) arrives with #100.
 */
#include "mock_v4l2.h"

#include <string.h>

const struct mock_ops *mock_model_for_node(const char *node)
{
    if (node && strstr(node, "nvdec"))
        return &mock_dec_ops;
    return NULL;
}
