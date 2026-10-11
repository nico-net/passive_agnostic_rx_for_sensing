/* Blind dedicated-PDSCH grant solver (T26b). See nr_pdsch_solver.h. */
#include "nr_pdsch_solver.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "common/utils/LOG/log.h"

#define SOLVER_RNTIS 64
#define SOLVER_PENDING 4096 /* FIFO-overwritten: probes dropped stale in the queue never report back */
#define SOLVER_MAX_IDS 1024
#define SOLVER_SEARCH_MAX_RB 48   /* search probes only readings this narrow (cost; the small grants decode best) */
#define SOLVER_CAND_TRIES 96      /* confirmation probes before an unconfirmed candidate is dropped */
#define BM_BYTES (NR_PDSCH_SOLVER_MAX_LAYOUTS / 8)

typedef struct { uint64_t key; uint32_t count; } coverage_entry_t;
/* internal diagnostic reasons, after the caller-side NR_PDSCH_SOLVER_WHY_* ones */
enum { W_PLAN = NR_PDSCH_SOLVER_WHY_CALLER_N, W_CSIRS, W_LEADER, W_HOG, W_NLSKIP, W_BACKOFF, W_WIDE, W_PROBES, W_POK, W_PFAIL,
       W_PNOCRED, W_N };
static const char *const kWhy[W_N] = {"gate", "live", "budget0", "noread", "throttle", "cap", "weak", "plan", "csirs", "leader", "hog",
                                      "nlskip", "backoff", "wide", "probes", "p_ok", "p_fail", "p_nocred"};
typedef struct { coverage_entry_t *entries; size_t cap, used; } solver_coverage_t;

typedef struct {
  bool used, solved, has_cand;
  uint16_t rnti;
  uint32_t cursor;
  solver_coverage_t *coverage;
  uint32_t layout_failures;
  bool layouts_reopened;
  /* candidate (one CRC pass) */
  uint8_t *cand_bm;
  uint8_t cand_add_pos;
  int8_t cand_table;      /* -1: the pass did not discriminate tables */
  uint8_t cand_S;
  nr_dci_bits_t cand_payload;
  uint64_t cand_dci;
  uint32_t cand_tries;
  /* solution */
  int rep;
  int n_ev;                 /* CRC-passed (payload, S) pairs: the representative must map them consistently */
  nr_dci_bits_t ev_p[16];
  uint8_t ev_S[16];
  uint8_t add_pos;
  int8_t table;
  int8_t S_by_tda[16];
  int8_t rm;               /* CSI-RS rate matching for this RNTI: -1 unknown, 0 no, 1 yes (CRC-selected) */
  uint32_t full_tr, full_ok, cons_fail;
  /* T26c per-RNTI report: absolute slots (signal time) of the first grant and of the solving CRC pass */
  uint64_t first_abs, solved_abs;
  uint32_t grants;
  int32_t scr, cand_scr;   /* T26c: PDSCH scrambling id hypothesis (-1 = default ids; else the id); solved/candidate */
  int32_t key;             /* T26c: CORESET key of this RNTI's last planned DCI */
  uint32_t n_probe;        /* T26c: probes planned while unsolved (search backoff) */
  uint32_t alt_hash;       /* T26c: g_alt_hash seen at the last plan */
  uint32_t comb_try[4], comb_ok[4]; /* T26d ISAC_RX_MRC_PER_RNTI: CRC per receiver {1 best branch, 3 MRC, 4 NW-MRC, 5 IRC} */
  float snr_ema;           /* T26c: post-equalisation SNR (dB) EMA of this RNTI's full decodes, NAN-free: n_snr = 0 unknown */
  uint32_t n_snr;
  uint32_t pref_gap;       /* T26d ISAC_PDSCH_SOLVER_PREF: grants since the last search-preferred grant */
  uint32_t why[W_N];       /* SOLVERCOVER diagnostics (see NR_PDSCH_SOLVER_WHY_*) */
  uint64_t last_abs;       /* SOLVERCOVER: signal time of the last grant (activity, eviction) */
  double fcredit;          /* SOLVERCOVER ISAC_PDSCH_SOLVER_FAIRSHARE: this RNTI's share of the probe rate, in probes */
  uint64_t fc_abs;         /*   time of the last accrual (0 = never: cold start gets a full budget) */
  uint32_t cred16, cred24; /* TIMSOLVE2 ISAC_PDSCH_SOLVER_SIGNIF: credited search probes while unsolved, by CRC length */
  uint8_t k16, k24;        /*   consistent CRC passes of the current candidate, by CRC length */
} solver_rnti_t;

typedef struct {
  bool used;
  uint32_t token;
  int slot;
  uint8_t phase;          /* 0 = search/candidate, 1 = solved */
  uint8_t S, add_pos, table, probe;
  bool table_disc;        /* this MCS's (Qm, R) differ between the tables tried */
  bool learn_S, learn_table, learn_rm;
  uint8_t rm;
  int tda;                /* solved: tda index the representative read */
  uint64_t dci;
  uint64_t abs;           /* T26c: absolute slot of the grant */
  int32_t scr;
  uint8_t comb;           /* T26d: combining mode of this variant (0 = receiver default) */
  uint8_t n_layers, mcs;
  bool crc24;             /* TIMSOLVE2: the probed code block carries a 24-bit CRC (TBS > 3824), else CRC16 */
  uint16_t dmrs_ports;
  nr_dci_bits_t payload;
  int n_ids;
  uint16_t ids[SOLVER_MAX_IDS];
} pending_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static solver_rnti_t g_r[SOLVER_RNTIS];
static pending_t g_p[SOLVER_PENDING];
static uint32_t g_token = 1; /* sequence part of a token; the low 12 bits are the pending index (O(1) feedback) */
typedef struct {
  int32_t ids[16], key;
  int n;
  uint32_t hash;
  uint64_t now_abs;
  bool decided;
  bool own; /* TIM-PDSCH: ids are this RNTI's own (nr_pdsch_solver_set_alt_scr32) */
} solver_plan_ctx_t;
static solver_plan_ctx_t g_plan_ctx = {.key = -1}; /* legacy path: process-global grant context */
static _Thread_local solver_plan_ctx_t t_plan_ctx = {.key = -1}; /* ISAC_PDSCH_SOLVER_THREAD_CTX: per scan consumer */
/* solver_thread_ctx_enabled: returns ISAC_PDSCH_SOLVER_THREAD_CTX (cached on first call, atomic): 1 = per-thread plan context instead of the global one.
 *   Read once from the environment; used by plan_ctx() and by the set_alt_scr/set_now/lock paths to choose the mutex policy.
 *   Callers: plan_ctx(), nr_pdsch_solver_set_alt_scr(), nr_pdsch_solver_set_now().
 */
static bool solver_thread_ctx_enabled(void)
{
  static _Atomic int en = -1;
  int v = atomic_load(&en);
  if (v < 0) {
    v = getenv("ISAC_PDSCH_SOLVER_THREAD_CTX") && atoi(getenv("ISAC_PDSCH_SOLVER_THREAD_CTX")) ? 1 : 0;
    atomic_store(&en, v);
  }
  return v == 1;
}
/* plan_ctx: returns the plan context (per-thread or process-global) that the grant planner reads its key, hash and slot time from.
 *   Pure selector, no locking; the caller holds g_mu unless thread-context mode is on.
 *   Callers: nr_pdsch_solver_set_alt_scr(), nr_pdsch_solver_set_now(), nr_pdsch_solver_plan_slot().
 */
static solver_plan_ctx_t *plan_ctx(void) { return solver_thread_ctx_enabled() ? &t_plan_ctx : &g_plan_ctx; }
static struct { int32_t key, scr; } g_key_scr[16]; /* first solved scrambling per CORESET: its other RNTIs try it only */
static int g_n_key_scr;
static uint64_t g_dci_seq;
static nr_pdsch_solver_tda_fn g_tda_fn;
static void *g_tda_ctx;
static nr_pdsch_solver_stats_t g_st;
static int g_nl_pref_gap = -1;
static int g_coverage = -1;
static int g_layout_recover = -1;
static int g_fairshare = -1; /* ISAC_PDSCH_SOLVER_FAIRSHARE, re-read after nr_pdsch_solver_reset() */
static int g_reuse = -1; /* ISAC_PDSCH_SOLVER_REUSE, re-read after nr_pdsch_solver_reset() */
static int g_prior_crc = -1; /* ISAC_PDSCH_SOLVER_PRIOR_CRC, re-read after nr_pdsch_solver_reset() */
static int g_signif = -1;    /* TIMSOLVE2 ISAC_PDSCH_SOLVER_SIGNIF, re-read after nr_pdsch_solver_reset() */
static int g_prior_ids = -1; /* TIMSOLVE2 ISAC_PDSCH_SOLVER_PRIOR_IDS, re-read after nr_pdsch_solver_reset() */
/* cell prior: the first solution, offered first to other RNTIs */
static bool g_have_prior;
static int g_prior_rep;
static uint8_t g_prior_add_pos;
static int8_t g_prior_table, g_prior_S_by_tda[16];

/* nr_pdsch_solver_enabled: Callers: nr_pdsch_passive_decode.c (probe-c1 switch), nr_pdcch_blind_monitor_rt.c (RNTI routing). */
bool nr_pdsch_solver_enabled(void)
{
  static int en = -1;
  if (en < 0) {
    const char *e = getenv("ISAC_PDSCH_SOLVER");
    en = (e && atoi(e) != 0) ? 1 : 0;
  }
  return en == 1;
}

/* nr_pdsch_solver_set_tda_fn: installs the TDRA-to-start-symbol callback and its context used to map a DCI payload to a time-domain allocation index.
 *   Takes g_mu; the callback is later invoked by tda_of() under the same lock.
 *   Caller: nr_pdcch_blind_monitor_rt.c (solver_tda_of, per monitor configuration).
 */
void nr_pdsch_solver_set_tda_fn(nr_pdsch_solver_tda_fn fn, void *ctx)
{
  pthread_mutex_lock(&g_mu);
  g_tda_fn = fn;
  g_tda_ctx = ctx;
  pthread_mutex_unlock(&g_mu);
}

/* nr_pdsch_solver_reset: Frees per-RNTI candidate bitmaps and coverage tables under g_mu. Resets the NL/coverage/layout-recover settings;
 *   the other env caches are function-static and are not re-read. Callers: tests/nr_pdsch_solver_test.cc only.
 */
void nr_pdsch_solver_reset(void)
{
  pthread_mutex_lock(&g_mu);
  g_nl_pref_gap = -1;
  g_coverage = -1;
  g_layout_recover = -1;
  g_fairshare = -1;
  g_reuse = -1;
  g_prior_crc = -1;
  g_signif = -1;
  g_prior_ids = -1;
  for (int i = 0; i < SOLVER_RNTIS; i++) {
    free(g_r[i].cand_bm);
    if (g_r[i].coverage) free(g_r[i].coverage->entries);
    free(g_r[i].coverage);
    memset(&g_r[i], 0, sizeof(g_r[i]));
  }
  memset(g_p, 0, sizeof(g_p));
  memset(&g_st, 0, sizeof(g_st));
  g_have_prior = false;
  g_n_key_scr = 0;
  pthread_mutex_unlock(&g_mu);
}

