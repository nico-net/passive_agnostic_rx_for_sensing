/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS Task 7, host side of the PUSCH graph pass: see nr_gpu_pass_ul.h and nr_gpu_pass.h. */
#include "nr_gpu_pass_ul.h"
#include "nr_pdsch_gpu_fep.h" /* NR_GPU_PASS_RING */
#include "PHY/CODING/coding_defs.h"
#include "PHY/NR_REFSIG/nr_refsig.h"
#include "PHY/NR_REFSIG/dmrs_nr.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/NR_TRANSPORT/nr_sch_dmrs.h"
#include "PHY/NR_TRANSPORT/nr_ulsch_passive_branch.h"
#include "PHY/NR_TRANSPORT/nr_ulsch_passive_timing.h"
#include "common/utils/nr/nr_common.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr_cell.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "nr_gpu_pass_fp32_gate.h"
#include <time.h>

int nr_get_R_ldpc_decoder(int rvidx, int E, int BG, int Z, int *llrLen, int round);

/* ------------------------------------------------------------------ the per-RNTI hypothesis cache ------------------------------------------------------------------ */
#define UL_HYP_N 64
typedef struct {
  int used;
  uint16_t rnti;
  uint64_t stamp;
  ul_hyp_t h;
} hyp_entry_t;
/* MPCI G3: the cache of one cell. Callers are the cell's bound threads (decode consumers, the bound bg-discovery worker), so every function below reads and writes the cache of
 * nr_cell_idx(): two cells that reuse a C-RNTI never see each other's hypothesis. Cell 0 only (N = 1): exactly the former process-wide cache. */
typedef hyp_entry_t hyp_arr_t[UL_HYP_N];
NR_CELL_STATIC(hyp_arr_t, g_hyp)
#define g_hyp NR_CELL_REF(g_hyp)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_hyp_lock, PTHREAD_MUTEX_INITIALIZER)
#define g_hyp_lock NR_CELL_REF(g_hyp_lock)
NR_CELL_STATIC(uint64_t, g_hyp_clock)
#define g_hyp_clock NR_CELL_REF(g_hyp_clock)
NR_CELL_STATIC(_Atomic uint64_t, g_hyp_hit)
#define g_hyp_hit NR_CELL_REF(g_hyp_hit)
NR_CELL_STATIC(_Atomic uint64_t, g_hyp_miss)
#define g_hyp_miss NR_CELL_REF(g_hyp_miss)
NR_CELL_STATIC(_Atomic uint64_t, g_hyp_stale)
#define g_hyp_stale NR_CELL_REF(g_hyp_stale)
NR_CELL_STATIC(_Atomic uint64_t, g_hyp_put)
#define g_hyp_put NR_CELL_REF(g_hyp_put)

int ul_hyp_cache_get(uint16_t rnti, ul_hyp_t *h)
{
  int found = 0;
  pthread_mutex_lock(&g_hyp_lock);
  for (int i = 0; i < UL_HYP_N; i++)
    if (g_hyp[i].used && g_hyp[i].rnti == rnti) {
      if (h)
        *h = g_hyp[i].h;
      found = 1;
      break;
    }
  pthread_mutex_unlock(&g_hyp_lock);
  return found;
}

void ul_hyp_cache_put(uint16_t rnti, const ul_hyp_t *h)
{
  if (!h)
    return;
  pthread_mutex_lock(&g_hyp_lock);
  hyp_entry_t *e = NULL, *lru = &g_hyp[0];
  for (int i = 0; i < UL_HYP_N && !e; i++) {
    if (g_hyp[i].used && g_hyp[i].rnti == rnti)
      e = &g_hyp[i];
    else if (!g_hyp[i].used || (lru->used && g_hyp[i].stamp < lru->stamp))
      lru = &g_hyp[i];
  }
  if (!e)
    e = lru;
  e->used = 1;
  e->rnti = rnti;
  e->h = *h;
  e->stamp = ++g_hyp_clock;
  pthread_mutex_unlock(&g_hyp_lock);
  atomic_fetch_add_explicit(&g_hyp_put, 1, memory_order_relaxed);
}

int ul_hyp_cache_put_if_new(uint16_t rnti, const ul_hyp_t *h)
{
  pthread_mutex_lock(&g_hyp_lock);
  for (int i = 0; i < UL_HYP_N; i++)
    if (g_hyp[i].used && g_hyp[i].rnti == rnti && (g_hyp[i].h.n_ok > 0 || g_hyp[i].h.abs_slot >= h->abs_slot)) {
      pthread_mutex_unlock(&g_hyp_lock); /* a decode refreshed the entry meanwhile: it is newer than the search */
      return 0;
    }
  pthread_mutex_unlock(&g_hyp_lock);
  ul_hyp_cache_put(rnti, h); /* shortcut: the check and the put are two lock sections; only the worker calls this, a decode landing between them wins on the next refresh */
  return 1;
}

