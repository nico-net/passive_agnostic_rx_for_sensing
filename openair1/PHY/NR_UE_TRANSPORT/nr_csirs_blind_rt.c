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
#include "nr_pdsch_passive_decode.h" /* CSIFULLAB */
/* Weak: unit-test targets link this file without the acquisition state. */
extern void nr_passive_acq_note_carrier_from_rs(const char *evidence) __attribute__((weak));
#include "PHY/MODULATION/modulation_UE.h"
#include "nr_csirs_blind_search.h"
#include "nr_csirs_observer.h"
#include "nr_csirs_monitor.h"
#include "nr_csirs_disc_sweep_internal.h"

#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "common/utils/LOG/log.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <time.h>

/* Focused blind-search fixtures link this file without the discovery/monitor units. */
extern bool nr_csirs_disc_enabled(void) __attribute__((weak));
extern int nr_csirs_monitor_add_dynamic(const fapi_nr_dl_config_csirs_pdu_rel15_t *, uint16_t, uint16_t, int16_t)
    __attribute__((weak));
extern void nr_csirs_monitor_remove_dynamic(const fapi_nr_dl_config_csirs_pdu_rel15_t *) __attribute__((weak));

/* Weak for the standalone blind-search fixture; the receiver links the epoch owner. */
__attribute__((weak)) void nr_cfg_epoch_note_csirs_map_change(void);
static nr_csirs_observer_t g_csirs_obs;
/* DL processing can use multiple workers, and rate matching is read by decode workers. */
static __thread bool *g_disc_fep_map;
uint64_t csirs_disc_readmit_skipped;
uint64_t csirs_disc_confirm_drops;
uint64_t csirs_disc_confirm_wait_max_us;
uint64_t csirs_disc_confirm_cpu_scores,csirs_disc_confirm_gpu_scores;
/* Standalone FD dump fixtures do not link the PHY FEP. */
#pragma weak nr_slot_fep
static pthread_mutex_t g_csirs_state_lock = PTHREAD_MUTEX_INITIALIZER;
/* nr_csirs_blind_rt_discovery_init: one-time setup of the CSI-RS search state lock with priority inheritance, so a low-priority holder cannot starve a real-time actor.
 *   Re-creates g_csirs_state_lock as PTHREAD_PRIO_INHERIT; falls back to a plain mutex when the attribute is unsupported.
 *   Called by nr_csirs_disc.c (init_once) before the discovery worker and the DL actors start.
 */
void nr_csirs_blind_rt_discovery_init(void)
{
  pthread_mutexattr_t attr;
  if(pthread_mutexattr_init(&attr)) return;
  if(!pthread_mutexattr_setprotocol(&attr,PTHREAD_PRIO_INHERIT)
      && !pthread_mutex_destroy(&g_csirs_state_lock)) {
    if(pthread_mutex_init(&g_csirs_state_lock,&attr)) pthread_mutex_init(&g_csirs_state_lock,NULL);
  }
  pthread_mutexattr_destroy(&attr);
}
static void discovery_publish(void);
static void discovery_queue_reset(void);
static void discovery_mark_admitted(int);
/* csirs_unlock: cleanup handler for guard-scoped lock holders: publishes the discovery state (discovery_publish) and releases the mutex.
 *   Used with __attribute__((cleanup)) in nr_csirs_blind_rt_slot() and nr_csirs_blind_rt_rate_match_all(). A NULL lock is a no-op.
 */
static void csirs_unlock(pthread_mutex_t **lock) { if (*lock) { discovery_publish(); pthread_mutex_unlock(*lock); } }
static int g_timing = -1;
static int g_reconf = -1;
static uint64_t now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
/* nr_csirs_blind_rt_metrics: fills the CSI-RS part of the passive metrics: observer counters and the number of discovery re-admissions skipped.
 *   Reads atomics and the observer's counters. Caller: nr_passive_metrics.c (metrics emission).
 */
void nr_csirs_blind_rt_metrics(nr_passive_metrics_t *m)
{
  nr_csirs_observer_metrics(&g_csirs_obs, m);
  m->csirs_disc_readmit_skipped = __atomic_load_n(&csirs_disc_readmit_skipped, __ATOMIC_RELAXED);
}
void nr_csirs_blind_rt_cfr_time_us(uint64_t us) { nr_csirs_observer_add_time(&g_csirs_obs, NR_CSIRS_TIME_CFR, us); }
static nr_csirs_resource_t resource_key(const nr_csirs_blind_state_t *s, int k, bool zp)
{
  const nr_csirs_candidate_t *c = &s->cand[s->conf_idx[k]];
  return (nr_csirs_resource_t){.row = c->row, .ports = nr_csirs_blind_row_ports(c->row),
      .density = c->freq_density, .period = s->conf_period[k], .offset = s->conf_off[k][0],
      .offset2 = s->conf_off[k][1], .n_offsets = s->conf_n_off[k], .zp = zp,
      .freq_domain = c->freq_domain, .start_rb = c->start_rb, .nr_of_rbs = c->nr_of_rbs,
      .symb_l0 = c->symb_l0};
}
static void map_changed(void)
{
  if (nr_cfg_epoch_note_csirs_map_change) nr_cfg_epoch_note_csirs_map_change();
}

/* Reference planes actually allocated. NZP scores plane 0; ZP scores their occupancy union.
 * Rows 1-5 need at most 4 ports,
 * and for the 8-32-port rows every port >= 3 writes into plane 3 as a shared union sink, so a
 * 32-port candidate costs no extra 7 MB of per-thread buffers nor a 7 MB memset per slot. */
#define NR_CSIRS_BLIND_RT_LEGACY_PORTS 4
#define NR_CSIRS_BLIND_RT_MAX_PORTS 32
#define NR_CSIRS_BLIND_RT_ALL_PORTS 32

static nr_csirs_blind_state_t g_st;
static nr_csirs_blind_state_t g_zp;      /* zero-power search over the same candidates */
static void fill_pdu(const nr_csirs_candidate_t *c, uint8_t csi_type, fapi_nr_dl_config_csirs_pdu_rel15_t *out);
static int g_disc = -1;
static float g_disc_strength[NR_CSIRS_BLIND_MAX_CAND];
static uint8_t g_disc_probe[NR_CSIRS_BLIND_MAX_CAND][2];
static uint32_t g_disc_visits[NR_CSIRS_BLIND_MAX_CAND][2];
static uint32_t g_disc_scored_slot[NR_CSIRS_BLIND_MAX_CAND];
static struct {
  nr_csirs_candidate_t candidate;
  uint16_t period, off[2];
  uint8_t n_off;
} g_disc_export[NR_CSIRS_BLIND_MAX_CAND];

/* nzp_observer_score: re-scores one confirmed NZP CSI-RS occasion and reports whether the observer revoked it (reconfiguration detection).
 *   Only active with reconfiguration (g_reconf). Revocation removes the dynamic CSI-RS from the monitor; inconclusive scores (between the noise and confirmation bars) do not revoke.
 *   Caller: score_candidate().
 */
static bool nzp_observer_score(int idx, uint32_t slot, double rho)
{
  if (!g_reconf) return false;
  int due[NR_CSIRS_BLIND_DISC_MAX_CONF];
  const int n_due = nr_csirs_blind_occurring(&g_st, slot, due, NR_CSIRS_BLIND_DISC_MAX_CONF);
  for (int j = 0; j < n_due; ++j)
    if (due[j] == idx)
      for (int k = 0; k < g_st.n_conf; ++k)
        if (g_st.conf_idx[k] == idx) {
          const nr_csirs_resource_t key = resource_key(&g_st, k, false);
          /* Scores between the noise bar and the confirmation bar are inconclusive. */
          const bool revoked = nr_csirs_observer_due(&g_csirs_obs, &key, slot, rho >= 2.0);
          if (revoked && g_disc > 0 && nr_csirs_monitor_remove_dynamic) {
            fapi_nr_dl_config_csirs_pdu_rel15_t pdu = {0};
            const nr_csirs_candidate_t *c = &g_st.cand[idx];
            pdu.row = c->row;
            pdu.freq_domain = c->freq_domain;
            pdu.symb_l0 = c->symb_l0;
            pdu.symb_l1 = c->symb_l1;
            pdu.cdm_type = c->cdm_type;
            pdu.freq_density = c->freq_density;
            pdu.start_rb = c->start_rb;
            pdu.nr_of_rbs = c->nr_of_rbs;
            pdu.scramb_id = c->scramb_id;
            nr_csirs_monitor_remove_dynamic(&pdu);
          }
          return revoked;
        }
  return false;
}
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

/* discovery_empty_state: resets one CSI-RS search state (candidate population, confirmations, counters) to empty; confirmed and pinned indices are set to -1.
 *   Caller: nr_csirs_blind_rt_add_discovered(), nr_csirs_blind_rt_discovery_reset() and nr_csirs_blind_rt_slot() (first arm, discovery mode).
 */
static void discovery_empty_state(nr_csirs_blind_state_t *st)
{
  memset(st, 0, sizeof(*st));
  st->confirmed = -1;
  st->pinned = -1;
}

/* discovery_same_resource: true when two candidates describe the same physical CSI-RS resource: nested or near-equal RB extents, identical row, symbols, CDM, density, scrambling and slot delta.
 *   Used when a new discovered hypothesis is merged with an existing candidate. Disjoint extents or different IDs never match.
 *   Caller: nr_csirs_blind_rt_add_discovered().
 */
