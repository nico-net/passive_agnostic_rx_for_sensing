/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 *
 * Batched polar successive-cancellation decode on the GPU for DCI-sized blocks -- a bit-exact
 * port of polar_decoder_int16()'s steady-state path (nrPolar_tools/nr_polar_decoder.c +
 * nr_polar_decoding_tools.c): rate de-matching, the frozen-subtree-pruned SC tree with
 * min-sum F / saturating G and `alpha <= 0 -> 1` hard decisions, one thread block per item.
 * Information-bit extraction and the CRC stay on the host, done with the same code the CPU
 * decoder uses, so the only thing that has to be bit-exact is the u vector.
 *
 * "Steady state" matters: the CPU tree's betaInit flags are never reset between decodes, so
 * from the second decode on every G uses the sign+saturating-sub path and every frozen beta is
 * a permanent -1. The live blind search runs thousands of decodes per cached params object, so
 * that is the behaviour that counts, and it is what this port reproduces (the FIRST CPU decode
 * of a fresh params object differs at int16 saturation edges -- see the test, which warms up).
 *
 * Prototype scope (2026-09-17): synchronous batch API for measurement. The live wiring needs
 * the async single-worker pattern of nr_pdsch_gpu_fep -- see the report.
 */
#ifndef NR_POLAR_SC_CUDA_H
#define NR_POLAR_SC_CUDA_H

#include <stdint.h>
#include "PHY/NR_UE_TRANSPORT/nr_dci_bits.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NPC_MAX_N 512     /* NR_POLAR_DCI_N_MAX = 9 */
#define NPC_MAX_E 2048    /* AL16 = 1728 (repetition rate matching: E > N) */
#define NPC_MAX_OPS 2048  /* <= 3 * (2N - 1) */
#define NPC_MAX_PARAMS 704 /* up to 140 lengths x five aggregation levels */

/* Register one (DCI length, aggregation level) so its patterns and op list live on the device.
 * Idempotent; returns a params id >= 0, or -1. Uses nr_polar_params() for the patterns. */
int npc_register(uint16_t dci_length, uint8_t aggregation_level);

typedef struct {
  int pid;            /* from npc_register */
  const int16_t *llr; /* E int16 LLRs, the same vector polar_decoder_int16() gets */
} npc_item_t;

/* Decode n items as one batch (H2D, one kernel, D2H, host extraction + CRC).
 * crc[i] is what polar_decoder_int16() returns (24-bit CRC xor RNTI-masked CRC),
 * payload[i] is its three right-aligned payload words. Returns 0 on success. */
int npc_decode_batch(const npc_item_t *items, int n, uint32_t *crc, nr_dci_bits_t *payload);

/* Items sharing LLR vectors: item i decodes vec + vidx[i]*vstride with params pid[i]; only the
 * n_vec distinct vectors are copied/uploaded. Returns 0 on success. */
int npc_decode_batch_vec(const int16_t *vec, int vstride, int n_vec, const int *vidx, const int *pid, int n,
                         uint32_t *crc, nr_dci_bits_t *payload);

/* Time split of the last npc_decode_batch, microseconds. */
typedef struct { double h2d, kernel, d2h, host; } npc_timing_t;
npc_timing_t npc_last_timing(void);

#ifdef __cplusplus
}
#endif
#endif
