/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Engine adapter for the CB0 elimination channel: see nr_td_cb0_adapter.h. The ONLY runtime file that calls the
 * engine's CB0 API; the ELIM-fix merge adapts this file. */
#include "nr_td_cb0_adapter.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "nr_td_cb0_sched.h"

static _Atomic uint64_t s_alarms, s_elims;

bool nr_td_cb0a_engine_wired(void)
{
#ifdef NR_TD_CB0_X_COUNT
  return true;
#else
  return false;
#endif
}

void nr_td_cb0a_stats(uint64_t *premise_alarms, uint64_t *eliminations)
{
  uint64_t a = atomic_load(&s_alarms);
#ifdef NR_TD_CB0_X_COUNT
  uint64_t ea = 0;
  nr_pdsch_config_sweep_cb0_stats(&ea, NULL);
  if (ea > a)
    a = ea;
#endif
  if (premise_alarms)
    *premise_alarms = a;
  if (eliminations)
    *eliminations = atomic_load(&s_elims);
}

bool nr_td_cb0a_is_active(const nr_pdsch_config_sweep_state_t *st, int i)
{
  if (i < 0 || i >= st->n_hyp)
    return false;
  const uint64_t bit = 1ull << (i & 63);
  const int w = i >> 6;
  if (st->dormant[NR_TD_DORMANT_ELIM][w] & bit)
    return false; /* ELIM is honoured even while fail-open (ELIM fix semantics; base clears ELIM on fail-open) */
  if (st->fail_open)
    return true;
  for (int c = 0; c < NR_TD_DORMANT_CAUSES; c++)
    if (st->dormant[c][w] & bit)
      return false;
  return true;
}

/* One snapshot buffer per thread (the state is ~350 KB: never on the stack). */
static nr_pdsch_config_sweep_state_t *snap_buf(void)
{
  static __thread nr_pdsch_config_sweep_state_t *b;
  if (b == NULL)
    b = malloc(sizeof(*b));
  return b;
}

static int count_elim(const nr_pdsch_config_sweep_state_t *st)
{
  int n = 0;
  for (int i = 0; i < st->n_hyp; i++)
    n += nr_pdsch_config_sweep_is_eliminated(st, i) ? 1 : 0;
  return n;
}

bool nr_td_cb0a_active_set(const nr_pdsch_sweep_ticket_t *t, nr_td_cb0a_set_t *out)
{
  nr_pdsch_config_sweep_state_t *st = snap_buf();
  if (st == NULL || t == NULL || out == NULL || !nr_pdsch_config_sweep_snapshot(t, st))
    return false;
  out->n_hyp = st->n_hyp;
  out->winner = st->winner;
  out->fail_open = st->fail_open;
  out->n_active = 0;
  out->n_elim = count_elim(st);
  for (int i = 0; i < st->n_hyp; i++)
    if (nr_td_cb0a_is_active(st, i)) {
      out->idx[out->n_active] = i;
      out->hyp[out->n_active] = st->hyp[i];
      out->n_active++;
    }
  return true;
}

static bool same_hyp(const nr_pdsch_cfg_hypothesis_t *a, const nr_pdsch_cfg_hypothesis_t *b)
{
  return nr_td_cb0_hyp_key(a) == nr_td_cb0_hyp_key(b) && a->tda_start == b->tda_start && a->tda_length == b->tda_length
         && a->k0 == b->k0 && a->dmrs_mask == b->dmrs_mask && a->mcs_table == b->mcs_table;
}

bool nr_td_cb0a_feed(const nr_pdsch_sweep_ticket_t *t, const nr_td_cb0a_grant_t *g, nr_td_cb0a_feed_out_t *o)
{
  nr_td_cb0a_feed_out_t loc;
  if (o == NULL)
    o = &loc;
  memset(o, 0, sizeof(*o));
  if (t == NULL || g == NULL)
    return false;
  /* Premise (q >= p, runtime guard): on an admissible batch the scheduled hypothesis's CB0 must pass whenever its TB
   * passed (same IQ, same decoder or a dominating one). Checked here whether or not the engine is wired. */
  if (g->inadmissible == 0 && g->tb_hyp >= 0 && g->tb_pos >= 0 && g->tb_pos < g->n && g->tb_pass && !g->pass[g->tb_pos]) {
    o->premise_alarm = true;
    atomic_fetch_add(&s_alarms, 1);
  }
  /* Re-validate: the indices were read before the decodes; the context may have been pruned / re-indexed since. */
  nr_pdsch_config_sweep_state_t *st = snap_buf();
  if (st == NULL || !nr_pdsch_config_sweep_snapshot(t, st)) {
    o->reindexed = true; /* stale ticket (context reset / re-keyed since the set was read): nothing is credited */
    return false;
  }
  bool moved = st->n_hyp != g->n_hyp_snapshot;
  for (int k = 0; k < g->n && !moved; k++)
    moved = g->idx[k] < 0 || g->idx[k] >= st->n_hyp || !same_hyp(&st->hyp[g->idx[k]], &g->hyp[k]);
  if (moved) {
    o->reindexed = true;
    return false;
  }
#ifdef NR_TD_CB0_X_COUNT
  {
    const int e0 = count_elim(st);
    nr_td_cb0_grant_t eg = {.idx = g->idx,
                            .pass = g->pass,
                            .n = g->n,
                            .cb0_decoder = g->cb0_decoder,
                            .tb_hyp = g->tb_hyp,
                            .tb_pass = g->tb_pass,
                            .tb_decoder = g->tb_decoder,
                            .inadmissible = g->inadmissible & NR_TD_CB0_R_ENGINE_MASK};
    if (g->inadmissible & ~NR_TD_CB0_R_ENGINE_MASK) /* runtime-only reasons: still a rejected grant for the engine */
      eg.inadmissible |= NR_TD_CB0_X_LDPC_ERROR;
    o->fed = nr_pdsch_config_sweep_feedback_cb0(t, &eg);
    if (o->fed && nr_pdsch_config_sweep_snapshot(t, st)) {
      const int e1 = count_elim(st);
      o->elim_delta = e1 > e0 ? e1 - e0 : 0;
      atomic_fetch_add(&s_elims, (uint64_t)o->elim_delta);
    }
  }
#endif
  return o->fed;
}

void nr_td_cb0a_note_tb_decoder(const nr_pdsch_sweep_ticket_t *t, uint8_t tb_decoder)
{
  (void)t;
  (void)tb_decoder;
  /* ELIM fix: nr_pdsch_config_sweep_note_tb_decoder(state, decoder) has no live-context (ticket) form in the WIP API;
   * wire it here when the merged engine provides one (or feeds it from feedback()). */
}
