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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c
 * \brief Implementation for nr_pdcch_coreset_map.h -- see that header for the design rationale.
 */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "nr_pdcch_coreset_map.h"

/* Real current signatures (openair1/PHY/NR_REFSIG/nr_refsig.h) -- do NOT copy the task brief's
 * approximate forward declarations verbatim. In particular nr_pdcch_dmrs_ref()'s third argument
 * is an RB COUNT, not a raw output-element count: it writes nb_rb_coreset*3 complex pilots
 * (nr_dmrs_rx.c: `for (i = 0; i < (nb_rb_coreset*6)>>1; i++)`), i.e. 3 DM-RS REs/RB starting from
 * absolute RB 0 of the gold sequence generated for N_RB_DL RBs. Passing an already-tripled count
 * here (as a naive reading of "count" might suggest) overflows the pilot buffer by 3x. */
extern uint32_t *nr_gold_pdcch(int N_RB_DL, int symbols_per_slot, unsigned short n_idDMRS, int ns, int l);
extern void nr_pdcch_dmrs_ref(const unsigned int *nr_gold_pdcch, c16_t *output, unsigned short nb_rb_coreset);

// Pure noise gives |corr| ~= sqrt(pi)/(2*sqrt(18)) ~= 0.209 for an 18-pilot (6-RB, 3 DM-RS
// RE/RB) window -- this project's own already-derived figure (dci_nr.c's nr_pdcch_blind_dmrs_probe
// comment). A real DM-RS measured 0.8-0.95 live. Set the bar at 4x the noise floor (~0.836), well
// clear of noise, comfortably below a real hit -- NOT at 0.5x(noise+signal), because the noise
// distribution's own upper tail (not just its mean) is what a real significance bar must clear;
// see dci_nr.c's own comment on why "max over many trials grows only as sqrt(ln(trials)/18)".
#define CORESET_MAP_CORR_THRESHOLD 0.836

int nr_pdcch_coreset_map_scan(const c16_t* rxdataF,
                              int          ofdm_symbol_size,
                              int          n_rb_carrier,
                              int          first_carrier_offset,
                              uint16_t     scrambling_id,
                              int          slot,
                              int          symbol,
                              nr_pdcch_coreset_candidate_t* candidates_out,
                              int          max_candidates)
{
  const int n_windows = n_rb_carrier / 6;
  if (n_windows <= 0 || max_candidates <= 0) {
    return 0;
  }

  // Generate the reference DM-RS for the whole carrier once, indexed by absolute RB (rb*3 + p),
  // exactly mirroring dci_nr.c's own pilot layout for nr_pdcch_channel_estimation().
  uint32_t *gold = nr_gold_pdcch(n_rb_carrier, 14, scrambling_id, slot, symbol);
  c16_t pilot[n_rb_carrier * 3];
  nr_pdcch_dmrs_ref(gold, pilot, (unsigned short)n_rb_carrier);

  int found = 0;
  /* TEMPORARY DIAGNOSTIC (2026-09-05, Task 5 live validation): zero candidates ever cleared the
   * significance bar on live air over 2800+ calls. Track raw max/rb0 correlation regardless of
   * threshold, rate-limited, to see how close (or far) live air gets vs the 0.836 bar and vs the
   * synthetic test's 0.8-0.95 assumption. */
  static int s_diag = -1;
  if (s_diag < 0)
    s_diag = (getenv("ISAC_DISCOVER_DIAG") != NULL) ? 1 : 0;
  static int s_calls = 0;
  s_calls++;
  double diag_max_corr = 0.0;
  int diag_max_rb = -1;
  double diag_rb0_corr = -1.0;
  for (int w = 0; w < n_windows; w++) {
    const int rb_offset = w * 6;
    double cr = 0.0, ci = 0.0, py = 0.0, px = 0.0;
    for (int rb = rb_offset; rb < rb_offset + 6; rb++) {
      for (int p = 0; p < 3; p++) {
        const int k = (first_carrier_offset + rb * 12 + 1 + 4 * p) % ofdm_symbol_size;
        const c16_t y = rxdataF[k];
        const c16_t x = pilot[rb * 3 + p];  // already conj(transmitted DM-RS)
        cr += (double)y.r * x.r - (double)y.i * x.i;
        ci += (double)y.r * x.i + (double)y.i * x.r;
        py += (double)y.r * y.r + (double)y.i * y.i;
        px += (double)x.r * x.r + (double)x.i * x.i;
      }
    }
    const double denom = sqrt(py * px);
    const double corr = (denom > 0.0) ? sqrt(cr * cr + ci * ci) / denom : 0.0;
    if (s_diag) {
      if (rb_offset == 0) diag_rb0_corr = corr;
      if (corr > diag_max_corr) { diag_max_corr = corr; diag_max_rb = rb_offset; }
    }
    if (corr >= CORESET_MAP_CORR_THRESHOLD) {
      if (found < max_candidates) {
        candidates_out[found].rb_offset = rb_offset;
        candidates_out[found].corr      = corr;
        found++;
      }
    }
  }
  if (s_diag && (s_calls % 200) == 1) {
    printf("COREMAPDIAG calls=%d rb0_corr=%.4f max_corr=%.4f max_rb=%d thresh=%.3f\n", s_calls,
          diag_rb0_corr, diag_max_corr, diag_max_rb, CORESET_MAP_CORR_THRESHOLD);
    fflush(stdout);
  }

  // Insertion sort by descending corr -- found is small (<= max_candidates), no need for qsort.
  for (int i = 1; i < found; i++) {
    nr_pdcch_coreset_candidate_t v = candidates_out[i];
    int j = i - 1;
    while (j >= 0 && candidates_out[j].corr < v.corr) {
      candidates_out[j + 1] = candidates_out[j];
      j--;
    }
    candidates_out[j + 1] = v;
  }
  return found;
}
