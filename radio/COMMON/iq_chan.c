/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI C2: in-process channelizer, see iq_chan.h. The block loop is the logic of the old shm_iq_chan.c run_live() (same overrun / discontinuity / epoch rules)
 * without the process, the plan.json and the GPU pipeline; the work is behind iq_chan_backend_t. */
#define _GNU_SOURCE
#include "iq_chan.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ALIGN_SAMPLES 16 /* positions stay multiples of the largest decimation, so any entry can be added at any time */

static uint64_t now_ns(void) { return shm_iq_now_ns(); }

static int decim_ok(int d) { return d >= 1 && d <= 16 && !(d & (d - 1)); }

static void *alloc_samples(size_t n_samples)
{
  size_t bytes = (n_samples * 4 + 63) & ~(size_t)63;
  if (!bytes) bytes = 64;
  return aligned_alloc(64, bytes);
}

/* ================================================================ CPU backend */
typedef struct {
  iq_ddc_t d[SHM_IQ_MAX_CH];
  unsigned nch;
} cpu_cell_t;

/* Parallel execution. A job = one (cell, channel) iq_ddc; its filter history and mixer phase live in that iq_ddc only, the input block is read-only and every job
 * writes its own output buffer, so jobs are independent and the result does not depend on which thread runs which job (bit-identical to k = 1). The caller of
 * run() is worker 0; k - 1 extra threads wait for a new block. Per block the caller writes the job list, sets `pending`, and publishes one 64-bit word
 *   W = generation << 20 | njobs << 10 | next_index
 * with a release store; every worker claims jobs with fetch_add(W, 1) and a claim is valid only if its index < the njobs carried IN THE SAME WORD, so a worker that wakes
 * late (or is still leaving the previous block) can never run a job of the wrong block, and the caller waits only for jobs that were really claimed (`pending`), never for a
 * worker that is not scheduled. The job list is double-buffered by generation parity; the caller rewrites a buffer only after pending reached 0. Nothing is
 * allocated per block. Idle workers spin briefly (yield), then sleep 50 us. install/retire/set_shift/reset run on the caller between blocks, when no job is claimed. */
#define CPU_MAX_WORKERS 64
#define W_GEN(v) ((v) >> 20)
#define W_N(v) (((v) >> 10) & 1023u)
#define W_IDX(v) ((v) & 1023u)
typedef struct {
  iq_chan_geom_t g;
  cpu_cell_t *slot[IQ_CHAN_MAX_CELLS];
  int k; /* threads including the caller */
  pthread_t th[CPU_MAX_WORKERS];
  _Atomic uint64_t w;
  _Atomic int quit, pending, err;
  uint8_t job_slot[2][IQ_CHAN_MAX_CELLS * SHM_IQ_MAX_CH], job_ch[2][IQ_CHAN_MAX_CELLS * SHM_IQ_MAX_CH];
  const int16_t *const *in;
  int16_t *const *outp;
  int n;
} cpu_ctx_t;

static void run_job(cpu_ctx_t *x, int par, int j)
{
  cpu_cell_t *c = x->slot[x->job_slot[par][j]];
  const unsigned a = x->job_ch[par][j];
  if (iq_ddc_process(&c->d[a], x->in[a], x->n, x->outp[x->job_slot[par][j] * x->g.nch + a]) != x->n / c->d[a].decim) atomic_store(&x->err, 1);
}

static void drain_jobs(cpu_ctx_t *x)
{
  for (;;) {
    const uint64_t v = atomic_fetch_add_explicit(&x->w, 1, memory_order_acq_rel);
    if (W_IDX(v) >= W_N(v)) return;
    run_job(x, (int)(W_GEN(v) & 1), (int)W_IDX(v));
    atomic_fetch_sub_explicit(&x->pending, 1, memory_order_acq_rel);
  }
}

static void *worker_main(void *arg)
{
  cpu_ctx_t *x = arg;
  uint64_t seen = W_GEN(atomic_load(&x->w));
  int spins = 0;
  for (;;) {
    const uint64_t g = W_GEN(atomic_load_explicit(&x->w, memory_order_acquire));
    if (g != seen) { seen = g; spins = 0; drain_jobs(x); continue; }
    if (atomic_load(&x->quit)) return NULL;
    if (++spins < 4000) sched_yield();
    else { struct timespec w = {0, 50000}; nanosleep(&w, NULL); }
  }
}

static void stop_workers(cpu_ctx_t *x)
{
  if (x->k > 1) {
    atomic_store(&x->quit, 1);
    for (int i = 1; i < x->k; ++i) pthread_join(x->th[i], NULL);
  }
  x->k = 1;
  atomic_store(&x->quit, 0);
}

/* k threads including the caller; cpus (ncpus entries, may be NULL) pin worker i to cpus[i % ncpus] (the caller is pinned by iq_chan_start). 0 on success. */
static int cpu_set_workers(void *ctx, int k, const int *cpus, int ncpus)
{
  cpu_ctx_t *x = ctx;
  if (k < 1) k = 1;
  if (k > CPU_MAX_WORKERS) k = CPU_MAX_WORKERS;
  stop_workers(x);
  for (int i = 1; i < k; ++i) {
    pthread_attr_t at;
    pthread_attr_init(&at);
    if (cpus && ncpus > 0) {
      cpu_set_t cs;
      CPU_ZERO(&cs);
      CPU_SET(cpus[i % ncpus] >= 0 && cpus[i % ncpus] < CPU_SETSIZE ? cpus[i % ncpus] : 0, &cs);
      pthread_attr_setaffinity_np(&at, sizeof(cs), &cs);
    }
    const int rc = pthread_create(&x->th[i], &at, worker_main, x);
    pthread_attr_destroy(&at);
    if (rc) { x->k = i; stop_workers(x); return -1; }
  }
  x->k = k;
  return 0;
}