/* ---- spec tables ---- */
static const uint16_t kQR[3][32][2] = { /* Qm, R*1024*2 (R x 2048 keeps the .5 entries exact) */
  {{2,240},{2,314},{2,386},{2,502},{2,616},{2,758},{2,898},{2,1052},{2,1204},{2,1358},{4,680},{4,756},{4,868},{4,980},
   {4,1106},{4,1232},{4,1316},{6,876},{6,932},{6,1034},{6,1134},{6,1232},{6,1332},{6,1438},{6,1544},{6,1644},{6,1746},
   {6,1820},{6,1896}},
  {{2,240},{2,386},{2,616},{2,898},{2,1204},{4,756},{4,868},{4,980},{4,1106},{4,1232},{4,1316},{6,932},{6,1034},
   {6,1134},{6,1232},{6,1332},{6,1438},{6,1544},{6,1644},{6,1746},{8,1365},{8,1422},{8,1508},{8,1594},{8,1682},
   {8,1770},{8,1833},{8,1896}},
  {{2,60},{2,80},{2,100},{2,128},{2,156},{2,198},{2,240},{2,314},{2,386},{2,502},{2,616},{2,758},{2,898},{2,1052},
   {2,1204},{4,680},{4,756},{4,868},{4,980},{4,1106},{4,1232},{6,876},{6,932},{6,1034},{6,1134},{6,1232},{6,1332},
   {6,1438},{6,1544}}};
static const int kQRn[3] = {29, 28, 29};

/* nr_pdsch_solver_mcs_qr: looks up modulation order Qm and code rate R x 2048 for an MCS index in one of the three PDSCH MCS tables (0..2).
 *   Returns false for an out-of-range table or MCS; otherwise writes *qm and *r2 from kQR.
 *   Callers: solver planning (table filtering), nr_pdcch_blind_monitor_rt.c (SNR gate), tests.
 */
bool nr_pdsch_solver_mcs_qr(int table, int mcs, int *qm, int *r2)
{
  if (table < 0 || table > 2 || mcs < 0 || mcs >= kQRn[table])
    return false;
  *qm = kQR[table][mcs][0];
  *r2 = kQR[table][mcs][1];
  return true;
}

/* nr_pdsch_solver_dmrs_mask: returns the DM-RS symbol bitmask (bit per OFDM symbol) for a type-A PDSCH with first DM-RS symbol l0, start S and length L.
 *   Adds the additional DM-RS positions for add_pos 1..3 from a small table indexed by duration class (8-9, 10-12, 13-14 symbols).
 *   Pure function. Callers: emit() in this file, nr_pdsch_passive_decode.c (DM-RS mask of a decode).
 */
uint16_t nr_pdsch_solver_dmrs_mask(int l0, int S, int L, int add_pos)
{
  const int ld = S + L; /* type A: duration counted from the slot start */
  uint16_t m = (uint16_t)(1u << l0);
  if (ld < 8 || add_pos == 0)
    return m;
  static const int8_t pos[3][3][4] = { /* [ld 8-9 / 10-12 / 13-14][add_pos-1][...] -1 = none */
    {{7, -1, -1, -1}, {7, -1, -1, -1}, {7, -1, -1, -1}},
    {{9, -1, -1, -1}, {6, 9, -1, -1}, {5, 8, -1, -1}},
    {{11, -1, -1, -1}, {7, 11, -1, -1}, {5, 8, 11, -1}}};
  const int b = ld <= 9 ? 0 : ld <= 12 ? 1 : 2;
  for (int k = 0; k < 4 && pos[b][add_pos - 1][k] >= 0; k++)
    m |= (uint16_t)(1u << pos[b][add_pos - 1][k]);
  return m;
}

/* tables_for: lists the MCS tables (priority qam256, then 64QAM, then 16QAM) whose (Qm, R) pair differs for one MCS index.
 *   Duplicates (same Qm and R) are dropped, so a result of one table means the MCS is unambiguous.
 *   Writes table ids to t[] and returns their count (0..3). Callers: plan_known(), plan_coverage(), nr_pdsch_solver_plan_slot().
 */
static int tables_for(int mcs, uint8_t *t)
{
  static const uint8_t prio[3] = {1, 0, 2};
  int n = 0, q[3], r[3];
  for (int k = 0; k < 3; k++) {
    int qm, r2;
    if (!nr_pdsch_solver_mcs_qr(prio[k], mcs, &qm, &r2))
      continue;
    bool dup = false;
    for (int j = 0; j < n; j++)
      dup |= q[j] == qm && r[j] == r2;
    if (dup)
      continue;
    q[n] = qm; r[n] = r2; t[n++] = prio[k];
  }
  return n;
}

/* slot_of: finds the RNTI entry for rnti, optionally allocating a free one (create=true) and initialising its defaults (rep/table/rm/scr = -1).
 *   Linear scan over SOLVER_RNTIS entries; returns NULL when absent (or table full). idx, if given, receives the entry index.
 *   Caller: all solver entry points; g_mu must be held.
 */
static solver_rnti_t *slot_of(uint16_t rnti, bool create, int *idx)
{
  for (int i = 0; i < SOLVER_RNTIS; i++)
    if (g_r[i].used && g_r[i].rnti == rnti) { if (idx) *idx = i; return &g_r[i]; }
  if (!create)
    return NULL;
  for (int i = 0; i < SOLVER_RNTIS; i++)
    if (!g_r[i].used) {
      memset(&g_r[i], 0, sizeof(g_r[i]));
      g_r[i].used = true;
      g_r[i].rnti = rnti;
      g_r[i].rep = -1;
      g_r[i].table = -1;
      g_r[i].rm = -1;
      g_r[i].scr = g_r[i].cand_scr = -1;
      memset(g_r[i].S_by_tda, -1, sizeof(g_r[i].S_by_tda));
      if (idx) *idx = i;
      return &g_r[i];
    }
  /* SOLVERCOVER ISAC_PDSCH_SOLVER_FAIRSHARE: a full table used to refuse every new RNTI for the rest of the run (no entry
   * is ever freed: RNTI churn over minutes exceeds 64). Reuse the least recently active entry idle for >= 2 s of signal. */
  int lru = -1;
  const uint64_t now = plan_ctx()->now_abs;
  for (int i = 0; nr_pdsch_solver_fairshare_on() && i < SOLVER_RNTIS; i++)
    if (now >= g_r[i].last_abs + 4000 && (lru < 0 || g_r[i].last_abs < g_r[lru].last_abs)) lru = i;
  if (lru < 0)
    return NULL;
  for (int k = 0; k < SOLVER_PENDING; k++) /* its in-flight variants must not credit the new RNTI */
    if (g_p[k].used && g_p[k].slot == lru) g_p[k].used = false;
  LOG_A(PHY, "SENSING: PDSCH_SOLVER table full: evicting rnti=0x%04x (idle %.1f s, solved=%d) for rnti=0x%04x\n", g_r[lru].rnti,
        0.0005 * (double)(now - g_r[lru].last_abs), (int)g_r[lru].solved, rnti);
  free(g_r[lru].cand_bm);
  if (g_r[lru].coverage) free(g_r[lru].coverage->entries);
  free(g_r[lru].coverage);
  memset(&g_r[lru], 0, sizeof(g_r[lru]));
  g_r[lru].used = false;
  return slot_of(rnti, true, idx);
}

/* nr_pdsch_solver_reopen_layouts: returns true once an unsolved RNTI has been reopened to the full layout set after too many failed CRC probes.
 *   Reads ISAC_PDSCH_SOLVER_LAYOUT_RECOVER (threshold, 0 = off) on first use; sets layouts_reopened and logs the event.
 *   Caller: nr_pdcch_blind_monitor_rt.c (layout pruning). Takes g_mu.
 */
bool nr_pdsch_solver_reopen_layouts(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  if (g_layout_recover < 0) {
    const char *e = getenv("ISAC_PDSCH_SOLVER_LAYOUT_RECOVER");
    g_layout_recover = e ? atoi(e) : 0;
    if (g_layout_recover < 0) g_layout_recover = 0;
  }
  solver_rnti_t *r = slot_of(rnti, false, NULL);
  if (g_layout_recover > 0 && r && !r->solved && !r->layouts_reopened
      && r->layout_failures >= (uint32_t)g_layout_recover) {
    r->layouts_reopened = true;
    LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x reopening pruned layouts after %u failed CRC probes\n",
          rnti, r->layout_failures);
  }
  const bool reopened = g_layout_recover > 0 && r && r->layouts_reopened;
  pthread_mutex_unlock(&g_mu);
  return reopened;
}

/* nr_pdsch_solver_rep: returns the representative layout index of a solved RNTI, or -1 when the RNTI is unknown or unsolved.
 *   Read-only under g_mu. Callers: nr_pdcch_blind_monitor_rt.c, tests/nr_pdsch_solver_test.cc.
 */
int nr_pdsch_solver_rep(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  const int rep = (r && r->solved) ? r->rep : -1;
  pthread_mutex_unlock(&g_mu);
  return rep;
}

bool nr_pdsch_solver_fairshare_on(void)
{
  if (g_fairshare < 0) g_fairshare = getenv("ISAC_PDSCH_SOLVER_FAIRSHARE") && atoi(getenv("ISAC_PDSCH_SOLVER_FAIRSHARE")) ? 1 : 0;
  return g_fairshare == 1;
}

/* prior_crc_on: TIMBIMODAL ISAC_PDSCH_SOLVER_PRIOR_CRC=1 (default off): an RNTI's solution seeds the other RNTIs (cell prior,
 *   CORESET scrambling, REUSE) only after it decoded a full TB with CRC OK. Two chance CRC-16 probe passes (tiny low-rate TBs)
 *   once "solved" OTA TIM run ab_Tr2_1_094729 on a wrong layout, and that prior and scrambling stayed for the whole run. */
static bool prior_crc_on(void)
{
  if (g_prior_crc < 0) g_prior_crc = getenv("ISAC_PDSCH_SOLVER_PRIOR_CRC") && atoi(getenv("ISAC_PDSCH_SOLVER_PRIOR_CRC")) ? 1 : 0;
  return g_prior_crc == 1;
}

/* promote: makes a solved RNTI's solution the cell prior (if none yet) and its scrambling its CORESET's (if none yet). Under g_mu. */
static void promote(const solver_rnti_t *r)
{
  int have = 0;
  for (int q = 0; q < g_n_key_scr && !have; q++) have = g_key_scr[q].key == r->key;
  if (!have && g_n_key_scr < 16) { g_key_scr[g_n_key_scr].key = r->key; g_key_scr[g_n_key_scr++].scr = r->scr; }
  if (!g_have_prior) {
    g_have_prior = true;
    g_prior_rep = r->rep;
    g_prior_add_pos = r->add_pos;
    g_prior_table = r->table;
    memcpy(g_prior_S_by_tda, r->S_by_tda, sizeof(g_prior_S_by_tda));
  }
}

/* TIMSOLVE2 ISAC_PDSCH_SOLVER_SIGNIF=1 (default off): an RNTI solves only on statistically significant CRC evidence. A wrong
 * hypothesis passes a CRC16 code block (TBS <= 3824, TS 38.212 7.2.1) with probability 2^-16 and a CRC24 one with 2^-24.
 * Bonferroni over every hypothesis tried: under "all wrong" the passes of an RNTI are Poisson with mean cred16 2^-16 (CRC16)
 * and cred24 2^-24 (CRC24), cred = its credited search probes so far. The candidate's consistent passes (distinct DCIs)
 * solve when P(at least that many chance passes) < SIGNIF_ALPHA. So one CRC24 pass solves at once, two CRC16 passes solve
 * a quiet RNTI, and a busy one (thousands of probes: OTA ab_Tr2_1_094729 0x47f0, solved on 2 chance passes) needs a third.
 * SIGNIF_ALPHA is a false-solve rate per RNTI, not a cell parameter. */
