/* Blind dedicated-PDSCH grant solver (T26b). See nr_pdsch_solver.h. */
#include "nr_pdsch_solver.h"
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "common/utils/LOG/log.h"

#define SOLVER_RNTIS 16
#define SOLVER_PENDING 4096 /* FIFO-overwritten: probes dropped stale in the queue never report back */
#define SOLVER_MAX_IDS 1024
#define SOLVER_SEARCH_MAX_RB 48   /* search probes only readings this narrow (cost; the small grants decode best) */
#define SOLVER_CAND_TRIES 96      /* confirmation probes before an unconfirmed candidate is dropped */
#define BM_BYTES (NR_PDSCH_SOLVER_MAX_LAYOUTS / 8)

typedef struct {
  bool used, solved, has_cand;
  uint16_t rnti;
  uint32_t cursor;
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
  nr_dci_bits_t payload;
  int n_ids;
  uint16_t ids[SOLVER_MAX_IDS];
} pending_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static solver_rnti_t g_r[SOLVER_RNTIS];
static pending_t g_p[SOLVER_PENDING];
static uint32_t g_token = 1;
static uint64_t g_dci_seq;
static nr_pdsch_solver_tda_fn g_tda_fn;
static void *g_tda_ctx;
static nr_pdsch_solver_stats_t g_st;
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
  for (int i = 0; i < SOLVER_RNTIS; i++) {
    free(g_r[i].cand_bm);
    memset(&g_r[i], 0, sizeof(g_r[i]));
  }
  memset(g_p, 0, sizeof(g_p));
  memset(&g_st, 0, sizeof(g_st));
  g_have_prior = false;
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
      memset(g_r[i].S_by_tda, -1, sizeof(g_r[i].S_by_tda));
      if (idx) *idx = i;
      return &g_r[i];
    }
  return NULL;
}

