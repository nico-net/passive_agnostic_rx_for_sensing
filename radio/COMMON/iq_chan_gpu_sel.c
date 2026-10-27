/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI G5: the CHAN path (plan R5): which backend the in-process channelizer uses when its creator does not name one. Called by
 * iq_chan_backend_select() when no selector is installed (iq_chan.c). One decision per process, through the per-path registry nr_gpu_path:
 *   - the GPU module libshm_iq_chan_gpu.so is looked for next to libnr_gpu_platform.so (the receiver's build directory; dladdr), never by a path option;
 *   - gpu_ok = the module loads, a device of a GPU tier is there, the SELF-CHECK passes (GPU vs iq_ddc on the load's own geometry: every output component
 *     within 1 LSB and at most 1 in 1000 differing, the figure of the old --selftest --gpu) and the channelizer's bytes fit the GPU budget headroom;
 *   - nr_gpu_path_begin(CHAN, gpu_ok, BENCH): SHARED tier -> GPU; DISCRETE -> trial: GPU time per block vs one CPU thread's time per block on the same
 *     load (nr_gpu_path_finish decides); CPU tier or no module -> CPU.
 * No link dependency on the platform library: its functions are found with dlsym in the running process (absent in the pure-CPU unit tests of iq_chan:
 * the CPU backend, as before). No environment variable or option of its own. */
#define _GNU_SOURCE
#include "iq_chan.h"
#include "shm_iq_chan_gpu.h"
#include "nr_gpu_path.h"
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef int (*path_begin_fn)(nr_gpu_path_t, int, int);
typedef int (*path_finish_fn)(nr_gpu_path_t, double, double, int, const char *);
typedef size_t (*headroom_fn)(void);

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_decided;
static const iq_chan_backend_t *g_choice;
static void *g_dl;
static const iq_chan_gpu_api_t *g_api;

const iq_chan_gpu_api_t *iq_chan_gpu_load(void)
{
  if (g_api) return g_api;
  void *sym = dlsym(RTLD_DEFAULT, "nr_gpu_path_begin");
  Dl_info di;
  if (!sym || !dladdr(sym, &di) || !di.dli_fname) return NULL;
  char path[4096];
  const char *slash = strrchr(di.dli_fname, '/');
  const int dirlen = slash ? (int)(slash - di.dli_fname) : 0;
  snprintf(path, sizeof(path), "%.*s%slibshm_iq_chan_gpu.so", dirlen, di.dli_fname, slash ? "/" : "");
  g_dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!g_dl) {
    fprintf(stderr, "SENSING: IQ_CHAN_GPU module not loaded: %s\n", dlerror());
    return NULL;
  }
  const iq_chan_gpu_api_t *(*entry)(void) = (const iq_chan_gpu_api_t *(*)(void))dlsym(g_dl, "iq_chan_gpu_api");
  const iq_chan_gpu_api_t *a = entry ? entry() : NULL;
  if (!a || a->abi != IQ_CHAN_GPU_ABI) {
    fprintf(stderr, "SENSING: IQ_CHAN_GPU module %s has no matching entry (abi %d, want %d)\n", path, a ? a->abi : -1, IQ_CHAN_GPU_ABI);
    return NULL;
  }
  g_api = a;
  return a;
}

/* deterministic test input: noise plus one tone per cell centre per channel (so every filter carries signal) */
static void selfcheck_input(int16_t *x, int n, double rate, const iq_chan_gpu_cell_t *cells, int nc, unsigned ch)
{
  uint32_t s = 0x9e3779b9u * (ch + 1);
  for (int i = 0; i < n; ++i) {
    double re = 0, im = 0;
    for (int k = 0; k < nc; ++k) {
      const double f = cells[k].shift_hz + (0.37 + 0.11 * ch) * cells[k].passband_hz * (k & 1 ? -1 : 1);
      const double ph = fmod(2 * M_PI * f / rate * i, 2 * M_PI);
      re += 2500 * cos(ph); im += 2500 * sin(ph);
    }
    s = s * 1664525u + 1013904223u;
    re += (int)(s >> 22) - 512;
    s = s * 1664525u + 1013904223u;
    im += (int)(s >> 22) - 512;
    x[2 * i] = (int16_t)lrint(re); x[2 * i + 1] = (int16_t)lrint(im);
  }
}

int iq_chan_gpu_selfcheck(const iq_chan_load_t *l, int block, iq_chan_gpu_check_t *res)
{
  const iq_chan_gpu_api_t *a = iq_chan_gpu_load();
  memset(res, 0, sizeof(*res));
  if (!a || !l || !l->n_cells || !l->nch || l->nch > SHM_IQ_MAX_CH) return -1;
  if (block <= 0) block = IQ_CHAN_DEFAULT_BLOCK;
  iq_chan_gpu_cell_t cells[IQ_CHAN_MAX_CELLS];
  for (unsigned k = 0; k < l->n_cells; ++k) {
    cells[k].decim = l->cell[k].decim > 0 ? l->cell[k].decim : 1;
    cells[k].passband_hz = l->cell[k].passband_hz > 0 ? l->cell[k].passband_hz : 0.3 * l->in_rate / cells[k].decim;
    cells[k].shift_hz = l->cell[k].shifted || cells[k].decim == 1 ? 0.1237 * l->in_rate * (k & 1 ? 1 : -1) : 0;
  }
  const int nb = 3;
  int16_t *in[SHM_IQ_MAX_CH] = {0};
  int rc = -1;
  for (unsigned c = 0; c < l->nch; ++c) {
    if (!(in[c] = malloc((size_t)block * nb * 4))) goto out;
    selfcheck_input(in[c], block * nb, l->in_rate, cells, (int)l->n_cells, c);
  }
  rc = a->compare(l->nch, l->in_rate, block, nb, cells, (int)l->n_cells, (const int16_t *const *)in, res);
out:
  for (unsigned c = 0; c < SHM_IQ_MAX_CH; ++c) free(in[c]);
  return rc;
}