void ul_hyp_cache_drop(uint16_t rnti)
{
  pthread_mutex_lock(&g_hyp_lock);
  for (int i = 0; i < UL_HYP_N; i++)
    if (g_hyp[i].used && g_hyp[i].rnti == rnti)
      g_hyp[i].used = 0;
  pthread_mutex_unlock(&g_hyp_lock);
}

void ul_hyp_cache_note_fail(uint16_t rnti, int max)
{
  pthread_mutex_lock(&g_hyp_lock);
  for (int i = 0; i < UL_HYP_N; i++)
    if (g_hyp[i].used && g_hyp[i].rnti == rnti) {
      if (++g_hyp[i].h.fail_streak >= max)
        g_hyp[i].used = 0;
      break;
    }
  pthread_mutex_unlock(&g_hyp_lock);
}

int nr_gpu_pass_ul_accept(const nr_gpu_pass_ul_out_t *o, int shadow)
{
  if (o->r.need_cpu && !shadow) {
    nr_gpu_pass_ul_note(NR_GPU_PASS_UL_EV_NEED_CPU);
    return 0;
  }
  return 1;
}

void ul_hyp_cache_clear(void)
{
  pthread_mutex_lock(&g_hyp_lock);
  memset(g_hyp, 0, sizeof(g_hyp));
  g_hyp_clock = 0;
  pthread_mutex_unlock(&g_hyp_lock);
}

int nr_gpu_pass_ul_hyp_lookup(uint16_t rnti, uint32_t epoch, int64_t abs_slot, ul_hyp_t *h)
{
  const int max_age = 2000;
  ul_hyp_t t;
  if (!ul_hyp_cache_get(rnti, &t)) {
    atomic_fetch_add_explicit(&g_hyp_miss, 1, memory_order_relaxed);
    return 0;
  }
  if (t.epoch != epoch || (abs_slot && (abs_slot <= t.abs_slot || abs_slot - t.abs_slot > max_age))) {
    atomic_fetch_add_explicit(&g_hyp_stale, 1, memory_order_relaxed);
    return 0;
  }
  atomic_fetch_add_explicit(&g_hyp_hit, 1, memory_order_relaxed);
  if (h)
    *h = t;
  return 1;
}

void nr_gpu_pass_ul_cache_counts(uint64_t *hit, uint64_t *miss, uint64_t *stale, uint64_t *put)
{
  if (hit) *hit = g_hyp_hit;
  if (miss) *miss = g_hyp_miss;
  if (stale) *stale = g_hyp_stale;
  if (put) *put = g_hyp_put;
}

/* ------------------------------------------------------------------ the pass ------------------------------------------------------------------ */
enum { FB_OFF, FB_TP, FB_HOP, FB_PTRS, FB_SCOPE, FB_RV, FB_CELL, FB_TBPARAM, FB_DMRS, FB_MODULE, FB_BUSY_OR_UNSEEN, FB_NOMEM };
static const char *const fb_name[NR_GPU_PASS_UL_FB_N] = {"off", "transform_precoding", "frequency_hopping", "ptrs", "scope", "rv", "cell", "tbparam", "dmrs", "module",
                                                         "unseen_or_busy", "nomem"};
#define UL_CTX 8
static struct {
  nr_gpu_pass_ul_try_fn try_;
  void (*counters)(uint64_t o[16]);
  _Atomic int on;
  int nbrx, check;
  _Atomic uint64_t fb[NR_GPU_PASS_UL_FB_N], ev[NR_GPU_PASS_UL_EV_N], runs, shadow_n;
} U;
/* MPCI G3: the job, pilot and estimate buffers of the decode contexts of THIS cell (the contexts are numbered per cell: cell 1 ctx 0 is not cell 0 ctx 0). Heap pointers, 8 per cell. */
typedef struct { nr_gpu_pass_job_t *job; int16_t *pil; int16_t *est; } ul_ctx_t;
typedef ul_ctx_t ul_ctx_arr_t[UL_CTX];
NR_CELL_STATIC(ul_ctx_arr_t, g_ulctx)
#define g_ulctx NR_CELL_REF(g_ulctx)
/* cells registered with the pass module for the UL (cell 0 is up with the module: U.on) */
static _Atomic int g_ul_cell_on[NR_CELL_MAX];
static _Atomic int g_ul_cell_nbrx[NR_CELL_MAX]; /* MPCI G3b: RX antennas of the cell's FEP geometry; 0 = those of cell 0 (U.nbrx) */
void nr_gpu_pass_ul_cell_enable(int cell)
{
  if (cell > 0 && cell < NR_CELL_MAX)
    atomic_store(&g_ul_cell_on[cell], 1);
}
void nr_gpu_pass_ul_cell_enable_nb(int cell, int nbrx)
{
  if (cell > 0 && cell < NR_CELL_MAX) {
    atomic_store(&g_ul_cell_nbrx[cell], nbrx);
    atomic_store(&g_ul_cell_on[cell], 1);
  }
}

