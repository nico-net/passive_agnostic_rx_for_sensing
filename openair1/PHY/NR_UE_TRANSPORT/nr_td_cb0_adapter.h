/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Thin adapter between the runtime CB0 wiring and the Technique D engine (nr_pdsch_config_sweep). The engine's CB0
 * API is being changed by the ELIM fix round (td/elim-channel: per-grant feed_cb0_grant with the scheduled
 * hypothesis's TB outcome, premise check, dominance rule, admissibility bits NR_TD_CB0_X_*). Everything the runtime
 * needs from the engine goes through this file, so the merge touches only nr_td_cb0_adapter.c.
 *   - the ACTIVE set of a live context (a consistent snapshot, read before any decode of the grant);
 *   - the per-grant feed (CB0 batch + the scheduled hypothesis's TB outcome + inadmissibility bits);
 *   - the TB-decoder note for a TB outcome fed without a CB0 grant;
 *   - counters (premise alarms, eliminations).
 * Engine API detection: the ELIM-fix header defines NR_TD_CB0_X_COUNT. Without it (base 4ca2ce2825) the engine has
 * no live-context CB0 entry and runtime contexts never enable cb0_elim, so nr_td_cb0a_engine_wired() is false and the
 * feed only runs the adapter's own premise check (TB PASS with CB0 FAIL on the scheduled hypothesis of an admissible
 * batch, logged TD_CB0_PREMISE_ALARM) -- nothing is credited and nothing is eliminated. */
#ifndef NR_TD_CB0_ADAPTER_H
#define NR_TD_CB0_ADAPTER_H
#include <stdbool.h>
#include <stdint.h>
#include "nr_pdsch_config_sweep.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NR_TD_CB0A_MAX_ACTIVE NR_PDSCH_SWEEP_MAX_HYP
typedef struct {
  int n_hyp;   /* catalogue size of the snapshot */
  int winner;  /* >= 0: settled */
  bool fail_open;
  int n_active;
  int n_elim;  /* hypotheses dormant through the CB0 elimination cause */
  int *idx;    /* n_active context indices (caller-owned, NR_TD_CB0A_MAX_ACTIVE) */
  nr_pdsch_cfg_hypothesis_t *hyp; /* their values (caller-owned, NR_TD_CB0A_MAX_ACTIVE) */
} nr_td_cb0a_set_t;
/* Snapshot of the ticket's live context. false: stale ticket / no context. */
bool nr_td_cb0a_active_set(const nr_pdsch_sweep_ticket_t *t, nr_td_cb0a_set_t *out);

typedef struct {
  const int *idx;                        /* context indices of the batch members with a verdict */
  const nr_pdsch_cfg_hypothesis_t *hyp;  /* their values as read by nr_td_cb0a_active_set (re-validated before crediting) */
  const bool *pass;
  int n;
  int n_hyp_snapshot;    /* catalogue size when the set was read */
  uint8_t cb0_decoder;   /* NR_TD_CB0_DEC_* */
  int tb_hyp;            /* the scheduled hypothesis (ticket->hypothesis), -1 = none */
  bool tb_pass;
  uint8_t tb_decoder;    /* NRLDPC_DECODER_* */
  int tb_pos;            /* position of tb_hyp in idx[], -1 = not in the batch */
  uint32_t inadmissible; /* NR_TD_CB0_R_* bits (nr_td_cb0_sched.h); 0..12 are the engine's NR_TD_CB0_X_* bits */
} nr_td_cb0a_grant_t;
typedef struct {
  bool fed;           /* the engine received the grant */
  bool reindexed;     /* the context moved since the set was read: nothing credited */
  bool premise_alarm; /* admissible batch, scheduled hypothesis TB PASS and CB0 FAIL */
  int elim_delta;     /* new eliminations in the context after the feed */
} nr_td_cb0a_feed_out_t;
bool nr_td_cb0a_feed(const nr_pdsch_sweep_ticket_t *t, const nr_td_cb0a_grant_t *g, nr_td_cb0a_feed_out_t *o);
/* A full-TB outcome fed without a CB0 grant (budget skip, no batch): the engine's dominance bookkeeping. */
void nr_td_cb0a_note_tb_decoder(const nr_pdsch_sweep_ticket_t *t, uint8_t tb_decoder);
/* true when the engine has the per-grant CB0 API (ELIM fix merged). */
bool nr_td_cb0a_engine_wired(void);
/* Process-wide: premise alarms seen by the adapter (or the engine once wired), eliminations observed. */
void nr_td_cb0a_stats(uint64_t *premise_alarms, uint64_t *eliminations);
/* The engine-defined "active" predicate on a state (adapter-local copy until the engine exports one). */
bool nr_td_cb0a_is_active(const nr_pdsch_config_sweep_state_t *st, int i);

#ifdef __cplusplus
}
#endif
#endif
