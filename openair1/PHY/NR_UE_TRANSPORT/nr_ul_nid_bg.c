/* See nr_ul_nid_bg.h. Env (all read once; everything default off):
 *   ISAC_UL_NID_BG=1            module on
 *   ISAC_UL_NID_BG_IQ=1         stage B (hypothesis expansion on a private IQ copy)
 *   ISAC_UL_NID_BG_SNR=6        min DM-RS SNR (dB) of an admitted grant
 *   ISAC_UL_NID_BG_QM=2         max modulation order of an admitted grant (QPSK first)
 *   ISAC_UL_NID_BG_MIN_RB=16    min PRBs (wide low-MCS grants carry CRC24 TBs: one hit confirms)
 *   ISAC_UL_NID_BG_GAP_MS=500   stage B: min time between two admitted grants of one RNTI (no SNR gate there: every hypothesis measures its own SNR)
 *   ISAC_UL_NID_BG_REPEAT=4     stage A: max sweeps of one (RNTI, layout hypothesis); distinct hypotheses are never throttled
 *   ISAC_UL_NID_BG_TRIALS=1     admit CRC-first trial grants (their LLRs carry a layout variant)
 *   ISAC_UL_NID_BG_CPU_PCT=50   worker duty cycle: after a job it sleeps cpu*(100-pct)/pct
 *   ISAC_UL_NID_BG_JOB_MS=15000 CPU-time cap of one job
 *   ISAC_UL_NID_BG_HYP=1000     stage B hypotheses per job (cursor per RNTI)
 *   ISAC_UL_NID_BG_TOPM=12      stage B: id sweeps on the TOPM highest-DM-RS-SNR distinct hypotheses of a job
 *   ISAC_UL_NID_BG_ADDPOS=1     stage B: also the other dmrs-AdditionalPosition readings (0 = the lock's current one only)
 *   ISAC_UL_NID_BG_CORE=<cpu>   pin the worker thread (default: unpinned, nice 19)
 *   ISAC_UL_NID_BG_IDLE=1       SCHED_IDLE worker (default off)
 *   ISAC_UL_NID_BG_NBWP=4       stage B BWP hypotheses of the lock's ranking walked per DCI (variants: ISAC_UL_MCS_TABLE_SWEEP / K2_PAIR / TDA_PAIR as in the CRC trials)
 *   ISAC_UL_NID_BG_MCS=4        stage B admits grants with an MCS index <= this (QPSK in every table); stage A needs the decoded Qm <= ISAC_UL_NID_BG_QM
 *   ISAC_UL_NID_BG_DROP=100     qualified real-time failures (and no pass) that drop a confirmed entry */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nr_ul_nid_bg.h"
#include "common/utils/nr_core_plan.h"
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "PHY/CODING/coding_defs.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/gold.h"
#include "common/utils/LOG/log.h"
#include "nr_pdcch_ul_layout_lock.h"
#include "nr_pusch_ul_disc.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h" /* nr_get_Qm_ul, nr_get_code_rate_ul */
#include "nr_pdsch_passive_decode.h" /* nr_pdsch_gw_cb0_t, nr_pdsch_passive_gw_cb0_decode (the CPU CB0 LDPC + all-zero guard) */

#define NJOB 4
#define RNTI_CAP 64
#define PEND_CAP 16
#define SEEN_CAP 96
#define LLR_MAX (1u << 20)
#define CAND_MAX 512
#define HYP_MAX 720

typedef struct {
  bool used, confirmed;
  uint16_t rnti;
  uint64_t last_ns;
  struct { uint64_t key; uint32_t n; } seen[SEEN_CAP];
  uint8_t nseen;
  nr_ul_nid_pend_t pend[PEND_CAP];
  uint8_t npend;
  nr_ul_nid_conf_t conf;
  uint32_t rt_ok, rt_fail, cursor;
  int dmrs_best; /* DM-RS id with the best SNR of an earlier job, -1 none */
} nid_rnti_t;

static nr_ul_nid_job_t g_job[NJOB];
static nid_rnti_t g_rnti[RNTI_CAP];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static nr_ul_nid_stats_t g_st;
static _Atomic int g_on = -1, g_iq_on = -1, g_stop;
static float g_snr;
static int g_idle, g_core = -1, g_topm, g_addpos_n, g_dbg_try = -1, g_gap_ms, g_nbwp, g_mcs_max, g_qm, g_min_rb, g_repeat, g_trials, g_cpu_pct, g_job_ms, g_hyp_per_job, g_drop;
static int g_nlog;
static nr_ul_nid_hyp_fn g_fn;
static nr_ul_nid_prep_fn g_prep;
static pthread_t g_thr;
static bool g_started;
/* ids confirmed for other RNTIs: tried first for a new one (learned at run time, never a constant) */
static uint16_t g_learned[8];
static int g_nlearned;