int nr_gpu_pass_ul_active(void) { return atomic_load_explicit(&U.on, memory_order_relaxed); }

#define UL_SAMP 65536
static uint32_t g_samp[5][UL_SAMP]; /* 0..3: grant_us by kind; 4: the pass call itself (builder + graph + wait) */
static _Atomic uint32_t g_samp_n[5];
static void samp_add(int k, uint32_t us)
{
  const uint32_t i = atomic_fetch_add_explicit(&g_samp_n[k], 1, memory_order_relaxed);
  if (i < UL_SAMP)
    g_samp[k][i] = us;
}
void nr_gpu_pass_ul_grant_us(int kind, uint32_t us)
{
  if (kind >= 0 && kind < 4)
    samp_add(kind, us);
}
static int cmp_u32(const void *a, const void *b)
{
  const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y;
}
static void samp_pct(int k, uint32_t *n, uint32_t *p50, uint32_t *p99, uint32_t *mx)
{
  uint32_t c = atomic_load(&g_samp_n[k]);
  c = c > UL_SAMP ? UL_SAMP : c;
  *n = atomic_load(&g_samp_n[k]);
  *p50 = *p99 = *mx = 0;
  if (!c)
    return;
  uint32_t *t = malloc(c * sizeof(uint32_t)); /* census only: not on the decode path */
  if (!t)
    return;
  memcpy(t, g_samp[k], c * sizeof(uint32_t));
  qsort(t, c, sizeof(uint32_t), cmp_u32);
  *p50 = t[c / 2];
  *p99 = t[(uint32_t)((uint64_t)c * 99 / 100)];
  *mx = t[c - 1];
  free(t);
}

void nr_gpu_pass_ul_note(int ev)
{
  if (ev >= 0 && ev < NR_GPU_PASS_UL_EV_N)
    atomic_fetch_add_explicit(&U.ev[ev], 1, memory_order_relaxed);
}

int nr_gpu_pass_ul_snr_note(int big)
{
  static nr_fp32_gate_t gate;
  if (big)
    nr_gpu_pass_ul_note(NR_GPU_PASS_UL_EV_SNR_BIG);
  if (!nr_fp32_gate_note(&gate, big))
    return 0;
  nr_gpu_pass_ul_note(NR_GPU_PASS_UL_EV_SHADOW_BAD);
  if (atomic_exchange(&U.on, 0))
    LOG_E(PHY, "SENSING: GPUPASS_UL DISABLED: DM-RS snr_db of the pass differs by > 1 dB from the old path on > 1 %% of the CRC-OK shadows\n");
  return 1;
}

int nr_gpu_pass_ul_shadow_due(void)
{
  if (U.check <= 0)
    return 0;
  return (atomic_fetch_add_explicit(&U.shadow_n, 1, memory_order_relaxed) + 1) % (uint64_t)U.check == 0;
}

int nr_gpu_pass_ul_shadow(int mismatch)
{
  nr_gpu_pass_ul_note(NR_GPU_PASS_UL_EV_SHADOW);
  if (!mismatch)
    return 0;
  nr_gpu_pass_ul_note(NR_GPU_PASS_UL_EV_SHADOW_BAD);
  if (atomic_exchange(&U.on, 0))
    LOG_E(PHY, "SENSING: GPUPASS_UL DISABLED: the pass disagrees with the old path -- the old path decodes from now on\n");
  return 1;
}