static void *cpu_create(const iq_chan_geom_t *g)
{
  cpu_ctx_t *x = calloc(1, sizeof(*x));
  if (x) x->g = *g;
  return x;
}

static void cpu_release(void *ctx, void *cell)
{
  (void)ctx;
  cpu_cell_t *c = cell;
  if (!c) return;
  for (unsigned a = 0; a < c->nch; ++a) iq_ddc_free(&c->d[a]);
  free(c);
}

static void cpu_destroy(void *ctx)
{
  cpu_ctx_t *x = ctx;
  if (!x) return;
  stop_workers(x);
  for (int s = 0; s < IQ_CHAN_MAX_CELLS; ++s) cpu_release(x, x->slot[s]);
  free(x);
}

static void *cpu_prepare(void *ctx, const iq_chan_entry_t *e)
{
  cpu_ctx_t *x = ctx;
  cpu_cell_t *c = calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->nch = x->g.nch;
  const double atten = e->atten_db > 0 ? e->atten_db : IQ_CHAN_DEFAULT_ATTEN_DB;
  for (unsigned a = 0; a < c->nch; ++a)
    if (iq_ddc_init(&c->d[a], x->g.in_rate, e->decim, e->passband_hz, atten)) {
      for (unsigned b = 0; b < a; ++b) iq_ddc_free(&c->d[b]);
      free(c);
      return NULL;
    }
  return c;
}

static void cpu_install(void *ctx, int slot, void *cell)
{
  cpu_ctx_t *x = ctx;
  x->slot[slot] = cell;
}

static void *cpu_retire(void *ctx, int slot)
{
  cpu_ctx_t *x = ctx;
  void *c = x->slot[slot];
  x->slot[slot] = NULL;
  return c;
}

static void cpu_set_shift(void *ctx, int slot, double shift_hz)
{
  cpu_cell_t *c = ((cpu_ctx_t *)ctx)->slot[slot];
  if (c) for (unsigned a = 0; a < c->nch; ++a) iq_ddc_set_shift(&c->d[a], shift_hz);
}

static void cpu_reset(void *ctx, int slot)
{
  cpu_ctx_t *x = ctx;
  for (int s = 0; s < IQ_CHAN_MAX_CELLS; ++s)
    if ((slot < 0 || slot == s) && x->slot[s])
      for (unsigned a = 0; a < x->slot[s]->nch; ++a) iq_ddc_reset(&x->slot[s]->d[a]);
}

static int cpu_run(void *ctx, const int16_t *const *in, int n, int16_t *const *out)
{
  cpu_ctx_t *x = ctx;
  const uint64_t gen = W_GEN(atomic_load_explicit(&x->w, memory_order_relaxed)) + 1;
  const int par = (int)(gen & 1);
  int nj = 0;
  for (int s = 0; s < IQ_CHAN_MAX_CELLS; ++s) {
    cpu_cell_t *c = x->slot[s];
    if (!c || !out[s * x->g.nch]) continue;
    for (unsigned a = 0; a < c->nch; ++a) { x->job_slot[par][nj] = (uint8_t)s; x->job_ch[par][nj] = (uint8_t)a; ++nj; }
  }
  x->in = in; x->outp = out; x->n = n;
  atomic_store(&x->err, 0);
  atomic_store_explicit(&x->pending, nj, memory_order_relaxed);
  atomic_store_explicit(&x->w, gen << 20 | (uint64_t)nj << 10, memory_order_release); /* publishes the job list, in/out/n, pending, err */
  drain_jobs(x);
  while (atomic_load_explicit(&x->pending, memory_order_acquire) > 0) __asm__ __volatile__("" ::: "memory"); /* only jobs that were claimed */
  return atomic_load(&x->err) ? -1 : 0;
}

static size_t cpu_capacity(const iq_chan_load_t *load)
{
  iq_chan_bench_t b;
  if (iq_chan_cpu_bench(load, 0, 0.25, &b)) return 0;
  return (size_t)b.samples_per_s;
}

const iq_chan_backend_t iq_chan_backend_cpu = {
  .name = "cpu", .create = cpu_create, .destroy = cpu_destroy, .prepare = cpu_prepare, .release = cpu_release, .install = cpu_install,
  .retire = cpu_retire, .set_shift = cpu_set_shift, .reset = cpu_reset, .run = cpu_run, .capacity = cpu_capacity, .set_workers = cpu_set_workers,
};

/* ---------------------------------------------------------------- bench */
int iq_chan_cpu_bench(const iq_chan_load_t *load, int block, double seconds, iq_chan_bench_t *out) { return iq_chan_cpu_bench_k(load, block, seconds, 1, NULL, 0, out); }