static long envl(const char *k, long d)
{
  const char *v = getenv(k);
  return v && *v ? atol(v) : d;
}
static void load_env(void)
{
  const char *e = getenv("ISAC_UL_NID_BG");
  const int on = !e || !strcmp(e, "1");
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_SNR not in the frozen config t26zE (2026-10-18); slated for removal */
  g_snr = (float)envl("ISAC_UL_NID_BG_SNR", 6);
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_QM not in the frozen config t26zE (2026-10-18); slated for removal */
  g_qm = (int)envl("ISAC_UL_NID_BG_QM", 2);
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_MIN_RB not in the frozen config t26zE (2026-10-18); slated for removal */
  g_min_rb = (int)envl("ISAC_UL_NID_BG_MIN_RB", 16);
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_REPEAT not in the frozen config t26zE (2026-10-18); slated for removal */
  g_repeat = (int)envl("ISAC_UL_NID_BG_REPEAT", 4);
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_GAP_MS not in the frozen config t26zE (2026-10-18); slated for removal */
  g_gap_ms = (int)envl("ISAC_UL_NID_BG_GAP_MS", 500);
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_DBG_TRY not in the frozen config t26zE (2026-10-18); slated for removal */
  g_dbg_try = (int)envl("ISAC_UL_NID_BG_DBG_TRY", -1);
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_TOPM not in the frozen config t26zE (2026-10-18); slated for removal */
  g_topm = (int)envl("ISAC_UL_NID_BG_TOPM", 12);
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_ADDPOS not in the frozen config t26zE (2026-10-18); slated for removal */
  g_addpos_n = envl("ISAC_UL_NID_BG_ADDPOS", 1) ? 3 : 1;
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_TRIALS not in the frozen config t26zE (2026-10-18); slated for removal */
  g_trials = (int)envl("ISAC_UL_NID_BG_TRIALS", 1);
  g_cpu_pct = (int)envl("ISAC_UL_NID_BG_CPU_PCT", 10);
  if (g_cpu_pct < 1) g_cpu_pct = 1;
  if (g_cpu_pct > 100) g_cpu_pct = 100;
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_JOB_MS not in the frozen config t26zE (2026-10-18); slated for removal */
  g_job_ms = (int)envl("ISAC_UL_NID_BG_JOB_MS", 15000);
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_HYP not in the frozen config t26zE (2026-10-18); slated for removal */
  g_hyp_per_job = (int)envl("ISAC_UL_NID_BG_HYP", 1000);
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_DROP not in the frozen config t26zE (2026-10-18); slated for removal */
  g_drop = (int)envl("ISAC_UL_NID_BG_DROP", 100);
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_LOG not in the frozen config t26zE (2026-10-18); slated for removal */
  g_nlog = (int)envl("ISAC_UL_NID_BG_LOG", 0);
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_CORE not in the frozen config t26zE (2026-10-18); slated for removal */
  g_core = (int)envl("ISAC_UL_NID_BG_CORE", -1);
  g_idle = (int)envl("ISAC_UL_NID_BG_IDLE", 1);
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_NBWP not in the frozen config t26zE (2026-10-18); slated for removal */
  g_nbwp = (int)envl("ISAC_UL_NID_BG_NBWP", 4);
  /* DEFERRED/DO NOT USE: ISAC_UL_NID_BG_MCS not in the frozen config t26zE (2026-10-18); slated for removal */
  g_mcs_max = (int)envl("ISAC_UL_NID_BG_MCS", 4);
  const char *iq = getenv("ISAC_UL_NID_BG_IQ");
  atomic_store(&g_iq_on, on && (!iq || !strcmp(iq, "1")));
  atomic_store(&g_on, on);
  if (on)
    LOG_A(PHY, "SENSING: UL_NID_BG on snr>=%.1f qm<=%d min_rb=%d repeat=%d trials=%d cpu_pct=%d job_ms=%d stageB=%d hyp/job=%d nbwp=%d mcs<=%d\n", g_snr, g_qm,
          g_min_rb, g_repeat, g_trials, g_cpu_pct, g_job_ms, (int)atomic_load(&g_iq_on), g_hyp_per_job, g_nbwp, g_mcs_max);
}
bool nr_ul_nid_bg_enabled(void)
{
  if (atomic_load_explicit(&g_on, memory_order_relaxed) < 0) load_env();
  return atomic_load_explicit(&g_on, memory_order_relaxed) > 0;
}
bool nr_ul_nid_bg_iq_enabled(void)
{
  return nr_ul_nid_bg_enabled() && atomic_load_explicit(&g_iq_on, memory_order_relaxed) > 0;
}

static uint64_t now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static uint64_t cpu_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ---------------------------------------------------------------- admission */
static nid_rnti_t *rnti_get_locked(uint16_t rnti, bool create)
{
  nid_rnti_t *free_ = NULL, *old = NULL;
  for (int i = 0; i < RNTI_CAP; i++) {
    if (g_rnti[i].used && g_rnti[i].rnti == rnti) return &g_rnti[i];
    if (!g_rnti[i].used && !free_) free_ = &g_rnti[i];
    if (g_rnti[i].used && !g_rnti[i].confirmed && (!old || g_rnti[i].last_ns < old->last_ns)) old = &g_rnti[i];
  }
  if (!create) return NULL;
  nid_rnti_t *r = free_ ? free_ : (old ? old : &g_rnti[0]);
  memset(r, 0, sizeof(*r));
  r->used = true;
  r->rnti = rnti;
  r->dmrs_best = -1;
  return r;
}

