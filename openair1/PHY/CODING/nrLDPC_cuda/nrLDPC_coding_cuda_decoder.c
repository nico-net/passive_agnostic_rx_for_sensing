/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/* Transport-block LDPC decoding with the code blocks decoded as ONE GPU batch (libldpc_cuda.so).
 *
 * Same interface and the same CPU-side math as nrLDPC_coding_segment_decoder.c, which this is derived
 * from: de-interleaving, rate de-matching (HARQ soft combining into d), filler bits and int8 packing
 * still run per segment in the thread pool. What changes is the decode itself: instead of one
 * LDPCdecoder() call per segment (per-segment GPU launches are latency-bound: 88 us/segment measured
 * against 19 us on the CPU), the packed segments land in a pinned batch buffer and every segment of a
 * TB is decoded by one ldpc_batch_decode(). The CRC of each segment is then checked on the CPU, exactly
 * as the CPU decoder's early termination would have.
 *
 * Iterations: 2 x max_ldpc_iterations -- the GPU runs flooding min-sum, which needs about twice the
 * CPU decoder's iterations for the same BLER (see nr_ldpc_cuda.c). */

#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_segment/nr_rate_matching.h"
#include "PHY/CODING/coding_extern.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/CODING/nrLDPC_extern.h"
#include "defs.h"
#include "common/utils/LOG/log.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
/* LDPC_BENCH=1: per-TB decode wall time and whole-process CPU time (getrusage, so the thread pool's
 * segment tasks are included), averaged and printed every 200 calls. For comparing libldpc (CPU) and
 * libldpc_cuda (GPU) on the same workload; off by default. */
#include <sys/resource.h>
static int ldpc_bench_on(void)
{
  static int on = -1;
  if (on < 0) {
    const char *e = getenv("LDPC_BENCH");
    on = e && atoi(e);
  }
  return on;
}
static double ldpc_bench_cpu_s(void)
{
  struct rusage u;
  getrusage(RUSAGE_SELF, &u);
  return u.ru_utime.tv_sec + u.ru_stime.tv_sec + 1e-6 * (u.ru_utime.tv_usec + u.ru_stime.tv_usec);
}
static double ldpc_bench_wall_s(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + 1e-9 * t.tv_nsec;
}
static void ldpc_bench_add(const char *who, double wall, double cpu, int segs)
{
  static __thread double sw, sc;
  static __thread long n, ns;
  sw += wall; sc += cpu; ns += segs;
  if (++n % 200 == 0)
    printf("LDPC_BENCH %s: %ld TBs, %.1f segs/TB, wall %.1f us/TB, cpu %.1f us/TB\n", who, n, (double)ns / n,
           1e6 * sw / n, 1e6 * sc / n);
}

#include <string.h>

struct ThreadContext;
struct ThreadContext *ldpc_decoder_init(int make_stream);
uint32_t ldpc_batch_llr_stride(void);
uint32_t ldpc_batch_bits_stride(void);
int ldpc_pool_init(uint32_t cap);
int8_t *ldpc_pool_host_llr(void);
uint8_t *ldpc_pool_host_bits(void);
uint32_t ldpc_pool_max_launch(void);
void ldpc_pool_decode(uint32_t BG, uint32_t Z, uint32_t num_iter, int n_req, const uint32_t *first, const uint32_t *count,
                      const uint32_t *K);

#include <pthread.h>
#include <stdatomic.h>

typedef struct {
  uint32_t BG, Z, Kc, K, F, A, C, E, Qm, rv_index, tbslbrm;
  short *llr;
  int16_t *d;
  bool d_to_be_cleared;
  int8_t *batch_llr; /* this segment's slot in the pinned pool */
  bool prep_ok;
  task_ans_t *ans;
} nrLDPC_cuda_seg_t;

/* De-interleave, rate de-match, fill and pack one segment into its pool slot. */
static void cuda_prepare_segment(void *arg)
{
  nrLDPC_cuda_seg_t *s = (nrLDPC_cuda_seg_t *)arg;
  const int K = s->K, Kprime = K - s->F, E = s->E, Z = s->Z;
  int16_t harq_e[E];
  nr_deinterleaving_ldpc(E, s->Qm, harq_e, s->llr);
  s->prep_ok = nr_rate_matching_ldpc_rx(s->tbslbrm, s->BG, Z, s->d, harq_e, s->C, s->rv_index, s->d_to_be_cleared,
                                        E, s->F, K - s->F - 2 * Z) != -1;
  if (s->prep_ok) {
    int16_t z[68 * 384 + 16] __attribute__((aligned(16)));
    memset(z, 0, 2 * Z * sizeof(*z));                                /* first 2*Z punctured bits */
    memset(z + Kprime, 127, s->F * sizeof(*z));                      /* filler bits */
    memcpy(z + 2 * Z, s->d, (Kprime - 2 * Z) * sizeof(*z));          /* coded bits before the filler */
    memcpy(z + K, s->d + (K - 2 * Z), (s->Kc * Z - K) * sizeof(*z)); /* skip the filler */
    /* saturate to int8 with the segment decoder's SIMD pack (a scalar loop here cost ~0.5 ms per
     * 24-segment TB -- more than OAI's whole CPU decode, measured with LDPC_BENCH) */
    simde__m128i *pv = (simde__m128i *)z, *pl = (simde__m128i *)s->batch_llr;
    for (int i = 0, j = 0; j < (int)((s->Kc * Z) >> 4); i += 2, j++)
      pl[j] = simde_mm_packs_epi16(pv[i], pv[i + 1]);
  }
  completed_task_ans(s->ans);
}

