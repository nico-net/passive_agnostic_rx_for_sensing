/* Per-unit cost accounting of the sensing mode (spec 2026-11-03-two-modes-design.md section 5.2, plan Task S5).
 * A unit is (PCI, source, RNTI). Every hand-off reports its thread CPU, its enqueue-to-start wait and its lateness into 50 ms buckets of the
 * calling thread's cell (nr_cell_idx()). Lock-free on the hot path (atomics, CAS, no allocation); creating a row takes a try-lock and, if busy, drops that one
 * sample. All calls are no-ops outside the SENSING mode. */
#ifndef NR_SENSE_UNIT_H
#define NR_SENSE_UNIT_H
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "PHY/NR_UE_ISAC/nr_isac_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NR_UNIT_PDSCH_DMRS = 0, NR_UNIT_PDSCH_DATA, NR_UNIT_PUSCH_DMRS, NR_UNIT_SRS, NR_UNIT_SRC_N } nr_unit_src_t;

#define NR_UNIT_ROWS 64              /* RNTIs per cell, LRU on the last message */
#define NR_UNIT_LATE_NS 20000000ull  /* a job whose enqueue-to-end time exceeds this is late (the DECSLOW threshold of the PDSCH queue) */

/* One job of unit (s, rnti): thread CPU, enqueue-to-start wait, late flag. est_usable_prbs > 0 counts the job as one message of the unit (rate, wideband rate);
 * 0 attributes cost only (a second stage of an already counted message). t_ns = CLOCK_MONOTONIC ns (nr_sense_now_ns) at the job's end. */
void nr_sense_unit_job(nr_unit_src_t s, uint16_t rnti, uint64_t cpu_ns, uint64_t wait_ns, bool late, uint16_t est_usable_prbs, uint64_t t_ns);
/* One exported CFR record of the unit (from the sense writer's emit_rs). */
void nr_sense_unit_cfr(nr_unit_src_t s, uint16_t rnti, uint16_t valid_prbs, float snr_db, uint64_t t_ns);
void nr_sense_unit_violation(nr_unit_src_t s, uint16_t rnti); /* a drop inside an enabled unit */
void nr_sense_unit_skip(nr_unit_src_t s, uint16_t rnti);      /* a message of a disabled unit, gated before work: also counts as a (never produced) message */
/* The scheduler's switch; units default to enabled. Creates the row. */
void nr_sense_unit_set_enabled(int cell, uint16_t rnti, nr_unit_src_t s, bool enabled);

typedef struct {
  uint16_t rnti; nr_unit_src_t src; bool enabled;
  float cpu_p95_ns, cpu_p99_ns;        /* per 50 ms bucket demand over the last SENSE_WIN_DECISIONS decisions (nearest-rank, idle buckets count as 0) */
  float wait_p99_ns; uint32_t late, violations, skips; /* wait: per-bucket maximum, p99 over the buckets that saw a job; counters are cumulative */
  float rate_hz, wb_rate_hz;           /* messages / those with est_usable >= min_prb, per second over the 320-bucket window */
  float valid_ratio, snr_db; uint64_t last_valid_wb_ns, last_msg_ns;
} nr_sense_unit_view_t;
/* scheduler thread; returns the number of units written (only units with history) */
int nr_sense_unit_snapshot(int cell, nr_sense_unit_view_t *out, int max);
int nr_sense_unit_snapshot_at(int cell, uint64_t now_ns, nr_sense_unit_view_t *out, int max); /* now_ns explicit (tests, replay) */
uint64_t nr_sense_unit_bucket_cpu(int cell, uint16_t rnti, nr_unit_src_t s, uint64_t t_ns); /* CPU ns accumulated in the bucket holding t_ns (0 if none) */
void nr_sense_unit_reset(int cell);                                                           /* tests / cell restart */

/* Thread CPU clock for the sites: 0 outside the SENSING mode (the one mode branch per site), else a nonzero stamp. */
static inline uint64_t nr_sense_unit_cpu_ns(void)
{
  if (nr_isac_mode() != NR_ISAC_MODE_SENSING)
    return 0;
  struct timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec + 1;
}

#ifdef __cplusplus
}
#endif
#endif
