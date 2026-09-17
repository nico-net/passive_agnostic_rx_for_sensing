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
#include "PHY/MODULATION/modulation_UE.h"
#include "nr_csirs_blind_search.h"

#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "common/utils/LOG/log.h"

#include <stdlib.h>
#include <string.h>

/* Max CSI-RS ports over the rows kRows enumerates (row 4). */
#define NR_CSIRS_BLIND_RT_MAX_PORTS 4

static nr_csirs_blind_state_t g_st;
static nr_csirs_blind_state_t g_zp;      /* zero-power search over the same candidates */
static double   g_zp_null[64];
static int      g_zp_null_n, g_zp_null_w;
static int      g_on = -1;      /* -1 = not read, 0 = off, 1 = on */
static int      g_armed;
static uint64_t g_slots;
/* Recent scores from OTHER candidates, for the RELATIVE detection bar. An absolute correlation
 * threshold would have to be recalibrated per deployment for occupancy, gain and bandwidth --
 * exactly the kind of constant this project replaces with a measured one. */
#define NULLWIN 64
static double   g_null[NULLWIN];
static int      g_null_n, g_null_w;

static double median_of(const double *src, int n)
{
  if (n < 8) {
    return -1.0;   /* not enough population yet to say what "standing out" means */
  }
  double t[NULLWIN];
  memcpy(t, src, sizeof(t[0]) * (size_t)n);
  for (int i = 1; i < n; i++) {
    const double v = t[i];
    int j = i - 1;
    while (j >= 0 && t[j] > v) { t[j + 1] = t[j]; j--; }
    t[j + 1] = v;
  }
  return t[n / 2];
}
static double null_median(void) { return median_of(g_null, g_null_n); }

