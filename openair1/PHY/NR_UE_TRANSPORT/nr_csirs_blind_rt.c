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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c */

#include "nr_csirs_blind_rt.h"
#include "nr_csirs_blind_search.h"

#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "common/utils/LOG/log.h"

#include <stdlib.h>
#include <string.h>

static nr_csirs_blind_state_t g_st;
static int      g_on = -1;      /* -1 = not read, 0 = off, 1 = on */
static int      g_armed;
static uint64_t g_slots;
/* Recent scores from OTHER candidates, for the RELATIVE detection bar. An absolute correlation
 * threshold would have to be recalibrated per deployment for occupancy, gain and bandwidth --
 * exactly the kind of constant this project replaces with a measured one. */
#define NULLWIN 64
static double   g_null[NULLWIN];
static int      g_null_n, g_null_w;

static double null_median(void)
{
  if (g_null_n < 8) {
    return -1.0;   /* not enough population yet to say what "standing out" means */
  }
  double t[NULLWIN];
  memcpy(t, g_null, sizeof(t[0]) * (size_t)g_null_n);
  for (int i = 1; i < g_null_n; i++) {
    const double v = t[i];
    int j = i - 1;
    while (j >= 0 && t[j] > v) { t[j + 1] = t[j]; j--; }
    t[j + 1] = v;
  }
  return t[g_null_n / 2];
}

void nr_csirs_blind_rt_slot(const PHY_VARS_NR_UE *ue, int slot, uint32_t absolute_slot,
                            const c16_t *rxdataF_ant0)
{
  if (g_on < 0) {
    const char *e = getenv("ISAC_CSIRS_BLIND");
    g_on = (e != NULL && atoi(e) != 0) ? 1 : 0;
  }
  if (!g_on || ue == NULL || rxdataF_ant0 == NULL) {
    return;
  }
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  if (g_armed == 0) {
    /* scramblingID is taken as the PCI, which acquisition already gives us. Sweeping 1024 values
     * would cost three orders of magnitude for a parameter that can simply be tried first. */
    if (nr_csirs_blind_init(&g_st, fp->N_RB_DL, fp->Nid_cell) <= 0) {
      g_on = 0;
      return;
    }
    g_armed = 1;
    LOG_I(PHY, "SENSING: CSIRS_BLIND armed: %d candidates, N_RB=%d scramb_id=%d (assumed = PCI)\n",
          g_st.n, fp->N_RB_DL, fp->Nid_cell);
  }
  const int idx = nr_csirs_blind_next(&g_st);
  if (idx < 0) {
    return;
  }
  const nr_csirs_candidate_t *c = &g_st.cand[idx];

  /* Build the reference with the REAL generator. The RE-to-sequence mapping is row-dependent
   * (k-prime/l-prime, CDM groups, density parity) and a hand-rolled version that is subtly wrong
   * does not look like a weak signal -- it looks like a dead channel, which is the misdiagnosis
   * this whole module exists to avoid. */
  const uint32_t n_re = (uint32_t)fp->ofdm_symbol_size * NR_SYMBOLS_PER_SLOT;
  c16_t *ref = (c16_t *)calloc(n_re, sizeof(c16_t));
  if (ref == NULL) {
    return;
  }
  c16_t *refp[1] = {ref};
  const csi_mapping_parms_t parms = get_csi_mapping_parms(c->row, c->freq_domain, c->symb_l0,
                                                          c->symb_l1);
  nr_generate_csi_rs(fp, &parms, AMP, slot, c->freq_density, c->start_rb, c->nr_of_rbs,
                     c->symb_l0, c->symb_l1, c->row, c->scramb_id, 0 /* power offset */,
                     c->cdm_type, refp);

  /* Score only the symbol the candidate places its resource on: correlating the whole slot would
   * dilute the oracle with 13 symbols of unrelated PDSCH. */
  const uint32_t off = (uint32_t)c->symb_l0 * (uint32_t)fp->ofdm_symbol_size;
  const double rho = nr_csirs_blind_correlate((const int16_t *)&rxdataF_ant0[off],
                                              (const int16_t *)&ref[off],
                                              fp->ofdm_symbol_size);
  free(ref);
  if (rho < 0.0) {
    return;   /* unscorable: this candidate maps no RE in this symbol */
  }
  const double nullv = null_median();
  const bool done = nr_csirs_blind_feed(&g_st, idx, absolute_slot, rho, nullv);
  /* Feed the null AFTER scoring, so a candidate is never compared against itself. */
  g_null[g_null_w] = rho;
  g_null_w = (g_null_w + 1) % NULLWIN;
  if (g_null_n < NULLWIN) {
    g_null_n++;
  }
  if (done) {
    uint16_t period = 0, offset = 0;
    const nr_csirs_candidate_t *w = nr_csirs_blind_confirmed(&g_st, &period, &offset);
    char line[128];
    if (w != NULL && nr_csirs_blind_format(w, period, offset, line, sizeof(line)) > 0) {
      LOG_A(PHY, "SENSING: CSIRS_BLIND CONFIRMED after %llu slots -- csirs_monitor = \"%s\"\n",
            (unsigned long long)g_slots, line);
    }
  }
  if ((++g_slots % 20000) == 0) {
    LOG_I(PHY, "SENSING: CSIRS_BLIND slots=%llu candidates=%d null_median=%.3f confirmed=%d\n",
          (unsigned long long)g_slots, g_st.n, nullv, g_st.confirmed);
  }
}
