#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "PHY/TOOLS/nr_stage_split.h"
#include <sched.h>
#include "nr_pdsch_gpu_exact.h"
#include "nr_pdsch_gpu_demod_host.h"
#include "nr_passive_cfg_epoch.h"
#include "nr_passive_sample_lifetime.h"
#include "PHY/TOOLS/nr_gpu_budget.h"
extern _Atomic long nr_ue_diag_producer_absolute_slot;
/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c
 * \brief See nr_pdsch_passive_decode.h for scope and for why this does not simply call
 * nr_dlsch_decoding().
 */

#include "nr_pdsch_passive_decode.h"
#include "nr_snr_gate.h"
#include "nr_recov_yield.h"
#include "nr_hq_retry.h"
#include "nr_pdsch_qm_oracle.h"
#include "nr_blind_stats.h"
#include "nr_pdcch_blind_monitor.h" // nr_pdcch_blind_dmrs_mask (RA TDRA sweep)
#include "nr_pdsch_chest_key.h" // K32: complete chest-cache key
#include "nr_llr_norm.h" // K38: LLR-norm shift over code block 0's span
#include "nr_pdsch_solver.h"
#include "nr_scrambling_id_sweep.h" // per-RNTI dataScramblingIdentityPDSCH TB-CRC walk (Task 13)
#include "nr_td_grantwork.h" // GrantWork-lite: per-grant shared FEP/chest/LLR per geometry signature
#include "nr_pdsch_adaptive_config.h" // hypothesis -> PDU (start, length, DM-RS mask, MCS table)
#include "PHY/CODING/nrLDPC_defs.h" // LDPC_decoderfunc_t (CPU reference CB0 decode)
#include "common/utils/load_module_shlib.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <math.h>
#include <pthread.h> // sqrt/floor/log10 (EQDIAG)
#include "nr_pdsch_evm_acc.h"
#include "nr_passive_tpool.h"
#include "nr_pdsch_retry_pred.h"
#include "nr_pdsch_gpu_ldpc_host.h"
#include "nr_gpu_pass_dl.h"
#include "nr_gpu_pass_hq.h"
#include "nr_gpu_pass_retry.h" // GPUPASS Task 4
#include "PHY/TOOLS/nr_gpu_platform.h"
static void gp_thread_prewarm(void);
#include "nr_pdsch_cpu_route.h" /* BATCHCPU */
#include <stdarg.h> // rep_append (CHESTDIAG)
#include <stdio.h>  // vsnprintf

#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"

/* Per-RX-antenna noise variance published by nr_pdsch_channel_estimation(). Declared here rather
 * than in a shared header: the natural home (defs_nr_UE.h) is a large file whose top section is
 * inside an #ifdef __cplusplus block, and a declaration placed there is silently invisible to C. */
#define NR_DL_CHEST_MAX_ANT 8
extern __thread uint32_t nr_dl_chest_nvar_ant[];
extern __thread int nr_dl_chest_only_ant;
extern __thread int nr_dlsch_chest_per_symbol; // nr_dlsch_demodulation.c: read the estimate at the data symbol itself

#include "PHY/CODING/coding_defs.h"
#include "PHY/NR_REFSIG/dmrs_nr.h" // get_num_dmrs_re_per_rb, nr_chest_time_domain_avg
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/TOOLS/nr_gpu_path.h" // per-path GPU/CPU choice (GPUPASS trial on DISCRETE)
#include "PHY/MODULATION/modulation_UE.h" // nr_slot_fep, nr_slot_fep_ant
#include "nr_pdsch_ptrs_unav.h"
#include "nr_pdsch_prb_set.h" // nr_prb_segments, nr_prb_gather_index (non-contiguous PRB sets)
#include "nr_pdsch_host_fast.h" // NEON unscramble
#include "nr_arm_sweep.h" // generic per-RNTI Wilson pick/latch core shared by the VRB-L and PRG sweeps
#include "nr_harq_init_tx.h" // per-(RNTI, pid) reserved-MCS retransmission record, shared with the UL decoder
#include "nr_csirs_blind_search.h" // nr_csirs_blind_re_energy / nr_csirs_blind_zp_grant_score
#include "nr_csirs_blind_rt.h" // nr_csirs_blind_rt_zp_grant_evidence
#include "PHY/NR_REFSIG/nr_refsig.h" // nr_gold_pdsch, nr_pdsch_dmrs_rx (ZP grant evidence: own-DM-RS presence)
#include "PHY/MODULATION/nr_sfo_state.h" // SFO eps of the GPU pass vs the old path
#include "PHY/NR_TRANSPORT/nr_sch_dmrs.h" // get_delta
_Static_assert(sizeof(((freq_alloc_bitmap_t *)0)->prb_list) == NR_PRB_SET_MAX * sizeof(uint16_t),
               "freq_alloc_bitmap_t.prb_list (common/utils/bits.h) must hold NR_PRB_SET_MAX PRBs");
#include "common/utils/threadPool/task_ans.h" // init_task_ans/join_task_ans/completed_task_ans, for the per-antenna FEP dispatch below
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "PHY/TOOLS/tools_defs.h"
#include "executables/nr-uesoftmodem.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_ldpc_cap.h" /* RANK2-ITER: the one LDPC iteration cap */
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.h" /* WIDE-S0: nr_pdcch_dmrs_rank_grid() for the PDCCH occupancy diagnostic */

/* ---- INNER COST BREAKDOWN (ISAC_PDCCH_TIMING=1, shares the blind monitor's switch) -------------
 * The outer probe in nr_pdcch_blind_monitor_rt.c measures this whole function as ONE ~774 us stage,
 * which is enough to prove it blows the 500 us slot deadline but NOT enough to choose how to move
 * it off the PHY receive thread. The split matters because the two candidate designs have opposite
 * constraints:
 *   - if the FEP dominates, a deferred consumer must re-read ue->common_vars.rxdata, whose steady-
 *     state write region is ONE FRAME, so a given slot's samples are overwritten exactly 10 ms
 *     later -- a hard, silent deadline the consumer would have to police;
 *   - if the demodulation/LDPC dominate, the tap can hand over already-transformed data and the
 *     consumer has no deadline at all, at the cost of copying it.
 * So this is not curiosity: it is the measurement that picks the architecture. */
#define PDTIM_FEP   0
#define PDTIM_CHEST 1
#define PDTIM_ALLOC 2
#define PDTIM_DEMOD 3
#define PDTIM_LDPC  4
#define PDTIM_N     5
static const char *const kPdtimName[PDTIM_N] = {"fep", "chest", "alloc", "demod", "ldpc"};
static uint64_t g_pdtim_ns[PDTIM_N] = {0};
static uint64_t g_pdtim_n[PDTIM_N]  = {0};
static uint64_t g_pdtim_max[PDTIM_N] = {0};
static double g_pdtim_ss[PDTIM_N] = {0}; /* sum of squared stage times (us^2): per-decode std in PDTIM */
static uint64_t g_pdtim_calls = 0;
static uint64_t g_pdtim_cpu_ns[PDTIM_N] = {0}; /* T26c: thread CPU time per stage -- wall >> cpu means preempted/blocked */
/* per-path trial of the graph pass (nr_gpu_path.h, DISCRETE): the side of the current queue decode call: 0 none, 1 GPU (pass tried),
 * 2 CPU (kept off the pass by the trial), 3 GPU picked but the pass not reached. Reset and read by the queue around each decode call. */
static __thread int t_gp_path_side;
void nr_pdsch_passive_path_trial_reset(void) { t_gp_path_side = 0; }
int nr_pdsch_passive_path_trial_side(void) { return t_gp_path_side == 1 ? 1 : t_gp_path_side == 2 ? 0 : -1; }
static __thread uint64_t t_pdtim_cpu0;
static _Atomic int g_pdtim_on = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */

static inline int pdtim_enabled(void)
{
  if (g_pdtim_on < 0) {
    g_pdtim_on = 1;
  }
  return g_pdtim_on;
}

/* pdtim_now: returns the monotonic time in ns and stores the calling thread's CPU time in t_pdtim_cpu0 (used for the stage timers).
 *   Two clock_gettime calls; only reached when ISAC_PDCCH_TIMING is set. Callers: decode stages in passive_decode_body().
 */
static inline uint64_t pdtim_now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  t_pdtim_cpu0 = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* pdtim_add: adds the wall and thread-CPU time of one decode stage k (since t0 from pdtim_now) to the PDTIM counters.
 *   No-op unless ISAC_PDCCH_TIMING is set. Updates count, total, max and CPU time of the stage in plain (non-atomic) globals.
 *   Callers: passive_decode_body() after the FEP, channel estimation, allocation, demodulation and LDPC stages.
 */
/* DECGPU: 10-us histograms per stage and of the per-decode
 * sum fep+chest+demod+ldpc, printed as PDTIMPCT p50/p90/p99 next to every PDTIM line. */
#define PDTIM_HB 2000 /* 10 us bins, last bin = >= 19.99 ms */
static _Atomic uint32_t g_pdtim_hist[PDTIM_N + 1][PDTIM_HB];
static _Atomic uint64_t g_pdtim_ldpc_tb, g_pdtim_ldpc_cb; /* passive_ldpc_decode_core() calls and code blocks decoded */
static __thread uint64_t t_pdtim_sum, t_pdtim_sum_n;
static inline void pdtim_hist(int k, uint64_t d)
{
  const uint64_t b = d / 10000;
  atomic_fetch_add_explicit(&g_pdtim_hist[k][b < PDTIM_HB ? b : PDTIM_HB - 1], 1, memory_order_relaxed);
}
/* GPUSCHED: the per-body sum split by class -- real = first body of
 * a full decode, retry = later bodies of the same nr_pdsch_passive_decode() call (recovery arms), probe = first-code-block
 * solver probe -- with exact mean, p50/p90/p99, max and the bodies above 5 / 20 ms, printed as PDTIMCLS after PDTIMPCT. */
#define PDTIM_NCLS 3
static const char *const kPdtimCls[PDTIM_NCLS] = {"real", "retry", "probe"};
static _Atomic uint32_t g_cls_hist[PDTIM_NCLS][PDTIM_HB];
static _Atomic uint64_t g_cls_ns[PDTIM_NCLS], g_cls_max[PDTIM_NCLS], g_cls_gt5[PDTIM_NCLS], g_cls_gt20[PDTIM_NCLS];
static _Atomic uint64_t g_cls_ns1[PDTIM_NCLS], g_cls_ns5[PDTIM_NCLS], g_cls_ns20[PDTIM_NCLS]; /* DECTAIL: time spent in bodies > 1 / 5 / 20 ms */
static __thread unsigned t_pdtim_body; /* bodies of the current nr_pdsch_passive_decode() call (passive_decode_body) */
#define RETRY_BUDGET_NS 1000000ull /* FATTAILS: a decode call that has run this long skips its long-shot recovery arms */
static uint64_t dec_now_ns(void);
static __thread uint64_t t_dec_first_ns; /* SYSAN e2e: wall of this call's first body */
static __thread uint64_t t_dec_body_ns, t_dec_stage_ns, t_dec_t0_ns; /* FATTAILS DECSLOW: this call's body wall and fep+chest+demod+ldpc wall */
/* FATTAILS DECSLOW: the last nr_pdsch_passive_decode() call's bodies, their wall time and their stage time (ns) */
void nr_pdsch_passive_decode_slow_stats(unsigned *bodies, uint64_t *body_ns, uint64_t *stage_ns)
{
  *bodies = t_pdtim_body;
  *body_ns = t_dec_body_ns;
  *stage_ns = t_dec_stage_ns;
}
uint64_t nr_pdsch_passive_decode_first_body_ns(void) { return t_dec_first_ns; }
static __thread uint32_t t_pdtim_stage_us[PDTIM_N], t_pdtim_stage_cpu_us[PDTIM_N]; /* this body's stage wall / thread-CPU times (PDTIMSLOW) */
static _Atomic int g_pdtim_slow_left = 200;
/* DECODETIME: per class x stage, the stage wall time (us) held by bodies of duration band <1 / 1-5 / 5-20 / >20 ms (PDTIMCLSST) */
static _Atomic uint64_t g_cls_stg[PDTIM_NCLS][PDTIM_N][4];
static bool pdtim_is_probe(void);

/* ---- DECODETIME: per-thread persistent decode buffers that the GPU kernels touch -----------------------------------
 * Heap-backed TLS handles (see the PERSISTENT SCRATCH note in passive_decode_body); file scope so that
 * nr_pdsch_passive_decode_pin_prewarm() can allocate and pin them at consumer start, off the decode path. */
static __thread fourDimArray_t *toFree, *toFree2, *toFree3, *toFree4, *toFree5; /* estimates, comp, mag, magb, magr */
static __thread int16_t *llr_tls;      /* LLRs: grow-only (at the maximum once when pinning is on) */
static __thread uint32_t llr_cap_tls;
static __thread bool bufs_pinned;
void nr_pdsch_gpu_chest_pin_prewarm(void) __attribute__((weak)); /* nr_dl_channel_estimation.c */
/* decode_bufs_prepare: allocates the five constant-shape buffers (idempotent) and, with ISAC_PDSCH_GPU_HOST_PIN=1, pins
 * them once per thread. shortcut: a later reallocation (shape change, never seen: the shapes are frame-parm constants)
 * would leave the registration stale; add an unpin if a shape ever becomes grant-dependent. */
static void decode_bufs_prepare(const NR_DL_FRAME_PARMS *fp)
{
  const uint32_t pdsch_est_size = ((fp->symbols_per_slot * fp->ofdm_symbol_size + 15) / 16) * 16;
  const uint32_t rx_size_symbol = (fp->N_RB_DL * NR_NB_SC_PER_RB + 15) & ~15;
  allocCast2D(pdsch_dl_ch_estimates, int32_t, toFree, fp->nb_antennas_rx * NR_MAX_NB_LAYERS, pdsch_est_size, true);
  allocCast3D(rxdataF_comp, c16_t, toFree2, fp->symbols_per_slot, NR_MAX_NB_LAYERS, rx_size_symbol, true);
  allocCast3D(dl_ch_mag, c16_t, toFree3, NR_SYMBOLS_PER_SLOT, NR_MAX_NB_LAYERS, rx_size_symbol, true);
  allocCast3D(dl_ch_magb, c16_t, toFree4, NR_SYMBOLS_PER_SLOT, NR_MAX_NB_LAYERS, rx_size_symbol, true);
  allocCast3D(dl_ch_magr, c16_t, toFree5, NR_SYMBOLS_PER_SLOT, NR_MAX_NB_LAYERS, rx_size_symbol, true);
  if (bufs_pinned || !nr_pdsch_gpu_host_pin_enabled())
    return;
  bufs_pinned = true;
  const size_t mb = (size_t)NR_SYMBOLS_PER_SLOT * NR_MAX_NB_LAYERS * rx_size_symbol * sizeof(c16_t);
  nr_pdsch_gpu_host_pin(pdsch_dl_ch_estimates, (size_t)fp->nb_antennas_rx * NR_MAX_NB_LAYERS * pdsch_est_size * sizeof(int32_t));
  nr_pdsch_gpu_host_pin(rxdataF_comp, (size_t)fp->symbols_per_slot * NR_MAX_NB_LAYERS * rx_size_symbol * sizeof(c16_t));
  nr_pdsch_gpu_host_pin(dl_ch_mag, mb);
  nr_pdsch_gpu_host_pin(dl_ch_magb, mb);
  nr_pdsch_gpu_host_pin(dl_ch_magr, mb);
}
/* llr_prepare: the per-thread LLR buffer with at least `need` int16 entries (grow-only; cleared by the caller). With pinning
 * on it is allocated once at the largest G of the carrier, so no later growth has to unpin/re-pin (cudaHostUnregister
 * measured 22 ms mean, 186 ms max). */
static int16_t *llr_prepare(const NR_DL_FRAME_PARMS *fp, uint32_t need)
{
  if (llr_cap_tls >= need)
    return llr_tls;
  if (nr_pdsch_gpu_host_pin_enabled()) {
    const uint32_t llr_max = ALIGNARRAYSIZE((uint32_t)fp->N_RB_DL * NR_NB_SC_PER_RB * NR_SYMBOLS_PER_SLOT * 8 * NR_MAX_NB_LAYERS, 32);
    if (need < llr_max)
      need = llr_max;
    nr_pdsch_gpu_host_unpin(llr_tls);
  }
  free(llr_tls);
  llr_tls = (int16_t *)malloc16_clear(need * sizeof(int16_t));
  llr_cap_tls = llr_tls ? need : 0;
  if (llr_tls)
    nr_pdsch_gpu_host_pin(llr_tls, (size_t)need * sizeof(int16_t));
  return llr_tls;
}
/* seg_virt_prepare: this thread's data-ordered gather buffer (segmented grants), allocated and pinned once (file scope so that the
 * prewarm below can create it before the first grant). */
static __thread c16_t *t_seg_virt;
static __thread size_t t_seg_virt_cap;
static c16_t *seg_virt_prepare(const NR_DL_FRAME_PARMS *fp)
{
  const size_t need = (size_t)fp->nb_antennas_rx * fp->samples_per_slot_wCP;
  if (t_seg_virt_cap < need) {
    nr_pdsch_gpu_host_unpin(t_seg_virt); /* DECODETIME: read by the GPU demod */
    free(t_seg_virt);
    t_seg_virt = (c16_t *)malloc16(need * sizeof(c16_t));
    t_seg_virt_cap = t_seg_virt ? need : 0;
    nr_pdsch_gpu_host_pin(t_seg_virt, need * sizeof(c16_t));
  }
  return t_seg_virt;
}
int nr_pdsch_pin_prewarm_all(void) { return 0; } /* NEEDS: caller in nr_pdsch_passive_queue.c to be dropped, then delete this */
/* nr_pdsch_passive_decode_pin_prewarm: ISAC_PDSCH_GPU_HOST_PIN=1 only -- allocate and pin this consumer thread's GPU-touched
 * buffers before its first grant (flag off: no-op, buffers appear lazily as before). Caller: the PDSCH queue consumer. */
void nr_pdsch_passive_decode_pin_prewarm(PHY_VARS_NR_UE *ue)
{
  gp_thread_prewarm(); /* GPUPASS: ISAC_GPU_PASS=1 only */
  if (!nr_pdsch_gpu_host_pin_enabled())
    return;
  decode_bufs_prepare(&ue->frame_parms);
  llr_prepare(&ue->frame_parms, 1);
  if (nr_pdsch_gpu_chest_pin_prewarm)
    nr_pdsch_gpu_chest_pin_prewarm();
}
static void pdtim_cls_note(uint64_t d)
{
  const int c = pdtim_is_probe() ? 2 : t_pdtim_body > 1 ? 1 : 0;
  const int band = d > 20000000 ? 3 : d > 5000000 ? 2 : d > 1000000 ? 1 : 0;
  for (int k = 0; k < PDTIM_N; k++)
    atomic_fetch_add_explicit(&g_cls_stg[c][k][band], t_pdtim_stage_us[k], memory_order_relaxed);
  if (d > 20000000 && atomic_fetch_sub(&g_pdtim_slow_left, 1) > 0) { /* a body above 20 ms: when, and which stage */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    LOG_I(PHY, "SENSING: PDTIMSLOW t=%ld.%03ld class=%s us=%lu fep=%u/%u chest=%u/%u demod=%u/%u ldpc=%u/%u (wall/cpu us)\n", (long)ts.tv_sec,
          ts.tv_nsec / 1000000, kPdtimCls[c], (unsigned long)(d / 1000), t_pdtim_stage_us[PDTIM_FEP], t_pdtim_stage_cpu_us[PDTIM_FEP],
          t_pdtim_stage_us[PDTIM_CHEST], t_pdtim_stage_cpu_us[PDTIM_CHEST], t_pdtim_stage_us[PDTIM_DEMOD], t_pdtim_stage_cpu_us[PDTIM_DEMOD],
          t_pdtim_stage_us[PDTIM_LDPC], t_pdtim_stage_cpu_us[PDTIM_LDPC]);
  }
  const uint64_t b = d / 10000;
  atomic_fetch_add_explicit(&g_cls_hist[c][b < PDTIM_HB ? b : PDTIM_HB - 1], 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_cls_ns[c], d, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_cls_gt5[c], d > 5000000, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_cls_gt20[c], d > 20000000, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_cls_ns1[c], d > 1000000 ? d : 0, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_cls_ns5[c], d > 5000000 ? d : 0, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_cls_ns20[c], d > 20000000 ? d : 0, memory_order_relaxed);
  uint64_t mx = atomic_load_explicit(&g_cls_max[c], memory_order_relaxed);
  while (d > mx && !atomic_compare_exchange_weak(&g_cls_max[c], &mx, d)) {}
}
/* pdtim_q3: p50/p90/p99 (bin centres, us) of a 10-us histogram; returns the count. */
static uint64_t pdtim_q3(_Atomic uint32_t *h, int p[3])
{
  uint64_t n = 0, c = 0;
  for (int b = 0; b < PDTIM_HB; b++)
    n += h[b];
  const double q[3] = {0.5, 0.9, 0.99};
  int i = 0;
  p[0] = p[1] = p[2] = 0;
  for (int b = 0; b < PDTIM_HB && i < 3; b++) {
    c += h[b];
    while (i < 3 && n && (double)c >= q[i] * (double)n)
      p[i++] = b * 10 + 5;
  }
  return n;
}
static void pdtim_cls_report(void)
{
  char rep[900];
  int u = 0;
  for (int k = 0; k < PDTIM_NCLS; k++) {
    int p[3];
    const uint64_t n = pdtim_q3(g_cls_hist[k], p);
    u += snprintf(rep + u, sizeof(rep) - u, "%s[n=%lu mean=%.1f p50=%d p90=%d p99=%d max=%.1f gt5ms=%lu gt20ms=%lu share_gt1/5/20ms=%.2f/%.2f/%.2f] ", kPdtimCls[k],
                  (unsigned long)n, n ? (double)atomic_load(&g_cls_ns[k]) / (double)n / 1000.0 : 0.0, p[0], p[1], p[2],
                  (double)atomic_load(&g_cls_max[k]) / 1000.0, (unsigned long)atomic_load(&g_cls_gt5[k]),
                  (unsigned long)atomic_load(&g_cls_gt20[k]), (double)atomic_load(&g_cls_ns1[k]) / (double)(atomic_load(&g_cls_ns[k]) + 1),
                  (double)atomic_load(&g_cls_ns5[k]) / (double)(atomic_load(&g_cls_ns[k]) + 1),
                  (double)atomic_load(&g_cls_ns20[k]) / (double)(atomic_load(&g_cls_ns[k]) + 1));
  }
  LOG_I(PHY, "SENSING: PDTIMCLS us %s\n", rep);
  /* stage x duration band: per class, per stage, ms of stage wall time in bodies <1 / 1-5 / 5-20 / >20 ms */
  u = 0;
  for (int c = 0; c < PDTIM_NCLS; c++) {
    u += snprintf(rep + u, sizeof(rep) - u, "%s[", kPdtimCls[c]);
    for (int k = 0; k < PDTIM_N; k++) {
      if (k == PDTIM_ALLOC)
        continue;
      u += snprintf(rep + u, sizeof(rep) - u, "%s=%.0f/%.0f/%.0f/%.0f ", kPdtimName[k], atomic_load(&g_cls_stg[c][k][0]) / 1000.0,
                    atomic_load(&g_cls_stg[c][k][1]) / 1000.0, atomic_load(&g_cls_stg[c][k][2]) / 1000.0, atomic_load(&g_cls_stg[c][k][3]) / 1000.0);
    }
    u += snprintf(rep + u, sizeof(rep) - u, "] ");
  }
  LOG_I(PHY, "SENSING: PDTIMCLSST ms(<1/1-5/5-20/>20ms bodies) %s\n", rep);
}
static void pdtim_pct_report(void)
{
  char rep[400];
  int u = 0;
  for (int k = 0; k <= PDTIM_N && u < (int)sizeof(rep) - 60; k++) {
    if (k == PDTIM_ALLOC)
      continue;
    uint64_t n = 0, c = 0;
    for (int b = 0; b < PDTIM_HB; b++)
      n += g_pdtim_hist[k][b];
    const double q[3] = {0.5, 0.9, 0.99};
    int p[3] = {0, 0, 0}, i = 0;
    for (int b = 0; b < PDTIM_HB && i < 3; b++) {
      c += g_pdtim_hist[k][b];
      while (i < 3 && n && (double)c >= q[i] * (double)n)
        p[i++] = b * 10 + 5;
    }
    u += snprintf(rep + u, sizeof(rep) - u, "%s[n=%lu p50=%d p90=%d p99=%d] ", k == PDTIM_N ? "sum" : kPdtimName[k], (unsigned long)n, p[0], p[1],
                  p[2]);
  }
  LOG_I(PHY, "SENSING: PDTIMPCT us %sldpc_tb=%lu ldpc_cb=%lu\n", rep, (unsigned long)atomic_load(&g_pdtim_ldpc_tb),
        (unsigned long)atomic_load(&g_pdtim_ldpc_cb));
}

static inline void pdtim_add(int k, uint64_t t0)
{
  if (g_pdtim_on <= 0) {
    return;
  }
  const uint64_t c0 = t_pdtim_cpu0;
  const uint64_t d = pdtim_now() - t0;
  if (k != PDTIM_ALLOC) {
    pdtim_hist(k, d);
    if (k == PDTIM_FEP) {
      t_pdtim_sum = t_pdtim_sum_n = 0; /* a decode that returned early left a partial sum */
      memset(t_pdtim_stage_us, 0, sizeof(t_pdtim_stage_us));
      memset(t_pdtim_stage_cpu_us, 0, sizeof(t_pdtim_stage_cpu_us));
    }
    t_pdtim_stage_us[k] = (uint32_t)(d / 1000);
    t_pdtim_stage_cpu_us[k] = (uint32_t)((t_pdtim_cpu0 - c0) / 1000);
    t_pdtim_sum += d;
    t_pdtim_sum_n++;
  }
  g_pdtim_cpu_ns[k] += t_pdtim_cpu0 - c0;
  g_pdtim_ns[k] += d;
  g_pdtim_ss[k] += ((double)d / 1000.0) * ((double)d / 1000.0);
  g_pdtim_n[k]++;
  if (d > g_pdtim_max[k]) {
    g_pdtim_max[k] = d;
  }
}

/* pdtim_report: prints the per-stage PDTIM summary (mean, CPU mean, max, total) once every 200 decode calls.
 *   Runs only with ISAC_PDCCH_TIMING set. Caller: passive_decode_body() at its end.
 */
static void pdtim_report(void)
{
  if (g_pdtim_on <= 0) {
    return;
  }
  g_pdtim_calls++;
  if (t_pdtim_sum_n) {
    pdtim_hist(PDTIM_N, t_pdtim_sum);
    pdtim_cls_note(t_pdtim_sum);
    t_dec_stage_ns += t_pdtim_sum;
    t_pdtim_sum = t_pdtim_sum_n = 0;
  }
  if ((g_pdtim_calls % 200) != 0) {
    return;
  }
  pdtim_pct_report();
  pdtim_cls_report();
  char rep[900];
  int u = 0;
  for (int k = 0; k < PDTIM_N && u < (int)sizeof(rep) - 110; k++) {
    const double mu = g_pdtim_n[k] ? (double)g_pdtim_ns[k] / (double)g_pdtim_n[k] / 1000.0 : 0.0;
    const double var = g_pdtim_n[k] > 1 ? (g_pdtim_ss[k] - (double)g_pdtim_n[k] * mu * mu) / (double)(g_pdtim_n[k] - 1) : 0.0;
    u += snprintf(rep + u, sizeof(rep) - u, "%s[n=%lu mean=%.1fus cpu=%.1fus max=%.1fus tot=%.2fs sd=%.1fus] ", kPdtimName[k],
                  (unsigned long)g_pdtim_n[k],
                  g_pdtim_n[k] ? (double)g_pdtim_ns[k] / (double)g_pdtim_n[k] / 1000.0 : 0.0,
                  g_pdtim_n[k] ? (double)g_pdtim_cpu_ns[k] / (double)g_pdtim_n[k] / 1000.0 : 0.0,
                  (double)g_pdtim_max[k] / 1000.0, (double)g_pdtim_ns[k] / 1e9, var > 0 ? sqrt(var) : 0.0);
  }
  LOG_I(PHY, "SENSING: PDTIM calls=%lu %s\n", (unsigned long)g_pdtim_calls, rep);
}

/* evm_fill: the decode's EVM / kappa / branch outputs from nr_pdsch_evm_acc (disarms it). Caller: the decode, right after the
 * demodulation, or after the LDPC stage when the GPU demodulation deferred its EVM sums (ISAC_PDSCH_GPU_EXACT_DEMOD_EVM_DEFER). */
static void evm_fill(nr_pdsch_passive_decode_result_t *out, const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config, bool demod_ok)
{
  if (nr_pdsch_evm_acc.armed) {
    nr_pdsch_evm_acc_t *ea = &nr_pdsch_evm_acc;
    ea->armed = 0;
    if (demod_ok && ea->n_sym > 0 && ea->ref > 0.0) {
      out->evm_valid = true;
      out->evm_pct = (float)(sqrt(ea->err / ea->ref) * 100.0);
      out->evm_cpe_pct = (float)(sqrt(ea->err_cpe / ea->ref) * 100.0);
      out->kappa = ea->kappa_n ? (float)(ea->kappa_sum / ea->kappa_n) : 0.f;
      out->rho = ea->rho_n ? (float)(ea->rho_sum / ea->rho_n) : 0.f;
      for (int a = 0; a < 4; a++) {
        out->br_alpha[a] = ea->alpha_n[a] ? (float)(ea->alpha_abs[a] / ea->alpha_n[a]) : 0.f;
        out->br_nvar_db[a] = nr_dl_chest_nvar_ant[a] > 0 ? (float)(10.0 * log10((double)nr_dl_chest_nvar_ant[a])) : -99.f;
      }
      out->evm_nbrx = (uint8_t)ea->nbrx;
      out->evm_nsym = (uint16_t)ea->n_sym;
      out->dmrs_ports = (uint16_t)dlsch_config->dmrs_ports;
      out->n_cdm = (uint8_t)dlsch_config->n_dmrs_cdm_groups;
      out->dmrs_add = (uint8_t)__builtin_popcount(dlsch_config->dlDmrsSymbPos);
      for (int a = 0; a < ea->nbrx && a < 4; a++)
        out->br_snr_db[a] = (ea->sig[a] > 0.0 && ea->res[a] > 0.0) ? (float)(10.0 * log10(ea->sig[a] / ea->res[a])) : -99.f;
      for (int l = 0; l < 4; l++)
        out->layer_snr_db[l] = ea->err_layer[l] > 0.0 ? (float)(10.0 * log10(ea->ref_layer[l] / ea->err_layer[l])) : -99.f;
    }
  }
}

/* ---- PARMSET: distinct decode-parameter census (always on, printed at teardown) ---------------
 * PASSIVE_RX_ONLY_HANDOVER.md section 23.3: this receiver decodes 0 of 104,143 transport blocks on
 * some runs and 90 % on others, at matched load, dwell and receiver health. A hard zero over five
 * figures is a WRONG-PARAMETER signature, not a weak-link one -- a channel 1 dB under the waterfall
 * gives a few percent, never exactly none. So the question is simply: which parameter differs
 * between a 90 % run and a 0 % run?
 *
 * TBPARM already prints all of this, but UNCAPPED -- 100k lines at 1500 decodes/s, heavy enough to
 * perturb the very run being characterised. This records the DISTINCT tuples instead, with a count
 * each, which is what a diff actually needs and costs one linear scan of a 16-entry table per
 * decode. */
#define PARMSET_MAX 16
typedef struct {
  uint8_t  mcs, tbl, Qm, bg, nl, cdm, nscid, refpt, ssym, nsym, dmrs_len;
  uint16_t R, ports, scramb;
  uint32_t tbs, dmrsmask;
  uint64_t count;
} parmset_t;
static parmset_t g_parmset[PARMSET_MAX];
static int       g_parmset_n = 0;
static uint64_t  g_parmset_other = 0;
static pthread_mutex_t g_parmset_lock = PTHREAD_MUTEX_INITIALIZER;

static void parmset_record(const parmset_t *k)
{
  pthread_mutex_lock(&g_parmset_lock);
  for (int i = 0; i < g_parmset_n; i++) {
    parmset_t *e = &g_parmset[i];
    if (e->mcs == k->mcs && e->tbl == k->tbl && e->Qm == k->Qm && e->bg == k->bg && e->nl == k->nl
        && e->cdm == k->cdm && e->nscid == k->nscid && e->refpt == k->refpt && e->ssym == k->ssym
        && e->nsym == k->nsym && e->dmrs_len == k->dmrs_len && e->R == k->R && e->ports == k->ports
        && e->scramb == k->scramb && e->tbs == k->tbs && e->dmrsmask == k->dmrsmask) {
      e->count++;
      pthread_mutex_unlock(&g_parmset_lock);
      return;
    }
  }
  if (g_parmset_n < PARMSET_MAX) {
    g_parmset[g_parmset_n] = *k;
    g_parmset[g_parmset_n].count = 1;
    g_parmset_n++;
  } else {
    g_parmset_other++;
  }
  pthread_mutex_unlock(&g_parmset_lock);
}

void nr_pdsch_passive_parmset_dump(void)
{
  pthread_mutex_lock(&g_parmset_lock);
  for (int i = 0; i < g_parmset_n; i++) {
    const parmset_t *e = &g_parmset[i];
    LOG_I(PHY,
          "SENSING: PARMSET[%d] n=%lu mcs=%u tbl=%u Qm=%u R=%u tbs=%u bg=%u nl=%u cdm=%u ports=0x%x "
          "nscid=%u refpt=%u sym=%u+%u dmrs_len=%u dmrsmask=0x%x scramb=%u\n",
          i, (unsigned long)e->count, e->mcs, e->tbl, e->Qm, e->R, e->tbs, e->bg, e->nl, e->cdm,
          e->ports, e->nscid, e->refpt, e->ssym, e->nsym, e->dmrs_len, e->dmrsmask, e->scramb);
  }
  if (g_parmset_other)
    LOG_I(PHY, "SENSING: PARMSET overflow=%lu (more than %d distinct tuples)\n",
          (unsigned long)g_parmset_other, PARMSET_MAX);
  pthread_mutex_unlock(&g_parmset_lock);
}

// Distinct from the attached path's 1000 + harq_pid (phy_procedures_nr_ue.c) and from
// nr_dlsch_decoding()'s 2*harq_pid + cw_idx, so a hardware LDPC accelerator's per-harq_unique_pid
// segment buffers can never be shared between a real HARQ process and an overheard one.
#define NR_PDSCH_PASSIVE_HARQ_TAG_BASE 2000

// ---------------------------------------------------------------------------------------------
// Private HARQ context. One per decoding thread: the RT tap runs from the UE's per-slot RX thread,
// and a thread never has two decodes in flight, so per-thread state needs no locking. Buffers are
// sized exactly as nr_init_dl_harq_processes() sizes the real ones (same segment-count formula), so
// a payload the real UE could hold fits here too.
// ---------------------------------------------------------------------------------------------
typedef struct {
  int      n_rb_dl;      // the N_RB_DL these buffers were sized for; a change forces a realloc
  uint32_t a_segments;
  uint8_t *b;
  uint8_t *c;
  int16_t *d;
  uint32_t processedSegments;
  int      llrLen;
  decode_abort_t abort_decode;
} passive_harq_t;

/* ---- LDPC failure-mode census (always on; printed with the periodic summary) -----------------
 * The passive receiver decodes either ~40-65 % of transport blocks or ~0 %, deterministic for a
 * whole run, and PASSIVE_RX_ONLY_HANDOVER.md §29 has now refuted every upstream cause: the channel
 * estimate is VALID in the failing runs (9-18.6 dB) and its SNR is ANTI-correlated with CRC. So the
 * fault is downstream of the equaliser -- and this function already distinguishes the two places it
 * can be, it just discarded the distinction by returning false from both:
 *   seg_fail : a per-SEGMENT CRC failed  -> LDPC did not converge -> the LLRs are wrong
 *              (demodulation, LLR scaling, rate de-matching, descrambling)
 *   tb_fail  : every segment CRC PASSED but the TB CRC did not -> the decode was CORRECT and the
 *              reassembly/TBS/CRC-type is wrong
 *   zero_tb  : decoded to an all-zero payload (the known false-pass guard below)
 * Those two point at completely different code, so this split is the next fork in the diagnosis.
 * Also records how many segments of C actually decoded, which separates "nothing works" from
 * "one segment is marginal". */
/* ---- LLR quality, ATTRIBUTED BY OUTCOME (§30's next measurement) -----------------------------
 * §30 pinned the fault to the LLRs: every failure is a per-segment LDPC non-convergence
 * (`tb_fail = 0` in every run), while §29 showed the channel estimate feeding them is VALID at
 * 9-18.6 dB. So the question is what the LLRs actually look like, and the discriminator is:
 *   saturated at the int16 rail -> the SCALING is wrong (nvar / log2_maxh)
 *   crushed towards zero        -> no soft information reaches the decoder
 *   healthy magnitude, decodes nothing -> the SIGNS are wrong, i.e. descrambling
 * Binned by whether that transport block subsequently decoded, so a 43 % run compares its OWN
 * successes against its OWN failures -- far stronger than comparing across runs, which is what
 * every cross-run comparison in §§17-29 was reduced to.
 * Sampled every 32nd LLR: G reaches ~150k and this runs per decode. */
/* Four receive streams are estimated independently, then exactly one demodulation/LDPC
 * pipeline runs. Per-antenna CFR extraction remains downstream of the decoded X and uses every
 * raw Y branch; CRC failure never triggers another antenna decode. */
static _Atomic uint64_t g_llr_n[2]    = {0, 0}; // [0] = TB failed, [1] = TB decoded
static _Atomic uint64_t g_llr_absum[2] = {0, 0};
static _Atomic uint64_t g_llr_zero[2]  = {0, 0};
static _Atomic uint64_t g_llr_sat[2]   = {0, 0};
/* Clipping at the INT8 rail, which is the one that actually applies. g_llr_sat above counts
 * |llr| >= 32767, but nothing ever reaches the int16 rail -- and it is not the limit anyway:
 * nrLDPC_coding_segment_decoder.c packs the int16 LLRs down to int8 with simde_mm_packs_epi16(),
 * a SATURATING pack, so every |llr| > 127 is flattened to +-127 before the decoder sees it. A
 * clipped LLR is a hard decision, and belief propagation on hard decisions cannot correct
 * anything. Measured mean |llr| on this receiver is 232-498, i.e. 2-4x that rail. */
static _Atomic uint64_t g_llr_clip8[2] = {0, 0};
/* SIGNED sum, and the count of positive LLRs. mean|LLR| (above) is BLIND to a sign bias, and a bias
 * is exactly what would explain the dominant failure mode: all-zeros is a valid codeword for any
 * linear code, so LDPC settles on it whenever the LLRs systematically favour 0-bits. With one UE on
 * the cell every accepted grant is a REAL grant carrying REAL data, and MAC padding carries an LCID
 * subheader so it is not all-zero either -- so a bias in the soft information is the remaining
 * mechanism that produces an all-zero transport block from a non-empty one. Balanced data should
 * give mean ~ 0 and pos ~ 50 %. */
/* Grant SHAPE per outcome. §33 left one question: what distinguishes the ~47 % of transport blocks
 * that decode to all-zeros from the ones that carry data, given the gNB has 590 kB backlogged and is
 * transmitting real data on essentially every grant. If the all-zero population clusters on a
 * particular TBS, PRB count or RV, that is a structural clue; if its shape distribution is identical
 * to the decoding population's, the cause is not in the grant at all. Indexed [0]=zero_tb,
 * [1]=decoded-with-data, [2]=seg_fail. */
static _Atomic uint64_t g_shape_n[3]   = {0, 0, 0};
static _Atomic uint64_t g_shape_tbs[3] = {0, 0, 0};
static _Atomic uint64_t g_shape_rb[3]  = {0, 0, 0};
/* Per-TB outcome histogram over ALLOCATION SIZE (2026-09-07). The SHAPE means above already show
 * that failing TBs are the SMALLER allocations in every run measured, which REFUTES the `q^n`
 * code-block-count explanation (failures have FEWER blocks, not more). What a mean cannot answer is
 * the shape of that dependence, and the two candidates predict different pictures:
 *   link margin       -> a smooth gradient, and the crossover MOVES between runs
 *   a code/shape bug  -> a step, and the decodable set is the SAME set every run
 * The second is what the run-to-run data hints at: across a 3.4x CRC swing the DECODED population
 * barely moved (mean_prb 21.2/21.5/18.4/20.8) while the FAILED one grew (11.9 -> 17.0). 16 buckets
 * of 18 RB spans the 273 PRB carrier. */
#define NR_PDSCH_RBHIST_BINS 16
static _Atomic uint64_t g_rbhist[3][NR_PDSCH_RBHIST_BINS];
/* MCSHIST: CRC by MCS index, split narrow (<128 PRB) / wide (>=128 PRB). Separates link margin
 * (wide fails only at high MCS) from a width bug (wide fails at every MCS). */
static _Atomic uint64_t g_mcshist[2][3][32];
static _Atomic uint64_t g_mcs_segs[32], g_mcs_segs_ok[32], g_mcs_tbs[32], g_mcs_rb[32];
/* V2 HARQ soft-combining and PT-RS sweep state, declared here so the periodic report can read it. */
static _Atomic uint64_t g_hq_retx_try, g_hq_retx_ok, g_hq_tbs_override, g_hq_busy_skip, g_hq_first;
static _Atomic uint64_t g_hqb_evict, g_hqb_nospace; /* device HARQ blocks (GPU pass): sums dropped to make room, grants decoded without keeping theirs */
/* g_harqc's own lock (harqc_entry_t/g_harqc are declared further down, next to their only other
 * users) and the reserved-MCS retransmission record (gap-harq lane) that shares it -- both declared
 * this early, alongside the harqc counters above, for the SAME reason: nr_pdsch_passive_ldpc_stats_
 * dump() (right below) is defined before harqc_entry_t and must be able to read g_dl_harq_init's
 * hit/evict counts under the same lock. See nr_harq_init_tx.h for why TBS/base-graph (not
 * modulation order) is the field a reserved MCS cannot supply on its own. */
static pthread_mutex_t g_harqc_lock = PTHREAD_MUTEX_INITIALIZER;
static nr_harq_init_tx_table_t g_dl_harq_init;
static pthread_mutex_t g_ptrs_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_lbrm_nl = 4;        // TBS_LBRM layer term n_L, CELL-WIDE seed (4 = spec ceiling), see rnti_dec()
/* PER-RNTI n_L AND PT-RS ARM. Both are dedicated-RRC properties of the UE (maxMIMO-layers
 * capability, phaseTrackingRS) and only cell-common in practice, so each RNTI latches its own,
 * SEEDED from the cell-wide value, and the cell-wide value is promoted only once two distinct
 * RNTIs latched the same one. nl == 0 means "not latched, read the cell seed". With a single RNTI
 * this is exactly the old cell-wide behaviour (its own latch is the only one, read back on the
 * next grant); the cell seed is then never promoted and never read past the first latch. */
/* PER-RNTI DCI 1_1 VRB-to-PRB BUNDLE SIZE (L=2 or L=4, RRC vrb-ToPRB-Interleaver -- invisible to a
 * passive receiver) and PER-RNTI PRB-BUNDLING/PRG SIZE (0=wideband/2/4, RRC PRB-bundling-type --
 * also invisible): two small hypotheses decided by the TB CRC, same shape as the PT-RS sweep above
 * (Wilson-interval arm selection + latch), both built on nr_arm_sweep.h's generic pick/latch core
 * rather than each re-deriving it -- WITHOUT nr_hyp_sweep's ~1.16 MB nr_hyp_sweep_state_t (measured
 * from its only two users in-tree -- see openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_discovery.c's own
 * sizing comment): just a couple of trial/pass counters and a latch per sweep, RNTI_DEC_MAX (16) of
 * which cost nothing worth heap-allocating.
 * VRB-L: arm 0 = L2, arm 1 = L4 (nr_pdsch_vrbl_pick() below).
 * PRG:   arm 0 = wideband (prg=0), arm 1 = 2, arm 2 = 4 (nr_prg_arm_value() below). */
#define NR_VRBL_ARMS 2
#define NR_PRG_ARMS 3
typedef nr_arm_sweep_t nr_vrbl_sweep_t;
typedef nr_arm_sweep_gated_t nr_prg_sweep_t;
static int vrbl_sweep_pick(const nr_vrbl_sweep_t *s) { return nr_arm_sweep_pick(s, NR_VRBL_ARMS); }
static int vrbl_sweep_feed(nr_vrbl_sweep_t *s, int arm, bool tb_ok) { return nr_arm_sweep_feed(s, NR_VRBL_ARMS, arm, tb_ok); }
/* PRG is EVIDENCE-TRIGGERED (final review C1): arm 0 (wideband = the exact pre-sweep contiguous path) is
 * used exclusively until it shows a sustained CRC deficit while the link is healthy, see
 * nr_arm_sweep_gated_t. */
static int prg_sweep_pick(const nr_prg_sweep_t *s) { return nr_arm_sweep_gated_pick(s, NR_PRG_ARMS); }
static int prg_sweep_feed(nr_prg_sweep_t *s, int arm, bool tb_ok, bool link_ok)
{
  return nr_arm_sweep_gated_feed(s, NR_PRG_ARMS, arm, tb_ok, link_ok);
}
/// arm -> PRB-bundling size (0/2/4), the field prg_sweep's arm index maps to.
static uint8_t nr_prg_arm_value(int arm) { return arm == 2 ? 4 : (arm == 1 ? 2 : 0); }
/* 64 slots, evidence-protected eviction (final review I4, the policy T1 gave nr_pdsch_config_sweep.c's
 * rnti_ctx()): a slot holding any trial, latch or walk is evicted only when EVERY slot holds evidence,
 * so a burst of one-off noise RNTIs can no longer throw away a real UE's latched VRB-L/PRG/PT-RS/n_L or
 * data-identity decision. SI-/RA-/P-RNTI grants never get a slot (rnti_sweepable()): those identities
 * are not UEs and carry no dedicated RRC configuration. ~2.1 KB/slot, 135 KB of BSS. */
#define RNTI_DEC_MAX 64
typedef struct { uint16_t rnti; uint64_t touched; int nl; nr_vrbl_sweep_t vrbl; nr_prg_sweep_t prg; nr_scrambling_id_sweep_t data_id; } rnti_dec_t;
static rnti_dec_t g_rnti_dec[RNTI_DEC_MAX];
static uint64_t g_rnti_dec_clock;
/* rnti_class is nr_blind_rnti_class_t: 2 = SI, 3 = RA, 4 = P. */
/* BLINDFIX F7: the MCS table of the cell from TB-CRC evidence instead of the config value.
 * A full-TB CRC pass under a DCI 1_1 grant supports every table that reads this MCS to the same (Qm, R) as the table the
 * decode used, and only counts when at least one table reads it differently (nbs_table_ev_*, sign test at alpha 1e-3). */
static _Atomic uint32_t g_mcs_tbl_pass[3];
static void mcs_table_evidence_feed(uint8_t mcs, uint8_t used)
{
  if (used > 2)
    return;
  bool same[3], differs = false;
  for (int t = 0; t < 3; t++) {
    same[t] = nr_get_Qm_dl(mcs, (uint8_t)t) == nr_get_Qm_dl(mcs, used) && nr_get_code_rate_dl(mcs, (uint8_t)t) == nr_get_code_rate_dl(mcs, used);
    differs |= !same[t];
  }
  for (int t = 0; t < 3 && differs; t++)
    if (same[t])
      atomic_fetch_add(&g_mcs_tbl_pass[t], 1);
}
int nr_pdsch_passive_mcs_table_evidence(void)
{
  nbs_table_ev_t e;
  for (int t = 0; t < 3; t++)
    e.pass[t] = atomic_load(&g_mcs_tbl_pass[t]);
  return nbs_table_ev_best(&e);
}
static bool rnti_sweepable(uint16_t rnti, uint8_t rnti_class)
{
  return rnti != 0 && rnti < 0xFFFE && rnti_class != 2 && rnti_class != 3 && rnti_class != 4;
}
/* Evidence = any latch or walk of this RNTI's own, or >= NR_ARM_SWEEP_LATCH_MIN_OK trials in total: a
 * one-off noise-floor RNTI (one or two false-accept grants) never reaches that, a real UE does within a
 * few grants -- a bare "any trial" test would let every noise RNTI protect itself after its first decode. */
static bool rnti_dec_evidence(const rnti_dec_t *r)
{
  if (r->nl || r->data_id.n > 0 || r->prg.explore || r->prg.s.latched >= 0 || r->vrbl.latched >= 0)
    return true;
  uint32_t tr = 0;
  for (int a = 0; a < NR_ARM_SWEEP_MAX; a++)
    tr += r->vrbl.tr[a] + r->prg.s.tr[a];
  return tr >= NR_ARM_SWEEP_LATCH_MIN_OK;
}
/* under g_ptrs_lock. NULL only when !create and the RNTI has no slot. */
static rnti_dec_t *rnti_dec_find(uint16_t rnti, bool create)
{
  int victim = -1;
  bool victim_ev = true;
  for (int i = 0; i < RNTI_DEC_MAX; i++) {
    if (g_rnti_dec[i].rnti == rnti) {
      g_rnti_dec[i].touched = ++g_rnti_dec_clock;
      return &g_rnti_dec[i];
    }
    const bool ev = g_rnti_dec[i].rnti && rnti_dec_evidence(&g_rnti_dec[i]);
    if (victim < 0 || (victim_ev && !ev) || (ev == victim_ev && g_rnti_dec[i].touched < g_rnti_dec[victim].touched)) {
      victim = i;
      victim_ev = ev;
    }
  }
  if (!create)
    return NULL;
  rnti_dec_t *r = &g_rnti_dec[victim];
  static _Atomic int s_evict_logs = 20; /* log budget shared by the passivePdsch consumers */
  if (r->rnti && victim_ev && s_evict_logs > 0) {
    s_evict_logs--;
    LOG_W(PHY, "SENSING: RNTI_DEC all %d slots hold evidence: evicted rnti=0x%04x for rnti=0x%04x\n", RNTI_DEC_MAX,
          r->rnti, rnti);
  }
  memset(r, 0, sizeof(*r));
  r->rnti = rnti;
  r->touched = ++g_rnti_dec_clock;
  r->vrbl.latched = -1; // tr/ok already zeroed by the memset above
  nr_arm_sweep_gated_init(&r->prg);
  return r;
}
static rnti_dec_t *rnti_dec(uint16_t rnti) { return rnti_dec_find(rnti, true); }
static int rnti_nl_get(uint16_t rnti, bool sweepable)
{
  if (!sweepable)
    return atomic_load(&g_lbrm_nl);
  pthread_mutex_lock(&g_ptrs_lock);
  const rnti_dec_t *r = rnti_dec(rnti);
  const int v = r->nl ? r->nl : atomic_load(&g_lbrm_nl);
  pthread_mutex_unlock(&g_ptrs_lock);
  return v;
}
/* Returns the value this RNTI read before the latch (for the log). */
static bool rnti_nl_latched(uint16_t rnti, bool sweepable)
{
  if (!sweepable)
    return false;
  pthread_mutex_lock(&g_ptrs_lock);
  const bool l = rnti_dec(rnti)->nl != 0;
  pthread_mutex_unlock(&g_ptrs_lock);
  return l;
}
static int rnti_nl_latch(uint16_t rnti, bool sweepable, int nl)
{
  if (!nr_cfg_epoch_work_current()) return nl;
  if (!sweepable)
    return nl; /* no per-RNTI state for SI/RA/P: nothing to latch, nothing to log */
  pthread_mutex_lock(&g_ptrs_lock);
  if (!nr_cfg_epoch_work_current()) { pthread_mutex_unlock(&g_ptrs_lock); return nl; }
  rnti_dec_t *r = rnti_dec(rnti);
  const int prev = r->nl ? r->nl : atomic_load(&g_lbrm_nl);
  r->nl = nl;
  if (atomic_load(&g_lbrm_nl) != nl)
    for (int i = 0; i < RNTI_DEC_MAX; i++)
      if (g_rnti_dec[i].rnti && g_rnti_dec[i].rnti != rnti && g_rnti_dec[i].nl == nl) {
        atomic_store(&g_lbrm_nl, nl);
        break;
      }
  pthread_mutex_unlock(&g_ptrs_lock);
  return prev;
}
/* Per-RNTI dataScramblingIdentityPDSCH walk (Task 13; gate redesigned by final review I1). Called ONLY
 * for dedicated-class grants (nr_scrambling_dedicated()): everything else uses N_ID^cell by spec.
 * `advance_ok` = nr_scrambling_walk_eligible() (DM-RS id of the grant's nSCID decided, link healthy,
 * >= NR_SCR_WALK_MIN_FAILS dedicated CRC fails), computed by the caller. It gates only whether the walk
 * may START or MOVE; a LATCHED result is always returned (review fix round 1, finding 1: gating the
 * latch on a stall measure threw the just-found identity away on the next grant). Every start, step,
 * wrap and latch is logged: a silent walk is indistinguishable from a broken link. */
static _Atomic int g_data_id_walks; /* RNTIs whose walk has started (or latched): the lock-free fast path below */
uint16_t nr_pdsch_passive_data_id_current(uint16_t rnti, uint16_t pci, int dmrs_id, bool advance_ok)
{
  /* Final review I8: this runs on the scan thread for every dedicated DL grant. On a cell where no walk
   * ever started (every deployment seen so far) it takes no lock at all. */
  if (!advance_ok && atomic_load_explicit(&g_data_id_walks, memory_order_acquire) == 0)
    return pci;
  pthread_mutex_lock(&g_ptrs_lock);
  rnti_dec_t *r = rnti_dec_find(rnti, advance_ok);
  int id = -1;
  if (r && r->data_id.n > 0 && r->data_id.latched >= 0) {
    id = r->data_id.latched; // always honour a latched result, regardless of advance_ok
  } else if (r && advance_ok) {
    if (r->data_id.n == 0) {
      nr_scrambling_id_sweep_init(&r->data_id, pci, dmrs_id);
      atomic_fetch_add_explicit(&g_data_id_walks, 1, memory_order_release);
      LOG_A(PHY, "SENSING: DATA_ID_WALK START rnti=0x%04x first candidate=%d (reason: DM-RS id %d decided, link healthy, "
                 ">= %d consecutive dedicated TB CRC fails)\n",
            rnti, nr_scrambling_id_sweep_current(&r->data_id), dmrs_id, NR_SCR_WALK_MIN_FAILS);
    }
    id = nr_scrambling_id_sweep_current(&r->data_id);
  }
  pthread_mutex_unlock(&g_ptrs_lock);
  return (uint16_t)(id >= 0 ? id : pci);
}
void nr_pdsch_passive_data_id_feed(uint16_t rnti, bool tb_crc_ok)
{
  if (!nr_cfg_epoch_work_current()) return;
  pthread_mutex_lock(&g_ptrs_lock);
  if (!nr_cfg_epoch_work_current()) { pthread_mutex_unlock(&g_ptrs_lock); return; }
  rnti_dec_t *r = rnti_dec_find(rnti, false);
  if (r && r->data_id.n > 0 && r->data_id.latched < 0) {
    const int tried = nr_scrambling_id_sweep_current(&r->data_id);
    /* A latch is permanent, so ONE pass is not enough: a wrong candidate can pass by accident (measured:
     * a QPSK/tbs=2024 trial hypothesis passed under n_id=121 after 122 tries on a second attached UE
     * that really uses the PCI; the latch then pinned 121 and every later decode of that UE failed).
     * The true id passes again within a few grants (92 % of its real-layout grants); a coincidence does
     * not. So hold the candidate after its first pass and latch on the second; a fail clears the hold. */
    static uint32_t s_pend_cand[65536]; /* guarded by g_ptrs_lock; 0 = none, else candidate+1 */
    int fed = tb_crc_ok ? 1 : 0;
    if (tb_crc_ok && s_pend_cand[rnti] != (uint32_t)tried + 1) {
      s_pend_cand[rnti] = (uint32_t)tried + 1; /* first pass at this candidate: stay on it, do not latch */
      pthread_mutex_unlock(&g_ptrs_lock);
      return;
    }
    if (!tb_crc_ok)
      s_pend_cand[rnti] = 0;
    nr_scrambling_id_sweep_feed(&r->data_id, fed);
    if (r->data_id.latched >= 0)
      LOG_A(PHY, "SENSING: DATA_ID_WALK LATCHED rnti=0x%04x n_id=%d after %u tries (reason: TB CRC pass)\n", rnti,
            r->data_id.latched, r->data_id.tries);
    else
      LOG_A(PHY, "SENSING: DATA_ID_WALK STEP rnti=0x%04x candidate=%d failed CRC -> next=%d (%d/%d)%s\n", rnti, tried,
            nr_scrambling_id_sweep_current(&r->data_id), r->data_id.pos, r->data_id.n,
            r->data_id.pos == 0 ? " WRAPPED: every candidate failed once, starting over from the PCI" : "");
  }
  pthread_mutex_unlock(&g_ptrs_lock);
}
static int rnti_vrbl_pick(uint16_t rnti)
{
  pthread_mutex_lock(&g_ptrs_lock);
  const int arm = vrbl_sweep_pick(&rnti_dec(rnti)->vrbl);
  pthread_mutex_unlock(&g_ptrs_lock);
  return arm;
}
static int rnti_vrbl_feed(uint16_t rnti, int arm, bool tb_ok)
{
  if (!nr_cfg_epoch_work_current()) return -1;
  pthread_mutex_lock(&g_ptrs_lock);
  if (!nr_cfg_epoch_work_current()) { pthread_mutex_unlock(&g_ptrs_lock); return -1; }
  const int latched = vrbl_sweep_feed(&rnti_dec(rnti)->vrbl, arm, tb_ok);
  pthread_mutex_unlock(&g_ptrs_lock);
  return latched;
}
int nr_pdsch_vrbl_pick(uint16_t rnti)
{
  return rnti_vrbl_pick(rnti) == 1 ? 4 : 2;
}
static int rnti_prg_pick(uint16_t rnti)
{
  pthread_mutex_lock(&g_ptrs_lock);
  const int arm = prg_sweep_pick(&rnti_dec(rnti)->prg);
  pthread_mutex_unlock(&g_ptrs_lock);
  return arm;
}
static int rnti_prg_feed(uint16_t rnti, int arm, bool tb_ok, bool link_ok, bool *explore_started)
{
  if (!nr_cfg_epoch_work_current()) return -1;
  pthread_mutex_lock(&g_ptrs_lock);
  if (!nr_cfg_epoch_work_current()) { pthread_mutex_unlock(&g_ptrs_lock); return -1; }
  nr_prg_sweep_t *p = &rnti_dec(rnti)->prg;
  const bool was = p->explore;
  const int latched = prg_sweep_feed(p, arm, tb_ok, link_ok);
  *explore_started = !was && p->explore;
  pthread_mutex_unlock(&g_ptrs_lock);
  return latched;
}

/* A segmented (prg 2/4) arm that cannot decode this grant at all (PT-RS, CSI-RS parity, bad list) is a
 * FAILED trial of that arm, not a non-event: unfed, its untried Wilson bound (1.0) would be picked on every
 * grant for ever (final review C1). Arm 0 never gets here. */
static void prg_arm_unsupported(uint16_t rnti, int prg_arm)
{
  if (prg_arm > 0) {
    bool explore_started;
    rnti_prg_feed(rnti, prg_arm, false, true, &explore_started);
  }
}

/* ---- DL CRC bookkeeping for the scrambling-identity walk and the PRG trigger (final review I1/C1).
 * Every decode path -- the deferred consumer AND the in-line decode in nr_pdcch_blind_monitor_rt.c --
 * reports each TB outcome here once (layout probes excluded: a code-block-0 probe is a layout
 * hypothesis, not evidence about the scrambling identity). */
static nr_scr_link_t g_dl_scr_link;
/* WINDOWED, resettable: DEDICATED-class (nr_scrambling_dedicated()) CRC fails since this RNTI's last
 * dedicated pass. Grants that use N_ID^cell (SIB1, RAR, CSS fallback) neither count nor reset it: they
 * pass under a wrong dedicated identity and would otherwise keep the walk from ever opening. */
static _Atomic uint32_t g_ded_fails_since_ok[65536];
/* A dedicated CRC pass PROVES the data identity in use (c_init = RNTI*2^15 + n_ID: any other n_ID fails
 * LDPC). So an RNTI that has ever passed has nothing left for the walk to find. Without this, the
 * "20 consecutive fails" trigger fires on failures that have nothing to do with the identity: the
 * Technique D sweep deliberately decodes grants under WRONG layout hypotheses, which fail by
 * construction, so a streak of 20 is reached within a second of a correct id. The walk then leaves the
 * right id after ONE failed look (that grant was a wrong-layout trial) and needs 1024 further failing
 * grants to come back: measured on the SA bed as a UE that decodes for ~10 s, then sweeps ids for
 * minutes with perfect EVM (0 % TB CRC) or recovers after a random time. */
static _Atomic uint8_t g_ded_ever_ok[65536];
void nr_pdsch_passive_crc_note(uint16_t rnti, bool dedicated, bool crc_ok)
{
  if (!nr_cfg_epoch_work_current()) return;
  nr_scr_link_note(&g_dl_scr_link, rnti, dedicated, crc_ok);
  if (!dedicated)
    return;
  if (crc_ok) {
    atomic_store_explicit(&g_ded_fails_since_ok[rnti], 0, memory_order_relaxed);
    atomic_store_explicit(&g_ded_ever_ok[rnti], 1, memory_order_relaxed);
  } else
    atomic_fetch_add_explicit(&g_ded_fails_since_ok[rnti], 1, memory_order_relaxed);
}
uint32_t nr_pdsch_passive_rnti_ded_fails(uint16_t rnti)
{
  /* ponytail: a dataScramblingIdentity change by RRC reconfiguration AFTER a pass is not re-walked until
   * the RNTI changes; clear g_ded_ever_ok on a sustained zero-pass window if that case ever appears. */
  if (atomic_load_explicit(&g_ded_ever_ok[rnti], memory_order_relaxed))
    return 0;
  return atomic_load_explicit(&g_ded_fails_since_ok[rnti], memory_order_relaxed);
}
bool nr_pdsch_passive_link_healthy(uint16_t rnti)
{
  return nr_scr_link_healthy(&g_dl_scr_link, rnti);
}
/* RBMAP: which RBs the cell actually allocated, for the dashboard's spectrum strip. One counter per
 * RB, incremented per accepted grant over its allocation, printed as 273 density digits and reset --
 * so the strip shows the LAST window, not the run average. */
#define NR_RBMAP_MAX 275
static _Atomic uint32_t g_rbmap[NR_RBMAP_MAX];
static _Atomic uint32_t g_rbmap_ok[NR_RBMAP_MAX];
static _Atomic uint64_t g_rbmap_grants;
static __thread uint32_t t_seg_ok_last = 0; // segments that decoded in the last TB on this thread
/* RANK2-ITER: LDPC iterations of the last body on this thread (EVMGRANT it= itc= its=): max OAI numIter over the code blocks (a block that hit the cap reports the cap;
 * 0 = no LDPC ran), the cap in force, and the sum over the code blocks that decoded. Reset at the start of every body. */
static __thread uint8_t t_ldpc_it_max = 0, t_ldpc_it_cap = 0;
static __thread uint16_t t_ldpc_it_sum = 0;
/* Decoder that produced the last TB on this thread (NRLDPC_DECODER_*, K34/K36). Unit = the TB. Evidence is
 * stratified by this value: CPU plain min-sum and CUDA normalised min-sum (2x iterations) differ by about 1 dB (K36), so counts, CRC
 * passes and eliminations from different values are NEVER merged. */
static __thread uint8_t t_decoder_used_last = 0;
/* CB0 elimination (nr_td_cb0_wire.c): force the CPU TB decoder (libldpc.so's segment decoder, version "") for this
 * thread's next decodes, whatever loader.ldpc.shlibversion loaded for the UE. decoder_used still reports what ran. */
static __thread bool t_tb_force_cpu = false;
static nrLDPC_coding_decoder_t *g_tb_cpu_dec;
static pthread_once_t g_tb_cpu_once = PTHREAD_ONCE_INIT;
static void tb_cpu_load(void)
{
  loader_shlibfunc_t f[] = {{.fname = "nrLDPC_coding_init"}, {.fname = "nrLDPC_coding_decoder"}};
  if (load_module_version_shlib("ldpc", "", f, 2, NULL) >= 0 && f[0].fptr && f[1].fptr
      && ((nrLDPC_coding_init_t *)f[0].fptr)(32) == 0)
    g_tb_cpu_dec = (nrLDPC_coding_decoder_t *)f[1].fptr;
  else
    LOG_W(NR_PHY, "SENSING: CB0 wiring: CPU TB decoder (libldpc.so) not loadable, TB decodes keep the configured decoder\n");
}
static nrLDPC_coding_decoder_t *tb_cpu_decoder(void)
{
  pthread_once(&g_tb_cpu_once, tb_cpu_load);
  return g_tb_cpu_dec;
}
void nr_pdsch_passive_force_cpu_tb(bool on) { t_tb_force_cpu = on; }
uint8_t nr_pdsch_passive_last_decoder_used(void) { return t_decoder_used_last; }
static _Atomic uint64_t g_ldpc_tb_dec[3]; // [decoder_used]: TBs handed to the decoder (all outcomes), 0 = not reported
void nr_pdsch_passive_ldpc_tb_decoders(uint64_t *cpu, uint64_t *cuda)
{
  *cpu = atomic_load(&g_ldpc_tb_dec[1]);
  *cuda = atomic_load(&g_ldpc_tb_dec[2]);
}
static __thread int t_last_sk = -1;        // last TB outcome for TBRESULT: 1 decoded, 0 zero_tb, 2 seg_fail
static __thread uint32_t t_last_llr_have, t_last_data_bits;
static _Atomic uint64_t g_shape_rv[3]  = {0, 0, 0};
static _Atomic uint64_t g_shape_G[3]   = {0, 0, 0};
/* Segmentation parameters, binned by outcome. §34.4's hypothesis: filler bits F are ZEROS by
 * definition, and the reassembly copies `(K>>3) - (F>>3) - (C>1?3:0)` bytes per segment. If K/F/C
 * are computed for a different TBS than was transmitted, the copied bytes come from the FILLER
 * region -- giving an all-zero transport block while every segment CRC still passes, because the
 * LDPC decoded its codeword correctly and only the extraction window is wrong. That fits confident
 * zeros + valid segment CRCs + tb_fail=0 + a dependence on TBS. */
static _Atomic uint64_t g_shape_K[3] = {0, 0, 0};
static _Atomic uint64_t g_shape_F[3] = {0, 0, 0};
static _Atomic uint64_t g_shape_C[3] = {0, 0, 0};
static _Atomic uint64_t g_shape_Z[3] = {0, 0, 0};
/* Outcome binned DIRECTLY by symbol count, replacing §35's inference from a mean-TBS difference.
 * This cell uses two TDA entries, sym=1+13 (dmrs_len=3, mask 0x884) and sym=1+7 (dmrs_len=2, mask
 * 0x84). A short grant has a different DM-RS pattern, hence different nb_re_dmrs and G, so an error
 * confined to the 7-symbol variant would produce confidently wrong soft bits on exactly that subset
 * and leave 13-symbol grants untouched. [outcome][0]=short(<=9 sym), [1]=long. */
static _Atomic uint64_t g_nsym[3][2] = {{0, 0}, {0, 0}, {0, 0}};
/* Set by passive_ldpc_decode() for the TB it just processed; read by the caller once the outcome is
 * known. Thread-local because several consumers decode concurrently. */
static __thread uint32_t t_seg_K = 0, t_seg_F = 0, t_seg_C = 0, t_seg_Z = 0;
/* Rate-matching / decoder inputs, carried out of passive_ldpc_decode() for the PIPEDIAG census.
 * These are the quantities that decide WHICH LLRs each segment is handed; everything upstream of
 * them has now been eliminated by measurement (EVM identical at 9.9 % and 78.1 % CRC), so if the
 * fault is a stream-offset problem it must show as one of these differing by outcome. */
static __thread uint32_t t_seg_E = 0, t_seg_R = 0, t_seg_lbrm = 0, t_seg_BG = 0;
static __thread int t_decode_tdra_attempt;

extern __thread bool nr_pdsch_decode_wide_mrc;
static _Atomic int64_t  g_llr_sgnsum[2] = {0, 0};
static _Atomic uint64_t g_llr_pos[2]    = {0, 0};
/* Positive-rate split by BIT POSITION within each 16QAM symbol. nr_16qam_llr() emits 4 LLRs per
 * symbol: b0,b1 are the SIGN bits (llr = y) and b2,b3 the MAGNITUDE bits (llr = ch_mag - |y|).
 * If the equalised symbols are systematically SMALL relative to ch_mag -- which is what a
 * DM-RS-to-data power offset that we do not account for would produce, since ch_mag is derived from
 * the DM-RS -- then the magnitude bits go CONFIDENTLY POSITIVE while the sign bits stay balanced,
 * and LDPC reads the result as all-zeros. That is precisely the observed signature (89-91 % positive
 * overall, confident, no convergence failure). This split tells the two apart:
 *   sign ~50 % and magnitude ~100 %  -> amplitude/scaling mismatch, NOT a data or sequence problem
 *   both ~equally biased             -> the payload really is zeros
 * [outcome][0]=sign bits (i%4<2), [1]=magnitude bits (i%4>=2). */
static _Atomic uint64_t g_llr_posbit[2][2] = {{0, 0}, {0, 0}};
static _Atomic uint64_t g_llr_nbit[2][2]   = {{0, 0}, {0, 0}};
static _Atomic uint64_t g_llr_tb[2]    = {0, 0};

static _Atomic uint64_t g_ldpc_seg_fail = 0;
static _Atomic uint64_t g_ldpc_tb_fail  = 0;
static _Atomic uint64_t g_ldpc_zero_tb  = 0;
static _Atomic uint64_t g_ldpc_ok       = 0;
void nr_pdsch_passive_ldpc_counters(uint64_t *ok, uint64_t *seg_fail, uint64_t *tb_fail, uint64_t *zero_tb)
{
  *ok = atomic_load(&g_ldpc_ok);
  *seg_fail = atomic_load(&g_ldpc_seg_fail);
  *tb_fail = atomic_load(&g_ldpc_tb_fail);
  *zero_tb = atomic_load(&g_ldpc_zero_tb);
}
static _Atomic uint64_t g_fep_hit = 0, g_fep_miss = 0, g_chest_hit = 0, g_chest_miss = 0; // per-slot sharing
static _Atomic uint64_t g_gpu_llr_jobs = 0, g_gpu_cpu_jobs = 0; // decodes fed by the GPU front end vs the CPU chain
static _Atomic uint64_t g_lbrm_try[5], g_lbrm_ok[5]; // per hypothesised n_L
static _Atomic uint64_t g_llr_norm_shift[9]; /* TBs by applied right shift */
static _Atomic uint64_t g_rv_census[2][4]; // [mcs>=24][rv]: does this cell retransmit at rv 0? (HARQ gate)
static _Atomic uint64_t g_ldpc_iface_err = 0;
static _Atomic uint64_t g_seg_ok_sum    = 0; // segments that decoded, summed over failing TBs
static _Atomic uint64_t g_seg_tot_sum   = 0; // C, summed over the same TBs
/* Per-SEGMENT-INDEX outcome, over every multi-segment TB. The aggregate says only that 18.5 % of
 * segments converge on a failing TB; it cannot distinguish the two explanations, which point at
 * completely different code:
 *   failures concentrated at HIGH r  -> the LLR stream is being consumed with a drifting offset,
 *                                       i.e. rate de-matching / E / k0 / segment extraction
 *   failures UNIFORM across r        -> every segment sees equally bad soft input, i.e. the fault
 *                                       is common to the TB and not an indexing walk
 * Recorded for every decoded TB, not just failing ones, so the rate is per-index and not a
 * histogram of where failures happen to be dense. */
#define NR_PDSCH_SEGIDX_MAX 32
static _Atomic uint64_t g_segidx_tot[NR_PDSCH_SEGIDX_MAX];
static _Atomic uint64_t g_segidx_fail[NR_PDSCH_SEGIDX_MAX];
/* Same histogram CONDITIONED ON C. Unconditioned, index and TB size are confounded: r0 is counted
 * over every TB while r>=3 exists only in C>=4 TBs, so the apparent "r3-r5 fail at 80 % whatever
 * the run does" could equally be "large TBs fail". Three buckets: C<=2, C in 3..4, C>=5. */
#define NR_PDSCH_CBUCKETS 3
static _Atomic uint64_t g_segidxc_tot[NR_PDSCH_CBUCKETS][NR_PDSCH_SEGIDX_MAX];
static _Atomic uint64_t g_segidxc_fail[NR_PDSCH_CBUCKETS][NR_PDSCH_SEGIDX_MAX];
static inline int nr_pdsch_cbucket(uint32_t C) { return (C <= 2) ? 0 : ((C <= 4) ? 1 : 2); }

/* ---- PIPEDIAG: outcome-attributed census of the WHOLE downstream chain -----------------------
 * Everything from the equaliser onwards, split DECODED vs FAILED, so the stage where the two
 * populations diverge names itself instead of being guessed at. Upstream is already excluded by
 * measurement: EVM is ~10 % in BOTH a 9.9 % and a 78.1 % CRC run, residual CFO is +31 Hz, SFO
 * +2.2 ppm, and int8 clipping is HIGHER in the best run than the worst.
 * The load-bearing entry is llr_have vs G: G is what nr_get_G() budgets and what the rate matcher
 * consumes, while llr_have is how many LLRs nr_rx_pdsch() actually produced. If those disagree,
 * every segment after the shortfall is fed stale or zero soft input -- which is exactly the
 * signature seen (healthy magnitudes, catastrophic non-convergence, tb_fail = 0). */
#define PIPE_N_FIELDS 14
static _Atomic uint64_t g_pipe_n[2];
static _Atomic uint64_t g_pipe_sum[2][PIPE_N_FIELDS];
static const char *const kPipeName[PIPE_N_FIELDS] = {
    "G", "llr_have", "valid_re", "re_x_Qm_Nl", "C", "K", "Z", "F", "E", "R", "lbrm", "Qm", "Nl", "nsym"};

/* nr_pdsch_passive_ldpc_stats_dump: prints the passive decoder's accumulated statistics: GPU/CPU FEP job counts, slot-share hit rates, LDPC and LLR diagnostics.
 *   Output is LOG_I lines tagged SENSING (LLRDIAG, RBHIST, MCSHIST, HARQC, PTRS_SWEEP, PIPEDIAG, SEGIDX, ...) and reads only atomics.
 *   Caller: nr_pdcch_blind_monitor_rt.c (periodic summary).
 */
void nr_pdsch_passive_ldpc_stats_dump(void)
{
  const uint64_t sf = atomic_load(&g_ldpc_seg_fail), tf = atomic_load(&g_ldpc_tb_fail);
  LOG_I(PHY, "SENSING: GPU_FEP jobs gpu=%lu cpu=%lu\n", (unsigned long)atomic_load(&g_gpu_llr_jobs),
        (unsigned long)atomic_load(&g_gpu_cpu_jobs));
  LOG_I(PHY, "SENSING: SLOTSHARE fep hit/miss=%lu/%lu chest hit/miss=%lu/%lu\n",
        (unsigned long)atomic_load(&g_fep_hit), (unsigned long)atomic_load(&g_fep_miss),
        (unsigned long)atomic_load(&g_chest_hit), (unsigned long)atomic_load(&g_chest_miss));
  const uint64_t zt = atomic_load(&g_ldpc_zero_tb), ok = atomic_load(&g_ldpc_ok);
  const uint64_t ie = atomic_load(&g_ldpc_iface_err);
  const uint64_t so = atomic_load(&g_seg_ok_sum), st = atomic_load(&g_seg_tot_sum);
  for (int k = 0; k < 2; k++) {
    const uint64_t n = atomic_load(&g_llr_n[k]);
    if (n == 0) {
      continue;
    }
    LOG_I(PHY,
          "SENSING: LLRDIAG %s tbs=%lu n=%lu mean_abs=%.1f mean_signed=%+.2f pos=%.2f%% "
          "zero=%.2f%% saturated=%.4f%%\n",
          k ? "DECODED" : "FAILED  ", (unsigned long)atomic_load(&g_llr_tb[k]), (unsigned long)n,
          (double)atomic_load(&g_llr_absum[k]) / (double)n,
          (double)atomic_load(&g_llr_sgnsum[k]) / (double)n,
          100.0 * (double)atomic_load(&g_llr_pos[k]) / (double)n,
          100.0 * (double)atomic_load(&g_llr_zero[k]) / (double)n,
          100.0 * (double)atomic_load(&g_llr_sat[k]) / (double)n);
    LOG_I(PHY, "SENSING: LLRCLIP %s clipped_at_int8=%.2f%% (|llr|>127 is flattened to +-127 by "
               "simde_mm_packs_epi16 before the decoder)\n",
          k ? "DECODED" : "FAILED  ",
          100.0 * (double)atomic_load(&g_llr_clip8[k]) / (double)n);
  }
  {
    /* The direct test: all-zero RATE for short versus long grants. If short grants are dramatically
     * worse, the bug is in the 7-symbol TDA handling. */
    const uint64_t zs = atomic_load(&g_nsym[0][0]), zl = atomic_load(&g_nsym[0][1]);
    const uint64_t ds = atomic_load(&g_nsym[1][0]), dl = atomic_load(&g_nsym[1][1]);
    const uint64_t fs = atomic_load(&g_nsym[2][0]), fl = atomic_load(&g_nsym[2][1]);
    if (zs + zl + ds + dl + fs + fl > 0) {
      LOG_I(PHY,
            "SENSING: NSYM short(<=9sym): zero=%lu decoded=%lu segfail=%lu -> zero_rate=%.1f%% | "
            "long(>9sym): zero=%lu decoded=%lu segfail=%lu -> zero_rate=%.1f%%\n",
            (unsigned long)zs, (unsigned long)ds, (unsigned long)fs,
            (zs + ds) ? 100.0 * (double)zs / (double)(zs + ds) : 0.0,
            (unsigned long)zl, (unsigned long)dl, (unsigned long)fl,
            (zl + dl) ? 100.0 * (double)zl / (double)(zl + dl) : 0.0);
    }
  }
  {
    static const char *const kNm[3] = {"ZERO_TB ", "DECODED ", "SEG_FAIL"};
    for (int k = 0; k < 3; k++) {
      const uint64_t n = atomic_load(&g_shape_n[k]);
      if (n == 0) {
        continue;
      }
      const double dn = (double)n;
      const double mK = (double)atomic_load(&g_shape_K[k]) / dn;
      const double mF = (double)atomic_load(&g_shape_F[k]) / dn;
      const double mC = (double)atomic_load(&g_shape_C[k]) / dn;
      LOG_I(PHY,
            "SENSING: SHAPE %s n=%lu mean_tbs=%.0f mean_prb=%.1f mean_G=%.0f mean_rv=%.3f "
            "K=%.0f F=%.0f C=%.2f Z=%.0f F/K=%.4f payload_bytes=%.0f\n",
            kNm[k], (unsigned long)n,
            (double)atomic_load(&g_shape_tbs[k]) / dn,
            (double)atomic_load(&g_shape_rb[k]) / dn,
            (double)atomic_load(&g_shape_G[k]) / dn,
            (double)atomic_load(&g_shape_rv[k]) / dn,
            mK, mF, mC, (double)atomic_load(&g_shape_Z[k]) / dn,
            (mK > 0.0) ? mF / mK : 0.0,
            /* what the reassembly actually copies per segment, times C -- if this drifts away from
             * TBS/8 for the all-zero population, the extraction window is the bug. */
            mC * ((mK / 8.0) - (mF / 8.0) - ((mC > 1.0) ? 3.0 : 0.0)));
    }
  }
  for (int k = 0; k < 2; k++) {
    const uint64_t ns = atomic_load(&g_llr_nbit[k][0]), nm = atomic_load(&g_llr_nbit[k][1]);
    if (ns == 0 || nm == 0) {
      continue;
    }
    LOG_I(PHY, "SENSING: BITPOS %s sign_bits_pos=%.2f%% magnitude_bits_pos=%.2f%%\n",
          k ? "DECODED" : "FAILED  ",
          100.0 * (double)atomic_load(&g_llr_posbit[k][0]) / (double)ns,
          100.0 * (double)atomic_load(&g_llr_posbit[k][1]) / (double)nm);
  }
  {
    /* RBHIST: per-TB CRC as a FUNCTION of allocation size, not as a mean. Read the rate column --
     * a smooth ramp is a link-margin gradient, a step is a shape bug. */
    char hb[512];
    size_t u = 0;
    for (int b = 0; b < NR_PDSCH_RBHIST_BINS && u < sizeof(hb) - 40; b++) {
      const uint64_t okn = atomic_load(&g_rbhist[1][b]);
      const uint64_t fn  = atomic_load(&g_rbhist[2][b]) + atomic_load(&g_rbhist[0][b]);
      if (okn + fn == 0) {
        continue;
      }
      u += snprintf(hb + u, sizeof(hb) - u, "%d-%d:%.0f%%(%lu) ",
                    b * 273 / NR_PDSCH_RBHIST_BINS, (b + 1) * 273 / NR_PDSCH_RBHIST_BINS - 1,
                    100.0 * (double)okn / (double)(okn + fn), (unsigned long)(okn + fn));
    }
    if (u > 0) {
      LOG_I(PHY, "SENSING: RBHIST crc_ok%% by PRB alloc: %s\n", hb);
    }
  }
  for (int w = 0; w < 2; w++) {
    char mb[640];
    size_t u = 0;
    for (int m = 0; m < 32 && u < sizeof(mb) - 32; m++) {
      const uint64_t okn = atomic_load(&g_mcshist[w][1][m]);
      const uint64_t fn  = atomic_load(&g_mcshist[w][2][m]) + atomic_load(&g_mcshist[w][0][m]);
      if (okn + fn == 0)
        continue;
      const uint64_t sg = atomic_load(&g_mcs_segs[m]), sgok = atomic_load(&g_mcs_segs_ok[m]);
      u += snprintf(mb + u, sizeof(mb) - u, "%d:%.0f%%(%lu,segs %.0f%%,tbs %lu,rb %lu) ", m, 100.0 * (double)okn / (double)(okn + fn),
                    (unsigned long)(okn + fn), sg ? 100.0 * (double)sgok / (double)sg : 0.0,
                    (unsigned long)(atomic_load(&g_mcs_tbs[m]) / (okn + fn)), (unsigned long)(atomic_load(&g_mcs_rb[m]) / (okn + fn)));
    }
    if (u > 0)
      LOG_I(PHY, "SENSING: MCSHIST %s crc_ok%% by mcs: %s\n", w ? "WIDE(>=128prb)" : "NARROW(<128prb)", mb);
  }
  {
    uint64_t init_tx_hits, init_tx_evicts;
    pthread_mutex_lock(&g_harqc_lock);
    init_tx_hits   = g_dl_harq_init.hits;
    init_tx_evicts = g_dl_harq_init.evicts;
    pthread_mutex_unlock(&g_harqc_lock);
    LOG_I(PHY, "SENSING: HARQC first=%lu retx_combined=%lu/%lu tbs_from_first=%lu busy_skip=%lu | init_tx hit=%lu evict=%lu | dev_blocks evict=%lu nospace=%lu | rv census mcs<24 [%lu %lu %lu %lu] mcs>=24 [%lu %lu %lu %lu] | lbrm n_L=%d try/ok 4:%lu/%lu 2:%lu/%lu 1:%lu/%lu | llr_norm shift0..4 [%lu %lu %lu %lu %lu]\n",
          (unsigned long)atomic_load(&g_hq_first), (unsigned long)atomic_load(&g_hq_retx_ok),
          (unsigned long)atomic_load(&g_hq_retx_try), (unsigned long)atomic_load(&g_hq_tbs_override),
          (unsigned long)atomic_load(&g_hq_busy_skip),
          (unsigned long)init_tx_hits, (unsigned long)init_tx_evicts, (unsigned long)atomic_load(&g_hqb_evict), (unsigned long)atomic_load(&g_hqb_nospace),
          (unsigned long)atomic_load(&g_rv_census[0][0]), (unsigned long)atomic_load(&g_rv_census[0][1]),
          (unsigned long)atomic_load(&g_rv_census[0][2]), (unsigned long)atomic_load(&g_rv_census[0][3]),
          (unsigned long)atomic_load(&g_rv_census[1][0]), (unsigned long)atomic_load(&g_rv_census[1][1]),
          (unsigned long)atomic_load(&g_rv_census[1][2]), (unsigned long)atomic_load(&g_rv_census[1][3]),
          atomic_load(&g_lbrm_nl), (unsigned long)atomic_load(&g_lbrm_try[4]), (unsigned long)atomic_load(&g_lbrm_ok[4]),
          (unsigned long)atomic_load(&g_lbrm_try[2]), (unsigned long)atomic_load(&g_lbrm_ok[2]),
          (unsigned long)atomic_load(&g_lbrm_try[1]), (unsigned long)atomic_load(&g_lbrm_ok[1]),
          (unsigned long)atomic_load(&g_llr_norm_shift[0]), (unsigned long)atomic_load(&g_llr_norm_shift[1]),
          (unsigned long)atomic_load(&g_llr_norm_shift[2]), (unsigned long)atomic_load(&g_llr_norm_shift[3]),
          (unsigned long)atomic_load(&g_llr_norm_shift[4]));
  }
  {
    /* Density per RB as one digit 0-9 relative to the busiest RB in this window, plus the CRC-OK
     * share on the same axis. Reset after printing: the strip is a live picture, not a run total. */
    uint32_t occ[NR_RBMAP_MAX], okc[NR_RBMAP_MAX], mx = 0;
    for (int rb = 0; rb < NR_RBMAP_MAX; rb++) {
      occ[rb] = atomic_exchange(&g_rbmap[rb], 0);
      okc[rb] = atomic_exchange(&g_rbmap_ok[rb], 0);
      if (occ[rb] > mx) mx = occ[rb];
    }
    const uint64_t ng = atomic_exchange(&g_rbmap_grants, 0);
    if (mx > 0) {
      char ob[NR_RBMAP_MAX + 1], kb[NR_RBMAP_MAX + 1];
      int n = 0;
      for (int rb = 0; rb < NR_RBMAP_MAX; rb++, n++) {
        ob[n] = (char)('0' + (occ[rb] * 9 + mx / 2) / mx);
        kb[n] = occ[rb] ? (char)('0' + (okc[rb] * 9 + occ[rb] / 2) / occ[rb]) : '0';
      }
      ob[n] = kb[n] = '\0';
      LOG_I(PHY, "SENSING: RBMAP dl grants=%llu peak=%u occ=%s crc=%s\n",
            (unsigned long long)ng, mx, ob, kb);
    }
  }
  {
    /* PIPEDIAG: one line per outcome, every downstream quantity as a mean. Read it by DIFFING the
     * two rows: any field that differs between DECODED and FAILED is the stage that matters, and
     * every field that matches is eliminated. */
    for (int k = 1; k >= 0; k--) {
      const uint64_t n = atomic_load(&g_pipe_n[k]);
      if (n == 0) {
        continue;
      }
      char pb[512];
      size_t u = 0;
      for (int f = 0; f < PIPE_N_FIELDS && u < sizeof(pb) - 32; f++) {
        u += snprintf(pb + u, sizeof(pb) - u, "%s=%.1f ", kPipeName[f],
                      (double)atomic_load(&g_pipe_sum[k][f]) / (double)n);
      }
      LOG_I(PHY, "SENSING: PIPEDIAG %s n=%lu %s\n", k ? "DECODED" : "FAILED  ",
            (unsigned long)n, pb);
    }
  }
  {
    /* SEGIDX: per-segment-index failure rate. A rising trend with r means the LLR stream is being
     * walked with a drifting offset (rate de-matching / E / k0); a flat profile means every segment
     * gets equally bad input and the indexing is fine. */
    char sb[420];
    size_t u = 0;
    for (int r = 0; r < NR_PDSCH_SEGIDX_MAX && u < sizeof(sb) - 24; r++) {
      const uint64_t t = atomic_load(&g_segidx_tot[r]);
      if (t < 100) {
        continue;  // too few samples at this index to read a rate from
      }
      u += snprintf(sb + u, sizeof(sb) - u, "r%d:%.0f%%(%lu) ", r,
                    100.0 * (double)atomic_load(&g_segidx_fail[r]) / (double)t, (unsigned long)t);
    }
    if (u > 0) {
      LOG_I(PHY, "SENSING: SEGIDX fail%% by segment index: %s\n", sb);
    }
    /* Within one C bucket the TBs are the same size, so any remaining trend with r is a genuine
     * index effect and not the size confound. */
    static const char *const kCB[NR_PDSCH_CBUCKETS] = {"C<=2", "C=3-4", "C>=5"};
    for (int cb = 0; cb < NR_PDSCH_CBUCKETS; cb++) {
      char cbuf[300];
      size_t v = 0;
      for (int r = 0; r < NR_PDSCH_SEGIDX_MAX && v < sizeof(cbuf) - 24; r++) {
        const uint64_t t = atomic_load(&g_segidxc_tot[cb][r]);
        if (t < 100) {
          continue;
        }
        v += snprintf(cbuf + v, sizeof(cbuf) - v, "r%d:%.0f%%(%lu) ", r,
                      100.0 * (double)atomic_load(&g_segidxc_fail[cb][r]) / (double)t,
                      (unsigned long)t);
      }
      if (v > 0) {
        LOG_I(PHY, "SENSING: SEGIDXC %s %s\n", kCB[cb], cbuf);
      }
    }
  }
  LOG_I(PHY,
        "SENSING: LDPCDIAG ok=%lu seg_fail=%lu tb_fail=%lu zero_tb=%lu iface_err=%lu "
        "segs_decoded=%lu/%lu (%.1f%%)\n",
        (unsigned long)ok, (unsigned long)sf, (unsigned long)tf, (unsigned long)zt,
        (unsigned long)ie, (unsigned long)so, (unsigned long)st,
        st ? (100.0 * (double)so / (double)st) : 0.0);

}


static __thread passive_harq_t g_harq; // zero-initialised per thread

static bool passive_harq_prepare(passive_harq_t *h, int n_rb_dl)
{
  if (h->b != NULL && h->n_rb_dl == n_rb_dl) {
    return true;
  }
  free(h->b);
  free(h->c);
  free(h->d);
  h->b = NULL;
  h->c = NULL;
  h->d = NULL;

  // Same formula as nr_init_dl_harq_processes(): the number of segments scales with bandwidth.
  uint32_t a_segments = MAX_NUM_NR_DLSCH_SEGMENTS;
  if (n_rb_dl != 273) {
    a_segments = (a_segments * (uint32_t)n_rb_dl) / 273 + 1;
  }

  h->b = (uint8_t *)malloc16_clear(a_segments * 1056);
  h->c = (uint8_t *)malloc16(a_segments * sizeof(*h->c) * 1056);
  h->d = (int16_t *)malloc16(a_segments * sizeof(*h->d) * 3 * 8448);
  if (h->b == NULL || h->c == NULL || h->d == NULL) {
    free(h->b);
    free(h->c);
    free(h->d);
    h->b = NULL;
    h->c = NULL;
    h->d = NULL;
    LOG_E(NR_PHY, "SENSING: passive PDSCH decode -- could not allocate the private HARQ buffers\n");
    return false;
  }
  init_abort(&h->abort_decode);
  h->a_segments = a_segments;
  h->n_rb_dl    = n_rb_dl;
  return true;
}

/// LDPC decode of one transport block against the private HARQ context. Mirrors
/// nr_dlsch_decoding()'s parameter assembly; differs ONLY in that every piece of per-process state
/// it reads or writes lives in `h` instead of ue->dl_harq_processes[][], and in that this is always
/// a first (and only) HARQ round -- a passive receiver has no soft buffer from an earlier grant it
/// never saw, so there is nothing to combine and `d_to_be_cleared` is unconditionally true.
/* ---- HARQ SOFT COMBINING (V2) -------------------------------------------------------------------
 * A stateless receiver cannot decode a retransmission: rv 2/3 alone is mostly parity (measured OTA:
 * ISAC_RV_RETRY rescued 0 of 150k), and the failed first transmission it should be combined with was
 * thrown away. The LDPC segment decoder already accumulates into its soft buffer when
 * d_to_be_cleared is false -- exactly what the attached UE does -- so what was missing is a soft
 * buffer that SURVIVES between grants. A small shared cache keyed by (rnti, harq pid): a grant whose
 * NDI did not toggle for that process is a retransmission and is decoded INTO the stored buffer with
 * the first transmission's TBS. Only the FIRST decode call per TB uses it (retries must not add the
 * same LLRs twice), and a busy entry is skipped rather than waited on (a consumer must never block
 * behind another's decode). Evicted least-recently-used; 64 entries (T26c, was 16) cover the live HARQ processes
 * of a busy cell (LRU beyond that). */
#define NR_HARQC_N 64
#define NR_HARQC_RV0_EVIDENCE 64 /* same-NDI rv-0 grants on undecoded processes before rv-0 combining is admitted */
typedef struct {
  _Atomic int busy;
  bool used, soft_valid;
  uint16_t rnti;
  uint8_t pid, ndi;
  int64_t first_slot; /* absolute slot of the first transmission of the stored TB (a rescue retires only that TB) */
  uint32_t tbs;
  uint64_t last;
  int16_t *d;
  size_t cap;
  /* the soft bits of a failed TB live on the host (d) or, when on_gpu, in the device HARQ blocks blk[0 .. gC) of the GPU pass (nr_gpu_pass.h): gN entries each, block r at r * gKcz on the host.
   * hb holds the blocks taken from the pool (g_hqb_pool) while the sum is on the device. */
  bool on_gpu;
  uint8_t gC;
  uint32_t gN, gKcz;
  nr_hq_blocks_t hb;
} harqc_entry_t;
static harqc_entry_t g_harqc[NR_HARQC_N];
/* g_harqc_lock and g_dl_harq_init (the reserved-MCS retransmission record that shares this lock)
 * are declared much earlier in this file, alongside g_hq_first and friends -- see the comment
 * there for why. */
static uint64_t g_harqc_clock;
/* ---- the device HARQ blocks (NR_GPU_HQ_BLK of NR_GPU_HQ_SEG int16, one code block each), handed to the entries that keep a failed TB's soft sum on the device. Under g_harqc_lock. ---- */
static nr_hq_pool_t g_hqb_pool;
static void hqb_release(harqc_entry_t *e) /* lock held */
{
  nr_hq_pool_put(&g_hqb_pool, &e->hb);
  e->on_gpu = false;
}
static void hqb_release_locked(harqc_entry_t *e)
{
  pthread_mutex_lock(&g_harqc_lock);
  hqb_release(e);
  pthread_mutex_unlock(&g_harqc_lock);
}
/* the idle entry that held the blocks longest (its sum is dropped) */
static nr_hq_blocks_t *hqb_victim(void *ctx, const nr_hq_blocks_t *except)
{
  (void)ctx;
  harqc_entry_t *v = NULL;
  for (int i = 0; i < NR_HARQC_N; i++) {
    harqc_entry_t *x = &g_harqc[i];
    if (&x->hb != except && x->hb.n > 0 && !atomic_load(&x->busy) && (v == NULL || x->last < v->last))
      v = x;
  }
  if (v == NULL)
    return NULL;
  v->soft_valid = false;
  v->on_gpu = false;
  return &v->hb;
}
/* gives e (busy, held by the caller) C blocks; takes the blocks of the least recently used idle entries when the pool is short (their sums are dropped). 0 = e has >= C blocks. */
static int hqb_ensure(harqc_entry_t *e, int C)
{
  pthread_mutex_lock(&g_harqc_lock);
  if (e->hb.n < C && e->on_gpu && e->soft_valid)
    e->soft_valid = false; /* growing a live sum is not possible: its blocks are the old ones */
  int ev = 0;
  const int rc = nr_hq_pool_ensure(&g_hqb_pool, &e->hb, C, hqb_victim, NULL, &ev);
  atomic_fetch_add(&g_hqb_evict, (uint64_t)ev);
  if (rc != 0)
    atomic_fetch_add(&g_hqb_nospace, 1);
  pthread_mutex_unlock(&g_harqc_lock);
  return rc;
}
static __thread struct { int armed; uint16_t rnti; uint8_t pid, ndi; int64_t slot; } t_hq;
static __thread int16_t *t_hq_d = NULL;
static __thread uint16_t t_csi_prb_map[NR_PRB_SET_MAX]; /* CSI-FULL: virtual -> physical PRB of a segmented grant */
/* CSI-FULL: the real CRB per virtual RB of a segmented grant, for the extractor (nr_dlsch_csi_prb_map); the caller clears it after the demod */
static void gp_seg_csi_map(const nr_prb_seg_t *seg, int nseg, const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config, const freq_alloc_bitmap_t *freq_alloc)
{
  if (!nr_csirs_full_enabled() || !dlsch_config->numCsiRsForRateMatching)
    return;
  for (int s = 0; s < nseg; s++)
    for (int r = 0; r < seg[s].n_prb && seg[s].data_index + r < NR_PRB_SET_MAX; r++)
      t_csi_prb_map[seg[s].data_index + r] = seg[s].prb_start + r;
  nr_dlsch_csi_prb_map = t_csi_prb_map;
  nr_dlsch_csi_prb_map_n = freq_alloc->num_rbs;
}
static __thread bool t_probe_first_seg = false; /* decode segment 0 only; outcome in t_probe_seg_ok */
static __thread bool t_probe_seg_ok = false;
void nr_pdsch_passive_probe_mode(bool on) { t_probe_first_seg = on; t_probe_seg_ok = false; }
static bool pdtim_is_probe(void) { return t_probe_first_seg; }
void nr_pdsch_passive_ptrs_sweep_allow(bool on) { (void)on; } /* NEEDS: callers in nr_pdsch_passive_queue.c and the declaration in nr_pdsch_passive_decode.h go, the PT-RS sweep is deleted */
bool nr_pdsch_passive_probe_outcome(void) { return t_probe_seg_ok; }

static __thread const int16_t *t_last_llr = NULL;
static __thread uint32_t t_last_G = 0;
static __thread double t_last_fep_fo = 0.0; /* FEP frequency offset of the last decode (the recovery arms re-use it) */
uint32_t nr_pdsch_passive_last_llr(const int16_t **p) { *p = t_last_llr; return t_last_G; }
/* RECOVERY_ASYNC (Task 6): 1 = consumer: a failed first pass whose recovery arms would run is returned as it is and flagged deferred;
 * 2 = the pdschRecov worker: runs the arms (S toggle first, no backlog yield). 0 = unchanged behaviour. */
static __thread int t_recov_mode;
static __thread bool t_recov_deferred;
void nr_pdsch_passive_recov_mode(int mode) { t_recov_mode = mode; t_recov_deferred = false; }
bool nr_pdsch_passive_recov_deferred(void) { return t_recov_deferred; }
/* retry arms vs HARQ combining: allowed inline only without combining; with it only in the async lane (nr_hq_retry.h) */
#define RETRY_ARMS_OK() nr_hq_retry_lane(true, t_recov_mode)
static __thread c16_t **t_owned_iq;
static __thread int t_owned_slot = -1;
static __thread uint64_t t_fep_done_ns;
/* nr_pdsch_passive_set_owned_iq: sets the thread-local pointer to the slot IQ buffer that this consumer owns for one slot (rx == NULL clears it).
 *   Lets the decoder read the samples it already transformed instead of the live RX buffer.
 *   Callers: nr_pdsch_passive_queue.c (set before a job, clear after) and tests/nr_ssb_rate_match_prod_check.c.
 */
void nr_pdsch_passive_set_owned_iq(c16_t **rx, int slot)
{
  t_owned_iq = rx;
  t_owned_slot = rx ? slot : -1;
}
/* nr_pdsch_passive_iq_for_slot: returns the owned IQ buffer when slot matches the one registered by nr_pdsch_passive_set_owned_iq, otherwise the live buffer.
 *   Used by the passive queue (FEP and debug dump) so that the same samples feed both the FEP and the channel estimate.
 */
c16_t **nr_pdsch_passive_iq_for_slot(int slot, c16_t **live)
{
  return t_owned_iq && slot == t_owned_slot ? t_owned_iq : live;
}
/* nr_pdsch_passive_last_fep_ns: returns the time (ns) at which the last FEP of this thread finished, as recorded in t_fep_done_ns.
 *   Read by the passive queue for CPU-cost accounting. Thread-local, no locking.
 */
uint64_t nr_pdsch_passive_last_fep_ns(void) { return t_fep_done_ns; }

static __thread const int16_t *t_llr_ovr = NULL;
static __thread uint32_t t_llr_ovr_n = 0;
void nr_pdsch_passive_set_llr_override(const int16_t *llr, uint32_t n) { t_llr_ovr = n ? llr : NULL; t_llr_ovr_n = n; }

/* ---- GRANTWORK-LITE (ISAC_TD_GRANTWORK=1; levers spec 9.1, plan R1) ---------------------------------
 * A decode with a GrantWork attached (nr_pdsch_passive_set_grantwork) looks its geometry signature up in
 * it before the FEP. READY: the FEP, channel estimate, equaliser and LLR stages (and descramble + scale
 * normalisation) are skipped and the immutable decoder-input LLRs of the entry are used -- the LDPC tail
 * (HARQ, rv / LBRM retries, sweeps, stats) is unchanged. OWNER (first decode of that signature): the
 * decode runs exactly as without GrantWork (the FEP covers the whole slot once) and publishes its
 * decoder-input LLRs right before the LDPC. Not used for: GPU-fed decodes, segmented (PRB-list / PRG)
 * grants, PT-RS, CSI-RS rate matching, a grant that differs from the gw's own, and (unless
 * ISAC_TD_GW_PROBE=1) layout probes, which keep their probe horizon. */
static __thread nr_td_grantwork_t *t_gw = NULL;
static __thread bool t_gw_capture = false;  /* compute-only (the lazy entry computation): publish, then return */
static __thread bool t_gw_probe_ok = false; /* ISAC_TD_GW_PROBE: a layout probe may take its LLRs from the gw */
static __thread bool t_gw_owner = false;    /* this decode owns the entry t_gw_sig (publish or abandon) */
static __thread uint64_t t_gw_sig = 0;
static __thread bool t_gw_used = false, t_gw_ready = false; /* last decode: LLRs via the gw / served READY */
/* Reruns (equivalence checks, the lazy computation): no learning-state side effect -- no PT-RS / PRG
 * pick, no HARQ soft combining, no sweep feed, no LBRM latch, no BRANCHFO commit, no HARQ record. */
static __thread bool t_quiet = false;
static __thread bool t_last_hq_retx = false; /* last decode combined a retransmission into a soft buffer */

/* Published with each entry: what the skipped stages produced that the rest of the decode reads. */
typedef struct {
  uint32_t nvar;
  uint8_t qm_measured;
  uint32_t dl_valid_re[NR_SYMBOLS_PER_SLOT];
  uint64_t llr_n, llr_absum, llr_zero, llr_sat, llr_clip8, llr_pos;
  int64_t llr_sgnsum;
  uint64_t posbit[2], nbit[2];
  uint64_t fp; /* grant fingerprint (sanity) */
} gw_meta_t;
_Static_assert(sizeof(gw_meta_t) <= NR_TD_GW_META_MAX, "gw meta fits");

/* Everything the lazy computation needs to decode the gw's grant under another geometry. */
typedef struct {
  PHY_VARS_NR_UE *ue;
  UE_nr_rxtx_proc_t proc;
  fapi_nr_dl_config_dlsch_pdu_rel15_t pdu; /* pristine (as enqueued, before any decode wrote into it) */
  freq_alloc_bitmap_t fa;
  nr_pdsch_passive_grant_t grant;
  double fo_hz;
  uint8_t k0;
  uint64_t fp;
  uint32_t config_epoch; /* original PDSCH admission stamp for lazy work */
  uint8_t main_decoder_used; /* decoder_used of the job's main decode (0 = not known yet) */
} gw_ctx_t;

/* gw_fingerprint: hashes the grant-constant inputs of the receive chain (RNTI, slot, scrambling, DM-RS, BWP, allocation, frequency offset, receiver flags) with FNV-1a.
 *   A decode may share a GrantWork entry only when its fingerprint equals the entry's (env-dependent fields included: MRC/IRC/QAM override, CSI-RS extent).
 *   Callers: passive_decode_body() (GrantWork lookup) and nr_pdsch_passive_grantwork_begin().
 */
static uint64_t gw_fingerprint(const fapi_nr_dl_config_dlsch_pdu_rel15_t *p, const freq_alloc_bitmap_t *fa,
                               const nr_pdsch_passive_grant_t *g, double fo)
{
  uint64_t h = 1469598103934665603ull;
#define GWFP(v) do { uint64_t x_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { h ^= (x_ >> (8 * b_)) & 0xff; h *= 1099511628211ull; } } while (0)
  GWFP(g->rnti); GWFP(g->source_absolute_slot); GWFP(p->dlDataScramblingId); GWFP(p->dlDmrsScramblingId);
  GWFP(p->dmrs_ports); GWFP(p->nscid); GWFP(p->n_dmrs_cdm_groups); GWFP(p->dmrsConfigType); GWFP(p->refPoint);
  GWFP(p->BWPStart); GWFP(p->BWPSize); GWFP(fa->first_rb); GWFP(fa->num_rbs); GWFP(fa->n_prb_list); GWFP(fa->prg);
  uint64_t fob; memcpy(&fob, &fo, sizeof(fob)); GWFP(fob);
  { extern __thread int nr_dlsch_mrc_mode_override, nr_dlsch_irc_override;
    GWFP(nr_dlsch_mrc_mode_override + 1); GWFP(nr_dlsch_irc_override + 1); } /* T26d: LLRs depend on the receiver */
  { uint64_t hh_ = 0; if (nr_ssb_rm_extra.crb) { hh_ = nr_ssb_rm_extra.symbols; for (int i_ = 0; i_ < 275; i_++) hh_ = hh_ * 1099511628211ull + nr_ssb_rm_extra.crb[i_] + 1; } GWFP(hh_); } /* WIDE-S0 PDCCH exclusion */
  GWFP(nr_dlsch_csi_rb_extent()); /* LOW-MCS: CSI-RS exclusion extent changes the RE set */
#undef GWFP
  return h;
}

static uint64_t gw_key_of(const fapi_nr_dl_config_dlsch_pdu_rel15_t *p, uint8_t k0, int nl, int qm)
{
  nr_pdsch_cfg_hypothesis_t h = {0};
  h.tda_start = (uint8_t)p->start_symbol;
  h.tda_length = (uint8_t)p->number_symbols;
  h.k0 = k0;
  h.dmrs_mask = (uint16_t)(p->dlDmrsSymbPos & 0x3FFF);
  return nr_td_grantwork_key(&h, nl, qm);
}

bool nr_pdsch_passive_gpu_job(const PHY_VARS_NR_UE *ue, const fapi_nr_dl_config_dlsch_pdu_rel15_t *pdu,
                              const freq_alloc_bitmap_t *fa, const nr_pdsch_passive_grant_t *grant, int slot_rx,
                              bool probe, nr_gpu_pdsch_job_t *job)
{
  memset(job, 0, sizeof(*job));
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  /* what the GPU does not model: PT-RS, CSI-RS rate matching (the decode also sweeps PT-RS arms on
   * agnostic runs -- it clears the override itself when it arms one) */
  extern bool nr_pdsch_gpu_csi_disjoint(const fapi_nr_dl_config_dlsch_pdu_rel15_t *, const freq_alloc_bitmap_t *);
  if ((pdu->pduBitmap & 0x1) || !nr_pdsch_gpu_csi_disjoint(pdu, fa)) return false;
  int nl = 0;
  for (int i = 0; i < 12 && nl < 4; i++)
    if ((pdu->dmrs_ports >> i) & 1)
      job->ports[nl++] = (uint8_t)i;
  if (nl < 1 || nl > fp->nb_antennas_rx || nl > 4 || __builtin_popcount(pdu->dmrs_ports) != nl)
    return false;
  const uint8_t Qm = nr_get_Qm_dl(grant->mcs, grant->mcs_table);
  const uint32_t R = nr_get_code_rate_dl(grant->mcs, grant->mcs_table);
  if (fa->n_prb_list || fa->prg) /* segmented chest + data-ordered gather are CPU-only (Task 9) */
    return false;
  if (Qm == 0 || R == 0 || fa->num_rbs == 0 || pdu->dlDmrsSymbPos == 0)
    return false;
  job->start_rb = (uint16_t)(pdu->BWPStart + fa->first_rb); /* CRB0-relative, as nr_pdsch_channel_estimation's start_sc */
  job->nb_rb = (uint16_t)fa->num_rbs;
  job->start_symbol = (uint8_t)pdu->start_symbol;
  job->nb_symbols = (uint8_t)pdu->number_symbols;
  job->dmrs_mask = (uint16_t)pdu->dlDmrsSymbPos;
  job->dmrs_type = (uint8_t)(pdu->dmrsConfigType == NFAPI_NR_DMRS_TYPE1 ? 1 : 2);
  job->n_cdm_groups_no_data = (uint8_t)pdu->n_dmrs_cdm_groups;
  job->Nl = (uint8_t)nl;
  job->Qm = Qm;
  job->dmrs_scrambling_id = pdu->dlDmrsScramblingId;
  job->nscid = (uint8_t)pdu->nscid;
  job->dmrs_ref_rb = (uint16_t)(pdu->refPoint ? pdu->BWPStart : 0); /* rb_offset = first_rb + (refPoint ? 0 : BWPStart) */
  job->slot = (uint8_t)slot_rx; /* n_s,f for c_init, as proc->nr_slot_rx in the CPU chest */
  job->time_interp = 0;
  if (probe) { /* code block 0 plus one symbol of slack, as the CPU probe horizon */
    const uint8_t nb_re_dmrs = get_num_dmrs_re_per_rb(pdu->dmrsConfigType, pdu->n_dmrs_cdm_groups);
    const uint16_t dmrs_len = get_num_dmrs(pdu->dlDmrsSymbPos);
    const uint32_t tbs = nr_compute_tbs(Qm, (uint16_t)R, fa->num_rbs, pdu->number_symbols, nb_re_dmrs * dmrs_len,
                                        grant->nb_rb_oh, grant->tb_scaling, (uint8_t)nl);
    const uint32_t G = nr_get_G(fa->num_rbs, pdu->number_symbols, nb_re_dmrs, dmrs_len, 0, Qm, (uint8_t)nl);
    if (tbs == 0 || G == 0)
      return false;
    const uint32_t Kcb = (get_BG(tbs, (uint16_t)R) == 2) ? 3840u : 8448u, B = tbs + 24u;
    const uint32_t C = (B <= Kcb) ? 1u : (B + (Kcb - 24u) - 1u) / (Kcb - 24u);
    if (C > 1)
      job->max_llr = (G + C - 1) / C + (uint32_t)fa->num_rbs * 12u * Qm * nl;
  }
  return true;
}
static __thread bool t_hq_clear = true;
static __thread uint32_t t_hq_A = 0;

static harqc_entry_t *harqc_acquire(uint16_t rnti, uint8_t pid)
{
  pthread_mutex_lock(&g_harqc_lock);
  harqc_entry_t *hit = NULL, *lru = NULL;
  for (int i = 0; i < NR_HARQC_N; i++) {
    harqc_entry_t *e = &g_harqc[i];
    if (e->used && e->rnti == rnti && e->pid == pid) { hit = e; break; }
    if (atomic_load(&e->busy)) continue;
    if (lru == NULL || !e->used || (lru->used && e->last < lru->last)) lru = e;
  }
  harqc_entry_t *e = hit ? hit : lru;
  int expect = 0;
  if (e == NULL || !atomic_compare_exchange_strong(&e->busy, &expect, 1)) {
    pthread_mutex_unlock(&g_harqc_lock);
    return NULL;
  }
  if (!hit) {
    hqb_release(e);
    e->used = true; e->rnti = rnti; e->pid = pid; e->soft_valid = false; e->tbs = 0; e->on_gpu = false;
  }
  e->last = ++g_harqc_clock;
  pthread_mutex_unlock(&g_harqc_lock);
  return e;
}

/* A retry arm of the async lane passed CRC for (rnti, pid, ndi) first sent at job_slot: the failed first pass left a soft sum for that TB; it is dead now, so the entry is retired as for
 * a first-pass success (soft_valid false, device blocks back to the pool). Never allocates an entry. The entry lock is the busy flag: a consumer holds it for one decode, the worker waits for it
 * (bounded; on timeout the sum stays, harmless). A retransmission racing this either combined first (the clear then follows it) or finds the sum gone and decodes alone. */
static void hq_nap(void) { usleep(100); }
static void harqc_rescued(uint16_t rnti, uint8_t pid, uint8_t ndi, int64_t job_slot)
{
  harqc_entry_t *e = NULL;
  pthread_mutex_lock(&g_harqc_lock);
  for (int i = 0; i < NR_HARQC_N && !e; i++)
    if (g_harqc[i].used && g_harqc[i].rnti == rnti && g_harqc[i].pid == pid)
      e = &g_harqc[i];
  pthread_mutex_unlock(&g_harqc_lock);
  if (e == NULL || !nr_hq_lock_wait(&e->busy, hq_nap, 20))
    return;
  if (nr_hq_rescue_match(e->soft_valid, e->ndi, e->first_slot, ndi, job_slot)) {
    e->soft_valid = false;
    hqb_release_locked(e);
  }
  atomic_store(&e->busy, 0);
}

/* passive_ldpc_decode_core: LDPC stage of one passive decode: builds the TB parameters (segmentation, E, rate matching, HARQ buffer) and runs the LDPC decoder.
 *   Returns true when every segment decodes and passes CRC. A one-code-block probe uses the TB CRC of its single block.
 *   Side effects: updates the per-thread HARQ buffer and decoder-used tag.
 *   Caller: passive_ldpc_decode().
 */
static bool passive_ldpc_decode_core(PHY_VARS_NR_UE *ue,
                                const UE_nr_rxtx_proc_t *proc,
                                passive_harq_t *h,
                                const fapi_nr_dl_cw_info_t *cw,
                                const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                int16_t *llr,
                                int number_rbs,
                                uint32_t G)
{
  nrLDPC_TB_decoding_parameters_t TB_parameters = {0};
  nrLDPC_slot_decoding_parameters_t slot_parameters = {.frame = proc->frame_rx,
                                                       .slot = proc->nr_slot_rx,
                                                       .nb_TBs = 1,
                                                       .threadPool = nr_passive_tpool(&get_nrUE_params()->Tpool),
                                                       .TBs = &TB_parameters};

  h->processedSegments = 0;

  TB_parameters.harq_unique_pid = NR_PDSCH_PASSIVE_HARQ_TAG_BASE + dlsch_config->harq_process_nbr;
  TB_parameters.G = G;
  TB_parameters.nb_rb = number_rbs;
  TB_parameters.Qm = cw->qamModOrder;
  TB_parameters.mcs = cw->mcs;
  TB_parameters.nb_layers = cw->Nl;
  TB_parameters.BG = cw->ldpcBaseGraph;
  TB_parameters.A = t_hq_A ? t_hq_A : cw->TBS; // HARQ: a retransmission keeps its first TBS
  TB_parameters.processedSegments = &h->processedSegments;

  nr_segmentation(NULL,
                  NULL,
                  lenWithCrc(1, TB_parameters.A), // max size, in case of a single segment
                  &TB_parameters.C,
                  &TB_parameters.K,
                  &TB_parameters.Z,
                  &TB_parameters.F,
                  TB_parameters.BG);
  if (TB_parameters.C > h->a_segments) {
    LOG_W(NR_PHY, "SENSING: passive PDSCH decode -- too many segments C=%u (cap %u)\n", TB_parameters.C, h->a_segments);
    return false;
  }

  TB_parameters.max_ldpc_iterations = (uint8_t)nr_pdsch_ldpc_cap(); /* RANK2-ITER: the one cap (nr_pdsch_ldpc_cap.h) */
  memset(TB_parameters.decodeIters, 0, sizeof(TB_parameters.decodeIters));
  TB_parameters.rv_index = cw->rv;
  TB_parameters.tbslbrm = dlsch_config->tbslbrm;
  TB_parameters.abort_decode = &h->abort_decode;
  set_abort(&h->abort_decode, false);

  TB_parameters.llr = llr;
  TB_parameters.c = h->c;
  TB_parameters.d = t_hq_d ? t_hq_d : h->d; // HARQ: a persistent per-(rnti,pid) soft buffer
  t_seg_K = TB_parameters.K;
  t_seg_F = TB_parameters.F;
  t_seg_C = TB_parameters.C;
  t_seg_Z = TB_parameters.Z;
  TB_parameters.E = nr_get_E(TB_parameters.G, TB_parameters.C, TB_parameters.Qm, TB_parameters.nb_layers, 0);
  TB_parameters.E2 = TB_parameters.E;
  TB_parameters.first_rE2 = TB_parameters.C;
  TB_parameters.R = nr_get_R_ldpc_decoder(TB_parameters.rv_index, TB_parameters.E, TB_parameters.BG, TB_parameters.Z,
                                          &h->llrLen, 0 /* DLround: always the first, see above */);
  /* Copied HERE, not next to K/F/C/Z above: E and R are ASSIGNED a few lines up from this point,
   * so the earlier copy read them before they existed and PIPEDIAG printed E=0.0 R=0.0 for a whole
   * campaign. The code rate had to be recovered as (K-F)*C/G instead. */
  t_seg_E    = TB_parameters.E;
  t_seg_R    = TB_parameters.R;
  t_seg_lbrm = TB_parameters.tbslbrm;
  t_seg_BG   = TB_parameters.BG;
  for (uint32_t r = 1; r < TB_parameters.C; r++) {
    const int Er = nr_get_E(TB_parameters.G, TB_parameters.C, TB_parameters.Qm, TB_parameters.nb_layers, r);
    if (Er != TB_parameters.E) {
      TB_parameters.E2 = Er;
      TB_parameters.R2 = nr_get_R_ldpc_decoder(TB_parameters.rv_index, Er, TB_parameters.BG, TB_parameters.Z,
                                               &h->llrLen, 0);
      TB_parameters.first_rE2 = r;
      break;
    }
  }

  TB_parameters.d_to_be_cleared = t_hq_clear; // false only for a retransmission being combined
  /* LAYOUT PROBE (2026-09-15). A DCI-layout trial does not need the whole transport block: the first
   * code block's own CRC already says whether the LLRs are right (rank-4 bed: 431 live layouts,
   * 59 segments and 8.6 ms CPU per TB -- a full decode per trial cannot converge). With
   * t_probe_first_seg the decoder sees C=1: segment 0 with the E/K/Z the full C gave it. The TB is
   * NOT reported decoded; the outcome feeds only the layout search. */
  /* NOT by setting C = 1: that told the decoder the TB is one code block, so it checked segment 0
   * against a CRC24A over the whole TBS instead of its own CRC24B -- every probe failed by
   * construction (ISAC_PROBE_ALL on a pinned conf that decodes 97 %: 0/455, 2026-09-16), OTA
   * 0/286000 included. nb_segments_to_decode keeps C, K', E and the offsets of the real TB. */
  const uint32_t C_full = TB_parameters.C;
  TB_parameters.nb_segments_to_decode = (t_probe_first_seg && C_full > 1) ? 1 : 0;
  const uint32_t C_dec = TB_parameters.nb_segments_to_decode ? 1 : C_full;
  atomic_fetch_add_explicit(&g_pdtim_ldpc_tb, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_pdtim_ldpc_cb, C_dec, memory_order_relaxed);
  for (uint32_t r = 0; r < TB_parameters.C; r++) {
    TB_parameters.decodeSuccess[r] = false;
  }
  reset_meas(&TB_parameters.ts_deinterleave);
  reset_meas(&TB_parameters.ts_rate_unmatch);
  reset_meas(&TB_parameters.ts_seg_prep);
  reset_meas(&TB_parameters.ts_ldpc_decode);

  nrLDPC_coding_decoder_t *tb_dec = ue->nrLDPC_coding_interface.nrLDPC_coding_decoder;
  if (t_tb_force_cpu) { /* ISAC_TD_TB_CPU_WHILE_ACQ (CB0 elimination): libldpc's CPU decoder for this TB */
    nrLDPC_coding_decoder_t *cpu = tb_cpu_decoder();
    if (cpu != NULL)
      tb_dec = cpu;
  }
  if (nr_pdsch_gpu_ldpc_decode(&slot_parameters, tb_dec) != 0) { /* DECGPU: tb_dec itself unless ISAC_PDSCH_GPU_EXACT_LDPC=1 */
    LOG_W(NR_PHY, "SENSING: passive PDSCH decode -- nrLDPC_coding_decoder failed\n");
    atomic_fetch_add(&g_ldpc_iface_err, 1);
    t_decoder_used_last = 0; /* no valid result: do not leave the previous TB's decoder */
    return false;
  }
  t_decoder_used_last = TB_parameters.decoder_used;
  if (!t_probe_first_seg) {
    nr_pdsch_ldpc_iters_summary(TB_parameters.decodeIters, TB_parameters.decodeSuccess, (int)C_dec, &t_ldpc_it_max, &t_ldpc_it_sum);
    t_ldpc_it_cap = TB_parameters.max_ldpc_iterations;
  }
  atomic_fetch_add(&g_ldpc_tb_dec[TB_parameters.decoder_used < 3 ? TB_parameters.decoder_used : 0], 1);
  LOG_D(NR_PHY, "SENSING: passive PDSCH decode decoder_used=%u (1=CPU min-sum, 2=CUDA normalised min-sum x2 iterations)\n", TB_parameters.decoder_used);

  {
    uint32_t seg_ok = 0;
    for (uint32_t r = 0; r < C_dec; r++) {
      if (TB_parameters.decodeSuccess[r]) {
        seg_ok++;
      }
      if (r < NR_PDSCH_SEGIDX_MAX) {
        const int cb = nr_pdsch_cbucket(TB_parameters.C);
        atomic_fetch_add(&g_segidx_tot[r], 1);
        atomic_fetch_add(&g_segidxc_tot[cb][r], 1);
        if (!TB_parameters.decodeSuccess[r]) {
          atomic_fetch_add(&g_segidx_fail[r], 1);
          atomic_fetch_add(&g_segidxc_fail[cb][r], 1);
        }
      }
    }
    t_seg_ok_last = seg_ok;
    /* T26b: a probe of a ONE-code-block TB decodes that block with the TB CRC (CRC16/24A), but its outcome was never
     * recorded (only C > 1 set t_probe_seg_ok), so every single-CB probe read "failed" -- measured on the PCI 415
     * replay: probes whose dumped LLRs decode offline at LDPC iteration 1 reported CRC fail. ISAC_PDSCH_PROBE_C1=1
     * (implied by ISAC_PDSCH_SOLVER=1) records it, same all-zero guard, still never a decoded TB. */
    if (t_probe_first_seg) { /* a probe of a one-code-block TB records its outcome too */
      t_probe_seg_ok = (seg_ok == 1);
      /* The all-zero guard the full-TB path applies, on segment 0: an all-zero code block carries a
       * zero CRC24B, so LLRs that are mostly "0" (wrong layout, wrong scrambling, empty grant) pass
       * as a hit. On the rank-4 bed such hits landed 1/1 on a dozen different layouts and never
       * reproduced, and exploiting them starved the search (8 hits, then none in 20000 probes). */
      if (t_probe_seg_ok) {
        const uint32_t seg_bytes = (TB_parameters.K >> 3) - (TB_parameters.F >> 3) - 3;
        uint32_t i = 0;
        while (i < seg_bytes && h->c[i] == 0)
          i++;
        if (i == seg_bytes) {
          t_probe_seg_ok = false;
          atomic_fetch_add(&g_ldpc_zero_tb, 1);
        }
      }
      return false; /* a probe never counts as a decoded TB */
    }
    if (seg_ok != TB_parameters.C) {
      /* LDPC did not converge on at least one segment -> the LLRs feeding it are wrong. */
      atomic_fetch_add(&g_ldpc_seg_fail, 1);
      atomic_fetch_add(&g_seg_ok_sum, seg_ok);
      atomic_fetch_add(&g_seg_tot_sum, TB_parameters.C);
      return false;
    }
  }

  // Reassemble the transport block, exactly as nr_dlsch_decoding() does.
  uint32_t offset = 0, r_offset = 0;
  for (uint32_t r = 0; r < TB_parameters.C; r++) {
    const uint32_t seg_bytes =
        (TB_parameters.K >> 3) - (TB_parameters.F >> 3) - ((TB_parameters.C > 1) ? 3 : 0);
    memcpy(h->b + offset, h->c + r_offset, seg_bytes);
    offset += seg_bytes;
    r_offset += (TB_parameters.K >> 3);
  }

  if (TB_parameters.C > 1 && !check_crc(h->b, lenWithCrc(1, cw->TBS), crcType(1, cw->TBS))) {
    /* Every segment decoded and CRC'd correctly, so the LLRs and the LDPC were RIGHT -- the fault is
     * in reassembly, TBS, or the CRC type. Completely different code from the seg_fail path. */
    atomic_fetch_add(&g_ldpc_tb_fail, 1);
    return false;
  }

  // The same all-zero-payload guard the real decoder applies: an all-zero TB with a zero CRC is a
  // known recurring false pass, and on this path it would inject a constant, information-free X.
  const uint32_t sz = cw->TBS / 8;
  if (h->b[sz] == 0 && h->b[sz + 1] == 0) {
    uint32_t i = 0;
    while (i < sz && h->b[i] == 0) {
      i++;
    }
    if (i == sz) {
      atomic_fetch_add(&g_ldpc_zero_tb, 1);
      return false;
    }
  }
  atomic_fetch_add(&g_ldpc_ok, 1);
  return true;
}

/* The retransmission decision of the HARQ store, shared by the CPU decode and the GPU pass: nr_hq_retx_rule (nr_hq_retry.h), 1 combine / 0 first / -1 decode alone,
 * entry untouched. The RV only sets the circular-buffer start the rate de-matcher already takes from cw->rv. Combining into an rv-0 grant was withheld so a wrong
 * layout hypothesis (junk NDI/PID) cannot poison a fresh decode; it is admitted only once THIS cell has shown it retransmits at rv 0 -- same NDI with rv 0 on a
 * still-undecoded process, seen NR_HARQC_RV0_EVIDENCE times (the census the HARQC line reports). *A: the TBS to decode with (a retransmission has its first
 * transmission's TBS, the rule requires it equal). */
static int harq_retx_decide(const harqc_entry_t *e, const fapi_nr_dl_cw_info_t *cw, uint8_t ndi, uint32_t *A)
{
  static _Atomic uint32_t s_rv0_retx_seen;
  const bool same_ndi = e->soft_valid && e->ndi == ndi;
  /* Evidence needs the TBS to match the stored first transmission too: a junk-layout NDI matches by chance half the time, a junk TBS does not, so the count cannot be filled by mis-parsed grants. */
  if (same_ndi && cw->rv == 0 && e->tbs != 0 && e->tbs == cw->TBS)
    atomic_fetch_add(&s_rv0_retx_seen, 1);
  const bool rv0_admitted = atomic_load(&s_rv0_retx_seen) >= NR_HARQC_RV0_EVIDENCE;
  *A = cw->TBS;
  const int d = nr_hq_retx_rule(e->soft_valid, e->ndi, e->tbs, ndi, cw->rv, cw->TBS, rv0_admitted);
  if (d < 0)
    atomic_fetch_add(&g_hq_tbs_override, 1); /* HARQC "tbs_from_first": now the count of same-NDI grants with another TBS, decoded alone */
  return d;
}

static bool passive_ldpc_decode(PHY_VARS_NR_UE *ue,
                                const UE_nr_rxtx_proc_t *proc,
                                passive_harq_t *h,
                                const fapi_nr_dl_cw_info_t *cw,
                                const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                int16_t *llr,
                                int number_rbs,
                                uint32_t G)
{
  /* A layout PROBE is HARQ-neutral: it neither combines into nor updates a process. A probe returns
   * "not decoded" by construction, so letting it through marked the entry soft_valid and every
   * following same-NDI grant was combined into a stale buffer -- segment 0 failed 92 % in probe mode
   * against 3 % in full mode on the pinned rank-4 bed (2026-09-16). */
  if (!t_hq.armed || t_probe_first_seg || t_quiet)
    return passive_ldpc_decode_core(ue, proc, h, cw, dlsch_config, llr, number_rbs, G);
  t_hq.armed = 0; /* first call per TB only */
  harqc_entry_t *e = harqc_acquire(t_hq.rnti, t_hq.pid);
  if (e == NULL) {
    atomic_fetch_add(&g_hq_busy_skip, 1);
    return passive_ldpc_decode_core(ue, proc, h, cw, dlsch_config, llr, number_rbs, G);
  }
  const size_t need = (size_t)h->a_segments * 68u * 384u; /* max Kc*Z per segment, the decoder's stride */
  if (e->cap < need) {
    free(e->d);
    e->d = (int16_t *)malloc16(need * sizeof(int16_t));
    e->cap = e->d ? need : 0;
    e->soft_valid = false;
  }
  if (e->d == NULL) {
    atomic_store(&e->busy, 0);
    return passive_ldpc_decode_core(ue, proc, h, cw, dlsch_config, llr, number_rbs, G);
  }
  uint32_t A = cw->TBS;
  const int hq_d = harq_retx_decide(e, cw, t_hq.ndi, &A);
  if (hq_d < 0) { /* not this process's TB: decoded alone, the stored sum stays for its own retransmission */
    atomic_store(&e->busy, 0);
    return passive_ldpc_decode_core(ue, proc, h, cw, dlsch_config, llr, number_rbs, G);
  }
  bool retx = hq_d > 0;
  if (retx && e->on_gpu && ((size_t)e->gC * e->gKcz > e->cap || nr_gpu_pass_dl_hq_xfer(1, e->hb.id, e->d, e->gC, (int)e->gKcz, (int)e->gN) != 0)) {
    retx = false; /* the soft bits are on the device and cannot be fetched: decode as a first transmission */
    A = cw->TBS;
  }
  if (e->hb.n > 0)
    hqb_release_locked(e); /* from here the host buffer is the truth (this decode overwrites it) */
  t_hq_d = e->d;
  t_last_hq_retx = retx;
  t_hq_clear = !retx;
  t_hq_A = A;
  const bool ok = passive_ldpc_decode_core(ue, proc, h, cw, dlsch_config, llr, number_rbs, G);
  t_hq_d = NULL;
  t_hq_clear = true;
  t_hq_A = 0;
  if (retx) {
    atomic_fetch_add(&g_hq_retx_try, 1);
    if (ok) atomic_fetch_add(&g_hq_retx_ok, 1);
  } else {
    atomic_fetch_add(&g_hq_first, 1);
  }
  e->ndi = t_hq.ndi;
  if (!retx) e->first_slot = t_hq.slot;
  e->tbs = A;
  e->soft_valid = !ok; /* keep the soft bits only while the TB is still undecoded */
  atomic_store(&e->busy, 0);
  return ok;
}

/* Per-antenna FEP task, dispatched across the thread pool by the FEP loop below when
 * nb_antennas_rx > 1 -- see nr_slot_fep_ant()'s definition-site comment (slot_fep_nr.c) for why
 * this exists. rxdataF_flat/stride reconstruct the VLA-typed pointer nr_slot_fep_ant() expects;
 * a plain struct field cannot carry a runtime-sized array type directly. */
typedef struct {
  double fo_hz;
  PHY_VARS_NR_UE *ue;
  const NR_DL_FRAME_PARMS *fp;
  unsigned int slot;
  int start_symbol;
  int number_symbols;
  unsigned int ant;
  c16_t *rxdataF_flat;
  uint32_t stride;
  c16_t **rxdata;
  task_ans_t *ans;
} nr_slot_fep_ant_task_t;

static void nr_slot_fep_ant_task(void *arg)
{
  nr_slot_fep_ant_task_t *a = (nr_slot_fep_ant_task_t *)arg;
  c16_t(*rxdataF)[a->stride] = (c16_t(*)[a->stride])a->rxdataF_flat;
  for (int m = a->start_symbol; m < a->start_symbol + a->number_symbols; m++) {
    nr_slot_fep_ant_snapshot(a->ue, a->fp, a->slot, m, a->ant, rxdataF, link_type_dl, 0, a->rxdata, a->fo_hz);
  }
  completed_task_ans(a->ans);
}

/* ---- PER-SLOT SHARING (2026-09-15). The queue consumer dequeues every grant of one slot as a
 * group and tells the decoder so: FEP is then done ONCE for the whole slot, and the channel
 * estimate ONCE over the union of the group's RB ranges (when the DM-RS configuration matches),
 * with later grants of the group reusing both. Measured before: fep 102 us + chest 324 us of a
 * ~0.7 ms grant, repeated per grant -- and grants per slot is what grows with UE count.
 * All state is thread-local; the caches are keyed on the slot and on the DM-RS configuration, so
 * a mismatch simply misses. ponytail: one-entry caches, a ring of slots if consumers ever
 * interleave slots within a thread. */
typedef struct { int on; int rb_lo, rb_n; } nr_pdsch_slot_share_t;
static __thread nr_pdsch_slot_share_t t_share = {0, 0, 0};
static __thread struct { long slot; double fo; int valid; } t_fep_cache = {0, 0.0, 0};
/* CHEST CACHE (K32, 2026-10-01). Keyed on every estimator input (nr_pdsch_chest_key.h). The estimate
 * is always built over the slot's FULL DM-RS set, and the cache holds its OWN copy of the DM-RS rows,
 * written once on a miss and never touched again: a decode works on pdsch_dl_ch_estimates, a per-call
 * working copy that time interpolation, chest_time averaging, branch zeroing and SFO rotation mutate
 * freely. nvar is cached RAW (the pre-division sum and the per-branch values): its divisor depends on
 * the grant's own number_symbols, so the division and the per-branch substitution run per call. A hit
 * therefore produces exactly what a miss would. Before this, the key had no probe horizon / start or
 * length of symbols, kept 8 of the 12 port bits, no BWP/refPoint, fo or antenna count, and a hit
 * reused the working buffer itself, already post-processed by the previous grant. */
static __thread struct {
  nr_pdsch_chest_key_t key;
  int valid;
  uint32_t nvar_sum; /* sum over (DM-RS symbol x layer) of the per-antenna mean, before the divisor */
  uint32_t nvar_ant[NR_DL_CHEST_MAX_ANT]; /* nr_dl_chest_nvar_ant[] as the last estimator call left it */
  int n_dmrs_sym, dmrs_first, dmrs_last;
  c16_t *est; /* [stored row][DM-RS symbol k][ofdm_symbol_size], immutable once stored; stored row = layer x
               * antenna, or the layer alone when one branch is estimated (the others are zero rows) */
  size_t est_cap;
} t_chest_cache = {0};

/* ZP EVIDENCE FROM THIS DECODED GRANT (G5 review of merge 060edd290c, Important 1). Every ZP entry the blind
 * CSI-RS search handed this grant for rate matching is scored on the grant's OWN PRBs: a true ZP is dark there,
 * PDSCH on a false one is as bright as the grant's data -- independent of how much of the band the grant covers,
 * of the traffic pattern, and of an NZP boost, which is what the search's full-band score cannot see. One
 * reference symbol, nearest the ZP symbol (inside the real PDSCH whenever the ZP symbol is, even under a wrong
 * start-symbol hypothesis). The grant only counts when its first DM-RS symbol carries THIS cell's DM-RS
 * (scrambling id, nSCID, ports of the grant) on its PRBs: raw energy cannot tell a false DCI accept or a wrong
 * PRB/k0 hypothesis landing on a co-channel neighbour -- which does not rate-match our ZP -- from our own
 * PDSCH (G5 round 2). Only dedicated-class grants count: the ZP set is UE-dedicated PDSCH-Config, common PDSCH
 * need not respect it. Cost: two symbols of grant REs plus the ZP REs, per antenna. */
double nr_pdsch_passive_zp_grant_score(const NR_DL_FRAME_PARMS *fp, const fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg,
                                       const freq_alloc_bitmap_t *fa, const c16_t *rxdataF_flat, uint32_t stride,
                                       int slot_rx, int fep_s0, int fep_n, uint16_t skip_symbols, bool dedicated, int i)
{
  if (!dedicated || i < 0 || i >= cfg->numCsiRsForRateMatching || cfg->csiRsForRateMatching[i].csi_type != 2)
    return -1.0;
  const int s_lo = cfg->start_symbol > fep_s0 ? cfg->start_symbol : fep_s0;
  const int s_hi = cfg->start_symbol + cfg->number_symbols < fep_s0 + fep_n ? cfg->start_symbol + cfg->number_symbols
                                                                             : fep_s0 + fep_n;
  fapi_nr_dl_config_dlsch_pdu_rel15_t all = *cfg, one = *cfg, others = *cfg;
  one.numCsiRsForRateMatching = 1;
  one.csiRsForRateMatching[0] = cfg->csiRsForRateMatching[i];
  others.numCsiRsForRateMatching = 0;
  for (int j = 0; j < cfg->numCsiRsForRateMatching; j++)
    if (j != i)
      others.csiRsForRateMatching[others.numCsiRsForRateMatching++] = cfg->csiRsForRateMatching[j];
  uint32_t zp_bm[NR_SYMBOLS_PER_SLOT] = {0};
  int zp_first = -1, dmrs = -1;
  for (int m = s_lo; m < s_hi; m++) {
    zp_bm[m] = nr_dlsch_csi_overlap_bitmap(&one, m) & ~nr_dlsch_csi_overlap_bitmap(&others, m);
    if (zp_bm[m] && ((skip_symbols >> m) & 1))
      return -1.0; // ZP symbol shares a symbol with the SSB: SSB REs are not ours to judge by
    if (zp_bm[m] && zp_first < 0)
      zp_first = m;
    if (((cfg->dlDmrsSymbPos >> m) & 1) && dmrs < 0)
      dmrs = m;
  }
  if (zp_first < 0 || dmrs < 0 || cfg->dmrs_ports == 0)
    return -1.0; // the grant does not cross the ZP symbol, or no DM-RS symbol was FFT'd
  int ref = -1;
  for (int m = s_lo; m < s_hi; m++)
    if (!((cfg->dlDmrsSymbPos >> m) & 1) && !((skip_symbols >> m) & 1) && nr_dlsch_csi_overlap_bitmap(&all, m) == 0
        && (ref < 0 || abs(m - zp_first) < abs(ref - zp_first)))
      ref = m;
  if (ref < 0)
    return -1.0;
  /* The grant's DM-RS: the CDM group of its lowest port, one pilot sequence per distinct fd-OCC in that group
   * (ports differing only in td-OCC are the same sequence on one symbol). */
  const uint8_t type = cfg->dmrsConfigType;
  const int p0 = __builtin_ctz(cfg->dmrs_ports);
  const uint8_t delta = get_delta(p0, type);
  const int per_rb = type == NFAPI_NR_DMRS_TYPE1 ? 6 : 4;
  const uint8_t k_of[4] = {delta, (uint8_t)(delta + (per_rb == 6 ? 2 : 1)), (uint8_t)(delta + (per_rb == 6 ? 4 : 6)),
                           (uint8_t)(delta + (per_rb == 6 ? 6 : 7))};
  const int pilot_crb0 = cfg->refPoint ? cfg->BWPStart : 0;
  const int n_rb_gen = cfg->BWPStart + cfg->BWPSize - pilot_crb0;
  c16_t pil[2][6 * 275] __attribute__((aligned(16))); // stack, not TLS: 13 KB of __thread shifts the TLS layout
  const int16_t *pilots[2];
  int n_ports = 0, fd_seen = 0;
  const uint32_t *gold = nr_gold_pdsch(fp->N_RB_DL, fp->symbols_per_slot, cfg->dlDmrsScramblingId, cfg->nscid, slot_rx, dmrs);
  for (int p = 0; p < 12 && n_ports < 2 && n_rb_gen > 0 && n_rb_gen <= 275; p++) {
    if (!((cfg->dmrs_ports >> p) & 1) || get_delta(p, type) != delta || ((fd_seen >> (p & 1)) & 1))
      continue;
    fd_seen |= 1 << (p & 1);
    nr_pdsch_dmrs_rx(fp->Ncp, gold, pil[n_ports], 1000 + p, 0, n_rb_gen, type, 1 << 14);
    pilots[n_ports] = (const int16_t *)pil[n_ports];
    n_ports++;
  }
  if (n_ports == 0)
    return -1.0;
  double e_zp = 0.0, e_data = 0.0, coh = 0.0, inc = 0.0, rx_pow = 0.0;
  uint32_t n_zp = 0, n_data = 0, n_blocks = 0;
  for (int a = 0; a < fp->nb_antennas_rx; a++) {
    const c16_t *ant = &rxdataF_flat[(size_t)a * stride];
    nr_csirs_blind_re_energy((const int16_t *)&ant[(size_t)ref * fp->ofdm_symbol_size], fp->ofdm_symbol_size,
                             fp->first_carrier_offset, fa->bitmap, cfg->BWPSize, cfg->BWPStart, 0xFFF, 0xFFF, &e_data,
                             &n_data);
    nr_csirs_blind_pilot_coherence((const int16_t *)&ant[(size_t)dmrs * fp->ofdm_symbol_size], fp->ofdm_symbol_size,
                                   fp->first_carrier_offset, fa->bitmap, cfg->BWPSize, cfg->BWPStart, pilots, n_ports,
                                   pilot_crb0, per_rb, k_of, &coh, &inc, &rx_pow, &n_blocks);
    for (int m = zp_first; m < s_hi; m++)
      if (zp_bm[m])
        nr_csirs_blind_re_energy((const int16_t *)&ant[(size_t)m * fp->ofdm_symbol_size], fp->ofdm_symbol_size,
                                 fp->first_carrier_offset, fa->bitmap, cfg->BWPSize, cfg->BWPStart, zp_bm[m] & 0xFFF,
                                 (zp_bm[m] >> 16) & 0xFFF, &e_zp, &n_zp);
  }
  const double presence = nr_csirs_blind_pilot_presence(coh, inc, n_blocks, n_ports);
  return nr_csirs_blind_zp_grant_score(e_zp, n_zp, e_data, n_data, presence,
                                       n_blocks ? presence * rx_pow / (4.0 * n_blocks) : 0.0);
}

void nr_pdsch_passive_set_slot_share(int on, int rb_lo, int rb_n)
{
  t_share.on = on; t_share.rb_lo = rb_lo; t_share.rb_n = rb_n;
}
/* The band the channel estimate covers: the grant's, or with the slot share on the group's union of RBs above the grant's first RB, so the slot's next
 * grant can reuse it. Widen UPWARD only: nr_pdsch_channel_estimation() writes dl_ch from INDEX 0 = the first RB of the freq_alloc IT was given, and
 * nr_dlsch_extract_rbs() reads it back from index 0 assuming that is THIS grant's first RB, so the estimate's base RB must be this grant's first_rb
 * (a lower base would hand the equaliser a channel shifted by (first_rb - rb_lo)*12 subcarriers). The chest key carries rb_lo AND rb_n: grants of one
 * group with the same first RB compute the same union, hence the same key; a grant starting higher re-estimates. */
static void share_chest_band(const freq_alloc_bitmap_t *fa, const fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg, freq_alloc_bitmap_t *fa_out,
                             fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg_out)
{
  *cfg_out = *cfg;
  *fa_out = *fa;
  if (t_share.on && t_share.rb_n > 0) {
    const int lo = (int)fa->first_rb;
    const int hi0 = t_share.rb_lo + t_share.rb_n, hi1 = (int)(fa->first_rb + fa->num_rbs);
    const int hi = hi0 > hi1 ? hi0 : hi1;
    *fa_out = set_bitmap_from_start_size(lo, hi - lo);
    cfg_out->start_rb = lo; cfg_out->number_rbs = hi - lo;
  }
}

/* nr_pdsch_passive_alloc_normalise() moved to nr_pdsch_prb_set.c (2026-09-27): it is pure (only
 * freq_alloc_bitmap_t + nr_prb_list_normalise(), both already there) and had no unit test because
 * this file pulls in PHY_VARS_NR_UE/NFAPI and can't link into the lightweight test_nr_pdsch_prb_set
 * target. Declared via nr_pdsch_passive_decode.h's include of nr_pdsch_prb_set.h. */


/* passive_decode_body: one complete passive PDSCH decode of a grant: FEP, channel estimation, equaliser, demodulation (LLRs) and LDPC, in that order.
 *   Starts with the PRG hypothesis (PRG sweep), the TBS/LBRM derivation and the GrantWork lookup (skips FEP/chest/LLR when a READY entry exists).
 *   Stages: FEP per symbol with per-antenna parallelism; channel estimate; demodulation (nr_rx_pdsch); LLR fill; LDPC via passive_ldpc_decode().
 *   Writes the result to *out and its status (CRC_OK / CRC_FAIL / UNSUPPORTED). Caller: passive_decode_retry() only.
 */
/* GPUPASS: per-consumer host buffers, allocated and prefaulted at consumer start (before the RX fence), never on the pass path */
static __thread nr_gpu_pass_dl_out_t *t_gpp = NULL; /* ~150 KB */
/* SFO (PHY/MODULATION/nr_sfo_state.h): the eps the pass of this decode used. The old path's FEP of the same slot reads the published eps
 * when it runs; if it changed in between (rare: hysteresis), pass and old path legitimately differ and the shadow check must not count it. */
static __thread int64_t t_gp_sfo_q;
static inline bool gp_sfo_same(void) { return nr_sfo_eps_q() == t_gp_sfo_q; }
static __thread uint8_t *t_gp_shadow = NULL;        /* the TB of a shadow-checked pass: sized to the largest TB up front */
#define GP_SHADOW_CAP NR_GPU_GRAPH_SHADOW_CAP /* nr_gpu_graph_pool.h: the v2 pool's shadow buffers have the same size */
static __thread uint64_t t_gp_fhash = 0;           /* shadow of a CRC-failing pass: hash of its final LLRs and its count of decoded blocks */
static __thread uint32_t t_gp_fok = 0;
static __thread bool t_gp_sbad = false;           /* FP32 CRC-OK shadow: the LLR signs of the old path disagree */
static __thread bool t_gp_claim_tried = false;    /* v2: this thread asked the pool once (a refusal is not retried: nomem gate from then on) */
static void gp_thread_prewarm(void)
{
  if (!nr_gpu_pass_active() || t_gpp)
    return;
  if (nr_gpu_graph_on() && !nr_gpu_allocations_allowed()) { /* v2: a thread that starts after the RX fence takes a prewarmed set (NULL = pool empty: nomem gate) */
    if (!t_gp_claim_tried) {
      t_gp_claim_tried = true;
      t_gpp = nr_gpu_graph_pool_claim(&t_gp_shadow);
    }
    return;
  }
  t_gp_shadow = (uint8_t *)malloc(GP_SHADOW_CAP);
  t_gpp = (nr_gpu_pass_dl_out_t *)aligned_alloc(64, (sizeof(nr_gpu_pass_dl_out_t) + 63) & ~(size_t)63);
  if (!t_gp_shadow || !t_gpp) {
    free(t_gp_shadow); free(t_gpp);
    t_gp_shadow = NULL; t_gpp = NULL;
    return;
  }
  memset(t_gp_shadow, 0, GP_SHADOW_CAP);
  memset(t_gpp, 0, sizeof(*t_gpp));
  nr_gpu_pass_dl_prewarm(t_gpp); /* the job and the LLR copy buffer */
}
/* Task 5: FP32 pass vs old path: the LLRs are compared by sign (a pair disagrees when the signs are strictly opposite; counted over the positions where either is
 * nonzero). The policy of the FP32 shadow checks: > 0.1 % sign disagreement disables the pass; a CRC-verdict difference alone is tolerated and counted (fp32_flip). */
static bool gp_sign_bad(const int16_t *a, const int16_t *b, uint32_t n)
{
  uint64_t bad = 0, tot = 0;
  for (uint32_t i = 0; i < n; i++) {
    tot += a[i] != 0 || b[i] != 0;
    bad += (a[i] > 0 && b[i] < 0) || (a[i] < 0 && b[i] > 0);
  }
  return bad * 1000 > tot;
}
static uint64_t gp_llr_hash(const int16_t *p, uint32_t n)
{
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < n; i++)
    h = (h ^ (uint16_t)p[i]) * 1099511628211ull;
  return h;
}

/* the facts of the body the graph pass needs, resolved as the body's own statics resolve them */
static int gp_iters(void)
{
  return nr_pdsch_ldpc_cap();
}
/* t_hq.armed is set for these decodes (soft combining across retransmissions) */
static bool gp_hq_arms(void) { return !t_quiet && atomic_load(&g_ldpc_ok) >= 100; }
/* What passive_ldpc_decode_core leaves behind for a transport block every code block of which decoded (the GPU LDPC wrapper reports decoder_used = CPU: it is
 * bit-exact with it). The TB itself is in g_harq.b, assembled by the pass. */
static void gp_commit_decode(const nr_gpu_pass_dl_out_t *g, const fapi_nr_dl_cw_info_t *cw, uint32_t G, uint32_t tbslbrm, bool tb_ok)
{
  const int C_dec = t_probe_first_seg && g->C > 1 ? 1 : g->C; /* v2 Task 2: a probe decoded code block 0 (passive_ldpc_decode_core's C_dec) */
  uint32_t seg_ok = 0;
  for (int r = 0; r < C_dec; r++)
    seg_ok += g->r.seg_ok[r] != 0;
  g_harq.processedSegments = seg_ok;
  if (!t_probe_first_seg) {
    bool okv[NR_GPU_PASS_MAX_CB];
    for (int r = 0; r < C_dec; r++)
      okv[r] = g->r.seg_ok[r] != 0;
    nr_pdsch_ldpc_iters_summary(g->r.iters, okv, C_dec, &t_ldpc_it_max, &t_ldpc_it_sum);
    t_ldpc_it_cap = (uint8_t)gp_iters();
  }
  t_seg_K = g->K; t_seg_F = g->F; t_seg_C = (uint32_t)g->C; t_seg_Z = g->Z;
  t_seg_E = g->E; t_seg_R = g->R; t_seg_lbrm = tbslbrm; t_seg_BG = (uint32_t)g->BG;
  t_decoder_used_last = NRLDPC_DECODER_CPU;
  atomic_fetch_add(&g_ldpc_tb_dec[NRLDPC_DECODER_CPU], 1);
  for (int r = 0; r < C_dec && r < NR_PDSCH_SEGIDX_MAX; r++) {
    const int cb = nr_pdsch_cbucket((uint32_t)g->C);
    atomic_fetch_add(&g_segidx_tot[r], 1);
    atomic_fetch_add(&g_segidxc_tot[cb][r], 1);
    if (!g->r.seg_ok[r]) {
      atomic_fetch_add(&g_segidx_fail[r], 1);
      atomic_fetch_add(&g_segidxc_fail[cb][r], 1);
    }
  }
  t_seg_ok_last = seg_ok;
  if (t_probe_first_seg) { /* passive_ldpc_decode_core's probe outcome: block 0's CRC, the all-zero guard on its bytes, never a TB */
    t_probe_seg_ok = seg_ok == 1;
    if (t_probe_seg_ok) {
      const uint32_t seg_bytes = (g->K >> 3) - (g->F >> 3) - 3; /* the pass put block 0's bytes at the head of the TB buffer */
      uint32_t i = 0;
      while (i < seg_bytes && g_harq.b[i] == 0)
        i++;
      if (i == seg_bytes) {
        t_probe_seg_ok = false;
        atomic_fetch_add(&g_ldpc_zero_tb, 1);
      }
    }
    return;
  }
  if (seg_ok != (uint32_t)g->C) { /* what passive_ldpc_decode_core counts for a TB with a failed block */
    atomic_fetch_add(&g_ldpc_seg_fail, 1);
    atomic_fetch_add(&g_seg_ok_sum, seg_ok);
    atomic_fetch_add(&g_seg_tot_sum, (uint64_t)g->C);
    return;
  }
  if (!tb_ok) { /* every block decoded, the reassembled TB fails its CRC */
    atomic_fetch_add(&g_ldpc_tb_fail, 1);
    return;
  }
  atomic_fetch_add(&g_ldpc_ok, 1);
}

static nr_pdsch_passive_decode_status_t passive_decode_body_core(PHY_VARS_NR_UE *ue,
                                                            const UE_nr_rxtx_proc_t *proc,
                                                            fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                                            const freq_alloc_bitmap_t *freq_alloc,
                                                            const nr_pdsch_passive_grant_t *grant,
                                                            c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                                            nr_pdsch_passive_decode_result_t *out)
{
  t_ldpc_it_max = 0; t_ldpc_it_cap = 0; t_ldpc_it_sum = 0;
  NR_CFG_EPOCH_WORK(nr_cfg_epoch_work_stamp(), NULL);
  t_fep_done_ns = 0;
  memset(out, 0, sizeof(*out));
  t_gw_used = t_gw_ready = false;
  t_last_hq_retx = false;
  out->status = NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED;
  t_last_sk = -1;
  t_last_llr_have = t_last_data_bits = 0;
  t_last_llr = NULL; t_last_G = 0; t_seg_E = 0; /* never read a previous decode's LLRs as this one's */

  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;

  /* ---- PRB-BUNDLING (PRG) HYPOTHESIS (full-running-agnosticity Task 12) -----------------------
   * RRC pdsch-Config PRB-bundling-type/bundleSize decides whether the gNB may switch precoder
   * every `prg` PRBs; interpolating the channel estimate across that boundary mixes two precoders
   * and costs CRC on a commercial MIMO cell. That field is invisible here, so it is a per-RNTI
   * 3-arm hypothesis {wideband(0), 2, 4} decided by the TB CRC -- same shape as the VRB-L sweep,
   * except a wrong prg guess only moves chest INTERPOLATION boundaries, never which REs are read,
   * so (unlike VRB-L) the whole hypothesis lives here instead of needing a caller-side PRB-list
   * rebuild. An explicit caller-set freq_alloc->prg (no producer sets one today) always wins, and
   * the GPU front end already computed its channel estimate assuming prg==0 (it bails to CPU for
   * any nonzero prg -- nr_pdsch_passive_gpu_job()) so this sweep does not run on a GPU-assisted
   * decode either. Arm 0 (wideband) is a true no-op: `freq_alloc` is left pointing at the caller's
   * own struct, so an unlatched RNTI still on arm 0 costs nothing beyond the reads below.
   * EVIDENCE-TRIGGERED (final review C1): arm 0 is used EXCLUSIVELY until it has shown <= 25 % CRC over
   * 32 trials while the link was healthy (nr_arm_sweep_gated_t); before, the Wilson pick sent ~2/3 of all
   * decodes from the first grant onto the ~100x costlier segmented path, which also skips DMRSFO/SFO/the
   * chest cache/PT-RS/GPU. DCI 1_1, sweepable RNTIs only. */
  freq_alloc_bitmap_t fa_prg;
  int prg_arm = -1;
  const bool sweepable = rnti_sweepable(grant->rnti, grant->rnti_class);
  /* DCI 1_1 only: TS 38.214 5.1.2.3 fixes the PRG for 1_0 (and SI/RA/P grants carry no RRC config). */
  if (freq_alloc->prg == 0 && t_llr_ovr_n == 0 && grant->dci11 && sweepable && !t_quiet) {
    prg_arm = rnti_prg_pick(grant->rnti);
    const uint8_t prg_val = nr_prg_arm_value(prg_arm);
    if (prg_val != 0) {
      fa_prg = *freq_alloc;
      fa_prg.prg = prg_val;
      freq_alloc = &fa_prg;
    }
  }

  /* ---- NON-CONTIGUOUS PRB SETS (full-running-agnosticity Task 9) ------------------------------
   * freq_alloc->n_prb_list > 0 carries a DATA-ORDERED PRB list (RA type 0, interleaved VRB) and
   * prg > 0 a PRB bundling size. Both split the allocation into contiguous segments (Task 8's
   * nr_prb_segments). ONE segment with prg == 0 -- every grant produced today -- takes EXACTLY the
   * previous code path; nothing below changes for it (every new branch is guarded by seg_path).
   * Otherwise: the channel is estimated once per segment (never interpolated across a gap or a
   * PRG boundary) and nr_rx_pdsch() demodulates a VIRTUAL contiguous allocation at BWP PRBs
   * 0..n-1 into which the segments' REs are gathered in data order, so the LLR stream -- and
   * dl_valid_re, rxdataF_comp, EQDIAG -- come out in data order with no change to nr_rx_pdsch().
   * A list grant is normalised first so first_rb/last_rb/num_rbs/bitmap agree with the list
   * (TBS, G, CSI-RS unavailable-RE count and the stats all read those). */
  freq_alloc_bitmap_t fa_list;
  nr_prb_seg_t seg[NR_PRB_SET_MAX];
  int nseg = 1;
  if (freq_alloc->n_prb_list > 0 || freq_alloc->prg > 0) {
    uint16_t contig[NR_PRB_SET_MAX];
    const uint16_t *prb = freq_alloc->prb_list;
    int n = freq_alloc->n_prb_list;
    if (n == 0) {
      n = freq_alloc->num_rbs;
      for (int i = 0; i < n && i < NR_PRB_SET_MAX; i++)
        contig[i] = (uint16_t)(freq_alloc->first_rb + i);
      prb = contig;
    }
    bool bad = n <= 0 || n > NR_PRB_SET_MAX;
    if (!bad && freq_alloc->n_prb_list > 0) {
      /* The producer (nr_pdsch_passive_queue_enqueue for queued grants) normalises; this re-derives
       * as a CHECK, because the data-aided tap and the queue probes read the CALLER's copy. */
      fa_list = *freq_alloc;
      bad = !nr_pdsch_passive_alloc_normalise(&fa_list, dlsch_config->BWPSize);
      if (!bad && (fa_list.first_rb != freq_alloc->first_rb || fa_list.last_rb != freq_alloc->last_rb
                   || fa_list.num_rbs != freq_alloc->num_rbs
                   || memcmp(fa_list.bitmap, freq_alloc->bitmap, sizeof(fa_list.bitmap)) != 0)) {
        static _Atomic unsigned long c_ = 0;
        const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
        if (n_ == 1 || (n_ % 200) == 0)
          LOG_W(PHY, "SENSING: PDSCH PRB-list grant NOT normalised by its producer n=%lu (num_rbs=%d vs %d "
                     "PRBs listed): call nr_pdsch_passive_alloc_normalise() -- the data-aided tap and the queue "
                     "probes read the un-normalised copy\n", n_, freq_alloc->num_rbs, fa_list.num_rbs);
      }
      freq_alloc = &fa_list;
    }
    nseg = bad ? -1 : nr_prb_segments(prb, n, dlsch_config->BWPStart, freq_alloc->prg, seg, NR_PRB_SET_MAX);
    if (nseg <= 0) {
      { static _Atomic unsigned long c_ = 0;
        const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
        if (n_ == 1 || (n_ % 200) == 0)
          LOG_A(PHY, "SENSING: PDSCH UNSUP@seg-list n=%lu (n_prb_list=%u num_rbs=%d bwp_size=%u)\n", n_,
                (unsigned)freq_alloc->n_prb_list, freq_alloc->num_rbs, (unsigned)dlsch_config->BWPSize); }
      prg_arm_unsupported(grant->rnti, prg_arm);
      return out->status;
    }
  }
  const bool seg_path = !(nseg == 1 && freq_alloc->prg == 0);
  if (seg_path) {
    static _Atomic unsigned long c_ = 0;
    const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
    if (n_ == 1 || (n_ % 1000) == 0)
      LOG_A(PHY, "SENSING: PDSCH segmented decode n=%lu (this grant: %d PRBs in %d segments, prg=%u)\n", n_,
            freq_alloc->num_rbs, nseg, (unsigned)freq_alloc->prg);
  }

  // ---- Scope: mirror nr_isac_pdsch_data_aided_submit()'s own guards. Decoding a grant whose
  // reconstruction we could not use anyway is pure CPU cost. ----
  /* PT-RS grants are refused: the density is RRC-only, invisible to a passive receiver, and G cannot be computed without it. */
  if (dlsch_config->pduBitmap & 0x1) {
    { static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_A(PHY, "SENSING: PDSCH UNSUP@%s n=%lu\n", seg_path ? "seg-ptrs" : "1373", n_); }
    if (seg_path)
      prg_arm_unsupported(grant->rnti, prg_arm);
    return out->status;
  }
  /* ---- SSB rate matching (TS 38.214 5.1.4): the CURRENT slot's SSB, observed blind (PSS x SSS of
   * the acquired PCI) -- no configured bitmap, no projected period, no SIB1. PSS/SSS need their FFT
   * before G. On a slot-cache hit the whole slot is already transformed; otherwise FEP exactly as the
   * main FEP below does, so a cached slot never holds a differently compensated symbol. */
  const double fep_fo = isnan(nr_slot_fep_fo_override_hz)
      ? (ue->cont_fo_comp ? ue->dl_Doppler_shift + ue->freq_offset : 0.0)
      : nr_slot_fep_fo_override_hz;
  const long share_slot = grant->source_absolute_slot;
  t_last_fep_fo = fep_fo;
  const int fep_hit = t_share.on && t_fep_cache.valid && t_fep_cache.slot == share_slot && t_fep_cache.fo == fep_fo;
  const uint16_t ssb_cand = nr_ssb_rm_candidates(fp, proc->nr_slot_rx, dlsch_config->start_symbol,
                                                 dlsch_config->number_symbols);
  if (ssb_cand && !fep_hit) {
    if (grant->check_sample_lifetime && !nr_passive_samples_valid(
            atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed),
            grant->source_absolute_slot, fp->slots_per_frame)) {
      { static _Atomic unsigned long c_ = 0;
        const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
        if (n_ == 1 || (n_ % 200) == 0)
          LOG_A(PHY, "SENSING: PDSCH UNSUP@ssb-fep-before n=%lu\n", n_); }
      return out->status; /* IQ already overwritten: nothing to observe */
    }
    const uint16_t fep_syms = ssb_cand | (ssb_cand << 2); // PSS at s, SSS at s+2
    /* SYSAN: these PSS/SSS symbols through the bit-exact GPU FEP (one launch for all antennas and symbols) instead of
     * 2 x nb_antennas_rx serial CPU FFTs on this consumer: measured +150-250 us p50 on the first body of SSB-candidate slots.
     * Same fallback rule as the main FEP below: CPU when the GPU path refuses. */
    int ssb_rng[14][2], n_ssb_rng = 0;
    for (int sym = 0; sym < 14; ++sym)
      if ((fep_syms >> sym) & 1) {
        ssb_rng[n_ssb_rng][0] = sym;
        ssb_rng[n_ssb_rng++][1] = 1;
      }
    const bool ssb_gpu_done = nr_pdsch_gpu_exact_fep_ranges(ue, fp, proc->nr_slot_rx, ssb_rng, n_ssb_rng, &rxdataF[0][0],
                                                                         fp->samples_per_slot_wCP,
                                                                         nr_pdsch_passive_iq_for_slot(proc->nr_slot_rx, ue->common_vars.rxdata)) == 0;
    for (int sym = 0; sym < 14 && !ssb_gpu_done; ++sym) {
      if (!((fep_syms >> sym) & 1))
        continue;
      if (fp->nb_antennas_rx > 1) {
        for (int ant = 0; ant < fp->nb_antennas_rx; ++ant)
          nr_slot_fep_ant_snapshot(ue, fp, proc->nr_slot_rx, sym, ant, rxdataF, link_type_dl, 0, nr_pdsch_passive_iq_for_slot(proc->nr_slot_rx, ue->common_vars.rxdata), fep_fo);
      } else {
        nr_slot_fep(ue, fp, proc->nr_slot_rx, sym, rxdataF, link_type_dl, 0, nr_pdsch_passive_iq_for_slot(proc->nr_slot_rx, ue->common_vars.rxdata));
      }
    }
    nr_gpu_graph_call_dirty(); /* v2 Task 4 review S1: the SSB FEP wrote the caller's rxdataF (fep_fo may be nonzero), which the held instance's D reads on DIRECT */
    if (grant->check_sample_lifetime && !nr_passive_samples_valid(
            atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed),
            grant->source_absolute_slot, fp->slots_per_frame)) {
      { static _Atomic unsigned long c_ = 0;
        const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
        if (n_ == 1 || (n_ % 200) == 0)
          LOG_A(PHY, "SENSING: PDSCH UNSUP@ssb-fep-after n=%lu\n", n_); }
      return out->status; /* overwritten IQ is not SSB evidence */
    }
  }
  nr_ssb_rm_event_t ssb_event = nr_ssb_rm_observe(fp, proc->frame_rx, proc->nr_slot_rx, ssb_cand, rxdataF);
  nr_ssb_rm_plan_t ssb;
  const bool ssb_ok = nr_ssb_rm_plan(&ssb_event, proc->frame_rx, proc->nr_slot_rx, fp->Nid_cell, grant->rnti, dlsch_config,
                                     freq_alloc, seg_path ? seg : NULL, seg_path ? nseg : 0, &ssb);
  const uint32_t ssb_unav = ssb.unav;
  if (ssb_event.symbols) {
    static _Atomic unsigned long count = 0;
    const unsigned long n = atomic_fetch_add_explicit(&count, 1, memory_order_relaxed) + 1;
    if (n <= 100 || n % 100 == 0)
      LOG_I(PHY, "PDSCH SSB-OBS n=%lu frame=%d slot=%d pci=%d symbols=0x%x crb=%u..%u overlap_re=%u refused=%d rnti=0x%x\n",
            n, ssb_event.frame, ssb_event.slot, ssb_event.pci, ssb_event.symbols, ssb_event.first_crb,
            ssb_event.last_crb, ssb_unav, !ssb_ok, grant->rnti);
  }
  if (!ssb_ok) {
    { static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_A(PHY, "SENSING: PDSCH UNSUP@ssb n=%lu (SSB REs under a DM-RS/PT-RS RE or an SI-RNTI grant)\n", n_); }
    prg_arm_unsupported(grant->rnti, prg_arm);
    return out->status;
  }
  if (ssb_unav) {
    static _Atomic unsigned long count = 0;
    const unsigned long n = atomic_fetch_add_explicit(&count, 1, memory_order_relaxed) + 1;
    if (n == 1 || n % 200 == 0)
      LOG_I(PHY, "PDSCH SSB-RM n=%lu frame=%d slot=%d pci=%d symbols=0x%x crb=%u..%u extra_re=%u rnti=0x%x\n",
            n, ssb_event.frame, ssb_event.slot, ssb_event.pci, ssb_event.symbols,
            ssb_event.first_crb, ssb_event.last_crb, ssb_unav, grant->rnti);
  }

  /* ---- CSI-RS rate matching: from the blind CSI-RS search's confirmed resource (the monitor fills
   * csiRsForRateMatching on the slots it occurs). The demodulator's own overlap bitmap skips the
   * REs; here only G needs the unavailable-RE count, taken from that same bitmap (TS 38.214 5.1.4.1:
   * the union of the rate-matching resources). Against a gNB that instead SUMS overlapping resources,
   * a single-CB TB that decoded before may now carry a few extra tail LLRs. */
  uint32_t csi_unav = 0;
  if (dlsch_config->numCsiRsForRateMatching > 0) {
    /* The extractor's own RE set (union over the resources, CRB parity), not the attached UE's
     * nr_ue_csi_rm_unav_res(), which sums overlapping resources: G and the LLR count must agree. */
    csi_unav = nr_dlsch_csi_unav_res(dlsch_config, freq_alloc);
    /* nr_dlsch_extract_rbs() picks the CSI-RS RE pattern by the PRB's CRB PARITY (density 0.5 differs
     * on even/odd RBs). The virtual layout moves segment s from PRB prb_start to PRB data_index, so it
     * is only exact when both have the same parity. ponytail: refused otherwise; a per-segment parity
     * swap of the overlap bitmap is the upgrade if such grants turn up. */
    for (int s = 0; seg_path && !nr_csirs_full_enabled() && s < nseg; s++) { /* CSI-FULL: per-PRB map instead */
      if ((seg[s].data_index ^ seg[s].prb_start) & 1) {
        { static _Atomic unsigned long c_ = 0;
          const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
          if (n_ == 1 || (n_ % 200) == 0)
            LOG_A(PHY, "SENSING: PDSCH UNSUP@seg-csirm-parity n=%lu\n", n_); }
        prg_arm_unsupported(grant->rnti, prg_arm);
        return out->status;
      }
    }
  }
  int n_ports = 0;
  for (int i = 0; i < 12; i++) {
    if ((dlsch_config->dmrs_ports >> i) & 0x1) {
      n_ports++;
    }
  }
  /* Multi-layer is supported up to the receive-antenna count: separating Nl spatial streams needs
   * at least Nl receive antennas, and nr_rx_pdsch()'s MIMO equaliser is what does the separation.
   * MEASURED on the live srsRAN cell (4 DL antennas, commercial UE): num_layers=4 on 97 % of grants,
   * 3 on ~2 %, 1-2 on a handful -- so the previous "single layer only" guard rejected essentially
   * every real grant (`pdsch_decode[try=0 unsup=...]`) and the data-aided source could never fire.
   * The cell cannot be reconfigured to rank 1: its RU has 4 DL ports and the DU refuses
   * nof_antennas_dl < 4 ("RU number of downlink ports=4 must match the number of transmission
   * antennas"). */
  if (n_ports < 1 || n_ports > fp->nb_antennas_rx) {
    { static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_A(PHY, "SENSING: PDSCH UNSUP@1405 n=%lu\n", n_); }
    return out->status; // cannot separate more layers than we have receive antennas
  }

  // ---- Codeword parameters. get_cw_info()'s arithmetic (nr_ue_procedures.c), minus the HARQ
  // bookkeeping a passive receiver has no state for. ----
  fapi_nr_dl_cw_info_t *cw = &dlsch_config->cw_info[0];
  memset(cw, 0, sizeof(*cw));
  cw->mcs = grant->mcs;
  cw->rv  = grant->rv;
  cw->Nl  = (uint8_t)n_ports; // was pinned to 1; the DM-RS port count IS the layer count
  cw->new_data_indicator = true;
  cw->qamModOrder = nr_get_Qm_dl(grant->mcs, grant->mcs_table);
  const uint32_t R = nr_get_code_rate_dl(grant->mcs, grant->mcs_table);
  const uint8_t nb_re_dmrs = get_num_dmrs_re_per_rb(dlsch_config->dmrsConfigType, dlsch_config->n_dmrs_cdm_groups);
  const uint16_t dmrs_len  = get_num_dmrs(dlsch_config->dlDmrsSymbPos);
  // MCS 28-31 (reserved-for-retransmission rows) have no code rate of their own (their modulation
  // order, from cw->qamModOrder above, IS already correct -- the spec table encodes it directly,
  // e.g. Table_51311[29..31] = {2,0},{4,0},{6,0}). TS 38.214 5.1.3.1: the UE reuses the TBS and
  // base graph of the initial transmission of this HARQ process. nr_harq_init_tx.h is that record,
  // keyed by (RNTI, HARQ pid) and gated on the NDI not having toggled since it was taken; a miss
  // means the true initial transmission was never observed and the grant stays refused, as before.
  nr_harq_init_tx_t init_tx = {0};
  bool have_init_tx = false;
  if (cw->qamModOrder == 0 || R == 0) {
    pthread_mutex_lock(&g_harqc_lock);
    have_init_tx = nr_harq_init_tx_lookup(&g_dl_harq_init, grant->rnti, grant->harq_pid, grant->ndi, &init_tx);
    pthread_mutex_unlock(&g_harqc_lock);
    if (!have_init_tx) {
      { static _Atomic unsigned long c_ = 0;
        const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
        if (n_ == 1 || (n_ % 200) == 0)
          LOG_A(PHY, "SENSING: PDSCH UNSUP@1423 n=%lu\n", n_); }
      return out->status;
    }
    if (init_tx.nl != cw->Nl) {
      // Layer count is this grant's OWN antenna-ports field, not replayed from the record: it
      // decides how the transmitter mapped RE-to-layer THIS occasion, which the DM-RS ports of
      // THIS DCI already reflect. A mismatch is merely logged -- it does not by itself mean the
      // record is wrong, since the spec permits a retransmission to use a different layer count.
      static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_W(PHY, "SENSING: PDSCH reserved-MCS retx rnti=0x%04x pid=%u layer count changed %u->%u\n",
              grant->rnti, grant->harq_pid, init_tx.nl, cw->Nl);
    }
  }
  cw->targetCodeRate = have_init_tx ? (uint16_t)init_tx.code_rate : (uint16_t)R;
  cw->TBS = have_init_tx ? init_tx.tbs
                        : nr_compute_tbs(cw->qamModOrder, (uint16_t)R, freq_alloc->num_rbs, dlsch_config->number_symbols,
                                         nb_re_dmrs * dmrs_len, grant->nb_rb_oh, grant->tb_scaling, cw->Nl);
  if (cw->TBS == 0) {
    { static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_A(PHY, "SENSING: PDSCH UNSUP@1431 n=%lu\n", n_); }
    return out->status;
  }
  cw->ldpcBaseGraph = have_init_tx ? init_tx.bg : get_BG(cw->TBS, cw->targetCodeRate);
  // The record write itself is deferred to the TB CRC outcome (G5 review, gap-harq): recording here,
  // right after these parameters are merely COMPUTED and before any decode is attempted, let a single
  // blind DCI false-accept (a random payload whose CRC happened to mask to an in-range RNTI, same
  // residual risk the mismatched-bits gate exists for) seed a bogus (rnti, pid, ndi) record that a
  // LATER, genuine reserved-MCS grant on that same process would then trust. Only a CRC-verified TB
  // is strong enough evidence -- see the write site at the CRC_OK branch below.
  dlsch_config->n_codewords = 1;
  /* TBS_LBRM's layer term is n_L = min(maxMIMO-LayersPDSCH, 4) -- the UE's CAPABILITY (TS 38.212
   * 5.4.2.1), NOT the rank of this particular grant. It was hardcoded to 1, which is wrong for any
   * modern UE and matters because TBS_LBRM sets N_ref and hence the LDPC circular-buffer bound
   * N_cb: get it wrong and rate DE-matching reads a buffer of the wrong length, so the CRC can
   * never pass however clean the LLRs are.
   *
   * MEASURED, and it rules out deriving this from the observed rank: on this cell our own attached
   * UE is scheduled nl=1 while its capability file declares maxNumberMIMO-LayersPDSCH=fourLayers,
   * so the gNB uses n_L=4 for it too. A "largest rank seen" heuristic would give 1 and be wrong.
   * The gNB prints its own value: tb_size_lbrm=159749 bytes = 1277992 bits, which is n_L=4.
   *
   * A passive receiver cannot read the capability off the air, so this is the deployment fact the
   * module already takes on trust elsewhere (like csirs_monitor and dci_length_override). 4 is both
   * the spec ceiling and this deployment's value, so it is the default; re-derive per deployment if
   * a cell serves layer-limited UEs. */
  /* ...and it is NOT always 4: the OAI phy-test gNB has no UE capability, set_dl_maxmimolayers()
   * falls back to 2, and N_ref then binds on every long full-band 256QAM rank-4 TB (C=119:
   * N_ref 8054 < E 9664, the transmitter wraps its circular buffer 1610 bits early) while short
   * ones (C=43) and every rank-2 / 64QAM TB stay under N_ref -- the 5-9 % "MCS-25 wall" on the
   * rfsim rank-4 bed (2026-09-16). It is decided by the TB CRC like every other cell property: a
   * failed TB whose E exceeds N_ref under a smaller n_L is re-dematched under that n_L (LDPC only,
   * the LLRs are untouched) and a pass latches it for this RNTI (rnti_nl_latch). */
  const int nl_tbslbrm = rnti_nl_get(grant->rnti, sweepable);
  /* TS 38.212 5.4.2.1 sizes N_ref from TBS_LBRM over the carrier's LARGEST configured DL BWP, not
   * over whatever frequency reference this particular grant uses. Measured 2026-08-21: on a
   * CORESET#0 format-1_0 grant BWPSize is 48 and this produced lbrm=229576 against the gNB's own
   * 1277992 -- harmless there only because N_ref cannot bind on an 808-bit TB, and wrong the moment
   * a large TB is decoded. */
  const uint16_t bw_lbrm = grant->bw_tbslbrm > 0 ? grant->bw_tbslbrm : dlsch_config->BWPSize;
  /* ...and its MODULATION term is likewise a cell property, not this grant's table. MEASURED
   * 2026-08-21 on a SIB1 grant: passing the grant's own (format-1_0-forced) table 0 gave
   * lbrm=950984 against the gNB's 1277992, a clean Qm 6-vs-8 ratio. Fixing only the bandwidth left
   * this half wrong. */
  const uint8_t tbl_lbrm = grant->mcs_table_lbrm >= 0 ? (uint8_t)grant->mcs_table_lbrm : grant->mcs_table;
  dlsch_config->tbslbrm = nr_compute_tbslbrm(tbl_lbrm, bw_lbrm, (uint8_t)nl_tbslbrm);

  const uint32_t G = nr_get_G(freq_alloc->num_rbs, dlsch_config->number_symbols, nb_re_dmrs, dmrs_len,
                              csi_unav + ssb_unav,
                              cw->qamModOrder, cw->Nl);
  if (G == 0) {
    { static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_A(PHY, "SENSING: PDSCH UNSUP@1475 n=%lu\n", n_); }
    return out->status;
  }

  out->cw = *cw;
  out->G  = G;

  {
    parmset_t k = {0};
    k.mcs      = (uint8_t)grant->mcs;
    k.tbl      = (uint8_t)grant->mcs_table;
    k.Qm       = (uint8_t)cw->qamModOrder;
    k.bg       = (uint8_t)cw->ldpcBaseGraph;
    k.nl       = (uint8_t)cw->Nl;
    k.cdm      = (uint8_t)dlsch_config->n_dmrs_cdm_groups;
    k.nscid    = (uint8_t)dlsch_config->nscid;
    k.refpt    = (uint8_t)dlsch_config->refPoint;
    k.ssym     = (uint8_t)dlsch_config->start_symbol;
    k.nsym     = (uint8_t)dlsch_config->number_symbols;
    k.dmrs_len = (uint8_t)dmrs_len;
    k.R        = (uint16_t)cw->targetCodeRate;
    k.ports    = (uint16_t)dlsch_config->dmrs_ports;
    k.scramb   = (uint16_t)dlsch_config->dlDataScramblingId;
    k.tbs      = (uint32_t)cw->TBS;
    k.dmrsmask = (uint32_t)dlsch_config->dlDmrsSymbPos;
    parmset_record(&k);
  }

  if (!passive_harq_prepare(&g_harq, fp->N_RB_DL)) {
    out->status = NR_PDSCH_PASSIVE_DECODE_ERROR;
    { static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_A(PHY, "SENSING: PDSCH UNSUP@1523 n=%lu\n", n_); }
    return out->status;
  }

  /* ---- GRANTWORK lookup (see t_gw). READY -> skip to the LDPC with the shared LLRs; OWNER -> compute as
   * always and publish before the LDPC; anything else -> the unchanged path without GrantWork. */
  nr_td_gw_llr_view_t gw_view = {0};
  const gw_meta_t *gw_meta = NULL;
  bool gw_ready = false;
  t_gw_owner = false;
  if (t_gw != NULL && t_llr_ovr == NULL && !seg_path && !(dlsch_config->pduBitmap & 0x1) && csi_unav == 0
      && dlsch_config->numCsiRsForRateMatching == 0 && nr_dlsch_csi_erase_n == 0 /* CSI-FULL erasure */
      && (!t_probe_first_seg || t_gw_probe_ok || t_gw_capture)) {
    const gw_ctx_t *gc = (const gw_ctx_t *)nr_td_grantwork_ctx(t_gw);
    const double fo_now = isnan(nr_slot_fep_fo_override_hz) ? (ue->cont_fo_comp ? ue->dl_Doppler_shift + ue->freq_offset : 0.0)
                                                            : nr_slot_fep_fo_override_hz;
    if (gc != NULL && gc->fp == gw_fingerprint(dlsch_config, freq_alloc, grant, fo_now)
        && (void *)&rxdataF[0][0] == nr_td_grantwork_fep(t_gw)) {
      const uint64_t sig = gw_key_of(dlsch_config, gc->k0, cw->Nl, cw->qamModOrder);
      const nr_td_gw_acq_t a = nr_td_grantwork_acquire(t_gw, sig, &gw_view);
      if (a == NR_TD_GW_ACQ_READY && gw_view.G == G && gw_view.meta_len == sizeof(gw_meta_t)
          && ((const gw_meta_t *)gw_view.meta)->fp == gc->fp) {
        gw_ready = true;
        gw_meta = (const gw_meta_t *)gw_view.meta;
      } else if (a == NR_TD_GW_ACQ_OWNER) {
        t_gw_owner = true;
        t_gw_sig = sig;
      }
    }
  }
  t_gw_used = gw_ready || t_gw_owner;
  t_gw_ready = gw_ready;
  if (t_gw_capture && !t_gw_owner) /* the lazy computation: READY already, or not computable here */
    return gw_ready ? NR_PDSCH_PASSIVE_DECODE_CRC_FAIL : out->status;
  const bool gw_tim = t_gw_owner && nr_td_gw_tim_on();
  uint64_t gw_t_fep = 0, gw_t_chest = 0, gw_t_demod = 0;

  /* ---- PROBE HORIZON. A layout probe decodes code block 0 only, and its E bits sit in the first
   * data symbols of the allocation (rank-4 64QAM full band: E0 = 11.8 kbit against 78 kbit per
   * symbol). Everything past the last symbol it needs -- FEP, channel estimation of later DM-RS
   * symbols, equalisation, LLRs -- is wasted; a probe then costs the LDPC of one segment plus one
   * or two symbols of PHY instead of a whole slot (8 ms at rank 4), which is what lets probing keep
   * up with 1500 grants/s OTA instead of dropping 95 % of the queue. Symbols after the horizon are
   * skipped (their LLR count stays 0, so segment 0's bits are still the first in the buffer); the
   * allocation's last symbol is still visited because nr_rx_pdsch() emits the LLRs there. */
  int probe_last_sym = -1;
  if (t_probe_first_seg && !ssb_unav && !t_gw_used) { /* GrantWork: full slot */
    const uint32_t Kcb = (cw->ldpcBaseGraph == 2) ? 3840u : 8448u;
    const uint32_t B = cw->TBS + 24u;
    const uint32_t C_est = (B <= Kcb) ? 1u : (B + (Kcb - 24u) - 1u) / (Kcb - 24u);
    if (C_est > 1) {
      const uint32_t E0 = (G + C_est - 1) / C_est;
      const uint32_t per_sym = (uint32_t)freq_alloc->num_rbs * 12u * cw->qamModOrder * cw->Nl;
      const uint32_t dmrs_sym_re = (dlsch_config->dmrsConfigType == NFAPI_NR_DMRS_TYPE1)
                                       ? 12u - 6u * dlsch_config->n_dmrs_cdm_groups
                                       : 12u - 4u * dlsch_config->n_dmrs_cdm_groups;
      uint32_t acc = 0;
      for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
        const bool is_dmrs = (dlsch_config->dlDmrsSymbPos >> m) & 1;
        acc += is_dmrs ? (uint32_t)freq_alloc->num_rbs * dmrs_sym_re * cw->qamModOrder * cw->Nl : per_sym;
        if (acc >= E0 + per_sym) { probe_last_sym = m; break; } /* one symbol of slack */
      }
      /* The first DM-RS symbol is always needed: the symbols before it are equalised against it. */
      const int first_dmrs = __builtin_ctz((unsigned)dlsch_config->dlDmrsSymbPos | (1u << 15));
      if (probe_last_sym >= 0 && probe_last_sym < first_dmrs) probe_last_sym = first_dmrs;
    }
  }
  const int probe_end = (probe_last_sym >= 0) ? probe_last_sym + 1 : dlsch_config->start_symbol + dlsch_config->number_symbols;

  /* ---- GPUPASS (Task 4, ISAC_GPU_PASS=1, default off): the whole DL pass of this grant -- exact FEP, chest, chest post, demod, descrambling, LLR norm, rate
   * dematching, LDPC, CRC -- as ONE captured CUDA graph with one host wait (nr_gpu_pass.h). Only a transport block that passes its CRC is taken: everything the
   * old path would have done is then committed below (labels gpu_llr_ready / gp_llr_done) from the pass's results. A CRC failure, a block the GPU decoder could
   * not settle, an unseen shape class, a busy pass instance or any gate that is not met leaves gp_ok false and the old path decodes from scratch, so a
   * failing grant costs the pass on top of the old path. Every ISAC_GPU_PASS_CHECK-th taken grant (default 64) is decoded by the old path too and compared. */
  bool gp_ok = false, gp_cmp = false, gp_tb = false, gp_tried = false, gp_fcmp = false;
  harqc_entry_t *gp_he = NULL; /* DL HARQ combining in the pass: the held (busy) store entry of this grant, released below */
  int gp_hq_mode = 0;          /* 0 none, 1 first transmission (the pass stores the soft sum), 2 retransmission (the pass combines with the stored sum) */
  uint32_t gp_hq_A = 0;
  bool gp_hq_retx = false;
  int gp_gate = -1; /* the first gate that keeps this grant off the pass (counted: nr_gpu_pass_dl.h NR_GPU_PASS_GATE_*) */
  if (nr_gpu_pass_active()) {
    const int gp_retry_old = 1; /* frozen value of the former ISAC_GPU_PASS_RETRY_OLD; only consulted when the split/RETRY graphs are off (memory cap), i.e. the GPU path cannot hold the retry */
    /* v2 Task 2 (ISAC_PDSCH_GPU_GRAPH_COVER): layout probes run the pass (CB0 only, their horizon), and slot-share grants: the pass estimates the same
     * widened band (share_chest_band; a segmented grant estimates per segment, the share does not widen it) and does its own FEP, so the FEP / chest caches
     * are not read (their contents are the same numbers). */
    const bool gp_cover = nr_gpu_graph_flag("COVER");
    const bool gp_probe_cov = gp_cover;
    const bool gp_share_cov = gp_cover;
    /* v2 Task 4: with ISAC_PDSCH_GPU_GRAPH_RETRY the retry bodies stay on the pass (held instance), RETRY_OLD is not consulted */
    { /* NR_TEST_FORCE_CPU_TB=1 (test seam, replay only): every grant takes the old path (host LLR normalisation, per-layer diagnostics) */
      static int fct = -1;
      if (fct < 0) fct = getenv("NR_TEST_FORCE_CPU_TB") ? 1 : 0;
      if (fct) t_tb_force_cpu = true;
    }
    gp_gate = (!nr_gpu_graph_flag("RETRY") && nr_gpu_pass_retry_to_old(gp_retry_old, t_pdtim_body, t_recov_mode == 2, t_quiet || t_decode_tdra_attempt != 0)) ? NR_GPU_PASS_GATE_RETRY
            : (seg_path && nseg > nr_gpu_pass_seg_max()) ? NR_GPU_PASS_GATE_SEG /* more segments than the largest class; fewer run the segmented classes */
            : ((t_probe_first_seg || probe_last_sym >= 0) && !gp_probe_cov) ? NR_GPU_PASS_GATE_PROBE
            : (t_share.on && !gp_share_cov) ? NR_GPU_PASS_GATE_SHARE
            : t_gw_ready ? NR_GPU_PASS_GATE_GW_READY
            : (t_gw_used || t_gw_capture) ? NR_GPU_PASS_GATE_GW /* the GrantWork owner publishes the un-normalised LLRs of this geometry for other hypotheses; the pass only returns the normalised ones */
            : t_llr_ovr != NULL ? NR_GPU_PASS_GATE_LLR_OVR
            : t_tb_force_cpu ? NR_GPU_PASS_GATE_FORCE_CPU
            : ((dlsch_config->pduBitmap & 0x1)) ? NR_GPU_PASS_GATE_PTRS
            : ue->chest_time == 1 ? NR_GPU_PASS_GATE_TINTERP
            : !nr_pdsch_gpu_ldpc_enabled() ? NR_GPU_PASS_GATE_LDPC_OFF
            : nr_dlsch_planned_branch(fp->nb_antennas_rx, cw->Nl) >= 0 ? NR_GPU_PASS_GATE_CHEST_ONLY
            : (!t_gpp && !nr_gpu_allocations_allowed() && (t_gp_claim_tried || !nr_gpu_graph_pool_has_free())) ? NR_GPU_PASS_GATE_NOMEM /* not prewarmed before the fence, no pool set */
            : -1;
    if (gp_gate < 0) { /* per-path choice (nr_gpu_path.h): SHARED -> GPU (no change); DISCRETE -> alternating trial on the first grants, then GPU or CPU */
      int pst = nr_gpu_path_state(NR_GPU_PATH_PASS_DL);
      if (pst == NR_GPU_PATH_UNSET)
        pst = nr_gpu_path_begin(NR_GPU_PATH_PASS_DL, 1, NR_GPU_PATH_MEASURE_TRIAL);
      /* once per decode call of the queue (retries keep the side). Layout probes (the solver's searches, on the pass with COVER) are never samples
       * (nr_pdsch_passive_queue.c), so they take no pick: they run on the pass unsampled. Before (sens6 rfsim 273 PRB rank 1, 2026-10-24) the probes
       * of the solve used up the 32 x n picks and the trial ended trial_incomplete with n_gpu = n_cpu = 0. */
      if (pst == NR_GPU_PATH_TRIAL && t_gp_path_side == 0 && !t_probe_first_seg && probe_last_sym < 0)
        t_gp_path_side = nr_gpu_path_pick(NR_GPU_PATH_PASS_DL) ? 3 : 2;
      if (pst == NR_GPU_PATH_CPU || (pst == NR_GPU_PATH_TRIAL && t_gp_path_side == 2))
        gp_gate = NR_GPU_PASS_GATE_PATH;
    }
    if (gp_gate >= 0)
      nr_gpu_pass_gate(gp_gate);
  }
  if (nr_gpu_pass_active() && gp_gate < 0) {
    static const nr_ssb_rm_mask_t gp_no_ssb = {0};
    fapi_nr_dl_config_dlsch_pdu_rel15_t gp_chest_cfg;
    freq_alloc_bitmap_t gp_chest_fa;
    /* no-chance gate inside the graph (nr_snr_gate.h): the pass stops after the chest when the DM-RS comb SNR < required - 5 dB. Probes (layout searches), SI/P/RA grants and
     * retransmissions (rv != 0: HARQ soft bits are needed) are never gated. */
    const double gp_req = nr_required_snr_db(grant->mcs, grant->mcs_table);
    const bool gp_gate_on = nr_snr_gate_applies(t_probe_first_seg || probe_last_sym >= 0, grant->rnti_class == 0, grant->rv, true) && isfinite(gp_req);
    const float gp_gate_thr = gp_gate_on ? (float)(gp_req - NR_SNR_GATE_MARGIN_DB) : 0.f;
    share_chest_band(freq_alloc, dlsch_config, &gp_chest_fa, &gp_chest_cfg);
    /* DL HARQ soft combining in the pass: the same decision as passive_ldpc_decode (harq_retx_decide) on the same store; a probe is HARQ-neutral. The soft bits live in device blocks of the entry
     * (hqb_ensure); a stored sum that is still on the host (an earlier CPU decode) is uploaded first. Anything that cannot be arranged decodes without combining (counted) and leaves no stale sum. */
    if (gp_hq_arms() && !t_probe_first_seg && probe_last_sym < 0) {
      gp_he = harqc_acquire(grant->rnti, grant->harq_pid);
      if (gp_he == NULL) {
        atomic_fetch_add(&g_hq_busy_skip, 1); /* as the CPU path: decoded without combining */
      } else {
        const int gp_d = harq_retx_decide(gp_he, cw, grant->ndi, &gp_hq_A);
        gp_hq_retx = gp_d > 0;
        if (gp_d < 0) { /* as the CPU path: decoded alone, the entry untouched (hq_mode 0) */
          atomic_store(&gp_he->busy, 0);
          gp_he = NULL;
        }
      }
      if (gp_he != NULL) {
        unsigned C = 0, K = 0, Z = 0, F = 0;
        const int Kc = cw->ldpcBaseGraph == 2 ? 52 : 68;
        if (gp_hq_retx && gp_he->on_gpu && gp_he->hb.n < (int)gp_he->gC) { /* cannot happen: the blocks of a live sum are never given away */
          gp_hq_retx = false;
          gp_he->soft_valid = false;
        }
        if (gp_hq_retx && !gp_he->on_gpu) { /* the sum is on the host: move it to device blocks */
          if (gp_he->d == NULL || nr_segmentation(NULL, NULL, lenWithCrc(1, gp_hq_A), &C, &K, &Z, &F, cw->ldpcBaseGraph) < 0 || C < 1 || C > NR_GPU_PASS_MAX_CB || (size_t)C * Kc * Z > gp_he->cap
              || hqb_ensure(gp_he, (int)C) != 0
              || nr_gpu_pass_dl_hq_xfer(0, gp_he->hb.id, gp_he->d, (int)C, Kc * (int)Z, (cw->ldpcBaseGraph == 1 ? 66 : 50) * (int)Z) != 0) {
            gp_hq_retx = false; /* a first transmission */
          } else {
            gp_he->on_gpu = true;
            gp_he->gC = (uint8_t)C;
          }
        }
        if (!gp_hq_retx) {
          gp_hq_A = cw->TBS;
          if (nr_segmentation(NULL, NULL, lenWithCrc(1, gp_hq_A), &C, &K, &Z, &F, cw->ldpcBaseGraph) < 0 || C < 1 || C > NR_GPU_PASS_MAX_CB || hqb_ensure(gp_he, (int)C) != 0) {
            gp_he->soft_valid = false; /* no blocks: this grant is decoded without keeping its soft bits */
            hqb_release_locked(gp_he);
          } else {
            gp_hq_mode = 1;
          }
        } else {
          gp_hq_mode = 2;
        }
      }
    }
    {
      const nr_gpu_pass_dl_in_t gin = {.ue = ue, .proc = proc, .dlsch_config = dlsch_config, .freq_alloc = freq_alloc, .seg = seg_path ? seg : NULL, .nseg = seg_path ? nseg : 0, .cw = cw,
                                       .ssb_rm = ssb_unav ? &ssb.dem : &gp_no_ssb, .rxdataF = &rxdataF[0][0], .rx_stride = fp->samples_per_slot_wCP,
                                       .rnti = grant->rnti, .G = G, .tbslbrm = dlsch_config->tbslbrm,
                                       .est_size = ((fp->symbols_per_slot * fp->ofdm_symbol_size + 15) / 16) * 16,
                                       .rx_size_symbol = (fp->N_RB_DL * NR_NB_SC_PER_RB + 15) & ~15u,
                                       .dmrs_full = (uint16_t)(dlsch_config->dlDmrsSymbPos & ((1u << fp->symbols_per_slot) - 1u)),
                                       .first_symbol = nr_ssb_rm_first_data_symbol(dlsch_config, freq_alloc, &ssb), .nvfix = 0,
                                       .branch = nr_dlsch_planned_branch(fp->nb_antennas_rx, cw->Nl), .norm = 1,
                                       .it_max = gp_iters(), .chest_diag = 0, .fep_fo = fep_fo, .tb = g_harq.b,
                                       .probe = t_probe_first_seg, .probe_last_sym = probe_last_sym, .chest_fa = &gp_chest_fa, .chest_cfg = &gp_chest_cfg,
                                       .gate_on = gp_gate_on, .gate_thr_db = gp_gate_thr, .hq_mode = gp_hq_mode, .hq_tbs = gp_hq_retx ? gp_hq_A : 0, .hq_blk = gp_hq_mode ? gp_he->hb.id : NULL};
      if (!t_gpp)
        gp_thread_prewarm(); /* before the RX fence: allocates; after it (v2): claims a pool set, lazily, once per thread */
      memset(&nr_pdsch_evm_acc, 0, sizeof(nr_pdsch_evm_acc)); /* the demod job reads whether the EVM is armed; the block below re-arms it for the old path */
      nr_pdsch_evm_acc.armed = 1;
      gp_tried = true;
      if (t_gp_path_side == 3)
        t_gp_path_side = 1; /* the GPU side of the trial ran the pass */
      if (seg_path)
        gp_seg_csi_map(seg, nseg, dlsch_config, freq_alloc); /* the demod geometry reads the CSI-FULL map of the virtual allocation */
      t_gp_sfo_q = nr_sfo_eps_q(); /* the pass builds its SFO steps from this eps (nr_gpu_pass_dl.c) */
      const int gp_rc = t_gpp ? nr_gpu_pass_dl_run(&gin, t_gpp) : 1;
      nr_dlsch_csi_prb_map = NULL; /* the old path sets and clears its own */
      nr_dlsch_csi_prb_map_n = 0;
      if (gp_he != NULL && gp_hq_mode != 0 && gp_rc == 0 && !t_gpp->r.skip) { /* the pass ran its rate dematching: the store entry follows what passive_ldpc_decode would leave */
        if (t_gpp->r.need_cpu) {
          gp_he->soft_valid = false; /* the old path decodes this grant from scratch; the device sum already holds it, so it must not be combined again */
          hqb_release_locked(gp_he);
        } else {
          const int Kc = cw->ldpcBaseGraph == 2 ? 52 : 68;
          if (gp_hq_retx) {
            atomic_fetch_add(&g_hq_retx_try, 1);
            if (t_gpp->r.tb_ok) atomic_fetch_add(&g_hq_retx_ok, 1);
          } else {
            atomic_fetch_add(&g_hq_first, 1);
          }
          t_last_hq_retx = gp_hq_retx;
          gp_he->ndi = grant->ndi;
          if (!gp_hq_retx) gp_he->first_slot = grant->source_absolute_slot;
          gp_he->tbs = gp_hq_retx ? gp_hq_A : cw->TBS;
          gp_he->soft_valid = !t_gpp->r.tb_ok; /* the soft bits are kept only while the TB is undecoded */
          gp_he->on_gpu = true;
          gp_he->gC = (uint8_t)t_gpp->C;
          gp_he->gKcz = (uint32_t)(Kc * t_gpp->Z);
          gp_he->gN = (uint32_t)((cw->ldpcBaseGraph == 1 ? 66 : 50) * t_gpp->Z);
          if (t_gpp->r.tb_ok)
            hqb_release_locked(gp_he); /* decoded: the sum is dead, the blocks go back to the pool */
        }
      }
      if (gp_rc == 0 && t_gpp->r.skip) { /* the in-graph gate fired: no demod / LDPC / CRC ran; the verdict is CRC fail, skip = 1, and no arm can help */
        if (gp_he) atomic_store(&gp_he->busy, 0);
        out->skip = 1;
        out->status = NR_PDSCH_PASSIVE_DECODE_CRC_FAIL;
        return out->status;
      }
      if (gp_rc == 0) {
        if (t_gpp->r.need_cpu) {
          nr_gpu_pass_note(NR_GPU_PASS_EV_RETRY_OLD); /* a block only the CPU decoder can settle: the old path decodes from scratch */
        } else if (!t_gpp->r.tb_ok) {
          /* CRC failure: the pass IS the old first body (bit-identical). Its LLRs are on the host (t_gpp->llr) and the bookkeeping of a failed first decode is
           * committed at gp_llr_done; the retry arms of the old path (RV, LBRM n_L, ...) then run on them, exactly as after a failed first body. */
          if (gp_hq_mode != 2 && nr_gpu_pass_shadow_due_fail()) { /* sampled (a combining retransmission is not: the old path would see the sum the pass has already stored): the old path decodes this first body too; its verdicts and LLR hash are compared with the pass's */
            gp_fcmp = true;
            t_gp_fhash = gp_llr_hash(t_gpp->llr, G);
            t_gp_fok = 0;
            for (int r = 0; r < t_gpp->C; r++)
              t_gp_fok += t_gpp->r.seg_ok[r] != 0;
            nr_gpu_pass_note(NR_GPU_PASS_EV_SHADOW_FAIL);
          } else {
            gp_ok = true;
            nr_gpu_pass_note(t_probe_first_seg ? NR_GPU_PASS_EV_PROBE : NR_GPU_PASS_EV_CONT_FAIL); /* a probe is never a TB: its outcome is seg_ok[0] */
          }
        } else {
          const uint32_t sz = (gp_hq_retx ? gp_hq_A : cw->TBS) / 8;
          bool zero = g_harq.b[sz] == 0 && g_harq.b[sz + 1] == 0;
          for (uint32_t i = 0; zero && i < sz; i++)
            zero = g_harq.b[i] == 0;
          if (zero) { /* the all-zero TB with a zero CRC is the known false pass: the old path counts and refuses it */
            nr_gpu_pass_note(NR_GPU_PASS_EV_ZERO_TB);
          } else if (gp_hq_mode != 2 && nr_gpu_pass_shadow_due()) { /* the old path decodes this one too; the verdicts are compared at the end */
            if (t_gp_shadow && sz + 8 <= GP_SHADOW_CAP) {
              memcpy(t_gp_shadow, g_harq.b, sz);
              gp_cmp = true;
            }
            nr_gpu_pass_note(NR_GPU_PASS_EV_SHADOW);
          } else {
            gp_ok = true;
            gp_tb = true;
          }
        }
      }
    }
  }

  if (gp_he != NULL) { /* the pass block is over: the old path (shadow, fallback) re-acquires the entry itself */
    if (gp_cmp || gp_fcmp)
      gp_he->soft_valid = false; /* a first transmission the old path decodes too: it must see a fresh process, not the sum the pass has just stored */
    atomic_store(&gp_he->busy, 0);
  }
  if (!gp_ok)
    nr_gpu_graph_call_dirty(); /* v2 Task 4: this body runs the stage path, which may rewrite rxdataF: the held instance's estimate is not reused */

  /* GPU LLRs in hand: everything from here to the LLR buffer (FEP, channel estimation, equaliser,
   * demodulator) is what the GPU already did for this slot. A PT-RS arm or CSI-RS rate matching
   * armed above changes the RE budget the GPU did not model, so that job stays on the CPU path. */
  const int16_t *gpu_llr = ((dlsch_config->pduBitmap & 0x1) || csi_unav || ssb_unav || nr_dlsch_csi_erase_n) ? NULL : t_llr_ovr;
  const uint32_t gpu_llr_n = gpu_llr ? t_llr_ovr_n : 0;
  atomic_fetch_add(gpu_llr ? &g_gpu_llr_jobs : &g_gpu_cpu_jobs, 1);

  // ---- FEP every symbol of the allocation. The caller keeps this buffer: the data-aided submit
  // needs the SAME Y samples to form Ĥ = Y/X, and re-transforming them would be both wasteful and a
  // chance for the two views to diverge. ----
  const int      pdtim_on = pdtim_enabled();
  const uint64_t pdt_fep  = pdtim_on ? pdtim_now() : 0;
  /* PASSIVE-RX ANTENNA PARALLELISM (2026-09-03): measured 4-antenna FEP at ~4x a single antenna's
   * cost (79.9us -> 321.3us), which together with channel estimation's own ~4x scaling pushed
   * this inline RT-thread decode over the 500us/slot budget and collapsed CRC at MCS25 (0% at 4
   * antennas vs 76-93% at 1) independent of MRC mode -- see nr_slot_fep_ant()'s comment in
   * slot_fep_nr.c. Each antenna's FEP is independent, so dispatch one per antenna across the
   * thread pool instead of looping them serially. nb_antennas_rx==1 skips the pool and matches
   * the previous behaviour exactly. */
  const int fep_s0 = t_share.on ? 0 : dlsch_config->start_symbol;
  const int fep_n  = t_share.on ? fp->symbols_per_slot
                     : (probe_last_sym >= 0 ? probe_end - dlsch_config->start_symbol : dlsch_config->number_symbols);
  if (gw_tim)
    gw_t_fep = nr_td_gw_now_ns();
  /* K32: the channel estimate always covers EVERY DM-RS symbol of the slot (the grant's full
   * dlDmrsSymbPos), never a probe horizon or the [S, S+L) window, so those symbols are transformed
   * too. A probe still skips the DATA symbols past its horizon; only the DM-RS symbols it would have
   * skipped are added, one range each. With the slot share on, the whole slot is transformed anyway. */
  const uint16_t dmrs_full = (uint16_t)(dlsch_config->dlDmrsSymbPos & ((1u << fp->symbols_per_slot) - 1u));
  int fep_rng[1 + NR_SYMBOLS_PER_SLOT][2] = {{fep_s0, fep_n}};
  int n_fep_rng = 1;
  for (int m = 0; m < fp->symbols_per_slot && m < NR_SYMBOLS_PER_SLOT; m++)
    if (((dmrs_full >> m) & 1) && (m < fep_s0 || m >= fep_s0 + fep_n)) {
      fep_rng[n_fep_rng][0] = m;
      fep_rng[n_fep_rng++][1] = 1;
    }
  /* GrantWork owner (fix round 1, I5): the same symbols as without GrantWork ([S, S+L) + every DM-RS symbol), minus
   * those an earlier signature of this grant already transformed with the same offset (per-symbol mask, extended
   * lazily). With the slot share on the whole slot is transformed as before. */
  uint16_t gw_fep_need = 0;
  bool gw_fep_hit = false;
  if (t_gw_owner && !fep_hit) {
    for (int g = 0; g < n_fep_rng; g++)
      for (int m = fep_rng[g][0]; m < fep_rng[g][0] + fep_rng[g][1] && m < NR_SYMBOLS_PER_SLOT; m++)
        gw_fep_need |= (uint16_t)(1u << m);
    if (!t_share.on) {
      const uint16_t todo = gw_fep_need & (uint16_t)~nr_td_grantwork_fep_mask(t_gw, fep_fo);
      gw_fep_hit = todo == 0;
      n_fep_rng = 0;
      for (int m = 0; m < NR_SYMBOLS_PER_SLOT; m++) {
        if (!((todo >> m) & 1))
          continue;
        if (n_fep_rng > 0 && fep_rng[n_fep_rng - 1][0] + fep_rng[n_fep_rng - 1][1] == m) {
          fep_rng[n_fep_rng - 1][1]++;
        } else {
          fep_rng[n_fep_rng][0] = m;
          fep_rng[n_fep_rng++][1] = 1;
        }
      }
    }
  }
  atomic_fetch_add(fep_hit || gw_fep_hit ? &g_fep_hit : &g_fep_miss, 1);
  if (gpu_llr || gw_ready || gp_ok) {
    /* the GPU transformed this slot / the GrantWork entry already holds this geometry's LLRs / the graph pass did the whole decode */
  } else if (fep_hit || gw_fep_hit) {
    /* same slot, same offset: this thread transformed it for the previous grant of the group */
  } else if (nr_pdsch_gpu_exact_fep_ranges(ue, fp, proc->nr_slot_rx, fep_rng, n_fep_rng, &rxdataF[0][0], fp->samples_per_slot_wCP,
                                           nr_pdsch_passive_iq_for_slot(proc->nr_slot_rx, ue->common_vars.rxdata)) == 0) {
    /* GPU6: bit-exact GPU FEP (ISAC_PDSCH_GPU_EXACT_FEP) -- identical rxdataF for the CPU chain below */
  } else if (fp->nb_antennas_rx > 1) {
    for (int g = 0; g < n_fep_rng; g++) {
      nr_slot_fep_ant_task_t fep_tasks[fp->nb_antennas_rx];
      task_ans_t fep_ans;
      init_task_ans(&fep_ans, fp->nb_antennas_rx);
      for (unsigned int ant = 0; ant < (unsigned int)fp->nb_antennas_rx; ant++) {
        fep_tasks[ant] = (nr_slot_fep_ant_task_t){.fo_hz = fep_fo, .ue = ue,
                                                  .fp = fp,
                                                  .slot = proc->nr_slot_rx,
                                                  .start_symbol = fep_rng[g][0],
                                                  .number_symbols = fep_rng[g][1],
                                                  .ant = ant,
                                                  .rxdataF_flat = &rxdataF[0][0],
                                                  .stride = fp->samples_per_slot_wCP,
                                                  .rxdata = nr_pdsch_passive_iq_for_slot(proc->nr_slot_rx, ue->common_vars.rxdata),
                                                  .ans = &fep_ans};
        task_t t = {.func = nr_slot_fep_ant_task, .args = &fep_tasks[ant]};
        pushTpool(nr_passive_tpool(&get_nrUE_params()->Tpool), t);
      }
      join_task_ans(&fep_ans);
    }
  } else {
    for (int g = 0; g < n_fep_rng; g++)
      for (int m = fep_rng[g][0]; m < fep_rng[g][0] + fep_rng[g][1]; m++)
        nr_slot_fep(ue, fp, proc->nr_slot_rx, m, rxdataF, link_type_dl, 0, nr_pdsch_passive_iq_for_slot(proc->nr_slot_rx, ue->common_vars.rxdata));
  }
  if (!fep_hit && !gpu_llr && !gw_ready && !gw_fep_hit) {
    t_fep_cache.slot = share_slot; t_fep_cache.fo = fep_fo; t_fep_cache.valid = t_share.on && (!gp_ok || t_gpp->fep_mask == 0x3fff); /* a grant-sized pass leaves the rest of the slot untransformed */
  }
  if (t_gw_owner && !fep_hit)
    nr_td_grantwork_fep_done(t_gw, fep_fo, gw_fep_need);
  if (gw_tim) {
    const uint64_t t_ = nr_td_gw_now_ns();
    nr_td_gw_tim_add(NR_TD_GWTIM_FEP, t_ - gw_t_fep);
    gw_t_chest = t_;
  }
  pdtim_add(PDTIM_FEP, pdt_fep);
  if (t_owned_iq && !gpu_llr) t_fep_done_ns = nr_td_gw_now_ns();

  if (grant->check_sample_lifetime && !nr_passive_samples_valid(
          atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed),
          grant->source_absolute_slot, fp->slots_per_frame)) {
    out->status = NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED;
    if (gp_ok)
      nr_gpu_pass_note(NR_GPU_PASS_EV_LIFETIME);
    { static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_A(PHY, "SENSING: PDSCH UNSUP@1625 n=%lu\n", n_); }
    return out->status; /* overwritten IQ is not CRC evidence */
  }
  for (int i = 0; !gpu_llr && !gw_ready && grant->source_absolute_slot >= 0 && i < dlsch_config->numCsiRsForRateMatching; i++) {
    if (dlsch_config->csiRsForRateMatching[i].csi_type != 2)
      continue; // NZP-only slots cost nothing here
    const double zs = nr_pdsch_passive_zp_grant_score(fp, dlsch_config, freq_alloc, &rxdataF[0][0],
                                                      fp->samples_per_slot_wCP, proc->nr_slot_rx, fep_s0, fep_n,
                                                      ssb_event.symbols, grant->scr_dedicated, i);
    if (zs >= 0.0)
      if (nr_cfg_epoch_work_current()) nr_csirs_blind_rt_zp_grant_evidence((uint32_t)grant->source_absolute_slot, &dlsch_config->csiRsForRateMatching[i], zs);
  }

  // ---- Channel estimation on the DM-RS symbols. ----
  {
    extern __thread uint32_t nr_dl_chest_delay_epoch; /* GPU stage-record context key: one epoch per decode */
    if (++nr_dl_chest_delay_epoch == 0) nr_dl_chest_delay_epoch = 1;
  }
  const uint64_t pdt_che = pdtim_on ? pdtim_now() : 0;
  const uint32_t pdsch_est_size = ((fp->symbols_per_slot * fp->ofdm_symbol_size + 15) / 16) * 16;
  /* ---- PERSISTENT SCRATCH, NOT PER-GRANT allocation --------------------------------------------
   * These handles used to be locals, allocated and freed on every decode. allocCast2D/3D are
   * DESIGNED to persist -- CheckArrAllocated only allocates `if (!(ArraY))`, and resizeAllowed
   * exists so a shape change is handled -- and declaring the handle as a local defeated exactly
   * that.
   *
   * The cost is not the malloc, it is the pages. malloc16_clear() is memalign + memset, and above
   * glibc's mmap threshold every allocation returns fresh pages that the memset then faults in one
   * by one, with munmap giving them back at the end. Measured on these exact shapes
   * (tests/passive_rx/allocbench.c, run twice on this host): 1469 us per grant to
   * allocate+clear+free ~4 MB, against 75 us to clear the same buffers when they persist. ~19.6x.
   *
   * THREAD-LOCAL, not static: nr_pdsch_passive_queue runs N consumers concurrently and each needs
   * its own. Heap-backed via the existing allocator rather than a __thread array -- the AoA work
   * records a shifted TLS layout producing an AVX alignment fault, and these are far larger than
   * the buffer that did it. */
  decode_bufs_prepare(fp); /* DECODETIME: allocates (idempotent) and, with ISAC_PDSCH_GPU_HOST_PIN=1, pins the GPU-touched buffers */
  // One estimate per (layer, rx antenna) -- nr_rx_pdsch() indexes this as nl*nb_antennas_rx + aarx,
  // matching nr_ue_pdsch_procedures()'s own allocation. Estimating only layer 0 (as this used to)
  // gives the equaliser nothing to separate the other layers with.
  /* dim1 at the MAXIMUM layer count, not cw->Nl: nr_rx_pdsch() indexes this as nl*nb_antennas_rx +
   * aarx, so an over-allocation is harmless, while sizing it by the runtime layer count would make
   * the shape vary per grant and force a reallocation (and a "resizing" log line) on every change.
   * dim2 is the stride and is already constant. */
  allocCast2D(pdsch_dl_ch_estimates, int32_t, toFree, fp->nb_antennas_rx * NR_MAX_NB_LAYERS, pdsch_est_size, true);
  if (gpu_llr || gw_ready || gp_ok)
    goto gpu_llr_ready; /* after the variably-modified declaration above, which a jump may not cross */

  uint32_t nvar = 0, nvar_den = 1u; // nvar_den: hoisted divisor, also cached by the slot-share
  int n_dmrs_sym = 0;
  int dmrs_first = -1, dmrs_last = -1; 
  /* SINGLE-BRANCH DECODE (P39): when the demodulator reads one branch (ISAC_RX_MRC_MODE=0 -> branch 0,
   * or a pinned branch), estimate only that branch. The other three were estimated and then never read:
   * 4x the chest cost (~1.2 ms/grant OTA) for nothing. Sensing does not use this estimate -- it forms
   * H_i = Y_i / X from the raw per-antenna FEP output, which is still produced for every antenna. */
  const int chest_only = nr_dlsch_planned_branch(fp->nb_antennas_rx, cw->Nl);
  /* Estimate over the group's union of RBs so the slot's next grant can reuse it. */
  fapi_nr_dl_config_dlsch_pdu_rel15_t chest_cfg;
  freq_alloc_bitmap_t chest_alloc;
  share_chest_band(freq_alloc, dlsch_config, &chest_alloc, &chest_cfg);
  const nr_pdsch_chest_key_t chest_key = {.slot = share_slot,
                                          .dmrs_pos = dmrs_full,
                                          .cfg_type = (uint8_t)dlsch_config->dmrsConfigType,
                                          .nscid = dlsch_config->nscid,
                                          .cdm = dlsch_config->n_dmrs_cdm_groups,
                                          .nl = cw->Nl,
                                          .dmrs_ports = (uint16_t)(dlsch_config->dmrs_ports & 0xfffu),
                                          .scr = dlsch_config->dlDmrsScramblingId,
                                          .rb_lo = (int)chest_alloc.first_rb,
                                          .rb_n = (int)chest_alloc.num_rbs,
                                          .bwp_start = dlsch_config->BWPStart,
                                          .bwp_size = dlsch_config->BWPSize,
                                          .ref_point = dlsch_config->refPoint,
                                          .only_ant = chest_only,
                                          .n_ant = fp->nb_antennas_rx,
                                          .fo_hz = fep_fo,
                                          .seg = seg_path};
  const bool chest_cacheable = t_share.on && !seg_path;
  const int chest_hit = chest_cacheable && t_chest_cache.valid && nr_pdsch_chest_key_eq(&t_chest_cache.key, &chest_key);
  atomic_fetch_add(chest_hit ? &g_chest_hit : &g_chest_miss, 1);
  /* Cached rows: every (layer, antenna) row the estimator fills (one branch per layer when
   * chest_only >= 0), at every DM-RS symbol of the slot. Stored compactly: cache row j = r, or the
   * layer r / n_ant when a single branch is estimated. */
  const int chest_nrow = cw->Nl * fp->nb_antennas_rx;
  const int chest_ndmrs = __builtin_popcount(dmrs_full);
  const int chest_nstore = chest_only >= 0 ? cw->Nl : chest_nrow;
  if (chest_hit) {
    /* Working copy of the immutable cached estimate: everything below may mutate it. The rows the
     * estimator does not fill are ZEROED at each DM-RS symbol exactly as a miss leaves them
     * (nr_pdsch_channel_estimation() memsets every antenna's row before skipping the unplanned
     * ones): DMRSFO (row 0), the branch gate, CHESTDIAG and EQDIAG read them, so hit == miss bit for bit. */
    for (int r = 0; r < chest_nrow; r++) {
      const bool filled = chest_only < 0 || r % fp->nb_antennas_rx == chest_only;
      const int j = chest_only >= 0 ? r / fp->nb_antennas_rx : r;
      for (int m = 0, k = 0; m < fp->symbols_per_slot; m++) {
        if (!((dmrs_full >> m) & 1))
          continue;
        c16_t *dst = (c16_t *)&pdsch_dl_ch_estimates[r][fp->ofdm_symbol_size * m];
        if (filled)
          memcpy(dst, &t_chest_cache.est[((size_t)j * chest_ndmrs + k) * fp->ofdm_symbol_size],
                 sizeof(c16_t) * fp->ofdm_symbol_size);
        else
          memset(dst, 0, sizeof(c16_t) * fp->ofdm_symbol_size);
        k++;
      }
    }
    nvar = t_chest_cache.nvar_sum; n_dmrs_sym = t_chest_cache.n_dmrs_sym;
    dmrs_first = t_chest_cache.dmrs_first; dmrs_last = t_chest_cache.dmrs_last;
    memcpy(nr_dl_chest_nvar_ant, t_chest_cache.nvar_ant, sizeof(t_chest_cache.nvar_ant));
  }
  /* ---- SEGMENTED ESTIMATE (seg_path only). nr_pdsch_channel_estimation() memsets its whole
   * output row and writes it from index 0 = the first RB of the allocation it is given, so each
   * segment is estimated into the row, parked at its DATA offset (data_index*12) in seg_h, and the
   * row is then rewritten in that data-ordered layout -- the one nr_rx_pdsch() below reads for the
   * virtual contiguous allocation. A 1-PRB segment is fine: the estimator has no minimum size (its
   * FIR/pilot loop is the same arithmetic the attached UE runs on 1-RB grants). nvar is weighted by
   * segment width so the sum keeps its (DM-RS symbol x layer) meaning; nr_dl_chest_nvar_ant[] ends
   * up holding the WIDTH-WEIGHTED MEAN across this symbol's segments (fix round 1, 15658710b9) --
   * NOT just the last segment's raw value, which is what this comment used to say before that fix
   * landed a few lines below. It is still only the last DM-RS SYMBOL's mean across symbols, the
   * same last-call approximation the branch-substitution block further down already documents. The
   */
  static __thread c16_t *seg_h = NULL;
  static __thread size_t seg_h_cap = 0;
  const int nsc_seg = freq_alloc->num_rbs * NR_NB_SC_PER_RB;
  if (seg_path && seg_h_cap < (size_t)fp->nb_antennas_rx * nsc_seg) {
    free(seg_h);
    seg_h_cap = (size_t)fp->nb_antennas_rx * NR_PRB_SET_MAX * NR_NB_SC_PER_RB;
    seg_h = (c16_t *)malloc16(seg_h_cap * sizeof(c16_t));
    if (seg_h == NULL) {
      seg_h_cap = 0;
      out->status = NR_PDSCH_PASSIVE_DECODE_ERROR;
      return out->status;
    }
  }
  uint32_t gch_nv[14][4], gch_nva[14][4][NR_DL_CHEST_MAX_ANT]; /* GPU-DEMOD stage 2 (ISAC_PDSCH_GPU_EXACT_CHEST) */
  const int gseg = seg_path ? nr_pdsch_gpu_chest_grant(ue, proc, dlsch_config, freq_alloc, seg, nseg, cw->Nl, dmrs_full, pdsch_est_size,
                                                       pdsch_dl_ch_estimates, fp->samples_per_slot_wCP, rxdataF, 0, gch_nv, gch_nva) : -1;
  if (gseg == 0) { /* what the loop below leaves in its locals */
    chest_alloc = set_bitmap_from_start_size(seg[nseg - 1].prb_start, seg[nseg - 1].n_prb);
    chest_cfg.start_rb = seg[nseg - 1].prb_start;
    chest_cfg.number_rbs = seg[nseg - 1].n_prb;
    chest_cfg.resource_alloc = 1;
  }
  for (int m = 0; seg_path && m < fp->symbols_per_slot; m++) { /* K32: every DM-RS symbol of the slot */
    if (!((dmrs_full >> m) & 1))
      continue;
    if (dmrs_first < 0)
      dmrs_first = m;
    dmrs_last = m;
    for (int nl = 0; nl < cw->Nl && gseg == 0; nl++) {
      nvar += gch_nv[m][nl];
      memcpy(nr_dl_chest_nvar_ant, gch_nva[m][nl], sizeof(gch_nva[m][nl]));
    }
    for (int nl = 0; nl < cw->Nl && gseg != 0; nl++) {
      uint64_t nv = 0;
      uint64_t nv_ant[NR_DL_CHEST_MAX_ANT] = {0}; /* per-branch, width-weighted: see the publication below */
      for (int s = 0; s < nseg; s++) {
        chest_alloc = set_bitmap_from_start_size(seg[s].prb_start, seg[s].n_prb);
        chest_cfg.start_rb = seg[s].prb_start;
        chest_cfg.number_rbs = seg[s].n_prb;
        chest_cfg.resource_alloc = 1; /* each segment is contiguous */
        uint32_t nvar_tmp = 0;
        nr_pdsch_channel_estimation(ue, proc, &chest_cfg, &chest_alloc, nl,
                                    get_dmrs_port(nl, dlsch_config->dmrs_ports), (unsigned char)m, pdsch_est_size,
                                    pdsch_dl_ch_estimates, fp->samples_per_slot_wCP, rxdataF, &nvar_tmp);
        nv += (uint64_t)nvar_tmp * seg[s].n_prb;
        for (int a = 0; a < fp->nb_antennas_rx && a < NR_DL_CHEST_MAX_ANT; a++)
          nv_ant[a] += (uint64_t)nr_dl_chest_nvar_ant[a] * seg[s].n_prb;
        for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++)
          memcpy(&seg_h[aarx * nsc_seg + seg[s].data_index * NR_NB_SC_PER_RB],
                 &pdsch_dl_ch_estimates[nl * fp->nb_antennas_rx + aarx][fp->ofdm_symbol_size * m],
                 (size_t)seg[s].n_prb * NR_NB_SC_PER_RB * sizeof(c16_t));
      }
      nvar += (uint32_t)(nv / (uint64_t)freq_alloc->num_rbs);
      /* The estimator overwrites nr_dl_chest_nvar_ant[] per call, so after the loop it would hold the
       * LAST segment's value -- possibly a single PRB -- which the per-branch nvar substitution
       * (per-branch nvar) then feeds the equaliser. Publish the width-weighted mean. */
      for (int a = 0; a < fp->nb_antennas_rx && a < NR_DL_CHEST_MAX_ANT; a++)
        nr_dl_chest_nvar_ant[a] = (uint32_t)(nv_ant[a] / (uint64_t)freq_alloc->num_rbs);
      for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++) {
        c16_t *row = (c16_t *)&pdsch_dl_ch_estimates[nl * fp->nb_antennas_rx + aarx][fp->ofdm_symbol_size * m];
        memset(row, 0, sizeof(c16_t) * fp->ofdm_symbol_size);
        memcpy(row, &seg_h[aarx * nsc_seg], (size_t)nsc_seg * sizeof(c16_t));
      }
    }
    n_dmrs_sym++;
  }
  nr_dl_chest_only_ant = chest_only; /* P39: non-segmented path only; reset after the loop */
  const int gch = (!seg_path && !chest_hit && chest_only < 0) ? nr_pdsch_gpu_chest_grant(ue, proc, &chest_cfg, &chest_alloc, NULL, 0, cw->Nl,
      dmrs_full, pdsch_est_size, pdsch_dl_ch_estimates, fp->samples_per_slot_wCP, rxdataF, 0, gch_nv, gch_nva) : -1;
  for (int m = 0; !seg_path && !chest_hit && m < fp->symbols_per_slot; m++) { /* K32: every DM-RS symbol of the slot */
    if (!((dmrs_full >> m) & 1)) {
      continue;
    }
    if (dmrs_first < 0) {
      dmrs_first = m;
    }
    dmrs_last = m;
    for (int nl = 0; nl < cw->Nl; nl++) { // mirrors nr_ue_pdsch_procedures()'s per-layer loop
      uint32_t nvar_tmp = 0;
      if (gch == 0) { nvar_tmp = gch_nv[m][nl]; memcpy(nr_dl_chest_nvar_ant, gch_nva[m][nl], sizeof(gch_nva[m][nl])); } else
      nr_pdsch_channel_estimation(ue, proc, &chest_cfg, &chest_alloc, nl,
                                  get_dmrs_port(nl, dlsch_config->dmrs_ports), (unsigned char)m, pdsch_est_size,
                                  pdsch_dl_ch_estimates, fp->samples_per_slot_wCP, rxdataF, &nvar_tmp);
      nvar += nvar_tmp;
    }
    n_dmrs_sym++;
  }
  nr_dl_chest_only_ant = -1;
  if (n_dmrs_sym == 0) {
    { static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_A(PHY, "SENSING: PDSCH UNSUP@1792 n=%lu\n", n_); }
    return out->status; // no DM-RS in the allocation: nothing to equalise against
  }
  if (chest_cacheable && !chest_hit) {
    /* Store the IMMUTABLE copy now, before any post-processing below touches the working buffer. */
    const size_t need = (size_t)chest_nstore * chest_ndmrs * fp->ofdm_symbol_size;
    t_chest_cache.valid = 0;
    if (t_chest_cache.est_cap < need) {
      free(t_chest_cache.est);
      t_chest_cache.est = (c16_t *)malloc16(need * sizeof(c16_t));
      t_chest_cache.est_cap = t_chest_cache.est ? need : 0;
    }
    if (t_chest_cache.est) {
      for (int r = 0; r < chest_nrow; r++) {
        if (chest_only >= 0 && r % fp->nb_antennas_rx != chest_only)
          continue;
        const int j = chest_only >= 0 ? r / fp->nb_antennas_rx : r;
        for (int m = 0, k = 0; m < fp->symbols_per_slot; m++) {
          if (!((dmrs_full >> m) & 1))
            continue;
          memcpy(&t_chest_cache.est[((size_t)j * chest_ndmrs + k) * fp->ofdm_symbol_size],
                 &pdsch_dl_ch_estimates[r][fp->ofdm_symbol_size * m], sizeof(c16_t) * fp->ofdm_symbol_size);
          k++;
        }
      }
      t_chest_cache.key = chest_key;
      t_chest_cache.nvar_sum = nvar;
      memcpy(t_chest_cache.nvar_ant, nr_dl_chest_nvar_ant, sizeof(t_chest_cache.nvar_ant));
      t_chest_cache.n_dmrs_sym = n_dmrs_sym;
      t_chest_cache.dmrs_first = dmrs_first;
      t_chest_cache.dmrs_last = dmrs_last;
      t_chest_cache.valid = 1;
    }
  }
  nr_dlsch_chest_per_symbol = 0;
  /* SFO cross-check (PHY/MODULATION/nr_sfo_state.h). On a real grant (no probe, no trial) with two or more DM-RS symbols the channel and the
   * precoder are static over the slot, so between the first and the last DM-RS estimate a common phase is residual CFO and a phase SLOPE across
   * subcarriers is residual timing drift: the eps the FEP did not remove. Weighted LS fit of the per-4-RB phase against the signed subcarrier,
   * fed back to the provider with the grant's coherence as weight (a hypothesis without DM-RS reads ~0 coherence and is ignored). */
  if (dmrs_first >= 0 && dmrs_last > dmrs_first && !seg_path && !t_probe_first_seg && probe_last_sym < 0 && !t_quiet && cw->Nl > 0
      && chest_alloc.num_rbs >= 24) {
    const int N = fp->ofdm_symbol_size, nrx = fp->nb_antennas_rx < 4 ? fp->nb_antennas_rx : 4, nre = 12 * chest_alloc.num_rbs;
    const int k0 = (fp->first_carrier_offset + (chest_cfg.BWPStart + chest_alloc.first_rb) * 12) % N;
    double sw = 0, swx = 0, swy = 0, swxx = 0, swxy = 0, coh_n = 0, coh_d = 0;
    for (int c0 = 0; c0 + 48 <= nre; c0 += 48) {
      double cr = 0, ci = 0, am = 0;
      for (int l = 0; l < (int)cw->Nl; l++)
        for (int a = 0; a < nrx; a++) {
          const int16_t *h1 = (const int16_t *)&pdsch_dl_ch_estimates[l * fp->nb_antennas_rx + a][N * dmrs_first];
          const int16_t *h2 = (const int16_t *)&pdsch_dl_ch_estimates[l * fp->nb_antennas_rx + a][N * dmrs_last];
          for (int i = c0; i < c0 + 48; i++) {
            const double x1r = h1[2 * i], x1i = h1[2 * i + 1], x2r = h2[2 * i], x2i = h2[2 * i + 1];
            cr += x2r * x1r + x2i * x1i;
            ci += x2i * x1r - x2r * x1i;
            am += sqrt((x1r * x1r + x1i * x1i) * (x2r * x2r + x2i * x2i));
          }
        }
      const double mag = sqrt(cr * cr + ci * ci);
      if (!(mag > 0))
        continue;
      const double x = nr_sfo_bin_k((k0 + c0 + 24) % N, N), y = atan2(ci, cr);
      sw += mag; swx += mag * x; swy += mag * y; swxx += mag * x * x; swxy += mag * x * y;
      coh_n += mag; coh_d += am;
    }
    const double den = sw * swxx - swx * swx, coh = coh_d > 0 ? coh_n / coh_d : 0;
    const int dn = nr_sfo_symbol_offset(N, fp->nb_prefix_samples, fp->nb_prefix_samples0, fp->numerology_index, fp->symbols_per_slot, proc->nr_slot_rx,
                                        dmrs_last)
                   - nr_sfo_symbol_offset(N, fp->nb_prefix_samples, fp->nb_prefix_samples0, fp->numerology_index, fp->symbols_per_slot, proc->nr_slot_rx,
                                          dmrs_first);
    if (den > 0 && dn > 0 && coh > 0.5) {
      const double slope = (sw * swxy - swx * swy) / den; /* rad per subcarrier */
      /* a remaining drift d = eps_res * dn samples turns subcarrier k by -2 pi k d / N (nr_sfo_rot.h): eps_res = -slope N / (2 pi dn) */
      const double eps_res = -slope * N / (2 * M_PI * dn);
      nr_sfo_note_dmrs_residual(eps_res, coh);
      /* the common term at DC as a frequency: what the FEP's frequency offset leaves (log only) */
      const double hz0 = (swy - slope * swx) / sw / (2 * M_PI * dn / (1000.0 * fp->samples_per_subframe));
      static _Atomic uint64_t s_n;
      static double s_sum, s_hz; /* log only: a torn sum costs one line's mean */
      s_sum += eps_res;
      s_hz += hz0;
      const uint64_t n = atomic_fetch_add_explicit(&s_n, 1, memory_order_relaxed) + 1;
      if (n % 4000 == 0) {
        LOG_I(PHY, "SENSING: SFO_DMRS n=%lu mean residual %+.3f ppm, common term at DC %+.1f Hz (last %+.3f ppm, coherence %.2f) applied eps %+.3f ppm\n",
              (unsigned long)n, s_sum / 4000 * 1e6, s_hz / 4000, eps_res * 1e6, coh, nr_sfo_eps() * 1e6);
        s_sum = s_hz = 0;
      }
    }
  }
  /* EVMGRANT dmrs_snr / dmrs_pw / dmrs_ref (CPU chest, GPU exact chest, chest cache): last DM-RS symbol and layer, as nr_dl_chest_nvar_ant. The GPU graph pass fills
   * the same from its host copy of the estimate (nr_gpu_pass_graph.cu); a gpu_llr / gw_ready grant has no chest of its own and stays path 0 (-99). */
  {
    nr_dmrs_snr_init(&out->dmrs_snr);
    out->dmrs_path = 0;
    if (dmrs_last >= 0 && cw->Nl > 0) {
      const int nrx = fp->nb_antennas_rx < 4 ? fp->nb_antennas_rx : 4, Nfft = fp->ofdm_symbol_size;
      const int nrb = seg_path ? freq_alloc->num_rbs : chest_alloc.num_rbs;
      const int16_t *rows[4] = {0, 0, 0, 0};
      uint32_t nres[4] = {0, 0, 0, 0};
      for (int a = 0; a < nrx; a++) {
        rows[a] = (const int16_t *)&pdsch_dl_ch_estimates[(cw->Nl - 1) * fp->nb_antennas_rx + a][Nfft * dmrs_last];
        nres[a] = nr_dl_chest_nvar_ant[a];
      }
      /* LS = rx * pilot >> 15 and |pilot| = 2^14 / beta_dmrs: LS power = rx power * (1 / (2 beta))^2 */
      const double beta = get_beta_dmrs(dlsch_config->n_dmrs_cdm_groups, dlsch_config->dmrsConfigType == NFAPI_NR_DMRS_TYPE2);
      nr_dmrs_snr_fill(&out->dmrs_snr, rows, nrx, 12 * nrb, nres, dlsch_config->dmrsConfigType == NFAPI_NR_DMRS_TYPE2 ? 2 : 1, 4.0 * beta * beta);
      if (!seg_path)
        out->dmrs_snr.ref_db = nr_dmrs_ref_db((const int16_t *)&rxdataF[0][Nfft * dmrs_last], Nfft, fp->first_carrier_offset, 12 * fp->N_RB_DL,
                                              (fp->first_carrier_offset + (chest_cfg.BWPStart + chest_alloc.first_rb) * 12) % Nfft, 12 * nrb);
      out->dmrs_path = chest_hit ? 4 : ((seg_path ? gseg : gch) == 0) ? 2 : 1;
    }
  }
  /* TAIL-FIX no-chance gate (nr_snr_gate.h): the chest's own DM-RS SNR (comb) is more than 5 dB below the SNR this MCS needs: no demod, no LDPC, no retry.
   * Probes (layout searches) are discovery and SI/P/RA grants carry no MCS-table evidence: never gated. With HARQ combining on a retransmission is always decoded.
   * (The GPU graph pass estimates inside its graph and is not gated here.) */
  if (out->dmrs_path != 0
      && nr_snr_gate_applies(t_probe_first_seg || probe_last_sym >= 0, grant->rnti_class == 0, grant->rv, true)
      && nr_snr_no_chance(out->dmrs_snr.comb_db, grant->mcs, grant->mcs_table)) {
    out->skip = 1;
    out->status = NR_PDSCH_PASSIVE_DECODE_CRC_FAIL;
    return out->status;
  }
  /* nvar normalisation: the mirror of nr_ue_pdsch_procedures() (upstream OAI arithmetic), kept bit-identical on purpose. */
  nvar_den = (uint32_t)(dlsch_config->number_symbols * cw->Nl * fp->nb_antennas_rx);
  if (nvar_den == 0) nvar_den = 1u;
  nvar /= nvar_den;
  /* ---- Match nvar to the branch actually decoded (2026-09-04) ---------------------------------
   * nvar above is the MEAN over every receive branch, but the rank-1 four-RX path decodes ONE
   * branch. On this rig three of the four are noise, so the mean tells the equaliser the channel is
   * far noisier than the branch it actually reads, and the LLRs clip -- which is why branch 0 scored
   * 51-64 % at 4 antennas but 76 % at --ue-nb-ant-rx 1, decoding the same branch either way.
   * This is the DL counterpart of the UL scale fix in nr_ulsch_demodulation.c.
   *
   * Only applied when the branch is known in advance (mode 0, the default) and that branch actually
   * produced an estimate; otherwise the mean stands, so nothing changes for the attached UE, for
   * mode 1/2/3, or for a single-antenna receiver. */
  {
    const int dl_branch = nr_dlsch_planned_branch(fp->nb_antennas_rx, cw->Nl);
    if (dl_branch >= 0 && dl_branch < NR_DL_CHEST_MAX_ANT && nr_dl_chest_nvar_ant[dl_branch] > 0) {
      /* SCALE-PRESERVING substitution, not a raw one. `nvar` at this point is
       *     (sum over n_dmrs_sym x Nl calls of the per-antenna MEAN) / den
       * while nr_dl_chest_nvar_ant[] holds one RAW per-antenna value. Swapping the mean for a
       * single branch means re-applying the same accumulation and divisor, otherwise the equaliser
       * gets a number that is off by (n_dmrs_sym x Nl)/den -- a large factor, and exactly the class
       * of silent scale error this file's history is full of.
       *
       * Approximation, stated: nr_dl_chest_nvar_ant[] is overwritten per call, so it carries the
       * LAST DM-RS symbol's value rather than an average over them. That is the same approximation
       * the per-antenna publication already makes, and at Nl=1 with 2-3 DM-RS symbols it is small
       * next to the 4x error it replaces. */
      const uint64_t scaled = (uint64_t)nr_dl_chest_nvar_ant[dl_branch] * (uint64_t)(n_dmrs_sym * cw->Nl);
      const uint32_t nvar_branch = (uint32_t)(scaled / nvar_den);
      if (nvar_branch > 0) {
        nvar = nvar_branch;
      }
    }
  }

  pdtim_add(PDTIM_CHEST, pdt_che);
  if (gw_tim) {
    const uint64_t t_ = nr_td_gw_now_ns();
    nr_td_gw_tim_add(NR_TD_GWTIM_CHEST, t_ - gw_t_chest);
  }

  out->nvar = nvar;
  out->qm_measured = 0;

  if (ue->chest_time == 1) { /* every DM-RS symbol is estimated, probes included (K32) */
    nr_chest_time_domain_avg(fp, (int32_t **)pdsch_dl_ch_estimates, dlsch_config->number_symbols,
                             dlsch_config->start_symbol, dlsch_config->dlDmrsSymbPos, freq_alloc->num_rbs, cw->Nl,
                             fp->nb_antennas_rx);
  }

  // ---- Demodulate to LLRs, symbol by symbol. nr_rx_pdsch() reads its transport-block parameters
  // out of a NR_UE_DLSCH_t and a NR_DL_UE_HARQ_t; both are stack-local here, deliberately (see the
  // header). `status = NR_ACTIVE` is what makes it apply the PTRS/symbol-span branch consistently
  // with the attached path -- with pduBitmap==0 it only selects the symbol bookkeeping. ----
gpu_llr_ready:;
  if (gpu_llr) { nvar = 0; nvar_den = 1u; n_dmrs_sym = 0; } /* skipped by the jump; read only on the guarded retries */
  if (gp_ok) { /* skipped by the jump too: what the graph pass measured (the chest post, the DM-RS FO tracker, the per-branch FO estimate) */
    nvar = t_gpp->r.nvar; nvar_den = 1u; n_dmrs_sym = t_gpp->ndmrs;
    out->dmrs_snr = t_gpp->r.dsnr; /* LS units: rx power = LS power * 4 beta^2 (see the chest-side fill) */
    if (out->dmrs_snr.pw_db > -90.f)
      out->dmrs_snr.pw_db += (float)(20.0 * log10(2.0 * get_beta_dmrs(dlsch_config->n_dmrs_cdm_groups, dlsch_config->dmrsConfigType == NFAPI_NR_DMRS_TYPE2)));
    out->dmrs_path = 3;
    dmrs_first = t_gpp->dmrs_first; dmrs_last = t_gpp->dmrs_last;
    out->nvar = nvar;
    for (int a = 0; a < NR_DL_CHEST_MAX_ANT; a++)
      nr_dl_chest_nvar_ant[a] = a < 4 ? t_gpp->nvar_ant[a] : 0;
    nr_gpu_pass_note(NR_GPU_PASS_EV_COMMIT);
  }
  if (gw_ready) { /* skipped by the jump too: what the entry's computation measured */
    nvar = gw_meta->nvar; nvar_den = 1u; n_dmrs_sym = 0;
    dmrs_first = dmrs_last = -1;
    out->nvar = gw_meta->nvar;
    out->qm_measured = gw_meta->qm_measured;
  }
  NR_UE_DLSCH_t dlsch = {0};
  dlsch.cw_info = *cw;
  dlsch.rnti = grant->rnti;
  dlsch.rnti_type = TYPE_C_RNTI_;
  dlsch.active = true;
  dlsch.max_ldpc_iterations = (uint8_t)nr_pdsch_ldpc_cap();

  NR_DL_UE_HARQ_t harq = {0};
  harq.status = NR_ACTIVE;
  harq.first_rx = 1;

  const uint64_t pdt_alc = pdtim_on ? pdtim_now() : 0;
  /* Sized from the PT-RS-FREE RE count, not from G. G subtracts the PT-RS REs, but the demodulator
   * emits LLRs by its own per-symbol RE bookkeeping; when a PT-RS hypothesis is wrong for this cell
   * (the density sweep tries them) the two disagree and nr_dlsch_layer_demapping() overran a G-sized
   * buffer -- SIGSEGV in memcpy on the first swept grant, OTA 2026-09-15 under gdb. The upper bound
   * costs a few hundred bytes; the LDPC still reads exactly G. */
  const uint32_t G_max = nr_get_G(freq_alloc->num_rbs, dlsch_config->number_symbols, nb_re_dmrs, dmrs_len,
                                  0, cw->qamModOrder, cw->Nl);
  const uint32_t rx_llr_buf_sz = ALIGNARRAYSIZE(G_max > G ? G_max : G, 32);
  /* Grow-only, and cleared each call: nr_rx_pdsch writes one symbol's worth at a time and the
   * decoder reads G of them, so stale bytes past this grant's G must not be visible. Clearing is
   * the 75 us the measurement above already accounts for; the allocation is what is being removed. */
  const bool llr_fresh = llr_cap_tls < rx_llr_buf_sz; /* a fresh malloc16_clear buffer is already zero */
  int16_t *llr = llr_prepare(fp, rx_llr_buf_sz);
  if (llr && !llr_fresh && !(gp_ok && gp_tb)) /* a decoded pass leaves the LLRs on the device */
    memset(llr, 0, (size_t)rx_llr_buf_sz * sizeof(int16_t));
  if (llr == NULL) {
    out->status = NR_PDSCH_PASSIVE_DECODE_ERROR;
    { static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_A(PHY, "SENSING: PDSCH UNSUP@2453 n=%lu\n", n_); }
    return out->status;
  }

  /* FIXED at the widest allocation this carrier can carry, rather than sized to this grant. It is
   * used for exactly two things -- the shape of the four buffers below and the stride handed to
   * nr_rx_pdsch() -- so as long as both agree, a larger stride is harmless: the equaliser writes
   * freq_alloc->num_rbs*12 entries into each row either way. Keeping it constant is what lets the
   * buffers persist across grants of different widths without reallocating. */
  const uint32_t rx_size_symbol = (fp->N_RB_DL * NR_NB_SC_PER_RB + 15) & ~15;
  /* Middle dimension MUST be NR_MAX_NB_LAYERS, not cw->Nl: nr_rx_pdsch() declares these as
   * c16_t buf[][NR_MAX_NB_LAYERS][pdsch_buf_size_max], so it indexes symbol m with a COMPILE-TIME
   * stride of NR_MAX_NB_LAYERS * pdsch_buf_size_max. Allocating with the runtime layer count made
   * the per-symbol stride too small for any grant with Nl < NR_MAX_NB_LAYERS, and every write past
   * symbol 0 landed outside the buffer.
   * It never showed up before because this cell only ever scheduled Nl = 4, where the two happen to
   * be equal. The moment the gNB was reconfigured to max_rank = 1 it segfaulted on the first
   * full-band (273 PRB) grant -- big enough for the overrun to leave the mapping. */

  allocCast3D(rxdataF_comp, c16_t, toFree2, fp->symbols_per_slot, NR_MAX_NB_LAYERS, rx_size_symbol, true);
  allocCast3D(dl_ch_mag, c16_t, toFree3, NR_SYMBOLS_PER_SLOT, NR_MAX_NB_LAYERS, rx_size_symbol, true);
  allocCast3D(dl_ch_magb, c16_t, toFree4, NR_SYMBOLS_PER_SLOT, NR_MAX_NB_LAYERS, rx_size_symbol, true);
  allocCast3D(dl_ch_magr, c16_t, toFree5, NR_SYMBOLS_PER_SLOT, NR_MAX_NB_LAYERS, rx_size_symbol, true);
  c16_t(*rho_dl_p)[NR_MAX_NB_LAYERS * NR_MAX_NB_LAYERS][rx_size_symbol] = NULL; /* the 2-layer ML correlation buffer: ML is not part of the receiver */

  c16_t ptrs_phase_per_slot[fp->nb_antennas_rx][NR_SYMBOLS_PER_SLOT];
  memset(ptrs_phase_per_slot, 0, sizeof(ptrs_phase_per_slot));
  int32_t ptrs_re_per_slot[fp->nb_antennas_rx][NR_SYMBOLS_PER_SLOT];
  memset(ptrs_re_per_slot, 0, sizeof(ptrs_re_per_slot));

  pdtim_add(PDTIM_ALLOC, pdt_alc);

  uint32_t dl_valid_re[NR_SYMBOLS_PER_SLOT] = {0};
  int32_t log2_maxh = 0;
  pdsch_scope_req_t scope_req = {.copy_chanest_to_scope = false, .copy_rxdataF_to_scope = false,
                                 .scope_rxdataF_offset = 0};

  // Same "first symbol carrying data" rule as nr_ue_pdsch_procedures().
  const int first_symbol_with_data = nr_ssb_rm_first_data_symbol(dlsch_config, freq_alloc, &ssb);
  /* Always a mask, even an empty one: with one, nr_rx_pdsch() takes its per-symbol RE count from what
   * nr_dlsch_extract_rbs() actually packed (CRB-parity CSI-RS, DM-RS, SSB) instead of its own CSI-RS
   * re-count, which walks CRB numbers over the BWP-relative bitmap and is wrong for BWPStart > 0. */
  static const nr_ssb_rm_mask_t no_ssb = {0};

  /* ---- DATA-ORDERED GATHER (seg_path only). nr_rx_pdsch() walks its allocation in increasing PRB
   * order (nr_dlsch_extract_rbs), which is not data order for interleaved VRBs, and it rereads the
   * estimate from index 0 for every bitmap block. So it is handed a VIRTUAL contiguous allocation at
   * BWP PRBs 0..n-1 whose subcarriers are the segments' REs gathered in data order, against the
   * data-ordered estimate built above. DM-RS positions within a PRB are the same in every PRB, so
   * moving whole PRBs keeps them. The caller's rxdataF is left untouched: the data-aided submit
   * forms Y/X on the REAL subcarriers. */
  freq_alloc_bitmap_t fa_virt;
  c16_t(*rxdataF_dem)[fp->samples_per_slot_wCP] = rxdataF;
  if (seg_path && gp_ok)
    fa_virt = set_bitmap_from_start_size(0, freq_alloc->num_rbs); /* the pass demodulated it from the real REs: no gather, no CSI map (the demod below is skipped) */
  if (seg_path && !gpu_llr && !gp_ok) {
    c16_t *virt = seg_virt_prepare(fp);
    /* memcpy runs instead of the per-RE modulo loop; bit-identical */
    int nre_fast = 0;
    for (int s = 0; s < nseg; s++) nre_fast += seg[s].n_prb * NR_NB_SC_PER_RB;
    const int nre = !virt ? -1 : (nre_fast <= NR_PRB_SET_MAX * NR_NB_SC_PER_RB ? nre_fast : -1);
    if (nre != freq_alloc->num_rbs * NR_NB_SC_PER_RB) { /* not nsc_seg: the GPU goto skips its initialiser */
      out->status = NR_PDSCH_PASSIVE_DECODE_ERROR;
      return out->status;
    }
    const int N = fp->ofdm_symbol_size, off0 = fp->first_carrier_offset + dlsch_config->BWPStart * NR_NB_SC_PER_RB;
    for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++)
      for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
        const c16_t *src = &rxdataF[aarx][m * N];
        c16_t *dst = &virt[(size_t)aarx * fp->samples_per_slot_wCP + (size_t)m * N];
        nr_prb_gather_copy(src, dst, seg, nseg, NR_NB_SC_PER_RB, off0, N);
      }
    fa_virt = set_bitmap_from_start_size(0, freq_alloc->num_rbs);
    rxdataF_dem = (c16_t(*)[fp->samples_per_slot_wCP])virt;
    gp_seg_csi_map(seg, nseg, dlsch_config, freq_alloc);
  }

  const uint64_t pdt_dem = pdtim_on ? pdtim_now() : 0;
  if (gw_tim)
    gw_t_demod = nr_td_gw_now_ns();
  bool demod_ok = true;
  const int last_sym = dlsch_config->start_symbol + dlsch_config->number_symbols - 1;
  if (gpu_llr) {
    const uint32_t n = gpu_llr_n < rx_llr_buf_sz ? gpu_llr_n : rx_llr_buf_sz;
    memcpy(llr, gpu_llr, (size_t)n * sizeof(int16_t)); /* the rest stays 0: a probe only needs code block 0 */
    for (int m = dlsch_config->start_symbol; m <= last_sym; m++)
      dl_valid_re[m] = 0;
  }
  if (gw_ready) { /* the shared, immutable decoder-input LLRs of this geometry */
    memcpy(llr, gw_view.llr, (size_t)G * sizeof(int16_t));
    memcpy(dl_valid_re, gw_meta->dl_valid_re, sizeof(dl_valid_re));
  }
  memset(&nr_pdsch_evm_acc, 0, sizeof(nr_pdsch_evm_acc)); /* the SNR gate needs the per-grant EVM measurement */
  nr_pdsch_evm_acc.armed = !gpu_llr && !gw_ready;
  const int gdem = gp_ok ? 0 /* the pass demodulated */ : (gpu_llr || gw_ready) ? -1 /* GPU-DEMOD (ISAC_PDSCH_GPU_EXACT_DEMOD): 0 = demodulated */
      : nr_pdsch_gpu_demod_grant(&(nr_pdsch_gpu_demod_call_t){ue, proc, &dlsch, seg_path ? &fa_virt : freq_alloc, dlsch_config, &harq,
          first_symbol_with_data, probe_last_sym, pdsch_est_size, (int32_t *)pdsch_dl_ch_estimates, llr, rx_llr_buf_sz, dl_valid_re,
          (c16_t *)rxdataF_dem, &log2_maxh, rx_size_symbol, (c16_t *)rxdataF_comp, (c16_t *)dl_ch_mag, (c16_t *)dl_ch_magb,
          (c16_t *)dl_ch_magr, (c16_t *)ptrs_phase_per_slot, (int32_t *)ptrs_re_per_slot, nvar, &scope_req, rho_dl_p, ssb_unav ? &ssb.dem : &no_ssb});
  if (gp_ok) { /* what nr_pdsch_gpu_demod_grant leaves: the valid-RE counts, the max-h exponent, the EVM sums and the branch used */
    for (int m = dlsch_config->start_symbol; m <= last_sym && m < NR_SYMBOLS_PER_SLOT; m++)
      dl_valid_re[m] = t_gpp->dl_valid_re[m];
    log2_maxh = t_gpp->r.log2_maxh;
    nr_pdsch_gpu_demod_pass_fold(&t_gpp->demod, &dlsch);
  }
  for (int m = dlsch_config->start_symbol; m <= last_sym && !gpu_llr && !gw_ready && gdem != 0; m++) {
    if (probe_last_sym >= 0 && m > probe_last_sym && m != last_sym)
      continue; /* probe: past the horizon, LLR count stays 0 for this symbol */
    if (nr_rx_pdsch(ue, proc, &dlsch, seg_path ? &fa_virt : freq_alloc, dlsch_config, &harq, (unsigned char)m,
                    m == first_symbol_with_data, (unsigned char)dlsch_config->harq_process_nbr, pdsch_est_size,
                    pdsch_dl_ch_estimates, llr, dl_valid_re, rxdataF_dem, &log2_maxh, rx_size_symbol,
                    fp->nb_antennas_rx, rxdataF_comp, dl_ch_mag, dl_ch_magb, dl_ch_magr, ptrs_phase_per_slot,
                    ptrs_re_per_slot, nvar, &scope_req, rho_dl_p /* NULL unless 2-layer ML may run */,
                    ssb_unav ? &ssb.dem : &no_ssb)
        < 0) {
      demod_ok = false;
      break;
    }
  }

  nr_dlsch_csi_prb_map = NULL; /* CSI-FULL */
  nr_dlsch_csi_prb_map_n = 0;
  pdtim_add(PDTIM_DEMOD, pdt_dem);
  out->l2maxh = (int8_t)log2_maxh; /* MRCBUG diag (EVMGRANT l2=) */
  /* MRCBUG diag (EVMGRANT zabs=): mean |Re z|, |Im z| of the int16 equaliser output in LSB (the stream the LLRs are cut from;
   * CPU demod, 1 layer, 0 = n/a). Replay b 2026-10-13: median 143-196 (single branch) vs 73-89 (4-branch MRC), p10 32. */
  out->zabs = 0.f;
  if (demod_ok && gdem != 0 && !gpu_llr && !gw_ready && cw->Nl == 1) {
    double zabs = 0.0; uint64_t nz = 0;
    for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
      const uint32_t n = dl_valid_re[m];
      const c16_t *z = rxdataF_comp[m][0];
      for (uint32_t i = 0; i < n; i++) zabs += abs(z[i].r) + abs(z[i].i);
      nz += 2 * n;
    }
    out->zabs = nz ? (float)(zabs / nz) : 0.f;
  }
  const bool evm_deferred = nr_pdsch_gpu_demod_evm_deferred(); /* DECGPU: the GPU EVM chain overlaps the LDPC stage */
  if (!evm_deferred)
    evm_fill(out, dlsch_config, demod_ok);

  /* ---- RE-BUDGET INVARIANT (CPU path, whole allocation demodulated). G above is what rate
   * de-matching will read; the demodulator must have produced exactly that many LLRs. A mismatch --
   * an SSB/CSI-RS/PT-RS/DM-RS RE model in nr_rx_pdsch() that disagrees with the one G was computed
   * from -- shifts every LLR after the first disagreement, so the TB cannot be CRC evidence of
   * anything: fail closed instead of feeding the sweeps a misleading CRC failure. */
  if (demod_ok && !gpu_llr && !gw_ready && probe_last_sym < 0) {
    uint64_t llr_n = 0;
    for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++)
      llr_n += (uint64_t)dl_valid_re[m] * cw->qamModOrder * cw->Nl;
    if (llr_n != G) {
      static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_W(PHY, "SENSING: PDSCH RE-budget mismatch n=%lu rnti=0x%x: demodulated %lu LLRs, G %u (ssb_unav %u csi_unav %u)\n", n_, grant->rnti, (unsigned long)llr_n, G, ssb_unav, csi_unav);
      prg_arm_unsupported(grant->rnti, prg_arm);
      out->status = NR_PDSCH_PASSIVE_DECODE_ERROR;
      return out->status;
    }
  }

  /* Qm oracle: same symbol choice as EQDIAG -- the one with the most valid data REs. */
  if (demod_ok && !gw_ready) {
    int qm_m = -1;
    uint32_t qm_n = 0;
    for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++)
      if (dl_valid_re[m] > qm_n) { qm_n = dl_valid_re[m]; qm_m = m; }
    if (qm_m >= 0)
      out->qm_measured = gp_ok ? (t_gpp->r.qm_valid ? (uint8_t)nr_pdsch_qm_classify(t_gpp->qm_row, qm_n > 4096 ? 4096 : qm_n, NULL) : 0)
                               : (uint8_t)nr_pdsch_qm_classify((const int16_t *)rxdataF_comp[qm_m][0], qm_n > 4096 ? 4096 : qm_n, NULL);
  }


  if (demod_ok) {
    /* Measured BEFORE unscrambling: descrambling only flips signs, so magnitudes are identical
     * either side of it and taking them here keeps this independent of whether the scrambling
     * sequence is the suspect. */
    uint64_t llr_n = 0, llr_absum = 0, llr_zero = 0, llr_sat = 0, llr_pos = 0, llr_clip8 = 0;
    int64_t  llr_sgnsum = 0;
    uint64_t posbit[2] = {0, 0}, nbit[2] = {0, 0};
    uint64_t zero_before = 0, pdt_ldp = 0;
    if (gw_ready) { /* descramble + normalisation already in the entry: restore what they measured */
      llr_n = gw_meta->llr_n; llr_absum = gw_meta->llr_absum; llr_zero = gw_meta->llr_zero; llr_sat = gw_meta->llr_sat;
      llr_pos = gw_meta->llr_pos; llr_clip8 = gw_meta->llr_clip8; llr_sgnsum = gw_meta->llr_sgnsum;
      memcpy(posbit, gw_meta->posbit, sizeof(posbit));
      memcpy(nbit, gw_meta->nbit, sizeof(nbit));
      zero_before = atomic_load(&g_ldpc_zero_tb);
      pdt_ldp = pdtim_on ? pdtim_now() : 0;
      t_last_llr = llr; t_last_G = G;
      atomic_fetch_add(&g_rv_census[grant->mcs >= 24][cw->rv & 3], 1);
      goto gw_llr_norm; /* the K38 shift depends on this decode's C: applied below, as the owner does */
    }
    if (gp_ok) { /* the pass descrambled, normalised and decoded on the device: only the bookkeeping of the decode is left */
      zero_before = atomic_load(&g_ldpc_zero_tb);
      pdt_ldp = pdtim_on ? pdtim_now() : 0;
      if (gp_tb) {
        t_last_llr = NULL; t_last_G = 0;
      } else {
        memcpy(llr, t_gpp->llr, (size_t)G * sizeof(int16_t)); /* the retry arms re-decode these */
        t_last_llr = llr; t_last_G = G;
      }
      atomic_fetch_add(&g_rv_census[grant->mcs >= 24][cw->rv & 3], 1);
      if (nr_gpu_graph_on()) { /* v2: the device computed the loops below (k_llr_stats, bit-equal) */
        const nr_gpu_llr_stats_t *ls = &t_gpp->r.llr_stats;
        llr_n = ls->llr_n; llr_absum = ls->llr_absum; llr_zero = ls->llr_zero; llr_sat = ls->llr_sat; llr_clip8 = ls->llr_clip8;
        llr_pos = ls->llr_pos; llr_sgnsum = ls->llr_sgnsum;
        memcpy(posbit, ls->posbit, sizeof(posbit));
        memcpy(nbit, ls->nbit, sizeof(nbit));
      }
      gp_commit_decode(t_gpp, cw, G, dlsch_config->tbslbrm, gp_tb);
      goto gp_llr_done;
    }
    for (uint32_t i = 0; i < G; i += 32) {
      const int v = llr[i] < 0 ? -llr[i] : llr[i];
      llr_absum += (uint64_t)v;
      llr_n++;
      if (v == 0) {
        llr_zero++;
      }
      if (v >= 32767) {
        llr_sat++;
      }
      if (v > 127) {
        llr_clip8++;   // the rail the LDPC decoder actually imposes
      }
    }

    zero_before = atomic_load(&g_ldpc_zero_tb);
    pdt_ldp = pdtim_on ? pdtim_now() : 0;
    /* NEON sign-mask unscramble, bit-identical to nr_codeword_unscrambling */
    nrhf_unscramble(llr, G, gold_cache((grant->rnti << 15) + (0 << 14) + dlsch_config->dlDataScramblingId, (G + 31) / 32));
    t_last_llr = llr; t_last_G = G; /* for the GPU self-check */

    /* SIGN statistics AFTER descrambling -- measuring them before is meaningless, and that was the
     * first version's mistake: the scrambler exists to randomise signs, so an all-zero transport
     * block scrambled gives balanced +/- by construction (measured: pos = 49.5 % / 49.0 %, which
     * proves only that the scrambler works). Post-descramble, a genuinely all-zero TB shows a strong
     * POSITIVE bias (OAI's convention: LLR > 0 favours bit 0), and a TB whose descrambling is wrong
     * stays balanced. That is the discriminator for the zero_tb population. */
    /* Step 4, not 32: the stride must not alias the 4-LLRs-per-symbol structure, or every sample
     * lands on the same bit position and the split is meaningless. Sample whole symbols instead. */
    for (uint32_t i = 0; i + 3 < G; i += 32) {
      for (uint32_t j = 0; j < 4; j++) {
        const int raw = llr[i + j];
        const int b   = (j < 2) ? 0 : 1;
        nbit[b]++;
        if (raw > 0) {
          posbit[b]++;
        }
      }
      llr_sgnsum += (int64_t)llr[i];
      if (llr[i] > 0) {
        llr_pos++;
      }
    }
    atomic_fetch_add(&g_rv_census[grant->mcs >= 24][cw->rv & 3], 1);
    if (t_gw_owner) { /* this geometry's LLRs, descrambled, NOT normalised (K38: the shift depends on each hypothesis's C) */
      if (gw_tim)
        nr_td_gw_tim_add(NR_TD_GWTIM_DEMOD, nr_td_gw_now_ns() - gw_t_demod);
      const gw_ctx_t *gc = (const gw_ctx_t *)nr_td_grantwork_ctx(t_gw);
      gw_meta_t m = {.nvar = nvar, .qm_measured = out->qm_measured,
                     .llr_n = llr_n, .llr_absum = llr_absum, .llr_zero = llr_zero, .llr_sat = llr_sat,
                     .llr_clip8 = llr_clip8, .llr_pos = llr_pos, .llr_sgnsum = llr_sgnsum, .fp = gc->fp};
      memcpy(m.dl_valid_re, dl_valid_re, sizeof(m.dl_valid_re));
      memcpy(m.posbit, posbit, sizeof(m.posbit));
      memcpy(m.nbit, nbit, sizeof(m.nbit));
      t_gw_owner = false;
      if (nr_td_grantwork_publish(t_gw, t_gw_sig, llr, G, cw->Nl, cw->qamModOrder, &m, sizeof(m)) != NR_TD_GW_OK)
        nr_td_grantwork_abandon(t_gw, t_gw_sig, NR_PDSCH_PASSIVE_DECODE_ERROR);
      if (t_gw_capture) { /* the lazy computation stops here: no LDPC, no feedback */
        out->status = NR_PDSCH_PASSIVE_DECODE_CRC_FAIL;
        return out->status;
      }
    }
  gw_llr_norm:;
    /* ---- LLR SCALE NORMALISATION before the int8 decoder ----------------------------------------
     * The decoder saturates every LLR to +-127 (simde_mm_packs_epi16). The demodulators' output
     * scale is NOT controlled: the fixed-point 4-layer MMSE scales by det(G) and gave mean |LLR|
     * 454 with 87 % clipped on the rank-4 bed (417 / 22 % OTA), the float path 79 / 20 %, a rank-2
     * decode 125 / 0.015 %. Min-sum is scale-invariant except for that clipping, so a uniform
     * right shift that brings the mean under LLR_NORM_TARGET costs nothing where the scale was
     * already right and keeps the soft information where it was not. */
    {
      if (G >= 64) {
        /* K38: the mean is taken over code block 0's span, ceil(G/C) LLRs, not all G: a layout probe
         * leaves every LLR past its horizon at 0, and a whole-buffer mean diluted by those zeros gave
         * the probe a smaller shift than the whole-slot decode of the same hypothesis (rank 4: CB0
         * LLRs 2^dk times larger, clipped at the int8 rail). Every decode has demodulated that span. */
        const uint32_t span = nr_llr_norm_span(G, nr_llr_norm_num_cb(cw->TBS, cw->ldpcBaseGraph));
        const int k = nr_llr_norm_shift_nl(llr, span, (int)cw->Nl, (int)cw->qamModOrder);
        {
          static int r2w_diag = -1; /* NR_TEST_RANK2W_DIAG=1: per-layer |LLR| statistics of 2-layer grants (test seam, replay only) */
          if (r2w_diag < 0) r2w_diag = getenv("NR_TEST_RANK2W_DIAG") ? 1 : 0;
          if (r2w_diag && cw->Nl == 2 && cw->qamModOrder >= 2)
            nr_llr_layer_diag(llr, G, cw->qamModOrder, k, grant->rnti, proc->frame_rx, proc->nr_slot_rx, grant->mcs);
        }
        if (k > 0) {
          for (uint32_t i = 0; i < G; i++) llr[i] = (int16_t)(llr[i] >> k);
          atomic_fetch_add(&g_llr_norm_shift[k], 1);
        } else {
          atomic_fetch_add(&g_llr_norm_shift[0], 1);
        }
      }
    }
    if (atomic_load(&g_ldpc_ok) >= 100 && !t_quiet) { /* only once the layout has bootstrapped */
      t_hq.armed = 1;
      t_hq.rnti = grant->rnti;
      t_hq.pid = grant->harq_pid;
      t_hq.ndi = grant->ndi;
      t_hq.slot = grant->source_absolute_slot;
    }
  gp_llr_done:;
    if (gp_tried && !gp_ok)
      nr_gpu_pass_note(NR_GPU_PASS_EV_OLD_FIRST);
    bool ldpc_ok = gp_ok ? gp_tb : passive_ldpc_decode(ue, proc, &g_harq, cw, dlsch_config, llr, freq_alloc->num_rbs, G);
    if (gp_fcmp && !gp_ok) { /* shadow of a CRC-failing pass: the old first body's verdicts and its post-normalisation LLRs (what the retry arms get) */
      const bool f32 = nr_gpu_pass_fp32_on();
      if (f32) /* verdict flips are rate-gated, the signs are not tolerated */
        (void)(gp_sfo_same() && nr_gpu_pass_fp32_shadow(ldpc_ok || t_seg_ok_last != t_gp_fok, gp_sign_bad(t_gpp->llr, llr, G)));
      else if (gp_sfo_same() && (ldpc_ok || t_seg_ok_last != t_gp_fok || gp_llr_hash(llr, G) != t_gp_fhash)) {
        nr_gpu_pass_note(NR_GPU_PASS_EV_SHADOW_FAIL_BAD);
        LOG_E(PHY, "SENSING: GPUPASS MISMATCH (CRC-fail shadow) rnti 0x%x nl %u mcs %u rb %d+%d tbs %u: pass blocks ok %u, old %u (old crc %d), llr hash %s (probe %d horizon %d share %d G %u)\n", grant->rnti, cw->Nl,
              grant->mcs, freq_alloc->first_rb, freq_alloc->num_rbs, cw->TBS, t_gp_fok, t_seg_ok_last, (int)ldpc_ok, gp_llr_hash(llr, G) == t_gp_fhash ? "equal" : "DIFFERENT",
              (int)t_probe_first_seg, probe_last_sym, (int)t_share.on, G);
        nr_gpu_pass_disable("pass and old first body disagree on a CRC failure");
      }
    }
    t_gp_sbad = gp_cmp && !gp_ok && nr_gpu_pass_fp32_on() && gp_sign_bad(t_gpp->llr, llr, G); /* FP32: the CRC-OK shadow compares the LLR signs too */
    t_hq.armed = 0;
    /* A probe's outcome is code block 0's CRC, and every hypothesis sweep below (PT-RS density,
     * the PDSCH config feedback in the queue, the layout search) must see it: with ldpc_ok false
     * for every probe the PT-RS sweep could never latch "absent" and only 1 arm in 7 was right --
     * exactly the 57/400 measured with ISAC_PROBE_ALL on a pinned conf that decodes 97 %. The TB
     * itself is still never reported decoded (the queue maps a probe onto CRC status only). */
    const bool probe_ok = t_probe_first_seg && t_probe_seg_ok;
    if (probe_ok)
      ldpc_ok = true;
    /* LBRM layer-term hypotheses: only when the hypothesis would change the bit selection (E beyond
     * the smaller N_ref), never on probes; a pass latches n_L for this RNTI. Cost: one extra LDPC
     * pass per failed long TB until latched. */
    if (!ldpc_ok && t_seg_C > 0) { /* probes too: the phone's n_L is unknown and LBRM binds on code block 0 as well */
      const int nl_now = rnti_nl_get(grant->rnti, sweepable);
      const uint32_t E_first = t_seg_E;
      const uint32_t tbs_now = dlsch_config->tbslbrm;
      const uint16_t bw_lbrm = grant->bw_tbslbrm > 0 ? grant->bw_tbslbrm : dlsch_config->BWPSize;
      const uint8_t tbl_lbrm = grant->mcs_table_lbrm >= 0 ? (uint8_t)grant->mcs_table_lbrm : grant->mcs_table;
      static const int alts[3] = {4, 2, 1};
      for (int a = 0; a < 3 && !ldpc_ok; a++) {
        const int nl_h = alts[a];
        if (nl_h == nl_now)
          continue;
        const uint32_t lbrm_h = nr_compute_tbslbrm(tbl_lbrm, bw_lbrm, (uint8_t)nl_h);
        const uint32_t nref_h = 3u * lbrm_h / (2u * t_seg_C);
        const uint32_t nref_now = 3u * tbs_now / (2u * t_seg_C);
        const uint32_t N = (t_seg_BG == 1 ? 66u : 50u) * t_seg_Z;
        /* the two hypotheses select the same bits unless E reaches past the smaller N_cb */
        if (E_first <= (nref_h < nref_now ? nref_h : nref_now) || (nref_h >= N && nref_now >= N))
          continue;
        dlsch_config->tbslbrm = lbrm_h;
        atomic_fetch_add(&g_lbrm_try[nl_h], 1);
        const bool full_ok = passive_ldpc_decode(ue, proc, &g_harq, cw, dlsch_config, llr, freq_alloc->num_rbs, G);
        if (full_ok || (t_probe_first_seg && t_probe_seg_ok)) {
          ldpc_ok = true;
          atomic_fetch_add(&g_lbrm_ok[nl_h], 1);
          if (!t_quiet && rnti_nl_latch(grant->rnti, sweepable, nl_h) != nl_h)
            LOG_A(PHY, "SENSING: LBRM layer term n_L=%d latched from the TB CRC for rnti 0x%04x (was %d): TBS_LBRM=%u C=%u E=%u N_ref=%u\n",
                  nl_h, grant->rnti, nl_now, lbrm_h, t_seg_C, E_first, nref_h);
        } else {
          dlsch_config->tbslbrm = tbs_now;
        }
      }
    }
    pdtim_add(PDTIM_LDPC, pdt_ldp);
    if (evm_deferred && nr_pdsch_gpu_demod_evm_finish(1) == 0)
      evm_fill(out, dlsch_config, demod_ok);

    /* DCI 1_1 interleaved VRB-to-PRB bundle-size sweep (grant->vrb_l == 0 for a non-interleaved or
     * DCI 1_0 grant -- nothing to feed back, see the field comment). */
    if ((grant->vrb_l == 2 || grant->vrb_l == 4) && !t_quiet) {
      const int latched = rnti_vrbl_feed(grant->rnti, grant->vrb_l == 4 ? 1 : 0, ldpc_ok);
      if (latched >= 0)
        LOG_A(PHY, "SENSING: VRB_IL rnti=0x%x L=%u latched\n", grant->rnti, latched == 0 ? 2 : 4);
    }
    /* PRB-bundling (PRG) hypothesis feedback (prg_arm < 0 when nothing was swept for this decode --
     * the caller had already set an explicit freq_alloc->prg, or a GPU-assisted decode -- see the
     * pick site above). */
    if (prg_arm >= 0 && !t_quiet) {
      bool explore_started = false;
      const int latched = rnti_prg_feed(grant->rnti, prg_arm, ldpc_ok, nr_scr_link_healthy(&g_dl_scr_link, grant->rnti),
                                        &explore_started);
      if (explore_started)
        LOG_A(PHY, "SENSING: PRG rnti=0x%x wideband CRC <= %.0f%% over %d trials with the link healthy: exploring prg=2/4\n",
              grant->rnti, 100.0 * NR_ARM_SWEEP_INCUMBENT_POOR_RATE, NR_ARM_SWEEP_INCUMBENT_MIN_TRIALS);
      if (latched >= 0)
        LOG_A(PHY, "SENSING: PRG rnti=0x%x prg=%u latched\n", grant->rnti, nr_prg_arm_value(latched));
    }
    {
      /* 1 = decoded with data, 0 = zero_tb, 2 = seg_fail. `out->status` is not set yet here, so the
       * zero/seg distinction comes from the counters passive_ldpc_decode just bumped. */
      const int sk = ldpc_ok ? 1 : ((atomic_load(&g_ldpc_zero_tb) != zero_before) ? 0 : 2);
      t_last_sk = sk;
      {
        /* llr_have is one past the last NON-ZERO LLR, not the produced length:
         * quantization and erasures can legitimately make the tail zero. Compare
         * G with the sum of dl_valid_re * Qm * Nl below to check the RE budget. */
        uint32_t llr_have = 0;
        if (gp_ok && gp_tb)
          llr_have = G; /* the LLRs are on the device */
        else
          for (int i = (int)G - 1; i >= 0; i--) {
            if (llr[i] != 0) { llr_have = (uint32_t)i + 1; break; }
          }
        t_last_llr_have = llr_have;
        uint32_t vre = 0;
        for (int m = dlsch_config->start_symbol;
             m < dlsch_config->start_symbol + dlsch_config->number_symbols && m < NR_SYMBOLS_PER_SLOT; m++) {
          vre += dl_valid_re[m];
        }
        const int pk = (sk == 1) ? 1 : 0;  // 1 = decoded, 0 = did not decode (zero_tb or seg_fail)
        t_last_data_bits = vre * cw->qamModOrder * cw->Nl;
        const uint64_t v[PIPE_N_FIELDS] = {
            G, llr_have, vre, (uint64_t)vre * cw->qamModOrder * cw->Nl,
            t_seg_C, t_seg_K, t_seg_Z, t_seg_F, t_seg_E, t_seg_R, t_seg_lbrm,
            cw->qamModOrder, cw->Nl, (uint64_t)dlsch_config->number_symbols};
        atomic_fetch_add(&g_pipe_n[pk], 1);
        for (int f = 0; f < PIPE_N_FIELDS; f++) {
          atomic_fetch_add(&g_pipe_sum[pk][f], v[f]);
        }
      }
      atomic_fetch_add(&g_shape_n[sk], 1);
      atomic_fetch_add(&g_shape_tbs[sk], (uint64_t)cw->TBS);
      atomic_fetch_add(&g_shape_rb[sk], (uint64_t)freq_alloc->num_rbs);
      {
        int rb_bin = (int)freq_alloc->num_rbs * NR_PDSCH_RBHIST_BINS / 273;
        if (rb_bin < 0) rb_bin = 0;
        if (rb_bin >= NR_PDSCH_RBHIST_BINS) rb_bin = NR_PDSCH_RBHIST_BINS - 1;
        atomic_fetch_add(&g_rbhist[sk][rb_bin], 1);
      }
      atomic_fetch_add(&g_mcshist[freq_alloc->num_rbs >= 128 ? 1 : 0][sk][grant->mcs & 31], 1);
      {
        const int rb0 = freq_alloc->first_rb, nrb = freq_alloc->num_rbs;
        for (int rb = rb0; rb < (seg_path ? freq_alloc->last_rb + 1 : rb0 + nrb) && rb < NR_RBMAP_MAX; rb++) {
          if (rb < 0) continue;
          if (seg_path && !check_rb_in_bitmap(freq_alloc, rb)) continue;
          atomic_fetch_add(&g_rbmap[rb], 1);
          if (sk == 1) atomic_fetch_add(&g_rbmap_ok[rb], 1);
        }
        atomic_fetch_add(&g_rbmap_grants, 1);
      }
      if (freq_alloc->num_rbs >= 128) {
        atomic_fetch_add(&g_mcs_tbs[grant->mcs & 31], (uint64_t)cw->TBS);
        atomic_fetch_add(&g_mcs_rb[grant->mcs & 31], (uint64_t)freq_alloc->num_rbs);
        atomic_fetch_add(&g_mcs_segs[grant->mcs & 31], (uint64_t)t_seg_C);
        atomic_fetch_add(&g_mcs_segs_ok[grant->mcs & 31], (uint64_t)t_seg_ok_last);
      }
      atomic_fetch_add(&g_shape_G[sk], (uint64_t)G);
      atomic_fetch_add(&g_shape_rv[sk], (uint64_t)grant->rv);
      atomic_fetch_add(&g_shape_K[sk], (uint64_t)t_seg_K);
      atomic_fetch_add(&g_shape_F[sk], (uint64_t)t_seg_F);
      atomic_fetch_add(&g_shape_C[sk], (uint64_t)t_seg_C);
      atomic_fetch_add(&g_shape_Z[sk], (uint64_t)t_seg_Z);
      atomic_fetch_add(&g_nsym[sk][(dlsch_config->number_symbols <= 9) ? 0 : 1], 1);
    }
    if (!gp_ok || nr_gpu_graph_on()) { /* a v1 pass-taken grant has no host LLR statistics: not counted, so the means are not diluted with zeros; v2 has them */
      const int k = ldpc_ok ? 1 : 0;
      atomic_fetch_add(&g_llr_n[k], llr_n);
      atomic_fetch_add(&g_llr_absum[k], llr_absum);
      atomic_fetch_add(&g_llr_zero[k], llr_zero);
      atomic_fetch_add(&g_llr_sat[k], llr_sat);
      atomic_fetch_add(&g_llr_clip8[k], llr_clip8);
      atomic_fetch_add(&g_llr_sgnsum[k], llr_sgnsum);
      atomic_fetch_add(&g_llr_pos[k], llr_pos);
      for (int b = 0; b < 2; b++) {
        atomic_fetch_add(&g_llr_posbit[k][b], posbit[b]);
        atomic_fetch_add(&g_llr_nbit[k][b], nbit[b]);
      }
      atomic_fetch_add(&g_llr_tb[k], 1);
    }
    if (ldpc_ok) {
      if (!t_probe_first_seg && !t_quiet && grant->dci11 && rnti_sweepable(grant->rnti, grant->rnti_class))
        mcs_table_evidence_feed(grant->mcs, grant->mcs_table);
      out->status = NR_PDSCH_PASSIVE_DECODE_CRC_OK;
      out->tb     = g_harq.b;
      // Reserved-MCS retransmission record (G5 review, gap-harq): only a CRC-VERIFIED TB is strong
      // enough evidence to seed/refresh this (rnti, pid)'s record -- see the computation site's
      // comment for why recording on mere "the MCS was resolvable" was not. SI-/RA-/P-RNTI grants
      // never carry a real NDI/HARQ-pid field and can never reach the reserved-MCS lookup at all
      // (format 1_0 keeps a hard reject for them), so recording them would only churn the table
      // with unusable entries. A grant that itself USED a stored record (have_init_tx) is a
      // retransmission, not a fresh resolvable MCS, so it does not refresh the record either --
      // only the genuinely resolvable grant that established have_init_tx=false does.
      if (!have_init_tx && !t_quiet && rnti_sweepable(grant->rnti, grant->rnti_class)) {
        pthread_mutex_lock(&g_harqc_lock);
        if (nr_cfg_epoch_work_current()) nr_harq_init_tx_record(&g_dl_harq_init, grant->rnti, grant->harq_pid, grant->ndi, cw->qamModOrder, cw->Nl,
                               cw->ldpcBaseGraph, cw->TBS, cw->targetCodeRate);
        pthread_mutex_unlock(&g_harqc_lock);
      }
    } else {
      out->status = NR_PDSCH_PASSIVE_DECODE_CRC_FAIL;
    }
  } else {
    out->status = NR_PDSCH_PASSIVE_DECODE_ERROR;
  }

  out->ldpc_it_max = t_ldpc_it_max;
  out->ldpc_it_cap = t_ldpc_it_cap;
  out->ldpc_it_sum = t_ldpc_it_sum;
  /* GrantWork admissibility (fix round 1, I2): what the job's full decode did that CB0 from shared work does not. */
  if (t_gw != NULL && !t_quiet && !t_gw_capture) {
    uint32_t f = 0;
    if (t_last_hq_retx || have_init_tx)
      f |= NR_TD_GW_F_HARQ;
    if (prg_arm > 0)
      f |= NR_TD_GW_F_ARM;
    nr_td_grantwork_flag(t_gw, f);
    gw_ctx_t *gc = (gw_ctx_t *)nr_td_grantwork_ctx(t_gw); /* the gw's own copy; written only on the job thread */
    if (gc != NULL && t_decoder_used_last)
      gc->main_decoder_used = t_decoder_used_last;
  }
  t_last_sk = -1;

  pdtim_report();

  /* Nothing is freed here any more: every buffer above persists for the life of this thread and is
   * reused by the next grant. See the note at the chest allocation for the measurement. */
  { static _Atomic unsigned long c_ = 0;
    const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
    if (n_ == 1 || (n_ % 200) == 0)
      LOG_A(PHY, "SENSING: PDSCH UNSUP@3234 n=%lu\n", n_); }
  if (gp_cmp && gp_sfo_same()) { /* shadow check: the old path decoded a grant the pass took; the verdict and the bytes must agree */
    const bool old_ok = out->status == NR_PDSCH_PASSIVE_DECODE_CRC_OK;
    if (nr_gpu_pass_fp32_on()) { /* FP32: a pass-OK / old-no-CRC flip is rate-gated; bytes of two CRC-OK decodes must still agree */
      if (nr_gpu_pass_fp32_shadow(!old_ok, t_gp_sbad) == 0 && old_ok && memcmp(out->tb, t_gp_shadow, cw->TBS / 8) != 0) {
        nr_gpu_pass_note(NR_GPU_PASS_EV_SHADOW_BAD);
        nr_gpu_pass_disable("pass and old path disagree (FP32, CRC ok but other bytes)");
      }
    } else if (!old_ok || memcmp(out->tb, t_gp_shadow, cw->TBS / 8) != 0) {
      nr_gpu_pass_note(NR_GPU_PASS_EV_SHADOW_BAD);
      LOG_E(PHY, "SENSING: GPUPASS MISMATCH rnti 0x%x nl %u mcs %u rb %d+%d tbs %u: pass CRC ok, old path %s\n", grant->rnti, cw->Nl, grant->mcs,
            freq_alloc->first_rb, freq_alloc->num_rbs, cw->TBS, old_ok ? "CRC ok but other bytes" : "no CRC ok");
      nr_gpu_pass_disable("pass and old path disagree");
    }
  }
  return out->status;
}

static nr_pdsch_passive_decode_status_t passive_decode_retry(PHY_VARS_NR_UE *ue,
                                                             const UE_nr_rxtx_proc_t *proc,
                                                             fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                                             const freq_alloc_bitmap_t *freq_alloc,
                                                             const nr_pdsch_passive_grant_t *grant,
                                                             c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                                             nr_pdsch_passive_decode_result_t *out);

/* nr_pdsch_passive_decode: entry point: decodes one dedicated PDSCH grant of the passive receiver and fills *out; returns the decode status.
 *   Path: passive_decode_retry() -> passive_decode_body().
 *   Callers: nr_pdsch_passive_queue.c (consumer threads, probe re-run), nr_pdcch_blind_monitor_rt.c (in-line decode), nr_passive_replay_capture.c.
 */
static nr_pdsch_passive_decode_status_t passive_decode_entry(PHY_VARS_NR_UE *ue,
                                                             const UE_nr_rxtx_proc_t *proc,
                                                             fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                                             const freq_alloc_bitmap_t *freq_alloc,
                                                             const nr_pdsch_passive_grant_t *grant,
                                                             c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                                             nr_pdsch_passive_decode_result_t *out);

nr_pdsch_passive_decode_status_t nr_pdsch_passive_decode(PHY_VARS_NR_UE *ue,
                                                         const UE_nr_rxtx_proc_t *proc,
                                                         fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                                         const freq_alloc_bitmap_t *freq_alloc,
                                                         const nr_pdsch_passive_grant_t *grant,
                                                         c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                                         nr_pdsch_passive_decode_result_t *out)
{
  const int gp_hold = nr_gpu_graph_call_begin(); /* v2 Task 4 (ISAC_PDSCH_GPU_GRAPH_RETRY): the call's bodies share one pass instance */
  const nr_pdsch_passive_decode_status_t st = passive_decode_entry(ue, proc, dlsch_config, freq_alloc, grant, rxdataF, out);
  if (t_recov_mode == 2 && st == NR_PDSCH_PASSIVE_DECODE_CRC_OK)
    harqc_rescued(grant->rnti, (uint8_t)grant->harq_pid, (uint8_t)grant->ndi, grant->source_absolute_slot);
  if (gp_hold) nr_gpu_graph_call_end();
  return st;
}

static nr_pdsch_passive_decode_status_t passive_decode_entry(PHY_VARS_NR_UE *ue,
                                                             const UE_nr_rxtx_proc_t *proc,
                                                             fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                                             const freq_alloc_bitmap_t *freq_alloc,
                                                             const nr_pdsch_passive_grant_t *grant,
                                                             c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                                             nr_pdsch_passive_decode_result_t *out)
{
  t_pdtim_body = 0; /* ISAC_PDTIM_CLASS: the next body is this call's first (real), later ones are retries */
  t_dec_body_ns = t_dec_stage_ns = t_dec_first_ns = 0;
  t_dec_t0_ns = dec_now_ns();
  /* CSI-FULL hook (ISAC_CSIRS_FULL=1): this grant's own cell CSI-RS set for its PDSCH slot (nr_csirs_blind_rt.c) */
  if (!t_probe_first_seg && !t_quiet && grant->dci11 && grant->rnti_class == 0) /* ISAC_CSIRS_FULL_AB diagnostic */
    nr_csirs_full_ab(ue, proc, dlsch_config, freq_alloc, grant, rxdataF);
  if (!t_probe_first_seg && !t_quiet && grant->dci11 && grant->rnti_class == 0) {
    t_quiet = true; /* the trial decodes must not teach the shared per-RNTI / layout state (ISAC_CSIRS_FULL_VALID) */
    nr_csirs_full_validate(ue, proc, dlsch_config, freq_alloc, grant, rxdataF);
    t_quiet = false;
  }
  nr_csirs_full_apply(dlsch_config, (uint32_t)(proc->frame_rx * ue->frame_parms.slots_per_frame + proc->nr_slot_rx));
  return passive_decode_retry(ue, proc, dlsch_config, freq_alloc, grant, rxdataF, out);
}

static __thread uint8_t t_pdcch_rm_crb[275]; /* WIDE-S0: per-CRB exclusion handed to nr_ssb_rm_extra by pdcch_rm_arm() */
/* rm_disarm: clears the WIDE-S0 exclusion (nr_ssb_rm_extra). */
static void rm_disarm(void) { nr_ssb_rm_extra = (nr_ssb_rm_extra_t){0, NULL}; }


/* pdcch_rm_arm: if the grant carries its scheduling PDCCH and the PDSCH allocation [start_symbol, +number_symbols) overlaps the PDCCH's CORESET
 *   symbols on at least one of the PDCCH's common RBs, loads that exclusion into nr_ssb_rm_extra (the SSB rate-matching plan's per-CRB/per-symbol
 *   mask's second layer, nr_ssb_rm_extra) and returns true. The PRB set compared is the allocation's own (contiguous or segmented). */
static bool pdcch_rm_arm(const fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg, const freq_alloc_bitmap_t *fa, const nr_pdsch_passive_grant_t *grant)
{
  if (!grant->pdcch_nsym || nr_ssb_rm_extra.crb)
    return false;
  uint16_t symbols = 0;
  for (int l = grant->pdcch_sym0; l < grant->pdcch_sym0 + grant->pdcch_nsym && l < 14; l++)
    if (l >= cfg->start_symbol && l < cfg->start_symbol + cfg->number_symbols)
      symbols |= (uint16_t)(1u << l);
  if (!symbols)
    return false;
  bool hit = false;
  for (int rb = 0; rb < cfg->BWPSize && rb < 275 && !hit; rb++) {
    const int crb = cfg->BWPStart + rb;
    hit = ((fa->bitmap[rb / 32] >> (rb % 32)) & 1) && crb < 275 && ((grant->pdcch_crb[crb >> 3] >> (crb & 7)) & 1);
  }
  if (!hit)
    return false;
  for (int c = 0; c < 275; c++)
    t_pdcch_rm_crb[c] = (grant->pdcch_crb[c >> 3] >> (c & 7)) & 1;
  nr_ssb_rm_extra = (nr_ssb_rm_extra_t){symbols, t_pdcch_rm_crb};
  return true;
}

/* passive_decode_body: passive_decode_body_core() with the scheduling PDCCH's REs excluded (pdcch_rm_arm()).
 *   Flag unset or the grant without a PDCCH overlap: exactly passive_decode_body_core(). */
static nr_pdsch_passive_decode_status_t passive_decode_body_gated(PHY_VARS_NR_UE *ue,
                                                            const UE_nr_rxtx_proc_t *proc,
                                                            fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                                            const freq_alloc_bitmap_t *freq_alloc,
                                                            const nr_pdsch_passive_grant_t *grant,
                                                            c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                                            nr_pdsch_passive_decode_result_t *out)
{
  t_pdtim_body++;
  if (!pdcch_rm_arm(dlsch_config, freq_alloc, grant))
    return passive_decode_body_core(ue, proc, dlsch_config, freq_alloc, grant, rxdataF, out);
  const nr_pdsch_passive_decode_status_t st = passive_decode_body_core(ue, proc, dlsch_config, freq_alloc, grant, rxdataF, out);
  rm_disarm();
  return st;
}

/* ISAC_E2E_TRACE stage split (EVMGRANT stg=): the first body's stage wall / thread-CPU (PDTIM timers; need ISAC_PDCCH_TIMING=1 ISAC_PDTIM_PCT=1,
 * else 0) and the wait buckets of PHY/TOOLS/nr_stage_split.h (this process: prep, wq; libpdsch_gpu.so: gpu waits, lk), in us */
static __thread struct { uint32_t gw[4], lk, prep, wq, cb; int core; uint32_t w[4], c[4]; } t_stg;
extern uint64_t *nr_ss_gpu_ptr(void);
static void stg_snapshot(void)
{
  const uint64_t *gp = nr_ss_gpu_ptr();
  if (gp == nr_ss_ns)
    gp = NULL; /* libpdsch_gpu.so bound the weak accumulators to this executable's copy: one set, count it once */
  static const int map[4] = {PDTIM_FEP, PDTIM_CHEST, PDTIM_DEMOD, PDTIM_LDPC};
  for (int k = 0; k < 4; k++) {
    t_stg.w[k] = t_pdtim_stage_us[map[k]];
    t_stg.c[k] = t_pdtim_stage_cpu_us[map[k]];
    t_stg.gw[k] = (uint32_t)(((gp ? gp[k] : 0) + nr_ss_ns[k]) / 1000);
  }
  t_stg.lk = (uint32_t)(((gp ? gp[NRSS_LK] : 0) + nr_ss_ns[NRSS_LK]) / 1000);
  t_stg.prep = (uint32_t)(nr_ss_ns[NRSS_PREP] / 1000);
  t_stg.wq = (uint32_t)(nr_ss_ns[NRSS_WQ] / 1000);
  t_stg.cb = t_seg_C;
  t_stg.core = sched_getcpu();
}
/* nr_pdsch_passive_decode_stg: " stg=fep:w/c/g,chest:w/c/g,demod:w/c/g,ldpc:w/c/g,prep:p,wq:q cb=C core=X lk=L" of the last call's first body (us) */
int nr_pdsch_passive_decode_stg(char *buf, size_t n)
{
  return snprintf(buf, n, " stg=fep:%u/%u/%u,chest:%u/%u/%u,demod:%u/%u/%u,ldpc:%u/%u/%u,prep:%u,wq:%u cb=%u core=%d lk=%u", t_stg.w[0], t_stg.c[0],
                  t_stg.gw[0], t_stg.w[1], t_stg.c[1], t_stg.gw[1], t_stg.w[2], t_stg.c[2], t_stg.gw[2], t_stg.w[3], t_stg.c[3], t_stg.gw[3],
                  t_stg.prep, t_stg.wq, t_stg.cb, t_stg.core, t_stg.lk);
}

/* BATCHCPU: route (CPU vs GPU chain) and gate one decode body; see nr_pdsch_cpu_route.h. Flags off: a plain call. */
static nr_pdsch_passive_decode_status_t passive_decode_body(PHY_VARS_NR_UE *ue,
                                                            const UE_nr_rxtx_proc_t *proc,
                                                            fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                                            const freq_alloc_bitmap_t *freq_alloc,
                                                            const nr_pdsch_passive_grant_t *grant,
                                                            c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                                            nr_pdsch_passive_decode_result_t *out)
{
  struct timespec tb;
  if (g_pdtim_on > 0)
    clock_gettime(CLOCK_MONOTONIC, &tb);
  const uint64_t tb0 = g_pdtim_on > 0 ? (uint64_t)tb.tv_sec * 1000000000ull + (uint64_t)tb.tv_nsec : 0;
  nr_pdsch_route_t rt = nr_pdsch_route_begin((int)freq_alloc->num_rbs, __builtin_popcount(dlsch_config->dmrs_ports), t_probe_first_seg);
  const bool stg_first = nr_ss_on && t_pdtim_body == 0; /* t_pdtim_body++ happens inside the gated body */
  if (stg_first) { /* ISAC_E2E_TRACE stage split: the first body starts from zero (the WQ wait of the pre-step was set at dequeue) */
    uint64_t *gp = nr_ss_gpu_ptr();
    for (int k = 0; k < NRSS_WQ; k++) {
      nr_ss_ns[k] = 0;
      if (gp) gp[k] = 0;
    }
  }
  const nr_pdsch_passive_decode_status_t st = passive_decode_body_gated(ue, proc, dlsch_config, freq_alloc, grant, rxdataF, out);
  nr_pdsch_route_end(&rt);
  if (stg_first)
    stg_snapshot();
  if (tb0) {
    clock_gettime(CLOCK_MONOTONIC, &tb);
    t_dec_body_ns += (uint64_t)tb.tv_sec * 1000000000ull + (uint64_t)tb.tv_nsec - tb0;
    if (!t_dec_first_ns)
      t_dec_first_ns = t_dec_body_ns;
  }
  return st;
}

/* passive_decode_retry: runs passive_decode_body() and, on a CRC failure, tries the recovery arms: TDRA start symbol, CSI-RS rate matching, DM-RS CDM groups, wide MRC.
 *   A recovered decode sets out->config_recovered. Each trial re-demodulates the same samples with quiet mode (t_quiet). Caller: passive_decode_entry().
 */
/* RETRY_YIELD (P99) for the DECODE-FIX recovery chain, mode 2 (the only one that runs):
 *  1: the S=0/1 toggle (arm 2, ~95 % of the Vodafone on-air rescues) runs first, and every later arm is skipped as soon as a
 *     full decode (non-probe job) waits in the PDSCH queue: the long-shot arms of one failed grant no longer delay the next
 *     grants into the stale window.
 *  2: 1 + adaptive arm gating (nr_pdsch_retry_gate.h), the same rule for every arm and cell: after 256 tries an arm (index
 *     arm + 32 * CSI variant) that rescued fewer than 1 in 50 of them runs on 1 in 16 chains only (on-air TIM: 12 rescues
 *     in 12097 chains; Vodafone: arm 2 rescues ~28 %, the other arms ~1 %).
 * When no arm body ran (all gated, or a yield before the first one) the restore decode is skipped: *out and the
 * thread-local decode view are still those of the plain body. Counters in the P99AGE line. */
#define RY_ARMS 128
#include "nr_pdsch_retry_gate.h"
/* weak: test binaries that link this file without the consumer pool (nr_pdsch_passive_queue.c) must not pull it in */
int nr_pdsch_passive_queue_backlog_real(void) __attribute__((weak));
static int ry_backlog(void) { return nr_pdsch_passive_queue_backlog_real ? nr_pdsch_passive_queue_backlog_real() : 0; }
static _Atomic uint64_t g_ry_chains, g_ry_yields, g_ry_gated, g_ry_try[RY_ARMS], g_ry_ok[RY_ARMS], g_ry_seen[RY_ARMS];
/* FATTAILS: the arms after the S toggle are skipped once this decode call has run RETRY_BUDGET_NS (counted as yields): a failed grant's long-shot arms
 * no longer hold its consumer for several bodies. */
static uint64_t dec_now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
/* TAIL-FIX: the recovery worker runs an arm only while no first-pass decode waits (DL ring or UL real queue, nr_pdsch_passive_first_pass_waiting); checked before every
 * arm (nr_recov_yield.h). A busy lane waits at most RY_IDLE_WAIT_NS and then gives the remaining arms up. Weak: test binaries do not link the queues. */
bool nr_pdsch_passive_first_pass_waiting(void) __attribute__((weak));
#define RY_IDLE_WAIT_NS 2000000ull
static bool ry_busy(void) { return nr_pdsch_passive_first_pass_waiting && nr_pdsch_passive_first_pass_waiting(); }
static void ry_nap(void) { usleep(100); }
static bool ry_wait_idle(void) { return nr_recov_wait_idle(ry_busy, dec_now_ns, ry_nap, RY_IDLE_WAIT_NS); }
/* ry_arm_worth: mode 2 gate of one arm (see above); counts the gated tries */
static bool ry_arm_worth(int k)
{
  if (nr_retry_gate_pass(atomic_load_explicit(&g_ry_try[k], memory_order_relaxed), atomic_load_explicit(&g_ry_ok[k], memory_order_relaxed),
                         atomic_fetch_add_explicit(&g_ry_seen[k], 1, memory_order_relaxed)))
    return true;
  atomic_fetch_add_explicit(&g_ry_gated, 1, memory_order_relaxed);
  return false;
}
void nr_pdsch_retry_yield_stats(uint64_t *chains, uint64_t *yields, uint64_t *gated)
{
  *chains = atomic_load_explicit(&g_ry_chains, memory_order_relaxed);
  *yields = atomic_load_explicit(&g_ry_yields, memory_order_relaxed);
  *gated = atomic_load_explicit(&g_ry_gated, memory_order_relaxed);
}

static nr_pdsch_passive_decode_status_t passive_decode_retry_impl(PHY_VARS_NR_UE *ue,
                                                             const UE_nr_rxtx_proc_t *proc,
                                                             fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                                             const freq_alloc_bitmap_t *freq_alloc,
                                                             const nr_pdsch_passive_grant_t *grant,
                                                             c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                                             nr_pdsch_passive_decode_result_t *out)
{
  /* nr_pdsch_retry_pred.h: the S toggle (arm 2) runs first, learnt per RNTI; the final re-decode of the plain reading after every arm failed is
   * skipped (it repeats the first body bit for bit; its only effect was to restore *out). */
  nr_pdsch_passive_decode_status_t st;
  if (t_recov_mode == 2) {
    /* RECOVERY_ASYNC worker: the consumer's first pass already failed; the arms below each re-demodulate from the IQ snapshot with their own
     * configuration and use nothing of that pass but its FO (the job's FO override) -- so no first body here. */
    st = NR_PDSCH_PASSIVE_DECODE_CRC_FAIL;
    t_last_fep_fo = nr_slot_fep_fo_override_hz;
  } else {
  st = passive_decode_body(ue, proc, dlsch_config, freq_alloc, grant, rxdataF, out);
  if (t_gw_owner) { /* left before the publish point: the same inputs fail the same way, make it sticky */
    t_gw_owner = false;
    nr_td_grantwork_abandon(t_gw, t_gw_sig, (int)st);
  }
  }
  if (out->skip) return st; /* no-chance gate: verdict is final, nothing to retry */
  if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK && grant->dci11 && grant->rnti_class == 0) nr_retry_pred_miss(grant->rnti);
  const nr_pdsch_passive_decode_result_t out_plain = *out; 
  const nr_pdsch_passive_decode_status_t st_plain = st;
  /* DECODE-FIX: CRC success for one DCI does not establish the TDRA or
   * CSI-RS exclusions for every later DCI. These default-off recovery arms
   * re-demodulate the SAME samples and accept only a full, nonzero TB CRC.
   * A successful PDU is retained for data-aided reconstruction. */
  /* DCI 1_0 in a common search space (RA/TC/P-RNTI) takes its TDRA row from pdsch-ConfigCommon
   * (TS 38.214 5.1.2.1.1 Table 5.1.2.1.1-1). An NSA-style SIB1 carries none, so the receiver falls back to default table A
   * (row 0 = S=2 L=12) although the cell's own list is not that table; every such grant then fails the CRC. Re-decode with the other
   * type-A starts S=0..3 ending at symbol 14 (DM-RS re-derived by TS 38.211 Table 7.4.1.1.2-3, 1_0 => pos2). */
  if (st == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL && !grant->dci11 && grant->rnti_class != 0 && grant->rnti_class != 2 && !t_quiet && !t_llr_ovr
      && !t_gw_capture && RETRY_ARMS_OK()
      && dlsch_config->start_symbol + dlsch_config->number_symbols == 14 && (dlsch_config->dlDmrsSymbPos & 0x3FFFu)) {
    if (t_recov_mode == 1) { /* RECOVERY_ASYNC: the sweep runs on the worker; the failed first pass stands (GrantWork owners are abandoned by the caller) */
      t_recov_deferred = true;
      return st;
    }
    if (t_recov_mode == 2 && !ry_wait_idle()) return st; /* TAIL-FIX: first-pass work waits, the worker yields */
    const fapi_nr_dl_config_dlsch_pdu_rel15_t original = *dlsch_config;
    const int l0 = __builtin_ctz(original.dlDmrsSymbPos & 0x3FFFu);
    const bool saved_quiet = t_quiet;
    const double saved_fo_override = nr_slot_fep_fo_override_hz;
    nr_slot_fep_fo_override_hz = t_last_fep_fo;
    t_quiet = true;
    bool ok = false;
    for (int S = 0; S <= 3 && !ok; S++) {
      if (S == (int)original.start_symbol || S > l0) continue;
      const int32_t m = nr_pdcch_blind_dmrs_mask(l0 == 3, 14 - S, S, 0, 2, 1);
      if (m <= 0) continue;
      fapi_nr_dl_config_dlsch_pdu_rel15_t cfg = original;
      cfg.start_symbol = (uint8_t)S;
      cfg.number_symbols = (uint8_t)(14 - S);
      cfg.dlDmrsSymbPos = (original.dlDmrsSymbPos & ~0x3FFFu) | (uint32_t)m;
      t_decode_tdra_attempt = 7 + 64 * (70 + S);
      st = passive_decode_body(ue, proc, &cfg, freq_alloc, grant, rxdataF, out);
      if (t_gw_owner) { t_gw_owner = false; nr_td_grantwork_abandon(t_gw, t_gw_sig, (int)st); }
      if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK) { ok = true; *dlsch_config = cfg; out->config_recovered = true; }
    }
    LOG_I(PHY, "SENSING: RATDRA rnti=0x%04x cls=%u fs=%d.%d S=%u L=%u -> S=%u L=%u ok=%d\n", grant->rnti, grant->rnti_class, proc->frame_rx,
          proc->nr_slot_rx, original.start_symbol, original.number_symbols, dlsch_config->start_symbol, dlsch_config->number_symbols, ok);
    if (!ok) {
      *dlsch_config = original;
      if (t_recov_mode != 2) { /* restore the plain view; the worker has no first body to restore */
      t_decode_tdra_attempt = 32;
      st = passive_decode_body(ue, proc, dlsch_config, freq_alloc, grant, rxdataF, out);
      if (t_gw_owner) { t_gw_owner = false; nr_td_grantwork_abandon(t_gw, t_gw_sig, (int)st); }
      }
    }
    nr_slot_fep_fo_override_hz = saved_fo_override;
    t_quiet = saved_quiet;
    t_decode_tdra_attempt = 0;
    return st;
  }
  /* Fix A (ISAC_PDSCH_TDA_FIELD=1): a grant in a TDD mixed slot carries a second legal (S,L) row (nr_tda_field_rows: SIB1 row picked by the
   * DCI's TDRA field, else the other legal row). Re-decode with it; the first CRC pass is kept. Logs TDAFIELD. */
  if (grant->tda_n > 1 && st == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL && grant->dci11 && grant->rnti_class == 0 && !t_probe_first_seg && !t_gw_capture && !t_gw_used
      && !t_quiet && !t_llr_ovr && RETRY_ARMS_OK() && (dlsch_config->dlDmrsSymbPos & 0x3FFFu)) {
    if (t_recov_mode == 1) { /* RECOVERY_ASYNC: runs on the worker; the failed first pass stands */
      t_recov_deferred = true;
      return st;
    }
    if (t_recov_mode == 2 && !ry_wait_idle()) return st; /* TAIL-FIX: first-pass work waits, the worker yields */
    const fapi_nr_dl_config_dlsch_pdu_rel15_t original = *dlsch_config;
    const bool saved_quiet = t_quiet;
    const double saved_fo_override = nr_slot_fep_fo_override_hz;
    nr_slot_fep_fo_override_hz = t_last_fep_fo;
    t_quiet = true;
    fapi_nr_dl_config_dlsch_pdu_rel15_t cfg = original;
    cfg.start_symbol = grant->tda_S[1];
    cfg.number_symbols = grant->tda_L[1];
    cfg.dlDmrsSymbPos = (original.dlDmrsSymbPos & ~0x3FFFu)
                        | nr_pdsch_solver_dmrs_mask(__builtin_ctz(original.dlDmrsSymbPos & 0x3FFFu), cfg.start_symbol, cfg.number_symbols, grant->tda_addpos);
    t_decode_tdra_attempt = 8 + 64 * 90;
    st = passive_decode_body(ue, proc, &cfg, freq_alloc, grant, rxdataF, out);
    if (t_gw_owner) { t_gw_owner = false; nr_td_grantwork_abandon(t_gw, t_gw_sig, (int)st); }
    const bool ok = st == NR_PDSCH_PASSIVE_DECODE_CRC_OK;
    LOG_I(PHY, "SENSING: TDAFIELD rnti=0x%04x fs=%d.%d abs=%ld S=%u L=%u -> S=%u L=%u ok=%d\n", grant->rnti, proc->frame_rx, proc->nr_slot_rx,
          grant->source_absolute_slot, original.start_symbol, original.number_symbols, cfg.start_symbol, cfg.number_symbols, ok);
    if (ok) {
      *dlsch_config = cfg;
      out->config_recovered = true;
    } else {
      *out = out_plain; /* as RETRY_NORESTORE: the plain view stands */
      st = st_plain;
    }
    nr_slot_fep_fo_override_hz = saved_fo_override;
    t_quiet = saved_quiet;
    t_decode_tdra_attempt = 0;
    if (ok) return st;
  }
  if (st != NR_PDSCH_PASSIVE_DECODE_CRC_FAIL || !grant->dci11 || grant->rnti_class != 0 || t_probe_first_seg || t_gw_capture || t_gw_used || t_quiet
      || t_llr_ovr || !RETRY_ARMS_OK())
    return st;
  if (t_recov_mode == 1) { /* RECOVERY_ASYNC: every arm below runs on the worker; the failed first pass stands */
    t_recov_deferred = true;
    return st;
  }
  const bool try_start = dlsch_config->start_symbol <= 1
                         && dlsch_config->start_symbol + dlsch_config->number_symbols == 14;
  const bool try_cdm = dlsch_config->dmrsConfigType == NFAPI_NR_DMRS_TYPE1
                       && dlsch_config->dmrs_ports && !(dlsch_config->dmrs_ports & ~3u)
                       && (dlsch_config->n_dmrs_cdm_groups == 1 || dlsch_config->n_dmrs_cdm_groups == 2);
  fapi_nr_dl_config_dlsch_pdu_rel15_t alternate_csi = *dlsch_config;
  bool try_csi = false;
  if (st == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL && !t_probe_first_seg && !t_quiet) {
    if (alternate_csi.numCsiRsForRateMatching > 0) {
      alternate_csi.numCsiRsForRateMatching = 0;
      try_csi = true;
    } else {
      const uint32_t slot = (uint32_t)proc->frame_rx * ue->frame_parms.slots_per_frame + proc->nr_slot_rx;
      alternate_csi.numCsiRsForRateMatching = nr_csirs_blind_rt_rate_match_all(
          slot, alternate_csi.csiRsForRateMatching, NR_CSI_RM_LEGACY_MAX);
      try_csi = alternate_csi.numCsiRsForRateMatching > 0 && nr_dlsch_csi_unav_res(&alternate_csi, freq_alloc) > 0;
    }
  }
  fapi_nr_dl_config_dlsch_pdu_rel15_t csi_variants[3];
  csi_variants[0] = alternate_csi;
  int n_csi_variants = 1;
  {
    /* Discovery sees several cells. A union of their resources is not this
     * UE's rate-matching configuration. Try a same-ID subset independently.
     * Its detected bandwidth can also stop before the configured edge: a
     * separate 16-RB extent hypothesis recovered a measured two-CB TB. */
    fapi_nr_dl_config_csirs_pdu_rel15_t resources[64];
    const uint32_t slot = (uint32_t)proc->frame_rx * ue->frame_parms.slots_per_frame + proc->nr_slot_rx;
    const int n = nr_csirs_blind_rt_rate_match_all(slot, resources, 64);
    csi_variants[1] = *dlsch_config;
    csi_variants[1].numCsiRsForRateMatching = 0;
    for (int i = 0; i < n && csi_variants[1].numCsiRsForRateMatching < NR_CSI_RM_LEGACY_MAX; i++) {
      if (resources[i].scramb_id != dlsch_config->dlDmrsScramblingId) continue;
      csi_variants[1].csiRsForRateMatching[csi_variants[1].numCsiRsForRateMatching++] = resources[i];
    }
    if (csi_variants[1].numCsiRsForRateMatching && nr_dlsch_csi_unav_res(&csi_variants[1], freq_alloc)) {
      n_csi_variants = 2;
      csi_variants[2] = csi_variants[1];
      bool changed = false;
      for (int i = 0; i < csi_variants[2].numCsiRsForRateMatching; i++) {
        fapi_nr_dl_config_csirs_pdu_rel15_t *p = &csi_variants[2].csiRsForRateMatching[i];
        const int start = p->start_rb & ~15;
        const int end = ((p->start_rb + p->nr_of_rbs + 15) / 16) * 16;
        if (end > 276) continue;
        changed |= start != p->start_rb || end - start != p->nr_of_rbs;
        p->start_rb = start;
        p->nr_of_rbs = end - start;
      }
      if (changed) n_csi_variants = 3;
    }
  }
  const bool try_wide = dlsch_config->cw_info[0].Nl == 1
                        && dlsch_config->cw_info[0].qamModOrder == 2;

  if (try_start || try_csi || try_cdm || try_wide || n_csi_variants > 1) {
    const fapi_nr_dl_config_dlsch_pdu_rel15_t original = *dlsch_config;
    const bool saved_quiet = t_quiet;
    const double saved_fo_override = nr_slot_fep_fo_override_hz;
    nr_slot_fep_fo_override_hz = t_last_fep_fo; /* same frequency compensation for paired trials */
    t_quiet = true; /* do not teach unrelated parameter sweeps from these trials */
    bool recovered = false, yielded = false;
    int ry_bodies = 0;
    atomic_fetch_add_explicit(&g_ry_chains, 1, memory_order_relaxed);
    /* Bits 0..3 select CSI-RS, S=0/1, CDM groups and wide MRC.
     * Type-1 ports 0/1 occupy CDM group 0; either one or two groups is legal. */
    for (int cv = 0; cv < n_csi_variants && !recovered && !yielded; cv++) {
      for (int a = 1; a <= 15 && !recovered; a++) {
        const int arm = cv == 0 && a <= 2 ? 3 - a : a; /* the S toggle (arm 2) first */
        if (cv > 0 && !(arm & 1)) continue;
        if (((arm & 1) && cv == 0 && !try_csi) || ((arm & 2) && !try_start)
            || ((arm & 4) && !try_cdm) || ((arm & 8) && !try_wide))
          continue;
        const int ry_k = (arm + 32 * cv) % RY_ARMS;
        if (!ry_arm_worth(ry_k))
          continue;
        if (t_recov_mode == 2 && !ry_wait_idle()) {
          yielded = true; /* first-pass work is waiting: the rest of this grant's arms are dropped (counted) */
          atomic_fetch_add_explicit(&g_ry_yields, 1, memory_order_relaxed);
          break;
        }
        if (t_recov_mode != 2 && !(cv == 0 && arm == 2)
            && (ry_backlog() > 0 || dec_now_ns() - t_dec_t0_ns > RETRY_BUDGET_NS)) {
          yielded = true; /* a full decode waits for a consumer: stop this grant's long-shot arms */
          atomic_fetch_add_explicit(&g_ry_yields, 1, memory_order_relaxed);
          break;
        }
        ry_bodies++;
        *dlsch_config = original;
        if (arm & 1) *dlsch_config = csi_variants[cv];
        if (arm & 2) {
          /* Type-A DM-RS positions at end symbol 14 agree for S=0 and S=1. */
          dlsch_config->start_symbol = 1 - original.start_symbol;
          dlsch_config->number_symbols = 14 - dlsch_config->start_symbol;
        }
        if (arm & 4) dlsch_config->n_dmrs_cdm_groups = 3 - original.n_dmrs_cdm_groups;
        nr_pdsch_decode_wide_mrc = (arm & 8) != 0;
        t_decode_tdra_attempt = arm + 64 * cv;
        st = passive_decode_body(ue, proc, dlsch_config, freq_alloc, grant, rxdataF, out);
        if (t_gw_owner) {
          t_gw_owner = false;
          nr_td_grantwork_abandon(t_gw, t_gw_sig, (int)st);
        }
        recovered = st == NR_PDSCH_PASSIVE_DECODE_CRC_OK;
        atomic_fetch_add_explicit(&g_ry_try[ry_k], 1, memory_order_relaxed);
        if (recovered) atomic_fetch_add_explicit(&g_ry_ok[ry_k], 1, memory_order_relaxed);
        if (recovered) {
          if (arm == 2 && cv == 0) nr_retry_pred_hit(grant->rnti);
          else nr_retry_pred_miss(grant->rnti);
        }
      }
    }
    LOG_I(PHY, "SENSING: DECODETDRA rnti=0x%04x fs=%d.%d abs=%ld before=0 after=%d S=%u L=%u csi=%u altS=%u altL=%u alt_csi=%u cdm=%u alt_cdm=%u arm=%d\n",
          grant->rnti, proc->frame_rx, proc->nr_slot_rx, grant->source_absolute_slot, recovered,
          original.start_symbol, original.number_symbols, original.numCsiRsForRateMatching,
          dlsch_config->start_symbol, dlsch_config->number_symbols, dlsch_config->numCsiRsForRateMatching, original.n_dmrs_cdm_groups, dlsch_config->n_dmrs_cdm_groups,
          t_decode_tdra_attempt);
    nr_pdsch_decode_wide_mrc = false;
    if (!recovered && ry_bodies > 0 && t_recov_mode != 2) {
      /* Restore the public result AND the thread-local decoder/LLR view
       * used by the queue's diagnostics and equivalence checks. */
      *dlsch_config = original;
      *out = out_plain;
      st = st_plain;
    }
    out->config_recovered = recovered;
    nr_slot_fep_fo_override_hz = saved_fo_override;
    t_quiet = saved_quiet;
    t_decode_tdra_attempt = 0;
  }
  return st;
}

/* Fix A (ISAC_PDSCH_TDA_FIELD): teaches the TDRA-field detector the (S,L) of every CRC-passing dedicated decode, with the field window the monitor recorded. */
static nr_pdsch_passive_decode_status_t passive_decode_retry(PHY_VARS_NR_UE *ue,
                                                             const UE_nr_rxtx_proc_t *proc,
                                                             fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                                             const freq_alloc_bitmap_t *freq_alloc,
                                                             const nr_pdsch_passive_grant_t *grant,
                                                             c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                                             nr_pdsch_passive_decode_result_t *out)
{
  const nr_pdsch_passive_decode_status_t st = passive_decode_retry_impl(ue, proc, dlsch_config, freq_alloc, grant, rxdataF, out);
  if (grant->tda_pos1 && st == NR_PDSCH_PASSIVE_DECODE_CRC_OK && !t_probe_first_seg && !t_quiet && !t_gw_capture && grant->dci_len) {
    nr_dci_bits_t pl = {{grant->dci_w[0], grant->dci_w[1], 0}};
    nr_tda_field_note_ok(grant->tda_pos1 - 1, pl, grant->dci_len, grant->tda_last < 13, dlsch_config->start_symbol, dlsch_config->number_symbols);
  }
  return st;
}

/* ---- GRANTWORK API (nr_pdsch_passive_decode.h) ---------------------------------------------------- */
void nr_pdsch_passive_set_grantwork(nr_td_grantwork_t *gw, bool probes_too)
{
  t_gw = gw;
  t_gw_probe_ok = gw != NULL && probes_too;
}

bool nr_pdsch_passive_last_used_grantwork(bool *served_ready)
{
  if (served_ready)
    *served_ready = t_gw_ready;
  return t_gw_used;
}

/* Lazy computation of a missing signature: decode the gw's grant under that geometry in compute-only mode
 * (whole slot, no probe horizon, no LDPC, no learning-state side effects). The decode publishes. */
static int gw_compute(void *vctx, nr_td_grantwork_t *gw, uint64_t sig, const void *hint)
{
  const gw_ctx_t *c = (const gw_ctx_t *)vctx;
  NR_CFG_EPOCH_WORK(c->config_epoch, NULL);
  if (!nr_cfg_epoch_work_current()) return NR_TD_GW_E_STALE;
  const nr_pdsch_cfg_hypothesis_t *h = (const nr_pdsch_cfg_hypothesis_t *)hint;
  nr_pdsch_cfg_hypothesis_t g = {0}; /* nr_td_signature() layout: S | L << 4 | k0 << 8 | mask << 16 | nl << 32 | qm << 36 */
  g.tda_start = (uint8_t)(sig & 0xF);
  g.tda_length = (uint8_t)((sig >> 4) & 0xF);
  g.k0 = (uint8_t)((sig >> 8) & 0x3F);
  g.dmrs_mask = (uint16_t)((sig >> 16) & 0x3FFF);
  const int nl = (int)((sig >> 32) & 0xF), qm = (int)((sig >> 36) & 0xF);
  if (g.k0 != c->k0)
    return NR_TD_GW_E_K0; /* another PDSCH slot: another GrantWork */
  if (nl != __builtin_popcount(c->pdu.dmrs_ports & 0xFFF))
    return NR_TD_GW_E_ARG;
  int table = -1;
  if (h != NULL && nr_get_Qm_dl(c->grant.mcs, h->mcs_table) == qm)
    table = h->mcs_table;
  for (int t = 0; t < 3 && table < 0; t++)
    if (nr_get_Qm_dl(c->grant.mcs, (uint8_t)t) == qm)
      table = t;
  if (table < 0)
    return NR_TD_GW_E_ARG;
  g.mcs_table = (uint8_t)table;
  fapi_nr_dl_config_dlsch_pdu_rel15_t pdu = c->pdu;
  nr_pdsch_passive_grant_t gr = c->grant;
  nr_pdsch_adaptive_apply(&g, &pdu, &gr.mcs_table, &gr.mcs_table_lbrm);
  pdu.dlDmrsSymbPos = (c->pdu.dlDmrsSymbPos & ~0x3FFFu) | g.dmrs_mask;

  nr_td_grantwork_t *const s_gw = t_gw;
  const bool s_cap = t_gw_capture, s_probe = t_probe_first_seg, s_quiet = t_quiet;
  const bool s_owner = t_gw_owner;
  const uint64_t s_sig = t_gw_sig;
  const int16_t *const s_ovr = t_llr_ovr;
  const uint32_t s_ovr_n = t_llr_ovr_n;
  const double s_fo = nr_slot_fep_fo_override_hz;
  t_gw = gw;
  t_gw_capture = true;
  t_probe_first_seg = false;
  t_quiet = true;
  t_llr_ovr = NULL;
  t_llr_ovr_n = 0;
  nr_slot_fep_fo_override_hz = c->fo_hz;
  nr_pdsch_passive_decode_result_t r;
  const uint32_t stride = c->ue->frame_parms.samples_per_slot_wCP;
  const nr_pdsch_passive_decode_status_t st =
      nr_pdsch_passive_decode(c->ue, &c->proc, &pdu, &c->fa, &gr, (c16_t(*)[stride])nr_td_grantwork_fep(gw), &r);
  t_gw = s_gw;
  t_gw_capture = s_cap;
  t_probe_first_seg = s_probe;
  t_quiet = s_quiet;
  t_gw_owner = s_owner;
  t_gw_sig = s_sig;
  t_llr_ovr = s_ovr;
  t_llr_ovr_n = s_ovr_n;
  nr_slot_fep_fo_override_hz = s_fo;
  return st == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL ? 0 : (int)st;
}

nr_td_grantwork_t *nr_pdsch_passive_grantwork_begin(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc,
                                                    const fapi_nr_dl_config_dlsch_pdu_rel15_t *pdu,
                                                    const freq_alloc_bitmap_t *fa, const nr_pdsch_passive_grant_t *grant,
                                                    double fo_hz, uint8_t k0, c16_t *rxdataF_flat,
                                                    const uint64_t *fep_gen)
{
  gw_ctx_t c;
  memset(&c, 0, sizeof(c));
  if (!nr_cfg_epoch_work_current()) return NULL;
  c.config_epoch = nr_cfg_epoch_work_stamp();
  c.ue = ue;
  c.proc = *proc;
  c.pdu = *pdu;
  c.fa = *fa;
  c.grant = *grant;
  c.fo_hz = fo_hz;
  c.k0 = k0;
  c.fp = gw_fingerprint(pdu, fa, grant, fo_hz);
  const nr_td_gw_job_t job = {.abs_slot = grant->source_absolute_slot,
                              .slots_per_frame = ue->frame_parms.slots_per_frame,
                              .fep = rxdataF_flat,
                              .fep_gen_src = fep_gen,
                              .compute = gw_compute,
                              .ctx = &c,
                              .ctx_len = sizeof(c)};
  return nr_td_grantwork_begin(&job);
}

/* Hypothesis TBS / table -> code block 0 parameters, exactly as nr_pdsch_passive_decode() (TBS, BG, TBS_LBRM)
 * and passive_ldpc_decode_core() (segmentation, E of r = 0) derive them for a full decode of that hypothesis. */
static int gw_cb0_params(const gw_ctx_t *c, const nr_pdsch_cfg_hypothesis_t *h, uint32_t G, int nl, int qm,
                         nr_td_cb0_params_t *p, uint32_t *flags)
{
  const uint32_t R = nr_get_code_rate_dl(c->grant.mcs, h->mcs_table);
  const uint8_t nb_re_dmrs = get_num_dmrs_re_per_rb(c->pdu.dmrsConfigType, c->pdu.n_dmrs_cdm_groups);
  const uint16_t dmrs_len = get_num_dmrs(h->dmrs_mask);
  const uint32_t A = nr_compute_tbs((uint16_t)qm, (uint16_t)R, c->fa.num_rbs, h->tda_length, nb_re_dmrs * dmrs_len,
                                    c->grant.nb_rb_oh, c->grant.tb_scaling, (uint8_t)nl);
  if (A == 0 || R == 0)
    return NR_TD_GW_E_ARG;
  const uint8_t BG = get_BG(A, (uint16_t)R);
  /* nr_pdsch_adaptive_apply(): the hypothesis's own table sizes TBS_LBRM; the job's own table keeps the job's
   * (cell-wide) LBRM table when it was given one. */
  const uint8_t tbl_lbrm = (h->mcs_table == c->grant.mcs_table && c->grant.mcs_table_lbrm >= 0) ? (uint8_t)c->grant.mcs_table_lbrm
                                                                                                  : h->mcs_table;
  const uint16_t bw_lbrm = c->grant.bw_tbslbrm > 0 ? c->grant.bw_tbslbrm : c->pdu.BWPSize;
  const int nl_lbrm = rnti_nl_get(c->grant.rnti, rnti_sweepable(c->grant.rnti, c->grant.rnti_class));
  uint32_t C = 0, K = 0, Z = 0, F = 0;
  nr_segmentation(NULL, NULL, lenWithCrc(1, A), &C, &K, &Z, &F, BG);
  if (C == 0)
    return NR_TD_GW_E_ARG;
  memset(p, 0, sizeof(*p));
  p->G = G;
  p->A = A;
  p->tbslbrm = nr_compute_tbslbrm(tbl_lbrm, bw_lbrm, (uint8_t)nl_lbrm);
  p->C = (int)C;
  p->K = (int)K;
  p->Z = (int)Z;
  p->F = (int)F;
  p->BG = BG;
  p->Qm = qm;
  p->Nl = nl;
  p->rv = c->grant.rv;
  const int E = nr_get_E(G, (uint8_t)C, (uint8_t)qm, (uint8_t)nl, 0);
  if (E <= 0)
    return NR_TD_GW_E_ARG;
  p->E = (uint32_t)E;
  /* I2: the full decode re-tries LBRM n_L hypotheses on a failed TB (probes too) while n_L is not latched, whenever
   * E reaches past the smaller N_ref -- the same condition as the decode's retry loop. CB0 here uses one n_L. */
  if (flags != NULL && !rnti_nl_latched(c->grant.rnti, rnti_sweepable(c->grant.rnti, c->grant.rnti_class))) {
    static const int alts[3] = {4, 2, 1};
    const uint32_t N = (BG == 1 ? 66u : 50u) * Z, nref_now = 3u * p->tbslbrm / (2u * C);
    for (int a = 0; a < 3; a++) {
      if (alts[a] == nl_lbrm)
        continue;
      const uint32_t nref_h = 3u * nr_compute_tbslbrm(tbl_lbrm, bw_lbrm, (uint8_t)alts[a]) / (2u * C);
      if (!(p->E <= (nref_h < nref_now ? nref_h : nref_now) || (nref_h >= N && nref_now >= N)))
        *flags |= NR_TD_GW_F_LBRM;
    }
  }
  return NR_TD_GW_OK;
}

int nr_pdsch_passive_gw_cb0(nr_td_grantwork_t *gw, const nr_pdsch_cfg_hypothesis_t *h, nr_pdsch_gw_cb0_t *out)
{
  if (gw == NULL || h == NULL || out == NULL)
    return NR_TD_GW_E_ARG;
  if (h->dmrs_mask == 0)
    return NR_TD_GW_E_ARG; /* M1: a legacy (mask-less) hypothesis does not say which symbols carry DM-RS */
  const gw_ctx_t *c = (const gw_ctx_t *)nr_td_grantwork_ctx(gw);
  NR_CFG_EPOCH_WORK(c->config_epoch, NULL);
  if (!nr_cfg_epoch_work_current()) return NR_TD_GW_E_STALE;
  if (h->k0 != c->k0)
    return NR_TD_GW_E_K0;
  const int nl = __builtin_popcount(c->pdu.dmrs_ports & 0xFFF);
  const int qm = nr_get_Qm_dl(c->grant.mcs, h->mcs_table);
  if (qm == 0 || nr_get_code_rate_dl(c->grant.mcs, h->mcs_table) == 0)
    return NR_TD_GW_E_ARG; /* reserved MCS: the TBS lives in a HARQ record, not in the hypothesis */
  nr_td_gw_llr_view_t v;
  const int rc = nr_td_grantwork_get_llr(gw, nr_td_grantwork_key(h, nl, qm), h, &v);
  out->flags = nr_td_grantwork_flags(gw);
  out->full_decoder_used = c->main_decoder_used;
  out->decoder = NR_TD_DEC_CPU;
  if (!nr_cfg_epoch_work_current()) return NR_TD_GW_E_STALE;
  if (rc != NR_TD_GW_OK)
    return rc;
  const uint64_t t0 = nr_td_gw_now_ns();
  nr_td_cb0_params_t p;
  uint32_t hflags = 0;
  int r2 = gw_cb0_params(c, h, v.G, nl, qm, &p, &hflags);
  if (r2 != NR_TD_GW_OK)
    return r2;
  out->flags |= hflags;
  const uint32_t dlen = (uint32_t)nr_td_gw_cb0_dlen(&p);
  if (out->dcap < dlen) {
    nr_td_gw_free(out->d);
    out->d = (int16_t *)nr_td_gw_alloc((size_t)dlen * sizeof(int16_t));
    out->dcap = out->d ? dlen : 0;
    if (out->d == NULL)
      return NR_TD_GW_E_NOMEM;
  }
  if (out->e0cap < p.E) {
    nr_td_gw_free(out->e0);
    out->e0 = (int16_t *)nr_td_gw_alloc((size_t)p.E * sizeof(int16_t));
    out->e0cap = out->e0 ? p.E : 0;
    if (out->e0 == NULL)
      return NR_TD_GW_E_NOMEM;
  }
  /* C1 (K38): the shared LLRs are not normalised; this hypothesis's own shift, over its own ceil(G/C). */
  int k = -1;
  r2 = nr_td_gw_cb0_input(v.llr, v.G, &p, true, out->e0, out->d, &k);
  if (r2 != NR_TD_GW_OK)
    return r2;
  out->p = p;
  out->llr = out->e0;
  out->llr_shared = v.llr;
  out->norm_k = k;
  out->sig = v.sig;
  if (nr_td_gw_tim_on())
    nr_td_gw_tim_add(NR_TD_GWTIM_CB0_RM, nr_td_gw_now_ns() - t0);
  return NR_TD_GW_OK;
}

/* The batch item of one hypothesis from shared work (nr_td_cb0_batch.h contract): the entry's un-normalised LLRs
 * (llr = view.llr, read in place: the caller retains the gw until the batch returns), the hypothesis's own TBS / R /
 * BG / TBS_LBRM / rv, 8 iterations, and its K38 shift k_h (nr_td_gw_cb0_input's rule, without the de-matching: the
 * batch de-matches). Same pre-checks and return codes as nr_pdsch_passive_gw_cb0(). *flags = grant bits + LBRM. */
int nr_pdsch_passive_gw_cb0_item(nr_td_grantwork_t *gw, const nr_pdsch_cfg_hypothesis_t *h, nr_td_cb0_item_t *it,
                                 uint32_t *flags)
{
  if (gw == NULL || h == NULL || it == NULL)
    return NR_TD_GW_E_ARG;
  if (flags)
    *flags = 0;
  if (h->dmrs_mask == 0)
    return NR_TD_GW_E_ARG;
  const gw_ctx_t *c = (const gw_ctx_t *)nr_td_grantwork_ctx(gw);
  NR_CFG_EPOCH_WORK(c->config_epoch, NULL);
  if (!nr_cfg_epoch_work_current()) return NR_TD_GW_E_STALE;
  if (h->k0 != c->k0)
    return NR_TD_GW_E_K0;
  const int nl = __builtin_popcount(c->pdu.dmrs_ports & 0xFFF);
  const int qm = nr_get_Qm_dl(c->grant.mcs, h->mcs_table);
  const uint32_t R = nr_get_code_rate_dl(c->grant.mcs, h->mcs_table);
  if (qm == 0 || R == 0)
    return NR_TD_GW_E_ARG;
  nr_td_gw_llr_view_t v;
  const int rc = nr_td_grantwork_get_llr(gw, nr_td_grantwork_key(h, nl, qm), h, &v);
  if (flags)
    *flags = nr_td_grantwork_flags(gw);
  if (!nr_cfg_epoch_work_current()) return NR_TD_GW_E_STALE;
  if (rc != NR_TD_GW_OK)
    return rc;
  nr_td_cb0_params_t p;
  uint32_t hflags = 0;
  const int r2 = gw_cb0_params(c, h, v.G, nl, qm, &p, &hflags);
  if (r2 != NR_TD_GW_OK)
    return r2;
  if (flags)
    *flags |= hflags;
  int k = 0;
  if (v.G >= 64)
    k = nr_llr_norm_shift_nl(v.llr, nr_llr_norm_span(v.G, nr_llr_norm_num_cb(p.A, p.BG)), nl, qm);
  memset(it, 0, sizeof(*it));
  it->llr = v.llr;
  it->G = v.G;
  it->Qm = (uint8_t)qm;
  it->Nl = (uint8_t)nl;
  it->rv = (uint8_t)p.rv;
  it->tbs = p.A;
  it->mcs_table = h->mcs_table;
  it->tbslbrm = p.tbslbrm;
  it->max_iter = (uint8_t)nr_pdsch_ldpc_cap(); /* passive_ldpc_decode_core(): max_ldpc_iterations */
  it->R = (uint16_t)R;
  it->bg = (uint8_t)p.BG; /* = get_BG(A, R), as the decode */
  it->llr_shift = (uint8_t)(k > 0 ? k : 0);
  return NR_TD_GW_OK;
}

void nr_pdsch_passive_gw_cb0_free(nr_pdsch_gw_cb0_t *cb)
{
  if (cb) {
    nr_td_gw_free(cb->d);
    nr_td_gw_free(cb->e0);
    cb->d = cb->e0 = NULL;
    cb->dcap = cb->e0cap = 0;
  }
}

/* The CPU reference decoder: libldpc.so (version "" forces it whatever loader.ldpc.shlibversion says), i.e.
 * NR_TD_DEC_CPU. It is NOT necessarily the decoder of the full decode (that one's decoder_used is reported apart,
 * nr_pdsch_gw_cb0_t.full_decoder_used): CRC evidence is not exchangeable between decoders (levers spec 9.3). */
static LDPC_decoderfunc_t *g_cb0_ldpc;
static pthread_once_t g_cb0_ldpc_once = PTHREAD_ONCE_INIT;
static void cb0_ldpc_load(void)
{
  loader_shlibfunc_t f[] = {{.fname = "LDPCdecoder"}};
  if (load_module_version_shlib("ldpc", "", f, 1, NULL) >= 0)
    g_cb0_ldpc = (LDPC_decoderfunc_t *)f[0].fptr;
}

void *nr_pdsch_passive_cb0_cpu_ldpc(void)
{
  pthread_once(&g_cb0_ldpc_once, cb0_ldpc_load);
  return (void *)g_cb0_ldpc;
}

bool nr_pdsch_passive_gw_cb0_decode(const nr_pdsch_gw_cb0_t *cb, bool *ldpc_ok)
{
  return nr_pdsch_passive_gw_cb_decode_iter(cb, 0, ldpc_ok);
}

bool nr_pdsch_passive_gw_cb_decode_iter(const nr_pdsch_gw_cb0_t *cb, int max_iter, bool *ldpc_ok)
{
  if (ldpc_ok)
    *ldpc_ok = false;
  pthread_once(&g_cb0_ldpc_once, cb0_ldpc_load);
  if (cb == NULL || cb->d == NULL || g_cb0_ldpc == NULL)
    return false;
  const uint64_t t0 = nr_td_gw_now_ns();
  const nr_td_cb0_params_t *p = &cb->p;
  /* nr_process_decode_segment(), r = 0 */
  /* M5: one block per thread (z | l | out), allocated once; a failed allocation leaves nothing half-set. */
  static __thread uint8_t *blk = NULL;
  if (blk == NULL) {
    blk = (uint8_t *)aligned_alloc(64, (68 * 384 + 64) * sizeof(int16_t) + (68 * 384 + 64) + 27008);
    if (blk == NULL)
      return false;
  }
  int16_t *const z = (int16_t *)blk;
  int8_t *const l = (int8_t *)(blk + (68 * 384 + 64) * sizeof(int16_t));
  int8_t *const out = l + (68 * 384 + 64); /* the segment decoder's llrProcBuf: OAI_LDPC_DECODER_MAX_NUM_LLR = 27000 */
  const int Kc = p->BG == 2 ? 52 : 68;
  const int Kprime = p->K - p->F;
  memset(z, 0, 2 * p->Z * sizeof(*z));
  memset(z + Kprime, 127, p->F * sizeof(*z));
  memcpy(z + 2 * p->Z, cb->d, (Kprime - 2 * p->Z) * sizeof(*z));
  memcpy(z + p->K, cb->d + (p->K - 2 * p->Z), (Kc * p->Z - p->K) * sizeof(*z));
  const int n8 = (((Kc * p->Z) >> 4) + 1) << 4; /* simde_mm_packs_epi16: signed saturation, 16 per step */
  for (int i = 0; i < n8; i++)
    l[i] = (int8_t)(z[i] > 127 ? 127 : (z[i] < -128 ? -128 : z[i]));
  int llrLen = 0;
  t_nrLDPC_dec_params dp = {.check_crc = check_crc};
  dp.BG = (uint8_t)p->BG;
  dp.Z = (uint16_t)p->Z;
  dp.numMaxIter = (uint8_t)nr_pdsch_ldpc_cap(); /* passive_ldpc_decode_core(): max_ldpc_iterations */
  if (max_iter > 0 && max_iter < dp.numMaxIter)
    dp.numMaxIter = (uint8_t)max_iter;
  dp.outMode = nrLDPC_outMode_BIT;
  dp.R = (uint8_t)nr_get_R_ldpc_decoder(p->rv, (int)p->E, p->BG, p->Z, &llrLen, 0);
  dp.crc_type = crcType(p->C, p->A);
  dp.Kprime = lenWithCrc(p->C, p->A);
  t_nrLDPC_time_stats ts = {0};
  decode_abort_t ab;
  init_abort(&ab);
  set_abort(&ab, false);
  const int it = g_cb0_ldpc(&dp, l, (uint8_t *)out, &ts, &ab);
  bool ok = it < dp.numMaxIter;
  if (ldpc_ok)
    *ldpc_ok = ok;
  if (ok) { /* the all-zero guard of the probe (C > 1) / of the full TB (C == 1) */
    const uint32_t nb = p->C > 1 ? (uint32_t)((p->K >> 3) - (p->F >> 3) - 3) : p->A >> 3;
    uint32_t i = 0;
    while (i < nb && out[i] == 0)
      i++;
    if (i == nb)
      ok = false;
  }
  if (nr_td_gw_tim_on())
    nr_td_gw_tim_add(NR_TD_GWTIM_CB0_LDPC, nr_td_gw_now_ns() - t0);
  return ok;
}


/* GrantWork / probe equivalence checks (debug, were ISAC_TD_GW_CHECK / ISAC_TD_GW_PROFILE / ISAC_TD_PROBE_EQUIV_CHECK) are gone.
 * NEEDS: delete these three stubs, their declarations in nr_pdsch_passive_decode.h and the calls in nr_pdsch_passive_queue.c. */
void nr_pdsch_passive_gw_check(nr_td_grantwork_t *gw, nr_pdsch_passive_decode_status_t st_a) { (void)gw; (void)st_a; }
void nr_pdsch_passive_gw_profile(nr_td_grantwork_t *gw, bool used) { (void)gw; (void)used; }
void nr_pdsch_passive_probe_equiv_check(PHY_VARS_NR_UE *ue,
                                        const UE_nr_rxtx_proc_t *proc,
                                        const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                        const freq_alloc_bitmap_t *freq_alloc,
                                        const nr_pdsch_passive_grant_t *grant,
                                        c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP])
{
  (void)ue; (void)proc; (void)dlsch_config; (void)freq_alloc; (void)grant; (void)rxdataF;
}
