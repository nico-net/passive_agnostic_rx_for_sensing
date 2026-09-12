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
#ifndef NR_DMRS_ID_ESTIMATE_H
#define NR_DMRS_ID_ESTIMATE_H
#include <stdbool.h>
#include <stdint.h>
#include "PHY/TOOLS/tools_defs.h" // c16_t only; the heavier PHY headers are not C++-clean and stay in the .c
#ifdef __cplusplus
extern "C" {
#endif

/* Blind estimation of the DM-RS scrambling identity (N_ID^{n_SCID}, 0..1023) of a PDSCH or
 * CP-OFDM PUSCH from received DM-RS resource elements. This is the one dedicated (RRC-configured,
 * ciphered) parameter the passive receiver has been ASSUMING equals the PCI: on this cell that
 * assumption is true, on another cell nothing would say it broke except the CRC rate collapsing.
 *
 * Method: for every candidate N_ID the type-1, port-0 pilot sequence is regenerated with the same
 * primitives the receiver's own channel estimator uses (nr_pdsch_dmrs_rx over a Gold sequence),
 * an LS estimate h_c[m] = pilot_c[m] * y[m] is formed on the comb pilots, and the score is the
 * adjacent-pilot coherence  |sum_m h_c[m] conj(h_c[m+1])| / sum_m |h_c[m]|^2.  Adjacent pilots are
 * two subcarriers apart, over which any physical channel is highly correlated, so the TRUE identity
 * sums coherently (score -> 1) while a wrong identity rotates each term by a pseudo-random QPSK
 * phase (score ~ 1/sqrt(M)). Scores accumulate across grants; a decision needs both a minimum number
 * of grants and a minimum margin, in dB, of the best candidate over the median of all 1024 -- a
 * relative gate, so it does not depend on gain, SNR or allocation size.
 *
 * Cost: ~1024 x (gold generation + 6*nb_rb complex MACs) per grant, single-threaded, no allocation
 * after init. Never call this on the PHY receive thread; the deferred decode consumers are the
 * intended call sites. */
#define NR_DMRS_ID_CANDIDATES 1024

typedef struct {
  /* Complex numerator sum h[m]conj(h[m-1]) and real denominator sum |h|^2, accumulated ACROSS
   * grants per candidate. Kept separate on purpose: accumulating per-grant |num|/den would be
   * biased positive (the magnitude of a random walk is never zero) and the margin would stop
   * growing with evidence; with complex accumulation a wrong candidate's numerator keeps
   * random-walking down as 1/sqrt(total pilots) while the true one adds coherently. */
  double   num_r[NR_DMRS_ID_CANDIDATES], num_i[NR_DMRS_ID_CANDIDATES], den[NR_DMRS_ID_CANDIDATES];
  uint32_t grants;                       // grants accumulated so far
  int      best_id;                      // -1 until decided
  double   margin_db;                    // best over median, at decision time
  bool     decided;
  int      assumed_id;                   // what the receiver is currently using (PCI by default)
  const char *label;                     // "PDSCH" / "PUSCH", for logging only
} nr_dmrs_id_state_t;

void nr_dmrs_id_init(nr_dmrs_id_state_t *st, const char *label, int assumed_id);

/* Accumulate one DM-RS symbol of one grant.
 *   rx_symbol        : frequency-domain samples of the DM-RS OFDM symbol, ofdm_symbol_size long
 *                      (i.e. &rxdataF[ant][symbol * ofdm_symbol_size]); delta of CDM group 0 is 0
 *   start_subcarrier : absolute subcarrier index of the allocation's first RB in that array,
 *                      already including first_carrier_offset (the estimator's bwp_start_subcarrier)
 *   rb_offset        : allocation's first RB relative to the DM-RS sequence reference point
 *                      (the estimator's rb_offset: first_rb + BWPStart unless refPoint says CORESET0)
 *   nb_rb            : allocation width in RBs
 *   N_RB, symbols_per_slot, slot, symbol, nscid : as passed to nr_gold_pdsch/nr_pdsch_dmrs_rx
 *   normal_cp        : 1 for normal CP (extended CP is not supported by the pilot generator either)
 * Returns the number of candidates scored (1024) or 0 on invalid input. */
int nr_dmrs_id_accumulate(nr_dmrs_id_state_t *st, const c16_t *rx_symbol, int ofdm_symbol_size,
                          int start_subcarrier, int rb_offset, int nb_rb, int N_RB,
                          int symbols_per_slot, int slot, int symbol, int nscid, int normal_cp);

/* Decide once enough evidence exists. Returns true exactly when the decision is first made.
 * min_margin_db is the best-over-median gate; 10 dB is comfortably above what 1023 wrong
 * candidates ever reach on a real allocation (measured: see the handover doc) and far below what
 * the true identity reaches with >= 50 pilots. */
bool nr_dmrs_id_decide(nr_dmrs_id_state_t *st, uint32_t min_grants, double min_margin_db);

/* Coherence score of one candidate, and its margin over the median of all 1024 in dB. */
double nr_dmrs_id_score(const nr_dmrs_id_state_t *st, int id);
double nr_dmrs_id_margin_db(const nr_dmrs_id_state_t *st, int id);

#ifdef __cplusplus
}
#endif
#endif
