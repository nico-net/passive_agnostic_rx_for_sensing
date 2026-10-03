/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* nr_td_cb0_bench: throughput of nr_td_cb0_batch (items/s) for N hypotheses per grant at 106 / 273 PRB.
 * Stages: CPU dematch, GPU dematch, CPU layered LDPC (1 and T threads), and an estimate of the GPU LDPC stage from
 * the existing CUDA pool (./libldpc_cuda.so ldpc_pool_decode, BG1 Z=384 worst case, unmodified). Not a ctest.
 * Run under flock /tmp/td_measure.lock. Usage: nr_td_cb0_bench [threads=8] */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nr_td_cb0_fixture.h"

static double now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + 1e-9 * t.tv_nsec;
}

/* N hypotheses on one grant: the true one plus wrong MCS (TBS) / rv / Qm, cycling. Wrong ones run max_iter. */
static void make_items(const cb0_fx_t *fx, uint16_t nrb, int n, nr_td_cb0_item_t *it)
{
  static const uint16_t Rs[] = {1200, 1930, 3080, 4340, 5170, 5530, 6160, 6580, 7190, 7720, 8220, 8730, 9100, 9480};
  for (int i = 0; i < n; i++) {
    nr_td_cb0_item_t x = cb0_fx_item(fx);
    if (i > 0) {
      const uint8_t Qm = (uint8_t)(2 + 2 * ((i / 14) % 3)); /* 2, 4, 6 */
      x.Qm = Qm;
      x.G = fx->G / fx->Qm * Qm;
      x.R = Rs[i % 14];
      x.tbs = cb0_fx_tbs(Qm, x.R, nrb, 12, 6, fx->Nl);
      x.rv = (uint8_t)((i / 42) % 4);
    }
    it[i] = x;
  }
}

typedef void (*pool_decode_t)(uint32_t, uint32_t, uint32_t, int, const uint32_t *, const uint32_t *, const uint32_t *);

