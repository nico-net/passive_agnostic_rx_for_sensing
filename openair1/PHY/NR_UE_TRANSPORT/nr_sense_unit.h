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
#include "nr_sense_quality.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NR_UNIT_PDSCH_DMRS = 0, NR_UNIT_PDSCH_DATA, NR_UNIT_PUSCH_DMRS, NR_UNIT_SRS, NR_UNIT_SRC_N } nr_unit_src_t;

#define NR_UNIT_ROWS 64              /* RNTIs per cell, LRU on the last message */
#define NR_UNIT_LATE_NS 20000000ull  /* a job whose enqueue-to-end time exceeds this is late (the DECSLOW threshold of the PDSCH queue) */

/* Rows (RNTIs) are opened only by continuity-capable calls: est_usable_prbs (job, skip) or valid_prbs (cfr) >= T_cell of the calling cell (nr_sense_cell_floor; sense_min_prb until the first decision). A cost-only or narrow call for an
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
/* Grant-level wideband path: a DL grant of >= sense_min_prb PRBs (the wideband target; NOT the continuity floor T_cell) with proven fields gets a DM-RS-only CFR whichever RNTI it belongs to,
 * whatever its unit says. wbg_admit (calling thread's cell, SENSING only) is asked by a tap site after the unit gate refused: true = run the tap. Per cell it is a token bucket
 * (depth 4 tokens) refilled at the rate the scheduler published (nr_sense_wbg_set_rate), NEVER unlimited: before the first decision, and after a cell reset, the rate is one tap per
 * SENSE_DECIDE_NS. A grant takes one token; with none left, a grant wider than the widest admitted since the last decision may borrow down to -2 tokens (so the widest of the window is
 * not starved), every other grant waits for the refill, which is time: tokens that accrued over the longest gap serve the next grant. This is a rate limiter with a widest-grant
 * exception, not a ranking of grants. A cell the scheduler does not decide for (index >= NR_SENSE_CELLS) admits nothing.
 * wbg_cost books the tap's thread CPU, wbg_ring_cost the writer's cost of its record (the scheduler prices both), wbg_cfr its record to the cell totals (never to an RNTI unit). */
bool nr_sense_wbg_admit(uint16_t prbs, uint64_t t_ns);
void nr_sense_wbg_cost(uint64_t cpu_ns);
void nr_sense_wbg_ring_cost(uint64_t ns);
void nr_sense_wbg_cfr(uint16_t valid_prbs, uint64_t t_ns);
/* A token that bought no tap goes back (atomic add on the bucket, capped at its depth): the enqueue was refused, the job was dropped / evicted / went stale, the CRC failed, the ring no longer held the slot,
 * the DM-RS symbol was invalid or nothing was exported. admitted - refunded == taps. nr_sense_wbg_wasted books the CPU of such a tap (no tap counted). */
void nr_sense_wbg_refund(void);
void nr_sense_wbg_wasted(uint64_t cpu_ns);
/* offered_wbg_overcap: wide grants offered by a cell at or beyond NR_SENSE_CELLS (no scheduler decision, hence no rate, hence no tap), per cell, in stats.overcap. NR_SENSE_CELLS is not raised. */
/* Valid-PRB bookkeeping of wide (nb_rb >= sense_min_prb) PDSCH records, writer thread (its cell), after nr_sense_quality_absent. wide = records, short = those with valid_cfr_prbs < sense_min_prb;
 * over the short ones: hit[r] = records where rule r removed >= 1 PRB, first[r] = records whose first rule (enum order) was r, prbs[r] = PRBs removed by r. Cumulative. */
void nr_sense_wbq_note(const nr_sense_quality_t *q);
typedef struct { uint64_t wide, shorts, hit[NR_SENSE_INV_N], first[NR_SENSE_INV_N], prbs[NR_SENSE_INV_N]; } nr_sense_wbq_stats_t;
void nr_sense_wbq_stats(int cell, nr_sense_wbq_stats_t *out);
void nr_sense_wbg_set_rate(int cell, float rate_hz); /* scheduler: grants/s allowed (>= 0); a negative value is the startup rate */
typedef struct { uint64_t offered, admitted, taps, cost_ns, records, ring_ns, refunded, overcap; float rate_hz; } nr_sense_wbg_stats_t; /* counters cumulative; rate_hz = the rate in force */
void nr_sense_wbg_stats(int cell, nr_sense_wbg_stats_t *out);
/* Site conditions (pure). Consumer site (solver-path job): the TB CRC must have passed (a CRC-failed grant's DCI was never verified: a chance DCI gives a random allocation) and the RNTI is a C-RNTI.
 * Monitor site: C-RNTI, proven fields (DCI 1_0 or a non-dl_auto 1_1) and no unsettled sweep hypothesis. Export: a record needs nvar > 0, a channel power > 0 and at least one RE. */
