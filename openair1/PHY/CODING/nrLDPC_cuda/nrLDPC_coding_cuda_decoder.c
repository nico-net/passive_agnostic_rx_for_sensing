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
 * Iterations: 2 x max_ldpc_iterations -- the GPU runs normalised (x3/4) flooding min-sum, 2x iterations, which
 * is about 1 dB MORE sensitive than OAI's CPU plain min-sum (K36, measured), so results are not exchangeable. The
 * old note said: needs about twice the
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

#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <time.h>

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
    /* Pack ceil(Kc*Z/16) vectors and zero the tail of the last one, so no uninitialised stack is read and
     * nothing is written past the Kc*Z bytes of this slot (Kc*Z <= slot stride, and a multiple of 16 for the
     * largest case BG1 Z=384, so a full slot is never overrun). */
    const int ncp = s->Kc * Z;
    memset(z + ncp, 0, 16 * sizeof(*z));
    /* saturate to int8 with the segment decoder's SIMD pack (a scalar loop here cost ~0.5 ms per
     * 24-segment TB -- more than OAI's whole CPU decode, measured with LDPC_BENCH) */
    simde__m128i *pv = (simde__m128i *)z, *pl = (simde__m128i *)s->batch_llr;
    for (int i = 0, j = 0; j < (ncp + 15) >> 4; i += 2, j++)
      pl[j] = simde_mm_packs_epi16(pv[i], pv[i + 1]);
  }
  completed_task_ans(s->ans);
}

/* ---- Dynamic batching (2026-09-15). One worker thread owns the GPU; every consumer thread's TBs are
 * queued as requests and decoded together, so the ~40 launches of a decode are shared by every TB in
 * flight instead of paid per TB. Submitters sleep on a condition variable while the GPU works.
 *
 * K34 safety (2026-10-03): every wait is bounded (LDPC_CUDA_TIMEOUT_MS, default 50), the queue is
 * bounded, slot reservation is all-or-nothing, and any GPU error / timeout / rejection sends the TB's
 * code blocks to the CPU decoder (same LLR input). A GPU result is accepted only if its
 * request status is OK; a poisoned slot can never be turned into a pass. ---- */
#define POOL_CAP 2048 /* slots (code blocks) in the pinned pool: ~53 MB LLR + 7 MB bits */
#define QUEUE_CAP 256
enum { REQ_QUEUED, REQ_INFLIGHT, REQ_DONE, REQ_ABANDONED };
typedef struct {
  uint32_t BG, Z, iters, first, count, K;
  int state; /* guarded by g_mu */
  int rc;    /* 0 = GPU decoded every block of this request */
} gpu_req_t;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv_work, g_cv_done; /* CLOCK_MONOTONIC conds, set up by cond_setup() */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void cond_setup(void)
{
  pthread_condattr_t at;
  pthread_condattr_init(&at);
  pthread_condattr_setclock(&at, CLOCK_MONOTONIC);
  pthread_cond_init(&g_cv_work, &at);
  pthread_cond_init(&g_cv_done, &at);
  pthread_condattr_destroy(&at);
}
static gpu_req_t *g_queue[QUEUE_CAP];
static int g_qn;
static uint8_t g_slot_used[POOL_CAP];
static _Atomic uint64_t g_batches, g_batched_reqs;
static _Atomic uint64_t g_fallbacks; /* TBs decoded on the CPU instead of the GPU (error, timeout, queue full, oversize, breaker) */

int ldpc_pool_decode(uint32_t BG, uint32_t Z, uint32_t num_iter, int n_req, const uint32_t *first, const uint32_t *count,
                     const uint32_t *K, int *req_rc);
void ldpc_pool_counters(uint64_t *errors, uint64_t *poisoned);
void ldpc_pool_test_hooks(int skip, int stall_ms, int inject);
int ldpc_pool_sticky(void);
int ldpc_pool_capturing(void);
uint32_t ldpc_pool_capture_epoch(void);