#define SIGNIF_ALPHA 1e-3
static bool signif_on(void)
{
  if (g_signif < 0) g_signif = getenv("ISAC_PDSCH_SOLVER_SIGNIF") && atoi(getenv("ISAC_PDSCH_SOLVER_SIGNIF")) ? 1 : 0;
  return g_signif == 1;
}
/* poisson_tail: P(X >= k), X ~ Poisson(lam); summed upwards (no 1 - small cancellation). */
static double poisson_tail(int k, double lam)
{
  if (k <= 0) return 1.0;
  double t = exp(-lam), s = 0.0;
  for (int j = 0; j < k + 40; j++) {
    if (j >= k) s += t;
    t *= lam / (j + 1);
  }
  return s;
}
/* chance_p: probability that an RNTI with all hypotheses wrong shows the candidate's passes by chance (see signif_on). */
static double chance_p(const solver_rnti_t *r)
{
  return poisson_tail(r->k16, r->cred16 * 0x1p-16) * poisson_tail(r->k24, r->cred24 * 0x1p-24);
}
/* significant: CRC16-only evidence needs >= 2 passes on distinct DCIs (and the Poisson bound); one CRC24 pass may suffice. */
static bool significant(const solver_rnti_t *r)
{
  return (r->k24 >= 1 || r->k16 >= 2) && chance_p(r) < SIGNIF_ALPHA;
}
/* tb_crc24: TS 38.214 5.1.3.2: TBS > 3824 (CRC24A, or code block 0 with CRC24B) iff N_info > 3824. The DM-RS overhead is
 * taken as 12 REs per DM-RS symbol and PRB (2 CDM groups): over-counting it can only under-estimate N_info, so an unsure
 * TB counts as CRC16 (the stricter rule). */
static bool tb_crc24(int table, int mcs, int nrb, int S, int l0, int add_pos, int nl)
{
  int qm, r2;
  if (nrb <= 0 || !nr_pdsch_solver_mcs_qr(table, mcs, &qm, &r2)) return false;
  const int L = 14 - S, nd = __builtin_popcount(nr_pdsch_solver_dmrs_mask(l0, S, L, add_pos));
  int nre = 12 * L - 12 * nd;
  if (nre > 156) nre = 156;
  return (double)nre * nrb * (r2 / 2048.0) * qm * (nl > 0 ? nl : 1) > 3824.0;
}

/* TIMSOLVE2 ISAC_PDSCH_SOLVER_PRIOR_IDS=1 (default off): reuse the CRC-proven solutions' layout dimensions (reading, S map,
 * add_pos, MCS table) on every grant of an unsolved RNTI with each of ITS OWN blind identities, in the identity form
 * (data scrambling = fixed id / = DM-RS id / default) the proven solutions use. Replaces the single per-grant scrambling
 * hypothesis of the cell prior. OTA TIM (ab_Tt1/Tt3, 2026-10-11): after the first solve, 73-78 % of the probes of RNTIs
 * that never decoded went to identity forms or dimensions no solved RNTI of the cell uses. */
static bool prior_ids_on(void)
{
  if (g_prior_ids < 0) g_prior_ids = getenv("ISAC_PDSCH_SOLVER_PRIOR_IDS") && atoi(getenv("ISAC_PDSCH_SOLVER_PRIOR_IDS")) ? 1 : 0;
  return g_prior_ids == 1;
}
/* scr_form: identity form of a scrambling hypothesis: -1 default ids, 0 data id = DM-RS id, D + 1 data id = D. */
static int scr_form(int32_t scr) { return scr < 0 ? -1 : (scr >> 16); }

/* nr_pdsch_solver_fair_budget: ISAC_PDSCH_SOLVER_FAIRSHARE=1: the probe rate is divided equally among the unsolved RNTIs
 * active in the last 0.2 s (a per-RNTI token bucket of rate/n_active, capped at `budget`), so a busy RNTI whose search
 * keeps failing cannot take the whole probe budget from the others (replay TIM 12 s: one RNTI planned 65 % of all probes,
 * the others got no budget on up to half of their grants: its probes kept the decode ring above the admission fill). A
 * new RNTI starts with a full budget (prompt cold start). Work-conserving: the share caps only when the bucket cannot
 * refill to its burst now (contended); with spare capacity every RNTI plans its full budget (replay TIM 12 s
 * at 0.25x: an always-on cap left capacity unused and solved fewer RNTIs). Returns the probes this grant may plan. */
int nr_pdsch_solver_fair_budget(uint16_t rnti, double rate_per_slot, bool contended, int budget)
{
  if (!nr_pdsch_solver_fairshare_on() || budget <= 0 || rate_per_slot <= 0.0)
    return budget;
  pthread_mutex_lock(&g_mu);
  const uint64_t now = plan_ctx()->now_abs;
  solver_rnti_t *r = slot_of(rnti, true, NULL);
  int out = budget;
  if (r && !r->solved) {
    r->last_abs = now;
    int n_active = 0;
    for (int i = 0; i < SOLVER_RNTIS; i++)
      if (g_r[i].used && !g_r[i].solved && now <= g_r[i].last_abs + 400) n_active++;
    if (n_active < 1) n_active = 1;
    if (r->fc_abs == 0) r->fcredit = budget;
    else if (now > r->fc_abs) r->fcredit += rate_per_slot / n_active * (double)(now - r->fc_abs);
    if (r->fcredit > budget) r->fcredit = budget;
    r->fc_abs = now ? now : 1;
    if (contended) out = r->fcredit >= 1.0 ? (int)r->fcredit : 0; /* work-conserving: cap only under contention */
    if (out > budget) out = budget;
  }
  pthread_mutex_unlock(&g_mu);
  return out;
}

/* nr_pdsch_solver_note_skip: counts one caller-side reason why a grant of rnti was not planned / a variant not decoded. */
void nr_pdsch_solver_note_skip(uint16_t rnti, int why)
{
  if (why < 0 || why >= NR_PDSCH_SOLVER_WHY_CALLER_N) return;
  static int s_why = -1; /* only ISAC_PDSCH_SOLVER_WHY=1 lets a never-planned RNTI take a table entry (flag off = unchanged) */
  if (s_why < 0) s_why = getenv("ISAC_PDSCH_SOLVER_WHY") && atoi(getenv("ISAC_PDSCH_SOLVER_WHY")) ? 1 : 0;
  pthread_mutex_lock(&g_mu);
  solver_rnti_t *r = slot_of(rnti, s_why == 1, NULL);
  if (r) r->why[why]++;
  pthread_mutex_unlock(&g_mu);
}

/* nr_pdsch_solver_note_snr: feeds one post-equalisation SNR sample (dB) of a full decode into the RNTI's exponential moving average.
 *   EMA weight 0.2 for new samples; the first sample initialises it. Ignored for unknown RNTIs.
 *   Caller: nr_pdsch_passive_queue.c (after a full decode, from its EVM).
 */
void nr_pdsch_solver_note_snr(uint16_t rnti, float snr_db)
{
  pthread_mutex_lock(&g_mu);
  solver_rnti_t *r = slot_of(rnti, false, NULL);
  if (r) {
    r->snr_ema = r->n_snr ? 0.8f * r->snr_ema + 0.2f * snr_db : snr_db;
    r->n_snr++;
  }
  pthread_mutex_unlock(&g_mu);
}

/* nr_pdsch_solver_snr: returns the SNR EMA of an RNTI in *snr_db, valid only after at least 3 full-decode samples.
 *   Returns false when the RNTI is unknown or has too few samples. Takes g_mu.
 *   Caller: nr_pdcch_blind_monitor_rt.c (SNR gate on the modulation choice).
 */
bool nr_pdsch_solver_snr(uint16_t rnti, float *snr_db)
{
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  const bool ok = r && r->n_snr >= 3;
  if (ok) *snr_db = r->snr_ema;
  pthread_mutex_unlock(&g_mu);
  return ok;
}

/* nr_pdsch_solver_set_alt_scr: publishes the scrambling-id hypotheses (ids[0..n-1], at most 16) of the CORESET of the next grant, with a hash of them.
 *   Enabled only with ISAC_PDSCH_SOLVER_SCR=1; otherwise it just records the CORESET key and the decided flag.
 *   Writes the plan context (per-thread or global, see plan_ctx). Caller: nr_pdcch_blind_monitor_rt.c before planning.
 */
void nr_pdsch_solver_set_alt_scr(int key, const uint16_t *ids, int n, bool first_is_decided)
{
  static _Atomic int s_en = -1;
  int en = atomic_load(&s_en);
  if (en < 0) {
    en = getenv("ISAC_PDSCH_SOLVER_SCR") && atoi(getenv("ISAC_PDSCH_SOLVER_SCR")) ? 1 : 0;
    atomic_store(&s_en, en);
  }
  if (!solver_thread_ctx_enabled()) pthread_mutex_lock(&g_mu);
  solver_plan_ctx_t *ctx = plan_ctx();
  ctx->n = 0;
  ctx->key = key;
  uint32_t h = 2166136261u;
  for (int i = 0; en && i < n && ctx->n < 16; i++) {
    ctx->ids[ctx->n++] = ids[i];
    h = (h ^ ids[i]) * 16777619u;
  }
  ctx->hash = h ^ (uint32_t)ctx->n;
  ctx->decided = first_is_decided && ctx->n > 0;
  ctx->own = false;
  if (!solver_thread_ctx_enabled()) pthread_mutex_unlock(&g_mu);
}

void nr_pdsch_solver_set_alt_scr32(int key, const int32_t *ids, int n, bool first_is_decided, bool own)
{
  if (!solver_thread_ctx_enabled()) pthread_mutex_lock(&g_mu);
  solver_plan_ctx_t *ctx = plan_ctx();
  ctx->n = 0;
  ctx->key = key;
  uint32_t h = 2166136261u;
  for (int i = 0; i < n && ctx->n < 16; i++) {
    ctx->ids[ctx->n++] = ids[i];
    h = (h ^ (uint32_t)ids[i]) * 16777619u;
  }
  ctx->hash = h ^ (uint32_t)ctx->n;
  ctx->decided = first_is_decided && ctx->n > 0;
  ctx->own = own && ctx->n > 0;
  if (!solver_thread_ctx_enabled()) pthread_mutex_unlock(&g_mu);
}

/* nr_pdsch_solver_set_now: sets the absolute slot time (abs_slot, signal time) of the grant about to be planned, used for solve-time statistics.
 *   Writes the plan context under g_mu (unless thread-context mode). Caller: nr_pdcch_blind_monitor_rt.c.
 */
void nr_pdsch_solver_set_now(uint64_t abs_slot)
{
  if (!solver_thread_ctx_enabled()) pthread_mutex_lock(&g_mu);
  plan_ctx()->now_abs = abs_slot;
  if (!solver_thread_ctx_enabled()) pthread_mutex_unlock(&g_mu);
}

/* nr_pdsch_solver_owns: reports whether the solver takes over the grants of this RNTI; currently it is simply the global enable flag.
 *   The rnti argument is not used yet. Caller: declared in the header, no call site outside the header.
 */
bool nr_pdsch_solver_owns(uint16_t rnti)
{
  return nr_pdsch_solver_enabled();
}

/* tda_of: maps a DCI payload to its TDRA index (0..15) for a given layout, through the registered callback.
 *   Returns 0 when no callback is registered. Called under g_mu from plan and feedback paths.
 */
static int tda_of(int layout, nr_dci_bits_t p)
{
  return g_tda_fn ? (g_tda_fn(g_tda_ctx, (uint16_t)layout, p) & 15) : 0;
}

/* pend_new: allocates the next pending-grant slot from a FIFO ring of SOLVER_PENDING entries, overwriting the oldest.
 *   Counts overwritten live entries in g_st.dropped (their CRC feedback will be lost). Caller: emit(), under g_mu.
 */
static pending_t *pend_new(void)
{
  static uint32_t cur;
  pending_t *p = &g_p[cur]; /* FIFO: the oldest entry is the one most likely lost in the queue */
  if (p->used) g_st.dropped++;
  cur = (cur + 1) % SOLVER_PENDING;
  return p;
}