static uint64_t hyp_key(const nr_ul_nid_rec_t *r, uint16_t id);
int nr_ul_nid_bg_admit(const nr_ul_nid_rec_t *r)
{
  if (!nr_ul_nid_bg_enabled()) return -1;
  if (g_nlog >= 2) { /* every offered record, capped: why does a hypothesis never qualify */
    static _Atomic int n;
    if (atomic_fetch_add(&n, 1) < 1500)
      LOG_A(PHY, "SENSING: UL_NID_OFFER rnti=0x%04x trial=%u hyp=%u@%u tbl=%u mcs=%u qm=%u rb=%u+%u sym=%u+%u tbs=%u dmrs_id=%u tried=%u snr=%.1f\n", r->rnti,
            r->trial, r->bwp_size, r->bwp_start, r->mcs_table, r->mcs, r->qm, r->start_rb, r->num_rb, r->start_sym, r->num_sym, r->tbs_bits, r->dmrs_id,
            r->tried_id, r->snr_db);
  }
  const bool b_on = nr_ul_nid_bg_iq_enabled(); /* stage B: the SNR is measured under each hypothesis, not under the real-time one */
  const bool qm_ok = b_on ? r->mcs <= g_mcs_max : r->qm <= g_qm;
  if (r->crc_ok || (!b_on && !(r->snr_db >= g_snr)) || !qm_ok || r->num_rb < g_min_rb || !r->tbs_bits || !r->G || r->G > LLR_MAX ||
      (r->trial && !g_trials) || r->nl != 1) {
    pthread_mutex_lock(&g_lock);
    g_st.dropped_gate++;
    if (!qm_ok) g_st.gate_qm++;
    else if (!b_on && !(r->snr_db >= g_snr)) g_st.gate_snr++;
    pthread_mutex_unlock(&g_lock);
    return -1;
  }
  const uint64_t t = now_ns();
  int idx = -1;
  pthread_mutex_lock(&g_lock);
  nid_rnti_t *e = rnti_get_locked(r->rnti, true);
  /* a hypothesis that was swept g_repeat times is not swept again: the budget goes to hypotheses not yet seen */
  const uint64_t hk = hyp_key(r, 0);
  int si = -1;
  for (int i = 0; i < e->nseen; i++)
    if (e->seen[i].key == hk) si = i;
  if (e->confirmed || (b_on && e->last_ns && t - e->last_ns < (uint64_t)g_gap_ms * 1000000ull) || (!b_on && si >= 0 && e->seen[si].n >= (uint32_t)g_repeat)) {
    g_st.dropped_gate++;
    g_st.gate_rate++;
  } else {
    for (int i = 0; i < NJOB; i++)
      if (g_job[i].st == 0) {
        g_job[i].st = 1;
        idx = i;
        break;
      }
    if (idx < 0) g_st.dropped_busy++;
    else {
      e->last_ns = t;
      if (si < 0) {
        if (e->nseen == SEEN_CAP) memmove(&e->seen[0], &e->seen[1], sizeof(e->seen[0]) * (SEEN_CAP - 1)), e->nseen--;
        si = e->nseen++;
        e->seen[si].key = hk;
        e->seen[si].n = 0;
      }
      e->seen[si].n++;
      g_job[idx].rec = *r;
      g_st.submitted++;
      if (g_nlog)
        LOG_A(PHY, "SENSING: UL_NID_ADMIT rnti=0x%04x trial=%u hyp=%u@%u tbl=%u mcs=%u qm=%u rb=%u+%u sym=%u+%u tbs=%u dmrs_id=%u tried=%u snr=%.1f slot=%ld\n", r->rnti,
              r->trial, r->bwp_size, r->bwp_start, r->mcs_table, r->mcs, r->qm, r->start_rb, r->num_rb, r->start_sym, r->num_sym, r->tbs_bits, r->dmrs_id,
              r->tried_id, r->snr_db, (long)r->abs_slot);
    }
  }
  pthread_mutex_unlock(&g_lock);
  return idx;
}
nr_ul_nid_job_t *nr_ul_nid_bg_slot(int idx)
{
  return &g_job[idx];
}
bool nr_ul_nid_bg_slot_reserve(int idx, uint32_t llr_n, uint32_t iq_n)
{
  nr_ul_nid_job_t *j = &g_job[idx];
  if (j->llr_cap < llr_n) {
    free(j->llr);
    j->llr = malloc((size_t)llr_n * sizeof(int16_t));
    j->llr_cap = j->llr ? llr_n : 0;
    if (!j->llr) return false;
  }
  if (iq_n && j->iq_cap < iq_n) {
    for (int a = 0; a < 4; a++) {
      free(j->iq[a]);
      free(j->iq2[a]);
      j->iq[a] = (c16_t *)aligned_alloc(64, (((size_t)iq_n * sizeof(c16_t)) + 63) & ~(size_t)63);
      j->iq2[a] = (c16_t *)aligned_alloc(64, (((size_t)iq_n * sizeof(c16_t)) + 63) & ~(size_t)63);
      if (!j->iq[a] || !j->iq2[a]) {
        j->iq_cap = 0;
        return false;
      }
    }
    j->iq_cap = iq_n;
  }
  j->have_iq2 = false;
  j->have_iq = false;
  return true;
}
void nr_ul_nid_bg_abort(int idx)
{
  pthread_mutex_lock(&g_lock);
  g_job[idx].st = 0;
  pthread_mutex_unlock(&g_lock);
}
void nr_ul_nid_bg_publish(int idx)
{
  pthread_mutex_lock(&g_lock);
  g_job[idx].st = 2;
  pthread_mutex_unlock(&g_lock);
}

