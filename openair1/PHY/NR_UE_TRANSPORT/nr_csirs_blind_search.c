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


/* ---- candidate enumeration and scheduling ------------------------------------------------------ */

#include <string.h>

/* Rows worth searching -- see the header for why the wide-port rows are excluded.
 * Row 1: 1 port, density 3, one symbol. Row 2: 1 port, density 1. Row 4: 4 ports, density 1. */
static const uint8_t kRows[]    = {1, 2, 4};
static const uint8_t kDensity[] = {3, 2, 2};   /* per row: 3 = three, 2 = one */
static const uint8_t kCdm[]     = {0, 0, 1};   /* per row: noCDM, noCDM, fd-CDM2 */
/* Frequency-domain bitmap width differs per row; a one-hot sweep over the legal positions is what
 * a real configuration always is. Row 1 has 3 positions, rows 2 and 4 have 12 and 3. */
static const uint8_t kFdBits[]  = {3, 12, 3};

#define CSIRS_DETECT_MARGIN 3.0   /* a hit must beat the null MEDIAN by this factor */
#define CSIRS_MIN_HITS      3

int nr_csirs_blind_enumerate(nr_csirs_candidate_t *out, int max, uint16_t n_rb, uint16_t scramb_id)
{
  if (out == NULL || max <= 0 || n_rb == 0) {
    return -1;
  }
  int n = 0;
  for (unsigned r = 0; r < sizeof(kRows) / sizeof(kRows[0]); r++) {
    for (uint8_t b = 0; b < kFdBits[r]; b++) {
      /* symb_l0 runs over the symbols a CSI-RS may start on. Symbol 0 and 1 are excluded: a CORESET
       * occupies the start of the slot and no cell places a measurement resource under it. */
      for (uint8_t l0 = 2; l0 < 13; l0++) {
        if (n >= max) {
          return n;
        }
        nr_csirs_candidate_t *c = &out[n++];
        memset(c, 0, sizeof(*c));
        c->row = kRows[r];
        c->freq_domain = (uint16_t)(1u << b);   /* one-hot: what a real configuration carries */
        c->symb_l0 = l0;
        c->symb_l1 = 0;
        c->cdm_type = kCdm[r];
        c->freq_density = kDensity[r];
        c->scramb_id = scramb_id;
        c->start_rb = 0;
        c->nr_of_rbs = n_rb;
      }
    }
  }
  return n;
}

int nr_csirs_blind_init(nr_csirs_blind_state_t *st, uint16_t n_rb, uint16_t scramb_id)
{
  if (st == NULL) {
    return 0;
  }
  memset(st, 0, sizeof(*st));
  st->confirmed = -1;
  const int n = nr_csirs_blind_enumerate(st->cand, NR_CSIRS_BLIND_MAX_CAND, n_rb, scramb_id);
  if (n <= 0) {
    return 0;
  }
  st->n = n;
  return n;
}

int nr_csirs_blind_next(nr_csirs_blind_state_t *st)
{
  if (st == NULL || st->n <= 0) {
    return -1;
  }
  if (st->confirmed >= 0) {
    return st->confirmed;
  }
  const int idx = st->cursor;
  st->cursor = (st->cursor + 1) % st->n;
  return idx;
}

bool nr_csirs_blind_feed(nr_csirs_blind_state_t *st, int idx, uint32_t absolute_slot,
                         double rho, double rho_null)
{
  if (st == NULL || idx < 0 || idx >= st->n) {
    return false;
  }
  if (st->confirmed >= 0) {
    return true;
  }
  st->tried[idx]++;
  if (rho > st->best_rho[idx]) {
    st->best_rho[idx] = rho;
  }
  /* RELATIVE bar. rho_null is what the other candidates are scoring right now, so the test is
   * "does this stand out from its own population", which needs no per-deployment calibration.
   * A negative rho (unscorable candidate) can never pass, which is the point of returning -1
   * rather than 0 from the correlator. */
  if (rho <= 0.0 || rho_null <= 0.0 || rho < CSIRS_DETECT_MARGIN * rho_null) {
    return false;
  }
  if (st->n_hit_slot[idx] < 8) {
    st->hit_slot[idx][st->n_hit_slot[idx]++] = absolute_slot;
  }
  st->hits[idx]++;
  if (st->n_hit_slot[idx] < CSIRS_MIN_HITS) {
    return false;
  }
  uint16_t p = 0, o = 0;
  if (!nr_csirs_blind_infer_period(st->hit_slot[idx], st->n_hit_slot[idx], CSIRS_MIN_HITS, &p, &o)) {
    return false;   /* scoring high is not enough -- it must also be PERIODIC */
  }
  st->confirmed = idx;
  st->period = p;
  st->offset = o;
  return true;
}

const nr_csirs_candidate_t *nr_csirs_blind_confirmed(const nr_csirs_blind_state_t *st,
                                                     uint16_t *period, uint16_t *offset)
{
  if (st == NULL || st->confirmed < 0) {
    return NULL;
  }
  if (period) *period = st->period;
  if (offset) *offset = st->offset;
  return &st->cand[st->confirmed];
}
