/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* nr_td_cb0_bench: nr_td_cb0_batch throughput, one batch = one grant's CB0 decodes.
 * Per (PRB, N, item set, max_it): CUDA LDPC through G1's pool (libldpc_cuda.so public TB entry; GPU iterations =
 * 2 x max_it) and CPU layered LDPC on T threads (GPU dematch). Item sets: "mixed" (1 truth + wrong MCS/Qm/rv),
 * "wrong" (wrong only: they run every iteration), "truth" (the true hypothesis only: early stop). Every item reads
 * its own LLR pointer, so the exact dedup does not merge anything (raw decode cost).
 * GPU busy % = mean of nvidia-smi utilization.gpu sampled every 100 ms during the timed loop.
 * Run with flock -x /tmp/td_measure.lock, nothing else on the GPU. Usage: nr_td_cb0_bench [threads=8] [seconds=1] */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
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

/* ---- GPU utilisation sampler ---- */
static atomic_int g_window;
static atomic_long g_usum, g_ucnt;
static void *sampler(void *arg)
{
  (void)arg;
  FILE *p = popen("nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits -lms 100", "r");
  if (!p)
    return NULL;
  char line[64];
  while (fgets(line, sizeof(line), p))
    if (atomic_load(&g_window)) {
      atomic_fetch_add(&g_usum, atol(line));
      atomic_fetch_add(&g_ucnt, 1);
    }
  pclose(p);
  return NULL;
}

enum { SET_MIXED, SET_WRONG, SET_TRUTH };
static const char *set_name[] = {"mixed", "wrong", "truth"};

/* wrong items: llr = base + i (distinct pointers, wrong anyway); truth items: copy i (identical content) */
static void make_items(const cb0_fx_t *fx, uint16_t nrb, int n, int set, int max_it, int16_t *shifted_base,
                       int16_t **truth_copies, nr_td_cb0_item_t *it)
{
  static const uint16_t Rs[] = {1200, 1930, 3080, 4340, 5530, 6160, 6580, 7190, 7720, 8220, 8730, 9100, 9480, 2510};
  for (int i = 0; i < n; i++) {
    nr_td_cb0_item_t x = cb0_fx_item(fx);
    x.max_iter = (uint8_t)max_it;
    const int truth = set == SET_TRUTH || (set == SET_MIXED && i == 0);
    if (truth) {
      x.llr = truth_copies[i];
    } else {
      const uint8_t Qm = (uint8_t)(2 + 2 * ((i / 14) % 3)); /* 2, 4, 6 */
      x.Qm = Qm;
      x.G = fx->G / fx->Qm * Qm;
      x.R = Rs[i % 14];
      x.tbs = cb0_fx_tbs(Qm, x.R, nrb, 12, 6, fx->Nl);
      x.rv = (uint8_t)((i / 42) % 4);
      x.llr = shifted_base + i;
    }
    it[i] = x;
  }
}