void nr_gpu_pass_ul_census(void)
{
  if (!U.try_ && !g_hyp_put && !g_hyp_miss)
    return;
  uint64_t o[16] = {0};
  if (U.counters)
    U.counters(o);
  {
    uint32_t n[5], p50[5], p99[5], mx[5];
    for (int k = 0; k < 5; k++)
      samp_pct(k, &n[k], &p50[k], &p99[k], &mx[k]);
    LOG_A(PHY,
          "SENSING: GPUPASS_UL_TIME grant_us[old n=%u p50=%u p99=%u max=%u | pass_ok n=%u p50=%u p99=%u max=%u | pass_fail n=%u p50=%u p99=%u max=%u | shadow n=%u p50=%u p99=%u max=%u] "
          "pass_call_us[n=%u p50=%u p99=%u max=%u]\n",
          n[0], p50[0], p99[0], mx[0], n[1], p50[1], p99[1], mx[1], n[2], p50[2], p99[2], mx[2], n[3], p50[3], p99[3], mx[3], n[4], p50[4], p99[4], mx[4]);
  }
  LOG_A(PHY,
        "SENSING: GPUPASS_UL on=%d runs=%lu launches=%lu graph_hits=%lu module_fb[unseen=%lu busy=%lu unsup=%lu err=%lu ldpc_key=%lu] tb_fail=%lu need_cpu=%lu wait_us_p50<=%lu p99<=%lu classes=%lu | host_fb[off=%lu "
        "tp=%lu hop=%lu ptrs=%lu scope=%lu rv=%lu cell=%lu tbparam=%lu dmrs=%lu module=%lu nomem=%lu] cache[hit=%lu miss=%lu stale=%lu put=%lu] ok=%lu fail=%lu refine=%lu shadow=%lu shadow_bad=%lu llr_inexact=%lu est_diff=%lu (ok: disabling %lu snr_only %lu state3 %lu; fail %lu) snr_big=%lu need_cpu_old=%lu prep_us[p50<=%lu p99<=%lu] run_us[p50<=%lu p99<=%lu] post_us[p50<=%lu p99<=%lu]\n",
        atomic_load(&U.on), (unsigned long)U.runs, (unsigned long)o[0], (unsigned long)o[1], (unsigned long)o[2], (unsigned long)o[3], (unsigned long)o[4], (unsigned long)o[5],
        (unsigned long)o[6], (unsigned long)o[7], (unsigned long)o[8], (unsigned long)o[9], (unsigned long)o[10], (unsigned long)o[11], (unsigned long)U.fb[FB_OFF],
        (unsigned long)U.fb[FB_TP], (unsigned long)U.fb[FB_HOP], (unsigned long)U.fb[FB_PTRS], (unsigned long)U.fb[FB_SCOPE], (unsigned long)U.fb[FB_RV], (unsigned long)U.fb[FB_CELL],
        (unsigned long)U.fb[FB_TBPARAM], (unsigned long)U.fb[FB_DMRS], (unsigned long)U.fb[FB_MODULE], (unsigned long)U.fb[FB_NOMEM], (unsigned long)g_hyp_hit,
        (unsigned long)g_hyp_miss, (unsigned long)g_hyp_stale, (unsigned long)g_hyp_put, (unsigned long)U.ev[NR_GPU_PASS_UL_EV_OK], (unsigned long)U.ev[NR_GPU_PASS_UL_EV_FAIL],
        (unsigned long)U.ev[NR_GPU_PASS_UL_EV_REFINE], (unsigned long)U.ev[NR_GPU_PASS_UL_EV_SHADOW], (unsigned long)U.ev[NR_GPU_PASS_UL_EV_SHADOW_BAD], (unsigned long)U.ev[NR_GPU_PASS_UL_EV_SHADOW_LLR_DIFF],
        (unsigned long)U.ev[NR_GPU_PASS_UL_EV_SHADOW_EST_DIFF], (unsigned long)U.ev[NR_GPU_PASS_UL_EV_EST_OK_DISABLING], (unsigned long)U.ev[NR_GPU_PASS_UL_EV_EST_OK_SNR_ONLY], (unsigned long)U.ev[NR_GPU_PASS_UL_EV_EST_OK_STATE3],
        (unsigned long)U.ev[NR_GPU_PASS_UL_EV_SHADOW_EST_DIFF_FAIL], (unsigned long)U.ev[NR_GPU_PASS_UL_EV_SNR_BIG],
        (unsigned long)U.ev[NR_GPU_PASS_UL_EV_NEED_CPU], (unsigned long)(o[12] & 0xffffffffu), (unsigned long)(o[12] >> 32), (unsigned long)(o[13] & 0xffffffffu),
        (unsigned long)(o[13] >> 32), (unsigned long)(o[14] & 0xffffffffu), (unsigned long)(o[14] >> 32));
}

static int fb(int why)
{
  const uint64_t n = atomic_fetch_add_explicit(&U.fb[why], 1, memory_order_relaxed) + 1;
  if (n == 1)
    LOG_A(PHY, "SENSING: GPUPASS_UL first fallback: %s\n", fb_name[why]);
  return 1;
}

