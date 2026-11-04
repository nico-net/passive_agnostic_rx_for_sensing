/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_dci11_prove.h"
#include "common/utils/nr_cell.h"
#include "nr_blind_stats.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <string.h>

#define NSW_EXT 6          /* ONESHOT .. ENHT3 in nr_dci11_switch_t order */
#define C_MAX 48           /* offsets c = K0 + K1 tested: 0..47 (K0 <= 32 and K1 <= 15 in TS 38.331) */
#define KEYS 128           /* (TDRA index <= 15) x (K1 index <= 7) */
#define C_ALL ((UINT64_C(1) << C_MAX) - 1)

typedef struct { uint8_t tb2, harq, dai, tpc2, k1, rest; } core_t;

static const uint8_t *tab(nr_dci11_switch_t w, int *n) { return nr_dci11_switch_widths(w, n); }

/* bit s set: the six extras can sum to s */
static uint64_t ext_sums(void)
{
  uint64_t ex = 1;
  for (int i = NR_DCI11_SW_ONESHOT; i <= NR_DCI11_SW_ENHT3; i++) {
    int m;
    const uint8_t *w = tab((nr_dci11_switch_t)i, &m);
    uint64_t nx = 0;
    for (int j = 0; j < m; j++) nx |= ex << w[j];
    ex = nx;
  }
  return ex;
}

/* every (tb2, harq, dai, tpc2, k1) whose extras can fill the rest; these five fix every extracted field */
static int enum_cores(const nr_dci11_layout_t *l, core_t *c, int max)
{
  int n[5], cnt = 0;
  const uint8_t *t[5];
  for (int i = 0; i < 5; i++) t[i] = tab((nr_dci11_switch_t)i, &n[i]);   /* TB2, HARQ, DAI, TPC2, P2H */
  const uint64_t ex = ext_sums();
  for (int a = 0; a < n[0]; a++)
    for (int b = 0; b < n[1]; b++)
      for (int d = 0; d < n[2]; d++)
        for (int e = 0; e < n[3]; e++)
          for (int k = 0; k < n[4]; k++) {
            const int rest = l->pre_ant - (t[0][a] + t[1][b] + t[2][d] + 2 /*TPC*/ + t[3][e] + 3 /*PRI*/ + t[4][k]);
            if (rest < 0 || rest >= 64 || !((ex >> rest) & 1)) continue;
            if (cnt < max) c[cnt] = (core_t){t[0][a], t[1][b], t[2][d], t[3][e], t[4][k], (uint8_t)rest};
            cnt++;
          }
  return cnt;
}

/* extras: first decomposition of `rest` (switch order, widths ascending), or all of them through `emit` */
typedef struct { nr_dci11_split_t base; nr_dci11_split_t *out; int max, count; bool first_only; } ext_ctx_t;
static bool ext_walk(ext_ctx_t *x, int i, int rest)
{
  if (i == NSW_EXT) {
    if (rest != 0) return false;
    if (x->count < x->max) x->out[x->count] = x->base;
    x->count++;
    return x->first_only;
  }
  int m;
  const uint8_t *w = tab((nr_dci11_switch_t)(NR_DCI11_SW_ONESHOT + i), &m);
  for (int j = 0; j < m; j++) {
    if (w[j] > rest) continue;
    x->base.ext[i] = w[j];
    if (ext_walk(x, i + 1, rest - w[j])) return true;
  }
  x->base.ext[i] = 0;
  return false;
}

static nr_dci11_split_t core_split(const core_t *c)
{
  nr_dci11_split_t s;
  memset(&s, 0, sizeof s);
  s.tb2 = c->tb2; s.harq = c->harq; s.dai = c->dai; s.tpc = 2; s.tpc2 = c->tpc2; s.pri = 3; s.k1 = c->k1;
  return s;
}

int nr_dci11_splits(const nr_dci11_layout_t *l, nr_dci11_split_t *out, int max)
{
  if (!l || (max > 0 && !out)) return -1;
  core_t cores[NR_DCI11_CORE_MAX];
  int nc = enum_cores(l, cores, NR_DCI11_CORE_MAX);
  if (nc > NR_DCI11_CORE_MAX) nc = NR_DCI11_CORE_MAX;
  ext_ctx_t x = {.out = out, .max = max < 0 ? 0 : max, .count = 0, .first_only = false};
  for (int i = 0; i < nc; i++) {
    x.base = core_split(&cores[i]);
    ext_walk(&x, 0, cores[i].rest);
  }
  return x.count;
}

/* ---- a bank seen in slot order ---- */
typedef struct { int m; const nr_dci11_obs_t *o[NR_DCI11_BANK]; int64_t slot[NR_DCI11_BANK]; } view_t;

/* the last NR_DCI11_BANK entries of the right length, sorted by slot (stable), one per slot */
static void make_view(view_t *v, const nr_dci11_offsets_t *o, const nr_dci11_obs_t *bank, int n)
{
  v->m = 0;
  for (int i = n > NR_DCI11_BANK ? n - NR_DCI11_BANK : 0; i < n; i++) {
    if (bank[i].len != o->total || bank[i].len > NR_DCI_MAX_PAYLOAD) continue;
    int j = v->m++;
    while (j > 0 && v->slot[j - 1] > bank[i].abs_slot) { v->o[j] = v->o[j - 1]; v->slot[j] = v->slot[j - 1]; j--; }
    v->o[j] = &bank[i];
    v->slot[j] = bank[i].abs_slot;
  }
  int w = 0;
  for (int i = 0; i < v->m; i++)
    if (i == 0 || v->slot[i] != v->slot[i - 1]) { v->o[w] = v->o[i]; v->slot[w] = v->slot[i]; w++; }
  v->m = w;
}

