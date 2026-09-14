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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.c
 * \brief Pure core of the blind NZP CSI-RS search. See the header for why it exists and why the
 *        reference sequence must come from the real generator rather than be re-derived.
 */

#include "nr_csirs_blind_search.h"

#include <math.h>
#include <stdio.h>

const uint16_t nr_csirs_blind_periods[NR_CSIRS_BLIND_N_PERIODS] = {
    4, 5, 8, 10, 16, 20, 32, 40, 64, 80, 160, 320, 640};

double nr_csirs_blind_correlate(const int16_t *rx_re_im, const int16_t *ref_re_im, int n)
{
  if (rx_re_im == NULL || ref_re_im == NULL || n <= 0) {
    return -1.0;
  }
  /* Accumulate the complex inner product and both energies in double: at 273 RB a density-3 row
   * carries thousands of REs, and an int32 accumulator of int16 products overflows well before
   * that (the same wrapping-accumulator class of bug already found in the MRC path). */
  double acc_r = 0.0, acc_i = 0.0, e_rx = 0.0, e_ref = 0.0;
  int used = 0;
  for (int i = 0; i < n; i++) {
    const double xr = (double)ref_re_im[2 * i], xi = (double)ref_re_im[2 * i + 1];
    if (xr == 0.0 && xi == 0.0) {
      continue; /* the reference does not occupy this RE -- scoring it would dilute the oracle */
    }
    const double yr = (double)rx_re_im[2 * i], yi = (double)rx_re_im[2 * i + 1];
    /* <y, x> = sum y * conj(x) */
    acc_r += yr * xr + yi * xi;
    acc_i += yi * xr - yr * xi;
    e_rx += yr * yr + yi * yi;
    e_ref += xr * xr + xi * xi;
    used++;
  }
  if (used == 0 || e_rx <= 0.0 || e_ref <= 0.0) {
    /* An unoccupied reference, or a dead slot. -1 is NOT 0: a candidate that cannot be scored must
     * not rank alongside one that was scored and genuinely mismatched. */
    return -1.0;
  }
  const double num = sqrt(acc_r * acc_r + acc_i * acc_i);
  const double den = sqrt(e_rx) * sqrt(e_ref);
  const double rho = num / den;
  return (rho > 1.0) ? 1.0 : rho; /* rounding can push a perfect match a hair over 1 */
}

bool nr_csirs_blind_infer_period(const uint32_t *hit_slots, int n_hits, int min_hits,
                                 uint16_t *period, uint16_t *offset)
{
  if (hit_slots == NULL || n_hits < 2 || min_hits < 2 || n_hits < min_hits) {
    return false;
  }
  uint32_t lo = hit_slots[0], hi = hit_slots[0];
  for (int i = 1; i < n_hits; i++) {
    if (hit_slots[i] < lo) lo = hit_slots[i];
    if (hit_slots[i] > hi) hi = hit_slots[i];
  }
  const uint32_t span = hi - lo;
  if (span == 0) {
    return false; /* every hit in one slot says nothing about periodicity */
  }
  /* LARGEST legal periodicity that explains every hit, searched downwards.
   *
   * Largest, not smallest: every DIVISOR of the true period also satisfies "same slot mod P", so
   * hits at period 20 are equally consistent with 4, 5 and 10, and a smallest-first rule returns 4
   * every time. A period LONGER than the truth is instead rejected outright, because its hits fall
   * at different phases. So the true period is the largest consistent one.
   *
   * Constraining to the 38.331 set is what makes this robust to a MISSED occurrence: a gap of 2P
   * still satisfies "same slot mod P", whereas a GCD over the deltas is thrown by it (and by any
   * single spurious hit). */
  for (int p = NR_CSIRS_BLIND_N_PERIODS - 1; p >= 0; p--) {
    const uint32_t P = nr_csirs_blind_periods[p];
    /* A period longer than the whole observation cannot be distinguished from "one occurrence";
     * claiming it would be unfalsifiable on this evidence. */
    if (P > span) {
      continue;
    }
    const uint32_t phase = hit_slots[0] % P;
    bool consistent = true;
    for (int i = 1; i < n_hits && consistent; i++) {
      if (hit_slots[i] % P != phase) {
        consistent = false;
      }
    }
    if (consistent) {
      if (period) *period = (uint16_t)P;
      if (offset) *offset = (uint16_t)phase;
      return true;
    }
  }
  return false;
}

int nr_csirs_blind_format(const nr_csirs_candidate_t *c, uint16_t period, uint16_t offset,
                          char *out, int out_len)
{
  if (c == NULL || out == NULL || out_len <= 0) {
    return 0;
  }
  const int n = snprintf(out, (size_t)out_len, "%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u",
                         c->row, c->start_rb, c->nr_of_rbs, c->freq_domain, c->symb_l0, c->symb_l1,
                         c->cdm_type, c->freq_density, c->scramb_id, period, offset);
  return (n > 0 && n < out_len) ? n : 0;
}