static bool discovery_same_resource(const nr_csirs_candidate_t *a, const nr_csirs_candidate_t *b)
{
  int ae = a->start_rb + a->nr_of_rbs, be = b->start_rb + b->nr_of_rbs;
  bool extent = (a->start_rb <= b->start_rb && ae >= be)
      || (b->start_rb <= a->start_rb && be >= ae)
      || (abs((int)a->start_rb - b->start_rb) <= 4 && abs(ae - be) <= 4);
  return extent && a->start_rb < be && b->start_rb < ae
      && a->row == b->row && a->freq_domain == b->freq_domain && a->symb_l0 == b->symb_l0
      && a->symb_l1 == b->symb_l1 && a->cdm_type == b->cdm_type && a->freq_density == b->freq_density
      && a->scramb_id == b->scramb_id && a->slot_delta == b->slot_delta;
}

/* discovery_union_seed: merges a new sweep lattice (period p, offset off) into the seed lattice of candidate i, returning the merged period and up to two phases.
 *   Fails for incompatible periods or phases (or for a row-1 TRS with non-adjacent phases); the caller then starts a fresh seed.
 *   Caller: nr_csirs_blind_rt_add_discovered().
 */
static bool discovery_union_seed(int i, uint16_t p, uint16_t off, uint16_t *period, uint16_t offsets[2], int *n)
{
  uint16_t old = g_st.seed_period[i];
  if (!old || !p || (old % p && p % old)) return false;
  *period = old < p ? old : p;
  *n = 0;
  uint16_t all[] = {g_st.seed_offset[i], g_st.seed_offset2[i], off};
  for (int j = 0; j < 3; ++j) {
    if (j == 1 && g_st.seed_n_off[i] < 2) continue;
    uint16_t phase = all[j] % *period;
    if (*n && offsets[0] == phase) continue;
    if (*n == 2) { if (offsets[1] == phase) continue; return false; }
    offsets[(*n)++] = phase;
  }
  /* A second TRS burst phase is adjacent, including wraparound. Unrelated
   * phases must not poison an existing candidate's confirmation evidence. */
  return g_st.cand[i].row != 1 || *n < 2 || abs((int)offsets[0] - offsets[1]) == 1
      || abs((int)offsets[0] - offsets[1]) == *period - 1;
}

/* nr_csirs_blind_rt_add_discovered: admits one discovered CSI-RS hypothesis (from the discovery sweep) into the search population and returns its candidate index (-1 when refused).
 *   Checks the candidate is safe, merges it with an existing resource on the same RBs (union of the seed lattice), or appends it to both banks (g_st and g_zp).
 *   Incompatible provisional evidence is replaced by the stronger one; confirmed bursts are kept. Publishes the discovery state. Caller: nr_csirs_disc.c (discovery process, nr_csirs_disc_process).
 */
int nr_csirs_blind_rt_add_discovered(const nr_csirs_disc_hyp_t *h, const nr_csirs_disc_win_t *w)
{
  if (!h || !w)
    return -1;
  pthread_mutex_lock(&g_csirs_state_lock);
  if (g_disc < 0)
    g_disc = nr_csirs_disc_enabled && nr_csirs_disc_enabled();
  if (g_disc <= 0) {
    pthread_mutex_unlock(&g_csirs_state_lock);
    return -1;
  }
  g_st.discovery_bank=true;
  if (!g_armed) {
    discovery_empty_state(&g_st);
    g_st.discovery_bank=true;
    discovery_empty_state(&g_zp);
    g_armed = 1;
  }
  nr_csirs_candidate_t c = {.row = h->row,
                            .freq_domain = h->freq_domain,
                            .symb_l0 = h->symb_l0,
                            .symb_l1 = h->symb_l1,
                            .cdm_type = h->cdm_type,
                            .freq_density = h->density,
                            .scramb_id = w->scramb_id,
                            .start_rb = h->rb_start,
                            .nr_of_rbs = h->n_rb,
                            .slot_delta = w->slot_delta};
  int at = -1, n = 1;
  uint16_t period = h->period, offsets[2] = {h->offset, 0};
  if (!period || h->offset >= period || !nr_csirs_blind_candidate_safe(&c)) {
    pthread_mutex_unlock(&g_csirs_state_lock);
    return -1;
  }
  for (int i = 0; i < g_st.n; ++i) {
    if (!discovery_same_resource(&g_st.cand[i], &c)) continue;
    uint16_t old = g_st.seed_period[i];
    if (!old || (old % h->period && h->period % old)) continue;
    if (!discovery_union_seed(i, h->period, h->offset, &period, offsets, &n)) {
      /* CSI-FULL (ISAC_CSIRS_FULL_BURSTS=1): a cell sends the same row-1 TRS geometry in more than one burst per period
       * (OTA PCI 415, captures 2026-10-08: ID 415 k0=0 l=4/8 and k0=1 l=5/9, RB 0-51, at abs%160 in {15,16} AND
       * {20,21}). An entry holds one burst (<= 2 adjacent phases), so a second burst is its own entry; it is
       * confirmed independently on its own occasions by the unchanged per-slot confirm path. Keep scanning for an
       * existing entry of that burst; none -> a new entry below. */
      if (nr_csirs_full_bursts_enabled() && h->row == 1) { /* ISAC_CSIRS_FULL_BURSTS */
        period = h->period; offsets[0] = h->offset; offsets[1] = 0; n = 1;
        continue;
      }
      /* Incompatible sweep phases can come from sequence leakage. Retain one
       * physical resource and the strongest provisional burst, never clones. */
      if (nr_csirs_blind_is_confirmed(&g_st, i) || w->z <= g_disc_strength[i]) {
        __atomic_fetch_add(&csirs_disc_readmit_skipped, 1, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&g_csirs_state_lock);
        return i;
      }
      period = h->period; offsets[0] = h->offset; offsets[1] = 0; n = 1;
      g_st.hits[i] = g_st.n_hit_slot[i] = 0;
      memset(g_st.disc_n_phase[i], 0, sizeof(g_st.disc_n_phase[i]));
      memset(g_st.disc_support[i], 0, sizeof(g_st.disc_support[i]));
      /* Only provisional evidence is replaced; an independently confirmed burst
       * wins over an incompatible sweep proposal until an epoch reset. */
    }
    at = i;
    __atomic_fetch_add(&csirs_disc_readmit_skipped, 1, __ATOMIC_RELAXED);
    if (c.nr_of_rbs > g_st.cand[i].nr_of_rbs) g_st.cand[i] = g_zp.cand[i] = c;
    break;
  }
  if (at < 0 && g_st.n < NR_CSIRS_BLIND_MAX_CAND && g_st.n == g_zp.n) {
    at = g_st.n++;
    g_zp.n++;
    g_st.cand[at] = g_zp.cand[at] = c;
    g_disc_scored_slot[at] = UINT32_MAX;
    discovery_mark_admitted(at);
    period = h->period; offsets[0] = h->offset; offsets[1] = 0; n = 1;
  }
  if (at >= 0) {
    if (g_st.seed_period[at] != period || g_st.seed_offset[at] != offsets[0]
        || g_st.seed_offset2[at] != offsets[1] || g_st.seed_n_off[at] != n) {
      g_disc_probe[at][0] = g_disc_probe[at][1] = 3;
      g_disc_visits[at][0] = g_disc_visits[at][1] = 0;
    }
    if (w->z > g_disc_strength[at]) g_disc_strength[at] = w->z;
    nr_csirs_blind_state_t *states[] = {&g_st, &g_zp};
    for (int j = 0; j < 2; ++j) {
      states[j]->seed_period[at] = period;
      states[j]->seed_offset[at] = offsets[0];
      states[j]->seed_offset2[at] = offsets[1];
      states[j]->seed_n_off[at] = n;
    }
  }
  discovery_publish();
  pthread_mutex_unlock(&g_csirs_state_lock);
  return at;
}

/* nr_csirs_blind_rt_discovery_confirmed: true when a discovered hypothesis matches a confirmed NZP resource of the search (same geometry, period and an offset phase).
 *   Read-only under g_csirs_state_lock. Caller: nr_csirs_disc.c (skips discovery hypotheses that are already confirmed).
 */
bool nr_csirs_blind_rt_discovery_confirmed(const nr_csirs_disc_hyp_t *h)
{
  if (!h)
    return false;
  pthread_mutex_lock(&g_csirs_state_lock);
  bool confirmed = false;
  for (int k = 0; k < g_st.n_conf && !confirmed; ++k) {
    const nr_csirs_candidate_t *c = &g_st.cand[g_st.conf_idx[k]];
    confirmed = c->row == h->row && c->freq_domain == h->freq_domain && c->symb_l0 == h->symb_l0
        && c->symb_l1 == h->symb_l1 && c->cdm_type == h->cdm_type && c->freq_density == h->density
        && c->start_rb == h->rb_start && c->nr_of_rbs == h->n_rb
        && g_st.conf_period[k] == h->period
        && (g_st.conf_off[k][0] == h->offset
            || (g_st.conf_n_off[k] == 2 && g_st.conf_off[k][1] == h->offset));
  }
  pthread_mutex_unlock(&g_csirs_state_lock);
  return confirmed;
}

/* nr_csirs_blind_rt_discovery_match: index (0..63) of the confirmed NZP resource that is the same physical resource as `h`, or -1.
 *   Tolerant version of discovery_confirmed for the rate-limit bookkeeping: blind_rt may widen/merge a resource after admission (e.g. rb 0:208 -> 0:217),
 *   so identity = first symbol, density, period, an offset phase, RB spans overlapping by >= half of the narrower one, and (same row) overlapping freq_domain.
 *   Read-only under g_csirs_state_lock. Caller: nr_csirs_disc.c rl_update().
 */
