/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static nr_csirs_disc_gpu_ops_t raw_ops;
static nr_csirs_disc_gpu_ops_t public_ops;
static pthread_mutex_t breaker_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t retry_after_ms;
static unsigned failures;

/* monotonic_ms: returns CLOCK_MONOTONIC time in milliseconds (clock of the GPU circuit breaker's retry timer). */
static uint64_t monotonic_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* breaker_ready: circuit breaker check: true when the GPU backend may be used now (the retry time after the last failure has passed).
 *   Takes breaker_lock. Called before every guarded GPU call and by nr_csirs_disc_gpu_load().
 */
static bool breaker_ready(void)
{
  bool ready;
  pthread_mutex_lock(&breaker_lock);
  ready = monotonic_ms() >= retry_after_ms;
  pthread_mutex_unlock(&breaker_lock);
  return ready;
}

/* breaker_result: records the outcome of one GPU call: success (rc >= 0) clears the failure count; a failure doubles the retry delay (100 ms << (failures-1), capped at 3.2 s).
 *   Takes breaker_lock. Called after every guarded GPU call.
 */
static void breaker_result(int rc)
{
  pthread_mutex_lock(&breaker_lock);
  if (rc >= 0) {
    failures = 0;
    retry_after_ms = 0;
  } else {
    if (failures < 6)
      failures++;
    retry_after_ms = monotonic_ms() + (100u << (failures > 5 ? 5 : failures - 1));
  }
  pthread_mutex_unlock(&breaker_lock);
}

/* guarded_footprints: GPU footprint detection behind the circuit breaker: returns -1 without calling the GPU while the breaker is open.
 *   Installed as public_ops.footprints by load_module(). Reports its result to breaker_result().
 */
static int guarded_footprints(const nr_csirs_disc_ring_t *r,
                              const nr_csirs_disc_fp_cfg_t *cfg,
                              nr_csirs_disc_fp_t *out,
                              int max_out)
{
  if (!raw_ops.footprints || !breaker_ready())
    return -1;
  const int rc = raw_ops.footprints(r, cfg, out, max_out);
  breaker_result(rc);
  return rc;
}

/* guarded_normalize: GPU normalisation of a ring behind the circuit breaker; returns -1 while the breaker is open.
 *   Installed as public_ops.normalize by load_module(). Reports its result to breaker_result().
 */
static int guarded_normalize(const nr_csirs_disc_ring_t *r,
                              const nr_csirs_disc_fp_cfg_t *cfg,
                              uint32_t *out)
{
  if (!raw_ops.normalize || !breaker_ready())
    return -1;
  const int rc = raw_ops.normalize(r, cfg, out);
  breaker_result(rc);
  return rc;
}

/* guarded_sweep: GPU correlation sweep of a set of hypotheses behind the circuit breaker; returns -1 while the breaker is open.
 *   Installed as public_ops.sweep by load_module(). Reports its result to breaker_result().
 */
static int guarded_sweep(const nr_csirs_disc_ring_t *r,
                         const nr_csirs_disc_hyp_t *hyps,
                         int n_hyp,
                         const nr_csirs_disc_sweep_cfg_t *cfg,
                         nr_csirs_disc_win_t *out)
{
  if (!raw_ops.sweep || !breaker_ready())
    return -1;
  const int rc = raw_ops.sweep(r, hyps, n_hyp, cfg, out);
  breaker_result(rc);
  return rc;
}

/* guarded_fft: GPU FFT behind the circuit breaker; returns -1 while the breaker is open.
 *   Installed as public_ops.fft by load_module(). Reports its result to breaker_result().
 */
static int guarded_fft(int fft, int transforms, const int16_t *in, int16_t *out)
{
  if (!raw_ops.fft || !breaker_ready()) return -1;
  int rc = raw_ops.fft(fft, transforms, in, out);
  breaker_result(rc);
  return rc;
}

/* guarded_fep: GPU FEP (symbol extraction into the ring) behind the circuit breaker; returns -1 while the breaker is open.
 *   Installed as public_ops.fep by load_module(). Reports its result to breaker_result().
 */
static int guarded_fep(const nr_csirs_disc_fep_cfg_t *c, const nr_csirs_disc_fep_slot_t *s,
                       int n, nr_csirs_disc_ring_t *r, int first)
{
  if (!raw_ops.fep || !breaker_ready()) return -1;
  int rc=raw_ops.fep(c,s,n,r,first);breaker_result(rc);return rc;
}
/* guarded_accumulate: GPU accumulation of the footprint statistics behind the circuit breaker; returns -1 while the breaker is open.
 *   Installed as public_ops.accumulate by load_module(). Reports its result to breaker_result().
 */
static int guarded_accumulate(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_fp_cfg_t *c,
                              uint64_t *s, uint64_t *q, uint64_t *s80, uint64_t *s64,
                              uint64_t *t, uint64_t *tq)
{
  if (!raw_ops.accumulate || !breaker_ready()) return -1;
  int rc=raw_ops.accumulate(r,c,s,q,s80,s64,t,tq);breaker_result(rc);return rc;
}

/* guarded_lattice: GPU lattice screen of the accumulated statistics behind the circuit breaker; returns -1 while the breaker is open.
 *   Installed as public_ops.lattice by load_module(). Reports its result to breaker_result().
 */
static int guarded_lattice(int rb,int p,uint64_t nt,const uint64_t *counts,
                           const uint64_t *sum,const uint64_t *sq,const uint64_t *total,
                           const uint64_t *totalq,uint16_t *masks)
{
  if(!raw_ops.lattice || !breaker_ready()) return -1;
  int rc=raw_ops.lattice(rb,p,nt,counts,sum,sq,total,totalq,masks);breaker_result(rc);return rc;
}

