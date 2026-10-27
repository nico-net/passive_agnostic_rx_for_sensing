/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Host-derived CPU layout of the passive receiver: see nr_core_layout.h for the rules. */
#define _GNU_SOURCE
#include "nr_core_layout.h"
#include "nr_core_plan.h"
#include "nr_core_pool.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <linux/capability.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

_Static_assert(NR_CL_MAX_CPUS == NR_CP_MAX_CPUS, "CPU mask size mismatch");
_Static_assert(NR_CL_MAX_CELLS == NR_POOL_MAX_CELLS, "cell count mismatch with the core pool");

const char *nr_cl_mode_name(nr_cl_mode_t m)
{
  static const char *const n[] = {"sep", "compact", "small"};
  return m >= NR_CL_SEP && m <= NR_CL_SMALL ? n[m] : "?";
}

void nr_cl_over_none(nr_cl_over_t *o)
{
  memset(o, 0, sizeof(*o));
  o->rt = o->ring = o->gpu = o->uss = o->nid = o->dash = -1;
}

/* ---------------------------------------------------------------- pure solver */
typedef struct {
  const nr_cl_topo_t *t;
  int n, rt, maxcap;
} ctx_t;

static int count(const unsigned char *m, int n)
{
  int k = 0;
  for (int c = 0; c < n; c++) k += m[c] != 0;
  return k;
}
static int first(const unsigned char *m, int n)
{
  for (int c = 0; c < n; c++)
    if (m[c]) return c;
  return -1;
}
static int last(const unsigned char *m, int n)
{
  for (int c = n - 1; c >= 0; c--)
    if (m[c]) return c;
  return -1;
}
/* capacity in 10 % buckets of the largest one (Linux capacity differs a few % inside one core class, 997 vs 1017 on GB10) */
static int bucket(const ctx_t *x, int c)
{
  const int cap = x->t->cap[c] > 0 ? x->t->cap[c] : x->maxcap;
  return (cap * 10 + x->maxcap / 2) / x->maxcap;
}
static int dom(const ctx_t *x, int c)
{
  return x->t->llc[c] >= 0 ? x->t->llc[c] : x->t->node[c] >= 0 ? x->t->node[c] : 0;
}
static int dist(const ctx_t *x, int c) { return x->rt >= 0 ? abs(c - x->rt) : c; }
/* near: RT's domain, high capacity, close to RT, low number */
static bool near_before(const ctx_t *x, int a, int b)
{
  if (x->rt >= 0) {
    const bool da = dom(x, a) == dom(x, x->rt), db = dom(x, b) == dom(x, x->rt);
    if (da != db) return da;
  }
  const int ba = bucket(x, a), bb = bucket(x, b);
  if (ba != bb) return ba > bb;
  if (dist(x, a) != dist(x, b)) return dist(x, a) < dist(x, b);
  return a < b;
}
/* far: other domain, low capacity, far from RT, high number */
static bool far_before(const ctx_t *x, int a, int b)
{
  if (x->rt >= 0) {
    const bool oa = dom(x, a) != dom(x, x->rt), ob = dom(x, b) != dom(x, x->rt);
    if (oa != ob) return oa;
  }
  const int ba = bucket(x, a), bb = bucket(x, b);
  if (ba != bb) return ba < bb;
  if (dist(x, a) != dist(x, b)) return dist(x, a) > dist(x, b);
  return a > b;
}
/* best RT CPU of cand: GPU NUMA node (multi-node hosts), capacity, lowest number */
static int best_rt(const ctx_t *x, const unsigned char *cand)
{
  const nr_cl_topo_t *t = x->t;
  int nodes = 0, seen = -2;
  for (int c = 0; c < x->n; c++)
    if (cand[c] && t->node[c] != seen) seen = t->node[c], nodes++;
  int best = -1;
  for (int c = 0; c < x->n; c++) {
    if (!cand[c]) continue;
    if (best < 0) { best = c; continue; }
    if (nodes > 1 && t->gpu_node >= 0) {
      const bool ga = t->node[c] == t->gpu_node, gb = t->node[best] == t->gpu_node;
      if (ga != gb) { if (ga) best = c; continue; }
    }
    if (bucket(x, c) > bucket(x, best)) best = c;
  }
  return best;
}
/* radio busy-poller on a host without isolated CPUs: a big core, away from the RT reader's L3 domain, low number */
static int best_io(const ctx_t *x, const unsigned char *cand)
{
  int best = -1;
  for (int c = 0; c < x->n; c++) {
    if (!cand[c]) continue;
    if (best < 0) { best = c; continue; }
    if (bucket(x, c) != bucket(x, best)) { if (bucket(x, c) > bucket(x, best)) best = c; continue; }
    const bool oc = x->rt >= 0 && dom(x, c) != dom(x, x->rt), ob = x->rt >= 0 && dom(x, best) != dom(x, x->rt);
    if (oc != ob && oc) best = c; /* else keep the lower number */
  }
  return best;
}
/* take up to k CPUs of cand in near/far order (distinct physical cores first when asked); marks out[] and returns the count */
static int pick(const ctx_t *x, const unsigned char *cand, int k, bool near, bool distinct, unsigned char *out, int *list)
{
  int v[NR_CL_MAX_CPUS], m = 0;
  for (int c = 0; c < x->n; c++)
    if (cand[c] && !out[c]) v[m++] = c;
  for (int i = 1; i < m; i++) { /* insertion sort (n <= 1024, once at start-up) */
    const int c = v[i];
    int j = i - 1;
    while (j >= 0 && (near ? near_before(x, c, v[j]) : far_before(x, c, v[j]))) v[j + 1] = v[j], j--;
    v[j + 1] = c;
  }
  int got = 0;
  for (int pass = distinct ? 0 : 1; pass < 2 && got < k; pass++)
    for (int i = 0; i < m && got < k; i++) {
      const int c = v[i];
      if (out[c]) continue;
      if (pass == 0) {
        bool shared = false;
        for (int j = 0; j < x->n && !shared; j++) shared = out[j] && j != c && x->t->core[j] == x->t->core[c];
        if (shared) continue;
      }
      out[c] = 1;
      if (list) list[got] = c;
      got++;
    }
  return got;
}
static void note(nr_core_layout_t *L, const char *what, int v)
{ /* v < 0: "what" alone */
  const size_t o = strlen(L->src);
  if (o >= sizeof(L->src) - 1) return;
  if (v < 0) snprintf(L->src + o, sizeof(L->src) - o, "%s%s", o ? "," : "", what);
  else snprintf(L->src + o, sizeof(L->src) - o, "%s%s=%d", o ? "," : "", what, v);
}
/* pool tokens in log order: capacity class first, then CPU number */
static void sort_pool(const ctx_t *x, int *v, int m)
{
  for (int i = 1; i < m; i++) {
    const int c = v[i];
    int j = i - 1;
    while (j >= 0 && (bucket(x, c) > bucket(x, v[j]) || (bucket(x, c) == bucket(x, v[j]) && c < v[j]))) v[j + 1] = v[j], j--;
    v[j + 1] = c;
  }
}

