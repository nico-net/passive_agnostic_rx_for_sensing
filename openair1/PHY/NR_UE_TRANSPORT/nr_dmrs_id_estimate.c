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
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */
#include "nr_dmrs_id_estimate.h"
#include "PHY/defs_nr_common.h"
#include "PHY/NR_REFSIG/nr_refsig.h"
#include "PHY/gold.h"
#include "common/utils/LOG/log.h"
#include "nfapi/open-nFAPI/nfapi/public_inc/nfapi_nr_interface.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Same c_init as refsig.c's nr_gold_pdsch() (TS 38.211 7.4.1.1.1), generated privately so a
 * 1024-candidate sweep never touches the receiver's shared per-thread gold cache. */
static void gold_for(uint32_t *seq, int words, int symbols_per_slot, int slot, int symbol, int nid, int nscid)
{
  uint64_t x2tmp0 = (((uint64_t)symbols_per_slot * slot + symbol + 1) * (((uint64_t)nid << 1) + 1)) << 17;
  uint32_t x2 = (uint32_t)((x2tmp0 + ((uint64_t)nid << 1) + nscid) % (1ULL << 31));
  uint32_t x1 = 0;
  seq[0] = gold_generic(&x1, &x2, 1);
  for (int n = 1; n < words; n++)
    seq[n] = gold_generic(&x1, &x2, 0);
}

void nr_dmrs_id_set_range(nr_dmrs_id_state_t *st, uint32_t first, uint32_t count)
{
  if (!st || count == 0)
    return;
  free(st->num_r); free(st->num_i); free(st->den);
  st->num_r = calloc(count, sizeof(double));
  st->num_i = calloc(count, sizeof(double));
  st->den   = calloc(count, sizeof(double));
  st->range_first = first;
  st->range_count = count;
  st->grants   = 0;
  st->decided  = false;
  st->best_id  = -1;
  st->margin_db = 0.0;
}

void nr_dmrs_id_init(nr_dmrs_id_state_t *st, const char *label, int assumed_id)
{
  memset(st, 0, sizeof(*st));
  st->best_id = -1;
  st->assumed_id = assumed_id;
  st->label = label ? label : "DMRS";
  nr_dmrs_id_set_range(st, 0, NR_DMRS_ID_CANDIDATES);
}

int nr_dmrs_id_accumulate(nr_dmrs_id_state_t *st, const c16_t *rx_symbol, int ofdm_symbol_size,
                          int start_subcarrier, int rb_offset, int nb_rb, int N_RB,
                          int symbols_per_slot, int slot, int symbol, int nscid, int normal_cp, int dmrs_type)
{
  if (!st || !rx_symbol || ofdm_symbol_size <= 0 || nb_rb <= 0 || rb_offset < 0 || N_RB <= 0
      || nb_rb + rb_offset > N_RB || st->decided || !st->num_r || st->range_count == 0)
    return 0;
  /* Type 1 CDM group 0: 6 pilot REs/RB, comb-2 (get_dmrs_freq_idx_ul: 4n+2k'). Type 2 CDM group 0:
   * 4 pilot REs/RB, 2 adjacent REs every 6 (get_dmrs_freq_idx_ul: 6n+k'). Was hardcoded to type 1's
   * pattern unconditionally -- a type-2 cell's estimate silently correlated against the wrong REs. */
  const int nb_dmrs = (dmrs_type == NFAPI_NR_DMRS_TYPE2) ? 4 : 6;
  const int words   = ((N_RB * 24) >> 5) + 1;
  const int npil    = nb_dmrs * nb_rb;
  const int ntot    = nb_dmrs * (nb_rb + rb_offset);   // pilots from the reference point
  uint32_t *seq     = malloc((size_t)words * sizeof(*seq));
  c16_t    *pilot   = malloc((size_t)ntot * sizeof(*pilot));
  if (!seq || !pilot) { free(seq); free(pilot); return 0; }

  for (uint32_t li = 0; li < st->range_count; ++li) {
    const uint32_t id = st->range_first + li;
    gold_for(seq, words, symbols_per_slot, slot, symbol, (int)id, nscid);
    /* Port 1000, first DM-RS symbol (lp = 0), unit scaling: identical to the receiver's estimator
     * except for the identity under test. */
    nr_pdsch_dmrs_rx(normal_cp ? NR_NORMAL : NR_EXTENDED, seq, pilot, 1000, 0, (unsigned short)(nb_rb + rb_offset), dmrs_type, 16384);
    const c16_t *pil = &pilot[nb_dmrs * rb_offset];
    double prev_r = 0, prev_i = 0, num_r = 0, num_i = 0, den = 0;
    for (int m = 0; m < npil; ++m) {
      /* get_dmrs_freq_idx_ul(n=m/2, k'=m%2, delta=0, dmrs_type), inlined: CDM group 0 either way. */
      const int off = (dmrs_type == NFAPI_NR_DMRS_TYPE2) ? (6 * (m / 2) + (m % 2)) : (4 * (m / 2) + 2 * (m % 2));
      const int re = ((start_subcarrier + off) % ofdm_symbol_size + ofdm_symbol_size) % ofdm_symbol_size;
      const c16_t h = c16mulShift(pil[m], rx_symbol[re], 15);
      const double hr = h.r, hi = h.i;
      den += hr * hr + hi * hi;
      if (m) { // h[m] * conj(h[m-1])
        num_r += hr * prev_r + hi * prev_i;
        num_i += hi * prev_r - hr * prev_i;
      }
      prev_r = hr; prev_i = hi;
    }
    st->num_r[li] += num_r; st->num_i[li] += num_i; st->den[li] += den;
  }
  free(seq); free(pilot);
  ++st->grants;
  return (int)st->range_count;
}