int main(int argc, char **argv)
{
  const int T = argc > 1 ? atoi(argv[1]) : 8;
  const double secs = argc > 2 ? atof(argv[2]) : 1.0;
  setenv("LDPC_CUDA_WARMUP_ITERS", "4", 0); /* pre-capture the 2 x 4 = 8 iteration graphs too */
  cb0_fx_init();
  const int gpu = nr_td_cb0_use_gpu(1);
  const int cuda = nr_td_cb0_use_cuda_ldpc(1);
  nr_td_cb0_use_cuda_ldpc(0);
  printf("# GPU dematch %s, CUDA LDPC pool %s, CPU threads %d\n", gpu ? "on" : "OFF", cuda ? "on" : "OFF", T);
  pthread_t sp;
  pthread_create(&sp, NULL, sampler, NULL);
  printf("prb,C_true,N,set,max_it,gpu_iters,cuda_us_per_batch,cuda_us_per_cb0,cuda_grants_per_s,gpu_busy_pct,"
         "cuda_fallback_items,cuda_pass,cpu_us_per_batch,cpu_us_per_cb0,cpu_grants_per_s,cpu_pass,verdict_mismatch\n");
  const uint16_t prbs[2] = {106, 273};
  const int Ns[4] = {64, 128, 256, 600};
  for (int p = 0; p < 2; p++) {
    cb0_fx_t fx = {.R = 5170, .Qm = 6, .Nl = 2, .rv = 0};
    fx.A = cb0_fx_tbs(6, fx.R, prbs[p], 12, 6, 2);
    fx.G = cb0_fx_G(prbs[p], 12, 6, 1, 6, 2);
    if (cb0_fx_encode(&fx, 11, 64) != 0)
      return 1;
    int16_t *base = malloc(((size_t)fx.G + 1024) * sizeof(int16_t));
    memcpy(base, fx.llr, (size_t)fx.G * sizeof(int16_t));
    memset(base + fx.G, 0, 1024 * sizeof(int16_t));
    int16_t *copies[600];
    for (int i = 0; i < 600; i++) {
      copies[i] = malloc((size_t)fx.G * sizeof(int16_t));
      memcpy(copies[i], fx.llr, (size_t)fx.G * sizeof(int16_t));
    }
    for (int k = 0; k < 4; k++)
      for (int set = 0; set < 3; set++)
        for (int mi = 8; mi >= 4; mi -= 4) {
          const int n = Ns[k];
          nr_td_cb0_item_t *it = calloc(n, sizeof(*it));
          nr_td_cb0_result_t *oc = calloc(n, sizeof(*oc)), *og = calloc(n, sizeof(*og));
          make_items(&fx, prbs[p], n, set, mi, base, copies, it);
          nr_td_cb0_set_threads(T);
          double cuda_us = -1, busy = -1;
          int fb = 0, cpass = 0;
          if (cuda) {
            nr_td_cb0_use_cuda_ldpc(1);
            nr_td_cb0_batch(it, n, og); /* warm */
            nr_td_cb0_stats_t s0, s1;
            nr_td_cb0_get_stats(&s0);
            atomic_store(&g_usum, 0);
            atomic_store(&g_ucnt, 0);
            atomic_store(&g_window, 1);
            int reps;
            double t0 = now(), dt;
            for (reps = 0; (dt = now() - t0) < secs || reps < 3; reps++)
              nr_td_cb0_batch(it, n, og);
            atomic_store(&g_window, 0);
            nr_td_cb0_get_stats(&s1);
            cuda_us = dt / reps * 1e6;
            busy = atomic_load(&g_ucnt) ? (double)atomic_load(&g_usum) / atomic_load(&g_ucnt) : -1;
            fb = (int)(s1.cuda_fallback_items - s0.cuda_fallback_items + s1.cuda_errors - s0.cuda_errors);
            for (int i = 0; i < n; i++)
              cpass += og[i].pass == 1;
            nr_td_cb0_use_cuda_ldpc(0);
          }
          nr_td_cb0_batch(it, n, oc);
          int reps;
          double t0 = now(), dt;
          for (reps = 0; (dt = now() - t0) < secs || reps < 2; reps++)
            nr_td_cb0_batch(it, n, oc);
          const double cpu_us = dt / reps * 1e6;
          int pass = 0, mism = 0;
          for (int i = 0; i < n; i++) {
            pass += oc[i].pass == 1;
            mism += cuda && og[i].pass != oc[i].pass;
          }
          printf("%u,%u,%d,%s,%d,%d,%.0f,%.2f,%.1f,%.0f,%d,%d,%.0f,%.2f,%.1f,%d,%d\n", prbs[p], fx.C, n, set_name[set], mi,
                 2 * mi, cuda_us, cuda_us / n, 1e6 / cuda_us, busy, fb, cpass, cpu_us, cpu_us / n, 1e6 / cpu_us, pass,
                 mism);
          fflush(stdout);
          free(it);
          free(oc);
          free(og);
        }
    for (int i = 0; i < 600; i++)
      free(copies[i]);
    free(base);
    cb0_fx_free(&fx);
  }
  return 0;
}