static int rd(const nr_dci11_obs_t *b, int pos, int w) { return (int)nr_dci_bits_field(&b->payload, b->len, pos, w); }

/* UL-capable offset mask of every DCI: bit c = slot + c is UL (or MIXED). false when the pattern is unknown. */
static bool ul_masks(const view_t *v, nr_ul_slot_fn ul, uint64_t *ulm)
{
  if (!ul) return false;
  for (int i = 0; i < v->m; i++) {
    ulm[i] = 0;
    for (int c = 0; c < C_MAX; c++) {
      int known = 0;
      const bool u = ul(v->slot[i] + c, &known);
      if (!known) return false;
      if (u) ulm[i] |= UINT64_C(1) << c;
    }
  }
  return true;
}

/* K1 OUTLIER RATE. The true table explains every genuine PDSCH grant, but the bank can hold DCIs that follow no table: a PDCCH CRC false alarm
 * accepted under a live RNTI (CRC16: about 2^-16 per candidate, thousands of candidates per second), an SPS release/deactivation DCI 1_1 (its K1
 * counts from the PDCCH slot), an RNTI reused by another UE (its K1 list differs). 1 % is a general upper bound for these together, not a cell
 * parameter. A split is refuted only when more DCIs than that are infeasible, significantly (binomial tail < alpha). */
#define K1_OUTLIER_P 0.01

/* per-key best offsets of one core: for each (TDRA, K1) key the offsets c that put the MOST of its DCIs on UL-capable slots (feas), key_of[i] = key of
 * DCI i. Returns the number of DCIs no single offset per key explains. */
static int k1_sets(const view_t *v, const nr_dci11_offsets_t *o, int base, const core_t *c, const uint64_t *ulm, uint64_t *feas, bool *used,
                   int *key_of)
{
  const int kpos = base + c->tb2 + c->harq + c->dai + 2 + c->tpc2 + 3;
  uint8_t cnt[KEYS][C_MAX];
  int nk[KEYS];
  memset(cnt, 0, sizeof cnt);
  memset(nk, 0, sizeof nk);
  for (int k = 0; k < KEYS; k++) used[k] = false;
  for (int i = 0; i < v->m; i++) {
    const int tda = o->tda_bits ? rd(v->o[i], o->tda, o->tda_bits) : 0;
    const int idx = c->k1 ? rd(v->o[i], kpos, c->k1) : 0;
    const int key = (tda & 15) * 8 + (idx & 7);
    key_of[i] = key;
    used[key] = true;
    nk[key]++;
    for (int b = 0; b < C_MAX; b++) cnt[key][b] += (uint8_t)((ulm[i] >> b) & 1);
  }
  int bad = 0;
  for (int k = 0; k < KEYS; k++) {
    feas[k] = C_ALL;
    if (!used[k]) continue;
    int best = 0;
    for (int b = 0; b < C_MAX; b++) best = cnt[k][b] > best ? cnt[k][b] : best;
    feas[k] = 0;
    for (int b = 0; b < C_MAX; b++) if (cnt[k][b] == best) feas[k] |= UINT64_C(1) << b;
    bad += nk[k] - best;
  }
  return bad;
}

typedef struct { int kpos, w, bad; uint64_t feas[KEYS]; int key_of[NR_DCI11_BANK]; } k1m_t;
#define K1_MEMO 24

static bool k1_refuted(int bad, int m, double alpha) { return bad > 0 && nbs_binom_sf((uint64_t)bad, (uint64_t)m, K1_OUTLIER_P) < alpha; }

uint64_t nr_dci11_k1_feasible(const nr_dci11_layout_t *l, const nr_dci11_offsets_t *o, const nr_dci11_obs_t *bank, int n, nr_ul_slot_fn ul,
                              const nr_dci11_split_t *s, int tda_idx, int k1_idx)
{
  if (!l || !o || !s || (n > 0 && !bank) || o->ant_ports < l->pre_ant || o->tda_bits > 4 || tda_idx < 0 || tda_idx > 15 || k1_idx < 0 || k1_idx > 7)
    return UINT64_MAX;
  view_t v;
  make_view(&v, o, bank, n);
  uint64_t ulm[NR_DCI11_BANK], feas[KEYS];
  bool used[KEYS];
  int key_of[NR_DCI11_BANK];
  if (!ul_masks(&v, ul, ulm)) return UINT64_MAX;
  const core_t c = {s->tb2, s->harq, s->dai, s->tpc2, s->k1, 0};
  const int bad = k1_sets(&v, o, o->ant_ports - l->pre_ant, &c, ulm, feas, used, key_of);
  if (k1_refuted(bad, v.m, NBS_SIGNIF_ALPHA)) return 0;
  const int key = tda_idx * 8 + k1_idx;
  return used[key] ? feas[key] : UINT64_MAX;
}

