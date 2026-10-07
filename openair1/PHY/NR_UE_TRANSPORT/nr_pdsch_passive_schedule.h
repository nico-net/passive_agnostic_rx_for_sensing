/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PDSCH_PASSIVE_SCHEDULE_H
#define NR_PDSCH_PASSIVE_SCHEDULE_H
#include "nr_passive_sample_lifetime.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Counters accumulate over the run; non-overlapping bins, in microseconds: <=100,250,500,1000,2000,
 * 4000,8000,16000, and >16000. Counts include failed work reaching that stage. */
#define NR_PDSCH_LAT_BINS 9
enum { NR_PDSCH_LAT_QUEUE, NR_PDSCH_LAT_FEP, NR_PDSCH_LAT_LDPC,
       NR_PDSCH_LAT_TOTAL, NR_PDSCH_LAT_GPU_WAIT, NR_PDSCH_LAT_STAGES };
enum { NR_PDSCH_DROP_QUEUE, NR_PDSCH_DROP_FUTURE, NR_PDSCH_DROP_COPY,
       NR_PDSCH_DROP_UNSUPPORTED, NR_PDSCH_DROP_ERROR,
       NR_PDSCH_GPU_FALLBACK, NR_PDSCH_DROP_REASONS };
typedef struct {
  uint64_t hist[NR_PDSCH_LAT_STAGES][NR_PDSCH_LAT_BINS];
  uint64_t reason[NR_PDSCH_DROP_REASONS];
} nr_pdsch_schedule_stats_t;
static inline unsigned nr_pdsch_latency_bin(uint64_t ns)
{
  static const uint64_t us[] = {100, 250, 500, 1000, 2000, 4000, 8000, 16000};
  unsigned i = 0;
  while (i < NR_PDSCH_LAT_BINS - 1 && ns > us[i] * 1000) ++i;
  return i;
}

/* Preserve original ring indices so both the CPU FEP (including absolute CFO phase)
 * and the GPU API can read the same snapshot. Copy only the slot window, including
 * early-FFT prefix and final-symbol slack. The rest of this private arena is never read.
 * Both guards are essential: a copy straddling overwrite must not gain ownership. */
static inline bool nr_pdsch_copy_iq(void *const dst[], const void *const src[], unsigned nant,
                                   size_t ring_bytes, size_t origin, size_t bytes,
                                   long source, long spf, long (*producer)(void *), void *ctx)
{
  if (!ring_bytes || origin >= ring_bytes || bytes > ring_bytes
      || !nr_passive_samples_valid(producer(ctx), source, spf)) return false;
  const size_t first = bytes < ring_bytes - origin ? bytes : ring_bytes - origin;
  for (unsigned a = 0; a < nant; ++a) {
    memcpy((char *)dst[a] + origin, (const char *)src[a] + origin, first);
    if (bytes > first) memcpy(dst[a], src[a], bytes - first);
  }
  return nr_passive_samples_valid(producer(ctx), source, spf);
}
#endif