int iq_chan_cpu_bench_k(const iq_chan_load_t *load, int block, double seconds, int k, const int *cpus, int ncpus, iq_chan_bench_t *out)
{
  if (!load || !load->n_cells || load->n_cells > IQ_CHAN_MAX_CELLS || !load->nch || load->nch > SHM_IQ_MAX_CH || !(load->in_rate > 0) || !(seconds > 0)) return -1;
  if (block <= 0) block = IQ_CHAN_DEFAULT_BLOCK;
  const iq_chan_geom_t g = {.in_rate = load->in_rate, .nch = load->nch, .block = block};
  void *ctx = iq_chan_backend_cpu.create(&g);
  if (!ctx) return -1;
  if (k > 1 && iq_chan_backend_cpu.set_workers(ctx, k, cpus, ncpus)) { iq_chan_backend_cpu.destroy(ctx); return -1; }
  int rc = -1;
  int16_t *in[SHM_IQ_MAX_CH] = {0}, *outb[IQ_CHAN_MAX_CELLS * SHM_IQ_MAX_CH] = {0};
  void *cells[IQ_CHAN_MAX_CELLS] = {0};
  for (unsigned a = 0; a < load->nch; ++a) {
    in[a] = alloc_samples((size_t)block);
    if (!in[a]) goto done;
    uint32_t r = 0x9e3779b9u * (a + 1);
    for (int i = 0; i < 2 * block; ++i) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; in[a][i] = (int16_t)((int)(r % 4001) - 2000); }
  }
  for (unsigned k = 0; k < load->n_cells; ++k) {
    const int d = load->cell[k].decim;
    if (!decim_ok(d) || block % d) goto done;
    iq_chan_entry_t e = {.decim = d, .passband_hz = load->cell[k].passband_hz > 0 ? load->cell[k].passband_hz : 0.3 * load->in_rate / d};
    cells[k] = iq_chan_backend_cpu.prepare(ctx, &e);
    if (!cells[k]) goto done;
    iq_chan_backend_cpu.install(ctx, (int)k, cells[k]);
    if (load->cell[k].shifted) iq_chan_backend_cpu.set_shift(ctx, (int)k, 0.1237 * load->in_rate * (k & 1 ? 1 : -1));
    for (unsigned a = 0; a < load->nch; ++a) {
      outb[k * load->nch + a] = alloc_samples((size_t)block / (size_t)d);
      if (!outb[k * load->nch + a]) goto done;
    }
  }
  {
    struct timespec c0, c1;
    clock_gettime(k > 1 ? CLOCK_PROCESS_CPUTIME_ID : CLOCK_THREAD_CPUTIME_ID, &c0);
    const uint64_t t0 = now_ns();
    uint64_t blocks = 0, t1 = t0;
    do {
      if (iq_chan_backend_cpu.run(ctx, (const int16_t *const *)in, block, outb)) goto done;
      ++blocks;
      t1 = now_ns();
    } while ((double)(t1 - t0) * 1e-9 < seconds);
    clock_gettime(k > 1 ? CLOCK_PROCESS_CPUTIME_ID : CLOCK_THREAD_CPUTIME_ID, &c1);
    out->seconds = (double)(t1 - t0) * 1e-9;
    out->blocks = (double)blocks;
    out->samples_per_s = (double)blocks * block / out->seconds;
    out->rt_factor = out->samples_per_s / load->in_rate;
    out->cpu_s = (double)(c1.tv_sec - c0.tv_sec) + 1e-9 * (double)(c1.tv_nsec - c0.tv_nsec);
    rc = 0;
  }
done:
  for (unsigned k = 0; k < load->n_cells && k < IQ_CHAN_MAX_CELLS; ++k) {
    if (cells[k]) { void *c = iq_chan_backend_cpu.retire(ctx, (int)k); iq_chan_backend_cpu.release(ctx, c); }
    for (unsigned a = 0; a < load->nch; ++a) free(outb[k * load->nch + a]);
  }
  for (unsigned a = 0; a < load->nch; ++a) free(in[a]);
  iq_chan_backend_cpu.destroy(ctx);
  return rc;
}

int iq_chan_threads_needed(const iq_chan_load_t *load, double per_thread_msps_measured, double margin)
{
  if (!load || !load->n_cells || !load->nch || !(load->in_rate > 0) || !(per_thread_msps_measured > 0) || !(margin > 0)) return 1;
  const double need = margin * load->in_rate / (per_thread_msps_measured * 1e6);
  int k = (int)ceil(need - 1e-9);
  const int units = (int)(load->n_cells * load->nch);
  if (k > units) k = units; /* more threads than (cell, channel) jobs cannot help */
  return k < 1 ? 1 : k;
}

/* ================================================================ backend registry */
#define MAX_BACKENDS 4
static pthread_mutex_t g_be_mu = PTHREAD_MUTEX_INITIALIZER;
static const iq_chan_backend_t *g_be[MAX_BACKENDS];
static iq_chan_select_fn g_sel;
static void *g_sel_user;

int iq_chan_backend_register(const iq_chan_backend_t *b)
{
  int rc = -1;
  if (!b || !b->name) return -1;
  pthread_mutex_lock(&g_be_mu);
  int free_i = -1;
  for (int i = 0; i < MAX_BACKENDS; ++i) {
    if (g_be[i] && !strcmp(g_be[i]->name, b->name)) { free_i = -2; break; }
    if (!g_be[i] && free_i == -1) free_i = i;
  }
  if (free_i >= 0) { g_be[free_i] = b; rc = 0; }
  pthread_mutex_unlock(&g_be_mu);
  return rc;
}

const iq_chan_backend_t *iq_chan_backend_find(const char *name)
{
  const iq_chan_backend_t *r = NULL;
  if (!strcmp(name, iq_chan_backend_cpu.name)) return &iq_chan_backend_cpu;
  pthread_mutex_lock(&g_be_mu);
  for (int i = 0; i < MAX_BACKENDS; ++i) if (g_be[i] && !strcmp(g_be[i]->name, name)) r = g_be[i];
  pthread_mutex_unlock(&g_be_mu);
  return r;
}

void iq_chan_set_selector(iq_chan_select_fn fn, void *user)
{
  pthread_mutex_lock(&g_be_mu);
  g_sel = fn; g_sel_user = user;
  pthread_mutex_unlock(&g_be_mu);
}

const iq_chan_backend_t *iq_chan_backend_select(const iq_chan_load_t *load)
{
  pthread_mutex_lock(&g_be_mu);
  iq_chan_select_fn fn = g_sel;
  void *u = g_sel_user;
  pthread_mutex_unlock(&g_be_mu);
  const iq_chan_backend_t *b = fn ? fn(load, u) : NULL;
  return b ? b : &iq_chan_backend_cpu;
}

/* ================================================================ channelizer */
typedef struct {
  int used, passthrough;
  int paused; /* G6: the entry is kept (filters, ring, consumers) but not computed; its ring is marked away so its consumers wait */
  iq_chan_entry_t e;
  void *state;
  shm_iq_ring_t out;
  int16_t *obuf[SHM_IQ_MAX_CH];
  uint64_t ring_bytes;
} cell_t;