static void build_core(const nr_cl_topo_t *t, const nr_cl_over_t *o, int nc, bool feed, nr_core_layout_t *L)
{
  nr_cl_over_t none;
  if (!o) nr_cl_over_none(&none), o = &none;
  memset(L, 0, sizeof(*L));
  L->n_cells = nc;
  L->feed = -1;
  L->feed_req = feed;
  for (int i = 0; i < NR_CL_MAX_CELLS; i++) L->rt_cell[i] = -1;
  L->rt = L->ring = L->uhd = L->gpu = L->uss = L->nid = L->dashboard = L->anchor = -1;
  ctx_t x = {.t = t, .n = t->ncpu > NR_CL_MAX_CPUS ? NR_CL_MAX_CPUS : (t->ncpu < 0 ? 0 : t->ncpu), .rt = -1, .maxcap = 1};
  const int n = x.n;
  for (int c = 0; c < n; c++)
    if (t->online[c] && t->cap[c] > x.maxcap) x.maxcap = t->cap[c];
  unsigned char *A = L->allowed, C[NR_CL_MAX_CPUS] = {0}, io[NR_CL_MAX_CPUS] = {0}, R[NR_CL_MAX_CPUS] = {0};
  bool deflt = true; /* the inherited mask is the kernel default (online minus isolated): not narrowed by taskset/cgroup */
  for (int c = 0; c < n; c++) {
    A[c] = t->online[c] && t->allowed[c] && !t->dpdk[c];
    L->usable[c] = t->online[c] && (t->allowed[c] || t->isolated[c]) && !t->dpdk[c];
    if (t->online[c] && (t->allowed[c] != 0) != !t->isolated[c]) deflt = false;
    C[c] = A[c] && !t->isolated[c];
  }
  if (!count(C, n)) memcpy(C, A, sizeof(C));
  const int rt_env = o->rt >= 0 && o->rt < n && L->usable[o->rt] ? o->rt : -1; /* an explicit reader CPU is never a radio poller */
  for (int c = 0; c < n; c++) io[c] = t->online[c] && t->isolated[c] && !t->dpdk[c] && (deflt || A[c]) && c != rt_env;
  /* radio IO: UHD control, then the ring producer, on isolated CPUs */
  if (o->has_uhd) {
    for (int c = 0; c < n; c++) L->uhd_mask[c] = o->uhd[c] && L->usable[c];
    L->uhd = first(L->uhd_mask, n);
    note(L, L->uhd >= 0 ? "uhd=env" : "uhd_env_ignored", -1);
  }
  if (L->uhd < 0 && t->radio)
    for (int c = 0; c < n; c++)
      if (io[c]) { L->uhd = c; break; }
  if (o->ring >= 0 && o->ring < n && L->usable[o->ring]) L->ring = o->ring, note(L, "ring=env", -1);
  else if (o->ring >= 0) note(L, "ring_env_ignored", o->ring);
  if (L->ring < 0 && t->radio)
    for (int c = 0; c < n; c++)
      if (io[c] && c != L->uhd && !L->uhd_mask[c]) { L->ring = c; break; }
  /* RT reader */
  const int anchor0 = count(C, n) >= 3 ? first(C, n) : -1;
  if (rt_env >= 0) L->rt = rt_env, note(L, "rt=env", -1);
  else {
    if (o->rt >= 0) note(L, "rt_env_ignored", o->rt);
    unsigned char cand[NR_CL_MAX_CPUS] = {0};
    for (int c = 0; c < n; c++) cand[c] = A[c] && t->isolated[c] && c != L->uhd && c != L->ring && !L->uhd_mask[c];
    if (!count(cand, n))
      for (int c = 0; c < n; c++) cand[c] = C[c] && c != anchor0 && c != L->uhd && c != L->ring;
    if (!count(cand, n))
      for (int c = 0; c < n; c++) cand[c] = C[c];
    L->rt = best_rt(&x, cand);
  }
  x.rt = L->rt;
  /* a radio with no isolated CPUs: its busy pollers take the next best CPUs after the RT reader */
  for (int k = 0; k < 2 && t->radio; k++) {
    int *slot = k ? &L->ring : &L->uhd;
    if (*slot >= 0) continue;
    unsigned char cand[NR_CL_MAX_CPUS] = {0};
    for (int c = 0; c < n; c++) cand[c] = C[c] && c != anchor0 && c != L->rt && c != L->uhd && c != L->ring && !L->uhd_mask[c];
    *slot = best_io(&x, cand);
  }
  if (L->uhd >= 0 && !count(L->uhd_mask, n)) L->uhd_mask[L->uhd] = 1;
  /* cells 1..nc-1: one more RT reader each, same candidate chain as the first one (isolated RT set, else general CPUs minus the anchor
   * and the radio IO); with >= 8 general CPUs the SMT sibling of a reader is only taken when nothing else is left */
  L->rt_cell[0] = L->rt;
  const int nread = nc + (feed ? 1 : 0); /* the feed reader (when planned) is reader number nc */
  for (int k = 1; k < nread && L->rt >= 0; k++) {
    int got = -1;
    for (int stage = 0; stage < 2 && got < 0; stage++)
      for (int pass = count(C, n) >= 8 ? 0 : 1; pass < 2 && got < 0; pass++) {
        unsigned char cand[NR_CL_MAX_CPUS] = {0};
        for (int c = 0; c < n; c++) {
          bool ok = stage == 0 ? (A[c] && t->isolated[c]) : (C[c] && c != anchor0);
          ok = ok && c != L->uhd && c != L->ring && !L->uhd_mask[c];
          for (int j = 0; j < k && ok; j++) ok = c != L->rt_cell[j] && (pass || t->core[c] != t->core[L->rt_cell[j]]);
          cand[c] = ok;
        }
        if (count(cand, n)) got = best_rt(&x, cand);
      }
    if (k < nc) L->rt_cell[k] = got; else L->feed = got;
  }
  if (L->rt >= 0) R[L->rt] = 1;
  for (int k = 1; k < nc; k++)
    if (L->rt_cell[k] >= 0) R[L->rt_cell[k]] = 1;
  if (L->feed >= 0) R[L->feed] = 1;
  if (L->ring >= 0) R[L->ring] = 1;
  for (int c = 0; c < n; c++) R[c] |= L->uhd_mask[c];
  /* W = the general CPUs minus the RT set (and its SMT siblings on hosts with >= 8 CPUs) */
  unsigned char Wm[NR_CL_MAX_CPUS] = {0};
  const bool keep_sib_idle = count(C, n) >= 8;
  for (int c = 0; c < n; c++) {
    if (!C[c] || R[c]) continue;
    bool sib = false;
    for (int r = 0; r < n && keep_sib_idle && !sib; r++) sib = R[r] && t->core[r] == t->core[c];
    Wm[c] = !sib;
  }
  const int W = count(Wm, n);
  L->W = W;
  L->anchor = count(C, n) >= 3 ? first(Wm, n) : -1;
  L->mode = W >= NR_CL_SEP_MIN_W ? NR_CL_SEP : W > NR_CL_SMALL_MAX_W ? NR_CL_COMPACT : NR_CL_SMALL;
  unsigned char taken[NR_CL_MAX_CPUS] = {0};
  if (L->mode != NR_CL_SMALL) {
    /* SEP: lane 5 + housekeeping 3 (anchor + leftovers); COMPACT: lane 4/3/2 + housekeeping = anchor */
    const int nlane = L->mode == NR_CL_SEP ? NR_CL_LANE : W >= 10 ? 4 : W >= 8 ? 3 : 2;
    const int nhk = L->mode == NR_CL_SEP ? NR_CL_SEP_HK : (L->anchor >= 0 ? 1 : 0);
    unsigned char cand[NR_CL_MAX_CPUS];
    memcpy(cand, Wm, sizeof(cand));
    if (L->anchor >= 0) cand[L->anchor] = 0;
    pick(&x, cand, nlane, false, false, L->lane, NULL);
    for (int c = 0; c < n; c++) cand[c] = cand[c] && !L->lane[c];
    int np = W - nlane - nhk;
    if (np > NR_CL_MAX_POOL) np = NR_CL_MAX_POOL;
    L->npool = pick(&x, cand, np, true, true, taken, L->pool);
    sort_pool(&x, L->pool, L->npool);
    for (int c = 0; c < n; c++) L->hk[c] = Wm[c] && !L->lane[c] && !taken[c];
    int l[NR_CL_LANE], k = 0;
    for (int c = 0; c < n && k < NR_CL_LANE; c++)
      if (L->lane[c]) l[k++] = c;
    L->gpu = k > 0 ? l[0] : -1;
    L->uss = k > 2 ? l[2] : -1;
    L->nid = k > 3 ? l[3] : -1;
    memcpy(L->check, L->lane, sizeof(L->check));
    for (int c = 0; c < n; c++) {
      L->recov[c] = L->lane[c] && c != L->gpu && c != L->uss;
      L->home[c] = L->hk[c] || L->lane[c];
    }
  } else {
    unsigned char H[NR_CL_MAX_CPUS] = {0};
    if (W == 0) {
      if (L->rt >= 0) H[L->rt] = 1;
    } else if (L->anchor >= 0)
      H[L->anchor] = 1;
    else
      pick(&x, Wm, 1, false, false, H, NULL);
    unsigned char cand[NR_CL_MAX_CPUS];
    for (int c = 0; c < n; c++) cand[c] = Wm[c] && !H[c];
    L->npool = W > 1 ? pick(&x, cand, NR_CL_MAX_POOL, true, true, taken, L->pool) : 0;
    sort_pool(&x, L->pool, L->npool);
    for (int c = 0; c < n; c++) {
      if (Wm[c] && !H[c] && !taken[c]) H[c] = 1;
      L->lane[c] = L->home[c] = L->hk[c] = L->check[c] = L->recov[c] = H[c];
    }
  }
  /* explicit pool list (ISAC_PDSCH_CORES / ISAC_PUSCH_CORES) */
  if (o->has_pool) {
    unsigned char P[NR_CL_MAX_CPUS] = {0};
    for (int c = 0; c < n; c++) P[c] = o->pool[c] && A[c] && !R[c];
    const int np = count(P, n);
    if (np >= 3 || (np >= 1 && np >= W)) {
      memset(taken, 0, sizeof(taken));
      L->npool = pick(&x, P, NR_CL_MAX_POOL, true, true, taken, L->pool);
      sort_pool(&x, L->pool, L->npool);
      bool lane_left = false;
      for (int c = 0; c < n; c++) {
        L->lane[c] = L->lane[c] && !taken[c];
        L->hk[c] = L->hk[c] && !taken[c];
        L->check[c] = L->check[c] && !taken[c];
        L->recov[c] = L->recov[c] && !taken[c];
        L->home[c] = A[c] && !t->isolated[c] && !taken[c] && !R[c];
        lane_left |= L->lane[c];
      }
      if (!lane_left) memcpy(L->lane, L->home, sizeof(L->lane));
      note(L, "pool=env", -1);
    } else
      note(L, "pool_env_ignored_usable", np);
  }
  if (o->has_lane) {
    unsigned char m[NR_CL_MAX_CPUS] = {0};
    for (int c = 0; c < n; c++) m[c] = o->lane[c] && t->online[c];
    if (count(m, n)) memcpy(L->lane, m, sizeof(m)), note(L, "lane=env", -1);
    else note(L, "lane_env_ignored", -1);
  }
  if (o->has_check) {
    unsigned char m[NR_CL_MAX_CPUS] = {0};
    for (int c = 0; c < n; c++) m[c] = o->check[c] && t->online[c];
    if (count(m, n)) memcpy(L->check, m, sizeof(m)), note(L, "check=env", -1);
  }
  const struct { int v; int *dst; const char *nm; } ws[] = {{o->gpu, &L->gpu, "gpu"}, {o->uss, &L->uss, "uss"}, {o->nid, &L->nid, "nid"}};
  for (unsigned i = 0; i < sizeof(ws) / sizeof(ws[0]); i++) {
    if (ws[i].v < 0) continue;
    char f[40];
    if (ws[i].v < n && L->usable[ws[i].v]) {
      *ws[i].dst = ws[i].v;
      snprintf(f, sizeof(f), "%s=env", ws[i].nm);
      note(L, f, -1);
    } else {
      snprintf(f, sizeof(f), "%s_env_ignored", ws[i].nm);
      note(L, f, ws[i].v);
    }
  }
  if (!count(L->home, n)) memcpy(L->home, L->lane, sizeof(L->home));
  if (o->dash >= 0 && o->dash < n && L->usable[o->dash]) {
    L->dashboard = o->dash;
    note(L, "dashboard=env", -1);
    return;
  }
  if (o->dash >= 0) note(L, "dashboard_env_ignored", o->dash);
  L->dashboard = last(L->hk, n);
  if (L->dashboard < 0) {
    unsigned char m[NR_CL_MAX_CPUS] = {0};
    for (int c = 0; c < n; c++) m[c] = L->lane[c] && A[c];
    L->dashboard = last(m, n);
  }
  if (L->dashboard < 0) L->dashboard = L->rt;
}