bool nr_ul_nid_bg_lookup(uint16_t rnti, nr_ul_nid_conf_t *c)
{
  if (!nr_ul_nid_bg_enabled()) return false;
  if (pthread_mutex_trylock(&g_lock)) return false;
  nid_rnti_t *e = rnti_get_locked(rnti, false);
  const bool ok = e && e->confirmed;
  if (ok) *c = e->conf;
  pthread_mutex_unlock(&g_lock);
  return ok;
}
void nr_ul_nid_bg_feedback(uint16_t rnti, bool ok, float snr_db)
{
  pthread_mutex_lock(&g_lock);
  nid_rnti_t *e = rnti_get_locked(rnti, false);
  if (e && e->confirmed) {
    if (ok) {
      e->rt_ok++;
      g_st.rt_ok++;
    } else if (snr_db >= g_snr) {
      e->rt_fail++;
      g_st.rt_fail++;
      if (!e->rt_ok && e->rt_fail >= (uint32_t)g_drop) {
        LOG_A(PHY, "SENSING: UL_NID_DROPPED rnti=0x%04x data_id=%u after %u qualified real-time failures and no pass\n", rnti, e->conf.data_id,
              e->rt_fail);
        e->confirmed = false;
        e->npend = 0;
        e->last_ns = 0;
        g_st.dropped_entries++;
      }
    }
  }
  pthread_mutex_unlock(&g_lock);
}
bool nr_ul_nid_bg_wants_next(const nr_pdcch_blind_ul_result_t *g)
{
  if (!nr_ul_nid_bg_iq_enabled() || g->mcs > g_mcs_max || g->num_rb < g_min_rb || g->nrOfLayers != 1) return false;
  pthread_mutex_lock(&g_lock);
  nid_rnti_t *e = rnti_get_locked(g->rnti, false);
  const bool ok = !(e && e->confirmed); /* the gap and the free job slot are not asked: the job runs ~100 ms after the copy */
  pthread_mutex_unlock(&g_lock);
  return ok;
}
void nr_ul_nid_bg_note_iq2(bool ok)
{
  pthread_mutex_lock(&g_lock);
  if (ok) g_st.iq2_ok++;
  else g_st.iq2_miss++;
  pthread_mutex_unlock(&g_lock);
}
void nr_ul_nid_bg_stats(nr_ul_nid_stats_t *s)
{
  pthread_mutex_lock(&g_lock);
  *s = g_st;
  pthread_mutex_unlock(&g_lock);
}

/* ---------------------------------------------------------------- the id sweep */
static void gold_sign(int16_t *l, uint32_t n, uint32_t cinit)
{
  uint32_t x1 = 0, x2 = cinit;
  uint32_t w = gold_generic(&x1, &x2, 1);
  for (uint32_t i = 0; i < n; i++) {
    if (i && (i & 31) == 0) w = gold_generic(&x1, &x2, 0);
    if ((w >> (i & 31)) & 1) l[i] = (int16_t)-l[i];
  }
}

/* hypothesis key: everything that must be equal for two hits of different slots to count as the same finding */
static uint64_t hyp_key(const nr_ul_nid_rec_t *r, uint16_t id)
{
  uint64_t k = r->k2;
  k = k * 1021 + r->bwp_size;
  k = k * 1021 + r->bwp_start;
  k = k * 7 + r->mcs_table;
  k = k * 65521 + r->dmrs_id;
  k = k * 3 + r->nscid;
  k = k * 17 + r->start_sym;
  k = k * 17 + r->num_sym;
  k = k * 3 + r->tp;
  k = k * 16411 + r->dmrs_mask;
  k = k * 1031 + id;
  return k ^ ((uint64_t)r->rnti << 48);
}

