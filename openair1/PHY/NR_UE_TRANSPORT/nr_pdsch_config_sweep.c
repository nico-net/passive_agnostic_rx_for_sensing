/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 */

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c
 * \brief Phase 3 Technique D. See the header for why the TB CRC is the required oracle and why
 *        hypotheses are interleaved per grant rather than tested in blocks.
 */

#include "nr_pdsch_config_sweep.h"
#include "nr_crc_evidence.h"
#include <string.h>
#include <pthread.h>

/* Minimum trials before a hypothesis may be declared. At the measured working rate (~76 % TB CRC)
 * and a wrong-hypothesis rate of ~0, a few hundred trials is already overwhelming; this is set for
 * the case where the link itself is marginal and the true rate is only a few per cent. */
#define SWEEP_MIN_TRIALS 300
/* The winner must beat the runner-up by this ratio, not merely lead it. Two hypotheses that differ
 * only in a field the current traffic never exercises (e.g. a second TDRA entry the scheduler is
 * not using) will score IDENTICALLY -- and in that case the honest answer is "undecided", not a
 * coin flip between them. */
#define SWEEP_WIN_RATIO 3.0
/* Absolute floor: a hypothesis that decodes essentially nothing cannot win by ratio alone against
 * a runner-up that decodes nothing at all. */
#define SWEEP_MIN_RATE 0.02

int nr_pdsch_config_sweep_init(nr_pdsch_config_sweep_state_t *st, int tda_count)
{
  return nr_pdsch_config_sweep_init_legal(st, tda_count, 0, NULL);
}

int nr_pdsch_config_sweep_init_legal(nr_pdsch_config_sweep_state_t *st, int tda_count,
                                   int typeA, nr_pdsch_legality_fn_t legality)
{
  if (st == NULL) {
    return 0;
  }
  memset(st, 0, sizeof(*st));
  st->winner = -1;
  (void)tda_count; /* Contexts are isolated by the observed index; list width is not inferred here. */

  /* Supported mapping-A catalog. Enumerate all entries, never a deployment-biased prefix. */
  static const uint8_t kSL[][2] = {
      {1, 13}, {0, 14}, {2, 12}, {1, 12}, {0, 13}, {2, 10}, {1, 7}, {0, 7},
  };
  static const uint8_t kAddPos[] = {0, 1, 2, 3};
  static const uint8_t kMaxLen[] = {1, 2};
  static const uint8_t kMcsTab[] = {0, 1, 2};

  for (unsigned a = 0; a < sizeof(kSL) / sizeof(kSL[0]); a++) {
    for (unsigned b = 0; b < sizeof(kAddPos); b++) {
      for (unsigned c = 0; c < sizeof(kMaxLen); c++) {
        for (unsigned d = 0; d < sizeof(kMcsTab); d++) {
          int32_t mask = 0;
          if (legality) {
            mask = legality(typeA, kSL[a][1], kSL[a][0], 0, kAddPos[b], kMaxLen[c]);
            if (mask <= 0)
              continue;
            bool equivalent = false;
            for (int i = 0; i < st->n_hyp; ++i) {
              const nr_pdsch_cfg_hypothesis_t *h = &st->hyp[i];
              if (h->tda_start == kSL[a][0] && h->tda_length == kSL[a][1]
                  && h->dmrs_mask == mask && h->mcs_table == kMcsTab[d])
                equivalent = true;
            }
            if (equivalent)
              continue;
          }
          /* Fail closed if the catalog ever grows beyond its declared bound. */
          if (st->n_hyp >= NR_PDSCH_SWEEP_MAX_HYP) {
            st->n_hyp = 0;
            return 0;
          }
          nr_pdsch_cfg_hypothesis_t *h = &st->hyp[st->n_hyp];
          h->dmrs_mask = (uint16_t)mask;
          h->tda_start    = kSL[a][0];
          h->tda_length   = kSL[a][1];
          h->dmrs_add_pos = kAddPos[b];
          h->dmrs_max_len = kMaxLen[c];
          h->mcs_table    = kMcsTab[d];
          st->order[st->n_hyp] = st->n_hyp;
          st->n_hyp++;
        }
      }
    }
  }
  return st->n_hyp;
}