static bool hostable(const nr_core_layout_t *L)
{
  for (int k = 0; k < L->n_cells; k++)
    if (L->rt_cell[k] < 0) return false;
  return L->npool >= NR_CL_MIN_POOL;
}
static int max_cells_f(const nr_cl_topo_t *t, const nr_cl_over_t *o, bool feed)
{
  nr_core_layout_t S;
  int m = 0;
  nr_cl_over_t none;
  if (!o) nr_cl_over_none(&none), o = &none;
  for (int k = 1; k <= NR_CL_MAX_CELLS; k++) {
    build_core(t, o, k, feed, &S);
    if (!hostable(&S)) break;
    m = k;
  }
  return m;
}
int nr_cl_max_cells(const nr_cl_topo_t *t, const nr_cl_over_t *o) { return max_cells_f(t, o, false); }
int nr_cl_build_nf(const nr_cl_topo_t *t, const nr_cl_over_t *o, int nc, bool feed, nr_core_layout_t *L)
{
  nc = nc < 1 ? 1 : nc > NR_CL_MAX_CELLS ? NR_CL_MAX_CELLS : nc;
  build_core(t, o, nc, feed, L);
  L->max_cells = max_cells_f(t, o, feed);
  return hostable(L) ? 0 : 1;
}
int nr_cl_build_n(const nr_cl_topo_t *t, const nr_cl_over_t *o, int nc, nr_core_layout_t *L) { return nr_cl_build_nf(t, o, nc, false, L); }
int nr_cl_build(const nr_cl_topo_t *t, const nr_cl_over_t *o, nr_core_layout_t *L)
{
  nr_cl_build_n(t, o, 1, L);
  return 0;
}