/* returns true when the hit passes the confirmation rule (or repeats the confirmed entry): only those feed the layout lock */
static bool note_hit(const nr_ul_nid_rec_t *r, uint16_t id, bool strong)
{
  const uint64_t key = hyp_key(r, id);
  pthread_mutex_lock(&g_lock);
  nid_rnti_t *e = rnti_get_locked(r->rnti, true);
  if (strong) g_st.hits_strong++;
  else g_st.hits_weak++;
  if (e->confirmed) {
    const bool same = e->conf.data_id == id;
    pthread_mutex_unlock(&g_lock);
    return same;
  }
  nr_ul_nid_pend_t *p = NULL;
  for (int i = 0; i < e->npend; i++)
    if (e->pend[i].key == key) p = &e->pend[i];
  if (!p) {
    if (e->npend == PEND_CAP) memmove(&e->pend[0], &e->pend[1], sizeof(e->pend[0]) * (PEND_CAP - 1)), e->npend--;
    p = &e->pend[e->npend++];
    memset(p, 0, sizeof(*p));
    p->key = key;
  }
  const bool conf = nr_ul_nid_confirm_rule(p, strong, r->abs_slot);
  LOG_A(PHY, "SENSING: UL_NID_HIT rnti=0x%04x data_id=%u dmrs_id=%u hyp=%u@%u tbl=%u rb=%u+%u sym=%u+%u tbs=%u crc=%s slot=%ld seen=%u%s\n", r->rnti, id,
        r->dmrs_id, r->bwp_size, r->bwp_start, r->mcs_table, r->start_rb, r->num_rb, r->start_sym, r->num_sym, r->tbs_bits, strong ? "24" : "16",
        (long)r->abs_slot, p->n, conf ? " CONFIRMED" : "");
  if (conf) {
    e->confirmed = true;
    e->conf = (nr_ul_nid_conf_t){.data_id = id, .dmrs_id = r->dmrs_id, .bwp_size = r->bwp_size, .bwp_start = r->bwp_start, .mcs_table = r->mcs_table,
                                 .dmrs_mask = r->dmrs_mask, .start_sym = r->start_sym, .num_sym = r->num_sym, .mapping = r->mapping, .have_ta = true, .ta = r->ta};
    e->rt_ok = e->rt_fail = 0;
    g_st.confirmed++;
    bool known = false;
    for (int i = 0; i < g_nlearned; i++) known |= g_learned[i] == id;
    if (!known && g_nlearned < 8) g_learned[g_nlearned++] = id;
  }
  pthread_mutex_unlock(&g_lock);
  return conf;
}

/* The same MCS reads as the same modulation and code rate under another table of the family: a hit cannot tell the tables apart (e.g. MCS 0 of tables 0 and 1) */
static bool table_ambiguous(const nr_ul_nid_rec_t *r, const nr_pdcch_blind_ul_result_t *tag)
{
  for (int t = tag->transform_precoding ? 3 : 0; t < (tag->transform_precoding ? 5 : 3); t++) {
    if (t == tag->mcs_table) continue;
    if (nr_get_Qm_ul(r->mcs, t) == nr_get_Qm_ul(r->mcs, tag->mcs_table) && nr_get_code_rate_ul(r->mcs, t) == nr_get_code_rate_ul(r->mcs, tag->mcs_table))
      return true;
  }
  return false;
}

/* One hypothesis: every candidate id on code block 0. llr = LLRs descrambled with r->tried_id. Returns hits. */
static int sweep(const nr_ul_nid_rec_t *r, const int16_t *llr, uint64_t cpu_deadline_ns, const nr_pdcch_blind_ul_result_t *tag)
{
  unsigned C = 0, K = 0, Z = 0, F = 0;
  if (r->tbs_bits == 0 || r->bg < 1 || r->bg > 2) return 0;
  nr_segmentation(NULL, NULL, (unsigned)lenWithCrc(1, (int)r->tbs_bits), &C, &K, &Z, &F, r->bg);
  if (!C) return 0;
  const int E = nr_get_E(r->G, (uint8_t)C, r->qm, r->nl, 0);
  if (E <= 0 || (uint32_t)E > r->G) return 0;
  const bool strong = C > 1 || r->tbs_bits > 3824;
  nr_pdsch_gw_cb0_t cb;
  memset(&cb, 0, sizeof(cb));
  cb.p = (nr_td_cb0_params_t){.G = r->G, .A = r->tbs_bits, .E = (uint32_t)E, .tbslbrm = 0, .C = (int)C, .K = (int)K, .Z = (int)Z, .F = (int)F,
                              .BG = r->bg, .Qm = r->qm, .Nl = r->nl, .rv = r->rv};
  const size_t dlen = (size_t)nr_td_gw_cb0_dlen(&cb.p);
  int16_t *neutral = malloc(((size_t)E + 64) * sizeof(int16_t)), *w = malloc(((size_t)E + 64) * sizeof(int16_t)), *d = malloc(dlen * sizeof(int16_t));
  if (!neutral || !w || !d) {
    free(neutral);
    free(w);
    free(d);
    return 0;
  }
  cb.d = d;
  memcpy(neutral, llr, (size_t)E * sizeof(int16_t));
  gold_sign(neutral, (uint32_t)E, ((uint32_t)r->rnti << 15) + r->tried_id);
  /* order: ids already confirmed for other RNTIs, then the PCI, then 0..1023 */
  uint16_t order[1024 + 10];
  int no = 0;
  bool seen[1024] = {false};
  uint16_t pre[10];
  int np = 0;
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < g_nlearned && np < 8; i++) pre[np++] = g_learned[i];
  pthread_mutex_unlock(&g_lock);
  if (np < 10) pre[np++] = r->pci;
  for (int i = 0; i < np; i++)
    if (pre[i] < 1024 && !seen[pre[i]]) seen[pre[i]] = true, order[no++] = pre[i];
  for (int id = 0; id < 1024; id++)
    if (!seen[id]) order[no++] = (uint16_t)id;
  int hits = 0;
  uint64_t cands = 0;
  if (g_nlog) {
    double m = 0;
    for (int i = 0; i < E; i++) m += abs(llr[i]);
    LOG_A(PHY, "SENSING: UL_NID_SWEEP rnti=0x%04x hyp=%u@%u tbl=%u k2=%u sym=%u+%u rb=%u+%u dmrs=%u tried=%u G=%u E=%d C=%u tbs=%u qm=%u snr=%.1f mean|llr|=%.1f crc_ok=%d\n", r->rnti,
          r->bwp_size, r->bwp_start, r->mcs_table, r->k2, r->start_sym, r->num_sym, r->start_rb, r->num_rb, r->dmrs_id, r->tried_id, r->G, E, C, r->tbs_bits,
          r->qm, r->snr_db, m / E, (int)r->crc_ok);
  }
  for (int i = 0; i < no && !atomic_load(&g_stop); i++) {
    if ((i & 31) == 31 && cpu_ns() > cpu_deadline_ns) break;
    memcpy(w, neutral, (size_t)E * sizeof(int16_t));
    gold_sign(w, (uint32_t)E, ((uint32_t)r->rnti << 15) + order[i]);
    cands++;
    if (nr_td_gw_cb0_extract(w, (uint32_t)E, &cb.p, d) != NR_TD_GW_OK) break;
    bool ldpc_ok = false;
    if (nr_pdsch_passive_gw_cb0_decode(&cb, &ldpc_ok)) {
      hits++;
      /* a hit that passed the confirmation rule is a decisive CRC pass for the layout lock (table, k2, symbol allocation, BWP hypothesis) */
      if (note_hit(r, order[i], strong) && tag) nr_ul_layout_nid_verdict(tag, table_ambiguous(r, tag), r->tbs_bits, (long)r->abs_slot);
    }
  }
  pthread_mutex_lock(&g_lock);
  g_st.sweeps++;
  g_st.cands += cands;
  pthread_mutex_unlock(&g_lock);
  if (tag) nr_ul_layout_trial_add(tag, (uint32_t)cands);
  free(neutral);
  free(w);
  free(d);
  return hits;
}

