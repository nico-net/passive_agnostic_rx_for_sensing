/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* nr_td_cb0_gpu_bench: CB0 GPU entry (libldpc_cuda.so ldpc_cb0_*, via nr_td_cb0_gpu_backend) vs the CPU backend.
 * One batch = one grant's CB0 decodes. Per (PRB, N, item set):
 *   gpu_sync  = GPU dematch (libtd_cb0_gpu.so) + CB0 GPU entry, synchronous, per grant;
 *   gpu_dec   = the CB0 GPU entry alone (decode of already dematched inputs);
 *   gpu_pipe  = pipelined grants: dematch grant k+1 while grant k decodes (async submit/collect, 2 buffers);
 *   cpu8      = nr_td_cb0_batch with CPU LDPC on T threads (GPU dematch), the CPU backend.
 * Grant (operator 2026-10-04: 4 RX only): a 4-RX grant, true rank Nl = 4 by default (argv 5), 64QAM R 0.505.
 * Item sets: "wrong" (wrong MCS / Qm / rv / rank Nl 1..Nl_true: every one runs all iterations), "truth" (the true
 * hypothesis on N private copies: early stop), "mixed" (1 truth + wrong). CB0 E and the LLR count follow from Nl and
 * Qm (G = RE x Qm x Nl), not from the RX count. GPU busy % = mean nvidia-smi utilization.gpu
 * sampled every 100 ms over the gpu_pipe loop. Memory strategy: the CB0 entry's (LDPC_CB0_MEM=explicit forces the copy
 * path); printed per row.
 * Run with flock -x /tmp/td_measure.lock, nothing else on the GPU.
 * Usage: nr_td_cb0_gpu_bench [threads=8] [seconds=1] [gpu_iters=0 (= 2 x max_it)] [cpu=1] [rank=4] */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "nr_td_cb0_fixture.h"

