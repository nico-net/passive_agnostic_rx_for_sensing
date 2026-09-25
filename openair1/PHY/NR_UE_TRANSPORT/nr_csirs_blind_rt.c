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

/* Reference planes actually allocated. Only plane 0 is ever scored; rows 1-5 need at most 4 ports,
 * and for the 8-32-port rows every port >= 3 writes into plane 3 as a shared sink (never read), so a
 * 32-port candidate costs no extra 7 MB of per-thread buffers nor a 7 MB memset per slot. */
#define NR_CSIRS_BLIND_RT_MAX_PORTS 4
#define NR_CSIRS_BLIND_RT_ALL_PORTS 32

static nr_csirs_blind_state_t g_st;
static nr_csirs_blind_state_t g_zp;      /* zero-power search over the same candidates */
static double   g_zp_null[64];
static int      g_zp_null_n, g_zp_null_w;
static int      g_on = -1;      /* -1 = not read, 0 = off, 1 = on */
static int      g_rank = -1;    /* ISAC_CSIRS_BLIND_RANK: keep scoring after a confirmation */
static bool     g_confirm_logged;
static int      g_armed;
static uint64_t g_slots;
/* Recent scores from OTHER candidates, for the RELATIVE detection bar. An absolute correlation
 * threshold would have to be recalibrated per deployment for occupancy, gain and bandwidth --
 * exactly the kind of constant this project replaces with a measured one. */
#define NULLWIN 64
static double   g_null[NULLWIN];
static int      g_null_n, g_null_w;
/* Rows 6-18, footprint-first (nr_csirs_blind_search.h): per-slot on/off evidence from the symbol
 * this tap FFTs anyway, matched against OAI's mapping table at most once per FP_MATCH_EVERY calls. */
#define FP_MATCH_EVERY 1000
static nr_csirs_blind_fp_t g_fp;
static bool     g_fp_dirty;
static uint64_t g_fp_last;
/* Per-candidate sequence-free evidence: mean power ON the candidate's REs vs the rest of its RBs.
 * Accumulated for EVERY scoring, not just the ones that pass a correlation bar -- the first cut
 * logged EPR only for z>3 candidates, which is 25 biased samples and cannot rank a 198-candidate
 * space. If the scramblingID is not the PCI, this is the ONLY statistic that can still see the
 * resource, because it never touches the sequence. */
static double   g_epr_sum[NR_CSIRS_BLIND_MAX_CAND];
static uint32_t g_epr_n[NR_CSIRS_BLIND_MAX_CAND];
/* IS IT THE SSB? (2026-09-19) Both sequence inputs are excluded by complete sweeps, yet the energy
 * test keeps finding the same pair. SSB blocks always start at symbol 2 or 8 WITHIN a slot, so
 * l=4/l=8 cannot separate "SSB" from "CSI-RS" by symbol. The SLOT can: an SSB burst only occupies
 * slots 0-3 (Case C, L=8) of the SSB-period frames, while a real CSI-RS follows its own periodicity.
 * So bin every high-EPR sighting of the leading candidate by slot and print the histogram. */
#define EPR_SLOT_BINS 20
static uint32_t g_epr_slot_hi[EPR_SLOT_BINS];
static uint32_t g_epr_slot_all[EPR_SLOT_BINS];
/* scramblingID sweep (ISAC_CSIRS_BLIND_IDSWEEP=1). MEASURED 2026-09-19 on Swisscom PCI 382: the
 * sequence-free EPR finds a TRS pair (row 1, same fd, symbols 4 and 8) at 4-5x the power of its
 * neighbouring REs, while the correlation against the PCI-derived sequence stays at the noise level
 * -- i.e. the POSITIONS are right and the SEQUENCE is wrong. scramblingID is dedicated RRC and need
 * not be the PCI, so sweep it: 8 ids per qualifying slot (a slot where EPR says the pilot is there),
 * which keeps the added RT cost ~0.3 ms and covers all 1024 in 128 such slots. */
