/*
 * mock_v4l2_models.c - extension point: which behaviour a device node gets.
 *
 * The skeleton (#98) registers no model: every node is a plain M2M device
 * that accepts buffers and never completes them. #99 adds the decoder model
 * (nvdec nodes), #100 the encoder model (nvenc nodes) by returning their
 * struct mock_ops here, keyed on the opened node path.
 */
#include "mock_v4l2.h"

const struct mock_ops *mock_model_for_node(const char *node)
{
    (void)node;
    return NULL;
}