int nr_csirs_blind_rt_discovery_match(const nr_csirs_disc_hyp_t *h)
{
  if (!h)
    return -1;
  int found = -1;
  pthread_mutex_lock(&g_csirs_state_lock);
  for (int k = 0; k < g_st.n_conf && k < 64 && found < 0; ++k) {
    const nr_csirs_candidate_t *c = &g_st.cand[g_st.conf_idx[k]];
    const int lo = c->start_rb > h->rb_start ? c->start_rb : h->rb_start;
    const int hi = c->start_rb + c->nr_of_rbs < h->rb_start + h->n_rb ? c->start_rb + c->nr_of_rbs : h->rb_start + h->n_rb;
    const int narrow = c->nr_of_rbs < h->n_rb ? c->nr_of_rbs : h->n_rb;
    if (c->symb_l0 == h->symb_l0 && c->freq_density == h->density && g_st.conf_period[k] == h->period
        && (g_st.conf_off[k][0] == h->offset || (g_st.conf_n_off[k] == 2 && g_st.conf_off[k][1] == h->offset))
        && hi > lo && 2 * (hi - lo) >= narrow && (c->row != h->row || (c->freq_domain & h->freq_domain)))
      found = k;
  }
  pthread_mutex_unlock(&g_csirs_state_lock);
  return found;
}

/* nr_csirs_blind_rt_discovery_reset: clears the discovery-fed search state (exports, visits, probes, strengths, both banks, queue) and forgets all confirmations.
 *   Withdraws each exported NZP resource from the monitor first (nr_csirs_monitor_remove_dynamic). Caller: nr_csirs_disc.c (epoch listener, on a new configuration epoch).
 */
void nr_csirs_blind_rt_discovery_reset(void)
{
  pthread_mutex_lock(&g_csirs_state_lock);
  if (g_disc > 0) {
    for (int i = 0; i < g_st.n; ++i)
      if (g_disc_export[i].n_off && nr_csirs_monitor_remove_dynamic) {
        fapi_nr_dl_config_csirs_pdu_rel15_t old;
        fill_pdu(&g_disc_export[i].candidate, 1, &old);
        nr_csirs_monitor_remove_dynamic(&old);
      }
    memset(g_disc_export, 0, sizeof(g_disc_export));
    memset(g_disc_visits, 0, sizeof(g_disc_visits));
    memset(g_disc_probe, 0, sizeof(g_disc_probe));
    memset(g_disc_strength, 0, sizeof(g_disc_strength));
    memset(g_disc_scored_slot, 0xff, sizeof(g_disc_scored_slot));
    discovery_empty_state(&g_st);
    g_st.discovery_bank=true;
    discovery_empty_state(&g_zp);
    g_armed = 1;
    g_conf_logged = 0;
    g_slots = 0;
    discovery_queue_reset();
    discovery_publish();
  }
  pthread_mutex_unlock(&g_csirs_state_lock);
}
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

/* discovery_score: correlation score of one candidate on one slot: mean normalised correlation of the received REs against the reference, per port and antenna, over blocks of the mapped REs.
 *   Returns the score averaged over antennas and ports (about 1 for a matching resource, noise otherwise), or -1 if the mapping fails; *n_used receives the RE count.
 *   Pure function on the given buffers. Caller: score_candidate().
 */
static double discovery_score(const NR_DL_FRAME_PARMS *fp,
                              const nr_csirs_candidate_t *c,
                              const c16_t *const *rx,
                              c16_t *const *refs,
                              int n_ant,
                              int block,
                              int *n_used)
{
  nr_csirs_disc_hyp_t h = {.row = c->row,
                           .freq_domain = c->freq_domain,
                           .symb_l0 = c->symb_l0,
                           .symb_l1 = c->symb_l1,
                           .cdm_type = c->cdm_type,
                           .density = c->freq_density,
                           .rb_start = c->start_rb,
                           .n_rb = c->nr_of_rbs,
                           .period = 1,
                           .offset = 0};
  nr_csirs_disc_sweep_map_t map;
  if (nr_csirs_disc_sweep_map(&h, fp->N_RB_DL, &map) < 0)
    return -1;
  double total = 0;
  int used = 0;
  for (int ant = 0; ant < n_ant; ++ant) {
    for (int p = 0; p < map.ports; ++p) {
      double sum = 0;
      int blocks = 0;
      const int first = map.begin[p], last = map.begin[p + 1];
      for (int begin = first; begin < last;) {
        const int end = last - begin < block ? last : begin + block;
        double ar = 0, ai = 0, er = 0, ex = 0;
        for (int i = begin; i < end; ++i) {
          const int crb = map.re[i].grid_index % (12 * fp->N_RB_DL);
          const int symbol = map.re[i].symbol;
          const int rk = symbol * fp->ofdm_symbol_size + (fp->first_carrier_offset + crb) % fp->ofdm_symbol_size;
          const int xk = symbol * fp->ofdm_symbol_size + crb;
          const double yr = rx[ant][rk].r, yi = rx[ant][rk].i;
          const double xr = refs[p][xk].r, xi = refs[p][xk].i;
          ar += yr * xr + yi * xi;
          ai += yi * xr - yr * xi;
          er += yr * yr + yi * yi;
          ex += xr * xr + xi * xi;
        }
        if (er > 0 && ex > 0) {
          sum += fmin(1, sqrt((ar * ar + ai * ai) / (er * ex)));
          blocks++;
        }
        begin = end;
      }
      const int count = last - first;
      const double score = blocks ? sum / blocks * sqrt((double)count / blocks) / 0.886 : 0;
      total += score;
      used += count;
    }
  }
  free(map.re);
  if (n_used)
    *n_used = used;
  return n_ant && map.ports ? total / (n_ant * map.ports) : -1;
}

#include "nr_blind_stats.h"
#include "nr_csirs_disc_confirm_internal.h"

static void score_candidate(PHY_VARS_NR_UE *ue, int slot, uint32_t absolute_slot,
                            c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP], int idx,
                            bool maintenance, bool nzp_recheck);


/* nr_csirs_blind_rt_slot_fep: entry point called once per DL slot with the FEP output: hands the slot to the discovery queue when discovery is enabled, otherwise runs the search directly.
 *   Stores the slot's symbol map (g_disc_fep_map) for the search. Caller: phy_procedures_nr_ue.c (after the FEP of each DL slot); tests/nr_csirs_blind_rt_test_bridge.c.
 */
void nr_csirs_blind_rt_slot_fep(PHY_VARS_NR_UE *ue, int slot, uint32_t absolute_slot,
                               c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP], bool map[14])
{
  if (nr_csirs_disc_enabled && nr_csirs_disc_enabled() && discovery_enqueue(ue, slot, absolute_slot, rxdataF, map)) return;
  g_disc_fep_map = map;
  nr_csirs_blind_rt_slot(ue, slot, absolute_slot, rxdataF);
  g_disc_fep_map = NULL;
}

/* nr_csirs_blind_rt_slot: one slot of the CSI-RS blind search: arms the search state on first use, logs confirmations, runs the per-slot scoring rotation and the ZP maintenance.
 *   Scores the due zero-power candidates, then one ordinary candidate per slot (ISAC_CSIRS_BLIND_PER_SLOT), then the re-scoring of confirmed NZP occasions when reconfiguration detection is on.
 *   Optional diagnostics: ISAC_CSIRS_BLIND_RANK (ranking mode, which keeps scoring after a confirmation), ISAC_CSIRS_BLIND_EPRDUMP, ISAC_CSIRS_BLIND_IDSWEEP, ISAC_CSIRS_DISC_CONFIRM_MAX, ISAC_PDCCH_TIMING, ISAC_RECONF.
 *   Writes the shared state under g_csirs_state_lock (cleanup releases it). Enabled by ISAC_CSIRS_BLIND=1 or by discovery. Caller: nr_csirs_blind_rt_slot_fep().
 */
