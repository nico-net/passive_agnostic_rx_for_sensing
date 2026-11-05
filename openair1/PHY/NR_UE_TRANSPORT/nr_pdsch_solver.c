/* Blind dedicated-PDSCH grant solver (T26b). See nr_pdsch_solver.h. */
#include "common/utils/nr_cell.h" /* mpci A5: per-cell state (NR_CELL_STATIC) */
#include "nr_pdsch_cell_prealloc.h"
#include "nr_blind_stats.h"
#include "nr_pdsch_solver.h"
#include "nr_signal_evidence.h"
#include "nr_snr_gate.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "common/utils/LOG/log.h"

#define SOLVER_RNTIS NR_PDSCH_SOLVER_RNTIS
#define SOLVER_PENDING 4096 /* FIFO-overwritten: probes dropped stale in the queue never report back */
#define SOLVER_MAX_IDS 2048     /* layouts of one reading kept with a pending probe (the candidate set a pass creates) */
#define COVERAGE_IDS 64         /* layouts of a reading whose trial counts stand for it in the coverage search (cost bound: the
                                 * counts are kept per layout id, and a reading of the full standard space can hold thousands) */
#define SOLVER_PHASES 160         /* slot-phase resolution: periods of 20, 40, 80 and 160 slots (10..80 ms at 30 kHz) all divide it */
#define SOLVER_PER0 20            /* phase period until the cell's passes show a longer one: one 10 ms frame at 30 kHz */
#define SOLVER_PHASE_FAIL 8       /* full decodes without one pass at a phase (the RNTI decodes elsewhere) before its (S, L) is searched */
#define SOLVER_CAND_TRIES 96      /* confirmation probes before an unconfirmed candidate is dropped */
#define BM_BYTES (NR_PDSCH_SOLVER_MAX_LAYOUTS / 8)
#define HQ_RING 64
#define PHYS_K 32                 /* E3: hypotheses tracked per RNTI by the sign test */

typedef struct { uint64_t key; uint32_t count; } coverage_entry_t;
/* internal diagnostic reasons, after the caller-side NR_PDSCH_SOLVER_WHY_* ones */
enum { W_PLAN = NR_PDSCH_SOLVER_WHY_CALLER_N, W_CSIRS, W_BACKOFF, W_WIDE, W_PROBES, W_POK, W_PFAIL,
       W_PNOCRED, W_NOREP, W_PARKED, W_N };
static const char *const kWhy[W_N] = {"gate", "live", "budget0", "noread", "throttle", "cap", "weak", "g_rv", "g_rvok", "g_cap", "plan", "csirs",
                                      "backoff", "norb", "probes", "p_ok", "p_fail", "p_nocred", "norep", "parked"};
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
  /* SOLVERPHASE: (S, L) of a solved RNTI per slot phase (abs slot mod the cell phase period g_per) where its full decodes fail while it decodes
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
  uint32_t why[W_N];
  nse_rnti_t nse;          /* PARK: evidence that any hypothesis sees this RNTI's DM-RS above the cell's noise floor (nr_signal_evidence.h) */
  int8_t id_q;             /* IDQUAL: best class of this RNTI's DM-RS identities (nr_dmrs_scan_rnti_id_quality): 2 proven, 1 untested, 0 wrong, -1 none */       /* SOLVERCOVER diagnostics (see NR_PDSCH_SOLVER_WHY_*) */
  uint64_t last_abs;       /* SOLVERCOVER: signal time of the last grant (activity, eviction) */
  double fcredit;          /* SOLVERCOVER fair share: this RNTI's share of the probe rate, in probes */
  uint64_t fc_abs;         /*   time of the last accrual (0 = never: cold start gets a full budget) */
  uint32_t cred16, cred24; /* TIMSOLVE2: credited search probes while unsolved, by CRC length */
  uint8_t k16, k24;        /*   consistent CRC passes of the current candidate, by CRC length */
  nr_dci_bits_t hq_ring[HQ_RING]; /* DCIRV: this RNTI's last DCI 1_1 payloads (oldest overwritten) */
  uint32_t hq_cnt, rvmis;  /*   payloads seen; RV-rescue notes since the last re-selection */
  uint64_t hq_abs;         /*   slot of the last payload (duplicate candidates of one DCI) */
  uint32_t hq_bad_prev;    /*   violations seen at the last re-selection (the alert fires when the count rises) */
  bool hrv_alert;          /*   traffic mode: a HARQ/RV violation was counted (nr_pdsch_solver_take_hrv_alert) */
  bool mcs_pos, promoted;  /*   a CRC pass at MCS > 0 happened; the solution has been allowed to seed the cell */
  /* E3 (traffic mode): the solution came from DM-RS physics (phys) or a family (fam); the sign test between hypotheses; the score ring; the Qm ledger */
  bool phys, fam;
  uint16_t dlen;           /*   DCI length of the last planned grant (families are per length) */
  uint8_t tick;
  uint8_t ring_n;          /*   scorable looks kept in ring (<= 16), bit set = miss */
  uint16_t ring;
  uint8_t px_n;
  uint64_t px_last_abs;
  struct { int32_t lid, scr; uint8_t add_pos; uint16_t det; } px_c[PHYS_K];
  uint16_t px_a[PHYS_K][PHYS_K]; /* px_a[i][j]: grants where i was significant and outranked j */
  uint64_t qm_last;
  uint16_t qm_agree[3], qm_dis[3];
  bool qm_poison;
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
  nr_dci_bits_t payload;
  int n_ids;
  uint16_t ids[SOLVER_MAX_IDS];
} pending_t;

NR_CELL_STATIC_INIT(pthread_mutex_t, g_mu, PTHREAD_MUTEX_INITIALIZER);
#define g_mu NR_CELL_REF(g_mu)
typedef solver_rnti_t g_r_arr_t[SOLVER_RNTIS];
NR_CELL_STATIC_HEAP(g_r_arr_t, g_r);
#define g_r NR_CELL_HEAP_REF(g_r_arr_t, g_r)
typedef pending_t g_p_arr_t[SOLVER_PENDING];
NR_CELL_STATIC_HEAP(g_p_arr_t, g_p);
#define g_p NR_CELL_HEAP_REF(g_p_arr_t, g_p)
NR_CELL_STATIC_INIT(uint32_t, g_token, 1);
#define g_token NR_CELL_REF(g_token) /* sequence part of a token; the low 12 bits are the pending index (O(1) feedback) */
typedef struct {
  int32_t ids[16], key;
  int n;
  uint32_t hash;
  uint64_t now_abs;
  bool decided;
  bool own; /* TIM-PDSCH: ids are this RNTI's own (nr_pdsch_solver_set_alt_scr32) */
  bool seeded_only; /* E4: plan only the seeded variants (candidate confirmation, cell prior / solved tuples), no coverage search */
  int8_t last_dl; /* SOLVERTDDEND: last DL symbol of the grant's slot from the broadcast TDD pattern (-1 = unknown) */
  int32_t pool[NR_PDSCH_SOLVER_POOL_MAX]; /* A3: DM-RS ids already seen in the cell (nr_dmrs_scan pool), most frequent first; this grant's RNTI has none of its own */
  int np;
} solver_plan_ctx_t;
static _Thread_local solver_plan_ctx_t t_plan_ctx = {.key = -1, .last_dl = -1}; /* per scan consumer */
/* plan_ctx: the plan context (per scan consumer thread) that the grant planner reads its key, hash and slot time from. */
static solver_plan_ctx_t *plan_ctx(void) { return &t_plan_ctx; }
typedef struct { int32_t key, scr; } key_scr_t;
typedef key_scr_t g_key_scr_arr_t[16];
NR_CELL_STATIC(g_key_scr_arr_t, g_key_scr);
#define g_key_scr NR_CELL_REF(g_key_scr) /* first solved scrambling per CORESET: its other RNTIs try it only */
NR_CELL_STATIC(int, g_n_key_scr);
#define g_n_key_scr NR_CELL_REF(g_n_key_scr)
NR_CELL_STATIC(uint64_t, g_dci_seq);
#define g_dci_seq NR_CELL_REF(g_dci_seq)
NR_CELL_STATIC(nr_pdsch_solver_tda_fn, g_tda_fn);
#define g_tda_fn NR_CELL_REF(g_tda_fn)
NR_CELL_STATIC(void *, g_tda_ctx);
#define g_tda_ctx NR_CELL_REF(g_tda_ctx)
NR_CELL_STATIC(nr_pdsch_solver_stats_t, g_st);
#define NSE_SOLVE_RING 32
typedef nse_ref_t g_nse_ref_arr_t[1];
NR_CELL_STATIC(g_nse_ref_arr_t, g_nse_ref); /* PARK: the cell's noise-floor reference and H1 shape (under g_mu) */
typedef nse_gate_t g_nse_gate_arr_t[2];
NR_CELL_STATIC(g_nse_gate_arr_t, g_nse_gate); /* no-chance margin statistics, [0] DL, [1] UL (under g_mu) */
typedef _Atomic uint32_t g_nse_gm_arr_t[2];
NR_CELL_STATIC(g_nse_gm_arr_t, g_nse_gm); /* published margin, 1/16 dB + 4096 (0 = design margin) */
typedef uint32_t g_nse_solve_arr_t[NSE_SOLVE_RING];
NR_CELL_STATIC(g_nse_solve_arr_t, g_nse_solve); /* probes spent by the last solved RNTIs before their solve (the cell's own search depth) */
NR_CELL_STATIC(uint32_t, g_nse_solve_n);
NR_CELL_STATIC(uint32_t, g_nse_logs);
#define g_st NR_CELL_REF(g_st)
#define g_nse_ref NR_CELL_REF(g_nse_ref)
#define g_nse_gate NR_CELL_REF(g_nse_gate)
#define g_nse_gm NR_CELL_REF(g_nse_gm)
#define g_nse_solve NR_CELL_REF(g_nse_solve)
#define g_nse_solve_n NR_CELL_REF(g_nse_solve_n)
#define g_nse_logs NR_CELL_REF(g_nse_logs)
#define LAYOUT_RECOVER 32 /* failed CRC probes after which a pruned layout set is reopened once */
/* FIRSTSLOT: CRC passes of the cell by slot phase (abs slot mod the cell period g_per, census at SOLVER_PHASES) and start symbol S (full-length allocations): the TDD pattern fixes
 * which S a slot phase carries, whatever the RNTI, so a new RNTI's seeded probe takes the S the phase's passes show (under g_mu). */
typedef uint32_t g_phs_arr_t[SOLVER_PHASES][4];
NR_CELL_STATIC(g_phs_arr_t, g_phs);
#define g_phs NR_CELL_REF(g_phs)
/* D10: the phase period of the cell (slots), learnt from those passes: the smallest of 20/40/80/160 under which every slot phase shows one S. A TDD
 * pattern of 20 ms (two patterns) or a CSI-RS period of 40 slots makes the phases differ at 40 (OTA 923); a constant 20 folds them together. */