typedef struct { int m; bool have, known; int64_t slot[NR_DCI11_BANK]; uint64_t ulm[NR_DCI11_BANK]; } ulctx_t;

static int prove_impl(const nr_dci11_layout_t *l, const nr_dci11_offsets_t *o, const nr_dci11_obs_t *bank, int n, nr_ul_slot_fn ul,
                      nr_dci11_split_t *alive, double *chance_p, int max, ulctx_t *cx)
{
  if (!l || !o || !alive || !chance_p || max <= 0 || (n > 0 && !bank) || o->ant_ports < l->pre_ant || o->total > NR_DCI_MAX_PAYLOAD) return -1;
  core_t cores[NR_DCI11_CORE_MAX];
  const int nc = enum_cores(l, cores, NR_DCI11_CORE_MAX);
  if (nc > NR_DCI11_CORE_MAX) return -1;
  const int base = o->ant_ports - l->pre_ant;
  view_t v;
  make_view(&v, o, bank, n);

  uint64_t ulm_local[NR_DCI11_BANK];
  uint64_t *ulm = ulm_local;
  bool k1_known;
  if (o->tda_bits > 4 || v.m == 0) k1_known = false;
  else if (cx && cx->have && cx->m == v.m && memcmp(cx->slot, v.slot, (size_t)v.m * sizeof v.slot[0]) == 0) {   /* same slots as the previous class: same answers */
    k1_known = cx->known;
    ulm = cx->ulm;
  } else {
    k1_known = ul_masks(&v, ul, ulm_local);
    if (cx) { cx->have = true; cx->m = v.m; cx->known = k1_known; memcpy(cx->slot, v.slot, (size_t)v.m * sizeof v.slot[0]); memcpy(cx->ulm, ulm_local, (size_t)v.m * sizeof ulm_local[0]); }
  }
  const double alpha = NBS_SIGNIF_ALPHA / (nc > 0 ? nc : 1);   /* Bonferroni over the splits compared */

  bool refuted[NR_DCI11_CORE_MAX] = {0}, h_sup[NR_DCI11_CORE_MAX] = {0}, d_sup[NR_DCI11_CORE_MAX] = {0};
  double h_p[NR_DCI11_CORE_MAX], d_p[NR_DCI11_CORE_MAX];
  bool h_any = false, d_any = false;
  k1m_t memo[K1_MEMO], spare;
  int nmemo = 0;

  for (int ci = 0; ci < nc; ci++) {
    const core_t *c = &cores[ci];
    h_p[ci] = d_p[ci] = 1.0;
    /* K1 vs the TDD pattern: refutation by significantly more infeasible DCIs than the outlier rate. The result depends only on where the K1 field
     * starts and how wide it is, which many cores share: computed once per (start, width). */
    const k1m_t *K = NULL;
    if (k1_known) {
      const int kpos = base + c->tb2 + c->harq + c->dai + 2 + c->tpc2 + 3;
      for (int q = 0; q < nmemo; q++) if (memo[q].kpos == kpos && memo[q].w == c->k1) { K = &memo[q]; break; }
      if (!K) {
        k1m_t *e = nmemo < K1_MEMO ? &memo[nmemo++] : &spare;
        bool used[KEYS];
        e->kpos = kpos; e->w = c->k1;
        e->bad = k1_sets(&v, o, base, c, ulm, e->feas, used, e->key_of);
        K = e;
      }
      if (k1_refuted(K->bad, v.m, alpha)) { refuted[ci] = true; continue; }
    }
    /* HARQ pid: steps of 1..3 between consecutive grants */
    if (v.m >= 2) {
      const int mask = (1 << c->harq) - 1, hp = base + c->tb2;
      int k = 0;
      for (int i = 1; i < v.m; i++) {
        const int step = (rd(v.o[i], hp, c->harq) - rd(v.o[i - 1], hp, c->harq)) & mask;
        k += step >= 1 && step <= 3;
      }
      const double p0 = 3.0 / (double)(1 << c->harq), p = nbs_binom_sf((uint64_t)k, (uint64_t)(v.m - 1), p0);
      if (p < alpha) { h_sup[ci] = true; h_p[ci] = p; h_any = true; }
    }
    /* counter DAI: of two consecutive grants that CAN share a HARQ-ACK slot (some feasible offsets of their keys land them on the same
     * slot), the later one reads the earlier + 1 mod 4 when they do. Pairs that cannot share one are not counted. Null: 1/4 per pair. */
    if (k1_known && v.m >= 2) {
      const int dp = base + c->tb2 + c->harq;
      int k = 0, nt = 0;
      for (int i = 1; i < v.m; i++) {
        const int64_t gap = v.slot[i] - v.slot[i - 1];
        const int ka = K->key_of[i - 1], kb = K->key_of[i];
        if (ka == kb || gap >= C_MAX || !(K->feas[kb] & (K->feas[ka] >> gap))) continue;   /* one key = one offset: n differs, so the slots differ */
        nt++;
        k += rd(v.o[i], dp, 2) == ((rd(v.o[i - 1], dp, 2) + 1) & 3);
      }
      const double p = nbs_binom_sf((uint64_t)k, (uint64_t)nt, 0.25);
      if (nt > 0 && p < alpha) { d_sup[ci] = true; d_p[ci] = p; d_any = true; }
    }
  }
  /* relative tests: only when some split is supported do the unsupported ones fall. A counter at some position can only be the DAI, so every
   * split that does not put a DAI field exactly there falls (a DAI-less one calling it TPC, a longer HARQ field swallowing it, ...). */
  /* The HARQ test locates the pid FIELD (where it starts), not its width: a 5-bit reading of a 4-bit pid (the extra bit is the next field) steps like a
   * pid, and a scheduler without the +1..3 habit (lowest free process) makes the true 4-bit reading look no better than noise while the smeared 5-bit one
   * looks "supported". So a start is supported when any width at it is, and widths are left to the other tests. */
  bool start_sup[2] = {false, false};
  for (int ci = 0; ci < nc; ci++) start_sup[cores[ci].tb2 ? 1 : 0] |= h_sup[ci];
  int na = 0;
  for (int ci = 0; ci < nc; ci++) {
    if (!refuted[ci] && h_any && !start_sup[cores[ci].tb2 ? 1 : 0]) refuted[ci] = true;
    if (!refuted[ci] && d_any && !(cores[ci].dai > 0 && d_sup[ci])) refuted[ci] = true;
    if (refuted[ci] || na >= max) continue;
    ext_ctx_t x = {.base = core_split(&cores[ci]), .out = &alive[na], .max = 1, .count = 0, .first_only = true};
    ext_walk(&x, 0, cores[ci].rest);
    chance_p[na] = (h_sup[ci] ? h_p[ci] : 1.0) * (cores[ci].dai > 0 && d_sup[ci] ? d_p[ci] : 1.0);
    na++;
  }
  return na;
}

