/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GrantWork-lite: see nr_td_grantwork.h. */
#define _GNU_SOURCE
#include "nr_td_grantwork.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_segment/nr_rate_matching.h"
#include "common/utils/LOG/log.h"
#include "nr_llr_norm.h"

/* ---------------------------------------------------------------------------------------------
 * Allocator. Every buffer carries a 64-byte header {magic, kind, capacity}; the payload is 64-byte
 * aligned. Freed buffers go to a small size-class pool (powers of two from 4 KiB), so the steady state
 * makes no allocation at all -- cudaMallocManaged costs tens of microseconds and must stay off the RT
 * path. */
#define GW_HDR 64
#define GW_MAGIC 0x47574246u /* "GWBF" */
#define GW_POOL_CLASSES 24
#define GW_POOL_DEPTH 32
typedef struct {
  uint32_t magic;
  uint32_t kind; /* 0 = malloc, 1 = cudaMallocManaged */
  size_t cap;    /* payload bytes */
} gw_hdr_t;
_Static_assert(sizeof(gw_hdr_t) <= GW_HDR, "header fits");

static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_pool[GW_POOL_CLASSES][GW_POOL_DEPTH];
static int g_pool_n[GW_POOL_CLASSES];
static _Atomic uint64_t g_alloc_fresh, g_alloc_reused, g_alloc_unified;

typedef int (*cuda_malloc_managed_fn)(void **, size_t, unsigned int);
typedef int (*cuda_free_fn)(void *);
static pthread_once_t g_cuda_once = PTHREAD_ONCE_INIT;
static cuda_malloc_managed_fn g_cuda_mm;
static cuda_free_fn g_cuda_free;
static bool g_unified;

static void cuda_init(void)
{
#ifdef NR_TD_GW_CUDA
  const char *e = getenv("ISAC_TD_GW_UNIFIED");
  if (e != NULL && atoi(e) == 0)
    return;
  static const char *const names[] = {"libcudart.so", "libcudart.so.13", "libcudart.so.12", "libcudart.so.11.0"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !g_unified; i++) {
    void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (h == NULL)
      continue;
    g_cuda_mm = (cuda_malloc_managed_fn)dlsym(h, "cudaMallocManaged");
    g_cuda_free = (cuda_free_fn)dlsym(h, "cudaFree");
    if (g_cuda_mm == NULL || g_cuda_free == NULL)
      continue;
    void *probe = NULL;
    if (g_cuda_mm(&probe, 4096, 1 /* cudaMemAttachGlobal */) == 0 && probe != NULL) {
      g_cuda_free(probe);
      g_unified = true;
    }
  }
#endif
}

bool nr_td_gw_unified(void)
{
  pthread_once(&g_cuda_once, cuda_init);
  return g_unified;
}

static int pool_class(size_t bytes)
{
  size_t c = 4096;
  int k = 0;
  while (c < bytes && k < GW_POOL_CLASSES - 1) {
    c <<= 1;
    k++;
  }
  return c >= bytes ? k : -1;
}

void *nr_td_gw_alloc(size_t bytes)
{
  if (bytes == 0)
    bytes = 1;
  const int k = pool_class(bytes);
  if (k >= 0) {
    pthread_mutex_lock(&g_pool_lock);
    if (g_pool_n[k] > 0) {
      void *p = g_pool[k][--g_pool_n[k]];
      pthread_mutex_unlock(&g_pool_lock);
      atomic_fetch_add(&g_alloc_reused, 1);
      return p;
    }
    pthread_mutex_unlock(&g_pool_lock);
  }
  const size_t cap = k >= 0 ? ((size_t)4096 << k) : bytes;
  uint8_t *base = NULL;
  uint32_t kind = 0;
  if (nr_td_gw_unified() && g_cuda_mm((void **)&base, cap + GW_HDR, 1) == 0 && base != NULL) {
    kind = 1;
    atomic_fetch_add(&g_alloc_unified, 1);
  } else {
    base = NULL;
    if (posix_memalign((void **)&base, GW_HDR, cap + GW_HDR) != 0)
      return NULL;
  }
  gw_hdr_t *h = (gw_hdr_t *)base;
  h->magic = GW_MAGIC;
  h->kind = kind;
  h->cap = cap;
  atomic_fetch_add(&g_alloc_fresh, 1);
  return base + GW_HDR;
}

