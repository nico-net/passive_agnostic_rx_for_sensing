/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Runtime wiring of the CB0 elimination channel into the passive PDSCH consumer (td/cb0-cpu-wiring, 2026-10-03).
 * ISAC_TD_CB0_ELIM=1 (engine flag, default 0) turns it on for Technique D contexts that are ACQUIRING (a ticket with a
 * generation, not settled, not a layout probe). Flag off: every entry point returns at its first test and the
 * receiver is unchanged (no snapshot, no decode, no log, no TB-decoder change).
 *
 * Per grant, on the consumer (job) thread:
 *   nr_td_cb0_wire_pre()  BEFORE the main decode: reads the context's active set, fixes the CB0 hypothesis set
 *                         (nr_td_cb0_sched.h hash subset + the scheduled hypothesis, only hypotheses with the job's k0:
 *                         one GrantWork = one PDSCH slot), admits it against the token bucket (or skips it whole), and
 *                         says whether the TB must be decoded on the CPU (ISAC_TD_TB_CPU_WHILE_ACQ, default 1).
 *   nr_td_cb0_wire_run()  AFTER the main decode, before nr_td_grantwork_job_end(): builds the items from GrantWork
 *                         (lazy entries on this thread), retains the gw, runs nr_td_cb0_exec() synchronously, releases.
 *   nr_td_cb0_wire_feed() AFTER the grant's TB feedback (and before anything that may re-index the context): the
 *                         admissibility decision (nr_td_cb0_admissibility) and the feed through nr_td_cb0_adapter.
 * Requires ISAC_TD_GRANTWORK=1: without it the channel refuses (logged once, reason no_grantwork).
 * Environment (read once): ISAC_TD_CB0_BUDGET_US (10000), ISAC_TD_CB0_CPU_PCT (30), ISAC_TD_CB0_THREADS (8),
 * ISAC_TD_CB0_RANK_MAX (4), ISAC_TD_TB_CPU_WHILE_ACQ (1), ISAC_TD_CB0_BACKEND (auto). */
#ifndef NR_TD_CB0_WIRE_H
#define NR_TD_CB0_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nr_pdsch_config_sweep.h"
#include "nr_td_cb0_batch.h"
#include "nr_td_cb0_sched.h"
#include "nr_td_grantwork.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ---- provided by nr_pdsch_passive_decode.c (declared here so the glue does not need the PHY headers) ---- */
int nr_pdsch_passive_gw_cb0_item(nr_td_grantwork_t *gw, const nr_pdsch_cfg_hypothesis_t *h, nr_td_cb0_item_t *it,
                                 uint32_t *flags);
void *nr_pdsch_passive_cb0_cpu_ldpc(void);
void nr_pdsch_passive_force_cpu_tb(bool on);

#define NR_TD_CB0_PLAN_MAX 512 /* items per grant (B is capped by it) */

typedef struct {
  int64_t abs_slot;  /* the grant's IQ slot (hash input) */
  uint8_t job_k0;    /* the job's k0 (= its GrantWork's PDSCH slot) */
  bool layout_probe; /* a DCI-layout probe job (CB0-only main decode): not a Technique D TB grant */
  bool gpu_job;      /* NR_GPU_FEP job: LLRs from the GPU front end */
  bool gw_on;        /* ISAC_TD_GRANTWORK=1 */
} nr_td_cb0_job_t;
/* Returns true when the grant is an acquiring Technique D grant with the channel on (then call _run and _feed);
 * *tb_cpu = force the CPU TB decoder for the main decode. */
bool nr_td_cb0_wire_pre(const nr_pdsch_sweep_ticket_t *t, nr_td_grantwork_t *gw, const nr_td_cb0_job_t *job, bool *tb_cpu);
void nr_td_cb0_wire_run(nr_td_grantwork_t *gw);
typedef struct {
  bool tb_fed;        /* the TB path fed this grant's TB outcome to the sweep */
  bool tb_pass;
  uint8_t tb_decoder; /* nr_pdsch_passive_last_decoder_used() of the main decode */
  bool iq_ok_after;   /* nr_passive_credit_allowed() after the main decode and the batch */
  uint8_t rv;
  int nl;
} nr_td_cb0_tb_t;
/* Idempotent per grant (the second call of a grant is a no-op). */
void nr_td_cb0_wire_feed(const nr_pdsch_sweep_ticket_t *t, const nr_td_cb0_tb_t *tb);
/* " cb0_grants=.. cb0_adm=.. cb0_items=.. cb0_elim=.. cb0_alarms=.." for the ticket's context; "" when the flag is off. */
const char *nr_td_cb0_wire_converged_suffix(const nr_pdsch_sweep_ticket_t *t);

typedef struct {
  uint64_t grants;     /* acquiring grants seen with the channel on */
  uint64_t batches;    /* batches run */
  uint64_t admissible; /* grants fed to the engine as admissible */
  uint64_t items;      /* CB0 decodes with a verdict */
  uint64_t inadmissible[NR_TD_CB0_R_COUNT];
  uint64_t budget_skips;
  uint64_t not_testable; /* members dropped before decoding: another k0 / parameter-determined (reserved MCS, failed geometry) */
  uint64_t cpu_us;       /* CPU-us spent in batches (wall * threads used) */
  uint64_t backend_cpu, backend_gpu;
  uint64_t premise_alarms, eliminations;
} nr_td_cb0_wire_stats_t;
void nr_td_cb0_wire_stats(nr_td_cb0_wire_stats_t *s);
/* Test hook: reset counters, scheduler and per-context table; re-read the environment at the next grant. */
void nr_td_cb0_wire_reset(void);
/* Test hooks: freeze the cost estimates; this thread's last planned set (context indices; -1 = no active plan) and its
 * pre-decode reasons. */
void nr_td_cb0_wire_test_freeze(bool on);
int nr_td_cb0_wire_test_last_set(int *idx, int max);
uint32_t nr_td_cb0_wire_test_last_reasons(void);
/* Test hooks: admit every grant (bucket off); set the bucket's tokens (CPU-us, capped at its capacity). */
void nr_td_cb0_wire_test_unlimited(bool on);
void nr_td_cb0_wire_test_set_tokens(double us);

#ifdef __cplusplus
}
#endif
#endif
