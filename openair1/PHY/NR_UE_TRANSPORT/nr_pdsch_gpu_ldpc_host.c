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
#include "nr_pdsch_host_fast.h"
#include "PHY/TOOLS/nr_stage_split.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_cpu_route.h" /* BATCHCPU */

/* One instance per user of the GPU decoder: the DL (ISAC_PDSCH_GPU_EXACT_LDPC) and the UL (ISAC_UL_GPU_LDPC, own context pool,
 * own counters and own CHECK/disable state: a UL mismatch never switches the DL off). */
typedef struct {
  nr_gpu_ldpc_run_fn run;
  _Atomic int on; /* -1 unresolved, 0 off, 1 on */
  int check;
  int ul;
  uint32_t min_bits; /* UL: TBs with fewer coded information bits (K x code blocks) stay on the CPU, where a call is cheaper than a launch */
  const char *tag;
  _Atomic uint64_t calls, cbs, fb_refuse, fb_syn, fb_err, chk_ok, chk_bad, ns_sum, ns_gpu, fb_small;
  _Atomic uint64_t fb_reason[NR_GPU_LDPC_NERR]; /* DL: fb_err by nr_gpu_ldpc_err (index -rc) */
  pthread_once_t once;
} gl_t;
static gl_t DLG = {.on = -1, .once = PTHREAD_ONCE_INIT, .tag = "GPU_EXACT_LDPC"};
static gl_t ULG = {.on = -1, .once = PTHREAD_ONCE_INIT, .ul = 1, .tag = "UL_GPU_LDPC"};
static nr_gpu_ldpc_ul_stats_fn ul_stats_fn;
static nrLDPC_coding_decoder_t *ul_cpu_dec;