NR_CELL_STATIC(int, g_per);
#define g_per NR_CELL_REF(g_per)
static inline int per_now(void) { return g_per ? g_per : SOLVER_PER0; }
static inline int ph_of(uint64_t abs) { return (int)(abs % (uint64_t)per_now()); }
NR_CELL_STATIC(nr_pdsch_solver_hq_fn, g_hq_fn);
#define g_hq_fn NR_CELL_REF(g_hq_fn) /* DCIRV */
NR_CELL_STATIC(void *, g_hq_ctx);
#define g_hq_ctx NR_CELL_REF(g_hq_ctx)
/* cell prior: the first solution, offered first to other RNTIs */
NR_CELL_STATIC(bool, g_have_prior);
#define g_have_prior NR_CELL_REF(g_have_prior)
NR_CELL_STATIC(int, g_prior_rep);
#define g_prior_rep NR_CELL_REF(g_prior_rep)
NR_CELL_STATIC(uint8_t, g_prior_add_pos);
#define g_prior_add_pos NR_CELL_REF(g_prior_add_pos)
NR_CELL_STATIC(int8_t, g_prior_table);
#define g_prior_table NR_CELL_REF(g_prior_table)
typedef int8_t g_prior_S_by_tda_arr_t[16];
NR_CELL_STATIC(g_prior_S_by_tda_arr_t, g_prior_S_by_tda);
#define g_prior_S_by_tda NR_CELL_REF(g_prior_S_by_tda)
typedef int8_t g_prior_L_by_tda_arr_t[16];
NR_CELL_STATIC(g_prior_L_by_tda_arr_t, g_prior_L_by_tda);
#define g_prior_L_by_tda NR_CELL_REF(g_prior_L_by_tda)
typedef uint32_t g_lcur_arr_t[SOLVER_RNTIS][16];
NR_CELL_STATIC(g_lcur_arr_t, g_lcur);
#define g_lcur NR_CELL_REF(g_lcur) /* SOLVERTDDL: rotation cursor of the length search per RNTI and TDA index */
NR_CELL_STATIC(bool, t_ov);
#define t_ov NR_CELL_REF(t_ov) /* SOLVERPHASE: variants being emitted are phase-override ones */
NR_CELL_STATIC(int, t_L);
#define t_L NR_CELL_REF(t_L) /* length of the variants being emitted (0 = 14 - S); set by plan_known under g_mu */

/* nr_pdsch_solver_phase_period: the learnt slot-phase period of the cell (see g_per). Callers: tests, the replay diagnostic. */
int nr_pdsch_solver_phase_period(void)
{
  pthread_mutex_lock(&g_mu);
  const int P = per_now();
  pthread_mutex_unlock(&g_mu);
  return P;
}

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

/* nr_pdsch_solver_reset: Frees per-RNTI candidate bitmaps and coverage tables under g_mu. Callers: tests/nr_pdsch_solver_test.cc and the
 * monitor's epoch listener on a new cell identity (HARD_RESET: nothing solved for the old cell holds). */
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
  memset(g_nse_ref, 0, sizeof(g_nse_ref));
  memset(g_nse_gate, 0, sizeof(g_nse_gate));
  memset(g_phs, 0, sizeof(g_phs));
  g_per = SOLVER_PER0;
  memset(g_nse_solve, 0, sizeof(g_nse_solve));
  g_nse_solve_n = 0;
  atomic_store(&g_nse_gm[0], 0);
  atomic_store(&g_nse_gm[1], 0);
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
      g_r[i].id_q = -1; /* IDQUAL: no identity known yet (0 would read "all wrong") */
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
/* N2: "verified" against the 0.5 null accepts violation rates up to ~30 % at n = 64. A layout whose fields may be PRINTED, or that may win a tie, must also have the one-sided upper
 * confidence bound of its violation rate (Wilson score interval, alpha = NBS_SIGNIF_ALPHA, z = 3.09) below 0.2. */
#define HQ_TRUST_MAX_RATE 0.2
bool nr_pdsch_solver_hq_trusted(int n, int bad)
{
  if (!nr_pdsch_solver_hq_verified(n, bad)) return false;
  const double z = 3.0902323, p = (double)bad / (double)n, z2 = z * z, nn = (double)n;
  const double up = (p + z2 / (2 * nn) + z * sqrt(p * (1 - p) / nn + z2 / (4 * nn * nn))) / (1 + z2 / nn);
  return up < HQ_TRUST_MAX_RATE;
}
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
  return nr_pdsch_solver_hq_trusted(n, bad);   /* N8: a 20-30 % layout must not seed the cell */
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
#define SIGNIF_ALPHA NBS_SIGNIF_ALPHA
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
void nr_pdsch_solver_note_id_quality(uint16_t rnti, int q)
{
  pthread_mutex_lock(&g_mu);
  solver_rnti_t *r = slot_of(rnti, false, NULL);
  if (r) r->id_q = (int8_t)q;
  pthread_mutex_unlock(&g_mu);
}
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

/* cover_ok: the search has spent at least as many probes on this RNTI as the cell's own solved RNTIs needed (p90 of the last 32 solves) -- a
 * miss then means "no DM-RS", not "hypothesis not tried yet"; with fewer than 5 solves in the cell the former back-off depth (200) stands.
 * g_mu held. */
