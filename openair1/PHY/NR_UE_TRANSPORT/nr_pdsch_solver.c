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
#define SOLVER_MAX_IDS 2048     /* layouts of one reading kept with a pending probe (the candidate set a pass creates) */
#define COVERAGE_IDS 64         /* layouts of a reading whose trial counts stand for it in the coverage search (cost bound: the
                                 * counts are kept per layout id, and a reading of the full standard space can hold thousands) */
#define SOLVER_PHASES 20          /* slot phases: one 10 ms frame at 30 kHz; TDD patterns divide it */
#define SOLVER_PHASE_FAIL 8       /* full decodes without one pass at a phase (the RNTI decodes elsewhere) before its (S, L) is searched */
#define SOLVER_CAND_TRIES 96      /* confirmation probes before an unconfirmed candidate is dropped */
#define BM_BYTES (NR_PDSCH_SOLVER_MAX_LAYOUTS / 8)
#define HQ_RING 64

typedef struct { uint64_t key; uint32_t count; uint32_t snr_n; float snr_sum, snr_sum2; } coverage_entry_t; /* snr_*: EVIDENCE RANK (plan_coverage) */
/* internal diagnostic reasons, after the caller-side NR_PDSCH_SOLVER_WHY_* ones */
enum { W_PLAN = NR_PDSCH_SOLVER_WHY_CALLER_N, W_CSIRS, W_BACKOFF, W_WIDE, W_PROBES, W_POK, W_PFAIL,
       W_PNOCRED, W_NOREP, W_N };
static const char *const kWhy[W_N] = {"gate", "live", "budget0", "noread", "throttle", "cap", "weak", "plan", "csirs",
                                      "backoff", "norb", "probes", "p_ok", "p_fail", "p_nocred", "norep"};
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
  uint8_t ev_S[16], ev_L[16]; /* DCI11-RESOLVER: (S, L) of the pass -- the TDA field the representative reads must map them */
  uint8_t add_pos;
  int8_t table;
  int8_t S_by_tda[16];
  /* SOLVERPHASE: (S, L) of a solved RNTI per slot phase (abs slot mod SOLVER_PHASES) where its full decodes fail while it decodes
   * elsewhere: a TDD special slot allocation the DCI's TDA index (tda_bits = 0 layouts: none) cannot tell from a full slot */
  int8_t ov_S[SOLVER_PHASES], ov_L[SOLVER_PHASES];
  uint32_t ph_tr[SOLVER_PHASES], ph_ok[SOLVER_PHASES], ov_cur[SOLVER_PHASES];
  int8_t L_by_tda[16];     /* SOLVERTDDL: PDSCH length per TDA index when it is not 14 - S (TDD special slot); -1 = 14 - S */
  int8_t rm;               /* CSI-RS rate matching for this RNTI: -1 unknown, 0 no, 1 yes (CRC-selected) */
  uint32_t full_tr, full_ok, cons_fail;
  /* T26c per-RNTI report: absolute slots (signal time) of the first grant and of the solving CRC pass */
  uint64_t first_abs, solved_abs;
  uint32_t grants;
  int32_t scr, cand_scr;   /* T26c: PDSCH scrambling id hypothesis (-1 = default ids; else the id); solved/candidate */
  int32_t key;             /* T26c: CORESET key of this RNTI's last planned DCI */
  uint32_t n_probe;        /* T26c: probes planned while unsolved (search backoff) */
  uint32_t alt_hash;       /* T26c: g_alt_hash seen at the last plan */
  uint32_t comb_try[4], comb_ok[4]; /* T26d: CRC per receiver {1 best branch, 3 MRC} */
  float snr_ema;           /* T26c: post-equalisation SNR (dB) EMA of this RNTI's full decodes, NAN-free: n_snr = 0 unknown */
  uint32_t n_snr;
  uint32_t why[W_N];       /* SOLVERCOVER diagnostics (see NR_PDSCH_SOLVER_WHY_*) */
  uint64_t last_abs;       /* SOLVERCOVER: signal time of the last grant (activity, eviction) */
  double fcredit;          /* SOLVERCOVER fair share: this RNTI's share of the probe rate, in probes */
  uint64_t fc_abs;         /*   time of the last accrual (0 = never: cold start gets a full budget) */
  uint32_t cred16, cred24; /* TIMSOLVE2: credited search probes while unsolved, by CRC length */
  uint8_t k16, k24;        /*   consistent CRC passes of the current candidate, by CRC length */
  nr_dci_bits_t hq_ring[HQ_RING]; /* DCIRV: this RNTI's last DCI 1_1 payloads (oldest overwritten) */
  uint32_t hq_cnt, rvmis;  /*   payloads seen; RV-rescue notes since the last re-selection */
  uint64_t hq_abs;         /*   slot of the last payload (duplicate candidates of one DCI) */
  bool mcs_pos, promoted;  /*   a CRC pass at MCS > 0 happened; the solution has been allowed to seed the cell */
} solver_rnti_t;

typedef struct {
  bool used;
  uint32_t token;
  int slot;
  uint8_t phase;          /* 0 = search/candidate, 1 = solved */
  uint8_t S, L, add_pos, table, probe;
  bool ov;                /* SOLVERPHASE variant: probe or full decode with the phase's own (S, L) */
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
  uint8_t cov_v1;         /* SOLVERREFUND: plan_coverage() variant + 1 that this probe charged to the coverage table (0 = none) */
  uint16_t dmrs_ports;
  float snr;              /* EVIDENCE RANK: DM-RS comb SNR (dB) the probe measured, NAN until nr_pdsch_solver_note_probe_snr() */
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
  int8_t last_dl; /* SOLVERTDDEND: last DL symbol of the grant's slot from the broadcast TDD pattern (-1 = unknown) */
} solver_plan_ctx_t;
static _Thread_local solver_plan_ctx_t t_plan_ctx = {.key = -1, .last_dl = -1}; /* per scan consumer */
/* plan_ctx: the plan context (per scan consumer thread) that the grant planner reads its key, hash and slot time from. */
static solver_plan_ctx_t *plan_ctx(void) { return &t_plan_ctx; }
static struct { int32_t key, scr; } g_key_scr[16]; /* first solved scrambling per CORESET: its other RNTIs try it only */
static int g_n_key_scr;
static uint64_t g_dci_seq;
static nr_pdsch_solver_tda_fn g_tda_fn;
static void *g_tda_ctx;
static nr_pdsch_solver_stats_t g_st;
#define LAYOUT_RECOVER 32 /* failed CRC probes after which a pruned layout set is reopened once */
static nr_pdsch_solver_hq_fn g_hq_fn; /* DCIRV */
static void *g_hq_ctx;
/* cell prior: the first solution, offered first to other RNTIs */
static bool g_have_prior;
static int g_prior_rep;
static uint8_t g_prior_add_pos;
static int8_t g_prior_table, g_prior_S_by_tda[16], g_prior_L_by_tda[16];
static uint32_t g_lcur[SOLVER_RNTIS][16]; /* SOLVERTDDL: rotation cursor of the length search per RNTI and TDA index */
static bool t_ov; /* SOLVERPHASE: variants being emitted are phase-override ones */
static int t_L; /* length of the variants being emitted (0 = 14 - S); set by plan_known under g_mu */

/* nr_pdsch_solver_enabled: Callers: nr_pdsch_passive_decode.c (probe-c1 switch), nr_pdcch_blind_monitor_rt.c (RNTI routing). */
bool nr_pdsch_solver_enabled(void)
{
  return true;
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

/* nr_pdsch_solver_reset: Frees per-RNTI candidate bitmaps and coverage tables under g_mu. Callers: tests/nr_pdsch_solver_test.cc only. */
void nr_pdsch_solver_reset(void)
{
  pthread_mutex_lock(&g_mu);
  for (int i = 0; i < SOLVER_RNTIS; i++) {
    free(g_r[i].cand_bm);
    if (g_r[i].coverage) free(g_r[i].coverage->entries);
    free(g_r[i].coverage);
    memset(&g_r[i], 0, sizeof(g_r[i]));
  }
  memset(g_p, 0, sizeof(g_p));
  memset(g_lcur, 0, sizeof(g_lcur));
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
      memset(g_r[i].L_by_tda, -1, sizeof(g_r[i].L_by_tda));
      memset(g_r[i].ov_S, -1, sizeof(g_r[i].ov_S));
      memset(g_r[i].ov_L, -1, sizeof(g_r[i].ov_L));
      memset(g_lcur[i], 0, sizeof(g_lcur[i]));
      if (idx) *idx = i;
      return &g_r[i];
    }
  /* SOLVERCOVER: a full table used to refuse every new RNTI for the rest of the run (no entry
   * is ever freed: RNTI churn over minutes exceeds 64). Reuse the least recently active entry idle for >= 2 s of signal. */
  int lru = -1;
  const uint64_t now = plan_ctx()->now_abs;
  for (int i = 0; i < SOLVER_RNTIS; i++)
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
 *   Threshold LAYOUT_RECOVER failed probes; sets layouts_reopened and logs the event.
 *   Caller: nr_pdcch_blind_monitor_rt.c (layout pruning). Takes g_mu.
 */
bool nr_pdsch_solver_reopen_layouts(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  solver_rnti_t *r = slot_of(rnti, false, NULL);
  if (r && !r->solved && !r->layouts_reopened
      && r->layout_failures >= (uint32_t)LAYOUT_RECOVER) {
    r->layouts_reopened = true;
    LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x reopening pruned layouts after %u failed CRC probes\n",
          rnti, r->layout_failures);
  }
  const bool reopened = r && r->layouts_reopened;
  pthread_mutex_unlock(&g_mu);
  return reopened;
}