int nr_cl_mask_str(const unsigned char *m, int n, char *buf, size_t len)
{
  size_t o = 0;
  buf[0] = 0;
  for (int c = 0; c < n && o + 16 < len; c++) {
    if (!m[c]) continue;
    int e = c;
    while (e + 1 < n && m[e + 1]) e++;
    o += (size_t)snprintf(buf + o, len - o, "%s%d", o ? "," : "", c);
    if (e != c) o += (size_t)snprintf(buf + o, len - o, "-%d", e);
    c = e;
  }
  if (!o) snprintf(buf, len, "-");
  return (int)o;
}

int nr_cl_format(const nr_cl_topo_t *t, const nr_core_layout_t *L, char *buf, size_t len)
{
  const int n = t->ncpu > NR_CL_MAX_CPUS ? NR_CL_MAX_CPUS : t->ncpu;
  char on[256], al[256], is[256], nz[256], dp[128], cap[256] = "", lane[256], home[256], hk[256], chk[256], rc[256], uhd[64], pool[256] = "";
  nr_cl_mask_str(t->online, n, on, sizeof(on));
  nr_cl_mask_str(t->allowed, n, al, sizeof(al));
  nr_cl_mask_str(t->isolated, n, is, sizeof(is));
  nr_cl_mask_str(t->nohz, n, nz, sizeof(nz));
  nr_cl_mask_str(t->dpdk, n, dp, sizeof(dp));
  nr_cl_mask_str(L->lane, n, lane, sizeof(lane));
  nr_cl_mask_str(L->home, n, home, sizeof(home));
  nr_cl_mask_str(L->hk, n, hk, sizeof(hk));
  nr_cl_mask_str(L->check, n, chk, sizeof(chk));
  nr_cl_mask_str(L->recov, n, rc, sizeof(rc));
  nr_cl_mask_str(L->uhd_mask, n, uhd, sizeof(uhd));
  { /* capacity classes: "1024:0-5/793:6-13" */
    unsigned char done[NR_CL_MAX_CPUS] = {0};
    size_t o = 0;
    for (int c = 0; c < n; c++) {
      if (!t->online[c] || done[c]) continue;
      unsigned char m[NR_CL_MAX_CPUS] = {0};
      for (int d = c; d < n; d++)
        if (t->online[d] && !done[d] && t->cap[d] == t->cap[c]) m[d] = done[d] = 1;
      char s[256];
      nr_cl_mask_str(m, n, s, sizeof(s));
      if (o + strlen(s) + 16 < sizeof(cap)) o += (size_t)snprintf(cap + o, sizeof(cap) - o, "%s%d:%s", o ? "/" : "", t->cap[c], s);
    }
  }
  int smt = 0, ndom = 0, nnode = 0;
  {
    unsigned char seenl[NR_CL_MAX_CPUS] = {0}, seenn[NR_CL_MAX_CPUS] = {0};
    for (int c = 0; c < n; c++) {
      if (!t->online[c]) continue;
      smt |= t->core[c] != c;
      if (t->llc[c] >= 0 && t->llc[c] < NR_CL_MAX_CPUS && !seenl[t->llc[c]]) seenl[t->llc[c]] = 1, ndom++;
      if (t->node[c] >= 0 && t->node[c] < NR_CL_MAX_CPUS && !seenn[t->node[c]]) seenn[t->node[c]] = 1, nnode++;
    }
  }
  for (int i = 0, o = 0; i < L->npool && o + 8 < (int)sizeof(pool); i++) o += snprintf(pool + o, sizeof(pool) - (size_t)o, "%s%d", i ? "," : "", L->pool[i]);
  if (!L->npool) snprintf(pool, sizeof(pool), "-");
  char cells[160] = "";
  if (L->n_cells > 1) { /* N=1: the line is unchanged */
    int o = snprintf(cells, sizeof(cells), " cells=%d rt_cells=", L->n_cells);
    for (int k = 0; k < L->n_cells && o + 8 < (int)sizeof(cells); k++) o += snprintf(cells + o, sizeof(cells) - (size_t)o, "%s%d", k ? "," : "", L->rt_cell[k]);
    snprintf(cells + o, sizeof(cells) - (size_t)o, " max_cells=%d", L->max_cells);
  }
  if (L->feed_req) {
    const size_t o = strlen(cells);
    snprintf(cells + o, sizeof(cells) - o, " feed=%d", L->feed);
  }
  return snprintf(buf, len,
                  "SENSING: CORE_PLAN host=%s cpus=%d online=%s allowed=%s isolated=%s nohz_full=%s cap=%s smt=%d llc=%d numa=%d gpu_node=%d"
                  " rt_cap=%d radio=%d dpdk=%s -> mode=%s W=%d rt=%d uhd=%s ring=%d pool=%s n=%d lane=%s home=%s hk=%s dashboard=%d"
                  " gpu=%d uss=%d nid=%d check=%s recov=%s src=%s%s",
                  t->host[0] ? t->host : "?", count(t->online, n), on, al, is, nz, cap, smt, ndom, nnode, t->gpu_node, t->rt_capable, t->radio,
                  dp, nr_cl_mode_name(L->mode), L->W, L->rt, uhd, L->ring, pool, L->npool, lane, home, hk, L->dashboard, L->gpu, L->uss,
                  L->nid, chk, rc, L->src[0] ? L->src : "auto", cells);
}

