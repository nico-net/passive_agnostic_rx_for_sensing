/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* DECGPU exact LDPC, host side: ISAC_PDSCH_GPU_EXACT_LDPC=1 (default off) decodes a passive PDSCH TB with the GPU port of
 * OAI's CPU decoder (nr_pdsch_gpu_ldpc.cu) instead of libldpc.so, with the same result per code block:
 *   - deinterleaving, rate dematching into the HARQ buffer d, the 2Z zeros / filler 127s / int8 packing: verbatim
 *     nr_process_decode_segment() (nrLDPC_coding_segment_decoder.c), on the CPU;
 *   - the iterations: the GPU, all code blocks of the TB in one launch; it returns the hard decision after every
 *     iteration at which OAI checks the CRC;
 *   - the stop rule: OAI's -- the first snapshot whose check_crc() passes is the decoded block, none = failure (c
 *     zeroed, decodeSuccess false, abort set, so the later blocks of the TB fail as with the inline thread pool).
 *     The GPU stops a block early only at a zero syndrome; if that snapshot's CRC fails (a codeword with a wrong
 *     CRC: later iterations unknown) the whole TB is redone by the CPU decoder.
 * Refused (CPU decoder): HARQ combining into a kept buffer (d not cleared), more than one TB per call, > 17 iterations.
 * ISAC_PDSCH_GPU_EXACT_LDPC_CHECK=N (N >= 1): every N-th TB is also decoded by the CPU, which gives the result used;
 * every code block's success flag and bytes are compared and the first difference disables the GPU path. */
