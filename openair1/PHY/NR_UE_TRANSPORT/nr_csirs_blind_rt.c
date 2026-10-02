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
#include "nr_csirs_observer.h"

#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "common/utils/LOG/log.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <time.h>

/* Weak for the standalone blind-search fixture; the receiver links the epoch owner. */
__attribute__((weak)) void nr_cfg_epoch_note_csirs_map_change(void);
static nr_csirs_observer_t g_csirs_obs;
static int g_timing = -1;
static int g_reconf = -1;
static uint64_t now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
void nr_csirs_blind_rt_metrics(nr_passive_metrics_t *m) { nr_csirs_observer_metrics(&g_csirs_obs, m); }
void nr_csirs_blind_rt_cfr_time_us(uint64_t us) { nr_csirs_observer_add_time(&g_csirs_obs, NR_CSIRS_TIME_CFR, us); }
static nr_csirs_resource_t resource_key(const nr_csirs_blind_state_t *s, int k, bool zp)
{
  const nr_csirs_candidate_t *c = &s->cand[s->conf_idx[k]];
  return (nr_csirs_resource_t){.row = c->row, .ports = nr_csirs_blind_row_ports(c->row),
      .density = c->freq_density, .period = s->conf_period[k], .offset = s->conf_off[k][0],
      .offset2 = s->conf_off[k][1], .n_offsets = s->conf_n_off[k], .zp = zp,
      .freq_domain = c->freq_domain, .start_rb = c->start_rb, .nr_of_rbs = c->nr_of_rbs};
}
static void map_changed(void)
{
  if (nr_cfg_epoch_note_csirs_map_change) nr_cfg_epoch_note_csirs_map_change();
}

/* Reference planes actually allocated. NZP scores plane 0; ZP scores their occupancy union.
 * Rows 1-5 need at most 4 ports,
 * and for the 8-32-port rows every port >= 3 writes into plane 3 as a shared union sink, so a
 * 32-port candidate costs no extra 7 MB of per-thread buffers nor a 7 MB memset per slot. */
#define NR_CSIRS_BLIND_RT_MAX_PORTS 4
#define NR_CSIRS_BLIND_RT_ALL_PORTS 32

static nr_csirs_blind_state_t g_st;
static nr_csirs_blind_state_t g_zp;      /* zero-power search over the same candidates */
static double   g_zp_null[64];
static int      g_zp_null_n, g_zp_null_w;
static int      g_on = -1;      /* -1 = not read, 0 = off, 1 = on */
static int      g_rank = -1;    /* ISAC_CSIRS_BLIND_RANK: keep scoring after a confirmation */
static int      g_conf_logged;   /* g_st confirmations already logged */
/* Candidate-indexed counters survive export-bank compaction. Diagnostic details are capped;
 * every confirmation still carries its complete bounded epoch history in the normal log. */
static uint32_t g_zp_logged[NR_CSIRS_BLIND_MAX_CAND];
static uint8_t g_zp_events[NR_CSIRS_BLIND_MAX_CAND];
#define ZP_EVENT_LIMIT 32
/* Diagnostic lifetime state is indexed by immutable candidate index, never export-bank position.
 * Neither evidence epochs nor export compaction/reset may clear this accounting. */
enum zp_maint_outcome {
  ZPM_DUPLICATE, ZPM_SETUP_INVALID, ZPM_RHO_UNSCORABLE, ZPM_SCORE_INVALID,
  ZPM_POPULATION_UNKNOWN, ZPM_POPULATION_SUPPRESSED, ZPM_QUALIFIED_HOLE, ZPM_OCCUPIED, ZPM_OUTCOMES
};
static const char *const zp_maint_names[ZPM_OUTCOMES] = {
  "duplicate", "setup_invalid", "rho_unscorable", "score_invalid",
  "population_unknown", "population_suppressed", "qualified_hole", "occupied"
};
static struct {
  uint64_t scheduled, reached, outcome[ZPM_OUTCOMES];
  uint32_t first_slot, last_slot;
} g_zp_maint[NR_CSIRS_BLIND_MAX_CAND];