/* ---------------------------------------------------------------- probe */
static int read_text(const char *path, char *buf, size_t n)
{
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  size_t got = fread(buf, 1, n - 1, f);
  fclose(f);
  buf[got] = 0;
  while (got && isspace((unsigned char)buf[got - 1])) buf[--got] = 0;
  return 0;
}
static int cpu_int(int c, const char *leaf, int dflt)
{
  char p[256], v[64];
  snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/%s", c, leaf);
  return read_text(p, v, sizeof(v)) || !v[0] ? dflt : atoi(v);
}
static int cpu_first_of_list(int c, const char *leaf)
{
  char p[256], v[4096];
  unsigned char m[NR_CL_MAX_CPUS];
  snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/%s", c, leaf);
  if (read_text(p, v, sizeof(v)) || nr_core_parse_list(v, m)) return -1;
  for (int i = 0; i < NR_CL_MAX_CPUS; i++)
    if (m[i]) return i;
  return -1;
}
static bool cap_sys_nice(void)
{
  struct __user_cap_header_struct hdr = {.version = _LINUX_CAPABILITY_VERSION_3};
  struct __user_cap_data_struct cap[2];
  if (syscall(SYS_capget, &hdr, cap) == -1) return false;
  return (cap[0].effective & (1u << CAP_SYS_NICE)) != 0;
}