/* nr_pdsch_solver_layouts: the solved RNTI's candidate layout bitmap as an id list (see the header). Takes g_mu. */
int nr_pdsch_solver_layouts(uint16_t rnti, uint16_t *ids, int max)
{
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  int n = -1;
  if (r && r->solved && r->cand_bm && ids && max > 0) {
    n = 0;
    for (int b = 0; b < BM_BYTES && n >= 0; b++)
      for (int k = 0; r->cand_bm[b] && k < 8; k++)
        if ((r->cand_bm[b] >> k) & 1) {
          if (n >= max) { n = -1; break; }
          ids[n++] = (uint16_t)(b * 8 + k);
        }
  }
  pthread_mutex_unlock(&g_mu);
  return n;
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

/* TIMBIMODAL: an RNTI's solution seeds the other RNTIs (cell prior, CORESET scrambling, reuse) only after it decoded a full TB
 * with CRC OK. Two chance CRC-16 probe passes (tiny low-rate TBs) once "solved" OTA TIM run ab_Tr2_1_094729 on a wrong layout,
 * and that prior and scrambling stayed for the whole run. */

/* ---- DCIRV: HARQ/RV consistency of a layout's NDI / RV / pid readings (see nr_pdsch_solver.h) ---- */
#define RVC_P0 0.15   /* assumed P(violation | true layout): missed DCIs, rv sequences other than 0,2,3,1 */
#define RVC_Q 0.5     /* a wrong layout violates at least about this often (random NDI/RV bits; a one-bit shift: rv = pid MSB) */
#define RVC_ALPHA 1e-3
#define RVC_MAX_SCORED 256 /* layouts scored per selection (cost bound) */
void nr_pdsch_solver_set_hq_fn(nr_pdsch_solver_hq_fn fn, void *ctx)
{
  pthread_mutex_lock(&g_mu);
  g_hq_fn = fn;
  g_hq_ctx = ctx;
  pthread_mutex_unlock(&g_mu);
}
bool nr_pdsch_solver_rvc_on(void)
{
  /* always on since CRC-BUGS 2026-10-19: TIM PCI 923 OTA, 147 of 148 rv != 0 DCIs were read while the representative was
   * the lowest-id layout among the CRC-equivalent ones, with the MCS 2 bits early; the true layout reads rv 0 on all 6000 RMDCI rows. */
  return true;
}
/* P(X >= k) for X ~ Binomial(n, p) (lower = false) or P(X <= k) (lower = true). */
static double binom_tail(int n, int k, double p, bool lower)
{
  double s = 0;
  for (int j = lower ? 0 : k; j <= (lower ? k : n); j++)
    s += exp(lgamma(n + 1.0) - lgamma(j + 1.0) - lgamma(n - j + 1.0) + j * log(p) + (n - j) * log1p(-p));
  return s > 1 ? 1 : s;
}
bool nr_pdsch_solver_hq_refuted(int n, int bad) { return n > 0 && bad > 0 && binom_tail(n, bad, RVC_P0, false) < RVC_ALPHA; }
bool nr_pdsch_solver_hq_verified(int n, int bad) { return n > 0 && binom_tail(n, bad, RVC_Q, true) < RVC_ALPHA; }
/* rv_next_ok: retransmission rv (!= 0) follows the last rv of its pid in the order 0,2,3,1 by one step, or two (a missed DCI). */
static bool rv_next_ok(int last, int rv)
{
  static const int pos[4] = {0, 3, 1, 2}; /* rv -> position in 0,2,3,1 */
  const int d = pos[rv & 3] - pos[last & 3];
  return d == 1 || d == 2;
}
/* hq_score: the layout's readings over the RNTI's payload ring, oldest first. n = grants whose pid was seen before (their NDI
 * toggle is known), bad = new transmissions with rv != 0 plus retransmissions with rv != 0 that do not follow the pid's last rv. */
static void hq_score(const solver_rnti_t *r, int layout, int *n, int *bad)
{
  uint8_t seen[16] = {0}, lndi[16], lrv[16];
  const uint32_t cnt = r->hq_cnt < HQ_RING ? r->hq_cnt : HQ_RING, first = r->hq_cnt < HQ_RING ? 0 : r->hq_cnt % HQ_RING;
  *n = *bad = 0;
  for (uint32_t i = 0; g_hq_fn && i < cnt; i++) {
    uint8_t ndi, rv, pid;
    if (!g_hq_fn(g_hq_ctx, (uint16_t)layout, r->hq_ring[(first + i) % HQ_RING], &ndi, &rv, &pid)) continue;
    pid &= 15;
    if (seen[pid]) {
      (*n)++;
      *bad += (ndi != lndi[pid]) ? rv != 0 : (rv != 0 && !rv_next_ok(lrv[pid], rv));
    }
    seen[pid] = 1;
    lndi[pid] = ndi;
    lrv[pid] = rv;
  }
}
static bool bm_has(const uint8_t *bm, int id) { return bm && ((bm[id >> 3] >> (id & 7)) & 1); }
/* seed_ok: a solution may seed the cell / other RNTIs: a CRC pass at MCS > 0 fixed the MCS offset, or the HARQ readings verify it. */
static bool seed_ok(const solver_rnti_t *r)
{
  if (!g_hq_fn || r->mcs_pos) return true;
  int n, bad;
  hq_score(r, r->rep, &n, &bad);
  return nr_pdsch_solver_hq_verified(n, bad);
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
    memcpy(g_prior_L_by_tda, r->L_by_tda, sizeof(g_prior_L_by_tda));
  }
}

/* try_promote: promote() once the solution may seed (DCIRV seed_ok; without the flag always). Under g_mu. */
static void try_promote(solver_rnti_t *r)
{
  if (r->promoted || !seed_ok(r)) return;
  r->promoted = true;
  promote(r);
}

/* TIMSOLVE2: an RNTI solves only on statistically significant CRC evidence. A wrong
 * hypothesis passes a CRC16 code block (TBS <= 3824, TS 38.212 7.2.1) with probability 2^-16 and a CRC24 one with 2^-24.
 * Bonferroni over every hypothesis tried: under "all wrong" the passes of an RNTI are Poisson with mean cred16 2^-16 (CRC16)
 * and cred24 2^-24 (CRC24), cred = its credited search probes so far. The candidate's consistent passes (distinct DCIs)
 * solve when P(at least that many chance passes) < SIGNIF_ALPHA. So one CRC24 pass solves at once, two CRC16 passes solve
 * a quiet RNTI, and a busy one (thousands of probes: OTA ab_Tr2_1_094729 0x47f0, solved on 2 chance passes) needs a third.
 * SIGNIF_ALPHA is a false-solve rate per RNTI, not a cell parameter. */
#define SIGNIF_ALPHA 1e-3
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
static bool tb_crc24(int table, int mcs, int nrb, int S, int L, int l0, int add_pos, int nl)
{
  int qm, r2;
  if (nrb <= 0 || !nr_pdsch_solver_mcs_qr(table, mcs, &qm, &r2)) return false;
  const int nd = __builtin_popcount(nr_pdsch_solver_dmrs_mask(l0, S, L, add_pos));
  int nre = 12 * L - 12 * nd;
  if (nre > 156) nre = 156;
  return (double)nre * nrb * (r2 / 2048.0) * qm * (nl > 0 ? nl : 1) > 3824.0;
}

/* TIMSOLVE2: reuse the CRC-proven solutions' layout dimensions (reading, S map,
 * add_pos, MCS table) on every grant of an unsolved RNTI with each of ITS OWN blind identities, in the identity form
 * (data scrambling = fixed id / = DM-RS id / default) the proven solutions use. Replaces the single per-grant scrambling
 * hypothesis of the cell prior. OTA TIM (ab_Tt1/Tt3, 2026-10-11): after the first solve, 73-78 % of the probes of RNTIs
 * that never decoded went to identity forms or dimensions no solved RNTI of the cell uses. */
/* scr_form: identity form of a scrambling hypothesis: -1 default ids, 0 data id = DM-RS id, D + 1 data id = D. */
static int scr_form(int32_t scr) { return scr < 0 ? -1 : (scr >> 16); }

/* nr_pdsch_solver_fair_budget: the probe rate is divided equally among the unsolved RNTIs
 * active in the last 0.2 s (a per-RNTI token bucket of rate/n_active, capped at `budget`), so a busy RNTI whose search
 * keeps failing cannot take the whole probe budget from the others (replay TIM 12 s: one RNTI planned 65 % of all probes,
 * the others got no budget on up to half of their grants: its probes kept the decode ring above the admission fill). A
 * new RNTI starts with a full budget (prompt cold start). Work-conserving: the share caps only when the bucket cannot
 * refill to its burst now (contended); with spare capacity every RNTI plans its full budget (replay TIM 12 s
 * at 0.25x: an always-on cap left capacity unused and solved fewer RNTIs). Returns the probes this grant may plan. */
int nr_pdsch_solver_fair_budget(uint16_t rnti, double rate_per_slot, bool contended, int budget)
{
  if (budget <= 0 || rate_per_slot <= 0.0)
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
  pthread_mutex_lock(&g_mu);
  solver_rnti_t *r = slot_of(rnti, true, NULL); /* a never-planned RNTI takes a table entry */
  if (r) r->why[why]++;
  pthread_mutex_unlock(&g_mu);
}

static int comb_ci(uint8_t comb);
/* nr_pdsch_solver_note_snr: feeds one post-equalisation SNR sample (dB) of a full decode into the RNTI's exponential moving average.
 *   EMA weight 0.2 for new samples; the first sample initialises it. Ignored for unknown RNTIs.
 *   Caller: nr_pdsch_passive_queue.c (after a full decode, from its EVM).
 */
void nr_pdsch_solver_note_snr(uint16_t rnti, float snr_db, int comb, bool crc_ok)
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
 *   Writes the plan context (per thread, see plan_ctx). Caller: nr_pdcch_blind_monitor_rt.c before planning.
 */
void nr_pdsch_solver_set_alt_scr(int key, const uint16_t *ids, int n, bool first_is_decided)
{
  solver_plan_ctx_t *ctx = plan_ctx();
  ctx->n = 0;
  ctx->key = key;
  uint32_t h = 2166136261u;
  for (int i = 0; i < n && ctx->n < 16; i++) {
    ctx->ids[ctx->n++] = ids[i];
    h = (h ^ ids[i]) * 16777619u;
  }
  ctx->hash = h ^ (uint32_t)ctx->n;
  ctx->decided = first_is_decided && ctx->n > 0;
  ctx->own = false;
}

void nr_pdsch_solver_set_alt_scr32(int key, const int32_t *ids, int n, bool first_is_decided, bool own)
{
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
}

/* nr_pdsch_solver_set_now: sets the absolute slot time (abs_slot, signal time) of the grant about to be planned, used for solve-time statistics.
 *   Writes the plan context under g_mu (unless thread-context mode). Caller: nr_pdcch_blind_monitor_rt.c.
 */