int nr_dci11_prove(const nr_dci11_layout_t *l, const nr_dci11_offsets_t *o, const nr_dci11_obs_t *bank, int n, nr_ul_slot_fn ul,
                   nr_dci11_split_t *alive, double *chance_p, int max)
{
  return prove_impl(l, o, bank, n, ul, alive, chance_p, max, NULL);
}

int nr_dci11_mcs28_new_tb(const nr_dci11_layout_t *l, const nr_dci11_offsets_t *o, const nr_dci11_obs_t *bank, int n, const nr_dci11_split_t *s)
{
  if (!l || !o || !s || (n > 0 && !bank) || o->ant_ports < l->pre_ant || s->harq < 4 || s->harq > 5) return -1;
  view_t v;
  make_view(&v, o, bank, n);
  int8_t last[32];
  memset(last, -1, sizeof last);
  const int hp = o->ant_ports - l->pre_ant + s->tb2;
  int cnt = 0;
  for (int i = 0; i < v.m; i++) {
    const int pid = rd(v.o[i], hp, s->harq), ndi = rd(v.o[i], o->mcs + 5, 1), mcs = rd(v.o[i], o->mcs, 5);
    if (last[pid] >= 0 && last[pid] != ndi && mcs == 28) cnt++;
    last[pid] = (int8_t)ndi;
  }
  return cnt;
}

/* ---- layout classes ---- */
/* what the prover reads of a layout: where pre_ant starts and how long it is, the TDRA field, the MCS/NDI/RV block and the DCI length */
bool nr_dci11_class_same(const nr_dci11_layout_t *l1, const nr_dci11_offsets_t *o1, const nr_dci11_layout_t *l2, const nr_dci11_offsets_t *o2)
{
  return l1->pre_ant == l2->pre_ant && o1->ant_ports - l1->pre_ant == o2->ant_ports - l2->pre_ant && o1->mcs == o2->mcs && o1->tda == o2->tda &&
         o1->tda_bits == o2->tda_bits && o1->total == o2->total;
}

int nr_dci11_classes(const nr_dci11_resolver_t *r, nr_dci11_class_t *out, int max)
{
  if (!r || !out || max <= 0) return -1;
  if (r->n_truncated) return -2;   /* the stored set is incomplete: the true layout may be missing, so no class set exists */
  const int n = __atomic_load_n(&r->n_hyp, __ATOMIC_ACQUIRE);
  int cnt = 0;
  for (int i = nr_alive_next(r->alive, 0, n); i < n; i = nr_alive_next(r->alive, i + 1, n)) {
    int k = 0;
    while (k < cnt && !nr_dci11_class_same(&out[k].l, &out[k].o, &r->hyp[i], &r->off[i])) k++;
    if (k < cnt) continue;
    if (cnt == max) return -1;
    out[cnt].l = r->hyp[i];
    out[cnt].o = r->off[i];
    cnt++;
  }
  return cnt;
}

/* the field reading of a surviving split in absolute bit positions: two classes that read the same bits the same way are one answer */
typedef struct { uint16_t harq_pos; uint8_t harq, dai, tpc2, k1; } sig_t;   /* the TDRA/MCS offsets only steer the tests, they do not change what is emitted */
static sig_t sig_of(const nr_dci11_class_t *c, const nr_dci11_split_t *s)
{
  sig_t g;
  memset(&g, 0, sizeof g);
  g.harq_pos = (uint16_t)(c->o.ant_ports - c->l.pre_ant + s->tb2);
  g.harq = s->harq; g.dai = s->dai; g.tpc2 = s->tpc2; g.k1 = s->k1;
  return g;
}

