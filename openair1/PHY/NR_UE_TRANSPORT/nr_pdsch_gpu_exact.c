/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPU6: bit-exact GPU FEP for the passive PDSCH decoder -- see nr_pdsch_gpu_exact.h. */
#include "nr_pdsch_gpu_exact.h"
#include "nr_pdsch_gpu_fep.h"
#include "nr_pdsch_gpu_demod_host.h"
#include "nr_pdsch_gpu_ldpc_host.h"
#include "nr_pdsch_gpu_chest.h"
#include "PHY/MODULATION/modulation_UE.h"
#include "common/utils/LOG/log.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_cpu_route.h" /* BATCHCPU */

typedef int (*exact_fn_t)(const int16_t *const *, uint32_t, uint32_t, double, const int16_t *, uint16_t, int16_t *, size_t, int);
static exact_fn_t g_fn;
static void (*g_stats)(int *, uint64_t *, uint64_t *);
static int (*g_delay_fn)(const int16_t *const *, int, int *);
static nr_gpu_chest_fn g_chest_fn; /* GPU-DEMOD stage 2 */
static _Atomic uint64_t g_delay_rows, g_delay_calls, g_delay_cpu, g_delay_checked, g_delay_bad;
static _Atomic int g_on;
static int g_check = 1;
static _Atomic uint64_t g_calls, g_fallback, g_chk_ok, g_chk_bad, g_ns_sum, g_ns_max;
static _Atomic uint64_t g_hist[16];
static _Atomic uint64_t g_pdcch_gpu, g_pdcch_cpu; /* ISAC_PDCCH_GPU_EXACT_FEP: CORESET symbols on the GPU / CPU */ /* wall time incl. lock wait, 64 us buckets */

/* now_ns: monotonic clock in nanoseconds (timing of GPU calls).
 *   How it works: clock_gettime(CLOCK_MONOTONIC).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.c:139, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.c:143.
 */
static uint64_t now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* nr_pdsch_gpu_exact_warmup: one-time setup of the bit-exact GPU FEP: enables it only when ISAC_PDSCH_GPU_EXACT_FEP == 1 and the cell matches.
 *   How it works: Requires N=4096, <= 4 RX and 14 symbols, else logs and returns 0 (CPU FEP). dlopens libpdsch_gpu.so (cwd fallback), resolves nr_gpu_fep_api, nr_pdsch_gpu_exact_fep2, _stats and _delay, calls api->init. ISAC_PDSCH_GPU_EXACT_CHECK sets g_check (default 1) and g_on is set. Returns 1 when enabled.
 *   ISAC_* flags read: ISAC_PDSCH_GPU_EXACT_CHECK, ISAC_PDSCH_GPU_EXACT_FEP.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.h:14, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:2116.
 */
static void (*g_lowprio_fn)(int); /* GPUSCHED ISAC_PDSCH_GPU_PRIO=1: libpdsch_gpu.so nr_pdsch_gpu_set_lowprio */
void nr_pdsch_gpu_exact_set_lowprio(int low)
{
  if (g_lowprio_fn)
    g_lowprio_fn(low);
}