static int t_rm = 0; static bool t_learn_rm = false; /* set by plan for this grant (under g_mu) */
static int32_t t_scr = -1; /* T26c scrambling hypothesis for this grant (-1 default) */
static uint8_t t_comb; /* T26d combining mode for this grant's variants (0 = default) */
/* comb_on: returns the receiver-combining mode ISAC_RX_MRC_PER_RNTI (cached): 0 off, 1 {best, MRC}, 2 adds NW-MRC and IRC, 3 uses IRC with DM-RS covariance.
 *   Read once from the environment. Used by comb_n(), comb_k(), comb_pick() and the feedback counters.
 */
static int comb_on(void)
{
  static int en = -1;
  if (en < 0) en = getenv("ISAC_RX_MRC_PER_RNTI") ? atoi(getenv("ISAC_RX_MRC_PER_RNTI")) : 0;
  return en;
}
/* Mode for this grant: unsolved -> alternate 1/3 by grant parity; solved -> the mode with the better CRC rate
 * (Laplace), exploring the other on 1 grant in 8. */
/* receivers in play: ISAC_RX_MRC_PER_RNTI=1 -> {1,3}; =2 -> {1,3,4,5} (4 = noise-weighted gain-corrected MRC, 5 = IRC) */
/* comb_k: maps a receiver index k (0..3) to the receiver id stored in the pending entry: 1 best branch, 3 MRC, 4 NW-MRC, 5 IRC (6 at mode 3).
 *   Callers: comb_pick().
 */
static uint8_t comb_k(int k) { static const uint8_t kComb[4] = {1, 3, 4, 5}; return (k == 3 && comb_on() >= 3) ? 6 : kComb[k]; }
/* comb_n: returns how many receivers take part in the per-RNTI choice: 4 with ISAC_RX_MRC_PER_RNTI >= 2, otherwise 2.
 *   Caller: comb_pick().
 */
static int comb_n(void) { return comb_on() >= 2 ? 4 : 2; }
/* comb_ab: returns ISAC_RX_MRC_PER_RNTI_AB (cached): 1 selects a fixed round robin over the receivers instead of exploitation.
 *   Caller: comb_pick().
 */
static int comb_ab(void) /* ISAC_RX_MRC_PER_RNTI_AB=1: post-solve round robin over the receivers (paired A/B, no exploitation) */
{
  static int en = -1;
  if (en < 0) en = getenv("ISAC_RX_MRC_PER_RNTI_AB") ? atoi(getenv("ISAC_RX_MRC_PER_RNTI_AB")) : 0;
  return en;
}
/* comb_pick: chooses the receiver id for one grant of an RNTI. Unsolved RNTIs always use the configured receiver (0).
 *   Solved RNTIs: best receiver by Laplace-smoothed CRC rate, exploring another one on 1 grant in 8 (or round robin with _AB).
 *   Uses r->comb_try/comb_ok counters filled by nr_pdsch_solver_feedback(). Caller: nr_pdsch_solver_plan_slot().
 */
static uint8_t comb_pick(const solver_rnti_t *r)
{
  if (!comb_on()) return 0;
  const int n = comb_n();
  if (!r->solved) return 0; /* the layout search runs on the configured receiver only (alternating receivers while
                            * unsolved multiplied the search space and starved whole populations: replays n3/n4) */
  if (comb_ab()) return comb_k((int)(r->grants % (uint32_t)n));
  int best = 0;
  double bp = -1.0;
  for (int k = 0; k < n; k++) {
    const double p = (r->comb_ok[k] + 1.0) / (r->comb_try[k] + 2.0);
    if (p > bp) { bp = p; best = k; }
  }
  if ((r->grants % 8u) == 7u) return comb_k((int)((best + 1 + (r->grants / 8u) % (uint32_t)(n - 1)) % (uint32_t)n)); /* explore */
  return comb_k(best);
}
/* emit: appends one decode variant (reading, S, add_pos, table, probe flag, TDA) to out[] and books a pending entry carrying its feedback token.
 *   Skips the variant when S > l0 (DM-RS before the PDSCH start), when out[] is full, or when it duplicates an entry already in out[].
 *   Fills the variant's DM-RS mask and copies the reading's layouts into the pending slot. Caller: the plan functions; g_mu held.
 */
static bool emit(nr_pdsch_solver_var_t *out, int *n, int max, int slot, uint8_t phase, int reading,
                 const nr_pdsch_solver_reading_t *rd, int l0, int S, int add_pos, int table, bool probe, bool table_disc,
                 bool learn_S, bool learn_table, int tda, uint64_t dci, nr_dci_bits_t payload)
{
  if (*n >= max)
    return false;
  if (S > l0) /* type A: the first DM-RS symbol l0 must lie inside the PDSCH (S <= l0); else the demapper overruns */
    return true;
  for (int i = 0; i < *n; i++) /* never the same decode twice for one grant */
    if (out[i].reading == reading && out[i].S == S && out[i].add_pos == add_pos && out[i].table == table && out[i].rm == t_rm
        && out[i].scr == t_scr)
      return true;
  pending_t *p = pend_new();
  memset(p, 0, offsetof(pending_t, ids));
  p->used = true;
  p->token = ((g_token++ & 0xFFFFFu) << 12) | (uint32_t)(p - g_p); /* never 0: g_token starts at 1 and skips 0 mod 2^20 */
  if ((g_token & 0xFFFFFu) == 0) g_token++;
  p->abs = plan_ctx()->now_abs;
  p->slot = slot;
  p->phase = phase;
  p->S = (uint8_t)S; p->add_pos = (uint8_t)add_pos; p->table = (uint8_t)table; p->probe = probe;
  p->table_disc = table_disc; p->learn_S = learn_S; p->learn_table = learn_table; p->tda = tda;
  p->dci = dci;
  p->rm = (uint8_t)t_rm;
  p->scr = t_scr;
  p->comb = t_comb;
  p->n_layers = rd[reading].n_layers;
  p->mcs = rd[reading].mcs;
  p->crc24 = tb_crc24(table, rd[reading].mcs, rd[reading].num_rb, S, l0, add_pos, rd[reading].n_layers);
  p->dmrs_ports = rd[reading].dmrs_ports;
  p->learn_rm = t_learn_rm;
  p->payload = payload;
  p->n_ids = rd[reading].n_ids < SOLVER_MAX_IDS ? rd[reading].n_ids : SOLVER_MAX_IDS;
  memcpy(p->ids, rd[reading].ids, (size_t)p->n_ids * sizeof(uint16_t));
  nr_pdsch_solver_var_t *v = &out[(*n)++];
  v->reading = reading;
  v->S = (uint8_t)S;
  v->L = (uint8_t)(14 - S);
  v->add_pos = (uint8_t)add_pos;
  v->table = (uint8_t)table;
  v->dmrs_mask = nr_pdsch_solver_dmrs_mask(l0, S, 14 - S, add_pos);
  v->probe = probe;
  v->token = p->token;
  v->rm = (uint8_t)t_rm;
  v->scr = t_scr;
  v->comb = t_comb;
  return true;
}

/* probe_hog: ISAC_PDSCH_SOLVER_FAIR=1 gate: true when this unsolved RNTI already spent more than twice the mean probe count of
 *   all unsolved RNTIs (plus slack). An RNTI with a candidate is never throttled. Caller: nr_pdsch_solver_plan_slot().
 */
static bool probe_hog(const solver_rnti_t *r)
{
  static int en = -1;
  if (en < 0) en = getenv("ISAC_PDSCH_SOLVER_FAIR") && atoi(getenv("ISAC_PDSCH_SOLVER_FAIR")) ? 1 : 0;
  if (!en || r->has_cand) return false;
  uint64_t tot = 0;
  int n = 0;
  for (int i = 0; i < SOLVER_RNTIS; i++)
    if (g_r[i].used && !g_r[i].solved) { tot += g_r[i].n_probe; n++; }
  return n > 1 && (uint64_t)r->n_probe * n > 2 * tot + 32u * (uint64_t)n;
}

/* leader_ok: ISAC_PDSCH_SOLVER_LEADER gate: whether this unsolved RNTI may search on its CORESET (true when the CORESET already has a solved RNTI).
 *   Otherwise only the busiest unsolved RNTI of the same CORESET (highest grant count) searches. Off: always true.
 *   Caller: nr_pdsch_solver_plan_slot().
 */
static bool leader_ok(const solver_rnti_t *r)
{
  static int en = -1;
  if (en < 0) en = getenv("ISAC_PDSCH_SOLVER_LEADER") && atoi(getenv("ISAC_PDSCH_SOLVER_LEADER")) ? 1 : 0;
  if (!en) return true;
  for (int i = 0; i < SOLVER_RNTIS; i++) /* any solved RNTI on the same CORESET: everybody searches (with the prior) */
    if (g_r[i].used && g_r[i].solved && g_r[i].key == r->key) return true;
  const solver_rnti_t *lead = NULL;
  for (int i = 0; i < SOLVER_RNTIS; i++)
    if (g_r[i].used && !g_r[i].solved && g_r[i].key == r->key && (!lead || g_r[i].grants > lead->grants)) lead = &g_r[i];
  return lead == NULL || lead == r;
}

/* search_pref_skip: ISAC_PDSCH_SOLVER_PREF=N: true when this grant should be skipped, so that an unsolved RNTI searches only on low-MCS grants.
 *   MCS <= ISAC_PDSCH_SOLVER_PREF_MCS (default 4) always searches; others are skipped until N grants have passed (pref_gap counter).
 *   Legacy MCS-only preference, used only when ISAC_PDSCH_SOLVER_NL_PREF is off. Caller: nr_pdsch_solver_plan_slot().
 */
static bool search_pref_skip(solver_rnti_t *r, int mcs)
{
  static int n_gap = -1, mcs_max = 4;
  if (n_gap < 0) {
    n_gap = getenv("ISAC_PDSCH_SOLVER_PREF") ? atoi(getenv("ISAC_PDSCH_SOLVER_PREF")) : 0;
    if (getenv("ISAC_PDSCH_SOLVER_PREF_MCS")) mcs_max = atoi(getenv("ISAC_PDSCH_SOLVER_PREF_MCS"));
  }
  if (n_gap <= 0 || r->has_cand) return false;
  if (mcs <= mcs_max) { r->pref_gap = 0; return false; }
  if (++r->pref_gap >= (uint32_t)n_gap) { r->pref_gap = 0; return false; }
  return true;
}

/* nl_pref_gap: returns ISAC_PDSCH_SOLVER_NL_PREF (cached), the grant gap after which the one-layer-QPSK preference is dropped; 0 = off.
 *   Caller: nr_pdsch_solver_plan_slot() and plan_known().
 */
static int nl_pref_gap(void)
{
  if (g_nl_pref_gap < 0)
    g_nl_pref_gap = getenv("ISAC_PDSCH_SOLVER_NL_PREF") ? atoi(getenv("ISAC_PDSCH_SOLVER_NL_PREF")) : 0;
  return g_nl_pref_gap;
}

/* reading_nl_qpsk: true when a reading is single-layer and its MCS is QPSK (Qm = 2) in at least one MCS table.
 *   Used to prefer readings that can decode at the measured SNR. Callers: nr_pdsch_solver_plan_slot().
 */
static bool reading_nl_qpsk(const nr_pdsch_solver_reading_t *rd)
{
  if (rd->n_layers != 1) return false;
  for (int table = 0; table < 3; table++) {
    int qm, r2;
    if (nr_pdsch_solver_mcs_qr(table, rd->mcs, &qm, &r2) && qm == 2) return true;
  }
  return false;
}