static void zp_maint_summary(int idx)
{
  const nr_csirs_candidate_t *c = &g_zp.cand[idx];
  char first[24] = "NA", last[24] = "NA";
  uint64_t total = 0;
  for (int k = 0; k < ZPM_OUTCOMES; k++)
    total += g_zp_maint[idx].outcome[k];
  assert(total == g_zp_maint[idx].reached);
  if (g_zp_maint[idx].reached) {
    snprintf(first, sizeof(first), "%u", g_zp_maint[idx].first_slot);
    snprintf(last, sizeof(last), "%u", g_zp_maint[idx].last_slot);
  }
  LOG_I(PHY, "SENSING: CSIRS_BLIND ZP_MAINT_SUMMARY idx=%d row=%u fd=%u l0=%u l1=%u density=%u "
             "start_rb=%u nrb=%u scramb_id=%u cdm=%u epoch=%u active=%d scheduled=%llu reached=%llu duplicate=%llu "
             "setup_invalid=%llu rho_unscorable=%llu score_invalid=%llu population_unknown=%llu "
             "population_suppressed=%llu qualified_hole=%llu occupied=%llu first_slot=%s last_slot=%s\n",
        idx, c->row, c->freq_domain, c->symb_l0, c->symb_l1, c->freq_density, c->start_rb, c->nr_of_rbs,
        c->scramb_id, c->cdm_type, g_zp.zp_epoch[idx], nr_csirs_blind_is_confirmed(&g_zp, idx),
        (unsigned long long)g_zp_maint[idx].scheduled, (unsigned long long)g_zp_maint[idx].reached,
        (unsigned long long)g_zp_maint[idx].outcome[ZPM_DUPLICATE],
        (unsigned long long)g_zp_maint[idx].outcome[ZPM_SETUP_INVALID],
        (unsigned long long)g_zp_maint[idx].outcome[ZPM_RHO_UNSCORABLE],
        (unsigned long long)g_zp_maint[idx].outcome[ZPM_SCORE_INVALID],
        (unsigned long long)g_zp_maint[idx].outcome[ZPM_POPULATION_UNKNOWN],
        (unsigned long long)g_zp_maint[idx].outcome[ZPM_POPULATION_SUPPRESSED],
        (unsigned long long)g_zp_maint[idx].outcome[ZPM_QUALIFIED_HOLE],
        (unsigned long long)g_zp_maint[idx].outcome[ZPM_OCCUPIED], first, last);
}

static void zp_maint_end(int idx, uint32_t slot, bool duplicate, enum zp_maint_outcome outcome,
                         double rho, double score, double other_score, double population)
{
  /* Runtime setup/rho failures and the feed's score-validity guard precede duplicate handling.
   * A repeated slot is a duplicate only after it supplies a valid pair of ZP scores. */
  if (duplicate && outcome >= ZPM_POPULATION_UNKNOWN)
    outcome = ZPM_DUPLICATE;
  const uint64_t count = ++g_zp_maint[idx].outcome[outcome];
  uint64_t total = 0;
  for (int k = 0; k < ZPM_OUTCOMES; k++)
    total += g_zp_maint[idx].outcome[k];
  assert(total == g_zp_maint[idx].reached);
  if (count != 1)
    return; // one detail per outcome per candidate lifetime; summaries remain uncapped
  char values[4][32];
  const double numbers[] = {rho, score, other_score, population};
  for (int k = 0; k < 4; k++) {
    if (isfinite(numbers[k]))
      snprintf(values[k], sizeof(values[k]), "%.6f", numbers[k]);
    else
      snprintf(values[k], sizeof(values[k]), "NA");
  }
  LOG_I(PHY, "SENSING: CSIRS_BLIND ZP_MAINT_DETAIL idx=%d epoch=%u active=%d abs_slot=%u outcome=%s "
             "rho=%s score=%s other_score=%s population=%s\n",
        idx, g_zp.zp_epoch[idx], nr_csirs_blind_is_confirmed(&g_zp, idx), slot, zp_maint_names[outcome],
        values[0], values[1], values[2], values[3]);
}
/* Diagnostic only: exact row-2 old-score/new-score disagreement, counted once per candidate
 * and absolute slot. One detailed line per candidate; aggregate uses the existing status cadence. */
static struct { uint64_t count; uint32_t last_slot; } g_zp_geometry_veto[NR_CSIRS_BLIND_MAX_CAND];
static uint64_t g_zp_geometry_veto_total;
static int      g_armed;
static uint64_t g_slots;
/* Recent scores from OTHER candidates, for the RELATIVE detection bar. An absolute correlation
 * threshold would have to be recalibrated per deployment for occupancy, gain and bandwidth --
 * exactly the kind of constant this project replaces with a measured one. */
#define NULLWIN 64
static double   g_null[NULLWIN];
static int      g_null_n, g_null_w;
/* Rows 6-18, footprint-first (nr_csirs_blind_search.h): per-slot on/off evidence from the symbol
 * this tap FFTs anyway, matched against OAI's mapping table at most once per FP_MATCH_EVERY calls.
 * OPT-IN (ISAC_CSIRS_BLIND_WIDE=1, default off): appending and pinning fits changes the order in which
 * the population is searched, and a union of two narrow resources sharing a period/offset (e.g. two
 * row-4 resources at k0=0 and k0=4) fits a row-6 footprint. Off, none of this code runs and the rows
 * 1-5 search is exactly what it was. */
#define FP_MATCH_EVERY 1000
static int      g_wide = -1;   /* -1 = not read, 0 = off, 1 = on */
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

/* DECODED-GRANT ZP EVIDENCE (G5 review of 060edd290c, Important 1). The passive PDSCH decode consumers score
 * every ZP entry they were handed against the grant's own PRBs (nr_pdsch_passive_decode.c) and post it here.
 * They run on other threads and behind this search, so the evidence is queued and applied by the next
 * nr_csirs_blind_rt_slot(), the only writer of g_zp. ponytail: bounded queue, overflow counted and dropped --
 * one decoded grant per ZP occasion is enough and the drain runs every slot. */
#define ZP_GRANT_QUEUE 64
static pthread_mutex_t g_zp_grant_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
  uint32_t slot;
  fapi_nr_dl_config_csirs_pdu_rel15_t zp;
  double score;
} zp_grant_ev_t;
static zp_grant_ev_t g_zp_grant_q[ZP_GRANT_QUEUE];
static int g_zp_grant_qn;
static uint64_t g_zp_grant_dropped; /* written under g_zp_grant_lock */
static uint64_t g_zp_grant_dropped_seen, g_zp_grant_applied, g_zp_grant_contradictions; /* scan thread only */

