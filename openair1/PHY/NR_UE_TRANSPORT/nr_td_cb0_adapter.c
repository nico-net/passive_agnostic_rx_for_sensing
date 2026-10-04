/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Engine adapter for the CB0 elimination channel: see nr_td_cb0_adapter.h. The ONLY runtime file that calls the
 * engine's CB0 API; the ELIM-fix merge adapts this file. */
#include "nr_td_cb0_adapter.h"
#include "nr_passive_cfg_epoch.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "nr_td_cb0_sched.h"

static _Atomic uint64_t s_alarms, s_elims;

void nr_td_cb0a_stats_reset(void)
{
  atomic_store(&s_alarms, 0);
  atomic_store(&s_elims, 0);
#ifdef NR_TD_CB0_X_COUNT
  nr_pdsch_config_sweep_cb0_stats_reset();
#endif
}

bool nr_td_cb0a_engine_wired(void)
{
#ifdef NR_TD_CB0_X_COUNT
  return true;
#else
  return false;
#endif
}

#ifdef NR_TD_CB0_X_COUNT
/* The runtime reasons 0..12 ARE the engine's NR_TD_CB0_X_* bits. */
_Static_assert(NR_TD_CB0_X_COUNT == 13 && NR_TD_CB0_R_ENGINE_MASK == 0x1FFFu, "CB0 reason bits");
_Static_assert(NR_TD_CB0_X_NOT_NEW_RV0 == 1u << NR_TD_CB0_R_NOT_NEW_RV0 && NR_TD_CB0_X_GATED == 1u << NR_TD_CB0_R_GATED
                   && NR_TD_CB0_X_IQ_STALE == 1u << NR_TD_CB0_R_IQ_STALE && NR_TD_CB0_X_LBRM == 1u << NR_TD_CB0_R_LBRM
                   && NR_TD_CB0_X_RV_RETRY == 1u << NR_TD_CB0_R_RV_RETRY && NR_TD_CB0_X_PRG_PTRS == 1u << NR_TD_CB0_R_PRG_PTRS
                   && NR_TD_CB0_X_MEMBER_STALE == 1u << NR_TD_CB0_R_MEMBER_STALE
                   && NR_TD_CB0_X_LLR_SCALE == 1u << NR_TD_CB0_R_LLR_SCALE && NR_TD_CB0_X_GPU_LLR == 1u << NR_TD_CB0_R_GPU_LLR
                   && NR_TD_CB0_X_LDPC_ERROR == 1u << NR_TD_CB0_R_LDPC_ERROR && NR_TD_CB0_X_RANK == 1u << NR_TD_CB0_R_RANK
                   && NR_TD_CB0_X_DECODER == 1u << NR_TD_CB0_R_DECODER && NR_TD_CB0_X_CONTRACT == 1u << NR_TD_CB0_R_CONTRACT,
               "CB0 reason bit positions");
/* G1 decoder codes are the engine's decoder codes. */
_Static_assert((int)NR_TD_DEC_CPU == 1 && (int)NR_TD_DEC_CUDA == 2, "decoder codes");
#endif

void nr_td_cb0a_stats(uint64_t *premise_alarms, uint64_t *eliminations)
{
  uint64_t a = atomic_load(&s_alarms); /* adapter-side check (diagnostic; identical rule on admissible batches) */
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
  return nr_pdsch_config_sweep_is_active(st, i); /* the engine's own predicate (ELIM honoured during fail-open) */
}

/* State access: in place, under the engine's lock (nr_pdsch_config_sweep_with_context), never a copy of the ~350 KB
 * state. The read of the active set copies only the active (index, hypothesis) pairs; the feed re-validates the set and
 * credits the grant in ONE lock section (the context cannot move between the check and the credit). */
static int count_elim(const nr_pdsch_config_sweep_state_t *st)
{
  int n = 0;
  for (int i = 0; i < st->n_hyp; i++)
    n += nr_pdsch_config_sweep_is_eliminated(st, i) ? 1 : 0;
  return n;
}

static void active_set_cb(nr_pdsch_config_sweep_state_t *st, void *arg)
{
  nr_td_cb0a_set_t *out = arg;
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
}