/* search_backoff: ISAC_PDSCH_SOLVER_BACKOFF=N: an RNTI with no candidate and at least N probes spent is searched on 1 grant in 8 only.
 *   Limit is read once; 0 disables it. Caller: nr_pdsch_solver_plan_slot().
 */
static bool search_backoff(const solver_rnti_t *r)
{
  static int lim = -1;
  if (lim < 0) lim = getenv("ISAC_PDSCH_SOLVER_BACKOFF") ? atoi(getenv("ISAC_PDSCH_SOLVER_BACKOFF")) : 0;
  return lim > 0 && !r->has_cand && r->n_probe >= (uint32_t)lim && (r->grants % 8u) != 0;
}

static const uint8_t kS[4] = {1, 0, 2, 3};

/* has_id: true when a reading's layout list (ids) contains layout id. Linear scan over at most a few dozen ids.
 *   Caller: nr_pdsch_solver_plan_slot().
 */
static bool has_id(const nr_pdsch_solver_reading_t *r, int id)
{
  for (int i = 0; i < r->n_ids; i++)
    if (r->ids[i] == id) return true;
  return false;
}

/* coverage_find: returns the hash-table entry for a coverage key (linear probing), new or existing, for the RNTI's trial-count table.
 *   The caller must ensure room (coverage_room) first. Callers: plan_coverage(), coverage_room().
 */