void nr_csirs_blind_rt_zp_grant_evidence(uint32_t pdsch_absolute_slot, const fapi_nr_dl_config_csirs_pdu_rel15_t *zp,
                                         double score)
{
  if (zp == NULL || zp->csi_type != 2)
    return;
  pthread_mutex_lock(&g_zp_grant_lock);
  if (g_zp_grant_qn < ZP_GRANT_QUEUE) {
    g_zp_grant_q[g_zp_grant_qn].slot = pdsch_absolute_slot;
    g_zp_grant_q[g_zp_grant_qn].zp = *zp;
    g_zp_grant_q[g_zp_grant_qn].score = score;
    g_zp_grant_qn++;
  } else {
    g_zp_grant_dropped++;
  }
  pthread_mutex_unlock(&g_zp_grant_lock);
}

static bool zp_same_geometry(const nr_csirs_candidate_t *c, const fapi_nr_dl_config_csirs_pdu_rel15_t *p)
{
  return c->row == p->row && c->freq_domain == p->freq_domain && c->symb_l0 == p->symb_l0 && c->symb_l1 == p->symb_l1
         && c->cdm_type == p->cdm_type && c->freq_density == p->freq_density && c->start_rb == p->start_rb
         && c->nr_of_rbs == p->nr_of_rbs;
}

static void zp_grant_drain(void)
{
  int n;
  uint64_t dropped;
  zp_grant_ev_t q[ZP_GRANT_QUEUE];
  pthread_mutex_lock(&g_zp_grant_lock);
  n = g_zp_grant_qn;
  memcpy(q, g_zp_grant_q, sizeof(q[0]) * (size_t)n);
  g_zp_grant_qn = 0;
  dropped = g_zp_grant_dropped;
  pthread_mutex_unlock(&g_zp_grant_lock);
  g_zp_grant_dropped_seen = dropped;
  for (int e = 0; e < n; e++) {
    for (int k = 0; k < g_zp.n_conf; k++) {
      const int idx = g_zp.conf_idx[k];
      if (!zp_same_geometry(&g_zp.cand[idx], &q[e].zp))
        continue;
      const uint32_t revoked = g_zp.zp_revocations[idx];
      const nr_csirs_resource_t old_key = resource_key(&g_zp, k, true);
      nr_csirs_blind_zp_grant_feed(&g_zp, idx, q[e].slot, q[e].score);
      if (g_zp.zp_revocations[idx] != revoked) {
        nr_csirs_observer_revoke_zp(&g_csirs_obs, &old_key);
        /* Decoded-grant contradiction revokes the export; four missed pilot occasions
         * are still required before treating absence alone as a map change. */
      }
      g_zp_grant_applied++;
      if (q[e].score >= 0.0 && q[e].score <= NR_CSIRS_BLIND_ZP_MIN_SCORE) {
        g_zp_grant_contradictions++;
        LOG_A(PHY, "SENSING: CSIRS_BLIND ZP_GRANT_EVIDENCE idx=%d abs_slot=%u score=%.3f contradiction %s "
                   "(applied=%llu contradictions=%llu dropped=%llu)\n",
              idx, q[e].slot, q[e].score, g_zp.zp_revocations[idx] != revoked ? "REVOKED" : "kept",
              (unsigned long long)g_zp_grant_applied, (unsigned long long)g_zp_grant_contradictions,
              (unsigned long long)dropped);
      }
      break;
    }
  }
}

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