void nr_csirs_blind_rt_slot(PHY_VARS_NR_UE *ue, int slot, uint32_t absolute_slot,
                            c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP])
{
  const c16_t *rxdataF_ant0 = (rxdataF != NULL) ? &rxdataF[0][0] : NULL;
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
    nr_csirs_blind_init(&g_zp, fp->N_RB_DL, fp->Nid_cell);
    g_armed = 1;
    LOG_I(PHY, "SENSING: CSIRS_BLIND armed: %d candidates, N_RB=%d scramb_id=%d (assumed = PCI)\n",
          g_st.n, fp->N_RB_DL, fp->Nid_cell);
  }
  /* NOTHING TO SEARCH ONCE CONFIRMED. nr_csirs_blind_next() returns the confirmed index forever
   * after confirmation, so every later call regenerated the SAME resource's full-slot reference
   * (memset of up to 4 x 57 KB, nr_generate_csi_rs, an FEP, a correlation) and re-scored it -- and
   * the ZP search below, which shares idx, could only ever re-test that one candidate. Measured
   * 2026-09-17 on the PDCCH scan consumer: ~1.85 ms of untimed work per occasion, 68 % of PDCCH
   * occasions dropped, and 281k "CONFIRMED" lines in one 600 s run. The rate-matcher and the sensing
   * capture read the stored state (rate_match_from), which this leaves untouched. */
  if (g_st.confirmed >= 0) {
    static bool s_logged;
    if (!s_logged) {
      s_logged = true;
      uint16_t period = 0, offset = 0;
      const nr_csirs_candidate_t *w = nr_csirs_blind_confirmed(&g_st, &period, &offset);
      char line[128];
      if (w != NULL && nr_csirs_blind_format(w, period, offset, line, sizeof(line)) > 0)
        LOG_A(PHY, "SENSING: CSIRS_BLIND CONFIRMED after %llu slots -- csirs_monitor = \"%s\" (search stopped)\n",
              (unsigned long long)g_slots, line);
    }
    return;
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
  /* ONE BUFFER PER PORT, not one buffer. nr_generate_csi_rs() writes dataF[p] for every port the
   * ROW defines, so handing it a single-element array made row 4 (4 ports) write through
   * dataF[1..3] and segfault the receiver -- caught on air, first run after wiring.
   * The count is taken from the row rather than assumed: rows 1 and 2 are single-port, row 4 is
   * four-port, and a future row added to kRows must extend this table with it. */
  int n_ports = 1;
  switch (c->row) {
    case 4: n_ports = 4; break;
    case 1:
    case 2:
    default: n_ports = 1; break;
  }

  /* ---- ALLOCATED ONCE, NOT PER SLOT -----------------------------------------------------------
   * This runs on the PHY receive thread. The first version called calloc()/free() every slot for
   * up to 4 x 229 KB -- roughly 900 KB of allocation churn per slot, gigabytes per second at this
   * slot rate, on the one thread that must keep draining the USRP. That is precisely the shape of
   * fault that stops the host consuming the stream in time, and on this X410 a single RX overflow
   * halts the stream permanently rather than recovering.
   * Thread-local and grow-only: each consumer keeps its own buffers, and the zeroing that calloc
   * used to provide is done explicitly below -- nr_generate_csi_rs() writes only the REs its row
   * occupies, so a stale buffer would leave the PREVIOUS candidate's symbols in place and the
   * correlator would score a mixture of two hypotheses. */
  static __thread c16_t *t_refbuf[NR_CSIRS_BLIND_RT_MAX_PORTS];
  static __thread uint32_t t_refbuf_re;
  if (t_refbuf_re < n_re) {
    for (int p = 0; p < NR_CSIRS_BLIND_RT_MAX_PORTS; p++) {
      c16_t *nb = (c16_t *)realloc(t_refbuf[p], (size_t)n_re * sizeof(c16_t));
      if (nb == NULL) {
        return;   /* keep whatever we had; a short buffer is never used because t_refbuf_re stands */
      }
      t_refbuf[p] = nb;
    }
    t_refbuf_re = n_re;
  }
  for (int p = 0; p < n_ports; p++) {
    memset(t_refbuf[p], 0, (size_t)n_re * sizeof(c16_t));
  }
  c16_t *ref = t_refbuf[0];
  c16_t **refp = t_refbuf;
  const csi_mapping_parms_t parms = get_csi_mapping_parms(c->row, c->freq_domain, c->symb_l0,
                                                          c->symb_l1);
  nr_generate_csi_rs(fp, &parms, AMP, slot, c->freq_density, c->start_rb, c->nr_of_rbs,
                     c->symb_l0, c->symb_l1, c->row, c->scramb_id, 0 /* power offset */,
                     c->cdm_type, refp);

  /* Score only the symbol the candidate places its resource on: correlating the whole slot would
   * dilute the oracle with 13 symbols of unrelated PDSCH. The monitor only FFT'd the CORESET
   * symbols, so transform this candidate's symbol now (antenna 0) -- without this the search
   * scored an empty buffer and never fired (OTA 2026-09-14: 15 min, zero progress lines). */
  nr_slot_fep_ant(ue, fp, (unsigned)slot, (unsigned)c->symb_l0, 0, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
  const uint32_t off = (uint32_t)c->symb_l0 * (uint32_t)fp->ofdm_symbol_size;
  const double rho = nr_csirs_blind_correlate((const int16_t *)&rxdataF_ant0[off],
                                              (const int16_t *)&ref[off],
                                              fp->ofdm_symbol_size);
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
  /* ZERO-POWER hypothesis on the same candidate and symbol (no extra FEP or reference): does the
   * pattern carry no energy while the PDSCH around it does? Confirmed the same way (periodic). */
  if (g_zp.confirmed < 0) {
    const double zs = nr_csirs_blind_zero_score((const int16_t *)&rxdataF_ant0[off], (const int16_t *)&ref[off],
                                                fp->ofdm_symbol_size);
    if (zs >= 0.0) {
      const double znull = median_of(g_zp_null, g_zp_null_n);
      if (nr_csirs_blind_zp_feed(&g_zp, idx, absolute_slot, zs, znull)) {
        uint16_t period = 0, offset = 0;
        const nr_csirs_candidate_t *w = nr_csirs_blind_confirmed(&g_zp, &period, &offset);
        char line[128];
        if (w != NULL && nr_csirs_blind_format(w, period, offset, line, sizeof(line)) > 0)
          LOG_A(PHY, "SENSING: CSIRS_BLIND ZP CONFIRMED after %llu slots (rate-matching only) -- \"%s\"\n",
                (unsigned long long)g_slots, line);
      }
      g_zp_null[g_zp_null_w] = zs;
      g_zp_null_w = (g_zp_null_w + 1) % NULLWIN;
      if (g_zp_null_n < NULLWIN) g_zp_null_n++;
    }
  }
  (void)done; // logged once, by the early return above on the next call
  if ((++g_slots % 20000) == 0) {
    LOG_I(PHY, "SENSING: CSIRS_BLIND slots=%llu candidates=%d null_median=%.3f confirmed=%d\n",
          (unsigned long long)g_slots, g_st.n, nullv, g_st.confirmed);
  }
}

/* RATE MATCHING FROM THE BLIND SEARCH (2026-09-15). The passive PDSCH decoder used to refuse any
 * grant flagged with CSI-RS rate matching because nothing told it WHICH REs to skip; the blind
 * search now does, so its confirmed resource is offered as the FAPI PDU the demodulator already
 * understands. ZP CSI-RS (a pure rate-matching pattern) is searched by energy (nr_csirs_blind_zero_score)
 * on the same candidates and offered separately as csi_type 2. */
static bool rate_match_from(const nr_csirs_blind_state_t *st, uint8_t csi_type, uint32_t absolute_slot,
                            fapi_nr_dl_config_csirs_pdu_rel15_t *out)
{
  if (out == NULL || g_on <= 0 || g_armed == 0 || st->confirmed < 0 || st->period == 0)
    return false;
  if ((absolute_slot % st->period) != (st->offset % st->period))
    return false;
  const nr_csirs_candidate_t *c = &st->cand[st->confirmed];
  memset(out, 0, sizeof(*out));
  out->start_rb = c->start_rb;
  out->nr_of_rbs = c->nr_of_rbs;
  out->csi_type = csi_type;
  out->row = c->row;
  out->freq_domain = c->freq_domain;
  out->symb_l0 = c->symb_l0;
  out->symb_l1 = c->symb_l1;
  out->cdm_type = c->cdm_type;
  out->freq_density = c->freq_density;
  out->scramb_id = c->scramb_id;
  return true;
}
bool nr_csirs_blind_rt_rate_match(uint32_t absolute_slot, fapi_nr_dl_config_csirs_pdu_rel15_t *out)
{
  return rate_match_from(&g_st, 1 /* NZP */, absolute_slot, out);
}
bool nr_csirs_blind_rt_rate_match_zp(uint32_t absolute_slot, fapi_nr_dl_config_csirs_pdu_rel15_t *out)
{
  return rate_match_from(&g_zp, 2 /* ZP: rate matching only, no estimation */, absolute_slot, out);
}
