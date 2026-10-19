/*
 * mock_faults.h - fault and delay injection shared by the mock libraries.
 *
 *   NVMPI_MOCK_FAULT = point@N[+][,point@N[+]...]
 *       point  one of: dqbuf qbuf reqbufs streamon expbuf alloc open
 *       N      1-based call number at which the point fails
 *       +      sticky: call N and every later call fail (default: only N)
 *   NVMPI_MOCK_DELAY_US = microseconds added to every successful DQBUF
 *       (a slow decoder); capped at MOCK_DELAY_MAX_US.
 *
 * What counts as "a call" is fixed per point so N is deterministic even when
 * the application polls:
 *   dqbuf     a DQBUF that WOULD succeed (a buffer is ready); the buffer stays
 *             queued for the application when the fault fires
 *   qbuf      a QBUF that passed validation
 *   reqbufs   a REQBUFS with count > 0 (REQBUFS(0), the teardown, never fails)
 *   streamon  a STREAMON that passed validation
 *   expbuf    an EXPBUF that passed validation
 *   alloc     an NvBufSurfaceCreate/Allocate call (libnvbufsurface)
 *   open      a v4l2_open of a /dev node (libnvv4l2): fails with ENODEV, so
 *             a codec's device creation (NvVideo{Encoder,Decoder}) fails
 *
 * The spec is re-read on every call; changing the variable resets all
 * counters. Parsing is defensive: a malformed or unknown token is reported
 * once on stderr and ignored, it never aborts the process.
 *
 * Each mock library compiles its own copy (hidden symbols), so counters are
 * per library; a point is only ever evaluated by the library that owns it.
 */
#ifndef NVMPI_MOCK_FAULTS_H
#define NVMPI_MOCK_FAULTS_H

enum mock_fault_pt {
    MFP_DQBUF,
    MFP_QBUF,
    MFP_REQBUFS,
    MFP_STREAMON,
    MFP_EXPBUF,
    MFP_ALLOC,
    MFP_OPEN,
    MFP_COUNT
};

#define MOCK_DELAY_MAX_US 10000000u   /* 10 s: a typo must not hang CI    */

/* Count this call; returns 1 when the fault fires for it, else 0.
 * Thread-safe. */
int mock_fault_hit(enum mock_fault_pt pt);

/* Sleep NVMPI_MOCK_DELAY_US (no-op when unset/invalid). Call WITHOUT any
 * mock lock held. */
void mock_fault_delay(void);

#endif /* NVMPI_MOCK_FAULTS_H */
