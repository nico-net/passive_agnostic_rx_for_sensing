/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
/*! \file nr_td_cb0_gpu_backend.c
 * \brief GPU LDPC backend of nr_td_cb0_batch: the dedicated CB0 entry of libldpc_cuda.so (nrLDPC_cb0_cuda.h), loaded
 * with dlopen so td_cb0_batch keeps no CUDA link dependency. See nr_td_cb0_batch.h (nr_td_cb0_backend_t).
 */
#define _GNU_SOURCE
#include "nr_td_cb0_batch.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PHY/CODING/nrLDPC_cuda/nrLDPC_cb0_cuda.h"

static int (*f_submit)(const ldpc_cb0_item_t *, int, const int8_t *, size_t, int, ldpc_cb0_ticket_t **);
static int (*f_collect)(ldpc_cb0_ticket_t *, ldpc_cb0_result_t *, uint8_t *, size_t);

uint8_t nr_td_cb0_gpu_iters(uint8_t max_iter)
{
  static int over = -1;
  if (over < 0) {
    const char *e = getenv("ISAC_TD_CB0_GPU_ITERS");
    over = (e && atoi(e) > 0) ? atoi(e) : 0;
    if (over > LDPC_CB0_MAX_ITERS)
      over = LDPC_CB0_MAX_ITERS;
    if (over)
      fprintf(stderr, "nr_td_cb0: ISAC_TD_CB0_GPU_ITERS=%d overrides 2 x max_iter (dominance proof required)\n", over);
  }
  if (over)
    return (uint8_t)over;
  const int it = 2 * max_iter;
  return (uint8_t)(it > LDPC_CB0_MAX_ITERS ? LDPC_CB0_MAX_ITERS : it);
}

typedef struct {
  ldpc_cb0_ticket_t *t;
  int n;
  int8_t *valid;
  ldpc_cb0_result_t *res;
} gpu_ticket_t;

static void fail_all(const nr_td_cb0_meta_t *meta, int n, nr_td_cb0_result_t *out)
{
  for (int i = 0; i < n; i++)
    if (meta ? meta[i].valid : 1) {
      out[i].pass = -1;
      out[i].err = NR_TD_CB0_ERR_GPU;
      out[i].iters = 0;
      out[i].decoder_used = NR_TD_CB0_DEC_CUDA_FLOODING;
    }
}

static int gpu_submit(const nr_td_cb0_meta_t *meta, const int8_t *l, int n, void **ticket)
{
  *ticket = NULL;
  if (n <= 0 || !meta || !l)
    return -1;
  ldpc_cb0_item_t *it = calloc(n, sizeof(*it));
  gpu_ticket_t *t = calloc(1, sizeof(*t));
  if (t) {
    t->valid = calloc(n, 1);
    t->res = calloc(n, sizeof(*t->res));
  }
  if (!it || !t || !t->valid || !t->res) {
    free(it);
    if (t) {
      free(t->valid);
      free(t->res);
    }
    free(t);
    return -1;
  }
  for (int i = 0; i < n; i++) {
    const nr_td_cb0_meta_t *m = &meta[i];
    t->valid[i] = m->valid;
    if (!m->valid)
      continue; /* iters = 0: the entry reports it inconclusive, we keep the caller's result */
    it[i].BG = m->BG;
    it[i].Z = (uint16_t)m->Z;
    it[i].K = (uint16_t)m->K;
    it[i].Kprime = (uint16_t)m->Kprime_crc;
    it[i].crc_type = m->crc_type;
    it[i].iters = nr_td_cb0_gpu_iters(m->max_iter);
    /* cb0_guard: C > 1 the probe's segment bytes, C == 1 the TB payload + the 2 bytes after it (all zero <=> the
     * payload is zero and out[sz], out[sz + 1] are zero) */
    it[i].guard_bytes = (uint16_t)(m->C > 1 ? (m->K >> 3) - (m->F >> 3) - 3 : m->A / 8 + 2);
  }
  const int rc = f_submit(it, n, l, NR_TD_CB0_L_STRIDE, 0, &t->t);
  free(it);
  if (rc != LDPC_CB0_OK) {
    free(t->valid);
    free(t->res);
    free(t);
    return rc;
  }
  t->n = n;
  *ticket = t;
  return 0;
}