int nr_gpu_pass_ul_requested(void)
{
  return 1; /* Fix A+D: no runtime switch; nr_gpu_pass_ul_active() tells whether the classes are cached */
}

void nr_gpu_pass_ul_prewarm(int ctx)
{
  if (!nr_gpu_pass_ul_requested() || ctx < 0 || ctx >= UL_CTX || g_ulctx[ctx].job)
    return;
  nr_gpu_pass_job_t *jp = (nr_gpu_pass_job_t *)aligned_alloc(64, (sizeof(*jp) + 63) & ~(size_t)63);
  int16_t *pil = (int16_t *)aligned_alloc(64, (size_t)NR_GPU_PASS_MAX_DMRS * 6 * 273 * 4 + 64);
  int16_t *est = (int16_t *)aligned_alloc(64, (size_t)16 * 4096 * 4);
  if (!jp || !pil || !est) {
    free(jp);
    free(pil);
    free(est);
    return;
  }
  memset(jp, 0, sizeof(*jp));
  memset(pil, 0, (size_t)NR_GPU_PASS_MAX_DMRS * 6 * 273 * 4 + 64);
  memset(est, 0, (size_t)16 * 4096 * 4);
  g_ulctx[ctx].job = jp;
  g_ulctx[ctx].pil = pil;
  g_ulctx[ctx].est = est;
}