int nr_pdsch_solver_rep(uint16_t rnti)
{
  pthread_mutex_lock(&g_mu);
  const solver_rnti_t *r = slot_of(rnti, false, NULL);
  const int rep = (r && r->solved) ? r->rep : -1;
  pthread_mutex_unlock(&g_mu);
  return rep;
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
static bool emit(nr_pdsch_solver_var_t *out, int *n, int max, int slot, uint8_t phase, int reading,
                 const nr_pdsch_solver_reading_t *rd, int l0, int S, int add_pos, int table, bool probe, bool table_disc,
                 bool learn_S, bool learn_table, int tda, uint64_t dci, nr_dci_bits_t payload)
{
  if (*n >= max)
    return false;
  if (S > l0) /* type A: the first DM-RS symbol l0 must lie inside the PDSCH (S <= l0); else the demapper overruns */
    return true;
  for (int i = 0; i < *n; i++) /* never the same decode twice for one grant */
    if (out[i].reading == reading && out[i].S == S && out[i].add_pos == add_pos && out[i].table == table && out[i].rm == t_rm)
      return true;
  pending_t *p = pend_new();
  memset(p, 0, offsetof(pending_t, ids));
  p->used = true;
  p->token = g_token++;
  if (g_token == 0) g_token = 1;
  p->slot = slot;
  p->phase = phase;
  p->S = (uint8_t)S; p->add_pos = (uint8_t)add_pos; p->table = (uint8_t)table; p->probe = probe;
  p->table_disc = table_disc; p->learn_S = learn_S; p->learn_table = learn_table; p->tda = tda;
  p->dci = dci;
  p->rm = (uint8_t)t_rm;
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
  return true;
}

static const uint8_t kS[4] = {1, 0, 2, 3};

static bool has_id(const nr_pdsch_solver_reading_t *r, int id)
{
  for (int i = 0; i < r->n_ids; i++)
    if (r->ids[i] == id) return true;
  return false;
}

/* known-parameter variants for one reading (solved RNTI, or the cell prior on an unsolved one) */
static void plan_known(nr_pdsch_solver_var_t *out, int *n, int max, int slot, uint8_t phase, int reading,
                       const nr_pdsch_solver_reading_t *rd, int l0, int rep, uint8_t add_pos, int8_t table,
                       const int8_t *S_by_tda, int mcs, uint64_t dci, nr_dci_bits_t payload)
{
  uint8_t tl[3];
  const int nt = tables_for(mcs, tl);
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
  int slot = -1, n = 0;
  solver_rnti_t *r = slot_of(rnti, true, &slot);
  if (r == NULL) {
    pthread_mutex_unlock(&g_mu);
    return 0;
  }
  const uint64_t dci = ++g_dci_seq;
  g_st.planned++;
  if ((g_st.planned % 500) == 0)
    LOG_A(PHY, "SENSING: PDSCH_SOLVER stats grants=%llu probes=%llu probe_ok=%llu fulls=%llu full_ok=%llu candidates=%llu "
               "solved=%llu dropped=%llu\n", (unsigned long long)g_st.planned, (unsigned long long)g_st.probes,
          (unsigned long long)g_st.probe_ok, (unsigned long long)g_st.fulls, (unsigned long long)g_st.full_ok,
          (unsigned long long)g_st.candidates, (unsigned long long)g_st.solved, (unsigned long long)g_st.dropped);
  t_rm = 0;
  t_learn_rm = false;
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
      const int nt = tables_for(mcs, tl);
      if (nt == 0) break;
      emit(out, &n, budget, slot, 1, i, rd, l0, S_here >= 0 ? S_here : 1, r->add_pos, r->table >= 0 ? r->table : tl[0],
           true, nt > 1, false, false, tda_of(r->rep, payload), dci, payload);
      refine++;
    }
  } else if (csirs_slot) {
    /* no search on a slot carrying discovered CSI-RS: its rate matching is unknown, a miss would be false evidence */
  } else {
    /* 1. confirm a candidate: the reading sharing most layouts with it */
    if (r->has_cand) {
      int best = -1, best_ov = 0;
      for (int i = 0; i < n_rd; i++) {
        int ov = 0;
        for (int k = 0; k < rd[i].n_ids; k++)
          ov += (r->cand_bm[rd[i].ids[k] >> 3] >> (rd[i].ids[k] & 7)) & 1;
        if (ov > best_ov) { best_ov = ov; best = i; }
      }
      if (best >= 0 && rd[best].num_rb <= SOLVER_SEARCH_MAX_RB) {
        uint8_t tl[3];
        const int nt = tables_for(mcs, tl);
        for (int si = 0; si < 4; si++)
          for (int ti = 0; ti < (r->cand_table >= 0 ? 1 : nt); ti++)
            emit(out, &n, budget, slot, 0, best, rd, l0, kS[si], r->cand_add_pos,
                 r->cand_table >= 0 ? r->cand_table : tl[ti], true, nt > 1, false, false, -1, dci, payload);
      }
    }
    /* 2. the cell prior (another RNTI's solution), first */
    if (g_have_prior) {
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
      if (rd[i].num_rb > 0 && rd[i].num_rb <= SOLVER_SEARCH_MAX_RB) order[no++] = i;
    for (int a = 1; a < no; a++)
      for (int b = a; b > 0 && rd[order[b]].n_ids > rd[order[b - 1]].n_ids; b--) {
        const int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
      }
    uint8_t tl[3];
    const int nt = tables_for(mcs, tl);
    if (no > 0 && nt > 0) {
      /* add_pos tier outer (1, 2, 0, 3); inside a tier every reading gets all S in one go, because the true S
       * changes from grant to grant with the TDA index while the true reading is there on every grant */
      static const uint8_t kA[4] = {1, 2, 0, 3};
      const uint32_t total = (uint32_t)(4 * no * 4 * nt);
      for (uint32_t k = 0; k < total && n < budget; k++) {
        const uint32_t idx = r->cursor++ % total;
        const int a = (int)(idx / (uint32_t)(no * 4 * nt)), ri = order[(idx / (uint32_t)(4 * nt)) % (uint32_t)no],
                  si = (int)((idx / (uint32_t)nt) % 4u), tt = tl[idx % (uint32_t)nt];
        emit(out, &n, budget, slot, 0, ri, rd, l0, kS[si], kA[a], tt, true, nt > 1, false, false, -1, dci, payload);
      }
    }
  }
  for (int i = 0; i < n; i++)
    out[i].probe ? g_st.probes++ : g_st.fulls++;
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
  for (int k = 0; k < SOLVER_PENDING; k++)
    if (g_p[k].used && g_p[k].token == token) { p = &g_p[k]; break; }
  if (p == NULL || !credit) {
    if (p) p->used = false;
    pthread_mutex_unlock(&g_mu);
    return;
  }
  solver_rnti_t *r = &g_r[p->slot];
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
      if (kept == 0 || p->add_pos != r->cand_add_pos) {
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
        r->table = r->cand_table >= 0 ? r->cand_table : (p->table_disc ? (int8_t)p->table : -1);
        g_st.solved++;
        LOG_A(PHY, "SENSING: PDSCH_SOLVER rnti=0x%04x SOLVED by 2 independent CRC passes: layout %d (%d consistent) "
                   "add_pos=%u table=%d S[tda %d]=%u S[tda %d]=%u\n",
              r->rnti, r->rep, kept, r->add_pos, r->table, tda_of(r->rep, r->cand_payload), r->cand_S,
              tda_of(r->rep, p->payload), p->S);
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