/* open_sibling: opens a shared library by name, first through the normal loader path and then next to the running executable (/proc/self/exe).
 *   Returns the dlopen handle or NULL. Used by load_module() for libcsirs_disc_gpu.so and libnr_csirs_disc_gpu_sweep.so.
 */
static void *open_sibling(const char *name)
{
  void *handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
  if (handle)
    return handle;
  char path[4096];
  const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (n <= 0)
    return NULL;
  path[n] = 0;
  char *slash = strrchr(path, '/');
  if (!slash || (size_t)(slash - path) + strlen(name) + 2 > sizeof(path))
    return NULL;
  strcpy(slash + 1, name);
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

/* load_module: one-time (pthread_once) loader of the CSI-RS discovery GPU modules: fills raw_ops from the two shared libraries and installs the guarded public_ops.
 *   Also takes the unified-memory allocator hooks when both are exported. Modules stay loaded for the process lifetime.
 *   Called only through pthread_once from nr_csirs_disc_gpu_load().
 */
static void load_module(void)
{
  void *fp_handle = open_sibling("libcsirs_disc_gpu.so");
  if (fp_handle) {
    const nr_csirs_disc_gpu_ops_t *(*get_ops)(void) = dlsym(fp_handle, "nr_csirs_disc_gpu_ops");
    const nr_csirs_disc_gpu_ops_t *module_ops = get_ops ? get_ops() : NULL;
    if (module_ops) {
      raw_ops.footprints = module_ops->footprints;
      raw_ops.normalize = module_ops->normalize;
      raw_ops.fft = module_ops->fft;
      raw_ops.fep = module_ops->fep;
      raw_ops.accumulate = module_ops->accumulate;
      raw_ops.lattice = module_ops->lattice;
      raw_ops.device_alloc=module_ops->device_alloc;raw_ops.device_free=module_ops->device_free;
      raw_ops.prepare=module_ops->prepare;raw_ops.detect=module_ops->detect;raw_ops.reset=module_ops->reset;raw_ops.readback=module_ops->readback;raw_ops.upload_symbols=module_ops->upload_symbols;
    }
    void *(*alloc)(size_t) = (void *(*)(size_t))dlsym(fp_handle, "nr_csirs_disc_gpu_alloc_unified");
    void (*free_fn)(void *) = (void (*)(void *))dlsym(fp_handle, "nr_csirs_disc_gpu_free_unified");
    if (module_ops && alloc && free_fn) {
      nr_csirs_disc_alloc_unified = alloc;
      nr_csirs_disc_free_unified = free_fn;
    }
  }

  void *sweep_handle = open_sibling("libnr_csirs_disc_gpu_sweep.so");
  if (sweep_handle) {
    raw_ops.confirm=dlsym(sweep_handle,"nr_csirs_disc_gpu_confirm_entry");
    raw_ops.refine=dlsym(sweep_handle,"nr_csirs_disc_gpu_refine_entry");
    raw_ops.sweep_prepare=dlsym(sweep_handle,"nr_csirs_disc_gpu_sweep_prepare");
    raw_ops.sweep = (int (*)(const nr_csirs_disc_ring_t *, const nr_csirs_disc_hyp_t *, int,
                             const nr_csirs_disc_sweep_cfg_t *, nr_csirs_disc_win_t *))
        dlsym(sweep_handle, "nr_csirs_disc_gpu_sweep_entry");
  }
  public_ops=raw_ops;

  public_ops.footprints = raw_ops.footprints ? guarded_footprints : NULL;
  public_ops.normalize = raw_ops.normalize ? guarded_normalize : NULL;
  public_ops.fft = raw_ops.fft ? guarded_fft : NULL;
  public_ops.fep = raw_ops.fep ? guarded_fep : NULL;
  public_ops.accumulate = raw_ops.accumulate ? guarded_accumulate : NULL;
  public_ops.lattice = raw_ops.lattice ? guarded_lattice : NULL;
  public_ops.sweep = raw_ops.sweep ? guarded_sweep : NULL;
}

/* nr_csirs_disc_gpu_ops: returns the guarded GPU operations table, or NULL when no GPU module provides footprints or sweep.
 *   Caller: nr_csirs_disc_gpu_load() (and the GPU module, nr_csirs_disc_gpu.cu, through its own entry).
 */
const nr_csirs_disc_gpu_ops_t *nr_csirs_disc_gpu_ops(void)
{
  return (public_ops.footprints || public_ops.sweep) ? &public_ops : NULL;
}

/* nr_csirs_disc_gpu_load: returns the GPU operations for the discovery pipeline, loading the modules on first use; NULL when disabled or on the CPU backend.
 *   Enabled only with ISAC_CSIRS_DISC=1 and ISAC_CSIRS_DISC_BACKEND not "cpu" (auto or gpu). Returns NULL while the breaker is open.
 *   Callers: nr_csirs_disc.c (pipeline init and batches), nr_csirs_disc_feed.c (ring allocation and worker), nr_csirs_disc_td.c and the confirmation internals.
 */
const nr_csirs_disc_gpu_ops_t *nr_csirs_disc_gpu_load(void)
{
  const char *enabled = getenv("ISAC_CSIRS_DISC");
  const char *backend = getenv("ISAC_CSIRS_DISC_BACKEND");
  if (!enabled || strcmp(enabled, "1") || (backend && !strcmp(backend, "cpu")))
    return NULL;
  if (backend && strcmp(backend, "auto") && strcmp(backend, "gpu"))
    return NULL;
  pthread_once(&once, load_module);
  return breaker_ready()?nr_csirs_disc_gpu_ops():NULL;
}
