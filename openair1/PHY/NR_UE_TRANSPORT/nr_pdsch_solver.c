/* Blind dedicated-PDSCH grant solver (T26b). See nr_pdsch_solver.h. */
#include "nr_pdsch_solver.h"
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
} solver_plan_ctx_t;
static solver_plan_ctx_t g_plan_ctx = {.key = -1}; /* legacy path: process-global grant context */
static _Thread_local solver_plan_ctx_t t_plan_ctx = {.key = -1}; /* ISAC_PDSCH_SOLVER_THREAD_CTX: per scan consumer */
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
/* cell prior: the first solution, offered first to other RNTIs */
static bool g_have_prior;
static int g_prior_rep;
static uint8_t g_prior_add_pos;
static int8_t g_prior_table, g_prior_S_by_tda[16];

bool nr_pdsch_solver_enabled(void)
{
  static int en = -1;
  if (en < 0) {
    const char *e = getenv("ISAC_PDSCH_SOLVER");
    en = (e && atoi(e) != 0) ? 1 : 0;
  }
  return en == 1;
}

void nr_pdsch_solver_set_tda_fn(nr_pdsch_solver_tda_fn fn, void *ctx)
{
  pthread_mutex_lock(&g_mu);
  g_tda_fn = fn;
  g_tda_ctx = ctx;
  pthread_mutex_unlock(&g_mu);
}

void nr_pdsch_solver_reset(void)
{
  pthread_mutex_lock(&g_mu);
  g_nl_pref_gap = -1;
  g_coverage = -1;
  g_layout_recover = -1;
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

bool nr_pdsch_solver_mcs_qr(int table, int mcs, int *qm, int *r2)
{
  if (table < 0 || table > 2 || mcs < 0 || mcs >= kQRn[table])
    return false;
  *qm = kQR[table][mcs][0];
  *r2 = kQR[table][mcs][1];
  return true;
}

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

/* tables whose (Qm, R) differ for this MCS, priority 1 (qam256), 0, 2 */
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
  return NULL;
}

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

int nr_pdsch_solver_rep(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  const int rep = (r && r->solved) ? r->rep : -1;
  pthread_mutex_unlock(&g_mu);
  return rep;
}

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

bool nr_pdsch_solver_snr(uint16_t rnti, float *snr_db)
{
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  const bool ok = r && r->n_snr >= 3;
  if (ok) *snr_db = r->snr_ema;
  pthread_mutex_unlock(&g_mu);
  return ok;
}

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
  if (!solver_thread_ctx_enabled()) pthread_mutex_unlock(&g_mu);
}

void nr_pdsch_solver_set_now(uint64_t abs_slot)
{
  if (!solver_thread_ctx_enabled()) pthread_mutex_lock(&g_mu);
  plan_ctx()->now_abs = abs_slot;
  if (!solver_thread_ctx_enabled()) pthread_mutex_unlock(&g_mu);
}

bool nr_pdsch_solver_owns(uint16_t rnti)
{
  return nr_pdsch_solver_enabled();
}

static int tda_of(int layout, nr_dci_bits_t p)
{
  return g_tda_fn ? (g_tda_fn(g_tda_ctx, (uint16_t)layout, p) & 15) : 0;
}

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
static int comb_on(void)
{
  static int en = -1;
  if (en < 0) en = getenv("ISAC_RX_MRC_PER_RNTI") ? atoi(getenv("ISAC_RX_MRC_PER_RNTI")) : 0;
  return en;
}
/* Mode for this grant: unsolved -> alternate 1/3 by grant parity; solved -> the mode with the better CRC rate
 * (Laplace), exploring the other on 1 grant in 8. */
/* receivers in play: ISAC_RX_MRC_PER_RNTI=1 -> {1,3}; =2 -> {1,3,4,5} (4 = noise-weighted gain-corrected MRC, 5 = IRC) */
/* =3 -> {1,3,4,6} (6 = IRC with DM-RS residual covariance, ISAC_RX_IRC=4) */
static uint8_t comb_k(int k) { static const uint8_t kComb[4] = {1, 3, 4, 5}; return (k == 3 && comb_on() >= 3) ? 6 : kComb[k]; }
static int comb_n(void) { return comb_on() >= 2 ? 4 : 2; }
static int comb_ab(void) /* ISAC_RX_MRC_PER_RNTI_AB=1: post-solve round robin over the receivers (paired A/B, no exploitation) */
{
  static int en = -1;
  if (en < 0) en = getenv("ISAC_RX_MRC_PER_RNTI_AB") ? atoi(getenv("ISAC_RX_MRC_PER_RNTI_AB")) : 0;
  return en;
}
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

/* ISAC_PDSCH_SOLVER_PREF=<N> (T26d, default 0 = off): an unsolved RNTI searches on its low-MCS grants (MCS <=
 * ISAC_PDSCH_SOLVER_PREF_MCS, default 4: QPSK in every MCS table) and skips the others, unless N grants went by without
 * a preferred one. Measured: probe CRC on the true hypothesis ~4-5 % because most grants are QAM / multi-layer, which
 * almost never pass at this receiver; QPSK grants pass 25-90 %. Layer count is not known at plan time (readings carry
 * no ports), so only the MCS is used. */
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