static double med3(const double *v) { const double a = v[0], b = v[1], c = v[2]; return a < b ? (b < c ? b : a < c ? c : a) : (a < c ? a : b < c ? c : b); }
static const char *place_name(int p) { return p == IQ_CHAN_GPU_PLACE_GROUP ? "group" : "rt"; }

const iq_chan_backend_t *iq_chan_path_select(const iq_chan_load_t *load)
{
  path_begin_fn begin = (path_begin_fn)dlsym(RTLD_DEFAULT, "nr_gpu_path_begin");
  if (!begin) return NULL; /* no GPU platform in this process: CPU, as before */
  pthread_mutex_lock(&g_mu);
  if (g_decided) { const iq_chan_backend_t *b = g_choice; pthread_mutex_unlock(&g_mu); return b; }
  const iq_chan_gpu_api_t *a = iq_chan_gpu_load();
  iq_chan_gpu_check_t chk = {0};
  size_t bytes = 0, head = SIZE_MAX;
  int gpu_ok = a && load && load->n_cells && a->available();
  const char *why = !a ? "no_module" : !gpu_ok ? "no_device" : "";
  if (gpu_ok && (iq_chan_gpu_selfcheck(load, 0, &chk) || !chk.ok)) { gpu_ok = 0; why = "selfcheck"; }
  if (gpu_ok) {
    /* the module's own admission rule: committed bytes of every GPU module + the channelizer's <= the budget B (shm_iq_chan_gpu.cu mb_acquire) */
    headroom_fn tot = (headroom_fn)dlsym(RTLD_DEFAULT, "nr_gpu_membudget_total"), com = (headroom_fn)dlsym(RTLD_DEFAULT, "nr_gpu_membudget_committed_total");
    bytes = a->bytes(load, 0);
    const size_t B = tot ? tot() : SIZE_MAX, used = com ? com() : 0;
    head = B == SIZE_MAX ? SIZE_MAX : B > used ? B - used : 0;
    if (bytes > head) { gpu_ok = 0; why = "gpu_mem"; }
  }
  int st = begin(NR_GPU_PATH_CHAN, gpu_ok, NR_GPU_PATH_MEASURE_BENCH);
  if (st == NR_GPU_PATH_TRIAL) { /* DISCRETE: GPU vs one CPU thread on this load, us per block (medians of 3 short runs each) */
    path_finish_fn fin = (path_finish_fn)dlsym(RTLD_DEFAULT, "nr_gpu_path_finish");
    double g[3], c[3];
    int n = 0;
    for (; n < 3; ++n) {
      iq_chan_gpu_bench_t gb;
      iq_chan_bench_t cb;
      if (a->bench(load, 0, 0.1, &gb) || iq_chan_cpu_bench(load, 0, 0.1, &cb) || cb.blocks <= 0) break;
      g[n] = gb.wall_ms_per_block * 1e3;
      c[n] = 1e6 * cb.seconds / cb.blocks;
    }
    char det[96];
    snprintf(det, sizeof(det), "load=%u cells x %u ch at %.2f MS/s", load->n_cells, load->nch, load->in_rate * 1e-6);
    if (fin) st = fin(NR_GPU_PATH_CHAN, n == 3 ? med3(g) : NAN, n == 3 ? med3(c) : NAN, n == 3 ? 3 : 0, det);
  }
  if (a)
    fprintf(stderr, "SENSING: IQ_CHAN_GPU gpu_ok=%d%s%s selfcheck_max_lsb=%ld selfcheck_differing=%ld/%ld bytes=%zu headroom_mb=%lld tier=%s placement=%s choice=%s\n",
            gpu_ok, gpu_ok ? "" : " why=", why, chk.max_lsb, chk.differing, chk.total, bytes, head == SIZE_MAX ? -1LL : (long long)(head >> 20),
            a->tier_shared() ? "shared" : "discrete", place_name(a->placement()), st == NR_GPU_PATH_GPU ? "gpu" : "cpu");
  g_choice = st == NR_GPU_PATH_GPU && gpu_ok ? a->backend : NULL;
  if (g_choice) iq_chan_backend_register(g_choice);
  g_decided = 1;
  pthread_mutex_unlock(&g_mu);
  return g_choice;
}

void iq_chan_gpu_sel_test_reset(void)
{
  pthread_mutex_lock(&g_mu);
  g_decided = 0;
  g_choice = NULL;
  pthread_mutex_unlock(&g_mu);
}