static void score_candidate(PHY_VARS_NR_UE *ue, int slot, uint32_t absolute_slot,
                            c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP], int idx, bool maintenance);

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
  if (g_timing < 0) {
    const char *e = getenv("ISAC_PDCCH_TIMING");
    g_timing = e && atoi(e) != 0;
  }
  if (g_reconf < 0) {
    const char *e = getenv("ISAC_RECONF");
    g_reconf = e && strcmp(e, "1") == 0;
  }
  const uint64_t slot_start_us = g_timing ? now_us() : 0;
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  if (g_armed == 0) {
    /* scramblingID is taken as the PCI, which acquisition already gives us. Sweeping 1024 values
     * would cost three orders of magnitude for a parameter that can simply be tried first. */
    if (nr_csirs_blind_init(&g_st, fp->N_RB_DL, fp->Nid_cell) <= 0) {
      g_on = 0;
      return;
    }
    nr_csirs_blind_init(&g_zp, fp->N_RB_DL, fp->Nid_cell);
    memset(g_zp_geometry_veto, 0, sizeof(g_zp_geometry_veto));
    g_zp_geometry_veto_total = 0;
    memset(g_zp_logged, 0, sizeof(g_zp_logged));
    memset(g_zp_events, 0, sizeof(g_zp_events));
    memset(g_zp_maint, 0, sizeof(g_zp_maint));
    g_armed = 1;
    LOG_I(PHY, "SENSING: CSIRS_BLIND armed: %d candidates, N_RB=%d scramb_id=%d (assumed = PCI)\n",
          g_st.n, fp->N_RB_DL, fp->Nid_cell);
  }
  /* Historical cost issue: nr_csirs_blind_next() used to return the confirmed index forever
   * after confirmation, so every later call regenerated the SAME resource's full-slot reference
   * (memset of up to 4 x 57 KB, nr_generate_csi_rs, an FEP, a correlation) and re-scored it -- and
   * the ZP search below, which shares idx, could only ever re-test that one candidate. Measured
   * 2026-09-17 on the PDCCH scan consumer: ~1.85 ms of untimed work per occasion, 68 % of PDCCH
   * occasions dropped, and 281k "CONFIRMED" lines in one 600 s run. The rate-matcher and the sensing
   * capture read the stored state (rate_match_from), which this leaves untouched.
   * 2026-09-27: confirmed candidates now LEAVE the rotation instead of owning it, and the search goes
   * on for other resources until capacity is reached. The OCUDU bed carries a TRS pair, a CQI row 2
   * and a ZP. Failed MCS-10 grants align with these slots; a CRC improvement still needs live
   * validation, as does the CPU cost of this opt-in search. Silence cannot prove completeness. */
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
   * on (the export explicitly suppresses both banks), which is why it is diagnostic only. */
  if (g_rank < 0) {
    const char *e = getenv("ISAC_CSIRS_BLIND_RANK");
    g_rank = (e != NULL && atoi(e) != 0) ? 1 : 0;
  }
  /* Every confirmation is logged once; nr_csirs_blind_next() skips confirmed candidates. */
  for (; g_conf_logged < g_st.n_conf; g_conf_logged++) {
    const int k = g_conf_logged, widx = g_st.conf_idx[k];
    const nr_csirs_resource_t key = resource_key(&g_st, k, false);
    if (!g_rank && nr_csirs_observer_confirm(&g_csirs_obs, &key, absolute_slot)) map_changed();
    char line[128], phase[32] = {0};
    if (nr_csirs_blind_format(&g_st.cand[widx], g_st.conf_period[k], g_st.conf_off[k][0], line, sizeof(line)) <= 0)
      continue;
    if (g_st.conf_n_off[k] == 2)
      snprintf(phase, sizeof(phase), " and offset %u", (unsigned)g_st.conf_off[k][1]);
    if (g_rank)
      LOG_A(PHY,
            "SENSING: CSIRS_BLIND CONFIRMED[rank] after %llu slots -- \"%s\"%s z=%.2f hits=%u "
            "tried=%u z_median=%.2f (search continues)\n",
            (unsigned long long)g_slots, line, g_st.conf_n_off[k] == 2 ? " (two phases)" : "", g_st.best_rho[widx],
            g_st.hits[widx], g_st.tried[widx], null_median());
    else
      LOG_A(PHY, "SENSING: CSIRS_BLIND CONFIRMED #%d after %llu slots -- csirs_monitor = \"%s\"%s (search continues)\n",
            k, (unsigned long long)g_slots, line, phase);
  }
  if (g_rank && g_st.n_conf > 0) {
    /* keep the population moving; see RANKING MODE above */
    g_st.n_conf = 0;
    g_st.confirmed = -1;
    g_conf_logged = 0;
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
  zp_grant_drain();
  /* Shared bounded admission bank: exported maintenance plus unexported probation
   * and divisor probes, even when NZP has retired the geometry. This list is NOT
   * the rate-match bank. At most MAX_CONF scores plus one ordinary discovery. */
  int occurring[NR_CSIRS_BLIND_MAX_CONF];
  const int n = nr_csirs_blind_zp_due(&g_zp, absolute_slot, occurring, NR_CSIRS_BLIND_MAX_CONF);
  for (int k = 0; k < n; k++) {
    g_zp_maint[occurring[k]].scheduled++;
    score_candidate(ue, slot, absolute_slot, rxdataF, occurring[k], true);
  }
  /* Alternate independent discovery rotations. A geometry retired by NZP must
   * remain eligible for ZP re-admission after pressure eviction. Still at most
   * one ordinary score, deduplicated against the bounded maintenance snapshot. */
  int idx = nr_csirs_blind_next((g_slots & 1) ? &g_st : &g_zp);
  if (idx < 0)
    idx = nr_csirs_blind_next((g_slots & 1) ? &g_zp : &g_st);
  bool measured = false;
  for (int k = 0; k < n; k++)
    if (occurring[k] == idx)
      measured = true;
  if (idx >= 0 && !measured)
    score_candidate(ue, slot, absolute_slot, rxdataF, idx, false);
  /* Re-score confirmed NZP occasions only with reconfiguration detection enabled.
   * The search bank remains intact; four scored misses produce one SOFT signal. */
  if (g_reconf && nr_cfg_epoch_note_csirs_map_change) {
    int due[NR_CSIRS_BLIND_MAX_CONF];
    const int nn = nr_csirs_blind_occurring(&g_st, absolute_slot, due, NR_CSIRS_BLIND_MAX_CONF);
    for (int k = 0; k < nn; ++k)
      if (due[k] != idx) score_candidate(ue, slot, absolute_slot, rxdataF, due[k], true);
  }
  if (g_timing) nr_csirs_observer_add_time(&g_csirs_obs, NR_CSIRS_TIME_SEARCH, now_us() - slot_start_us);
  if ((++g_slots % 20000) == 0) {
    if (g_timing) {
      nr_passive_metrics_t tm = {0};
      nr_csirs_observer_metrics(&g_csirs_obs, &tm);
      LOG_A(PHY, "SENSING: CSIRS_TIMING slots=%llu search_us_per_slot=%.2f idsweep_us_per_slot=%.2f "
                 "confirm_us_per_slot=%.2f cfr_us_per_slot=%.2f\n",
            (unsigned long long)g_slots, (double)tm.csirs_search_us / g_slots,
            (double)tm.csirs_idsweep_us / g_slots, (double)tm.csirs_confirm_us / g_slots,
            (double)tm.csirs_cfr_us / g_slots);
    }
    LOG_I(PHY, "SENSING: CSIRS_BLIND slots=%llu candidates=%d null_median=%.3f confirmed nzp=%d zp=%d zp_geometry_veto=%llu "
               "zp_grant_evidence=%llu contradictions=%llu dropped=%llu\n",
          (unsigned long long)g_slots, g_st.n, null_median(), g_st.n_conf, g_zp.n_conf,
          (unsigned long long)g_zp_geometry_veto_total, (unsigned long long)g_zp_grant_applied,
          (unsigned long long)g_zp_grant_contradictions, (unsigned long long)g_zp_grant_dropped_seen);
    for (int k = 0; k < g_zp.n; k++)
      if (g_zp_maint[k].scheduled || nr_csirs_blind_is_confirmed(&g_zp, k))
        zp_maint_summary(k);
  }
}

