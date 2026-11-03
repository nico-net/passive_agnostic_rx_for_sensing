/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI-DBG3: confirmation of a GPU exact FEP self-check mismatch (nr_pdsch_gpu_exact.c).
 *
 * The GPU FEP and its CPU reference both read the cell's live IQ ring, one after the other. When the producer overwrites the slot in
 * between (a consumer that lags past the ring, a ring overrun / stream discontinuity), the two read different samples and differ although
 * the GPU is bit-exact: measured 2026-10-27 (mp27e_P1, two cells): `[c1] GPU_EXACT_FEP self-check MISMATCH` right after `SHMIQ_OVERRUN` /
 * `RXDISCONT` on that cell, after 4000 matching checks; it switched the exact FEP / chest (and so the graph pass) off for EVERY cell.
 *
 * A mismatch is therefore confirmed only when it repeats on input that stays put: CPU reference A, GPU re-run, CPU reference B, in this
 * order, on the same (antenna, symbol). A != B: the input moved during the confirmation (void). A == B: the GPU re-run saw the same input
 * as both references, so GPU != A is a GPU fault (confirmed) and GPU == A means the first comparison raced the producer (void). A refused
 * GPU re-run cannot confirm anything (void). The decision uses only the data: no ring, slot or host knowledge.
 *
 * cpu(ctx, out) / gpu(ctx, out): compute the checked row (`bytes` bytes) into out, 0 = done. scratch: 3 * bytes, suitably aligned. */
#ifndef NR_FEP_SELFCHECK_H
#define NR_FEP_SELFCHECK_H
#include <stddef.h>
#include <string.h>

enum { NR_FEP_SELFCHECK_VOID = 0, NR_FEP_SELFCHECK_CONFIRMED = 1 };

static inline int nr_fep_selfcheck_confirm(int (*cpu)(void *, void *), int (*gpu)(void *, void *), void *ctx, size_t bytes, void *scratch)
{
  unsigned char *a = (unsigned char *)scratch, *g = a + bytes, *b = g + bytes;
  if (cpu(ctx, a) != 0)
    return NR_FEP_SELFCHECK_VOID;
  const int gpu_rc = gpu(ctx, g);
  if (cpu(ctx, b) != 0 || gpu_rc != 0 || memcmp(a, b, bytes) != 0)
    return NR_FEP_SELFCHECK_VOID;
  return memcmp(a, g, bytes) != 0 ? NR_FEP_SELFCHECK_CONFIRMED : NR_FEP_SELFCHECK_VOID;
}

/* A confirmed mismatch is a GPU fault only when it repeats: OTA 2026-11-03 (415+923, 4 of 8 pair runs) one confirmed mismatch switched the exact
 * FEP / chest / graph pass off for every cell of the process (decode p50 5 us -> 5 ms, ring overruns). A defective kernel fails every check, so
 * the path goes off after NR_FEP_SELFCHECK_FAULT_RUN confirmed checks in a row (a matching check restarts the count); the slot of each confirmed
 * mismatch takes the CPU FEP. */
enum { NR_FEP_SELFCHECK_FAULT_RUN = 3 };
static inline int nr_fep_selfcheck_fault(int confirmed_in_a_row) { return confirmed_in_a_row >= NR_FEP_SELFCHECK_FAULT_RUN; }

/* int16 samples of a and b that differ (*n) and the largest |a - b| (*maxabs): what a confirmed mismatch looks like, for its log line. */
static inline void nr_fep_selfcheck_diff(const void *a, const void *b, size_t bytes, int *n, int *maxabs)
{
  const short *x = (const short *)a, *y = (const short *)b;
  *n = 0;
  *maxabs = 0;
  for (size_t i = 0; i < bytes / sizeof(short); i++) {
    const int d = x[i] > y[i] ? x[i] - y[i] : y[i] - x[i];
    if (d) {
      ++*n;
      if (d > *maxabs) *maxabs = d;
    }
  }
}
#endif
