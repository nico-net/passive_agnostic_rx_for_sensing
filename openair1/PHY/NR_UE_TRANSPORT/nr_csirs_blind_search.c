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
  if (st->confirmed >= 0) {
    return st->confirmed;
  }
  if (st->pin_left > 0 && st->pinned >= 0 && st->pinned < st->n) {
    st->pin_left--;
    return st->pinned;
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

/* Same grid-alignment requirement as the other two comparators: this reads rxdataF at the
 * reference's RE positions, so it must map CRB order to FFT order. It searches ZP CSI-RS, whose
 * only evidence IS the energy, so a misaligned read does not merely weaken it -- it measures a
 * different part of the spectrum entirely. */
double nr_csirs_blind_zero_score_shift(const int16_t *rx_re_im, const int16_t *ref_re_im, int n,
                                       int rx_shift)
{
  if (rx_re_im == NULL || ref_re_im == NULL || n <= 0) {
    return -1.0;
  }
  /* RBs the pattern touches: a 12-RE granularity mask on the caller's index (symbol-relative
   * indices keep the RB grid; a whole-symbol buffer starts at RB 0 either way). */
  double e_on = 0.0, e_off = 0.0;
  int n_on = 0, n_off = 0;
  for (int rb0 = 0; rb0 + 12 <= n; rb0 += 12) {
    bool touched = false;
    for (int i = rb0; i < rb0 + 12; i++) {
      if (ref_re_im[2 * i] != 0 || ref_re_im[2 * i + 1] != 0) { touched = true; break; }
    }
    if (!touched) {
      continue;
    }
    for (int i = rb0; i < rb0 + 12; i++) {
      const int j = (int)(((long)i + rx_shift) % n);
      const double yr = (double)rx_re_im[2 * j], yi = (double)rx_re_im[2 * j + 1];
      const double e = yr * yr + yi * yi;
      if (ref_re_im[2 * i] != 0 || ref_re_im[2 * i + 1] != 0) { e_on += e; n_on++; }
      else { e_off += e; n_off++; }
    }
  }
  if (n_on == 0 || n_off == 0 || e_off <= 0.0) {
    return -1.0;
  }
  const double ratio = (e_on / n_on) / (e_off / n_off);
  return 1.0 - (ratio > 1.0 ? 1.0 : ratio);
}

bool nr_csirs_blind_zp_feed(nr_csirs_blind_state_t *st, int idx, uint32_t absolute_slot,
                            double score, double score_null)
{
  if (st == NULL || idx < 0 || idx >= st->n) {
    return false;
  }
  if (st->confirmed >= 0) {
    return true;
  }
  /* Same relative bar as the NZP feed; the null population is the other candidates' zero scores,
   * which sit near 0 on data (E_on ~ E_off). A minimum absolute margin keeps a null median of
   * ~0 from turning every small fluctuation into a hit. */
  const bool hit = score > 0.5 && score_null >= 0.0 && score > CSIRS_DETECT_MARGIN * score_null;
  st->tried[idx]++;
  if (score > st->best_rho[idx]) {
    st->best_rho[idx] = score;
  }
  if (!hit) {
    return false;
  }
  st->hits[idx]++;
  if (st->n_hit_slot[idx] < 8) {
    st->hit_slot[idx][st->n_hit_slot[idx]++] = absolute_slot;
  }
  if (st->n_hit_slot[idx] < CSIRS_MIN_HITS || st->hits[idx] * 2 > st->tried[idx]) {
    return false; /* not enough evidence, or a structural hole (hit on most tests) */
  }
  uint16_t p = 0, o = 0;
  if (!nr_csirs_blind_infer_period(st->hit_slot[idx], st->n_hit_slot[idx], CSIRS_MIN_HITS, &p, &o)) {
    return false;
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
