/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_dci11_prove.h"
#include "common/utils/nr_cell.h"
#include "nr_blind_stats.h"
#include <pthread.h>
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

int nr_dci11_prove(const nr_dci11_layout_t *l, const nr_dci11_offsets_t *o, const nr_dci11_obs_t *bank, int n, nr_ul_slot_fn ul,
                   nr_dci11_split_t *alive, double *chance_p, int max)
{
  if (!l || !o || !alive || !chance_p || max <= 0 || (n > 0 && !bank) || o->ant_ports < l->pre_ant || o->total > NR_DCI_MAX_PAYLOAD) return -1;
  core_t cores[NR_DCI11_CORE_MAX];
  const int nc = enum_cores(l, cores, NR_DCI11_CORE_MAX);
  if (nc > NR_DCI11_CORE_MAX) return -1;
  const int base = o->ant_ports - l->pre_ant;
  view_t v;
  make_view(&v, o, bank, n);

  uint64_t ulm[NR_DCI11_BANK];
  const bool k1_known = o->tda_bits <= 4 && v.m > 0 && ul_masks(&v, ul, ulm);
  const double alpha = NBS_SIGNIF_ALPHA / (nc > 0 ? nc : 1);   /* Bonferroni over the splits compared */

  bool refuted[NR_DCI11_CORE_MAX] = {0}, h_sup[NR_DCI11_CORE_MAX] = {0}, d_sup[NR_DCI11_CORE_MAX] = {0};
  double h_p[NR_DCI11_CORE_MAX], d_p[NR_DCI11_CORE_MAX];
  bool h_any = false, d_any = false;

  for (int ci = 0; ci < nc; ci++) {
    const core_t *c = &cores[ci];
    h_p[ci] = d_p[ci] = 1.0;
    /* K1 vs the TDD pattern: refutation by significantly more infeasible DCIs than the outlier rate */
    uint64_t feas[KEYS];
    bool used[KEYS];
    int key_of[NR_DCI11_BANK];
    if (k1_known && k1_refuted(k1_sets(&v, o, base, c, ulm, feas, used, key_of), v.m, alpha)) { refuted[ci] = true; continue; }
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
        const int ka = key_of[i - 1], kb = key_of[i];
        if (ka == kb || gap >= C_MAX || !(feas[kb] & (feas[ka] >> gap))) continue;   /* one key = one offset: n differs, so the slots differ */
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
  uint32_t fed, proved_at;
  int64_t last_slot;
  uint8_t head, cnt;
  nr_dci11_obs_t obs[NR_DCI11_BANK];
  bool proven;
  nr_dci11_class_t cls;
  nr_dci11_split_t split;
} bank_t;
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
  } else if (f && b->cnt && (abs_slot < b->last_slot - f->max_back_slots || abs_slot > b->last_slot + f->max_gap_slots)) {
    /* silent for longer than the UE-context expiry, or the clock went back: the RNTI may belong to another UE now, or to another run */
    const uint8_t keep_used = b->used;
    const uint16_t keep_rnti = b->rnti;
    memset(b, 0, sizeof *b);
    b->used = keep_used;
    b->rnti = keep_rnti;
  }
  b->tick = ++g_bank.tick;
  if (!b->cnt || abs_slot > b->last_slot) b->last_slot = abs_slot;
  b->obs[b->head] = (nr_dci11_obs_t){payload, len, abs_slot};
  b->head = (uint8_t)((b->head + 1) % NR_DCI11_BANK);
  if (b->cnt < NR_DCI11_BANK) b->cnt++;
  b->fed++;
  pthread_mutex_unlock(&g_bank_mu);
}

bool nr_dci11_bank_maybe_prove(uint16_t rnti, const nr_dci11_class_t *cls, int ncls, nr_ul_slot_fn ul)
{
  if (!cls || ncls <= 0 || ncls > NR_DCI11_CLASS_MAX) return false;
  nr_dci11_obs_t snap[NR_DCI11_BANK];   /* copy under the cell lock, prove outside it */
  int n;
  uint32_t fed;
  bool was = false;
  sig_t prev;
  memset(&prev, 0, sizeof prev);
  pthread_mutex_lock(&g_bank_mu);
  bank_t *b = bank_find(rnti);
  if (!b || b->fed - b->proved_at < BANK_DUE) { pthread_mutex_unlock(&g_bank_mu); return false; }
  n = b->cnt;
  fed = b->fed;
  for (int i = 0; i < n; i++) snap[i] = b->obs[(b->head + NR_DCI11_BANK - n + i) % NR_DCI11_BANK];
  if (b->proven) { was = true; prev = sig_of(&b->cls, &b->split); }
  pthread_mutex_unlock(&g_bank_mu);

  /* survivors over every class: distinct readings (absolute positions), the first one kept */
  bool have = false, multi = false, prev_alive = false;
  nr_dci11_class_t first_c;
  nr_dci11_split_t first_s;
  double first_p = 1.0;
  memset(&first_c, 0, sizeof first_c);
  memset(&first_s, 0, sizeof first_s);
  sig_t first_sig;
  memset(&first_sig, 0, sizeof first_sig);
  for (int c = 0; c < ncls; c++) {
    nr_dci11_split_t alive[NR_DCI11_CORE_MAX];
    double cp[NR_DCI11_CORE_MAX];
    const int na = nr_dci11_prove(&cls[c].l, &cls[c].o, snap, n, ul, alive, cp, NR_DCI11_CORE_MAX);
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
  b = bank_find(rnti);   /* the entry may have been evicted or reset meanwhile */
  bool changed = false;
  if (b) {
    b->proved_at = fed;
    if (was && b->proven && prev_alive) {
      /* HYSTERESIS: a proof stands until the data refute it; losing significance in a later look is not a refutation */
    } else if (proven_now) {
      changed = !b->proven || memcmp(&first_sig, &prev, sizeof prev) != 0;
      b->proven = true; b->cls = first_c; b->split = first_s;
    } else if (b->proven) {
      b->proven = false;
      changed = true;
    }
  }
  pthread_mutex_unlock(&g_bank_mu);
  return changed;
}

bool nr_dci11_bank_proven_any(uint16_t rnti, nr_dci11_class_t *cls, nr_dci11_split_t *out)
{
  if (!cls || !out) return false;
  pthread_mutex_lock(&g_bank_mu);
  const bank_t *b = bank_find(rnti);
  const bool ok = b && b->proven;
  if (ok) { *cls = b->cls; *out = b->split; }
  pthread_mutex_unlock(&g_bank_mu);
  return ok;
}

bool nr_dci11_bank_proven(uint16_t rnti, const nr_dci11_layout_t *l, const nr_dci11_offsets_t *o, nr_dci11_split_t *out)
{
  nr_dci11_class_t c;
  if (!l || !o || !out || !nr_dci11_bank_proven_any(rnti, &c, out)) return false;
  return nr_dci11_class_same(&c.l, &c.o, l, o);
}

void nr_dci11_bank_reset(void)
{
  pthread_mutex_lock(&g_bank_mu);
  memset(&g_bank, 0, sizeof g_bank);
  pthread_mutex_unlock(&g_bank_mu);
}

int64_t nr_dci11_tdd_slot(int64_t mono, int frame, int slot, int spf)
{
  if (mono < 0 || frame < 0 || frame >= 1024 || slot < 0 || spf <= 0 || slot >= spf) return -1;
  const int64_t w = 1024 * (int64_t)spf, f = (int64_t)frame * spf + slot;
  int64_t d = ((f - mono) % w + w) % w;
  if (d > w / 2) d -= w;
  return mono + d >= 0 ? mono + d : -1;
}