int nr_pdsch_config_sweep_next(nr_pdsch_config_sweep_state_t *st, nr_pdsch_cfg_hypothesis_t *out)
{
  if (st == NULL || out == NULL || st->n_hyp <= 0) {
    return -1;
  }
  if (st->winner >= 0) {
    *out = st->hyp[st->winner];
    return st->winner;
  }
  if (!st->cursor) nr_crc_shuffle(st->order, st->n_hyp, &st->random_state);
  const int idx = st->order[st->cursor];
  st->cursor = (st->cursor + 1) % st->n_hyp;
  *out = st->hyp[idx];
  return idx;
}

static double rate_of(const nr_pdsch_config_sweep_state_t *st, int i)
{
  return (st->trials[i] > 0) ? ((double)st->ok[i] / (double)st->trials[i]) : 0.0;
}

int nr_pdsch_config_sweep_feed(nr_pdsch_config_sweep_state_t *st, int idx, bool tb_crc_ok)
{
  if (st == NULL || idx < 0 || idx >= st->n_hyp) {
    return (st != NULL) ? st->winner : -1;
  }
  if (st->winner >= 0) {
    return st->winner;
  }
  st->trials[idx]++;
  if (tb_crc_ok) {
    st->ok[idx]++;
  }

  if ((st->trials[idx] % 16) == 0) {
    int leader=0;
    for(int i=1;i<st->n_hyp;i++)
      if(rate_of(st,i)>rate_of(st,leader)) leader=i;
    double lo,hi;
    nr_crc_interval(st->ok[leader],st->trials[leader],NR_PDSCH_SWEEP_MAX_HYP,&lo,&hi);
    /* The absolute floor was 0.60, which silently assumed the TRUE config decodes at >=60 %.
     * MEASURED OTA 2026-09-13: the winning hypothesis decodes at 124/311 = 40 %, so its Wilson
     * lower bound can never reach 0.60 -- early separation could NEVER fire on this link and every
     * acquisition paid the full fallback of SWEEP_MIN_TRIALS x n_hyp (~300 x 233 = 70,000 grants,
     * ~11 min). The pairwise test below is the one that actually carries the evidence: the leader's
     * lower bound must clear EVERY other hypothesis's upper bound. Keep only a token floor so a
     * dead link (everything near zero) cannot "separate", and let the separation test decide. */
    bool separated=st->trials[leader]>=64 && lo>=SWEEP_MIN_RATE;
    for(int i=0;i<st->n_hyp && separated;i++) {
      if(i==leader) continue;
      double other_lo,other_hi;
      nr_crc_interval(st->ok[i],st->trials[i],NR_PDSCH_SWEEP_MAX_HYP,&other_lo,&other_hi);
      if(other_hi>=lo) separated=false;
    }
    if(separated) { st->winner=leader; return leader; }
  }
  /* Decide only when EVERY hypothesis has had a fair shot -- otherwise the first one to reach the
   * threshold wins by being early in the rotation rather than by being right. */
  for (int i = 0; i < st->n_hyp; i++) {
    if (st->trials[i] < SWEEP_MIN_TRIALS) {
      return -1;
    }
  }
  int best = 0, second = -1;
  for (int i = 1; i < st->n_hyp; i++) {
    if (rate_of(st, i) > rate_of(st, best)) {
      best = i;
    }
  }
  for (int i = 0; i < st->n_hyp; i++) {
    if (i != best && (second < 0 || rate_of(st, i) > rate_of(st, second))) {
      second = i;
    }
  }
  const double rb = rate_of(st, best);
  const double rs = (second >= 0) ? rate_of(st, second) : 0.0;
  if (rb >= SWEEP_MIN_RATE && (rs <= 0.0 || rb >= SWEEP_WIN_RATIO * rs)) {
    st->winner = best;
  }
  return st->winner;
}

int nr_pdsch_config_sweep_winner(const nr_pdsch_config_sweep_state_t *st)
{
  return (st != NULL) ? st->winner : -1;
}