bool nr_td_cb0a_active_set(const nr_pdsch_sweep_ticket_t *t, nr_td_cb0a_set_t *out)
{
  if (t == NULL || out == NULL || out->idx == NULL || out->hyp == NULL)
    return false;
  return nr_pdsch_config_sweep_with_context(t, active_set_cb, out);
}

static bool same_hyp(const nr_pdsch_cfg_hypothesis_t *a, const nr_pdsch_cfg_hypothesis_t *b)
{
  return nr_td_cb0_hyp_key(a) == nr_td_cb0_hyp_key(b) && a->tda_start == b->tda_start && a->tda_length == b->tda_length
         && a->k0 == b->k0 && a->dmrs_mask == b->dmrs_mask && a->mcs_table == b->mcs_table;
}

typedef struct {
  const nr_td_cb0a_grant_t *g;
  nr_td_cb0a_feed_out_t *o;
} feed_arg_t;

static void feed_cb(nr_pdsch_config_sweep_state_t *st, void *arg)
{
  const feed_arg_t *a = arg;
  const nr_td_cb0a_grant_t *g = a->g;
  nr_td_cb0a_feed_out_t *o = a->o;
  /* Re-validate: the indices were read before the decodes; the context may have been pruned / re-indexed since. */
  bool moved = st->n_hyp != g->n_hyp_snapshot;
  for (int k = 0; k < g->n && !moved; k++)
    moved = g->idx[k] < 0 || g->idx[k] >= st->n_hyp || !same_hyp(&st->hyp[g->idx[k]], &g->hyp[k]);
  if (moved) {
    o->reindexed = true;
    return;
  }
#ifdef NR_TD_CB0_X_COUNT
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
  nr_pdsch_config_sweep_feed_cb0_grant(st, &eg);
  o->fed = true;
  const int e1 = count_elim(st);
  o->elim_delta = e1 > e0 ? e1 - e0 : 0;
#endif
}

bool nr_td_cb0a_feed(const nr_pdsch_sweep_ticket_t *t, const nr_td_cb0a_grant_t *g, nr_td_cb0a_feed_out_t *o)
{
  nr_td_cb0a_feed_out_t loc;
  if (o == NULL)
    o = &loc;
  memset(o, 0, sizeof(*o));
  if (t == NULL || g == NULL || !nr_cfg_epoch_work_current())
    return false;
  /* Premise (q >= p, runtime guard): on an admissible batch the scheduled hypothesis's CB0 must pass whenever its TB
   * passed (same IQ, same decoder or a dominating one). Checked here whether or not the engine is wired. */
  if (g->inadmissible == 0 && g->tb_hyp >= 0 && g->tb_pos >= 0 && g->tb_pos < g->n && g->tb_pass && !g->pass[g->tb_pos]) {
    o->premise_alarm = true;
    atomic_fetch_add(&s_alarms, 1);
  }
  const feed_arg_t a = {.g = g, .o = o};
  if (!nr_pdsch_config_sweep_with_context(t, feed_cb, (void *)&a)) {
    o->reindexed = true; /* stale ticket (context reset / re-keyed since the set was read): nothing is credited */
    return false;
  }
  if (o->elim_delta > 0)
    atomic_fetch_add(&s_elims, (uint64_t)o->elim_delta);
  return o->fed;
}

void nr_td_cb0a_note_tb_decoder(const nr_pdsch_sweep_ticket_t *t, int tb_hyp, uint8_t tb_decoder)
{
#ifdef NR_TD_CB0_X_COUNT
  /* The engine has no ticket form of note_tb_decoder; feed_cb0_grant records tb_decoder for any in-range tb_hyp and returns
   * before crediting when the batch is empty (n = 0), so an empty grant through the ticket form IS the note. */
  if (t == NULL || tb_hyp < 0)
    return;
  const nr_td_cb0_grant_t eg = {.idx = NULL, .pass = NULL, .n = 0, .cb0_decoder = 0, .tb_hyp = tb_hyp, .tb_pass = false,
                                .tb_decoder = tb_decoder, .inadmissible = 0};
  nr_pdsch_config_sweep_feedback_cb0(t, &eg);
#else
  (void)t;
  (void)tb_hyp;
  (void)tb_decoder;
#endif
}