double nr_dmrs_port_pair_coherence(const c16_t *rx_symbol, int ofdm_symbol_size, int start_subcarrier,
                                   int rb_offset, int nb_rb, int N_RB, int symbols_per_slot, int slot,
                                   int symbol, int nscid, int nid, int normal_cp)
{
  if (!rx_symbol || ofdm_symbol_size <= 0 || nb_rb <= 0 || rb_offset < 0 || N_RB <= 0 || nb_rb + rb_offset > N_RB
      || nid < 0 || nid >= NR_DMRS_ID_CANDIDATES)
    return -1.0;
  const int words = ((N_RB * 24) >> 5) + 1, npil = 6 * nb_rb, ntot = 6 * (nb_rb + rb_offset);
  uint32_t *seq = malloc((size_t)words * sizeof(*seq));
  c16_t *pilot  = malloc((size_t)ntot * sizeof(*pilot));
  if (!seq || !pilot) { free(seq); free(pilot); return -1.0; }
  gold_for(seq, words, symbols_per_slot, slot, symbol, nid, nscid);
  nr_pdsch_dmrs_rx(normal_cp ? NR_NORMAL : NR_EXTENDED, seq, pilot, 1000, 0, (unsigned short)(nb_rb + rb_offset),
                   NFAPI_NR_DMRS_TYPE1, 16384);
  const c16_t *pil = &pilot[6 * rb_offset];
  int re = ((start_subcarrier % ofdm_symbol_size) + ofdm_symbol_size) % ofdm_symbol_size;
  double num_r = 0, num_i = 0, den = 0, er = 0, ei = 0;
  for (int m = 0; m < npil; ++m) {
    const c16_t h = c16mulShift(pil[m], rx_symbol[re], 15);
    const double hr = h.r, hi = h.i;
    den += hr * hr + hi * hi;
    if (m & 1) { num_r += er * hr + ei * hi; num_i += ei * hr - er * hi; } // h[2n] conj(h[2n+1])
    else       { er = hr; ei = hi; }
    re = (re + 2) % ofdm_symbol_size;
  }
  free(seq); free(pilot);
  return den > 0 ? 2.0 * sqrt(num_r * num_r + num_i * num_i) / den : 0.0; // x2: pairs count half the energy
}

/* Per-PRB DM-RS pair coherence over the WHOLE carrier (port 1000, type 1, CRB0-referenced sequence,
 * i.e. refPoint 0): out[p] = 2|sum h2n conj(h2n+1)| / sum|h|^2 over CRB p's 6 pilots. Close to 1 where
 * PDSCH DM-RS with this sequence is present -- any UE's -- and small where it is not. Used by the
 * passive BWP discovery (nr_passive_bwp.h) to place a grant on the carrier. */
