#include "nr_passive_sample_lifetime.h"
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
#include "nr_pdsch_qm_oracle.h"
#include "nr_pdsch_chest_key.h" // K32: complete chest-cache key
#include "nr_scrambling_id_sweep.h" // per-RNTI dataScramblingIdentityPDSCH TB-CRC walk (Task 13)

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <math.h>
#include <pthread.h> // sqrt/floor/log10 (EQDIAG)
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
extern __thread int nr_dl_chest_diag_request;
extern __thread int nr_dlsch_chest_per_symbol; // nr_dlsch_demodulation.c: read the estimate at the data symbol itself

#include "PHY/CODING/coding_defs.h"
#include "PHY/NR_REFSIG/dmrs_nr.h" // get_num_dmrs_re_per_rb, nr_chest_time_domain_avg
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/MODULATION/modulation_UE.h" // nr_slot_fep, nr_slot_fep_ant
#include "nr_pdsch_ptrs_unav.h"
#include "nr_agnostic_v2.h"
#include "nr_pdsch_prb_set.h" // nr_prb_segments, nr_prb_gather_index (non-contiguous PRB sets)
#include "nr_arm_sweep.h" // generic per-RNTI Wilson pick/latch core shared by the VRB-L and PRG sweeps
#include "nr_harq_init_tx.h" // per-(RNTI, pid) reserved-MCS retransmission record, shared with the UL decoder
#include "nr_csirs_blind_search.h" // nr_csirs_blind_re_energy / nr_csirs_blind_zp_grant_score
#include "nr_csirs_blind_rt.h" // nr_csirs_blind_rt_zp_grant_evidence
#include "PHY/NR_REFSIG/nr_refsig.h" // nr_gold_pdsch, nr_pdsch_dmrs_rx (ZP grant evidence: own-DM-RS presence)
#include "PHY/NR_TRANSPORT/nr_sch_dmrs.h" // get_delta
_Static_assert(sizeof(((freq_alloc_bitmap_t *)0)->prb_list) == NR_PRB_SET_MAX * sizeof(uint16_t),
               "freq_alloc_bitmap_t.prb_list (common/utils/bits.h) must hold NR_PRB_SET_MAX PRBs");
#include "common/utils/threadPool/task_ans.h" // init_task_ans/join_task_ans/completed_task_ans, for the per-antenna FEP dispatch below
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "PHY/TOOLS/tools_defs.h"
#include "executables/nr-uesoftmodem.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"

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
static uint64_t g_pdtim_calls = 0;
static _Atomic int g_pdtim_on = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */

static inline int pdtim_enabled(void)
{
  if (g_pdtim_on < 0) {
    g_pdtim_on = (getenv("ISAC_PDCCH_TIMING") != NULL) ? 1 : 0;
  }
  return g_pdtim_on;
}

static inline uint64_t pdtim_now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static inline void pdtim_add(int k, uint64_t t0)
{
  if (g_pdtim_on <= 0) {
    return;
  }
  const uint64_t d = pdtim_now() - t0;
  g_pdtim_ns[k] += d;
  g_pdtim_n[k]++;
  if (d > g_pdtim_max[k]) {
    g_pdtim_max[k] = d;
  }
}