enum { OP_ADD = 1, OP_REMOVE, OP_RETUNE, OP_PAUSE, OP_RESUME };
typedef struct {
  int kind, slot;
  cell_t cell;          /* ADD: the prepared cell; REMOVE: the removed cell handed back */
  double shift_hz, centre_hz;
} op_t;

struct iq_chan {
  iq_chan_cfg_t cfg;
  const iq_chan_backend_t *be;
  void *ctx;
  shm_iq_ring_t in;
  int in_cons;
  unsigned nch;
  double in_rate;
  int block;
  int16_t *raw[SHM_IQ_MAX_CH];
  int16_t *outp[IQ_CHAN_MAX_CELLS * SHM_IQ_MAX_CH];
  cell_t cell[IQ_CHAN_MAX_CELLS];
  unsigned nused;
  unsigned npaused; /* entries in use but paused (G6) */
  _Atomic uint64_t pos; /* read by iq_chan_pos() from other threads */
  uint64_t epoch;
  int pos_valid, seek_pending, base_valid;
  int64_t base_tick;
  double ring_centre;
  size_t budget_left;
  /* thread and command channel */
  pthread_t th;
  int running;
  atomic_int stop;
  pthread_mutex_t api_mu, mu;
  pthread_cond_t cv;
  op_t *op;
  atomic_int op_pending;
  /* stats */
  _Atomic uint64_t blocks, in_samples, overruns, discont, dropped_in, epochs, work_ns, work_max_ns, lag_max, step_gap_max_ns, run_ns;
  uint64_t last_step_ns;
  int threads, threads_needed;
  double capacity_sps;
};

iq_chan_t *iq_chan_create(const iq_chan_cfg_t *cfg)
{
  if (!cfg || !cfg->in || !cfg->in->hdr) { errno = EINVAL; return NULL; }
  iq_chan_t *c = calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->cfg = *cfg;
  c->in_cons = -1;
  if (cfg->in->backing == SHM_IQ_BACKING_ANON) {
    if (shm_iq_ring_view(&c->in, cfg->in)) goto fail;
  } else if (shm_iq_ring_attach(&c->in, cfg->in->name[0] == '/' ? cfg->in->name + 1 : cfg->in->name)) goto fail;
  const shm_iq_hdr_t *h = c->in.hdr;
  c->nch = h->channels;
  c->in_rate = h->rate;
  c->block = cfg->block;
  if (c->block <= 0) {
    c->block = IQ_CHAN_DEFAULT_BLOCK;
    while (c->block > 4096 && (uint64_t)c->block > h->capacity / 8) c->block /= 2;
  }
  if (c->block < ALIGN_SAMPLES || c->block % ALIGN_SAMPLES || (uint64_t)c->block > h->capacity / 2) { errno = EINVAL; goto fail; }
  c->be = cfg->backend ? cfg->backend : iq_chan_backend_select(cfg->load_hint);
  const iq_chan_geom_t g = {.in_rate = c->in_rate, .nch = c->nch, .block = c->block};
  c->ctx = c->be->create(&g);
  if (!c->ctx) { errno = ENOMEM; goto fail; }
  for (unsigned a = 0; a < c->nch; ++a) {
    c->raw[a] = alloc_samples((size_t)c->block);
    if (!c->raw[a]) { errno = ENOMEM; goto fail; }
  }
  c->budget_left = cfg->out_ring_budget_bytes ? cfg->out_ring_budget_bytes : SIZE_MAX;
  c->in_cons = shm_iq_ring_consumer_register(&c->in);
  c->epoch = atomic_load(&h->rf_epoch);
  c->ring_centre = shm_iq_ring_center(&c->in);
  pthread_mutex_init(&c->api_mu, NULL);
  pthread_mutex_init(&c->mu, NULL);
  pthread_cond_init(&c->cv, NULL);
  return c;
fail:
  {
    const int e = errno;
    for (unsigned a = 0; a < SHM_IQ_MAX_CH; ++a) free(c->raw[a]);
    if (c->ctx) c->be->destroy(c->ctx);
    if (c->in.hdr) shm_iq_ring_detach(&c->in);
    free(c);
    errno = e;
  }
  return NULL;
}

static double entry_shift(const iq_chan_t *c, const iq_chan_entry_t *e) { return e->centre_hz > 0 ? e->centre_hz - c->ring_centre : e->shift_hz; }
static double entry_centre(const iq_chan_t *c, const iq_chan_entry_t *e) { return e->centre_hz > 0 ? e->centre_hz : c->ring_centre + e->shift_hz; }

static void warn_window(const iq_chan_t *c, const cell_t *x, double shift)
{
  if (fabs(shift) + x->e.passband_hz > 0.5 * c->in_rate)
    fprintf(stderr, "IQ_CHAN_WARN cell=%s band %.3f MHz +- %.3f MHz leaves the ring window %.3f MHz +- %.3f MHz\n", x->e.tag, entry_centre(c, &x->e) / 1e6,
            x->e.passband_hz / 1e6, c->ring_centre / 1e6, 0.5 * c->in_rate / 1e6);
}

