/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* BATCHCPU (default off). Routing of one PDSCH decode body between the GPU chain and the unchanged CPU chain, and a
 * bounded GPU admission gate. Everything here is decided per body from measured latencies (no per-cell constants).
 *  ISAC_PDSCH_CPU_SMALL=<maxPRB>: grants with <= maxPRB PRB and <= ISAC_PDSCH_CPU_SMALL_NL (default 2) layers are
 *    candidates; a candidate runs on the CPU path when the EWMA of the CPU body time of its size class is below the EWMA of
 *    the GPU body time (which includes the GPU queue wait) of the same class; the losing arm is re-sampled 1 in 32.
 *  ISAC_PDSCH_GPU_BATCH=1: at most ISAC_PDSCH_GPU_BATCH_SLOTS (default 3) bodies hold the GPU at once, probes leave one
 *    slot to real grants; a waiter that has waited as long as its CPU estimate (candidates) or 3 ms goes on.
 *    NOT a kernel batcher: no kernel is fused across grants.
 * Flags off: nr_pdsch_route_begin() is a no-op and nr_pdsch_route_cpu() is false. */
#ifndef NR_PDSCH_CPU_ROUTE_H
#define NR_PDSCH_CPU_ROUTE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint64_t t0;
  int8_t cls;     /* size class, -1 = not tracked */
  int8_t cpu;     /* this body runs on the CPU path */
  int8_t held;    /* holds a GPU gate slot */
} nr_pdsch_route_t;

nr_pdsch_route_t nr_pdsch_route_begin(int nprb, int layers, bool probe);
void nr_pdsch_route_end(nr_pdsch_route_t *r);
/* true while the calling thread's current body must not use any GPU stage (FEP, chest, demod, LDPC) */
bool nr_pdsch_route_cpu(void);
#endif