static coverage_entry_t *coverage_find(solver_coverage_t *c, uint64_t key)
{
  uint64_t h = key * UINT64_C(0x9e3779b97f4a7c15);
  h = (h ^ (h >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  size_t i = (h ^ (h >> 27)) & (c->cap - 1);
  while (c->entries[i].key && c->entries[i].key != key) i = (i + 1) & (c->cap - 1);
  return &c->entries[i];
}

/* coverage_room: makes sure the RNTI's coverage hash table can take `extra` more keys, growing or resetting it as needed (max 2^18 entries).
 *   When the table is full at the maximum size it is cleared and a new coverage round starts. Returns false only on allocation failure.
 *   Caller: plan_coverage().
 */
static bool coverage_room(solver_coverage_t *c, size_t extra)
{
  if (c->cap && (c->used + extra) * 4 < c->cap * 3) return true;
  /* Bounded history: at most 4 MiB/RNTI, allocated only for visited hypotheses.
   * Exhaustion starts a new coverage round instead of making a hypothesis final. */
  if (c->cap == (1u << 18)) {
    memset(c->entries, 0, c->cap * sizeof(*c->entries));
    c->used = 0;
    return true;
  }
  solver_coverage_t grown = {.cap = c->cap ? c->cap * 2 : 4096, .used = c->used};
  while ((c->used + extra) * 4 >= grown.cap * 3 && grown.cap < (1u << 18)) grown.cap *= 2;
  grown.entries = calloc(grown.cap, sizeof(*grown.entries));
  if (!grown.entries) return false;
  for (size_t i = 0; i < c->cap; i++)
    if (c->entries[i].key) *coverage_find(&grown, c->entries[i].key) = c->entries[i];
  free(c->entries);
  *c = grown;
  return true;
}

/* coverage_key: builds the 64-bit coverage key for one physical hypothesis: scrambling id, layout id, TDA index and variant.
 *   variant 63 means the aggregate count over all timing cases. Pure function. Callers: plan_coverage().
 */
static uint64_t coverage_key(int id, int variant, nr_dci_bits_t payload)
{
  const int tda = variant == 63 ? 0 : tda_of(id, payload); /* 63 = all timing cases' trial count */
  return (((((uint64_t)(t_scr + 1) << 15) | (unsigned)id) << 4 | tda) << 6 | variant) + 1;
}

/* plan_coverage: ISAC_PDSCH_SOLVER_COVERAGE=1 planner: fills the grant's budget with the least-tried physical hypotheses across readings.
 *   Scores each (reading, S, add_pos, table) variant by how often it was tried, balances the budget per layout, and emits up to budget variants.
 *   Records every emitted trial in the RNTI's coverage table. Caller: nr_pdsch_solver_plan_slot() (unsolved RNTIs).
 */
static void plan_coverage(solver_rnti_t *r, nr_pdsch_solver_var_t *out, int *n, int max, int slot,
                          const nr_pdsch_solver_reading_t *rd, const int *order, int no,
                          int l0, bool qpsk, uint64_t dci, nr_dci_bits_t payload)
{
  if (!r->coverage) r->coverage = calloc(1, sizeof(*r->coverage));
  if (!r->coverage || !coverage_room(r->coverage, 2 * SOLVER_MAX_IDS)) return;
  solver_coverage_t *c = r->coverage;
  static const uint8_t starts[4] = {1, 0, 2, 3}, adds[4] = {1, 2, 0, 3}, tables[3] = {1, 0, 2};
  uint32_t score[128][48];
  uint32_t load[128];
  int nt[128];
  for (int k = 0; k < no; k++) {
    load[k] = UINT32_MAX;
    for (int j = 0; j < rd[order[k]].n_ids; j++) {
      const int id = rd[order[k]].ids[j];
      if (id >= NR_PDSCH_SOLVER_MAX_LAYOUTS) continue;
      const uint32_t count = coverage_find(c, coverage_key(id, 63, payload))->count;
      if (count < load[k]) load[k] = count;
    }
    uint8_t available[3];
    nt[k] = tables_for(rd[order[k]].mcs, available);
    for (int v = 0; v < 48; v++) {
      score[k][v] = UINT32_MAX;
      const int table = tables[(v / 4) % 3];
      bool valid = false;
      for (int t = 0; t < nt[k]; t++) valid |= available[t] == table;
      int qm, r2;
      if (!valid || starts[v % 4] > l0
          || (qpsk && nr_pdsch_solver_mcs_qr(table, rd[order[k]].mcs, &qm, &r2) && qm != 2)) continue;
      for (int j = 0; j < rd[order[k]].n_ids; j++) {
        const int id = rd[order[k]].ids[j];
        if (id >= NR_PDSCH_SOLVER_MAX_LAYOUTS) continue;
        const uint32_t count = coverage_find(c, coverage_key(id, v, payload))->count;
        if (count < score[k][v]) score[k][v] = count;
      }
    }
  }
  for (int step = 0; step < no * 48 && *n < max; step++) {
    int best = -1, variant = -1;
    uint32_t count = UINT32_MAX, total = UINT32_MAX;
    /* Fairness is per layout, not per TDRA: a wrong wide TDRA field must not get
     * sixteen times the budget of a narrow one merely by producing new indices.
     * Within that share choose the least-tried eligible physical hypothesis. */
    for (int v = 0; v < 48; v++)
      for (int k = 0; k < no; k++)
        if (score[k][v] != UINT32_MAX && (load[k] < total || (load[k] == total && score[k][v] < count))) {
          best = k; variant = v; count = score[k][v]; total = load[k];
        }
    if (best < 0 || !coverage_room(c, 2 * rd[order[best]].n_ids)) break;
    if (load[best] < UINT32_MAX - 1) load[best]++;
    score[best][variant] = UINT32_MAX; /* no duplicate physical decode in this DCI */
    const int S = starts[variant % 4], table = tables[(variant / 4) % 3], add = adds[variant / 12];
    emit(out, n, max, slot, 0, order[best], rd, l0, S, add, table, true, nt[best] > 1, false, false, -1, dci, payload);
    for (int j = 0; j < rd[order[best]].n_ids; j++) {
      const int id = rd[order[best]].ids[j];
      if (id >= NR_PDSCH_SOLVER_MAX_LAYOUTS) continue;
      for (int q = 0; q < 2; q++) {
        const uint64_t key = coverage_key(id, q ? 63 : variant, payload);
        coverage_entry_t *e = coverage_find(c, key);
        if (!e->key) { e->key = key; c->used++; }
        if (e->count < UINT32_MAX - 1) e->count++;
      }
    }
  }
}

/* plan_known: emits the variants for a known layout (the solution, the reused best solution, or the cell prior) for one reading.
 *   Uses the learnt S for the TDA index and table when known; otherwise tries the four start symbols and the candidate tables as probes.
 *   Phase 1 (solved) emits counted decodes; phase 0 emits probes. Callers: nr_pdsch_solver_plan_slot().
 */
static void plan_known(nr_pdsch_solver_var_t *out, int *n, int max, int slot, uint8_t phase, int reading,
                       const nr_pdsch_solver_reading_t *rd, int l0, int rep, uint8_t add_pos, int8_t table,
                       const int8_t *S_by_tda, int mcs, uint64_t dci, nr_dci_bits_t payload)
{
  uint8_t tl[3];
  const int nt = tables_for(nl_pref_gap() > 0 ? rd[reading].mcs : mcs, tl);
  if (nt == 0)
    return;
  const int tda = tda_of(rep, payload);
  const int S = S_by_tda[tda];
  const bool t_known = table >= 0 || nt == 1;
  const int t_use = table >= 0 ? table : tl[0];
  if (phase == 1 && S >= 0 && t_known) {
    emit(out, n, max, slot, phase, reading, rd, l0, S, add_pos, t_use, false, nt > 1, false, false, tda, dci, payload);
    return;
  }
  for (int si = 0; si < 4; si++) {
    const int s = S >= 0 ? S : kS[si];
    for (int ti = 0; ti < (t_known ? 1 : nt); ti++)
      emit(out, n, max, slot, phase, reading, rd, l0, s, add_pos, t_known ? t_use : tl[ti], true, nt > 1, S < 0,
           !t_known && nt > 1, tda, dci, payload);
    if (S >= 0)
      break;
  }
}

/* nr_pdsch_solver_plan: planning entry point for an ordinary (non-CSI-RS) slot; forwards to nr_pdsch_solver_plan_slot() with csirs_slot = false.
 *   Arguments and result as in nr_pdsch_solver_plan_slot(). Callers: tests/nr_pdsch_solver_test.cc.
 */
int nr_pdsch_solver_plan(uint16_t rnti, nr_dci_bits_t payload, uint16_t len, uint8_t mcs, int l0,
                         const nr_pdsch_solver_reading_t *rd, int n_rd, int budget, nr_pdsch_solver_var_t *out)
{
  return nr_pdsch_solver_plan_slot(rnti, payload, len, mcs, l0, false, rd, n_rd, budget, out);
}

/* nr_pdsch_solver_plan_slot: Plans the PDSCH decode variants for one DCI grant of an RNTI, written to out[] (returns the count, at most budget).
 *   Solved RNTI: known layout plus refinement probes. Unsolved: candidate confirmation, reuse prior, then the search (coverage or round robin).
 *   Gated by ISAC_PDSCH_SOLVER (returns 0 when off); logs per-RNTI statistics every 500/1000 grants. Takes g_mu itself.
 *   csirs_slot: solved RNTIs with unknown CSI-RS rate matching probe both rate-match choices; unsolved RNTIs do not search.
 *   Caller: nr_pdcch_blind_monitor_rt.c (per decoded DCI).
 */
int nr_pdsch_solver_plan_slot(uint16_t rnti, nr_dci_bits_t payload, uint16_t len, uint8_t mcs, int l0, bool csirs_slot,
                              const nr_pdsch_solver_reading_t *rd, int n_rd, int budget, nr_pdsch_solver_var_t *out)
{
  (void)len;
  if (!nr_pdsch_solver_enabled() || rd == NULL || n_rd <= 0 || out == NULL || budget <= 0)
    return 0;
  if (budget > NR_PDSCH_SOLVER_MAX_VAR)
    budget = NR_PDSCH_SOLVER_MAX_VAR;
  pthread_mutex_lock(&g_mu);
  const solver_plan_ctx_t *ctx = plan_ctx();
  int slot = -1, n = 0;
  solver_rnti_t *r = slot_of(rnti, true, &slot);
  if (r == NULL) {
    pthread_mutex_unlock(&g_mu);
    return 0;
  }
  const uint64_t dci = ++g_dci_seq;
  g_st.planned++;
  if (!r->first_abs) r->first_abs = ctx->now_abs ? ctx->now_abs : 1;
  if (r->key == ctx->key && r->alt_hash != ctx->hash) r->n_probe = 0; /* new hypotheses on ITS CORESET: search again */
  r->alt_hash = ctx->hash;
  r->key = ctx->key;
  r->grants++;
  bool nl_preferred = false, nl_fallback = false, nl_skip = false;
  const int nl_gap = nl_pref_gap();
  if (nl_gap > 0 && !r->solved) {
    for (int i = 0; i < n_rd; i++)
      if (rd[i].num_rb > 0 && rd[i].num_rb <= SOLVER_SEARCH_MAX_RB && reading_nl_qpsk(&rd[i])) {
        if (r->has_cand) {
          bool overlaps = false;
          for (int k = 0; k < rd[i].n_ids && !overlaps; k++)
            overlaps = (r->cand_bm[rd[i].ids[k] >> 3] >> (rd[i].ids[k] & 7)) & 1;
          if (!overlaps) continue;
          if (r->cand_table >= 0) {
            int qm, r2;
            if (!nr_pdsch_solver_mcs_qr(r->cand_table, rd[i].mcs, &qm, &r2) || qm != 2) continue;
          }
        }
        nl_preferred = true;
        break;
      }
    if (nl_preferred) r->pref_gap = 0;
    else if (++r->pref_gap >= (uint32_t)nl_gap) { r->pref_gap = 0; nl_fallback = true; }
    else nl_skip = true;
  }
  if ((g_st.planned % 1000) == 0) { /* T26c per-RNTI report: solve time in signal time (30 kHz: 0.5 ms per slot) */
    char b[3000];
    int u = 0;
    for (int i = 0; i < SOLVER_RNTIS && u < (int)sizeof(b) - 80; i++) {
      const solver_rnti_t *q = &g_r[i];
      if (!q->used) continue;
      u += snprintf(b + u, sizeof(b) - u, " 0x%04x:g%u,%s%.0fms,full%u/%u,snr%.1f,m1:%u/%u,m3:%u/%u,nw:%u/%u,irc:%u/%u", q->rnti, q->grants,
                    q->solved_abs ? "solve=" : "unsolved_age=",
                    0.5 * (double)((q->solved_abs ? q->solved_abs : ctx->now_abs) - q->first_abs), q->full_ok, q->full_tr,
                    q->n_snr ? q->snr_ema : -99.f, q->comb_ok[0], q->comb_try[0], q->comb_ok[1], q->comb_try[1],
                    q->comb_ok[2], q->comb_try[2], q->comb_ok[3], q->comb_try[3]);
    }
    LOG_A(PHY, "SENSING: PDSCH_SOLVER per-rnti%s\n", b);
    static int s_why = -1;
    if (s_why < 0) s_why = getenv("ISAC_PDSCH_SOLVER_WHY") && atoi(getenv("ISAC_PDSCH_SOLVER_WHY")) ? 1 : 0;
    for (int i = 0; s_why && i < SOLVER_RNTIS; i++) {
      const solver_rnti_t *q = &g_r[i];
      if (!q->used) continue;
      u = 0;
      for (int w = 0; w < W_N && u < (int)sizeof(b) - 32; w++)
        if (q->why[w]) u += snprintf(b + u, sizeof(b) - u, " %s=%u", kWhy[w], q->why[w]);
      LOG_A(PHY, "SENSING: PDSCH_SOLVER why rnti=0x%04x solved=%d grants=%u%s\n", q->rnti, (int)q->solved, q->grants, u ? b : "");
    }
  }
  if ((g_st.planned % 500) == 0)
    LOG_A(PHY, "SENSING: PDSCH_SOLVER stats grants=%llu probes=%llu probe_ok=%llu fulls=%llu full_ok=%llu candidates=%llu "
               "solved=%llu dropped=%llu\n", (unsigned long long)g_st.planned, (unsigned long long)g_st.probes,
          (unsigned long long)g_st.probe_ok, (unsigned long long)g_st.fulls, (unsigned long long)g_st.full_ok,
          (unsigned long long)g_st.candidates, (unsigned long long)g_st.solved, (unsigned long long)g_st.dropped);
  t_rm = 0;
  t_learn_rm = false;
  t_scr = r->solved ? r->scr : -1;
  t_comb = comb_pick(r);
  if (r->solved) {
    int ri = 0;
    for (int i = 0; i < n_rd; i++)
      if (has_id(&rd[i], r->rep)) { ri = i; break; }
    if (csirs_slot && r->rm < 0) { /* unknown: probe with and without, learn by CRC */
      for (int k = 1; k >= 0; k--) {
        t_rm = k;
        t_learn_rm = true;
        const int n0 = n;
        plan_known(out, &n, budget, slot, 1, ri, rd, l0, r->rep, r->add_pos, r->table, r->S_by_tda, mcs, dci, payload);
        for (int q = n0; q < n; q++) out[q].probe = 1; /* a rate-match trial is a probe, never a counted TB */
      }
      t_learn_rm = false;
    } else {
      t_rm = csirs_slot ? r->rm : 0;
      plan_known(out, &n, budget, slot, 1, ri, rd, l0, r->rep, r->add_pos, r->table, r->S_by_tda, mcs, dci, payload);
    }
    /* REFINEMENT: the solution's layouts can still disagree on fields two passes did not separate (e.g. where the
     * antenna-ports field sits). Each other reading those layouts give for THIS DCI gets one probe with the same
     * physical parameters; a pass narrows the layout set to that reading (feedback), and the representative follows. */
    const int S_here = r->S_by_tda[tda_of(r->rep, payload)];
    int refine = 0;
    for (int i = 0; i < n_rd && n < budget && refine < 4; i++) {
      if (i == ri || rd[i].num_rb > SOLVER_SEARCH_MAX_RB) continue;
      int ov = 0;
      for (int k = 0; k < rd[i].n_ids && !ov; k++)
        ov = (r->cand_bm[rd[i].ids[k] >> 3] >> (rd[i].ids[k] & 7)) & 1;
      if (!ov) continue;
      uint8_t tl[3];
      const int nt = tables_for(nl_gap > 0 ? rd[i].mcs : mcs, tl);
      if (nt == 0) break;
      emit(out, &n, budget, slot, 1, i, rd, l0, S_here >= 0 ? S_here : 1, r->add_pos, r->table >= 0 ? r->table : tl[0],
           true, nt > 1, false, false, tda_of(r->rep, payload), dci, payload);
      refine++;
    }
  } else if (csirs_slot) {
    r->why[W_CSIRS]++;
    /* no search on a slot carrying discovered CSI-RS: its rate matching is unknown, a miss would be false evidence */
  } else if (!r->has_cand && !leader_ok(r)) {
    r->why[W_LEADER]++;
    /* T26d ISAC_PDSCH_SOLVER_LEADER=1: until some RNTI of this CORESET solved, only its busiest unsolved RNTI searches --
     * spreading the probe budget over ~15 RNTIs left whole runs without a first solve (replays: ~40 % of runs); the
     * first solution then seeds every other RNTI through the reuse prior. */
  } else if (probe_hog(r)) {
    r->why[W_HOG]++;
    /* T26d: fair share of the probe budget -- an unsolved RNTI that already spent far more probes than the average
     * unsolved RNTI waits (a busy undecodable population starved the serving cell's RNTIs: replay f3 0x8f5a unsolved) */
  } else if (nl_skip || (nl_gap <= 0 && search_pref_skip(r, mcs))) {
    r->why[W_NLSKIP]++;
    /* Wait for a one-layer QPSK reading (NL_PREF), or a low-MCS grant under the legacy MCS-only preference. */
  } else if (!(nl_gap > 0 && nl_preferred) && search_backoff(r)) {
    r->why[W_BACKOFF]++;
    /* T26c ISAC_PDSCH_SOLVER_BACKOFF=<probes> (default off): an RNTI whose search has spent that many probes without a
     * single CRC pass is searched on 1 grant in 8 only -- the decode pool goes to RNTIs that decode */
  } else {
    /* 1. confirm a candidate: the reading sharing most layouts with it (under the candidate's own scrambling) */
    t_scr = r->has_cand ? r->cand_scr : -1;
    if (r->has_cand) {
      int best = -1, best_ov = 0;
      for (int i = 0; i < n_rd; i++) {
        if (nl_preferred && !reading_nl_qpsk(&rd[i])) continue;
        int ov = 0;
        for (int k = 0; k < rd[i].n_ids; k++)
          ov += (r->cand_bm[rd[i].ids[k] >> 3] >> (rd[i].ids[k] & 7)) & 1;
        if (ov > best_ov) { best_ov = ov; best = i; }
      }
      if (best >= 0 && rd[best].num_rb <= SOLVER_SEARCH_MAX_RB) {
        uint8_t tl[3];
        const int nt = tables_for(nl_gap > 0 ? rd[best].mcs : mcs, tl);
        for (int si = 0; si < 4; si++)
          for (int ti = 0; ti < (r->cand_table >= 0 ? 1 : nt); ti++) {
            const int table = r->cand_table >= 0 ? r->cand_table : tl[ti];
            int qm, r2;
            if (nl_preferred && nr_pdsch_solver_mcs_qr(table, rd[best].mcs, &qm, &r2) && qm != 2) continue;
            emit(out, &n, budget, slot, 0, best, rd, l0, kS[si], r->cand_add_pos,
                 table, true, nt > 1, false, false, -1, dci, payload);
          }
      }
    }
    /* T26c: the search alternates the scrambling hypothesis grant by grant when the CORESET has its own nID */
    { /* default ids, then each run-time alternative, one per grant -- unless another RNTI of this CORESET already
       * solved: then only that RNTI's scrambling (one RRC template per CORESET) */
      int known = 0;
      for (int q = 0; q < g_n_key_scr && !known && !ctx->own; q++) /* preference, not exclusive: 1 block in 4 still explores */
        if (g_key_scr[q].key == ctx->key && ((r->grants / 16u) % 4u) != 3u) { t_scr = g_key_scr[q].scr; known = 1; }
      if (!known) { /* T26d: the default ids alone for the first 48 grants -- cycling 6+ alternatives from the start made
                     * the serving population's search 6x slower (replay e9/e10: 0x8f5a unsolved) */
        if (ctx->own) { /* TIM-PDSCH: this RNTI's own blind identities, cycled grant by grant (no PCI default) */
          t_scr = ctx->ids[r->grants % (uint32_t)ctx->n];
        } else if (ctx->decided) { /* the decided id on 3 grants of 4, the default on the 4th */
          const uint32_t blk = r->grants / 16u, nd = (uint32_t)(ctx->n < 3 ? ctx->n : 3);
          t_scr = (blk % 4u) == 3u ? -1 : ctx->ids[(blk / 4u + blk) % nd]; /* top ids by block, default 1 block in 4 */
        } else {
          const uint32_t g = r->grants > 48 ? r->grants - 48 : 0;
          const uint32_t k = r->grants <= 48 ? 0 : (g / 16u) % (uint32_t)(ctx->n + 1);
          t_scr = k == 0 ? -1 : ctx->ids[k - 1];
        }
      }
    }
    /* 2. the cell prior (another RNTI's solution), first. ISAC_PDSCH_SOLVER_REUSE=1: the CURRENT solution of the best
     * solved RNTI (most full TB CRC passes; layouts refined, S map and MCS table learnt since) instead of the snapshot
     * taken at the first solve -- gNBs configure UEs from one RRC template. Still confirmed by this RNTI's own 2 passes. */
    if (g_reuse < 0) g_reuse = getenv("ISAC_PDSCH_SOLVER_REUSE") && atoi(getenv("ISAC_PDSCH_SOLVER_REUSE")) ? 1 : 0;
    const solver_rnti_t *best = NULL;
    const uint32_t min_ok = prior_crc_on() ? 1 : 0;
    if (g_reuse)
      for (int k = 0; k < SOLVER_RNTIS; k++)
        if (g_r[k].used && g_r[k].solved && g_r[k].rep >= 0 && g_r[k].full_ok >= min_ok && (!best || g_r[k].full_ok > best->full_ok))
          best = &g_r[k];
    bool ids_done = false;
    if (prior_ids_on() && ctx->own) { /* TIMSOLVE2: proven dims x each own identity in the proven form */
      const solver_rnti_t *src[3];
      int ns = 0;
      for (int k = 0; k < SOLVER_RNTIS; k++) { /* CRC-proven solutions, most full CRC passes first, distinct reps */
        const solver_rnti_t *q = &g_r[k];
        if (!q->used || !q->solved || q->rep < 0 || q->full_ok < 1) continue;
        int pos = ns;
        bool dup = false;
        for (int j = 0; j < ns; j++) dup |= src[j]->rep == q->rep;
        if (dup) continue;
        while (pos > 0 && src[pos - 1]->full_ok < q->full_ok) pos--;
        if (pos >= 3) continue;
        for (int j = (ns < 3 ? ns : 2); j > pos; j--) src[j] = src[j - 1];
        src[pos] = q;
        if (ns < 3) ns++;
      }
      int32_t hyp[16];
      int nh = 0;
      for (int q = 0; ns > 0 && q < ctx->n; q++)
        if (scr_form(ctx->ids[q]) == scr_form(src[0]->scr)) hyp[nh++] = ctx->ids[q];
      const int32_t saved = t_scr;
      for (int h = 0; h < nh; h++) {
        t_scr = hyp[(h + r->grants) % (uint32_t)nh]; /* rotate: a budget of 1 still covers every identity */
        int used[3], nu = 0;
        for (int j = 0; j < ns; j++)
          for (int i = 0; i < n_rd; i++)
            if (has_id(&rd[i], src[j]->rep)) {
              bool seen = false;
              for (int u = 0; u < nu; u++) seen |= used[u] == i;
              if (!seen) {
                used[nu++] = i;
                plan_known(out, &n, budget, slot, 0, i, rd, l0, src[j]->rep, src[j]->add_pos, src[j]->table, src[j]->S_by_tda,
                           mcs, dci, payload);
              }
              break;
            }
      }
      t_scr = saved;
      ids_done = nh > 0;
    }
    if (ids_done) {
      /* the proven dims were planned with the RNTI's own identities above */
    } else if (best) {
      for (int i = 0; i < n_rd; i++)
        if (has_id(&rd[i], best->rep)) {
          plan_known(out, &n, budget, slot, 0, i, rd, l0, best->rep, best->add_pos, best->table, best->S_by_tda, mcs, dci,
                     payload);
          break;
        }
    } else if (g_have_prior) {
      for (int i = 0; i < n_rd; i++)
        if (has_id(&rd[i], g_prior_rep)) {
          plan_known(out, &n, budget, slot, 0, i, rd, l0, g_prior_rep, g_prior_add_pos, g_prior_table,
                     g_prior_S_by_tda, mcs, dci, payload);
          break;
        }
    }
    /* 3. the search: (S, add_pos) outer, readings (most-shared first) inner, then tables */
    int order[128], no = 0;
    for (int i = 0; i < n_rd && no < 128; i++)
      if (rd[i].num_rb > 0 && rd[i].num_rb <= SOLVER_SEARCH_MAX_RB
          && (nl_gap <= 0 || !nl_preferred || nl_fallback || reading_nl_qpsk(&rd[i]))) order[no++] = i;
    for (int a = 1; a < no; a++)
      for (int b = a; b > 0 && rd[order[b]].n_ids > rd[order[b - 1]].n_ids; b--) {
        const int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
      }
    uint8_t tl[3];
    const int nt = tables_for(mcs, tl);
    if (no == 0) r->why[W_WIDE]++;
    if (no > 0 && (nl_gap > 0 || nt > 0)) {
      /* add_pos tier outer (1, 2, 0, 3); inside a tier every reading gets all S in one go, because the true S
       * changes from grant to grant with the TDA index while the true reading is there on every grant */
      static const uint8_t kA[4] = {1, 2, 0, 3};
      if (g_coverage < 0)
        g_coverage = getenv("ISAC_PDSCH_SOLVER_COVERAGE") && atoi(getenv("ISAC_PDSCH_SOLVER_COVERAGE"));
      if (g_coverage) {
        plan_coverage(r, out, &n, budget, slot, rd, order, no, l0, nl_preferred, dci, payload);
      } else if (nl_gap > 0) {
        int pair_rd[128 * 3], np = 0;
        uint8_t pair_t[128 * 3], pair_disc[128 * 3];
        for (int i = 0; i < no; i++) {
          uint8_t rtab[3];
          const int rn = tables_for(rd[order[i]].mcs, rtab);
          for (int t = 0; t < rn; t++) {
            int qm, r2;
            if (nl_preferred && nr_pdsch_solver_mcs_qr(rtab[t], rd[order[i]].mcs, &qm, &r2) && qm != 2)
              continue;
            pair_rd[np] = order[i]; pair_t[np] = rtab[t]; pair_disc[np++] = rn > 1;
          }
        }
        const uint32_t total = (uint32_t)(4 * np * 4);
        for (uint32_t k = 0; k < total && n < budget; k++) {
          const uint32_t idx = r->cursor++ % total;
          const int a = (int)(idx / (uint32_t)(np * 4)), pi = (int)((idx / 4u) % (uint32_t)np), si = (int)(idx % 4u);
          emit(out, &n, budget, slot, 0, pair_rd[pi], rd, l0, kS[si], kA[a], pair_t[pi], true,
               pair_disc[pi], false, false, -1, dci, payload);
        }
      } else {
        const uint32_t total = (uint32_t)(4 * no * 4 * nt);
        for (uint32_t k = 0; k < total && n < budget; k++) {
          const uint32_t idx = r->cursor++ % total;
          const int a = (int)(idx / (uint32_t)(no * 4 * nt)), ri = order[(idx / (uint32_t)(4 * nt)) % (uint32_t)no],
                    si = (int)((idx / (uint32_t)nt) % 4u), tt = tl[idx % (uint32_t)nt];
          emit(out, &n, budget, slot, 0, ri, rd, l0, kS[si], kA[a], tt, true, nt > 1, false, false, -1, dci, payload);
        }
      }
    }
  }
  for (int i = 0; i < n; i++) {
    out[i].probe ? g_st.probes++ : g_st.fulls++;
    if (out[i].probe && !r->solved) { r->n_probe++; r->why[W_PROBES]++; r->fcredit = r->fcredit >= 1.0 ? r->fcredit - 1.0 : 0.0; } /* no debt from uncapped grants */
  }
  r->last_abs = ctx->now_abs;
  if (!r->solved) r->why[W_PLAN]++;
  pthread_mutex_unlock(&g_mu);
  return n;
}

/* lowest: returns the lowest layout index set in a bitmap of BM_BYTES bytes, or -1 when empty.
 *   Used as the fallback representative when no consistent layout is found. Caller: nr_pdsch_solver_feedback().
 */
static int lowest(const uint8_t *bm)
{
  for (int b = 0; b < BM_BYTES; b++)
    if (bm[b])
      return b * 8 + __builtin_ctz(bm[b]);
  return -1;
}

/* choose_rep: picks the representative layout of a candidate set: the lowest one whose TDA mapping is consistent with all CRC-passed evidence.
 *   Fills r->S_by_tda with the S value per TDA index from the evidence; -1 entries are unknown. Returns -1 when none is consistent.
 *   Caller: nr_pdsch_solver_feedback() (candidate confirmation and each new solving pass).
 */
static int choose_rep(solver_rnti_t *r)
{
  for (int b = 0; b < BM_BYTES; b++) {
    if (!r->cand_bm[b])
      continue;
    for (int k = 0; k < 8; k++) {
      if (!((r->cand_bm[b] >> k) & 1))
        continue;
      const int id = b * 8 + k;
      int8_t m[16];
      memset(m, -1, sizeof(m));
      bool ok = true;
      for (int e = 0; e < r->n_ev && ok; e++) {
        const int t = tda_of(id, r->ev_p[e]);
        if (m[t] >= 0 && m[t] != r->ev_S[e]) ok = false;
        else m[t] = (int8_t)r->ev_S[e];
      }
      if (ok) {
        memcpy(r->S_by_tda, m, sizeof(m));
        return id;
      }
    }
  }
  return -1;
}
/* add_ev: appends one CRC-passed (payload, S) pair to the RNTI's evidence list, dropping the oldest when 16 are stored.
 *   Caller: nr_pdsch_solver_feedback().
 */
static void add_ev(solver_rnti_t *r, nr_dci_bits_t p, uint8_t S)
{
  if (r->n_ev < 16) { r->ev_p[r->n_ev] = p; r->ev_S[r->n_ev++] = S; }
  else { memmove(r->ev_p, r->ev_p + 1, 15 * sizeof(r->ev_p[0])); memmove(r->ev_S, r->ev_S + 1, 15); r->ev_p[15] = p; r->ev_S[15] = S; }
}

/* has_id_p: true when a pending entry's layout list contains id (debug logging only).
 *   Caller: nr_pdsch_solver_feedback() probe log.
 */
static int has_id_p(const pending_t *p, int id)
{
  for (int i = 0; i < p->n_ids; i++)
    if (p->ids[i] == id) return 1;
  return 0;
}
/* make_cand: starts a candidate for an RNTI from a CRC-passing pending entry: its layouts, S, add_pos, table and scrambling id.
 *   Allocates the layout bitmap on first use and counts the candidate. Caller: nr_pdsch_solver_feedback() under g_mu.
 */
static void make_cand(solver_rnti_t *r, const pending_t *p)
{
  if (r->cand_bm == NULL) r->cand_bm = calloc(1, BM_BYTES);
  if (r->cand_bm == NULL)
    return;
  memset(r->cand_bm, 0, BM_BYTES);
  for (int i = 0; i < p->n_ids; i++) r->cand_bm[p->ids[i] >> 3] |= (uint8_t)(1u << (p->ids[i] & 7));
  r->has_cand = true;
  r->cand_add_pos = p->add_pos;
  r->cand_table = p->table_disc ? (int8_t)p->table : -1;
  r->cand_S = p->S;
  r->cand_payload = p->payload;
  r->cand_dci = p->dci;
  r->cand_scr = p->scr;
  r->cand_tries = 0;
  r->k16 = !p->crc24;
  r->k24 = p->crc24;
  if (signif_on()) { r->n_ev = 0; add_ev(r, p->payload, p->S); }
  g_st.candidates++;
  LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x CANDIDATE: CRC pass S=%u add_pos=%u table=%d on %d layouts\n",
        r->rnti, p->S, p->add_pos, r->cand_table, p->n_ids);
}

/* solve_now: the candidate becomes the RNTI's solution (layouts cand_bm, evidence ev_*), p = the deciding pass. Under g_mu. */
static void solve_now(solver_rnti_t *r, const pending_t *p, int kept)
{
  r->solved = true;
  memset(r->S_by_tda, -1, sizeof(r->S_by_tda));
  r->rep = choose_rep(r);
  if (r->rep < 0) r->rep = lowest(r->cand_bm);
  r->add_pos = p->add_pos;
  r->scr = p->scr;
  r->table = r->cand_table >= 0 ? r->cand_table : (p->table_disc ? (int8_t)p->table : -1);
  g_st.solved++;
  r->solved_abs = p->abs;
  memset(r->comb_try, 0, sizeof(r->comb_try)); /* receiver choice from post-solve decodes only (pre-solve probes */
  memset(r->comb_ok, 0, sizeof(r->comb_ok));   /*  fail for every receiver and biased the choice) */
  if (signif_on())
    LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x SOLVED by %u CRC16 + %u CRC24 passes, P(chance)=%.1e over %u+%u probes: layout %d "
               "(%d consistent) add_pos=%u table=%d S[tda %d]=%u scr=%d\n",
          r->rnti, r->k16, r->k24, chance_p(r), r->cred16, r->cred24, r->rep, kept, r->add_pos, r->table,
          tda_of(r->rep, p->payload), p->S, (int)r->scr);
  else
    LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x SOLVED by 2 independent CRC passes: layout %d (%d consistent) "
               "add_pos=%u table=%d S[tda %d]=%u S[tda %d]=%u scr=%d\n",
          r->rnti, r->rep, kept, r->add_pos, r->table, tda_of(r->rep, r->cand_payload), r->cand_S,
          tda_of(r->rep, p->payload), p->S, (int)r->scr);
  if (!prior_crc_on()) promote(r); /* T26c: the CORESET's scrambling and the cell prior for the other RNTIs */
}