/* Prefer readings that can actually decode at the measured receiver SNR: one layer and QPSK. Keep the old
 * MCS-only preference independent, and use its grant-gap fallback when no preferred reading is available. */
static int nl_pref_gap(void)
{
  if (g_nl_pref_gap < 0)
    g_nl_pref_gap = getenv("ISAC_PDSCH_SOLVER_NL_PREF") ? atoi(getenv("ISAC_PDSCH_SOLVER_NL_PREF")) : 0;
  return g_nl_pref_gap;
}

static bool reading_nl_qpsk(const nr_pdsch_solver_reading_t *rd)
{
  if (rd->n_layers != 1) return false;
  for (int table = 0; table < 3; table++) {
    int qm, r2;
    if (nr_pdsch_solver_mcs_qr(table, rd->mcs, &qm, &r2) && qm == 2) return true;
  }
  return false;
}

static bool search_backoff(const solver_rnti_t *r)
{
  static int lim = -1;
  if (lim < 0) lim = getenv("ISAC_PDSCH_SOLVER_BACKOFF") ? atoi(getenv("ISAC_PDSCH_SOLVER_BACKOFF")) : 0;
  return lim > 0 && !r->has_cand && r->n_probe >= (uint32_t)lim && (r->grants % 8u) != 0;
}

static const uint8_t kS[4] = {1, 0, 2, 3};

static bool has_id(const nr_pdsch_solver_reading_t *r, int id)
{
  for (int i = 0; i < r->n_ids; i++)
    if (r->ids[i] == id) return true;
  return false;
}