int nr_cl_topo_read(nr_cl_topo_t *t, bool radio)
{
  memset(t, 0, sizeof(*t));
  t->gpu_node = -1;
  t->radio = radio;
  gethostname(t->host, sizeof(t->host) - 1);
  char buf[16384];
  if (read_text("/sys/devices/system/cpu/online", buf, sizeof(buf)) || nr_core_parse_list(buf, t->online)) {
    const long k = sysconf(_SC_NPROCESSORS_ONLN); /* no sysfs: assume 0..k-1 */
    for (long c = 0; c < k && c < NR_CL_MAX_CPUS; c++) t->online[c] = 1;
  }
  cpu_set_t a;
  if (sched_getaffinity(0, sizeof(a), &a) == 0)
    for (int c = 0; c < NR_CL_MAX_CPUS; c++) t->allowed[c] = CPU_ISSET(c, &a) != 0;
  else
    memcpy(t->allowed, t->online, sizeof(t->allowed));
  if (!read_text("/sys/devices/system/cpu/isolated", buf, sizeof(buf)) && buf[0]) nr_core_parse_list(buf, t->isolated);
  if (!read_text("/sys/devices/system/cpu/nohz_full", buf, sizeof(buf)) && buf[0] && strcmp(buf, "(null)")) nr_core_parse_list(buf, t->nohz);
  /* Intel hybrid without cpu_capacity: the PMU lists P (cpu_core) and E (cpu_atom) CPUs */
  unsigned char pcore[NR_CL_MAX_CPUS] = {0}, ecore[NR_CL_MAX_CPUS] = {0};
  const bool hybrid = !read_text("/sys/devices/cpu_core/cpus", buf, sizeof(buf)) && !nr_core_parse_list(buf, pcore)
                      && !read_text("/sys/devices/cpu_atom/cpus", buf, sizeof(buf)) && !nr_core_parse_list(buf, ecore);
  for (int c = 0; c < NR_CL_MAX_CPUS; c++) {
    t->core[c] = c;
    t->llc[c] = t->node[c] = -1;
    if (!t->online[c]) continue;
    t->ncpu = c + 1;
    t->cap[c] = cpu_int(c, "cpu_capacity", 0);
    if (!t->cap[c] && hybrid && (pcore[c] || ecore[c])) t->cap[c] = pcore[c] ? 1024 : 768;
    if (!t->cap[c]) t->cap[c] = cpu_int(c, "cpufreq/cpuinfo_max_freq", 0) / 1000;
    const int s = cpu_first_of_list(c, "topology/thread_siblings_list");
    if (s >= 0) t->core[c] = s;
    for (int i = 0; i < 10; i++) {
      char leaf[64];
      snprintf(leaf, sizeof(leaf), "cache/index%d/level", i);
      const int lv = cpu_int(c, leaf, -1);
      if (lv < 0) break;
      if (lv == 3) {
        snprintf(leaf, sizeof(leaf), "cache/index%d/shared_cpu_list", i);
        t->llc[c] = cpu_first_of_list(c, leaf);
        break;
      }
    }
    char p[96];
    snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d", c);
    DIR *d = opendir(p);
    for (struct dirent *e; d && (e = readdir(d));)
      if (!strncmp(e->d_name, "node", 4) && isdigit((unsigned char)e->d_name[4])) t->node[c] = atoi(e->d_name + 4);
    if (d) closedir(d);
  }
  { /* the GPU's NUMA node: first NVIDIA display/3D controller */
    DIR *d = opendir("/sys/bus/pci/devices");
    for (struct dirent *e; d && (e = readdir(d)) && t->gpu_node < 0;) {
      if (e->d_name[0] == '.') continue;
      char p[512], v[64];
      snprintf(p, sizeof(p), "/sys/bus/pci/devices/%s/vendor", e->d_name);
      if (read_text(p, v, sizeof(v)) || strcmp(v, "0x10de")) continue;
      snprintf(p, sizeof(p), "/sys/bus/pci/devices/%s/class", e->d_name);
      if (read_text(p, v, sizeof(v)) || strncmp(v, "0x03", 4)) continue;
      snprintf(p, sizeof(p), "/sys/bus/pci/devices/%s/numa_node", e->d_name);
      if (!read_text(p, v, sizeof(v))) t->gpu_node = atoi(v) >= 0 ? atoi(v) : -1;
    }
    if (d) closedir(d);
  }
  struct rlimit rl;
  t->rt_capable = cap_sys_nice() || (getrlimit(RLIMIT_RTPRIO, &rl) == 0 && rl.rlim_cur > 0);
  if (radio) { /* UHD DPDK lcores, the union of the config files UHD may read */
    char home[4096];
    snprintf(home, sizeof(home), "%s/.uhd/uhd.conf", getenv("HOME") ? getenv("HOME") : "");
    const char *paths[] = {"/etc/uhd/uhd.conf", home, "/root/.uhd/uhd.conf", getenv("UHD_CONFIG_FILE")};
    for (int i = 0; i < 4; i++)
      if (paths[i] && !read_text(paths[i], buf, sizeof(buf))) nr_core_parse_uhd(buf, t->dpdk);
  }
  return 0;
}