/* channelizer thread (or the caller of the API when no thread runs): apply one pending command at a block boundary */
static void apply_op(iq_chan_t *c, op_t *op)
{
  cell_t *x = &c->cell[op->slot];
  if (op->kind == OP_ADD) {
    *x = op->cell;
    x->used = 1;
    const double shift = entry_shift(c, &x->e);
    c->be->install(c->ctx, op->slot, x->state);
    c->be->set_shift(c->ctx, op->slot, shift);
    x->passthrough = x->e.decim == 1 && shift == 0;
    const double ctr = entry_centre(c, &x->e);
    if (ctr != shm_iq_ring_center(&x->out)) shm_iq_ring_new_epoch(&x->out, ctr); /* the ring centre moved between prepare and install */
    __atomic_add_fetch(&c->nused, 1, __ATOMIC_RELAXED);
    warn_window(c, x, shift);
    fprintf(stderr, "IQ_CHAN_TUNE cell=%s ring_centre_hz=%.3f cell_centre_hz=%.3f shift_hz=%.3f decim=%d backend=%s\n", x->e.tag, c->ring_centre, ctr, shift, x->e.decim, c->be->name);
  } else if (op->kind == OP_REMOVE) {
    op->cell = *x;
    op->cell.state = c->be->retire(c->ctx, op->slot);
    if (x->paused) __atomic_sub_fetch(&c->npaused, 1, __ATOMIC_RELAXED);
    memset(x, 0, sizeof(*x));
    __atomic_sub_fetch(&c->nused, 1, __ATOMIC_RELAXED);
    memset(&c->outp[op->slot * (int)c->nch], 0, c->nch * sizeof(c->outp[0]));
  } else if (op->kind == OP_PAUSE) {
    if (!x->paused) {
      x->paused = 1;
      __atomic_add_fetch(&c->npaused, 1, __ATOMIC_RELAXED);
      shm_iq_ring_set_away(&x->out, UINT64_MAX >> 1); /* no writes until resumed: its consumers wait instead of declaring the producer lost */
      fprintf(stderr, "IQ_CHAN_PAUSE cell=%s\n", x->e.tag);
    }
  } else if (op->kind == OP_RESUME) {
    if (x->paused) {
      x->paused = 0;
      __atomic_sub_fetch(&c->npaused, 1, __ATOMIC_RELAXED);
      c->be->reset(c->ctx, op->slot); /* the skipped input is gone: start from a fresh history; the next write is a forward gap (timestamp jump) */
      shm_iq_ring_return(&x->out, entry_centre(c, &x->e));
      fprintf(stderr, "IQ_CHAN_RESUME cell=%s\n", x->e.tag);
    }
  } else if (op->kind == OP_RETUNE) {
    x->e.shift_hz = op->shift_hz;
    x->e.centre_hz = op->centre_hz;
    const double shift = entry_shift(c, &x->e);
    c->be->set_shift(c->ctx, op->slot, shift);
    c->be->reset(c->ctx, op->slot); /* only this entry: the others keep their filter state */
    x->passthrough = x->e.decim == 1 && shift == 0;
    shm_iq_ring_new_epoch(&x->out, entry_centre(c, &x->e));
    warn_window(c, x, shift);
    fprintf(stderr, "IQ_CHAN_TUNE cell=%s ring_centre_hz=%.3f cell_centre_hz=%.3f shift_hz=%.3f decim=%d retune\n", x->e.tag, c->ring_centre, entry_centre(c, &x->e), shift, x->e.decim);
  }
}

static void apply_pending(iq_chan_t *c)
{
  pthread_mutex_lock(&c->mu);
  if (c->op) {
    apply_op(c, c->op);
    c->op = NULL;
    atomic_store(&c->op_pending, 0);
    pthread_cond_broadcast(&c->cv);
  }
  pthread_mutex_unlock(&c->mu);
}

/* hand one command to the channelizer thread and wait until it is applied (block boundary); without a thread apply it here */
static void submit_op(iq_chan_t *c, op_t *op)
{
  pthread_mutex_lock(&c->mu);
  if (!c->running) {
    apply_op(c, op);
  } else {
    c->op = op;
    atomic_store(&c->op_pending, 1);
    while (c->op) pthread_cond_wait(&c->cv, &c->mu);
  }
  pthread_mutex_unlock(&c->mu);
}

static void free_cell_buffers(iq_chan_t *c, cell_t *x)
{
  for (unsigned a = 0; a < c->nch; ++a) { free(x->obuf[a]); x->obuf[a] = NULL; }
}

int iq_chan_cell_add(iq_chan_t *c, const iq_chan_entry_t *e)
{
  if (!c || !e) return -EINVAL;
  if (!decim_ok(e->decim) || c->block % e->decim || (e->decim > 1 && !(e->passband_hz > 0))) return -EINVAL;
  pthread_mutex_lock(&c->api_mu);
  int rc = -ENOSPC, slot = -1;
  for (int i = 0; i < IQ_CHAN_MAX_CELLS; ++i) if (!c->cell[i].used) { slot = i; break; }
  if (slot < 0) goto out;
  op_t *op = calloc(1, sizeof(*op));
  if (!op) { rc = -ENOMEM; goto out; }
  cell_t *x = &op->cell;
  x->e = *e;
  if (!x->e.consumers) x->e.consumers = 4;
  op->kind = OP_ADD; op->slot = slot;
  /* narrow ring: sized from the budget (the host-RAM plan), never fewer than 8 blocks so a reader that lags a few blocks is not an overrun */
  const double rate_out = c->in_rate / e->decim;
  uint64_t need = 1;
  while (need < 8 * (uint64_t)(c->block / e->decim)) need <<= 1;
  const double tms = c->cfg.out_target_ms > 0 ? c->cfg.out_target_ms : SHM_IQ_RING_NARROW_TARGET_MS;
  uint64_t cap = shm_iq_ring_plan_capacity(c->budget_left, rate_out, c->nch, 1, tms, SHM_IQ_RING_NARROW_FLOOR_MS);
  if (cap < need) cap = (c->budget_left == SIZE_MAX || shm_iq_ring_bytes(c->nch, need) <= c->budget_left) ? need : 0;
  if (!cap) { rc = -ENOMEM; fprintf(stderr, "IQ_CHAN_REFUSE cell=%s narrow ring does not fit the budget (%zu bytes left)\n", e->tag, c->budget_left); free(op); goto out; }
  char name[96] = "";
  if (e->tag[0] && c->cfg.in->name[0]) snprintf(name, sizeof(name), "%s_%s", c->cfg.in->name[0] == '/' ? c->cfg.in->name + 1 : c->cfg.in->name, e->tag);
  const double ctr = e->centre_hz > 0 ? e->centre_hz : shm_iq_ring_center(&c->in) + e->shift_hz;
  if (shm_iq_ring_create(&x->out, SHM_IQ_BACKING_ANON, name[0] ? name : NULL, c->nch, cap, rate_out, ctr, x->e.consumers)) { rc = -errno; free(op); goto out; }
  if (c->in.hdr->version >= 2) shm_iq_ring_set_meta(&x->out, c->in.hdr->radio_id, c->in.hdr->time_source);
  shm_iq_ring_prefault(&x->out); /* page faults belong to start-up, not to the block loop */
  x->ring_bytes = shm_iq_ring_bytes(c->nch, x->out.hdr->capacity);
  for (unsigned a = 0; a < c->nch; ++a) {
    x->obuf[a] = alloc_samples((size_t)(c->block / e->decim));
    if (!x->obuf[a]) rc = -ENOMEM;
  }
  x->state = rc == -ENOMEM ? NULL : c->be->prepare(c->ctx, &x->e);
  if (!x->state) {
    free_cell_buffers(c, x);
    shm_iq_ring_detach(&x->out);
    free(op);
    rc = -ENOMEM;
    goto out;
  }
  if (c->budget_left != SIZE_MAX) c->budget_left -= x->ring_bytes;
  submit_op(c, op);
  rc = slot;
  free(op);
out:
  pthread_mutex_unlock(&c->api_mu);
  return rc;
}

