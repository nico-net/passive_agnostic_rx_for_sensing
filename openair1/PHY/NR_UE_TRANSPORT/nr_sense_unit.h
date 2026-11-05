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

/* Rows (RNTIs) are opened only by wideband-capable calls: est_usable_prbs (job, skip) or valid_prbs (cfr) >= sense_min_prb. A cost-only or narrow call for an
 * RNTI without a row changes no unit; its CPU is added to the cell's nr_sense_unit_unattributed_ns() so it stays in the baseline. The one exception is SRS:
 * it is a per-cell unit (rnti forced to 0 in every call) for both cost and messages (one message per exported record, counted by nr_sense_unit_cfr).
 * Messages: PDSCH_DATA is a message only when its daBg job runs (est_usable_prbs > 0 there; the decode stage passes 0), so a grant that was decoded but not
 * data-aided is not in the rate. `late` can count one grant twice (decode stage and recon stage). DM-RS sources report wait 0 and late false (inline, no queue). */
/* One job of unit (s, rnti): thread CPU, enqueue-to-start wait, late flag. est_usable_prbs > 0 counts the job as one message of the unit (rate, wideband rate);
 * 0 attributes cost only (a second stage of an already counted message). t_ns = CLOCK_MONOTONIC ns (nr_sense_now_ns) at the job's end. */
void nr_sense_unit_job(nr_unit_src_t s, uint16_t rnti, uint64_t cpu_ns, uint64_t wait_ns, bool late, uint16_t est_usable_prbs, uint64_t t_ns);
/* Cost-only attribution to a resource other than the pool: the sense writer's time for a record of the unit (SENSE_RING) and the daBg worker CPU of a data-aided message (DA). Never opens a row. */
void nr_sense_unit_cost_ring(nr_unit_src_t s, uint16_t rnti, uint64_t ns, uint64_t t_ns);
void nr_sense_unit_cost_da(nr_unit_src_t s, uint16_t rnti, uint64_t ns, uint64_t t_ns);
/* One exported CFR record of the unit (from the sense writer's emit_rs). */
void nr_sense_unit_cfr(nr_unit_src_t s, uint16_t rnti, uint16_t valid_prbs, float snr_db, uint64_t t_ns);
void nr_sense_unit_violation(nr_unit_src_t s, uint16_t rnti); /* a drop inside an ENABLED unit (counted only then); never opens a row. A drop of a disabled unit is not a violation */
/* A message of a disabled unit, gated before work: counts as a (never produced) message with its usable PRBs, so a disabled unit keeps its (wideband) rate.
 * t_ns = nr_sense_now_ns(). Like job(), it opens a row only when est_usable_prbs >= sense_min_prb. */
void nr_sense_unit_skip(nr_unit_src_t s, uint16_t rnti, uint16_t est_usable_prbs, uint64_t t_ns);
void nr_sense_unit_skip_w(nr_unit_src_t s, uint16_t rnti, uint16_t est_usable_prbs, uint64_t t_ns, unsigned weight); /* the skip stands for `weight` messages (rate and wideband rate), one skip */
/* The scheduler's switch. Creates the row. A row (RNTI) is created DISABLED in every source (S7): the scheduler enables it through its cold-unit path. */
void nr_sense_unit_set_enabled(int cell, uint16_t rnti, nr_unit_src_t s, bool enabled);
/* The gate of a unit (spec 5.6), lock-free, bounded (one scan of the 64 row keys), read by the cell of the calling thread. true = the work may run.
 * Outside the SENSING mode, and for a source that is not a unit (SSB, CSI-RS: never gated), it is always true. A unit without a row is disabled. */
bool nr_sense_unit_enabled(nr_unit_src_t s, uint16_t rnti);
/* Gate site helper: true = run the work. false = the unit is disabled: the message is counted as a disabled skip (nr_sense_unit_skip, which needs est_usable_prbs,
 * the usable PRBs of the message: it is what keeps the rate of a disabled unit and opens its row) and the caller must not start the work. SRS passes 0: a capture counts as
 * one message of sense_min_prb PRBs. */