static bool nse_cover_ok(const solver_rnti_t *r)
{
  uint32_t n = g_nse_solve_n < NSE_SOLVE_RING ? g_nse_solve_n : NSE_SOLVE_RING, depth = 200;
  if (n >= 5) {
    uint32_t v[NSE_SOLVE_RING];
    memcpy(v, g_nse_solve, n * sizeof(v[0]));
    for (uint32_t i = 1; i < n; i++)
      for (uint32_t j = i; j > 0 && v[j] < v[j - 1]; j--) { const uint32_t t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    depth = v[(uint32_t)(0.9 * (n - 1) + 0.5)];
  }
  return r->n_probe >= depth;
}

/* nr_pdsch_solver_note_dmrs: one decode (probe or full) of an RNTI's grant with its DM-RS comb SNR (EVMGRANT dmrs_snr_comb). Feeds the cell's
 *   noise-floor reference and the RNTI's park test (nr_signal_evidence.h); logs "SENSING: PARK" on a park / un-park (the first 32 of a cell, then
 *   every 16th). gate_obs: an ungated real first-transmission decode of a dedicated grant: its (margin to the MCS requirement, CRC) feeds the
 *   cell's no-chance margin. Caller: nr_pdsch_passive_queue.c after each decode. Takes g_mu. */
void nr_pdsch_solver_note_dmrs(uint16_t rnti, uint64_t abs_slot, uint32_t hyp, float comb_db, bool crc_ok, bool probe, int mcs, int tbl, bool gate_obs)
{
  if (!(comb_db > -90.f) || !isfinite(comb_db)) return;
  pthread_mutex_lock(&g_mu);
  solver_rnti_t *r = slot_of(rnti, false, NULL);
  if (r) {
    nse_ref_t *R = &g_nse_ref[0];
    const nse_event_t ev = nse_note(&r->nse, R, abs_slot, hyp, comb_db, crc_ok && !probe && r->solved, r->solved, nse_cover_ok(r));
    if (ev != NSE_NONE && (g_nse_logs++ < 32 || (g_nse_logs % 16) == 0))
      LOG_A(PHY, "SENSING: %s rnti=0x%04x evidence: all %d hypothesis classes tested as noise (SPRT, beta=%.2f) after %u decodes (parked %u x), probes=%u solved=%d "
                 "noise_mode=%.1f dB pool=%llu H1_shape=%.2f retest 1 in %u grants\n", ev == NSE_PARKED ? "PARK" : "UNPARK", r->rnti,
            __builtin_popcount(r->nse.seen), NSE_BETA, r->nse.n_obs, r->nse.n_park,
            r->n_probe, (int)r->solved, NSE_LO_DB + NSE_BW_DB * (R->mu_bin + 0.5f), (unsigned long long)R->n, nse_h1_shape(R),
            1u << (r->nse.pk_neg < NSE_THIN_MAX_SHIFT ? r->nse.pk_neg : NSE_THIN_MAX_SHIFT));
    if (gate_obs && !probe) {
      const double rq = nr_required_snr_db(mcs, tbl);
      if (isfinite(rq)) {
        nse_gate_t *G = &g_nse_gate[0];
        nse_gate_note(G, (double)comb_db - rq, crc_ok);
        uint32_t tot = 0;
        for (int b = 0; b < NSE_G_BINS; b++) tot += G->tries[b];
        if ((tot & 255u) == 0) {
          const double M = nse_gate_margin(G, NR_SNR_GATE_MARGIN_DB);
          const uint32_t q = (uint32_t)(lround(M * 16.0) + 4096);
          if (atomic_exchange(&g_nse_gm[0], q) != q)
            LOG_A(PHY, "SENSING: SNRGATE DL margin %.1f dB below the requirement (design %.1f), measured on %u ungated tries, Wilson upper bound of the pass rate below it <= %.3f\n",
                  M, NR_SNR_GATE_MARGIN_DB, tot, NSE_G_EPS);
        }
      }
    }
  }
  pthread_mutex_unlock(&g_mu);
}

/* nr_snr_gate_margin_db_cell: the cell-measured no-chance margin of nr_snr_gate.h (ul: 1 = PUSCH, never pooled with DL); the design margin
 *   until the cell's own data allow a tighter one. Lock-free. Caller: nr_snr_gate_margin_db(). */
double nr_snr_gate_margin_db_cell(int ul)
{
  const uint32_t q = atomic_load_explicit(&g_nse_gm[ul ? 1 : 0], memory_order_relaxed);
  return q ? ((double)q - 4096.0) / 16.0 : NR_SNR_GATE_MARGIN_DB;
}

/* nr_pdsch_solver_parked: whether the RNTI is currently parked (tests / diagnostics). */
bool nr_pdsch_solver_parked(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  const bool p = r && r->nse.parked;
  pthread_mutex_unlock(&g_mu);
  return p;
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
  ctx->np = 0;
}

/* nr_pdsch_solver_set_pool_ids: A3. Publishes the DM-RS identities the cell has shown (cell-wide pool of the blind scan) as first
 *   identities for the new RNTI of the grant about to be planned. Per consumer thread, cleared by set_alt_scr*(); call after them. */
void nr_pdsch_solver_set_pool_ids(const int32_t *ids, int n)
{
  solver_plan_ctx_t *ctx = plan_ctx();
  ctx->np = 0;
  for (int i = 0; ids && i < n && ctx->np < NR_PDSCH_SOLVER_POOL_MAX; i++) ctx->pool[ctx->np++] = ids[i];
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
  ctx->np = 0;
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

/* nr_pdsch_solver_set_seeded_only: E4 traffic triage of a first DCI. While set (per scan consumer thread; the caller sets it for every grant), an
 *   unsolved RNTI is planned with steps 1-2 only (candidate confirmation, the cell's seeded tuples / prior); the coverage search (step 3) is skipped.
 *   A cell without a seed or prior then plans nothing for it. */
void nr_pdsch_solver_set_seeded_only(bool on)
{
  plan_ctx()->seeded_only = on;
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
  NR_CELL_STATIC(uint32_t, cur);
#define cur NR_CELL_REF(cur)
  pending_t *p = &g_p[cur]; /* FIFO: the oldest entry is the one most likely lost in the queue */
  if (p->used) {
    g_st.dropped++;
    refund_untested(p); /* SOLVERREFUND: its feedback can no longer arrive; the probe counts as untested */
  }
  cur = (cur + 1) % SOLVER_PENDING;
  return p;
}
#undef cur

NR_CELL_STATIC(int, t_rm);
#define t_rm NR_CELL_REF(t_rm)
NR_CELL_STATIC(bool, t_learn_rm);
#define t_learn_rm NR_CELL_REF(t_learn_rm) /* set by plan for this grant (under g_mu) */
NR_CELL_STATIC_INIT(int32_t, t_scr, -1);
#define t_scr NR_CELL_REF(t_scr) /* T26c scrambling hypothesis for this grant (-1 default) */
NR_CELL_STATIC(uint8_t, t_comb);
#define t_comb NR_CELL_REF(t_comb) /* T26d combining mode for this grant's variants (0 = default) */
static int comb_ci(uint8_t comb) { return comb == 3 ? 1 : comb == 4 ? 2 : comb >= 5 ? 3 : 0; }
/* comb_pick: the receiver id for one grant of an RNTI: always the receiver default (0 = live-branch MRC for rank 1). A per-RNTI CRC race
 * against the best-branch receiver counted multi-layer grants (where the mode does nothing) and spent 1 grant in 8 on a receiver ~6 dB worse
 * on healthy branches (OTA 2026-10-30). comb_try/comb_ok keep counting per receiver for the PDSCH_SOLVER why line. Caller: nr_pdsch_solver_plan_slot(). */
static uint8_t comb_pick(const solver_rnti_t *r)
{
  (void)r;
  return 0;
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
  /* IDQUAL: the probe count alone cut off the ~45 % of RNTIs that need more than ~200 probes (PCI 923 OTA 2026-10-30: solved RNTIs needed
   * a median 143-220, p90 540-874) and kept spending on RNTIs whose every identity shows noise. An identity whose decodes see the RNTI's
   * DM-RS is never backed off; when every identity is proven wrong the search backs off after 64 probes (the scan may still bring a new one). */
  if (r->has_cand || r->id_q == 2) return false;
  return r->n_probe >= (r->id_q == 0 ? 64u : 200u) && (r->grants % 8u) != 0;
}

static const uint8_t kS[4] = {1, 0, 2, 3};

/* A2 SEEDTUPLES. The cell's CRC-proven solutions grouped by layout tuple (representative layout, add_pos, MCS table, S per TDA index,
 * identity form), strongest first: ranked by the NUMBER of promoted solved RNTIs sharing the tuple, ties by their summed full TB passes.
 * Replaces the single "best" solution as the seed of a new RNTI: a cell with two RRC templates (PCI 415) has two tuples, both are tried
 * in proportion to the RNTIs that use them. Two solutions are the same tuple when they agree wherever both know the field (-1 unknown).
 * Under g_mu. Callers: nr_pdsch_solver_plan_slot(). */
#define SEED_MAX 3 /* the group size of the former ctx->own source list (src[3]) */
typedef struct { const solver_rnti_t *src; uint32_t n, ok; } seed_t;
static bool seed_same(const solver_rnti_t *a, const solver_rnti_t *b)
{
  if (a->rep != b->rep || a->add_pos != b->add_pos || scr_form(a->scr) != scr_form(b->scr)) return false;
  if (a->table >= 0 && b->table >= 0 && a->table != b->table) return false;
  for (int i = 0; i < 16; i++)
    if (a->S_by_tda[i] >= 0 && b->S_by_tda[i] >= 0 && a->S_by_tda[i] != b->S_by_tda[i]) return false;
  return true;
}
static bool seed_eligible(const solver_rnti_t *q) { return q->used && q->solved && q->rep >= 0 && q->full_ok >= 1 && q->promoted; }
/* E3: len != 0 keeps only the RNTIs whose DCIs have that length (a layout family is per cell and DCI length); `max` tuples are returned. len = 0, max = SEED_MAX: the legacy call. */
static int seed_tuples_f(seed_t *top, int max, int len)
{
  seed_t g[SOLVER_RNTIS];
  int ng = 0;
  for (int k = 0; k < SOLVER_RNTIS; k++) {
    const solver_rnti_t *q = &g_r[k];
    if (!seed_eligible(q) || (len && q->dlen != len)) continue;
    int j = 0;
    while (j < ng && !seed_same(g[j].src, q)) j++;
    if (j == ng) { g[ng].src = q; g[ng].n = 0; g[ng].ok = 0; ng++; }
    g[j].n++;
    g[j].ok += q->full_ok;
    if (q->full_ok > g[j].src->full_ok) g[j].src = q; /* its most proven member represents the tuple (layouts refined, S map learnt) */
  }
  int nt = 0;
  for (int j = 0; j < ng; j++) {
    int pos = nt;
    while (pos > 0 && (top[pos - 1].n < g[j].n || (top[pos - 1].n == g[j].n && top[pos - 1].ok < g[j].ok))) pos--;
    if (pos >= max) continue;
    for (int z = (nt < max ? nt : max - 1); z > pos; z--) top[z] = top[z - 1];
    top[pos] = g[j];
    if (nt < max) nt++;
  }
  return nt;
}
static int seed_tuples(seed_t *top) { return seed_tuples_f(top, SEED_MAX, 0); }
/* seed_cellwide: whether most solved members of tuple `t` that sit in CORESET `key` use one scrambling (a cell-wide / per-CORESET
 * identity); that scrambling is then *scr. 1 = cell-wide (>= 2 members share it, a majority); 0 = per UE (PCI 923: the members'
 * identities differ, a new RNTI's is not any of theirs); -1 = a single member, no evidence of sharing either way; -2 = no member in this
 * CORESET (the caller keeps its CORESET-level scrambling logic). */
static int seed_cellwide(const seed_t *t, int32_t key, int32_t *scr)
{
  int32_t cand = -1;
  uint32_t c = 0, nk = 0, hit = 0;
  for (int k = 0; k < SOLVER_RNTIS; k++) {
    const solver_rnti_t *q = &g_r[k];
    if (!seed_eligible(q) || q->key != key || !seed_same(t->src, q)) continue;
    nk++;
    if (c == 0) { cand = q->scr; c = 1; } else if (q->scr == cand) c++; else c--;
  }
  for (int k = 0; k < SOLVER_RNTIS; k++) {
    const solver_rnti_t *q = &g_r[k];
    hit += seed_eligible(q) && q->key == key && seed_same(t->src, q) && q->scr == cand;
  }
  *scr = cand;
  if (nk == 0) return -2;
  if (nk < 2) return -1;
  return hit >= 2 && 2u * hit > nk;
}

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
    uint8_t available[3];
    nt[k] = tables_for(rd[order[k]].mcs, available);
    for (int v = 0; v < 48; v++) {
      score[k][v] = UINT32_MAX;
      const int table = tables[(v / 4) % 3];
      bool valid = false;
      for (int t = 0; t < nt[k]; t++) valid |= available[t] == table;
      if (!valid || starts[v % 4] > l0) continue;
      for (int j = 0; j < nc; j++) {
        const int id = rd[order[k]].ids[j];
        if (id >= NR_PDSCH_SOLVER_MAX_LAYOUTS) continue;
        const uint32_t count = coverage_find(c, coverage_key(id, v, payload))->count;
        if (count < score[k][v]) score[k][v] = count;
      }
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
        const double pk = ((double)load[k] + 1.0) / w[k];
        if (pk < total || (pk == total && score[k][v] < count)) {
          best = k; variant = v; count = score[k][v]; total = pk;
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
/* phase_S: the start symbol most CRC passes of the cell show at this slot phase (ties: the lower index of kS order), -1 = no pass yet. The census
 * is kept at 160 slots and folded by the cell period. g_mu held. */
static int phase_S(int ph)
{
  int best = -1;
  uint32_t bn = 0;
  for (int k = 0; k < 4; k++) {
    const int sv = kS[k];
    uint32_t c = 0;
    for (int b = ph; b < SOLVER_PHASES; b += per_now()) c += g_phs[b][sv];
    if (c > bn) { bn = c; best = sv; }
  }
  return best;
}

/* phase_conflict: whether folding the census by period P puts two slot phases with different start symbols on one phase. A census bucket speaks when
 * it has >= 2 passes and one S holds >= 2/3 of them. */
static bool phase_conflict(int P)
{
  for (int q = 0; q < P; q++) {
    int seen = -1;
    for (int b = q; b < SOLVER_PHASES; b += P) {
      uint32_t tot = 0, mx = 0;
      int mxs = -1;
      for (int k = 0; k < 4; k++) {
        tot += g_phs[b][k];
        if (g_phs[b][k] > mx) { mx = g_phs[b][k]; mxs = k; }
      }
      if (tot < 2 || mx * 3 < tot * 2) continue;
      if (seen >= 0 && seen != mxs) return true;
      seen = mxs;
    }
  }
  return false;
}

/* period_grow: moves the phase period to P (a multiple of the old one) and spreads each RNTI's per-phase tables over the finer phases (a learnt phase
 * stays learnt on each phase it now covers). g_mu held. */
static void period_grow(int P)
{
  const int old = per_now();
  for (int i = 0; i < SOLVER_RNTIS; i++) {
    solver_rnti_t *r = &g_r[i];
    if (!r->used) continue;
    for (int q = old; q < P; q++) {
      r->ov_S[q] = r->ov_S[q % old];
      r->ov_L[q] = r->ov_L[q % old];
      r->ph_tr[q] = r->ph_tr[q % old];
      r->ph_ok[q] = r->ph_ok[q % old];
      r->ov_cur[q] = r->ov_cur[q % old];
    }
  }
  LOG_A(PHY, "SENSING: PDSCH_SOLVER cell slot-phase period %d -> %d slots (CRC passes of different start symbols share a phase at %d)\n", old, P, old);
  g_per = P;
}

/* phase_census_note: one CRC pass (full-length allocation) with start symbol S at slot abs; lengthens the phase period while the census conflicts.
 * g_mu held. */
static void phase_census_note(uint64_t abs, int S)
{
  uint32_t *c = &g_phs[abs % SOLVER_PHASES][S];
  if (*c < UINT32_MAX) (*c)++;
  int P = per_now();
  while (P < SOLVER_PHASES && phase_conflict(P)) P *= 2;
  if (P > per_now()) period_grow(P);
}
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
  r->dlen = len;
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
  const bool thin = !r->solved && !csirs_slot && search_backoff(r) && !dmrs_supported(rd, n_rd); /* A1: 7 grants in 8 of a long unsuccessful search */
  if (r->solved) {
    int ri = -1;
    const int ph = ph_of(ctx->now_abs);
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
  } else if (r->nse.parked && !nse_due(&r->nse, r->grants) && !dmrs_supported(rd, n_rd)) {
    r->why[W_PARKED]++; /* PARK: no hypothesis shows DM-RS above the cell's noise floor (statistical test); re-tested 1 grant in 2^k */
  } else if (thin && r->nse.parked) {
    r->why[W_BACKOFF]++; /* a parked RNTI stays thinned (PARK): its re-test grants take the old backoff, no seeded probe either */
  } else {
    /* T26c: an RNTI whose search has spent 200 probes without a single CRC pass is searched on 1 grant in 8 only --
     * the decode pool goes to RNTIs that decode. A1 SEEDBACKOFF: the backoff thins the coverage search (step 3) only; the candidate
     * confirmation (step 1) and the seeded probe (step 2: one reading, one variant on the cell's solved tuples / pool ids) run on every
     * grant, so a new RNTI whose layout the cell already knows is not delayed 8x once its search has spent its probes. */
    if (thin) r->why[W_BACKOFF]++;
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
    /* A2: up to SEED_MAX layout tuples (representative, add_pos, table, S map, identity form) of the cell's solved RNTIs, ranked by how many
     * share them; each grant seeds one, in proportion to those counts. Each is still confirmed by this RNTI's own passes. */
    seed_t seeds[SEED_MAX];
    const int nseed = seed_tuples(seeds);
    int ord[SEED_MAX] = {0, 1, 2};
    uint32_t visit = 0;
    if (nseed > 0) {
      uint32_t tot = 0;
      for (int j = 0; j < nseed; j++) tot += seeds[j].n;
      const uint32_t pos = r->grants % tot;
      uint32_t acc = 0;
      int first = 0;
      while (first < nseed - 1 && pos >= acc + seeds[first].n) acc += seeds[first++].n;
      visit = (r->grants / tot) * seeds[first].n + (pos - acc); /* this tuple's visit count of the RNTI */
      ord[0] = first;
      for (int j = 0, w = 1; j < nseed; j++)
        if (j != first) ord[w++] = j;
    }
    const int smax = thin && n + 1 < budget ? n + 1 : budget; /* A1: one variant while the coverage search is thinned */
    const int32_t seed_scr0 = t_scr;
    bool ids_done = false;
    if (ctx->own && nseed > 0) { /* TIMSOLVE2: proven dims x each own identity in the proven form */
      for (int jj = 0; jj < nseed; jj++) {
        const seed_t *sd = &seeds[ord[jj]];
        int32_t hyp[16];
        int nh = 0;
        for (int q = 0; q < ctx->n; q++)
          if (scr_form(ctx->ids[q]) == scr_form(sd->src->scr)) hyp[nh++] = ctx->ids[q];
        for (int h = 0; h < nh; h++) {
          t_scr = hyp[(h + r->grants) % (uint32_t)nh]; /* rotate: a budget of 1 still covers every identity */
          for (int i = 0; i < n_rd; i++)
            if (has_id(&rd[i], sd->src->rep)) {
              plan_known(out, &n, smax, slot, 0, i, rd, l0, sd->src->rep, sd->src->add_pos, sd->src->table, sd->src->S_by_tda,
                         sd->src->L_by_tda, mcs, dci, payload);
              break;
            }
        }
        ids_done |= nh > 0;
      }
      t_scr = seed_scr0;
    }
    if (ids_done) {
      /* the proven dims were planned with the RNTI's own identities above */
    } else if (nseed > 0) {
      const seed_t *sd = &seeds[ord[0]];
      int32_t cw_scr = -1;
      const int cw = ctx->own ? -2 : seed_cellwide(sd, ctx->key, &cw_scr);
      const int form = scr_form(sd->src->scr);
      if (cw == 1) {
        t_scr = cw_scr; /* the tuple's own identity (415: 2 templates = 2 identities) */
      } else if ((cw == 0 || cw == -1) && ctx->np > 0 && form >= 0 && (cw == 0 || (visit & 1u))) {
        /* A3: per-UE identities (or no evidence of sharing yet, then every other visit): the identities the cell has shown, most frequent
         * first, in the tuple's identity form; cycled visit by visit */
        const int32_t id = ctx->pool[(cw == 0 ? visit : visit / 2u) % (uint32_t)ctx->np];
        t_scr = form == 0 ? id : (int32_t)((uint32_t)id | ((uint32_t)form << 16));
      } else if (cw == -1) {
        t_scr = cw_scr;
      }
      /* FIRSTSLOT: (1) the S of this slot phase (cell-wide CRC passes) first, then the tuple's own S -- a tda-keyed S copied from one member is
       * right only at the phases that member learnt it at (OTA 415 2026-10-31: seed S matched the phase's S in 11-13 of 60-71 first slots,
       * base 43 of 56). (2) the tuple's rep stands for a set of layouts (cand_bm) that two CRC passes did not separate; the readings carrying
       * most of that set are seeded too (2 more), as the refinement of a solved RNTI does, instead of the rep's reading alone. */
      const int tda_s = tda_of(sd->src->rep, payload) & 15;
      const int sph = phase_S(ph_of(ctx->now_abs));
      int8_t Sm[16], Lm[16];
      memcpy(Sm, sd->src->S_by_tda, sizeof(Sm));
      memcpy(Lm, sd->src->L_by_tda, sizeof(Lm));
      int rdi[3], nrd = 0;
      for (int i = 0; i < n_rd; i++)
        if (has_id(&rd[i], sd->src->rep)) { rdi[nrd++] = i; break; }
      if (sd->src->cand_bm)
        for (int e = 0; e < 2 && nrd < 3; e++) {
          int bi = -1, bov = 0;
          for (int i = 0; i < n_rd; i++) {
            bool used = false;
            for (int u = 0; u < nrd; u++) used |= rdi[u] == i;
            if (used) continue;
            int ov = 0;
            for (int k = 0; k < rd[i].n_ids; k++) ov += bm_has(sd->src->cand_bm, rd[i].ids[k]);
            if (ov > bov) { bov = ov; bi = i; }
          }
          if (bi < 0) break;
          rdi[nrd++] = bi;
        }
      for (int u = 0; u < nrd; u++) {
        if (sph >= 0 && sph != Sm[tda_s]) {
          int8_t S2[16], L2[16];
          memcpy(S2, Sm, sizeof(S2));
          memcpy(L2, Lm, sizeof(L2));
          S2[tda_s] = (int8_t)sph;
          L2[tda_s] = -1;
          plan_known(out, &n, smax, slot, 0, rdi[u], rd, l0, sd->src->rep, sd->src->add_pos, sd->src->table, S2, L2, mcs, dci, payload);
        }
        plan_known(out, &n, smax, slot, 0, rdi[u], rd, l0, sd->src->rep, sd->src->add_pos, sd->src->table, Sm, Lm, mcs, dci, payload);
      }
      t_scr = seed_scr0;
    } else if (g_have_prior) {
      for (int i = 0; i < n_rd; i++)
        if (has_id(&rd[i], g_prior_rep)) {
          plan_known(out, &n, smax, slot, 0, i, rd, l0, g_prior_rep, g_prior_add_pos, g_prior_table,
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
    if (no == 0 && !thin) r->why[W_WIDE]++;
    if (no > 0 && !thin && !ctx->seeded_only) /* (S, add_pos) tiers over the readings: every reading gets all S in one go, because the true S changes from grant to grant with the TDA index */
      plan_coverage(r, out, &n, budget, slot, rd, order, no, l0, dci, payload);
  }
  if (r->solved && r->nse.parked && !nse_due(&r->nse, r->grants)) { /* PARK: a solved RNTI whose DM-RS vanished keeps its full decodes, drops its probes */
    int m = 0;
    for (int i = 0; i < n; i++) {
      if (out[i].probe) {
        pending_t *q = &g_p[out[i].token & (SOLVER_PENDING - 1)];
        if (q->used && q->token == out[i].token) q->used = false;
        r->why[W_PARKED]++;
      } else {
        out[m++] = out[i];
      }
    }
    n = m;
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
  r->phys = r->fam = false; /* E3: a CRC-solved layout */
  r->qm_last = 0; memset(r->qm_agree, 0, sizeof r->qm_agree); memset(r->qm_dis, 0, sizeof r->qm_dis); r->qm_poison = false;
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
  g_nse_solve[g_nse_solve_n++ % NSE_SOLVE_RING] = r->n_probe;
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
    typedef _Atomic uint64_t s_lost_arr_t[2];
    NR_CELL_STATIC(s_lost_arr_t, s_lost);
#define s_lost NR_CELL_REF(s_lost)
    const uint64_t k = atomic_fetch_add(&s_lost[p == NULL ? 0 : 1], 1) + 1;
    if (k <= 5 || (k % 100) == 0)
      LOG_A(PHY, "SENSING: PDSCH_SOLVER lost CRC pass #%llu (%s) token=0x%08x\n", (unsigned long long)k,
            p == NULL ? "pending entry gone" : "no credit", token);
  }
#undef s_lost
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
  solver_rnti_t *r = &g_r[p->slot];
  if (crc_ok && p->S < 4 && p->L == 14u - p->S && !p->ov) phase_census_note(p->abs, (int)p->S);
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
    const int ph = ph_of(p->abs);
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
      r->ph_tr[ph_of(p->abs)]++;
      if (crc_ok) r->ph_ok[ph_of(p->abs)]++;
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
  if ((uint32_t)bad > r->hq_bad_prev) r->hrv_alert = true; /* a NEW violation, not an old one still in the ring */
  r->hq_bad_prev = (uint32_t)bad;
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
  if (r != NULL) r->hrv_alert = true;
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
#define TF_WMAX 4 /* TS 38.212 7.3.1.2.2: the TDRA field is 0..4 bits (a list of up to 16 rows) */
#define TF_EXPLORE_MIN 32 /* first-pass decodes of one value before its row may be called unknown (tf_dead) */
#define TF_DEAD_RATIO 8   /* a value passing 8x less often than a sibling value has an unknown row */
#define TF_ROW_MIN 3      /* CRC passes that establish a row (stray passes of other rows are then dropped) */
#define TF_K0_MAX 2       /* rows searched up to K0 = 2 (the queue waits up to 3 ms for a future slot) */
typedef struct { uint8_t S, L, k0; uint32_t n; } tf_row_t;
typedef struct {
  int pos; /* -1 free */
  uint32_t cls[TF_WMAX][1 << TF_WMAX][2];    /* [w-1][value][mixed] accepted DCIs */
  tf_row_t row[TF_WMAX][1 << TF_WMAX][2][3]; /* [w-1][value][mixed] (S,L,K0) of CRC-passing decodes */
  uint32_t res[TF_WMAX][1 << TF_WMAX][2][2]; /* [w-1][value][mixed][0 decodes, 1 CRC ok]: first-pass outcomes (nr_tda_field_note_result) */
  uint8_t cur[1 << TF_WMAX][2];              /* exploration cursor of a value whose row is unknown */
  uint8_t det_w;                             /* 0 undetected, else the accepted width */
  uint8_t boot_w;                            /* 0, else the width an undetected field was explored at (tf_eval accepts TF_ROW_MIN passes there) */
} tf_pos_t;
NR_CELL_STATIC_INIT(pthread_mutex_t, g_tf_mu, PTHREAD_MUTEX_INITIALIZER);
#define g_tf_mu NR_CELL_REF(g_tf_mu)
typedef tf_pos_t g_tf_arr_t[TF_NPOS];
NR_CELL_STATIC(g_tf_arr_t, g_tf);
#define g_tf NR_CELL_REF(g_tf)
typedef uint8_t g_tf_S_arr_t[16];
NR_CELL_STATIC(g_tf_S_arr_t, g_tf_S);
#define g_tf_S NR_CELL_REF(g_tf_S)
typedef uint8_t g_tf_L_arr_t[16];
NR_CELL_STATIC(g_tf_L_arr_t, g_tf_L);
#define g_tf_L NR_CELL_REF(g_tf_L)
NR_CELL_STATIC(int, g_tf_n);
#define g_tf_n NR_CELL_REF(g_tf_n)
NR_CELL_STATIC(int, g_tf_init);
#define g_tf_init NR_CELL_REF(g_tf_init)
NR_CELL_STATIC(int, g_tf_newrow_logs);
#define g_tf_newrow_logs NR_CELL_REF(g_tf_newrow_logs)

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
#define DB_W 8
typedef uint32_t g_db_arr_t[DB_W][2][2]; /* [bit][value][0 = decodes, 1 = CRC ok] */
NR_CELL_STATIC(g_db_arr_t, g_db);
#define g_db NR_CELL_REF(g_db)
NR_CELL_STATIC(uint32_t, g_db_n);
#define g_db_n NR_CELL_REF(g_db_n)
NR_CELL_STATIC(int, g_db_pos);
#define g_db_pos NR_CELL_REF(g_db_pos)
void nr_tda_field_forget(void)
{
  pthread_mutex_lock(&g_tf_mu);
  memset(g_tf, 0, sizeof(g_tf));
  g_tf_init = 0;
  tf_init_locked();
  g_db_n = 0;
  pthread_mutex_unlock(&g_tf_mu);
  nr_pm_forget(); /* the pre-MCS window learner of the same cell */
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
int nr_tda_field_width(int pos)
{
  pthread_mutex_lock(&g_tf_mu);
  const tf_pos_t *t = tf_get(pos, false);
  const int w = t ? t->det_w : 0;
  pthread_mutex_unlock(&g_tf_mu);
  return w;
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
      while (q < na && !(a[q].S == r->S && a[q].L == r->L && a[q].k0 == r->k0)) q++;
      if (q == na) { a[na] = *r; a[na].n = 0; na++; }
      a[q].n += r->n;
    }
  for (int q = 0; q < na; q++) if (a[q].n > bn) { bn = a[q].n; *best = a[q]; }
  *tot = sum;
  return bn;
}
/* the row of value v is unknown: >= TF_EXPLORE_MIN first-pass decodes since its row was established and none passed (without a sibling
 * requirement only if !need_sib), or a sibling value with >= TF_MIN passes passes TF_DEAD_RATIO times more often. PCI 403 OTA 2026-10-30:
 * value 01 passed once in 1453 decodes (a stray pass) against 1 in 13 for value 00; a zero-pass rule never fired. */
static bool tf_dead(const tf_pos_t *t, int wi, int v, int m, bool need_sib)
{
  const uint32_t *r = t->res[wi][v][m];
  if (r[0] < TF_EXPLORE_MIN) return false;
  if (r[1] == 0 && !need_sib) return true;
  for (int u = 0; u < (1 << (wi + 1)); u++) {
    const uint32_t *q = t->res[wi][u][m];
    if (u != v && q[1] >= TF_MIN && (uint64_t)r[1] * q[0] * TF_DEAD_RATIO < (uint64_t)q[1] * r[0]) return true;
  }
  return false;
}
static void tf_eval(tf_pos_t *t)
{
  if (t->det_w) return;
  /* The SIB1 common list is only a prior: a dedicated pdsch-TimeDomainAllocationList replaces it for C-RNTI DCIs, with its own
   * length (field width) and rows. Its width is tried first, then every width 1..TF_WMAX on CRC evidence (b). */
  const int wl = tf_list_w();
  for (int k = 0; k <= TF_WMAX && !t->det_w; k++) {
    const int w = k == 0 ? wl : k;
    if (w < 1 || w > TF_WMAX || (k > 0 && w == wl)) continue;
    const int wi = w - 1, nv = 1 << w;
    if (g_tf_n > 0 && w == wl) { /* (a) value depends on the TDD slot class */
      uint32_t tm = 0, to = 0, bm = 0, bo = 0; int dm = 0, dob = 0;
      for (int v = 0; v < nv; v++) {
        tm += t->cls[wi][v][1]; to += t->cls[wi][v][0];
        if (t->cls[wi][v][1] > bm) { bm = t->cls[wi][v][1]; dm = v; }
        if (t->cls[wi][v][0] > bo) { bo = t->cls[wi][v][0]; dob = v; }
      }
      if (tm >= TF_MIN && to >= TF_MIN && bm * 10 >= tm * 9 && bo * 10 >= to * 9 && dm != dob) t->det_w = (uint8_t)w;
    }
    if (!t->det_w) { /* (b) CRC-passing decodes of different values need different (S,L) */
      tf_row_t r[1 << TF_WMAX]; int q = 0; bool diff = false;
      const uint32_t vmin = w == t->boot_w ? TF_ROW_MIN : TF_MIN; /* an explored value: its established row is the evidence */
      for (int v = 0; v < nv; v++) {
        uint32_t tot, bn = tf_dom_v(t, wi, v, &r[q], &tot);
        if (tot >= vmin && bn * 10 >= tot * 9) {
          for (int k = 0; k < q; k++) diff |= r[k].S != r[q].S || r[k].L != r[q].L || r[k].k0 != r[q].k0;
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
    for (int w = 1; w <= TF_WMAX; w++)
      if (pos + w <= len) t->cls[w - 1][nr_dci_bits_field(&p, len, pos, w)][mixed]++;
    tf_eval(t);
  }
  pthread_mutex_unlock(&g_tf_mu);
}
void nr_tda_field_note_ok(int pos, nr_dci_bits_t p, int len, bool mixed, int S, int L)
{
  nr_tda_field_note_ok_k0(pos, p, len, mixed, S, L, 0);
}
void nr_tda_field_note_ok_k0(int pos, nr_dci_bits_t p, int len, bool mixed, int S, int L, int k0)
{
  if (pos < 0 || len <= pos) return;
  pthread_mutex_lock(&g_tf_mu);
  tf_pos_t *t = tf_get(pos, true);
  for (int w = 1; t && w <= TF_WMAX; w++) {
    if (pos + w > len) continue;
    tf_row_t *c = t->row[w - 1][nr_dci_bits_field(&p, len, pos, w)][mixed];
    int k = 0;
    while (k < 3 && !(c[k].n && c[k].S == S && c[k].L == L && c[k].k0 == k0)) k++;
    if (k == 3) { /* new row: take a free slot, else replace the weakest */
      const uint32_t v = (uint32_t)nr_dci_bits_field(&p, len, pos, w);
      if (tf_dead(t, w - 1, (int)v, mixed, false)) { /* the first pass of an unknown-row value (exploration): it supersedes the stray rows */
        for (int j = 0; j < 3; j++)
          if (c[j].n < TF_ROW_MIN) c[j].n = 0;
        t->res[w - 1][v][mixed][0] = t->res[w - 1][v][mixed][1] = 0;
      }
      k = 0;
      for (int j = 1; j < 3; j++) if (c[j].n < c[k].n) k = j;
      c[k].S = (uint8_t)S; c[k].L = (uint8_t)L; c[k].k0 = (uint8_t)k0; c[k].n = 0;
      if (w == t->det_w && g_tf_newrow_logs < 64) { /* a new row of the detected field: the list or the exploration found it */
        g_tf_newrow_logs++;
        LOG_A(PHY, "SENSING: TDAFIELD row pos=%d w=%d value=%u mixed=%d -> K0=%d S=%d L=%d (value decodes/ok before %u/%u)\n", pos, w,
              (unsigned)nr_dci_bits_field(&p, len, pos, w), (int)mixed, k0, S, L, t->res[w - 1][nr_dci_bits_field(&p, len, pos, w)][mixed][0],
              t->res[w - 1][nr_dci_bits_field(&p, len, pos, w)][mixed][1]);
      }
    }
    if (++c[k].n == TF_ROW_MIN) { /* an established row: earlier stray passes of other rows go, and the value's pass rate restarts */
      for (int j = 0; j < 3; j++)
        if (j != k && c[j].n < TF_ROW_MIN) c[j].n = 0;
      t->res[w - 1][nr_dci_bits_field(&p, len, pos, w)][mixed][0] = t->res[w - 1][nr_dci_bits_field(&p, len, pos, w)][mixed][1] = 0;
    }
  }
  if (t) tf_eval(t);
  pthread_mutex_unlock(&g_tf_mu);
}
void nr_tda_field_note_result(int pos, nr_dci_bits_t p, int len, bool mixed, bool ok)
{
  if (pos < 0 || len <= pos) return;
  char b[DB_W * 32 + TF_NPOS * 72];
  int u = 0;
  uint32_t n;
  pthread_mutex_lock(&g_tf_mu);
  tf_pos_t *t = tf_get(pos, true);
  for (int w = 1; t && w <= TF_WMAX && pos + w <= len; w++) {
    uint32_t *r = t->res[w - 1][nr_dci_bits_field(&p, len, pos, w)][mixed];
    r[0]++;
    r[1] += ok;
  }
  if (g_db_n == 0 || pos != g_db_pos) { memset(g_db, 0, sizeof(g_db)); g_db_n = 0; g_db_pos = pos; } /* a new window starts a new tally */
  for (int k = 0; k < DB_W && pos + k < len; k++) {
    const int v = (int)nr_dci_bits_field(&p, len, pos + k, 1);
    g_db[k][v][0]++;
    g_db[k][v][1] += ok;
  }
  n = ++g_db_n;
  if (n % 4000 == 0) {
    for (int k = 0; k < DB_W && pos + k < len; k++)
      u += snprintf(b + u, sizeof(b) - u, " b%d=%u/%u|%u/%u", k, g_db[k][0][1], g_db[k][0][0], g_db[k][1][1], g_db[k][1][0]);
    /* the learner's own view: every tracked window, its detected width and the 2-bit values' decodes/ok in full slots */
    u += snprintf(b + u, sizeof(b) - u, " | tf");
    for (int i = 0; i < TF_NPOS && u < (int)sizeof(b) - 64; i++)
      if (g_tf[i].pos >= 0)
        u += snprintf(b + u, sizeof(b) - u, " p%d/w%d=%u/%u,%u/%u,%u/%u,%u/%u", g_tf[i].pos, g_tf[i].det_w, g_tf[i].res[1][0][0][1],
                      g_tf[i].res[1][0][0][0], g_tf[i].res[1][1][0][1], g_tf[i].res[1][1][0][0], g_tf[i].res[1][2][0][1],
                      g_tf[i].res[1][2][0][0], g_tf[i].res[1][3][0][1], g_tf[i].res[1][3][0][0]);
  }
  pthread_mutex_unlock(&g_tf_mu);
  if (u) LOG_A(PHY, "SENSING: DCIBITS pos=%d len=%d n=%u (bit k after pos: ok/decodes at value 0 | value 1; tf: window/width = 2-bit values ok/decodes)%s\n", pos, len, n, b);
}
int nr_tda_field_rows(int pos, nr_dci_bits_t p, int len, int last, int max, uint8_t *S, uint8_t *L, bool *detected)
{
  const int8_t lastk[TF_K0_MAX + 1] = {(int8_t)last, -1, -1}, dlk[TF_K0_MAX + 1] = {0, 0, 0};
  uint8_t K0[2];
  return nr_tda_field_rows_k0(pos, p, len, lastk, dlk, max, S, L, K0, detected);
}
/* The exploration candidates of one grant: for K0 = 0..TF_K0_MAX into a slot with downlink, mapping type A rows starting at S = 0..3 and
 * ending at the slot's last DL symbol (dl - 1) or its last PDSCH-able symbol (last: flexible symbols included). Order: K0, S, end; the
 * start symbols the decode already tries go last (S = 1 is the solver's usual reading, S = 0 its retry toggle), so S = 2, 3 come first. */
static int tf_candidates(const int8_t *last, const int8_t *dl, tf_row_t *c, int max)
{
  int n = 0;
  for (int k0 = 0; k0 <= TF_K0_MAX; k0++) {
    int ends[2], ne = 0;
    if (dl[k0] >= 3) ends[ne++] = dl[k0] - 1;
    if (last[k0] >= 2 && (ne == 0 || last[k0] != ends[0])) ends[ne++] = last[k0];
    const int kSo[4] = {2, 3, 0, 1};
    for (int si = 0; si < 4; si++)
      for (int e = 0, S0 = kSo[si]; e < ne && n < max; e++) {
        const int Lc = ends[e] + 1 - S0;
        if (Lc >= 3) c[n++] = (tf_row_t){.S = (uint8_t)S0, .L = (uint8_t)Lc, .k0 = (uint8_t)k0, .n = 0};
      }
  }
  return n;
}
int nr_tda_field_rows_k0(int pos, nr_dci_bits_t p, int len, const int8_t *last, const int8_t *dl, int max, uint8_t *S, uint8_t *L,
                         uint8_t *K0, bool *detected)
{
  int n = 0;
  if (detected) *detected = false;
  if (pos < 0 || len <= pos || max <= 0 || !S || !L || !K0 || !last || !dl) return 0;
  const bool mixed = last[0] < 13;
  pthread_mutex_lock(&g_tf_mu);
  tf_pos_t *t = tf_get(pos, false);
  const int w = t ? t->det_w : 0;
  if (detected) *detected = w > 0;
  if (w) { /* a detected field: the (S,L,K0) learnt for (value, slot class), CRC-arbitrated, comes first (list or not) */
    const int v = (int)nr_dci_bits_field(&p, len, pos, w);
    tf_row_t b = {0}; uint32_t tot;
    const uint32_t bn = tf_dom(t, w - 1, v, mixed, &b, &tot);
    const bool dead = tf_dead(t, w - 1, v, mixed, false);
    if (bn && !dead && (bn >= TF_ROW_MIN || bn == tot) && bn * 10 >= tot * 8 && b.k0 <= TF_K0_MAX && last[b.k0] >= 0 && b.S + b.L - 1 <= last[b.k0]) {
      S[0] = b.S; L[0] = b.L; K0[0] = b.k0; n = 1;
    } else if (dead) {
      /* a value whose grants never pass with the solver's reading names a row nobody learnt (a dedicated list row, a cross-slot K0):
       * each of its grants tries the next TDD-legal candidate instead; the first CRC pass is learnt above. Costs no extra decode. */
      tf_row_t c[4 * 2 * (TF_K0_MAX + 1)];
      const int nc = tf_candidates(last, dl, c, (int)(sizeof(c) / sizeof(c[0])));
      if (nc > 0) {
        const tf_row_t r = c[t->cur[v][mixed]++ % nc];
        S[0] = r.S; L[0] = r.L; K0[0] = r.k0; n = 1;
      }
    }
  }
  for (int wb = 1; t && !w && n == 0 && wb <= TF_WMAX && pos + wb <= len; wb++) {
    /* BOOTSTRAP: an undetected field whose value v has an unknown row (tf_dead, a sibling value of the same width passing) is explored
     * like a detected one (PCI 403 OTA 2026-10-30: value 1 = S2 L12 failed 475-6825 times per run, and the field was detected only when
     * some RNTI happened to solve with an S = 2 layout). The explored row's TF_ROW_MIN-th pass makes rule (b) detect the field at wb. */
    const int v = (int)nr_dci_bits_field(&p, len, pos, wb);
    if (!tf_dead(t, wb - 1, v, mixed, true)) continue;
    if (!t->boot_w) t->boot_w = (uint8_t)wb;
    tf_row_t c[4 * 2 * (TF_K0_MAX + 1)];
    const int nc = tf_candidates(last, dl, c, (int)(sizeof(c) / sizeof(c[0])));
    if (nc > 0) {
      const tf_row_t r = c[t->cur[v][mixed]++ % nc];
      S[0] = r.S; L[0] = r.L; K0[0] = r.k0; n = 1;
    }
  }
  if (g_tf_n > 0) {
    if (n > 0) goto out;        /* a learnt or explored row names the grant: no second decode */
    if (!mixed && !w) goto out; /* an unconfirmed field in a full slot: keep the solver's reading */
    const int wl = tf_list_w();
    const uint32_t v = w == wl ? (uint32_t)nr_dci_bits_field(&p, len, pos, w) : 0xFFu; /* the list row only at the list's width */
    if (v < (uint32_t)g_tf_n && g_tf_S[v] + g_tf_L[v] - 1 <= last[0]) { S[n] = g_tf_S[v]; L[n] = g_tf_L[v]; K0[n] = 0; n++; }
    for (int r = 0; r < g_tf_n && n < max && !w; r++) { /* a detected field names the row: the other legal rows are tried only without it (<= 2 decodes) */
      if (g_tf_S[r] + g_tf_L[r] - 1 > last[0]) continue;
      S[n] = g_tf_S[r]; L[n] = g_tf_L[r]; K0[n] = 0; n++;
    }
  }
out:
  pthread_mutex_unlock(&g_tf_mu);
  return n;
}

/* ---- PMEXPLORE (see nr_pdsch_solver.h) ---- */
#define PM_NWIN 4
#define PM_EXPLORE_MIN 32
typedef struct {
  int pos, nbits;                        /* -1: free */
  uint32_t res[16][NR_PM_HYP_N][2];      /* [value][hyp][0 decodes, 1 CRC ok] */
  int8_t learnt[16];                     /* hypothesis that passed for the value, -1 none */
  uint8_t cur[16];
} pm_win_t;
typedef pm_win_t g_pm_arr_t[PM_NWIN];
NR_CELL_STATIC(g_pm_arr_t, g_pm);
#define g_pm NR_CELL_REF(g_pm)
NR_CELL_STATIC(int, g_pm_init);
#define g_pm_init NR_CELL_REF(g_pm_init)
NR_CELL_STATIC(int, g_pm_logs);
#define g_pm_logs NR_CELL_REF(g_pm_logs)
static pm_win_t *pm_get(int pos, int nbits, bool create) /* g_tf_mu held */
{
  if (!g_pm_init) {
    g_pm_init = 1;
    for (int i = 0; i < PM_NWIN; i++) { g_pm[i].pos = -1; memset(g_pm[i].learnt, -1, sizeof(g_pm[i].learnt)); }
  }
  for (int i = 0; i < PM_NWIN; i++)
    if (g_pm[i].pos == pos && g_pm[i].nbits == nbits) return &g_pm[i];
  for (int i = 0; create && i < PM_NWIN; i++)
    if (g_pm[i].pos < 0) {
      memset(&g_pm[i], 0, sizeof(g_pm[i]));
      memset(g_pm[i].learnt, -1, sizeof(g_pm[i].learnt));
      g_pm[i].pos = pos; g_pm[i].nbits = nbits;
      return &g_pm[i];
    }
  return NULL;
}
int nr_pm_hyp(int pos, int nbits, nr_dci_bits_t p, int len)
{
  if (pos < 0 || nbits < 1 || nbits > 4 || pos + nbits > len) return NR_PM_HYP_PLAIN;
  const int v = (int)nr_dci_bits_field(&p, len, pos, nbits);
  int h = NR_PM_HYP_PLAIN;
  pthread_mutex_lock(&g_tf_mu);
  pm_win_t *w = pm_get(pos, nbits, false);
  if (w) {
    if (w->learnt[v] >= 0)
      h = w->learnt[v];
    else if (w->res[v][NR_PM_HYP_PLAIN][0] >= PM_EXPLORE_MIN && w->res[v][NR_PM_HYP_PLAIN][1] == 0)
      h = 1 + w->cur[v]++ % (NR_PM_HYP_N - 1); /* the plain reading never passes for this value: try the others in turn */
  }
  pthread_mutex_unlock(&g_tf_mu);
  return h;
}
void nr_pm_note_result(int pos, int nbits, nr_dci_bits_t p, int len, int hyp, bool ok)
{
  if (pos < 0 || nbits < 1 || nbits > 4 || pos + nbits > len || hyp < 0 || hyp >= NR_PM_HYP_N) return;
  const int v = (int)nr_dci_bits_field(&p, len, pos, nbits);
  bool log = false;
  uint32_t n0 = 0;
  pthread_mutex_lock(&g_tf_mu);
  pm_win_t *w = pm_get(pos, nbits, true);
  if (w) {
    w->res[v][hyp][0]++;
    w->res[v][hyp][1] += ok;
    if (ok && hyp != NR_PM_HYP_PLAIN && w->learnt[v] < 0 && w->res[v][NR_PM_HYP_PLAIN][1] == 0) {
      w->learnt[v] = (int8_t)hyp;
      n0 = w->res[v][NR_PM_HYP_PLAIN][0];
      log = g_pm_logs++ < 32;
    }
  }
  pthread_mutex_unlock(&g_tf_mu);
  if (log)
    LOG_A(PHY, "SENSING: PMEXPLORE learnt pos=%d bits=%d value=%d -> %s (plain decodes before %u, none passed)\n", pos, nbits, v,
          hyp == NR_PM_HYP_ILV2 ? "interleaved VRB-to-PRB L=2" : "interleaved VRB-to-PRB L=4", n0);
}
void nr_pm_forget(void)
{
  pthread_mutex_lock(&g_tf_mu);
  g_pm_init = 0;
  pthread_mutex_unlock(&g_tf_mu);
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

/* mpci A5: allocates cell `cell`'s heap objects (zeroed) before the RX fence; see nr_pdsch_cell_prealloc.h */
void nr_pdsch_solver_cell_prealloc(int cell)
{
  NR_CELL_HEAP_PREALLOC(g_p_arr_t, g_p, cell);
  NR_CELL_HEAP_PREALLOC(g_r_arr_t, g_r, cell);
}

/* ================= E3 (traffic mode): CRC-free SOLVED path from DM-RS physics, layout families, Qm ledger ================= */
#define PHYS_MIN_DET 10      /* ceil(log2(1 / NBS_SIGNIF_ALPHA)): a one-sided sign test cannot reach alpha with fewer detections, whatever the rivals */
#define PHYS_GROUP_MAX 256   /* tie group size the HARQ/RV test examines (RVC_MAX_SCORED) */
#define PHYS_ADMIT 3         /* new hypotheses admitted per grant (the best significant ones not tracked yet) */
#define PHYS_RING 16         /* looks of the score-drop rule */
#define PHYS_P0 0.25         /* a layout that is right is not confirmed on up to this share of its scorable looks (low SNR, a neighbour's DM-RS) */
#define QM_MIN 8             /* distinct-slot informative grants of a Qm consensus */

/* phys_forget: clears every E3 state of an unsolved / reopened RNTI (the sign test, the score ring, the phys flags; the Qm ledger stays with the RNTI's life). */
static void phys_forget(solver_rnti_t *r)
{
  r->phys = r->fam = false;
  r->px_n = 0;
  r->px_last_abs = 0;
  memset(r->px_c, 0, sizeof r->px_c);
  memset(r->px_a, 0, sizeof r->px_a);
  r->ring = 0;
  r->ring_n = 0;
  r->tick = 0;
}

int nr_pdsch_solver_layout_evidence(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  const int ev = !r || !r->solved ? NR_LAYOUT_EV_NONE : (r->phys && r->full_ok == 0) ? NR_LAYOUT_EV_PHYS : NR_LAYOUT_EV_CRC;
  pthread_mutex_unlock(&g_mu);
  return ev;
}

bool nr_pdsch_solver_derive_inputs(uint16_t rnti, int tda, bool *crc, int *add_pos, int *table, int *S, int *L, bool *ov)
{
  bool ok = false;
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  if (r && r->solved && r->table >= 0 && tda >= 0 && tda < 16) {
    ok = true;
    *crc = !(r->phys && r->full_ok == 0);
    *add_pos = r->add_pos; *table = r->table;
    *S = r->S_by_tda[tda];
    *L = r->S_by_tda[tda] < 0 ? -1 : r->L_by_tda[tda] > 0 ? r->L_by_tda[tda] : 14 - r->S_by_tda[tda];
    *ov = false;
    for (int i = 0; i < SOLVER_PHASES; i++) if (r->ov_S[i] >= 0) *ov = true;
  }
  pthread_mutex_unlock(&g_mu);
  return ok;
}

bool nr_pdsch_solver_phys_unconfirmed(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  const bool u = r && r->solved && r->phys && r->full_ok == 0;
  pthread_mutex_unlock(&g_mu);
  return u;
}

/* the ids of a reading are in no particular order (the monitor appends them per FDRA class); a reading holds the layouts of one DCI, a few dozen to a few thousand */
static bool ids_has(const uint16_t *ids, int n, int id)
{
  for (int i = 0; i < n; i++)
    if (ids[i] == id) return true;
  return false;
}

/* phys_solve: the RNTI becomes SOLVED on the reading `ids` (add_pos, scr as scored). No CRC evidence exists: table, S, L and rate matching are unknown and are
 * learnt by CRC like any solved RNTI's; the solution does not seed the cell until a full TB passes (seed_eligible). rep is forced (the evidence layout). Under g_mu. */
static bool phys_solve(solver_rnti_t *r, int rep, int32_t scr, uint8_t add_pos, uint64_t abs_slot, bool fam)
{
  if (rep < 0 || rep >= NR_PDSCH_SOLVER_MAX_LAYOUTS) return false;
  if (r->cand_bm == NULL) r->cand_bm = calloc(1, BM_BYTES);
  if (r->cand_bm == NULL) return false;
  memset(r->cand_bm, 0, BM_BYTES);
  r->cand_bm[rep >> 3] |= (uint8_t)(1u << (rep & 7));   /* only the layout the evidence picked: never "the lowest id of an equivalent set" */
  r->solved = true;
  r->has_cand = false;
  memset(r->S_by_tda, -1, sizeof(r->S_by_tda));
  memset(r->L_by_tda, -1, sizeof(r->L_by_tda));
  r->n_ev = 0;
  r->rep = rep;
  r->add_pos = add_pos;
  r->scr = scr;
  r->table = -1;
  r->phys = true;
  r->fam = fam;
  r->px_n = 0;
  memset(r->px_c, 0, sizeof r->px_c);
  memset(r->px_a, 0, sizeof r->px_a);
  r->ring = 0;
  r->ring_n = 0;
  r->tick = 0;
  r->qm_last = 0; memset(r->qm_agree, 0, sizeof r->qm_agree); memset(r->qm_dis, 0, sizeof r->qm_dis); r->qm_poison = false;   /* votes belong to a layout */
  r->solved_abs = abs_slot;
  memset(r->comb_try, 0, sizeof(r->comb_try));
  memset(r->comb_ok, 0, sizeof(r->comb_ok));
  LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x SOLVED by DM-RS physics (%s, no CRC): layout %d add_pos=%u scr=%d\n", r->rnti, fam ? "family" : "sign test", r->rep,
        r->add_pos, (int)r->scr);
  return true;
}

static void px_drop(solver_rnti_t *r, int i)
{
  const int n = r->px_n;
  for (int a = i; a + 1 < n; a++) r->px_c[a] = r->px_c[a + 1];
  for (int a = 0; a < n; a++)
    for (int b = i; b + 1 < n; b++) r->px_a[a][b] = r->px_a[a][b + 1];
  for (int a = i; a + 1 < n; a++) memcpy(r->px_a[a], r->px_a[a + 1], sizeof r->px_a[a]);
  r->px_n--;
}

/* px_leader: the tracked hypothesis that was significant on >= PHYS_MIN_DET grants and beats every other tracked one significantly, else -1. A tie (identical
 * rank on every grant: A = B = 0) is never a win: footprint-twins keep the RNTI unsolved. */
static int px_leader(const solver_rnti_t *r)
{
  const int n = r->px_n;
  for (int i = 0; i < n; i++) {
    if (r->px_c[i].det < PHYS_MIN_DET) continue;
    bool ok = true;
    int nrival = 0;
    for (int j = 0; j < n; j++) if (j != i && !(r->px_a[i][j] == 0 && r->px_a[j][i] == 0 && r->px_c[j].det >= PHYS_MIN_DET)) nrival++;
    for (int j = 0; j < n && ok; j++) {
      if (j == i) continue;
      const uint64_t A = r->px_a[i][j], B = r->px_a[j][i];
      if (A == 0 && B == 0 && r->px_c[j].det >= PHYS_MIN_DET) continue;   /* a twin: equally significant on as many grants and never out-ranked either way */
      ok = A > B && nbs_binom_sf(A, A + B, 0.5) < NBS_SIGNIF_ALPHA / (double)(nrival > 0 ? nrival : 1);
    }
    if (ok) return i;
  }
  return -1;
}

int nr_pdsch_solver_phys_note2(uint16_t rnti, uint64_t abs_slot, const nr_pdsch_phys_hyp_t *h, int n, nr_pdsch_e1_fn e1, void *e1_ctx, uint16_t dci_len, bool incomplete)
{
  if (!h || n <= 0) return 0;
  pthread_mutex_lock(&g_mu);
  solver_rnti_t *r = slot_of(rnti, true, NULL);
  if (r == NULL || r->solved || (abs_slot != 0 && abs_slot == r->px_last_abs)) { pthread_mutex_unlock(&g_mu); return 0; }
  r->px_last_abs = abs_slot;
  r->dlen = dci_len;
  int hi[PHYS_K];
  for (int i = 0; i < r->px_n; i++) {   /* this grant's hypothesis of each tracked one */
    hi[i] = -1;
    for (int k = 0; k < n && hi[i] < 0; k++)
      if (h[k].scr == r->px_c[i].scr && h[k].add_pos == r->px_c[i].add_pos && ids_has(h[k].ids, h[k].n_ids, r->px_c[i].lid)) hi[i] = k;
  }
  for (int i = 0; i < r->px_n; i++) {
    if (hi[i] < 0 || !h[hi[i]].sig) continue;
    if (r->px_c[i].det < UINT16_MAX) r->px_c[i].det++;
    for (int j = 0; j < r->px_n; j++)
      if (j != i && hi[j] >= 0 && h[hi[i]].rank > h[hi[j]].rank && r->px_a[i][j] < UINT16_MAX) r->px_a[i][j]++;
  }
  /* admit the best significant hypotheses that are not tracked yet; room is made by dropping the weakest (fewest detections), never the one in front */
  for (int e = 0; e < PHYS_ADMIT; e++) {
    int best = -1;
    for (int k = 0; k < n; k++) {
      if (!h[k].sig || h[k].n_ids <= 0) continue;
      bool tracked = false;
      for (int i = 0; i < r->px_n && !tracked; i++) {
        tracked = h[k].scr == r->px_c[i].scr && h[k].add_pos == r->px_c[i].add_pos && ids_has(h[k].ids, h[k].n_ids, r->px_c[i].lid);
        /* a footprint twin of a tracked hypothesis (same scr / add_pos, exactly its rank this grant) belongs to its tie group, it is not a rival to track */
        if (!tracked && hi[i] >= 0 && h[hi[i]].sig && h[hi[i]].scr == h[k].scr && h[hi[i]].add_pos == h[k].add_pos && h[hi[i]].rank == h[k].rank) tracked = true;
      }
      if (!tracked && (best < 0 || h[k].rank > h[best].rank)) best = k;
    }
    if (best < 0) break;
    if (r->px_n == PHYS_K) {
      int w = 0;
      for (int i = 1; i < r->px_n; i++) if (r->px_c[i].det < r->px_c[w].det) w = i;
      px_drop(r, w);
      for (int i = 0; i < r->px_n; i++) {   /* the indices shifted: map again */
        hi[i] = -1;
        for (int q = 0; q < n && hi[i] < 0; q++)
          if (h[q].scr == r->px_c[i].scr && h[q].add_pos == r->px_c[i].add_pos && ids_has(h[q].ids, h[q].n_ids, r->px_c[i].lid)) hi[i] = q;
      }
    }
    const int i = r->px_n++;
    hi[i] = best;
    r->px_c[i].lid = h[best].ids[0];   /* any member names the reading: a later DCI reads this layout into the reading it belongs to there */
    r->px_c[i].scr = h[best].scr;
    r->px_c[i].add_pos = h[best].add_pos;
    r->px_c[i].det = 0;
    for (int j = 0; j <= i; j++) r->px_a[i][j] = r->px_a[j][i] = 0;   /* tracked from the next grant on: no counts yet, so it cannot lead */
  }
  int fb = 0;
  for (int i = 1; i < r->px_n; i++) if (r->px_c[i].det > r->px_c[fb].det) fb = i;
  if (r->px_n && r->px_c[fb].det > 0 && (r->px_c[fb].det % 25) == 0) {   /* diagnostic: the sign test's state (tracked hypotheses, detections, the front one's worst rival) */
    const int b = fb;
    int wa = -1, wb = -1;
    double wp = -1;
    for (int j = 0; r->px_n && j < r->px_n; j++) {
      if (j == b) continue;
      const uint64_t A = r->px_a[b][j], B = r->px_a[j][b];
      const double pv = A > B ? nbs_binom_sf(A, A + B, 0.5) : 1.0;
      if (pv > wp) { wp = pv; wa = (int)A; wb = (int)B; }
    }
    LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x phys sign test: tracked=%d front lid=%d add_pos=%u det=%u worst rival A=%d B=%d p=%.2e\n", rnti, r->px_n,
          r->px_n ? r->px_c[b].lid : -1, r->px_n ? r->px_c[b].add_pos : 0, r->px_n ? r->px_c[b].det : 0, wa, wb, wp);
  }
  int got = 0;
  for (int guard = 0; guard < PHYS_K && !incomplete; guard++) {   /* an incomplete hypothesis list (tie group possibly truncated) never decides */
    const int lead = px_leader(r);
    if (lead < 0) break;
    int k = -1;
    for (int q = 0; q < n && k < 0; q++)
      if (h[q].scr == r->px_c[lead].scr && h[q].add_pos == r->px_c[lead].add_pos && ids_has(h[q].ids, h[q].n_ids, r->px_c[lead].lid)) k = q;
    if (k < 0 || !h[k].sig) break;   /* the winner's reading is absent from this grant, or not confirmable in it: decide on a grant that has it */
    /* the tie group: every significant hypothesis of this grant with the winner's scr / add_pos and exactly its rank */
    uint16_t gid[PHYS_GROUP_MAX];
    int ng = 0, ok_id = -1, nver = 0;
    bool too_many = false, undecided = false;
    for (int q = 0; q < n && !too_many; q++) {
      if (!h[q].sig || h[q].scr != h[k].scr || h[q].add_pos != h[k].add_pos || h[q].rank != h[k].rank) continue;
      for (int z = 0; z < h[q].n_ids; z++) {
        bool dup = false;
        for (int d = 0; d < ng && !dup; d++) dup = gid[d] == h[q].ids[z];
        if (dup) continue;
        if (ng == PHYS_GROUP_MAX) { too_many = true; break; }
        gid[ng++] = h[q].ids[z];
      }
    }
    if (too_many) break;
    if (ng == 1) ok_id = gid[0];
    else {   /* footprint twins: only the HARQ/RV history of the RNTI can separate them */
      if (!g_hq_fn) break;
      for (int d = 0; d < ng; d++) {
        int hn, hb;
        hq_score(r, gid[d], &hn, &hb);
        if (nr_pdsch_solver_hq_trusted(hn, hb)) { nver++; ok_id = gid[d]; }
        else if (!nr_pdsch_solver_hq_refuted(hn, hb)) undecided = true;
      }
      if (nver != 1 || undecided) break;   /* not decisive: stay unsolved, wait for more grants */
    }
    if (e1) { uint16_t one = (uint16_t)ok_id; if (!e1(e1_ctx, &one, 1)) { px_drop(r, lead); continue; } }   /* its E1 bank refutes the class */
    got = phys_solve(r, ok_id, h[k].scr, h[k].add_pos, abs_slot, false) ? 1 : 0;
    if (!got) px_drop(r, lead);
    break;
  }
  pthread_mutex_unlock(&g_mu);
  return got;
}

static bool phys_hq_refuted(const solver_rnti_t *r)
{
  int n, bad;
  if (!g_hq_fn || r->rep < 0) return false;
  hq_score(r, r->rep, &n, &bad);
  return nr_pdsch_solver_hq_refuted(n, bad);
}
bool nr_pdsch_solver_phys_fields_ok(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  bool ok = true;
  if (r && r->solved && r->phys && r->full_ok == 0) {
    int n = 0, bad = 0;
    ok = false;
    if (g_hq_fn) { hq_score(r, r->rep, &n, &bad); ok = nr_pdsch_solver_hq_trusted(n, bad); }
  }
  pthread_mutex_unlock(&g_mu);
  return ok;
}

int nr_pdsch_solver_phys_note(uint16_t rnti, uint64_t abs_slot, const nr_pdsch_phys_hyp_t *h, int n, nr_pdsch_e1_fn e1, void *e1_ctx, uint16_t dci_len)
{
  return nr_pdsch_solver_phys_note2(rnti, abs_slot, h, n, e1, e1_ctx, dci_len, false);
}

bool nr_pdsch_solver_phys_check(uint16_t rnti, bool scorable, bool hit)
{
  if (!scorable) {   /* an unscorable grant is neither hit nor miss, but the HARQ/RV history still counts */
    pthread_mutex_lock(&g_mu);
    solver_rnti_t *r0 = slot_of(rnti, false, NULL);
    const bool ref = r0 && r0->solved && r0->phys && r0->full_ok == 0 && phys_hq_refuted(r0);
    if (ref) { r0->solved = false; r0->has_cand = false; g_st.dropped++; phys_forget(r0); }
    pthread_mutex_unlock(&g_mu);
    return ref;
  }
  pthread_mutex_lock(&g_mu);
  solver_rnti_t *r = slot_of(rnti, false, NULL);
  bool reopened = false;
  if (r && r->solved && r->phys && r->full_ok == 0 && phys_hq_refuted(r)) {
    LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x phys layout %d refuted by its HARQ/RV history: reopening\n", r->rnti, r->rep);
    r->solved = false; r->has_cand = false; g_st.dropped++; phys_forget(r); reopened = true;
  } else if (r && r->solved && r->phys && r->full_ok == 0) {
    r->ring = (uint16_t)((r->ring << 1) | (hit ? 0u : 1u));
    if (r->ring_n < PHYS_RING) r->ring_n++;
    if (r->ring_n == PHYS_RING) {
      const int miss = __builtin_popcount(r->ring);
      if (miss > 0 && binom_tail(PHYS_RING, miss, PHYS_P0, false) < NBS_SIGNIF_ALPHA) {
        LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x DM-RS score of the solved layout %d dropped (%d misses in the last %d scorable grants): reopening\n",
              r->rnti, r->rep, miss, PHYS_RING);
        r->solved = false;
        r->has_cand = false;
        g_st.dropped++;
        phys_forget(r);
        reopened = true;
      }
    }
  }
  pthread_mutex_unlock(&g_mu);
  return reopened;
}

bool nr_pdsch_solver_solution(uint16_t rnti, int32_t *scr, uint8_t *add_pos)
{
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  const bool ok = r && r->solved;
  if (ok) { if (scr) *scr = r->scr; if (add_pos) *add_pos = r->add_pos; }
  pthread_mutex_unlock(&g_mu);
  return ok;
}

bool nr_pdsch_solver_phys_tick(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  solver_rnti_t *r = slot_of(rnti, false, NULL);
  const bool t = r && r->solved && r->phys && r->full_ok == 0 && (++r->tick % 16u) == 0;
  pthread_mutex_unlock(&g_mu);
  return t;
}

int nr_pdsch_solver_families(uint16_t dci_len, nr_pdsch_family_t *out, int max)
{
  if (!out || max <= 0 || dci_len == 0) return 0;
  seed_t top[8];
  pthread_mutex_lock(&g_mu);
  const int nt = seed_tuples_f(top, max < 8 ? max : 8, dci_len);
  for (int i = 0; i < nt; i++) {
    out[i].rep = top[i].src->rep;
    out[i].add_pos = top[i].src->add_pos;
    out[i].scr = top[i].src->scr;
    out[i].members = top[i].n;
  }
  pthread_mutex_unlock(&g_mu);
  return nt;
}

bool nr_pdsch_solver_family_match(uint16_t rnti, nr_dci_bits_t payload, uint16_t len)
{
  (void)payload;   /* what the payload must satisfy is the monitor's: the family layout's reading of it, E1 and E2 */
  nr_pdsch_family_t f;
  if (nr_pdsch_solver_families(len, &f, 1) < 1) return false;
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  const bool unsolved = !r || !r->solved;
  pthread_mutex_unlock(&g_mu);
  return unsolved;
}

int nr_pdsch_solver_family_accept(uint16_t rnti, const nr_pdsch_family_t *f, const uint16_t *ids, int n_ids, int32_t scr, uint16_t dci_len, uint64_t abs_slot,
                                  nr_pdsch_e1_fn e1, void *e1_ctx)
{
  if (!f || !ids || n_ids <= 0 || !ids_has(ids, n_ids, f->rep)) return 0;
  seed_t top[8];
  pthread_mutex_lock(&g_mu);
  bool exists = false;
  const int nt = seed_tuples_f(top, 8, dci_len);
  for (int i = 0; i < nt && !exists; i++)
    exists = top[i].src->rep == f->rep && top[i].src->add_pos == f->add_pos && scr_form(top[i].src->scr) == scr_form(scr)
             && scr_form(f->scr) == scr_form(scr);
  solver_rnti_t *r = exists ? slot_of(rnti, true, NULL) : NULL;
  int ok = 0;
  uint16_t one = (uint16_t)f->rep;
  if (r && !r->solved && (!e1 || e1(e1_ctx, &one, 1))) {   /* E1 judges the family's own layout, not any layout of the reading */
    r->dlen = dci_len;
    ok = phys_solve(r, f->rep, scr, f->add_pos, abs_slot, true) ? 1 : 0;
  }
  pthread_mutex_unlock(&g_mu);
  return ok;
}

/* ---- Qm ledger ---- */
void nr_pdsch_solver_qm_note(uint16_t rnti, uint64_t abs_slot, int mcs, int qm)
{
  if (qm != 2 && qm != 4 && qm != 6 && qm != 8) return;   /* 0 = undecided: no vote */
  pthread_mutex_lock(&g_mu);
  solver_rnti_t *r = slot_of(rnti, false, NULL);
  if (r && r->solved && !r->qm_poison && (abs_slot == 0 || abs_slot != r->qm_last)) {
    int q[3] = {0, 0, 0};
    bool valid[3];
    int nvalid = 0, distinct = 0, match = 0;
    for (int t = 0; t < 3; t++) {
      int rr;
      valid[t] = nr_pdsch_solver_mcs_qr(t, mcs, &q[t], &rr);
      if (!valid[t]) continue;
      nvalid++;
      match += q[t] == qm;
      bool seen = false;
      for (int u = 0; u < t && !seen; u++) seen = valid[u] && q[u] == q[t];
      distinct += !seen;
    }
    if (nvalid > 0 && match == 0) {
      r->qm_poison = true;   /* a confident Qm that no table gives for this MCS: the classifier or the layout is wrong for this RNTI */
    } else if (distinct > 1) {   /* informative: the tables disagree about this MCS */
      r->qm_last = abs_slot;
      for (int t = 0; t < 3; t++) {
        if (!valid[t]) continue;
        uint16_t *c = (q[t] == qm) ? &r->qm_agree[t] : &r->qm_dis[t];
        if (*c < UINT16_MAX) (*c)++;
      }
    }
  }
  pthread_mutex_unlock(&g_mu);
}

static int qm_table_locked(const solver_rnti_t *r)
{
  if (!r->solved || r->qm_poison) return -1;
  int win = -1;
  for (int t = 0; t < 3; t++)
    if (r->qm_dis[t] == 0 && r->qm_agree[t] >= QM_MIN) {
      if (win >= 0) return -1;   /* two tables fit every vote: not decided */
      win = t;
    }
  if (win < 0) return -1;
  for (int t = 0; t < 3; t++)
    if (t != win && r->qm_dis[t] == 0) return -1;   /* a table the data never contradicted is still open */
  return win;
}

int nr_pdsch_solver_qm_table(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  const int t = r ? qm_table_locked(r) : -1;
  pthread_mutex_unlock(&g_mu);
  return t;
}

int nr_pdsch_solver_table_evidence(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  int ev = 0;
  if (r && r->solved && r->table >= 0) ev = qm_table_locked(r) == r->table ? 2 : 1;
  pthread_mutex_unlock(&g_mu);
  return ev;
}

void nr_pdsch_solver_reopen(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  solver_rnti_t *r = slot_of(rnti, false, NULL);
  if (r != NULL && r->solved) {
    r->solved = false; r->has_cand = false; g_st.dropped++;
  }
  if (r != NULL) phys_forget(r);
  pthread_mutex_unlock(&g_mu);
}
nr_layout_state_t nr_pdsch_solver_layout_state(uint16_t rnti, uint32_t *checks_ok, uint32_t *checks_fail)
{
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  if (checks_ok) *checks_ok = r ? r->full_ok : 0;
  if (checks_fail) *checks_fail = r ? r->full_tr - r->full_ok : 0;
  const nr_layout_state_t ls = r && r->solved ? NR_LAYOUT_SOLVED : r && r->has_cand ? NR_LAYOUT_CANDIDATE : NR_LAYOUT_UNSOLVED;
  pthread_mutex_unlock(&g_mu);
  return ls;
}
bool nr_pdsch_solver_take_hrv_alert(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  solver_rnti_t *r = slot_of(rnti, false, NULL);
  const bool a = r && r->hrv_alert;
  if (r) r->hrv_alert = false;
  pthread_mutex_unlock(&g_mu);
  return a;
}