void nr_td_gw_free(void *p)
{
  if (p == NULL)
    return;
  uint8_t *base = (uint8_t *)p - GW_HDR;
  gw_hdr_t *h = (gw_hdr_t *)base;
  if (h->magic != GW_MAGIC)
    abort(); /* not ours: a double free or a foreign pointer */
  const int k = pool_class(h->cap);
  if (k >= 0 && ((size_t)4096 << k) == h->cap) {
    pthread_mutex_lock(&g_pool_lock);
    if (g_pool_n[k] < GW_POOL_DEPTH) {
      g_pool[k][g_pool_n[k]++] = p;
      pthread_mutex_unlock(&g_pool_lock);
      return;
    }
    pthread_mutex_unlock(&g_pool_lock);
  }
  h->magic = 0;
  if (h->kind == 1)
    g_cuda_free(base);
  else
    free(base);
}

void nr_td_gw_alloc_stats(uint64_t *fresh, uint64_t *reused, uint64_t *unified)
{
  *fresh = atomic_load(&g_alloc_fresh);
  *reused = atomic_load(&g_alloc_reused);
  *unified = atomic_load(&g_alloc_unified);
}

/* ---------------------------------------------------------------------------------------------
 * GrantWork. */
enum { E_EMPTY = 0, E_COMPUTING = 1, E_READY = 2, E_FAILED = 3 };

typedef struct {
  uint64_t sig;
  int state;
  pthread_t owner;
  int computes; /* computations started (diagnostic; 1 at most by construction) */
  int status;
  int16_t *llr; /* nr_td_gw_alloc'd */
  uint32_t G;
  uint8_t nl, qm;
  uint8_t meta[NR_TD_GW_META_MAX];
  uint32_t meta_len;
  uint64_t t0_ns, compute_ns;
} gw_entry_t;

struct nr_td_grantwork_s {
  _Atomic int refcount;
  long abs_slot;
  int spf;
  void *fep;
  const uint64_t *fep_gen_src;
  uint64_t fep_gen;
  uint16_t fep_mask;
  double fep_fo;
  pthread_t job_thread;
  _Atomic bool job_ended;
  _Atomic uint32_t flags;
  nr_td_gw_compute_fn compute;
  void *ctx;
  pthread_mutex_t lock;         /* entry table */
  pthread_cond_t cv;            /* an entry left COMPUTING */
  pthread_mutex_t compute_lock; /* recursive: one computation at a time (shared FEP buffer) */
  int n;
  gw_entry_t e[NR_TD_GW_MAX_SIG];
};

nr_td_grantwork_t *nr_td_grantwork_begin(const nr_td_gw_job_t *job)
{
  if (job == NULL || job->slots_per_frame <= 0 || (job->ctx_len > 0 && job->ctx == NULL))
    return NULL;
  nr_td_grantwork_t *gw = calloc(1, sizeof(*gw));
  if (gw == NULL)
    return NULL;
  if (job->ctx_len > 0) {
    gw->ctx = malloc(job->ctx_len);
    if (gw->ctx == NULL) {
      free(gw);
      return NULL;
    }
    memcpy(gw->ctx, job->ctx, job->ctx_len);
  }
  atomic_init(&gw->refcount, 1);
  gw->abs_slot = job->abs_slot;
  gw->spf = job->slots_per_frame;
  gw->fep = job->fep;
  gw->fep_gen_src = job->fep_gen_src;
  gw->fep_gen = job->fep_gen_src ? __atomic_load_n(job->fep_gen_src, __ATOMIC_ACQUIRE) : 0;
  gw->compute = job->compute;
  gw->job_thread = pthread_self();
  pthread_mutex_init(&gw->lock, NULL);
  pthread_cond_init(&gw->cv, NULL);
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&gw->compute_lock, &a);
  pthread_mutexattr_destroy(&a);
  return gw;
}

nr_td_grantwork_t *nr_td_grantwork_retain(nr_td_grantwork_t *gw)
{
  if (gw)
    atomic_fetch_add(&gw->refcount, 1);
  return gw;
}

void nr_td_grantwork_release(nr_td_grantwork_t *gw)
{
  if (gw == NULL || atomic_fetch_sub(&gw->refcount, 1) != 1)
    return;
  for (int i = 0; i < gw->n; i++)
    nr_td_gw_free(gw->e[i].llr);
  pthread_mutex_destroy(&gw->compute_lock);
  pthread_cond_destroy(&gw->cv);
  pthread_mutex_destroy(&gw->lock);
  free(gw->ctx);
  free(gw);
}