/* ---------------------------------------------------------------- stage B hypotheses */
static bool confirmed_now(uint16_t rnti)
{
  pthread_mutex_lock(&g_lock);
  nid_rnti_t *e = rnti_get_locked(rnti, false);
  const bool c = e && e->confirmed;
  pthread_mutex_unlock(&g_lock);
  return c;
}

static void run_job(nr_ul_nid_job_t *j)
{
  const uint64_t c0 = cpu_ns(), deadline = c0 + (uint64_t)g_job_ms * 1000000ull;
  const nr_pdcch_blind_ul_result_t *g = &j->grant;
  if (j->rec.qm <= g_qm && j->rec.snr_db >= g_snr) sweep(&j->rec, j->llr, deadline, nr_ul_layout_is_tagged(g) && nr_ul_layout_is_trial(g) ? g : NULL);
  if (j->have_iq && g_fn && !confirmed_now(j->rec.rnti)) {
    if (g_prep) g_prep(j);
    nr_ul_nid_cap_t cap;
    cap.llr = malloc(LLR_MAX * sizeof(int16_t));
    cap.llr_cap = LLR_MAX;
    typedef struct { nr_pdcch_blind_ul_result_t hg; int delta; uint16_t id; float snr; } cand_t;
    cand_t *cd = malloc(sizeof(cand_t) * CAND_MAX);
    nr_pdcch_blind_ul_result_t *hl = malloc(sizeof(*hl) * HYP_MAX);
    int ncd = 0, nh = 0;
    /* the lock's hypotheses of this DCI (BWP hypothesis x tail x table / k2 / symbol allocation x dmrs-AdditionalPosition), built once */
    uint32_t total = 0, next = 0;
    while (hl && nh < HYP_MAX && nh < g_hyp_per_job) {
      const int n = nr_ul_layout_hyp_list(g->rnti, g->dci_length, g->raw_payload, next, g_nbwp, g_addpos_n, &hl[nh], 1, &total, &next);
      if (n < 1) {
        if (!total || next == 0) break;
        continue;
      }
      if (hl[nh].k2 >= g->k2 && hl[nh].k2 - g->k2 <= 1 && (hl[nh].k2 == g->k2 || j->have_iq2)) nh++;
      if (next == 0) break; /* wrapped: every combination listed */
    }
    /* DM-RS id candidates: the background discovery's accepted one, what the real-time decode used, the PCI, the ids latched for other RNTIs, the one this RNTI
     * showed before. Phase 0 keeps the ids whose DM-RS SNR clears the gate on at least one of the first hypotheses (the SNR separates a right id from a
     * wrong one by 30 dB; the cached discovery id of a wrong-slot reading wanders). */
    uint16_t ids[8];
    int nid = 0;
    {
      const nr_pusch_ul_disc_result_t dc = nr_pusch_ul_disc_cached(g->rnti);
      int known[4];
      const int nk = nr_pusch_ul_disc_known_ids(known, 4);
      int cl[10], ncl = 0;
      pthread_mutex_lock(&g_lock);
      nid_rnti_t *e = rnti_get_locked(j->rec.rnti, true);
      const int best = e->dmrs_best;
      pthread_mutex_unlock(&g_lock);
      if (best >= 0) cl[ncl++] = best;
      if (dc.accepted && dc.id >= 0) cl[ncl++] = dc.id;
      cl[ncl++] = g->ul_dmrs_scrambling_id;
      cl[ncl++] = j->rec.pci;
      for (int i = 0; i < nk && ncl < 10; i++) cl[ncl++] = known[i];
      for (int c = 0; c < ncl; c++) {
        bool dup = cl[c] < 0;
        for (int i = 0; i < nid; i++) dup |= ids[i] == cl[c];
        if (!dup && nid < 8) ids[nid++] = (uint16_t)cl[c];
      }
    }
    bool idok[8] = {false};
    float idsnr[8];
    for (int i = 0; i < 8; i++) idsnr[i] = -99.f;
    const int nref = nh < 36 ? nh : 36;
    int done = 0;
    for (int phase = 0; phase < 2 && cd && hl && cap.llr; phase++) {
      for (int hx = phase ? nref : 0; hx < (phase ? nh : nref) && !atomic_load(&g_stop) && cpu_ns() < deadline; hx++) {
        const int delta = (int)hl[hx].k2 - (int)g->k2;
        for (int di = 0; di < nid; di++) {
          if (phase && !idok[di]) continue;
          nr_pdcch_blind_ul_result_t h2 = hl[hx];
          h2.ul_dmrs_scrambling_id = ids[di];
          if (g_dbg_try >= 0) h2.data_scrambling_id = (uint16_t)g_dbg_try; /* ISAC_UL_NID_BG_DBG_TRY=<id>: DIAGNOSTIC, a ground-truth id for validating the hypothesis chain; never a receiver input */
          memset(&cap.rec, 0, sizeof(cap.rec));
          cap.got = false;
          const bool got = g_fn(j, &h2, j->rec.ta_nom, delta, &cap) && cap.got && cap.rec.G <= LLR_MAX;
          if (g_nlog)
            LOG_A(PHY, "SENSING: UL_NID_HYP rnti=0x%04x hx=%d/%d hyp=%u@%u tbl=%u dk2=%d sym=%u+%u map=%u mask=0x%x rb=%u+%u dmrs=%u got=%d snr=%.1f tbs=%u\n", g->rnti, hx,
                  nh, h2.bwp_size, h2.bwp_start, h2.mcs_table, delta, h2.start_symbol, h2.num_symbols, h2.mapping_type, h2.ul_dmrs_symb_pos, h2.start_rb,
                  h2.num_rb, h2.ul_dmrs_scrambling_id, (int)got, got ? cap.rec.snr_db : -99.f, got ? cap.rec.tbs_bits : 0);
          if (got && cap.rec.snr_db >= g_snr) {
            idok[di] = true;
            if (cap.rec.snr_db > idsnr[di]) idsnr[di] = cap.rec.snr_db;
            if (ncd < CAND_MAX) cd[ncd++] = (cand_t){.hg = h2, .delta = delta, .id = ids[di], .snr = cap.rec.snr_db};
          }
          done++;
          pthread_mutex_lock(&g_lock);
          g_st.hyps++;
          pthread_mutex_unlock(&g_lock);
        }
      }
      if (phase == 0) { /* no id clears the gate on the first hypotheses: nothing of this grant is aligned (not this UE's grant, or not decodable) */
        bool any = false;
        for (int di = 0; di < nid; di++) any |= idok[di];
        if (!any) break;
      }
    }
    if (ncd) { /* remember the id with the best SNR for this RNTI */
      int bi = 0;
      for (int di = 1; di < nid; di++) if (idsnr[di] > idsnr[bi]) bi = di;
      pthread_mutex_lock(&g_lock);
      rnti_get_locked(j->rec.rnti, true)->dmrs_best = ids[bi];
      pthread_mutex_unlock(&g_lock);
    }
    /* phase 2: the id sweep on the g_topm hypotheses with the highest DM-RS SNR (the SNR separates the slot and the DM-RS pattern; only the CRC the range, the
     * table and the id), exact duplicates (twin BWP sizes reading the same PRBs) once */
    for (int a = 1; a < ncd; a++)
      for (int b2 = a; b2 > 0 && cd[b2].snr > cd[b2 - 1].snr; b2--) { const cand_t t = cd[b2]; cd[b2] = cd[b2 - 1]; cd[b2 - 1] = t; }
    int swept = 0;
    for (int a = 0; a < ncd && swept < g_topm && cap.llr && !atomic_load(&g_stop) && cpu_ns() < deadline && !confirmed_now(j->rec.rnti); a++) {
      bool dup = false;
      for (int b2 = 0; b2 < a && !dup; b2++)
        dup = cd[b2].snr == cd[a].snr && cd[b2].delta == cd[a].delta && cd[b2].hg.mcs_table == cd[a].hg.mcs_table &&
              cd[b2].hg.ul_dmrs_symb_pos == cd[a].hg.ul_dmrs_symb_pos && cd[b2].hg.bwp_start + cd[b2].hg.start_rb == cd[a].hg.bwp_start + cd[a].hg.start_rb &&
              cd[b2].hg.num_rb == cd[a].hg.num_rb;
      if (dup) continue;
      memset(&cap.rec, 0, sizeof(cap.rec));
      cap.got = false;
      if (g_fn(j, &cd[a].hg, j->rec.ta_nom, cd[a].delta, &cap) && cap.got && cap.rec.G <= LLR_MAX) {
        cap.rec.trial = 1;
        cap.rec.abs_slot = j->rec.abs_slot + cd[a].delta;
        sweep(&cap.rec, cap.llr, deadline, &cd[a].hg);
        swept++;
      }
    }
    (void)done;
    free(hl);
    free(cd);
    free(cap.llr);
  }
  const uint64_t used = cpu_ns() - c0;
  pthread_mutex_lock(&g_lock);
  g_st.jobs_done++;
  g_st.cpu_us += used / 1000;
  pthread_mutex_unlock(&g_lock);
  /* duty cycle: pct% of one core at most */
  const uint64_t sleep_ns = used * (uint64_t)(100 - g_cpu_pct) / (uint64_t)g_cpu_pct;
  for (uint64_t s = 0; s < sleep_ns && !atomic_load(&g_stop); s += 20000000ull) usleep(20000);
}