static inline bool nr_sense_wbg_consumer_ok(bool crc_ok, bool c_rnti) { return crc_ok && c_rnti; }
static inline bool nr_sense_wbg_monitor_ok(bool c_rnti, bool proven_fields, bool sweep_generation, bool sweep_settled) { return c_rnti && proven_fields && (!sweep_generation || sweep_settled); }
/* The tap itself (at tap time, not decode time): the TB CRC passed and the ring still holds the slot (credit), else the FEP would read another slot's IQ. */
static inline bool nr_sense_wbg_tap_ok(bool crc_ok, bool iq_ok) { return crc_ok && iq_ok; }
static inline bool nr_sense_wbg_export_ok(float nvar, double pw, uint32_t n_re) { return nvar > 0.f && pw > 0.0 && n_re > 0; }
/* After the unit gate of a tap site: unit_open (the gate returned true) -> 0, the unit's own tap runs and wbg takes nothing (no double export); else 1 = wbg admitted, -1 = none. */
int nr_sense_wbg_after_gate(bool unit_open, bool site_ok, uint16_t prbs, uint64_t t_ns);
/* Single sensing cell (operator 2026-11-06: no rationing where there is no contention). The scheduler publishes, at each decision, whether exactly one sensing cell is active (nr_sense_single_set).
 * While set (SENSING only): the PDSCH_DATA / PUSCH_DMRS / SRS unit gates are open (nr_sense_unit_enabled), the PDSCH_DMRS unit gate is closed so every proven C-RNTI grant of any width takes the grant-level
 * path, and nr_sense_wbg_admit has no width floor and no token bucket. Real overload still drops (queues, ring, workers) into the existing counters. >= 2 cells: all of this is off, the scheduler as before. */
void nr_sense_single_set(bool single);
bool nr_sense_single(void);
/* A TB is proven (its identity and layout verified) when the TB CRC passed, or, with more than one code block, at least one code block passed its CRC24B (C == 1 has no code-block CRC). */
static inline bool nr_sense_tb_proven(bool tb_crc_ok, unsigned cb_total, unsigned cb_ok) { return tb_crc_ok || (cb_total > 1 && cb_ok >= 1); }
/* Per-cell totals for the metrics (cumulative since start; survive row eviction). */
typedef struct {
  uint64_t violations, skips;                 /* drops inside enabled units / messages gated off */
  uint64_t wb_valid, wb_valid_prbs;           /* wideband CFR records (valid_cfr_prbs >= sense_min_prb) and the sum of their valid PRBs */
  uint64_t gap_max_ns;                        /* longest time between two wideband CFR records of the cell */
  uint32_t units_enabled;                     /* units enabled now */
} nr_sense_unit_cell_stats_t;
void nr_sense_unit_cell_stats(int cell, nr_sense_unit_cell_stats_t *out);
/* S9 continuity feed: the DCI arrivals of a cell (accepted DL and UL grants with their PRB count; independent of what is decoded or exported). nr_sense_dci_arrival records one for the calling
 * thread's cell (sensing mode only). nr_sense_unit_arrival_gaps: for each threshold thr[i] (PRBs), the arrivals of the last window_ns before now_ns with >= thr[i] PRBs: gaps are the times between
 * consecutive such arrivals, the window start and now included as ends (a silent window is one long gap); p95 = nearest rank. The ring keeps the last 4096 arrivals; when it has wrapped the window
 * shortens to its oldest arrival. n = arrivals counted, rate_hz = n / window. Scheduler thread (~80 KB of stack). */
void nr_sense_dci_arrival(uint16_t prbs, uint64_t t_ns);
typedef struct { uint64_t p95_gap_ns; float rate_hz; uint32_t n; } nr_sense_gapstat_t;
void nr_sense_unit_arrival_gaps(int cell, uint64_t now_ns, uint64_t window_ns, const int *thr, int n_thr, nr_sense_gapstat_t *out);
/* The PDSCH decode stage (LDPC of a grant) in SENSING mode: every wideband grant is decoded whatever its unit says, so this is a protected baseline, not a unit cost. Its CPU joins the
 * cell's unattributed (baseline) cost; its queue wait feeds the baseline of the decode queue (nr_sense_unit_take_decode_wait_max: longest wait since the previous take). */
void nr_sense_unit_decode_job(uint64_t cpu_ns, uint64_t wait_ns, uint64_t t_ns);
uint64_t nr_sense_unit_take_decode_wait_max(int cell);
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
void nr_sense_unit_cell_resync(int cell); /* production: the cell synchronised again / changed (SENSING only): unit_reset + T_cell back to sense_min_prb */
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
