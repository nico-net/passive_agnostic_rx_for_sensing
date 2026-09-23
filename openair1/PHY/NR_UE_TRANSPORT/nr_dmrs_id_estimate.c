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

void nr_dmrs_id_init(nr_dmrs_id_state_t *st, const char *label, int assumed_id)
{
  memset(st, 0, sizeof(*st));
  st->best_id = -1;
  st->assumed_id = assumed_id;
  st->label = label ? label : "DMRS";
}

int nr_dmrs_id_accumulate(nr_dmrs_id_state_t *st, const c16_t *rx_symbol, int ofdm_symbol_size,
                          int start_subcarrier, int rb_offset, int nb_rb, int N_RB,
                          int symbols_per_slot, int slot, int symbol, int nscid, int normal_cp)
{
  if (!st || !rx_symbol || ofdm_symbol_size <= 0 || nb_rb <= 0 || rb_offset < 0 || N_RB <= 0
      || nb_rb + rb_offset > N_RB || st->decided)
    return 0;
  const int words   = ((N_RB * 24) >> 5) + 1;
  const int npil    = 6 * nb_rb;                 // type 1: one pilot every 2nd subcarrier
  const int ntot    = 6 * (nb_rb + rb_offset);   // pilots from the reference point
  uint32_t *seq     = malloc((size_t)words * sizeof(*seq));
  c16_t    *pilot   = malloc((size_t)ntot * sizeof(*pilot));
  if (!seq || !pilot) { free(seq); free(pilot); return 0; }

  for (int id = 0; id < NR_DMRS_ID_CANDIDATES; ++id) {
    gold_for(seq, words, symbols_per_slot, slot, symbol, id, nscid);
    /* Port 1000, first DM-RS symbol (lp = 0), unit scaling: identical to the receiver's estimator
     * except for the identity under test. */
    nr_pdsch_dmrs_rx(normal_cp ? NR_NORMAL : NR_EXTENDED, seq, pilot, 1000, 0, (unsigned short)(nb_rb + rb_offset), NFAPI_NR_DMRS_TYPE1, 16384);
    const c16_t *pil = &pilot[6 * rb_offset];
    int re = ((start_subcarrier % ofdm_symbol_size) + ofdm_symbol_size) % ofdm_symbol_size;
    double prev_r = 0, prev_i = 0, num_r = 0, num_i = 0, den = 0;
    for (int m = 0; m < npil; ++m) {
      const c16_t h = c16mulShift(pil[m], rx_symbol[re], 15);
      const double hr = h.r, hi = h.i;
      den += hr * hr + hi * hi;
      if (m) { // h[m] * conj(h[m-1])
        num_r += hr * prev_r + hi * prev_i;
        num_i += hi * prev_r - hr * prev_i;
      }
      prev_r = hr; prev_i = hi;
      re = (re + 2) % ofdm_symbol_size;
    }
    st->num_r[id] += num_r; st->num_i[id] += num_i; st->den[id] += den;
  }
  free(seq); free(pilot);
  ++st->grants;
  return NR_DMRS_ID_CANDIDATES;
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
double nr_dmrs_id_score(const nr_dmrs_id_state_t *st, int id)
{
  if (!st || id < 0 || id >= NR_DMRS_ID_CANDIDATES || st->den[id] <= 0) return 0.0;
  return sqrt(st->num_r[id] * st->num_r[id] + st->num_i[id] * st->num_i[id]) / st->den[id];
}
static double median_score(const nr_dmrs_id_state_t *st)
{
  double tmp[NR_DMRS_ID_CANDIDATES];
  for (int i = 0; i < NR_DMRS_ID_CANDIDATES; ++i) tmp[i] = nr_dmrs_id_score(st, i);
  qsort(tmp, NR_DMRS_ID_CANDIDATES, sizeof(double), cmp_double);
  return 0.5 * (tmp[NR_DMRS_ID_CANDIDATES / 2 - 1] + tmp[NR_DMRS_ID_CANDIDATES / 2]);
}
double nr_dmrs_id_margin_db(const nr_dmrs_id_state_t *st, int id)
{
  const double med = median_score(st), sc = nr_dmrs_id_score(st, id);
  if (med <= 0 || sc <= 0) return -INFINITY;
  return 10.0 * log10(sc / med);
}

bool nr_dmrs_id_decide(nr_dmrs_id_state_t *st, uint32_t min_grants, double min_margin_db)
{
  if (!st || st->decided || st->grants < min_grants) return false;
  double sc[NR_DMRS_ID_CANDIDATES];
  int best = 0;
  for (int id = 0; id < NR_DMRS_ID_CANDIDATES; ++id) { sc[id] = nr_dmrs_id_score(st, id); if (sc[id] > sc[best]) best = id; }
  const double margin = nr_dmrs_id_margin_db(st, best);
  if (margin < min_margin_db) return false;
  /* Runner-up too: a decision is only as good as its separation from the next candidate. */
  int second = best == 0 ? 1 : 0;
  for (int id = 0; id < NR_DMRS_ID_CANDIDATES; ++id)
    if (id != best && sc[id] > sc[second]) second = id;
  const double sep = 10.0 * log10(sc[best] / (sc[second] > 0 ? sc[second] : 1e-300));
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