/* ---- tunables / test hooks: environment read ONCE, tests override through ldpc_cuda_test_hooks() ---- */
static volatile int g_timeout_ms = 50, g_queue_cap = QUEUE_CAP, g_breaker_n = 4, g_breaker_ms = 5000;
static void tunables_init(void)
{
  const char *e;
  if ((e = getenv("LDPC_CUDA_TIMEOUT_MS")) && atoi(e) > 0) g_timeout_ms = atoi(e);
  if ((e = getenv("LDPC_CUDA_TEST_QUEUE_CAP")) && atoi(e) >= 0 && atoi(e) < QUEUE_CAP) g_queue_cap = atoi(e);
  if ((e = getenv("LDPC_CUDA_BREAKER_N")) && atoi(e) > 0) g_breaker_n = atoi(e);
  if ((e = getenv("LDPC_CUDA_BREAKER_S")) && atoi(e) > 0) g_breaker_ms = atoi(e) * 1000;
}
static void tunables_env_once(void)
{
  static pthread_once_t o = PTHREAD_ONCE_INIT;
  pthread_once(&o, tunables_init);
}

/* ---- circuit breaker (K34): N consecutive GPU errors or any wait timeout -> bypass the GPU for g_breaker_ms;
 * a sticky CUDA error -> bypass for good. State 0 closed, 1 bypassed, 2 permanently off (ldpc_cuda_disabled). ---- */
static _Atomic int g_consec_err, g_perm_off;
static _Atomic long long g_bypass_until_ms;
static long long mono_ms(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int breaker_state(void)
{
  if (atomic_load(&g_perm_off))
    return 2;
  return mono_ms() < atomic_load(&g_bypass_until_ms) ? 1 : 0;
}
enum { W_FULL, W_QUEUE, W_TIMEOUT, W_ERRORS, W_STICKY, W_WARMUP, W_CAPTURE, W_NREASON };
static void warn_once(int reason, const char *what) /* one-shot per reason */
{
  static atomic_int done[W_NREASON];
  if (!atomic_exchange(&done[reason], 1))
    LOG_W(PHY, "CUDA LDPC: %s -> CPU fallback (one-shot warning per reason; see ldpc_cuda_fallbacks/disabled)\n", what);
}
static void breaker_trip(int reason, const char *what, bool permanent)
{
  if (permanent)
    atomic_store(&g_perm_off, 1);
  else
    atomic_store(&g_bypass_until_ms, mono_ms() + g_breaker_ms);
  atomic_store(&g_consec_err, 0);
  warn_once(reason, what);
}
static void breaker_note_launch(bool err)
{
  if (!err) {
    atomic_store(&g_consec_err, 0);
    return;
  }
  if (ldpc_pool_sticky())
    breaker_trip(W_STICKY, "sticky CUDA error, GPU decoder disabled permanently", true);
  else if (atomic_fetch_add(&g_consec_err, 1) + 1 >= g_breaker_n)
    breaker_trip(W_ERRORS, "consecutive GPU decode errors, GPU bypassed for a few seconds", false);
}

/* ISAC_METRICS export (dlsym'd by nr_passive_metrics.c): errors = launch/CUDA errors, fallbacks = TBs sent
 * to the CPU decoder, poisoned = slots filled with the poison pattern, disabled = breaker state. */
void ldpc_cuda_get_counters4(uint64_t *errors, uint64_t *fallbacks, uint64_t *poisoned, uint64_t *disabled)
{
  ldpc_pool_counters(errors, poisoned);
  *fallbacks = atomic_load(&g_fallbacks);
  *disabled = (uint64_t)breaker_state();
}
void ldpc_cuda_get_counters(uint64_t *errors, uint64_t *fallbacks, uint64_t *poisoned)
{
  uint64_t d;
  ldpc_cuda_get_counters4(errors, fallbacks, poisoned, &d);
}

/* Test API (K34 tests only). queue_cap < 0, timeout/breaker <= 0 leave a value. */
void ldpc_cuda_test_hooks(int skip, int stall_ms, int inject, int queue_cap, int timeout_ms, int breaker_n, int breaker_ms)
{
  tunables_env_once();
  ldpc_pool_test_hooks(skip, stall_ms, inject);
  if (queue_cap >= 0) g_queue_cap = queue_cap > QUEUE_CAP ? QUEUE_CAP : queue_cap;
  if (timeout_ms > 0) g_timeout_ms = timeout_ms;
  if (breaker_n > 0) g_breaker_n = breaker_n;
  if (breaker_ms > 0) g_breaker_ms = breaker_ms;
}
void ldpc_cuda_test_reset(void) /* close the breaker */
{
  atomic_store(&g_consec_err, 0);
  atomic_store(&g_bypass_until_ms, 0);
  atomic_store(&g_perm_off, 0);
}
int ldpc_cuda_test_slots_used(void)
{
  int n = 0;
  pthread_mutex_lock(&g_mu);
  for (int i = 0; i < POOL_CAP; i++)
    n += g_slot_used[i];
  pthread_mutex_unlock(&g_mu);
  return n;
}

static int pool_timeout_ms(void)
{
  tunables_env_once();
  return g_timeout_ms;
}
static void deadline_after_ms(struct timespec *ts, int ms)
{
  clock_gettime(CLOCK_MONOTONIC, ts);
  ts->tv_sec += ms / 1000;
  ts->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec++;
    ts->tv_nsec -= 1000000000L;
  }
}

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
  for (;;) {
    gpu_req_t *take[QUEUE_CAP];
    pthread_mutex_lock(&g_mu);
    while (g_qn == 0)
      pthread_cond_wait(&g_cv_work, &g_mu);
    const int nt = g_qn;
    memcpy(take, g_queue, nt * sizeof(*take));
    g_qn = 0;
    for (int a = 0; a < nt; a++)
      take[a]->state = REQ_INFLIGHT;
    pthread_mutex_unlock(&g_mu);
    /* group by (BG, Z, iterations); the pool splits requests above its per-launch cap itself */
    bool used[QUEUE_CAP] = {false};
    for (int a = 0; a < nt; a++) {
      if (used[a])
        continue;
      uint32_t first[QUEUE_CAP], count[QUEUE_CAP], K[QUEUE_CAP];
      int idx[QUEUE_CAP], rcs[QUEUE_CAP];
      int nr = 0;
      for (int b = a; b < nt; b++) {
        if (used[b] || take[b]->BG != take[a]->BG || take[b]->Z != take[a]->Z || take[b]->iters != take[a]->iters)
          continue;
        used[b] = true;
        first[nr] = take[b]->first;
        count[nr] = take[b]->count;
        K[nr] = take[b]->K;
        idx[nr] = b;
        nr++;
      }
      const int rc_all = ldpc_pool_decode(take[a]->BG, take[a]->Z, take[a]->iters, nr, first, count, K, rcs);
      breaker_note_launch(rc_all != 0);
      pthread_mutex_lock(&g_mu);
      for (int i = 0; i < nr; i++) {
        gpu_req_t *q = take[idx[i]];
        q->rc = rcs[i];
        if (q->state == REQ_ABANDONED) { /* submitter timed out and fell back: we own request and slots now */
          memset(&g_slot_used[q->first], 0, q->count);
          free(q);
        } else {
          q->state = REQ_DONE;
        }
      }
      pthread_cond_broadcast(&g_cv_done);
      pthread_mutex_unlock(&g_mu);
      atomic_fetch_add(&g_batches, 1);
      atomic_fetch_add(&g_batched_reqs, nr);
    }
  }
  return NULL;
}