static void print_stats(const char *tag)
{
  nr_ul_nid_stats_t s;
  nr_ul_nid_bg_stats(&s);
  LOG_A(PHY,
        "SENSING: UL_NID_STATS%s submitted=%lu gate_drop=%lu busy_drop=%lu jobs=%lu hyps=%lu sweeps=%lu cands=%lu hits_weak=%lu hits_strong=%lu confirmed=%lu "
        "gate[snr=%lu qm=%lu rate=%lu] entries_dropped=%lu rt_ok=%lu rt_fail=%lu iq2[ok=%lu miss=%lu] cpu_s=%.2f\n",
        tag, (unsigned long)s.submitted, (unsigned long)s.dropped_gate, (unsigned long)s.dropped_busy, (unsigned long)s.jobs_done, (unsigned long)s.hyps,
        (unsigned long)s.sweeps, (unsigned long)s.cands, (unsigned long)s.hits_weak, (unsigned long)s.hits_strong, (unsigned long)s.confirmed,
        (unsigned long)s.gate_snr, (unsigned long)s.gate_qm, (unsigned long)s.gate_rate, (unsigned long)s.dropped_entries, (unsigned long)s.rt_ok, (unsigned long)s.rt_fail, (unsigned long)s.iq2_ok, (unsigned long)s.iq2_miss, (double)s.cpu_us / 1e6);
}

