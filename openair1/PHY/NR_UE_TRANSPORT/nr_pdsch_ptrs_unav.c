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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_ptrs_unav.c */

#include "nr_pdsch_ptrs_unav.h"

/* K in {2,4}, L in {1,2,4}: TS 38.214 Tables 5.1.6.3-1 (time) and 5.1.6.3-2 (frequency). Ordered
 * densest-first so the most common configurations are tried early in a round-robin sweep. */
const uint8_t nr_ptrs_density_k[NR_PTRS_DENSITY_N] = {2, 2, 2, 4, 4, 4};
const uint8_t nr_ptrs_density_l[NR_PTRS_DENSITY_N] = {1, 2, 4, 1, 2, 4};

uint32_t nr_pdsch_ptrs_unav_res(uint16_t nb_rb, uint8_t start_symbol, uint8_t nb_symbols,
                                uint16_t dmrs_symb_pos, uint8_t k_density, uint8_t l_density,
                                uint8_t n_ports)
{
  if (nb_rb == 0 || nb_symbols == 0 || n_ports == 0
      || (k_density != 2 && k_density != 4)
      || (l_density != 1 && l_density != 2 && l_density != 4)) {
    return 0;
  }
  /* One PT-RS subcarrier per K resource blocks. A partial group still carries one, hence the
   * ceiling -- dropping it would under-count G on any allocation that is not a multiple of K. */
  const uint32_t sc_per_symbol = ((uint32_t)nb_rb + k_density - 1u) / k_density;

  /* Walk the allocation and count the symbols that actually carry PT-RS. The L-th-symbol cadence
   * restarts after each DM-RS symbol rather than running blindly from the allocation start: PT-RS
   * is not mapped on a DM-RS symbol, and the phase reference is re-established there. Counting
   * with a blind stride would mis-count exactly on the configurations with more than one DM-RS
   * symbol -- which is every additionalPosition > 0 cell. */
  uint32_t symbols = 0;
  int since_ref = -1;   /* -1 until the first DM-RS symbol establishes the reference */
  for (int s = start_symbol; s < start_symbol + nb_symbols; s++) {
    const bool is_dmrs = (s >= 0 && s < 16) && ((dmrs_symb_pos >> s) & 0x1u);
    if (is_dmrs) {
      since_ref = 0;    /* cadence restarts after the DM-RS */
      continue;
    }
    if (since_ref < 0) {
      /* Data symbols before any DM-RS carry no phase reference to track. */
      continue;
    }
    if ((since_ref % l_density) == 0) {
      symbols++;
    }
    since_ref++;
  }
  return sc_per_symbol * symbols * (uint32_t)n_ports;
}

/* ---- PT-RS density sweep ---------------------------------------------------------------------- */
#define PTRS_LATCH_MIN_OK 8
#include <math.h>
#include <string.h>
static void ptrs_wilson(uint32_t ok, uint32_t n, double *lo, double *hi)
{
  if (n == 0) { *lo = 0.0; *hi = 1.0; return; }
  const double z = 1.96, nn = (double)n, p = (double)ok / nn, d = 1.0 + z * z / nn;
  const double c = p + z * z / (2.0 * nn), q = z * sqrt(p * (1.0 - p) / nn + z * z / (4.0 * nn * nn));
  *lo = (c - q) / d; *hi = (c + q) / d;
  if (*lo < 0.0) *lo = 0.0;
  if (*hi > 1.0) *hi = 1.0;
}
void nr_ptrs_sweep_init(nr_ptrs_sweep_t *s)
{
  memset(s, 0, sizeof(*s));
  s->latched = -1;
}
int nr_ptrs_sweep_pick(const nr_ptrs_sweep_t *s)
{
  if (s->latched >= 0)
    return s->latched;
  /* RESOLVE THE BLOCKER. Plain upper-bound selection stops sampling a rival once its upper bound
   * falls below the LEADER'S upper bound -- but latching needs it below the leader's LOWER bound, so
   * at a low true decode rate the two meet in the middle and nothing ever latches (measured in the
   * unit test: 5000 grants at 30 %, no decision). Once the leader has enough successes to latch,
   * spend grants on whichever rival is still in the way until it is ruled out. */
  int lead = -1;
  double lead_p = -1.0;
  for (int a = 0; a < NR_PTRS_ARMS; a++)
    if (s->tr[a] > 0 && (double)s->ok[a] / (double)s->tr[a] > lead_p) {
      lead_p = (double)s->ok[a] / (double)s->tr[a];
      lead = a;
    }
  if (lead >= 0 && s->ok[lead] >= PTRS_LATCH_MIN_OK) {
    double llo, lhi;
    ptrs_wilson(s->ok[lead], s->tr[lead], &llo, &lhi);
    int block = -1;
    double block_hi = -1.0;
    for (int a = 0; a < NR_PTRS_ARMS; a++) {
      if (a == lead)
        continue;
      double lo, hi;
      ptrs_wilson(s->ok[a], s->tr[a], &lo, &hi);
      if (hi >= llo && hi > block_hi) {
        block_hi = hi;
        block = a;
      }
    }
    if (block >= 0)
      return block;
    return lead;
  }
  int arg = 0;
  double best = -1.0;
  for (int a = 0; a < NR_PTRS_ARMS; a++) {
    double lo, hi;
    ptrs_wilson(s->ok[a], s->tr[a], &lo, &hi);
    /* Tie-break toward fewer trials so every arm gets its first looks in order. */
    if (hi > best + 1e-12 || (fabs(hi - best) <= 1e-12 && s->tr[a] < s->tr[arg])) {
      best = hi;
      arg = a;
    }
  }
  return arg;
}
int nr_ptrs_sweep_feed(nr_ptrs_sweep_t *s, int arm, bool tb_ok)
{
  if (arm < 0 || arm >= NR_PTRS_ARMS || s->latched >= 0)
    return s->latched;
  s->tr[arm]++;
  if (tb_ok)
    s->ok[arm]++;
  if (s->ok[arm] < PTRS_LATCH_MIN_OK)
    return -1;
  double lo, hi;
  ptrs_wilson(s->ok[arm], s->tr[arm], &lo, &hi);
  for (int a = 0; a < NR_PTRS_ARMS; a++) {
    if (a == arm)
      continue;
    double lo2, hi2;
    ptrs_wilson(s->ok[a], s->tr[a], &lo2, &hi2);
    if (hi2 >= lo)
      return -1;
  }
  s->latched = arm;
  return arm;
}
bool nr_ptrs_sweep_arm(int arm, uint8_t *K, uint8_t *L)
{
  static const uint8_t kK[NR_PTRS_ARMS] = {0, 2, 2, 2, 4, 4, 4};
  static const uint8_t kL[NR_PTRS_ARMS] = {0, 1, 2, 4, 1, 2, 4};
  if (arm <= 0 || arm >= NR_PTRS_ARMS)
    return false;
  *K = kK[arm];
  *L = kL[arm];
  return true;
}
