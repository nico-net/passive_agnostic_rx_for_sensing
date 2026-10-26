/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPU6: bit-exact GPU FEP for the passive PDSCH decoder -- see nr_pdsch_gpu_exact.h. */
#include "PHY/MODULATION/nr_sfo_state.h"
#include "PHY/TOOLS/nr_stage_split.h"
#include "nr_pdsch_gpu_exact.h"
#include "nr_pdsch_gpu_fep.h"
#include "nr_pdsch_gpu_demod_host.h"
#include "nr_pdsch_gpu_ldpc_host.h"
#include "nr_pdsch_gpu_chest.h"
#include "PHY/MODULATION/modulation_UE.h"
#include "common/utils/LOG/log.h"
#include "PHY/TOOLS/nr_gpu_mem.h" /* nr_gpu_model() */
#include "PHY/TOOLS/nr_gpu_memstat.h"
#include "PHY/NR_UE_TRANSPORT/nr_gpu_stage_census.h"
#include "PHY/TOOLS/nr_gpu_path.h" /* per-path GPU/CPU choice */
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_cpu_route.h" /* BATCHCPU */

typedef int (*exact_fn_t)(const int16_t *const *, uint32_t, uint32_t, double, const int16_t *, uint16_t, int16_t *, size_t, int, const uint32_t *, int);
static exact_fn_t g_fn;
static void (*g_stats)(int *, uint64_t *, uint64_t *);
static int (*g_delay_fn)(const int16_t *const *, int, int *);
static nr_gpu_chest_fn g_chest_fn; /* GPU-DEMOD stage 2 */
static nr_gpu_stage_stats_fn g_stage_stats; /* Q4: census of the staged DISCRETE runs of the module (NULL on a module without it) */
static _Atomic uint64_t g_delay_rows, g_delay_calls, g_delay_cpu, g_delay_checked, g_delay_bad;
static _Atomic int g_on;     /* the module's exact working set is up (also gates the chest / delay entries and the graph pass warm-up) */
static _Atomic int g_fep_on; /* the FEP itself runs on the GPU: g_on and the per-path choice (nr_gpu_path.h, NR_GPU_PATH_FEP) */
static _Atomic uint64_t g_calls, g_fallback, g_chk_ok, g_chk_bad, g_ns_sum, g_ns_max;
static _Atomic uint64_t g_hist[16];

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

/* nr_pdsch_gpu_exact_warmup: one-time setup of the bit-exact GPU FEP: enables it only when the cell matches.
 *   How it works: Requires N=4096, <= 4 RX and 14 symbols, else logs and returns 0 (CPU FEP). dlopens libpdsch_gpu.so (cwd fallback), resolves nr_gpu_fep_api, nr_pdsch_gpu_exact_fep2, _stats and _delay, calls api->init. g_on is set. Returns 1 when enabled.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.h:14, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:2116.
 */