/* ---- Dynamic batching (2026-09-15). One worker thread owns the GPU; every consumer thread's TBs are
 * queued as requests and decoded together, so the ~40 launches of a decode are shared by every TB in
 * flight instead of paid per TB. Submitters sleep on a condition variable while the GPU works. ---- */
#define POOL_CAP 2048 /* slots (code blocks) in the pinned pool: ~53 MB LLR + 7 MB bits */
typedef struct {
  uint32_t BG, Z, iters, first, count, K;
  volatile bool done;
} gpu_req_t;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv_work = PTHREAD_COND_INITIALIZER, g_cv_done = PTHREAD_COND_INITIALIZER;
static gpu_req_t *g_queue[256];
static int g_qn;
static uint8_t g_slot_used[POOL_CAP];
static _Atomic uint64_t g_batches, g_batched_reqs;

/* first-fit contiguous run of n free slots; caller holds g_mu */
static int slots_reserve(uint32_t n)
{
  for (uint32_t i = 0, run = 0; i < POOL_CAP; i++) {
    run = g_slot_used[i] ? 0 : run + 1;
    if (run == n) {
      memset(&g_slot_used[i + 1 - n], 1, n);
      return (int)(i + 1 - n);
    }
  }
  return -1;
}

static void *gpu_worker(void *arg)
{
  (void)arg;
  const uint32_t max_launch = ldpc_pool_max_launch();
  for (;;) {
    gpu_req_t *take[256];
    pthread_mutex_lock(&g_mu);
    while (g_qn == 0)
      pthread_cond_wait(&g_cv_work, &g_mu);
    const int nt = g_qn;
    memcpy(take, g_queue, nt * sizeof(*take));
    g_qn = 0;
    pthread_mutex_unlock(&g_mu);
    /* group by (BG, Z, iterations), split at the per-launch cap */
    bool used[256] = {false};
    for (int a = 0; a < nt; a++) {
      if (used[a])
        continue;
      uint32_t first[256], count[256], K[256], n = 0;
      int nr = 0;
      for (int b = a; b < nt; b++) {
        if (used[b] || take[b]->BG != take[a]->BG || take[b]->Z != take[a]->Z || take[b]->iters != take[a]->iters
            || n + take[b]->count > max_launch)
          continue;
        used[b] = true;
        first[nr] = take[b]->first;
        count[nr] = take[b]->count;
        K[nr] = take[b]->K;
        n += take[b]->count;
        nr++;
      }
      ldpc_pool_decode(take[a]->BG, take[a]->Z, take[a]->iters, nr, first, count, K);
      atomic_fetch_add(&g_batches, 1);
      atomic_fetch_add(&g_batched_reqs, nr);
    }
    pthread_mutex_lock(&g_mu);
    for (int a = 0; a < nt; a++)
      take[a]->done = true;
    pthread_cond_broadcast(&g_cv_done);
    pthread_mutex_unlock(&g_mu);
  }
  return NULL;
}

int32_t nrLDPC_coding_init(void)
{
  if (ldpc_pool_init(POOL_CAP) != 0)
    return -1;
  pthread_t th;
  if (pthread_create(&th, NULL, gpu_worker, NULL) != 0)
    return -1;
  pthread_detach(th);
  return 0;
}

int32_t nrLDPC_coding_shutdown(void)
{
  return 0;
}

