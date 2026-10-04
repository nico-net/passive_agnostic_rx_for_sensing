/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
/*! \file nrLDPC_cb0_cuda.h
 * \brief Dedicated CB0 batch entry of libldpc_cuda.so (Technique D CB0 elimination channel), separate from the TB path.
 *
 * Input: already rate-dematched int8 decoder inputs (Kc*Z values each: 2Z punctured zeros, coded bits, F fillers at
 * 127 -- the layout of nr_td_cb0_dematch / the TB path's pool slots), item i at llr + i * llr_stride, plus per item
 * (BG, Z, K, K', CRC type, guard bytes, GPU iterations). Items may mix BG / Z / iterations in one submission.
 *
 * Algorithm: exactly G1's TB decoder (ldpc_decoder.cu ldpc_pool_decode): normalised (x3/4) flooding min-sum, int8
 * messages, syndrome early termination checked after every iteration but the last, hard decision from the last
 * posterior. Bits are bit-identical to ldpc_pool_decode on the same input and iteration count (tested). The CRC
 * (CRC24A / CRC24B / CRC16, OAI check_crc semantics) and the all-zero guard are evaluated on the GPU; only a small
 * per-item result (and optionally the bits) comes back.
 *
 * Iterations: item.iters is the number of GPU flooding iterations to run. G1's TB rule is 2 x max_ldpc_iterations;
 * the CB0 channel must stay at least as sensitive as the TB decoder that feeds the same context (dominance rule), so
 * a caller must not go below the TB decoder's equivalent unless that cap is proven per codeword (paired harness
 * ldpc_cuda_pool_bler paired, task-CB0GPU report).
 *
 * Memory strategy (picked at run time, ldpc_cb0_mem_mode()):
 *  - UNIFIED (integrated GPU, cudaDevAttrIntegrated = 1, e.g. GB10): kernels read the caller's buffer IN PLACE (host
 *    pageable, pinned or managed: no host<->device copy); per-item results and bits are written by the GPU straight
 *    into pinned mapped host memory.
 *  - EXPLICIT (discrete GPU, e.g. RTX 4070 sm_89; or forced with LDPC_CB0_MEM=explicit): device / managed (when
 *    cudaDevAttrConcurrentManagedAccess) inputs are read in place; host inputs go host -> device with async copies in
 *    large pieces on a dedicated copy stream (pageable memory first packed into this module's pinned staging, piece
 *    by piece, so the CPU pack of piece p+1 overlaps the DMA of piece p); every in-flight submission owns its staging
 *    and device arena (double / triple buffering across grants); results and bits come back with one D2H copy each.
 *  Correctness never depends on unified memory: the EXPLICIT path is exercised on GB10 by the tests (forced).
 *
 * Asynchrony: ldpc_cb0_submit enqueues everything on the module's own low-priority stream and returns; collect waits
 * (bounded) for that submission. Up to LDPC_CB0_SLOTS submissions can be in flight (the receiver dematches grant N+1
 * while grant N decodes). The caller's llr buffer must stay valid and unchanged until collect returns.
 * ldpc_cb0_decode is the synchronous wrapper.
 *
 * Safety (G1 semantics, separate state): every wait is bounded (LDPC_CB0_TIMEOUT_MS, default LDPC_CUDA_TIMEOUT_MS or
 * 200, plus LDPC_CB0_TIMEOUT_US_PER_ITEM, default 100, per item). Any CUDA error, timeout or sticky error fails the
 * WHOLE submission: every result is crc_ok = -1 (inadmissible batch), never a stale pass. The CB0 entry has its OWN
 * circuit breaker (LDPC_CB0_BREAKER_N consecutive failures, default 4 -> bypass for LDPC_CB0_BREAKER_S, default 5 s;
 * sticky -> off for good): a CB0 failure never touches the TB path's breaker, counters or pool.
 */
#ifndef NRLDPC_CB0_CUDA_H
#define NRLDPC_CB0_CUDA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LDPC_CB0_ABI 1
#define LDPC_CB0_BITS_STRIDE 1088 /* bytes per item in the bits output (K <= 8448 bits = 1056 bytes, padded) */
#define LDPC_CB0_MAX_ITERS 64

typedef struct {
  uint8_t BG;           /* 1 or 2 */
  uint8_t crc_type;     /* CRC24_A 0, CRC24_B 1, CRC16 2 (coding_defs.h) */
  uint16_t Z;           /* lifting size (38.212 Table 5.3.2-1) */
  uint16_t K;           /* code block length: K bits are hard-decided (bits output) */
  uint16_t Kprime;      /* lenWithCrc(C, A): CRC over bits [0, Kprime), multiple of 8, <= K */
  uint16_t guard_bytes; /* result.zero = 1 iff decoded bytes [0, guard_bytes) are all zero (0: no guard) */
  uint8_t iters;        /* GPU flooding iterations, 1..LDPC_CB0_MAX_ITERS */
  uint8_t pad;
} ldpc_cb0_item_t;

