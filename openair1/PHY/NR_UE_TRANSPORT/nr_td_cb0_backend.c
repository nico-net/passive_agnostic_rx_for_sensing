/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
/*! \file nr_td_cb0_backend.c
 * \brief CB0 batch backend selection (ISAC_TD_CB0_BACKEND), GPU registration hook and the GPU failure rule.
 * See the "Backend interface" block of nr_td_cb0_batch.h. Plain C, no CUDA dependency: the CPU backend is
 * nr_td_cb0_batch_cpu(); a GPU backend is whatever the GPU entry registers.
 */
#define _GNU_SOURCE
#include "nr_td_cb0_batch.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static pthread_mutex_t g_be_lock = PTHREAD_MUTEX_INITIALIZER;
static nr_td_cb0_backend_t g_gpu_be;
static bool g_gpu_be_set;
static int g_mode = -1;     /* NR_TD_CB0_BE_*; -1 = read the environment */
static int g_backoff;       /* GPU batches still to skip after a failure */
static int g_backoff_len = -1;
static bool g_warned_fallback;
static _Atomic uint64_t s_batches_cpu, s_batches_gpu, s_gpu_failed, s_gpu_skipped;

static uint64_t now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int mode_from_env(void)
{
  const char *e = getenv("ISAC_TD_CB0_BACKEND");
  if (e == NULL || *e == 0 || strcasecmp(e, "auto") == 0)
    return NR_TD_CB0_BE_AUTO;
  if (strcasecmp(e, "cpu") == 0)
    return NR_TD_CB0_BE_CPU;
  if (strcasecmp(e, "gpu") == 0)
    return NR_TD_CB0_BE_GPU;
  fprintf(stderr, "SENSING: ISAC_TD_CB0_BACKEND=%s unknown, using auto\n", e);
  return NR_TD_CB0_BE_AUTO;
}

int nr_td_cb0_backend_mode(void)
{
  pthread_mutex_lock(&g_be_lock);
  if (g_mode < 0)
    g_mode = mode_from_env();
  const int m = g_mode;
  pthread_mutex_unlock(&g_be_lock);
  return m;
}

void nr_td_cb0_backend_mode_set(int mode)
{
  pthread_mutex_lock(&g_be_lock);
  g_mode = mode < 0 ? mode_from_env() : mode;
  g_backoff = 0;
  g_warned_fallback = false;
  pthread_mutex_unlock(&g_be_lock);
}

void nr_td_cb0_register_gpu_backend(const nr_td_cb0_backend_t *be)
{
  pthread_mutex_lock(&g_be_lock);
  if (be != NULL && be->decode != NULL) {
    g_gpu_be = *be;
    g_gpu_be_set = true;
  } else {
    memset(&g_gpu_be, 0, sizeof(g_gpu_be));
    g_gpu_be_set = false;
  }
  g_backoff = 0;
  pthread_mutex_unlock(&g_be_lock);
}

void nr_td_cb0_backend_get_stats(nr_td_cb0_backend_stats_t *s)
{
  if (s == NULL)
    return;
  s->batches_cpu = atomic_load(&s_batches_cpu);
  s->batches_gpu = atomic_load(&s_batches_gpu);
  s->gpu_failed = atomic_load(&s_gpu_failed);
  s->gpu_skipped = atomic_load(&s_gpu_skipped);
}

/* Decide the backend for one batch (before any outcome). Returns true for the GPU with a copy of its descriptor. */
static bool pick_gpu(nr_td_cb0_backend_t *be)
{
  pthread_mutex_lock(&g_be_lock);
  if (g_mode < 0)
    g_mode = mode_from_env();
  if (g_backoff_len < 0) {
    const char *e = getenv("ISAC_TD_CB0_GPU_BACKOFF");
    g_backoff_len = e ? atoi(e) : 64;
    if (g_backoff_len < 1)
      g_backoff_len = 1;
  }
  bool gpu = false;
  if (g_mode != NR_TD_CB0_BE_CPU && g_gpu_be_set) {
    if (g_backoff > 0) {
      g_backoff--;
      atomic_fetch_add(&s_gpu_skipped, 1);
    } else {
      *be = g_gpu_be;
      gpu = true;
    }
  }
  const bool warn = g_mode == NR_TD_CB0_BE_GPU && !gpu && !g_warned_fallback;
  if (warn)
    g_warned_fallback = true;
  pthread_mutex_unlock(&g_be_lock);
  if (gpu && be->healthy != NULL && !be->healthy(be->ctx)) {
    atomic_fetch_add(&s_gpu_skipped, 1);
    gpu = false;
  }
  if (warn)
    fprintf(stderr, "SENSING: ISAC_TD_CB0_BACKEND=gpu but no usable GPU backend: CB0 batches run on the CPU backend\n");
  return gpu;
}

static void gpu_failed(void)
{
  pthread_mutex_lock(&g_be_lock);
  g_backoff = g_backoff_len > 0 ? g_backoff_len : 64;
  pthread_mutex_unlock(&g_be_lock);
  atomic_fetch_add(&s_gpu_failed, 1);
}

int nr_td_cb0_exec(const nr_td_cb0_item_t *items, int n, nr_td_cb0_result_t *out, nr_td_cb0_exec_t *info)
{
  nr_td_cb0_exec_t loc;
  if (info == NULL)
    info = &loc;
  memset(info, 0, sizeof(*info));
  if (n < 0 || (n > 0 && (items == NULL || out == NULL)))
    return -1;
  if (n == 0)
    return 0;
  nr_td_cb0_backend_t be;
  const bool gpu = pick_gpu(&be);
  const uint64_t t0 = now_ns();
  if (gpu) {
    for (int i = 0; i < n; i++)
      out[i] = (nr_td_cb0_result_t){0};
    const int rc = be.decode(be.ctx, items, n, out);
    info->backend = NR_TD_CB0_BE_GPU;
    atomic_fetch_add(&s_batches_gpu, 1);
    uint8_t dec = 0;
    bool bad = rc != 0;
    for (int i = 0; i < n && !bad; i++) {
      if (out[i].err == NR_TD_CB0_ERR_GPU)
        bad = true;
      else if (out[i].pass != -1) {
        if (out[i].decoder_used == 0 || (dec != 0 && out[i].decoder_used != dec))
          info->mixed = 1, bad = true;
        dec = out[i].decoder_used;
      }
    }
    if (bad) {
      for (int i = 0; i < n; i++) {
        out[i].pass = -1;
        out[i].err = NR_TD_CB0_ERR_GPU;
      }
      info->failed = 1;
      gpu_failed();
    }
  } else {
    nr_td_cb0_batch_cpu(items, n, out);
    info->backend = NR_TD_CB0_BE_CPU;
    info->threads = nr_td_cb0_get_threads();
    atomic_fetch_add(&s_batches_cpu, 1);
  }
  info->wall_ns = now_ns() - t0;
  uint8_t dec = 0;
  for (int i = 0; i < n; i++) {
    if (out[i].pass == -1)
      continue;
    info->decoded++;
    info->sum_iters += out[i].iters;
    if (dec == 0)
      dec = out[i].decoder_used;
    else if (out[i].decoder_used != dec)
      info->mixed = 1;
  }
  info->decoder = info->mixed ? 0 : dec;
  return info->decoded;
}