static double now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + 1e-9 * t.tv_nsec;
}

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
      const uint8_t Qm = (uint8_t)(2 + 2 * ((i / 14) % 3));
      const uint8_t Nl = (uint8_t)(fx->Nl - (i / 168) % fx->Nl); /* rank hypotheses Nl_true .. 1 */
      x.Qm = Qm;
      x.Nl = Nl;
      x.G = fx->G / (fx->Qm * fx->Nl) * Qm * Nl;
      x.R = Rs[i % 14];
      x.tbs = cb0_fx_tbs(Qm, x.R, nrb, 12, 6, Nl);
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
  const int gpu_iters = argc > 3 ? atoi(argv[3]) : 0;
  const int do_cpu = argc > 4 ? atoi(argv[4]) : 1;
  const int rank = argc > 5 ? atoi(argv[5]) : 4;
  if (gpu_iters > 0) {
    char b[16];
    snprintf(b, sizeof(b), "%d", gpu_iters);
    setenv("ISAC_TD_CB0_GPU_ITERS", b, 1);
  }
  cb0_fx_init();
  const int dm = nr_td_cb0_use_gpu(1);
  const nr_td_cb0_backend_t *be = nr_td_cb0_gpu_backend();
  if (!be) {
    fprintf(stderr, "no CUDA CB0 backend\n");
    return 1;
  }
  int (*mode)(void) = NULL;
  void *h = dlopen("libldpc_cuda.so", RTLD_NOW | RTLD_NOLOAD);
  if (!h)
    h = dlopen("./libldpc_cuda.so", RTLD_NOW | RTLD_NOLOAD);
  if (h)
    mode = (int (*)(void))dlsym(h, "ldpc_cb0_mem_mode");
  const int mm = mode ? mode() : 0;
  printf("# GPU dematch %s, CB0 entry memory %s, CPU threads %d, gpu iters %s\n", dm ? "on" : "OFF",
         mm == 1 ? "unified" : mm == 2 ? "explicit" : "?", T, gpu_iters > 0 ? argv[3] : "2 x max_it");
  pthread_t sp;
  pthread_create(&sp, NULL, sampler, NULL);
  printf("# 4-RX grant, true rank %d\n", rank);
  printf("mem,prb,C_true,N,set,max_it,gpu_it,dematch_us,gpu_dec_us,gpu_dec_us_per_cb0,gpu_sync_us,gpu_sync_us_per_cb0,"
         "gpu_pipe_us,gpu_pipe_us_per_cb0,gpu_pipe_grants_s,gpu_busy_pct,gpu_pass,gpu_fail_batches,cpu_us,cpu_us_per_cb0,"
         "cpu_grants_s,cpu_pass,verdict_mismatch,speedup_pipe_vs_cpu\n");
  const uint16_t prbs[2] = {106, 273};
  const int Ns[3] = {64, 256, 600};
  for (int p = 0; p < 2; p++) {
    cb0_fx_t fx = {.R = 5170, .Qm = 6, .Nl = (uint8_t)rank, .rv = 0};
    fx.A = cb0_fx_tbs(6, fx.R, prbs[p], 12, 6, rank);
    fx.G = cb0_fx_G(prbs[p], 12, 6, 1, 6, rank);
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
    for (int k = 0; k < 3; k++)
      for (int set = 0; set < 3; set++) {
        const int n = Ns[k], mi = 8;
        /* row filters (profiling): CB0_BENCH_PRB / CB0_BENCH_N / CB0_BENCH_SET */
        const char *fp = getenv("CB0_BENCH_PRB"), *fn = getenv("CB0_BENCH_N"), *fs = getenv("CB0_BENCH_SET");
        if ((fp && atoi(fp) != prbs[p]) || (fn && atoi(fn) != n) || (fs && strcmp(fs, set_name[set])))
          continue;
        nr_td_cb0_item_t *it = calloc(n, sizeof(*it));
        nr_td_cb0_meta_t *meta = calloc(n, sizeof(*meta));
        nr_td_cb0_result_t *og = calloc(n, sizeof(*og)), *oc = calloc(n, sizeof(*oc));
        int8_t *l[2], *st = calloc(n, 1);
        for (int b = 0; b < 2; b++)
          l[b] = aligned_alloc(64, (size_t)n * NR_TD_CB0_L_STRIDE);
        make_items(&fx, prbs[p], n, set, mi, base, copies, it);
        for (int i = 0; i < n; i++)
          nr_td_cb0_meta(&it[i], &meta[i]);
        /* warm (graph captures, first touches) */
        nr_td_cb0_dematch(it, n, 1, l[0], NULL, st);
        be->decode(meta, l[0], n, og);
        /* dematch alone */
        int reps;
        double t0 = now(), dt;
        for (reps = 0; (dt = now() - t0) < secs / 4 || reps < 3; reps++)
          nr_td_cb0_dematch(it, n, 1, l[0], NULL, st);
        const double dem_us = dt / reps * 1e6;
        /* decode alone */
        int failb = 0;
        t0 = now();
        for (reps = 0; (dt = now() - t0) < secs || reps < 3; reps++)
          failb += be->decode(meta, l[0], n, og) != 0;
        const double dec_us = dt / reps * 1e6;
        /* synchronous grant: dematch + decode */
        t0 = now();
        for (reps = 0; (dt = now() - t0) < secs || reps < 3; reps++) {
          nr_td_cb0_dematch(it, n, 1, l[0], NULL, st);
          failb += be->decode(meta, l[0], n, og) != 0;
        }
        const double sync_us = dt / reps * 1e6;
        /* pipelined grants: dematch k+1 (buffer (k+1)&1) while k decodes */
        atomic_store(&g_usum, 0);
        atomic_store(&g_ucnt, 0);
        atomic_store(&g_window, 1);
        void *tk = NULL;
        nr_td_cb0_dematch(it, n, 1, l[0], NULL, st);
        if (be->submit(meta, l[0], n, &tk) != 0)
          failb++;
        t0 = now();
        for (reps = 0; (dt = now() - t0) < secs || reps < 3; reps++) {
          nr_td_cb0_dematch(it, n, 1, l[(reps + 1) & 1], NULL, st);
          if (tk && be->collect(tk, og) != 0)
            failb++;
          tk = NULL;
          if (be->submit(meta, l[(reps + 1) & 1], n, &tk) != 0)
            failb++;
        }
        if (tk && be->collect(tk, og) != 0)
          failb++;
        dt = now() - t0;
        atomic_store(&g_window, 0);
        const double pipe_us = dt / reps * 1e6;
        const double busy = atomic_load(&g_ucnt) ? (double)atomic_load(&g_usum) / atomic_load(&g_ucnt) : -1;
        int gpass = 0;
        for (int i = 0; i < n; i++)
          gpass += og[i].pass == 1;
        /* CPU backend: nr_td_cb0_batch, CPU LDPC on T threads, GPU dematch */
        double cpu_us = -1;
        int cpass = -1, mism = -1;
        if (do_cpu) {
          nr_td_cb0_set_threads(T);
          nr_td_cb0_batch(it, n, oc);
          t0 = now();
          for (reps = 0; (dt = now() - t0) < secs || reps < 2; reps++)
            nr_td_cb0_batch(it, n, oc);
          cpu_us = dt / reps * 1e6;
          cpass = mism = 0;
          for (int i = 0; i < n; i++) {
            cpass += oc[i].pass == 1;
            mism += og[i].pass != oc[i].pass;
          }
        }
        printf("%s,%u,%u,%d,%s,%d,%d,%.0f,%.0f,%.2f,%.0f,%.2f,%.0f,%.2f,%.1f,%.0f,%d,%d,%.0f,%.2f,%.1f,%d,%d,%.2f\n",
               mm == 1 ? "unified" : "explicit", prbs[p], fx.C, n, set_name[set], mi, nr_td_cb0_gpu_iters(mi), dem_us,
               dec_us, dec_us / n, sync_us, sync_us / n, pipe_us, pipe_us / n, 1e6 / pipe_us, busy, gpass, failb,
               cpu_us, cpu_us / n, cpu_us > 0 ? 1e6 / cpu_us : -1, cpass, mism, cpu_us > 0 ? cpu_us / pipe_us : -1);
        fflush(stdout);
        free(it);
        free(meta);
        free(og);
        free(oc);
        free(st);
        free(l[0]);
        free(l[1]);
      }
    for (int i = 0; i < 600; i++)
      free(copies[i]);
    free(base);
    cb0_fx_free(&fx);
  }
  /* _exit: the nvidia-smi sampler (popen) and the CUDA / G1 worker teardown can hang a normal exit for minutes, which
   * would keep the measurement lock held after the data is out */
  fflush(stdout);
  _exit(0);
}