enum {
  LDPC_CB0_OK = 0,
  LDPC_CB0_E_ARG = -1,      /* bad call / bad item (per item: err) */
  LDPC_CB0_E_CUDA = -2,     /* CUDA error in this submission */
  LDPC_CB0_E_TIMEOUT = -3,  /* not done within the deadline: abandoned */
  LDPC_CB0_E_BUSY = -4,     /* every in-flight slot taken */
  LDPC_CB0_E_BYPASSED = -5, /* CB0 breaker open */
  LDPC_CB0_E_DISABLED = -6, /* sticky error / no device / init failed */
  LDPC_CB0_E_TOOBIG = -7,   /* n > ldpc_cb0_max_items() */
};

typedef struct {
  int8_t crc_ok;        /* 1 CRC pass, 0 fail, -1 no verdict (bad item, or the submission failed) */
  uint8_t iters;        /* iterations executed (< item.iters iff early termination) */
  uint8_t converged;    /* 1: syndrome satisfied before the last iteration (early termination) */
  uint8_t zero;         /* guard: decoded bytes [0, guard_bytes) all zero */
  uint8_t decoder_used; /* 2 = CUDA flooding (NRLDPC_DECODER_CUDA_FLOODING), 0 = not decoded */
  int8_t err;           /* LDPC_CB0_OK or LDPC_CB0_E_* */
  uint8_t pad[2];
} ldpc_cb0_result_t;

typedef struct ldpc_cb0_ticket ldpc_cb0_ticket_t;

/* Idempotent. 0, or LDPC_CB0_E_DISABLED. Called by submit on first use. */
int ldpc_cb0_init(void);
/* Enqueue n items (llr: item i at llr + i * llr_stride, Kc*Z <= llr_stride bytes valid). want_bits: also return the K
 * hard-decided bits per item (collect's bits). On success *t is set and collect MUST be called once. On failure no
 * ticket exists and the batch has no verdict. */
int ldpc_cb0_submit(const ldpc_cb0_item_t *items, int n, const int8_t *llr, size_t llr_stride, int want_bits,
                    ldpc_cb0_ticket_t **t);
/* Wait for a submission (bounded). out[n]: per item results; bits (may be NULL; needs want_bits) item i at
 * bits + i * bits_stride, (K + 7) / 8 bytes. Returns LDPC_CB0_OK or a negative code; on any failure EVERY out[i] has
 * crc_ok = -1 and err = that code. The ticket is released either way. */
int ldpc_cb0_collect(ldpc_cb0_ticket_t *t, ldpc_cb0_result_t *out, uint8_t *bits, size_t bits_stride);
/* Synchronous wrapper: submit + collect. */
int ldpc_cb0_decode(const ldpc_cb0_item_t *items, int n, const int8_t *llr, size_t llr_stride, ldpc_cb0_result_t *out,
                    uint8_t *bits, size_t bits_stride);

/* 1 = usable now: device present, entry initialised, CB0 breaker closed (not bypassed, no sticky error). Cheap. */
int ldpc_cb0_healthy(void);
/* 1 = UNIFIED, 2 = EXPLICIT, 0 = not initialised / disabled. */
int ldpc_cb0_mem_mode(void);
/* Largest n per submission (LDPC_CB0_MAX_ITEMS, default 2048). */
int ldpc_cb0_max_items(void);

typedef struct {
  uint64_t submits, items, ok, cuda_errors, timeouts, busy, bypassed, sticky, breaker_trips, graphs, h2d_bytes;
  uint64_t state; /* breaker: 0 closed, 1 bypassed, 2 permanently off */
  uint64_t mode;  /* ldpc_cb0_mem_mode() */
} ldpc_cb0_counters_t;
void ldpc_cb0_get_counters(ldpc_cb0_counters_t *c);

/* ---- tests only ---- */
/* inject: 1 = CUDA error at the next submits, 2 = stall the stream stall_ms (timeout), 3 = report a sticky error.
 * 0 clears. */
void ldpc_cb0_test_hooks(int inject, int stall_ms, int timeout_ms, int breaker_n, int breaker_ms);
void ldpc_cb0_test_reset(void); /* close the CB0 breaker, clear the (simulated) sticky state */
/* Switch the memory strategy (1 unified, 2 explicit) while idle; returns the active mode (unified needs an
 * integrated GPU). */
int ldpc_cb0_test_set_mem(int mode);

#ifdef __cplusplus
}
#endif

#endif