int iq_chan_cell_remove(iq_chan_t *c, int id)
{
  if (!c || id < 0 || id >= IQ_CHAN_MAX_CELLS) return -EINVAL;
  pthread_mutex_lock(&c->api_mu);
  int rc = -ENOENT;
  if (c->cell[id].used) {
    op_t *op = calloc(1, sizeof(*op));
    if (!op) { pthread_mutex_unlock(&c->api_mu); return -ENOMEM; }
    op->kind = OP_REMOVE; op->slot = id;
    submit_op(c, op);
    c->be->release(c->ctx, op->cell.state);
    free_cell_buffers(c, &op->cell);
    if (c->budget_left != SIZE_MAX) c->budget_left += op->cell.ring_bytes;
    shm_iq_ring_detach(&op->cell.out); /* consumers' views keep the memory until they detach */
    free(op);
    rc = 0;
  }
  pthread_mutex_unlock(&c->api_mu);
  return rc;
}

int iq_chan_cell_retune(iq_chan_t *c, int id, double shift_hz, double centre_hz)
{
  if (!c || id < 0 || id >= IQ_CHAN_MAX_CELLS) return -EINVAL;
  pthread_mutex_lock(&c->api_mu);
  int rc = -ENOENT;
  if (c->cell[id].used) {
    op_t *op = calloc(1, sizeof(*op));
    if (!op) { pthread_mutex_unlock(&c->api_mu); return -ENOMEM; }
    op->kind = OP_RETUNE; op->slot = id; op->shift_hz = shift_hz; op->centre_hz = centre_hz;
    submit_op(c, op);
    free(op);
    rc = 0;
  }
  pthread_mutex_unlock(&c->api_mu);
  return rc;
}

static int submit_simple(iq_chan_t *c, int id, int kind)
{
  if (!c || id < 0 || id >= IQ_CHAN_MAX_CELLS) return -EINVAL;
  pthread_mutex_lock(&c->api_mu);
  int rc = -ENOENT;
  if (c->cell[id].used) {
    op_t op; /* no allocation: pause / resume run after the receiver's RX fence */
    memset(&op, 0, sizeof(op));
    op.kind = kind; op.slot = id;
    submit_op(c, &op);
    rc = 0;
  }
  pthread_mutex_unlock(&c->api_mu);
  return rc;
}
int iq_chan_cell_pause(iq_chan_t *c, int id) { return submit_simple(c, id, OP_PAUSE); }
int iq_chan_cell_resume(iq_chan_t *c, int id) { return submit_simple(c, id, OP_RESUME); }
int iq_chan_cell_paused(const iq_chan_t *c, int id) { return c && id >= 0 && id < IQ_CHAN_MAX_CELLS && c->cell[id].used ? __atomic_load_n(&c->cell[id].paused, __ATOMIC_RELAXED) : -1; }

shm_iq_ring_t *iq_chan_cell_ring(iq_chan_t *c, int id)
{
  return c && id >= 0 && id < IQ_CHAN_MAX_CELLS && c->cell[id].used ? &c->cell[id].out : NULL;
}

void iq_chan_seek(iq_chan_t *c, uint64_t pos)
{
  pthread_mutex_lock(&c->mu);
  c->pos = pos - pos % ALIGN_SAMPLES; c->pos_valid = 1; c->seek_pending = 1;
  pthread_mutex_unlock(&c->mu);
}

uint64_t iq_chan_pos(const iq_chan_t *c) { return c->pos; }

static void retune_all(iq_chan_t *c)
{
  for (int s = 0; s < IQ_CHAN_MAX_CELLS; ++s) {
    cell_t *x = &c->cell[s];
    if (!x->used) continue;
    const double shift = entry_shift(c, &x->e);
    c->be->set_shift(c->ctx, s, shift);
    x->passthrough = x->e.decim == 1 && shift == 0;
    shm_iq_ring_new_epoch(&x->out, entry_centre(c, &x->e));
    warn_window(c, x, shift);
  }
  c->be->reset(c->ctx, -1); /* the new run must not start with old-tuned filter history */
}

static void update_max(_Atomic uint64_t *m, uint64_t v)
{
  uint64_t o = atomic_load(m);
  while (v > o && !atomic_compare_exchange_weak(m, &o, v)) {}
}