int32_t nrLDPC_coding_init(void)
{
  static pthread_mutex_t init_mu = PTHREAD_MUTEX_INITIALIZER;
  static int inited; /* idempotent: a second call must not start a second worker */
  pthread_mutex_lock(&init_mu);
  if (inited) {
    pthread_mutex_unlock(&init_mu);
    return 0;
  }
  pthread_once(&g_once, cond_setup);
  tunables_env_once();
  if (ldpc_pool_init(POOL_CAP) != 0) {
    pthread_mutex_unlock(&init_mu);
    return -1;
  }
  /* Warm-up: the CUDA module load and every first use of a CUDA-graph key (BG, Z, batch size rounded up to a power
   * of two, iterations) cost time that would otherwise land on a receiver thread. Pre-capture, for BG1 Z=384 and
   * BG2 Z=96 (the common sizes), batch sizes 1..512 and 2x{5,10} iterations. Other Z values still capture on first
   * use; that is bounded (waiters extend their timeout while the worker captures, and a timeout that overlapped a
   * capture never trips the breaker). A failing warm-up leaves the GPU disabled (CPU only), the plugin still loads. */
  {
    static const struct { uint32_t BG, Z, K; } wu[2] = {{1, 384, 8448}, {2, 96, 960}};
    static const uint32_t its[2] = {10, 20};
    memset(ldpc_pool_host_llr(), 0, (size_t)512 * ldpc_batch_llr_stride());
    for (int i = 0; i < 2 && !atomic_load(&g_perm_off); i++)
      for (int k = 0; k < 2 && !atomic_load(&g_perm_off); k++)
        for (uint32_t nb = 1; nb <= 512; nb <<= 1) {
          const uint32_t first = 0, count = nb;
          int rc = 0;
          if (ldpc_pool_decode(wu[i].BG, wu[i].Z, its[k], 1, &first, &count, &wu[i].K, &rc) != 0) {
            breaker_trip(W_WARMUP, "warm-up launch failed, GPU decoder disabled", true);
            break;
          }
        }
  }
  pthread_t th;
  if (pthread_create(&th, NULL, gpu_worker, NULL) != 0) {
    pthread_mutex_unlock(&init_mu);
    return -1;
  }
  pthread_detach(th);
  inited = 1;
  pthread_mutex_unlock(&init_mu);
  return 0;
}