static int      g_ids = -1;
static int      g_slotsweep = -1;   /* ISAC_CSIRS_BLIND_SLOTSWEEP: sweep the slot index in c_init */
static double   g_slot_best_z;
static int      g_slot_best_off = -1;
static uint16_t g_id_next;
static int      g_id_pin = -1;   /* candidate the sweep is locked to; -1 = not chosen yet */
static double   g_id_best_z;
static uint16_t g_id_best;
static bool     g_id_solved;

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
  /* RANKING MODE (ISAC_CSIRS_BLIND_RANK=1, diagnostic only). MEASURED 2026-09-19 on a live
   * commercial cell: two identical 300 s runs both CONFIRMED, but on DIFFERENT resources
   * ("...:2:6:...:16:3" vs "...:4:5:...:16:8"; agreeing only on row 2 / density / period 16). The
   * cell may genuinely carry several CSI-RS resources -- the search stops at the FIRST confirmation,
   * so different runs would legitimately land on different real ones -- or some confirmations are
   * false at a 3x-null-median, 3-hit bar over 198 candidates x thousands of slots. The stored
   * ranking is what separates those, and stopping early throws it away.
   *
   * So in this mode: log EVERY confirmation, clear it, and keep scoring the whole population, then
   * print the top candidates by best correlation. A real resource sits far above the rest and
   * repeats across runs; a false positive shuffles. The rate-matcher gets no resource while this is
   * on (g_st.confirmed is cleared), which is why it is diagnostic only. */
  if (g_rank < 0) {
    const char *e = getenv("ISAC_CSIRS_BLIND_RANK");
    g_rank = (e != NULL && atoi(e) != 0) ? 1 : 0;
  }
  if (g_st.confirmed >= 0) {
    uint16_t period = 0, offset = 0;
    const nr_csirs_candidate_t *w = nr_csirs_blind_confirmed(&g_st, &period, &offset);
    char line[128];
    if (w != NULL && nr_csirs_blind_format(w, period, offset, line, sizeof(line)) > 0) {
      const int widx = g_st.confirmed;
      if (g_rank)
        LOG_A(PHY,
              "SENSING: CSIRS_BLIND CONFIRMED[rank] after %llu slots -- \"%s\" z=%.2f hits=%u "
              "tried=%u z_median=%.2f (search continues)\n",
              (unsigned long long)g_slots, line, g_st.best_rho[widx], g_st.hits[widx],
              g_st.tried[widx], null_median());
      else if (!g_confirm_logged)
        LOG_A(PHY, "SENSING: CSIRS_BLIND CONFIRMED after %llu slots -- csirs_monitor = \"%s\" (search stopped)\n",
              (unsigned long long)g_slots, line);
    }
    if (!g_rank) {
      g_confirm_logged = true;
      return;
    }
    g_st.confirmed = -1; /* keep the population moving; see RANKING MODE above */
  }
  if (g_rank && (g_slots % 4000) == 3999) {
    /* Top 5 by best SCALE-FREE score (best_rho now holds z = rho*sqrt(n_re); noise ~0.89). */
    int top[5] = {-1, -1, -1, -1, -1};
    for (int i = 0; i < g_st.n; i++)
      for (int k = 0; k < 5; k++)
        if (top[k] < 0 || g_st.best_rho[i] > g_st.best_rho[top[k]]) {
          for (int m = 4; m > k; m--)
            top[m] = top[m - 1];
          top[k] = i;
          break;
        }
    char buf[512];
    int off2 = 0;
    for (int k = 0; k < 5 && top[k] >= 0 && off2 < (int)sizeof(buf) - 64; k++) {
      const nr_csirs_candidate_t *c2 = &g_st.cand[top[k]];
      off2 += snprintf(buf + off2, sizeof(buf) - off2, "[row%u fd%u l%u rho=%.3f hits=%u/%u] ",
                       c2->row, c2->freq_domain, c2->symb_l0, g_st.best_rho[top[k]],
                       g_st.hits[top[k]], g_st.tried[top[k]]);
    }
    LOG_A(PHY, "SENSING: CSIRS_BLIND RANK slots=%llu z_median=%.2f (noise ~0.89) top: %s\n",
          (unsigned long long)g_slots, null_median(), buf);
    /* Same ranking by the SEQUENCE-FREE statistic. A real boosted pilot sits above 1 here even when
     * the sequence hypothesis is wrong; if the whole space reads ~1, no enumerated position carries
     * one and the resource is outside the search space rather than mis-sequenced. */
    int etop[5] = {-1, -1, -1, -1, -1};
    for (int i = 0; i < g_st.n && i < NR_CSIRS_BLIND_MAX_CAND; i++) {
      if (g_epr_n[i] < 4)
        continue;   /* too few samples to average */
      const double mi = g_epr_sum[i] / g_epr_n[i];
      for (int k = 0; k < 5; k++)
        if (etop[k] < 0 || mi > g_epr_sum[etop[k]] / g_epr_n[etop[k]]) {
          for (int m = 4; m > k; m--)
            etop[m] = etop[m - 1];
          etop[k] = i;
          break;
        }
    }
    char ebuf[512];
    int eo = 0;
    for (int k = 0; k < 5 && etop[k] >= 0 && eo < (int)sizeof(ebuf) - 64; k++) {
      const nr_csirs_candidate_t *c3 = &g_st.cand[etop[k]];
      eo += snprintf(ebuf + eo, sizeof(ebuf) - eo, "[row%u fd%u l%u epr=%.2f n=%u] ", c3->row,
                     c3->freq_domain, c3->symb_l0, g_epr_sum[etop[k]] / g_epr_n[etop[k]], g_epr_n[etop[k]]);
    }
    LOG_A(PHY, "SENSING: CSIRS_BLIND EPRRANK slots=%llu (1.0 = no pilot) top: %s\n",
          (unsigned long long)g_slots, ebuf);
    if (g_id_pin >= 0) {
      char hb[256];
      int ho = 0;
      for (int b = 0; b < EPR_SLOT_BINS && ho < (int)sizeof(hb) - 12; b++)
        ho += snprintf(hb + ho, sizeof(hb) - ho, "%u/%u ", g_epr_slot_hi[b], g_epr_slot_all[b]);
      LOG_A(PHY, "SENSING: CSIRS_BLIND EPRSLOT pinned row%u fd%u l%u, epr>2 by slot%%20 (SSB lives in "
                 "slots 0-3): %s\n",
            g_st.cand[g_id_pin].row, g_st.cand[g_id_pin].freq_domain, g_st.cand[g_id_pin].symb_l0, hb);
    }
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
   * From the enumerator's own table, so a row added there cannot get the wrong port count here: rows
   * 3 and 5 (2 and 4 ports) were generated with only port 0 cleared. */
  const int n_ports = nr_csirs_blind_row_ports(c->row);
  if (n_ports <= 0 || n_ports > NR_CSIRS_BLIND_RT_ALL_PORTS)
    return;
  const int n_planes = (n_ports < NR_CSIRS_BLIND_RT_MAX_PORTS) ? n_ports : NR_CSIRS_BLIND_RT_MAX_PORTS;

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
  for (int p = 0; p < n_planes; p++) {
    memset(t_refbuf[p], 0, (size_t)n_re * sizeof(c16_t));
  }
  c16_t *ref = t_refbuf[0];
  c16_t *refp[NR_CSIRS_BLIND_RT_ALL_PORTS];
  for (int p = 0; p < NR_CSIRS_BLIND_RT_ALL_PORTS; p++)
    refp[p] = t_refbuf[p < NR_CSIRS_BLIND_RT_MAX_PORTS ? p : NR_CSIRS_BLIND_RT_MAX_PORTS - 1];
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
  const uint32_t off_sym = (uint32_t)c->symb_l0 * (uint32_t)fp->ofdm_symbol_size;
  /* Footprint evidence for rows 6-18 from the symbol just transformed: one pass over the carrier. */
  uint16_t on_even = 0, on_odd = 0;
  nr_csirs_blind_symbol_on((const int16_t *)&rxdataF_ant0[off_sym], fp->ofdm_symbol_size,
                           fp->first_carrier_offset, fp->N_RB_DL, &on_even, &on_odd);
  if (nr_csirs_blind_fp_record(&g_fp, c->symb_l0, on_even, on_odd, absolute_slot))
    g_fp_dirty = true;
  /* Sub-band size for the channel-robust score, in OCCUPIED REs. 32 REs is ~11 RB for a density-3
   * row (4 MHz at 30 kHz) -- narrow enough that the channel is flat across it, wide enough that
   * noise stays well below a match: noise reads ~1.0, a perfect match sqrt(32)/0.886 = 6.4, and a
   * realistic 10 dB-SNR match with some residual drift lands around 4-5. */
#define CSIRS_BLIND_SUBBAND_RE 32
  int n_used = 0;
  const double rho = nr_csirs_blind_correlate_blocks_shift((const int16_t *)&rxdataF_ant0[off_sym],
                                                     (const int16_t *)&ref[off_sym],
                                                     fp->ofdm_symbol_size, CSIRS_BLIND_SUBBAND_RE,
                                                     fp->first_carrier_offset, &n_used);
  if (rho < 0.0) {
    return;   /* unscorable: this candidate maps no RE in this symbol */
  }
  /* SCALE-FREE SCORE. rho is not comparable across candidates -- noise gives ~0.89/sqrt(n_used), and
   * n_used is 273 for a density-one row-2 candidate but 819 for density-three at 273 RB, so the old
   * relative-to-the-null bar systematically picked the SMALLEST candidates. MEASURED: winners
   * 0.19-0.22 = the expected max of ~950 noise draws at n_used=273; every confirmation this module
   * produced OTA was that artefact. z = rho*sqrt(n_used) is ~0.89 for noise at any size and
   * sqrt(n_used) (>16 here) for a true match, so the bar below is absolute, not relative. */
  const double z = rho;   /* already normalised: ~1.0 noise, ~6.4 perfect, at any candidate size */
  /* Sequence-free positional evidence, logged next to z: if z stays at noise while this shows
   * periodic structure, the POSITIONS are right and the SEQUENCE (scramblingID != PCI) is wrong --
   * which the correlation alone cannot distinguish from an empty hypothesis. */
  const double epr = nr_csirs_blind_energy_ratio_shift((const int16_t *)&rxdataF_ant0[off_sym],
                                                 (const int16_t *)&ref[off_sym], fp->ofdm_symbol_size,
                                                 fp->first_carrier_offset);
  if (epr > 0.0 && idx < NR_CSIRS_BLIND_MAX_CAND) {
    g_epr_sum[idx] += epr;
    g_epr_n[idx]++;
  }
  if (epr > 0.0 && idx == g_id_pin && slot >= 0) {
    const int b = slot % EPR_SLOT_BINS;
    g_epr_slot_all[b]++;
    if (epr > 2.0)
      g_epr_slot_hi[b]++;
  }
  /* SPAN PROFILE (rank mode). MEASURED 2026-09-19 on Swisscom: the best candidate scores rho 0.22
   * against a 0.045 null -- 5x the null, but nowhere near the ~1 a correct known sequence must give,
   * while its HIT RATE (93/1798 = 5.2 %) matches a real period-16 resource. The candidates fix
   * start_rb=0 / nr_of_rbs=N_RB_DL, so a resource covering a NARROWER band dilutes the whole-symbol
   * correlation by ~sqrt(real_RBs / N_RB_DL) and can never score high. Correlating G slices of the
   * SAME buffers separates the two: a real resource is a contiguous block of high slices, a wrong
   * SEQUENCE (e.g. scramb_id != PCI) is flat. Slices are FFT-index ranges, so a carrier-contiguous
   * resource shows up as high slices at BOTH ends when it straddles DC. */
  static uint64_t s_span_last;
  if (g_rank && z > 3.0 && g_slots - s_span_last >= 200) {
    s_span_last = g_slots;
    enum { G = 12 };
    const uint32_t step = (uint32_t)fp->ofdm_symbol_size / G;
    char prof[256];
    int po = 0;
    for (int g = 0; g < G && po < (int)sizeof(prof) - 8; g++) {
      const double r = nr_csirs_blind_correlate((const int16_t *)&rxdataF_ant0[off_sym + (uint32_t)g * step],
                                                (const int16_t *)&ref[off_sym + (uint32_t)g * step], (int)step);
      po += snprintf(prof + po, sizeof(prof) - po, "%s%.2f", g ? " " : "", (r < 0.0) ? 0.0 : r);
    }
    LOG_A(PHY, "SENSING: CSIRS_BLIND SPAN row%u fd%u l%u z=%.2f rho=%.3f n_re=%d epr=%.2f slices[%d]: %s\n",
          c->row, c->freq_domain, c->symb_l0, z, rho, n_used, epr, G, prof);
  }
  /* ABSOLUTE bar: feed() tests score >= CSIRS_DETECT_MARGIN (3.0) * null, so a fixed null of 2.0
   * demands z >= 6, i.e. ~6.7x the 0.89 noise level, at ANY candidate size. The measured null median
   * is still tracked and logged, for visibility only. */
  const double nullv = 4.0 / 3.0;   /* feed()'s 3x margin => confirm at z >= 4 (noise ~1) */
  const bool done = nr_csirs_blind_feed(&g_st, idx, absolute_slot, z, nullv);
  /* Keep the observed z population for the diagnostics. */
  g_null[g_null_w] = z;
  g_null_w = (g_null_w + 1) % NULLWIN;
  if (g_null_n < NULLWIN) {
    g_null_n++;
  }
  /* ZERO-POWER hypothesis on the same candidate and symbol (no extra FEP or reference): does the
   * pattern carry no energy while the PDSCH around it does? Confirmed the same way (periodic). */
  if (g_zp.confirmed < 0) {
    const double zs = nr_csirs_blind_zero_score_shift((const int16_t *)&rxdataF_ant0[off_sym], (const int16_t *)&ref[off_sym],
                                                fp->ofdm_symbol_size, fp->first_carrier_offset);
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
  /* ---- scramblingID sweep on a candidate whose POSITIONS already look like a pilot ---- */
  if (g_ids < 0) {
    const char *e = getenv("ISAC_CSIRS_BLIND_IDSWEEP");
    g_ids = (e != NULL && atoi(e) != 0) ? 1 : 0;
  }
  /* PIN THE SWEEP TO ONE CANDIDATE. First cut advanced the id cursor on ANY slot whose candidate
   * had epr > 2, but the round-robin serves a different candidate every slot, so a "full pass"
   * spread its 1024 ids over many candidates and tested none of them -- the verdict it printed was
   * meaningless. The sweep now locks onto the candidate with the highest MEAN epr (>= 32 samples,
   * so the choice is not made on noise) and only advances while that same candidate is scored. */
  if (g_ids && !g_id_solved && g_id_pin < 0) {
    double best_m = 2.0;   /* nothing below 2x its neighbours is worth sweeping 1024 ids for */
    for (int i = 0; i < g_st.n && i < NR_CSIRS_BLIND_MAX_CAND; i++) {
      if (g_epr_n[i] < 32)
        continue;
      const double m = g_epr_sum[i] / g_epr_n[i];
      if (m > best_m) {
        best_m = m;
        g_id_pin = i;
      }
    }
    if (g_id_pin >= 0) {
      LOG_A(PHY, "SENSING: CSIRS_BLIND IDSWEEP pinned to row%u fd%u l%u (mean epr=%.2f over %u) -- "
                 "sweeping 1024 scramblingIDs on THAT candidate only\n",
            g_st.cand[g_id_pin].row, g_st.cand[g_id_pin].freq_domain, g_st.cand[g_id_pin].symb_l0,
            best_m, g_epr_n[g_id_pin]);
      /* The sweep advances only on visits that land on a CSI-RS slot; visit it every call. */
      nr_csirs_blind_pin(&g_st, g_id_pin, NR_CSIRS_BLIND_PIN_SWEEP_CALLS);
    }
  }
  /* SLOT-INDEX SWEEP (ISAC_CSIRS_BLIND_SLOTSWEEP=1). MEASURED 2026-09-19 on Swisscom PCI 382: a
   * COMPLETE 1024-value scramblingID sweep, pinned to one candidate and scored with the
   * channel-robust statistic, peaked at z=1.40 against a noise floor of 1.0 and a bar of 4 -- so the
   * scramblingID is NOT what makes the sequence wrong, while the sequence-free energy test keeps
   * finding the same TRS pair at ~3x its neighbours.
   *
   * The remaining input to the sequence is the SLOT: c_init = 2^10*(14*n_s + l + 1)*(2*N_ID+1) + N_ID
   * (TS 38.211 7.4.1.5.2). Crucially the RE MAPPING does not depend on n_s at all -- so if our slot
   * numbering is offset from the cell's (half-frame ambiguity, or an SFN/slot origin off by a fixed
   * amount), every sequence we generate is wrong while the positions stay exactly right. That is
   * precisely the pattern observed. Only ~20 offsets at mu=1, so the whole space fits in ONE visit. */
  if (g_slotsweep < 0) {
    const char *e = getenv("ISAC_CSIRS_BLIND_SLOTSWEEP");
    g_slotsweep = (e != NULL && atoi(e) != 0) ? 1 : 0;
  }
  if (g_slotsweep && idx == g_id_pin && epr > 1.5 && g_slot_best_off < 0) {
    const int n_slots = fp->slots_per_frame;
    for (int off = 0; off < n_slots; off++) {
      const int trial_slot = (slot + off) % n_slots;
      memset(&t_refbuf[0][off_sym], 0, (size_t)fp->ofdm_symbol_size * sizeof(c16_t));
      for (int pp = 1; pp < n_planes; pp++)
        memset(&t_refbuf[pp][off_sym], 0, (size_t)fp->ofdm_symbol_size * sizeof(c16_t));
      const csi_mapping_parms_t sp = get_csi_mapping_parms(c->row, c->freq_domain, c->symb_l0, c->symb_l1);
      nr_generate_csi_rs(fp, &sp, AMP, trial_slot, c->freq_density, c->start_rb, c->nr_of_rbs,
                         c->symb_l0, c->symb_l1, c->row, c->scramb_id, 0, c->cdm_type, refp);
      int sn = 0;
      const double sz = nr_csirs_blind_correlate_blocks_shift((const int16_t *)&rxdataF_ant0[off_sym],
                                                        (const int16_t *)&ref[off_sym],
                                                        fp->ofdm_symbol_size, CSIRS_BLIND_SUBBAND_RE,
                                                        fp->first_carrier_offset, &sn);
      if (sz > g_slot_best_z) {
        g_slot_best_z = sz;
        if (sz >= 4.0) {
          g_slot_best_off = off;
          LOG_A(PHY,
                "SENSING: CSIRS_BLIND SLOTSWEEP SOLVED row%u fd%u l%u slot_offset=%+d z=%.2f "
                "(our slot %d -> cell slot %d, epr=%.2f) -- the receiver's slot numbering is the "
                "sequence error, not the scramblingID\n",
                c->row, c->freq_domain, c->symb_l0, off, sz, slot, trial_slot, epr);
        }
      }
    }
    if (g_slot_best_off < 0)
      LOG_A(PHY, "SENSING: CSIRS_BLIND SLOTSWEEP all %d offsets, best z=%.2f (bar 4, noise ~1) -- the "
                 "slot index is NOT the sequence error either\n", fp->slots_per_frame, g_slot_best_z);
  }
  if (g_ids && !g_id_solved && idx == g_id_pin && epr > 1.5) {
    /* 32 ids per visit, not 8: the pinned candidate comes round only once per pass over the
     * candidate list (~0.3 s), so at 8 the 1024 ids did not finish inside a 300 s capture and the
     * run ended with no verdict at all. 32 ids is ~1.3 ms of extra work on a visit that already
     * costs an FEP plus a reference generation. */
    nr_csirs_candidate_t trial = *c;
    for (int k = 0; k < 32; k++) {
      trial.scramb_id = g_id_next;
      g_id_next = (uint16_t)((g_id_next + 1) & 1023);
      memset(&t_refbuf[0][off_sym], 0, (size_t)fp->ofdm_symbol_size * sizeof(c16_t));
      for (int pp = 1; pp < n_planes; pp++)
        memset(&t_refbuf[pp][off_sym], 0, (size_t)fp->ofdm_symbol_size * sizeof(c16_t));
      const csi_mapping_parms_t tp = get_csi_mapping_parms(trial.row, trial.freq_domain, trial.symb_l0,
                                                           trial.symb_l1);
      nr_generate_csi_rs(fp, &tp, AMP, slot, trial.freq_density, trial.start_rb, trial.nr_of_rbs,
                         trial.symb_l0, trial.symb_l1, trial.row, trial.scramb_id, 0, trial.cdm_type,
                         refp);
      int tn = 0;
      const double tz = nr_csirs_blind_correlate_blocks_shift((const int16_t *)&rxdataF_ant0[off_sym],
                                                        (const int16_t *)&ref[off_sym], fp->ofdm_symbol_size,
                                                        CSIRS_BLIND_SUBBAND_RE,
                                                        fp->first_carrier_offset, &tn);
      if (tz > g_id_best_z) {
        g_id_best_z = tz;
        g_id_best = trial.scramb_id;
        if (tz >= 4.0) {   /* noise ~1.0; 1024 ids give a noise max near 2.5-3 */
          g_id_solved = true;
          /* CLOSE THE GAP: until this line the solved id was print-only -- the pinned candidate's
           * OWN scramb_id (assumed = PCI at init) stayed wrong, so the ordinary per-slot
           * nr_csirs_blind_feed() below kept scoring the wrong sequence forever and g_st.confirmed
           * never left -1 even after a correct id was found. The candidate's tried/hits/best_rho
           * history under the WRONG id is harmless to leave in place: hits[] only increments past
           * feed()'s CSIRS_DETECT_MARGIN bar, which the wrong sequence could never clear, so it is
           * already at 0. Patching the id in place lets the existing, already-tested confirm path
           * (feed() + nr_csirs_blind_infer_period()) take over on the next visit rather than
           * duplicating that logic here. */
          g_st.cand[g_id_pin].scramb_id = trial.scramb_id;
          nr_csirs_blind_pin(&g_st, g_id_pin, NR_CSIRS_BLIND_PIN_CONFIRM_CALLS);
          LOG_A(PHY,
                "SENSING: CSIRS_BLIND IDSWEEP SOLVED row%u fd%u l%u scramb_id=%u z=%.1f (PCI=%d, "
                "epr=%.2f) -- csirs_monitor scramblingID is NOT the PCI; candidate corrected, "
                "confirmation resumes on next visit\n",
                trial.row, trial.freq_domain, trial.symb_l0, trial.scramb_id, tz, fp->Nid_cell, epr);
          break;
        }
      }
    }
    if (!g_id_solved && (g_id_next % 256) == 0)
      LOG_A(PHY, "SENSING: CSIRS_BLIND IDSWEEP progress %u/1024 ids, best so far id=%u z=%.2f (bar 4, noise ~1)\n",
            (unsigned)g_id_next, g_id_best, g_id_best_z);
    if (!g_id_solved && g_id_next == 0)
      LOG_A(PHY, "SENSING: CSIRS_BLIND IDSWEEP full pass, no id reached z=4: best id=%u z=%.2f "
                 "(row%u fd%u l%u epr=%.2f) -- sequence error is NOT the scramblingID\n",
            g_id_best, g_id_best_z, c->row, c->freq_domain, c->symb_l0, epr);
  }
  /* Rows 6-18: match the measured footprints against OAI's table (~60 us, hence rate-limited) and
   * hand each new fit to the ordinary confirm path -- round-robin, feed(), IDSWEEP -- by appending it
   * to the population. Pinned for a confirm budget when nothing else holds the pin, since round-robin
   * alone would reach it only once per ~700 calls. Nothing is appended on a cell without periodic
   * pair-structured energy, which leaves the rows 1-5 search exactly as it was. */
  if (g_fp_dirty && g_slots - g_fp_last >= FP_MATCH_EVERY) {
    g_fp_dirty = false;
    g_fp_last = g_slots;
    nr_csirs_candidate_t fit[16];
    const int n_fit = nr_csirs_blind_fp_match(&g_fp, fp->N_RB_DL, fp->Nid_cell, fit, 16);
    for (int i = 0; i < n_fit; i++) {
      const int at = nr_csirs_blind_append(&g_st, &fit[i]);
      if (at < 0)
        continue;
      nr_csirs_blind_append(&g_zp, &fit[i]); /* g_zp mirrors g_st index for index */
      if (g_st.pin_left == 0)
        nr_csirs_blind_pin(&g_st, at, NR_CSIRS_BLIND_PIN_CONFIRM_CALLS);
      LOG_A(PHY, "SENSING: CSIRS_BLIND FOOTPRINT row%u fd0x%x l0=%u l1=%u density=%u cdm=%u (%d ports) "
                 "fits the measured periodic RE pattern -- candidate %d added to the confirm path\n",
            fit[i].row, fit[i].freq_domain, fit[i].symb_l0, fit[i].symb_l1, fit[i].freq_density,
            fit[i].cdm_type, nr_csirs_blind_row_ports(fit[i].row), at);
    }
  }
  /* the sweep above reused the shared reference buffer; the candidate's own reference is stale now */
  (void)done; // logged once, by the early return above on the next call
  if ((++g_slots % 20000) == 0) {
    /* was: passed nullv (the fixed 4/3 confirm-bar constant) here instead of the real population
     * statistic -- every line of a whole OTA run read a frozen 1.333 regardless of the actual null
     * behaviour. null_median() is the function this field is named after; use it. */
    LOG_I(PHY, "SENSING: CSIRS_BLIND slots=%llu candidates=%d null_median=%.3f confirmed=%d\n",
          (unsigned long long)g_slots, g_st.n, null_median(), g_st.confirmed);
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