void nr_dmrs_prb_coherence(const c16_t *rx_symbol, int ofdm_symbol_size, int first_carrier_offset, int N_RB,
                           int symbols_per_slot, int slot, int symbol, int nscid, int nid, int normal_cp,
                           float *out)
{
  const int words = ((N_RB * 24) >> 5) + 1, npil = 6 * N_RB;
  uint32_t *seq = malloc((size_t)words * sizeof(*seq));
  c16_t *pilot  = malloc((size_t)npil * sizeof(*pilot));
  if (!seq || !pilot || N_RB <= 0) {
    free(seq); free(pilot);
    for (int p = 0; p < N_RB; p++) out[p] = 0.0f;
    return;
  }
  gold_for(seq, words, symbols_per_slot, slot, symbol, nid, nscid);
  nr_pdsch_dmrs_rx(normal_cp ? NR_NORMAL : NR_EXTENDED, seq, pilot, 1000, 0, (unsigned short)N_RB,
                   NFAPI_NR_DMRS_TYPE1, 16384);
  int re = ((first_carrier_offset % ofdm_symbol_size) + ofdm_symbol_size) % ofdm_symbol_size;
  for (int p = 0; p < N_RB; p++) {
    double num_r = 0, num_i = 0, den = 0, er = 0, ei = 0;
    for (int m = 0; m < 6; m++) {
      const c16_t h = c16mulShift(pilot[6 * p + m], rx_symbol[re], 15);
      const double hr = h.r, hi = h.i;
      den += hr * hr + hi * hi;
      if (m & 1) { num_r += er * hr + ei * hi; num_i += ei * hr - er * hi; }
      else       { er = hr; ei = hi; }
      re = (re + 2) % ofdm_symbol_size;
    }
    out[p] = den > 0 ? (float)(2.0 * sqrt(num_r * num_r + num_i * num_i) / den) : 0.0f;
  }
  free(seq); free(pilot);
}

static int cmp_double(const void *a, const void *b)
{
  const double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}
/* Score by LOCAL index into the current window (0..range_count-1), no id translation. */
static double score_local(const nr_dmrs_id_state_t *st, uint32_t li)
{
  if (!st || !st->den || li >= st->range_count || st->den[li] <= 0) return 0.0;
  return sqrt(st->num_r[li] * st->num_r[li] + st->num_i[li] * st->num_i[li]) / st->den[li];
}
double nr_dmrs_id_score(const nr_dmrs_id_state_t *st, int id)
{
  if (!st || id < 0 || (uint32_t)id < st->range_first) return 0.0;
  const uint32_t li = (uint32_t)id - st->range_first;
  return score_local(st, li);
}
static double median_score(const nr_dmrs_id_state_t *st)
{
  double *tmp = malloc((size_t)st->range_count * sizeof(double));
  if (!tmp) return 0.0;
  for (uint32_t i = 0; i < st->range_count; ++i) tmp[i] = score_local(st, i);
  qsort(tmp, st->range_count, sizeof(double), cmp_double);
  const double med = 0.5 * (tmp[(st->range_count - 1) / 2] + tmp[st->range_count / 2]);
  free(tmp);
  return med;
}
double nr_dmrs_id_margin_db(const nr_dmrs_id_state_t *st, int id)
{
  if (!st) return -INFINITY;
  const double med = median_score(st), sc = nr_dmrs_id_score(st, id);
  if (med <= 0 || sc <= 0) return -INFINITY;
  return 10.0 * log10(sc / med);
}

bool nr_dmrs_id_decide(nr_dmrs_id_state_t *st, uint32_t min_grants, double min_margin_db)
{
  if (!st || st->decided || st->grants < min_grants || st->range_count == 0) return false;
  uint32_t best_li = 0;
  double best_sc = score_local(st, 0);
  for (uint32_t li = 1; li < st->range_count; ++li) {
    const double sc = score_local(st, li);
    if (sc > best_sc) { best_sc = sc; best_li = li; }
  }
  const int best = (int)(st->range_first + best_li);
  const double margin = nr_dmrs_id_margin_db(st, best);
  if (margin < min_margin_db) return false;
  /* Runner-up too: a decision is only as good as its separation from the next candidate. */
  uint32_t second_li = best_li == 0 ? 1 : 0;
  double second_sc = st->range_count > 1 ? score_local(st, second_li) : 0.0;
  for (uint32_t li = 0; li < st->range_count; ++li) {
    if (li == best_li) continue;
    const double sc = score_local(st, li);
    if (sc > second_sc) { second_sc = sc; second_li = li; }
  }
  const double sep = 10.0 * log10(best_sc / (second_sc > 0 ? second_sc : 1e-300));
  const int second = (int)(st->range_first + second_li);
  st->best_id = best;
  st->margin_db = margin;
  st->decided = true;
  if (best == st->assumed_id)
    LOG_A(PHY, "SENSING: DMRS_ID %s CONFIRMED n_id=%d (assumed %d) margin_over_median=%.1f dB "
               "over_runner_up=%.1f dB (runner-up %d) grants=%u\n",
          st->label, best, st->assumed_id, margin, sep, second, st->grants);
  else
    LOG_E(PHY, "SENSING: DMRS_ID %s MISMATCH estimated n_id=%d but receiver assumes %d "
               "(margin_over_median=%.1f dB over_runner_up=%.1f dB grants=%u) -- every %s decode "
               "is descrambling with the wrong identity\n",
          st->label, best, st->assumed_id, margin, sep, st->grants, st->label);
  return true;
}