/* ---- per-cell banks ---- */
#define BANK_RNTIS 128
#define BANK_DUE 16
typedef struct {
  bool used;
  uint16_t rnti;
  uint64_t tick;
  uint32_t fed, proved_at, rev;   /* rev: unique per life of the ring (new entry / ring drop), so a proof computed on an older ring is discarded */
  int64_t last_slot;
  uint8_t head, cnt;
  nr_dci11_obs_t obs[NR_DCI11_BANK];
  uint64_t due_since, last_try;   /* while deferred: when the look was first held back (oldest-due-first) and when it was last attempted (a silent RNTI stops blocking) */
  bool pend_scoped;               /* the held-back look is a scoped (own-class) one */
  bool proven, scoped, deferred;   /* scoped: proved within a CRC-solved RNTI's own class only (not unique over the resolver's set); deferred: counted as budget-deferred since the last look */
  nr_dci11_class_t cls;
  nr_dci11_split_t split;
  int alive1;                      /* 1 + splits alive over all classes at the last look (0: no look, or a class the prover could not examine) */
} bank_t;
/* ring generation: process-wide and never reset (a reset of the bank must not let a proof taken before it match a ring created after it) */
static _Atomic uint32_t g_ring_rev;
static uint32_t next_rev(void) { return atomic_fetch_add_explicit(&g_ring_rev, 1, memory_order_relaxed) + 1u; }
typedef struct { bank_t e[BANK_RNTIS]; uint64_t tick, ident; } bank_tab_t;
NR_CELL_STATIC(bank_tab_t, g_bank)
#define g_bank NR_CELL_REF(g_bank)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_bank_mu, PTHREAD_MUTEX_INITIALIZER)
#define g_bank_mu NR_CELL_REF(g_bank_mu)

static bank_t *bank_find(uint16_t rnti)
{
  for (int i = 0; i < BANK_RNTIS; i++)
    if (g_bank.e[i].used && g_bank.e[i].rnti == rnti) return &g_bank.e[i];
  return NULL;
}

void nr_dci11_bank_feed(uint16_t rnti, nr_dci_bits_t payload, uint16_t len, int64_t abs_slot, const nr_dci11_feed_t *f)
{
  pthread_mutex_lock(&g_bank_mu);
  if (f && f->ident != g_bank.ident) {   /* another cell identity: nothing of the old one means anything (K1 counts slots of another pattern) */
    memset(&g_bank, 0, sizeof g_bank);
    g_bank.ident = f->ident;
  }
  bank_t *b = bank_find(rnti);
  if (!b) {   /* a free entry, else the least recently fed */
    b = &g_bank.e[0];
    for (int i = 0; i < BANK_RNTIS; i++) {
      if (!g_bank.e[i].used) { b = &g_bank.e[i]; break; }
      if (g_bank.e[i].tick < b->tick) b = &g_bank.e[i];
    }
    memset(b, 0, sizeof *b);
    b->used = true;
    b->rnti = rnti;
    b->rev = next_rev();
  } else if (f && b->cnt && (abs_slot < b->last_slot - f->max_back_slots || abs_slot > b->last_slot + f->max_gap_slots)) {
    /* silent for longer than the UE-context expiry, or the clock went back: the RNTI may belong to another UE now, or to another run */
    const uint8_t keep_used = b->used;
    const uint16_t keep_rnti = b->rnti;
    memset(b, 0, sizeof *b);
    b->used = keep_used;
    b->rnti = keep_rnti;
    b->rev = next_rev();
  }
  b->tick = ++g_bank.tick;
  if (!b->cnt || abs_slot > b->last_slot) b->last_slot = abs_slot;
  b->obs[b->head] = (nr_dci11_obs_t){payload, len, abs_slot};
  b->head = (uint8_t)((b->head + 1) % NR_DCI11_BANK);
  if (b->cnt < NR_DCI11_BANK) b->cnt++;
  b->fed++;
  pthread_mutex_unlock(&g_bank_mu);
}

static bool bank_due_locked(const bank_t *b) { return b && b->proved_at <= b->fed && b->fed - b->proved_at >= BANK_DUE; }

bool nr_dci11_bank_due(uint16_t rnti)
{
  pthread_mutex_lock(&g_bank_mu);
  const bool due = bank_due_locked(bank_find(rnti));
  pthread_mutex_unlock(&g_bank_mu);
  return due;
}