int main(int argc, char **argv)
{
  const int T = argc > 1 ? atoi(argv[1]) : 8;
  cb0_fx_init();
  /* GPU LDPC pool (existing libldpc_cuda.so, used read-only for the estimate) */
  void *h = dlopen("./libldpc_cuda.so", RTLD_LAZY | RTLD_GLOBAL);
  int (*pool_init)(uint32_t) = h ? dlsym(h, "ldpc_pool_init") : NULL;
  int8_t *(*pool_llr)(void) = h ? dlsym(h, "ldpc_pool_host_llr") : NULL;
  pool_decode_t pool_dec = h ? (pool_decode_t)dlsym(h, "ldpc_pool_decode") : NULL;
  const int have_pool = pool_init && pool_llr && pool_dec && pool_init(2048) == 0;
  if (!have_pool)
    printf("# GPU LDPC pool not available (%s)\n", h ? "symbols" : dlerror());
  const int gpu = nr_td_cb0_use_gpu(1);
  printf("# GPU dematch module: %s\n", gpu ? "loaded" : "NOT available");
  printf("prb,N,C_true,cpu_dematch_us_per_item,gpu_dematch_us_per_batch,gpu_dematch_us_per_item,"
         "ldpc1_us_per_item,batch_gpu+cpuLDPC_T%d_items_per_s,batch_cpu_T1_items_per_s,"
         "gpu_ldpc_est_us_per_cb_it8,gpu_ldpc_est_us_per_cb_it16,pass\n", T);
  const uint16_t prbs[2] = {106, 273};
  const int Ns[6] = {32, 64, 256, 600, 1024, 0};
  for (int p = 0; p < 2; p++) {
    cb0_fx_t fx = {.R = 5170, .Qm = 6, .Nl = 2, .rv = 0};
    fx.A = cb0_fx_tbs(6, fx.R, prbs[p], 12, 6, 2);
    fx.G = cb0_fx_G(prbs[p], 12, 6, 1, 6, 2);
    if (cb0_fx_encode(&fx, 11, 64) != 0)
      return 1;
    for (int k = 0; Ns[k]; k++) {
      const int n = Ns[k];
      nr_td_cb0_item_t *it = calloc(n, sizeof(*it));
      nr_td_cb0_result_t *out = calloc(n, sizeof(*out));
      int8_t *l = aligned_alloc(64, (size_t)n * NR_TD_CB0_L_STRIDE);
      int8_t *st = calloc(n, 1);
      make_items(&fx, prbs[p], n, it);
      double t0, dt;
      int reps;
      /* CPU dematch */
      for (reps = 0, t0 = now(); (dt = now() - t0) < 0.3 || reps < 2; reps++)
        nr_td_cb0_dematch(it, n, 0, l, NULL, st);
      const double cpu_dm = dt / reps / n * 1e6;
      /* GPU dematch (kernel + sync, LLRs read in place) */
      double gpu_dm = -1;
      if (gpu) {
        nr_td_cb0_dematch(it, n, 1, l, NULL, st); /* warm-up */
        for (reps = 0, t0 = now(); (dt = now() - t0) < 0.3 || reps < 3; reps++)
          nr_td_cb0_dematch(it, n, 1, l, NULL, st);
        gpu_dm = dt / reps * 1e6;
      }
      /* CPU LDPC alone, 1 thread, on dematched buffers */
      nr_td_cb0_meta_t *meta = calloc(n, sizeof(*meta));
      for (int i = 0; i < n; i++)
        nr_td_cb0_meta(&it[i], &meta[i]);
      nr_td_cb0_dematch(it, n, 0, l, NULL, st);
      nr_td_cb0_set_threads(1);
      for (reps = 0, t0 = now(); (dt = now() - t0) < 0.5 || reps < 1; reps++)
        cb0_ldpc_decode_batch(meta, l, n, out);
      const double ldpc1 = dt / reps / n * 1e6;
      /* full batch: GPU dematch + CPU LDPC on T threads */
      nr_td_cb0_set_threads(T);
      nr_td_cb0_use_gpu(gpu);
      for (reps = 0, t0 = now(); (dt = now() - t0) < 0.5 || reps < 1; reps++)
        nr_td_cb0_batch(it, n, out);
      const double ips_gpu = n * reps / dt;
      int pass = 0;
      for (int i = 0; i < n; i++)
        pass += out[i].pass == 1;
      /* full batch: all CPU, 1 thread */
      nr_td_cb0_use_gpu(0);
      nr_td_cb0_set_threads(1);
      for (reps = 0, t0 = now(); (dt = now() - t0) < 0.5 || reps < 1; reps++)
        nr_td_cb0_batch(it, n, out);
      const double ips_cpu = n * reps / dt;
      nr_td_cb0_use_gpu(gpu);
      /* GPU LDPC estimate: n BG1 Z=384 K=8448 blocks (the true item's l, repeated) through the existing pool */
      double est[2] = {-1, -1};
      if (have_pool) {
        int8_t *pl = pool_llr();
        for (int i = 0; i < n; i++)
          memcpy(pl + (size_t)i * 68 * 384, l, 68 * 384);
        const uint32_t iters[2] = {8, 16};
        for (int q = 0; q < 2; q++) {
          for (int warm = 0; warm < 2; warm++) {
            for (reps = 0, t0 = now(); (dt = now() - t0) < 0.3 || reps < 2; reps++)
              for (int off = 0; off < n; off += 512) {
                const uint32_t first = off, count = n - off < 512 ? n - off : 512, K = 8448;
                pool_dec(1, 384, iters[q], 1, &first, &count, &K);
              }
          }
          est[q] = dt / reps / n * 1e6;
        }
      }
      printf("%u,%d,%u,%.2f,%.1f,%.3f,%.1f,%.0f,%.0f,%.2f,%.2f,%d\n", prbs[p], n, fx.C, cpu_dm, gpu_dm,
             gpu_dm > 0 ? gpu_dm / n : -1, ldpc1, ips_gpu, ips_cpu, est[0], est[1], pass);
      fflush(stdout);
      free(meta);
      free(it);
      free(out);
      free(l);
      free(st);
    }
    cb0_fx_free(&fx);
  }
  return 0;
}
