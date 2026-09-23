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
#include "common/utils/LOG/log.h"

/* OBSERVABILITY (2026-09-17). This module had NO logging at all, which made its central claim --
 * "a cell-wide prior is only published once two DISTINCT RNTIs converge on the same fields" --
 * unfalsifiable from a run log: a live no-regression test could not tell "did not promote" from
 * "never executed". Every log below marks a state TRANSITION (a context opening, converging, being
 * reopened, a prior published or withdrawn), so the volume is bounded by how often the receiver
 * actually learns something, not by grant rate. The one event that can be driven by noise RNTIs --
 * context eviction -- is rate limited. */

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

  /* Mapping-A catalog: EVERY legal (S, L) of TS 38.214 Table 5.1.2.1-1 (S 0..3, L 3..14, S+L <= 14),
   * not a curated prefix (the previous 8 pairs were the two lab cells' entries -- a deployment bias
   * the search must not carry). k0 (TDRA slot offset) is a TDRA-entry property like S/L and is
   * enumerated 0..2; the consumer decodes slot + k0. */
  uint8_t kSL[64][2];
  unsigned n_sl = 0;
  for (uint8_t S = 0; S <= 3; S++)
    for (uint8_t L = 3; S + L <= 14; L++) {
      kSL[n_sl][0] = S; kSL[n_sl][1] = L; n_sl++;
    }
  static const uint8_t kK0[]     = {0, 1}; /* k0 = 2 is not enumerated: 3024 raw entries would not fit the per-context state */
  static const uint8_t kAddPos[] = {0, 1, 2, 3};
  static const uint8_t kMaxLen[] = {1, 2};
  static const uint8_t kMcsTab[] = {0, 1, 2};

  for (unsigned a = 0; a < n_sl; a++) {
   for (unsigned e = 0; e < sizeof(kK0); e++) {
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
              if (h->tda_start == kSL[a][0] && h->tda_length == kSL[a][1] && h->k0 == kK0[e]
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
          h->k0           = kK0[e];
          h->dmrs_add_pos = kAddPos[b];
          h->dmrs_max_len = kMaxLen[c];
          h->mcs_table    = kMcsTab[d];
          st->order[st->n_hyp] = st->n_hyp;
          st->n_hyp++;
        }
      }
    }
   }
  }
  return st->n_hyp;
}

/* OBSERVED DM-RS SYMBOL MASK (2026-09-15). The DM-RS symbol pattern of a grant is directly
 * measurable (per-symbol DM-RS coherence over its PRBs), and it pins (S,L) x add_pos x max_len to the
 * one or two catalog entries whose effective mask matches -- leaving only mcs_table to the TB CRC.
 * Measured need: 809 live DCI layouts x ~233 hypotheses here = a joint space no probing converges on.
 * Nothing matched -> the catalog is left whole (the measurement may be wrong; a decode still can tell). */
int nr_pdsch_config_sweep_prune_mask(nr_pdsch_config_sweep_state_t *st, uint16_t dmrs_mask)
{
  if (st == NULL || st->n_hyp <= 0 || dmrs_mask == 0)
    return 0;
  nr_pdsch_cfg_hypothesis_t keep[NR_PDSCH_SWEEP_MAX_HYP];
  int n = 0;
  for (int i = 0; i < st->n_hyp; i++)
    if (st->hyp[i].dmrs_mask == dmrs_mask)
      keep[n++] = st->hyp[i];
  if (n <= 0 || n == st->n_hyp)
    return n == st->n_hyp ? n : 0;
  memcpy(st->hyp, keep, (size_t)n * sizeof(keep[0]));
  st->n_hyp = n;
  memset(st->trials, 0, sizeof(st->trials));
  memset(st->ok, 0, sizeof(st->ok));
  for (int i = 0; i < n; i++)
    st->order[i] = i;
  st->cursor = 0;
  st->winner = -1;
  return n;
}