static uint64_t now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void load(void)
{
  gl_t *G = &DLG;
  const char *e = (getenv("ISAC_PDSCH_GPU_EXACT_LDPC") ?: "1");
  if (!e || atoi(e) != 1) {
    atomic_store(&G->on, 0);
    return;
  }
  void *h = dlopen("libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  nr_gpu_ldpc_init_fn init = h ? (nr_gpu_ldpc_init_fn)dlsym(h, "nr_pdsch_gpu_ldpc_init") : NULL;
  G->run = h ? (nr_gpu_ldpc_run_fn)dlsym(h, "nr_pdsch_gpu_ldpc_run") : NULL;
  if (!init || !G->run || init() != 0) {
    LOG_W(PHY, "SENSING: GPU_EXACT_LDPC requested but libpdsch_gpu.so has no usable LDPC entry -- CPU decoder\n");
    atomic_store(&G->on, 0);
    return;
  }
  G->check = getenv("ISAC_PDSCH_GPU_EXACT_LDPC_CHECK") ? atoi(getenv("ISAC_PDSCH_GPU_EXACT_LDPC_CHECK")) : 0;
  LOG_A(PHY, "SENSING: GPU_EXACT_LDPC enabled: OAI min-sum decoder on the GPU, bit-exact; check=%d\n", G->check);
  atomic_store(&G->on, 1);
}

int nr_pdsch_gpu_ldpc_enabled(void)
{
  gl_t *G = &DLG;
  if (atomic_load_explicit(&G->on, memory_order_relaxed) < 0)
    pthread_once(&G->once, load);
  return atomic_load_explicit(&G->on, memory_order_relaxed) == 1;
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
static int prep_segment_ref(nrLDPC_TB_decoding_parameters_t *tb, int r, tls_t *t, int8_t *l, int *Eout, uint8_t *Rout)
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
  static int s_hf = -1; /* ISAC_PDSCH_HOST_FAST=1 (default off): NEON deinterleave / rate-match / pack, bit-identical */
  if (s_hf < 0)
    s_hf = atoi(getenv("ISAC_PDSCH_HOST_FAST") ?: "1") ? 1 : 0;
  if (s_hf && tb->C != 0 && tb->d_to_be_cleared == 1 && tb->rv_index < 4 && tb->BG >= 1 && tb->BG <= 2) {
    static const uint8_t k0[2][4] = {{0, 17, 33, 56}, {0, 13, 25, 43}};
    const uint32_t N = (tb->BG == 1) ? 66u * Z : 50u * Z;
    const uint32_t Nref = tb->tbslbrm ? 3u * tb->tbslbrm / (2u * tb->C) : N;
    const uint32_t Ncb = Nref < N ? Nref : N;
    const uint32_t ind = (k0[tb->BG - 1][tb->rv_index] * Ncb / N) * Z, Foff = K - F - 2 * Z;
    if (Foff <= Ncb && nrhf_deinterleave(E, tb->Qm, t->e, tb->llr + off)) {
      memset(d, 0, N * sizeof(int16_t));
      nrhf_rm_rx(d, t->e, E, ind, Ncb, Foff, F);
      nrhf_pack(l, d, Z, K, F, lenWithCrc(tb->C, tb->A), Kc * Z);
      return 0;
    }
  }
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

/* ISAC_PDSCH_HOST_FAST=1 (default off): NEON deinterleave / rate-match / int8 pack, bit-identical to prep_segment_ref();
 * =2: run both and compare (logs the first difference, uses the reference result). Returns -2 when the fast path does not apply. */
static int prep_segment_fast(nrLDPC_TB_decoding_parameters_t *tb, int r, tls_t *t, int8_t *l, int *Eout, uint8_t *Rout)
{
  const int Kc = tb->BG == 2 ? 52 : 68, Z = tb->Z, K = tb->K, F = tb->F;
  if (tb->C == 0 || tb->d_to_be_cleared != 1 || tb->rv_index >= 4 || tb->BG < 1 || tb->BG > 2)
    return -2;
  int E, off;
  if (r < tb->first_rE2) {
    *Rout = tb->R; E = tb->E; off = r * E;
  } else {
    *Rout = tb->R2; E = tb->E2; off = tb->first_rE2 * tb->E + (r - tb->first_rE2) * E;
  }
  static const uint8_t k0[2][4] = {{0, 17, 33, 56}, {0, 13, 25, 43}};
  const uint32_t N = (tb->BG == 1) ? 66u * Z : 50u * Z;
  const uint32_t Nref = tb->tbslbrm ? 3u * tb->tbslbrm / (2u * tb->C) : N;
  const uint32_t Ncb = Nref < N ? Nref : N;
  const uint32_t ind = (k0[tb->BG - 1][tb->rv_index] * Ncb / N) * Z, Foff = K - F - 2 * Z;
  int16_t *d = tb->d + (size_t)r * Kc * Z;
  if (Foff > Ncb || !nrhf_deinterleave(E, tb->Qm, t->e, tb->llr + off))
    return -2;
  *Eout = E;
  memset(d, 0, N * sizeof(int16_t));
  nrhf_rm_rx(d, t->e, E, ind, Ncb, Foff, F);
  nrhf_pack(l, d, Z, K, F, lenWithCrc(tb->C, tb->A), Kc * Z);
  return 0;
}

static int prep_segment(nrLDPC_TB_decoding_parameters_t *tb, int r, tls_t *t, int8_t *l, int *Eout, uint8_t *Rout)
{
  static int s_hf = -1;
  if (s_hf < 0)
    s_hf = getenv("ISAC_PDSCH_HOST_FAST") ? atoi(getenv("ISAC_PDSCH_HOST_FAST")) : 1;
  if (s_hf <= 0)
    return prep_segment_ref(tb, r, t, l, Eout, Rout);
  const int rc = prep_segment_fast(tb, r, t, l, Eout, Rout);
  if (rc != -2 && s_hf != 2)
    return rc;
  if (rc == -2)
    return prep_segment_ref(tb, r, t, l, Eout, Rout);
  const int Kc = tb->BG == 2 ? 52 : 68, Z = tb->Z;
  const size_t nd = (size_t)(tb->BG == 2 ? 50 : 66) * Z; /* the cleared / accumulated part of d */
  int8_t *sv = malloc((size_t)Kc * Z);
  int16_t *dv = malloc((size_t)Kc * Z * sizeof(int16_t));
  int16_t *d = tb->d + (size_t)r * Kc * Z;
  memcpy(sv, l, (size_t)Kc * Z);
  memcpy(dv, d, (size_t)Kc * Z * sizeof(int16_t));
  const int rr = prep_segment_ref(tb, r, t, l, Eout, Rout);
  if (memcmp(sv, l, (size_t)Kc * Z) || memcmp(dv, d, nd * sizeof(int16_t))) {
    int i = 0;
    while (i < Kc * Z && sv[i] == l[i]) i++;
    LOG_E(PHY, "SENSING: HOST_FAST MISMATCH r %d C %u BG %u Z %d K %u F %u A %u E %u Qm %u rv %u lbrm %u first l diff at %d (fast %d ref %d), d %s\n", r,
          tb->C, tb->BG, Z, tb->K, tb->F, tb->A, *Eout, tb->Qm, tb->rv_index, tb->tbslbrm, i, i < Kc * Z ? sv[i] : 0, i < Kc * Z ? l[i] : 0,
          memcmp(dv, d, nd * sizeof(int16_t)) ? "DIFF" : "same");
  }
  free(sv);
  free(dv);
  return rr;
}

static inline uint32_t tb_ndec(const nrLDPC_TB_decoding_parameters_t *tb)
{
  return (tb->nb_segments_to_decode > 0 && tb->nb_segments_to_decode < tb->C) ? tb->nb_segments_to_decode : tb->C;
}

/* GPU decode of one TB into c (K/8 bytes per block) and ok[]. 0 = done; 1 = needs the CPU decoder (nothing of the
 * caller's output is final; d holds the dematched blocks, which the CPU decoder recomputes since d is cleared). */
static int gpu_tb(gl_t *G, nrLDPC_TB_decoding_parameters_t *tb, uint8_t *c, bool *ok, uint64_t *gpu_ns)
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
    const uint64_t ss_prep = nr_ss_now();
    for (int k = 0; k < n; k++) {
      int E;
      uint8_t R;
      int8_t *l = t->l + (size_t)k * (68 * 384 + 16);
      rm_bad[k] = prep_segment(tb, (int)r0 + k, t, l, &E, &R) != 0;
      t->jobs[k] = (nr_gpu_ldpc_job_t){.BG = tb->BG, .R = R, .Z = (uint16_t)tb->Z, .K = (uint16_t)tb->K,
                                       .it_max = tb->max_ldpc_iterations, .llr = l};
    }
    nr_ss_add(NRSS_PREP, ss_prep);
    const uint64_t g0 = now_ns();
    const int rc = G->run(t->jobs, n);
    if (rc != 0) {
      atomic_fetch_add_explicit(&G->fb_err, 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&G->fb_reason[rc < 0 && -rc < NR_GPU_LDPC_NERR ? -rc : 0], 1, memory_order_relaxed);
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
        atomic_fetch_add_explicit(&G->fb_syn, 1, memory_order_relaxed);
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

#define FBR(i) ((unsigned long)atomic_load(&G->fb_reason[i]))
static void stats(gl_t *G, uint64_t n)
{
  if (!(n == 1 || n == 100 || n % 2000 == 0))
    return;
  LOG_A(PHY, "SENSING: %s tbs=%lu cbs=%lu check_ok=%lu check_bad=%lu fallback[refused,syn0,err]=[%lu,%lu,%lu] fallback_err[init,range,param,graph,stream,sentinel,cap,rt,busy,deadline]=[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu] other=%lu mean=%.0f us (gpu %.0f)\n",
        G->tag, (unsigned long)n, (unsigned long)atomic_load(&G->cbs), (unsigned long)atomic_load(&G->chk_ok), (unsigned long)atomic_load(&G->chk_bad),
        (unsigned long)atomic_load(&G->fb_refuse), (unsigned long)atomic_load(&G->fb_syn), (unsigned long)atomic_load(&G->fb_err),
        FBR(1), FBR(2), FBR(3), FBR(4), FBR(5), FBR(6), FBR(7), FBR(8), FBR(9), FBR(10), FBR(0),
        atomic_load(&G->ns_sum) / 1e3 / n, atomic_load(&G->ns_gpu) / 1e3 / n);
}

static int32_t decode_impl(gl_t *G, nrLDPC_slot_decoding_parameters_t *p, nrLDPC_coding_decoder_t *cpu)
{
  if (p->nb_TBs != 1 || (!G->ul && nr_pdsch_route_cpu())) /* BATCHCPU */
    return cpu(p);
  nrLDPC_TB_decoding_parameters_t *tb = &p->TBs[0];
  if (!tb->d_to_be_cleared || tb->max_ldpc_iterations < 1 || tb->max_ldpc_iterations > NR_GPU_LDPC_MAX_SNAP + 1
      || tb->C > NR_LDPC_MAX_NUM_CB || tb->K > 8448) {
    atomic_fetch_add_explicit(&G->fb_refuse, 1, memory_order_relaxed);
    return cpu(p);
  }
  if (G->ul && (uint64_t)tb->K * tb_ndec(tb) < G->min_bits) {
    atomic_fetch_add_explicit(&G->fb_small, 1, memory_order_relaxed);
    return cpu(p);
  }
  const uint64_t t0 = now_ns();
  const uint64_t n = atomic_fetch_add_explicit(&G->calls, 1, memory_order_relaxed) + 1;
  const uint32_t n_dec = tb_ndec(tb);
  atomic_fetch_add_explicit(&G->cbs, n_dec, memory_order_relaxed);
  const int check = G->check == 1 || (G->check > 1 && n % (uint64_t)G->check == 0);
  uint64_t gns = 0;
  int32_t rc = 0;
  tb->decoder_used = NRLDPC_DECODER_CPU; /* bit-exact with it */
  *tb->processedSegments = 0;
  if (!check) {
    if (gpu_tb(G, tb, tb->c, tb->decodeSuccess, &gns) != 0) {
      rc = cpu(p);
    } else {
      for (uint32_t r = 0; r < n_dec; r++)
        *tb->processedSegments += tb->decodeSuccess[r];
    }
  } else {
    tls_t *t = tls(tb->G);
    bool okg[NR_LDPC_MAX_NUM_CB] = {0};
    const bool ab0 = check_abort(tb->abort_decode);
    const int r_g = t ? gpu_tb(G, tb, t->c_chk, okg, &gns) : 1;
    set_abort(tb->abort_decode, ab0);
    rc = cpu(p);
    if (r_g == 0 && rc == 0) {
      const int KB = tb->K >> 3;
      int bad = -1;
      for (uint32_t r = 0; r < n_dec && bad < 0; r++)
        if (okg[r] != tb->decodeSuccess[r] || (okg[r] && memcmp(t->c_chk + (size_t)r * KB, tb->c + (size_t)r * KB, KB) != 0))
          bad = (int)r;
      if (bad < 0) {
        atomic_fetch_add_explicit(&G->chk_ok, 1, memory_order_relaxed);
      } else {
        atomic_fetch_add_explicit(&G->chk_bad, 1, memory_order_relaxed);
        atomic_store(&G->on, 0);
        LOG_E(PHY, "SENSING: %s MISMATCH cb %d of %u (C %u BG %u Z %u K %u R %u/%u iters %u): gpu ok %d cpu ok %d -- GPU LDPC disabled\n",
              G->tag, bad, n_dec, tb->C, tb->BG, tb->Z, tb->K, tb->R, tb->R2, tb->max_ldpc_iterations, okg[bad], tb->decodeSuccess[bad]);
      }
    }
  }
  atomic_fetch_add_explicit(&G->ns_sum, now_ns() - t0, memory_order_relaxed);
  atomic_fetch_add_explicit(&G->ns_gpu, gns, memory_order_relaxed);
  stats(G, n);
  return rc;
}

int32_t nr_pdsch_gpu_ldpc_decode(nrLDPC_slot_decoding_parameters_t *p, nrLDPC_coding_decoder_t *cpu)
{
  if (!nr_pdsch_gpu_ldpc_enabled())
    return cpu(p);
  return decode_impl(&DLG, p, cpu);
}

/* ---- ULLDPC: ISAC_UL_GPU_LDPC=1 (default off). The passive PUSCH decode (nr_ulsch_decoding) calls the coding interface
 * once per TB; nr_pusch_gpu_ldpc_wrap() returns a decoder that sends the TB to the UL pool of libpdsch_gpu.so and calls the
 * original (CPU) decoder for everything it refuses (HARQ-combined buffers, a busy / over-cap / late GPU, any error). Same
 * bit-exact code path and CRC stop rule as the DL (gpu_tb), so the CRC verdicts and decoded bytes are the CPU decoder's.
 * ISAC_UL_GPU_LDPC_CHECK=N (N >= 1, default 64): every N-th TB is also decoded on the CPU and compared. */
static void ul_load(void)
{
  gl_t *G = &ULG;
  const char *e = getenv("ISAC_UL_GPU_LDPC");
  if (e && atoi(e) != 1) {
    atomic_store(&G->on, 0);
    return;
  }
  void *h = dlopen("libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  nr_gpu_ldpc_ul_init_fn init = h ? (nr_gpu_ldpc_ul_init_fn)dlsym(h, "nr_pusch_gpu_ldpc_init") : NULL;
  G->run = h ? (nr_gpu_ldpc_run_fn)dlsym(h, "nr_pusch_gpu_ldpc_run") : NULL;
  ul_stats_fn = h ? (nr_gpu_ldpc_ul_stats_fn)dlsym(h, "nr_pusch_gpu_ldpc_stats") : NULL;
  const char *pc = getenv("ISAC_UL_GPU_LDPC_PCT"), *dl = getenv("ISAC_UL_GPU_LDPC_DEADLINE_US");
  if (!init || !G->run || !ul_stats_fn || init(pc ? atoi(pc) : 15, dl ? atoi(dl) : 4000) != 0) {
    LOG_W(PHY, "SENSING: UL_GPU_LDPC requested but unavailable -- CPU LDPC decoder\n");
    atomic_store(&G->on, 0);
    return;
  }
  G->min_bits = getenv("ISAC_UL_GPU_LDPC_MIN_BITS") ? (uint32_t)atoi(getenv("ISAC_UL_GPU_LDPC_MIN_BITS")) : 0;
  G->check = getenv("ISAC_UL_GPU_LDPC_CHECK") ? atoi(getenv("ISAC_UL_GPU_LDPC_CHECK")) : 64;
  LOG_A(PHY, "SENSING: UL_GPU_LDPC enabled: bit-exact GPU decoder for PUSCH TBs, check=%d\n", G->check);
  atomic_store(&G->on, 1);
}

int nr_pusch_gpu_ldpc_enabled(void)
{
  if (atomic_load_explicit(&ULG.on, memory_order_relaxed) < 0)
    pthread_once(&ULG.once, ul_load);
  return atomic_load_explicit(&ULG.on, memory_order_relaxed) == 1;
}

static int32_t ul_decode(nrLDPC_slot_decoding_parameters_t *p)
{
  if (!nr_pusch_gpu_ldpc_enabled())
    return ul_cpu_dec(p);
  return decode_impl(&ULG, p, ul_cpu_dec);
}

nrLDPC_coding_decoder_t *nr_pusch_gpu_ldpc_wrap(nrLDPC_coding_decoder_t *cpu)
{
  if (!cpu || !nr_pusch_gpu_ldpc_enabled())
    return cpu;
  ul_cpu_dec = cpu;
  return ul_decode;
}

void nr_pusch_gpu_ldpc_report(char *buf, size_t n)
{
  uint64_t o[12] = {0};
  if (ul_stats_fn)
    ul_stats_fn(o);
  gl_t *G = &ULG;
  snprintf(buf, n,
           "on=%d tbs=%lu cbs=%lu gpu_calls=%lu gpu_ms=%lu max_us=%lu busy=%lu over_cap=%lu rt_defer=%lu late=%lu deadline=%lu err=%lu "
           "reclaimed=%lu part=%lu | cpu_refused=%lu small=%lu syn0=%lu run_fail=%lu check_ok=%lu check_bad=%lu",
           atomic_load(&G->on) == 1, (unsigned long)atomic_load(&G->calls), (unsigned long)atomic_load(&G->cbs), (unsigned long)o[0],
           (unsigned long)(o[2] / 1000), (unsigned long)o[3], (unsigned long)o[4], (unsigned long)o[5], (unsigned long)o[6],
           (unsigned long)o[7], (unsigned long)o[8], (unsigned long)o[9], (unsigned long)o[10], (unsigned long)o[11],
           (unsigned long)atomic_load(&G->fb_refuse), (unsigned long)atomic_load(&G->fb_small), (unsigned long)atomic_load(&G->fb_syn),
           (unsigned long)atomic_load(&G->fb_err), (unsigned long)atomic_load(&G->chk_ok), (unsigned long)atomic_load(&G->chk_bad));
}