bool nr_sense_unit_gate(nr_unit_src_t s, uint16_t rnti, uint16_t est_usable_prbs);
/* The decode policy lets a check decode through for a disabled unit: it stands for `grants` grants of the RNTI. The next skip of the unit books them as its message weight. No row, no effect. */
void nr_sense_unit_offer(nr_unit_src_t s, uint16_t rnti, unsigned grants);
/* As nr_sense_unit_gate for a site under a long lock (g_phase2_mu): only the read-only check runs there. A false result queues the skip on the calling thread;
 * nr_sense_unit_flush_pending() books the queued skips and must be called by the same thread after the unlock. */
bool nr_sense_unit_gate_deferred(nr_unit_src_t s, uint16_t rnti, uint16_t est_usable_prbs);
void nr_sense_unit_flush_pending(void);
/* Per-cell totals for the metrics (cumulative since start; survive row eviction). */
typedef struct {
  uint64_t violations, skips;                 /* drops inside enabled units / messages gated off */
  uint64_t wb_valid, wb_valid_prbs;           /* wideband CFR records (valid_cfr_prbs >= sense_min_prb) and the sum of their valid PRBs */
  uint64_t gap_max_ns;                        /* longest time between two wideband CFR records of the cell */
  uint32_t units_enabled;                     /* units enabled now */
} nr_sense_unit_cell_stats_t;
void nr_sense_unit_cell_stats(int cell, nr_sense_unit_cell_stats_t *out);
/* CPU ns of cost-only calls that had no row (cumulative, per cell). */
uint64_t nr_sense_unit_unattributed_ns(int cell);

typedef struct {
  uint16_t rnti; nr_unit_src_t src; bool enabled;
  float cpu_p95_ns, cpu_p99_ns;        /* per 50 ms bucket demand over the last SENSE_WIN_DECISIONS decisions (nearest-rank, idle buckets count as 0; p99 ignores the 3 busiest buckets of 320) */
  float ring_p99_ns, da_p99_ns;         /* p99 per 50 ms bucket of the unit's writer (record formatting + quality) cost and of its daBg worker CPU: the demand on SENSE_RING and on DA */
  float cpu_max_ns;                    /* the busiest of those buckets (burst) */
  float wait_p99_ns; uint32_t late, violations, skips; /* wait: per-bucket maximum, p99 over the buckets that saw a job; counters are cumulative since the row was created (re-baseline when gen changes) */
  float rate_hz, wb_rate_hz;           /* messages / those with est_usable >= min_prb, per second over min(16 s, now - first_ns) (at least one bucket) */
  float valid_ratio, snr_db; uint64_t last_valid_wb_ns, last_msg_ns;
  uint64_t last_cfr_ns;                /* time of the latest CFR record (0: none); valid_ratio is the EWMA of the records, the first record initialises it */
  uint32_t n_cfr;                      /* CFR records of this unit since the row was created (0: valid_ratio is unknown, not bad) */
  uint16_t est_usable_prbs;            /* usable PRBs of the unit's latest message that reported any (0: none yet, always for SRS) */
  uint64_t first_ns;                   /* first call of this unit (its history length) */
  uint32_t gen;                        /* row generation: changes when the row is evicted and re-created; counters restart from 0 */
} nr_sense_unit_view_t;
/* scheduler thread; returns the number of units written (only units with history) */
int nr_sense_unit_snapshot(int cell, nr_sense_unit_view_t *out, int max);
int nr_sense_unit_snapshot_at(int cell, uint64_t now_ns, nr_sense_unit_view_t *out, int max); /* now_ns explicit (tests, replay) */
uint64_t nr_sense_unit_bucket_cpu(int cell, uint16_t rnti, nr_unit_src_t s, uint64_t t_ns); /* CPU ns accumulated in the bucket holding t_ns (0 if none) */
void nr_sense_unit_test_seam(void (*f)(void));                                                /* tests: called between locating a row and updating it */
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