/* DM-RS symbol masks the oracle has MEASURED on this cell (per-symbol coherence, independent of
 * any hypothesis). A new context is pruned to catalog entries producing one of them. Without this
 * the observation only reached the one context it was made on, and the layout rotation churns
 * contexts (keyed by RNTI x observed TDA index) faster than any of them walks past the add_pos 0
 * entries at the head of the catalog: OTA 2026-09-16, every PARMSET dmrsmask was 0x4 while the
 * oracle read 0x884 on every slot it looked at. */
#define OBS_MASKS_MAX 8
typedef struct {
  uint16_t mask[OBS_MASKS_MAX];
  int8_t   last[OBS_MASKS_MAX]; /* last PDSCH symbol measured with that mask, -1 = unknown */
  int8_t   k0[OBS_MASKS_MAX];   /* k0 of the job the mask was measured on, -1 = unknown */
  int n;
} obs_set_t;
typedef struct {
  bool valid;
  uint64_t configuration;
  uint8_t mcs_table, dmrs_add_pos, dmrs_max_len;
} prior_t;

/* PER-RNTI EVIDENCE. mcs-Table, dmrs-AdditionalPosition, maxLength and the DM-RS symbol set are
 * per-UE in the spec (dedicated RRC) and only cell-common in practice. Each RNTI therefore keeps its
 * own prior and observations, SEEDED from the cell-wide ones (the seed is what made sibling TDA
 * contexts converge 29x sooner -- see the CELL-WIDE PRIOR note in the header); the cell-wide ones
 * are PROMOTED only once two distinct RNTIs agree, so one UE with a private config can neither
 * poison the cell prior nor be forced onto it. Single-RNTI behaviour is unchanged by construction:
 * with one RNTI its own prior/observations are exactly what the cell-wide ones used to be, and the
 * (never-promoted) cell-wide ones are read only when the RNTI has nothing of its own. */
#define RNTI_CTX_MAX 16
typedef struct {
  uint16_t rnti;
  uint64_t touched;
  prior_t prior;
  obs_set_t obs;
} rnti_ctx_t;
static rnti_ctx_t g_rnti[RNTI_CTX_MAX];
static obs_set_t g_obs;   /* cell-wide: observations two RNTIs agree on */
static prior_t   g_prior; /* cell-wide: a prior two RNTIs converged on */
static uint64_t g_generation, g_clock;

/* ponytail: LRU by select order, no idle clock -- an active UE is touched every grant and so is
 * never the victim; only one-off noise-floor RNTIs churn. Add an idle-time floor if a real UE ever
 * gets evicted by a burst of >16 RNTIs inside one of its own grant gaps. */
static rnti_ctx_t *rnti_ctx(uint16_t rnti, bool create)
{
  int victim = 0;
  for (int i = 0; i < RNTI_CTX_MAX; i++) {
    if (g_rnti[i].rnti == rnti) {
      g_rnti[i].touched = ++g_clock;
      return &g_rnti[i];
    }
    if (g_rnti[i].touched < g_rnti[victim].touched)
      victim = i;
  }
  if (!create)
    return NULL;
  rnti_ctx_t *r = &g_rnti[victim];
  const uint16_t evicted = r->rnti;
  const bool had_prior = r->prior.valid;
  memset(r, 0, sizeof(*r));
  r->rnti = rnti;
  r->touched = ++g_clock;
  /* Rate limited: a burst of one-off noise-floor RNTIs churns this slot and must not flood. Losing
   * a context that had already CONVERGED is the case worth seeing, so it is logged separately and
   * louder -- that is the "a real UE got evicted by noise" failure the LRU comment warns about. */
  if (evicted && had_prior)
    LOG_W(PHY, "SWEEP: evicted CONVERGED context rnti=0x%04x to make room for rnti=0x%04x\n",
          evicted, rnti);
  else {
    static int s_left = 20;
    if (s_left > 0) {
      s_left--;
      LOG_I(PHY, "SWEEP: new per-RNTI context rnti=0x%04x%s%s\n", rnti,
            evicted ? " (evicted unconverged rnti=" : "", evicted ? "...)" : "");
    }
  }
  return r;
}

