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
int8_t *ldpc_batch_llr_buffer(uint32_t n);
uint32_t ldpc_batch_llr_stride(void);
uint32_t ldpc_batch_bits_stride(void);
const uint8_t *ldpc_batch_decode(uint32_t BG, uint32_t Z, uint32_t first, uint32_t n, uint32_t block_length,
                                 uint32_t num_iter);

typedef struct {
  uint32_t BG, Z, Kc, K, F, A, C, E, Qm, rv_index, tbslbrm;
  short *llr;
  int16_t *d;
  bool d_to_be_cleared;
  int8_t *batch_llr; /* this segment's slot in the pinned batch buffer */
  bool prep_ok;
  task_ans_t *ans;
} nrLDPC_cuda_seg_t;

/* De-interleave, rate de-match, fill and pack one segment into its batch slot. */
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
    for (int i = 0; i < (int)(s->Kc * Z); i++)                       /* saturate to int8 */
      s->batch_llr[i] = (int8_t)(z[i] > 127 ? 127 : z[i] < -128 ? -128 : z[i]);
  }
  completed_task_ans(s->ans);
}

int32_t nrLDPC_coding_init(void)
{
  return ldpc_decoder_init(1) ? 0 : -1; /* global base-graph tables once, before any worker thread */
}

int32_t nrLDPC_coding_shutdown(void)
{
  return 0;
}

int32_t nrLDPC_coding_decoder_impl(nrLDPC_slot_decoding_parameters_t *slot)
{
  int nb = 0;
  for (int t = 0; t < slot->nb_TBs; t++)
    nb += slot->TBs[t].C;
  if (nb == 0)
    return 0;
  int8_t *batch = ldpc_batch_llr_buffer(nb);
  const uint32_t in_stride = ldpc_batch_llr_stride(), bits_stride = ldpc_batch_bits_stride();
  nrLDPC_cuda_seg_t seg[nb];
  task_ans_t ans;
  init_task_ans(&ans, nb);

  /* phase A: CPU prep of every segment of every TB, in the thread pool */
  int k = 0;
  for (int t = 0; t < slot->nb_TBs; t++) {
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
      s->batch_llr = batch + (size_t)k * in_stride;
      s->prep_ok = false;
      s->ans = &ans;
      task_t task = {.func = &cuda_prepare_segment, .args = s};
      pushTpool(slot->threadPool, task);
    }
  }
  join_task_ans(&ans);

  /* phase B + C: one GPU batch per TB (all its segments share BG and Z), then the CRC per segment */
  k = 0;
  for (int t = 0; t < slot->nb_TBs; t++) {
    nrLDPC_TB_decoding_parameters_t *tb = &slot->TBs[t];
    const int C = tb->C;
    const uint32_t Kprime = lenWithCrc(C, tb->A);
    const uint8_t crc_type = crcType(C, tb->A);
    start_meas(&tb->ts_ldpc_decode);
    const uint8_t *bits = ldpc_batch_decode(tb->BG, tb->Z, k, C, tb->K, 2 * tb->max_ldpc_iterations);
    stop_meas(&tb->ts_ldpc_decode);
    for (int r = 0; r < C; r++, k++) {
      const uint8_t *b = bits + (size_t)r * bits_stride;
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
  return rc;
}
