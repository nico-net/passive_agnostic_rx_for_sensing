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
#include <string.h>

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
  if (st == NULL) {
    return 0;
  }
  memset(st, 0, sizeof(*st));
  st->winner = -1;
  (void)tda_count;  /* The list LENGTH is pinned by Technique C's dci_length; only the CONTENTS of
                     * the entry actually referenced by the grants under test are searched here.
                     * Sweeping every entry of a multi-entry list would need traffic that exercises
                     * every index, which is not something a passive receiver can arrange. */

  /* TS 38.214 Table 5.1.2.1-1 shapes that a real deployment actually uses, plus this deployment's
   * own observed S=1/L=13. Enumerated rather than swept exhaustively over (S, L): most (S, L)
   * combinations are illegal for mapping type A (which requires S + L >= 2 and the DM-RS to fall
   * inside the allocation), and an illegal entry is rejected before it ever reaches a decode, so
   * spending trials on it wastes the round-robin budget. */
  static const uint8_t kSL[][2] = {
      {1, 13}, {0, 14}, {2, 12}, {1, 12}, {0, 13}, {2, 10}, {1, 7}, {0, 7},
  };
  static const uint8_t kAddPos[] = {0, 1, 2, 3};
  static const uint8_t kMaxLen[] = {1, 2};
  static const uint8_t kMcsTab[] = {1, 0, 2};  /* 256QAM first: this cell schedules MCS 24-25 */

  for (unsigned a = 0; a < sizeof(kSL) / sizeof(kSL[0]); a++) {
    for (unsigned b = 0; b < sizeof(kAddPos); b++) {
      for (unsigned c = 0; c < sizeof(kMaxLen); c++) {
        for (unsigned d = 0; d < sizeof(kMcsTab); d++) {
          if (st->n_hyp >= NR_PDSCH_SWEEP_MAX_HYP) {
            return st->n_hyp;
          }
          nr_pdsch_cfg_hypothesis_t *h = &st->hyp[st->n_hyp];
          h->tda_start    = kSL[a][0];
          h->tda_length   = kSL[a][1];
          h->dmrs_add_pos = kAddPos[b];
          h->dmrs_max_len = kMaxLen[c];
          h->mcs_table    = kMcsTab[d];
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
  const int idx = st->cursor;
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

/* ---- Process-wide singleton (see the header) --------------------------------------------------- */
static nr_pdsch_config_sweep_state_t g_sweep;
static int g_sweep_on = 0;

void nr_pdsch_config_sweep_enable_global(int tda_count)
{
  nr_pdsch_config_sweep_init(&g_sweep, tda_count);
  g_sweep_on = 1;
}

int nr_pdsch_config_sweep_next_global(nr_pdsch_cfg_hypothesis_t *out)
{
  return g_sweep_on ? nr_pdsch_config_sweep_next(&g_sweep, out) : -1;
}

int nr_pdsch_config_sweep_feed_global(int idx, bool tb_crc_ok)
{
  return g_sweep_on ? nr_pdsch_config_sweep_feed(&g_sweep, idx, tb_crc_ok) : -1;
}

int nr_pdsch_config_sweep_winner_global(void)
{
  return g_sweep_on ? nr_pdsch_config_sweep_winner(&g_sweep) : -1;
}

bool nr_pdsch_config_sweep_result_global(nr_pdsch_cfg_hypothesis_t *out)
{
  const int w = nr_pdsch_config_sweep_winner_global();
  if (w < 0 || out == NULL) {
    return false;
  }
  *out = g_sweep.hyp[w];
  return true;
}