void nr_dmrs_id_2stage_init(nr_dmrs_id_2stage_t *t, const char *label, int assumed_id)
{
  memset(t, 0, sizeof(*t));
  nr_dmrs_id_init(&t->s1, label, assumed_id);
  t->s2.best_id = -1;
  t->s2.assumed_id = assumed_id;
  t->s2.label = t->s1.label;
  t->s1_grants = NR_DMRS_ID_STAGE1_GRANTS;
  t->s2_throttle = NR_DMRS_ID_STAGE2_THROTTLE;
  t->s2_max_evals = NR_DMRS_ID_STAGE2_MAX_EVALS;
}

bool nr_dmrs_id_2stage_accumulate(nr_dmrs_id_2stage_t *t, const c16_t *rx_symbol, int ofdm_symbol_size,
                                  int start_subcarrier, int rb_offset, int nb_rb, int N_RB, int symbols_per_slot,
                                  int slot, int symbol, int nscid, int normal_cp, int dmrs_type)
{
  if (!t || t->decided_p1 > 0)
    return false;
  if (nr_dmrs_id_accumulate(&t->s1, rx_symbol, ofdm_symbol_size, start_subcarrier, rb_offset, nb_rb, N_RB,
                            symbols_per_slot, slot, symbol, nscid, normal_cp, dmrs_type)
      && nr_dmrs_id_decide(&t->s1, 16, 10.0)) {
    __atomic_store_n(&t->decided_p1, t->s1.best_id + 1, __ATOMIC_RELEASE);
    return true;
  }
  if (!t->s2_armed && t->s1.grants >= t->s1_grants) {
    t->s2_armed = true;
    nr_dmrs_id_set_range(&t->s2, NR_DMRS_ID_CANDIDATES, NR_DMRS_ID_SPACE - NR_DMRS_ID_CANDIDATES);
    LOG_W(PHY, "SENSING: DMRS_ID %s 0..%d undecided after %u grants: also sweeping %d..%d (throttled 1/%u, at most %u "
               "evaluations); 0..%d keeps accumulating\n",
          t->s1.label, NR_DMRS_ID_CANDIDATES - 1, t->s1.grants, NR_DMRS_ID_CANDIDATES, NR_DMRS_ID_SPACE - 1,
          t->s2_throttle, t->s2_max_evals, NR_DMRS_ID_CANDIDATES - 1);
  }
  if (!t->s2_armed || t->s2_evals >= t->s2_max_evals || !t->s2.num_r)
    return false;
  if ((t->s2_tick++ % (t->s2_throttle ? t->s2_throttle : 1)) != 0)
    return false;
  t->s2_evals++;
  if (nr_dmrs_id_accumulate(&t->s2, rx_symbol, ofdm_symbol_size, start_subcarrier, rb_offset, nb_rb, N_RB,
                            symbols_per_slot, slot, symbol, nscid, normal_cp, dmrs_type)
      && nr_dmrs_id_decide(&t->s2, 16, 10.0)) {
    __atomic_store_n(&t->decided_p1, t->s2.best_id + 1, __ATOMIC_RELEASE);
    return true;
  }
  if (t->s2_evals >= t->s2_max_evals) {
    LOG_W(PHY, "SENSING: DMRS_ID %s %d..%d undecided after %u evaluations: stage 2 stopped (0..%d still accumulating)\n",
          t->s2.label, NR_DMRS_ID_CANDIDATES, NR_DMRS_ID_SPACE - 1, t->s2_evals, NR_DMRS_ID_CANDIDATES - 1);
    free(t->s2.num_r); free(t->s2.num_i); free(t->s2.den);
    t->s2.num_r = t->s2.num_i = t->s2.den = NULL;
    t->s2.range_count = 0;
  }
  return false;
}
