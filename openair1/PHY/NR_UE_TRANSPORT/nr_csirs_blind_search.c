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

#include "PHY/nr_phy_common/inc/nr_phy_common.h"

#include <math.h>
#include <stdio.h>

const uint16_t nr_csirs_blind_periods[NR_CSIRS_BLIND_N_PERIODS] = {
    4, 5, 8, 10, 16, 20, 32, 40, 64, 80, 160, 320, 640};

uint16_t nr_csirs_blind_zp_lattice_horizon(void)
{
  uint16_t maximum = 0;
  for (int i = 0; i < NR_CSIRS_BLIND_N_PERIODS; i++)
    if (nr_csirs_blind_periods[i] > maximum)
      maximum = nr_csirs_blind_periods[i];
  return maximum;
}

_Static_assert(NR_CSIRS_BLIND_ZP_PHASE_BITS == 1399, "legal phase lattice storage changed");
_Static_assert(sizeof(((nr_csirs_zp_probation_t *)0)->occupied) == 1399 * sizeof(uint32_t),
               "bounded timestamp ledger");

double nr_csirs_blind_correlate(const int16_t *rx_re_im, const int16_t *ref_re_im, int n)
{
  return nr_csirs_blind_correlate_n(rx_re_im, ref_re_im, n, NULL);
}

/* MEASURED 2026-09-19 on a live cell, and it invalidated every confirmation this module had made:
 * |rho| of N random complex pairs is ~0.89/sqrt(N), so the score depends on how many REs a candidate
 * occupies (273 for row 2 density-one, 819 for density-three at 273 RB). The relative bar compared a
 * candidate against the MEDIAN of the others, i.e. against a different N -- so the smallest
 * candidates always won on chance alone. Observed: winners 0.19-0.22 = exactly the expected MAXIMUM
 * of ~950 noise draws at N=273, null median 0.045 = noise at the larger N. Callers should therefore
 * use rho * sqrt(n_used), which is ~0.89 for noise at ANY N and grows as sqrt(N) for a true match. */