void nr_pdsch_solver_set_now(uint64_t abs_slot)
{
  plan_ctx()->now_abs = abs_slot;
}

/* nr_pdsch_solver_set_slot_last: SOLVERTDDEND. Sets the last DL symbol (0..13) of the slot of the grant about to be planned, as read
 *   from the cell's broadcast TDD pattern (SIB1 / learnt), or -1 when unknown. Per scan consumer thread, like set_now; the caller sets
 *   it for every grant. Caller: nr_pdcch_blind_monitor_rt.c.
 */
void nr_pdsch_solver_set_slot_last(int last)
{
  plan_ctx()->last_dl = (int8_t)(last >= 0 && last <= 13 ? last : -1);
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
static void refund_untested(const pending_t *p);
static pending_t *pend_new(void)
{
  static uint32_t cur;
  pending_t *p = &g_p[cur]; /* FIFO: the oldest entry is the one most likely lost in the queue */
  if (p->used) {
    g_st.dropped++;
    refund_untested(p); /* SOLVERREFUND: its feedback can no longer arrive; the probe counts as untested */
  }
  cur = (cur + 1) % SOLVER_PENDING;
  return p;
}

static int t_rm = 0; static bool t_learn_rm = false; /* set by plan for this grant (under g_mu) */
static int32_t t_scr = -1; /* T26c scrambling hypothesis for this grant (-1 default) */
static uint8_t t_comb; /* T26d combining mode for this grant's variants (0 = default) */
/* Receivers in play per RNTI (frozen ISAC_RX_MRC_PER_RNTI=1): 1 best branch, 3 MRC. comb_k maps a receiver index to the id stored in the pending entry. */
static uint8_t comb_k(int k) { return k ? 3 : 1; }
static int comb_ci(uint8_t comb) { return comb == 3 ? 1 : comb == 4 ? 2 : comb >= 5 ? 3 : 0; }
/* comb_pick: chooses the receiver id for one grant of an RNTI. Unsolved RNTIs always use the configured receiver (0).
 *   Solved RNTIs: best receiver by Laplace-smoothed CRC rate (r->comb_try/comb_ok, filled by nr_pdsch_solver_feedback()), exploring the other on 1 grant in 8.
 *   Caller: nr_pdsch_solver_plan_slot().
 */
static uint8_t comb_pick(const solver_rnti_t *r)
{
  if (!r->solved) return 0; /* the layout search runs on the configured receiver only (alternating receivers while
                            * unsolved multiplied the search space and starved whole populations: replays n3/n4) */
  int best = 0;
  double bp = -1.0;
  for (int k = 0; k < 2; k++) {
    const double p = (r->comb_ok[k] + 1.0) / (r->comb_try[k] + 2.0);
    if (p > bp) { bp = p; best = k; }
  }
  if ((r->grants % 8u) == 7u) return comb_k(1 - best);
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
  const int L = t_L > 0 ? t_L : 14 - S;
  for (int i = 0; i < *n; i++) /* never the same decode twice for one grant */
    if (out[i].reading == reading && out[i].S == S && out[i].L == L && out[i].add_pos == add_pos && out[i].table == table && out[i].rm == t_rm
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
  p->ov = t_ov;
  p->S = (uint8_t)S; p->L = (uint8_t)L; p->add_pos = (uint8_t)add_pos; p->table = (uint8_t)table; p->probe = probe;
  p->table_disc = table_disc; p->learn_S = learn_S; p->learn_table = learn_table; p->tda = tda;
  p->dci = dci;
  p->rm = (uint8_t)t_rm;
  p->scr = t_scr;
  p->comb = t_comb;
  p->snr = NAN;
  p->n_layers = rd[reading].n_layers;
  p->mcs = rd[reading].mcs;
  p->crc24 = tb_crc24(table, rd[reading].mcs, rd[reading].num_rb, S, L, l0, add_pos, rd[reading].n_layers);
  p->dmrs_ports = rd[reading].dmrs_ports;
  p->learn_rm = t_learn_rm;
  p->payload = payload;
  p->n_ids = rd[reading].n_ids < SOLVER_MAX_IDS ? rd[reading].n_ids : SOLVER_MAX_IDS;
  memcpy(p->ids, rd[reading].ids, (size_t)p->n_ids * sizeof(uint16_t));
  nr_pdsch_solver_var_t *v = &out[(*n)++];
  v->reading = reading;
  v->S = (uint8_t)S;
  v->L = (uint8_t)L;
  v->add_pos = (uint8_t)add_pos;
  v->table = (uint8_t)table;
  v->dmrs_mask = nr_pdsch_solver_dmrs_mask(l0, S, L, add_pos);
  v->probe = probe;
  v->token = p->token;
  v->rm = (uint8_t)t_rm;
  v->scr = t_scr;
  v->comb = t_comb;
  return true;
}

/* dmrs_supported: some reading of this DCI is well supported by the DM-RS of this slot (fit >= 0.8): the RNTI's PDSCH is on air
 * where a layout says, so its search is not backed off (DCI11-RESOLVER: the back-off is a compute bound for searches the air gives
 * no support to, not a prior). */
static bool dmrs_supported(const nr_pdsch_solver_reading_t *rd, int n)
{
  for (int i = 0; i < n; i++)
    if (rd[i].fit >= 0.8f) return true;
  return false;
}

/* search_backoff: an RNTI with no candidate and at least 200 probes spent is searched on 1 grant in 8 only (frozen ISAC_PDSCH_SOLVER_BACKOFF=200).
 *   Caller: nr_pdsch_solver_plan_slot().
 */
static bool search_backoff(const solver_rnti_t *r)
{
  return !r->has_cand && r->n_probe >= 200u && (r->grants % 8u) != 0;
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
static uint64_t coverage_key_scr(int id, int variant, nr_dci_bits_t payload, int32_t scr)
{
  const int tda = variant == 63 ? 0 : tda_of(id, payload); /* 63 = all timing cases' trial count */
  return (((((uint64_t)(uint32_t)(scr + 1) << 16) | (unsigned)id) << 4 | tda) << 6 | variant) + 1;
}
static uint64_t coverage_key(int id, int variant, nr_dci_bits_t payload) { return coverage_key_scr(id, variant, payload, t_scr); }

/* coverage_refund: SOLVERREFUND (2026-10-24). plan_coverage() charges a hypothesis to the coverage table when it PLANS the probe, but a
 * planned probe can still be refused before any decode (probe token bucket / ring fill in pdsch_gate_admit, per-occasion cap, full
 * queue, expired IQ): its feedback arrives with credit=false. Such a hypothesis was never tested, yet it stayed counted, so the
 * least-tried-first search skipped it for a whole coverage round. [MEASURED, sens6 rfsim 273 PRB 4 RX @4a94af8c0b] 3933 of 7659
 * planned probes were throttled (106 PRB: 0 of 591); the true (reading, S=1, add_pos=1, table 0) was planned, throttled and not
 * planned again in 100 s, so the RNTI never solved. Undo the planning-time charge of an uncredited probe. Caller: feedback, g_mu held. */
static void coverage_refund(solver_rnti_t *r, const pending_t *p)
{
  if (!p->cov_v1 || !r->coverage || !r->coverage->entries) return;
  solver_coverage_t *c = r->coverage;
  const int nc = p->n_ids < COVERAGE_IDS ? p->n_ids : COVERAGE_IDS; /* the ids plan_coverage() charged */
  for (int j = 0; j < nc; j++) {
    const int id = p->ids[j];
    if (id >= NR_PDSCH_SOLVER_MAX_LAYOUTS) continue;
    for (int q = 0; q < 2; q++) {
      const uint64_t key = coverage_key_scr(id, q ? 63 : p->cov_v1 - 1, p->payload, p->scr);
      coverage_entry_t *e = coverage_find(c, key);
      if (e->key == key && e->count > 0) e->count--; /* absent: the table started a new round since the plan */
    }
  }
}

/* coverage_note_snr: EVIDENCE RANK. Adds a credited search probe's measured DM-RS SNR (the comb SNR of the probe's own DM-RS symbol / port
 *   / CDM-group hypothesis, nr_pdsch_passive_queue.c) to the coverage entries it was charged to (its layouts x variant, and the layouts'
 *   aggregate). It is a CRC-free likelihood of the DM-RS part of the hypothesis: a wrong start symbol, additional-position set or port set
 *   correlates no DM-RS (sens6 OTA lab cell 2026-10-25: true-shape probes ~30-35 dB, wrong ones <= 0-17 dB). It does not depend on the MCS
 *   table, whose hypotheses tie and keep the uniform least-tried share. g_mu held. */
static void coverage_note_snr(const pending_t *p)
{
  if (!p->cov_v1 || p->phase != 0 || !p->probe || !isfinite(p->snr) || p->slot < 0 || p->slot >= SOLVER_RNTIS) return;
  solver_rnti_t *r = &g_r[p->slot];
  if (!r->used || r->solved || !r->coverage || !r->coverage->entries) return;
  const int nc = p->n_ids < COVERAGE_IDS ? p->n_ids : COVERAGE_IDS;
  for (int j = 0; j < nc; j++) {
    const int id = p->ids[j];
    if (id >= NR_PDSCH_SOLVER_MAX_LAYOUTS) continue;
    for (int q = 0; q < 2; q++) {
      const uint64_t key = coverage_key_scr(id, q ? 63 : p->cov_v1 - 1, p->payload, p->scr);
      coverage_entry_t *e = coverage_find(r->coverage, key);
      if (e->key != key) continue; /* the table started a new round since the plan */
      e->snr_n++;
      e->snr_sum += p->snr;
      e->snr_sum2 += p->snr * p->snr;
    }
  }
}

void nr_pdsch_solver_note_probe_snr(uint32_t token, float snr_db)
{
  if (token == 0 || !isfinite(snr_db)) return;
  pthread_mutex_lock(&g_mu);
  pending_t *q = &g_p[token & (SOLVER_PENDING - 1)];
  if (q->used && q->token == token) q->snr = snr_db;
  pthread_mutex_unlock(&g_mu);
}

/* refund_untested: an unsolved RNTI's search probe that was never tested (uncredited feedback, or its pending entry overwritten
 * before any feedback) neither consumes search coverage nor counts toward the search back-off. g_mu held. */
static void refund_untested(const pending_t *p)
{
  if (!p->used || p->phase != 0 || !p->probe || p->slot < 0 || p->slot >= SOLVER_RNTIS) return;
  solver_rnti_t *r = &g_r[p->slot];
  if (!r->used || r->solved) return;
  coverage_refund(r, p);
  if (r->n_probe > 0) r->n_probe--;
}

/* EVIDENCE RANK statistics of one hypothesis: mean and variance of its probes' SNR (dB). */
typedef struct { uint32_t n; double mean, var; } ev_stat_t;
/* ev_take: keeps, of the entries offered, the one with the highest mean (a reading or variant is as good as its best layout). */
static void ev_take(ev_stat_t *s, const coverage_entry_t *e)
{
  if (!e->snr_n) return;
  const double m = e->snr_sum / e->snr_n;
  if (s->n && m <= s->mean) return;
  s->n = e->snr_n;
  s->mean = m;
  s->var = e->snr_n > 1 ? fmax(0.0, (e->snr_sum2 - e->snr_n * m * m) / (e->snr_n - 1)) : 0.0;
}
/* ev_outranks: a's mean SNR is significantly above b's (Welch statistic > 3.09, one-sided Q = 1e-3); needs >= 2 probes each. */
static bool ev_outranks(const ev_stat_t *a, const ev_stat_t *b)
{
  if (a->n < 2 || b->n < 2) return false;
  const double se = sqrt(a->var / a->n + b->var / b->n);
  return a->mean - b->mean > 3.09 * se && a->mean > b->mean;
}

/* plan_coverage: planner: fills the grant's budget with the least-tried physical hypotheses across readings.
 *   Scores each (reading, S, add_pos, table) variant by how often it was tried, balances the budget per layout, and emits up to budget variants.
 *   Records every emitted trial in the RNTI's coverage table. Caller: nr_pdsch_solver_plan_slot() (unsolved RNTIs).
 */
static void plan_coverage(solver_rnti_t *r, nr_pdsch_solver_var_t *out, int *n, int max, int slot,
                          const nr_pdsch_solver_reading_t *rd, const int *order, int no,
                          int l0, uint64_t dci, nr_dci_bits_t payload)
{
  if (!r->coverage) r->coverage = calloc(1, sizeof(*r->coverage));
  if (!r->coverage || !coverage_room(r->coverage, 2 * SOLVER_MAX_IDS)) return;
  solver_coverage_t *c = r->coverage;
  static const uint8_t starts[4] = {1, 0, 2, 3}, adds[4] = {1, 2, 0, 3}, tables[3] = {1, 0, 2};
  uint32_t score[128][48];
  uint32_t load[128];
  int nt[128];
  ev_stat_t vsnr[128][48]; /* EVIDENCE RANK: measured probe SNR of the variant (its best-mean layout); n = 0: never measured */
  ev_stat_t rsnr[128];     /* ... and of the reading (its layouts' aggregate) */
  /* EVIDENCE WEIGHT (DCI11-RESOLVER): a reading's share of the probes is proportional to its DM-RS fit (rd.fit, the monitor's
   * F1 of the reading's PRBs against the PRBs that carry this RNTI's DM-RS in this slot), so the readings the air supports are
   * probed first and most, and a reading with no DM-RS support still gets a small share (never zero: a wrong identity or a weak
   * slot can make the truth look unsupported). Without any fit on this DCI every reading weighs the same (the coverage order). */
  double w[128];
  bool any_fit = false;
  for (int k = 0; k < no; k++) any_fit |= rd[order[k]].fit >= 0.f;
  for (int k = 0; k < no; k++) w[k] = !any_fit ? 1.0 : rd[order[k]].fit >= 0.f ? 0.05 + rd[order[k]].fit : 0.5;
  for (int k = 0; k < no; k++) {
    load[k] = UINT32_MAX;
    const int nc = rd[order[k]].n_ids < COVERAGE_IDS ? rd[order[k]].n_ids : COVERAGE_IDS;
    for (int j = 0; j < nc; j++) {
      const int id = rd[order[k]].ids[j];
      if (id >= NR_PDSCH_SOLVER_MAX_LAYOUTS) continue;
      const uint32_t count = coverage_find(c, coverage_key(id, 63, payload))->count;
      if (count < load[k]) load[k] = count;
    }
    rsnr[k] = (ev_stat_t){0};
    for (int j = 0; j < nc; j++) {
      const int id = rd[order[k]].ids[j];
      if (id >= NR_PDSCH_SOLVER_MAX_LAYOUTS) continue;
      ev_take(&rsnr[k], coverage_find(c, coverage_key(id, 63, payload)));
    }
    uint8_t available[3];
    nt[k] = tables_for(rd[order[k]].mcs, available);
    for (int v = 0; v < 48; v++) {
      score[k][v] = UINT32_MAX;
      vsnr[k][v] = (ev_stat_t){0};
      const int table = tables[(v / 4) % 3];
      bool valid = false;
      for (int t = 0; t < nt[k]; t++) valid |= available[t] == table;
      if (!valid || starts[v % 4] > l0) continue;
      for (int j = 0; j < nc; j++) {
        const int id = rd[order[k]].ids[j];
        if (id >= NR_PDSCH_SOLVER_MAX_LAYOUTS) continue;
        const coverage_entry_t *e = coverage_find(c, coverage_key(id, v, payload));
        if (e->count < score[k][v]) score[k][v] = e->count;
        ev_take(&vsnr[k][v], e);
      }
    }
  }
  /* EVIDENCE RANK. Each hypothesis's share of the probes falls with its rank in the measured SNR ordering (share ~ 1/rank: Zipf), for
   * the readings of this grant and for the variants within a reading; a never-measured hypothesis ranks first, so every hypothesis is
   * still tried and the order only decides how the rest of the budget is spent. Rank = 1 + the number of hypotheses whose mean SNR is
   * SIGNIFICANTLY higher (ev_outranks: Welch difference > 3.09 sigma, Q = 1e-3, the level of the other screens here): statistically tied
   * hypotheses share a rank, so where wrong hypotheses equalise as well as the true one (rfsim: a strict order of near-equal means
   * slowed the solve from ~2k to 3.5-5.2k probes) the allocation stays the uniform least-tried one. No SNR threshold, no cell scale.
   * Before (uniform least-tried): 270 of 100634 probes tested the true (S, add_pos, table, layout) before its 2 solving CRC passes,
   * 56 of them at the true rank (sens6 OTA lab cell 2026-10-25 03:53). */
  int rrank[128], vrank[128][48];
  for (int k = 0; k < no; k++) {
    rrank[k] = 1;
    for (int q = 0; q < no; q++)
      if (ev_outranks(&rsnr[q], &rsnr[k])) rrank[k]++;
    for (int v = 0; v < 48; v++) {
      vrank[k][v] = 1;
      for (int u = 0; u < 48; u++)
        if (score[k][u] != UINT32_MAX && ev_outranks(&vsnr[k][u], &vsnr[k][v])) vrank[k][v]++;
    }
  }
  for (int step = 0; step < no * 48 && *n < max; step++) {
    int best = -1, variant = -1;
    uint32_t count = UINT32_MAX;
    double total = 1e300;
    /* Fairness is per layout, not per TDRA: a wrong wide TDRA field must not get
     * sixteen times the budget of a narrow one merely by producing new indices.
     * Within that share (weighted by the DM-RS evidence) choose the least-tried eligible physical hypothesis. */
    for (int v = 0; v < 48; v++)
      for (int k = 0; k < no; k++) {
        if (score[k][v] == UINT32_MAX) continue;
        const double pk = ((double)load[k] + 1.0) * rrank[k] / w[k];
        const uint32_t ev = (score[k][v] + 1u) * (uint32_t)vrank[k][v];
        if (pk < total || (pk == total && ev < count)) {
          best = k; variant = v; count = ev; total = pk;
        }
      }
    if (best < 0 || !coverage_room(c, 2 * rd[order[best]].n_ids)) break;
    if (load[best] < UINT32_MAX - 1) load[best]++;
    score[best][variant] = UINT32_MAX; /* no duplicate physical decode in this DCI */
    const int S = starts[variant % 4], table = tables[(variant / 4) % 3], add = adds[variant / 12];
    const int n_before = *n;
    emit(out, n, max, slot, 0, order[best], rd, l0, S, add, table, true, nt[best] > 1, false, false, -1, dci, payload);
    if (*n > n_before) g_p[out[n_before].token & (SOLVER_PENDING - 1)].cov_v1 = (uint8_t)(variant + 1);
    const int nc = rd[order[best]].n_ids < COVERAGE_IDS ? rd[order[best]].n_ids : COVERAGE_IDS;
    for (int j = 0; j < nc; j++) {
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
                       const int8_t *S_by_tda, const int8_t *L_by_tda, int mcs, uint64_t dci, nr_dci_bits_t payload)
{
  uint8_t tl[3];
  const int nt = tables_for(rd[reading].mcs, tl);
  if (nt == 0)
    return;
  const int tda = tda_of(rep, payload);
  const int S = S_by_tda[tda];
  const bool t_known = table >= 0 || nt == 1;
  const int t_use = table >= 0 ? table : tl[0];
  const int Lk = L_by_tda ? L_by_tda[tda & 15] : -1;
  if (phase == 1 && S >= 0 && t_known) {
    t_L = Lk > 0 ? Lk : 0;
    emit(out, n, max, slot, phase, reading, rd, l0, S, add_pos, t_use, false, nt > 1, false, false, tda, dci, payload);
    t_L = 0;
    return;
  }
  /* SOLVERTDDL: a solved RNTI's TDA index with no S yet is also searched with PDSCH lengths shorter than 14 - S (TDD special
   * slot: the PDSCH ends where the DL symbols end; every length 14 - S probe fails there and S would never be learnt). 4 (S, end
   * symbol) hypotheses per grant, rotating, alternating with the full-length ones so that neither starves under the probe bucket. */
  static const uint8_t kE[10] = {5, 9, 10, 11, 8, 7, 6, 12, 4, 3};
  const uint32_t lc = (phase == 1 && S < 0) ? g_lcur[slot][tda & 15]++ : 0;
  for (int pass = 0; pass < 2; pass++) {
    const bool shortp = (pass == 0) == ((lc & 1u) != 0u);
    if (shortp) {
      if (!(phase == 1 && S < 0)) continue;
      for (int q = 0; q < 4; q++) {
        const uint32_t j = ((lc >> 1) * 4u + (uint32_t)q) % 40u;
        const int s = kS[j % 4], e = kE[j / 4], Ls = e - s + 1;
        if (e < l0 || Ls < 2) continue;
        t_L = Ls;
        for (int ti = 0; ti < (t_known ? 1 : nt); ti++)
          emit(out, n, max, slot, phase, reading, rd, l0, s, add_pos, t_known ? t_use : tl[ti], true, nt > 1, true,
               !t_known && nt > 1, tda, dci, payload);
        t_L = 0;
      }
      continue;
    }
    for (int si = 0; si < 4; si++) {
      const int s = S >= 0 ? S : kS[si];
      t_L = (S >= 0 && Lk > 0) ? Lk : 0;
      for (int ti = 0; ti < (t_known ? 1 : nt); ti++)
        emit(out, n, max, slot, phase, reading, rd, l0, s, add_pos, t_known ? t_use : tl[ti], true, nt > 1, S < 0,
             !t_known && nt > 1, tda, dci, payload);
      t_L = 0;
      if (S >= 0)
        break;
    }
  }
}

/* plan_ov_probes: SOLVERPHASE search of a failing slot phase: 4 (S, end symbol) hypotheses per grant, rotating over S in {1,0,2,3} and
 * end symbols of a TDD special slot, as first-code-block probes with the RNTI's solved add_pos / table. A pass sets the phase's (S, L). */
static void plan_ov_probes(solver_rnti_t *r, int ph, nr_pdsch_solver_var_t *out, int *n, int max, int slot, int ri,
                           const nr_pdsch_solver_reading_t *rd, int l0, int e_first, uint64_t dci, nr_dci_bits_t payload)
{
  static const uint8_t kE[10] = {5, 9, 10, 11, 8, 7, 6, 12, 4, 3};
  uint8_t tl[3];
  const int nt = tables_for(rd[ri].mcs, tl);
  if (nt == 0) return;
  const int table = r->table >= 0 ? r->table : tl[0];
  t_ov = true;
  for (int q = 0; q < 4; q++) {
    /* SOLVERTDDEND: the slot's broadcast DL end is tried first, with each start symbol (the first 4 hypotheses of the phase); the
     * rotation over the other end symbols follows (a gNB may also use flexible symbols: the TDD end orders, it does not exclude) */
    const uint32_t j0 = r->ov_cur[ph]++;
    const uint32_t j = j0 % 40u;
    const int s = kS[j % 4], e = (e_first >= 0 && j0 < 4u) ? e_first : kE[j / 4], Ls = e - s + 1;
    if (e < l0 || Ls < 2) continue;
    t_L = Ls;
    emit(out, n, max, slot, 1, ri, rd, l0, s, r->add_pos, table, true, nt > 1, false, false, -1, dci, payload);
  }
  t_L = 0;
  t_ov = false;
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
 *   Logs per-RNTI statistics every 500/1000 grants. Takes g_mu itself.
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
  /* DCI11-RESOLVER: no width limit and no modulation preference -- every reading is a candidate, ordered and weighted by the DM-RS
   * fit (plan_coverage) and decided by CRC. The former 48-RB / BWP-width search cap and the one-layer-QPSK preference were priors
   * that kept a cell's true reading out of the search (full-band grants, 64QAM). */
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
    for (int i = 0; i < SOLVER_RNTIS; i++) {
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
  t_L = 0;
  t_learn_rm = false;
  t_scr = r->solved ? r->scr : -1;
  t_comb = comb_pick(r);
  if (r->solved) {
    int ri = -1;
    const int ph = (int)(ctx->now_abs % SOLVER_PHASES);
    for (int i = 0; i < n_rd; i++)
      if (has_id(&rd[i], r->rep)) { ri = i; break; }
    if (ri < 0) {
      /* no reading of this DCI comes from the solution's layout: decoding reading 0 would use another layout's MCS/NDI/rv (CRC-BUGS
       * 2026-10-19), so there is no full decode; the refinement probes below still run */
      r->why[W_NOREP]++;
    } else if (r->ov_S[ph] >= 0 && !(csirs_slot && r->rm < 0) && r->L_by_tda[tda_of(r->rep, payload) & 15] < 0) {
      /* SOLVERPHASE, now a FALLBACK (DCI11-RESOLVER): this phase's own allocation is used only while the representative's TDA
       * index has no short length of its own; once the TDA field explains the mixed slot (evidence above), the field decides. */
      uint8_t tl0[3];
      if (tables_for(rd[ri].mcs, tl0) > 0) {
        t_rm = csirs_slot ? r->rm : 0;
        t_ov = true;
        t_L = r->ov_L[ph];
        emit(out, &n, budget, slot, 1, ri, rd, l0, r->ov_S[ph], r->add_pos, r->table >= 0 ? r->table : tl0[0], false, false, false,
             false, -1, dci, payload);
        t_L = 0;
        t_ov = false;
      }
    } else if (csirs_slot && r->rm < 0) { /* unknown: probe with and without, learn by CRC */
      for (int k = 1; k >= 0; k--) {
        t_rm = k;
        t_learn_rm = true;
        const int n0 = n;
        plan_known(out, &n, budget, slot, 1, ri, rd, l0, r->rep, r->add_pos, r->table, r->S_by_tda, r->L_by_tda, mcs, dci, payload);
        for (int q = n0; q < n; q++) out[q].probe = 1; /* a rate-match trial is a probe, never a counted TB */
      }
      t_learn_rm = false;
    } else {
      t_rm = csirs_slot ? r->rm : 0;
      plan_known(out, &n, budget, slot, 1, ri, rd, l0, r->rep, r->add_pos, r->table, r->S_by_tda, r->L_by_tda, mcs, dci, payload);
    }
    /* SOLVERTDDEND: when the slot's broadcast TDD pattern ends the DL before the allocation the solution would decode (S + L - 1 >
     * last DL symbol), the phase is searched from its first grant instead of after SOLVER_PHASE_FAIL failed full decodes [MEASURED,
     * sens6 rfsim 273 PRB 4 RX: 13 of 15 post-solve full-decode failures were mixed-slot grants decoded as full-slot ones while the
     * phase learner waited]. Without TDD knowledge (-1) the old rule stands. */
    bool tdd_short = false;
    if (ri >= 0 && ctx->last_dl >= 0 && ctx->last_dl < 13) {
      const int tda_h = tda_of(r->rep, payload) & 15;
      const int Sh = r->S_by_tda[tda_h] >= 0 ? r->S_by_tda[tda_h] : 1, Lh = r->L_by_tda[tda_h] > 0 ? r->L_by_tda[tda_h] : 14 - Sh;
      tdd_short = Sh + Lh - 1 > ctx->last_dl;
    }
    if (ri >= 0 && r->ov_S[ph] < 0 && r->ph_ok[ph] == 0 && !csirs_slot
        && ((r->ph_tr[ph] >= SOLVER_PHASE_FAIL && r->full_ok >= 20) || (tdd_short && r->full_ok >= 1)))
      plan_ov_probes(r, ph, out, &n, budget, slot, ri, rd, l0, tdd_short ? ctx->last_dl : -1, dci, payload);
    /* REFINEMENT: the solution's layouts can still disagree on fields two passes did not separate (e.g. where the
     * antenna-ports field sits). Each other reading those layouts give for THIS DCI gets one probe with the same
     * physical parameters; a pass narrows the layout set to that reading (feedback), and the representative follows. */
    const int S_here = r->S_by_tda[tda_of(r->rep, payload)];
    int refine = 0;
    for (int i = 0; i < n_rd && n < budget && refine < 4; i++) {
      if (i == ri) continue;
      int ov = 0;
      for (int k = 0; k < rd[i].n_ids && !ov; k++)
        ov = (r->cand_bm[rd[i].ids[k] >> 3] >> (rd[i].ids[k] & 7)) & 1;
      if (!ov) continue;
      uint8_t tl[3];
      const int nt = tables_for(rd[i].mcs, tl);
      if (nt == 0) break;
      { const int Lh = r->L_by_tda[tda_of(r->rep, payload) & 15]; t_L = (S_here >= 0 && Lh > 0) ? Lh : 0; }
      emit(out, &n, budget, slot, 1, i, rd, l0, S_here >= 0 ? S_here : 1, r->add_pos, r->table >= 0 ? r->table : tl[0],
           true, nt > 1, false, false, tda_of(r->rep, payload), dci, payload);
      t_L = 0;
      refine++;
    }
  } else if (csirs_slot) {
    r->why[W_CSIRS]++;
    /* no search on a slot carrying discovered CSI-RS: its rate matching is unknown, a miss would be false evidence */
  } else if (search_backoff(r) && !dmrs_supported(rd, n_rd)) {
    r->why[W_BACKOFF]++;
    /* T26c: an RNTI whose search has spent 200 probes without a single CRC pass is searched on 1 grant in 8 only --
     * the decode pool goes to RNTIs that decode */
  } else {
    /* 1. confirm a candidate: the reading sharing most layouts with it (under the candidate's own scrambling) */
    t_scr = r->has_cand ? r->cand_scr : -1;
    if (r->has_cand) {
      int best = -1, best_ov = 0;
      for (int i = 0; i < n_rd; i++) {
        int ov = 0;
        for (int k = 0; k < rd[i].n_ids; k++)
          ov += (r->cand_bm[rd[i].ids[k] >> 3] >> (rd[i].ids[k] & 7)) & 1;
        if (ov > best_ov) { best_ov = ov; best = i; }
      }
      if (best >= 0) {
        uint8_t tl[3];
        const int nt = tables_for(rd[best].mcs, tl);
        for (int si = 0; si < 4; si++)
          for (int ti = 0; ti < (r->cand_table >= 0 ? 1 : nt); ti++) {
            const int table = r->cand_table >= 0 ? r->cand_table : tl[ti];
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
    /* 2. the cell prior (another RNTI's solution), first. The CURRENT solution of the best
     * solved RNTI (most full TB CRC passes; layouts refined, S map and MCS table learnt since) instead of the snapshot
     * taken at the first solve -- gNBs configure UEs from one RRC template. Still confirmed by this RNTI's own 2 passes. */
    const solver_rnti_t *best = NULL;
    for (int k = 0; k < SOLVER_RNTIS; k++)
        if (g_r[k].used && g_r[k].solved && g_r[k].rep >= 0 && g_r[k].full_ok >= 1 && g_r[k].promoted && (!best || g_r[k].full_ok > best->full_ok))
          best = &g_r[k];
    bool ids_done = false;
    if (ctx->own) { /* TIMSOLVE2: proven dims x each own identity in the proven form */
      const solver_rnti_t *src[3];
      int ns = 0;
      for (int k = 0; k < SOLVER_RNTIS; k++) { /* CRC-proven solutions, most full CRC passes first, distinct reps */
        const solver_rnti_t *q = &g_r[k];
        if (!q->used || !q->solved || q->rep < 0 || q->full_ok < 1 || !q->promoted) continue;
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
                plan_known(out, &n, budget, slot, 0, i, rd, l0, src[j]->rep, src[j]->add_pos, src[j]->table, src[j]->S_by_tda, src[j]->L_by_tda,
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
          plan_known(out, &n, budget, slot, 0, i, rd, l0, best->rep, best->add_pos, best->table, best->S_by_tda, best->L_by_tda, mcs, dci,
                     payload);
          break;
        }
    } else if (g_have_prior) {
      for (int i = 0; i < n_rd; i++)
        if (has_id(&rd[i], g_prior_rep)) {
          plan_known(out, &n, budget, slot, 0, i, rd, l0, g_prior_rep, g_prior_add_pos, g_prior_table,
                     g_prior_S_by_tda, g_prior_L_by_tda, mcs, dci, payload);
          break;
        }
    }
    /* 3. the search: readings in DM-RS-fit order (best first; stable, so without a fit the caller's order stands), each
     * weighted by its fit in plan_coverage */
    int order[128], no = 0;
    for (int i = 0; i < n_rd && no < 128; i++)
      if (rd[i].num_rb > 0) order[no++] = i;
    for (int a = 1; a < no; a++)
      for (int b = a; b > 0 && rd[order[b]].fit > rd[order[b - 1]].fit; b--) {
        const int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
      }
    if (no == 0) r->why[W_WIDE]++;
    if (no > 0) /* (S, add_pos) tiers over the readings: every reading gets all S in one go, because the true S changes from grant to grant with the TDA index */
      plan_coverage(r, out, &n, budget, slot, rd, order, no, l0, dci, payload);
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
static bool ev_map(const solver_rnti_t *r, int id, int8_t *m, int8_t *mL)
{
  /* DCI11-RESOLVER: consistency is on the whole time allocation (S, L) of every CRC pass, not on S alone. A layout that reads no
   * TDA field (or the wrong bits) maps a TDD mixed slot's short PDSCH (rfsim 106 PRB: gNB row 2 = S1 L5) and a full slot's
   * (S1 L13) to the same index: inconsistent, so the layout whose TDA field tells them apart becomes the representative. */
  memset(m, -1, 16);
  memset(mL, -1, 16);
  for (int e = 0; e < r->n_ev; e++) {
    const int t = tda_of(id, r->ev_p[e]);
    if (m[t] >= 0 && (m[t] != r->ev_S[e] || mL[t] != r->ev_L[e])) return false;
    m[t] = (int8_t)r->ev_S[e];
    mL[t] = (int8_t)r->ev_L[e];
  }
  return true;
}
/* ev_apply: the S map of the chosen layout, and the PDSCH length of every index whose evidence is shorter than 14 - S (TDD mixed
 * slot). Same layout as before: an index whose passes have left the 16-entry evidence ring keeps what was learnt (a rare mixed-slot
 * index must not be forgotten behind full-slot passes); a new layout reads the indices differently, so it starts from its evidence. */
static void ev_apply(solver_rnti_t *r, int id, const int8_t *m, const int8_t *mL)
{
  const bool same = id == r->rep;
  for (int t = 0; t < 16; t++) {
    if (m[t] >= 0) {
      r->S_by_tda[t] = m[t];
      r->L_by_tda[t] = (mL[t] > 0 && mL[t] != 14 - m[t]) ? mL[t] : -1;
    } else if (!same) {
      r->S_by_tda[t] = -1;
      r->L_by_tda[t] = -1;
    }
  }
}
/* DCIRV: with hq, the candidates are scored by their HARQ/RV readings (refuted ones skipped, the
 * lowest violation rate wins, the current rep wins ties); without hq a solved RNTI keeps its rep while it stays consistent.
 * *all_refuted: some layout was consistent with the evidence but every one was refuted. */
static int choose_rep(solver_rnti_t *r, bool hq, bool *all_refuted)
{
  const bool rvc = g_hq_fn != NULL;
  int8_t m[16], bm[16], mL[16], bmL[16];
  int best = -1, scored = 0, refuted = 0;
  double best_ratio = 2;
  if (all_refuted) *all_refuted = false;
  if (rvc && !hq && r->solved && r->rep >= 0 && bm_has(r->cand_bm, r->rep) && ev_map(r, r->rep, m, mL)) {
    ev_apply(r, r->rep, m, mL);
    return r->rep;
  }
  for (int b = 0; b < BM_BYTES; b++) {
    if (!r->cand_bm[b])
      continue;
    for (int k = 0; k < 8; k++) {
      if (!((r->cand_bm[b] >> k) & 1))
        continue;
      const int id = b * 8 + k;
      if (!ev_map(r, id, m, mL))
        continue;
      if (!(rvc && hq)) {
        ev_apply(r, id, m, mL);
        return id;
      }
      if (scored++ >= RVC_MAX_SCORED) goto done;
      int n, bad;
      hq_score(r, id, &n, &bad);
      if (nr_pdsch_solver_hq_refuted(n, bad)) { refuted++; continue; }
      const double ratio = (bad + 0.5) / (n + 1.0);
      if (best < 0 || ratio < best_ratio - 1e-12 || (id == r->rep && ratio <= best_ratio + 1e-12)) {
        best = id;
        best_ratio = ratio;
        memcpy(bm, m, sizeof(m));
        memcpy(bmL, mL, sizeof(mL));
      }
    }
  }
done:
  if (best >= 0) ev_apply(r, best, bm, bmL);
  else if (all_refuted) *all_refuted = refuted > 0;
  return best;
}
/* add_ev: appends one CRC-passed (payload, S) pair to the RNTI's evidence list, dropping the oldest when 16 are stored.
 *   Caller: nr_pdsch_solver_feedback().
 */
static void add_ev(solver_rnti_t *r, nr_dci_bits_t p, uint8_t S, uint8_t L)
{
  if (r->n_ev >= 16) {
    memmove(r->ev_p, r->ev_p + 1, 15 * sizeof(r->ev_p[0])); memmove(r->ev_S, r->ev_S + 1, 15); memmove(r->ev_L, r->ev_L + 1, 15);
    r->n_ev = 15;
  }
  r->ev_p[r->n_ev] = p; r->ev_S[r->n_ev] = S; r->ev_L[r->n_ev++] = L;
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
  r->n_ev = 0;
  add_ev(r, p->payload, p->S, p->L);
  g_st.candidates++;
  LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x CANDIDATE: CRC pass S=%u add_pos=%u table=%d on %d layouts\n",
        r->rnti, p->S, p->add_pos, r->cand_table, p->n_ids);
}

/* solve_now: the candidate becomes the RNTI's solution (layouts cand_bm, evidence ev_*), p = the deciding pass. Under g_mu. */
static void solve_now(solver_rnti_t *r, const pending_t *p, int kept)
{
  r->solved = true;
  memset(r->S_by_tda, -1, sizeof(r->S_by_tda));
  memset(r->L_by_tda, -1, sizeof(r->L_by_tda));
  bool refuted = false;
  r->rep = choose_rep(r, true, &refuted);
  if (refuted) { /* DCIRV: every layout the CRC passes allow reads inconsistent NDI/RV/pid: keep searching */
    r->solved = false;
    r->has_cand = false;
    g_st.dropped++;
    LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x solve REFUSED: all candidate layouts violate HARQ/RV rules\n", r->rnti);
    return;
  }
  if (r->rep < 0) r->rep = lowest(r->cand_bm);
  r->add_pos = p->add_pos;
  r->scr = p->scr;
  r->table = r->cand_table >= 0 ? r->cand_table : (p->table_disc ? (int8_t)p->table : -1);
  g_st.solved++;
  r->solved_abs = p->abs;
  memset(r->comb_try, 0, sizeof(r->comb_try)); /* receiver choice from post-solve decodes only (pre-solve probes */
  memset(r->comb_ok, 0, sizeof(r->comb_ok));   /*  fail for every receiver and biased the choice) */
  LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x SOLVED by %u CRC16 + %u CRC24 passes, P(chance)=%.1e over %u+%u probes: layout %d "
             "(%d consistent) add_pos=%u table=%d S[tda %d]=%u scr=%d\n",
        r->rnti, r->k16, r->k24, chance_p(r), r->cred16, r->cred24, r->rep, kept, r->add_pos, r->table,
        tda_of(r->rep, p->payload), p->S, (int)r->scr);
}

/* nr_pdsch_solver_feedback: consumes the CRC result of one solver decode (identified by its token): updates statistics, candidates, and solved state.
 *   Two CRC passes with consistent layouts and S solve the RNTI (sets prior and CORESET scrambling); failed candidates are dropped after several tries.
 *   Solved RNTIs learn rate matching, table, representative and S map; 256 failed full decodes without a pass reopen the search.
 *   Logs each credited probe. Called by nr_pdsch_passive_queue.c after each decode. Takes g_mu.
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
  { /* one line per credited search probe -- which hypothesis was tried */
    if (p != NULL && credit && p->phase == 0) {
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
    if (p && !credit) refund_untested(p); /* SOLVERREFUND */
    if (p) p->used = false;
    pthread_mutex_unlock(&g_mu);
    return;
  }
  coverage_note_snr(p);
  solver_rnti_t *r = &g_r[p->slot];
  if (r->used && p->phase == 0 && !r->solved && !crc_ok && r->layout_failures < UINT32_MAX)
    r->layout_failures++;
  if (r->used && p->phase == 0 && p->probe && !r->solved) /* the Bonferroni count of the significance test */
    p->crc24 ? r->cred24++ : r->cred16++;
  if (p->comb >= 1 && (!r->solved || !p->probe)) {
    const int ci = comb_ci(p->comb);
    r->comb_try[ci]++;
    if (crc_ok) r->comb_ok[ci]++;
  }
  if (crc_ok && p->mcs > 0) r->mcs_pos = true; /* DCIRV: the MCS offset is pinned by a nonzero reading */
  if (p->probe && crc_ok) g_st.probe_ok++;
  if (!p->probe && crc_ok) g_st.full_ok++;
  if (r->used && p->phase == 1 && r->solved && p->ov) { /* SOLVERPHASE (fallback, see plan): its passes are (S, L) evidence */
    const int ph = (int)(p->abs % SOLVER_PHASES);
    if (crc_ok) { /* DCI11-RESOLVER: the representative must map this allocation through its TDA field like every other pass */
      add_ev(r, p->payload, p->S, p->L);
      const int old_rep = r->rep;
      const int nr = choose_rep(r, false, NULL);
      if (nr >= 0 && nr != old_rep) {
        r->rep = nr;
        LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x slot phase %d pass S=%u L=%u: representative layout %d -> %d (its TDA field "
                   "maps this allocation)\n", r->rnti, ph, p->S, p->L, old_rep, nr);
      }
    }
    if (p->probe) {
      if (crc_ok && r->ov_S[ph] < 0) {
        r->ov_S[ph] = (int8_t)p->S;
        r->ov_L[ph] = (int8_t)p->L;
        LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x slot phase %d learnt S=%u L=%u (its full decodes failed %u of %u)\n", r->rnti, ph,
              p->S, p->L, r->ph_tr[ph] - r->ph_ok[ph], r->ph_tr[ph]);
      }
    } else {
      r->full_tr++;
      r->ph_tr[ph]++;
      if (crc_ok) { r->full_ok++; r->ph_ok[ph]++; }
    }
  }
  if (r->used && p->phase == 1 && r->solved && !p->ov) {
    if (!p->probe) {
      r->ph_tr[p->abs % SOLVER_PHASES]++;
      if (crc_ok) r->ph_ok[p->abs % SOLVER_PHASES]++;
      r->full_tr++;
      if (crc_ok) {
        r->full_ok++;
        r->cons_fail = 0;
        if (r->full_ok == 1) try_promote(r); /* first CRC-proven full TB: now it may seed the cell */
        if (r->full_ok <= 20 || (r->full_ok % 100) == 0)
          LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x full TB CRC OK #%u of %u full decodes (S=%u table=%u layout %d)\n",
                r->rnti, r->full_ok, r->full_tr, p->S, p->table, r->rep);
      }
      else if (++r->cons_fail >= 256 && r->full_ok == 0) { /* a solution that never decodes a full TB: reopen */
        LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x solution never decoded a full TB in 256 grants: reopening\n", r->rnti);
        r->solved = false; r->has_cand = false; g_st.dropped++;
      }
    }
    if (!r->promoted && r->full_ok >= 1) try_promote(r);
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
          if (memcmp(r->cand_bm, nb, BM_BYTES) != 0) memcpy(r->cand_bm, nb, BM_BYTES);
          free(nb);
        }
      }
    }
    if (crc_ok) {
      r->cons_fail = 0;
      add_ev(r, p->payload, p->S, p->L);
      if (p->learn_S && p->probe && p->tda >= 0 && p->tda < 16 && p->L != 14 - p->S) { /* SOLVERTDDL */
        r->L_by_tda[p->tda] = (int8_t)p->L;
        LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x tda %d learnt S=%u L=%u (shorter than 14 - S: TDD special slot)\n", r->rnti, p->tda, p->S, p->L);
      }
      const int old_rep = r->rep;
      const int nr = choose_rep(r, false, NULL);
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
      if (r->has_cand && significant(r)) solve_now(r, p, p->n_ids); /* one CRC24 pass */
    } else if (p->dci != r->cand_dci) {
      uint8_t *nb = calloc(1, BM_BYTES);
      int kept = 0;
      for (int i = 0; nb && i < p->n_ids; i++)
        if ((r->cand_bm[p->ids[i] >> 3] >> (p->ids[i] & 7)) & 1) {
          nb[p->ids[i] >> 3] |= (uint8_t)(1u << (p->ids[i] & 7));
          kept++;
        }
      if (kept == 0 || p->add_pos != r->cand_add_pos || p->scr != r->cand_scr
          || (r->cand_table >= 0 && p->table_disc && p->table != r->cand_table)) {
        /* a pass the candidate cannot explain: after the candidate has had its confirmation chances, the newer
         * evidence takes its place (a chance CRC pass must not block the search) */
        if (r->cand_tries >= 16) {
          make_cand(r, p);
          if (r->has_cand && significant(r)) solve_now(r, p, p->n_ids);
        }
      } else { /* a consistent pass on another DCI: count it, solve once significant */
        memcpy(r->cand_bm, nb, BM_BYTES);
        p->crc24 ? r->k24++ : r->k16++;
        add_ev(r, p->payload, p->S, p->L);
        r->cand_dci = p->dci; /* the next pass must come from yet another DCI */
        if (significant(r)) solve_now(r, p, kept);
        else
          LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x candidate pass %u+%u not yet significant: P(chance)=%.1e over %u+%u probes\n",
                r->rnti, r->k16, r->k24, chance_p(r), r->cred16, r->cred24);
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

/* hq_reselect: a solved RNTI's layout is re-chosen from the HARQ/RV readings: when refuted, or (force) when another
 * unrefuted candidate violates less. No candidate left: the solution is dropped and the search reopens. Under g_mu. */
static void hq_reselect(solver_rnti_t *r, bool force)
{
  if (!g_hq_fn || !r->solved || r->rep < 0 || !r->cand_bm) return;
  int n, bad;
  hq_score(r, r->rep, &n, &bad);
  const bool refuted = nr_pdsch_solver_hq_refuted(n, bad);
  LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x HQSCORE layout %d: %d violations in %d grants (%s)%s\n", r->rnti, r->rep, bad, n,
        refuted ? "refuted" : "kept", force ? " [rv rescues]" : "");
  if (!refuted && !(force && bad > 0)) return;
  const int old = r->rep;
  bool all_refuted = false;
  const int nr = choose_rep(r, true, &all_refuted);
  if (nr >= 0 && nr != old) {
    r->rep = nr;
    if (r->promoted && g_have_prior && g_prior_rep == old) g_prior_rep = nr;
    LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x HARQ/RV consistency: layout %d -> %d (old: %d violations in %d grants)\n",
          r->rnti, old, nr, bad, n);
  } else if (nr < 0 && refuted && all_refuted) {
    LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x HARQ/RV consistency: layout %d refuted (%d violations in %d grants), "
               "no alternative: reopening\n", r->rnti, old, bad, n);
    if (r->promoted) g_have_prior = false;
    r->solved = r->has_cand = r->promoted = false;
    g_st.dropped++;
  }
}
void nr_pdsch_solver_note_dci(uint16_t rnti, nr_dci_bits_t payload, uint64_t abs_slot)
{
  pthread_mutex_lock(&g_mu);
  solver_rnti_t *r = slot_of(rnti, true, NULL);
  if (r != NULL && (abs_slot == 0 || abs_slot != r->hq_abs)) {
    r->hq_abs = abs_slot;
    r->hq_ring[r->hq_cnt++ % HQ_RING] = payload;
    if (r->solved && (r->hq_cnt % 16) == 0) {
      hq_reselect(r, false);
      if (r->solved && !r->promoted && r->full_ok >= 1) try_promote(r);
    }
  }
  pthread_mutex_unlock(&g_mu);
}
void nr_pdsch_solver_note_rv_mismatch(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  solver_rnti_t *r = slot_of(rnti, false, NULL);
  if (r != NULL && ++r->rvmis >= 3) {
    r->rvmis = 0;
    hq_reselect(r, true);
  }
  pthread_mutex_unlock(&g_mu);
}

/* The per-RNTI layout cache is off (its caller only asks nr_rnti_layout_cache_on()); the entry points remain for that caller. */
bool nr_rnti_layout_cache_on(void) { return false; }
int nr_pdsch_solver_layout_cache(uint16_t rnti, uint64_t key, int max_fail, uint16_t *ids, int max)
{
  (void)rnti; (void)key; (void)max_fail; (void)ids; (void)max;
  return -1;
}
void nr_pdsch_solver_layout_cache_stats(nr_pdsch_solver_lc_stats_t *s) { memset(s, 0, sizeof(*s)); }

/* nr_pdsch_solver_stats: copies the solver's global counters (planned, probes, fulls, CRC passes, candidates, solved, dropped) into *s.
 *   Takes g_mu. No callers outside the header yet.
 */
void nr_pdsch_solver_stats(nr_pdsch_solver_stats_t *s)
{
  pthread_mutex_lock(&g_mu);
  *s = g_st;
  pthread_mutex_unlock(&g_mu);
}

/* ---- Fix A: DCI 1_1 TDRA field (see nr_pdsch_solver.h) ---- */
#define TF_NPOS 4
#define TF_MIN 8 /* decisions need >= TF_MIN DCIs per class / OK decodes per value; purity >= 90 % */
typedef struct { uint8_t S, L; uint32_t n; } tf_row_t;
typedef struct {
  int pos; /* -1 free */
  uint32_t cls[2][4][2];      /* [w-1][value][mixed] accepted DCIs */
  tf_row_t row[2][4][2][3];   /* [w-1][value][mixed] (S,L) of CRC-passing decodes */
  uint8_t det_w;              /* 0 undetected, else the accepted width */
} tf_pos_t;
static pthread_mutex_t g_tf_mu = PTHREAD_MUTEX_INITIALIZER;
static tf_pos_t g_tf[TF_NPOS];
static uint8_t g_tf_S[16], g_tf_L[16];
static int g_tf_n, g_tf_init;

bool nr_tda_field_on(void)
{
  return true;
}
static void tf_init_locked(void)
{
  if (g_tf_init) return;
  g_tf_init = 1;
  for (int i = 0; i < TF_NPOS; i++) g_tf[i].pos = -1;
}
void nr_tda_field_reset(void)
{
  pthread_mutex_lock(&g_tf_mu);
  memset(g_tf, 0, sizeof(g_tf));
  g_tf_init = 0; g_tf_n = 0;
  tf_init_locked();
  pthread_mutex_unlock(&g_tf_mu);
}
void nr_tda_field_set_rows(const uint8_t *S, const uint8_t *L, int n)
{
  if (!S || !L || n < 0) return;
  if (n > 16) n = 16;
  pthread_mutex_lock(&g_tf_mu);
  memcpy(g_tf_S, S, (size_t)n); memcpy(g_tf_L, L, (size_t)n);
  g_tf_n = n;
  pthread_mutex_unlock(&g_tf_mu);
}
static int tf_list_w(void) { int w = 0; while ((1 << w) < g_tf_n) w++; return w; }
static tf_pos_t *tf_get(int pos, bool create)
{
  tf_init_locked();
  int freei = -1;
  for (int i = 0; i < TF_NPOS; i++) {
    if (g_tf[i].pos == pos) return &g_tf[i];
    if (g_tf[i].pos < 0 && freei < 0) freei = i;
  }
  if (!create || freei < 0) return NULL;
  memset(&g_tf[freei], 0, sizeof(g_tf[freei]));
  g_tf[freei].pos = pos;
  return &g_tf[freei];
}
/* dominant (S,L) of one (w, value, class) cell: returns its count, *tot the cell's total */
static uint32_t tf_dom(const tf_pos_t *t, int wi, int v, int m, tf_row_t *best, uint32_t *tot)
{
  uint32_t bn = 0, sum = 0;
  for (int k = 0; k < 3; k++) {
    sum += t->row[wi][v][m][k].n;
    if (t->row[wi][v][m][k].n > bn) { bn = t->row[wi][v][m][k].n; *best = t->row[wi][v][m][k]; }
  }
  *tot = sum;
  return bn;
}
/* dominant (S,L) of one value over both slot classes (CRC detection): count, total */
static uint32_t tf_dom_v(const tf_pos_t *t, int wi, int v, tf_row_t *best, uint32_t *tot)
{
  tf_row_t a[6]; int na = 0; uint32_t sum = 0, bn = 0;
  for (int m = 0; m < 2; m++)
    for (int k = 0; k < 3; k++) {
      const tf_row_t *r = &t->row[wi][v][m][k];
      if (!r->n) continue;
      sum += r->n;
      int q = 0;
      while (q < na && !(a[q].S == r->S && a[q].L == r->L)) q++;
      if (q == na) { a[na] = *r; a[na].n = 0; na++; }
      a[q].n += r->n;
    }
  for (int q = 0; q < na; q++) if (a[q].n > bn) { bn = a[q].n; *best = a[q]; }
  *tot = sum;
  return bn;
}
static void tf_eval(tf_pos_t *t)
{
  if (t->det_w) return;
  const int wl = tf_list_w();
  for (int w = 1; w <= 2 && !t->det_w; w++) {
    if (g_tf_n > 0 && w != wl) continue;
    const int wi = w - 1, nv = 1 << w;
    if (g_tf_n > 0) { /* (a) value depends on the TDD slot class */
      uint32_t tm = 0, to = 0, bm = 0, bo = 0; int dm = 0, dob = 0;
      for (int v = 0; v < nv; v++) {
        tm += t->cls[wi][v][1]; to += t->cls[wi][v][0];
        if (t->cls[wi][v][1] > bm) { bm = t->cls[wi][v][1]; dm = v; }
        if (t->cls[wi][v][0] > bo) { bo = t->cls[wi][v][0]; dob = v; }
      }
      if (tm >= TF_MIN && to >= TF_MIN && bm * 10 >= tm * 9 && bo * 10 >= to * 9 && dm != dob) t->det_w = (uint8_t)w;
    }
    if (!t->det_w) { /* (b) CRC-passing decodes of different values need different (S,L) */
      tf_row_t r[4]; int q = 0; bool diff = false;
      for (int v = 0; v < nv; v++) {
        uint32_t tot, bn = tf_dom_v(t, wi, v, &r[q], &tot);
        if (tot >= TF_MIN && bn * 10 >= tot * 9) {
          for (int k = 0; k < q; k++) diff |= r[k].S != r[q].S || r[k].L != r[q].L;
          q++;
        }
      }
      if (q >= 2 && diff) t->det_w = (uint8_t)w;
    }
    if (t->det_w)
      LOG_A(PHY, "SENSING: TDAFIELD detected pos=%d w=%d list=%d (DCIs mixed/other per value %u/%u %u/%u)\n", t->pos, w, g_tf_n,
            t->cls[wi][0][1], t->cls[wi][0][0], t->cls[wi][1][1], t->cls[wi][1][0]);
  }
}
void nr_tda_field_note_dci(int pos, nr_dci_bits_t p, int len, bool mixed)
{
  if (pos < 0 || len <= pos) return;
  pthread_mutex_lock(&g_tf_mu);
  tf_pos_t *t = tf_get(pos, true);
  if (t) {
    for (int w = 1; w <= 2; w++)
      if (pos + w <= len) t->cls[w - 1][nr_dci_bits_field(&p, len, pos, w)][mixed]++;
    tf_eval(t);
  }
  pthread_mutex_unlock(&g_tf_mu);
}
void nr_tda_field_note_ok(int pos, nr_dci_bits_t p, int len, bool mixed, int S, int L)
{
  if (pos < 0 || len <= pos) return;
  pthread_mutex_lock(&g_tf_mu);
  tf_pos_t *t = tf_get(pos, true);
  for (int w = 1; t && w <= 2; w++) {
    if (pos + w > len) continue;
    tf_row_t *c = t->row[w - 1][nr_dci_bits_field(&p, len, pos, w)][mixed];
    int k = 0;
    while (k < 3 && !(c[k].n && c[k].S == S && c[k].L == L)) k++;
    if (k == 3) { /* new row: take a free slot, else replace the weakest */
      k = 0;
      for (int j = 1; j < 3; j++) if (c[j].n < c[k].n) k = j;
      c[k].S = (uint8_t)S; c[k].L = (uint8_t)L; c[k].n = 0;
    }
    c[k].n++;
  }
  if (t) tf_eval(t);
  pthread_mutex_unlock(&g_tf_mu);
}
int nr_tda_field_rows(int pos, nr_dci_bits_t p, int len, int last, int max, uint8_t *S, uint8_t *L, bool *detected)
{
  int n = 0;
  if (detected) *detected = false;
  if (pos < 0 || len <= pos || max <= 0 || !S || !L) return 0;
  const bool mixed = last < 13;
  pthread_mutex_lock(&g_tf_mu);
  tf_pos_t *t = tf_get(pos, false);
  const int w = t ? t->det_w : 0;
  if (detected) *detected = w > 0;
  if (g_tf_n > 0) {
    if (!mixed && !w) goto out; /* an unconfirmed field in a full slot: keep the solver's reading */
    const uint32_t v = w ? (uint32_t)nr_dci_bits_field(&p, len, pos, w) : 0xFFu;
    if (v < (uint32_t)g_tf_n && g_tf_S[v] + g_tf_L[v] - 1 <= last) { S[n] = g_tf_S[v]; L[n] = g_tf_L[v]; n++; }
    for (int r = 0; r < g_tf_n && n < max && !w; r++) { /* a detected field names the row: the other legal rows are tried only without it (<= 2 decodes) */
      if (g_tf_S[r] + g_tf_L[r] - 1 > last) continue;
      S[n] = g_tf_S[r]; L[n] = g_tf_L[r]; n++;
    }
  } else if (w) { /* no list (NSA): the (S,L) learnt for (value, slot class), CRC-arbitrated */
    const int v = (int)nr_dci_bits_field(&p, len, pos, w);
    tf_row_t b = {0}; uint32_t tot;
    const uint32_t bn = tf_dom(t, w - 1, v, mixed, &b, &tot);
    if (bn >= 3 && bn * 10 >= tot * 8 && b.S + b.L - 1 <= last) { S[0] = b.S; L[0] = b.L; n = 1; }
  }
out:
  pthread_mutex_unlock(&g_tf_mu);
  return n;
}

/* ---- SOLVERGATE: probe admission as a reservation (see nr_pdsch_solver.h) ---- */
void nr_probe_gate_init(nr_probe_gate_t *g, double rate, double burst)
{
  memset(g, 0, sizeof(*g));
  g->rate = rate;
  g->burst = burst;
  g->tokens = burst;
  g->last_abs = -1;
}

static void probe_gate_refill(nr_probe_gate_t *g, long abs_slot)
{
  if (g->last_abs >= 0 && abs_slot > g->last_abs) g->tokens += g->rate * (double)(abs_slot - g->last_abs);
  if (abs_slot > g->last_abs) g->last_abs = abs_slot;
  if (g->tokens > g->burst) g->tokens = g->burst;
}

int nr_probe_gate_reserve(nr_probe_gate_t *g, long abs_slot, int headroom, int max, int *resv)
{
  probe_gate_refill(g, abs_slot);
  int n = (int)g->tokens;
  if (headroom >= 0) {
    const int room = headroom - g->resv_jobs;
    if (room < n) n = room;
  }
  if (n > max) n = max;
  if (n <= 0) return 0;
  g->tokens -= n;
  g->resv_jobs += n;
  *resv += n;
  return n;
}

bool nr_probe_gate_take(nr_probe_gate_t *g, long abs_slot, bool ring_ok, int *resv)
{
  if (*resv > 0) {
    (*resv)--;
    if (g->resv_jobs > 0) g->resv_jobs--;
    return true;
  }
  probe_gate_refill(g, abs_slot);
  if (g->tokens >= 1.0 && ring_ok) {
    g->tokens -= 1.0;
    return true;
  }
  return false;
}

void nr_probe_gate_release(nr_probe_gate_t *g, int *resv)
{
  if (*resv <= 0) return;
  g->tokens += *resv;
  if (g->tokens > g->burst) g->tokens = g->burst;
  g->resv_jobs = g->resv_jobs > *resv ? g->resv_jobs - *resv : 0;
  *resv = 0;
}