/* All shared accesses, including winner publication and reset, use one short mutex. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
  uint64_t configuration, generation, touched;
  uint16_t rnti;
  uint8_t tda;
  int tda_count, typeA;
  bool reported;
  uint64_t outcomes, locked_trials, locked_passes;
  uint64_t failure_streak, reacquisitions;
  double reference_crc_lower;
  nr_pdsch_config_sweep_state_t state;
} sweep_context_t;
static sweep_context_t g_contexts[NR_PDSCH_SWEEP_MAX_CONTEXTS];
static uint64_t g_generation, g_clock;
static nr_pdsch_sweep_reporter_t g_reporter;
static uint32_t g_recovery_minimum_failures = 32;
static double g_recovery_probability_budget = 1e-6;
bool nr_pdsch_config_sweep_set_recovery_policy(uint32_t minimum_failures, double probability_budget)
{
  if (!minimum_failures || !isfinite(probability_budget)
      || probability_budget <= 0 || probability_budget >= 1)
    return false;
  pthread_mutex_lock(&g_lock);
  g_recovery_minimum_failures = minimum_failures;
  g_recovery_probability_budget = probability_budget;
  pthread_mutex_unlock(&g_lock);
  return true;
}

/* Compare a run of failures with the frozen conservative rate at convergence.
 * Use summable budgets over feedback positions and generations, shared across
 * contexts. Correlated fading can also trigger this: only reopen local search,
 * never claim that CRC evidence alone identified a configuration change. */
static bool recovery_needed(const sweep_context_t *c)
{
  if (c->failure_streak < g_recovery_minimum_failures || c->reference_crc_lower <= 0)
    return false;
  const double n = (double)c->locked_trials, generation = (double)c->generation;
  const double budget = log(g_recovery_probability_budget) - log(NR_PDSCH_SWEEP_MAX_CONTEXTS)
                        - log(n + 1) - log(n + 2) - log(generation + 1) - log(generation + 2);
  return (double)c->failure_streak * log1p(-c->reference_crc_lower) <= budget;
}

static void reopen_context(sweep_context_t *c)
{
  const uint64_t previous = c->generation;
  nr_pdsch_sweep_report_t report = {
      .configuration=c->configuration, .rnti=c->rnti, .tda=c->tda,
      .outcomes=c->outcomes, .passes=c->locked_passes, .trials=c->locked_trials,
      .winner=-1, .invalidated=true, .previous_generation=previous,
      .generation=++g_generation, .reacquisitions=++c->reacquisitions,
      .failure_streak=c->failure_streak, .reference_crc_lower=c->reference_crc_lower};
  c->generation = report.generation;
  c->reported = false;
  c->outcomes = c->locked_trials = c->locked_passes = c->failure_streak = 0;
  c->reference_crc_lower = 0;
  /* Keep the already checked legal catalog, but discard stale decoding evidence. */
  memset(c->state.trials, 0, sizeof(c->state.trials));
  memset(c->state.ok, 0, sizeof(c->state.ok));
  c->state.winner = -1;
  c->state.cursor = 0;
  for (int i=0; i<c->state.n_hyp; ++i) c->state.order[i] = i;
  if (g_reporter) g_reporter(&report);
}

void nr_pdsch_config_sweep_set_reporter(nr_pdsch_sweep_reporter_t reporter)
{
  pthread_mutex_lock(&g_lock); g_reporter=reporter; pthread_mutex_unlock(&g_lock);
}

static sweep_context_t *ticket_context(const nr_pdsch_sweep_ticket_t *t)
{
  if (!t || !t->generation || t->context_slot >= NR_PDSCH_SWEEP_MAX_CONTEXTS)
    return NULL;
  sweep_context_t *c = &g_contexts[t->context_slot];
  /* Key is config+tda, NOT rnti: two UEs sharing a cell config + layout family carry the same
   * configuration key and refine one shared sweep (evidence pools -> converges N x faster with N UEs).
   * generation+slot+tda identify the context; the ticket rnti is informational only. */
  return c->generation == t->generation && c->tda == t->tda_index
         && t->hypothesis >= 0 && t->hypothesis < c->state.n_hyp ? c : NULL;
}