int iq_chan_step(iq_chan_t *c)
{
  { const uint64_t t = now_ns(); if (c->last_step_ns) update_max(&c->step_gap_max_ns, t - c->last_step_ns); c->last_step_ns = t; }
  if (atomic_load(&c->op_pending)) apply_pending(c);
  shm_iq_hdr_t *h = c->in.hdr;
  const uint64_t ep = atomic_load(&h->rf_epoch);
  if (ep != c->epoch) { /* producer retune / device reload: new shifts from the new centre, every output ring starts a new run */
    c->epoch = ep;
    c->ring_centre = shm_iq_ring_center(&c->in);
    retune_all(c);
    atomic_fetch_add(&c->epochs, 1);
  }
  const uint64_t head = atomic_load(&h->head);
  if (!c->base_valid) {
    if (!head || h->base_tick == INT64_MIN) return 0;
    c->base_tick = h->base_tick;
    c->base_valid = 1;
  }
  if (c->nused == c->npaused) { /* no entry (or all paused): consume nothing; follow the live edge so a new entry starts at the newest data (unless a seek set the start) */
    if (!c->seek_pending) { c->pos = head - head % ALIGN_SAMPLES; c->pos_valid = 1; }
    return 0;
  }
  const int r = shm_iq_ring_copy(&c->in, c->pos, c->block, c->nch, c->raw);
  if (r == 0 && atomic_load(&h->rf_epoch) != c->epoch) return 0; /* retune during the copy: redo with the new shifts (the copied samples are old-tuned) */
  if (r == 0) {
    const uint64_t t0 = now_ns();
    update_max(&c->lag_max, atomic_load(&h->head) - c->pos);
    for (int s = 0; s < IQ_CHAN_MAX_CELLS; ++s)
      for (unsigned a = 0; a < c->nch; ++a) c->outp[s * (int)c->nch + (int)a] = c->cell[s].used && !c->cell[s].paused && !c->cell[s].passthrough ? c->cell[s].obuf[a] : NULL;
    if (c->be->run(c->ctx, (const int16_t *const *)c->raw, c->block, c->outp)) { fprintf(stderr, "IQ_CHAN_ERR backend %s run failed\n", c->be->name); return -1; }
    const uint64_t t1 = now_ns();
    atomic_fetch_add(&c->run_ns, t1 - t0);
    for (int s = 0; s < IQ_CHAN_MAX_CELLS; ++s) {
      cell_t *x = &c->cell[s];
      if (!x->used || x->paused) continue;
      void *bufs[SHM_IQ_MAX_CH];
      for (unsigned a = 0; a < c->nch; ++a) bufs[a] = x->passthrough ? (void *)c->raw[a] : (void *)x->obuf[a];
      /* output timestamps are (base_tick + ring_pos) / decim, the formula of shm_iq.c */
      shm_iq_ring_write(&x->out, (int64_t)(((uint64_t)c->base_tick + c->pos) / (uint64_t)x->e.decim), bufs, c->block / x->e.decim, c->nch);
    }
    const uint64_t dt = now_ns() - t0;
    c->pos += (uint64_t)c->block;
    c->seek_pending = 0;
    shm_iq_ring_consumer_update(&c->in, c->in_cons, c->pos);
    atomic_fetch_add(&c->blocks, 1);
    atomic_fetch_add(&c->in_samples, (uint64_t)c->block);
    atomic_fetch_add(&c->work_ns, dt);
    update_max(&c->work_max_ns, dt);
    return 1;
  }
  if (r < 0) { /* overwritten (fell behind by more than the ring) or before valid_from (discontinuity of the producer) */
    const uint64_t vf = atomic_load(&h->valid_from), hd = atomic_load(&h->head);
    const int ov = c->pos >= vf;
    uint64_t to = ov ? hd : vf;
    to += (uint64_t)(ALIGN_SAMPLES - (int)(to % ALIGN_SAMPLES)) % ALIGN_SAMPLES;
    atomic_fetch_add(&c->dropped_in, to - c->pos);
    atomic_fetch_add(ov ? &c->overruns : &c->discont, 1);
    shm_iq_ring_consumer_overrun(&c->in, c->in_cons, to - c->pos, ov);
    fprintf(stderr, "IQ_CHAN_%s skipped_in=%llu new_pos=%llu\n", ov ? "OVERRUN" : "DISCONT", (unsigned long long)(to - c->pos), (unsigned long long)to);
    c->pos = to;
    c->be->reset(c->ctx, -1); /* history only: designs, shifts, rings and consumers stay */
    return 0;
  }
  return 0;
}

static void *thread_main(void *arg)
{
  iq_chan_t *c = arg;
  while (!atomic_load(&c->stop)) {
    const int r = iq_chan_step(c);
    if (r < 0) break;
    if (r == 0) {
      struct timespec w = {0, 20000}; /* polling like shm_iq.c; a futex wake needs a ring change */
      nanosleep(&w, NULL);
    }
  }
  return NULL;
}

int iq_chan_set_threads(iq_chan_t *c, int k, const int *cpus, int ncpus)
{
  if (c->running) return -EBUSY;
  if (k < 1) k = 1;
  if (c->be->set_workers && c->be->set_workers(c->ctx, k, cpus && ncpus > 1 ? cpus + 1 : NULL, ncpus > 1 ? ncpus - 1 : 0)) return -ENOMEM;
  c->threads = c->be->set_workers ? k : 1;
  return 0;
}

int iq_chan_start_k(iq_chan_t *c, const int *cpus, int ncpus, int k)
{
  if (c->running) return -EBUSY;
  if (k < 1) k = 1;
  if (iq_chan_set_threads(c, k, cpus, ncpus)) return -ENOMEM;
  atomic_store(&c->stop, 0);
  pthread_attr_t at;
  pthread_attr_init(&at);
  if (cpus && ncpus > 0) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpus[0] >= 0 && cpus[0] < CPU_SETSIZE ? cpus[0] : 0, &s); /* the channelizer thread is worker 0; the others are pinned to the following cpus */
    pthread_attr_setaffinity_np(&at, sizeof(s), &s);
  }
  pthread_mutex_lock(&c->mu);
  c->running = 1;
  pthread_mutex_unlock(&c->mu);
  const int rc = pthread_create(&c->th, &at, thread_main, c);
  pthread_attr_destroy(&at);
  if (rc) { pthread_mutex_lock(&c->mu); c->running = 0; pthread_mutex_unlock(&c->mu); return -rc; }
  return 0;
}