static coverage_entry_t *coverage_find(solver_coverage_t *c, uint64_t key)
{
  uint64_t h = key * UINT64_C(0x9e3779b97f4a7c15);
  h = (h ^ (h >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  size_t i = (h ^ (h >> 27)) & (c->cap - 1);
  while (c->entries[i].key && c->entries[i].key != key) i = (i + 1) & (c->cap - 1);
  return &c->entries[i];
}

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

static uint64_t coverage_key(int id, int variant, nr_dci_bits_t payload)
{
  const int tda = variant == 63 ? 0 : tda_of(id, payload); /* 63 = all timing cases' trial count */
  return (((((uint64_t)(t_scr + 1) << 15) | (unsigned)id) << 4 | tda) << 6 | variant) + 1;
}

/* A DCI changes the number/order of readings and which tables are eligible.
 * An index modulo that list can repeatedly skip the same physical hypothesis.
 * Count actual trials by (layout, TDRA, identity, S, add-pos, table). Ineligible
 * combinations remain untested; a high-MCS grant cannot consume a later QPSK
 * grant's table/start trial. Grouping identical readings still shares each probe. */
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

/* known-parameter variants for one reading (solved RNTI, or the cell prior on an unsolved one) */
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

int nr_pdsch_solver_plan(uint16_t rnti, nr_dci_bits_t payload, uint16_t len, uint8_t mcs, int l0,
                         const nr_pdsch_solver_reading_t *rd, int n_rd, int budget, nr_pdsch_solver_var_t *out)
{
  return nr_pdsch_solver_plan_slot(rnti, payload, len, mcs, l0, false, rd, n_rd, budget, out);
}

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
    /* no search on a slot carrying discovered CSI-RS: its rate matching is unknown, a miss would be false evidence */
  } else if (!r->has_cand && !leader_ok(r)) {
    /* T26d ISAC_PDSCH_SOLVER_LEADER=1: until some RNTI of this CORESET solved, only its busiest unsolved RNTI searches --
     * spreading the probe budget over ~15 RNTIs left whole runs without a first solve (replays: ~40 % of runs); the
     * first solution then seeds every other RNTI through the reuse prior. */
  } else if (probe_hog(r)) {
    /* T26d: fair share of the probe budget -- an unsolved RNTI that already spent far more probes than the average
     * unsolved RNTI waits (a busy undecodable population starved the serving cell's RNTIs: replay f3 0x8f5a unsolved) */
  } else if (nl_skip || (nl_gap <= 0 && search_pref_skip(r, mcs))) {
    /* Wait for a one-layer QPSK reading (NL_PREF), or a low-MCS grant under the legacy MCS-only preference. */
  } else if (!(nl_gap > 0 && nl_preferred) && search_backoff(r)) {
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
      for (int q = 0; q < g_n_key_scr && !known; q++) /* preference, not exclusive: 1 block in 4 still explores */
        if (g_key_scr[q].key == ctx->key && ((r->grants / 16u) % 4u) != 3u) { t_scr = g_key_scr[q].scr; known = 1; }
      if (!known) { /* T26d: the default ids alone for the first 48 grants -- cycling 6+ alternatives from the start made
                     * the serving population's search 6x slower (replay e9/e10: 0x8f5a unsolved) */
        if (ctx->decided) { /* the decided id on 3 grants of 4, the default on the 4th */
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
    static int s_reuse = -1;
    if (s_reuse < 0) s_reuse = getenv("ISAC_PDSCH_SOLVER_REUSE") && atoi(getenv("ISAC_PDSCH_SOLVER_REUSE")) ? 1 : 0;
    const solver_rnti_t *best = NULL;
    if (s_reuse)
      for (int k = 0; k < SOLVER_RNTIS; k++)
        if (g_r[k].used && g_r[k].solved && g_r[k].rep >= 0 && (!best || g_r[k].full_ok > best->full_ok))
          best = &g_r[k];
    if (best) {
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
    if (out[i].probe && !r->solved) r->n_probe++;
  }
  pthread_mutex_unlock(&g_mu);
  return n;
}

static int lowest(const uint8_t *bm)
{
  for (int b = 0; b < BM_BYTES; b++)
    if (bm[b])
      return b * 8 + __builtin_ctz(bm[b]);
  return -1;
}

/* Lowest layout of the candidate set whose TDA reading maps every CRC-passed (payload, S) pair consistently; fills
 * S_by_tda from the evidence. Layouts with a different TDA width read the same DCI as different indices, and only the
 * true width makes "S is a function of the TDA index" hold. */
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
static void add_ev(solver_rnti_t *r, nr_dci_bits_t p, uint8_t S)
{
  if (r->n_ev < 16) { r->ev_p[r->n_ev] = p; r->ev_S[r->n_ev++] = S; }
  else { memmove(r->ev_p, r->ev_p + 1, 15 * sizeof(r->ev_p[0])); memmove(r->ev_S, r->ev_S + 1, 15); r->ev_p[15] = p; r->ev_S[15] = S; }
}

static int has_id_p(const pending_t *p, int id)
{
  for (int i = 0; i < p->n_ids; i++)
    if (p->ids[i] == id) return 1;
  return 0;
}
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
  g_st.candidates++;
  LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x CANDIDATE: CRC pass S=%u add_pos=%u table=%d on %d layouts\n",
        r->rnti, p->S, p->add_pos, r->cand_table, p->n_ids);
}

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
  if (p == NULL || !credit) {
    if (p) p->used = false;
    pthread_mutex_unlock(&g_mu);
    return;
  }
  solver_rnti_t *r = &g_r[p->slot];
  if (r->used && p->phase == 0 && !r->solved && !crc_ok && r->layout_failures < UINT32_MAX)
    r->layout_failures++;
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
        if (r->cand_tries >= 16)
          make_cand(r, p);
      } else {
        memcpy(r->cand_bm, nb, BM_BYTES);
        r->solved = true;
        r->n_ev = 0;
        add_ev(r, r->cand_payload, r->cand_S);
        add_ev(r, p->payload, p->S);
        memset(r->S_by_tda, -1, sizeof(r->S_by_tda));
        r->rep = choose_rep(r);
        if (r->rep < 0) r->rep = lowest(r->cand_bm);
        r->add_pos = p->add_pos;
        r->scr = p->scr;
        { /* T26c: remember this CORESET's scrambling for its other RNTIs */
          int have = 0;
          for (int q = 0; q < g_n_key_scr && !have; q++) have = g_key_scr[q].key == r->key;
          if (!have && g_n_key_scr < 16) { g_key_scr[g_n_key_scr].key = r->key; g_key_scr[g_n_key_scr++].scr = r->scr; }
        }
        r->table = r->cand_table >= 0 ? r->cand_table : (p->table_disc ? (int8_t)p->table : -1);
        g_st.solved++;
        r->solved_abs = p->abs;
        memset(r->comb_try, 0, sizeof(r->comb_try)); /* receiver choice from post-solve decodes only (pre-solve probes */
        memset(r->comb_ok, 0, sizeof(r->comb_ok));   /*  fail for every receiver and biased the choice) */
        LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x SOLVED by 2 independent CRC passes: layout %d (%d consistent) "
                   "add_pos=%u table=%d S[tda %d]=%u S[tda %d]=%u scr=%d\n",
              r->rnti, r->rep, kept, r->add_pos, r->table, tda_of(r->rep, r->cand_payload), r->cand_S,
              tda_of(r->rep, p->payload), p->S, (int)r->scr);
        if (!g_have_prior) {
          g_have_prior = true;
          g_prior_rep = r->rep;
          g_prior_add_pos = r->add_pos;
          g_prior_table = r->table;
          memcpy(g_prior_S_by_tda, r->S_by_tda, sizeof(g_prior_S_by_tda));
        }
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

void nr_pdsch_solver_stats(nr_pdsch_solver_stats_t *s)
{
  pthread_mutex_lock(&g_mu);
  *s = g_st;
  pthread_mutex_unlock(&g_mu);
}