static int obs_find(const obs_set_t *o, uint16_t mask)
{
  for (int i = 0; i < o->n; i++)
    if (o->mask[i] == mask)
      return i;
  return -1;
}
/* Record (mask, last symbol, k0) into a set. A later, more specific observation refines the record; a
 * contradiction (a different last symbol under the same mask) relaxes it back to unknown: two TDRA
 * entries can share a mask. Returns the entry index, -1 when the set is full. */
static int obs_record(obs_set_t *o, uint16_t mask, int last_symbol, int k0)
{
  int k = obs_find(o, mask);
  if (k < 0 && o->n < OBS_MASKS_MAX) {
    k = o->n++;
    o->mask[k] = mask;
    o->last[k] = -1;
    o->k0[k] = -1;
  }
  if (k >= 0) {
    if (last_symbol >= 0) o->last[k] = (o->last[k] < 0 || o->last[k] == last_symbol) ? (int8_t)last_symbol : -1;
    if (k0 >= 0) o->k0[k] = (o->k0[k] < 0 || o->k0[k] == k0) ? (int8_t)k0 : -1;
  }
  return k;
}
/* An observation is (mask, last symbol, k0); an entry is consistent with it when its mask matches,
 * its S+L-1 equals the measured last symbol (when measured) and its k0 equals the job's (when the
 * mask was seen in the DCI's own slot the PDSCH is there: k0 of that job). */
static bool obs_admits(const nr_pdsch_cfg_hypothesis_t *h, const obs_set_t *o, int k)
{
  if (h->dmrs_mask != o->mask[k])
    return false;
  if (o->last[k] >= 0 && (int)h->tda_start + (int)h->tda_length - 1 != o->last[k])
    return false;
  if (o->k0[k] >= 0 && h->k0 != o->k0[k])
    return false;
  return true;
}
static bool obs_any_admits(const nr_pdsch_cfg_hypothesis_t *h, const obs_set_t *o)
{
  for (int k = 0; k < o->n; k++)
    if (obs_admits(h, o, k))
      return true;
  return false;
}
/* Keep the entries admitted by any observation of this RNTI or of the cell; untouched if none matches. */
static int prune_to_observed(nr_pdsch_config_sweep_state_t *st, const obs_set_t *own)
{
  if (st == NULL || st->n_hyp <= 0 || ((own ? own->n : 0) + g_obs.n) <= 0)
    return 0;
  nr_pdsch_cfg_hypothesis_t keep[NR_PDSCH_SWEEP_MAX_HYP];
  int n = 0;
  for (int i = 0; i < st->n_hyp; i++)
    if ((own && obs_any_admits(&st->hyp[i], own)) || obs_any_admits(&st->hyp[i], &g_obs))
      keep[n++] = st->hyp[i];
  if (n <= 0 || n == st->n_hyp)
    return n == st->n_hyp ? n : 0;
  memcpy(st->hyp, keep, (size_t)n * sizeof(keep[0]));
  st->n_hyp = n;
  memset(st->trials, 0, sizeof(st->trials));
  memset(st->ok, 0, sizeof(st->ok));
  for (int i = 0; i < n; i++)
    st->order[i] = i;
  st->cursor = 0;
  st->winner = -1;
  return n;
}