int nr_td_grantwork_refcount(const nr_td_grantwork_t *gw) { return gw ? atomic_load(&gw->refcount) : 0; }
void *nr_td_grantwork_fep(const nr_td_grantwork_t *gw) { return gw->fep; }
long nr_td_grantwork_abs_slot(const nr_td_grantwork_t *gw) { return gw->abs_slot; }
int nr_td_grantwork_slots_per_frame(const nr_td_grantwork_t *gw) { return gw->spf; }
const void *nr_td_grantwork_ctx(const nr_td_grantwork_t *gw) { return gw->ctx; }
bool nr_td_grantwork_fep_alive(const nr_td_grantwork_t *gw)
{
  return gw->fep_gen_src == NULL || __atomic_load_n(gw->fep_gen_src, __ATOMIC_ACQUIRE) == gw->fep_gen;
}
uint16_t nr_td_grantwork_fep_mask(const nr_td_grantwork_t *gw, double fo_hz)
{
  return (gw->fep_fo == fo_hz && nr_td_grantwork_fep_alive(gw)) ? gw->fep_mask : 0;
}
void nr_td_grantwork_fep_done(nr_td_grantwork_t *gw, double fo_hz, uint16_t mask)
{
  if (gw->fep_fo != fo_hz)
    gw->fep_mask = 0;
  gw->fep_fo = fo_hz;
  gw->fep_mask |= mask;
}
void nr_td_grantwork_flag(nr_td_grantwork_t *gw, uint32_t bits)
{
  if (gw)
    atomic_fetch_or(&gw->flags, bits);
}
uint32_t nr_td_grantwork_flags(const nr_td_grantwork_t *gw)
{
  return gw ? atomic_load(&((nr_td_grantwork_t *)gw)->flags) : 0;
}
static _Atomic uint64_t g_refused;
void nr_td_grantwork_job_end(nr_td_grantwork_t *gw)
{
  if (gw)
    atomic_store(&gw->job_ended, true);
}
uint64_t nr_td_grantwork_refused_count(void) { return atomic_load(&g_refused); }
static bool may_compute(const nr_td_grantwork_t *gw)
{
  return pthread_equal(gw->job_thread, pthread_self()) && !atomic_load(&((nr_td_grantwork_t *)gw)->job_ended);
}

static gw_entry_t *find(nr_td_grantwork_t *gw, uint64_t sig)
{
  for (int i = 0; i < gw->n; i++)
    if (gw->e[i].sig == sig)
      return &gw->e[i];
  return NULL;
}

static void fill_view(const gw_entry_t *e, nr_td_gw_llr_view_t *out)
{
  if (out == NULL)
    return;
  out->sig = e->sig;
  out->llr = e->llr;
  out->G = e->G;
  out->nl = e->nl;
  out->qm = e->qm;
  out->status = e->status;
  out->meta = e->meta_len ? e->meta : NULL;
  out->meta_len = e->meta_len;
  out->compute_ns = e->compute_ns;
}

/* Under gw->lock. Waits out another thread's computation. Returns the entry (created EMPTY when absent)
 * or NULL (table full; *err set). */
static gw_entry_t *lookup_wait(nr_td_grantwork_t *gw, uint64_t sig, int *err)
{
  for (;;) {
    gw_entry_t *e = find(gw, sig);
    if (e == NULL) {
      if (gw->n >= NR_TD_GW_MAX_SIG) {
        *err = NR_TD_GW_E_FULL;
        return NULL;
      }
      e = &gw->e[gw->n++];
      memset(e, 0, sizeof(*e));
      e->sig = sig;
      e->state = E_EMPTY;
      return e;
    }
    if (e->state != E_COMPUTING || pthread_equal(e->owner, pthread_self()))
      return e;
    pthread_cond_wait(&gw->cv, &gw->lock);
  }
}

static void claim(gw_entry_t *e)
{
  e->state = E_COMPUTING;
  e->owner = pthread_self();
  e->computes++;
  e->t0_ns = nr_td_gw_now_ns();
}