int nr_pdsch_gpu_exact_warmup(PHY_VARS_NR_UE *ue)
{
  const char *pr = getenv("ISAC_PDSCH_GPU_PRIO");
  if (pr && atoi(pr) == 1) {
    void *hp = dlopen("libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
    if (!hp)
      hp = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
    g_lowprio_fn = hp ? (void (*)(int))dlsym(hp, "nr_pdsch_gpu_set_lowprio") : NULL;
    LOG_A(PHY, "SENSING: ISAC_PDSCH_GPU_PRIO=1: probes and retry bodies on low-priority PDSCH GPU streams (%s)\n",
          g_lowprio_fn ? "ok" : "module unavailable, off");
  }
  if (ue)
    nr_pdsch_gpu_demod_warmup(&ue->frame_parms); /* GPU-DEMOD: independent knob, same module and warm-up point */
  nr_pdsch_gpu_ldpc_enabled(); /* GPUSHARE: GPU LDPC contexts and graphs before RX, not at the first TB (no-op when off) */
  const char *e = getenv("ISAC_PDSCH_GPU_EXACT_FEP");
  if (!e || atoi(e) != 1 || !ue)
    return 0;
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  if (fp->ofdm_symbol_size != 4096 || fp->nb_antennas_rx > 4 || fp->symbols_per_slot != 14) {
    LOG_W(PHY, "SENSING: GPU_EXACT_FEP needs N=4096, <=4 RX, 14 symbols (N=%d) -- CPU FEP\n", fp->ofdm_symbol_size);
    return 0;
  }
  void *h = dlopen("libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  const nr_gpu_fep_api_t *(*get)(void) = h ? (const nr_gpu_fep_api_t *(*)(void))dlsym(h, "nr_gpu_fep_api") : NULL;
  const nr_gpu_fep_api_t *api = get ? get() : NULL;
  g_fn = h ? (exact_fn_t)dlsym(h, "nr_pdsch_gpu_exact_fep2") : NULL;
  g_stats = h ? (void (*)(int *, uint64_t *, uint64_t *))dlsym(h, "nr_pdsch_gpu_exact_stats") : NULL;
  g_delay_fn = h ? (int (*)(const int16_t *const *, int, int *))dlsym(h, "nr_pdsch_gpu_exact_delay") : NULL;
  g_chest_fn = h ? (nr_gpu_chest_fn)dlsym(h, "nr_pdsch_gpu_exact_chest") : NULL;
  const nr_gpu_fep_cfg_t cfg = {.nant = fp->nb_antennas_rx,
                                .ofdm_symbol_size = fp->ofdm_symbol_size,
                                .nb_prefix_samples = fp->nb_prefix_samples,
                                .nb_prefix_samples0 = fp->nb_prefix_samples0,
                                .symbols_per_slot = fp->symbols_per_slot,
                                .first_carrier_offset = fp->first_carrier_offset,
                                .n_rb_dl = fp->N_RB_DL,
                                .samples_per_ms = fp->samples_per_subframe,
                                .ofdm_offset_divisor = (int)fp->ofdm_offset_divisor};
  if (!api || !g_fn || api->init(&cfg) != 0) {
    LOG_W(PHY, "SENSING: GPU_EXACT_FEP requested but libpdsch_gpu.so is unavailable -- CPU FEP\n");
    return 0;
  }
  const char *c = getenv("ISAC_PDSCH_GPU_EXACT_CHECK");
  g_check = c ? atoi(c) : 1;
  atomic_store(&g_on, 1);
  LOG_A(PHY, "SENSING: GPU_EXACT_FEP enabled: %d RX, N=%d, bit-exact FEP on the GPU, CPU chest/demod/decode; check=%d\n",
        fp->nb_antennas_rx, fp->ofdm_symbol_size, g_check);
  return 1;
}

/* CPU reference for the self-check: nr_slot_fep_ant() of (ant, sym) into a thread-local slot buffer. */
/* check_symbol: self-check: recomputes one (antenna, symbol) FEP on the CPU and compares it bit-exactly with the GPU output.
 *   How it works: Uses a thread-local reference buffer; if it cannot be allocated the check counts as a pass. Returns 1 on an exact match.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.c:164.
 */
static int check_symbol(PHY_VARS_NR_UE *ue, const NR_DL_FRAME_PARMS *fp, int slot, int ant, int sym, const c16_t *gpu,
                        c16_t **rxdata)
{
  static __thread c16_t *ref;
  static __thread size_t ref_n;
  const size_t n = (size_t)fp->nb_antennas_rx * fp->samples_per_slot_wCP;
  if (ref_n < n) {
    free(ref);
    ref = aligned_alloc(64, ((n * sizeof(c16_t) + 63) / 64) * 64);
    ref_n = ref ? n : 0;
  }
  if (!ref)
    return 0; /* cannot check: count as pass, the path stays as configured */
  nr_slot_fep_ant(ue, fp, slot, sym, ant, (c16_t(*)[fp->samples_per_slot_wCP])ref, link_type_dl, 0, rxdata);
  const size_t o = (size_t)ant * fp->samples_per_slot_wCP + (size_t)sym * fp->ofdm_symbol_size;
  return memcmp(ref + o, gpu + o, sizeof(c16_t) * fp->ofdm_symbol_size) == 0;
}

static int exact_run(PHY_VARS_NR_UE *ue, const NR_DL_FRAME_PARMS *fp, int slot, uint16_t mask, c16_t *rxdataF, size_t stride,
                     c16_t **rxdata, int try_only);

/* nr_pdsch_gpu_exact_fep_ranges: GPU exact FEP for PDSCH symbol ranges.
 *   How it works: Returns -1 when the GPU path is off. Builds a 14-bit symbol mask from the ranges and calls exact_run with try_only = 0.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.h:17, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2517.
 */
int nr_pdsch_gpu_exact_fep_ranges(PHY_VARS_NR_UE *ue, const NR_DL_FRAME_PARMS *fp, int slot, const int (*rng)[2], int n_rng,
                                  c16_t *rxdataF, size_t stride, c16_t **rxdata)
{
  if (!atomic_load_explicit(&g_on, memory_order_relaxed) || nr_pdsch_route_cpu()) /* BATCHCPU */
    return -1;
  uint16_t mask = 0;
  for (int g = 0; g < n_rng; g++)
    for (int m = rng[g][0]; m < rng[g][0] + rng[g][1] && m < 14; m++)
      mask |= (uint16_t)(1u << m);
  return exact_run(ue, fp, slot, mask, rxdataF, stride, rxdata, 0);
}

/* nr_pdcch_gpu_exact_fep_symbol: GPU exact FEP for one PDCCH symbol.
 *   How it works: ISAC_PDCCH_GPU_EXACT_FEP == 1 enables it (read once). Calls exact_run with try_only = 1, so the scan falls back to CPU when the GPU is busy. Counts GPU and CPU uses.
 *   ISAC_* flags read: ISAC_PDCCH_GPU_EXACT_FEP.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:5448, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.h:21.
 */
int nr_pdcch_gpu_exact_fep_symbol(PHY_VARS_NR_UE *ue, const NR_DL_FRAME_PARMS *fp, int slot, int symbol, c16_t *rxdataF,
                                  c16_t **rxdata)
{
  static int on = -1;
  if (on < 0) {
    const char *e = getenv("ISAC_PDCCH_GPU_EXACT_FEP");
    on = e && atoi(e) == 1;
  }
  if (!on || !atomic_load_explicit(&g_on, memory_order_relaxed) || symbol < 0 || symbol >= 14)
    return -1;
  /* the scan never waits for a PDSCH consumer's GPU call: busy -> CPU FEP */
  const int rc = exact_run(ue, fp, slot, (uint16_t)(1u << symbol), rxdataF, fp->samples_per_slot_wCP, rxdata, 1);
  atomic_fetch_add_explicit(rc ? &g_pdcch_cpu : &g_pdcch_gpu, 1, memory_order_relaxed);
  return rc;
}

/* exact_run: core of the GPU exact FEP: runs the masked symbols on the GPU and self-checks them.
 *   How it works: Refuses slots with a frequency offset (cont_fo_comp, not synchronized, or a branch FO) and returns -1 with a one-time log. Calls the GPU entry, times it, and records the histogram. Self-check follows g_check: 1 checks a rotating symbol every 16 calls (or on every try-only call), 2 checks all symbols.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.c:102, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.c:116.
 */
static int exact_run(PHY_VARS_NR_UE *ue, const NR_DL_FRAME_PARMS *fp, int slot, uint16_t mask, c16_t *rxdataF, size_t stride,
                     c16_t **rxdata, int try_only)
{
  /* The CPU FEP de-rotates a frequency offset only with --cont-fo-comp or a per-branch FO; the GPU
   * does not reproduce that rotation, so those slots stay on the CPU. */
  bool fo = ue->cont_fo_comp != 0 || !ue->is_synchronized;
  for (int a = 0; a < fp->nb_antennas_rx && !fo; a++)
    fo = nr_ue_get_branch_fo_hz(a) != 0.0;
  if (fo || !mask) {
    if (atomic_fetch_add_explicit(&g_fallback, 1, memory_order_relaxed) == 0)
      LOG_A(PHY, "SENSING: GPU_EXACT_FEP first fallback: cont_fo_comp=%d synchronized=%d branch_fo0=%.1f mask=0x%x\n",
            ue->cont_fo_comp, ue->is_synchronized, nr_ue_get_branch_fo_hz(0), mask);
    return -1;
  }
  const int16_t *rx[4] = {0};
  for (int a = 0; a < fp->nb_antennas_rx; a++)
    rx[a] = (const int16_t *)rxdata[a];
  const c16_t *rot = fp->symbol_rotation[link_type_dl] + (slot % fp->slots_per_subframe) * fp->symbols_per_slot;
  const uint64_t t0 = now_ns();
  /* no lock here: the module gives each concurrent call its own stream (direct mode) */
  const int rc = g_fn(rx, 2u * fp->samples_per_frame, get_samples_slot_timestamp(fp, slot), 0.0, (const int16_t *)rot, mask,
                      (int16_t *)rxdataF, stride, try_only);
  const uint64_t dt = now_ns() - t0;
  if (rc) {
    if (atomic_fetch_add_explicit(&g_fallback, 1, memory_order_relaxed) == 0)
      LOG_A(PHY, "SENSING: GPU_EXACT_FEP first fallback: GPU call refused (rc=%d, slot %d)\n", rc, slot);
    return -1;
  }
  const uint64_t n = atomic_fetch_add_explicit(&g_calls, 1, memory_order_relaxed) + 1;
  atomic_fetch_add_explicit(&g_ns_sum, dt, memory_order_relaxed);
  uint64_t mx = atomic_load(&g_ns_max);
  while (dt > mx && !atomic_compare_exchange_weak(&g_ns_max, &mx, dt)) {}
  atomic_fetch_add_explicit(&g_hist[dt / 64000 < 15 ? dt / 64000 : 15], 1, memory_order_relaxed);
  if (g_check == 2 || (g_check == 1 && (!try_only || n % 16 == 0))) {
    /* spot check (1): one masked (antenna, symbol) per call, rotating; full check (2): all of them */
    int pairs[4 * 14][2], np = 0, ok = 1;
    for (int a = 0; a < fp->nb_antennas_rx; a++)
      for (int m = 0; m < 14; m++)
        if (mask >> m & 1) {
          pairs[np][0] = a;
          pairs[np++][1] = m;
        }
    for (int k = g_check == 1 ? (int)(n % np) : 0; k < np && ok; k++) {
      ok = check_symbol(ue, fp, slot, pairs[k][0], pairs[k][1], rxdataF, rxdata);
      if (g_check == 1)
        break;
    }
    atomic_fetch_add_explicit(ok ? &g_chk_ok : &g_chk_bad, 1, memory_order_relaxed);
    if (!ok) {
      atomic_store(&g_on, 0);
      LOG_E(PHY, "SENSING: GPU_EXACT_FEP self-check MISMATCH at slot %d -- GPU FEP disabled, CPU FEP from now on\n", slot);
      return -1;
    }
  }
  if (n == 1 || n == 10 || n == 100 || n == 1000 || n % 2000 == 0) {
    uint64_t h[16], tot = 0, acc = 0;
    for (int i = 0; i < 16; i++) tot += h[i] = atomic_load(&g_hist[i]);
    int p50 = 15, p99 = 15;
    for (int i = 0; i < 16; i++) {
      acc += h[i];
      if (p50 == 15 && acc * 2 >= tot) p50 = i;
      if (acc * 100 >= tot * 99) { p99 = i; break; }
    }
    int direct = -1;
    uint64_t late = 0, busy = 0;
    if (g_stats)
      g_stats(&direct, &late, &busy);
    LOG_A(PHY, "SENSING: GPU_EXACT_FEP calls=%lu fallback=%lu (late=%lu busy=%lu) check_ok=%lu check_bad=%lu direct=%d pdcch_gpu=%lu pdcch_cpu=%lu "
          "delay_calls=%lu delay_rows=%lu delay_cpu=%lu delay_checked=%lu delay_bad=%lu "
          "mean=%.0f us p50<=%d us p99<=%d us max=%.0f us (wall)\n",
          (unsigned long)n, (unsigned long)atomic_load(&g_fallback), (unsigned long)late, (unsigned long)busy,
          (unsigned long)atomic_load(&g_chk_ok), (unsigned long)atomic_load(&g_chk_bad), direct,
          (unsigned long)atomic_load(&g_pdcch_gpu), (unsigned long)atomic_load(&g_pdcch_cpu),
          (unsigned long)atomic_load(&g_delay_calls), (unsigned long)atomic_load(&g_delay_rows),
          (unsigned long)atomic_load(&g_delay_cpu), (unsigned long)atomic_load(&g_delay_checked),
          (unsigned long)atomic_load(&g_delay_bad),
          atomic_load(&g_ns_sum) / 1e3 / n, 64 * (p50 + 1), 64 * (p99 + 1), atomic_load(&g_ns_max) / 1e3);
  }
  return 0;
}

/* GPU6 ISAC_CHEST_GPU_DELAY: the channel estimator's delay transform for up to 16 LS rows (see
 * nr_dl_channel_estimation.c). Shares the module lock with the FEP. -1 -> the estimator runs the CPU IDFT. */
/* nr_pdsch_gpu_exact_delay_rows: GPU delay estimation for n rows of the channel estimate.
 *   How it works: Returns -1 when the GPU path or the delay entry is missing. On a GPU error it counts the rows as CPU fallback.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:83, openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:1640, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.h:25.
 */
int nr_pdsch_gpu_exact_delay_rows(const c16_t *const *rows, int n, int *out)
{
  if (!atomic_load_explicit(&g_on, memory_order_relaxed) || !g_delay_fn)
    return -1;
  const int rc = g_delay_fn((const int16_t *const *)rows, n, out);
  if (rc)
    atomic_fetch_add_explicit(&g_delay_cpu, (uint64_t)n, memory_order_relaxed);
  return rc;
}
/* nr_pdsch_gpu_exact_delay_note: accumulates the delay-check counters.
 *   How it works: Adds gpu_rows, checked and bad to the atomic counters (calls and rows only when gpu_rows > 0).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:84, openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:101, openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:1646, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.h:26.
 */
void nr_pdsch_gpu_exact_delay_note(int gpu_rows, int checked, int bad)
{
  if (gpu_rows) {
    atomic_fetch_add_explicit(&g_delay_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_delay_rows, (uint64_t)gpu_rows, memory_order_relaxed);
  }
  atomic_fetch_add_explicit(&g_delay_checked, (uint64_t)checked, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_delay_bad, (uint64_t)bad, memory_order_relaxed);
}

/* GPU-DEMOD stage 2: the module's chest entry once the exact path is on (NULL otherwise). */
nr_gpu_chest_fn nr_pdsch_gpu_exact_chest_entry(void)
{
  return atomic_load_explicit(&g_on, memory_order_relaxed) ? g_chest_fn : NULL;
}