int nr_pdsch_config_sweep_prune_to(nr_pdsch_config_sweep_state_t *st, uint8_t mcs_table,
                                   uint8_t dmrs_add_pos, uint8_t dmrs_max_len)
{
  if (st == NULL || st->n_hyp <= 0) {
    return 0;
  }
  nr_pdsch_cfg_hypothesis_t keep[NR_PDSCH_SWEEP_MAX_HYP];
  int n = 0;
  for (int i = 0; i < st->n_hyp; i++) {
    const nr_pdsch_cfg_hypothesis_t *h = &st->hyp[i];
    if (h->mcs_table == mcs_table && h->dmrs_add_pos == dmrs_add_pos
        && h->dmrs_max_len == dmrs_max_len) {
      keep[n++] = *h;
    }
  }
  /* Nothing matched: the prior does not describe this catalog at all. Leave the full catalog in
   * place rather than emptying it -- a context with no hypotheses can never converge. */
  if (n <= 0) {
    return 0;
  }
  memcpy(st->hyp, keep, (size_t)n * sizeof(keep[0]));
  st->n_hyp = n;
  /* Evidence is per-index and the indices have just moved; keeping it would attribute one
   * hypothesis's trials to another. */
  memset(st->trials, 0, sizeof(st->trials));
  memset(st->ok, 0, sizeof(st->ok));
  for (int i = 0; i < n; i++) {
    st->order[i] = i;
  }
  st->cursor = 0;
  st->winner = -1;
  return n;
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
  /* EXPLOIT a hypothesis that has already passed a CRC: three trials in four go to the one with
   * the most passes, the fourth keeps the round-robin exploring. Pure round-robin spent 5/6 of
   * every probe on hypotheses already refuted by evidence, so a layout that hit once took ~1300
   * grants to hit again (rank-4 bed, 809 live layouts, 2026-09-16). Ties keep the lowest index. */
  int hot = -1;
  for (int i = 0; i < st->n_hyp; i++)
    if (st->ok[i] > 0 && (hot < 0 || st->ok[i] > st->ok[hot]))
      hot = i;
  /* Only until the hot one has the 64 trials the separation test needs; after that the fair
   * round-robin resumes so a marginal link (5 % true rate) still reaches SWEEP_MIN_TRIALS on
   * every hypothesis. */
  if (hot >= 0 && st->trials[hot] < 64 && (st->exploit_tick++ & 3) != 3) {
    *out = st->hyp[hot];
    return hot;
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
  /* Kept so a context pruned by the cell-wide prior can rebuild its full catalog without the
   * caller having to hand the legality function back. */
  nr_pdsch_legality_fn_t legality;
  enum { PRIORED_NONE = 0, PRIORED_OWN, PRIORED_CELL } priored; /* which prior pruned this catalog */
  nr_pdsch_config_sweep_state_t state;
} sweep_context_t;

/* Outcomes a pruned context may spend before the prior is judged wrong for it. ~250 per hypothesis
 * at the pruned width -- ample to clear SWEEP_MIN_RATE if the prior is right, and 35x cheaper than
 * the ~70,000 the full catalog costs if it is not. */
#define PRIOR_PROBATION 2000

static sweep_context_t g_contexts[NR_PDSCH_SWEEP_MAX_CONTEXTS];
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
  /* A reopen says this context's evidence is no longer trusted. If its catalog had been pruned by
   * the cell-wide prior, restore the full one: the prior is the most likely thing to be wrong when
   * a previously converged context starts failing. */
  if (c->priored && c->legality) {
    nr_pdsch_config_sweep_init_legal(&c->state, c->tda_count, c->typeA, c->legality);
    c->priored = PRIORED_NONE;
  }
  /* Keep the already checked legal catalog, but discard stale decoding evidence. */
  memset(c->state.trials, 0, sizeof(c->state.trials));
  memset(c->state.ok, 0, sizeof(c->state.ok));
  c->state.winner = -1;
  c->state.cursor = 0;
  for (int i=0; i<c->state.n_hyp; ++i) c->state.order[i] = i;
  /* A reopen is the signal that a CONVERGED context stopped working -- the most valuable thing in
   * this log, because it is how a wrong prior announces itself. reacquisitions rising steadily on
   * one RNTI means its catalog keeps being re-derived. */
  LOG_W(PHY,
        "SWEEP: rnti=0x%04x tda=%u REOPENED (reacquisition #%u, failure streak %u, "
        "locked %llu/%llu, reference lower bound %.3f)\n",
        c->rnti, (unsigned)c->tda, (unsigned)c->reacquisitions, (unsigned)report.failure_streak,
        (unsigned long long)report.passes, (unsigned long long)report.trials,
        report.reference_crc_lower);
  if (g_reporter) g_reporter(&report);
}