nr_td_gw_acq_t nr_td_grantwork_acquire(nr_td_grantwork_t *gw, uint64_t sig, nr_td_gw_llr_view_t *out)
{
  if (gw == NULL)
    return NR_TD_GW_ACQ_ERR;
  int err = 0;
  pthread_mutex_lock(&gw->lock);
  gw_entry_t *e = lookup_wait(gw, sig, &err);
  nr_td_gw_acq_t r;
  bool claimed = false;
  if (e == NULL) {
    atomic_fetch_or(&gw->flags, NR_TD_GW_F_FULL);
    r = NR_TD_GW_ACQ_ERR;
  } else if (e->state == E_READY) {
    fill_view(e, out);
    r = NR_TD_GW_ACQ_READY;
  } else if (e->state == E_FAILED) {
    fill_view(e, out);
    r = NR_TD_GW_ACQ_FAILED;
  } else if (e->state == E_COMPUTING) { /* claimed by this very thread (lazy path re-entering) */
    r = NR_TD_GW_ACQ_OWNER;
  } else if (!nr_td_grantwork_fep_alive(gw) || !may_compute(gw)) {
    if (!nr_td_grantwork_fep_alive(gw))
      atomic_fetch_or(&gw->flags, NR_TD_GW_F_STALE);
    else
      atomic_fetch_add(&g_refused, 1);
    gw->n--; /* the EMPTY slot just created (always the last one) */
    r = NR_TD_GW_ACQ_ERR;
  } else {
    claim(e);
    claimed = true;
    r = NR_TD_GW_ACQ_OWNER;
  }
  pthread_mutex_unlock(&gw->lock);
  if (claimed) /* one compute-lock level per claim, released by publish / abandon */
    pthread_mutex_lock(&gw->compute_lock);
  return r;
}

static void finish(nr_td_grantwork_t *gw, gw_entry_t *e)
{
  e->compute_ns = nr_td_gw_now_ns() - e->t0_ns;
  pthread_cond_broadcast(&gw->cv);
}

int nr_td_grantwork_publish(nr_td_grantwork_t *gw, uint64_t sig, const int16_t *llr, uint32_t G, uint8_t nl, uint8_t qm,
                            const void *meta, uint32_t meta_len)
{
  if (gw == NULL || (G > 0 && llr == NULL) || meta_len > NR_TD_GW_META_MAX)
    return NR_TD_GW_E_ARG;
  int16_t *buf = nr_td_gw_alloc((size_t)(G ? G : 1) * sizeof(int16_t));
  if (buf == NULL)
    return NR_TD_GW_E_NOMEM;
  memcpy(buf, llr, (size_t)G * sizeof(int16_t));
  pthread_mutex_lock(&gw->lock);
  gw_entry_t *e = find(gw, sig);
  if (e == NULL || e->state != E_COMPUTING || !pthread_equal(e->owner, pthread_self())) {
    pthread_mutex_unlock(&gw->lock);
    nr_td_gw_free(buf);
    return NR_TD_GW_E_ARG;
  }
  e->llr = buf;
  e->G = G;
  e->nl = nl;
  e->qm = qm;
  e->meta_len = meta_len;
  if (meta_len)
    memcpy(e->meta, meta, meta_len);
  e->state = E_READY;
  finish(gw, e);
  pthread_mutex_unlock(&gw->lock);
  pthread_mutex_unlock(&gw->compute_lock);
  if (nr_td_gw_tim_on())
    nr_td_gw_tim_add(NR_TD_GWTIM_SHARED, e->compute_ns);
  return NR_TD_GW_OK;
}

/* Marks a still-COMPUTING entry owned by this thread FAILED. Returns true when it did. */
static bool fail_owned(nr_td_grantwork_t *gw, uint64_t sig, int status)
{
  pthread_mutex_lock(&gw->lock);
  gw_entry_t *e = find(gw, sig);
  const bool mine = e && e->state == E_COMPUTING && pthread_equal(e->owner, pthread_self());
  if (mine) {
    e->state = E_FAILED;
    e->status = status;
    finish(gw, e);
  }
  pthread_mutex_unlock(&gw->lock);
  return mine;
}

void nr_td_grantwork_abandon(nr_td_grantwork_t *gw, uint64_t sig, int status)
{
  if (gw && fail_owned(gw, sig, status))
    pthread_mutex_unlock(&gw->compute_lock);
}