static void score_candidate(PHY_VARS_NR_UE *ue, int slot, uint32_t absolute_slot,
                            c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP], int idx, bool maintenance)
{
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const c16_t *rxdataF_ant0 = &rxdataF[0][0];
  if (idx < 0) {
    return;
  }
  const nr_csirs_candidate_t *c = &g_st.cand[idx];
  if (!maintenance) {
    const nr_csirs_resource_t candidate = {.row = c->row, .ports = nr_csirs_blind_row_ports(c->row),
        .density = c->freq_density, .freq_domain = c->freq_domain,
        .start_rb = c->start_rb, .nr_of_rbs = c->nr_of_rbs};
    nr_csirs_observer_candidate(&g_csirs_obs, &candidate, absolute_slot);
  }
  const uint64_t confirm_start_us = maintenance && g_timing ? now_us() : 0;
  /* Snapshot the feed's existing duplicate predicate before any feed mutates it. This affects
   * accounting only: repeated calls still execute exactly the historical receiver path. */
  const bool duplicate = (g_zp.tried[idx] > 0 || g_zp.zp_epoch[idx] > 0)
      && g_zp.zp_last_slot[idx] == absolute_slot;
  if (maintenance) {
    if (g_zp_maint[idx].reached++ == 0)
      g_zp_maint[idx].first_slot = absolute_slot;
    g_zp_maint[idx].last_slot = absolute_slot;
  }
  if (c->row == 5 && c->symb_l0 + 1 >= fp->symbols_per_slot) {
    if (maintenance)
      zp_maint_end(idx, absolute_slot, duplicate, ZPM_SETUP_INVALID, NAN, NAN, NAN, NAN);
    return;   /* row 5 maps l0 and l0+1, NOT the separate symb_l1 parameter */
  }

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
  if (n_ports <= 0 || n_ports > NR_CSIRS_BLIND_RT_ALL_PORTS) {
    if (maintenance)
      zp_maint_end(idx, absolute_slot, duplicate, ZPM_SETUP_INVALID, NAN, NAN, NAN, NAN);
    return;
  }
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
        if (maintenance)
          zp_maint_end(idx, absolute_slot, duplicate, ZPM_SETUP_INVALID, NAN, NAN, NAN, NAN);
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
  if (g_wide < 0) {
    const char *e = getenv("ISAC_CSIRS_BLIND_WIDE");
    g_wide = (e != NULL && atoi(e) != 0) ? 1 : 0;
  }
  if (g_wide && !maintenance) {
    uint16_t on_even = 0, on_odd = 0;
    nr_csirs_blind_symbol_on((const int16_t *)&rxdataF_ant0[off_sym], fp->ofdm_symbol_size,
                             fp->first_carrier_offset, fp->N_RB_DL, &on_even, &on_odd);
    if (nr_csirs_blind_fp_record(&g_fp, c->symb_l0, on_even, on_odd, absolute_slot))
      g_fp_dirty = true;
  }
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
  for (int k = 0; k < g_st.n_conf; ++k)
    if (g_st.conf_idx[k] == idx) {
      const nr_csirs_resource_t key = resource_key(&g_st, k, false);
      if (nr_csirs_observer_due(&g_csirs_obs, &key, absolute_slot, rho >= 4.0)) map_changed();
    }
  if (maintenance && g_timing)
    nr_csirs_observer_add_time(&g_csirs_obs, NR_CSIRS_TIME_CONFIRM, now_us() - confirm_start_us);
  double epr = NAN;
  bool done = false;
  if (rho < 0.0) {
    /* A zero-energy mapped comb is unscorable as an NZP sequence but is exactly the observation
     * the independent ZP energy path must evaluate. Invalid mappings still fail closed there. */
    goto score_zero_power;
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
  epr = nr_csirs_blind_energy_ratio_shift((const int16_t *)&rxdataF_ant0[off_sym],
                                                 (const int16_t *)&ref[off_sym], fp->ofdm_symbol_size,
                                                 fp->first_carrier_offset);
  if (!maintenance && epr > 0.0 && idx < NR_CSIRS_BLIND_MAX_CAND) {
    g_epr_sum[idx] += epr;
    g_epr_n[idx]++;
  }
  if (!maintenance && epr > 0.0 && idx == g_id_pin && slot >= 0) {
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
  if (!maintenance && g_rank && z > 3.0 && g_slots - s_span_last >= 200) {
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
  done = !maintenance && nr_csirs_blind_feed(&g_st, idx, absolute_slot, z, nullv);
  /* Keep the observed z population for the diagnostics. */
  if (!maintenance) {
    g_null[g_null_w] = z;
    g_null_w = (g_null_w + 1) % NULLWIN;
    if (g_null_n < NULLWIN)
      g_null_n++;
  }
score_zero_power:
  /* ZERO-POWER hypothesis: score the full mapped occupancy union, not just port 0's REs.
   * Every mapped row-5 symbol must be a measured periodic hole. OAI's row-5 mapping puts
   * ports 0/1 on l0 and ports 2/3 on l0+1; plane 0 has NO REs on l0+1.
   * One additional antenna-0 FFT and score on discovery and predicted maintenance occasions.
   * No neighbour-symbol FFT (tried on sdd/gap-csirs): a hole that is dark only because nothing is
   * scheduled there is withdrawn by contradiction revocation in nr_csirs_blind_zp_feed_pair(), and
   * REs dark only beside a pilot (lab G4, one antenna of an 8-port cell) already score ~0 here. */
  if (maintenance || !nr_csirs_blind_is_confirmed(&g_zp, idx)) {
    const int16_t *zp_refs[NR_CSIRS_BLIND_RT_MAX_PORTS];
    for (int p = 0; p < n_planes; p++)
      zp_refs[p] = (const int16_t *)&t_refbuf[p][off_sym];
    double old_score = -1.0;
    const double zs = nr_csirs_blind_zero_score_evidence_shift((const int16_t *)&rxdataF_ant0[off_sym], zp_refs, n_planes,
                                                            fp->ofdm_symbol_size, fp->first_carrier_offset,
                                                            c->row == 2 ? &old_score : NULL);
    double zs_other = zs;
    if (c->row == 5) {
      const unsigned symbol = c->symb_l0 + 1;
      const uint32_t off_other = symbol * fp->ofdm_symbol_size;
      zs_other = -1.0;
      if (nr_slot_fep_ant(ue, fp, (unsigned)slot, symbol, 0, rxdataF, link_type_dl, 0, ue->common_vars.rxdata) == 0) {
        for (int p = 0; p < n_planes; p++)
          zp_refs[p] = (const int16_t *)&t_refbuf[p][off_other];
        zs_other = nr_csirs_blind_zero_score_ports_shift((const int16_t *)&rxdataF_ant0[off_other], zp_refs, n_planes,
                                                       fp->ofdm_symbol_size, fp->first_carrier_offset);
      }
    }
    const double znull = median_of(g_zp_null, g_zp_null_n);
    if (maintenance) {
      const double joint = fmin(zs, zs_other);
      const enum zp_maint_outcome outcome = !isfinite(zs) || !isfinite(zs_other) || zs < 0.0 || zs_other < 0.0
          ? ZPM_SCORE_INVALID : joint <= NR_CSIRS_BLIND_ZP_MIN_SCORE ? ZPM_OCCUPIED
          : znull < 0.0 || !isfinite(znull) ? ZPM_POPULATION_UNKNOWN
          : nr_csirs_blind_zp_score_qualifies(joint, znull) ? ZPM_QUALIFIED_HOLE : ZPM_POPULATION_SUPPRESSED;
      zp_maint_end(idx, absolute_slot, duplicate, outcome, rho, zs, zs_other, znull);
    }
    if (c->row == 2 && nr_csirs_blind_zp_score_qualifies(old_score, znull)
        && !nr_csirs_blind_zp_score_qualifies(zs, znull)
        && (g_zp_geometry_veto[idx].count == 0 || g_zp_geometry_veto[idx].last_slot != absolute_slot)) {
      g_zp_geometry_veto[idx].last_slot = absolute_slot;
      g_zp_geometry_veto_total++;
      if (++g_zp_geometry_veto[idx].count == 1)
        LOG_A(PHY, "SENSING: CSIRS_BLIND ZP_GEOMETRY_VETO row=%u fd=%u l0=%u l1=%u density=%u "
                   "start_rb=%u nrb=%u abs_slot=%u old_score=%.6f completeness_score=%.6f null=%.6f total=%llu\n",
              c->row, c->freq_domain, c->symb_l0, c->symb_l1, c->freq_density, c->start_rb, c->nr_of_rbs,
              absolute_slot, old_score, zs, znull, (unsigned long long)g_zp_geometry_veto_total);
    }
    if (zs >= 0.0 && zs_other >= 0.0) {
      const uint32_t zhits = g_zp.hits[idx];
      const uint32_t epoch = g_zp.zp_epoch[idx];
      const uint32_t revoked = g_zp.zp_revocations[idx];
      const uint8_t contradictions = g_zp.zp_contradictions[idx];
      char previous_hits[96] = {0};
      int previous_used = 0;
      for (int h = 0; h < g_zp.n_hit_slot[idx]; h++)
        previous_used += snprintf(previous_hits + previous_used, sizeof(previous_hits) - previous_used,
                                  "%s%u", h ? "," : "", g_zp.hit_slot[idx][h]);
      const int saved_zp_pin = g_zp.pinned;
      const uint32_t saved_zp_pin_left = g_zp.pin_left;
      nr_csirs_blind_zp_feed_pair(&g_zp, idx, absolute_slot, zs, zs_other, znull);
      /* Admitted maintenance already has a bounded due path. Letting it seize either discovery
       * rotation starves candidates that have never been measured. Ordinary discovery retains
       * the existing hit pin, but maintenance cannot create or renew one. */
      if (maintenance) {
        g_zp.pinned = saved_zp_pin;
        g_zp.pin_left = saved_zp_pin_left;
      }
      const bool is_confirmed = nr_csirs_blind_is_confirmed(&g_zp, idx);
      if (g_zp.zp_revocations[idx] != revoked) {
        nr_csirs_resource_t old = {.row = c->row, .ports = nr_csirs_blind_row_ports(c->row),
            .density = c->freq_density, .zp = 1, .freq_domain = c->freq_domain,
            .start_rb = c->start_rb, .nr_of_rbs = c->nr_of_rbs};
        nr_csirs_observer_revoke_zp(&g_csirs_obs, &old);
      }
      if (g_zp.zp_revocations[idx] != revoked)
        zp_maint_summary(idx);
      char hit_history[96] = {0};
      int used = 0;
      for (int h = 0; h < g_zp.n_hit_slot[idx]; h++)
        used += snprintf(hit_history + used, sizeof(hit_history) - used, "%s%u",
                         h ? "," : "", g_zp.hit_slot[idx][h]);
      const char *event = g_zp.zp_revocations[idx] != revoked ? "REVOKED"
          : g_zp.zp_epoch[idx] != epoch ? "EPOCH"
          : g_zp.hits[idx] != zhits ? "HIT"
          : g_zp.zp_contradictions[idx] != contradictions ? "CONTRADICTION" : NULL;
      if (g_zp.zp_revocations[idx] != revoked)
        LOG_A(PHY, "SENSING: CSIRS_BLIND ZP REVOKED idx=%d abs_slot=%u epoch=%u hits=%s "
                   "period=%u offsets=%u,%u n_off=%u score=%.6f other_score=%.6f revocations=%u\n",
              idx, absolute_slot, epoch, previous_hits, g_zp.zp_selected_period[idx],
              g_zp.zp_selected_off[idx][0], g_zp.zp_selected_off[idx][1], g_zp.zp_selected_n_off[idx],
              zs, zs_other, g_zp.zp_revocations[idx]);
      if (event && g_zp_events[idx] < ZP_EVENT_LIMIT) {
        g_zp_events[idx]++;
        LOG_A(PHY, "SENSING: CSIRS_BLIND ZP_EVIDENCE idx=%d row=%u fd=%u l0=%u l1=%u density=%u "
                   "event=%s abs_slot=%u epoch=%u hits=%s previous_epoch=%u previous_hits=%s "
                   "selected_period=%u offsets=%u,%u n_off=%u contradictions=%u revocations=%u detail=%u/%u\n",
              idx, c->row, c->freq_domain, c->symb_l0, c->symb_l1, c->freq_density, event, absolute_slot,
              g_zp.zp_epoch[idx], hit_history, epoch, previous_hits, g_zp.zp_selected_period[idx],
              g_zp.zp_selected_off[idx][0], g_zp.zp_selected_off[idx][1], g_zp.zp_selected_n_off[idx],
              g_zp.zp_contradictions[idx], g_zp.zp_revocations[idx],
              g_zp_events[idx], ZP_EVENT_LIMIT);
      }
      /* A ZP hit pins the SHARED rotation (g_st drives which candidate is scored) for the same reason
       * an NZP hit does; a ZP confirmation counts as news for the search budget. */
      if (!maintenance && g_zp.hits[idx] != zhits && g_st.pin_left == 0 && g_st.pinned != idx)
        nr_csirs_blind_pin(&g_st, idx, NR_CSIRS_BLIND_PIN_CONFIRM_CALLS);
      for (int k = 0; k < g_zp.n_conf; k++) {
        if (g_zp.conf_idx[k] != idx || !is_confirmed || g_zp_logged[idx] == g_zp.zp_epoch[idx])
          continue;
        g_zp_logged[idx] = g_zp.zp_epoch[idx];
        const nr_csirs_resource_t key = resource_key(&g_zp, k, true);
        if (nr_csirs_observer_confirm(&g_csirs_obs, &key, absolute_slot)) map_changed();
        zp_maint_summary(idx);
        char line[128], phase[32] = {0};
        if (g_zp.conf_n_off[k] == 2)
          snprintf(phase, sizeof(phase), " and offset %u", (unsigned)g_zp.conf_off[k][1]);
        if (nr_csirs_blind_format(&g_zp.cand[g_zp.conf_idx[k]], g_zp.conf_period[k], g_zp.conf_off[k][0], line, sizeof(line)) > 0)
          LOG_A(PHY, "SENSING: CSIRS_BLIND ZP CONFIRMED #%d after %llu slots (rate-matching only) -- \"%s\"%s "
                     "idx=%d abs_slot=%u epoch=%u hits=%s revocations=%u\n",
                k, (unsigned long long)g_slots, line, phase, idx, absolute_slot, g_zp.zp_epoch[idx],
                hit_history, g_zp.zp_revocations[idx]);
      }
      if (!maintenance) {
        g_zp_null[g_zp_null_w] = zs < zs_other ? zs : zs_other;
        g_zp_null_w = (g_zp_null_w + 1) % NULLWIN;
        if (g_zp_null_n < NULLWIN)
          g_zp_null_n++;
      }
    }
  }
  if (maintenance || rho < 0.0)
    return;
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
    const uint64_t ids_start_us = g_timing ? now_us() : 0;
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
    if (g_timing) nr_csirs_observer_add_time(&g_csirs_obs, NR_CSIRS_TIME_IDSWEEP, now_us() - ids_start_us);
  }
  /* Rows 6-18: match the measured footprints against OAI's table (~60 us, hence rate-limited) and
   * hand each new fit to the ordinary confirm path -- round-robin, feed(), IDSWEEP -- by appending it
   * to the population. Pinned for a confirm budget when nothing else holds the pin, since round-robin
   * alone would reach it only once per ~700 calls. Nothing is appended on a cell without periodic
   * pair-structured energy, which leaves the rows 1-5 search exactly as it was. */
  if (g_wide && g_fp_dirty && g_slots - g_fp_last >= FP_MATCH_EVERY) {
    g_fp_dirty = false;
    g_fp_last = g_slots;
    nr_csirs_candidate_t fit[16];
    const int n_fit = nr_csirs_blind_fp_match(&g_fp, fp->N_RB_DL, fp->Nid_cell, fit, 16);
    for (int i = 0; i < n_fit; i++) {
      const int at = nr_csirs_blind_append(&g_st, &fit[i]);
      if (at < 0)
        continue;
      /* g_zp mirrors g_st index for index: the ZP feed below scores g_zp at g_st's idx. */
      const int zat = nr_csirs_blind_append(&g_zp, &fit[i]);
      static bool s_zp_diverged;
      if (zat != at && !s_zp_diverged) {
        s_zp_diverged = true;
        LOG_E(PHY, "SENSING: CSIRS_BLIND FOOTPRINT g_zp append index %d != g_st index %d -- ZP search no longer "
                   "mirrors the NZP population\n", zat, at);
      }
      if (g_st.pin_left == 0)
        nr_csirs_blind_pin(&g_st, at, NR_CSIRS_BLIND_PIN_CONFIRM_CALLS);
      LOG_A(PHY, "SENSING: CSIRS_BLIND FOOTPRINT row%u fd0x%x l0=%u l1=%u density=%u cdm=%u (%d ports) "
                 "fits the measured periodic RE pattern -- candidate %d added to the confirm path\n",
            fit[i].row, fit[i].freq_domain, fit[i].symb_l0, fit[i].symb_l1, fit[i].freq_density,
            fit[i].cdm_type, nr_csirs_blind_row_ports(fit[i].row), at);
    }
  }
  /* the sweep above reused the shared reference buffer; the candidate's own reference is stale now */
  (void)done; // logged once at the start of the next call
}

/* RATE MATCHING FROM THE BLIND SEARCH (2026-09-15). The passive PDSCH decoder used to refuse any
 * grant flagged with CSI-RS rate matching because nothing told it WHICH REs to skip; the blind
 * search now does, so its confirmed resource is offered as the FAPI PDU the demodulator already
 * understands. ZP CSI-RS (a pure rate-matching pattern) is searched by energy (nr_csirs_blind_zero_score)
 * on the same candidates and offered separately as csi_type 2. */
static void fill_pdu(const nr_csirs_candidate_t *c, uint8_t csi_type, fapi_nr_dl_config_csirs_pdu_rel15_t *out)
{
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
}
int nr_csirs_blind_rt_rate_match_all(uint32_t absolute_slot, fapi_nr_dl_config_csirs_pdu_rel15_t *out, int max)
{
  /* Ranking is diagnostic only: neither newly confirmed NZP nor retained ZP may alter PDSCH. */
  if (out == NULL || max <= 0 || g_on <= 0 || g_armed == 0 || g_rank > 0)
    return 0;
  int n = 0, idx[NR_CSIRS_BLIND_MAX_CONF];
  const int nn = nr_csirs_blind_occurring(&g_st, absolute_slot, idx, NR_CSIRS_BLIND_MAX_CONF);
  for (int i = 0; i < nn && n < max; i++)
    fill_pdu(&g_st.cand[idx[i]], 1 /* NZP */, &out[n++]);
  const int nz = nr_csirs_blind_occurring(&g_zp, absolute_slot, idx, NR_CSIRS_BLIND_MAX_CONF);
  for (int i = 0; i < nz && n < max; i++)
  {
    fill_pdu(&g_zp.cand[idx[i]], 2 /* ZP: rate matching only, no estimation */, &out[n++]);
    nr_csirs_observer_export_zp(&g_csirs_obs);
  }
  return n;
}