static int gpu_collect(void *ticket, nr_td_cb0_result_t *out)
{
  gpu_ticket_t *t = ticket;
  if (!t)
    return -1;
  const int rc = f_collect(t->t, t->res, NULL, 0);
  for (int i = 0; i < t->n; i++) {
    if (!t->valid[i])
      continue;
    if (rc != LDPC_CB0_OK || t->res[i].crc_ok < 0) {
      out[i].pass = -1;
      out[i].err = NR_TD_CB0_ERR_GPU;
      out[i].iters = 0;
      out[i].decoder_used = NR_TD_CB0_DEC_CUDA_FLOODING;
      continue;
    }
    out[i].pass = (int8_t)(t->res[i].crc_ok == 1 && !t->res[i].zero);
    out[i].iters = t->res[i].iters;
    out[i].decoder_used = NR_TD_CB0_DEC_CUDA_FLOODING;
    out[i].err = NR_TD_CB0_OK;
  }
  /* a per-item -1 with a successful collect cannot happen for a valid meta item (same validity rules); if it did, the
   * batch is treated as failed so the caller never mixes verdicts of one grant */
  int bad = rc != LDPC_CB0_OK;
  for (int i = 0; !bad && i < t->n; i++)
    bad = t->valid[i] && t->res[i].crc_ok < 0;
  if (bad && rc == LDPC_CB0_OK)
    for (int i = 0; i < t->n; i++)
      if (t->valid[i]) {
        out[i].pass = -1;
        out[i].err = NR_TD_CB0_ERR_GPU;
      }
  free(t->valid);
  free(t->res);
  free(t);
  return bad ? (rc != LDPC_CB0_OK ? rc : -1) : 0;
}

static int gpu_decode(const nr_td_cb0_meta_t *meta, const int8_t *l, int n, nr_td_cb0_result_t *out)
{
  if (n <= 0)
    return 0;
  void *t = NULL;
  const int rc = gpu_submit(meta, l, n, &t);
  if (rc != 0) {
    fail_all(meta, n, out);
    return rc < 0 ? rc : -1;
  }
  return gpu_collect(t, out);
}

static const nr_td_cb0_backend_t g_backend = {"cuda-cb0", NR_TD_CB0_DEC_CUDA_FLOODING, gpu_decode, gpu_submit, gpu_collect};
static const nr_td_cb0_backend_t *g_ptr;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void load(void)
{
  void *h = dlopen("libldpc_cuda.so", RTLD_NOW | RTLD_NOLOAD); /* the receiver's TB plugin instance, if loaded */
  if (!h)
    h = dlopen("libldpc_cuda.so", RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
  if (!h)
    h = dlopen("./libldpc_cuda.so", RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
  if (!h)
    return;
  int (*init)(void) = (int (*)(void))dlsym(h, "ldpc_cb0_init");
  f_submit = (int (*)(const ldpc_cb0_item_t *, int, const int8_t *, size_t, int, ldpc_cb0_ticket_t **))dlsym(h, "ldpc_cb0_submit");
  f_collect = (int (*)(ldpc_cb0_ticket_t *, ldpc_cb0_result_t *, uint8_t *, size_t))dlsym(h, "ldpc_cb0_collect");
  if (!init || !f_submit || !f_collect || init() != 0)
    return;
  g_ptr = &g_backend;
}

const nr_td_cb0_backend_t *nr_td_cb0_gpu_backend(void)
{
  pthread_once(&g_once, load);
  return g_ptr;
}