/* nr_pdsch_solver_feedback: consumes the CRC result of one solver decode (identified by its token): updates statistics, candidates, and solved state.
 *   Two CRC passes with consistent layouts and S solve the RNTI (sets prior and CORESET scrambling); failed candidates are dropped after several tries.
 *   Solved RNTIs learn rate matching, table, representative and S map; 256 failed full decodes without a pass reopen the search.
 *   ISAC_PDSCH_SOLVER_PROBELOG=1 logs each credited probe. Called by nr_pdsch_passive_queue.c after each decode. Takes g_mu.
 */
void nr_pdsch_solver_feedback(uint32_t token, bool crc_ok, bool credit)
{
  if (token == 0)
    return;
  pthread_mutex_lock(&g_mu);
  pending_t *p = NULL;
  { /* O(1): the token carries its pending index (was a linear scan of all 4096 entries under g_mu) */
    pending_t *q = &g_p[token & (SOLVER_PENDING - 1)];
    if (q->used && q->token == token) p = q;
  }
  if (crc_ok && (p == NULL || !credit)) { /* T26d diagnostic: a CRC pass the solver could not use */
    static _Atomic uint64_t s_lost[2];
    const uint64_t k = atomic_fetch_add(&s_lost[p == NULL ? 0 : 1], 1) + 1;
    if (k <= 5 || (k % 100) == 0)
      LOG_A(PHY, "SENSING: PDSCH_SOLVER lost CRC pass #%llu (%s) token=0x%08x\n", (unsigned long long)k,
            p == NULL ? "pending entry gone" : "no credit", token);
  }
  { /* ISAC_PDSCH_SOLVER_PROBELOG=1 (T26d diagnostic): one line per credited search probe -- which hypothesis was tried */
    static int s_pl = -1;
    if (s_pl < 0) s_pl = getenv("ISAC_PDSCH_SOLVER_PROBELOG") ? atoi(getenv("ISAC_PDSCH_SOLVER_PROBELOG")) : 0;
    if (s_pl && p != NULL && credit && p->phase == 0) {
      int lo = 65535, hi = -1;
      for (int i = 0; i < p->n_ids; i++) { if (p->ids[i] < lo) lo = p->ids[i]; if (p->ids[i] > hi) hi = p->ids[i]; }
      const solver_rnti_t *rr = (p->slot >= 0 && p->slot < SOLVER_RNTIS) ? &g_r[p->slot] : NULL;
      LOG_A(PHY, "SENSING: PDSCH_SOLVER probe rnti=0x%04x key=%d dci=%llu abs=%llu crc=%d probe=%u S=%u add_pos=%u table=%u scr=%d Nl=%u mcs=%u ports=0x%x n_ids=%d ids=%d..%d "
                 "has1662=%d has1664=%d has2102=%d has2104=%d tok=%u\n", rr ? rr->rnti : 0, rr ? rr->key : -1,
            (unsigned long long)p->dci, (unsigned long long)p->abs, (int)crc_ok, p->probe, p->S, p->add_pos,
            p->table, p->scr, p->n_layers, p->mcs, p->dmrs_ports, p->n_ids, lo, hi,
            has_id_p(p, 1662), has_id_p(p, 1664), has_id_p(p, 2102), has_id_p(p, 2104), token);
      if (crc_ok) {
        char ids[8192];
        int used = 0;
        for (int i = 0; i < p->n_ids && used < (int)sizeof(ids) - 8; i++)
          used += snprintf(ids + used, sizeof(ids) - used, "%s%u", i ? "," : "", p->ids[i]);
        ids[used] = 0;
        LOG_A(PHY, "SENSING: PDSCH_SOLVER evidence tok=%u dci=%llu layouts=%s\n",
              token, (unsigned long long)p->dci, ids);
      }
    }
  }
  if (p != NULL && p->phase == 0 && p->probe && g_r[p->slot].used && !g_r[p->slot].solved)
    g_r[p->slot].why[!credit ? W_PNOCRED : crc_ok ? W_POK : W_PFAIL]++;
  if (p == NULL || !credit) {
    if (p) p->used = false;
    pthread_mutex_unlock(&g_mu);
    return;
  }
  solver_rnti_t *r = &g_r[p->slot];
  if (r->used && p->phase == 0 && !r->solved && !crc_ok && r->layout_failures < UINT32_MAX)
    r->layout_failures++;
  if (r->used && p->phase == 0 && p->probe && !r->solved) /* the Bonferroni count of ISAC_PDSCH_SOLVER_SIGNIF */
    p->crc24 ? r->cred24++ : r->cred16++;
  if (p->comb >= 1 && (!r->solved || !p->probe)) {
    const int ci = p->comb == 3 ? 1 : p->comb == 4 ? 2 : p->comb >= 5 ? 3 : 0;
    r->comb_try[ci]++;
    if (crc_ok) r->comb_ok[ci]++;
  }
  if (p->probe && crc_ok) g_st.probe_ok++;
  if (!p->probe && crc_ok) g_st.full_ok++;
  if (r->used && p->phase == 1 && r->solved) {
    if (!p->probe) {
      r->full_tr++;
      if (crc_ok) {
        r->full_ok++;
        r->cons_fail = 0;
        if (r->full_ok == 1 && prior_crc_on()) promote(r); /* first CRC-proven full TB: now it may seed the cell */
        if (r->full_ok <= 20 || (r->full_ok % 100) == 0)
          LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x full TB CRC OK #%u of %u full decodes (S=%u table=%u layout %d)\n",
                r->rnti, r->full_ok, r->full_tr, p->S, p->table, r->rep);
      }
      else if (++r->cons_fail >= 256 && r->full_ok == 0) { /* a solution that never decodes a full TB: reopen */
        LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x solution never decoded a full TB in 256 grants: reopening\n", r->rnti);
        r->solved = false; r->has_cand = false; g_st.dropped++;
      }
    }
    if (crc_ok && p->learn_rm && r->rm < 0) {
      r->rm = (int8_t)p->rm;
      LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x CSI-RS rate matching %s (CRC-selected per RNTI)\n", r->rnti,
            p->rm ? "APPLIED" : "NOT applied");
    }
    if (crc_ok && r->cand_bm) { /* narrow the solution's layouts to those that read this DCI as the passing variant */
      int kept = 0;
      for (int i = 0; i < p->n_ids; i++) kept += (r->cand_bm[p->ids[i] >> 3] >> (p->ids[i] & 7)) & 1;
      if (kept > 0) {
        uint8_t *nb = calloc(1, BM_BYTES);
        if (nb) {
          for (int i = 0; i < p->n_ids; i++)
            if ((r->cand_bm[p->ids[i] >> 3] >> (p->ids[i] & 7)) & 1) nb[p->ids[i] >> 3] |= (uint8_t)(1u << (p->ids[i] & 7));
          memcpy(r->cand_bm, nb, BM_BYTES);
          free(nb);
        }
      }
    }
    if (crc_ok) {
      r->cons_fail = 0;
      add_ev(r, p->payload, p->S);
      const int old_rep = r->rep;
      const int nr = choose_rep(r);
      if (nr >= 0) r->rep = nr;
      if (p->learn_S || r->rep != old_rep)
        LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x learnt S=%u (evidence %d) representative layout %d\n", r->rnti, p->S,
              r->n_ev, r->rep);
    }
    if (crc_ok && p->learn_table && r->table < 0) {
      r->table = (int8_t)p->table;
      if (g_have_prior && g_prior_table < 0) g_prior_table = r->table;
      LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x learnt MCS table %u\n", r->rnti, p->table);
    }
  } else if (r->used && p->phase == 0 && !r->solved && crc_ok) {
    if (!r->has_cand) {
      make_cand(r, p);
      if (signif_on() && r->has_cand && significant(r)) solve_now(r, p, p->n_ids); /* one CRC24 pass */
    } else if (p->dci != r->cand_dci) {
      uint8_t *nb = calloc(1, BM_BYTES);
      int kept = 0;
      for (int i = 0; nb && i < p->n_ids; i++)
        if ((r->cand_bm[p->ids[i] >> 3] >> (p->ids[i] & 7)) & 1) {
          nb[p->ids[i] >> 3] |= (uint8_t)(1u << (p->ids[i] & 7));
          kept++;
        }
      if (kept == 0 || p->add_pos != r->cand_add_pos || p->scr != r->cand_scr
          || (nl_pref_gap() > 0 && r->cand_table >= 0 && p->table_disc && p->table != r->cand_table)) {
        /* a pass the candidate cannot explain: after the candidate has had its confirmation chances, the newer
         * evidence takes its place (a chance CRC pass must not block the search) */
        if (r->cand_tries >= 16) {
          make_cand(r, p);
          if (signif_on() && r->has_cand && significant(r)) solve_now(r, p, p->n_ids);
        }
      } else if (signif_on()) { /* a consistent pass on another DCI: count it, solve once significant */
        memcpy(r->cand_bm, nb, BM_BYTES);
        p->crc24 ? r->k24++ : r->k16++;
        add_ev(r, p->payload, p->S);
        r->cand_dci = p->dci; /* the next pass must come from yet another DCI */
        if (significant(r)) solve_now(r, p, kept);
        else
          LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x candidate pass %u+%u not yet significant: P(chance)=%.1e over %u+%u probes\n",
                r->rnti, r->k16, r->k24, chance_p(r), r->cred16, r->cred24);
      } else {
        memcpy(r->cand_bm, nb, BM_BYTES);
        r->n_ev = 0;
        add_ev(r, r->cand_payload, r->cand_S);
        add_ev(r, p->payload, p->S);
        solve_now(r, p, kept);
      }
      free(nb);
    }
  } else if (r->used && p->phase == 0 && !r->solved && !crc_ok && r->has_cand) {
    if (++r->cand_tries > SOLVER_CAND_TRIES * 4) { /* ~96 grants of confirmation probes: candidate was a chance pass */
      LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x candidate dropped (no confirmation)\n", r->rnti);
      r->has_cand = false;
      g_st.dropped++;
    }
  }
  p->used = false;
  pthread_mutex_unlock(&g_mu);
}

/* nr_pdsch_solver_stats: copies the solver's global counters (planned, probes, fulls, CRC passes, candidates, solved, dropped) into *s.
 *   Takes g_mu. No callers outside the header yet.
 */
void nr_pdsch_solver_stats(nr_pdsch_solver_stats_t *s)
{
  pthread_mutex_lock(&g_mu);
  *s = g_st;
  pthread_mutex_unlock(&g_mu);
}