double nr_csirs_blind_correlate_n(const int16_t *rx_re_im, const int16_t *ref_re_im, int n, int *n_used)
{
  if (n_used != NULL) {
    *n_used = 0;
  }
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
  if (n_used != NULL) {
    *n_used = used;
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
  int distinct = 0;
  for (int i = 0; i < n_hits; i++) {
    bool duplicate = false;
    for (int j = 0; j < i; j++)
      duplicate |= hit_slots[i] == hit_slots[j];
    distinct += !duplicate;
  }
  if (distinct < min_hits)
    return false; /* multiple symbols in one slot do not add independent period evidence */
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

bool nr_csirs_blind_infer_period2(const uint32_t *hit_slots, int n_hits, int min_hits,
                                  uint16_t *period, uint16_t off[2], int *n_off)
{
  if (hit_slots == NULL || min_hits < 2 || n_hits < min_hits)
    return false;
  uint16_t p = 0, o = 0;
  if (nr_csirs_blind_infer_period(hit_slots, n_hits, min_hits, &p, &o)) {
    if (period) *period = p;
    if (off) off[0] = o;
    if (n_off) *n_off = 1;
    return true;
  }
  if (hit_slots == NULL || n_hits < 4 || n_hits < min_hits)
    return false; /* two phases x two hits each is the minimum evidence */
  uint32_t lo = hit_slots[0], hi = hit_slots[0];
  for (int i = 1; i < n_hits; i++) {
    if (hit_slots[i] < lo) lo = hit_slots[i];
    if (hit_slots[i] > hi) hi = hit_slots[i];
  }
  for (int pi = NR_CSIRS_BLIND_N_PERIODS - 1; pi >= 0; pi--) {
    const uint32_t P = nr_csirs_blind_periods[pi];
    if (P > hi - lo)
      continue;
    uint32_t ph[2] = {hit_slots[0] % P, 0};
    int cnt[2] = {0, 0}, n_ph = 1;
    bool ok = true;
    for (int i = 0; i < n_hits && ok; i++) {
      bool duplicate = false;
      for (int j = 0; j < i; j++)
        duplicate |= hit_slots[i] == hit_slots[j];
      if (duplicate)
        continue;
      const uint32_t v = hit_slots[i] % P;
      if (v == ph[0]) {
        cnt[0]++;
      } else if (n_ph == 2 && v == ph[1]) {
        cnt[1]++;
      } else if (n_ph == 1) {
        ph[1] = v;
        cnt[1] = 1;
        n_ph = 2;
      } else {
        ok = false;
      }
    }
    if (!ok || n_ph != 2 || cnt[0] < 2 || cnt[1] < 2 || cnt[0] + cnt[1] < min_hits)
      continue;
    const int a = ph[0] < ph[1] ? 0 : 1;
    if (period) *period = (uint16_t)P;
    if (off) { off[0] = (uint16_t)ph[a]; off[1] = (uint16_t)ph[1 - a]; }
    if (n_off) *n_off = 2;
    return true;
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
/* Rows enumerated, TS 38.211 Table 7.4.1.5.3-1. EXTENDED 2026-09-19 from {1,2,4}.
 *
 * WHAT GATES THIS LIST, and it is a hazard not a preference: get_csi_mapping_parms() locates the
 * REs by walking the bitmap until it has found the number of set bits the ROW requires --
 * `while (found < 4)` for row 6, `< 2` for row 8, `< 6` for row 9. Its loop has no bound, so a
 * candidate whose bitmap carries FEWER set bits than its row needs spins forever, on the PHY
 * receive thread. Every row below therefore needs exactly ONE set bit (`found < 1`), which is what
 * makes a one-hot enumeration safe; kRowNeedBits records the requirement and enumerate() checks it.
 *
 * Rows 6-18 (8/12/16/24/32 ports) need 2-6 simultaneous bits, i.e. a combinatorial bitmap
 * enumeration far too large to round-robin; they are reached footprint-first instead
 * (nr_csirs_blind_fp_match) and appended to the population only when the air shows them. */
static const uint8_t kRows[]     = {1, 2, 3, 4, 5};
static const uint8_t kCdm[]      = {0, 0, 1, 1, 1};   /* noCDM, noCDM, fd-CDM2, fd-CDM2, fd-CDM2 */

/* Per row 1-18, TS 38.211 Table 7.4.1.5.3-1: antenna ports, and the set bitmap bits
 * get_csi_mapping_parms() walks for (its loop is unbounded -- fewer bits spins it forever). Neither is
 * derivable by calling the mapping function: the port count sizes the buffers it writes, and the bit
 * count is what makes calling it safe at all. The RE mapping itself is NOT transcribed here. */
static const uint8_t kRowPorts[18]    = {1, 1, 2, 4, 4, 8, 8, 8, 12, 12, 16, 16, 24, 24, 24, 32, 32, 32};
static const uint8_t kRowNeedBits[18] = {1, 1, 1, 1, 1, 4, 2, 2, 6, 3, 4, 4, 3, 3, 3, 4, 4, 4};

/* Bitmap width per row. Row 1 is b3..b0 -- FOUR bits: the generator places its REs at k0, k0+4,
 * k0+8 with k0 in {0,1,2,3}. This said 3, so k0=3 was never enumerated and a quarter of the row-1
 * (TRS) space was unreachable no matter how long a search ran. */
static const uint8_t kFdBits[]   = {4, 12, 3, 3, 3};

/* Densities to try per row. Rows 2 and 3 admit dot5 (even or odd RBs); a cell using dot5 puts
 * its REs on half the RBs a density-one candidate tests -- which reads as a half-strength match,
 * not as a miss, so both parities have to be enumerated rather than inferred.
 * 0 = dot5 even RB, 1 = dot5 odd RB, 2 = one, 3 = three. */
static const uint8_t kDensities[][3] = {{3, 0xFF, 0xFF}, {2, 0, 1}, {2, 0, 1},
                                        {2, 0xFF, 0xFF}, {2, 0xFF, 0xFF}};

#define CSIRS_DETECT_MARGIN 3.0   /* a hit must beat the null MEDIAN by this factor */
#define CSIRS_MIN_HITS      3

int nr_csirs_blind_enumerate(nr_csirs_candidate_t *out, int max, uint16_t n_rb, uint16_t scramb_id)
{
  if (out == NULL || max <= 0 || n_rb == 0) {
    return -1;
  }
  int n = 0;
  for (unsigned r = 0; r < sizeof(kRows) / sizeof(kRows[0]); r++) {
    if (nr_csirs_blind_row_needs_bits(kRows[r]) != 1) {
      continue;   /* see kRows: a multi-bit row would spin the generator's unbounded bitmap walk */
    }
    for (unsigned d = 0; d < 3; d++) {
      const uint8_t density = kDensities[r][d];
      if (density == 0xFF) {
        continue;
      }
      for (uint8_t b = 0; b < kFdBits[r]; b++) {
        /* symb_l0 covers every symbol a CSI-RS may start on. Symbols 0-1 are included: only a
         * 3-symbol CORESET reaches symbol 2, and a cell is free to place a resource above a
         * 1-symbol one -- excluding them was an assumption about the scheduler, not a constraint
         * from the spec. Rows whose pattern uses l0+1 (row 5) must leave room for it. */
        const uint8_t l_max = (kRows[r] == 5) ? 12 : 13;
        for (uint8_t l0 = 0; l0 <= l_max; l0++) {
          if (n >= max) {
            return n;
          }
          nr_csirs_candidate_t *c = &out[n++];
          memset(c, 0, sizeof(*c));
          c->row = kRows[r];
          c->freq_domain = (uint16_t)(1u << b);   /* one-hot: exactly the one set bit these rows need */
          c->symb_l0 = l0;
          c->symb_l1 = 0;
          c->cdm_type = kCdm[r];
          c->freq_density = density;
          c->scramb_id = scramb_id;
          c->start_rb = 0;
          c->nr_of_rbs = n_rb;
        }
      }
    }
  }
  return n;
}

int nr_csirs_blind_row_needs_bits(uint8_t row)
{
  return (row >= 1 && row <= 18) ? kRowNeedBits[row - 1] : -1;
}

int nr_csirs_blind_row_ports(uint8_t row)
{
  return (row >= 1 && row <= 18) ? kRowPorts[row - 1] : 0;
}

bool nr_csirs_blind_candidate_safe(const nr_csirs_candidate_t *c)
{
  if (c == NULL) {
    return false;
  }
  const int need = nr_csirs_blind_row_needs_bits(c->row);
  if (need < 0) {
    return false;   /* a row this module does not know: never hand it to the generator */
  }
  int set = 0;
  for (int b = 0; b < 16; b++) {
    if ((c->freq_domain >> b) & 1u) {
      set++;
    }
  }
  return set >= need;
}

int nr_csirs_blind_init(nr_csirs_blind_state_t *st, uint16_t n_rb, uint16_t scramb_id)
{
  if (st == NULL) {
    return 0;
  }
  memset(st, 0, sizeof(*st));
  st->confirmed = -1;
  st->pinned = -1;
  st->pin_left = 0;
  const int n = nr_csirs_blind_enumerate(st->cand, NR_CSIRS_BLIND_MAX_CAND, n_rb, scramb_id);
  if (n <= 0) {
    return 0;
  }
  st->n = n;
  return n;
}

void nr_csirs_blind_pin(nr_csirs_blind_state_t *st, int idx, uint32_t budget)
{
  if (st == NULL || idx < 0 || idx >= st->n)
    return;
  st->pinned = idx;
  st->pin_left = budget;
}

int nr_csirs_blind_next(nr_csirs_blind_state_t *st)
{
  if (st == NULL || st->n <= 0) {
    return -1;
  }
  /* Silence does not prove completeness: legal periods extend to 640 slots, and the caller may
   * skip slots. Keep the one-candidate work bound and stop only when no search capacity remains. */
  if (st->n_conf >= NR_CSIRS_BLIND_MAX_CONF || st->n_conf >= st->n) {
    return -1;
  }
  if (st->pin_left > 0 && st->pinned >= 0 && st->pinned < st->n && !nr_csirs_blind_is_confirmed(st, st->pinned)) {
    st->pin_left--;
    return st->pinned;
  }
  if (st->cycle_left == 0) {
    if (st->cycle_pad > 0 && !nr_csirs_blind_is_confirmed(st, st->cycle_last)) {
      st->cycle_pad--;
      return st->cycle_last;
    }
    const int active = st->n - st->n_conf;
    int stride = active;
    /* Every legal period has only 2 and 5 as prime factors. Pad a round by at most three
     * calls so its stride is coprime to both; regular slot visits then sample every phase. */
    while (stride % 2 == 0 || stride % 5 == 0)
      stride++;
    st->cycle_left = active;
    st->cycle_pad = stride - active;
  }
  for (int k = 0; k < st->n; k++) {
    const int idx = st->cursor;
    st->cursor = (st->cursor + 1) % st->n;
    if (!nr_csirs_blind_is_confirmed(st, idx)) {
      st->cycle_left--;
      st->cycle_last = idx;
      return idx;
    }
  }
  return -1;
}

bool nr_csirs_blind_is_confirmed(const nr_csirs_blind_state_t *st, int idx)
{
  if (st == NULL)
    return false;
  for (int k = 0; k < st->n_conf; k++)
    if (st->conf_idx[k] == idx)
      return true;
  return false;
}

int nr_csirs_blind_occurring(const nr_csirs_blind_state_t *st, uint32_t absolute_slot, int *idx_out, int max)
{
  int n = 0;
  for (int k = 0; st != NULL && idx_out != NULL && k < st->n_conf && n < max; k++) {
    const uint32_t P = st->conf_period[k];
    if (P == 0)
      continue;
    const uint32_t ph = absolute_slot % P;
    bool on = false;
    for (int j = 0; j < st->conf_n_off[k]; j++)
      on |= ph == st->conf_off[k][j] % P;
    if (on)
      idx_out[n++] = st->conf_idx[k];
  }
  return n;
}

static nr_csirs_zp_probation_t *zp_bank(nr_csirs_blind_state_t *st, int idx)
{
  for (int k = 0; k < NR_CSIRS_BLIND_MAX_CONF; k++)
    if (st->zp_bank[k].owner == idx + 1)
      return &st->zp_bank[k];
  return NULL;
}

static bool zp_on(uint32_t slot, unsigned period, const uint16_t off[2], unsigned n_off)
{
  for (unsigned j = 0; period && j < n_off; j++)
    if (slot % period == off[j] % period)
      return true;
  return false;
}

int nr_csirs_blind_zp_due(const nr_csirs_blind_state_t *st, uint32_t slot, int *out, int max)
{
  int n = 0;
  for (int k = 0; st && out && k < NR_CSIRS_BLIND_MAX_CONF && n < max; k++) {
    const nr_csirs_zp_probation_t *b = &st->zp_bank[k];
    if (!b->owner || st->zp_failed_run[b->owner - 1] == UINT64_MAX)
      continue;
    bool due = !b->period || zp_on(slot, b->period, b->off, b->n_off);
    if (!nr_csirs_blind_is_confirmed(st, b->owner - 1))
      for (int pi = 0; !due && pi < NR_CSIRS_BLIND_N_PERIODS; pi++) {
        const unsigned d = nr_csirs_blind_periods[pi];
        due = b->period > d && b->period % d == 0 && zp_on(slot, d, b->off, b->n_off);
      }
    if (due)
      out[n++] = b->owner - 1;
  }
  return n;
}

static void zp_failed(nr_csirs_blind_state_t *st, int idx, nr_csirs_zp_probation_t *b)
{
  st->zp_evidence_floor[idx] = st->zp_last_slot[idx];
  uint64_t rejected = 1; /* every contradicted measured trial carries evidence, even before a vote */
  for (int j = 0; j < b->n_off; j++)
    if (b->votes[j] > rejected)
      rejected = b->votes[j];
  if (b->promotion_votes > rejected)
    rejected = b->promotion_votes;
  /* Repeatedly selecting the same transient after fresh epochs is multiple-testing evidence,
   * not permission to retry forever at max(previous_streak)+1. Accumulate the contradicted
   * measured support with saturation; a stable resource can still recover by supplying a longer
   * prospective run, while overflow fails closed through the existing UINT64_MAX sentinel. */
  if (st->zp_failed_run[idx] > UINT64_MAX - rejected)
    st->zp_failed_run[idx] = UINT64_MAX;
  else
    st->zp_failed_run[idx] += rejected;
  b->period = b->n_off = 0;
  b->votes[0] = b->votes[1] = b->promotion_votes = 0;
}

static uint64_t zp_required_votes(const nr_csirs_blind_state_t *st, int idx, unsigned period)
{
  if (!period || st->zp_failed_run[idx] == UINT64_MAX)
    return UINT64_MAX;
  uint64_t need = (nr_csirs_blind_zp_lattice_horizon() + period - 1) / period;
  if (need < CSIRS_MIN_HITS) need = CSIRS_MIN_HITS;
  if (need <= st->zp_failed_run[idx]) need = st->zp_failed_run[idx] + 1;
  return need;
}

/* Capacity pressure may replace only an unexported admission after enough time
 * for a fully measured probation plus one lattice horizon. Unknown observations
 * cannot reserve all eight entries forever. Durable failed-run debt and a fresh
 * evidence floor survive replacement; no old supporting hit can be recycled. */
static nr_csirs_zp_probation_t *zp_admit(nr_csirs_blind_state_t *st, int idx, uint32_t slot)
{
  nr_csirs_zp_probation_t *victim = NULL;
  const uint64_t horizon = nr_csirs_blind_zp_lattice_horizon();
  for (int k = 0; k < NR_CSIRS_BLIND_MAX_CONF; k++) {
    nr_csirs_zp_probation_t *b = &st->zp_bank[k];
    if (!b->owner) { victim = b; break; }
    const int old = b->owner - 1;
    if (nr_csirs_blind_is_confirmed(st, old) || slot < b->admitted_slot)
      continue;
    const unsigned p = b->period ? b->period : st->zp_selected_period[old];
    const uint64_t need = zp_required_votes(st, old, p);
    uint64_t lease = UINT64_MAX;
    if (p && need <= (UINT64_MAX - horizon) / p)
      lease = need * p + horizon;
    const bool expired = st->zp_failed_run[old] == UINT64_MAX
        || (uint64_t)(slot - b->admitted_slot) >= lease;
    if (expired && (!victim || b->admitted_slot < victim->admitted_slot))
      victim = b;
  }
  if (!victim)
    return NULL;
  if (victim->owner) {
    const int old = victim->owner - 1;
    st->zp_evidence_floor[old] = slot;
    st->n_hit_slot[old] = 0;
    st->tried[old] = st->zp_holes[old] = 0;
    memset(st->zp_rejected_phase[old], 0, sizeof(st->zp_rejected_phase[old]));
  }
  memset(victim, 0, sizeof(*victim));
  victim->owner = idx + 1;
  victim->admitted_slot = slot;
  memset(victim->occupied, 0xff, sizeof(victim->occupied));
  for (unsigned bit = 0; bit < NR_CSIRS_BLIND_ZP_PHASE_BITS; bit++)
    if (st->zp_rejected_phase[idx][bit / 64] & (UINT64_C(1) << (bit % 64)))
      victim->occupied[bit] = slot;
  return victim;
}

static void zp_occupied(nr_csirs_zp_probation_t *b, uint32_t slot)
{
  unsigned base = 0;
  for (int pi = 0; pi < NR_CSIRS_BLIND_N_PERIODS; pi++) {
    const unsigned p = nr_csirs_blind_periods[pi];
    b->occupied[base + slot % p] = slot;
    base += p;
  }
}

static bool zp_ledger_rejected(const nr_csirs_zp_probation_t *b, unsigned p,
                               const uint16_t off[2], unsigned n_off, uint32_t start)
{
  unsigned base = 0;
  for (int pi = 0; pi < NR_CSIRS_BLIND_N_PERIODS; pi++) {
    if (nr_csirs_blind_periods[pi] == p) {
      for (unsigned j = 0; j < n_off; j++) {
        const uint32_t last = b->occupied[base + off[j] % p];
        if (last != UINT32_MAX && last >= start)
          return true;
      }
      return false;
    }
    base += nr_csirs_blind_periods[pi];
  }
  return true;
}

static bool zp_divisors_resolved(const nr_csirs_zp_probation_t *b)
{
  for (int pi = 0; pi < NR_CSIRS_BLIND_N_PERIODS; pi++) {
    const unsigned d = nr_csirs_blind_periods[pi];
    if (d >= b->period || b->period % d)
      continue;
    bool additional = false;
    for (unsigned s = 0; s < b->period; s++)
      additional |= zp_on(s, d, b->off, b->n_off) && !zp_on(s, b->period, b->off, b->n_off);
    if (additional && !zp_ledger_rejected(b, d, b->off, b->n_off, b->support_start))
      return false;
  }
  return true;
}

static void zp_export(nr_csirs_blind_state_t *st, int idx, nr_csirs_zp_probation_t *b)
{
  const int k = st->n_conf++;
  st->conf_idx[k] = idx;
  st->conf_period[k] = b->period;
  memcpy(st->conf_off[k], b->off, sizeof(b->off));
  st->conf_n_off[k] = b->n_off;
  b->promotion_votes = b->votes[0] > b->votes[1] ? b->votes[0] : b->votes[1];
  if (st->confirmed < 0) {
    st->confirmed = idx;
    st->period = b->period;
    st->offset = b->off[0];
  }
}

static void zp_restart_epoch(nr_csirs_blind_state_t *st, int idx, uint32_t absolute_slot)
{
  if (st->zp_epoch[idx] == UINT32_MAX) {
    st->zp_failed_run[idx] = UINT64_MAX;
    return;
  }
  st->zp_epoch[idx]++;
  st->hit_slot[idx][0] = absolute_slot;
  st->n_hit_slot[idx] = 1;
  memset(st->zp_rejected_phase[idx], 0, sizeof(st->zp_rejected_phase[idx]));
  /* Admission timestamps and failed-run history deliberately survive this reset. */
}

/* Observations arrive in slot order. Bits accumulate only after the first supporting hit and
 * are checked when the next hit proposes a period, so their span is exactly that evidence epoch.
 * One bit per legal phase retains every contradiction without an observation-rate-dependent FIFO. */
static void zp_record_rejection(nr_csirs_blind_state_t *st, int idx, uint32_t absolute_slot)
{
  if (st->n_hit_slot[idx] == 0)
    return;
  unsigned base = 0;
  for (int i = 0; i < NR_CSIRS_BLIND_N_PERIODS; i++) {
    const unsigned p = nr_csirs_blind_periods[i];
    const unsigned bit = base + absolute_slot % p;
    st->zp_rejected_phase[idx][bit / 64] |= UINT64_C(1) << (bit % 64);
    base += p;
  }
}

static bool zp_phase_rejected(const nr_csirs_blind_state_t *st, int idx, uint16_t period,
                              const uint16_t off[2], int n_off)
{
  unsigned base = 0;
  for (int i = 0; i < NR_CSIRS_BLIND_N_PERIODS; i++) {
    if (nr_csirs_blind_periods[i] == period) {
      for (int j = 0; j < n_off; j++) {
        const unsigned bit = base + off[j];
        if (st->zp_rejected_phase[idx][bit / 64] & (UINT64_C(1) << (bit % 64)))
          return true;
      }
      return false;
    }
    base += nr_csirs_blind_periods[i];
  }
  return true; /* an unknown period has no checked phase evidence */
}

/* Record hit slot + test periodicity; on success add idx to the confirmed list. Shared by the NZP and
 * ZP feeds. A hit on a still-unpinned search pins the candidate: round-robin revisits it every n
 * slots, which is a FIXED phase of the period whenever n shares a factor with it, so the repeats a
 * lucky first hit needs might otherwise never be looked at. */
static bool record_hit(nr_csirs_blind_state_t *st, int idx, uint32_t absolute_slot, bool structural, bool zp)
{
  nr_csirs_zp_probation_t *bank = zp ? zp_bank(st, idx) : NULL;
  if (zp && st->zp_evidence_floor[idx] && absolute_slot <= st->zp_evidence_floor[idx])
    return false;
  if (zp && st->hits[idx] == UINT32_MAX) {
    st->zp_failed_run[idx] = UINT64_MAX;
    return false;
  }
  for (int i = 0; i < st->n_hit_slot[idx]; i++)
    if (st->hit_slot[idx][i] == absolute_slot)
      return false;
  st->hits[idx]++;
  if (zp && (st->n_hit_slot[idx] == 0 || st->n_hit_slot[idx] == 8)) {
    zp_restart_epoch(st, idx, absolute_slot);
  } else if (st->n_hit_slot[idx] < 8) {
    st->hit_slot[idx][st->n_hit_slot[idx]++] = absolute_slot;
  }
  if (st->pin_left == 0 && st->pinned != idx)
    nr_csirs_blind_pin(st, idx, NR_CSIRS_BLIND_PIN_CONFIRM_CALLS);
  if (st->n_hit_slot[idx] < CSIRS_MIN_HITS || structural || st->n_conf >= NR_CSIRS_BLIND_MAX_CONF) {
    return false;
  }
  uint16_t p = 0, off[2] = {0, 0};
  int n_off = 0;
  if (!nr_csirs_blind_infer_period2(st->hit_slot[idx], st->n_hit_slot[idx], CSIRS_MIN_HITS, &p, off, &n_off)) {
    if (zp && st->n_hit_slot[idx] == 8)
      zp_restart_epoch(st, idx, absolute_slot);
    return false;   /* scoring high is not enough -- it must also be PERIODIC */
  }
  if (zp) {
    for (int j = 0; j < n_off; j++) {
      int supports = 0;
      for (int h = 0; h < st->n_hit_slot[idx]; h++)
        supports += st->hit_slot[idx][h] % p == off[j];
      if (supports < CSIRS_MIN_HITS)
        return false;
    }
    st->zp_selected_period[idx] = p;
    memcpy(st->zp_selected_off[idx], off, sizeof(off));
    st->zp_selected_n_off[idx] = n_off;
  }
  if (zp && zp_phase_rejected(st, idx, p, off, n_off)) {
    zp_restart_epoch(st, idx, absolute_slot);
    return false;
  }
  if (zp) {
    if (st->zp_failed_run[idx] == UINT64_MAX)
      return false;
    if (!bank)
      bank = zp_admit(st, idx, absolute_slot);
    if (!bank)
      return false; // combined probation/export bank is full
    if (zp_ledger_rejected(bank, p, off, n_off, st->hit_slot[idx][0])) {
      zp_restart_epoch(st, idx, absolute_slot);
      return false;
    }
    bank->period = p;
    bank->n_off = n_off;
    memcpy(bank->off, off, sizeof(off));
    bank->support_start = st->hit_slot[idx][0];
    bank->proposed_slot = absolute_slot;
    bank->votes[0] = bank->votes[1] = bank->promotion_votes = 0;
    return false; // discovery can NEVER write the ZP export bank
  }
  const int k = st->n_conf++;
  st->conf_idx[k] = idx;
  st->conf_period[k] = p;
  st->conf_off[k][0] = off[0];
  st->conf_off[k][1] = off[1];
  st->conf_n_off[k] = (uint8_t)n_off;
  st->cycle_left = st->cycle_pad = 0;
  if (st->pinned == idx)
    st->pin_left = 0;
  if (st->confirmed < 0) {
    st->confirmed = idx;
    st->period = p;
    st->offset = off[0];
  }
  return true;
}

bool nr_csirs_blind_feed(nr_csirs_blind_state_t *st, int idx, uint32_t absolute_slot,
                         double rho, double rho_null)
{
  if (st == NULL || idx < 0 || idx >= st->n) {
    return false;
  }
  if (nr_csirs_blind_is_confirmed(st, idx)) {
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
  return record_hit(st, idx, absolute_slot, false, false);
}

/* ZERO-POWER SCORE. What a ZP CSI-RS is: REs the PDSCH is rate-matched around, dark while the PDSCH
 * around them carries data. Its only evidence IS the energy.
 *
 * GRID ALIGNMENT. Like the other comparators this reads rxdataF at the reference's RE positions, so it
 * maps CRB order to FFT order (rx_shift). A misaligned read does not merely weaken it -- it measures a
 * different part of the spectrum entirely.
 *
 * OCCUPANCY is the union of the generated port planes (plane 0 alone omits other CDM groups, e.g. row
 * 4's ports 2/3). BASELINE (E_off) is the WEAKEST off-pattern subcarrier-in-RB class, each class first
 * averaged over the touched RBs so one faded QAM sample is not a veto:
 * - Not the mean (the first cut): a mean is dominated by any boosted pilot. Lab G4, 2026-09-28: an
 *   8-port cell seen by one antenna through an identity channel delivers port 0 only (k=0,1), k=2..11
 *   are empty, and every candidate there read 1 - noise/(2*pilot/11) ~ 1 -> a false ZP every run. With
 *   the weakest class every such candidate has a dark off-pattern class beside it and reads ~0.
 * - Not a median: removing a row-2 quiet tone from a half-occupied RB leaves six active vs five quiet
 *   off-pattern tones, so a quiet SUBSET of a wider hole would score as a hole.
 * - A candidate must therefore explain the COMPLETE quiet pattern of its RBs: another quiet class makes
 *   the geometry unidentifiable and the score ~0. Wide holes (8 RE/RB, dark REs the majority) still
 *   read ~1 when the candidate covers all of them; two independent holes in one symbol (ZP + CSI-IM
 *   on different REs) or a candidate covering only part of a wider hole are deliberately refused.
 * - At most CSIRS_ZP_BRIGHT_CLASSES off-pattern classes: unscorable. Up to three bright classes may be
 *   a sparse pilot (port 0 of a wider NZP, a TRS-like comb), not data; with so few free classes the
 *   weakest one can be a pilot, which is the lab false ZP in another shape. Rows 1-5 (<= 4 classes per
 *   RB) never reach this guard.
 *
 * NOT GUARANTEED in one symbol: dark REs that cover EVERY dark class beside >= 4 bright classes that are
 * not PDSCH (a wide NZP, several NZP resources, in a slot without PDSCH on those REs) read exactly like
 * data + a ZP hole. A single-slot neighbour-symbol test was tried (sdd/gap-csirs cb5c358) but costs two
 * extra FFTs per candidate hit; the cure kept here is the lifecycle in nr_csirs_blind_zp_feed_pair():
 * held-out probation before export, then two scorable occupied predicted occasions revoke an export, so
 * a false resource is withdrawn as soon as a PDSCH puts data on its REs -- provided the weakest bright
 * class is not boosted >= 3 dB above the PDSCH EPRE: then data on the pattern still scores > 0.5 and is
 * not counted as a contradiction.
 *
 * -1 (unscorable, never a hit): empty reference, <= CSIRS_ZP_BRIGHT_CLASSES off-pattern classes, or an
 * off-pattern class with exactly zero energy (a noise-free symbol holding nothing but a pilot). */
#define CSIRS_ZP_BRIGHT_CLASSES 3
static bool zp_reference_occupied(const int16_t *const *refs, int n_refs, int i)
{
  for (int p = 0; p < n_refs; p++)
    if (refs[p][2 * i] != 0 || refs[p][2 * i + 1] != 0)
      return true;
  return false;
}

double nr_csirs_blind_zero_score_evidence_shift(const int16_t *rx_re_im, const int16_t *const *refs,
                                               int n_refs, int n, int rx_shift, double *median_score)
{
  if (median_score != NULL)
    *median_score = -1.0;
  if (rx_re_im == NULL || refs == NULL || n_refs <= 0 || n <= 0) {
    return -1.0;
  }
  for (int p = 0; p < n_refs; p++)
    if (refs[p] == NULL)
      return -1.0;
  /* RBs the pattern touches: a 12-RE granularity mask on the caller's index (symbol-relative
   * indices keep the RB grid; a whole-symbol buffer starts at RB 0 either way). */
  double e_on = 0.0, off_power[12] = {0};
  int n_on = 0, off_count[12] = {0};
  double median_power = 0.0;
  int median_count = 0;
  for (int rb0 = 0; rb0 + 12 <= n; rb0 += 12) {
    bool touched = false;
    for (int i = rb0; i < rb0 + 12; i++) {
      if (zp_reference_occupied(refs, n_refs, i)) { touched = true; break; }
    }
    if (!touched) {
      continue;
    }
    double rb_off[12];
    int rb_n_off = 0;
    for (int i = rb0; i < rb0 + 12; i++) {
      const int j = (int)(((long)i + rx_shift) % n);
      const double yr = (double)rx_re_im[2 * j], yi = (double)rx_re_im[2 * j + 1];
      const double e = yr * yr + yi * yi;
      if (zp_reference_occupied(refs, n_refs, i)) { e_on += e; n_on++; }
      else {
        off_power[i - rb0] += e;
        off_count[i - rb0]++;
        if (median_score != NULL)
          rb_off[rb_n_off++] = e;
      }
    }
    if (rb_n_off > 0) {
      for (int i = 1; i < rb_n_off; i++) {
        const double value = rb_off[i];
        int j = i;
        while (j > 0 && rb_off[j - 1] > value) {
          rb_off[j] = rb_off[j - 1];
          j--;
        }
        rb_off[j] = value;
      }
      median_power += rb_off[(rb_n_off - 1) / 2] * rb_n_off;
      median_count += rb_n_off;
    }
  }
  if (median_score != NULL && n_on > 0 && median_count > 0 && median_power > 0.0)
    *median_score = 1.0 - fmin(1.0, (e_on / n_on) / (median_power / median_count));
  /* A candidate must explain the complete quiet pattern, not just one quiet tone of a
   * wider comb. A median reference cannot establish that: removing a row-2 quiet tone
   * from a half-occupied RB leaves SIX active vs FIVE quiet off-pattern tones.
   * Compare against every off-pattern subcarrier class, averaged over the same touched
   * RBs first so one low-amplitude QAM sample is not a veto. The weakest class bounds
   * the background: another quiet class makes this geometry unidentifiable. This is
   * deliberately conservative when independent resources create additional holes. */
  double e_off = -1.0;
  int n_cls = 0;
  for (int k = 0; k < 12; k++) {
    if (off_count[k] > 0) {
      const double mean = off_power[k] / off_count[k];
      n_cls++;
      if (e_off < 0.0 || mean < e_off)
        e_off = mean;
    }
  }
  /* Too few free classes to tell a sparse pilot from data: unscorable, never a hit. */
  if (n_on == 0 || n_cls <= CSIRS_ZP_BRIGHT_CLASSES || e_off <= 0.0) {
    return -1.0;
  }
  const double ratio = (e_on / n_on) / e_off;
  return 1.0 - (ratio > 1.0 ? 1.0 : ratio);
}

double nr_csirs_blind_zero_score_ports_shift(const int16_t *rx_re_im, const int16_t *const *refs,
                                             int n_refs, int n, int rx_shift)
{
  return nr_csirs_blind_zero_score_evidence_shift(rx_re_im, refs, n_refs, n, rx_shift, NULL);
}

double nr_csirs_blind_zero_score_shift(const int16_t *rx_re_im, const int16_t *ref_re_im, int n,
                                       int rx_shift)
{
  return nr_csirs_blind_zero_score_ports_shift(rx_re_im, &ref_re_im, 1, n, rx_shift);
}

bool nr_csirs_blind_zp_feed(nr_csirs_blind_state_t *st, int idx, uint32_t absolute_slot,
                            double score, double score_null)
{
  return nr_csirs_blind_zp_feed_pair(st, idx, absolute_slot, score, score, score_null);
}

bool nr_csirs_blind_zp_score_qualifies(double score, double score_null)
{
  return isfinite(score) && score > NR_CSIRS_BLIND_ZP_MIN_SCORE && score_null >= 0.0
         && score > CSIRS_DETECT_MARGIN * score_null;
}

static void zp_withdraw(nr_csirs_blind_state_t *st, int idx, nr_csirs_zp_probation_t *bank)
{
  for (int k = 0; k < st->n_conf; k++) {
    if (st->conf_idx[k] != idx)
      continue;
    for (int j = k; j + 1 < st->n_conf; j++) {
      st->conf_idx[j] = st->conf_idx[j + 1];
      st->conf_period[j] = st->conf_period[j + 1];
      st->conf_n_off[j] = st->conf_n_off[j + 1];
      memcpy(st->conf_off[j], st->conf_off[j + 1], sizeof(st->conf_off[j]));
    }
    st->n_conf--;
    st->confirmed = st->n_conf ? st->conf_idx[0] : -1;
    st->period = st->n_conf ? st->conf_period[0] : 0;
    st->offset = st->n_conf ? st->conf_off[0][0] : 0;
    if (st->zp_revocations[idx] == UINT32_MAX)
      st->zp_failed_run[idx] = UINT64_MAX;
    else
      st->zp_revocations[idx]++;
    break;
  }
  if (bank)
    zp_failed(st, idx, bank);
  st->zp_contradictions[idx] = 0;
  st->n_hit_slot[idx] = 0;
  st->tried[idx] = st->zp_holes[idx] = 0;
  memset(st->zp_rejected_phase[idx], 0, sizeof(st->zp_rejected_phase[idx]));
  st->cycle_left = st->cycle_pad = 0;
}

bool nr_csirs_blind_zp_feed_pair(nr_csirs_blind_state_t *st, int idx, uint32_t absolute_slot,
                                double score, double other_score, double score_null)
{
  if (st == NULL || idx < 0 || idx >= st->n) {
    return false;
  }
  const bool confirmed = nr_csirs_blind_is_confirmed(st, idx);
  nr_csirs_zp_probation_t *bank = zp_bank(st, idx);
  /* Slot wrap/backwards replay and evidence overflow fail closed until cell reset. */
  if (st->zp_failed_run[idx] == UINT64_MAX || absolute_slot == UINT32_MAX
      || ((st->tried[idx] || st->zp_epoch[idx]) && absolute_slot < st->zp_last_slot[idx])
      || st->tried[idx] == UINT32_MAX) {
    st->zp_failed_run[idx] = UINT64_MAX;
    zp_withdraw(st, idx, bank);
    return false;
  }
  if (!isfinite(score) || score < 0.0 || !isfinite(other_score) || other_score < 0.0
      || ((st->tried[idx] > 0 || st->zp_epoch[idx] > 0) && st->zp_last_slot[idx] == absolute_slot))
    return confirmed;
  st->zp_last_slot[idx] = absolute_slot;
  /* Same relative bar as the NZP feed; the null population is the other candidates' zero scores,
   * which sit near 0 on data (E_on ~ E_off). A minimum absolute margin keeps a null median of
   * ~0 from turning every small fluctuation into a hit. */
  /* A joint hit validates BOTH symbols at the same inferred period/phase. Count a raw hole on
   * EITHER symbol for the structural veto: a persistent hole in one symbol must not become
   * "periodic" just because the other symbol is only occasionally empty. This union is
   * conservative if the symbols also carry unrelated holes at different occasions. */
  const bool hole = score > NR_CSIRS_BLIND_ZP_MIN_SCORE || other_score > NR_CSIRS_BLIND_ZP_MIN_SCORE;
  const double joint_score = fmin(score, other_score);
  const bool hit = nr_csirs_blind_zp_score_qualifies(joint_score, score_null);
  if (bank && joint_score <= NR_CSIRS_BLIND_ZP_MIN_SCORE)
    zp_occupied(bank, absolute_slot);
  if (confirmed) {
    for (int k = 0; k < st->n_conf; k++) {
      if (st->conf_idx[k] != idx)
        continue;
      bool predicted = false;
      for (int j = 0; j < st->conf_n_off[k]; j++)
        predicted |= absolute_slot % st->conf_period[k] == st->conf_off[k][j];
      if (!predicted)
        return true;
      if (hit)
        st->zp_contradictions[idx] = 0;
      if (joint_score > NR_CSIRS_BLIND_ZP_MIN_SCORE || ++st->zp_contradictions[idx] < 2)
        return true;
      zp_withdraw(st, idx, bank);
      return false;
    }
  }
  st->tried[idx]++;
  st->zp_holes[idx] += hole;
  if (joint_score > st->best_rho[idx]) {
    st->best_rho[idx] = joint_score;
  }
  if (bank && bank->period) {
    const bool on = zp_on(absolute_slot, bank->period, bank->off, bank->n_off);
    if (on && joint_score <= NR_CSIRS_BLIND_ZP_MIN_SCORE) {
      zp_failed(st, idx, bank);
      st->n_hit_slot[idx] = 0;
      zp_record_rejection(st, idx, absolute_slot);
      return false;
    }
    if (!hit)
      return false; // unknown/suppressed holes cannot supply held-out evidence
    if (on) {
      if (st->hits[idx] == UINT32_MAX) {
        st->zp_failed_run[idx] = UINT64_MAX;
        return false;
      }
      st->hits[idx]++;
      for (int j = 0; j < bank->n_off; j++) {
        if (absolute_slot > bank->proposed_slot && absolute_slot % bank->period == bank->off[j]) {
          if (bank->votes[j] == UINT64_MAX) {
            st->zp_failed_run[idx] = UINT64_MAX;
            return false;
          }
          bank->votes[j]++;
        }
      }
      const uint64_t lattice = nr_csirs_blind_zp_lattice_horizon();
      const uint64_t need = zp_required_votes(st, idx, bank->period);
      bool ready = absolute_slot - bank->proposed_slot >= lattice;
      for (int j = 0; j < bank->n_off; j++)
        ready &= bank->votes[j] >= need;
      if (ready && (uint64_t)st->zp_holes[idx] * 2 <= st->tried[idx] && zp_divisors_resolved(bank)
          && st->n_conf < NR_CSIRS_BLIND_MAX_CONF) {
        zp_export(st, idx, bank);
        return true;
      }
      return false;
    }
    /* A new measured interstitial hole changes the fitted period; do not
     * extrapolate a divisor merely from missing intermediate observations. */
    bool interstitial = false;
    for (int pi = 0; pi < NR_CSIRS_BLIND_N_PERIODS; pi++) {
      const unsigned d = nr_csirs_blind_periods[pi];
      if (d < bank->period && bank->period % d == 0 && zp_on(absolute_slot, d, bank->off, bank->n_off)
          && !zp_ledger_rejected(bank, d, bank->off, bank->n_off, bank->support_start))
        interstitial = true;
    }
    if (!interstitial)
      return false;
    bank->period = bank->n_off = 0;
    bank->votes[0] = bank->votes[1] = 0;
  }
  if (!hit) {
    /* A population-suppressed raw hole is unresolved, not evidence of occupancy.
     * Only a required symbol lacking a hole contradicts the proposed ZP phase. */
    if (joint_score <= NR_CSIRS_BLIND_ZP_MIN_SCORE)
      zp_record_rejection(st, idx, absolute_slot);
    return false;
  }
  /* Count raw holes, not population-qualified detections: a varying null can suppress a hit,
   * but cannot turn that hole into evidence of data. Repeated occasions count only once. */
  return record_hit(st, idx, absolute_slot, (uint64_t)st->zp_holes[idx] * 2 > st->tried[idx], true);
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

/* SEQUENCE-FREE positional evidence (2026-09-19). The correlation oracle needs the right sequence,
 * which needs the right scramblingID -- assumed to be the PCI here, and dedicated RRC is free to set
 * it otherwise, in which case a REAL resource scores like noise and is indistinguishable from an
 * empty hypothesis. This statistic ignores the sequence entirely and asks only whether the REs the
 * candidate's POSITIONS point at carry different power from their neighbours in the same RBs: mean
 * |y|^2 on the pattern REs over mean |y|^2 on the other REs of the touched RBs. A boosted pilot
 * reads > 1, an unused (zero-power) pattern < 1, and noise/PDSCH ~1. Restricted to touched RBs so an
 * unallocated guard band cannot skew it. Pure. Returns -1.0 when either side has no REs. */
/* Same grid-alignment argument as nr_csirs_blind_correlate_blocks_shift(): this compares power ON
 * the reference REs against power off them, so it too must read rxdataF at the FFT-ordered
 * position. Without the shift it measured power at unrelated subcarriers, which is why the OTA
 * "energy" reading drifted (3.27 -> 3.0 -> 1.4) instead of settling on a real pilot. */
double nr_csirs_blind_energy_ratio_shift(const int16_t *rx_re_im, const int16_t *ref_re_im, int n,
                                         int rx_shift)
{
  if (rx_re_im == NULL || ref_re_im == NULL || n <= 0) {
    return -1.0;
  }
  double e_on = 0.0, e_off = 0.0;
  int n_on = 0, n_off = 0;
  for (int rb0 = 0; rb0 + 12 <= n; rb0 += 12) {
    bool touched = false;
    for (int i = rb0; i < rb0 + 12; i++) {
      if (ref_re_im[2 * i] != 0 || ref_re_im[2 * i + 1] != 0) {
        touched = true;
        break;
      }
    }
    if (!touched) {
      continue;
    }
    for (int i = rb0; i < rb0 + 12; i++) {
      const int j = (int)(((long)i + rx_shift) % n);
      const double yr = (double)rx_re_im[2 * j], yi = (double)rx_re_im[2 * j + 1];
      const double e = yr * yr + yi * yi;
      if (ref_re_im[2 * i] != 0 || ref_re_im[2 * i + 1] != 0) {
        e_on += e;
        n_on++;
      } else {
        e_off += e;
        n_off++;
      }
    }
  }
  if (n_on == 0 || n_off == 0 || e_off <= 0.0) {
    return -1.0;
  }
  return (e_on / n_on) / (e_off / n_off);
}

double nr_csirs_blind_zero_score(const int16_t *rx_re_im, const int16_t *ref_re_im, int n)
{
  return nr_csirs_blind_zero_score_shift(rx_re_im, ref_re_im, n, 0);
}

double nr_csirs_blind_energy_ratio(const int16_t *rx_re_im, const int16_t *ref_re_im, int n)
{
  return nr_csirs_blind_energy_ratio_shift(rx_re_im, ref_re_im, n, 0);
}

/* CHANNEL-ROBUST SCORE (2026-09-19). The flat correlation above is the wrong oracle on air: it sums
 * y*conj(x) coherently across the WHOLE band, but the propagation channel rotates each subcarrier's
 * phase, and over 273 RB at 30 kHz even 100 ns of delay spread turns the phase by tens of radians
 * end to end. A perfectly correct sequence then averages to ~zero, which is exactly what was
 * measured OTA: the sequence-free energy test finds a TRS pair at 4-6x its neighbours while the
 * correlation sits at the noise floor for every scramblingID.
 *
 * So: correlate COHERENTLY inside sub-bands narrow enough for the channel to be flat, and combine
 * the magnitudes NON-COHERENTLY across them. Same single pass, no FFT. The return is normalised so
 * that noise reads ~1.0 at any candidate size and a perfect match reads ~sqrt(REs per block):
 * mean_b(rho_b) * sqrt(n_per_block) / 0.886, with 0.886 = E|rho| for Rayleigh noise.
 * `sub_res` is the number of consecutive REs per sub-band (of the reference's OCCUPIED REs). */
/* GRID ALIGNMENT, measured 2026-09-20 and the reason this search never matched on air.
 * nr_generate_csi_rs() writes the reference at CRB-order indices (k = rb*12 + koverline + kp, bins
 * 0..N_RB*12), which is the gNB transmit-grid convention. The UE rxdataF that nr_slot_fep_ant()
 * produces is FFT-ordered: the carrier starts at first_carrier_offset and wraps. Correlating the
 * two at the SAME index compares subcarriers ~2458 bins apart at 273 PRB / 4096, so the score sat
 * at the noise floor for every scramblingID (complete 1024 sweep) and every slot (complete 20
 * sweep) -- the sequence was never the problem, we were never looking at the resource.
 *
 * OAI own working receiver does exactly this mapping: csi_rx.c:185 reads
 *   k = (first_carrier_offset + rb*12 + koverline[cdm_id] + kp) % ofdm_symbol_size
 * so @p rx_shift is that first_carrier_offset and the modulo is the wrap.
 *
 * Shifting the RX read rather than rotating the reference keeps sub-bands contiguous in real
 * frequency, which the channel-robust score depends on -- rotating the reference instead would put
 * one sub-band astride the wrap, joining REs from opposite band edges. */
double nr_csirs_blind_correlate_blocks_shift(const int16_t *rx_re_im, const int16_t *ref_re_im, int n,
                                             int sub_res, int rx_shift, int *n_used)
{
  if (n_used != NULL) {
    *n_used = 0;
  }
  if (rx_re_im == NULL || ref_re_im == NULL || n <= 0 || sub_res <= 0) {
    return -1.0;
  }
  double acc_r = 0.0, acc_i = 0.0, e_rx = 0.0, e_ref = 0.0;
  double rho_sum = 0.0;
  int in_block = 0, blocks = 0, used = 0;
  for (int i = 0; i <= n; i++) {
    const bool flush = (i == n) || (in_block == sub_res);
    if (flush && in_block > 0) {
      if (e_rx > 0.0 && e_ref > 0.0) {
        const double r = sqrt(acc_r * acc_r + acc_i * acc_i) / (sqrt(e_rx) * sqrt(e_ref));
        rho_sum += (r > 1.0) ? 1.0 : r;
        blocks++;
      }
      acc_r = acc_i = e_rx = e_ref = 0.0;
      in_block = 0;
    }
    if (i == n) {
      break;
    }
    const double xr = (double)ref_re_im[2 * i], xi = (double)ref_re_im[2 * i + 1];
    if (xr == 0.0 && xi == 0.0) {
      continue;
    }
    const int j = (int)(((long)i + rx_shift) % n);
    const double yr = (double)rx_re_im[2 * j], yi = (double)rx_re_im[2 * j + 1];
    acc_r += yr * xr + yi * xi;
    acc_i += yi * xr - yr * xi;
    e_rx += yr * yr + yi * yi;
    e_ref += xr * xr + xi * xi;
    in_block++;
    used++;
  }
  if (n_used != NULL) {
    *n_used = used;
  }
  if (blocks == 0 || used == 0) {
    return -1.0;
  }
  const double per_block = (double)used / (double)blocks;
  return (rho_sum / blocks) * sqrt(per_block) / 0.886;
}

/* Zero-shift form: both grids already share one convention. Used by the offline tests, which build
 * rx and ref themselves and therefore cannot disagree about the layout. */
double nr_csirs_blind_correlate_blocks(const int16_t *rx_re_im, const int16_t *ref_re_im, int n,
                                       int sub_res, int *n_used)
{
  return nr_csirs_blind_correlate_blocks_shift(rx_re_im, ref_re_im, n, sub_res, 0, n_used);
}

/* BEST CONTIGUOUS RUN (2026-09-19). The mean-over-sub-bands score assumes the resource covers the
 * whole carrier, because that is what the candidates assert (start_rb 0, nr_of_rbs = N_RB_DL). A real
 * CSI-RS often does not: a TRS is commonly configured over a subset of the BWP. If it covers a
 * fraction f of the band, the MEAN reads ~f * sqrt(per_block)/0.886 -- for 52 of 273 RB that is ~1.2,
 * which is exactly where the OTA scores sat (1.31-1.40) after both the scramblingID and the slot
 * index had been excluded by complete sweeps. In other words the sequence may have been right all
 * along and simply averaged away.
 *
 * So score the best CONTIGUOUS RUN of sub-bands instead, and report where it is: that both detects a
 * partial-band resource and hands back its extent, which the caller can turn into start_rb/nr_of_rbs.
 * Runs of one block are excluded -- a single block is the easiest thing for noise to win. */
double nr_csirs_blind_correlate_bestrun(const int16_t *rx_re_im, const int16_t *ref_re_im, int n,
                                        int sub_res, int *first_block_out, int *n_blocks_out,
                                        int *n_used_out)
{
  if (first_block_out) *first_block_out = -1;
  if (n_blocks_out) *n_blocks_out = 0;
  if (n_used_out) *n_used_out = 0;
  if (rx_re_im == NULL || ref_re_im == NULL || n <= 0 || sub_res <= 0) {
    return -1.0;
  }
  enum { MAXB = 256 };
  double rho[MAXB];
  int nb = 0, used_total = 0;
  double acc_r = 0.0, acc_i = 0.0, e_rx = 0.0, e_ref = 0.0;
  int in_block = 0;
  for (int i = 0; i <= n && nb < MAXB; i++) {
    const bool flush = (i == n) || (in_block == sub_res);
    if (flush && in_block > 0) {
      rho[nb++] = (e_rx > 0.0 && e_ref > 0.0)
                      ? sqrt(acc_r * acc_r + acc_i * acc_i) / (sqrt(e_rx) * sqrt(e_ref))
                      : 0.0;
      acc_r = acc_i = e_rx = e_ref = 0.0;
      in_block = 0;
    }
    if (i == n) break;
    const double xr = (double)ref_re_im[2 * i], xi = (double)ref_re_im[2 * i + 1];
    if (xr == 0.0 && xi == 0.0) continue;
    const double yr = (double)rx_re_im[2 * i], yi = (double)rx_re_im[2 * i + 1];
    acc_r += yr * xr + yi * xi;
    acc_i += yi * xr - yr * xi;
    e_rx += yr * yr + yi * yi;
    e_ref += xr * xr + xi * xi;
    in_block++;
    used_total++;
  }
  if (n_used_out) *n_used_out = used_total;
  if (nb < 2) return -1.0;
  /* Best mean over any contiguous run of >= 2 blocks, normalised the same way as the whole-band
   * score so the two are directly comparable (noise ~1.0, perfect ~sqrt(sub_res)/0.886). */
  const double norm = sqrt((double)sub_res) / 0.886;
  double best = 0.0;
  for (int i = 0; i < nb; i++) {
    double sum = 0.0;
    for (int j = i; j < nb; j++) {
      sum += rho[j];
      const int len = j - i + 1;
      if (len < 2) continue;
      const double z = (sum / len) * norm;
      if (z > best) {
        best = z;
        if (first_block_out) *first_block_out = i;
        if (n_blocks_out) *n_blocks_out = len;
      }
    }
  }
  return best;
}

/* ---- rows 6-18: footprint-first (see the header) ------------------------------------------------ */

int nr_csirs_blind_footprint(const nr_csirs_candidate_t *c, uint16_t sym_mask[NR_CSIRS_BLIND_NSYM])
{
  memset(sym_mask, 0, NR_CSIRS_BLIND_NSYM * sizeof(sym_mask[0]));
  if (!nr_csirs_blind_candidate_safe(c)) {
    return -1;
  }
  /* OAI's own table, walked exactly as csi_rs_resource_mapping() walks it for one RB. */
  const csi_mapping_parms_t p = get_csi_mapping_parms(c->row, c->freq_domain, c->symb_l0, c->symb_l1);
  for (int ji = 0; ji < p.size; ji++) {
    for (int kp = 0; kp <= p.kprime; kp++) {
      for (int lp = 0; lp <= p.lprime; lp++) {
        const int k = p.koverline[ji] + kp, l = p.loverline[ji] + lp;
        if (k < 0 || k >= 12 || l < 0 || l >= NR_CSIRS_BLIND_NSYM) {
          return -1;
        }
        sym_mask[l] |= (uint16_t)(1u << k);
      }
    }
  }
  int n = 0;
  for (int l = 0; l < NR_CSIRS_BLIND_NSYM; l++) {
    n += __builtin_popcount(sym_mask[l]);
  }
  return n;
}

/* Largest-ratio gap of the sorted 12 powers: everything above it is "on" when the gap clears
 * NR_CSIRS_BLIND_FP_GAP. On noise or flat PDSCH the neighbouring sorted means differ by a few tens of
 * percent (each is a mean over >= 25 REs at 51 PRB per parity), never 2x. */
static uint16_t on_mask(const double v[12])
{
  double s[12];
  memcpy(s, v, sizeof(s));
  for (int i = 1; i < 12; i++) {
    const double x = s[i];
    int j = i - 1;
    while (j >= 0 && s[j] > x) { s[j + 1] = s[j]; j--; }
    s[j + 1] = x;
  }
  double best = 0.0, cut = 0.0;
  for (int i = 0; i < 11; i++) {
    if (s[i + 1] <= 0.0) {
      continue;
    }
    const double r = (s[i] > 0.0) ? s[i + 1] / s[i] : INFINITY;
    if (r > best) {
      best = r;
      cut = s[i + 1];
    }
  }
  if (best < NR_CSIRS_BLIND_FP_GAP) {
    return 0;
  }
  uint16_t m = 0;
  for (int k = 0; k < 12; k++) {
    if (v[k] >= cut) {
      m |= (uint16_t)(1u << k);
    }
  }
  return m;
}

void nr_csirs_blind_symbol_on(const int16_t *rx_re_im, int n_fft, int rx_shift, int n_rb,
                              uint16_t *on_even, uint16_t *on_odd)
{
  *on_even = *on_odd = 0;
  if (rx_re_im == NULL || n_fft <= 0 || n_rb <= 0 || n_rb * 12 > n_fft) {
    return;
  }
  double e[2][12] = {{0}};
  for (int i = 0; i < n_rb * 12; i++) {
    const int j = (int)(((long)i + rx_shift) % n_fft);
    const double yr = rx_re_im[2 * j], yi = rx_re_im[2 * j + 1];
    e[(i / 12) & 1][i % 12] += yr * yr + yi * yi;
  }
  *on_even = on_mask(e[0]);
  *on_odd = (n_rb > 1) ? on_mask(e[1]) : 0;
}

bool nr_csirs_blind_fp_record(nr_csirs_blind_fp_t *fp, int symbol, uint16_t on_even, uint16_t on_odd,
                              uint32_t absolute_slot)
{
  if (fp == NULL || symbol < 0 || symbol >= NR_CSIRS_BLIND_NSYM) {
    return false;
  }
  /* density index == freq_density: 0 = dot5 even RB, 1 = dot5 odd RB, 2 = one */
  const uint16_t by_d[3] = {(uint16_t)(on_even & ~on_odd), (uint16_t)(on_odd & ~on_even),
                            (uint16_t)(on_even & on_odd)};
  bool testable = false;
  for (int d = 0; d < 3; d++) {
    for (int k = 0; k < 12; k++) {
      if (!((by_d[d] >> k) & 1)) {
        continue;
      }
      /* Ring, not first-8: one spurious hit early on must not disqualify a cell forever. */
      uint8_t *w = &fp->w[d][symbol][k];
      fp->hit_slot[d][symbol][k][*w] = absolute_slot;
      *w = (uint8_t)((*w + 1) & 7);
      if (fp->n_hit[d][symbol][k] < 8) {
        fp->n_hit[d][symbol][k]++;
      }
      if (fp->n_hit[d][symbol][k] >= CSIRS_MIN_HITS) {
        testable = true;
      }
    }
  }
  return testable;
}

static bool cell_periodic(const nr_csirs_blind_fp_t *fp, int d, int l, int k)
{
  return nr_csirs_blind_infer_period(fp->hit_slot[d][l][k], fp->n_hit[d][l][k], CSIRS_MIN_HITS, NULL, NULL);
}

static bool fp_inside(const uint16_t *a, const uint16_t *b)   /* a subset of b */
{
  for (int l = 0; l < NR_CSIRS_BLIND_NSYM; l++) {
    if (a[l] & ~b[l]) {
      return false;
    }
  }
  return true;
}

/* CDM group size from OAI's table: ports / CDM groups (1, 2, 4, 8 -> noCDM, fd-CDM2, cdm4, cdm8). */
static uint8_t row_cdm_type(int row)
{
  const csi_mapping_parms_t p = get_csi_mapping_parms(row, 0x3F, 4, 8);   /* 6 bits: safe for any row */
  const int gs = p.ports / p.size;
  return (uint8_t)(gs >= 8 ? 3 : gs >= 4 ? 2 : gs >= 2 ? 1 : 0);
}

#define FP_MATCH_TMP 64
static int match_group(const uint16_t mask[NR_CSIRS_BLIND_NSYM], uint8_t density, uint16_t n_rb,
                       uint16_t scramb_id, nr_csirs_candidate_t *out, int max)
{
  /* WIDEST ROWS FIRST, sub-footprints rejected on insertion. A row-18 pattern admits 66+ narrower
   * fits (rows 6-15 inside it); enumerating narrow rows first filled the scratch before rows 15-18 were
   * reached, and the truth never made it into the result. Ports only fall as the row index falls, so a
   * later fit can never strictly contain an earlier one: every kept fit is maximal, and hitting the cap
   * drops only further maximal fits -- truncation is harmless by construction. */
  uint16_t mfp[FP_MATCH_TMP][NR_CSIRS_BLIND_NSYM];
  const int cap = (max < FP_MATCH_TMP) ? max : FP_MATCH_TMP;
  int nm = 0;
  for (int row = 18; row >= 6 && nm < cap; row--) {
    const int need = nr_csirs_blind_row_needs_bits((uint8_t)row), ports = nr_csirs_blind_row_ports((uint8_t)row);
    /* Density 0.5 exists only for the 16-32-port rows (11-18); rows 6-10 are density one. */
    if (density != 2 && ports < 16) {
      continue;
    }
    nr_csirs_candidate_t c = {0};
    c.row = (uint8_t)row;
    c.cdm_type = row_cdm_type(row);
    c.freq_density = density;
    c.scramb_id = scramb_id;
    c.start_rb = 0;
    c.nr_of_rbs = n_rb;
    /* Does the row place anything at l1? Asked of the table, not assumed. */
    uint16_t a[NR_CSIRS_BLIND_NSYM], b[NR_CSIRS_BLIND_NSYM], sym[NR_CSIRS_BLIND_NSYM];
    c.freq_domain = 0x3F;
    c.symb_l0 = 0;
    c.symb_l1 = 5;
    nr_csirs_blind_footprint(&c, a);
    c.symb_l1 = 6;
    nr_csirs_blind_footprint(&c, b);
    const bool uses_l1 = memcmp(a, b, sizeof(a)) != 0;
    for (int l0 = 0; l0 < NR_CSIRS_BLIND_NSYM; l0++) {
      /* l1 in {2..12} (38.211) and AFTER l0: the swapped order is the same RE set with the CDM groups
       * relabelled, a configuration no gNB sends, and it would double every l1-row hypothesis. */
      for (int l1 = uses_l1 ? (l0 + 1 > 2 ? l0 + 1 : 2) : 0; l1 <= (uses_l1 ? 12 : 0); l1++) {
        c.symb_l0 = (uint8_t)l0;
        c.symb_l1 = (uint8_t)l1;
        /* The symbol set does not depend on the bitmap: prune every (l0, l1) that overlaps itself,
         * leaves the slot, or touches a symbol with no measured energy before trying 20 bitmaps. */
        c.freq_domain = 0x3F;
        if (nr_csirs_blind_footprint(&c, sym) != ports) {
          continue;
        }
        bool dark = false;
        for (int l = 0; l < NR_CSIRS_BLIND_NSYM && !dark; l++) {
          dark = sym[l] && !mask[l];
        }
        if (dark) {
          continue;
        }
        for (int bm = 1; bm < 64; bm++) {
          if (__builtin_popcount(bm) != need) {
            continue;
          }
          c.freq_domain = (uint16_t)bm;
          if (nr_csirs_blind_footprint(&c, sym) != ports || !fp_inside(sym, mask)) {
            continue;
          }
          /* A strict sub-footprint (row 11 inside row 16, ...) is the same energy explained by fewer
           * ports, and the sequence stage could confirm it on its port-0 REs alone. Equal footprints
           * (rows 16/17, ...) are kept: only the sequence stage can separate those. */
          bool dominated = false;
          for (int j = 0; j < nm && !dominated; j++) {
            dominated = fp_inside(sym, mfp[j]) && !fp_inside(mfp[j], sym);
          }
          if (dominated || nm == cap) {
            continue;
          }
          out[nm] = c;
          memcpy(mfp[nm++], sym, sizeof(sym));
        }
      }
    }
  }
  return nm;
}

int nr_csirs_blind_fp_match(const nr_csirs_blind_fp_t *fp, uint16_t n_rb, uint16_t scramb_id,
                            nr_csirs_candidate_t *out, int max)
{
  if (fp == NULL || out == NULL || max <= 0) {
    return 0;
  }
  int n = 0;
  for (int d = 0; d < 3 && n < max; d++) {
    bool used[NR_CSIRS_BLIND_NSYM][12] = {{false}}, per[NR_CSIRS_BLIND_NSYM][12];
    for (int l = 0; l < NR_CSIRS_BLIND_NSYM; l++)
      for (int k = 0; k < 12; k++)
        per[l][k] = cell_periodic(fp, d, l, k);
    for (int l = 0; l < NR_CSIRS_BLIND_NSYM; l++) {
      for (int k = 0; k < 12; k++) {
        if (used[l][k] || !per[l][k]) {
          continue;
        }
        /* One resource's cells share its (period, offset): group every periodic cell whose hits are
         * jointly periodic with this seed's. */
        uint16_t mask[NR_CSIRS_BLIND_NSYM] = {0};
        mask[l] = (uint16_t)(1u << k);
        used[l][k] = true;
        for (int l2 = 0; l2 < NR_CSIRS_BLIND_NSYM; l2++) {
          for (int k2 = 0; k2 < 12; k2++) {
            if (used[l2][k2] || !per[l2][k2]) {
              continue;
            }
            uint32_t joint[16];
            const int n1 = fp->n_hit[d][l][k], n2 = fp->n_hit[d][l2][k2];
            memcpy(joint, fp->hit_slot[d][l][k], (size_t)n1 * sizeof(joint[0]));
            memcpy(joint + n1, fp->hit_slot[d][l2][k2], (size_t)n2 * sizeof(joint[0]));
            if (nr_csirs_blind_infer_period(joint, n1 + n2, CSIRS_MIN_HITS, NULL, NULL)) {
              mask[l2] |= (uint16_t)(1u << k2);
              used[l2][k2] = true;
            }
          }
        }
        n += match_group(mask, (uint8_t)d, n_rb, scramb_id, out + n, max - n);
      }
    }
  }
  return n;
}

int nr_csirs_blind_append(nr_csirs_blind_state_t *st, const nr_csirs_candidate_t *c)
{
  if (st == NULL || c == NULL || st->n >= NR_CSIRS_BLIND_MAX_CAND) {
    return -1;
  }
  for (int i = 0; i < st->n; i++) {
    const nr_csirs_candidate_t *o = &st->cand[i];
    if (o->row == c->row && o->freq_domain == c->freq_domain && o->symb_l0 == c->symb_l0
        && o->symb_l1 == c->symb_l1 && o->freq_density == c->freq_density) {
      return -1;
    }
  }
  const int idx = st->n++;
  st->cand[idx] = *c;
  return idx;
}
