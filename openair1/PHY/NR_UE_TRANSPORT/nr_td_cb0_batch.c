/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
/*! \file nr_td_cb0_batch.c
 * \brief Batched CB0 decode of many Technique D hypotheses per grant: API, CPU reference, LDPC adapter.
 * See nr_td_cb0_batch.h. Every step below names the receiver code it reproduces; the equivalence is enforced by
 * test_nr_td_cb0_batch (CpuReferenceMatchesFullDecodeCb0 runs the receiver's own nrLDPC_coding_decoder).
 */
#define _GNU_SOURCE
#include "nr_td_cb0_batch.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "PHY/sse_intrin.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_defs.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_segment/nr_rate_matching.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "common/utils/threadPool/thread-pool.h"

uint8_t get_BG(uint32_t A, uint16_t R); /* openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.c */

/* TS 38.212 Table 5.4.2.1-2 numerators, as nr_rate_matching.c's index_k0 (static there). */
static const uint8_t cb0_index_k0[2][4] = {{0, 17, 33, 56}, {0, 13, 25, 43}};

#define CB0_LDPC_OUT_BYTES 27000 /* OAI_LDPC_DECODER_MAX_NUM_LLR of nrLDPC_coding_segment_decoder.c */

static LDPC_decoderfunc_t *g_dec;
static int g_threads = 1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
/* The CRC tables (crc_byte.c) are zero until crcTableInit(): on aarch64 CRC16 / CRC24A / CRC24B then return 0 for
 * every input and the decoder's CRC early stop accepts all-zero-CRC words. nr-uesoftmodem initialises them in
 * nr_init_ue.c; a standalone caller (tests, benchmarks, a GPU worker started first) may not, so do it once here.
 * Idempotent: it rewrites the same constants. */
static pthread_once_t g_crc_once = PTHREAD_ONCE_INIT;
static const nr_td_cb0_gpu_api_t *g_gpu; /* loaded module, NULL = not loaded */
static int g_gpu_on = -1;                /* -1: decide from NR_GPU_CB0 at the first batch */

void nr_td_cb0_set_ldpc_decoder(void *ldpc_decoder_fn)
{
  pthread_mutex_lock(&g_lock);
  g_dec = (LDPC_decoderfunc_t *)ldpc_decoder_fn;
  pthread_mutex_unlock(&g_lock);
}

void nr_td_cb0_set_threads(int n)
{
  pthread_mutex_lock(&g_lock);
  g_threads = n < 1 ? 1 : (n > 64 ? 64 : n);
  pthread_mutex_unlock(&g_lock);
}