bool nr_pdsch_config_sweep_select(uint64_t configuration, uint16_t rnti, uint8_t tda_index,
                                 int tda_count, int typeA, nr_pdsch_legality_fn_t legality,
                                 nr_pdsch_sweep_ticket_t *ticket, nr_pdsch_cfg_hypothesis_t *out)
{
  if (ticket)
    memset(ticket, 0, sizeof(*ticket));
  if (!ticket || !out || !legality || !rnti || tda_index >= 16
      || tda_count < 0 || tda_count > 16 || (tda_count && tda_index >= tda_count))
    return false;
  pthread_mutex_lock(&g_lock);
  int found = -1, victim = 0;
  for (int i = 0; i < NR_PDSCH_SWEEP_MAX_CONTEXTS; ++i) {
    sweep_context_t *c = &g_contexts[i];
    if (c->generation && c->configuration == configuration
        && c->tda == tda_index && c->tda_count == tda_count && c->typeA == typeA) {
      found = i;
      break;
    }
    if (c->touched < g_contexts[victim].touched)
      victim = i;
  }
  if (found < 0) {
    found = victim;
    sweep_context_t *c = &g_contexts[found];
    memset(c, 0, sizeof(*c));
    c->configuration = configuration;
    c->generation = ++g_generation;
    c->rnti = rnti;
    c->tda = tda_index;
    c->tda_count = tda_count;
    c->typeA = typeA;
    nr_pdsch_config_sweep_init_legal(&c->state, tda_count, typeA, legality);
  }
  sweep_context_t *c = &g_contexts[found];
  c->touched = ++g_clock;
  const int h = nr_pdsch_config_sweep_next(&c->state, out);
  if (h >= 0)
    *ticket = (nr_pdsch_sweep_ticket_t){.generation=c->generation, .context_slot=found,
                                       .rnti=rnti, .tda_index=tda_index, .hypothesis=h, .settled=c->state.winner >= 0};
  pthread_mutex_unlock(&g_lock);
  return h >= 0;
}

bool nr_pdsch_config_sweep_feedback(const nr_pdsch_sweep_ticket_t *ticket, bool crc_ok,
                                   nr_pdsch_cfg_hypothesis_t *winner)
{
  pthread_mutex_lock(&g_lock);
  sweep_context_t *c = ticket_context(ticket);
  bool announced = false;
  if (c) {
    int w = nr_pdsch_config_sweep_feed(&c->state, ticket->hypothesis, crc_ok);
    ++c->outcomes;
    if (ticket->settled && ticket->hypothesis == w) {
      ++c->locked_trials; c->locked_passes += crc_ok;
      c->failure_streak = crc_ok ? 0 : c->failure_streak + 1;
      if (recovery_needed(c)) {
        reopen_context(c);
        pthread_mutex_unlock(&g_lock);
        return false;
      }

      if (g_reporter && c->locked_trials % 1000 == 0) {
        nr_pdsch_sweep_report_t r={.configuration=c->configuration,.rnti=c->rnti,.tda=c->tda,
          .operational=true,.passes=c->locked_passes,.trials=c->locked_trials};
        g_reporter(&r);
      }
    }
    if (c->outcomes % 10000 == 0 || (crc_ok && !c->reported)) {
      const nr_pdsch_config_sweep_state_t *s=&c->state;
      int best=0; uint32_t minimum=UINT32_MAX;
      for(int i=0;i<s->n_hyp;i++) {
        if(s->trials[i]<minimum) minimum=s->trials[i];
        if((double)s->ok[i]/(s->trials[i]?s->trials[i]:1)
            >(double)s->ok[best]/(s->trials[best]?s->trials[best]:1)) best=i;
      }
      const nr_pdsch_cfg_hypothesis_t *h=&s->hyp[best];
      if (g_reporter) {
        nr_pdsch_sweep_report_t r={.configuration=c->configuration,.rnti=c->rnti,.tda=c->tda,
          .outcomes=c->outcomes,.minimum=minimum,.passes=s->ok[best],.trials=s->trials[best],
          .hypothesis=*h,.winner=w};
        g_reporter(&r);
      }
    }
    if (w >= 0 && !c->reported) {
      c->reported = true;
      double reference_upper;
      nr_crc_interval(c->state.ok[w], c->state.trials[w], NR_PDSCH_SWEEP_MAX_HYP,
                      &c->reference_crc_lower, &reference_upper);

      announced = true;
      if (winner)
        *winner = c->state.hyp[w];
    }
  }
  pthread_mutex_unlock(&g_lock);
  return announced;
}