/* k is derived: the per-thread capacity of THIS load is measured on this host (a short iq_chan_cpu_bench), iq_chan_threads_needed() turns it into a thread count,
 * and the cpus the core plan gave are the ceiling. Fewer cpus than needed: run with what is given and publish the shortfall (stats.threads_needed > threads,
 * stats.capacity_sps = measured capacity with the threads that run) for the admission model. The load is load_hint of the config, else the current entries. */
#define IQ_CHAN_MARGIN 1.25
int iq_chan_start(iq_chan_t *c, const int *cpus, int ncpus)
{
  iq_chan_load_t l;
  if (c->cfg.load_hint) l = *c->cfg.load_hint;
  else {
    memset(&l, 0, sizeof(l));
    l.in_rate = c->in_rate; l.nch = c->nch;
    for (int s = 0; s < IQ_CHAN_MAX_CELLS; ++s)
      if (c->cell[s].used && !c->cell[s].paused) {
        l.cell[l.n_cells].decim = c->cell[s].e.decim; l.cell[l.n_cells].passband_hz = c->cell[s].e.passband_hz;
        l.cell[l.n_cells].shifted = c->cell[s].e.shift_hz != 0 || c->cell[s].e.centre_hz > 0; l.n_cells++;
      }
  }
  int k = 1;
  c->threads_needed = 1; c->capacity_sps = 0;
  iq_chan_bench_t b;
  if (l.n_cells && c->be == &iq_chan_backend_cpu && iq_chan_cpu_bench(&l, c->block, 0.2, &b) == 0) {
    const int have = ncpus > 0 && cpus ? ncpus : 1, units = (int)(l.n_cells * l.nch), ceil_k = have < units ? have : units;
    const int need0 = iq_chan_threads_needed(&l, b.samples_per_s * 1e-6, IQ_CHAN_MARGIN);
    k = need0 < ceil_k ? need0 : ceil_k;
    c->capacity_sps = b.samples_per_s;
    /* Scaling is not linear (jobs are cell x channel and cells differ in cost): measure the capacity with k threads and add threads while it is below the margin,
     * up to the cpus given and the number of jobs. */
    for (;;) {
      if (k > 1) {
        iq_chan_bench_t bk;
        if (iq_chan_cpu_bench_k(&l, c->block, 0.2, k, cpus && ncpus > 1 ? cpus + 1 : NULL, ncpus > 1 ? ncpus - 1 : 0, &bk)) break;
        c->capacity_sps = bk.samples_per_s;
      }
      if (c->capacity_sps >= IQ_CHAN_MARGIN * c->in_rate || k >= ceil_k) break;
      ++k;
    }
    c->threads_needed = c->capacity_sps >= IQ_CHAN_MARGIN * c->in_rate ? k : (k < units ? k + 1 : k); /* margin not met: at least one more thread would be asked for */
    if (k < c->threads_needed)
      fprintf(stderr, "IQ_CHAN_SHORTFALL threads_needed=%d threads=%d capacity=%.1f MS/s in_rate=%.2f MS/s\n", c->threads_needed, k, c->capacity_sps * 1e-6, c->in_rate * 1e-6);
  }
  return iq_chan_start_k(c, cpus, ncpus, k);
}

void iq_chan_stop(iq_chan_t *c)
{
  if (!c->running) return;
  atomic_store(&c->stop, 1);
  pthread_join(c->th, NULL);
  pthread_mutex_lock(&c->mu);
  c->running = 0;
  if (c->op) { apply_op(c, c->op); c->op = NULL; atomic_store(&c->op_pending, 0); pthread_cond_broadcast(&c->cv); } /* a command raced with the stop */
  pthread_mutex_unlock(&c->mu);
}

void iq_chan_stats(const iq_chan_t *c, iq_chan_stats_t *st)
{
  st->blocks = atomic_load(&c->blocks); st->in_samples = atomic_load(&c->in_samples);
  st->overruns = atomic_load(&c->overruns); st->discont = atomic_load(&c->discont); st->dropped_in = atomic_load(&c->dropped_in);
  st->run_ns = atomic_load(&c->run_ns); st->lag_max = atomic_load(&c->lag_max); st->step_gap_max_ns = atomic_load(&c->step_gap_max_ns); st->epochs = atomic_load(&c->epochs); st->work_ns = atomic_load(&c->work_ns); st->work_max_ns = atomic_load(&c->work_max_ns);
  st->threads = c->threads ? c->threads : 1; st->threads_needed = c->threads_needed ? c->threads_needed : 1; st->capacity_sps = c->capacity_sps;
  st->n_cells = __atomic_load_n(&c->nused, __ATOMIC_RELAXED); st->n_paused = __atomic_load_n(&c->npaused, __ATOMIC_RELAXED); st->backend = c->be->name;
}

void iq_chan_destroy(iq_chan_t *c)
{
  if (!c) return;
  iq_chan_stop(c);
  for (int s = 0; s < IQ_CHAN_MAX_CELLS; ++s) {
    cell_t *x = &c->cell[s];
    if (!x->used) continue;
    c->be->release(c->ctx, c->be->retire(c->ctx, s));
    free_cell_buffers(c, x);
    shm_iq_ring_detach(&x->out);
  }
  c->be->destroy(c->ctx);
  for (unsigned a = 0; a < SHM_IQ_MAX_CH; ++a) free(c->raw[a]);
  shm_iq_ring_consumer_release(&c->in, c->in_cons);
  shm_iq_ring_detach(&c->in);
  pthread_mutex_destroy(&c->api_mu); pthread_mutex_destroy(&c->mu); pthread_cond_destroy(&c->cv);
  free(c);
}