int nr_td_grantwork_get_llr(nr_td_grantwork_t *gw, uint64_t sig, const void *hint, nr_td_gw_llr_view_t *out)
{
  if (gw == NULL || out == NULL)
    return NR_TD_GW_E_ARG;
  memset(out, 0, sizeof(*out));
  int err = 0;
  pthread_mutex_lock(&gw->lock);
  gw_entry_t *e = lookup_wait(gw, sig, &err);
  if (e == NULL) {
    pthread_mutex_unlock(&gw->lock);
    atomic_fetch_or(&gw->flags, NR_TD_GW_F_FULL);
    return err;
  }
  if (e->state == E_READY || e->state == E_FAILED) {
    fill_view(e, out);
    pthread_mutex_unlock(&gw->lock);
    if (e->state == E_READY && nr_td_gw_tim_on())
      nr_td_gw_tim_add(NR_TD_GWTIM_HIT, 0);
    return e->state == E_READY ? NR_TD_GW_OK : NR_TD_GW_E_FAILED;
  }
  if (e->state == E_COMPUTING) { /* this thread is mid-computation of the same entry: a caller bug */
    pthread_mutex_unlock(&gw->lock);
    return NR_TD_GW_E_ARG;
  }
  if (gw->compute == NULL || !nr_td_grantwork_fep_alive(gw) || !may_compute(gw)) {
    int rc = NR_TD_GW_E_NOCOMPUTE;
    if (gw->compute != NULL && !nr_td_grantwork_fep_alive(gw)) {
      rc = NR_TD_GW_E_STALE;
      atomic_fetch_or(&gw->flags, NR_TD_GW_F_STALE);
    } else if (gw->compute != NULL) {
      rc = NR_TD_GW_E_NOTOWNER;
      atomic_fetch_add(&g_refused, 1);
    }
    gw->n--; /* drop the EMPTY slot just created */
    pthread_mutex_unlock(&gw->lock);
    return rc;
  }
  claim(e);
  pthread_mutex_unlock(&gw->lock);

  pthread_mutex_lock(&gw->compute_lock);
  const int rc = gw->compute(gw->ctx, gw, sig, hint);
  /* The claim's lock level is released by whoever ends the computation: publish / abandon inside the
   * callback (directly or via the decoder's re-entrant acquire), or here when the callback returned with
   * the entry still COMPUTING -- it then fails with the callback's status. */
  if (fail_owned(gw, sig, rc != 0 ? rc : NR_TD_GW_E_FAILED))
    pthread_mutex_unlock(&gw->compute_lock);

  pthread_mutex_lock(&gw->lock);
  e = find(gw, sig);
  fill_view(e, out);
  const int st = e->state;
  pthread_mutex_unlock(&gw->lock);
  return st == E_READY ? NR_TD_GW_OK : NR_TD_GW_E_FAILED;
}

int nr_td_grantwork_compute_count(const nr_td_grantwork_t *gw, uint64_t sig)
{
  nr_td_grantwork_t *g = (nr_td_grantwork_t *)gw;
  pthread_mutex_lock(&g->lock);
  const gw_entry_t *e = find(g, sig);
  const int c = e ? e->computes : 0;
  pthread_mutex_unlock(&g->lock);
  return c;
}

int nr_td_grantwork_n_entries(const nr_td_grantwork_t *gw)
{
  nr_td_grantwork_t *g = (nr_td_grantwork_t *)gw;
  pthread_mutex_lock(&g->lock);
  const int n = g->n;
  pthread_mutex_unlock(&g->lock);
  return n;
}

uint64_t nr_td_grantwork_key(const nr_pdsch_cfg_hypothesis_t *h, int nl, int qm)
{
  nr_pdsch_cfg_hypothesis_t c = *h;
  if (c.dmrs_mask != 0) {
    c.mapping_type = 0;
    c.dmrs_add_pos = 0;
    c.dmrs_max_len = 0;
  }
  c.mcs_table = 0;
  return nr_td_signature(&c, nl, qm);
}

/* ---------------------------------------------------------------------------------------------
 * CB0 extraction: the r = 0 work of nrLDPC_coding_segment_decoder.c (nr_process_decode_segment). */