int32_t nrLDPC_coding_shutdown(void)
{
  return 0;
}

/* CPU fallback: the renamed nrLDPC_decoder.c (see CMakeLists). Input = the int8 LLR block in the
 * layout the GPU got. Returns true and fills c (K/8 bytes) on CRC pass. */
int32_t ldpc_cpu_LDPCdecoder(t_nrLDPC_dec_params *p, int8_t *p_llr, uint8_t *p_out, t_nrLDPC_time_stats *ts, decode_abort_t *ab);
static bool cpu_decode_segment(const nrLDPC_TB_decoding_parameters_t *tb, int r, const int8_t *llr, uint8_t *c)
{
  t_nrLDPC_dec_params dp = {.check_crc = check_crc};
  dp.BG = tb->BG;
  dp.Z = tb->Z;
  dp.R = r < tb->first_rE2 ? tb->R : tb->R2;
  dp.numMaxIter = tb->max_ldpc_iterations;
  dp.outMode = nrLDPC_outMode_BIT;
  dp.crc_type = crcType(tb->C, tb->A);
  dp.Kprime = lenWithCrc(tb->C, tb->A);
  const int Kc = tb->BG == 2 ? 52 : 68;
  int8_t l[68 * 384 + 16] __attribute__((aligned(32)));
  int8_t out[27000] __attribute__((aligned(32)));
  memcpy(l, llr, (size_t)Kc * tb->Z);
  t_nrLDPC_time_stats pt = {0};
  /* honour the TB's shared abort flag exactly like the segment decoder (C>1: once one code block failed the others
   * stop early, the TB is lost anyway); a private flag only if the caller gave none */
  decode_abort_t own;
  decode_abort_t *ab = tb->abort_decode;
  if (!ab) {
    init_abort(&own);
    ab = &own;
  }
  const int it = ldpc_cpu_LDPCdecoder(&dp, l, (uint8_t *)out, &pt, ab);
  if (ab == &own)
    pthread_mutex_destroy(&own.mutex_failure);
  if (it < dp.numMaxIter) {
    memcpy(c, out, tb->K >> 3);
    return true;
  }
  return false;
}

static __thread double t_prep_s, t_gpu_s; /* LDPC_BENCH phase split */
/* Segments to decode for a TB: nb_segments_to_decode (0 = all C) -- a layout probe wants code
 * block 0 alone, with the CRC type / K' / offsets of the full C. */
static inline int tb_ndec(const nrLDPC_TB_decoding_parameters_t *tb)
{
  return (tb->nb_segments_to_decode > 0 && tb->nb_segments_to_decode < tb->C) ? (int)tb->nb_segments_to_decode : (int)tb->C;
}