static int env_cpu(const char *name)
{
  const char *e = getenv(name);
  if (!e || !*e) return -1;
  char *end;
  const long v = strtol(e, &end, 10);
  return end == e || v < 0 || v >= NR_CL_MAX_CPUS ? -1 : (int)v;
}
static bool env_list(const char *name, unsigned char *m)
{
  const char *e = getenv(name);
  unsigned char tmp[NR_CL_MAX_CPUS];
  if (!e || !*e || nr_core_parse_list(e, tmp)) return false;
  for (int c = 0; c < NR_CL_MAX_CPUS; c++) m[c] |= tmp[c];
  return true;
}
void nr_cl_over_from_env(nr_cl_over_t *o)
{
  nr_cl_over_none(o);
  o->rt = env_cpu("ISAC_UE_RT_CORE");
  o->ring = env_cpu("ISAC_USRP_RING_CORE");
  o->gpu = env_cpu("ISAC_GPU_WORKER_CORE");
  o->uss = env_cpu("ISAC_PDCCH_USS_CORE");
  o->nid = env_cpu("ISAC_PDCCH_NID_GPU_CORE");
  o->dash = env_cpu("ISAC_DASHBOARD_CORE");
  o->has_uhd = env_list("ISAC_UHD_CTRL_CORES", o->uhd);
  o->has_pool = env_list("ISAC_PDSCH_CORES", o->pool);
  o->has_pool |= env_list("ISAC_PUSCH_CORES", o->pool);
  o->has_lane = env_list("ISAC_FLOAT_CORES", o->lane);
  o->has_check = env_list("ISAC_GPU_CHECK_CORES", o->check);
}

/* ---------------------------------------------------------------- process-wide plan */
static nr_cl_topo_t g_topo;
static nr_core_layout_t g_lay;
static bool g_active;

static void activate(const nr_cl_topo_t *t, const nr_cl_over_t *o, int n_cells, bool feed)
{
  if (t != &g_topo) g_topo = *t;
  nr_cl_build_nf(&g_topo, o, n_cells, feed, &g_lay);
  g_active = true;
}
void nr_core_layout_set_for_test(const nr_cl_topo_t *t, const nr_cl_over_t *o) { activate(t, o, 1, false); }

static const char *const g_keys[] = {"ISAC_UE_RT_CORE", "ISAC_PDSCH_CORES", "ISAC_PUSCH_CORES", "ISAC_FLOAT_CORES", "ISAC_GPU_CHECK_CORES",
                                     "ISAC_GPU_WORKER_CORE", "ISAC_PDCCH_USS_CORE", "ISAC_PDCCH_NID_GPU_CORE", "ISAC_DASHBOARD_CORE",
                                     "ISAC_UHD_CTRL_CORES", "ISAC_USRP_RING_CORE"};