void nr_pdsch_config_sweep_context_stats(uint64_t configuration, uint16_t rnti, uint8_t tda, int typeA,
                                         uint32_t *passes, uint32_t *trials)
{
  *passes = *trials = 0;
  pthread_mutex_lock(&g_lock);
  for (int i=0;i<NR_PDSCH_SWEEP_MAX_CONTEXTS;++i) {
    const sweep_context_t *c=&g_contexts[i];
    if (c->generation && c->configuration==configuration && c->tda==tda && c->typeA==typeA) {
      for (int h=0; h<c->state.n_hyp; ++h) { *passes += c->state.ok[h]; *trials += c->state.trials[h]; }
      break;
    }
  }
  pthread_mutex_unlock(&g_lock);
}
bool nr_pdsch_config_sweep_is_settled(uint64_t configuration, uint16_t rnti, uint8_t tda, int typeA)
{
  pthread_mutex_lock(&g_lock);
  bool settled=false;
  for (int i=0;i<NR_PDSCH_SWEEP_MAX_CONTEXTS;++i) {
    const sweep_context_t *c=&g_contexts[i];
    if (c->generation && c->configuration==configuration
        && c->tda==tda && c->tda_count==0 && c->typeA==typeA && c->state.winner>=0) {
      settled=true;
      break;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return settled;
}
/* Diagnostic only: how many live keyed contexts currently hold a winner. Read by the acquisition
 * state tracker (nr_passive_acq_state.c) at the RT periodic summary; not a decision input. */
int nr_pdsch_config_sweep_settled_count(void)
{
  pthread_mutex_lock(&g_lock);
  int n=0;
  for (int i=0;i<NR_PDSCH_SWEEP_MAX_CONTEXTS;++i)
    if (g_contexts[i].generation && g_contexts[i].state.winner>=0) ++n;
  pthread_mutex_unlock(&g_lock);
  return n;
}

void nr_pdsch_config_sweep_reset_all(void)
{
  pthread_mutex_lock(&g_lock);
  memset(g_contexts, 0, sizeof(g_contexts));
  /* Do not rewind generation: in-flight jobs from before reset must remain invalid. */
  pthread_mutex_unlock(&g_lock);
}

bool nr_pdsch_config_sweep_snapshot(const nr_pdsch_sweep_ticket_t *ticket,
                                   nr_pdsch_config_sweep_state_t *out)
{
  if (!out)
    return false;
  pthread_mutex_lock(&g_lock);
  sweep_context_t *c = ticket_context(ticket);
  if (c)
    *out = c->state;
  pthread_mutex_unlock(&g_lock);
  return c != NULL;
}

/* Compatibility for pure legacy tests; not used by the receive pipeline. */
static nr_pdsch_config_sweep_state_t g_sweep;
static bool g_sweep_on;
void nr_pdsch_config_sweep_enable_global(int tda_count)
{
  pthread_mutex_lock(&g_lock);
  nr_pdsch_config_sweep_init(&g_sweep, tda_count);
  g_sweep_on = true;
  pthread_mutex_unlock(&g_lock);
}
int nr_pdsch_config_sweep_next_global(nr_pdsch_cfg_hypothesis_t *out)
{
  pthread_mutex_lock(&g_lock);
  int r = g_sweep_on ? nr_pdsch_config_sweep_next(&g_sweep, out) : -1;
  pthread_mutex_unlock(&g_lock);
  return r;
}
int nr_pdsch_config_sweep_feed_global(int idx, bool tb_crc_ok)
{
  pthread_mutex_lock(&g_lock);
  int r = g_sweep_on ? nr_pdsch_config_sweep_feed(&g_sweep, idx, tb_crc_ok) : -1;
  pthread_mutex_unlock(&g_lock);
  return r;
}
int nr_pdsch_config_sweep_winner_global(void)
{
  pthread_mutex_lock(&g_lock);
  int r = g_sweep_on ? g_sweep.winner : -1;
  pthread_mutex_unlock(&g_lock);
  return r;
}
bool nr_pdsch_config_sweep_result_global(nr_pdsch_cfg_hypothesis_t *out)
{
  pthread_mutex_lock(&g_lock);
  bool ok = out && g_sweep_on && g_sweep.winner >= 0;
  if (ok)
    *out = g_sweep.hyp[g_sweep.winner];
  pthread_mutex_unlock(&g_lock);
  return ok;
}
