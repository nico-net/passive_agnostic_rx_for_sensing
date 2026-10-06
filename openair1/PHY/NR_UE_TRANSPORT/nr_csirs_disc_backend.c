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

static uint64_t monotonic_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static bool breaker_ready(void)
{
  bool ready;
  pthread_mutex_lock(&breaker_lock);
  ready = monotonic_ms() >= retry_after_ms;
  pthread_mutex_unlock(&breaker_lock);
  return ready;
}

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

static int guarded_fft(int fft, int transforms, const int16_t *in, int16_t *out)
{
  if (!raw_ops.fft || !breaker_ready()) return -1;
  int rc = raw_ops.fft(fft, transforms, in, out);
  breaker_result(rc);
  return rc;
}

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

/* Keep modules loaded for the lifetime of managed allocations and the worker. */
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
    }
    void *(*alloc)(size_t) = (void *(*)(size_t))dlsym(fp_handle, "nr_csirs_disc_gpu_alloc_unified");
    void (*free_fn)(void *) = (void (*)(void *))dlsym(fp_handle, "nr_csirs_disc_gpu_free_unified");
    if (module_ops && alloc && free_fn) {
      nr_csirs_disc_alloc_unified = alloc;
      nr_csirs_disc_free_unified = free_fn;
    }
  }

  void *sweep_handle = open_sibling("libnr_csirs_disc_gpu_sweep.so");
  if (sweep_handle)
    raw_ops.sweep = (int (*)(const nr_csirs_disc_ring_t *, const nr_csirs_disc_hyp_t *, int,
                             const nr_csirs_disc_sweep_cfg_t *, nr_csirs_disc_win_t *))
        dlsym(sweep_handle, "nr_csirs_disc_gpu_sweep_entry");

  public_ops.footprints = raw_ops.footprints ? guarded_footprints : NULL;
  public_ops.normalize = raw_ops.normalize ? guarded_normalize : NULL;
  public_ops.fft = raw_ops.fft ? guarded_fft : NULL;
  public_ops.sweep = raw_ops.sweep ? guarded_sweep : NULL;
}

const nr_csirs_disc_gpu_ops_t *nr_csirs_disc_gpu_ops(void)
{
  return (public_ops.footprints || public_ops.sweep) ? &public_ops : NULL;
}

const nr_csirs_disc_gpu_ops_t *nr_csirs_disc_gpu_load(void)
{
  const char *enabled = getenv("ISAC_CSIRS_DISC");
  const char *backend = getenv("ISAC_CSIRS_DISC_BACKEND");
  if (!enabled || strcmp(enabled, "1") || (backend && !strcmp(backend, "cpu")))
    return NULL;
  if (backend && strcmp(backend, "auto") && strcmp(backend, "gpu"))
    return NULL;
  pthread_once(&once, load_module);
  return nr_csirs_disc_gpu_ops();
}