int nr_gpu_pass_ul_run(const nr_gpu_pass_ul_in_t *in, nr_gpu_pass_ul_out_t *out)
{
  if (!nr_gpu_pass_ul_active())
    return fb(FB_OFF);
  const uint64_t t_entry = nr_gpu_pass_now_ns();
  const int cell = nr_cell_idx(); /* MPCI G3: the consumer thread is bound to its cell; the job carries it to the GPU module */
  atomic_fetch_add_explicit(&U.runs, 1, memory_order_relaxed);
  if (in->transform_precoding)
    return fb(FB_TP);
  if (in->frequency_hopping)
    return fb(FB_HOP);
  if (in->ptrs)
    return fb(FB_PTRS);
  if (in->nr_of_layers != 1 || in->dmrs_config_type != 0 || !in->ncp_normal || in->rv != 0 || in->n_dmrs_cdm_groups_no_data < 1 || in->n_dmrs_cdm_groups_no_data > 2)
    return fb(in->rv != 0 ? FB_RV : FB_SCOPE);
  if (cell != 0 && !atomic_load_explicit(&g_ul_cell_on[cell], memory_order_relaxed))
    return fb(FB_CELL); /* a further cell uses the pass only after nr_gpu_pass_cell_init + nr_gpu_pass_ul_cell_enable */
  if (in->nb_rx != (cell != 0 && atomic_load_explicit(&g_ul_cell_nbrx[cell], memory_order_relaxed) ? atomic_load_explicit(&g_ul_cell_nbrx[cell], memory_order_relaxed) : U.nbrx) || in->N != 4096 || in->symbols_per_slot != 14 || in->num_rb < 1 || in->num_rb > 273 || in->start_symbol < 0 || in->num_symbols < 1 ||
      in->start_symbol + in->num_symbols > 14 || !in->symbol_rot || in->ctx < 0 || in->ctx >= UL_CTX || in->keep >= in->nb_rx)
    return fb(FB_CELL);
  if (in->qam_mod_order != 2 && in->qam_mod_order != 4 && in->qam_mod_order != 6 && in->qam_mod_order != 8)
    return fb(FB_SCOPE);
  nr_gpu_pass_job_t *j = g_ulctx[in->ctx].job;
  int16_t *pilbuf = g_ulctx[in->ctx].pil;
  if (!j || !pilbuf)
    return fb(FB_NOMEM);
  memset(j, 0, offsetof(nr_gpu_pass_job_t, dj));
  memset(&j->ul, 0, sizeof(j->ul));
  j->t_entry_ns = t_entry;
  j->cell = (uint8_t)cell;
  nr_gpu_pass_ul_t *u = &j->ul;
  const int qm = in->qam_mod_order, nrb = in->num_rb, end = in->start_symbol + in->num_symbols;
  const uint16_t range = (uint16_t)(((1u << in->num_symbols) - 1u) << in->start_symbol);
  /* ---- DM-RS symbols of the grant, and the symbol maps */
  const uint16_t dm = in->ul_dmrs_symb_pos & range;
  const int nd = __builtin_popcount((unsigned)dm);
  if (nd < 1 || nd > NR_GPU_PASS_MAX_DMRS || (in->ul_dmrs_symb_pos & ~range & 0x3fffu))
    return fb(FB_DMRS); /* DM-RS outside the grant: the old estimator would read a symbol it never estimated */
  int k = 0;
  for (int m = in->start_symbol; m < end; m++)
    if (dm >> m & 1)
      u->dmrs_sym[k++] = m;
  const int cdm = in->n_dmrs_cdm_groups_no_data;
  const int nb_re_dmrs = 6 * cdm; /* type 1 */
  uint32_t G = 0;
  u->meas_sym = -1;
  for (int m = 0; m < NR_GPU_PASS_UL_NSYM; m++) {
    u->win[m] = -1;
    u->est_sym[m] = 0;
    if (m < in->start_symbol || m >= end)
      continue;
    u->win[m] = in->win[m];
    if (u->win[m] < 0)
      return fb(FB_CELL);
    u->rot[m][0] = in->symbol_rot[2 * m];
    u->rot[m][1] = in->symbol_rot[2 * m + 1];
    u->sfo_dphi[m] = in->sfo_dphi[m];
    u->sfo_kref = in->sfo_kref;
    u->is_dmrs[m] = dm >> m & 1;
    u->nre[m] = (uint16_t)(u->is_dmrs[m] ? nrb * (12 - cdm * 6) : nrb * 12);
    u->est_sym[m] = (int8_t)get_valid_dmrs_idx_for_channel_est(in->ul_dmrs_symb_pos, (uint8_t)m);
    if (u->est_sym[m] < 0 || !(dm >> u->est_sym[m] & 1))
      return fb(FB_DMRS);
    u->llr_off[m] = G;
    G += (uint32_t)u->nre[m] * qm;
    if (u->nre[m] && u->meas_sym < 0)
      u->meas_sym = m;
  }
  if (u->meas_sym < 0)
    return fb(FB_DMRS);
  if (G != nr_get_G(nrb, in->num_symbols, nb_re_dmrs, nd, 0, qm, 1) || G == 0)
    return fb(FB_TBPARAM);
  /* ---- transport block parameters, as nr_ulsch_decoding derives them (first transmission, round 0) */
  unsigned C = 0, K = 0, Z = 0, F = 0;
  if (nr_segmentation(NULL, NULL, lenWithCrc(1, in->tbs_bits), &C, &K, &Z, &F, in->bg) < 0 || C < 1 || C > NR_GPU_PASS_UL_MAX_CB || (Z & 3) || (K & 7) || (K >> 3) > 1056)
    return fb(FB_TBPARAM);
  const int ncb = nr_gpu_pass_ncb_class((int)C), nrbc = nr_gpu_pass_nrb_class(nrb);
  if (!ncb || !nrbc || in->max_ldpc_iterations < 1 || in->max_ldpc_iterations > 17)
    return fb(FB_TBPARAM);
  const uint32_t E = (uint32_t)nr_get_E(G, (uint8_t)C, (uint8_t)qm, 1, 0);
  int llrLen = 0;
  uint32_t E2 = E, first = C;
  uint8_t R = (uint8_t)nr_get_R_ldpc_decoder(in->rv, (int)E, in->bg, (int)Z, &llrLen, 0), R2 = R;
  for (uint32_t r = 1; r < C; r++) {
    const int Er = nr_get_E(G, (uint8_t)C, (uint8_t)qm, 1, (uint8_t)r);
    if ((uint32_t)Er != E) {
      E2 = (uint32_t)Er;
      R2 = (uint8_t)nr_get_R_ldpc_decoder(in->rv, Er, in->bg, (int)Z, &llrLen, 0);
      first = r;
      break;
    }
  }
  j->A = in->tbs_bits;
  j->C = C; j->K = K; j->Z = Z; j->F = F; j->E = E; j->E2 = E2; j->first_rE2 = first; j->tbslbrm = in->tbslbrm;
  j->BG = (uint8_t)in->bg; j->R = R; j->R2 = R2; j->rv = (uint8_t)in->rv; j->Qm = (uint8_t)qm; j->it_max = (uint8_t)in->max_ldpc_iterations;
  j->G = G;
  j->norm = 0;
  j->scr = in->scr_override ? in->scr_override : gold_cache(((uint32_t)in->rnti << 15) + (0u << 14) + (uint32_t)in->data_scrambling_id, (int)((G + 31) / 32));
  /* ---- front end and grant geometry */
  for (int a = 0; a < in->nb_rx; a++) {
    u->iq[a] = in->iq[a];
    u->rx[a] = in->rx[a];
    if (!u->iq[a])
      return fb(FB_CELL);
  }
  u->iq_len = in->iq_len;
  u->rot_nb_rb = in->n_rb_ul;
  u->rot_fco = in->first_carrier_offset;
  u->nrb = nrb;
  u->k0 = (int)(((uint32_t)in->first_carrier_offset + (uint32_t)(in->bwp_start + in->rb_start) * 12u) % 4096u);
  u->ndmrs = nd;
  u->keep = in->keep;
  const int port = get_dmrs_port(0, in->dmrs_ports);
  if (port < 0 || port > 3)
    return fb(FB_SCOPE);
  u->delta = get_delta((uint8_t)port, (uint8_t)in->dmrs_config_type);
  if (u->delta > 1)
    return fb(FB_SCOPE);
  /* ---- pilots: nr_pusch_channel_estimation()'s own generation, per DM-RS symbol */
  const float beta = get_beta_dmrs(cdm, false);
  const int16_t scaling = (int16_t)((1 / beta) * (1 << 14));
  for (int d = 0; d < nd; d++) {
    if (in->pil_override[d]) {
      u->pil[d] = in->pil_override[d];
      continue;
    }
    c16_t *pil = (c16_t *)(pilbuf + (size_t)d * 6 * 273 * 2);
    const uint32_t *gold = nr_gold_pusch(in->n_rb_ul, in->symbols_per_slot, in->dmrs_scrambling_id, in->nscid, in->slot, u->dmrs_sym[d]);
    nr_pusch_dmrs_rx(NR_NORMAL, gold, pil, (unsigned short)(1000 + port), (unsigned char)(d % 2), (unsigned short)nrb, (uint32_t)(in->bwp_start + in->rb_start) * 12u,
                     (uint8_t)in->dmrs_config_type, scaling);
    u->pil[d] = (const int16_t *)pil;
  }
  if (in->want_est)
    u->est_out = g_ulctx[in->ctx].est;
  /* ---- the job proper */
  j->shape = (nr_gpu_pass_shape_t){(uint16_t)nrbc, (uint16_t)in->num_symbols, 1, (uint16_t)qm, (uint16_t)nd, (uint16_t)ncb, 0, 0, 1};
  j->nbRx = in->nb_rx;
  j->nl = 1;
  j->ndmrs = nd;
  j->nrb = nrb;
  j->llr_out = in->llr_out;
  j->llr_out_all = in->llr_out_all;
  j->gate_on = (uint8_t)(in->gate_on != 0);
  j->gate_thr_db = in->gate_thr_db;
  u->on = 1;
  memset(&out->r, 0, sizeof(out->r));
  out->r.tb = in->tb;
  struct timespec t0_, t1_;
  clock_gettime(CLOCK_MONOTONIC, &t0_);
  const int rc = U.try_(j, &out->r);
  clock_gettime(CLOCK_MONOTONIC, &t1_);
  if (rc != 0)
    return fb(FB_MODULE);
  samp_add(4, (uint32_t)(((int64_t)(t1_.tv_sec - t0_.tv_sec) * 1000000000ll + (t1_.tv_nsec - t0_.tv_nsec)) / 1000));
  /* ---- what nr_rx_pusch_group_internal / nr_pusch_channel_estimation leave behind */
  const int nrx = in->nb_rx;
  out->ndmrs = nd;
  memcpy(out->dmrs_sym, u->dmrs_sym, sizeof(out->dmrs_sym));
  out->G = G; out->C = C; out->K = K; out->F = F; out->Z = Z; out->E = E; out->tbs_bits = in->tbs_bits;
  out->log2_maxh = out->r.ul_log2_maxh;
  uint32_t nvar = 0;
  double hacc = 0;
  for (int d = 0; d < nd; d++) {
    uint64_t noise = 0;
    for (int a = 0; a < nrx; a++) {
      noise += out->r.ul_acc[d * nrx + a];
      hacc += (double)out->r.ul_hsum[d * nrx + a];
    }
    nvar += (uint32_t)(noise / (uint64_t)(nrx * nrb * 12));
  }
  nvar /= (uint32_t)nd;
  out->nvar = nvar;
  out->dmrs_sig2 = hacc / ((double)nrb * 12 * nrx * nd);
  out->dmrs_snr_db = nr_ulsch_dmrs_snr_db(out->dmrs_sig2, (double)nvar);
  {
    /* the delay of the LAST DM-RS symbol (each call of the estimator replaces pvp->delay) */
    int peak[4], est[4];
    for (int a = 0; a < nrx; a++) {
      est[a] = out->r.ul_delay[(nd - 1) * nrx + a][0];
      peak[a] = out->r.ul_delay[(nd - 1) * nrx + a][2];
    }
    int best = 0;
    if (in->passive) {
      best = nr_ulsch_passive_timing_branch(nrx, in->timing_keep, peak);
      if (best < 0)
        best = 0;
    } else {
      for (int a = 1; a < nrx; a++)
        if (est[a] >= est[best])
          best = a;
    }
    out->est_delay = est[best];
    out->est_delay_max_val = peak[best];
  }
  out->est = in->want_est ? g_ulctx[in->ctx].est : NULL;
  return 0;
}