int nr_td_gw_cb0_extract(const int16_t *llr, uint32_t n_llr, const nr_td_cb0_params_t *p, int16_t *d)
{
  if (llr == NULL || p == NULL || d == NULL || p->C < 1 || p->C > 255 || (p->BG != 1 && p->BG != 2) || p->Z <= 0
      || p->Qm <= 0 || p->E == 0 || p->E > n_llr || p->K - p->F - 2 * p->Z < 0 || p->rv < 0 || p->rv > 3)
    return NR_TD_GW_E_ARG;
  static __thread int16_t *e_buf = NULL;
  static __thread uint32_t e_cap = 0;
  if (e_cap < p->E) {
    free(e_buf);
    e_buf = malloc((size_t)p->E * sizeof(int16_t));
    e_cap = e_buf ? p->E : 0;
    if (e_buf == NULL)
      return NR_TD_GW_E_NOMEM;
  }
  memset(d, 0, (size_t)nr_td_gw_cb0_dlen(p) * sizeof(int16_t));
  nr_deinterleaving_ldpc(p->E, (uint8_t)p->Qm, e_buf, (int16_t *)llr);
  if (nr_rate_matching_ldpc_rx(p->tbslbrm, (uint8_t)p->BG, (uint16_t)p->Z, d, e_buf, (uint8_t)p->C, (uint8_t)p->rv, 1, p->E,
                               (uint32_t)p->F, (uint32_t)(p->K - p->F - 2 * p->Z))
      == -1)
    return NR_TD_GW_E_RM;
  return NR_TD_GW_OK;
}

int nr_td_gw_cb0_input(const int16_t *llr, uint32_t G, const nr_td_cb0_params_t *p, bool norm, int16_t *e0, int16_t *d,
                       int *k_out)
{
  if (llr == NULL || p == NULL || e0 == NULL || p->E == 0 || p->E > G)
    return NR_TD_GW_E_ARG;
  int k = -1;
  if (norm && G >= 64) /* the decode's ISAC_LLR_NORM block, with this hypothesis's own C (K38) */
    k = nr_llr_norm_shift(llr, nr_llr_norm_span(G, nr_llr_norm_num_cb(p->A, p->BG)));
  if (k > 0)
    for (uint32_t i = 0; i < p->E; i++)
      e0[i] = (int16_t)(llr[i] >> k);
  else
    memcpy(e0, llr, (size_t)p->E * sizeof(int16_t));
  if (k_out)
    *k_out = k;
  return nr_td_gw_cb0_extract(e0, p->E, p, d);
}

/* ---------------------------------------------------------------------------------------------
 * GWTIM. */
static const char *const kGwTimName[NR_TD_GWTIM_N] = {"shared", "fep", "chest", "demod", "hit", "cb0_rm", "cb0_ldpc"};
static _Atomic uint64_t g_tim_ns[NR_TD_GWTIM_N], g_tim_n[NR_TD_GWTIM_N], g_tim_max[NR_TD_GWTIM_N];
static _Atomic int g_tim_on = -1;

bool nr_td_gw_tim_on(void)
{
  if (g_tim_on < 0)
    g_tim_on = getenv("ISAC_PDCCH_TIMING") != NULL ? 1 : 0;
  return g_tim_on > 0;
}

uint64_t nr_td_gw_now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void nr_td_gw_tim_add(int k, uint64_t ns)
{
  if (k < 0 || k >= NR_TD_GWTIM_N)
    return;
  atomic_fetch_add(&g_tim_ns[k], ns);
  const uint64_t n = atomic_fetch_add(&g_tim_n[k], 1) + 1;
  uint64_t m = atomic_load(&g_tim_max[k]);
  while (ns > m && !atomic_compare_exchange_weak(&g_tim_max[k], &m, ns)) {
  }
  if (k == NR_TD_GWTIM_CB0_LDPC && (n % 200) == 0)
    nr_td_gw_tim_report(true);
}

void nr_td_gw_tim_report(bool force)
{
  if (!nr_td_gw_tim_on() || !force)
    return;
  char rep[640];
  int u = 0;
  for (int k = 0; k < NR_TD_GWTIM_N && u < (int)sizeof(rep) - 90; k++) {
    const uint64_t n = atomic_load(&g_tim_n[k]);
    u += snprintf(rep + u, sizeof(rep) - u, "%s[n=%lu mean=%.1fus max=%.1fus] ", kGwTimName[k], (unsigned long)n,
                  n ? (double)atomic_load(&g_tim_ns[k]) / (double)n / 1000.0 : 0.0,
                  (double)atomic_load(&g_tim_max[k]) / 1000.0);
  }
  uint64_t f, r, un;
  nr_td_gw_alloc_stats(&f, &r, &un);
  LOG_I(PHY, "SENSING: GWTIM %s| alloc fresh=%lu reused=%lu unified=%lu (%s)\n", rep, (unsigned long)f, (unsigned long)r,
        (unsigned long)un, nr_td_gw_unified() ? "cudaMallocManaged" : "malloc");
}