void nr_pdsch_config_sweep_set_reporter(nr_pdsch_sweep_reporter_t reporter)
{
  pthread_mutex_lock(&g_lock); g_reporter=reporter; pthread_mutex_unlock(&g_lock);
}

void nr_pdsch_config_sweep_prior_reset(void)
{
  pthread_mutex_lock(&g_lock);
  g_prior.valid = false;
  for (int i = 0; i < RNTI_CTX_MAX; i++)
    g_rnti[i].prior.valid = false;
  pthread_mutex_unlock(&g_lock);
}

static bool prior_get_locked(const prior_t *p, uint64_t *configuration, uint8_t *mcs_table,
                             uint8_t *dmrs_add_pos, uint8_t *dmrs_max_len)
{
  if (!p || !p->valid)
    return false;
  if (configuration) *configuration = p->configuration;
  if (mcs_table)     *mcs_table     = p->mcs_table;
  if (dmrs_add_pos)  *dmrs_add_pos  = p->dmrs_add_pos;
  if (dmrs_max_len)  *dmrs_max_len  = p->dmrs_max_len;
  return true;
}
bool nr_pdsch_config_sweep_prior_get(uint64_t *configuration, uint8_t *mcs_table,
                                     uint8_t *dmrs_add_pos, uint8_t *dmrs_max_len)
{
  pthread_mutex_lock(&g_lock);
  const bool v = prior_get_locked(&g_prior, configuration, mcs_table, dmrs_add_pos, dmrs_max_len);
  pthread_mutex_unlock(&g_lock);
  return v;
}
bool nr_pdsch_config_sweep_rnti_prior_get(uint16_t rnti, uint64_t *configuration, uint8_t *mcs_table,
                                          uint8_t *dmrs_add_pos, uint8_t *dmrs_max_len)
{
  pthread_mutex_lock(&g_lock);
  const rnti_ctx_t *r = rnti_ctx(rnti, false);
  const bool v = r && prior_get_locked(&r->prior, configuration, mcs_table, dmrs_add_pos, dmrs_max_len);
  pthread_mutex_unlock(&g_lock);
  return v;
}

static bool prior_same(const prior_t *a, const prior_t *b)
{
  return a->valid && b->valid && a->configuration == b->configuration && a->mcs_table == b->mcs_table
         && a->dmrs_add_pos == b->dmrs_add_pos && a->dmrs_max_len == b->dmrs_max_len;
}
/* Promote to the cell-wide prior once a SECOND distinct RNTI has converged on the same fields. The
 * first alone stays private: one UE's dedicated config is not evidence about the cell. */
static void prior_promote_locked(const rnti_ctx_t *just_set)
{
  if (g_prior.valid)
    return;
  for (int i = 0; i < RNTI_CTX_MAX; i++)
    if (g_rnti[i].rnti && g_rnti[i].rnti != just_set->rnti && prior_same(&g_rnti[i].prior, &just_set->prior)) {
      g_prior = just_set->prior;
      /* The load-bearing line: it names BOTH RNTIs, so "two distinct UEs agreed" is verifiable from
       * the log instead of asserted. A promotion that ever prints the same RNTI twice is a bug. */
      LOG_W(PHY,
            "SWEEP: cell prior PROMOTED by agreement rnti=0x%04x + rnti=0x%04x -- "
            "mcs_table=%u dmrs_add_pos=%u dmrs_max_len=%u cfg=0x%llx\n",
            just_set->rnti, g_rnti[i].rnti, (unsigned)g_prior.mcs_table,
            (unsigned)g_prior.dmrs_add_pos, (unsigned)g_prior.dmrs_max_len,
            (unsigned long long)g_prior.configuration);
      return;
    }
}