static const nr_td_cb0_gpu_api_t *gpu_load(void)
{
  static int tried;
  static const nr_td_cb0_gpu_api_t *api;
  if (tried)
    return api;
  tried = 1;
  void *h = dlopen("libtd_cb0_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    h = dlopen("./libtd_cb0_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    return NULL;
  const nr_td_cb0_gpu_api_t *(*get)(void) = (const nr_td_cb0_gpu_api_t * (*)(void)) dlsym(h, "nr_td_cb0_gpu_api");
  const nr_td_cb0_gpu_api_t *a = get ? get() : NULL;
  if (a && a->abi == NR_TD_CB0_GPU_ABI)
    api = a;
  return api;
}

static int use_gpu_locked(int on)
{
  g_gpu_on = on ? 1 : 0;
  g_gpu = on ? gpu_load() : NULL;
  if (!g_gpu)
    g_gpu_on = 0;
  return g_gpu != NULL;
}

int nr_td_cb0_use_gpu(int on)
{
  pthread_mutex_lock(&g_lock);
  const int r = use_gpu_locked(on);
  pthread_mutex_unlock(&g_lock);
  return r;
}

/* ---- metadata: passive_ldpc_decode_core's parameter derivation for segment 0 ---- */
int nr_td_cb0_meta(const nr_td_cb0_item_t *it, nr_td_cb0_meta_t *m)
{
  memset(m, 0, sizeof(*m));
  m->err = NR_TD_CB0_ERR_ARG;
  if (!it->llr || it->G == 0 || !(it->Qm == 2 || it->Qm == 4 || it->Qm == 6 || it->Qm == 8) || it->Nl < 1 || it->Nl > 4
      || it->rv > 3 || it->tbs == 0 || it->max_iter == 0 || it->bg > 2 || (it->bg == 0 && it->R == 0)
      || it->llr_shift > 8)
    return m->err;
  m->BG = it->bg ? it->bg : get_BG(it->tbs, it->R); /* cw->ldpcBaseGraph = get_BG(cw->TBS, cw->targetCodeRate) */
  m->A = it->tbs;
  m->Qm = it->Qm;
  m->rv = it->rv;
  m->max_iter = it->max_iter;
  m->tbslbrm = it->tbslbrm;
  m->shift = it->llr_shift;
  m->err = NR_TD_CB0_ERR_SEG;
  unsigned int C, K, Z, F;
  if (nr_segmentation(NULL, NULL, lenWithCrc(1, m->A), &C, &K, &Z, &F, m->BG) < 0 || C == 0 || C > 255)
    return m->err;
  m->C = C;
  m->K = K;
  m->Z = Z;
  m->F = F;
  m->err = NR_TD_CB0_ERR_E;
  const int E = nr_get_E(it->G, (uint8_t)C, it->Qm, it->Nl, 0);
  if (E <= 0)
    return m->err;
  m->E = E;
  /* nr_rate_matching_ldpc_rx's circular buffer */
  m->Kc = m->BG == 2 ? 52 : 68;
  m->N = (m->BG == 1) ? 66 * Z : 50 * Z;
  m->Ncb = m->N;
  if (m->tbslbrm) {
    const uint32_t Nref = 3 * m->tbslbrm / (2 * C);
    if (Nref < m->Ncb)
      m->Ncb = Nref;
  }
  m->k0 = (cb0_index_k0[m->BG - 1][m->rv] * m->Ncb / m->N) * Z;
  m->Foffset = K - F - 2 * Z;
  m->err = NR_TD_CB0_ERR_RM;
  if (m->Foffset > m->Ncb) /* nr_rate_matching_ldpc_rx returns -1 */
    return m->err;
  const uint32_t L = m->Foffset + (m->Ncb > m->Foffset + F ? m->Ncb - m->Foffset - F : 0);
  if (L == 0) /* no writable position: the RX loop would never terminate */
    return m->err;
  int llrLen;
  m->R_dec = (uint8_t)nr_get_R_ldpc_decoder(m->rv, E, m->BG, Z, &llrLen, 0);
  m->crc_type = (uint8_t)crcType(C, m->A);
  m->Kprime_crc = lenWithCrc(C, m->A);
  m->err = NR_TD_CB0_OK;
  m->valid = 1;
  return 0;
}

/* ---- CPU reference dematch: nr_process_decode_segment up to the decoder input ---- */
typedef struct {
  int16_t *e;  /* E max of the job */
  int16_t *sh; /* shifted LLRs, E max */
  int16_t *d; /* NR_TD_CB0_D_STRIDE */
  int16_t *z; /* 68 * 384 + 16 */
  int8_t *l;  /* NR_TD_CB0_L_STRIDE */
} cb0_scratch_t;

static int scratch_alloc(cb0_scratch_t *s, uint32_t Emax)
{
  s->e = aligned_alloc(64, ((size_t)Emax * sizeof(int16_t) + 63) & ~(size_t)63);
  s->sh = aligned_alloc(64, ((size_t)Emax * sizeof(int16_t) + 63) & ~(size_t)63);
  s->d = aligned_alloc(64, NR_TD_CB0_D_STRIDE * sizeof(int16_t));
  s->z = aligned_alloc(64, (68 * 384 + 64) * sizeof(int16_t));
  s->l = aligned_alloc(64, NR_TD_CB0_L_STRIDE);
  return (s->e && s->sh && s->d && s->z && s->l) ? 0 : -1;
}

static void scratch_free(cb0_scratch_t *s)
{
  free(s->e);
  free(s->sh);
  free(s->d);
  free(s->z);
  free(s->l);
}

/* l: NR_TD_CB0_L_STRIDE bytes (Kc*Z valid, the rest 0); d_out: N values (may be NULL). */
static void cpu_dematch_one(const nr_td_cb0_item_t *it, const nr_td_cb0_meta_t *m, cb0_scratch_t *s, int8_t *l,
                            int16_t *d_out)
{
  const uint32_t E = m->E, K = m->K, Z = m->Z, F = m->F, Kc = m->Kc, Kprime = K - F;
  int16_t *d = s->d, *z = s->z;
  const int16_t *in = it->llr;
  if (m->shift) { /* K38 k_h: exactly the full decode's llr[i] >> k, on the E LLRs code block 0 reads */
    for (uint32_t i = 0; i < E; i++)
      s->sh[i] = (int16_t)(it->llr[i] >> m->shift);
    in = s->sh;
  }
  nr_deinterleaving_ldpc(E, m->Qm, s->e, (int16_t *)in);
  nr_rate_matching_ldpc_rx(m->tbslbrm, m->BG, Z, d, s->e, m->C, m->rv, 1 /* clear: new transmission */, E, F,
                           K - F - 2 * Z);
  memset(z, 0, 2 * Z * sizeof(*z));                          /* first 2*Z punctured bits */
  memset(z + Kprime, 127, F * sizeof(*z));                   /* filler bits (0x7F7F, saturates to 127) */
  memcpy(z + 2 * Z, d, (Kprime - 2 * Z) * sizeof(*z));       /* coded bits before the filler */
  memcpy(z + K, d + (K - 2 * Z), (Kc * Z - K) * sizeof(*z)); /* skip the filler */
  memset(z + Kc * Z, 0, 16 * sizeof(*z)); /* the segment decoder packs one block past Kc*Z from stack garbage */
  simde__m128i *pv = (simde__m128i *)z, *pl = (simde__m128i *)l;
  int j = 0;
  for (int i = 0; j < (int)((Kc * Z) >> 4) + 1; i += 2, j++)
    pl[j] = simde_mm_packs_epi16(pv[i], pv[i + 1]);
  memset(l + 16 * j, 0, NR_TD_CB0_L_STRIDE - 16 * j);
  if (d_out)
    memcpy(d_out, d, m->N * sizeof(*d));
}

/* The receiver's all-zero guards on a converged CB0 (bits = decoded K/8 bytes). */
static int cb0_guard(const nr_td_cb0_meta_t *m, const uint8_t *out, int pass)
{
  if (pass && m->C > 1) {
    /* the probe's all-zero guard on segment 0 (passive_ldpc_decode_core, t_probe_first_seg) */
    const uint32_t seg_bytes = (m->K >> 3) - (m->F >> 3) - 3;
    uint32_t i = 0;
    while (i < seg_bytes && out[i] == 0)
      i++;
    if (i == seg_bytes)
      pass = 0;
  } else if (pass) {
    /* C == 1: segment 0 is the TB; the full path's all-zero-payload guard */
    const uint32_t sz = m->A / 8;
    if (out[sz] == 0 && out[sz + 1] == 0) {
      uint32_t i = 0;
      while (i < sz && out[i] == 0)
        i++;
      if (i == sz)
        pass = 0;
    }
  }
  return pass;
}

/* ---- CPU layered LDPC decode of one dematched CB0, as nr_process_decode_segment + the probe guards ---- */
static void cpu_decode_one(LDPC_decoderfunc_t *dec, const nr_td_cb0_meta_t *m, const int8_t *l, nr_td_cb0_result_t *r)
{
  t_nrLDPC_dec_params p = {.check_crc = check_crc};
  p.BG = m->BG;
  p.Z = m->Z;
  p.numMaxIter = m->max_iter;
  p.outMode = nrLDPC_outMode_BIT;
  p.R = m->R_dec;
  p.crc_type = m->crc_type;
  p.Kprime = m->Kprime_crc;
  decode_abort_t ab;
  init_abort(&ab);
  t_nrLDPC_time_stats ts = {0};
  uint8_t out[CB0_LDPC_OUT_BYTES] __attribute__((aligned(32)));
  const int it = dec(&p, (int8_t *)l, out, &ts, &ab);
  pthread_mutex_destroy(&ab.mutex_failure);
  const int pass = cb0_guard(m, out, it < m->max_iter); /* nr_process_decode_segment: decodeIterations < numMaxIter */
  r->pass = (int8_t)pass;
  r->iters = (uint8_t)(it > 255 ? 255 : (it < 0 ? 0 : it));
  r->decoder_used = NR_TD_CB0_DEC_CPU_LAYERED;
}

/* ---- worker pool for one batch ---- */
typedef struct {
  const nr_td_cb0_item_t *items; /* NULL: every valid item has l */
  const nr_td_cb0_meta_t *meta;
  const int8_t *l;      /* n * NR_TD_CB0_L_STRIDE, or NULL */
  const int8_t *have_l; /* per item: l holds its dematched CB0 (NULL = all) */
  nr_td_cb0_result_t *out;
  LDPC_decoderfunc_t *dec;
  int n;
  uint32_t Emax;
  atomic_int next;
} cb0_job_t;

static void *cb0_worker(void *arg)
{
  cb0_job_t *j = arg;
  cb0_scratch_t s = {0};
  const bool need_cpu = j->items != NULL;
  if (need_cpu && scratch_alloc(&s, j->Emax) != 0) {
    scratch_free(&s);
    return NULL; /* items this worker would have taken stay with the others */
  }
  for (int i; (i = atomic_fetch_add(&j->next, 1)) < j->n;) {
    if (!j->meta[i].valid)
      continue;
    const bool gpu = j->l && (!j->have_l || j->have_l[i]);
    const int8_t *li;
    if (gpu) {
      li = j->l + (size_t)i * NR_TD_CB0_L_STRIDE;
    } else if (need_cpu) {
      cpu_dematch_one(&j->items[i], &j->meta[i], &s, s.l, NULL);
      li = s.l;
    } else {
      continue;
    }
    cpu_decode_one(j->dec, &j->meta[i], li, &j->out[i]);
    j->out[i].dematch_gpu = gpu;
  }
  scratch_free(&s);
  return NULL;
}

static void cb0_run(cb0_job_t *j, int threads)
{
  atomic_init(&j->next, 0);
  int T = threads < j->n ? threads : j->n;
  pthread_t th[64];
  int started = 0;
  for (int t = 1; t < T; t++)
    if (pthread_create(&th[started], NULL, cb0_worker, j) == 0)
      started++;
  cb0_worker(j);
  for (int t = 0; t < started; t++)
    pthread_join(th[t], NULL);
}

/* LDPC adapter. Today: the CPU layered decoder (libldpc.so's LDPCdecoder), the same decoder and iteration policy
 * as the receiver's full decode, so a CB0 FAIL is admissible elimination evidence under levers spec section 9.3.
 *
 * CUDA: wired (round 2) through G1's public TB entry -- see cb0_cuda_worker / nr_td_cb0_use_cuda_ldpc. That path
 * re-does the de-matching on the CPU inside the plugin, and G1's worker launches once per (BG, Z, iterations) group.
 * A zero-copy path (GPU dematch straight into the pool's pinned slots, one launch for mixed Z) needs a new entry in
 * libldpc_cuda.so (nrLDPC_cuda/*, owned by G1): proposed in the task-BATCH report, not done here.
 * The CUDA decoder (normalised flooding min-sum, 2x iterations) is NOT the CPU layered decoder: its CB0 outcomes may
 * feed the elimination channel only under the section 9.3 same-decoder / dominance rule. Results say decoder_used
 * per item; never mix them within one hypothesis's evidence. */
void cb0_ldpc_decode_batch(const nr_td_cb0_meta_t *meta, const int8_t *l, int n, nr_td_cb0_result_t *out)
{
  LDPC_decoderfunc_t *dec = g_dec;
  if (!dec || !l || n <= 0) {
    for (int i = 0; i < n; i++)
      if (meta[i].valid) {
        out[i].pass = -1;
        out[i].err = NR_TD_CB0_ERR_DECODER;
      }
    return;
  }
  pthread_once(&g_crc_once, crcTableInit);
  cb0_job_t j = {.items = NULL, .meta = meta, .l = l, .have_l = NULL, .out = out, .dec = dec, .n = n};
  cb0_run(&j, g_threads);
}

/* ---- GPU dematch of the valid, device-readable items; have_l[i] = 1 for those done ---- */
static int gpu_dematch(const nr_td_cb0_gpu_api_t *api, const nr_td_cb0_item_t *items, const nr_td_cb0_meta_t *meta,
                       int n, int8_t *l, int16_t *d, int8_t *have_l)
{
  nr_td_cb0_gpu_item_t *g = calloc(n, sizeof(*g));
  if (!g)
    return -1;
  int any = 0;
  for (int i = 0; i < n; i++) {
    have_l[i] = 0;
    if (!meta[i].valid || !api->ptr_ok(items[i].llr))
      continue; /* E = 0: the kernel skips the item */
    const nr_td_cb0_meta_t *m = &meta[i];
    g[i] = (nr_td_cb0_gpu_item_t){.llr = items[i].llr, .E = m->E, .Qm = m->Qm, .Kc = m->Kc, .Z = m->Z, .K = m->K,
                                  .F = m->F, .Ncb = m->Ncb, .k0 = m->k0, .Foffset = m->Foffset, .N = m->N,
                                  .shift = m->shift};
    have_l[i] = 1;
    any = 1;
  }
  int rc = any ? api->dematch(g, n, l, d) : 0;
  if (rc)
    memset(have_l, 0, n);
  free(g);
  return rc;
}

int nr_td_cb0_dematch(const nr_td_cb0_item_t *items, int n, int use_gpu, int8_t *l, int16_t *d, int8_t *status)
{
  if (n <= 0)
    return 0;
  nr_td_cb0_meta_t *meta = calloc(n, sizeof(*meta));
  int8_t *have = calloc(n, 1);
  if (!meta || !have) {
    free(meta);
    free(have);
    return -1;
  }
  uint32_t Emax = 1;
  for (int i = 0; i < n; i++) {
    nr_td_cb0_meta(&items[i], &meta[i]);
    if (meta[i].valid && meta[i].E > Emax)
      Emax = meta[i].E;
  }
  pthread_mutex_lock(&g_lock);
  int rc = 0;
  if (use_gpu) {
    const nr_td_cb0_gpu_api_t *api = gpu_load();
    if (!api) {
      rc = -1;
    } else if (api->pageable_ok()) {
      rc = gpu_dematch(api, items, meta, n, l, d, have);
    } else { /* through the module's managed scratch */
      int8_t *ls = api->scratch(0, (size_t)n * NR_TD_CB0_L_STRIDE);
      int16_t *ds = d ? api->scratch(1, (size_t)n * NR_TD_CB0_D_STRIDE * sizeof(int16_t)) : NULL;
      rc = (!ls || (d && !ds)) ? -1 : gpu_dematch(api, items, meta, n, ls, ds, have);
      if (rc == 0) {
        memcpy(l, ls, (size_t)n * NR_TD_CB0_L_STRIDE);
        if (d)
          memcpy(d, ds, (size_t)n * NR_TD_CB0_D_STRIDE * sizeof(int16_t));
      }
    }
  }
  pthread_mutex_unlock(&g_lock);
  if (rc == 0) {
    cb0_scratch_t s = {0};
    if (scratch_alloc(&s, Emax) != 0)
      rc = -1;
    for (int i = 0; rc == 0 && i < n; i++) {
      status[i] = meta[i].valid ? have[i] : -1;
      if (meta[i].valid && !have[i])
        cpu_dematch_one(&items[i], &meta[i], &s, l + (size_t)i * NR_TD_CB0_L_STRIDE,
                        d ? d + (size_t)i * NR_TD_CB0_D_STRIDE : NULL);
    }
    scratch_free(&s);
  }
  free(meta);
  free(have);
  return rc;
}

/* ---- counters ---- */
static _Atomic uint64_t st_items, st_decoded, st_cuda_chunks, st_cuda_errors, st_cuda_fb;

void nr_td_cb0_get_stats(nr_td_cb0_stats_t *s)
{
  s->items = atomic_load(&st_items);
  s->decoded = atomic_load(&st_decoded);
  s->cuda_chunks = atomic_load(&st_cuda_chunks);
  s->cuda_errors = atomic_load(&st_cuda_errors);
  s->cuda_fallback_items = atomic_load(&st_cuda_fb);
}

/* ---- CUDA LDPC through libldpc_cuda.so's public TB entry (G1's worker queue / pool / breaker / fallback) ---- */
static int32_t (*g_cuda_dec)(nrLDPC_slot_decoding_parameters_t *);
static int g_cuda_on = -1; /* -1: decide from NR_TD_CB0_CUDA_LDPC at the first batch */
static tpool_t g_inline_pool; /* no worker threads: the plugin's per-segment prep runs inline in our worker */

static int cuda_load(void)
{
  static int tried, ok;
  if (tried)
    return ok;
  tried = 1;
  void *h = dlopen("libldpc_cuda.so", RTLD_NOW | RTLD_NOLOAD); /* already the receiver's coding library */
  /* RTLD_DEEPBIND: the plugin's own symbols first. A host that also defines nrLDPC_coding_decoder_impl (a test linking
   * the segment decoder) would otherwise interpose the CPU decoder under the plugin's entry point. */
  if (!h)
    h = dlopen("libldpc_cuda.so", RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
  if (!h)
    h = dlopen("./libldpc_cuda.so", RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
  if (!h)
    return 0;
  int32_t (*init)(void) = (int32_t(*)(void))dlsym(h, "nrLDPC_coding_init");
  g_cuda_dec = (int32_t(*)(nrLDPC_slot_decoding_parameters_t *))dlsym(h, "nrLDPC_coding_decoder");
  if (!init || !g_cuda_dec || init() != 0) {
    g_cuda_dec = NULL;
    return 0;
  }
  char p[] = "n";
  initTpool(p, &g_inline_pool, false);
  ok = 1;
  return ok;
}

static int use_cuda_locked(int on)
{
  g_cuda_on = (on && cuda_load()) ? 1 : 0;
  return g_cuda_on;
}

int nr_td_cb0_use_cuda_ldpc(int on)
{
  pthread_mutex_lock(&g_lock);
  const int r = use_cuda_locked(on);
  pthread_mutex_unlock(&g_lock);
  return r;
}

#define CB0_CUDA_TBS 64 /* nrLDPC_coding_decoder takes at most 64 TBs per call */
typedef struct {
  const nr_td_cb0_item_t *items;
  const nr_td_cb0_meta_t *meta;
  nr_td_cb0_result_t *out;
  int n;
  uint32_t Emax;
  atomic_int next;
} cb0_cuda_job_t;

static void *cb0_cuda_worker(void *arg)
{
  cb0_cuda_job_t *j = arg;
  nrLDPC_TB_decoding_parameters_t *tb = calloc(CB0_CUDA_TBS, sizeof(*tb));
  int16_t *d = malloc((size_t)CB0_CUDA_TBS * 68 * 384 * sizeof(int16_t));
  uint8_t *c = malloc((size_t)CB0_CUDA_TBS * 1056 + 64);
  int16_t *sh = malloc((size_t)CB0_CUDA_TBS * j->Emax * sizeof(int16_t));
  decode_abort_t ab[CB0_CUDA_TBS];
  uint32_t processed[CB0_CUDA_TBS];
  if (!tb || !d || !c || !sh)
    goto done; /* its items stay undecoded: the caller marks them inconclusive */
  for (int t = 0; t < CB0_CUDA_TBS; t++)
    init_abort(&ab[t]);
  for (int g; (g = atomic_fetch_add(&j->next, CB0_CUDA_TBS)) < j->n;) {
    const int cnt = j->n - g < CB0_CUDA_TBS ? j->n - g : CB0_CUDA_TBS;
    for (int t = 0; t < cnt; t++) {
      const nr_td_cb0_item_t *it = &j->items[g + t];
      const nr_td_cb0_meta_t *m = &j->meta[g + t];
      nrLDPC_TB_decoding_parameters_t *b = &tb[t];
      memset(b, 0, sizeof(*b));
      const int16_t *llr = it->llr;
      if (m->shift) { /* K38 k_h on the E LLRs CB0 reads, as the full decode */
        int16_t *q = sh + (size_t)t * j->Emax;
        for (uint32_t i = 0; i < m->E; i++)
          q[i] = (int16_t)(it->llr[i] >> m->shift);
        llr = q;
      }
      /* passive_ldpc_decode_core's TB parameters, probe form (nb_segments_to_decode = 1 when C > 1) */
      b->harq_unique_pid = 0;
      b->processedSegments = &processed[t];
      b->G = it->G;
      b->Qm = m->Qm;
      b->nb_layers = it->Nl;
      b->BG = m->BG;
      b->rv_index = m->rv;
      b->max_ldpc_iterations = m->max_iter;
      set_abort(&ab[t], false);
      b->abort_decode = &ab[t];
      b->tbslbrm = m->tbslbrm;
      b->A = m->A;
      b->K = m->K;
      b->Z = m->Z;
      b->F = m->F;
      b->C = m->C;
      b->nb_segments_to_decode = m->C > 1 ? 1 : 0;
      b->E = b->E2 = (int)m->E;
      b->R = b->R2 = m->R_dec;
      b->first_rE2 = (int)m->C;
      b->llr = (short *)llr;
      b->c = c + (size_t)t * 1056;
      b->d = d + (size_t)t * 68 * 384;
      b->d_to_be_cleared = true;
    }
    nrLDPC_slot_decoding_parameters_t slot = {.nb_TBs = cnt, .threadPool = &g_inline_pool, .TBs = tb};
    const int32_t rc = g_cuda_dec(&slot);
    atomic_fetch_add(&st_cuda_chunks, 1);
    for (int t = 0; t < cnt; t++) {
      nr_td_cb0_result_t *r = &j->out[g + t];
      if (rc != 0) {
        r->pass = -1;
        r->err = NR_TD_CB0_ERR_GPU;
        r->decoder_used = NR_TD_CB0_DEC_CUDA_FLOODING;
        atomic_fetch_add(&st_cuda_errors, 1);
        continue;
      }
      const int cuda = tb[t].decoder_used == NRLDPC_DECODER_CUDA_FLOODING;
      if (!cuda)
        atomic_fetch_add(&st_cuda_fb, 1);
      r->decoder_used = cuda ? NR_TD_CB0_DEC_CUDA_FLOODING : NR_TD_CB0_DEC_CPU_LAYERED;
      r->pass = (int8_t)cb0_guard(&j->meta[g + t], tb[t].c, tb[t].decodeSuccess[0]);
      r->iters = 0; /* the TB entry does not report iterations */
    }
  }
  for (int t = 0; t < CB0_CUDA_TBS; t++)
    pthread_mutex_destroy(&ab[t].mutex_failure);
done:
  free(tb);
  free(d);
  free(c);
  free(sh);
  return NULL;
}

/* ---- exact per-batch deduplication: everything the CB0 verdict depends on ---- */
typedef struct {
  const int16_t *llr;
  uint32_t shift, BG, Z, K, F, E, Qm, rv, Ncb, max_iter, crc_type, Kprime_crc;
} cb0_key_t;

static int key_cmp(const void *a, const void *b, void *keys)
{
  const cb0_key_t *k = keys;
  const int r = memcmp(&k[*(const int *)a], &k[*(const int *)b], sizeof(cb0_key_t));
  return r ? r : (*(const int *)a - *(const int *)b); /* stable: the first item of a group is its representative */
}

int nr_td_cb0_batch(const nr_td_cb0_item_t *items, int n, nr_td_cb0_result_t *out)
{
  if (n <= 0)
    return 0;
  if (!items || !out)
    return -1;
  pthread_once(&g_crc_once, crcTableInit);
  nr_td_cb0_meta_t *meta = calloc(n, sizeof(*meta));
  cb0_key_t *key = calloc(n, sizeof(*key));
  int *ord = calloc(n, sizeof(int)), *rep = calloc(n, sizeof(int));
  nr_td_cb0_item_t *ritem = calloc(n, sizeof(*ritem));
  nr_td_cb0_meta_t *rmeta = calloc(n, sizeof(*rmeta));
  nr_td_cb0_result_t *rout = calloc(n, sizeof(*rout));
  int8_t *have = calloc(n, 1);
  if (!meta || !key || !ord || !rep || !ritem || !rmeta || !rout || !have) {
    for (int i = 0; i < n; i++)
      out[i] = (nr_td_cb0_result_t){.pass = -1, .err = NR_TD_CB0_ERR_ARG};
    goto out_free;
  }
  int nv = 0;
  for (int i = 0; i < n; i++) {
    out[i] = (nr_td_cb0_result_t){0};
    rep[i] = -1;
    if (nr_td_cb0_meta(&items[i], &meta[i]) != 0) {
      out[i].pass = -1;
      out[i].err = meta[i].err;
      continue;
    }
    out[i].C = (uint16_t)meta[i].C;
    out[i].tb_result = meta[i].C == 1;
    const nr_td_cb0_meta_t *m = &meta[i];
    cb0_key_t *k = &key[i]; /* field by field after the memset: memcmp must not see padding */
    memset(k, 0, sizeof(*k));
    k->llr = items[i].llr;
    k->shift = m->shift;
    k->BG = m->BG;
    k->Z = m->Z;
    k->K = m->K;
    k->F = m->F;
    k->E = m->E;
    k->Qm = m->Qm;
    k->rv = m->rv;
    k->Ncb = m->Ncb;
    k->max_iter = m->max_iter;
    k->crc_type = m->crc_type;
    k->Kprime_crc = m->Kprime_crc;
    ord[nv++] = i;
  }
  qsort_r(ord, nv, sizeof(int), key_cmp, key);
  int nr = 0;
  uint32_t Emax = 1;
  for (int a = 0; a < nv; a++) {
    const int i = ord[a];
    if (a > 0 && memcmp(&key[i], &key[ord[a - 1]], sizeof(cb0_key_t)) == 0) {
      rep[i] = rep[ord[a - 1]];
      out[i].dedup = 1;
      continue;
    }
    rep[i] = nr;
    ritem[nr] = items[i];
    rmeta[nr] = meta[i];
    if (meta[i].E > Emax)
      Emax = meta[i].E;
    nr++;
  }
  atomic_fetch_add(&st_items, n);
  atomic_fetch_add(&st_decoded, nr);

  pthread_mutex_lock(&g_lock);
  if (g_gpu_on < 0) {
    const char *e = getenv("NR_GPU_CB0");
    use_gpu_locked(e && atoi(e) == 1);
  }
  if (g_cuda_on < 0) {
    const char *e = getenv("NR_TD_CB0_CUDA_LDPC");
    use_cuda_locked(e && atoi(e) == 1);
  }
  if (nr > 0 && g_cuda_on == 1) {
    cb0_cuda_job_t j = {.items = ritem, .meta = rmeta, .out = rout, .n = nr, .Emax = Emax};
    atomic_init(&j.next, 0);
    const int T = g_threads < (nr + CB0_CUDA_TBS - 1) / CB0_CUDA_TBS ? g_threads : (nr + CB0_CUDA_TBS - 1) / CB0_CUDA_TBS;
    pthread_t th[64];
    int started = 0;
    for (int t = 1; t < T; t++)
      if (pthread_create(&th[started], NULL, cb0_cuda_worker, &j) == 0)
        started++;
    cb0_cuda_worker(&j);
    for (int t = 0; t < started; t++)
      pthread_join(th[t], NULL);
    /* "any error = the whole batch is inconclusive": one failed CUDA call voids every CUDA verdict of this call */
    int any_err = 0;
    for (int r = 0; r < nr; r++)
      any_err |= rout[r].err == NR_TD_CB0_ERR_GPU;
    for (int r = 0; r < nr; r++)
      if (any_err && rout[r].decoder_used == NR_TD_CB0_DEC_CUDA_FLOODING) {
        rout[r].pass = -1;
        rout[r].err = NR_TD_CB0_ERR_GPU;
      }
  } else if (nr > 0 && !g_dec) {
    for (int r = 0; r < nr; r++)
      rout[r] = (nr_td_cb0_result_t){.pass = -1, .err = NR_TD_CB0_ERR_DECODER};
  } else if (nr > 0) {
    int8_t *l = NULL;
    if (g_gpu_on == 1 && g_gpu) {
      l = g_gpu->scratch(0, (size_t)nr * NR_TD_CB0_L_STRIDE);
      if (l && gpu_dematch(g_gpu, ritem, rmeta, nr, l, NULL, have) != 0)
        l = NULL; /* CUDA error: every item falls back to the CPU dematch */
    }
    cb0_job_t j = {.items = ritem, .meta = rmeta, .l = l, .have_l = have, .out = rout, .dec = g_dec, .n = nr, .Emax = Emax};
    cb0_run(&j, g_threads);
  }
  pthread_mutex_unlock(&g_lock);
  for (int r = 0; r < nr; r++)
    if (rout[r].decoder_used == 0 && rout[r].err == 0) { /* a worker could not allocate its scratch */
      rout[r].pass = -1;
      rout[r].err = NR_TD_CB0_ERR_ARG;
    }
  for (int i = 0; i < n; i++) {
    if (rep[i] < 0)
      continue;
    const nr_td_cb0_result_t *r = &rout[rep[i]];
    out[i].pass = r->pass;
    out[i].iters = r->iters;
    out[i].decoder_used = r->decoder_used;
    out[i].err = r->err;
    out[i].dematch_gpu = r->dematch_gpu;
  }
out_free:;
  int decoded = 0;
  for (int i = 0; i < n; i++)
    decoded += out[i].pass != -1;
  free(meta);
  free(key);
  free(ord);
  free(rep);
  free(ritem);
  free(rmeta);
  free(rout);
  free(have);
  return decoded;
}