void nr_csirs_blind_rt_slot(PHY_VARS_NR_UE *ue, int slot, uint32_t absolute_slot,
                            c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP])
{
  if(discovery_queue_ready && !discovery_consuming) {
    bool map[14]={0};
    discovery_enqueue(ue,slot,absolute_slot,rxdataF,map);return;
  }
  pthread_mutex_t *guard __attribute__((cleanup(csirs_unlock))) = &g_csirs_state_lock;
  /* Production actors enqueue before this point. Only the discovery worker
   * (or the synchronous offline/legacy entry) owns scoring and mutable state. */
  uint64_t lock_start=g_disc_fep_map?now_us():0;
  pthread_mutex_lock(guard);
  if(discovery_consuming && discovery_consumed_epoch!=__atomic_load_n(&discovery_epoch,__ATOMIC_ACQUIRE)) return;
  if(g_disc_fep_map) {
    uint64_t wait=now_us()-lock_start;
    if(wait>__atomic_load_n(&csirs_disc_confirm_wait_max_us,__ATOMIC_RELAXED))
      __atomic_store_n(&csirs_disc_confirm_wait_max_us,wait,__ATOMIC_RELAXED);
  }
  const c16_t *rxdataF_ant0 = (rxdataF != NULL) ? &rxdataF[0][0] : NULL;
  if (g_on < 0) {
    const char *e = getenv("ISAC_CSIRS_BLIND");
    g_disc = nr_csirs_disc_enabled && nr_csirs_disc_enabled();
    g_on = (e != NULL && atoi(e) != 0) || g_disc;
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
    if (g_disc > 0) {
      discovery_empty_state(&g_st);
      g_st.discovery_bank=true;
      discovery_empty_state(&g_zp);
    } else {
      if (nr_csirs_blind_init(&g_st, fp->N_RB_DL, fp->Nid_cell) <= 0) {
        g_on = 0;
        return;
      }
      nr_csirs_blind_init(&g_zp, fp->N_RB_DL, fp->Nid_cell);
    }
    memset(g_zp_geometry_veto, 0, sizeof(g_zp_geometry_veto));
    g_zp_geometry_veto_total = 0;
    memset(g_zp_logged, 0, sizeof(g_zp_logged));
    memset(g_zp_events, 0, sizeof(g_zp_events));
    memset(g_zp_maint, 0, sizeof(g_zp_maint));
    g_armed = 1;
    LOG_I(PHY, "SENSING: CSIRS_BLIND armed: %d candidates, N_RB=%d scramb_id=%d%s\n",
          g_st.n, fp->N_RB_DL, fp->Nid_cell, g_disc > 0 ? " (discovery-fed)" : " (assumed = PCI)");
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
  for (int k = g_disc > 0 ? 0 : g_conf_logged; k < g_st.n_conf; ++k) {
    const int widx = g_st.conf_idx[k];
    if (g_disc > 0) {
      if (g_disc_export[widx].n_off == g_st.conf_n_off[k]
          && g_disc_export[widx].period == g_st.conf_period[k]
          && !memcmp(g_disc_export[widx].off, g_st.conf_off[k], sizeof(g_disc_export[widx].off))
          && !memcmp(&g_disc_export[widx].candidate, &g_st.cand[widx], sizeof(g_st.cand[widx]))) continue;
      if (g_disc_export[widx].n_off && nr_csirs_monitor_remove_dynamic) {
        fapi_nr_dl_config_csirs_pdu_rel15_t old;
        fill_pdu(&g_disc_export[widx].candidate, 1, &old);
        nr_csirs_monitor_remove_dynamic(&old);
      }
      if (g_disc_export[widx].n_off) {
        nr_csirs_resource_t updated = resource_key(&g_st, k, false), old = updated;
        old.start_rb = g_disc_export[widx].candidate.start_rb;
        old.nr_of_rbs = g_disc_export[widx].candidate.nr_of_rbs;
        old.period = g_disc_export[widx].period;
        old.offset = g_disc_export[widx].off[0];
        old.offset2 = g_disc_export[widx].off[1];
        old.n_offsets = g_disc_export[widx].n_off;
        nr_csirs_observer_refine(&g_csirs_obs, &old, &updated);
      }
      g_disc_export[widx].candidate = g_st.cand[widx];
      g_disc_export[widx].period = g_st.conf_period[k];
      memcpy(g_disc_export[widx].off, g_st.conf_off[k], sizeof(g_disc_export[widx].off));
      g_disc_export[widx].n_off = g_st.conf_n_off[k];
    }
    g_conf_logged = k + 1;
    const nr_csirs_resource_t key = resource_key(&g_st, k, false);
    if (!g_rank && nr_csirs_observer_confirm(&g_csirs_obs, &key, absolute_slot)) map_changed();
    if (!g_rank && g_disc > 0 && nr_csirs_monitor_add_dynamic) {
      fapi_nr_dl_config_csirs_pdu_rel15_t pdu;
      fill_pdu(&g_st.cand[widx], 1, &pdu);
      for (int phase = 0; phase < g_st.conf_n_off[k]; ++phase)
        nr_csirs_monitor_add_dynamic(&pdu,
                                     g_st.conf_period[k],
                                     g_st.conf_off[k][phase],
                                     g_st.cand[widx].slot_delta);
    }
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
    /* A sequence-confirmed NZP CSI-RS proves the grid's CRB indexing (Point A): verify the carrier when
     * SIB1 carried none (NSA), which arms the carrier-gated stages (e.g. the PDCCH nID snapshot sweep). */
    if (g_disc > 0 && !g_rank && nr_passive_acq_note_carrier_from_rs)
      nr_passive_acq_note_carrier_from_rs(line);
  }
  if (g_rank && g_st.n_conf > 0) {
    /* keep the population moving; see RANKING MODE above */
    g_st.n_conf = 0;
    g_st.confirmed = -1;
    g_conf_logged = 0;
  }
    /* ISAC_CSIRS_BLIND_EPRDUMP=<path> (diagnostic, rank mode only): every candidate's mean EPR,
     * observation count and best z, so a real CSI-RS can be told from PDSCH DM-RS / PDCCH energy
     * (added 2026-10-06 for the offline replay of PCI 415/421, where the top-5 lists were all DM-RS). */
  if (g_rank && (g_slots % 500) == 499) {
    const char *dump = getenv("ISAC_CSIRS_BLIND_EPRDUMP");
    if (dump) {
      FILE *f = fopen(dump, "w");
      if (f) {
        fprintf(f, "idx\trow\tfd\tl0\tl1\tcdm\tdensity\tepr_mean\tepr_n\tbest_z\thits\ttried\n");
        for (int i = 0; i < g_st.n && i < NR_CSIRS_BLIND_MAX_CAND; i++) {
          const nr_csirs_candidate_t *c = &g_st.cand[i];
          fprintf(f, "%d\t%u\t%u\t%u\t%u\t%u\t%u\t%.3f\t%u\t%.3f\t%u\t%u\n", i, c->row, c->freq_domain, c->symb_l0,
                  c->symb_l1, c->cdm_type, c->freq_density, g_epr_n[i] ? g_epr_sum[i] / g_epr_n[i] : 0.0, g_epr_n[i],
                  g_st.best_rho[i], g_st.hits[i], g_st.tried[i]);
        }
        fclose(f);
      }
    }
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
  int occurring[NR_CSIRS_BLIND_DISC_MAX_CONF];
  const int n = nr_csirs_blind_zp_due(&g_zp, absolute_slot, occurring, NR_CSIRS_BLIND_MAX_CONF);
  /* Preserve the T14 four-score admission rotation on the worker. */
  static int s_conf_max = -1;
  static unsigned s_conf_rot;
  if (s_conf_max < 0) {
    const char *e = getenv("ISAC_CSIRS_DISC_CONFIRM_MAX");
    s_conf_max = e && atoi(e) > 0 ? atoi(e) : 4;
  }
  int budget = g_disc > 0 ? s_conf_max : 1 << 30;
  const unsigned rot0 = g_disc > 0 ? s_conf_rot++ : 0;
  for (int kk = 0; kk < n && budget > 0; kk++) {
    const int k = (int)((rot0 + kk) % (unsigned)n);
    g_zp_maint[occurring[k]].scheduled++;
    score_candidate(ue, slot, absolute_slot, rxdataF, occurring[k], true, false);
    --budget;
  }
  /* Alternate independent discovery rotations. A geometry retired by NZP must
   * remain eligible for ZP re-admission after pressure eviction. Still at most
   * one ordinary score, deduplicated against the bounded maintenance snapshot. */
  /* ISAC_CSIRS_BLIND_PER_SLOT=N (default 1): ordinary discovery scores per slot. N > 1 is for slow
   * offline replay of a raw capture, where the real-time budget does not apply: the default one score
   * per slot gave each of ~767 candidates only 10-16 observations in 240 s OTA (2026-10-06, PCI 421),
   * below the IDSWEEP pin floor of 32, so the scramblingID sweep never started. */
  if (g_disc > 0) {
    /* Bound work to due canonical resources. Balance visits PER PHASE and
     * reverse tie order on alternate rounds; a slot-index rotation or a fixed
     * candidate order can sample only P, 2P or 4P aliases forever. Settled
     * confirmations leave this budget until admission adds a new lattice. */
    while (budget > 0) {
      int selected = -1, phase = 0;
      uint32_t least = UINT32_MAX;
      for (int i = 0; i < g_st.n; ++i) {
        uint16_t p = g_st.seed_period[i];
        if (!p || g_disc_scored_slot[i] == absolute_slot) continue;
        int ph = absolute_slot % p == g_st.seed_offset[i] ? 0
            : g_st.seed_n_off[i] == 2 && absolute_slot % p == g_st.seed_offset2[i] ? 1 : -1;
        bool unsettled = true;
        for (int k = 0; k < g_st.n_conf; ++k)
          if (g_st.conf_idx[k] == i) {
            /* A fade during the first three probes must not strand a late
             * burst phase, or keep a super-period after a denser admission. */
            unsettled = g_st.conf_n_off[k] < g_st.seed_n_off[i] || g_st.conf_period[k] > p;
            break;
          }
        if (ph >= 0 && (unsettled || g_disc_probe[i][ph])
            && (g_disc_visits[i][ph] < least
                || (g_disc_visits[i][ph] == least && (least & 1)))) {
          selected = i; phase = ph; least = g_disc_visits[i][ph];
        }
      }
      if (selected < 0) break;
      g_disc_scored_slot[selected] = absolute_slot;
      ++g_disc_visits[selected][phase];
      if (g_disc_probe[selected][phase]) --g_disc_probe[selected][phase];
      score_candidate(ue, slot, absolute_slot, rxdataF, selected, false, false);
      --budget;
    }
  }
  static int s_per_slot = -1;
  if (s_per_slot < 0) {
    const char *e = getenv("ISAC_CSIRS_BLIND_PER_SLOT");
    s_per_slot = e ? atoi(e) : 1;
    if (s_per_slot < 1)
      s_per_slot = 1;
    if (s_per_slot > 2 * NR_CSIRS_BLIND_MAX_CAND)
      s_per_slot = 2 * NR_CSIRS_BLIND_MAX_CAND;
    if (s_per_slot > 1)
      LOG_W(PHY, "SENSING: CSIRS_BLIND scoring %d discovery candidates per slot (offline/replay setting)\n", s_per_slot);
  }
  int idx = -1;
  for (int r = 0; r < (g_disc > 0 ? 0 : s_per_slot); r++) {
    nr_csirs_blind_state_t *first = ((g_slots + r) & 1) ? &g_st : &g_zp;
    nr_csirs_blind_state_t *other = (first == &g_st) ? &g_zp : &g_st;
    nr_csirs_blind_state_t *bank = first;
    idx = nr_csirs_blind_next(bank);
    if (idx < 0) {
      bank = other;
      idx = nr_csirs_blind_next(bank);
    }
    if (idx < 0)
      break;
    bool measured = false;
    for (int k = 0; k < n; k++)
      if (occurring[k] == idx)
        measured = true;
    /* both banks index the same candidate population (score_candidate updates both for idx); a
     * pinned candidate comes back every call, so score each idx at most once per slot */
    static uint32_t s_seen_slot[NR_CSIRS_BLIND_MAX_CAND];
    if (s_per_slot > 1 && idx < NR_CSIRS_BLIND_MAX_CAND) {
      if (s_seen_slot[idx] == absolute_slot + 1)
        continue;
      s_seen_slot[idx] = absolute_slot + 1;
    }
    if (!measured)
      score_candidate(ue, slot, absolute_slot, rxdataF, idx, false, false);
  }
  /* Re-score confirmed NZP occasions only with reconfiguration detection enabled.
   * The search bank remains intact; four scored misses produce one SOFT signal. */
  if (g_reconf && nr_cfg_epoch_note_csirs_map_change) {
    int due[NR_CSIRS_BLIND_DISC_MAX_CONF];
    const int nn = nr_csirs_blind_occurring(&g_st, absolute_slot, due, NR_CSIRS_BLIND_DISC_MAX_CONF);
    for (int k = 0; k < nn; ++k)
      if (due[k] != idx) score_candidate(ue, slot, absolute_slot, rxdataF, due[k], false, true);
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

/* score_candidate: scores one CSI-RS candidate on the current slot: builds the reference signal, correlates it with the received symbol, and feeds the scale-free z score to the search.
 *   Also computes the zero-power (ZP) occupancy score, the sequence-free energy ratio, and the footprint match for rows 6-18 (ISAC_CSIRS_BLIND_WIDE).
 *   Optional sweeps: scramblingID (ISAC_CSIRS_BLIND_IDSWEEP) and slot index (ISAC_CSIRS_BLIND_SLOTSWEEP) on a candidate whose positions already look like a pilot.
 *   maintenance = re-score of an exported resource (no new evidence); nzp_recheck = re-score of a confirmed NZP occasion. Callers: nr_csirs_blind_rt_slot() only.
 */
static void score_candidate(PHY_VARS_NR_UE *ue, int slot, uint32_t absolute_slot,
                            c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP], int idx,
                            bool maintenance, bool nzp_recheck)
{
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const c16_t *rxdataF_ant0 = &rxdataF[0][0];
  if (idx < 0) {
    return;
  }
  const nr_csirs_candidate_t *c = &g_st.cand[idx];
  if (discovery_consuming && discovery_consumed_ticket<discovery_birth[idx]) return;
  if (discovery_consuming && (discovery_symbols(c) & discovery_copied_symbols) != discovery_symbols(c)) return;
  if (!maintenance && !nzp_recheck) {
    const nr_csirs_resource_t candidate = {.row = c->row, .ports = nr_csirs_blind_row_ports(c->row),
        .density = c->freq_density, .freq_domain = c->freq_domain, .symb_l0 = c->symb_l0,
        .start_rb = c->start_rb, .nr_of_rbs = c->nr_of_rbs};
    nr_csirs_observer_candidate(&g_csirs_obs, &candidate, absolute_slot);
  }
  const uint64_t confirm_start_us = (maintenance || nzp_recheck) && g_timing ? now_us() : 0;
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

  int n_used=0;
  double rho=-1, gpu_scores[5];
  bool gpu_scored=g_disc>0 && discovery_gpu_score(fp,c,slot,&rxdataF[0][0],gpu_scores)==0;
  if(discovery_gpu && discovery_gpu->prepare && !gpu_scored) return;
  c16_t *ref=NULL;
  int n_planes=0;
  const uint32_t off_sym=(uint32_t)c->symb_l0*fp->ofdm_symbol_size;
  if(gpu_scored) {rho=gpu_scores[0];goto have_rho;}
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
  const int plane_limit = g_disc > 0 ? NR_CSIRS_BLIND_RT_MAX_PORTS : NR_CSIRS_BLIND_RT_LEGACY_PORTS;
  n_planes = n_ports < plane_limit ? n_ports : plane_limit;

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
    for (int p = 0; p < plane_limit; p++) {
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
  ref = t_refbuf[0];
  c16_t *refp[NR_CSIRS_BLIND_RT_ALL_PORTS];
  for (int p = 0; p < NR_CSIRS_BLIND_RT_ALL_PORTS; p++)
    refp[p] = t_refbuf[p < n_planes ? p : n_planes - 1];
  const csi_mapping_parms_t parms = get_csi_mapping_parms(c->row, c->freq_domain, c->symb_l0,
                                                          c->symb_l1);
  /* Under the same candidate lock as admission/scoring: no candidate can
   * appear between selecting its symbols and consuming the completed grid. */
  if (g_disc > 0 && g_disc_fep_map) {
    for (int j=0;j<parms.size;++j) for (int lp=0;lp<=parms.lprime;++lp) {
      int l = parms.loverline[j]+lp;
      if (l >= 0 && l < 14 && !g_disc_fep_map[l]) {
        nr_slot_fep(ue, fp, slot, l, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
        g_disc_fep_map[l] = true;
      }
    }
  }
  int reference_slot = (slot + c->slot_delta) % fp->slots_per_frame;
  if (reference_slot < 0)
    reference_slot += fp->slots_per_frame;
  nr_generate_csi_rs(fp, &parms, AMP, reference_slot, c->freq_density, c->start_rb, c->nr_of_rbs,
                     c->symb_l0, c->symb_l1, c->row, c->scramb_id, 0 /* power offset */,
                     c->cdm_type, refp);

  /* Score only the symbol the candidate places its resource on: correlating the whole slot would
   * dilute the oracle with 13 symbols of unrelated PDSCH. The monitor only FFT'd the CORESET
   * symbols, so transform this candidate's symbol now (antenna 0) -- without this the search
   * scored an empty buffer and never fired (OTA 2026-09-14: 15 min, zero progress lines). */
  if (g_disc <= 0)
    nr_slot_fep_ant(ue, fp, (unsigned)slot, (unsigned)c->symb_l0, 0, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
  /* Footprint evidence for rows 6-18 from the symbol just transformed: one pass over the carrier. */
  if (g_wide < 0) {
    const char *e = getenv("ISAC_CSIRS_BLIND_WIDE");
    g_wide = (e != NULL && atoi(e) != 0) ? 1 : 0;
  }
  if (g_wide && !maintenance && !nzp_recheck) {
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
  double rho_sq = 0.0;
  const int n_ant = g_disc > 0 ? fp->nb_antennas_rx : 1;
  for (int ant = 0; g_disc <= 0 && ant < n_ant; ++ant) {
    double best_port = -1.0;
    int best_used = 0;
    const int score_planes = g_disc > 0 ? n_planes : 1;
    for (int p = 0; p < score_planes; ++p) {
      int ant_used = 0;
      const double ant_rho = nr_csirs_blind_correlate_blocks_shift(
          (const int16_t *)&rxdataF[ant][off_sym],
          (const int16_t *)&t_refbuf[p][off_sym],
          fp->ofdm_symbol_size,
          g_disc > 0 ? 40 : CSIRS_BLIND_SUBBAND_RE,
          fp->first_carrier_offset,
          &ant_used);
      if (ant_rho > best_port) {
        best_port = ant_rho;
        best_used = ant_used;
      }
    }
    if (best_port >= 0.0) {
      rho_sq += best_port * best_port;
      n_used = best_used;
    }
  }
  const c16_t *rx_planes[NR_CSIRS_DISC_MAX_ANT];
  for (int ant = 0; ant < n_ant; ++ant)
    rx_planes[ant] = rxdataF[ant];
  rho = g_disc > 0
      ? discovery_score(fp, c, rx_planes, t_refbuf, n_ant, CSIRS_BLIND_SUBBAND_RE, &n_used)
      : n_ant > 0 ? sqrt(rho_sq / n_ant) : -1.0;
have_rho:
  if(g_disc>0) __atomic_fetch_add(gpu_scored?&csirs_disc_confirm_gpu_scores:&csirs_disc_confirm_cpu_scores,1,__ATOMIC_RELAXED);
  if (nzp_observer_score(idx, absolute_slot, rho)) map_changed();
  if ((maintenance || nzp_recheck) && g_timing)
    nr_csirs_observer_add_time(&g_csirs_obs, NR_CSIRS_TIME_CONFIRM, now_us() - confirm_start_us);
  if (nzp_recheck) return;
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
  const double z = rho; /* already normalised: ~1.0 noise, ~6.4 perfect, at any candidate size */
  /* Sequence-free positional evidence, logged next to z: if z stays at noise while this shows
   * periodic structure, the POSITIONS are right and the SEQUENCE (scramblingID != PCI) is wrong --
   * which the correlation alone cannot distinguish from an empty hypothesis. */
  epr = gpu_scored ? gpu_scores[1] : nr_csirs_blind_energy_ratio_shift((const int16_t *)&rxdataF_ant0[off_sym],
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
  if (!gpu_scored && !maintenance && g_rank && z > 3.0 && g_slots - s_span_last >= 200) {
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
  /* Legacy confirmation retains z >= 4. A discovery-qualified row-1 burst
   * has two independently swept adjacent phases: require three fresh z >= 3
   * hits on EACH phase. This permits weak TRS without admitting single-phase
   * coherent distractors (the dump's row-4 ID 418 remains below z=4). */
  const double nullv = g_disc > 0 && c->row == 1 && g_st.seed_n_off[idx] == 2 ? 1.0 : 4.0 / 3.0;   /* feed() applies its existing 3x margin */
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
    for (int p = 0; !gpu_scored && p < n_planes; p++)
      zp_refs[p] = (const int16_t *)&t_refbuf[p][off_sym];
    double old_score = gpu_scored ? gpu_scores[4] : -1.0;
    const double zs = gpu_scored ? gpu_scores[2] : nr_csirs_blind_zero_score_evidence_shift((const int16_t *)&rxdataF_ant0[off_sym], zp_refs, n_planes,
                                                            fp->ofdm_symbol_size, fp->first_carrier_offset,
                                                            c->row == 2 ? &old_score : NULL);
    double zs_other = gpu_scored ? gpu_scores[3] : zs;
    if (!gpu_scored && c->row == 5) {
      const unsigned symbol = c->symb_l0 + 1;
      const uint32_t off_other = symbol * fp->ofdm_symbol_size;
      zs_other = -1.0;
      if (g_disc > 0 || nr_slot_fep_ant(ue, fp, (unsigned)slot, symbol, 0, rxdataF, link_type_dl, 0, ue->common_vars.rxdata) == 0) {
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
      nr_csirs_resource_t old_zp_key = {0};
      bool had_zp_key = false;
      for (int k = 0; k < g_zp.n_conf; ++k)
        if (g_zp.conf_idx[k] == idx) {
          old_zp_key = resource_key(&g_zp, k, true);
          had_zp_key = true;
          break;
        }
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
      if (g_zp.zp_revocations[idx] != revoked && had_zp_key)
        nr_csirs_observer_revoke_zp(&g_csirs_obs, &old_zp_key);
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
    g_ids = g_disc <= 0 && (e != NULL && atoi(e) != 0);
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
  if (g_disc<=0 && g_slotsweep && idx == g_id_pin && epr > 1.5 && g_slot_best_off < 0) {
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
/* nr_csirs_blind_rt_observed_nzp: returns, in *out, one confirmed NZP CSI-RS resource occurring on this slot (for the observer and the rate matcher), if any.
 *   Discovery mode reads the exported set; otherwise it uses the search state under a trylock (no wait). Ranking mode (g_rank) reports nothing.
 *   Caller: nr_pdcch_blind_monitor_rt.c (observed NZP occasion of a slot).
 */
bool nr_csirs_blind_rt_observed_nzp(uint32_t absolute_slot, fapi_nr_dl_config_csirs_pdu_rel15_t *out)
{
  if(discovery_queue_ready) {
    if(!out) return false;
    fapi_nr_dl_config_csirs_pdu_rel15_t all[2*NR_CSIRS_BLIND_DISC_MAX_CONF];
    int n=discovery_export(absolute_slot,all,2*NR_CSIRS_BLIND_DISC_MAX_CONF);
    for(int i=0;i<n;++i) if(all[i].csi_type==1) {*out=all[i];return true;}
    return false;
  }
  if (!out || pthread_mutex_trylock(&g_csirs_state_lock) != 0) return false;
  bool found = false;
  if (g_on > 0 && g_armed != 0 && g_rank <= 0) {
    int idx[NR_CSIRS_BLIND_DISC_MAX_CONF];
    const int n = nr_csirs_blind_occurring(&g_st, absolute_slot, idx, NR_CSIRS_BLIND_DISC_MAX_CONF);
    if (n > 0) {
      fill_pdu(&g_st.cand[idx[0]], 1 /* confirmed NZP */, out);
      found = true;
    }
  }
  pthread_mutex_unlock(&g_csirs_state_lock);
  return found;
}
static int rate_match_all_n(uint32_t absolute_slot, fapi_nr_dl_config_csirs_pdu_rel15_t *out, int max);
/* nr_csirs_blind_rt_rate_match_all: fills out[] with every CSI-RS resource that occurs on this slot for PDSCH rate matching: confirmed NZP (csi_type 1) first, then retained ZP (csi_type 2).
 *   Returns the count (at most max). Returns 0 when the search is off, not armed, or in ranking mode, so that the diagnostic never changes PDSCH.
 *   Callers: nr_pdsch_passive_decode.c (CSI-RS rate matching in passive_decode_retry and the CSI population retry).
 */
int nr_csirs_blind_rt_rate_match_all(uint32_t absolute_slot, fapi_nr_dl_config_csirs_pdu_rel15_t *out, int max)
{
  /* CSI-FULL: NFAPI_MAX_NUM_CSI_RATEMATCH grew 4 -> 32; the existing callers pass NR_CSI_RM_LEGACY_MAX (4) so they
   * keep exactly the old first-4 view (the PDSCH solver also harvests scrambling-ID candidates from it). The
   * per-population set is nr_csirs_full_rate_match(), applied at decode time only. */
  return rate_match_all_n(absolute_slot, out, max);
}
static int rate_match_all_n(uint32_t absolute_slot, fapi_nr_dl_config_csirs_pdu_rel15_t *out, int max)
{
  if(discovery_queue_ready && !discovery_consuming) {
    int n=discovery_export(absolute_slot,out,max);
    for(int i=0;i<n;++i) if(out[i].csi_type==2) nr_csirs_observer_export_zp(&g_csirs_obs);
    return n;
  }
  pthread_mutex_t *guard __attribute__((cleanup(csirs_unlock))) = &g_csirs_state_lock;
  pthread_mutex_lock(guard);
  /* Ranking is diagnostic only: neither newly confirmed NZP nor retained ZP may alter PDSCH. */
  if (out == NULL || max <= 0 || g_on <= 0 || g_armed == 0 || g_rank > 0)
    return 0;
  int n = 0, idx[NR_CSIRS_BLIND_DISC_MAX_CONF];
  const int nn = nr_csirs_blind_occurring(&g_st, absolute_slot, idx, NR_CSIRS_BLIND_DISC_MAX_CONF);
  for (int i = 0; i < nn && n < max; i++)
    fill_pdu(&g_st.cand[idx[i]], 1 /* NZP */, &out[n++]);
  const int nz = nr_csirs_blind_occurring(&g_zp, absolute_slot, idx, NR_CSIRS_BLIND_DISC_MAX_CONF);
  for (int i = 0; i < nz && n < max; i++)
  {
    fill_pdu(&g_zp.cand[idx[i]], 2 /* ZP: rate matching only, no estimation */, &out[n++]);
    nr_csirs_observer_export_zp(&g_csirs_obs);
  }
  return n;
}

/* CSI-FULL (ISAC_CSIRS_FULL=1, default off). The complete CSI-RS set a grant's cell transmits in the PDSCH slot,
 * for the PDSCH rate matching of TS 38.214 5.1.4.1. Measured ground truth (tools/csirs_capture_map.py on both
 * PCI-415 captures, docs/superpowers/reports/csd/CSIFULL.md): six TRS scrambling IDs on one carrier, one per cell,
 * each a 52-RB row-1 density-3 TRS pair in its own RB third; a grant of DM-RS/data scrambling ID X decodes only
 * when the PDSCH skips cell X's resources and NOT the other cells' (their REs carry cell X's data). So the subset
 * is "scramblingID == the grant's DM-RS scrambling ID" (NZP and the ZP geometries the search attached to that ID),
 * deduplicated by RE geometry, without resources that would land on the grant's DM-RS symbols (the standard
 * forbids the overlap; nr_dlsch_extract_rbs() asserts on it). The CSI-RS-to-cell association by scramblingID is a
 * measured property of this network, not a TS requirement, hence the flag. RB extent: nr_dlsch_csi_rb_extent()
 * is on with this flag (TS 38.211 7.4.1.5.3). */
extern void nr_dlsch_csi_erase_set(const fapi_nr_dl_config_csirs_pdu_rel15_t *list, int n) __attribute__((weak));
static __thread int t_full_ab_depth; /* CSIFULLAB recursion guard */
static bool csirs_full_same_geometry(const fapi_nr_dl_config_csirs_pdu_rel15_t *a, const fapi_nr_dl_config_csirs_pdu_rel15_t *b)
{
  return a->row == b->row && a->freq_domain == b->freq_domain && a->symb_l0 == b->symb_l0 && a->symb_l1 == b->symb_l1
         && a->cdm_type == b->cdm_type && a->freq_density == b->freq_density && a->start_rb == b->start_rb
         && a->nr_of_rbs == b->nr_of_rbs;
}
static bool csirs_full_dmrs_clash(const fapi_nr_dl_config_csirs_pdu_rel15_t *c, uint16_t dmrs_mask)
{
  static const uint8_t num_l0[18] = {1, 1, 1, 1, 2, 1, 2, 2, 1, 2, 2, 2, 2, 2, 4, 2, 2, 4};
  if (c->row < 1 || c->row > 18)
    return true;
  for (int k = 0; k < num_l0[c->row - 1]; k++)
    if ((dmrs_mask >> (c->symb_l0 + k)) & 1)
      return true;
  if (c->row == 13 || c->row == 14 || c->row == 16 || c->row == 17)
    for (int k = 0; k < 2; k++)
      if ((dmrs_mask >> (c->symb_l1 + k)) & 1)
        return true;
  return false;
}
/* Every confirmed resource (any slot), NZP then ZP: the population's TRS RB blocks. */
static int full_all_confirmed(fapi_nr_dl_config_csirs_pdu_rel15_t *out, int max)
{
  int n = 0;
  if (discovery_queue_ready && !discovery_consuming) {
    if (discovery_disabled)
      return 0;
    unsigned b;
    for (;;) {
      b = __atomic_load_n(&discovery_active, __ATOMIC_ACQUIRE);
      unsigned users = __atomic_load_n(&discovery_exports[b].users, __ATOMIC_RELAXED);
      if (users == UINT32_MAX) continue;
      if (!__atomic_compare_exchange_n(&discovery_exports[b].users, &users, users + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) continue;
      if (b == __atomic_load_n(&discovery_active, __ATOMIC_ACQUIRE)) break;
      __atomic_fetch_sub(&discovery_exports[b].users, 1, __ATOMIC_RELEASE);
    }
    for (int i = 0; i < discovery_exports[b].n && n < max; ++i)
      out[n++] = discovery_exports[b].resource[i].p;
    __atomic_fetch_sub(&discovery_exports[b].users, 1, __ATOMIC_RELEASE);
    return n;
  }
  pthread_mutex_lock(&g_csirs_state_lock);
  if (g_on > 0 && g_armed != 0 && g_rank <= 0)
    for (int bank = 0; bank < 2; ++bank) {
      nr_csirs_blind_state_t *st = bank ? &g_zp : &g_st;
      for (int k = 0; k < st->n_conf && n < max; ++k)
        fill_pdu(&st->cand[st->conf_idx[k]], bank ? 2 : 1, &out[n++]);
    }
  pthread_mutex_unlock(&g_csirs_state_lock);
  return n;
}
static bool full_rb_overlap_half(const fapi_nr_dl_config_csirs_pdu_rel15_t *a, const fapi_nr_dl_config_csirs_pdu_rel15_t *b)
{
  const int lo = a->start_rb > b->start_rb ? a->start_rb : b->start_rb;
  const int hi = a->start_rb + a->nr_of_rbs < b->start_rb + b->nr_of_rbs ? a->start_rb + a->nr_of_rbs : b->start_rb + b->nr_of_rbs;
  const int m = a->nr_of_rbs < b->nr_of_rbs ? a->nr_of_rbs : b->nr_of_rbs;
  return hi - lo > 0 && 2 * (hi - lo) >= m;
}
/* ISAC_CSIRS_FULL_TRS_PAIR=1 (default off). TS 38.214 5.1.6.1.1: a TRS (NZP-CSI-RS-ResourceSet with trs-Info) is
 * row 1, density 3, and its resources come in pairs in one slot at symbols l and l+4 (l in {4,5,6}) with one
 * frequency allocation, period and slot. Discovery confirms the members independently, so in a short run one
 * member of a pair, or one of its two slot phases, is often missing. Occurrence of either member in this slot
 * implies both. */
static int full_trs_pair_enabled(void)
{
  static int s = -1;
  if (s < 0) s = getenv("ISAC_CSIRS_FULL_TRS_PAIR") && atoi(getenv("ISAC_CSIRS_FULL_TRS_PAIR")) ? 1 : 0;
  return s;
}
static bool full_trs_partner(const fapi_nr_dl_config_csirs_pdu_rel15_t *c, fapi_nr_dl_config_csirs_pdu_rel15_t *p)
{
  if (c->row != 1 || c->freq_density != 3 || c->csi_type == 2)
    return false;
  *p = *c;
  if (c->symb_l0 >= 4 && c->symb_l0 <= 6) p->symb_l0 = c->symb_l0 + 4;
  else if (c->symb_l0 >= 8 && c->symb_l0 <= 10) p->symb_l0 = c->symb_l0 - 4;
  else return false;
  return true;
}
static int full_add_trs_pairs(const fapi_nr_dl_config_csirs_pdu_rel15_t *conf, int n_conf, fapi_nr_dl_config_csirs_pdu_rel15_t *all,
                              int n_all, int max)
{
  for (int j = 0; j < n_conf; j++) {
    fapi_nr_dl_config_csirs_pdu_rel15_t p;
    if (!full_trs_partner(&conf[j], &p))
      continue;
    bool c_in = false, p_in = false;
    for (int i = 0; i < n_all; i++) {
      c_in |= csirs_full_same_geometry(&all[i], &conf[j]) && all[i].scramb_id == conf[j].scramb_id;
      p_in |= csirs_full_same_geometry(&all[i], &p) && all[i].scramb_id == p.scramb_id;
    }
    if (c_in && !p_in && n_all < max) all[n_all++] = p;
    if (p_in && !c_in && n_all < max) all[n_all++] = conf[j];
  }
  return n_all;
}
/* ISAC_CSIRS_FULL_BLOCK_INFER=1 (default off; a measured network property, not a TS rule). On the PCI-415 carrier
 * every TRS RB block carries all its row-1 TRS resources (both cells' pairs) in both bursts of the period
 * (tools/csirs_capture_map.py, both captures). Discovery confirms each (resource, burst) separately and a 12 s
 * run misses many. When any row-1 density-3 resource of a block occurs in this slot, every confirmed row-1
 * density-3 geometry of that block is taken to occur too. */
/* BLINDFIX F1: ISAC_CSIRS_FULL_BLOCK_INFER=2 infers a block member c into this slot only from a resource a that occurs in it
 * AND was measured co-scheduled with c in >= 2 slots of the 640-slot window (a network that never co-schedules its TRS
 * resources, or whose co-occurrence discovery has not seen, gets no inference: only measured resources rate-match).
 * =1 keeps the unconditional PCI-415 rule. */
static int full_block_infer_enabled(void)
{
  static int s = -1;
  if (s < 0) s = getenv("ISAC_CSIRS_FULL_BLOCK_INFER") ? atoi(getenv("ISAC_CSIRS_FULL_BLOCK_INFER")) : 0;
  return s;
}
static bool full_rb_overlap_half(const fapi_nr_dl_config_csirs_pdu_rel15_t *a, const fapi_nr_dl_config_csirs_pdu_rel15_t *b);
static int full_add_block(const fapi_nr_dl_config_csirs_pdu_rel15_t *conf, int n_conf, fapi_nr_dl_config_csirs_pdu_rel15_t *all,
                          int n_all, int max)
{
  const int n0 = n_all;
  for (int j = 0; j < n_conf && n_all < max; j++) {
    const fapi_nr_dl_config_csirs_pdu_rel15_t *c = &conf[j];
    if (c->row != 1 || c->freq_density != 3 || c->csi_type == 2)
      continue;
    bool present = false, block = false;
    for (int i = 0; i < n_all && !present; i++)
      present = csirs_full_same_geometry(&all[i], c) && all[i].scramb_id == c->scramb_id;
    for (int i = 0; i < n0 && !block; i++) {
      block = all[i].row == 1 && all[i].freq_density == 3 && all[i].csi_type != 2 && full_rb_overlap_half(&all[i], c);
      if (block && full_block_infer_enabled() == 2 && discovery_cooccur(&all[i], c) < 2)
        block = false;
    }
    if (!present && block)
      all[n_all++] = *c;
  }
  return n_all;
}
/* The population's rate-matching set in this slot: its own resources (scramblingID == population) AND every other
 * resource in the RB block of one of its own confirmed resources (any slot). Measured with CSIFULLAB on the PCI-415
 * capture: grant 0x37f3 512.16 (176 RB from RB 0) passes CRC only with own 415 + co-block 748 removed, not with
 * own only, not with nothing, not with every resource; population-249 grants fail when the 415/748 block (not
 * theirs) is removed. I.e. each cell rate-matches its own TRS and the co-channel neighbour's TRS in its RB block
 * (ZP protection), and transmits data on the other blocks' TRS REs. Everything else in the slot -> erase list. */
static void full_select(uint32_t slot, uint16_t pop, uint16_t dmrs_mask, fapi_nr_dl_config_csirs_pdu_rel15_t *sel, int *nsel,
                        int max_sel, fapi_nr_dl_config_csirs_pdu_rel15_t *er, int *ner, int max_er)
{
  fapi_nr_dl_config_csirs_pdu_rel15_t conf[2 * NR_CSIRS_BLIND_DISC_MAX_CONF], all[2 * NR_CSIRS_BLIND_DISC_MAX_CONF];
  const int n_conf = full_all_confirmed(conf, 2 * NR_CSIRS_BLIND_DISC_MAX_CONF);
  int n_all = rate_match_all_n(slot, all, 2 * NR_CSIRS_BLIND_DISC_MAX_CONF);
  if (full_trs_pair_enabled())
    n_all = full_add_trs_pairs(conf, n_conf, all, n_all, 2 * NR_CSIRS_BLIND_DISC_MAX_CONF);
  if (full_block_infer_enabled())
    n_all = full_add_block(conf, n_conf, all, n_all, 2 * NR_CSIRS_BLIND_DISC_MAX_CONF);
  *nsel = 0;
  if (ner) *ner = 0;
  for (int i = 0; i < n_all; i++) {
    if (csirs_full_dmrs_clash(&all[i], dmrs_mask))
      continue;
    bool in = all[i].scramb_id == pop;
    for (int j = 0; j < n_conf && !in; j++)
      in = conf[j].scramb_id == pop && conf[j].csi_type != 2 && full_rb_overlap_half(&conf[j], &all[i]);
    fapi_nr_dl_config_csirs_pdu_rel15_t *list = in ? sel : er;
    int *nl = in ? nsel : ner;
    const int mx = in ? max_sel : max_er;
    if (!list || !nl || (!in && all[i].csi_type == 2))
      continue;
    bool dup = false;
    for (int j = 0; j < *nl && !dup; j++)
      dup = csirs_full_same_geometry(&list[j], &all[i]);
    if (!dup && *nl < mx)
      list[(*nl)++] = all[i];
  }
}
int nr_csirs_full_rate_match(uint32_t pdsch_absolute_slot, uint16_t population_id, uint16_t dmrs_mask,
                             fapi_nr_dl_config_csirs_pdu_rel15_t *out, int max)
{
  int n = 0;
  full_select(pdsch_absolute_slot, population_id, dmrs_mask, out, &n, max, NULL, NULL, 0);
  return n;
}
void nr_csirs_full_apply(fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg, uint32_t pdsch_absolute_slot)
{
  if (!nr_csirs_full_enabled() || !cfg || t_full_ab_depth > 0) /* inside a CSIFULLAB hypothesis: keep its list */
    return;
  /* ISAC_CSIRS_FULL_ERASE=1 (default off): the resources NOT selected for this population (other RB blocks' TRS)
   * become LLR erasures in nr_dlsch_extract_rbs() (their REs carry this grant's data under a foreign TRS). */
  static int s_erase = -1;
  if (s_erase < 0)
    s_erase = getenv("ISAC_CSIRS_FULL_ERASE") && atoi(getenv("ISAC_CSIRS_FULL_ERASE")) ? 1 : 0;
  fapi_nr_dl_config_csirs_pdu_rel15_t erase[NFAPI_MAX_NUM_CSI_RATEMATCH];
  int n_sel = 0, n_erase = 0;
  full_select(pdsch_absolute_slot, cfg->dlDmrsScramblingId, cfg->dlDmrsSymbPos, cfg->csiRsForRateMatching, &n_sel,
              NFAPI_MAX_NUM_CSI_RATEMATCH, s_erase ? erase : NULL, s_erase ? &n_erase : NULL, NFAPI_MAX_NUM_CSI_RATEMATCH);
  cfg->numCsiRsForRateMatching = (uint8_t)n_sel;
  if (nr_dlsch_csi_erase_set) /* weak: fixtures link this file without the demodulator */
    nr_dlsch_csi_erase_set(erase, n_erase);
}

/* ISAC_CSIRS_FULL_AB=1 (diagnostic, default off; needs ISAC_CSIRS_FULL=1). Every eligible dedicated full decode in a
 * slot carrying discovered CSI-RS is first decoded under a fixed set of rate-matching hypotheses on the SAME
 * samples, and one CSIFULLAB line reports each hypothesis' CRC (1 = CRC on the hypothesis itself, r = only a
 * DECODE-FIX recovery arm passed, 0 = fail): none / own population / own + every resource sharing an own
 * resource's RB block (= the ISAC_CSIRS_FULL rule) / every resource in the slot / own with extents rounded out to 16 RBs; each with the
 * grant's S and, when legal (S<=1, end 14), with S toggled. With ISAC_CSIRS_FULL_ERASE=1 every resource NOT in the
 * hypothesis is erased. The caller then decodes normally; the AB results are never fed back. */
extern nr_pdsch_passive_decode_status_t nr_pdsch_passive_decode(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc,
                                                                fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                                                const freq_alloc_bitmap_t *freq_alloc,
                                                                const nr_pdsch_passive_grant_t *grant,
                                                                c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                                                nr_pdsch_passive_decode_result_t *out) __attribute__((weak));
void nr_csirs_full_ab(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, const fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg,
                      const freq_alloc_bitmap_t *fa, const nr_pdsch_passive_grant_t *grant,
                      c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP])
{
  static int s_ab = -1;
  if (s_ab < 0)
    s_ab = getenv("ISAC_CSIRS_FULL_AB") && atoi(getenv("ISAC_CSIRS_FULL_AB")) ? 1 : 0;
  if (!s_ab || !nr_csirs_full_enabled() || t_full_ab_depth > 0 || !nr_pdsch_passive_decode || !cfg)
    return;
  const uint32_t slot = (uint32_t)(proc->frame_rx * ue->frame_parms.slots_per_frame + proc->nr_slot_rx);
  fapi_nr_dl_config_csirs_pdu_rel15_t all[2 * NR_CSIRS_BLIND_DISC_MAX_CONF];
  int n_all = rate_match_all_n(slot, all, 2 * NR_CSIRS_BLIND_DISC_MAX_CONF);
  fapi_nr_dl_config_csirs_pdu_rel15_t conf[2 * NR_CSIRS_BLIND_DISC_MAX_CONF];
  const int n_conf = full_all_confirmed(conf, 2 * NR_CSIRS_BLIND_DISC_MAX_CONF);
  if (full_trs_pair_enabled())
    n_all = full_add_trs_pairs(conf, n_conf, all, n_all, 2 * NR_CSIRS_BLIND_DISC_MAX_CONF);
  if (full_block_infer_enabled())
    n_all = full_add_block(conf, n_conf, all, n_all, 2 * NR_CSIRS_BLIND_DISC_MAX_CONF);
  if (n_all <= 0)
    return;
  static int s_erase = -1;
  if (s_erase < 0)
    s_erase = getenv("ISAC_CSIRS_FULL_ERASE") && atoi(getenv("ISAC_CSIRS_FULL_ERASE")) ? 1 : 0;
  const uint16_t pop = cfg->dlDmrsScramblingId;
  bool in_h[6][2 * NR_CSIRS_BLIND_DISC_MAX_CONF];
  memset(in_h, 0, sizeof(in_h));
  int n_own = 0, n_range = 0;
  for (int i = 0; i < n_all; i++) {
    const bool own = all[i].scramb_id == pop;
    bool range = own; /* = the ISAC_CSIRS_FULL rule (full_select): own + own confirmed RB blocks */
    for (int j = 0; j < n_conf && !range; j++)
      range = conf[j].scramb_id == pop && conf[j].csi_type != 2 && full_rb_overlap_half(&conf[j], &all[i]);
    in_h[1][i] = in_h[4][i] = own;
    in_h[2][i] = in_h[5][i] = range;
    n_own += own;
    n_range += range;
    in_h[3][i] = true;
  }
  const bool can_toggle = cfg->start_symbol <= 1 && cfg->start_symbol + cfg->number_symbols == 14;
  char res[6][3];
  memset(res, '-', sizeof(res));
  t_full_ab_depth++;
  for (int h = 0; h < 6; h++) {
    for (int t = 0; t < (can_toggle ? 2 : 1); t++) {
      fapi_nr_dl_config_dlsch_pdu_rel15_t c = *cfg;
      c.numCsiRsForRateMatching = 0;
      fapi_nr_dl_config_csirs_pdu_rel15_t er[NFAPI_MAX_NUM_CSI_RATEMATCH];
      int ne = 0;
      for (int i = 0; i < n_all; i++) {
        if (csirs_full_dmrs_clash(&all[i], c.dlDmrsSymbPos))
          continue;
        fapi_nr_dl_config_csirs_pdu_rel15_t r = all[i];
        if (h == 4) {
          const int st = r.start_rb & ~15, en = ((r.start_rb + r.nr_of_rbs + 15) / 16) * 16;
          r.start_rb = st;
          r.nr_of_rbs = en - st;
        }
        bool dup = false;
        if (in_h[h][i]) {
          for (int j = 0; j < c.numCsiRsForRateMatching && !dup; j++)
            dup = csirs_full_same_geometry(&c.csiRsForRateMatching[j], &r);
          if (!dup && c.numCsiRsForRateMatching < NFAPI_MAX_NUM_CSI_RATEMATCH)
            c.csiRsForRateMatching[c.numCsiRsForRateMatching++] = r;
        } else if (s_erase && h != 5 && r.csi_type != 2) { /* h 5 = the rule without erasure */
          for (int j = 0; j < ne && !dup; j++)
            dup = csirs_full_same_geometry(&er[j], &r);
          if (!dup && ne < NFAPI_MAX_NUM_CSI_RATEMATCH)
            er[ne++] = r;
        }
      }
      if (t) {
        c.start_symbol = 1 - cfg->start_symbol;
        c.number_symbols = 14 - c.start_symbol;
      }
      if (nr_dlsch_csi_erase_set)
        nr_dlsch_csi_erase_set(er, ne);
      nr_pdsch_passive_decode_result_t o;
      memset(&o, 0, sizeof(o));
      const nr_pdsch_passive_decode_status_t st = nr_pdsch_passive_decode(ue, proc, &c, fa, grant, rxdataF, &o);
      res[h][t] = st == NR_PDSCH_PASSIVE_DECODE_CRC_OK ? (o.config_recovered ? 'r' : '1') : '0';
    }
    res[h][2] = 0;
  }
  t_full_ab_depth--;
  LOG_I(PHY, "SENSING: CSIFULLAB rnti=0x%04x fs=%d.%d ph=%u nl=%d mcs=%u rb=%u+%u S=%u L=%u pop=%u n_all=%d n_own=%d n_range=%d "
        "none=%s own=%s range=%s all=%s own16=%s range_noerase=%s\n",
        grant->rnti, proc->frame_rx, proc->nr_slot_rx, slot % 160, __builtin_popcount(cfg->dmrs_ports), grant->mcs,
        fa->first_rb, fa->num_rbs, cfg->start_symbol, cfg->number_symbols, pop, n_all, n_own, n_range, res[0], res[1], res[2], res[3],
        res[4], res[5]);
}