static bool bank_prove_impl(uint16_t rnti, const nr_dci11_class_t *cls, int ncls, nr_ul_slot_fn ul, bool scoped)
{
  if (ncls > NR_DCI11_CLASS_MAX) return false;
  nr_dci11_obs_t snap[NR_DCI11_BANK];   /* copy under the cell lock, prove outside it */
  int n;
  uint32_t fed, rev;
  bool was = false;
  sig_t prev;
  memset(&prev, 0, sizeof prev);
  pthread_mutex_lock(&g_bank_mu);
  bank_t *b = bank_find(rnti);
  if (!bank_due_locked(b)) { pthread_mutex_unlock(&g_bank_mu); return false; }
  if (!cls || ncls <= 0) {   /* a skipped round (no usable class set): look again after 16 more */
    b->proved_at = b->fed;
    b->deferred = false;
    b->due_since = 0;
    pthread_mutex_unlock(&g_bank_mu);
    return false;
  }
  n = b->cnt;
  fed = b->fed;
  rev = b->rev;
  for (int i = 0; i < n; i++) snap[i] = b->obs[(b->head + NR_DCI11_BANK - n + i) % NR_DCI11_BANK];
  if (b->proven && b->scoped == scoped) { was = true; prev = sig_of(&b->cls, &b->split); }   /* a proof of the other kind of look is not carried over */
  pthread_mutex_unlock(&g_bank_mu);

  /* survivors over every class: distinct readings (absolute positions), the first one kept. A class the prover cannot examine (-1) is AMBIGUOUS, not refuted. */
  bool have = false, multi = false, prev_alive = false;
  nr_dci11_class_t first_c;
  nr_dci11_split_t first_s;
  double first_p = 1.0;
  int n_alive = 0;
  memset(&first_c, 0, sizeof first_c);
  memset(&first_s, 0, sizeof first_s);
  sig_t first_sig;
  memset(&first_sig, 0, sizeof first_sig);
  ulctx_t cx;
  cx.have = false;   /* the UL-capable masks depend on the bank's slots only: computed once for all classes */
  for (int c = 0; c < ncls; c++) {
    nr_dci11_split_t alive[NR_DCI11_CORE_MAX];
    double cp[NR_DCI11_CORE_MAX];
    const int na = prove_impl(&cls[c].l, &cls[c].o, snap, n, ul, alive, cp, NR_DCI11_CORE_MAX, &cx);
    if (na < 0) { multi = true; n_alive = -1000000; continue; }
    n_alive += na;
    for (int j = 0; j < na; j++) {
      const sig_t g = sig_of(&cls[c], &alive[j]);
      if (was && memcmp(&g, &prev, sizeof g) == 0) prev_alive = true;
      if (!have) { have = true; first_c = cls[c]; first_s = alive[j]; first_p = cp[j]; first_sig = g; }
      else if (memcmp(&g, &first_sig, sizeof g) != 0) multi = true;
    }
  }
  /* one answer across all classes, significant at alpha / (classes compared) */
  const bool proven_now = have && !multi && first_p < NBS_SIGNIF_ALPHA / ncls;

  pthread_mutex_lock(&g_bank_mu);
  b = bank_find(rnti);   /* the entry may have been evicted, reset or restarted meanwhile: then this result belongs to a ring that no longer exists */
  bool changed = false;
  if (b && b->rev == rev) {
    b->proved_at = fed <= b->fed ? fed : b->fed;
    b->deferred = false;
    b->due_since = 0;
    b->alive1 = n_alive < 0 ? 0 : n_alive + 1;
    if (was && b->proven && prev_alive && !multi) {
      /* HYSTERESIS: a proof stands until the data refute it; losing significance in a later look is not a refutation. A surviving rival (multi) is
       * ambiguity and ends it. */
    } else if (proven_now) {
      changed = !b->proven || memcmp(&first_sig, &prev, sizeof prev) != 0;
      b->proven = true; b->scoped = scoped; b->cls = first_c; b->split = first_s;
    } else if (b->proven) {
      b->proven = false;
      changed = true;
    }
  }
  pthread_mutex_unlock(&g_bank_mu);
  return changed;
}

bool nr_dci11_bank_maybe_prove(uint16_t rnti, const nr_dci11_class_t *cls, int ncls, nr_ul_slot_fn ul)
{
  return bank_prove_impl(rnti, cls, ncls, ul, false);
}

bool nr_dci11_bank_proven_scope(uint16_t rnti, nr_dci11_class_t *cls, nr_dci11_split_t *out, bool *scoped)
{
  if (!cls || !out) return false;
  pthread_mutex_lock(&g_bank_mu);
  const bank_t *b = bank_find(rnti);
  const bool ok = b && b->proven;
  if (ok) { *cls = b->cls; *out = b->split; if (scoped) *scoped = b->scoped; }
  pthread_mutex_unlock(&g_bank_mu);
  return ok;
}

int nr_dci11_bank_alive(uint16_t rnti)
{
  pthread_mutex_lock(&g_bank_mu);
  const bank_t *b = bank_find(rnti);
  const int a = b && b->alive1 > 0 ? b->alive1 - 1 : -1;
  pthread_mutex_unlock(&g_bank_mu);
  return a;
}

bool nr_dci11_bank_proven_any(uint16_t rnti, nr_dci11_class_t *cls, nr_dci11_split_t *out)
{
  return nr_dci11_bank_proven_scope(rnti, cls, out, NULL);
}

bool nr_dci11_bank_proven(uint16_t rnti, const nr_dci11_layout_t *l, const nr_dci11_offsets_t *o, nr_dci11_split_t *out)
{
  nr_dci11_class_t c;
  if (!l || !o || !out || !nr_dci11_bank_proven_any(rnti, &c, out)) return false;
  return nr_dci11_class_same(&c.l, &c.o, l, o);
}