/* ISAC_E2E_TRACE stage split: switches the accumulators of this process and of libpdsch_gpu.so on; nr_ss_gpu_ptr() = the calling thread's GPU-side ones */
static uint64_t *(*g_ss_ptr_fn)(void);
extern int nr_passive_e2e_trace(void);
uint64_t *nr_ss_gpu_ptr(void) { return g_ss_ptr_fn ? g_ss_ptr_fn() : NULL; }
static void nr_ss_gpu_init(void)
{
  if (!nr_passive_e2e_trace())
    return;
  nr_ss_on = 1;
  void *h = dlopen("libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  void (*en)(int) = h ? (void (*)(int))dlsym(h, "nr_pdsch_gpu_ss_enable") : NULL;
  g_ss_ptr_fn = h ? (uint64_t * (*)(void)) dlsym(h, "nr_pdsch_gpu_ss_ptr") : NULL;
  if (en)
    en(1);
}

static void nr_pdsch_gpu_exact_fep_bench(const NR_DL_FRAME_PARMS *fp);
int nr_pdsch_gpu_exact_warmup(PHY_VARS_NR_UE *ue)
{
  nr_ss_gpu_init();
  if (ue)
    nr_pdsch_gpu_demod_warmup(&ue->frame_parms); /* GPU-DEMOD: independent knob, same module and warm-up point */
  nr_pdsch_gpu_ldpc_enabled(); /* GPUSHARE: GPU LDPC contexts and graphs before RX, not at the first TB (no-op when off) */
  if (!ue)
    return 0;
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  if (fp->ofdm_symbol_size > 4096 || fp->nb_antennas_rx > 4 || fp->symbols_per_slot != 14) {
    LOG_W(PHY, "SENSING: GPU_EXACT_FEP needs N <= 4096, <=4 RX, 14 symbols (N=%d) -- CPU FEP\n", fp->ofdm_symbol_size);
    nr_gpu_path_begin(NR_GPU_PATH_FEP, 0, NR_GPU_PATH_MEASURE_NONE);
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
    nr_gpu_path_begin(NR_GPU_PATH_FEP, 0, NR_GPU_PATH_MEASURE_NONE);
    return 0;
  }
  /* the bit-exact working set exists only for an FFT size the host's twin reproduces (N = 4096; x86: also 256..2048, self-tested at init) */
  int (*ready)(void) = (int (*)(void))dlsym(h, "nr_pdsch_gpu_exact_ready");
  if (ready ? !ready() : fp->ofdm_symbol_size != 4096) {
    LOG_W(PHY, "SENSING: GPU_EXACT_FEP: no bit-identical GPU twin of the host DFT at N=%d -- CPU FEP\n", fp->ofdm_symbol_size);
    nr_gpu_path_begin(NR_GPU_PATH_FEP, 0, NR_GPU_PATH_MEASURE_NONE);
    return 0;
  }
  if (fp->ofdm_symbol_size != 4096) { /* the chest / delay twins (idft4096) and the graph pass are N = 4096 only */
    g_delay_fn = NULL;
    g_chest_fn = NULL;
  }
  g_stage_stats = (nr_gpu_stage_stats_fn)dlsym(h, "nr_gpu_stage_stats");
  if (nr_gpu_model() != NR_GPU_MODEL_DIRECT && g_chest_fn) { /* Q4: the chest / delay entries read host rows in place on DIRECT; on DISCRETE they are staged (the FEP always was) */
    int (*sinit)(const int16_t *, size_t, int, int) = (int (*)(const int16_t *, size_t, int, int))dlsym(h, "nr_pdsch_gpu_exact_stage_init");
    if (!sinit || sinit((const int16_t *)fp->delay_table[0], sizeof(fp->delay_table), NR_MAX_OFDM_SYMBOL_SIZE, MAX_DELAY_COMP) != 0) {
      LOG_W(PHY, "SENSING: GPU_EXACT chest/delay staging unavailable (DISCRETE tier) -- CPU chest and delay\n");
      g_chest_fn = NULL;
      g_delay_fn = NULL;
    }
  }
  atomic_store(&g_on, 1);
  const char *(*twin)(void) = (const char *(*)(void))dlsym(h, "nr_gpu_isa_twin"); /* the host CPU's DFT / rotation twins (self-test at init) */
  /* per-path choice: SHARED (GB10) -> GPU at once, unchanged; DISCRETE -> the timed self-benchmark below decides (fail closed) */
  if (nr_gpu_path_begin(NR_GPU_PATH_FEP, 1, NR_GPU_PATH_MEASURE_BENCH) == NR_GPU_PATH_TRIAL)
    nr_pdsch_gpu_exact_fep_bench(fp);
  atomic_store(&g_fep_on, nr_gpu_path_use_gpu(NR_GPU_PATH_FEP));
  if (atomic_load(&g_fep_on))
    LOG_A(PHY, "SENSING: GPU_EXACT_FEP enabled: %d RX, N=%d, bit-exact FEP on the GPU, CPU chest/demod/decode; check=1 %s\n",
          fp->nb_antennas_rx, fp->ofdm_symbol_size, twin ? twin() : "dft_twin=?");
  else
    LOG_A(PHY, "SENSING: GPU_EXACT_FEP ready but the per-path choice is CPU (GPU_PATH path=fep): CPU FEP, N=%d %s\n", fp->ofdm_symbol_size,
          twin ? twin() : "dft_twin=?");
  /* the standalone chest / delay entries: in place on DIRECT (SHARED: GPU, unchanged); on DISCRETE the staged variants (Q4) are measured by a paired online trial
   * (nr_pdsch_gpu_chest_grant: the GPU chest and the CPU estimator on the same grant, both timed); without staging the entries are NULL (gpu_unavailable -> CPU) */
  nr_gpu_path_begin(NR_GPU_PATH_CHEST, g_chest_fn != NULL || g_delay_fn != NULL,
                    nr_gpu_model() == NR_GPU_MODEL_DIRECT ? NR_GPU_PATH_MEASURE_NONE : NR_GPU_PATH_MEASURE_TRIAL);
  return 1;
}

/* nr_pdsch_gpu_exact_fep_bench: the per-path self-benchmark of the FEP (DISCRETE tier, or NR_GPU_PATH_SELECT=bench).
 *   One synthetic slot (random IQ, slot 1 of a private ring, every symbol and antenna) through the GPU entry (staged path: H2D window, kernels,
 *   D2H, host copy) and through nr_slot_fep_ant() on the CPU, 2 warm-up + 8 timed runs each; medians. The CPU time is the per-antenna wall time
 *   (total / nb_rx): the passive decode runs one FEP task per antenna on the thread pool, so its wall time is one antenna's work when nb_rx cores
 *   are free (the CPU's best case). Every GPU output is compared bit for bit with the CPU's; a mismatch, a GPU refusal or an allocation failure
 *   ends the benchmark as bench_fail (CPU). The UE handed to the CPU FEP is a zeroed copy flagged synchronized without FO compensation, so
 *   both sides transform the same windows (the GPU path refuses FO slots at run time). */
static void nr_pdsch_gpu_exact_fep_bench(const NR_DL_FRAME_PARMS *fp)
{
  enum { WARM = 2, RUNS = 8 };
  const int nant = fp->nb_antennas_rx, N = fp->ofdm_symbol_size, slot = 1;
  const size_t ring = 2u * (size_t)fp->samples_per_frame;
  const size_t fsz = (size_t)nant * fp->samples_per_slot_wCP;
  c16_t *rx[4] = {0};
  c16_t *fg = aligned_alloc(64, ((fsz * sizeof(c16_t) + 63) / 64) * 64), *fc = aligned_alloc(64, ((fsz * sizeof(c16_t) + 63) / 64) * 64);
  PHY_VARS_NR_UE *fake = calloc(1, sizeof(*fake));
  double tg[RUNS], tc[RUNS];
  int ok = fg && fc && fake, bad = 0, ng = 0, nc = 0;
  uint32_t st = 0x5e5e6u;
  for (int a = 0; a < nant && ok; a++) {
    rx[a] = calloc(ring, sizeof(c16_t)); /* only the first slots are touched (zero pages elsewhere) */
    ok = rx[a] != NULL;
    const size_t used = 3u * (size_t)fp->samples_per_slot_wCP + 2u * (size_t)N;
    for (size_t i = 0; ok && i < used && i < ring; i++) {
      st = st * 1664525u + 1013904223u;
      rx[a][i].r = (int16_t)((int)(st >> 20) - 2048);
      st = st * 1664525u + 1013904223u;
      rx[a][i].i = (int16_t)((int)(st >> 20) - 2048);
    }
  }
  if (ok) {
    memset(fg, 0, fsz * sizeof(c16_t));
    memset(fc, 0, fsz * sizeof(c16_t));
    fake->is_synchronized = 1;
    fake->cont_fo_comp = 0;
    const int16_t *rxp[4] = {0};
    for (int a = 0; a < nant; a++) rxp[a] = (const int16_t *)rx[a];
    const c16_t *rot = fp->symbol_rotation[link_type_dl] + (slot % fp->slots_per_subframe) * fp->symbols_per_slot;
    const uint16_t mask = (uint16_t)((1u << fp->symbols_per_slot) - 1u);
    nr_sfo_pin(0); /* the bench compares against the CPU FEP without SFO (the GPU call passes no SFO steps) */
    for (int r = 0; r < WARM + RUNS && ok; r++) {
      const uint64_t t0 = now_ns();
      const int rc = g_fn(rxp, (uint32_t)ring, get_samples_slot_timestamp(fp, slot), 0.0, (const int16_t *)rot, mask, (int16_t *)fg, fp->samples_per_slot_wCP, 0, NULL, 0);
      const uint64_t t1 = now_ns();
      if (rc && r < WARM) { /* first use (module load, a late warm-up call that drains): not timed, not compared */
        for (int w = 0; w < 200 && g_fn(rxp, (uint32_t)ring, get_samples_slot_timestamp(fp, slot), 0.0, (const int16_t *)rot, mask, (int16_t *)fg,
                                         fp->samples_per_slot_wCP, 0, NULL, 0) != 0; w++)
          usleep(1000);
        continue;
      }
      if (rc) { ok = 0; break; } /* a timed call refused or late (2 ms deadline): the GPU is not usable at this rate */
      for (int a = 0; a < nant; a++)
        for (int l = 0; l < fp->symbols_per_slot; l++)
          nr_slot_fep_ant(fake, fp, slot, l, a, (c16_t(*)[fp->samples_per_slot_wCP])fc, link_type_dl, 0, rx);
      const uint64_t t2 = now_ns();
      bad += memcmp(fg, fc, fsz * sizeof(c16_t)) != 0;
      if (r >= WARM) {
        tg[ng++] = (t1 - t0) / 1e3;
        tc[nc++] = (t2 - t1) / 1e3 / nant;
      }
    }
  }
  nr_sfo_pin(INT64_MIN);
  const double mg = ok && !bad ? nr_gpu_path_median(tg, ng) : -1, mc = ok ? nr_gpu_path_median(tc, nc) : -1;
  char detail[160];
  snprintf(detail, sizeof(detail), "N=%d nb_rx=%d sym=%d cpu=per_antenna%s%s", N, nant, fp->symbols_per_slot, bad ? " MISMATCH" : "", ok ? "" : " gpu_refused_or_alloc");
  nr_gpu_path_finish(NR_GPU_PATH_FEP, mg, mc, ok && !bad ? ng : 0, detail);
  for (int a = 0; a < nant; a++) free(rx[a]);
  free(fg);
  free(fc);
  free(fake);
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
  if (!atomic_load_explicit(&g_fep_on, memory_order_relaxed) || nr_pdsch_route_cpu()) /* BATCHCPU; per-path choice */
    return -1;
  uint16_t mask = 0;
  for (int g = 0; g < n_rng; g++)
    for (int m = rng[g][0]; m < rng[g][0] + rng[g][1] && m < 14; m++)
      mask |= (uint16_t)(1u << m);
  return exact_run(ue, fp, slot, mask, rxdataF, stride, rxdata, 0);
}

/* the PDCCH scan always runs the CPU FEP (the GPU PDCCH FEP measured no scan gain); kept only for its caller */
int nr_pdcch_gpu_exact_fep_symbol(PHY_VARS_NR_UE *ue, const NR_DL_FRAME_PARMS *fp, int slot, int symbol, c16_t *rxdataF,
                                  c16_t **rxdata)
{
  (void)ue; (void)fp; (void)slot; (void)symbol; (void)rxdataF; (void)rxdata;
  return -1;
}

/* exact_run: core of the GPU exact FEP: runs the masked symbols on the GPU and self-checks them.
 *   How it works: Refuses slots with a frequency offset (cont_fo_comp, not synchronized, or a branch FO) and returns -1 with a one-time log. Calls the GPU entry, times it, and records the histogram. Self-check: a rotating symbol every 16 calls (every call when try-only).
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
  /* SFO (nr_sfo_state.h): one eps for the GPU job and the CPU spot check of this slot */
  const int64_t sfo_q = nr_sfo_eps_q();
  uint32_t sfo_dphi[14];
  const int sfo_on = nr_sfo_slot_dphi(sfo_q, fp->ofdm_symbol_size, fp->nb_prefix_samples, fp->nb_prefix_samples0, fp->numerology_index,
                                      fp->symbols_per_slot, slot, sfo_dphi);
  const uint64_t t0 = now_ns();
  /* no lock here: the module gives each concurrent call its own stream (direct mode) */
  const int rc = g_fn(rx, 2u * fp->samples_per_frame, get_samples_slot_timestamp(fp, slot), 0.0, (const int16_t *)rot, mask,
                      (int16_t *)rxdataF, stride, try_only, sfo_on ? sfo_dphi : NULL,
                      nr_sfo_kref(fp->ofdm_symbol_size, fp->first_carrier_offset, fp->ssb_start_subcarrier));
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
  if (!try_only || n % 16 == 0) {
    /* spot check: one masked (antenna, symbol) per call, rotating */
    int pairs[4 * 14][2], np = 0, ok = 1;
    for (int a = 0; a < fp->nb_antennas_rx; a++)
      for (int m = 0; m < 14; m++)
        if (mask >> m & 1) {
          pairs[np][0] = a;
          pairs[np++][1] = m;
        }
    const int k = (int)(n % np);
    nr_sfo_pin(sfo_q);
    ok = check_symbol(ue, fp, slot, pairs[k][0], pairs[k][1], rxdataF, rxdata);
    nr_sfo_pin(INT64_MIN);
    atomic_fetch_add_explicit(ok ? &g_chk_ok : &g_chk_bad, 1, memory_order_relaxed);
    if (!ok) {
      atomic_store(&g_on, 0);
      atomic_store(&g_fep_on, 0);
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
    uint64_t late = 0, busy = 0, sf[6] = {0}, sd[6] = {0}, sc[6] = {0};
    if (g_stats)
      g_stats(&direct, &late, &busy);
    if (g_stage_stats) {
      g_stage_stats(NR_GPU_STG_FEP, sf);
      g_stage_stats(NR_GPU_STG_DELAY, sd);
      g_stage_stats(NR_GPU_STG_CHEST, sc);
    }
    LOG_A(PHY, "SENSING: GPU_EXACT_FEP calls=%lu fallback=%lu (late=%lu busy=%lu) check_ok=%lu check_bad=%lu direct=%d "
          "delay_calls=%lu delay_rows=%lu delay_cpu=%lu delay_checked=%lu delay_bad=%lu "
          "mean=%.0f us p50<=%d us p99<=%d us max=%.0f us (wall) staged[fep runs=%lu h2d_kb=%lu d2h_kb=%lu | delay runs=%lu h2d_kb=%lu d2h_kb=%lu | "
          "chest runs=%lu h2d_kb=%lu d2h_kb=%lu refused=%lu]\n",
          (unsigned long)n, (unsigned long)atomic_load(&g_fallback), (unsigned long)late, (unsigned long)busy,
          (unsigned long)atomic_load(&g_chk_ok), (unsigned long)atomic_load(&g_chk_bad), direct,
          (unsigned long)atomic_load(&g_delay_calls), (unsigned long)atomic_load(&g_delay_rows),
          (unsigned long)atomic_load(&g_delay_cpu), (unsigned long)atomic_load(&g_delay_checked),
          (unsigned long)atomic_load(&g_delay_bad),
          atomic_load(&g_ns_sum) / 1e3 / n, 64 * (p50 + 1), 64 * (p99 + 1), atomic_load(&g_ns_max) / 1e3,
          (unsigned long)sf[0], (unsigned long)(sf[1] >> 10), (unsigned long)(sf[2] >> 10), (unsigned long)sd[0], (unsigned long)(sd[1] >> 10),
          (unsigned long)(sd[2] >> 10), (unsigned long)sc[0], (unsigned long)(sc[1] >> 10), (unsigned long)(sc[2] >> 10), (unsigned long)sc[5]);
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
  if (!atomic_load_explicit(&g_on, memory_order_relaxed) || !g_delay_fn || !nr_gpu_path_use_gpu(NR_GPU_PATH_CHEST))
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
  return atomic_load_explicit(&g_on, memory_order_relaxed) && nr_gpu_path_use_gpu(NR_GPU_PATH_CHEST) ? g_chest_fn : NULL;
}

/* DECODETIME: the GPU kernels read and write pageable host buffers (chest scratch,
 * LLRs, estimates, equaliser outputs) through the GB10's unified memory; the GPU's first touch of every new 4 KB page is a
 * GPU page fault that stalls the whole device (~12 us each: a 1744-row chest scratch grew by 14 MB = 40 ms, hitting every
 * consumer at once). Pinning (cudaHostRegister) maps the buffer once, at allocation. */
static int (*g_pin_fn)(void *, size_t);
static int (*g_unpin_fn)(void *);
static pthread_once_t g_pin_once = PTHREAD_ONCE_INIT;
static void pin_init(void)
{
  if (nr_gpu_model() != NR_GPU_MODEL_DIRECT) { /* the page-fault fix is for in-place reads (SHARED tier); on DISCRETE it would pin hundreds of MB for nothing */
    LOG_A(PHY, "SENSING: GPU host pin: off (tier is not SHARED)\n");
    return;
  }
  void *h = dlopen("libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  g_pin_fn = h ? (int (*)(void *, size_t))dlsym(h, "nr_pdsch_gpu_exact_pin") : NULL;
  g_unpin_fn = h ? (int (*)(void *))dlsym(h, "nr_pdsch_gpu_exact_unpin") : NULL;
  if (!g_pin_fn || !g_unpin_fn)
    g_pin_fn = NULL;
  LOG_A(PHY, "SENSING: GPU host pin: GPU-touched host buffers pinned at allocation (%s)\n", g_pin_fn ? "ok" : "module unavailable, off");
}
/* GPUMEM ledger of the host-registered buffers (kind mgd, PER_CELL of the registering thread's cell): address -> bytes + tag, so an unpin books the release. */
#define PIN_LEDGER_MAX 512
static struct { void *p; size_t bytes; uint32_t tag; } g_pin_led[PIN_LEDGER_MAX];
static pthread_mutex_t g_pin_led_mu = PTHREAD_MUTEX_INITIALIZER;
static void pin_ledger_add(void *p, size_t bytes)
{
  const uint32_t prev = nr_gpu_memstat_enter(NR_MS_PDSCH_FEP, NR_MS_PER_CELL, -1), tag = nr_gpu_memstat_current();
  nr_gpu_memstat_leave(prev);
  pthread_mutex_lock(&g_pin_led_mu);
  int i = 0;
  while (i < PIN_LEDGER_MAX && g_pin_led[i].p) i++;
  if (i < PIN_LEDGER_MAX) { g_pin_led[i].p = p; g_pin_led[i].bytes = bytes; g_pin_led[i].tag = tag; }
  pthread_mutex_unlock(&g_pin_led_mu);
  nr_gpu_memstat_note_tag(tag, NR_MS_MGD, bytes); /* a full table only loses the release of a later unpin */
}
static void pin_ledger_del(void *p)
{
  pthread_mutex_lock(&g_pin_led_mu);
  for (int i = 0; i < PIN_LEDGER_MAX; i++)
    if (g_pin_led[i].p == p) {
      nr_gpu_memstat_release_tag(g_pin_led[i].tag, NR_MS_MGD, g_pin_led[i].bytes);
      g_pin_led[i].p = NULL;
      break;
    }
  pthread_mutex_unlock(&g_pin_led_mu);
}
int nr_pdsch_gpu_host_pin(void *p, size_t bytes)
{
  pthread_once(&g_pin_once, pin_init);
  if (!g_pin_fn || !p || !bytes)
    return -1;
  const int rc = g_pin_fn(p, bytes);
  if (rc) {
    static _Atomic int logged;
    if (!atomic_fetch_or(&logged, 1))
      LOG_W(PHY, "SENSING: GPU_HOST_PIN: cudaHostRegister(%zu bytes) failed rc=%d (buffer left pageable)\n", bytes, rc);
    return -1;
  }
  pin_ledger_add(p, bytes);
  return 0;
}
int nr_pdsch_gpu_host_pin_enabled(void)
{
  pthread_once(&g_pin_once, pin_init);
  return g_pin_fn != NULL;
}
void nr_pdsch_gpu_host_unpin(void *p)
{
  if (g_pin_fn && p) {
    g_unpin_fn(p);
    pin_ledger_del(p);
  }
}