/* ------------------------------------------------------------------ warm-up ------------------------------------------------------------------ */
static int g_warm_budget = 160;
void nr_gpu_pass_ul_test_budget(int classes) { g_warm_budget = classes; }

/* MPCI G3b: the PUSCH classes of a further cell's FEP geometry (the same list as the first cell's, the PRB classes up to max_nrb_class only), captured through `warmf` (bound to the geometry by the caller).
 * Returns the classes made. */
int nr_gpu_pass_ul_warm_geo(nr_gpu_pass_warm_fn warmf, int max_nrb_class)
{
  if (!warmf || !U.try_)
    return 0;
  static const int nd[] = {2, 1};
  static const int nrbs[] = {136, 217, 96, 48, 24, 12, 6, 273};
  int made = 0;
  for (int di = 0; di < 2; di++)
    for (int ni = 0; ni < 8 && made < g_warm_budget; ni++) {
      if (nr_gpu_pass_nrb_class(nrbs[ni]) > max_nrb_class)
        continue;
      const int cmax = nrbs[ni] * 132 * 8 * 93 / 100 / 8400 + 1;
      for (int ncb = 1; ncb <= 32 && made < g_warm_budget; ncb <<= 1) {
        if (ncb / 2 >= cmax)
          break;
        const nr_gpu_pass_shape_t sh = {(uint16_t)nr_gpu_pass_nrb_class(nrbs[ni]), 13, 1, 6, (uint16_t)nd[di], (uint16_t)ncb, 0, 0, 1};
        if (warmf(&sh, 0, 0, 0) == 0)
          made++;
      }
    }
  return made;
}