static __thread double t_prep_s, t_gpu_s; /* LDPC_BENCH phase split */
int32_t nrLDPC_coding_decoder_impl(nrLDPC_slot_decoding_parameters_t *slot)
{
  const double t_start = ldpc_bench_on() ? ldpc_bench_wall_s() : 0;
  const int nt = slot->nb_TBs;
  if (nt == 0 || nt > 64)
    return nt ? -1 : 0;
  int nb = 0;
  for (int t = 0; t < nt; t++)
    nb += slot->TBs[t].C;
  if (nb == 0)
    return 0;
  const uint32_t in_stride = ldpc_batch_llr_stride(), bits_stride = ldpc_batch_bits_stride();
  int8_t *pool_llr = ldpc_pool_host_llr();
  const uint8_t *pool_bits = ldpc_pool_host_bits();

  /* reserve each TB's slots (wait for the worker to free some if the pool is full) */
  int firsts[64];
  pthread_mutex_lock(&g_mu);
  for (int t = 0; t < nt; t++) {
    while ((firsts[t] = slots_reserve(slot->TBs[t].C)) < 0)
      pthread_cond_wait(&g_cv_done, &g_mu);
  }
  pthread_mutex_unlock(&g_mu);

  /* phase A: CPU prep of every segment, in the thread pool, straight into the pinned pool */
  nrLDPC_cuda_seg_t seg[nb];
  task_ans_t ans;
  init_task_ans(&ans, nb);
  int k = 0;
  for (int t = 0; t < nt; t++) {
    nrLDPC_TB_decoding_parameters_t *tb = &slot->TBs[t];
    *tb->processedSegments = 0;
    for (int r = 0; r < (int)tb->C; r++, k++) {
      nrLDPC_cuda_seg_t *s = &seg[k];
      const bool second = r >= (int)tb->first_rE2;
      s->BG = tb->BG;
      s->Z = tb->Z;
      s->Kc = tb->BG == 2 ? 52 : 68;
      s->K = tb->K;
      s->F = tb->F;
      s->A = tb->A;
      s->C = tb->C;
      s->Qm = tb->Qm;
      s->rv_index = tb->rv_index;
      s->tbslbrm = tb->tbslbrm;
      s->E = second ? tb->E2 : tb->E;
      s->llr = tb->llr + (second ? tb->first_rE2 * tb->E + (r - tb->first_rE2) * tb->E2 : r * tb->E);
      s->d = tb->d + r * s->Kc * s->Z;
      s->d_to_be_cleared = tb->d_to_be_cleared;
      s->batch_llr = pool_llr + (size_t)(firsts[t] + r) * in_stride;
      s->prep_ok = false;
      s->ans = &ans;
      task_t task = {.func = &cuda_prepare_segment, .args = s};
      pushTpool(slot->threadPool, task);
    }
  }
  join_task_ans(&ans);
  const double t_a = ldpc_bench_on() ? ldpc_bench_wall_s() : 0;

  /* phase B: queue one request per TB and sleep until the worker has decoded them */
  gpu_req_t req[nt];
  pthread_mutex_lock(&g_mu);
  for (int t = 0; t < nt; t++) {
    nrLDPC_TB_decoding_parameters_t *tb = &slot->TBs[t];
    req[t] = (gpu_req_t){.BG = tb->BG, .Z = tb->Z, .iters = 2 * tb->max_ldpc_iterations, .first = firsts[t],
                         .count = tb->C, .K = tb->K, .done = false};
    g_queue[g_qn++] = &req[t];
  }
  pthread_cond_signal(&g_cv_work);
  for (int t = 0; t < nt; t++)
    while (!req[t].done)
      pthread_cond_wait(&g_cv_done, &g_mu);
  pthread_mutex_unlock(&g_mu);
  if (ldpc_bench_on())
    t_gpu_s += ldpc_bench_wall_s() - t_a;

  /* phase C: the CRC of every segment, then release the slots */
  k = 0;
  for (int t = 0; t < nt; t++) {
    nrLDPC_TB_decoding_parameters_t *tb = &slot->TBs[t];
    const int C = tb->C;
    const uint32_t Kprime = lenWithCrc(C, tb->A);
    const uint8_t crc_type = crcType(C, tb->A);
    for (int r = 0; r < C; r++, k++) {
      const uint8_t *b = pool_bits + (size_t)(firsts[t] + r) * bits_stride;
      uint8_t *c = tb->c + r * (tb->K >> 3);
      const bool ok = seg[k].prep_ok && check_crc((uint8_t *)b, Kprime, crc_type);
      if (ok)
        memcpy(c, b, tb->K >> 3);
      else
        memset(c, 0, tb->K >> 3);
      tb->decodeSuccess[r] = ok;
      *tb->processedSegments += ok;
    }
  }
  pthread_mutex_lock(&g_mu);
  for (int t = 0; t < nt; t++)
    memset(&g_slot_used[firsts[t]], 0, slot->TBs[t].C);
  pthread_cond_broadcast(&g_cv_done);
  pthread_mutex_unlock(&g_mu);
  if (ldpc_bench_on())
    t_prep_s += t_a - t_start;
  return 0;
}

int32_t nrLDPC_coding_decoder(nrLDPC_slot_decoding_parameters_t *p)
{
  if (!ldpc_bench_on())
    return nrLDPC_coding_decoder_impl(p);
  int segs = 0;
  for (int t = 0; t < p->nb_TBs; t++)
    segs += p->TBs[t].C;
  const double w0 = ldpc_bench_wall_s(), c0 = ldpc_bench_cpu_s();
  const int32_t rc = nrLDPC_coding_decoder_impl(p);
  ldpc_bench_add("gpu", ldpc_bench_wall_s() - w0, ldpc_bench_cpu_s() - c0, segs);
  static __thread long nb;
  if (++nb % 200 == 0)
    printf("LDPC_BENCH gpu phases: prep %.1f us/TB, gpu wait %.1f us/TB, %.2f TBs per GPU batch\n", 1e6 * t_prep_s / nb,
           1e6 * t_gpu_s / nb, (double)atomic_load(&g_batched_reqs) / (double)(atomic_load(&g_batches) ? atomic_load(&g_batches) : 1));
  return rc;
}