static void pdtim_report(void)
{
  if (g_pdtim_on <= 0) {
    return;
  }
  g_pdtim_calls++;
  if ((g_pdtim_calls % 200) != 0) {
    return;
  }
  char rep[520];
  int u = 0;
  for (int k = 0; k < PDTIM_N && u < (int)sizeof(rep) - 90; k++) {
    u += snprintf(rep + u, sizeof(rep) - u, "%s[n=%lu mean=%.1fus max=%.1fus tot=%.2fs] ", kPdtimName[k],
                  (unsigned long)g_pdtim_n[k],
                  g_pdtim_n[k] ? (double)g_pdtim_ns[k] / (double)g_pdtim_n[k] / 1000.0 : 0.0,
                  (double)g_pdtim_max[k] / 1000.0, (double)g_pdtim_ns[k] / 1e9);
  }
  LOG_I(PHY, "SENSING: PDTIM calls=%lu %s\n", (unsigned long)g_pdtim_calls, rep);
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
/* The DMRSFO tracker's current SFO estimate, in ppm, for the correction stage below. Read-mostly
 * across consumer threads; a stale value only means one grant corrected with the previous estimate,
 * so no lock -- but _Atomic (relaxed), since a plain double read during the tracker's store is a data
 * race (N passivePdsch consumers, Task A7 follow-up). */
static _Atomic double g_sfo_ppm_ema = 0.0;
static double nr_pdsch_passive_sfo_ppm(void) { return atomic_load_explicit(&g_sfo_ppm_ema, memory_order_relaxed); }
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
static _Atomic uint64_t g_rv_try[4][32], g_rv_ok[4][32]; // ISAC_RV_RETRY rescues by [rv][mcs]
/* V2 HARQ soft-combining and PT-RS sweep state, declared here so the periodic report can read it. */
static _Atomic uint64_t g_hq_retx_try, g_hq_retx_ok, g_hq_tbs_override, g_hq_busy_skip, g_hq_first;
/* g_harqc's own lock (harqc_entry_t/g_harqc are declared further down, next to their only other
 * users) and the reserved-MCS retransmission record (gap-harq lane) that shares it -- both declared
 * this early, alongside the harqc counters above, for the SAME reason: nr_pdsch_passive_ldpc_stats_
 * dump() (right below) is defined before harqc_entry_t and must be able to read g_dl_harq_init's
 * hit/evict counts under the same lock. See nr_harq_init_tx.h for why TBS/base-graph (not
 * modulation order) is the field a reserved MCS cannot supply on its own. */
static pthread_mutex_t g_harqc_lock = PTHREAD_MUTEX_INITIALIZER;
static nr_harq_init_tx_table_t g_dl_harq_init;
static pthread_mutex_t g_ptrs_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_ptrs_arm_last = -2; // for the report
static _Atomic int g_lbrm_nl = 4;        // TBS_LBRM layer term n_L, CELL-WIDE seed (4 = spec ceiling), see rnti_dec()
static int g_ptrs_cell_arm = -1;         // PT-RS arm, CELL-WIDE seed; under g_ptrs_lock
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
 * nr_arm_sweep_gated_t. ISAC_PRG_SWEEP=0 disables the sweep entirely (always arm 0, no bookkeeping). */
static int prg_sweep_pick(const nr_prg_sweep_t *s) { return nr_arm_sweep_gated_pick(s, NR_PRG_ARMS); }
static int prg_sweep_feed(nr_prg_sweep_t *s, int arm, bool tb_ok, bool link_ok)
{
  return nr_arm_sweep_gated_feed(s, NR_PRG_ARMS, arm, tb_ok, link_ok);
}
static bool prg_sweep_enabled(void)
{
  static _Atomic int on = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
  if (on < 0) {
    const char *e = getenv("ISAC_PRG_SWEEP");
    on = (e != NULL && atoi(e) == 0) ? 0 : 1;
  }
  return on;
}
/// arm -> PRB-bundling size (0/2/4), the field prg_sweep's arm index maps to.
static uint8_t nr_prg_arm_value(int arm) { return arm == 2 ? 4 : (arm == 1 ? 2 : 0); }
/* 64 slots, evidence-protected eviction (final review I4, the policy T1 gave nr_pdsch_config_sweep.c's
 * rnti_ctx()): a slot holding any trial, latch or walk is evicted only when EVERY slot holds evidence,
 * so a burst of one-off noise RNTIs can no longer throw away a real UE's latched VRB-L/PRG/PT-RS/n_L or
 * data-identity decision. SI-/RA-/P-RNTI grants never get a slot (rnti_sweepable()): those identities
 * are not UEs and carry no dedicated RRC configuration. ~2.1 KB/slot, 135 KB of BSS. */
#define RNTI_DEC_MAX 64
typedef struct { uint16_t rnti; uint64_t touched; int nl; nr_ptrs_sweep_t ptrs; nr_vrbl_sweep_t vrbl; nr_prg_sweep_t prg; nr_scrambling_id_sweep_t data_id; } rnti_dec_t;
static rnti_dec_t g_rnti_dec[RNTI_DEC_MAX];
static uint64_t g_rnti_dec_clock;
/* rnti_class is nr_blind_rnti_class_t: 2 = SI, 3 = RA, 4 = P. */
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
  for (int a = 0; a < NR_PTRS_ARMS; a++)
    tr += r->ptrs.tr[a];
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
  nr_ptrs_sweep_init(&r->ptrs);
  r->ptrs.latched = g_ptrs_cell_arm; // seed: a cell-wide arm, or -1 = sweep from scratch
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
static int rnti_nl_latch(uint16_t rnti, bool sweepable, int nl)
{
  if (!sweepable)
    return nl; /* no per-RNTI state for SI/RA/P: nothing to latch, nothing to log */
  pthread_mutex_lock(&g_ptrs_lock);
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
  pthread_mutex_lock(&g_ptrs_lock);
  rnti_dec_t *r = rnti_dec_find(rnti, false);
  if (r && r->data_id.n > 0 && r->data_id.latched < 0) {
    const int tried = nr_scrambling_id_sweep_current(&r->data_id);
    nr_scrambling_id_sweep_feed(&r->data_id, tb_crc_ok ? 1 : 0);
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
static int rnti_ptrs_pick(uint16_t rnti)
{
  pthread_mutex_lock(&g_ptrs_lock);
  const int arm = nr_ptrs_sweep_pick(&rnti_dec(rnti)->ptrs);
  pthread_mutex_unlock(&g_ptrs_lock);
  return arm;
}
static int rnti_ptrs_feed(uint16_t rnti, int arm, bool tb_ok)
{
  pthread_mutex_lock(&g_ptrs_lock);
  const int latched = nr_ptrs_sweep_feed(&rnti_dec(rnti)->ptrs, arm, tb_ok);
  if (latched >= 0 && g_ptrs_cell_arm < 0)
    for (int i = 0; i < RNTI_DEC_MAX; i++)
      if (g_rnti_dec[i].rnti && g_rnti_dec[i].rnti != rnti && g_rnti_dec[i].ptrs.latched == latched) {
        g_ptrs_cell_arm = latched;
        break;
      }
  pthread_mutex_unlock(&g_ptrs_lock);
  return latched;
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
  pthread_mutex_lock(&g_ptrs_lock);
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
  pthread_mutex_lock(&g_ptrs_lock);
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
void nr_pdsch_passive_crc_note(uint16_t rnti, bool dedicated, bool crc_ok)
{
  nr_scr_link_note(&g_dl_scr_link, rnti, dedicated, crc_ok);
  if (!dedicated)
    return;
  if (crc_ok)
    atomic_store_explicit(&g_ded_fails_since_ok[rnti], 0, memory_order_relaxed);
  else
    atomic_fetch_add_explicit(&g_ded_fails_since_ok[rnti], 1, memory_order_relaxed);
}
uint32_t nr_pdsch_passive_rnti_ded_fails(uint16_t rnti)
{
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
/* mean |LLR| the int8 decoder gets: 127/40 ~ 3.2x headroom over the mean for the 256QAM outer bits */
#define LLR_NORM_TARGET 40u
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
    char rb[400];
    size_t u = 0;
    for (int m = 0; m < 32 && u < sizeof(rb) - 40; m++) {
      const uint64_t t2 = atomic_load(&g_rv_try[2][m]);
      if (t2 == 0)
        continue;
      u += snprintf(rb + u, sizeof(rb) - u, "mcs%d: rv2 %lu/%lu rv3 %lu/%lu rv1 %lu/%lu; ", m,
                    (unsigned long)atomic_load(&g_rv_ok[2][m]), (unsigned long)t2,
                    (unsigned long)atomic_load(&g_rv_ok[3][m]), (unsigned long)atomic_load(&g_rv_try[3][m]),
                    (unsigned long)atomic_load(&g_rv_ok[1][m]), (unsigned long)atomic_load(&g_rv_try[1][m]));
    }
    if (u > 0)
      LOG_I(PHY, "SENSING: RVRETRY rescued/tried by mcs: %s\n", rb);
  }
  if (nr_agnostic_v2()) {
    uint64_t init_tx_hits, init_tx_evicts;
    pthread_mutex_lock(&g_harqc_lock);
    init_tx_hits   = g_dl_harq_init.hits;
    init_tx_evicts = g_dl_harq_init.evicts;
    pthread_mutex_unlock(&g_harqc_lock);
    LOG_I(PHY, "SENSING: HARQC first=%lu retx_combined=%lu/%lu tbs_from_first=%lu busy_skip=%lu | init_tx hit=%lu evict=%lu | rv census mcs<24 [%lu %lu %lu %lu] mcs>=24 [%lu %lu %lu %lu] | lbrm n_L=%d try/ok 4:%lu/%lu 2:%lu/%lu 1:%lu/%lu | llr_norm shift0..4 [%lu %lu %lu %lu %lu]\n",
          (unsigned long)atomic_load(&g_hq_first), (unsigned long)atomic_load(&g_hq_retx_ok),
          (unsigned long)atomic_load(&g_hq_retx_try), (unsigned long)atomic_load(&g_hq_tbs_override),
          (unsigned long)atomic_load(&g_hq_busy_skip),
          (unsigned long)init_tx_hits, (unsigned long)init_tx_evicts,
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
    pthread_mutex_lock(&g_ptrs_lock);
    nr_ptrs_sweep_t sum = {0};
    int n_rnti = 0, n_latched = 0;
    for (int i = 0; i < RNTI_DEC_MAX; i++) {
      if (!g_rnti_dec[i].rnti)
        continue;
      n_rnti++;
      n_latched += g_rnti_dec[i].ptrs.latched >= 0;
      for (int a = 0; a < NR_PTRS_ARMS; a++) { sum.ok[a] += g_rnti_dec[i].ptrs.ok[a]; sum.tr[a] += g_rnti_dec[i].ptrs.tr[a]; }
    }
    LOG_I(PHY, "SENSING: PTRS_SWEEP cell_arm=%d rntis=%d latched=%d ok/trials per arm, all RNTIs [absent %u/%u | K2L1 %u/%u K2L2 %u/%u K2L4 %u/%u | K4L1 %u/%u K4L2 %u/%u K4L4 %u/%u]\n",
          g_ptrs_cell_arm, n_rnti, n_latched, sum.ok[0], sum.tr[0], sum.ok[1], sum.tr[1], sum.ok[2], sum.tr[2],
          sum.ok[3], sum.tr[3], sum.ok[4], sum.tr[4], sum.ok[5], sum.tr[5], sum.ok[6], sum.tr[6]);
    pthread_mutex_unlock(&g_ptrs_lock);
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

/// Bounded append for CHESTDIAG's report string. snprintf() returns the length it WOULD have
/// written, so accumulating its return value directly walks the buffer once the text is truncated --
/// which is easy to reach at Nl x nb_antennas_rx entries. Clamps to `cap - 1` instead.
static void rep_append(char *buf, size_t cap, int *u, const char *fmt, ...)
{
  if (cap == 0 || *u < 0 || (size_t)*u >= cap - 1) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf + *u, cap - (size_t)*u, fmt, ap);
  va_end(ap);
  if (n < 0) {
    return;
  }
  *u = ((size_t)(*u + n) >= cap - 1) ? (int)(cap - 1) : (*u + n);
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
 * behind another's decode). Evicted least-recently-used; 16 entries covers every live HARQ process
 * of two UEs, which is what a lab cell has. */
#define NR_HARQC_N 16
#define NR_HARQC_RV0_EVIDENCE 64 /* same-NDI rv-0 grants on undecoded processes before rv-0 combining is admitted */
typedef struct {
  _Atomic int busy;
  bool used, soft_valid;
  uint16_t rnti;
  uint8_t pid, ndi;
  uint32_t tbs;
  uint64_t last;
  int16_t *d;
  size_t cap;
} harqc_entry_t;
static harqc_entry_t g_harqc[NR_HARQC_N];
/* g_harqc_lock and g_dl_harq_init (the reserved-MCS retransmission record that shares this lock)
 * are declared much earlier in this file, alongside g_hq_first and friends -- see the comment
 * there for why. */
static uint64_t g_harqc_clock;
static __thread struct { int armed; uint16_t rnti; uint8_t pid, ndi; } t_hq;
static __thread int16_t *t_hq_d = NULL;
/* BRANCHFO residuals measured on this thread's current grant, committed only on a TB CRC pass. */
static __thread bool t_brfo_pending = false;
static __thread int t_brfo_nant = 0;
static __thread double t_brfo_d[NR_DL_CHEST_MAX_ANT];
static pthread_mutex_t s_brfo_lock = PTHREAD_MUTEX_INITIALIZER;
static void brfo_commit(void)
{
  static double s_fo_corr[NR_DL_CHEST_MAX_ANT];
  static _Atomic int s_brfo = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
  if (!t_brfo_pending)
    return;
  t_brfo_pending = false;
  if (s_brfo < 0) {
    const char *e = getenv("ISAC_RX_BRANCH_FO");
    s_brfo = (e != NULL) && (atoi(e) != 0); // default OFF (as before the port): on-air validation pending
  }
  if (!s_brfo)
    return;
  pthread_mutex_lock(&s_brfo_lock);
  for (int a = 0; a < t_brfo_nant && a < NR_DL_CHEST_MAX_ANT; a++) {
    /* INTEGRATE. `d` is the residual AFTER the correction already in the FEP, so the correction
     * must accumulate it, not be replaced by it -- replacing settled at half the offset (measured
     * -350..-700 Hz residual with the loop "on"). Gain 0.05/grant, clamp to the aliasing limit. */
    s_fo_corr[a] -= 0.05 * t_brfo_d[a];
    if (s_fo_corr[a] > 1500.0) s_fo_corr[a] = 1500.0;
    if (s_fo_corr[a] < -1500.0) s_fo_corr[a] = -1500.0;
    nr_ue_set_branch_fo_hz(a, s_fo_corr[a]);
  }
  pthread_mutex_unlock(&s_brfo_lock);
}
static __thread bool t_probe_first_seg = false; /* decode segment 0 only; outcome in t_probe_seg_ok */
static __thread bool t_probe_seg_ok = false;
void nr_pdsch_passive_probe_mode(bool on) { t_probe_first_seg = on; t_probe_seg_ok = false; }
static __thread bool t_ptrs_sweep_allow = true;
void nr_pdsch_passive_ptrs_sweep_allow(bool on) { t_ptrs_sweep_allow = on; }
bool nr_pdsch_passive_probe_outcome(void) { return t_probe_seg_ok; }

static __thread const int16_t *t_last_llr = NULL;
static __thread uint32_t t_last_G = 0;
/* Probe-equivalence rerun (ISAC_TD_PROBE_EQUIV_CHECK): process the whole slot despite probe mode, and
 * neither read nor fill the chest cache, so the reference is a cache-free full-slot decode. */
static __thread bool t_probe_no_horizon = false, t_chest_bypass = false;
static __thread int t_last_probe_horizon = -1, t_last_chest_hit = 0; /* for the PROBE_EQUIV log */
uint32_t nr_pdsch_passive_last_llr(const int16_t **p) { *p = t_last_llr; return t_last_G; }
static __thread const int16_t *t_llr_ovr = NULL;
static __thread uint32_t t_llr_ovr_n = 0;
void nr_pdsch_passive_set_llr_override(const int16_t *llr, uint32_t n) { t_llr_ovr = n ? llr : NULL; t_llr_ovr_n = n; }

bool nr_pdsch_passive_gpu_job(const PHY_VARS_NR_UE *ue, const fapi_nr_dl_config_dlsch_pdu_rel15_t *pdu,
                              const freq_alloc_bitmap_t *fa, const nr_pdsch_passive_grant_t *grant, int slot_rx,
                              bool probe, nr_gpu_pdsch_job_t *job)
{
  memset(job, 0, sizeof(*job));
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  /* what the GPU does not model: PT-RS, CSI-RS rate matching (the decode also sweeps PT-RS arms on
   * agnostic runs -- it clears the override itself when it arms one) */
  if ((pdu->pduBitmap & 0x1) || pdu->numCsiRsForRateMatching > 0)
    return false;
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
  {
    static _Atomic int s_tinterp = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
    if (s_tinterp < 0) { const char *e = getenv("ISAC_CHEST_TINTERP"); s_tinterp = (e != NULL && atoi(e) != 0) ? 1 : 0; }
    job->time_interp = (uint8_t)s_tinterp;
  }
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
    e->used = true; e->rnti = rnti; e->pid = pid; e->soft_valid = false; e->tbs = 0;
  }
  e->last = ++g_harqc_clock;
  pthread_mutex_unlock(&g_harqc_lock);
  return e;
}

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
                                                       .threadPool = &get_nrUE_params()->Tpool,
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

  TB_parameters.max_ldpc_iterations = 8; // same default init_nr_ue_dlsch() gives the real DLSCH
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
  /* SEGDIAG (ISAC_PDSCH_TBPARM=1): the full rate-matching / segmentation parameter set, on ONE
   * line, so an MCS that fails deterministically can be diffed against one that succeeds without
   * pairing log lines. Everything the LDPC decoder is handed is here. */
  {
    static _Atomic int s_sd = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
    if (s_sd < 0)
      s_sd = (getenv("ISAC_PDSCH_TBPARM") != NULL) ? 1 : 0;
    /* RATE-LIMITED (2026-08-25). This fired on EVERY decode: 30204 lines / 16 MB on a 48 s run,
     * and it shares ISAC_PDSCH_TBPARM with CHESTDIAG, which is capped at 12 -- so wanting the
     * 12 per-branch SNR samples used to cost the full SEGDIAG flood. The handover's 27.3 records an
     * instrument firing every occasion costing 163 MB/run and CAUSING the timing runaway it was
     * added to diagnose. 1-in-500 gives ~60 samples/run, and nr_pdsch_passive_parmset_dump() already
     * records every DISTINCT parameter tuple with counts, so nothing is lost by sampling. */
    static __thread unsigned long s_sd_n = 0;
    if (s_sd && (s_sd_n++ % 500) == 0)
      LOG_I(PHY,
            "SENSING: SEGDIAG mcs=%u BG=%u A=%u G=%u C=%u K=%u Z=%u F=%u E=%u E2=%u frE2=%u R=%u R2=%u "
            "Qm=%u nl=%u rv=%u tbslbrm=%u nb_rb=%d llrLen=%d\n",
            (unsigned)cw->mcs, (unsigned)TB_parameters.BG, (unsigned)TB_parameters.A, (unsigned)TB_parameters.G,
            (unsigned)TB_parameters.C, (unsigned)TB_parameters.K, (unsigned)TB_parameters.Z,
            (unsigned)TB_parameters.F, (unsigned)TB_parameters.E, (unsigned)TB_parameters.E2,
            (unsigned)TB_parameters.first_rE2, (unsigned)TB_parameters.R, (unsigned)TB_parameters.R2,
            (unsigned)TB_parameters.Qm, (unsigned)TB_parameters.nb_layers, (unsigned)TB_parameters.rv_index,
            (unsigned)TB_parameters.tbslbrm, TB_parameters.nb_rb, h->llrLen);
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
  for (uint32_t r = 0; r < TB_parameters.C; r++) {
    TB_parameters.decodeSuccess[r] = false;
  }
  reset_meas(&TB_parameters.ts_deinterleave);
  reset_meas(&TB_parameters.ts_rate_unmatch);
  reset_meas(&TB_parameters.ts_seg_prep);
  reset_meas(&TB_parameters.ts_ldpc_decode);

  if (ue->nrLDPC_coding_interface.nrLDPC_coding_decoder(&slot_parameters) != 0) {
    LOG_W(NR_PHY, "SENSING: passive PDSCH decode -- nrLDPC_coding_decoder failed\n");
    atomic_fetch_add(&g_ldpc_iface_err, 1);
    return false;
  }

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
    if (t_probe_first_seg && C_full > 1) {
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
  if (!t_hq.armed || t_probe_first_seg)
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
  /* A retransmission is "same NDI on this HARQ process" (TS 38.321 5.3.2.2); the RV only sets the
   * circular-buffer start the rate de-matcher already takes from cw->rv. Combining into an rv-0 grant
   * was withheld so a wrong layout hypothesis (junk NDI/PID) cannot poison a fresh decode; it is
   * admitted only once THIS cell has shown it retransmits at rv 0 -- same NDI with rv 0 on a still-
   * undecoded process, seen NR_HARQC_RV0_EVIDENCE times (the census the HARQC line reports). */
  static _Atomic uint32_t s_rv0_retx_seen;
  const bool same_ndi = e->soft_valid && e->ndi == t_hq.ndi;
  /* Evidence needs the TBS to match the stored first transmission too: a junk-layout NDI matches by
   * chance half the time, a junk TBS does not, so the count cannot be filled by mis-parsed grants. */
  if (same_ndi && cw->rv == 0 && e->tbs != 0 && e->tbs == cw->TBS)
    atomic_fetch_add(&s_rv0_retx_seen, 1);
  const bool rv0_admitted = atomic_load(&s_rv0_retx_seen) >= NR_HARQC_RV0_EVIDENCE;
  const bool retx = same_ndi && (cw->rv != 0 || rv0_admitted);
  uint32_t A = cw->TBS;
  if (retx && e->tbs != 0 && e->tbs != A) {
    A = e->tbs;
    atomic_fetch_add(&g_hq_tbs_override, 1);
  }
  t_hq_d = e->d;
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
  c16_t *est; /* [layer x antenna][DM-RS symbol k][ofdm_symbol_size], immutable once stored */
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

/* nr_pdsch_passive_alloc_normalise() moved to nr_pdsch_prb_set.c (2026-09-27): it is pure (only
 * freq_alloc_bitmap_t + nr_prb_list_normalise(), both already there) and had no unit test because
 * this file pulls in PHY_VARS_NR_UE/NFAPI and can't link into the lightweight test_nr_pdsch_prb_set
 * target. Declared via nr_pdsch_passive_decode.h's include of nr_pdsch_prb_set.h. */

nr_pdsch_passive_decode_status_t nr_pdsch_passive_decode(PHY_VARS_NR_UE *ue,
                                                         const UE_nr_rxtx_proc_t *proc,
                                                         fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                                         const freq_alloc_bitmap_t *freq_alloc,
                                                         const nr_pdsch_passive_grant_t *grant,
                                                         c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                                         nr_pdsch_passive_decode_result_t *out)
{
  memset(out, 0, sizeof(*out));
  out->status = NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED;
  t_last_sk = -1;
  t_last_llr_have = t_last_data_bits = 0;
  t_last_llr = NULL; t_last_G = 0; t_seg_E = 0; /* never read a previous decode's LLRs as this one's */
  t_last_probe_horizon = -1; t_last_chest_hit = 0;
  t_brfo_pending = false; /* the GPU path jumps past the BRANCHFO measurement */

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
   * chest cache/PT-RS/GPU. DCI 1_1, sweepable RNTIs only; ISAC_PRG_SWEEP=0 turns it off. */
  freq_alloc_bitmap_t fa_prg;
  int prg_arm = -1;
  const bool sweepable = rnti_sweepable(grant->rnti, grant->rnti_class);
  /* DCI 1_1 only: TS 38.214 5.1.2.3 fixes the PRG for 1_0 (and SI/RA/P grants carry no RRC config). */
  if (freq_alloc->prg == 0 && t_llr_ovr_n == 0 && grant->dci11 && sweepable && prg_sweep_enabled()) {
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
  /* ---- PT-RS: decodable once G accounts for the REs it steals -------------------------------
   * This used to return unconditionally, which cost the grant entirely -- no TB CRC evidence for
   * Technique D or the DCI-1_1 layout sweep, and no data-aided row. PT-RS removes REs from PDSCH,
   * so the only thing actually needed is the right `unav_res` for nr_get_G().
   * The densities come from ptrs-DensityRecommendationDL (dedicated RRC, invisible here), but the
   * densities it SELECTS are a six-element set -- K in {2,4} x L in {1,2,4}. ISAC_PTRS_K/L pin one
   * for now; sweeping all six against the TB CRC is the agnostic completion and needs air to run.
   * DEFAULT IS UNCHANGED: with neither set, PT-RS grants are still refused, so this cannot regress
   * a run that does not ask for it. */
  uint32_t ptrs_unav = 0;
  int ptrs_arm = -1;
  if (nr_agnostic_v2() && !seg_path && !(dlsch_config->pduBitmap & 0x1) && grant->mcs >= 10 && grant->mcs <= 27
      && t_ptrs_sweep_allow && !t_probe_first_seg && sweepable) {
    ptrs_arm = rnti_ptrs_pick(grant->rnti);
    uint8_t K, L;
    if (nr_ptrs_sweep_arm(ptrs_arm, &K, &L)) {
      dlsch_config->pduBitmap |= 0x1;
      /* OAI's encoding, NOT the literal densities: PTRSFreqDensity is K (2|4) but PTRSTimeDensity is
       * log2(L) -- get_L_ptrs() returns 2/1/0 for L = 4/2/1 and nr_pdsch_ptrs_processing() spaces
       * PT-RS symbols by 1 << PTRSTimeDensity. Writing L literally put L=4 at a 16-symbol spacing. */
      dlsch_config->PTRSFreqDensity = K;
      dlsch_config->PTRSTimeDensity = (L == 4) ? 2 : (L == 2) ? 1 : 0;
      dlsch_config->PTRSPortIndex = 1;
      dlsch_config->PTRSReOffset = 0;
    }
  }
  /* ponytail: PT-RS subcarriers are numbered over the scheduled PRBs in INCREASING PRB order
   * (TS 38.211 7.4.1.2.2), which the virtual data-ordered layout below does not preserve; refused
   * until a segmented grant with PT-RS is actually seen. */
  if (seg_path && (dlsch_config->pduBitmap & 0x1)) {
    { static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_A(PHY, "SENSING: PDSCH UNSUP@seg-ptrs n=%lu\n", n_); }
    prg_arm_unsupported(grant->rnti, prg_arm);
    return out->status;
  }
  if (dlsch_config->pduBitmap & 0x1) {
    /* _Atomic, L stored before K: a consumer that sees K resolved also sees L (N passivePdsch consumers) */
    static _Atomic int s_ptrs_k = -1, s_ptrs_l = -1;
    if (s_ptrs_k < 0) {
      const char *ek = getenv("ISAC_PTRS_K"), *el = getenv("ISAC_PTRS_L");
      s_ptrs_l = (el && *el) ? atoi(el) : 0;
      s_ptrs_k = (ek && *ek) ? atoi(ek) : 0;
    }
    int pk = s_ptrs_k, pl = s_ptrs_l;
    if (ptrs_arm > 0) { uint8_t K = 0, L = 0; nr_ptrs_sweep_arm(ptrs_arm, &K, &L); pk = K; pl = L; } /* the RE count wants literal L */
    if (pk <= 0 || pl <= 0) {
      { static _Atomic unsigned long c_ = 0;
        const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
        if (n_ == 1 || (n_ % 200) == 0)
          LOG_A(PHY, "SENSING: PDSCH UNSUP@1373 n=%lu\n", n_); }
      return out->status; // PT-RS, and no density given to compute G with
    }
    ptrs_unav = nr_pdsch_ptrs_unav_res(freq_alloc->num_rbs, dlsch_config->start_symbol,
                                       dlsch_config->number_symbols, dlsch_config->dlDmrsSymbPos,
                                       (uint8_t)pk, (uint8_t)pl, 1);
    if (ptrs_unav == 0) {
      { static _Atomic unsigned long c_ = 0;
        const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
        if (n_ == 1 || (n_ % 200) == 0)
          LOG_A(PHY, "SENSING: PDSCH UNSUP@1379 n=%lu\n", n_); }
      return out->status; // the density did not describe any PT-RS -- do not guess G
    }
  }
  /* ---- SSB rate matching (TS 38.214 5.1.4): the CURRENT slot's SSB, observed blind (PSS x SSS of
   * the acquired PCI) -- no configured bitmap, no projected period, no SIB1. PSS/SSS need their FFT
   * before G. On a slot-cache hit the whole slot is already transformed; otherwise FEP exactly as the
   * main FEP below does, so a cached slot never holds a differently compensated symbol. */
  const double fep_fo = isnan(nr_slot_fep_fo_override_hz)
      ? (ue->cont_fo_comp ? ue->dl_Doppler_shift + ue->freq_offset : 0.0)
      : nr_slot_fep_fo_override_hz;
  const long share_slot = grant->source_absolute_slot;
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
    for (int sym = 0; sym < 14; ++sym) {
      if (!((fep_syms >> sym) & 1))
        continue;
      if (fp->nb_antennas_rx > 1) {
        for (int ant = 0; ant < fp->nb_antennas_rx; ++ant)
          nr_slot_fep_ant_snapshot(ue, fp, proc->nr_slot_rx, sym, ant, rxdataF, link_type_dl, 0, ue->common_vars.rxdata, fep_fo);
      } else {
        nr_slot_fep(ue, fp, proc->nr_slot_rx, sym, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
      }
    }
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
  const nr_ssb_rm_event_t ssb_event = nr_ssb_rm_observe(fp, proc->frame_rx, proc->nr_slot_rx, ssb_cand, rxdataF);
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
    for (int s = 0; seg_path && s < nseg; s++) {
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
                              ptrs_unav + csi_unav + ssb_unav,
                              cw->qamModOrder, cw->Nl);
  if (G == 0) {
    { static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_A(PHY, "SENSING: PDSCH UNSUP@1475 n=%lu\n", n_); }
    return out->status;
  }
/* TBPARM probe (ISAC_PDSCH_TBPARM=1): every transport-block parameter the gNB also prints on its
   * own PDSCH line, so they can be compared one-for-one instead of inferred from a CRC failure.
   * gNB prints: mcs_index / mod / tbs / tb_size_lbrm / ldpc_base_graph / vrbs=[start..end). */
  {
    static _Atomic int s_tbp = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
    if (s_tbp < 0)
      s_tbp = (getenv("ISAC_PDSCH_TBPARM") != NULL) ? 1 : 0;
    if (s_tbp)
      LOG_I(PHY,
            "SENSING: TBPARM rnti=0x%x nl=%u mcs=%u tbl=%u Qm=%u R=%u tbs=%u G=%u lbrm=%u bg=%u "
            "prb=%u+%u nsym=%u dmrs_len=%u nb_re_dmrs=%u ports=0x%x cdm=%u type=%u pos=0x%x nscid=%u\n",
            grant->rnti, (unsigned)cw->Nl, (unsigned)grant->mcs, (unsigned)grant->mcs_table,
            (unsigned)cw->qamModOrder, (unsigned)cw->targetCodeRate, (unsigned)cw->TBS, G,
            (unsigned)dlsch_config->tbslbrm, (unsigned)cw->ldpcBaseGraph, (unsigned)freq_alloc->first_rb,
            (unsigned)freq_alloc->num_rbs, (unsigned)dlsch_config->number_symbols, (unsigned)dmrs_len,
            (unsigned)nb_re_dmrs, (unsigned)dlsch_config->dmrs_ports, (unsigned)dlsch_config->n_dmrs_cdm_groups,
            (unsigned)dlsch_config->dmrsConfigType, (unsigned)dlsch_config->dlDmrsSymbPos,
            (unsigned)dlsch_config->nscid);
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

  /* ---- PROBE HORIZON. A layout probe decodes code block 0 only, and its E bits sit in the first
   * data symbols of the allocation (rank-4 64QAM full band: E0 = 11.8 kbit against 78 kbit per
   * symbol). Everything past the last symbol it needs -- FEP, channel estimation of later DM-RS
   * symbols, equalisation, LLRs -- is wasted; a probe then costs the LDPC of one segment plus one
   * or two symbols of PHY instead of a whole slot (8 ms at rank 4), which is what lets probing keep
   * up with 1500 grants/s OTA instead of dropping 95 % of the queue. Symbols after the horizon are
   * skipped (their LLR count stays 0, so segment 0's bits are still the first in the buffer); the
   * allocation's last symbol is still visited because nr_rx_pdsch() emits the LLRs there. */
  int probe_last_sym = -1;
  static _Atomic int s_probe_horizon = -1; /* _Atomic (N consumers). ISAC_PROBE_HORIZON=0: probe with the whole slot processed (A/B of the horizon) */
  if (s_probe_horizon < 0) { const char *e = getenv("ISAC_PROBE_HORIZON"); s_probe_horizon = (e && atoi(e) == 0) ? 0 : 1; }
  if (t_probe_first_seg && s_probe_horizon && !ssb_unav && !t_probe_no_horizon) {
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
  t_last_probe_horizon = probe_last_sym;

  /* GPU LLRs in hand: everything from here to the LLR buffer (FEP, channel estimation, equaliser,
   * demodulator) is what the GPU already did for this slot. A PT-RS arm or CSI-RS rate matching
   * armed above changes the RE budget the GPU did not model, so that job stays on the CPU path. */
  const int16_t *gpu_llr = ((dlsch_config->pduBitmap & 0x1) || csi_unav || ssb_unav) ? NULL : t_llr_ovr;
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
  atomic_fetch_add(fep_hit ? &g_fep_hit : &g_fep_miss, 1);
  if (gpu_llr) {
    /* the GPU transformed this slot; nothing here reads rxdataF */
  } else if (fep_hit) {
    /* same slot, same offset: this thread transformed it for the previous grant of the group */
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
                                                  .rxdata = ue->common_vars.rxdata,
                                                  .ans = &fep_ans};
        task_t t = {.func = nr_slot_fep_ant_task, .args = &fep_tasks[ant]};
        pushTpool(&get_nrUE_params()->Tpool, t);
      }
      join_task_ans(&fep_ans);
    }
  } else {
    for (int g = 0; g < n_fep_rng; g++)
      for (int m = fep_rng[g][0]; m < fep_rng[g][0] + fep_rng[g][1]; m++)
        nr_slot_fep(ue, fp, proc->nr_slot_rx, m, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
  }
  if (!fep_hit && !gpu_llr) {
    t_fep_cache.slot = share_slot; t_fep_cache.fo = fep_fo; t_fep_cache.valid = t_share.on;
  }
  pdtim_add(PDTIM_FEP, pdt_fep);

  if (grant->check_sample_lifetime && !nr_passive_samples_valid(
          atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed),
          grant->source_absolute_slot, fp->slots_per_frame)) {
    out->status = NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED;
    { static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_A(PHY, "SENSING: PDSCH UNSUP@1625 n=%lu\n", n_); }
    return out->status; /* overwritten IQ is not CRC evidence */
  }
  for (int i = 0; !gpu_llr && grant->source_absolute_slot >= 0 && i < dlsch_config->numCsiRsForRateMatching; i++) {
    if (dlsch_config->csiRsForRateMatching[i].csi_type != 2)
      continue; // NZP-only slots cost nothing here
    const double zs = nr_pdsch_passive_zp_grant_score(fp, dlsch_config, freq_alloc, &rxdataF[0][0],
                                                      fp->samples_per_slot_wCP, proc->nr_slot_rx, fep_s0, fep_n,
                                                      ssb_event.symbols, grant->scr_dedicated, i);
    if (zs >= 0.0)
      nr_csirs_blind_rt_zp_grant_evidence((uint32_t)grant->source_absolute_slot, &dlsch_config->csiRsForRateMatching[i], zs);
  }

  // ---- Channel estimation on the DM-RS symbols. ----
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
  static __thread fourDimArray_t *toFree = NULL;
  // One estimate per (layer, rx antenna) -- nr_rx_pdsch() indexes this as nl*nb_antennas_rx + aarx,
  // matching nr_ue_pdsch_procedures()'s own allocation. Estimating only layer 0 (as this used to)
  // gives the equaliser nothing to separate the other layers with.
  /* dim1 at the MAXIMUM layer count, not cw->Nl: nr_rx_pdsch() indexes this as nl*nb_antennas_rx +
   * aarx, so an over-allocation is harmless, while sizing it by the runtime layer count would make
   * the shape vary per grant and force a reallocation (and a "resizing" log line) on every change.
   * dim2 is the stride and is already constant. */
  allocCast2D(pdsch_dl_ch_estimates, int32_t, toFree, fp->nb_antennas_rx * NR_MAX_NB_LAYERS, pdsch_est_size, true);
  if (gpu_llr)
    goto gpu_llr_ready; /* after the variably-modified declaration above, which a jump may not cross */

  uint32_t nvar = 0, nvar_den = 1u; // nvar_den: hoisted divisor, also cached by the slot-share
  int n_dmrs_sym = 0;
  /* XANT (ISAC_XANT=1, default off): time-domain cross-correlation of antenna a vs antenna 0 over
   * +/-3000 sample lags, on this slot's samples, once every ~60 s. A peak away from lag 0 is a stream
   * alignment offset between X410 channels -- the one thing every per-branch diagnostic so far
   * (equal DM-RS peak position, 45 % delay-profile compactness, nvar x4000) is consistent with. */
  {
    static _Atomic int s_xant = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
    if (s_xant < 0)
      s_xant = (getenv("ISAC_XANT") != NULL) ? 1 : 0;
    static _Atomic unsigned long s_xant_n = 0;
    if (s_xant && fp->nb_antennas_rx >= 2 && (atomic_fetch_add(&s_xant_n, 1) % 40000) == 0) {
      const c16_t **rx = (const c16_t **)ue->common_vars.rxdata;
      const unsigned total = (unsigned)fp->samples_per_frame;
      const unsigned base = (get_samples_slot_timestamp(fp, proc->nr_slot_rx) + 2u * (unsigned)(fp->ofdm_symbol_size + fp->nb_prefix_samples)) % total;
      const int L = 4096, MAXLAG = 3000;
      char line[200];
      int u = 0;
      for (int a = 1; a < fp->nb_antennas_rx && a < 4; a++) {
        double best = 0.0, e0 = 0.0, ea = 0.0;
        int bestlag = 0;
        for (int lag = -MAXLAG; lag <= MAXLAG; lag += 4) {
          double re = 0.0, im = 0.0;
          for (int n = 0; n < L; n += 2) {
            const c16_t x = rx[0][(base + n) % total];
            const c16_t y = rx[a][(base + n + lag + total) % total];
            re += (double)x.r * y.r + (double)x.i * y.i;
            im += (double)x.i * y.r - (double)x.r * y.i;
          }
          const double m = re * re + im * im;
          if (m > best) { best = m; bestlag = lag; }
        }
        for (int n = 0; n < L; n += 2) {
          const c16_t x = rx[0][(base + n) % total];
          const c16_t y = rx[a][(base + n + bestlag + total) % total];
          e0 += (double)x.r * x.r + (double)x.i * x.i;
          ea += (double)y.r * y.r + (double)y.i * y.i;
        }
        u += snprintf(line + u, sizeof(line) - u, " ant%d: lag=%+d rho=%.2f", a, bestlag,
                      (e0 > 0.0 && ea > 0.0) ? sqrt(best) / sqrt(e0 * ea) : 0.0);
      }
      LOG_A(PHY, "SENSING: XANT slot=%d (vs ant0, +/-%d lags, step 4):%s\n", proc->nr_slot_rx, MAXLAG, line);
    }
  }
  int dmrs_first = -1, dmrs_last = -1; // for the per-branch phase-slope estimator below
  /* SINGLE-BRANCH DECODE (P39): when the demodulator reads one branch (ISAC_RX_MRC_MODE=0 -> branch 0,
   * or a pinned branch), estimate only that branch. The other three were estimated and then never read:
   * 4x the chest cost (~1.2 ms/grant OTA) for nothing. Sensing does not use this estimate -- it forms
   * H_i = Y_i / X from the raw per-antenna FEP output, which is still produced for every antenna. */
  const int chest_only = nr_dlsch_planned_branch(fp->nb_antennas_rx, cw->Nl);
  /* Estimate over the group's union of RBs so the slot's next grant can reuse it. */
  fapi_nr_dl_config_dlsch_pdu_rel15_t chest_cfg = *dlsch_config;
  freq_alloc_bitmap_t chest_alloc = *freq_alloc;
  if (t_share.on && t_share.rb_n > 0) {
    /* Widen UPWARD only: nr_pdsch_channel_estimation() writes dl_ch from INDEX 0 = the first RB of
     * the freq_alloc IT was given, and nr_dlsch_extract_rbs() reads it back from index 0 assuming
     * that is THIS grant's first RB, so the estimate's base RB must be this grant's first_rb
     * (a lower base would hand the equaliser a channel shifted by (first_rb - rb_lo)*12
     * subcarriers). The key below carries rb_lo AND rb_n: grants of one group with the same first
     * RB compute the same union, hence the same key; a grant starting higher re-estimates. */
    const int lo = (int)freq_alloc->first_rb;
    const int hi0 = t_share.rb_lo + t_share.rb_n, hi1 = (int)(freq_alloc->first_rb + freq_alloc->num_rbs);
    const int hi = hi0 > hi1 ? hi0 : hi1;
    chest_alloc = set_bitmap_from_start_size(lo, hi - lo);
    chest_cfg.start_rb = lo; chest_cfg.number_rbs = hi - lo;
  }
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
  const bool chest_cacheable = t_share.on && !seg_path && !t_chest_bypass;
  const int chest_hit = chest_cacheable && t_chest_cache.valid && nr_pdsch_chest_key_eq(&t_chest_cache.key, &chest_key);
  atomic_fetch_add(chest_hit ? &g_chest_hit : &g_chest_miss, 1);
  t_last_chest_hit = chest_hit;
  /* Cached rows: every (layer, antenna) row the estimator writes (one branch when chest_only >= 0),
   * at every DM-RS symbol of the slot. */
  const int chest_nrow = cw->Nl * fp->nb_antennas_rx;
  const int chest_ndmrs = __builtin_popcount(dmrs_full);
  if (chest_hit) {
    /* Working copy of the immutable cached estimate: everything below may mutate it. */
    for (int r = 0; r < chest_nrow; r++) {
      if (chest_only >= 0 && r % fp->nb_antennas_rx != chest_only)
        continue;
      for (int m = 0, k = 0; m < fp->symbols_per_slot; m++) {
        if (!((dmrs_full >> m) & 1))
          continue;
        memcpy(&pdsch_dl_ch_estimates[r][fp->ofdm_symbol_size * m],
               &t_chest_cache.est[((size_t)r * chest_ndmrs + k) * fp->ofdm_symbol_size], sizeof(c16_t) * fp->ofdm_symbol_size);
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
   * ISAC_DC_FIX interpolation is not applied here. */
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
  for (int m = 0; seg_path && m < fp->symbols_per_slot; m++) { /* K32: every DM-RS symbol of the slot */
    if (!((dmrs_full >> m) & 1))
      continue;
    if (dmrs_first < 0)
      dmrs_first = m;
    dmrs_last = m;
    for (int nl = 0; nl < cw->Nl; nl++) {
      uint64_t nv = 0;
      uint64_t nv_ant[NR_DL_CHEST_MAX_ANT] = {0}; /* per-branch, width-weighted: see the publication below */
      for (int s = 0; s < nseg; s++) {
        chest_alloc = set_bitmap_from_start_size(seg[s].prb_start, seg[s].n_prb);
        chest_cfg.start_rb = seg[s].prb_start;
        chest_cfg.number_rbs = seg[s].n_prb;
        chest_cfg.resource_alloc = 1; /* each segment is contiguous */
        uint32_t nvar_tmp = 0;
        nr_dl_chest_diag_request = 1;
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
       * (ISAC_RX_NVAR_PERBRANCH, default on) then feeds the equaliser. Publish the width-weighted mean. */
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
      nr_dl_chest_diag_request = 1; // consumer thread only: BRDELAY/PDP diagnostics
      nr_pdsch_channel_estimation(ue, proc, &chest_cfg, &chest_alloc, nl,
                                  get_dmrs_port(nl, dlsch_config->dmrs_ports), (unsigned char)m, pdsch_est_size,
                                  pdsch_dl_ch_estimates, fp->samples_per_slot_wCP, rxdataF, &nvar_tmp);
      nvar += nvar_tmp;
      /* ---- DC-SUBCARRIER (LO-leakage) INTERPOLATION (ISAC_DC_FIX=1, default off pending live A/B) --
       * MEASURED 2026-09-18, office gNB PCI 2, 273 PRB, reproduced IDENTICALLY across two independent
       * captures minutes apart, two different serving UEs/precoding vectors (pm_index 35 and others):
       * EQDIAG evm_by_freq_bin shows a fixed spike at the diagnostic bin straddling absolute FFT index
       * 0 (evm 282%/111% both times, hrms normal there -- "the estimate is wrong, not the signal"),
       * while every other bin reads 12-16%. FFT index 0 is DC -- classic direct-conversion LO leakage,
       * a RECEIVER hardware property independent of cell/UE/precoding, confirmed by reproducing
       * identically under two different pm_index values. It was never excluded anywhere in this path.
       * For a 273 PRB / BWPStart=0 grant this lands at allocation-relative PRB ~136 -- inside only
       * 1-2 of ~27 code blocks at MCS 25, but a TB needs every block, so it single-handedly zeroes
       * TB success while low-MCS (SEGIDXC: most segment indices already measured at 92-100%, only
       * specific indices near-0%) tolerates it on redundancy/margin alone. Small linear-interpolation
       * window (+/-4 bins around DC) from the nearest unaffected neighbours -- cheap, and every other
       * grant/estimate is untouched (bit-identical when DC is not inside num_rbs*12 relative to this
       * layer's own indexing). Do NOT widen this window without new evidence: the diagnostic's own
       * bin resolution (204 REs) is far coarser than the true affected width. */
      {
        static _Atomic int s_dc_fix = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
        if (s_dc_fix < 0) {
          const char *e = getenv("ISAC_DC_FIX");
          s_dc_fix = (e != NULL && atoi(e) != 0) ? 1 : 0;
        }
        if (s_dc_fix) {
          const int N = fp->ofdm_symbol_size;
          const int W = 4; // +/- bins interpolated; W+1 used as the reference neighbour on each side
          for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++) {
            const int r = nl * fp->nb_antennas_rx + aarx;
            c16_t *h = (c16_t *)&pdsch_dl_ch_estimates[r][N * m];
            const c16_t lo = h[(N - (W + 1)) % N], hi = h[(W + 1) % N];
            for (int d = -W; d <= W; d++) {
              const double t = (double)(d + W + 1) / (double)(2 * (W + 1));
              const int idx = ((d % N) + N) % N;
              h[idx].r = (int16_t)lround((double)lo.r + t * ((double)hi.r - lo.r));
              h[idx].i = (int16_t)lround((double)lo.i + t * ((double)hi.i - lo.i));
            }
          }
        }
      }
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
    const size_t need = (size_t)chest_nrow * chest_ndmrs * fp->ofdm_symbol_size;
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
        for (int m = 0, k = 0; m < fp->symbols_per_slot; m++) {
          if (!((dmrs_full >> m) & 1))
            continue;
          memcpy(&t_chest_cache.est[((size_t)r * chest_ndmrs + k) * fp->ofdm_symbol_size],
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
  /* ---- TIME INTERPOLATION OF THE CHANNEL ESTIMATE (ISAC_CHEST_TINTERP=1, default off) ----------
   * nr_rx_pdsch() equalises each data symbol against the PREVIOUS DM-RS symbol's estimate
   * (get_valid_dmrs_idx_for_channel_est), up to 4 symbols away on this cell (DM-RS at 2/7/11). A
   * residual CFO of ~100 Hz (DMRSFO, measured) rotates the channel by ~5 deg over that gap, and a
   * 5 ppm SFO drifts it by ~10 deg at the band edge: an EVM floor of 7-10 % that MCS 18-19 tolerates
   * and 256QAM at MCS 25 (needs < ~3.5 %) does not. Measured floor in the flat band: 6-7 % against a
   * raw SNR of 33 dB. Linear complex interpolation between the bracketing DM-RS symbols (extrapolation
   * past the last / before the first) removes both to first order, using estimates we already have.
   * Written into the data symbol's own slot of pdsch_dl_ch_estimates; nr_dlsch_chest_per_symbol makes
   * nr_rx_pdsch read that slot. */
  static _Atomic int s_tinterp = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
  if (s_tinterp < 0) {
    const char *e = getenv("ISAC_CHEST_TINTERP");
    s_tinterp = (e != NULL && atoi(e) != 0) ? 1 : 0;
  }
  if (s_tinterp) {
    int dsym[NR_SYMBOLS_PER_SLOT], nd = 0;
    for (int m = 0; m < NR_SYMBOLS_PER_SLOT; m++) /* every DM-RS symbol of the slot is estimated (K32) */
      if ((dmrs_full >> m) & 1)
        dsym[nd++] = m;
    const int s0 = dlsch_config->start_symbol, s1 = s0 + dlsch_config->number_symbols;
    for (int m = s0; m < s1 && nd >= 1; m++) {
      if ((dlsch_config->dlDmrsSymbPos >> m) & 1)
        continue;
      int a, b;
      if (nd == 1) {
        a = b = dsym[0];
      } else {
        int i = 0;
        while (i + 1 < nd - 1 && dsym[i + 1] <= m) // last pair with dsym[i] <= m (or the first pair)
          i++;
        if (dsym[i] > m) i = 0;
        a = dsym[i]; b = dsym[i + 1];
      }
      const double t = (b == a) ? 0.0 : (double)(m - a) / (double)(b - a);
      for (int nl = 0; nl < cw->Nl; nl++) {
        for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++) {
          const int r = nl * fp->nb_antennas_rx + aarx;
          const c16_t *ha = (const c16_t *)&pdsch_dl_ch_estimates[r][fp->ofdm_symbol_size * a];
          const c16_t *hb = (const c16_t *)&pdsch_dl_ch_estimates[r][fp->ofdm_symbol_size * b];
          c16_t *hm = (c16_t *)&pdsch_dl_ch_estimates[r][fp->ofdm_symbol_size * m];
          for (uint32_t k = 0; k < fp->ofdm_symbol_size; k++) {
            const double re = ha[k].r + t * ((double)hb[k].r - ha[k].r);
            const double im = ha[k].i + t * ((double)hb[k].i - ha[k].i);
            hm[k].r = (int16_t)lround(re);
            hm[k].i = (int16_t)lround(im);
          }
        }
      }
    }
    nr_dlsch_chest_per_symbol = 1;
  } else {
    nr_dlsch_chest_per_symbol = 0;
  }
  /* nvar normalisation. ISAC_RX_NVAR_FIX=1 (opt-in, default OFF = bit-identical to before).
   *
   * The loop above accumulates (n_dmrs_sym x Nl) terms, each one already a PER-ANTENNA MEAN of
   * |LS_est - filtered_est|^2 (that per-antenna averaging is 18.3's fix inside
   * nr_pdsch_channel_estimation). So the mean is the sum over (n_dmrs_sym x Nl) -- and the divisor
   * used below is neither of those factors:
   *   - it divides by nb_antennas_rx AGAIN, though the helper already averaged over antennas: 4x;
   *   - it divides by number_symbols (13, the whole allocation) though only n_dmrs_sym (3) terms
   *     were summed: 4.33x.
   * MEASURED 2026-08-25 on run SNR1: the NVAR probe's pre-division value 20245 against CHESTDIAG's
   * post-division 1167 = 17.35x, matching 13/3 exactly. nvar sets the equaliser's confidence, so a
   * 12.4 dB under-estimate makes the LLRs too large and they clip -- a decode failure with a
   * perfectly healthy constellation, which is the signature 17.2b measured (post-equalisation EVM
   * flat at 47-62 % across runs decoding 82.6 % and 0.0 %). Same defect CLASS as 18.3, which took
   * CRC 0 % -> 54-71 % when the antenna part of it was corrected.
   *
   * DEFAULT STAYS OFF because the current form is a DELIBERATE mirror of nr_ue_pdsch_procedures(),
   * which carries the identical arithmetic -- this is upstream OAI behaviour, not a local slip, and
   * the attached path's gates were tuned against it. Flip it only on an alternated >= 5-run-per-arm
   * A/B at comparable offered load (19.3), never on inspection. */
  {
    static _Atomic int s_nvfix = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
    if (s_nvfix < 0) {
      s_nvfix = (getenv("ISAC_RX_NVAR_FIX") != NULL) ? atoi(getenv("ISAC_RX_NVAR_FIX")) : 0;
    }
    const uint32_t den = s_nvfix ? (uint32_t)(n_dmrs_sym * cw->Nl)
                                 : (uint32_t)(dlsch_config->number_symbols * cw->Nl * fp->nb_antennas_rx);
    nvar_den = (den > 0) ? den : 1u;
    nvar /= nvar_den;
  }
  /* ---- Match nvar to the branch actually decoded (2026-09-04) ---------------------------------
   * nvar above is the MEAN over every receive branch, but the rank-1 four-RX path decodes ONE
   * branch. On this rig three of the four are noise, so the mean tells the equaliser the channel is
   * far noisier than the branch it actually reads, and the LLRs clip -- which is why branch 0 scored
   * 51-64 % at 4 antennas but 76 % at --ue-nb-ant-rx 1, decoding the same branch either way.
   * This is the DL counterpart of the UL scale fix in nr_ulsch_demodulation.c.
   *
   * Only applied when the branch is known in advance (mode 0, the default) and that branch actually
   * produced an estimate; otherwise the mean stands, so nothing changes for the attached UE, for
   * mode 1/2/3, or for a single-antenna receiver. ISAC_RX_NVAR_PERBRANCH=0 disables it. */
  {
    static _Atomic int s_pb = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
    if (s_pb < 0) {
      const char *e = getenv("ISAC_RX_NVAR_PERBRANCH");
      s_pb = (e != NULL) ? atoi(e) : 1;
    }
    const int dl_branch = s_pb ? nr_dlsch_planned_branch(fp->nb_antennas_rx, cw->Nl) : -1;
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

  /* ---- PER-BRANCH FREQUENCY-OFFSET ESTIMATE (2026-09-03) --------------------------------------
   * THE measurement that decides why branches 1-3 are undecodable. Selection diversity established
   * that they rescue 0 of 15411 TBs while branch 2 is only 1.7 dB down -- too small a deficit to be
   * a link-budget failure, so their channel must be moving WITHIN the slot. This quantifies that
   * directly, per branch, from data already computed.
   *
   * Estimator: the DM-RS channel estimate at the first and last DM-RS symbol differ, for a static
   * channel, only by the phase a residual frequency offset accumulated between them:
   *     acc   = sum_k H_last[k] . conj(H_first[k])        (noise averages out over subcarriers)
   *     f_res = arg(acc) / (2.pi.dt)
   * Reported as a DIFFERENCE against branch 0, because the part common to all branches is already
   * handled by the shared sync loop -- what breaks a branch is the part that is NOT common.
   *
   * Unambiguous only for |arg| < pi, i.e. |f| < 1/(2.dt): with DM-RS at symbols 2 and 11 at 30 kHz
   * SCS that is ~1557 Hz. A branch beyond that aliases and reads small -- so treat a near-zero
   * value on an otherwise-dead branch with suspicion rather than as proof of coherence.
   *
   * ISAC_RX_BRANCH_FO=1 additionally APPLIES the estimate (nr_ue_set_branch_fo_hz -> the per-branch
   * de-rotation in nr_slot_fep_ant). Default is measure-and-log ONLY: correcting on the strength of
   * an unvalidated estimate is exactly the mistake this file's history is full of. */
  /* ---- DM-RS PHASE-SLOPE CFO/SFO TRACKER (2026-09-07) -----------------------------------------
   * WHY THIS EXISTS. The carrier frequency offset is estimated ONCE at acquisition and never
   * revisited (the CFO trim loop in phy_procedures_nr_ue.c measures but does not apply, and
   * --cont-fo-comp's PI loop is gated on a PBCH decode, so it updates at SSB rate, 20 ms, not per
   * slot). SAMPLING frequency offset is not estimated at ALL anywhere in this receive path -- the
   * only SFO code in the tree is NR_UE_ISAC's cpi_sfo_tracker, and the passive confs run with
   * sensing.enable = 0. Both errors accumulate ACROSS SYMBOLS WITHIN A SLOT, which is the shape of
   * the symptom: PDCCH lives on symbol 0 and decodes near-perfectly, PDSCH spans symbols 1-13 and
   * swings 5-78 % between otherwise identical runs.
   *
   * WHY IT IS ESSENTIALLY FREE HERE. This gNB sends dl_dmrs_symb_pos = 0x884, i.e. DM-RS on symbols
   * 2, 7 and 11 of every grant, and the loop above ALREADY channel-estimates each of them. So the
   * estimate costs one pass over an array that is already in cache, per grant, with ~200k grants a
   * run to average over. No new reference signal, no extra FFT, no PT-RS (which this cell does not
   * configure anyway).
   *
   * THE SEPARATION, which is the whole point of using a SLOPE rather than a mean. Between two
   * pilot-bearing symbols separated by dt:
   *     dphi(k) = arg( H_last[k] . conj(H_first[k]) ) = 2*pi*f_cfo*dt  -  2*pi*k*df*eps*dt
   * CFO is a CONSTANT phase on every subcarrier; SFO is a phase RAMP linear in the subcarrier index
   * k. Fit phase against k and the INTERCEPT is the CFO while the SLOPE is the SFO -- one
   * measurement yields both, and neither can masquerade as the other. (Standard result; see e.g.
   * US7224666B2 / EP1363435A2.)
   *
   * Rather than a full least-squares fit, the ramp is taken from the two halves of the occupied
   * band (negative-k and positive-k about DC) and differenced. One subtraction, no matrix, and the
   * noise averages over ~half the allocation on each side. A full LS fit is the upgrade path if the
   * slope ever needs more precision than this.
   *
   * MEASURE-ONLY BY DEFAULT, and that is not timidity: retuning the radio on an unvalidated
   * estimate killed it 2/2 on this rig, which is why the existing trim loop is two-stage too.
   * ISAC_DMRS_FO_APPLY=1 applies the CFO as a DIGITAL de-rotation through the same per-branch hook
   * nr_slot_fep_ant already uses -- never a hardware retune. SFO is reported only; correcting it
   * needs a per-subcarrier ramp in the equaliser, which is a bigger change than this. */
  if (!seg_path && dmrs_first >= 0 && dmrs_last > dmrs_first) { /* the SFO slope needs a frequency-ordered axis */
    static _Atomic uint64_t s_dfo_n = 0;
    /* SFO is counted separately from CFO: a grant too narrow to fit two slope groups yields a CFO
     * but NO SFO, and the two populations are not the same size (measured 46 % narrow on
     * captures/sfooff_r1_191752). Sharing one counter is what let an unmeasured SFO be folded in
     * as if it were a measurement -- see the gate below. */
    static _Atomic uint64_t s_sfo_n = 0;
    /* The EMA updates are READ-MODIFY-WRITE and this function runs on N consumer threads
     * (nr_pdsch_passive_queue.c starts several). Plain statics raced: a capture showed
     * "cfo=-41.3 Hz (ema +248.2)", an average nowhere near the samples feeding it. The published
     * g_sfo_ppm_ema is an atomic a consumer may read stale; the update itself needs this lock. The
     * critical section is a few flops. */
    static pthread_mutex_t s_dfo_lock = PTHREAD_MUTEX_INITIALIZER;
    static double s_cfo_ema = 0.0, s_sfo_ema = 0.0;
    const double dt_d = (1.0e-3 / (double)fp->slots_per_subframe) / (double)fp->symbols_per_slot
                        * (double)(dmrs_last - dmrs_first);
    const uint32_t N = fp->ofdm_symbol_size;
    const c16_t *h0 = (const c16_t *)&pdsch_dl_ch_estimates[0][N * dmrs_first];
    const c16_t *h1 = (const c16_t *)&pdsch_dl_ch_estimates[0][N * dmrs_last];
    /* Split at the midpoint of the OCCUPIED band, not at DC.
     * MEASURED 2026-09-07: splitting at N/2 put EVERY occupied subcarrier on one side
     * (n_sc = 0/292) and the slope estimator never ran -- SFO read 0.00 ppm in every grant of a
     * whole capture, which looks exactly like "no SFO" rather than like a broken estimator. The
     * grant lives in one contiguous stretch of the FFT array, so a DC split is not guaranteed to
     * divide it at all.
     * The ORIGIN of k does not matter for the slope: shifting k by k0 leaves d(phi)/dk unchanged
     * and moves only the intercept, by 2*pi*k0*df*eps*dt, which at these eps is far below the CFO
     * term it lands in. Any two well-separated groups of subcarriers give the same SFO. */
    uint32_t kmin = N, kmax = 0;
    for (uint32_t k = 0; k < N; k++) {
      if (h0[k].r != 0 || h0[k].i != 0 || h1[k].r != 0 || h1[k].i != 0) {
        if (k < kmin) kmin = k;
        if (k > kmax) kmax = k;
      }
    }
    const uint32_t kmid = (kmin <= kmax) ? (kmin + kmax) / 2 : 0;
    double re[2] = {0.0, 0.0}, im[2] = {0.0, 0.0}, ksum[2] = {0.0, 0.0}, magsum[2] = {0.0, 0.0};
    uint32_t kn[2] = {0, 0};
    for (uint32_t k = 0; k < N; k++) {
      const double rr = (double)h1[k].r * h0[k].r + (double)h1[k].i * h0[k].i;
      const double ii = (double)h1[k].i * h0[k].r - (double)h1[k].r * h0[k].i;
      if (rr == 0.0 && ii == 0.0) {
        continue;  // unallocated subcarrier
      }
      const int h = (k <= kmid) ? 0 : 1;
      re[h] += rr;
      im[h] += ii;
      ksum[h] += (double)k;
      /* |H1|.|H0| per subcarrier: the magnitude the coherent sum WOULD reach if every subcarrier
       * agreed in phase. Its ratio to |sum| is the validity gate below. */
      magsum[h] += sqrt(((double)h1[k].r * h1[k].r + (double)h1[k].i * h1[k].i)
                        * ((double)h0[k].r * h0[k].r + (double)h0[k].i * h0[k].i));
      kn[h]++;
    }
    /* ---- VALIDITY GATE ---------------------------------------------------------------------
     * COHERENCE, not energy. |sum(H1.conj(H0))| / sum(|H1||H0|) is 1 when every subcarrier reports
     * the same phase difference (a real, common CFO/SFO) and falls to ~1/sqrt(n) when the phases
     * are random, which is what noise looks like. It is dimensionless and self-normalising, so it
     * needs no reference level and no per-rig calibration -- the same reason this project prefers
     * dimensionless gates elsewhere.
     * MEASURED 2026-09-07: on a DEAF capture (4 TBs decoded, EVM 70 %, no signal on air) the
     * ungated estimator published cfo = -224 Hz and sfo = -12.24 ppm from pure noise, against
     * +40 Hz / +2.2 ppm on healthy runs. With the correction stage enabled that garbage would have
     * been applied to every later grant in the run. An estimator that cannot say "I do not know"
     * is a liability once anything consumes it. */
    const double coh_num = sqrt((re[0] + re[1]) * (re[0] + re[1]) + (im[0] + im[1]) * (im[0] + im[1]));
    const double coh_den = magsum[0] + magsum[1];
    const double coherence = (coh_den > 0.0) ? (coh_num / coh_den) : 0.0;
    /* ---- THE COMBINED COHERENCE MEASURES THE SFO, NOT THE NOISE -----------------------------
     * A real SFO makes the phase difference ramp LINEARLY across k, so a coherent sum over the
     * whole occupied band partially cancels: the value is |sinc(PHI/2)| where PHI is the total
     * ramp, 2*pi*df*eps*dt*K. At this geometry (df=30 kHz, dt=321 us, K=3276) PHI is 198486*eps,
     * so the combined coherence is a monotone function of the SFO:
     *     2.4 ppm -> 0.99    4.2 ppm -> 0.97    12 ppm -> 0.78    24 ppm -> 0.29
     * MEASURED on this rig: coh=0.97 at a reported 4.2 ppm -- the sinc prediction to two decimals.
     * The noise floor at n=3276 is 1/sqrt(n) = 0.017, so the 0.30 threshold sits 17x above noise
     * and is nowhere near it: what actually trips the gate is a LARGE REAL SFO, from ~24 ppm up.
     * That is backwards -- the estimator refuses exactly when the impairment it exists to measure
     * is worst, and reports "withheld" rather than "large". A wide-grant rejection observed at
     * coh=0.077 (n_sc=1628/1628) is consistent with ~30 ppm, not with a dead channel.
     *
     * PER-HALF coherence is the right gate. Each half spans half the band, so it suffers only
     * sinc(PHI/4) -- at 24 ppm that is 0.78 rather than 0.29 -- and it validates precisely the two
     * phases the slope is built from, which the combined figure never did (one half could be pure
     * noise while a strong other half carried the sum past the threshold). */
    const double coh_h[2] = {
        (magsum[0] > 0.0) ? sqrt(re[0] * re[0] + im[0] * im[0]) / magsum[0] : 0.0,
        (magsum[1] > 0.0) ? sqrt(re[1] * re[1] + im[1] * im[1]) / magsum[1] : 0.0};
    const double coh_min = (coh_h[0] < coh_h[1]) ? coh_h[0] : coh_h[1];
    /* Fall back to the combined figure when a half is too thin for its own coherence to mean
     * anything (1/sqrt(n) rises fast at small n, and a spurious rejection is as bad as a spurious
     * accept). 8 keeps the per-half floor at ~0.35 worst case. */
    const bool halves_usable = (kn[0] >= 8 && kn[1] >= 8);
    const double coh_gate = halves_usable ? coh_min : coherence;
    /* 0.3 is far above the ~1/sqrt(n) a random-phase population reaches at these n (n >= 28 gives
     * ~0.19, and the real populations here run several hundred), and far below the ~0.9+ a genuine
     * common rotation produces. Anything in between is not trustworthy enough to steer a
     * correction with. */
    static _Atomic uint64_t s_dfo_rej = 0;
    if (kn[0] + kn[1] > 0 && coh_gate < 0.30) {
      const uint64_t nrej = atomic_fetch_add(&s_dfo_rej, 1);
      if ((nrej % 500) == 0) {
        /* Print BOTH: a low combined with healthy halves is a large SFO, not a dead channel, and
         * the two were previously indistinguishable in this line. */
        LOG_I(PHY, "SENSING: DMRSFO REJECTED n=%lu coh=%.3f (halves %.3f/%.3f, gate %.3f < 0.30) "
                   "n_sc=%u/%u -- estimate withheld, not published\n",
              (unsigned long)nrej + 1, coherence, coh_h[0], coh_h[1], coh_gate, kn[0], kn[1]);
      }
    }
    if (kn[0] + kn[1] > 0 && coh_gate >= 0.30) {
      const double cfo_hz = atan2(im[0] + im[1], re[0] + re[1]) / (2.0 * M_PI * dt_d);
      double sfo_ppm = 0.0, sfo_dk = 0.0;
      bool sfo_measured = false;
      if (kn[0] > 16 && kn[1] > 16 && coh_h[0] >= 0.30 && coh_h[1] >= 0.30) {
        const double p_lo = atan2(im[0], re[0]), p_hi = atan2(im[1], re[1]);
        const double k_lo = ksum[0] / (double)kn[0], k_hi = ksum[1] / (double)kn[1];
        double dphi = p_hi - p_lo;
        while (dphi > M_PI)  dphi -= 2.0 * M_PI;   // the ramp must not alias across the branch cut
        while (dphi < -M_PI) dphi += 2.0 * M_PI;
        const double dk = k_hi - k_lo;
        if (dk != 0.0) {
          /* dphi/dk = -2*pi*df*eps*dt  ->  eps = -(dphi/dk) / (2*pi*df*dt) */
          const double df = (double)fp->subcarrier_spacing;
          sfo_ppm = -(dphi / dk) / (2.0 * M_PI * df * dt_d) * 1.0e6;
          sfo_dk = (dk < 0.0) ? -dk : dk;
          sfo_measured = true;
        }
      }
      const uint64_t dn = atomic_fetch_add(&s_dfo_n, 1);
      /* A grant with fewer than two usable slope groups produced NO SFO estimate. Folding the
       * initialiser 0.0 into the average records "unmeasurable" as "zero", which is not a neutral
       * default -- it drags the published value toward zero in proportion to how many narrow
       * grants the cell schedules. MEASURED on captures/sfooff_r1_191752: 49 of 107 DMRSFO samples
       * (46 %) had kn <= 16 and every one of them published sfo=+0.00, so the reported ema of
       * +4.33 ppm was averaged with ~46 % fabricated zeros and UNDERSTATES the true SFO. The
       * correction stage consumes exactly this value. Same distinction the CSI-RS correlator makes
       * between "cannot score" and "scored zero". */
      double sfo_pub;
      pthread_mutex_lock(&s_dfo_lock);
      s_cfo_ema = (dn == 0) ? cfo_hz : (0.99 * s_cfo_ema + 0.01 * cfo_hz);
      if (sfo_measured) {
        /* ---- WEIGHT BY PRECISION, NOT EQUALLY -------------------------------------------------
         * The slope is recovered from the phase difference between two subcarrier groups whose
         * centres are dk apart, so its variance goes as 1/dk^2. dk is set by the GRANT's
         * allocation, and this cell schedules everything from a couple of resource blocks to the
         * full carrier -- a factor of ~30 in dk, so a factor of ~1000 in variance.
         *
         * Equal weighting therefore let a narrow grant, whose own unambiguous range is
         * +/-1000 ppm and which resolves nothing at the few-ppm scale being measured, move the
         * average as much as a full-band grant. MEASURED: the published EMA reached -45.65 ppm
         * while instantaneous full-band estimates read +4.3 ppm -- outside the +/-31.6 ppm
         * unambiguous range of the very grants that dominate the population, which is impossible
         * for an average of valid measurements and is what exposed this.
         *
         * Inverse-variance weighting is the standard answer and needs no threshold: the step is
         * scaled by (dk/dk_ref)^2, unity for a full-band grant. A narrow grant still contributes,
         * in proportion to the information it actually carries. */
        const double dk_ref = (double)fp->N_RB_DL * 12.0 / 2.0;  /* full-band group separation */
        double rel = (dk_ref > 0.0) ? (sfo_dk / dk_ref) : 0.0;
        rel = rel * rel;
        if (rel > 1.0) {
          rel = 1.0;   /* a wider-than-reference separation is not MORE than fully informative */
        }
        const double a = 0.01 * rel;
        const uint64_t sn = atomic_fetch_add(&s_sfo_n, 1);
        /* Seed from the first FULL-WEIGHT sample rather than the first sample of any width: a
         * narrow first grant would otherwise set the starting point to a near-meaningless value
         * that the weighted updates then take a long time to walk away from. */
        if (sn == 0 || (s_sfo_ema == 0.0 && rel > 0.5)) {
          s_sfo_ema = sfo_ppm;
        } else {
          s_sfo_ema = (1.0 - a) * s_sfo_ema + a * sfo_ppm;
        }
        atomic_store_explicit(&g_sfo_ppm_ema, s_sfo_ema, memory_order_relaxed); // published for the SFO correction stage
      }
      sfo_pub = s_sfo_ema;
      const double cfo_pub = s_cfo_ema;
      pthread_mutex_unlock(&s_dfo_lock);
      static _Atomic int s_apply = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
      if (s_apply < 0) {
        const char *e = getenv("ISAC_DMRS_FO_APPLY");
        s_apply = (e != NULL && atoi(e) != 0) ? 1 : 0;
      }
      {
        if (s_apply) {
          /* Digital de-rotation only, applied to every branch in common. NOT nrue_ru_set_freq(). */
          for (int a = 0; a < fp->nb_antennas_rx && a < NR_DL_CHEST_MAX_ANT; a++) {
            nr_ue_set_branch_fo_hz(a, -cfo_pub);
          }
        }
      }
      if ((dn % 500) == 0) {
        char sfo_txt[64];
        if (sfo_measured) {
          /* The slope is recovered from a phase difference wrapped to (-pi, pi], so the SFO is
           * unambiguous only up to |eps| = 1 / (2 * df * dt * dk). Beyond that it ALIASES and
           * reports a small value -- indistinguishable from a clean clock. The CFO line has always
           * printed its own limit; the SFO never did, which matters now that the published figure
           * is suspected of understating the truth. */
          const double dk_lo = ksum[0] / (double)kn[0], dk_hi = ksum[1] / (double)kn[1];
          const double dk_abs = (dk_hi > dk_lo) ? (dk_hi - dk_lo) : (dk_lo - dk_hi);
          const double eps_max = (dk_abs > 0.0)
                                     ? 1.0e6 / (2.0 * (double)fp->subcarrier_spacing * dt_d * dk_abs)
                                     : 0.0;
          snprintf(sfo_txt, sizeof(sfo_txt), "%+.2f ppm (unamb +/-%.1f)", sfo_ppm, eps_max);
        } else {
          /* NOT "0.00": this grant was too narrow to fit two slope groups. Printing a zero here is
           * what made a broken estimator look like a quiet channel. */
          snprintf(sfo_txt, sizeof(sfo_txt), "n/a (kn<=16)");
        }
        LOG_I(PHY,
              "SENSING: DMRSFO cfo=%+.1f Hz (ema %+.1f) sfo=%s (ema %+.2f over %lu) "
              "sym %d->%d n_sc=%u/%u coh=%.2f (halves %.2f/%.2f) dk=%.0f unambiguous=+/-%.0f Hz apply=%d\n",
              cfo_hz, cfo_pub, sfo_txt, sfo_pub, (unsigned long)atomic_load(&s_sfo_n),
              dmrs_first, dmrs_last, kn[0], kn[1], coherence, coh_h[0], coh_h[1], sfo_dk,
              1.0 / (2.0 * dt_d), s_apply);
      }
    }
  }

  /* BRANCHFO: measured here, integrated only once this grant's TB CRC passes (see brfo_commit):
   * probes, unsettled layout hypotheses and false-accept DCIs give random slopes that would
   * random-walk the correction to its clamp. */
  t_brfo_pending = false;
  if (fp->nb_antennas_rx > 1 && dmrs_first >= 0 && dmrs_last <= dmrs_first) {
    static _Atomic int s_inert_logged = 0;
    if (!atomic_exchange(&s_inert_logged, 1))
      LOG_W(PHY, "SENSING: BRANCHFO inert: grant has one DM-RS symbol, no per-branch slope to measure\n");
  }
  if (fp->nb_antennas_rx > 1 && chest_only < 0 && dmrs_first >= 0 && dmrs_last > dmrs_first) {
    static _Atomic uint64_t s_fo_n = 0;
    static double s_fo_ema[NR_DL_CHEST_MAX_ANT];
    // symbol duration incl. CP: one slot is 1ms/slots_per_subframe, split into symbols_per_slot
    const double dt = (1.0e-3 / (double)fp->slots_per_subframe) / (double)fp->symbols_per_slot
                      * (double)(dmrs_last - dmrs_first);
    double fo[NR_DL_CHEST_MAX_ANT] = {0};
    for (int a = 0; a < fp->nb_antennas_rx && a < NR_DL_CHEST_MAX_ANT; a++) {
      const c16_t *h0 = (const c16_t *)&pdsch_dl_ch_estimates[a][fp->ofdm_symbol_size * dmrs_first];
      const c16_t *h1 = (const c16_t *)&pdsch_dl_ch_estimates[a][fp->ofdm_symbol_size * dmrs_last];
      double re = 0.0, im = 0.0;
      for (uint32_t k = 0; k < fp->ofdm_symbol_size; k++) {
        // H_last * conj(H_first); unallocated subcarriers are zero and contribute nothing
        re += (double)h1[k].r * h0[k].r + (double)h1[k].i * h0[k].i;
        im += (double)h1[k].i * h0[k].r - (double)h1[k].r * h0[k].i;
      }
      fo[a] = (re != 0.0 || im != 0.0) ? atan2(im, re) / (2.0 * M_PI * dt) : 0.0;
    }
    const uint64_t n = atomic_fetch_add(&s_fo_n, 1);
    pthread_mutex_lock(&s_brfo_lock);
    for (int a = 0; a < fp->nb_antennas_rx && a < NR_DL_CHEST_MAX_ANT; a++) {
      // Differential against branch 0: the common part is the shared sync loop's job, not ours.
      const double d = fo[a] - fo[0];
      s_fo_ema[a] = (n == 0) ? d : (0.99 * s_fo_ema[a] + 0.01 * d);
      t_brfo_d[a] = d;
    }
    pthread_mutex_unlock(&s_brfo_lock);
    t_brfo_nant = fp->nb_antennas_rx;
    t_brfo_pending = true;
    if ((n % 500) == 0) {
      LOG_I(PHY,
            "SENSING: BRANCHFO d_vs_br0=[%.1f %.1f %.1f %.1f] Hz (EMA, DM-RS sym %d->%d, "
            "unambiguous to +/-%.0f Hz)\n",
            s_fo_ema[0], fp->nb_antennas_rx > 1 ? s_fo_ema[1] : 0.0,
            fp->nb_antennas_rx > 2 ? s_fo_ema[2] : 0.0, fp->nb_antennas_rx > 3 ? s_fo_ema[3] : 0.0,
            dmrs_first, dmrs_last, 1.0 / (2.0 * dt));
    }
  }

  /* CHESTDIAG (ISAC_PDSCH_TBPARM=1): per-(layer,antenna) channel power, plus the layer-space Gram
   * matrix conditioning. This is the one remaining hypothesis for the rank-4 CRC failure that has
   * been asserted but never measured: separating Nl spatial streams requires the Nl x nbRx effective
   * channel to be well conditioned AT THIS RECEIVER, and the gNB chose its precoder from the SERVED
   * UE's PMI, not ours. If the layer columns are near-parallel here the MMSE inverse amplifies noise
   * without bound and no parameter fix can help; if they are well separated, the fault is in the
   * demodulation chain instead. Cheap: one pass over the already-computed estimates. */
  {
    static _Atomic int s_cd = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
    if (s_cd < 0) // ISAC_CHEST_DIAG=1 gives the 12 CHESTDIAG lines without TBPARM's per-TB volume
      s_cd = (getenv("ISAC_PDSCH_TBPARM") != NULL || getenv("ISAC_CHEST_DIAG") != NULL) ? 1 : 0;
    static _Atomic int s_cd_left = 40; /* log budget shared by the passivePdsch consumers */
    if (s_cd && s_cd_left > 0 && cw->Nl >= 1) {
      s_cd_left--;
      const int nsc = freq_alloc->num_rbs * NR_NB_SC_PER_RB;
      /* Estimates are written at ch_offset = ofdm_symbol_size * <DM-RS symbol>, NOT at
       * start_symbol -- reading start_symbol returns an untouched buffer (all zeros). */
      int sym = -1;
      for (int m2 = 0; m2 < NR_SYMBOLS_PER_SLOT; m2++) {
        if (dlsch_config->dlDmrsSymbPos & (1u << m2)) {
          sym = m2;
          break;
        }
      }
      if (sym < 0)
        sym = dlsch_config->start_symbol;
      char rep[1024];
      int u = 0;
      rep[0] = '\0';
      double pw[8][8];
      for (int l = 0; l < cw->Nl && l < 8; l++) {
        for (int a = 0; a < fp->nb_antennas_rx && a < 8; a++) {
          const c16_t *h = (const c16_t *)&pdsch_dl_ch_estimates[l * fp->nb_antennas_rx + a][fp->ofdm_symbol_size * sym];
          double acc = 0;
          for (int k = 0; k < nsc; k++)
            acc += (double)h[k].r * h[k].r + (double)h[k].i * h[k].i;
          pw[l][a] = acc / (nsc > 0 ? nsc : 1);
        }
      }
      /* Fill `rep`. It was declared, printed with %s and NEVER WRITTEN -- undefined behaviour on
       * every CHESTDIAG line, and it discarded the per-(layer,antenna) powers computed just above.
       * That is PASSIVE_RX_ONLY_HANDOVER.md 12.7's bug, reintroduced by the 13 revert (812f6b4af6).
       * Restored because it is the only per-BRANCH PDSCH SNR instrument in the tree, and both the
       * branch-imbalance work and the "can this link carry mcs 25" question need it.
       *
       * Two things to know before reading the numbers:
       *  - the dB are 10log10(pw / nvar) against the nvar THIS function has already divided by
       *    (number_symbols x Nl x nb_antennas_rx), mirroring nr_ue_pdsch_procedures(). The blind
       *    monitor's own SNR gate uses the RAW nvar, so the two are on different scales -- treat this
       *    as this path's scale, not as a calibrated link SNR;
       *  - that division is common-mode across antennas, so the BRANCH-TO-BRANCH deltas are exact
       *    whatever the convention, and those are the quantity the imbalance work actually needs.
       * Use ANTPOW for the raw antenna powers (RXBRANCH pw[] is a channel ESTIMATE and swings 10 dB
       * between transport blocks -- 20/21). */
      for (int l = 0; l < cw->Nl && l < 8; l++) {
        rep_append(rep, sizeof(rep), &u, "%spw=L%d[", l ? " " : "", l);
        for (int a = 0; a < fp->nb_antennas_rx && a < 8; a++) {
          rep_append(rep, sizeof(rep), &u, "%s%.0f", a ? "," : "", pw[l][a]);
        }
        rep_append(rep, sizeof(rep), &u, "] rel_dB=L%d[", l);
        for (int a = 0; a < fp->nb_antennas_rx && a < 8; a++) {
          /* Against the SHARED noise, deliberately. The per-antenna value exists (printed as
           * resid= below) but it is NOT a noise power and must not be used as an SNR denominator:
           * nr_dl_channel_estimation.c forms it as |dl_ls_est - dl_ch|^2, the residual between the
           * RAW LS estimate and the FILTERED one, which is noise PLUS filter mismatch. On a
           * frequency-selective branch the mismatch term scales with |H|, so the ratio collapses
           * towards a constant -- measured pw/resid = 2.3 on three branches at once while raw
           * power said those branches differed, which is the signature of a denominator tracking
           * its own numerator.
           * A shared denominator at least makes the RELATIVE dB between branches exact, which is
           * what this line is used for. It is NOT an absolute per-branch SNR and is not labelled
           * as one. For judging a single antenna, use raw ANTPOW power -- no estimator involved. */
          rep_append(rep, sizeof(rep), &u, "%s%.1f", a ? "," : "",
                     (nvar > 0 && pw[l][a] > 0.0) ? 10.0 * log10(pw[l][a] / (double)nvar) : -99.0);
        }
        /* resid, not nvar: |LS - filtered|^2 per antenna. Reported because the SPREAD across
         * branches is diagnostic (a branch whose estimate the filter cannot fit stands out), but it
         * is not a noise power -- see the snr_dB comment above. */
        rep_append(rep, sizeof(rep), &u, "] resid=L%d[", l);
        for (int a = 0; a < fp->nb_antennas_rx && a < 8; a++) {
          rep_append(rep, sizeof(rep), &u, "%s%u", a ? "," : "",
                     (a < NR_DL_CHEST_MAX_ANT) ? nr_dl_chest_nvar_ant[a] : 0u);
        }
        rep_append(rep, sizeof(rep), &u, "]");
      }
      /* MIMO separability, done properly (2026-08-20). The previous statistic summed the layer
       * inner product over antennas AND subcarriers with POWER weighting, so a single dominant
       * branch (ch0 measured 11-17 dB above the others) made it return ~1 mechanically, whatever
       * the true spatial structure was. It could not distinguish "layers are parallel" from
       * "only one antenna is alive".
       *
       * Correct measure: per-subcarrier Gram matrix over LAYERS, G = H^H H with H[antenna][layer],
       * after normalising each ANTENNA branch by its own RMS so per-chain gain cannot bias the
       * geometry. Then the normalised determinant det(G) / prod(G_ii) in [0,1]:
       *   1  -> layers mutually orthogonal, full rank, 4 streams separable
       *   0  -> layers collinear, rank deficient, streams NOT separable
       * Computed by Cholesky (G is Hermitian positive semi-definite), which also yields the
       * per-layer residual L_ii^2 / G_ii: how much NEW information each layer adds beyond the
       * previous ones. That is the honest "effective rank" read-out. */
      double bn[8];
      for (int a = 0; a < fp->nb_antennas_rx && a < 8; a++) {
        double acc = 0;
        for (int l = 0; l < cw->Nl && l < 8; l++)
          acc += pw[l][a];
        bn[a] = (acc > 0) ? 1.0 / sqrt(acc) : 0.0; // per-branch gain normalisation
      }
      const int NL = (cw->Nl < 4) ? cw->Nl : 4;
      double gr[4][4] = {{0}}, gi[4][4] = {{0}};
      for (int x = 0; x < NL; x++) {
        for (int y = 0; y < NL; y++) {
          double ar = 0, ai = 0;
          for (int a = 0; a < fp->nb_antennas_rx && a < 8; a++) {
            const c16_t *hx = (const c16_t *)&pdsch_dl_ch_estimates[x * fp->nb_antennas_rx + a][fp->ofdm_symbol_size * sym];
            const c16_t *hy = (const c16_t *)&pdsch_dl_ch_estimates[y * fp->nb_antennas_rx + a][fp->ofdm_symbol_size * sym];
            const double w = bn[a] * bn[a];
            for (int k = 0; k < nsc; k++) {
              ar += w * ((double)hx[k].r * hy[k].r + (double)hx[k].i * hy[k].i);
              ai += w * ((double)hx[k].r * hy[k].i - (double)hx[k].i * hy[k].r);
            }
          }
          gr[x][y] = ar;
          gi[x][y] = ai;
        }
      }
      /* Cholesky of the Hermitian Gram; lr/li hold L. */
      double lr[4][4] = {{0}}, li[4][4] = {{0}}, resid[4] = {0};
      int pd = 1;
      for (int x = 0; x < NL && pd; x++) {
        for (int y = 0; y <= x && pd; y++) {
          double sr = gr[x][y], si = gi[x][y];
          for (int m2 = 0; m2 < y; m2++) {
            sr -= lr[x][m2] * lr[y][m2] + li[x][m2] * li[y][m2];
            si -= li[x][m2] * lr[y][m2] - lr[x][m2] * li[y][m2];
          }
          if (x == y) {
            if (sr <= 0) { pd = 0; break; }
            lr[x][x] = sqrt(sr);
            li[x][x] = 0;
            resid[x] = (gr[x][x] > 0) ? sr / gr[x][x] : 0.0; // fraction of layer x NOT explained by earlier layers
          } else {
            const double d = lr[y][y];
            lr[x][y] = sr / d;
            li[x][y] = si / d;
          }
        }
      }
      double detg = 1.0, prod_diag = 1.0;
      for (int x = 0; x < NL; x++) {
        detg *= pd ? (lr[x][x] * lr[x][x]) : 0.0;
        prod_diag *= gr[x][x];
      }
      const double orth = (prod_diag > 0) ? detg / prod_diag : 0.0;
      LOG_I(PHY,
            "SENSING: CHESTDIAG nl=%u nvar=%u orth=%.4f resid=[%.3f,%.3f,%.3f,%.3f] %s\n",
            (unsigned)cw->Nl, nvar, orth, resid[0], resid[1], resid[2], resid[3], rep);
    }
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
  NR_UE_DLSCH_t dlsch = {0};
  dlsch.cw_info = *cw;
  dlsch.rnti = grant->rnti;
  dlsch.rnti_type = TYPE_C_RNTI_;
  dlsch.active = true;
  dlsch.max_ldpc_iterations = 8;

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
  static __thread int16_t  *llr     = NULL;
  static __thread uint32_t  llr_cap = 0;
  if (llr_cap < rx_llr_buf_sz) {
    free(llr);
    llr = (int16_t *)malloc16_clear(rx_llr_buf_sz * sizeof(int16_t));
    llr_cap = llr ? rx_llr_buf_sz : 0;
  } else {
    memset(llr, 0, (size_t)rx_llr_buf_sz * sizeof(int16_t));
  }
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

  static __thread fourDimArray_t *toFree2 = NULL;
  allocCast3D(rxdataF_comp, c16_t, toFree2, fp->symbols_per_slot, NR_MAX_NB_LAYERS, rx_size_symbol, true);
  static __thread fourDimArray_t *toFree3 = NULL;
  allocCast3D(dl_ch_mag, c16_t, toFree3, NR_SYMBOLS_PER_SLOT, NR_MAX_NB_LAYERS, rx_size_symbol, true);
  static __thread fourDimArray_t *toFree4 = NULL;
  allocCast3D(dl_ch_magb, c16_t, toFree4, NR_SYMBOLS_PER_SLOT, NR_MAX_NB_LAYERS, rx_size_symbol, true);
  static __thread fourDimArray_t *toFree5 = NULL;
  allocCast3D(dl_ch_magr, c16_t, toFree5, NR_SYMBOLS_PER_SLOT, NR_MAX_NB_LAYERS, rx_size_symbol, true);

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

  /* ---- PDSCH BRANCH-QUALITY GATE (ISAC_PDSCH_ANT_GATE=1, default OFF) --------------------------
   * 2026-09-23, ADDED BUT NOT LIVE-VALIDATED (no hardware available overnight to A/B it). Mirrors
   * dci_nr.c's `rough[a]` PDCCH gate exactly: nr_rx_pdsch() below MMSE/MRC-combines every rx antenna
   * UNCONDITIONALLY (no such gate exists on this path today), which is exactly the configuration
   * [[four-antenna-fep-chest-blows-rt-budget]] measured failing on THIS receiver's own X410 -- two
   * branches physically 8-15 dB down (bad RX1 cabling on channels 1/3, confirmed with rx_gain/config
   * identical across channels) took 4-antenna PDSCH from 76-93% (1 ant) to ~0%, because MRC weights a
   * noise-dominated branch's channel estimate as signal and ADDS noise rather than combining gain.
   * Same metric as PDCCH: roughness = sum of |consecutive-RE channel estimate difference|^2 over
   * total power, on the FIRST DM-RS symbol (dmrs_first) since that is what every later data symbol's
   * equaliser reference is drawn from (get_valid_dmrs_idx_for_channel_est()); a branch whose roughness
   * exceeds 4x the best branch is memset to zero for ALL its symbols before nr_rx_pdsch() ever reads
   * it, exactly like the PDCCH gate zeroes pdcch_dl_ch_estimates_ext[a]. A zeroed channel estimate
   * contributes zero MRC weight, so the bad branch is excluded rather than merely down-weighted.
   * Default OFF: the PDCCH gate above is the validated pattern this ports, but ported code is not
   * measured code -- turn this on and re-run the branch-imbalance A/B ([[antpow-branch-shape-is-not-stable]]
   * warns the imbalance SHAPE itself is not stable run to run) before trusting a live PDSCH number
   * at NANT>1. */
  if (fp->nb_antennas_rx > 1 && dmrs_first >= 0) {
    static _Atomic int s_pdsch_ant_gate = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
    if (s_pdsch_ant_gate < 0)
      s_pdsch_ant_gate = (getenv("ISAC_PDSCH_ANT_GATE") != NULL && atoi(getenv("ISAC_PDSCH_ANT_GATE")) != 0) ? 1 : 0;
    if (s_pdsch_ant_gate) {
      double rough[8];
      double best_r = 1e30;
      const int nrx = fp->nb_antennas_rx > 8 ? 8 : fp->nb_antennas_rx; /* rough[] bound, matches dci_nr.c's <=8 */
      for (int a = 0; a < nrx; a++) {
        const c16_t *h = (const c16_t *)&pdsch_dl_ch_estimates[a][fp->ofdm_symbol_size * dmrs_first];
        double dp = 0.0, pw = 0.0;
        for (uint32_t i = 1; i < fp->ofdm_symbol_size; i++) {
          const double dr = (double)h[i].r - h[i - 1].r, di = (double)h[i].i - h[i - 1].i;
          dp += dr * dr + di * di;
          pw += (double)h[i].r * h[i].r + (double)h[i].i * h[i].i;
        }
        rough[a] = pw > 0.0 ? dp / pw : 1e30;
        if (rough[a] < best_r)
          best_r = rough[a];
      }
      int kept = 0;
      for (int a = 0; a < nrx; a++) {
        if (rough[a] > 4.0 * best_r) {
          for (uint32_t d = 0; d < (uint32_t)fp->symbols_per_slot; d++)
            memset((c16_t *)&pdsch_dl_ch_estimates[a][fp->ofdm_symbol_size * d], 0, sizeof(c16_t) * fp->ofdm_symbol_size);
        } else {
          kept |= 1 << a;
        }
      }
      static _Atomic int s_gate_log = 8; /* log budget shared by the passivePdsch consumers */
      if (s_gate_log > 0 && kept != (1 << nrx) - 1) {
        s_gate_log--;
        LOG_W(PHY, "SENSING: PDSCH branch gate kept=0x%x rough=[%.2f %.2f %.2f %.2f]\n", kept, rough[0],
              nrx > 1 ? rough[1] : -1.0, nrx > 2 ? rough[2] : -1.0, nrx > 3 ? rough[3] : -1.0);
      }
    }
  }


  /* ---- RESIDUAL SFO CORRECTION (ISAC_SFO_CORRECT=1, default off) ------------------------------
   * WHAT IS UNCORRECTED. nr_rx_pdsch() equalises EVERY data symbol against ONE DM-RS symbol's
   * estimate -- dl_ch_estimates[..][validDmrsEst * ofdm_symbol_size], chosen by
   * get_valid_dmrs_idx_for_channel_est() -- with no interpolation in time. A sampling-clock offset
   * eps makes the timing drift by eps*dt between that reference symbol and the data symbol, and a
   * timing shift tau is a phase ramp exp(-j*2*pi*k*df*tau) across subcarriers. So after
   * equalisation the residual is exactly
   *     exp(-j*2*pi*k*df*eps*(t_m - t_ref))
   * and nothing in this receive path removes it. The DMRSFO tracker above measures eps directly
   * (~2.4 ppm on this rig), which over symbols 2->11 is ~13 degrees at the band edge -- small for
   * QPSK, not small for the 256QAM this gNB actually schedules.
   *
   * HOW. Rotate the channel estimate rather than the data: the estimate is this decode's own
   * thread-local buffer (allocCast2D above), whereas rxdataF is shared with every other consumer
   * and must not be touched. Rotating H by the SAME phase the data drifted makes Y/H land back on
   * X. Each DM-RS slot carries its own applied-rotation accumulator, so a slot is always brought to
   * exactly (m - d) symbol periods however the reference switches between symbols -- setting the
   * absolute rotation, never blindly stepping, which would drift out of phase with the reference.
   *
   * SECOND-ORDER, and stated as such: the dominant defect on this receiver is LLR clipping at the
   * int8 rail (see LLRCLIP), which destroys information outright. This only stops a real but
   * smaller error accumulating across a slot. Opt-in until an A/B shows it earns its place. */
  static _Atomic int s_sfo_corr = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
  if (s_sfo_corr < 0) {
    const char *e = getenv("ISAC_SFO_CORRECT");
    s_sfo_corr = (e != NULL && atoi(e) != 0) ? 1 : 0;
  }
  double sfo_applied[NR_SYMBOLS_PER_SLOT] = {0};  // symbol-periods of rotation already applied, per slot
  const double sfo_eps = (s_sfo_corr && !nr_dlsch_chest_per_symbol && !seg_path) ? (nr_pdsch_passive_sfo_ppm() * 1.0e-6) : 0.0;
  const double sfo_tsym = (1.0e-3 / (double)fp->slots_per_subframe) / (double)fp->symbols_per_slot;

  /* ---- DATA-ORDERED GATHER (seg_path only). nr_rx_pdsch() walks its allocation in increasing PRB
   * order (nr_dlsch_extract_rbs), which is not data order for interleaved VRBs, and it rereads the
   * estimate from index 0 for every bitmap block. So it is handed a VIRTUAL contiguous allocation at
   * BWP PRBs 0..n-1 whose subcarriers are the segments' REs gathered in data order, against the
   * data-ordered estimate built above. DM-RS positions within a PRB are the same in every PRB, so
   * moving whole PRBs keeps them. The caller's rxdataF is left untouched: the data-aided submit
   * forms Y/X on the REAL subcarriers. */
  freq_alloc_bitmap_t fa_virt;
  c16_t(*rxdataF_dem)[fp->samples_per_slot_wCP] = rxdataF;
  if (seg_path && !gpu_llr) {
    static __thread c16_t *virt = NULL;
    static __thread size_t virt_cap = 0;
    const size_t need = (size_t)fp->nb_antennas_rx * fp->samples_per_slot_wCP;
    if (virt_cap < need) {
      free(virt);
      virt = (c16_t *)malloc16(need * sizeof(c16_t));
      virt_cap = virt ? need : 0;
    }
    static __thread int *gidx = NULL; /* heap, not a 13 kB frame on every decode's stack */
    if (gidx == NULL)
      gidx = (int *)malloc(NR_PRB_SET_MAX * NR_NB_SC_PER_RB * sizeof(int));
    const int nre = (virt && gidx) ? nr_prb_gather_index(seg, nseg, NR_NB_SC_PER_RB, gidx, NR_PRB_SET_MAX * NR_NB_SC_PER_RB) : -1;
    if (nre != freq_alloc->num_rbs * NR_NB_SC_PER_RB) { /* not nsc_seg: the GPU goto skips its initialiser */
      out->status = NR_PDSCH_PASSIVE_DECODE_ERROR;
      return out->status;
    }
    const int N = fp->ofdm_symbol_size, off0 = fp->first_carrier_offset + dlsch_config->BWPStart * NR_NB_SC_PER_RB;
    for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++)
      for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
        const c16_t *src = &rxdataF[aarx][m * N];
        c16_t *dst = &virt[(size_t)aarx * fp->samples_per_slot_wCP + (size_t)m * N];
        for (int i = 0; i < nre; i++)
          dst[(off0 + i) % N] = src[(off0 + gidx[i]) % N];
      }
    fa_virt = set_bitmap_from_start_size(0, freq_alloc->num_rbs);
    rxdataF_dem = (c16_t(*)[fp->samples_per_slot_wCP])virt;
  }

  const uint64_t pdt_dem = pdtim_on ? pdtim_now() : 0;
  bool demod_ok = true;
  const int last_sym = dlsch_config->start_symbol + dlsch_config->number_symbols - 1;
  if (gpu_llr) {
    const uint32_t n = gpu_llr_n < rx_llr_buf_sz ? gpu_llr_n : rx_llr_buf_sz;
    memcpy(llr, gpu_llr, (size_t)n * sizeof(int16_t)); /* the rest stays 0: a probe only needs code block 0 */
    for (int m = dlsch_config->start_symbol; m <= last_sym; m++)
      dl_valid_re[m] = 0;
  }
  for (int m = dlsch_config->start_symbol; m <= last_sym && !gpu_llr; m++) {
    if (probe_last_sym >= 0 && m > probe_last_sym && m != last_sym)
      continue; /* probe: past the horizon, LLR count stays 0 for this symbol */
    if (sfo_eps != 0.0) {
      /* Bring every DM-RS slot to the rotation this symbol needs. Cheap: at most 3 slots on this
       * cell, and only the ones that actually differ are touched. */
      for (int d = 0; d < fp->symbols_per_slot && d < NR_SYMBOLS_PER_SLOT; d++) {
        if (!((dlsch_config->dlDmrsSymbPos >> d) & 1)) {
          continue;
        }
        const double want = (double)(m - d);
        const double delta = want - sfo_applied[d];
        if (delta == 0.0) {
          continue;
        }
        sfo_applied[d] = want;
        const double c = -2.0 * M_PI * (double)fp->subcarrier_spacing * sfo_eps * delta * sfo_tsym;
        for (int r = 0; r < fp->nb_antennas_rx * NR_MAX_NB_LAYERS; r++) {
          c16_t *h = (c16_t *)&pdsch_dl_ch_estimates[r][fp->ofdm_symbol_size * d];
          for (uint32_t k = 0; k < fp->ofdm_symbol_size; k++) {
            if (h[k].r == 0 && h[k].i == 0) {
              continue;
            }
            /* The ramp is on the SIGNED subcarrier index: FFT bins >= N/2 are negative
             * frequencies. Raw k would put an extra 2*pi*fs*tau on that half of the band
             * (~0.2 turns at 5 ppm over 9 symbols), splitting the band at DC. */
            const long ks = (k < fp->ofdm_symbol_size / 2) ? (long)k : (long)k - (long)fp->ofdm_symbol_size;
            const double ph = c * (double)ks;
            const double cs = cos(ph), sn = sin(ph);
            const double hr = (double)h[k].r, hi = (double)h[k].i;
            h[k].r = (int16_t)lround(hr * cs - hi * sn);
            h[k].i = (int16_t)lround(hr * sn + hi * cs);
          }
        }
      }
    }
    if (nr_rx_pdsch(ue, proc, &dlsch, seg_path ? &fa_virt : freq_alloc, dlsch_config, &harq, (unsigned char)m,
                    m == first_symbol_with_data, (unsigned char)dlsch_config->harq_process_nbr, pdsch_est_size,
                    pdsch_dl_ch_estimates, llr, dl_valid_re, rxdataF_dem, &log2_maxh, rx_size_symbol,
                    fp->nb_antennas_rx, rxdataF_comp, dl_ch_mag, dl_ch_magb, dl_ch_magr, ptrs_phase_per_slot,
                    ptrs_re_per_slot, nvar, &scope_req, NULL /* rho_dl: single layer */,
                    ssb_unav ? &ssb.dem : &no_ssb)
        < 0) {
      demod_ok = false;
      break;
    }
  }

  pdtim_add(PDTIM_DEMOD, pdt_dem);

  /* ---- RE-BUDGET INVARIANT (CPU path, whole allocation demodulated). G above is what rate
   * de-matching will read; the demodulator must have produced exactly that many LLRs. A mismatch --
   * an SSB/CSI-RS/PT-RS/DM-RS RE model in nr_rx_pdsch() that disagrees with the one G was computed
   * from -- shifts every LLR after the first disagreement, so the TB cannot be CRC evidence of
   * anything: fail closed instead of feeding the sweeps a misleading CRC failure. */
  if (demod_ok && !gpu_llr && probe_last_sym < 0) {
    uint64_t llr_n = 0;
    for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++)
      llr_n += (uint64_t)dl_valid_re[m] * cw->qamModOrder * cw->Nl;
    if (llr_n != G) {
      static _Atomic unsigned long c_ = 0;
      const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
      if (n_ == 1 || (n_ % 200) == 0)
        LOG_W(PHY, "SENSING: PDSCH RE-budget mismatch n=%lu rnti=0x%x: demodulated %lu LLRs, G %u (ssb_unav %u csi_unav %u "
                   "ptrs_unav %u)\n", n_, grant->rnti, (unsigned long)llr_n, G, ssb_unav, csi_unav, ptrs_unav);
      prg_arm_unsupported(grant->rnti, prg_arm);
      out->status = NR_PDSCH_PASSIVE_DECODE_ERROR;
      return out->status;
    }
  }

  /* Qm oracle: same symbol choice as EQDIAG -- the one with the most valid data REs. */
  if (demod_ok) {
    int qm_m = -1;
    uint32_t qm_n = 0;
    for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++)
      if (dl_valid_re[m] > qm_n) { qm_n = dl_valid_re[m]; qm_m = m; }
    if (qm_m >= 0)
      out->qm_measured = (uint8_t)nr_pdsch_qm_classify((const int16_t *)rxdataF_comp[qm_m][0],
                                                       qm_n > 4096 ? 4096 : qm_n, NULL);
  }

  /* ---- EQDIAG: post-equalisation EVM (ISAC_PDSCH_EVM=1, default off) --------------------------
   * PASSIVE_RX_ONLY_HANDOVER.md §13 names this as "the measurement to take next, and why it was not
   * taken": the receiver goes bimodally 0 % / ~90 % PDSCH CRC across otherwise identical runs and
   * nothing measured so far separates "the signal reaching the equaliser is degraded" from "the
   * signal is fine and something downstream is wrong". EVM answers exactly that and nothing else:
   *   ~30 %  -> the constellation is as good as the 90.7 % runs (§12.1); the fault is DOWNSTREAM
   *             (descrambling, rate recovery, LDPC, TBS) and no amount of gain or geometry helps.
   *   >>40 % -> the signal itself is short; the fault is the LINK (gain, beam, channel, rank).
   * The §12.7 version of this probe was removed by the §13 revert; this reinstates it.
   *
   * Measured on rxdataF_comp, which is the EQUALISED symbol stream, and against the ideal QAM grid
   * for this grant's own modulation order -- normalised by the measured RMS, so the arbitrary
   * log2_maxh fixed-point scaling cancels and the number is comparable across runs and MCSs. */
  {
    static _Atomic int s_evm = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
    if (s_evm < 0)
      s_evm = (getenv("ISAC_PDSCH_EVM") != NULL) ? 1 : 0;
    static __thread unsigned long s_evm_n = 0;
    /* ISAC_PDSCH_EVM=2: EVM per (symbol, layer) -- the axis the single-symbol probe below cannot see.
     * Written for the rank-4 bed where 5-symbol grants decode and 12/13-symbol ones do not. */
    if (s_evm && getenv("ISAC_PDSCH_EVM")[0] == '2' && demod_ok && (s_evm_n % 10) == 0) {
      const int lmax = (1 << (cw->qamModOrder / 2)) - 1;
      double ms = 0.0; int nlev = 0;
      for (int l = 1; l <= lmax; l += 2) { ms += (double)l * l; nlev++; }
      const double ideal_pow = 2.0 * ms / nlev;
      char tb[1024]; int ut = 0;
      for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols && ut < 900; m++) {
        const uint32_t n = dl_valid_re[m] > 2048 ? 2048 : dl_valid_re[m];
        ut += snprintf(tb + ut, sizeof(tb) - ut, " s%d:", m);
        for (int l = 0; l < cw->Nl; l++) {
          if (n < 64) { ut += snprintf(tb + ut, sizeof(tb) - ut, "-/"); continue; }
          const c16_t *z = rxdataF_comp[m][l];
          double pw = 0.0;
          for (uint32_t i = 0; i < n; i++) pw += (double)z[i].r * z[i].r + (double)z[i].i * z[i].i;
          pw /= n;
          const double scale = pw > 0 ? sqrt(ideal_pow / pw) : 0.0;
          double err = 0.0;
          for (uint32_t i = 0; i < n; i++) {
            const double vi = z[i].r * scale, vq = z[i].i * scale;
            double si = 2.0 * floor(vi / 2.0) + 1.0, sq = 2.0 * floor(vq / 2.0) + 1.0;
            si = si > lmax ? lmax : si < -lmax ? -lmax : si;
            sq = sq > lmax ? lmax : sq < -lmax ? -lmax : sq;
            err += (vi - si) * (vi - si) + (vq - sq) * (vq - sq);
          }
          /* scale check: rms of the equalised samples against layer 0's mag threshold (the LLR
           * kernels use dl_ch_mag[m][0] for EVERY layer) -- a per-layer ratio away from the others
           * means the LLR thresholds are wrong for that layer even though the EVM is clean */
          const double mag0 = (double)dl_ch_mag[m][0][n / 2].r;
          ut += snprintf(tb + ut, sizeof(tb) - ut, "%.0f(%.2f)/", sqrt(err / n / ideal_pow) * 100.0,
                         mag0 > 0 ? sqrt(pw) / mag0 : -1.0);
        }
      }
      LOG_I(PHY, "SENSING: EQDIAG2 %d.%d rnti=0x%04x Qm=%u Nl=%u nsym=%u first_rb=%u nrb=%u ptrs_arm=%d bitmap=%u csirm=%u evm%%[sym:layer]%s\n", proc->frame_rx, proc->nr_slot_rx, grant->rnti, cw->qamModOrder, cw->Nl,
            dlsch_config->number_symbols, (unsigned)freq_alloc->first_rb, (unsigned)freq_alloc->num_rbs, ptrs_arm, (unsigned)dlsch_config->pduBitmap, (unsigned)dlsch_config->numCsiRsForRateMatching, tb);
    }
    if (s_evm && demod_ok && (s_evm_n++ % 200) == 0) {
      /* Sample the symbol carrying the MOST valid data REs: the last symbol of an allocation is
       * often DM-RS with none, and scoring a near-empty symbol reports noise as signal. */
      int best_m = -1;
      uint32_t best_n = 0;
      for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
        if (dl_valid_re[m] > best_n) {
          best_n = dl_valid_re[m];
          best_m = m;
        }
      }
      if (best_m >= 0 && best_n >= 64) {
        const uint32_t n = (best_n > 4096) ? 4096 : best_n;
        const c16_t *z = rxdataF_comp[best_m][0];
        double p = 0.0;
        for (uint32_t i = 0; i < n; i++) {
          p += (double)z[i].r * z[i].r + (double)z[i].i * z[i].i;
        }
        p /= (double)n;
        /* Ideal per-dimension levels are the odd integers +-1..+-(2^(Qm/2)-1); mean square of those
         * is the constellation's per-dimension power, so 2x it is the total. */
        const int lmax = (1 << (cw->qamModOrder / 2)) - 1; // 1 (QPSK), 3 (16QAM), 7 (64QAM), 15 (256QAM)
        double ms = 0.0;
        int nlev = 0;
        for (int l = 1; l <= lmax; l += 2) {
          ms += (double)l * l;
          nlev++;
        }
        ms /= (double)nlev;
        const double ideal_pow = 2.0 * ms;
        const double scale = (p > 0.0) ? sqrt(ideal_pow / p) : 0.0;
        /* Split by FREQUENCY POSITION within the allocation. rxdataF_comp holds this symbol's valid
         * data REs in increasing-subcarrier order, so quartile q covers the q-th quarter of the
         * allocated band. This is the one axis SEGIDX cannot see: rate matching + interleaving
         * smear frequency across every segment index, which is exactly why SEGIDX reads uniform.
         *   EVM FLAT across quartiles -> the estimate is equally good/bad everywhere; the loss is
         *                                broadband (link margin, or a fault common to the TB).
         *   EVM RISING with quartile   -> the estimate degrades with distance from its reference;
         *                                that is an indexing / reference-point error, and it also
         *                                explains why narrow grants decode and wide ones do not.
         * One shared scale for all four, so the quartiles are directly comparable. */
#define EQDIAG_NBIN 16
        double errsum = 0.0, errq[EQDIAG_NBIN] = {0.0};
        uint32_t nq[EQDIAG_NBIN] = {0};
        for (uint32_t i = 0; i < n; i++) {
          const double vi = (double)z[i].r * scale;
          const double vq = (double)z[i].i * scale;
          /* Slice to the nearest ODD integer, clamped to the constellation edge. */
          double si = 2.0 * floor(vi / 2.0) + 1.0;
          double sq = 2.0 * floor(vq / 2.0) + 1.0;
          if (si > lmax) si = lmax;
          if (si < -lmax) si = -lmax;
          if (sq > lmax) sq = lmax;
          if (sq < -lmax) sq = -lmax;
          const double e = (vi - si) * (vi - si) + (vq - sq) * (vq - sq);
          errsum += e;
          uint32_t q = (uint32_t)(((uint64_t)i * (uint64_t)EQDIAG_NBIN) / n);
          if (q >= EQDIAG_NBIN)
            q = EQDIAG_NBIN - 1;
          errq[q] += e;
          nq[q]++;
        }
        const double evm = sqrt((errsum / (double)n) / ideal_pow) * 100.0;
        /* Absolute FFT bin of allocation index 0, so a bad bin can be named in the receiver's own
         * frequency frame rather than in units of my bin width. The allocation wraps the FFT, so
         * index i sits at (start_re + i) % ofdm_symbol_size. */
        const int start_rb_abs = freq_alloc->first_rb + dlsch_config->BWPStart;
        const int start_re_abs = seg_path ? -1 /* bins are DATA order: no single FFT origin */
            : (fp->first_carrier_offset + start_rb_abs * NR_NB_SC_PER_RB) % fp->ofdm_symbol_size;
        char eb[768];
        int ub = 0;
        for (int q = 0; q < EQDIAG_NBIN && ub < (int)sizeof(eb) - 24; q++) {
          const double v = nq[q] ? sqrt((errq[q] / (double)nq[q]) / ideal_pow) * 100.0 : -1.0;
          ub += snprintf(eb + ub, sizeof(eb) - ub, "%s%.0f", q ? " " : "", v);
        }
        /* |H| over the SAME frequency bins. EVM alone cannot tell "the estimate is wrong here" from
         * "there is no signal here": both raise it. |H| separates them --
         *   |H| collapses where EVM spikes -> no signal / notch / the estimate found nothing there
         *   |H| flat while EVM spikes      -> signal present and the estimate is simply WRONG there
         * nr_pdsch_channel_estimation() writes dl_ch from index 0 relative to the allocation, at
         * ch_offset = ofdm_symbol_size * <DM-RS symbol> (verified by reading its writers), so index
         * i below is allocation subcarrier i -- the same axis the EVM bins use. */
        char hb[768];
        int uh = 0;
        {
          int dsym = -1;
          for (int m2 = 0; m2 < NR_SYMBOLS_PER_SLOT; m2++) {
            if (dlsch_config->dlDmrsSymbPos & (1u << m2)) {
              dsym = m2;
              break;
            }
          }
          const int nsc = freq_alloc->num_rbs * NR_NB_SC_PER_RB;
          if (dsym >= 0 && nsc >= EQDIAG_NBIN) {
            const c16_t *H = (const c16_t *)&pdsch_dl_ch_estimates[0][fp->ofdm_symbol_size * dsym];
            for (int q = 0; q < EQDIAG_NBIN && uh < (int)sizeof(hb) - 24; q++) {
              const int i0 = (int)(((long)q * nsc) / EQDIAG_NBIN);
              const int i1 = (int)(((long)(q + 1) * nsc) / EQDIAG_NBIN);
              double acc = 0.0;
              for (int i = i0; i < i1; i++)
                acc += (double)H[i].r * H[i].r + (double)H[i].i * H[i].i;
              const double rms = (i1 > i0) ? sqrt(acc / (double)(i1 - i0)) : 0.0;
              uh += snprintf(hb + uh, sizeof(hb) - uh, "%s%.0f", q ? " " : "", rms);
            }
            /* Per-RB raw |H| for the first 20 RBs of the allocation, keyed by ABSOLUTE start RB, so a
             * narrow and a wide grant covering the same RBs can be compared estimate-to-estimate. */
            uh += snprintf(hb + uh, sizeof(hb) - uh, "] start_rb=%d hrb[", start_rb_abs);
            for (int rb = 0; rb < 20 && rb < freq_alloc->num_rbs && uh < (int)sizeof(hb) - 24; rb++) {
              double acc = 0.0;
              for (int i = rb * NR_NB_SC_PER_RB; i < (rb + 1) * NR_NB_SC_PER_RB; i++)
                acc += (double)H[i].r * H[i].r + (double)H[i].i * H[i].i;
              uh += snprintf(hb + uh, sizeof(hb) - uh, "%s%.0f", rb ? " " : "", sqrt(acc / NR_NB_SC_PER_RB));
            }
          }
        }
        LOG_I(NR_PHY,
              "SENSING: EQDIAG rnti=0x%x Qm=%u sym=%d n=%u prb=%u start_re=%d nbin=%d evm=%.1f%% | "
              "evm_by_freq_bin[%s] hrms_by_freq_bin[%s] (each bin = %u REs starting at FFT bin "
              "(start_re + bin*%u) mod %u; EVM spike with |H| flat = the estimate is wrong there, "
              "EVM spike with |H| collapsed = no signal there)\n",
              grant->rnti, (unsigned)cw->qamModOrder, best_m, n, (unsigned)freq_alloc->num_rbs,
              start_re_abs, EQDIAG_NBIN, evm, eb, uh ? hb : "n/a", n / EQDIAG_NBIN,
              n / EQDIAG_NBIN, (unsigned)fp->ofdm_symbol_size);
      }
    }
  }

  if (demod_ok) {
    /* Measured BEFORE unscrambling: descrambling only flips signs, so magnitudes are identical
     * either side of it and taking them here keeps this independent of whether the scrambling
     * sequence is the suspect. */
    uint64_t llr_n = 0, llr_absum = 0, llr_zero = 0, llr_sat = 0, llr_pos = 0, llr_clip8 = 0;
    int64_t  llr_sgnsum = 0;
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

    /* ---- LLR RESCALE TO THE INT8 RAIL (ISAC_LLR_SCALE=<target mean |llr|>, 0/unset = off) -----
     * The decoder's own input stage saturates at +-127. With a measured mean |llr| of 232-498,
     * most of the distribution is clipped to a hard decision before belief propagation starts,
     * which is why healthy-magnitude LLRs fail to converge and why the failure rate tracks the
     * per-run LLR SCALE rather than the channel (EVM is normalised by measured RMS, so it is blind
     * to scale by construction and reads a clean 10 % on runs that decode 26 %).
     *
     * Rescale by the ratio of a target mean to the measured mean of THIS transport block, so the
     * correction follows whatever the per-run scale happens to be instead of assuming it. A target
     * well below the rail leaves headroom for the tail; the mean is a robust statistic here because
     * the distribution has no heavy tail once it is not clipped.
     *
     * NOT the root fix. The scale is wrong because nvar is wrong (see ISAC_RX_NVAR_FIX and this
     * project's own "nvar ~4x too small" finding); this only stops the wrongness reaching a rail
     * where information is destroyed rather than merely mis-weighted. Opt-in so the default path
     * stays bit-identical until an A/B says otherwise. */
    {
      static _Atomic int s_llr_scale = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
      if (s_llr_scale < 0) {
        const char *e = getenv("ISAC_LLR_SCALE");
        s_llr_scale = (e != NULL) ? atoi(e) : 0;
      }
      if (s_llr_scale > 0 && llr_n > 0 && llr_absum > 0) {
        const double mean_abs = (double)llr_absum / (double)llr_n;
        const double f = (double)s_llr_scale / mean_abs;
        if (f < 0.999 || f > 1.001) {
          for (uint32_t i = 0; i < G; i++) {
            int v = (int)lround((double)llr[i] * f);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            llr[i] = (int16_t)v;
          }
        }
      }
    }

    const uint64_t zero_before = atomic_load(&g_ldpc_zero_tb);
    const uint64_t pdt_ldp = pdtim_on ? pdtim_now() : 0;
    nr_dlsch_unscrambling(llr, G, 0 /* codeword */, dlsch_config->dlDataScramblingId, grant->rnti);
    t_last_llr = llr; t_last_G = G; /* for the GPU self-check */

    /* SIGN statistics AFTER descrambling -- measuring them before is meaningless, and that was the
     * first version's mistake: the scrambler exists to randomise signs, so an all-zero transport
     * block scrambled gives balanced +/- by construction (measured: pos = 49.5 % / 49.0 %, which
     * proves only that the scrambler works). Post-descramble, a genuinely all-zero TB shows a strong
     * POSITIVE bias (OAI's convention: LLR > 0 favours bit 0), and a TB whose descrambling is wrong
     * stays balanced. That is the discriminator for the zero_tb population. */
    uint64_t posbit[2] = {0, 0}, nbit[2] = {0, 0};
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
    /* ---- LLR SCALE NORMALISATION before the int8 decoder ----------------------------------------
     * The decoder saturates every LLR to +-127 (simde_mm_packs_epi16). The demodulators' output
     * scale is NOT controlled: the fixed-point 4-layer MMSE scales by det(G) and gave mean |LLR|
     * 454 with 87 % clipped on the rank-4 bed (417 / 22 % OTA), the float path 79 / 20 %, a rank-2
     * decode 125 / 0.015 %. Min-sum is scale-invariant except for that clipping, so a uniform
     * right shift that brings the mean under LLR_NORM_TARGET costs nothing where the scale was
     * already right and keeps the soft information where it was not. ISAC_LLR_NORM=0 disables. */
    {
      static _Atomic int s_norm = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
      if (s_norm < 0) { const char *e = getenv("ISAC_LLR_NORM"); s_norm = (e && atoi(e) == 0) ? 0 : 1; }
      if (s_norm && G >= 64) {
        uint64_t acc = 0; uint32_t cnt = 0;
        for (uint32_t i = 0; i < G; i += 16) { acc += (uint32_t)abs(llr[i]); cnt++; }
        const uint32_t mean = (uint32_t)(acc / cnt);
        int k = 0;
        while (k < 8 && (mean >> k) > LLR_NORM_TARGET) k++;
        if (k > 0) {
          for (uint32_t i = 0; i < G; i++) llr[i] = (int16_t)(llr[i] >> k);
          atomic_fetch_add(&g_llr_norm_shift[k], 1);
        } else {
          atomic_fetch_add(&g_llr_norm_shift[0], 1);
        }
      }
    }
    if (nr_agnostic_v2() && atomic_load(&g_ldpc_ok) >= 100) { /* only once the layout has bootstrapped */
      t_hq.armed = 1;
      t_hq.rnti = grant->rnti;
      t_hq.pid = grant->harq_pid;
      t_hq.ndi = grant->ndi;
    }
    bool ldpc_ok = passive_ldpc_decode(ue, proc, &g_harq, cw, dlsch_config, llr, freq_alloc->num_rbs, G);
    t_hq.armed = 0;
    /* A probe's outcome is code block 0's CRC, and every hypothesis sweep below (PT-RS density,
     * the PDSCH config feedback in the queue, the layout search) must see it: with ldpc_ok false
     * for every probe the PT-RS sweep could never latch "absent" and only 1 arm in 7 was right --
     * exactly the 57/400 measured with ISAC_PROBE_ALL on a pinned conf that decodes 97 %. The TB
     * itself is still never reported decoded (the queue maps a probe onto CRC status only). */
    const bool probe_ok = t_probe_first_seg && t_probe_seg_ok;
    if (probe_ok)
      ldpc_ok = true;
    /* ISAC_RV_RETRY=1 (default off): on a failed TB, re-run ONLY the LDPC stage with rv 2, 3, 1 on
     * the same (already descrambled) LLRs. Tests the hypothesis that some grants are retransmissions
     * whose RV field the current DCI-1_1 layout misreads as 0 (MCS-24 grants: 0 % of code blocks
     * decode on 3.5k TBs while MCS 25 gets 49 % -- a lower code rate cannot do that on SNR alone).
     * A rescue at rv=2 is proof; the counters are printed with MCSHIST. */
    {
      static _Atomic int s_rvr = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
      if (s_rvr < 0) {
        const char *e = getenv("ISAC_RV_RETRY");
        s_rvr = (e != NULL && atoi(e) != 0) ? 1 : 0;
      }
      if (s_rvr && !ldpc_ok) {
        static const uint8_t rvs[3] = {2, 3, 1};
        const uint8_t rv0 = cw->rv;
        for (int i = 0; i < 3 && !ldpc_ok; i++) {
          cw->rv = rvs[i];
          atomic_fetch_add(&g_rv_try[rvs[i]][grant->mcs & 31], 1);
          if (passive_ldpc_decode(ue, proc, &g_harq, cw, dlsch_config, llr, freq_alloc->num_rbs, G)) {
            ldpc_ok = true;
            atomic_fetch_add(&g_rv_ok[rvs[i]][grant->mcs & 31], 1);
          }
        }
        if (!ldpc_ok)
          cw->rv = rv0;
      }
    }
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
          if (rnti_nl_latch(grant->rnti, sweepable, nl_h) != nl_h)
            LOG_A(PHY, "SENSING: LBRM layer term n_L=%d latched from the TB CRC for rnti 0x%04x (was %d): TBS_LBRM=%u C=%u E=%u N_ref=%u\n",
                  nl_h, grant->rnti, nl_now, lbrm_h, t_seg_C, E_first, nref_h);
        } else {
          dlsch_config->tbslbrm = tbs_now;
        }
      }
    }
    pdtim_add(PDTIM_LDPC, pdt_ldp);

    if (ptrs_arm >= 0) {
      const int latched = rnti_ptrs_feed(grant->rnti, ptrs_arm, ldpc_ok);
      if (latched >= 0 && atomic_exchange(&g_ptrs_arm_last, latched) != latched) {
        uint8_t K = 0, L = 0;
        const bool any = nr_ptrs_sweep_arm(latched, &K, &L);
        LOG_A(PHY, "SENSING: PTRS_SWEEP LATCHED arm=%d (%s K=%u L=%u) from the TB CRC for rnti 0x%04x\n", latched,
              any ? "PT-RS present," : "no PT-RS", K, L, grant->rnti);
      }
    }
    /* DCI 1_1 interleaved VRB-to-PRB bundle-size sweep (grant->vrb_l == 0 for a non-interleaved or
     * DCI 1_0 grant -- nothing to feed back, see the field comment). */
    if (grant->vrb_l == 2 || grant->vrb_l == 4) {
      const int latched = rnti_vrbl_feed(grant->rnti, grant->vrb_l == 4 ? 1 : 0, ldpc_ok);
      if (latched >= 0)
        LOG_A(PHY, "SENSING: VRB_IL rnti=0x%x L=%u latched\n", grant->rnti, latched == 0 ? 2 : 4);
    }
    /* PRB-bundling (PRG) hypothesis feedback (prg_arm < 0 when nothing was swept for this decode --
     * the caller had already set an explicit freq_alloc->prg, or a GPU-assisted decode -- see the
     * pick site above). */
    if (prg_arm >= 0) {
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
        /* llr_have: index one past the last NON-ZERO LLR. An exactly-zero LLR is possible but
         * vanishingly rare in real soft output, so the last nonzero is a good proxy for how far
         * nr_rx_pdsch() actually filled the buffer -- and a shortfall against G is the thing being
         * hunted. Scanned backwards so a full buffer costs one comparison. */
        uint32_t llr_have = 0;
        for (int i = (int)G - 1; i >= 0; i--) {
          if (llr[i] != 0) { llr_have = (uint32_t)i + 1; break; }
        }
        t_last_llr_have = llr_have;
        /* LLRFILL (2026-09-16): per-symbol non-zero LLR counts of the assembled buffer, one shot when
         * llr_have < 0.9 G -- which symbols/layers come out empty (rank-4 bed: llr_have 3 % of G). */
        static _Atomic int s_llrfill_left = 3;
        if (cw->Nl > 1 && (llr_have != G || getenv("ISAC_LLRFILL_ALL")) && atomic_load(&s_llrfill_left) > 0) {
          atomic_fetch_sub(&s_llrfill_left, 1);
          char b[400]; int u = 0; uint32_t k = 0;
          for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols && u < 360; m++) {
            const uint32_t blk = dl_valid_re[m] * cw->qamModOrder * cw->Nl;
            uint32_t nz = 0, nzl[4] = {0, 0, 0, 0};
            for (uint32_t i = 0; i < blk && k + i < G; i++) if (llr[k + i] != 0) { nz++; nzl[(i / cw->qamModOrder) % cw->Nl]++; }
            u += snprintf(b + u, sizeof(b) - u, " s%d:%u/%u[%u,%u,%u,%u]", m, nz, blk, nzl[0], nzl[1], nzl[2], nzl[3]);
            k += blk;
          }
          LOG_A(PHY, "SENSING: LLRFILL Nl=%u Qm=%u G=%u have=%u%s\n", cw->Nl, cw->qamModOrder, G, llr_have, b);
        }
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
    {
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
      if (!t_probe_first_seg)
        brfo_commit();
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
      if (!have_init_tx && rnti_sweepable(grant->rnti, grant->rnti_class)) {
        pthread_mutex_lock(&g_harqc_lock);
        nr_harq_init_tx_record(&g_dl_harq_init, grant->rnti, grant->harq_pid, grant->ndi, cw->qamModOrder, cw->Nl,
                               cw->ldpcBaseGraph, cw->TBS, cw->targetCodeRate);
        pthread_mutex_unlock(&g_harqc_lock);
      }
    } else {
      out->status = NR_PDSCH_PASSIVE_DECODE_CRC_FAIL;
    }
  } else {
    out->status = NR_PDSCH_PASSIVE_DECODE_ERROR;
  }

  /* Per-RNTI outcome (ISAC_PDSCH_TBPARM=1). Run inside an ATTACHED UE this splits the decode
   * population into grants addressed to US and grants addressed to ANOTHER UE, with everything
   * else -- code, rank machinery, config, radio, slot -- held identical. That is the controlled
   * test for whether a passive PDSCH failure is positional (precoder conditioned for the served
   * UE) or a defect in the receive path. */
  {
    static _Atomic int s_tbp2 = -1; /* _Atomic: lazily resolved by every passivePdsch consumer (TSAN) */
    if (s_tbp2 < 0)
      s_tbp2 = (getenv("ISAC_PDSCH_TBPARM") != NULL) ? 1 : 0;
    if (s_tbp2) {
      /* These are the PDUs actually supplied to this TB after occasion/overlap filtering.
       * Keep geometry on the outcome line: export logs alone cannot attribute applied masks.
       * Tuple: type/row:start:nrb:bitmap:l0:l1:cdm:density:scramblingID. */
      char csi_detail[512] = "";
      int pos = 0;
      for (unsigned i = 0; i < dlsch_config->numCsiRsForRateMatching && i < NFAPI_MAX_NUM_CSI_RATEMATCH; i++) {
        const fapi_nr_dl_config_csirs_pdu_rel15_t *c = &dlsch_config->csiRsForRateMatching[i];
        const int n = snprintf(csi_detail + pos, sizeof(csi_detail) - pos,
                               "%s%u/%u:%u:%u:%u:%u:%u:%u:%u:%u", i ? ";" : "",
                               c->csi_type, c->row, c->start_rb, c->nr_of_rbs, c->freq_domain,
                               c->symb_l0, c->symb_l1, c->cdm_type, c->freq_density, c->scramb_id);
        if (n < 0 || (size_t)n >= sizeof(csi_detail) - pos)
          break;
        pos += n;
      }
      /* Every field needed to attribute a failure is on THIS line. Do NOT reconstruct it by pairing
       * against the preceding TBPARM line: decodes for different slots interleave in the log, so
       * adjacency-based pairing silently mis-attributes (it produced two mutually contradictory
       * breakdowns before this was fixed). Same class of error as the retracted "20 % dt bias". */
      LOG_I(PHY,
            "SENSING: TBRESULT rnti=0x%x nl=%u mcs=%u Qm=%u R=%u tbs=%u bg=%u prb=%u+%u "
            "cdm=%u dmrsmask=0x%x nscid=%u scramb=%u refpt=%u sym=%u+%u G=%u bwpstart=%u status=%s "
            "slot=%d.%d why=%s csirm=%u harq=%u rv=%u llr_have=%u data_bits=%u csirs=[%s]\n",
            grant->rnti, (unsigned)cw->Nl, (unsigned)grant->mcs, (unsigned)cw->qamModOrder,
            (unsigned)cw->targetCodeRate, (unsigned)cw->TBS, (unsigned)cw->ldpcBaseGraph,
            (unsigned)freq_alloc->first_rb, (unsigned)freq_alloc->num_rbs,
            (unsigned)dlsch_config->n_dmrs_cdm_groups,
            (unsigned)dlsch_config->dlDmrsSymbPos,
            (unsigned)dlsch_config->nscid,
            (unsigned)dlsch_config->dlDataScramblingId,
            (unsigned)dlsch_config->refPoint,
            (unsigned)dlsch_config->start_symbol,
            (unsigned)dlsch_config->number_symbols,
            (unsigned)out->G,
            (unsigned)dlsch_config->BWPStart,
            out->status == NR_PDSCH_PASSIVE_DECODE_CRC_OK ? "CRC_OK"
              : (out->status == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL ? "CRC_FAIL" : "ERROR"),
            proc->frame_rx, proc->nr_slot_rx,
            out->status == NR_PDSCH_PASSIVE_DECODE_CRC_OK ? "ok" : t_last_sk == 0 ? "zero_tb" : t_last_sk == 2 ? "seg_fail" : "-",
            (unsigned)dlsch_config->numCsiRsForRateMatching, (unsigned)grant->harq_pid, (unsigned)grant->rv,
            t_last_llr_have, t_last_data_bits, csi_detail);
    }
    t_last_sk = -1;
  }

  pdtim_report();

  /* Nothing is freed here any more: every buffer above persists for the life of this thread and is
   * reused by the next grant. See the note at the chest allocation for the measurement. */
  { static _Atomic unsigned long c_ = 0;
    const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
    if (n_ == 1 || (n_ % 200) == 0)
      LOG_A(PHY, "SENSING: PDSCH UNSUP@3234 n=%lu\n", n_); }
  return out->status;
}

/* ---- PROBE == FULL CHECK (ISAC_TD_PROBE_EQUIV_CHECK=1|2, debug, default off; K32 / spec V2) -----
 * A probe FAIL may only count as evidence against a hypothesis if the probe computed what the full
 * decode would have: code block 0's LLRs must be the same numbers. On 1 probed grant in 50 the same
 * hypothesis is decoded again with the probe horizon off and the chest cache bypassed -- a cache-free
 * whole-slot reference -- and the two CB0 LLR vectors (descrambled, the decoder's input) are compared
 * element-wise. Every rerun stays in probe mode (CB0-only LDPC): no HARQ, no TB, no feedback.
 *
 * =2 adds the CACHE-HIT arm (K32's own scenario: a probe fills the slot's chest cache, a later decode
 * of the slot hits it), run FIRST so the decoder's working buffer still holds an older slot's
 * estimate. A single-grant slot never shares, so the slot share is forced on for this grant alone:
 * (1) the probe again, cache on -> fills the cache; (2) a whole-slot decode, cache on -> hits it;
 * (3) the reference. (2) and (3) are compared over ALL G LLRs: a truncated or post-processed cached
 * estimate shows on the symbols after the probe horizon, not in code block 0. */
static bool equiv_rerun(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, const fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg,
                        const freq_alloc_bitmap_t *fa, const nr_pdsch_passive_grant_t *grant,
                        c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP], bool no_horizon, bool bypass)
{
  fapi_nr_dl_config_dlsch_pdu_rel15_t pdu = *cfg;
  nr_pdsch_passive_decode_result_t dec = {0};
  t_probe_no_horizon = no_horizon;
  t_chest_bypass = bypass;
  const nr_pdsch_passive_decode_status_t st = nr_pdsch_passive_decode(ue, proc, &pdu, fa, grant, rxdataF, &dec);
  t_probe_no_horizon = t_chest_bypass = false;
  return st != NR_PDSCH_PASSIVE_DECODE_ERROR && st != NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED && t_last_llr != NULL && t_last_G;
}

static uint32_t equiv_diff(const int16_t *a, const int16_t *b, uint32_t n, uint32_t *first, int *maxd)
{
  uint32_t bad = 0;
  *first = UINT32_MAX;
  *maxd = 0;
  for (uint32_t i = 0; i < n; i++)
    if (a[i] != b[i]) {
      if (!bad)
        *first = i;
      bad++;
      const int d = abs((int)a[i] - (int)b[i]);
      if (d > *maxd)
        *maxd = d;
    }
  return bad;
}

void nr_pdsch_passive_probe_equiv_check(PHY_VARS_NR_UE *ue,
                                        const UE_nr_rxtx_proc_t *proc,
                                        const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                        const freq_alloc_bitmap_t *freq_alloc,
                                        const nr_pdsch_passive_grant_t *grant,
                                        c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP])
{
  static _Atomic int s_on = -1; /* _Atomic: N consumers */
  if (s_on < 0) {
    const char *e = getenv("ISAC_TD_PROBE_EQUIV_CHECK");
    s_on = (e != NULL) ? atoi(e) : 0; /* 1: probe vs full; 2: also the cache-hit arm */
  }
  if (s_on <= 0 || !t_probe_first_seg || t_last_llr == NULL || t_seg_E == 0 || t_seg_E > t_last_G)
    return;
  static _Atomic uint64_t s_seen = 0, s_n = 0, s_bad = 0, s_el = 0, s_el_bad = 0, s_skip = 0;
  static _Atomic uint64_t s_hn = 0, s_hbad = 0, s_hel = 0, s_hel_bad = 0, s_hskip = 0;
  if ((atomic_fetch_add(&s_seen, 1) % 50) != 0)
    return;
  const uint32_t E = t_seg_E;
  const int probe_horizon = t_last_probe_horizon, probe_hit = t_last_chest_hit;
  const bool saved_ok = t_probe_seg_ok;
  static __thread int16_t *buf = NULL; /* [0, E): the probe's CB0 LLRs; [E, E+G): the cache-hit decode's LLRs */
  static __thread uint32_t buf_cap = 0;
  const uint32_t need = E + t_last_G;
  if (buf_cap < need) {
    free(buf);
    buf = malloc((size_t)need * sizeof(int16_t));
    buf_cap = buf ? need : 0;
    if (!buf)
      return;
  }
  memcpy(buf, t_last_llr, (size_t)E * sizeof(int16_t));
  uint32_t first;
  int maxd;

  if (s_on == 2 && freq_alloc->n_prb_list == 0 && freq_alloc->prg == 0) {
    const nr_pdsch_slot_share_t saved_share = t_share;
    t_share = (nr_pdsch_slot_share_t){1, (int)freq_alloc->first_rb, (int)freq_alloc->num_rbs};
    t_chest_cache.valid = 0;
    t_fep_cache.valid = 0;
    (void)equiv_rerun(ue, proc, dlsch_config, freq_alloc, grant, rxdataF, false, false); /* (1) the probe fills the cache */
    const int horizon1 = t_last_probe_horizon;
    const bool ok2 = equiv_rerun(ue, proc, dlsch_config, freq_alloc, grant, rxdataF, true, false); /* (2) hits it */
    const int hit2 = t_last_chest_hit;
    const uint32_t G2 = t_last_G;
    if (ok2 && E + G2 <= buf_cap)
      memcpy(buf + E, t_last_llr, (size_t)G2 * sizeof(int16_t));
    const bool ok3 = equiv_rerun(ue, proc, dlsch_config, freq_alloc, grant, rxdataF, true, true); /* (3) reference */
    t_share = saved_share;
    t_chest_cache.valid = 0;
    t_fep_cache.valid = 0;
    if (ok2 && ok3 && hit2 && t_last_G == G2 && E + G2 <= buf_cap) {
      const uint32_t bad = equiv_diff(buf + E, t_last_llr, G2, &first, &maxd);
      const uint64_t n = atomic_fetch_add(&s_hn, 1) + 1;
      const uint64_t nb = atomic_fetch_add(&s_hbad, bad != 0) + (bad != 0);
      const uint64_t el = atomic_fetch_add(&s_hel, G2) + G2;
      const uint64_t elb = atomic_fetch_add(&s_hel_bad, bad) + bad;
      LOG_A(PHY, "SENSING: PROBE_EQUIV_HIT mismatches=%lu/%lu llr_mismatch=%lu/%lu skipped=%lu | this: S=%d L=%d dmrs=0x%x "
            "G=%u diff=%u first=%d max|d|=%d probe_horizon=%d\n",
            (unsigned long)nb, (unsigned long)n, (unsigned long)elb, (unsigned long)el, (unsigned long)atomic_load(&s_hskip),
            dlsch_config->start_symbol, dlsch_config->number_symbols, dlsch_config->dlDmrsSymbPos, G2, bad,
            bad ? (int)first : -1, maxd, horizon1);
    } else if (atomic_fetch_add(&s_hskip, 1) < 5) {
      LOG_A(PHY, "SENSING: PROBE_EQUIV_HIT skipped: ok2=%d ok3=%d hit=%d G2=%u G3=%u\n", ok2, ok3, hit2, G2, t_last_G);
    }
  }

  const bool ok = equiv_rerun(ue, proc, dlsch_config, freq_alloc, grant, rxdataF, true, true);
  t_probe_seg_ok = saved_ok;
  if (!ok || t_seg_E != E) {
    atomic_fetch_add(&s_skip, 1);
    return;
  }
  const uint32_t bad = equiv_diff(buf, t_last_llr, E, &first, &maxd);
  const uint64_t n = atomic_fetch_add(&s_n, 1) + 1;
  const uint64_t nb = atomic_fetch_add(&s_bad, bad != 0) + (bad != 0);
  const uint64_t el = atomic_fetch_add(&s_el, E) + E;
  const uint64_t elb = atomic_fetch_add(&s_el_bad, bad) + bad;
  LOG_A(PHY, "SENSING: PROBE_EQUIV mismatches=%lu/%lu llr_mismatch=%lu/%lu skipped=%lu | this: rnti=0x%x S=%d L=%d dmrs=0x%x "
        "nl=%u E=%u diff=%u first=%d max|d|=%d horizon=%d chest_hit=%d\n",
        (unsigned long)nb, (unsigned long)n, (unsigned long)elb, (unsigned long)el, (unsigned long)atomic_load(&s_skip),
        grant->rnti, dlsch_config->start_symbol, dlsch_config->number_symbols, dlsch_config->dlDmrsSymbPos,
        (unsigned)dlsch_config->cw_info[0].Nl, E, bad, bad ? (int)first : -1, maxd, probe_horizon, probe_hit);
}