static void *worker(void *a)
{
  (void)a;
  if (nice(19) == -1) { /* best effort */ }
  if (g_idle) { /* ISAC_UL_NID_BG_IDLE=1: SCHED_IDLE, runs only on a core nothing else wants */
    const struct sched_param sp0 = {0};
    if (sched_setscheduler(0, SCHED_IDLE, &sp0)) LOG_W(PHY, "SENSING: UL_NID_BG cannot use SCHED_IDLE\n");
  }
  if (g_core >= 0) { /* ISAC_UL_NID_BG_CORE=<cpu>: keep the search off the downlink cores */
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(g_core, &cs);
    if (sched_setaffinity(0, sizeof(cs), &cs)) LOG_W(PHY, "SENSING: UL_NID_BG cannot pin to cpu %d\n", g_core);
  } else
    nr_core_float_enter(); /* started by a pinned PUSCH consumer: leave its core for the float lane */
  uint64_t last = now_ns();
  while (!atomic_load(&g_stop)) {
    if (now_ns() - last > 5000000000ull) {
      last = now_ns();
      print_stats("");
    }
    nr_ul_nid_job_t *j = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < NJOB; i++)
      if (g_job[i].st == 2) {
        g_job[i].st = 3;
        j = &g_job[i];
        break;
      }
    pthread_mutex_unlock(&g_lock);
    if (!j) {
      usleep(10000);
      continue;
    }
    run_job(j);
    pthread_mutex_lock(&g_lock);
    j->st = 0;
    pthread_mutex_unlock(&g_lock);
  }
  return NULL;
}

void nr_ul_nid_bg_start(nr_ul_nid_hyp_fn fn, nr_ul_nid_prep_fn prep)
{
  if (!nr_ul_nid_bg_enabled()) return;
  pthread_mutex_lock(&g_lock);
  if (g_started) {
    pthread_mutex_unlock(&g_lock);
    return;
  }
  g_started = true;
  g_fn = nr_ul_nid_bg_iq_enabled() ? fn : NULL;
  g_prep = nr_ul_nid_bg_iq_enabled() ? prep : NULL;
  pthread_mutex_unlock(&g_lock);
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED); /* never inherit SCHED_FIFO from the PUSCH worker */
  struct sched_param sp = {0};
  pthread_attr_setschedpolicy(&at, SCHED_OTHER);
  pthread_attr_setschedparam(&at, &sp);
  if (pthread_create(&g_thr, &at, worker, NULL)) LOG_E(PHY, "SENSING: UL_NID_BG worker not started\n");
  pthread_attr_destroy(&at);
}
void nr_ul_nid_bg_stop(void)
{
  atomic_store(&g_stop, 1);
  if (g_started) pthread_join(g_thr, NULL);
  g_started = false;
  atomic_store(&g_stop, 0);
}