static void bank_clear_proofs(void)
{
  pthread_mutex_lock(&g_bank_mu);
  for (int i = 0; i < BANK_RNTIS; i++) if (!g_bank.e[i].scoped) g_bank.e[i].proven = false;   /* a solved RNTI's scoped proof belongs to its known layout, not to this set */
  pthread_mutex_unlock(&g_bank_mu);
}

/* ---- the class set of the cell's resolver, cached ---- */
typedef struct {
  bool valid;
  const nr_dci11_resolver_t *r;
  int n_hyp, n_alive;
  uint32_t trunc;
  int status, ncls, total;   /* total: distinct classes counted up to NR_DCI11_CLASS_COUNT_MAX + 1 (the histogram of what E2/E3 must reduce) */
  nr_dci11_class_t cls[NR_DCI11_CLASS_MAX];
} cls_cache_t;
NR_CELL_STATIC(cls_cache_t, g_cls)
#define g_cls NR_CELL_REF(g_cls)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_cls_mu, PTHREAD_MUTEX_INITIALIZER)
#define g_cls_mu NR_CELL_REF(g_cls_mu)

int nr_dci11_class_set(const nr_dci11_resolver_t *r, nr_dci11_class_t *out, bool allow_compute, int *total)
{
  if (total) *total = 0;
  if (!r) return NR_DCI11_CLS_NONE;
  const int nh = __atomic_load_n(&r->n_hyp, __ATOMIC_ACQUIRE), na = r->n_alive;
  const uint32_t tr = r->n_truncated;
  pthread_mutex_lock(&g_cls_mu);
  if (g_cls.valid && g_cls.r == r && g_cls.n_hyp == nh && g_cls.n_alive == na && g_cls.trunc == tr) {
    const int st = g_cls.status;
    if (total) *total = g_cls.total;
    if (out && st > 0) memcpy(out, g_cls.cls, (size_t)g_cls.ncls * sizeof out[0]);
    pthread_mutex_unlock(&g_cls_mu);
    return st;
  }
  pthread_mutex_unlock(&g_cls_mu);
  if (!allow_compute) return NR_DCI11_CLS_NONE;   /* the receive thread never scans the resolver; no reading until a consumer has */
  nr_dci11_class_t tmp[NR_DCI11_CLASS_COUNT_MAX];
  int st = nr_dci11_classes(r, tmp, NR_DCI11_CLASS_COUNT_MAX);   /* >0 classes, 0 none alive, -1 more than the count cap, -2 incomplete set */
  int tot = st;
  if (st == -1) tot = NR_DCI11_CLASS_COUNT_MAX + 1;
  if (st == -2) { st = NR_DCI11_CLS_INCOMPLETE; tot = 0; }
  else if (st < 0 || st > NR_DCI11_CLASS_MAX) st = NR_DCI11_CLS_TOO_MANY;
  pthread_mutex_lock(&g_cls_mu);
  g_cls.valid = true; g_cls.r = r; g_cls.n_hyp = nh; g_cls.n_alive = na; g_cls.trunc = tr; g_cls.status = st; g_cls.total = tot;
  g_cls.ncls = st > 0 ? st : 0;
  if (st > 0) memcpy(g_cls.cls, tmp, (size_t)st * sizeof tmp[0]);
  pthread_mutex_unlock(&g_cls_mu);
  if (st <= 0) bank_clear_proofs();   /* no usable class set: every reading proved over a set is stale until a fresh proof (scoped ones are not) */
  if (total) *total = tot;
  if (out && st > 0) memcpy(out, tmp, (size_t)st * sizeof tmp[0]);
  return st;
}

void nr_dci11_bank_reset(void)
{
  pthread_mutex_lock(&g_bank_mu);
  memset(&g_bank, 0, sizeof g_bank);
  pthread_mutex_unlock(&g_bank_mu);
  pthread_mutex_lock(&g_cls_mu);
  g_cls.valid = false;
  pthread_mutex_unlock(&g_cls_mu);
}

int64_t nr_dci11_tdd_slot(int64_t mono, int frame, int slot, int spf)
{
  if (mono < 0 || frame < 0 || frame >= 1024 || slot < 0 || spf <= 0 || slot >= spf) return -1;
  const int64_t w = 1024 * (int64_t)spf, f = (int64_t)frame * spf + slot;
  int64_t d = ((f - mono) % w + w) % w;
  if (d > w / 2) d -= w;
  return mono + d >= 0 ? mono + d : -1;
}