int nr_gpu_pass_ul_warm(void *h, int nbrx, int fp32)
{
  (void)fp32; /* the PUSCH chain is integer: the FP32 instantiation only exists for the DL demod's MMSE */
  if (!h)
    return 0;
  nr_gpu_pass_warm_fn warmf = (nr_gpu_pass_warm_fn)dlsym(h, "nr_gpu_pass_warm");
  U.try_ = (nr_gpu_pass_ul_try_fn)dlsym(h, "nr_gpu_pass_ul_try");
  U.counters = (void (*)(uint64_t *))dlsym(h, "nr_gpu_pass_ul_counters");
  if (!warmf || !U.try_ || !U.counters) {
    LOG_W(PHY, "SENSING: GPUPASS_UL requested but libpdsch_gpu.so has no PUSCH pass: off\n");
    U.try_ = NULL;
    return 0;
  }
  static const int nd[] = {2, 1};
  const int nnd = 2;
  const int budget = g_warm_budget;
  static const int nrbs[] = {136, 217, 96, 48, 24, 12, 6, 273};
  int made = 0, failed = 0;
  for (int di = 0; di < nnd; di++)
    for (int ni = 0; ni < 8 && made < budget; ni++) {
      /* code blocks per pass: from 1 to the most the grant can carry (256QAM, rate 0.93, one layer) */
      const int cmax = nrbs[ni] * 132 * 8 * 93 / 100 / 8400 + 1;
      for (int ncb = 1; ncb <= 32 && made < budget; ncb <<= 1) {
        if (ncb / 2 >= cmax)
          break;
        const nr_gpu_pass_shape_t sh = {(uint16_t)nr_gpu_pass_nrb_class(nrbs[ni]), 13, 1, 6, (uint16_t)nd[di], (uint16_t)ncb, 0, 0, 1};
        if (warmf(&sh, 0, 0, 0) == 0)
          made++;
        else
          failed++;
      }
    }
  U.nbrx = nbrx;
  U.check = 0; /* Fix A+D: no shadow decode on the hot path; see nr_gpu_pass_test.cc (GpuPassUl.MatchesCpuDecode) */
  atomic_store(&U.on, made > 0);
  LOG_A(PHY, "SENSING: GPUPASS_UL %s: %d PUSCH shape classes x %d instances captured (%d failed), shadow check every %d passes\n", made > 0 ? "enabled" : "NOT enabled", made,
        NR_GPU_PASS_RING, failed, U.check);
  return made;
}