#include "nr_pdsch_gpu_ldpc_host.h"
#include "nr_pdsch_gpu_ldpc.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_segment/nr_rate_matching.h"
#include "common/utils/LOG/log.h"
#include "PHY/sse_intrin.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static nr_gpu_ldpc_run_fn g_run;
static _Atomic int g_on = -1; /* -1 unresolved, 0 off, 1 on */
static int g_check;
static _Atomic uint64_t g_calls, g_cbs, g_fb_refuse, g_fb_syn, g_fb_err, g_chk_ok, g_chk_bad, g_ns_sum, g_ns_gpu;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static uint64_t now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void load(void)
{
  const char *e = getenv("ISAC_PDSCH_GPU_EXACT_LDPC");
  if (!e || atoi(e) != 1) {
    atomic_store(&g_on, 0);
    return;
  }
  void *h = dlopen("libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  nr_gpu_ldpc_init_fn init = h ? (nr_gpu_ldpc_init_fn)dlsym(h, "nr_pdsch_gpu_ldpc_init") : NULL;
  g_run = h ? (nr_gpu_ldpc_run_fn)dlsym(h, "nr_pdsch_gpu_ldpc_run") : NULL;
  if (!init || !g_run || init() != 0) {
    LOG_W(PHY, "SENSING: GPU_EXACT_LDPC requested but libpdsch_gpu.so has no usable LDPC entry -- CPU decoder\n");
    atomic_store(&g_on, 0);
    return;
  }
  g_check = getenv("ISAC_PDSCH_GPU_EXACT_LDPC_CHECK") ? atoi(getenv("ISAC_PDSCH_GPU_EXACT_LDPC_CHECK")) : 0;
  LOG_A(PHY, "SENSING: GPU_EXACT_LDPC enabled: OAI min-sum decoder on the GPU, bit-exact; check=%d\n", g_check);
  atomic_store(&g_on, 1);
}

int nr_pdsch_gpu_ldpc_enabled(void)
{
  if (atomic_load_explicit(&g_on, memory_order_relaxed) < 0)
    pthread_once(&g_once, load);
  return atomic_load_explicit(&g_on, memory_order_relaxed) == 1;
}

/* per-thread scratch: jobs (with their snapshots), int8 inputs, the deinterleaved e */
typedef struct {
  nr_gpu_ldpc_job_t *jobs;
  int8_t *l;      /* [NR_LDPC_MAX_NUM_CB][68 * 384 + 16] */
  int16_t *e;
  uint32_t e_cap;
  uint8_t *c_chk; /* CHECK: GPU result bytes */
} tls_t;

static tls_t *tls(uint32_t E)
{
  static __thread tls_t t;
  if (!t.jobs) {
    t.jobs = malloc(sizeof(nr_gpu_ldpc_job_t) * NR_GPU_LDPC_MAX_JOBS);
    t.l = aligned_alloc(16, (size_t)NR_GPU_LDPC_MAX_JOBS * (68 * 384 + 16));
    t.c_chk = malloc((size_t)NR_LDPC_MAX_NUM_CB * (8448 / 8 + 8));
    if (!t.jobs || !t.l || !t.c_chk)
      return NULL;
  }
  if (t.e_cap < E) {
    free(t.e);
    t.e = malloc(sizeof(int16_t) * E);
    t.e_cap = t.e ? E : 0;
    if (!t.e)
      return NULL;
  }
  return &t;
}

/* nr_process_decode_segment() up to the decoder call: rate dematching into d, the decoder's int8 input l. 0 = ok. */
static int prep_segment(nrLDPC_TB_decoding_parameters_t *tb, int r, tls_t *t, int8_t *l, int *Eout, uint8_t *Rout)
{
  const int Kc = tb->BG == 2 ? 52 : 68, Z = tb->Z, K = tb->K, F = tb->F;
  int E, off;
  uint8_t R;
  if (r < tb->first_rE2) {
    R = tb->R;
    E = tb->E;
    off = r * E;
  } else {
    R = tb->R2;
    E = tb->E2;
    off = tb->first_rE2 * tb->E + (r - tb->first_rE2) * E;
  }
  *Eout = E;
  *Rout = R;
  int16_t *d = tb->d + (size_t)r * Kc * Z;
  nr_deinterleaving_ldpc(E, tb->Qm, t->e, tb->llr + off);
  if (nr_rate_matching_ldpc_rx(tb->tbslbrm, tb->BG, Z, d, t->e, tb->C, tb->rv_index, tb->d_to_be_cleared, E, F, K - F - 2 * Z) == -1)
    return -1;
  const int Kprime = lenWithCrc(tb->C, tb->A);
  int16_t z[68 * 384 + 16] __attribute__((aligned(16)));
  memset(z, 0, 2 * Z * sizeof(*z));
  memset(z + Kprime, 127, F * sizeof(*z)); /* as the CPU source: int16 0x7f7f, packed to 127 */
  memcpy(z + 2 * Z, d, (Kprime - 2 * Z) * sizeof(*z));
  memcpy(z + K, d + (K - 2 * Z), (Kc * Z - K) * sizeof(*z));
  simde__m128i *pv = (simde__m128i *)&z;
  simde__m128i *pl = (simde__m128i *)l;
  for (int i = 0, j = 0; j < ((Kc * Z) >> 4) + 1; i += 2, j++)
    pl[j] = simde_mm_packs_epi16(pv[i], pv[i + 1]);
  return 0;
}

static inline uint32_t tb_ndec(const nrLDPC_TB_decoding_parameters_t *tb)
{
  return (tb->nb_segments_to_decode > 0 && tb->nb_segments_to_decode < tb->C) ? tb->nb_segments_to_decode : tb->C;
}

/* GPU decode of one TB into c (K/8 bytes per block) and ok[]. 0 = done; 1 = needs the CPU decoder (nothing of the
 * caller's output is final; d holds the dematched blocks, which the CPU decoder recomputes since d is cleared). */
static int gpu_tb(nrLDPC_TB_decoding_parameters_t *tb, uint8_t *c, bool *ok, uint64_t *gpu_ns)
{
  const uint32_t n_dec = tb_ndec(tb);
  const int KB = tb->K >> 3;
  tls_t *t = tls(tb->G);
  if (!t)
    return 1;
  const int Kprime = lenWithCrc(tb->C, tb->A);
  const int crc_type = crcType(tb->C, tb->A);
  bool aborted = check_abort(tb->abort_decode);
  for (uint32_t r0 = 0; r0 < n_dec; r0 += NR_GPU_LDPC_MAX_JOBS) {
    const int n = (int)(n_dec - r0 < NR_GPU_LDPC_MAX_JOBS ? n_dec - r0 : NR_GPU_LDPC_MAX_JOBS);
    int rm_bad[NR_GPU_LDPC_MAX_JOBS] = {0};
    for (int k = 0; k < n; k++) {
      int E;
      uint8_t R;
      int8_t *l = t->l + (size_t)k * (68 * 384 + 16);
      rm_bad[k] = prep_segment(tb, (int)r0 + k, t, l, &E, &R) != 0;
      t->jobs[k] = (nr_gpu_ldpc_job_t){.BG = tb->BG, .R = R, .Z = (uint16_t)tb->Z, .K = (uint16_t)tb->K,
                                       .it_max = tb->max_ldpc_iterations, .llr = l};
    }
    const uint64_t g0 = now_ns();
    if (g_run(t->jobs, n) != 0) {
      atomic_fetch_add_explicit(&g_fb_err, 1, memory_order_relaxed);
      return 1;
    }
    *gpu_ns += now_ns() - g0;
    for (int k = 0; k < n; k++) { /* OAI's sequential verdicts: LDPCdecoder() per block, abort shared by the TB */
      const uint32_t r = r0 + k;
      uint8_t *cr = c + (size_t)r * KB;
      ok[r] = false;
      if (rm_bad[k])
        continue; /* nr_process_decode_segment() returns before the decoder: nothing written, no abort */
      const nr_gpu_ldpc_job_t *j = &t->jobs[k];
      int hit = -1;
      if (!aborted)
        for (int s = 0; s < j->nsnap && hit < 0; s++)
          if (check_crc((uint8_t *)j->snap[s], Kprime, crc_type))
            hit = s;
      if (hit >= 0) {
        memcpy(cr, j->snap[hit], KB);
        ok[r] = true;
        continue;
      }
      if (!aborted && j->syn0) { /* stopped on a codeword whose CRC fails: OAI would iterate on */
        atomic_fetch_add_explicit(&g_fb_syn, 1, memory_order_relaxed);
        return 1;
      }
      memset(cr, 0, KB);
      aborted = true; /* LDPCdecoder: numIter >= numMaxIter -> set_abort */
    }
  }
  if (aborted)
    set_abort(tb->abort_decode, true);
  return 0;
}

static void stats(uint64_t n)
{
  if (!(n == 1 || n == 100 || n % 2000 == 0))
    return;
  LOG_A(PHY, "SENSING: GPU_EXACT_LDPC tbs=%lu cbs=%lu check_ok=%lu check_bad=%lu fallback[refused,syn0,err]=[%lu,%lu,%lu] mean=%.0f us (gpu %.0f)\n",
        (unsigned long)n, (unsigned long)atomic_load(&g_cbs), (unsigned long)atomic_load(&g_chk_ok), (unsigned long)atomic_load(&g_chk_bad),
        (unsigned long)atomic_load(&g_fb_refuse), (unsigned long)atomic_load(&g_fb_syn), (unsigned long)atomic_load(&g_fb_err),
        atomic_load(&g_ns_sum) / 1e3 / n, atomic_load(&g_ns_gpu) / 1e3 / n);
}

int32_t nr_pdsch_gpu_ldpc_decode(nrLDPC_slot_decoding_parameters_t *p, nrLDPC_coding_decoder_t *cpu)
{
  if (!nr_pdsch_gpu_ldpc_enabled() || p->nb_TBs != 1)
    return cpu(p);
  nrLDPC_TB_decoding_parameters_t *tb = &p->TBs[0];
  if (!tb->d_to_be_cleared || tb->max_ldpc_iterations < 1 || tb->max_ldpc_iterations > NR_GPU_LDPC_MAX_SNAP + 1
      || tb->C > NR_LDPC_MAX_NUM_CB || tb->K > 8448) {
    atomic_fetch_add_explicit(&g_fb_refuse, 1, memory_order_relaxed);
    return cpu(p);
  }
  const uint64_t t0 = now_ns();
  const uint64_t n = atomic_fetch_add_explicit(&g_calls, 1, memory_order_relaxed) + 1;
  const uint32_t n_dec = tb_ndec(tb);
  atomic_fetch_add_explicit(&g_cbs, n_dec, memory_order_relaxed);
  const int check = g_check == 1 || (g_check > 1 && n % (uint64_t)g_check == 0);
  uint64_t gns = 0;
  int32_t rc = 0;
  tb->decoder_used = NRLDPC_DECODER_CPU; /* bit-exact with it */
  *tb->processedSegments = 0;
  if (!check) {
    if (gpu_tb(tb, tb->c, tb->decodeSuccess, &gns) != 0) {
      rc = cpu(p);
    } else {
      for (uint32_t r = 0; r < n_dec; r++)
        *tb->processedSegments += tb->decodeSuccess[r];
    }
  } else {
    tls_t *t = tls(tb->G);
    bool okg[NR_LDPC_MAX_NUM_CB] = {0};
    const bool ab0 = check_abort(tb->abort_decode);
    const int r_g = t ? gpu_tb(tb, t->c_chk, okg, &gns) : 1;
    set_abort(tb->abort_decode, ab0);
    rc = cpu(p);
    if (r_g == 0 && rc == 0) {
      const int KB = tb->K >> 3;
      int bad = -1;
      for (uint32_t r = 0; r < n_dec && bad < 0; r++)
        if (okg[r] != tb->decodeSuccess[r] || (okg[r] && memcmp(t->c_chk + (size_t)r * KB, tb->c + (size_t)r * KB, KB) != 0))
          bad = (int)r;
      if (bad < 0) {
        atomic_fetch_add_explicit(&g_chk_ok, 1, memory_order_relaxed);
      } else {
        atomic_fetch_add_explicit(&g_chk_bad, 1, memory_order_relaxed);
        atomic_store(&g_on, 0);
        LOG_E(PHY, "SENSING: GPU_EXACT_LDPC MISMATCH cb %d of %u (C %u BG %u Z %u K %u R %u/%u iters %u): gpu ok %d cpu ok %d -- GPU LDPC disabled\n",
              bad, n_dec, tb->C, tb->BG, tb->Z, tb->K, tb->R, tb->R2, tb->max_ldpc_iterations, okg[bad], tb->decodeSuccess[bad]);
      }
    }
  }
  atomic_fetch_add_explicit(&g_ns_sum, now_ns() - t0, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_ns_gpu, gns, memory_order_relaxed);
  stats(n);
  return rc;
}