static sweep_context_t *ticket_context(const nr_pdsch_sweep_ticket_t *t)
{
  if (!t || !t->generation || t->context_slot >= NR_PDSCH_SWEEP_MAX_CONTEXTS)
    return NULL;
  sweep_context_t *c = &g_contexts[t->context_slot];
  /* generation+slot+tda identify the context (the slot's generation changes on every reuse). */
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
  rnti_ctx_t *r = rnti_ctx(rnti, true);
  int found = -1, victim = 0;
  for (int i = 0; i < NR_PDSCH_SWEEP_MAX_CONTEXTS; ++i) {
    sweep_context_t *c = &g_contexts[i];
    if (c->generation && c->configuration == configuration && c->rnti == rnti
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
    c->legality = legality;
    nr_pdsch_config_sweep_init_legal(&c->state, tda_count, typeA, legality);
    /* Seed: this RNTI's own prior first (its other TDA contexts already converged on these fields),
     * else the cell-wide one. Scoped to the same configuration key either way: a different cell
     * config is a different DM-RS/PDSCH setup and its prior says nothing here. */
    const prior_t *seed = NULL;
    int from = PRIORED_NONE;
    if (r->prior.valid && r->prior.configuration == configuration) {
      seed = &r->prior; from = PRIORED_OWN;
    } else if (g_prior.valid && g_prior.configuration == configuration) {
      seed = &g_prior; from = PRIORED_CELL;
    }
    if (seed && nr_pdsch_config_sweep_prune_to(&c->state, seed->mcs_table, seed->dmrs_add_pos,
                                               seed->dmrs_max_len) > 0)
      c->priored = from;
    prune_to_observed(&c->state, &r->obs);
  }
  sweep_context_t *c = &g_contexts[found];
  c->touched = ++g_clock;
  const int h = nr_pdsch_config_sweep_next(&c->state, out);
  if (h >= 0)
    *ticket = (nr_pdsch_sweep_ticket_t){.generation=c->generation, .context_slot=found,
                                       .rnti=rnti, .tda_index=tda_index, .hypothesis=h, .settled=c->state.winner >= 0,
                                       .k0=out->k0};
  pthread_mutex_unlock(&g_lock);
  return h >= 0;
}

int nr_pdsch_config_sweep_observe_mask(const nr_pdsch_sweep_ticket_t *ticket, uint16_t dmrs_mask)
{
  return nr_pdsch_config_sweep_observe(ticket, dmrs_mask, -1, -1);
}

int nr_pdsch_config_sweep_observe(const nr_pdsch_sweep_ticket_t *ticket, uint16_t dmrs_mask, int last_symbol, int k0)
{
  if (ticket == NULL || ticket->generation == 0 || dmrs_mask == 0)
    return 0;
  pthread_mutex_lock(&g_lock);
  rnti_ctx_t *r = rnti_ctx(ticket->rnti, true);
  obs_record(&r->obs, dmrs_mask, last_symbol, k0);
  /* Promote to the cell-wide set once a second distinct RNTI has seen the same mask. */
  if (obs_find(&g_obs, dmrs_mask) < 0) {
    for (int i = 0; i < RNTI_CTX_MAX; i++)
      if (g_rnti[i].rnti && g_rnti[i].rnti != r->rnti && obs_find(&g_rnti[i].obs, dmrs_mask) >= 0) {
        const int j = obs_find(&g_rnti[i].obs, dmrs_mask);
        obs_record(&g_obs, dmrs_mask, g_rnti[i].obs.last[j], g_rnti[i].obs.k0[j]);
        break;
      }
  }
  if (obs_find(&g_obs, dmrs_mask) >= 0)
    obs_record(&g_obs, dmrs_mask, last_symbol, k0);
  sweep_context_t *c = ticket_context(ticket);
  int n = 0;
  if (c != NULL && c->state.winner < 0)
    n = prune_to_observed(&c->state, &r->obs);
  pthread_mutex_unlock(&g_lock);
  return n;
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
    if (c->priored && c->state.winner < 0 && c->outcomes >= PRIOR_PROBATION) {
      double best_rate = 0.0;
      for (int i = 0; i < c->state.n_hyp; i++) {
        const double r = c->state.trials[i]
                             ? (double)c->state.ok[i] / (double)c->state.trials[i]
                             : 0.0;
        if (r > best_rate) {
          best_rate = r;
        }
      }
      if (best_rate < SWEEP_MIN_RATE) {
        /* The prior does not hold for this context. Restore the full search and stop applying the
         * prior that seeded it -- publishing it was the error, and leaving it valid would make every
         * later context pay the same probation. */
        if (c->legality) {
          nr_pdsch_config_sweep_init_legal(&c->state, c->tda_count, c->typeA, c->legality);
        }
        if (c->priored == PRIORED_CELL) {
          g_prior.valid = false;
          LOG_W(PHY, "SWEEP: cell prior WITHDRAWN -- it did not hold for rnti=0x%04x tda=%u "
                     "(best rate %.3f < %.3f)\n", c->rnti, (unsigned)c->tda, best_rate, SWEEP_MIN_RATE);
        } else {
          rnti_ctx_t *r = rnti_ctx(c->rnti, false);
          if (r)
            r->prior.valid = false;
          LOG_W(PHY, "SWEEP: rnti=0x%04x prior WITHDRAWN for tda=%u (best rate %.3f < %.3f)\n",
                c->rnti, (unsigned)c->tda, best_rate, SWEEP_MIN_RATE);
        }
        c->priored = PRIORED_NONE;
        c->outcomes = 0;
        w = -1;
      }
    }
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
      /* Publish the UE-wide fields so this RNTI's sibling TDA contexts do not re-derive them. Only
       * the first converged context of the RNTI publishes: later ones are already cheap, and
       * re-publishing would let a context that converged under a prior reinforce that same prior.
       * The cell-wide prior is only ever set by agreement between two RNTIs. */
      rnti_ctx_t *r = rnti_ctx(c->rnti, true);
      if (!r->prior.valid) {
        r->prior = (prior_t){.valid = true, .configuration = c->configuration,
                             .mcs_table = c->state.hyp[w].mcs_table,
                             .dmrs_add_pos = c->state.hyp[w].dmrs_add_pos,
                             .dmrs_max_len = c->state.hyp[w].dmrs_max_len};
        LOG_W(PHY,
              "SWEEP: rnti=0x%04x CONVERGED tda=%u mcs_table=%u dmrs_add_pos=%u dmrs_max_len=%u "
              "(%u/%u trials on the winner, cfg=0x%llx) -- private to this RNTI until a second agrees\n",
              c->rnti, (unsigned)c->tda, (unsigned)c->state.hyp[w].mcs_table,
              (unsigned)c->state.hyp[w].dmrs_add_pos, (unsigned)c->state.hyp[w].dmrs_max_len,
              c->state.ok[w], c->state.trials[w], (unsigned long long)c->configuration);
        prior_promote_locked(r);
      }
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
    /* tda 0xFF = every TDA context of this configuration: a layout hypothesis reads the TDA
     * index at its own offset, so its evidence is spread over the contexts that index created. */
    if (c->generation && c->configuration==configuration && c->rnti==rnti && (tda == 0xFF || c->tda==tda) && c->typeA==typeA) {
      for (int h=0; h<c->state.n_hyp; ++h) { *passes += c->state.ok[h]; *trials += c->state.trials[h]; }
      if (tda != 0xFF)
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
    if (c->generation && c->configuration==configuration && c->rnti==rnti
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
  /* Every lookup/feedback path requires a live generation. Invalidate the small
   * identity fields now; select() clears the full state before reusing a slot.
   * Clearing all 1024 hypothesis arrays here used ~1.2 ms even for an empty bank.
   * touched=0 makes invalid slots eligible for reuse ahead of live contexts. */
  for (int i=0; i<NR_PDSCH_SWEEP_MAX_CONTEXTS; ++i) {
    g_contexts[i].generation=0;
    g_contexts[i].touched=0;
  }
  /* Priors and observations are evidence derived from those contexts; keeping them across a reset
   * would let a cleared run inherit conclusions it can no longer justify. */
  memset(g_rnti, 0, sizeof(g_rnti));
  memset(&g_obs, 0, sizeof(g_obs));
  g_prior.valid = false;
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