int32_t nrLDPC_coding_decoder_impl(nrLDPC_slot_decoding_parameters_t *slot)
{
  const double t_start = ldpc_bench_on() ? ldpc_bench_wall_s() : 0;
  const int nt = slot->nb_TBs;
  if (nt == 0 || nt > 64)
    return nt ? -1 : 0;
  int nb = 0;
  for (int t = 0; t < nt; t++)
    nb += tb_ndec(&slot->TBs[t]);
  if (nb == 0)
    return 0;
  const uint32_t in_stride = ldpc_batch_llr_stride(), bits_stride = ldpc_batch_bits_stride();
  int8_t *pool_llr = ldpc_pool_host_llr();
  const uint8_t *pool_bits = ldpc_pool_host_bits();
  const int tmo = pool_timeout_ms();

  /* Reserve every TB's slots ALL-OR-NOTHING (a partial hold while waiting for the rest could deadlock two
   * callers). No fit within the timeout, or more blocks than the pool: the whole call goes to the CPU,
   * with LLRs prepared in a private scratch buffer. */
  int firsts[64];
  pthread_once(&g_once, cond_setup);
  bool gpu = nb <= POOL_CAP && breaker_state() == 0;
  const bool bypassed = nb <= POOL_CAP && !gpu;
  if (gpu) {
    struct timespec dl;
    deadline_after_ms(&dl, tmo);
    pthread_mutex_lock(&g_mu);
    for (;;) {
      int t = 0;
      for (; t < nt; t++)
        if ((firsts[t] = slots_reserve(tb_ndec(&slot->TBs[t]))) < 0)
          break;
      if (t == nt)
        break;
      for (int u = 0; u < t; u++) /* roll back */
        memset(&g_slot_used[firsts[u]], 0, tb_ndec(&slot->TBs[u]));
      if (pthread_cond_timedwait(&g_cv_done, &g_mu, &dl) == ETIMEDOUT) {
        gpu = false;
        break;
      }
    }
    pthread_mutex_unlock(&g_mu);
  }
  int8_t *scratch = NULL; /* private LLR copies: CPU-only call, or a TB whose request was abandoned */
  if (!gpu) {
    if (!bypassed)
      warn_once(W_FULL, "slot pool full or TB larger than the pool");
    scratch = malloc((size_t)nb * in_stride);
    AssertFatal(scratch, "CUDA LDPC fallback scratch alloc failed\n");
  }

  /* phase A: CPU prep of every segment, in the thread pool, straight into the pinned pool (or scratch) */
  nrLDPC_cuda_seg_t seg[nb];
  task_ans_t ans;
  init_task_ans(&ans, nb);
  int k = 0;
  for (int t = 0; t < nt; t++) {
    nrLDPC_TB_decoding_parameters_t *tb = &slot->TBs[t];
    *tb->processedSegments = 0;
    for (int r = 0; r < tb_ndec(tb); r++, k++) {
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
      s->batch_llr = gpu ? pool_llr + (size_t)(firsts[t] + r) * in_stride : scratch + (size_t)k * in_stride;
      s->prep_ok = false;
      s->ans = &ans;
      task_t task = {.func = &cuda_prepare_segment, .args = s};
      pushTpool(slot->threadPool, task);
    }
  }
  join_task_ans(&ans);
  const double t_a = ldpc_bench_on() ? ldpc_bench_wall_s() : 0;

  /* phase B: queue one heap request per TB, wait with a deadline. A TB that is rejected (queue full) or
   * not finished at the deadline is decoded on the CPU from a private copy of its LLR slots. */
  gpu_req_t *req[64] = {NULL};
  bool tb_gpu_ok[64] = {false}, in_scratch[64];
  for (int t = 0; t < nt; t++)
    in_scratch[t] = !gpu;
  if (gpu) {
    struct timespec dl;
    const uint32_t ep0 = ldpc_pool_capture_epoch(); /* before queueing: a capture started by our own request counts */
    deadline_after_ms(&dl, tmo);
    pthread_mutex_lock(&g_mu);
    const int qcap = g_queue_cap;
    int queued = 0;
    for (int t = 0; t < nt; t++) {
      nrLDPC_TB_decoding_parameters_t *tb = &slot->TBs[t];
      if (g_qn >= qcap) {
        warn_once(W_QUEUE, "GPU request queue full");
        continue; /* req[t] stays NULL -> CPU */
      }
      gpu_req_t *q = calloc(1, sizeof(*q));
      AssertFatal(q, "CUDA LDPC request alloc failed\n");
      *q = (gpu_req_t){.BG = tb->BG, .Z = tb->Z, .iters = 2 * tb->max_ldpc_iterations, .first = firsts[t],
                       .count = tb_ndec(tb), .K = tb->K, .state = REQ_QUEUED};
      g_queue[g_qn++] = q;
      req[t] = q;
      queued++;
    }
    if (queued)
      pthread_cond_signal(&g_cv_work);
    int ext = 0;
    for (int t = 0; t < nt; t++) {
      while (req[t] && req[t]->state != REQ_DONE) {
        if (pthread_cond_timedwait(&g_cv_done, &g_mu, &dl) == ETIMEDOUT) {
          /* the worker is capturing a CUDA graph for a new (BG, Z, batch, iterations) key: that is a one-time
           * cost, not a hang -- wait on (bounded: 100 extensions) */
          if (ldpc_pool_capturing() && ext++ < 100) {
            deadline_after_ms(&dl, tmo);
            continue;
          }
          break;
        }
      }
    }
    pthread_mutex_unlock(&g_mu);
    /* timed-out TBs: snapshot the LLR slots (only we write them; the GPU only reads) BEFORE giving the
     * request to the worker, whose completion then frees the slots */
    for (int t = 0; t < nt; t++) {
      if (!req[t])
        continue;
      pthread_mutex_lock(&g_mu);
      const bool done = req[t]->state == REQ_DONE;
      pthread_mutex_unlock(&g_mu);
      if (done)
        continue;
      if (!scratch) {
        scratch = malloc((size_t)nb * in_stride);
        AssertFatal(scratch, "CUDA LDPC fallback scratch alloc failed\n");
      }
      int kk = 0;
      for (int u = 0; u < t; u++)
        kk += tb_ndec(&slot->TBs[u]);
      memcpy(scratch + (size_t)kk * in_stride, pool_llr + (size_t)firsts[t] * in_stride, (size_t)tb_ndec(&slot->TBs[t]) * in_stride);
      in_scratch[t] = true;
      if (ldpc_pool_capture_epoch() != ep0) /* the wait overlapped a graph capture: slow, not a GPU fault */
        warn_once(W_CAPTURE, "GPU wait overlapped a CUDA graph capture");
      else
        breaker_trip(W_TIMEOUT, "GPU wait timeout, GPU bypassed for a few seconds", false);
    }
  }
  if (ldpc_bench_on())
    t_gpu_s += ldpc_bench_wall_s() - t_a;
  /* settle each request: DONE -> usable iff rc == 0; otherwise remove it from the queue / abandon it */
  pthread_mutex_lock(&g_mu);
  for (int t = 0; t < nt; t++) {
    gpu_req_t *q = req[t];
    if (!q)
      continue;
    if (q->state == REQ_DONE) {
      tb_gpu_ok[t] = q->rc == 0;
      continue;
    }
    if (q->state == REQ_QUEUED) { /* never taken by the worker: unqueue, we own it */
      for (int i = 0; i < g_qn; i++)
        if (g_queue[i] == q) {
          memmove(&g_queue[i], &g_queue[i + 1], (g_qn - i - 1) * sizeof(*g_queue));
          g_qn--;
          break;
        }
      memset(&g_slot_used[q->first], 0, q->count); /* never launched: the slots are ours to release (LLRs are in scratch) */
      free(q);
    } else {
      q->state = REQ_ABANDONED; /* worker frees request AND slots when it finishes */
    }
    req[t] = NULL;
    firsts[t] = -1; /* slots no longer ours */
  }
  pthread_mutex_unlock(&g_mu);

  /* phase C: the CRC of every segment (GPU bits only from an OK request), CPU decode otherwise */
  k = 0;
  for (int t = 0; t < nt; t++) {
    nrLDPC_TB_decoding_parameters_t *tb = &slot->TBs[t];
    const int C = tb->C;
    const uint32_t Kprime = lenWithCrc(C, tb->A);
    const uint8_t crc_type = crcType(C, tb->A);
    const bool use_gpu = gpu && tb_gpu_ok[t];
    if (!use_gpu)
      atomic_fetch_add(&g_fallbacks, 1);
    tb->decoder_used = use_gpu ? NRLDPC_DECODER_CUDA_FLOODING : NRLDPC_DECODER_CPU;
    for (int r = 0; r < tb_ndec(tb); r++, k++) {
      uint8_t *c = tb->c + r * (tb->K >> 3);
      bool ok;
      if (use_gpu) {
        const uint8_t *b = pool_bits + (size_t)(firsts[t] + r) * bits_stride;
        ok = seg[k].prep_ok && check_crc((uint8_t *)b, Kprime, crc_type);
        if (ok)
          memcpy(c, b, tb->K >> 3);
      } else {
        const int8_t *l = in_scratch[t] ? scratch + (size_t)k * in_stride : pool_llr + (size_t)(firsts[t] + r) * in_stride;
        ok = seg[k].prep_ok && cpu_decode_segment(tb, r, l, c);
      }
      if (!ok)
        memset(c, 0, tb->K >> 3);
      tb->decodeSuccess[r] = ok;
      *tb->processedSegments += ok;
    }
  }
  pthread_mutex_lock(&g_mu);
  for (int t = 0; t < nt; t++)
    if (gpu && firsts[t] >= 0) {
      memset(&g_slot_used[firsts[t]], 0, tb_ndec(&slot->TBs[t]));
      free(req[t]);
    }
  pthread_cond_broadcast(&g_cv_done);
  pthread_mutex_unlock(&g_mu);
  free(scratch);
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