/* ---- CPU budget and the service entry the scan consumer calls ---- */
static uint64_t clock_real(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static uint64_t (*g_clock)(void) = clock_real;   /* test hook only (process-wide) */
void nr_dci11_set_clock(uint64_t (*fn)(void)) { g_clock = fn ? fn : clock_real; }

/* PROVE BUDGET: 20 ms of prover and class-scan time per second of wall time per cell = 2 % of one core on the scan consumers (a general bound on what an
 * optional evidence stage may take), split in two sub-budgets of 10 ms: scoped (a solved RNTI's own-class look, 56 us) and unscoped (class-set look, <= 820 us
 * per class). Unscoped demand can never consume the scoped share. Within a sub-budget the longest-waiting RNTI is served first. Fixed 1 s windows; a look
 * that starts under budget may overshoot by one proof (< 1 ms). */
#define BUDGET_NS 10000000ull
#define BUDGET_WINDOW_NS 1000000000ull
typedef struct { uint64_t start_ns[2], spent_ns[2]; } budget_t;   /* [0] unscoped, [1] scoped */
NR_CELL_STATIC(budget_t, g_budget)
#define g_budget NR_CELL_REF(g_budget)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_budget_mu, PTHREAD_MUTEX_INITIALIZER)
#define g_budget_mu NR_CELL_REF(g_budget_mu)

static bool budget_ok(uint64_t now, int k)
{
  pthread_mutex_lock(&g_budget_mu);
  if (now - g_budget.start_ns[k] >= BUDGET_WINDOW_NS || now < g_budget.start_ns[k]) { g_budget.start_ns[k] = now; g_budget.spent_ns[k] = 0; }
  const bool ok = g_budget.spent_ns[k] < BUDGET_NS;
  pthread_mutex_unlock(&g_budget_mu);
  return ok;
}
static void budget_charge(uint64_t ns, int k)
{
  pthread_mutex_lock(&g_budget_mu);
  g_budget.spent_ns[k] += ns;
  pthread_mutex_unlock(&g_budget_mu);
}
void nr_dci11_budget_reset(void)
{
  pthread_mutex_lock(&g_budget_mu);
  memset(&g_budget, 0, sizeof g_budget);
  pthread_mutex_unlock(&g_budget_mu);
}

/* Oldest-due first: the look of rnti may run only if no other RNTI's look of the same kind has been waiting longer (a waiting RNTI silent for > 2 windows
 * does not count). Returns true = run; false = held back (marks it waiting; *first = first hold-back of this episode). */
static bool bank_turn(uint16_t rnti, bool scoped, uint64_t t0, bool budget, bool *first)
{
  *first = false;
  pthread_mutex_lock(&g_bank_mu);
  bank_t *b = bank_find(rnti);
  bool run = b != NULL && budget;
  const uint64_t mine = b && b->deferred ? b->due_since : t0;
  if (run) {
    for (int i = 0; i < BANK_RNTIS; i++) {
      const bank_t *o = &g_bank.e[i];
      if (o != b && o->used && o->deferred && o->pend_scoped == scoped && o->due_since < mine && t0 - o->last_try <= 2 * BUDGET_WINDOW_NS) { run = false; break; }
    }
  }
  if (b && !run) {
    if (!b->deferred) { b->deferred = true; b->due_since = t0; b->pend_scoped = scoped; *first = true; }
    b->last_try = t0;
  }
  pthread_mutex_unlock(&g_bank_mu);
  return run;
}

unsigned nr_dci11_bank_service(uint16_t rnti, const nr_dci11_resolver_t *r, const nr_dci11_class_t *own, nr_ul_slot_fn ul, int *total_classes)
{
  unsigned fl = 0;
  if (total_classes) *total_classes = -1;
  if (!nr_dci11_bank_due(rnti)) return 0;
  const int k = own ? 1 : 0;
  const uint64_t t0 = g_clock();
  bool first;
  if (!bank_turn(rnti, own != NULL, t0, budget_ok(t0, k), &first)) {   /* over budget, or an older one goes first: it stays due, counted once per episode */
    return first ? NR_DCI11_SVC_DEFERRED | (own ? NR_DCI11_SVC_DEFERRED_SCOPED : 0u) : 0u;
  }
  bool changed;
  if (own) {
    changed = bank_prove_impl(rnti, own, 1, ul, true);   /* the layout is CRC-solved: its one class */
  } else {
    nr_dci11_class_t cls[NR_DCI11_CLASS_MAX];
    int tot = 0;
    const int nc = nr_dci11_class_set(r, cls, true, &tot);
    if (total_classes && nc != NR_DCI11_CLS_INCOMPLETE) *total_classes = tot;
    if (nc < 0) fl |= NR_DCI11_SVC_SKIPPED;
    changed = bank_prove_impl(rnti, cls, nc > 0 ? nc : 0, ul, false);
  }
  budget_charge(g_clock() - t0, k);
  return fl | (changed ? NR_DCI11_SVC_CHANGED : 0);
}

bool nr_dci11_emit_ok(bool proven, bool scoped, uint16_t dci_len, const nr_dci11_class_t *pc, const nr_dci11_layout_t *rep_l, const nr_dci11_offsets_t *rep_o,
                      const nr_dci11_class_t *set, int nset)
{
  if (!proven || !pc || dci_len != pc->o.total) return false;                                     /* only DCIs of the length the proof was made for */
  const bool rep = rep_l && rep_o;
  if (scoped) return rep && nr_dci11_class_same(&pc->l, &pc->o, rep_l, rep_o);                   /* a solved RNTI's own-class proof: only while it is still that class */
  if (!set || nset <= 0) return false;                                                            /* an unscoped one: only over the current usable class set ... */
  if (rep && !nr_dci11_class_same(&pc->l, &pc->o, rep_l, rep_o)) return false;
  for (int i = 0; i < nset; i++) if (nr_dci11_class_same(&set[i].l, &set[i].o, &pc->l, &pc->o)) return true;   /* ... and only while its class is still in it */
  return false;
}