static bool g_pin_nocap;
void nr_core_layout_init(bool passive, bool radio) { nr_core_layout_init_n(passive, radio, 1); }
void nr_core_layout_init_n(bool passive, bool radio, int n_cells) { nr_core_layout_init_nf(passive, radio, n_cells, false); }
int nr_core_layout_feed_cpu(void) { return g_active && g_lay.feed_req ? g_lay.feed : -1; }
void nr_core_layout_init_nf(bool passive, bool radio, int n_cells, bool feed)
{
  if (!passive || g_active) return;
  const char *off = getenv("ISAC_CORE_PLAN");
  if (off && !strcmp(off, "0")) {
    fprintf(stderr, "SENSING: CORE_PLAN off (ISAC_CORE_PLAN=0): legacy site core defaults\n");
    return;
  }
  static nr_cl_over_t o;
  nr_cl_topo_read(&g_topo, radio);
  nr_cl_over_from_env(&o);
  activate(&g_topo, &o, n_cells, feed && n_cells > 1);
  const char *pn = getenv("ISAC_CORE_PIN_NOCAP");
  g_pin_nocap = !(pn && !strcmp(pn, "0"));
  char line[4096];
  nr_cl_format(&g_topo, &g_lay, line, sizeof(line));
  fprintf(stderr, "%s\n", line);
  /* every reader of these keys (getenv, nr_env_core, the radio library) sees the plan; explicit values stay */
  char ex[512];
  size_t eo = 0;
  ex[0] = 0;
  for (unsigned i = 0; i < sizeof(g_keys) / sizeof(g_keys[0]); i++) {
    if (getenv(g_keys[i])) continue;
    if (!radio && (!strcmp(g_keys[i], "ISAC_UHD_CTRL_CORES") || !strcmp(g_keys[i], "ISAC_USRP_RING_CORE"))) continue;
    const char *v = nr_core_layout_env_default(g_keys[i]);
    const bool list = !strcmp(g_keys[i], "ISAC_PDSCH_CORES") || !strcmp(g_keys[i], "ISAC_PUSCH_CORES");
    if (!v || (!*v && !list)) continue; /* planned unpinned: leave unset, the reader then uses the process (float lane) mask */
    setenv(g_keys[i], v, 0);
    if (eo + strlen(g_keys[i]) + strlen(v) + 4 < sizeof(ex)) eo += (size_t)snprintf(ex + eo, sizeof(ex) - eo, " %s=%s", g_keys[i] + 5, v);
  }
  fprintf(stderr, "SENSING: CORE_PLAN exported:%s\n", eo ? ex : " none");
  if (!g_topo.rt_capable)
    fprintf(stderr,
            "SENSING: CORE_PLAN warning: no CAP_SYS_NICE and RLIMIT_RTPRIO=0: every thread runs SCHED_OTHER (no RT priority); "
            "the plan is applied as CPU affinity only%s\n",
            g_pin_nocap ? "" : " -- disabled by ISAC_CORE_PIN_NOCAP=0, threads float");
}
bool nr_core_layout_pin_nocap(void) { return g_active && g_pin_nocap; }
const nr_core_layout_t *nr_core_layout(void) { return g_active ? &g_lay : NULL; }
bool nr_core_layout_active(void) { return g_active; }

int nr_core_layout_filter_cpu(int core, const char *who)
{
  if (!g_active || core < 0) return core;
  if (core < NR_CL_MAX_CPUS && g_lay.usable[core]) return core;
  static _Atomic int logged;
  if (atomic_fetch_add(&logged, 1) < 16)
    fprintf(stderr, "SENSING: CORE_PLAN %s: core %d is not usable on this host (offline or outside the cpuset); left unpinned\n", who ? who : "thread", core);
  return -1;
}

const char *nr_core_layout_env_default(const char *name)
{
  if (!g_active || !name) return NULL;
  static char pool[256], uhd[256], lane[256], chk[256], one[6][16];
  const nr_core_layout_t *L = &g_lay;
  const int n = g_topo.ncpu > NR_CL_MAX_CPUS ? NR_CL_MAX_CPUS : g_topo.ncpu;
  if (!strcmp(name, "ISAC_PDSCH_CORES")) {
    pool[0] = 0;
    for (int i = 0, o = 0; i < L->npool && o + 8 < (int)sizeof(pool); i++) o += snprintf(pool + o, sizeof(pool) - (size_t)o, "%s%d", i ? "," : "", L->pool[i]);
    return pool;
  }
  if (!strcmp(name, "ISAC_PUSCH_CORES")) return ""; /* one pool: the PDSCH list carries it */
  if (!strcmp(name, "ISAC_UHD_CTRL_CORES")) {
    if (L->uhd < 0) return "";
    nr_cl_mask_str(L->uhd_mask, n, uhd, sizeof(uhd));
    return uhd;
  }
  if (!strcmp(name, "ISAC_FLOAT_CORES")) return nr_cl_mask_str(L->lane, n, lane, sizeof(lane)) ? lane : "";
  if (!strcmp(name, "ISAC_GPU_CHECK_CORES")) return nr_cl_mask_str(L->check, n, chk, sizeof(chk)) ? chk : "";
  const struct { const char *k; int v; } k1[] = {{"ISAC_UE_RT_CORE", L->rt},          {"ISAC_USRP_RING_CORE", L->ring},
                                                 {"ISAC_GPU_WORKER_CORE", L->gpu},     {"ISAC_PDCCH_USS_CORE", L->uss},
                                                 {"ISAC_PDCCH_NID_GPU_CORE", L->nid},  {"ISAC_DASHBOARD_CORE", L->dashboard}};
  for (unsigned i = 0; i < sizeof(k1) / sizeof(k1[0]); i++)
    if (!strcmp(name, k1[i].k)) {
      if (k1[i].v < 0) return strcmp(name, "ISAC_PDCCH_NID_GPU_CORE") ? "" : "-1"; /* nID: -1 = highest CPU of the lane */
      snprintf(one[i], sizeof(one[i]), "%d", k1[i].v);
      return one[i];
    }
  return NULL;
}
