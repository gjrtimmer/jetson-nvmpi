/*
 * enc_mock_cases.h - the case table of test_api_enc_mock.c (kept apart so
 * the fault matrix can be tuned without touching the case bodies). Included
 * once, after struct tcase and the case functions.
 */
#ifndef NVMPI_TEST_ENC_MOCK_CASES_H
#define NVMPI_TEST_ENC_MOCK_CASES_H

#include "enc_mock_teardown.h"
#include "enc_mock_backpressure.h"
#include "enc_mock_lowdelay.h"
#include "enc_mock_codec.h"
#include "enc_mock_rc.h"
#include "enc_mock_av1.h"
#include "enc_mock_p010.h"

static const struct tcase k_cases[] = {
    { "blocking_h264", NULL, blocking_h264, 0 },
    { "blocking_hevc", NULL, blocking_hevc, 0 },
    { "header_idr_h264", NULL, header_idr_h264, 0 },
    { "header_idr_hevc", NULL, header_idr_hevc, 0 },
    { "pipelined", NULL, pipelined, 0 },
    { "determinism", NULL, determinism, 0 },
    { "force_idr", NULL, force_idr, 0 },
    { "nonblock_flow", NULL, nonblock_flow, 0 },
    { "nonblock_eagain", NULL, nonblock_eagain, 0 },
    { "nonblock_backpressure", NULL, nonblock_backpressure, 0 },
    { "nonblock_backpressure_latency", NULL, nonblock_backpressure_latency, 0 },
    { "lowdelay_loop", NULL, lowdelay_loop, 0 },
    { "lowdelay_idle", NULL, lowdelay_idle, 0 },
    { "lowdelay_latency", NULL, lowdelay_latency, 0 },
    { "lowdelay_flush", NULL, lowdelay_flush, 0 },
    { "flush_restart", NULL, flush_restart, 0 },
    { "dmabuf_flow", NULL, dmabuf_flow, 0 },
    { "teardown_in_flight", NULL, teardown_in_flight, 0 },
    { "teardown_wrapped", NULL, teardown_wrapped, 0 },
    { "teardown_after_eos", NULL, teardown_after_eos, 0 },
    { "teardown_no_data", NULL, teardown_no_data, 0 },
    { "teardown_all_slots", NULL, teardown_all_slots, 0 },
    { "teardown_held_packet", NULL, teardown_held_packet, 0 },
    { "teardown_pool_empty_drop", NULL, teardown_pool_empty_drop, 0 },
    { "vbr_peak_bitrate", NULL, vbr_peak_bitrate, 0 },
    { "p010_hevc_main10", NULL, p010_hevc_main10, 0 },
    { "p010_hevc_main", NULL, p010_hevc_main, 0 },
    /* dqbuf@9999 never fires: it only marks the logged refusals as expected */
    { "p010_frame_bounds", "dqbuf@9999", p010_frame_bounds, 0 },
    /* open@1+: create must fail before the device is touched (see the case) */
    { "p010_create_gate", "open@1+", p010_create_gate, 0 },
#if NVMPI_HAVE_AV1_ENC
    { "av1_stream", NULL, av1_stream, 0 },
    { "av1_force_idr", NULL, av1_force_idr, 0 },
    { "av1_roundtrip", NULL, av1_roundtrip, 0 },
#endif
    /* open@1+: create must fail before the device is touched (see the case) */
    { "av1_create_gate", "open@1+", av1_create_gate, 0 },
    /* fault matrix: fn == NULL -> fault_case() */
    { "fault_none_control", "dqbuf@9999", NULL, 0 },
    { "fault_open@1", "open@1", NULL, 0 },        /* factory retry recovers */
    { "fault_open@1+", "open@1+", NULL, 1 },
    { "fault_reqbufs@1", "reqbufs@1", NULL, 1 },
    { "fault_reqbufs@2", "reqbufs@2", NULL, 1 },
    { "fault_streamon@1", "streamon@1", NULL, 1 },
    { "fault_streamon@2", "streamon@2", NULL, 1 },
    { "fault_qbuf@1", "qbuf@1", NULL, 1 },
    { "fault_qbuf@6", "qbuf@6", NULL, 1 },
    { "fault_qbuf@3+", "qbuf@3+", NULL, 1 },
    { "fault_dqbuf@1", "dqbuf@1", NULL, 1 },
    { "fault_dqbuf@4", "dqbuf@4", NULL, 1 },
    { "fault_dqbuf@2+", "dqbuf@2+", NULL, 1 },
    /* open@1+ proves create fails before the device is touched */
    { "create_unsupported_codec", "open@1+", create_unsupported_codec, 0 },
    { "fault_surface_alloc", "alloc@1", fault_surface_alloc, 0 },
    { "fault_dmabuf_alloc", "alloc@2", fault_dmabuf_alloc, 0 },
};

#endif /* NVMPI_TEST_ENC_MOCK_CASES_H */
