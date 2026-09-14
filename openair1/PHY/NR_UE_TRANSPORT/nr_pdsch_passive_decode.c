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

#include "PHY/CODING/coding_defs.h"
#include "PHY/NR_REFSIG/dmrs_nr.h" // get_num_dmrs_re_per_rb, nr_chest_time_domain_avg
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/MODULATION/modulation_UE.h" // nr_slot_fep, nr_slot_fep_ant
#include "nr_pdsch_ptrs_unav.h"
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
static int      g_pdtim_on    = -1;

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
/* Selection diversity: per-branch retry attempts and successes, indexed by receive branch.
 * `try` counts only RETRIES (the default path's own branch is not counted here), so
 * ok[b]/try[b] reads directly as "how often branch b rescued a TB the default branch lost".
 * This is the measurement that decides whether the other branches are usable at all -- the
 * standing hypothesis, never directly tested, is that only branch 0 is. */
static _Atomic uint64_t g_branch_try[NR_DL_CHEST_MAX_ANT] = {0};
static _Atomic uint64_t g_branch_ok[NR_DL_CHEST_MAX_ANT]  = {0};

/* Default ON: the retry runs only after the normal path has already failed, so it can add
 * successes but cannot remove any. ISAC_RX_BRANCH_RETRY=0 disables it for a clean A/B. */
static bool g_branch_retry_enabled(void)
{
  static int s_en = -1;
  if (s_en < 0) {
    const char *e = getenv("ISAC_RX_BRANCH_RETRY");
    s_en = (e != NULL) ? atoi(e) : 1;
  }
  return s_en != 0;
}

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
/* ---- SAME-CAPTURE ANTENNA SUBSET SCAN (ISAC_SUBSET_SCAN=<every Nth TB>, 0/unset = off) --------
 * Replays ONE captured transport block through all 15 non-empty subsets of the four receive
 * branches, reusing the identical samples, channel estimates, noise estimate, grant and decoder
 * settings -- only the set of branches entering the combiner differs. Comparing separate live runs
 * cannot answer whether four branches hurt: propagation, gain state and this rig's own 5-88 % CRC
 * swing all move between runs, and that confound has already produced several wrong conclusions.
 * Subsets are indexed by BIT POSITION = PHYSICAL branch, so {3} is physical channel 3's samples and
 * estimates, never a silent remap onto channel 0. */
#define NR_PDSCH_SUBSET_N 16
static const uint8_t kSubsetMask[NR_PDSCH_SUBSET_N] = {
    0x1, 0x2, 0x4, 0x8,                     /* {0} {1} {2} {3} */
    0x3, 0x5, 0x9, 0x6, 0xA, 0xC,           /* {0,1} {0,2} {0,3} {1,2} {1,3} {2,3} */
    0x7, 0xB, 0xD, 0xE,                     /* {0,1,2} {0,1,3} {0,2,3} {1,2,3} */
    0xF,                                    /* {0,1,2,3} */
    0x0};  /* CONTROL: no forcing at all -- byte-for-byte the primary path. If this reads 0 % while
            * the primary decode of the SAME TB read 77-87 %, then re-running the demod+decode chain
            * a second time is itself what fails, and every subset number is meaningless. */
static const char *const kSubsetName[NR_PDSCH_SUBSET_N] = {
    "{0}", "{1}", "{2}", "{3}", "{0,1}", "{0,2}", "{0,3}", "{1,2}", "{1,3}", "{2,3}",
    "{0,1,2}", "{0,1,3}", "{0,2,3}", "{1,2,3}", "{0,1,2,3}", "REPLAY-CTL"};
static _Atomic uint64_t g_subset_try[NR_PDSCH_SUBSET_N];
static _Atomic uint64_t g_subset_ok[NR_PDSCH_SUBSET_N];

/* The DMRSFO tracker's current SFO estimate, in ppm, for the correction stage below. Read-mostly
 * across consumer threads; a torn double would only mean one grant corrected with a slightly stale
 * value, which is why this is a plain double and not a lock. */
static double g_sfo_ppm_ema = 0.0;
static double nr_pdsch_passive_sfo_ppm(void) { return g_sfo_ppm_ema; }
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
  {
    /* SUBSET: every subset scored on the SAME transport blocks, so the comparison isolates the
     * combiner. Read {0} as the single-branch reference: any subset BELOW it is a case of adding a
     * branch making decoding worse. Same-board pairs vs cross-board pairs is the discriminator for
     * the per-daughterboard frequency-offset hypothesis (X410: ch0/1 on board A, ch2/3 on board B).*/
    char sb[520];
    size_t u = 0;
    for (int k = 0; k < NR_PDSCH_SUBSET_N && u < sizeof(sb) - 34; k++) {
      const uint64_t t = atomic_load(&g_subset_try[k]);
      if (t == 0) {
        continue;
      }
      u += snprintf(sb + u, sizeof(sb) - u, "%s=%.0f%%(%lu) ", kSubsetName[k],
                    100.0 * (double)atomic_load(&g_subset_ok[k]) / (double)t, (unsigned long)t);
    }
    if (u > 0) {
      LOG_I(PHY, "SENSING: SUBSET crc_ok%% on identical TBs: %s\n", sb);
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
  /* BRANCHSEL: what selection diversity actually bought, per branch. `rescued` is the number of
   * transport blocks that FAILED on the default branch and then decoded on this one -- so a column
   * that stays at 0/N says that branch is unusable however strong it looks, which is exactly the
   * per-daughterboard-offset question. Printed unconditionally alongside LDPCDIAG (one line per
   * census) rather than behind a probe env var, because it is the headline result of the feature. */
  {
    char bs[160];
    size_t u = 0;
    uint64_t tot_try = 0, tot_ok = 0;
    bs[0] = '\0';
    for (int b = 0; b < NR_DL_CHEST_MAX_ANT; b++) {
      const uint64_t bt = atomic_load(&g_branch_try[b]);
      const uint64_t bo = atomic_load(&g_branch_ok[b]);
      tot_try += bt;
      tot_ok += bo;
      if (u < sizeof(bs) - 1) {
        const int n = snprintf(bs + u, sizeof(bs) - u, "%s%d:%lu/%lu",
                               b ? " " : "", b, (unsigned long)bo, (unsigned long)bt);
        // snprintf returns what it WOULD have written; clamp so a truncation cannot walk past the end
        u = (n > 0 && (size_t)n < sizeof(bs) - u) ? u + (size_t)n : sizeof(bs) - 1;
      }
    }
    if (tot_try > 0) {
      LOG_I(PHY, "SENSING: BRANCHSEL rescued=%lu/%lu retries [%s] (branch:rescued/tried)\n",
            (unsigned long)tot_ok, (unsigned long)tot_try, bs);
    }
  }
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
static bool passive_ldpc_decode(PHY_VARS_NR_UE *ue,
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
  TB_parameters.A = cw->TBS;
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
  TB_parameters.d = h->d;
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
    static int s_sd = -1;
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

  TB_parameters.d_to_be_cleared = true;
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
    for (uint32_t r = 0; r < TB_parameters.C; r++) {
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

  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;

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
  if (dlsch_config->pduBitmap & 0x1) {
    static int s_ptrs_k = -1, s_ptrs_l = -1;
    if (s_ptrs_k < 0) {
      const char *ek = getenv("ISAC_PTRS_K"), *el = getenv("ISAC_PTRS_L");
      s_ptrs_k = (ek && *ek) ? atoi(ek) : 0;
      s_ptrs_l = (el && *el) ? atoi(el) : 0;
    }
    if (s_ptrs_k <= 0 || s_ptrs_l <= 0) {
      return out->status; // PT-RS, and no density given to compute G with
    }
    ptrs_unav = nr_pdsch_ptrs_unav_res(freq_alloc->num_rbs, dlsch_config->start_symbol,
                                       dlsch_config->number_symbols, dlsch_config->dlDmrsSymbPos,
                                       (uint8_t)s_ptrs_k, (uint8_t)s_ptrs_l, 1);
    if (ptrs_unav == 0) {
      return out->status; // the density did not describe any PT-RS -- do not guess G
    }
  }
  /* ---- CSI-RS rate matching: still refused, and NOT merely unimplemented ---------------------
   * Rate matching around CSI-RS needs the ZP CSI-RS resource configuration to know WHICH REs were
   * skipped. That is dedicated RRC and is not broadcast, so unlike PT-RS there is no small
   * discrete set to sweep -- the resource's row, bitmap, symbols and density would all have to be
   * recovered first. That is exactly what nr_csirs_blind_search.c exists to do; this becomes
   * possible once that search is wired and converging, and not before. */
  if (dlsch_config->numCsiRsForRateMatching > 0) {
    return out->status;
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
  if (cw->qamModOrder == 0 || R == 0) {
    // MCS 28-31 (reserved-for-retransmission rows) have no modulation order/code rate of their own:
    // a real UE takes them from the initial transmission. A passive receiver has no such history,
    // so such a grant is simply not decodable here. nr_pdcch_blind_decode_and_extract() already
    // rejects those, so reaching this means the MCS table assumption is wrong.
    return out->status;
  }
  const uint8_t nb_re_dmrs = get_num_dmrs_re_per_rb(dlsch_config->dmrsConfigType, dlsch_config->n_dmrs_cdm_groups);
  const uint16_t dmrs_len  = get_num_dmrs(dlsch_config->dlDmrsSymbPos);
  cw->targetCodeRate = (uint16_t)R;
  cw->TBS = nr_compute_tbs(cw->qamModOrder, (uint16_t)R, freq_alloc->num_rbs, dlsch_config->number_symbols,
                           nb_re_dmrs * dmrs_len, grant->nb_rb_oh, grant->tb_scaling, cw->Nl);
  if (cw->TBS == 0) {
    return out->status;
  }
  cw->ldpcBaseGraph = get_BG(cw->TBS, cw->targetCodeRate);
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
  const int nl_tbslbrm = 4; // = min(maxMIMO-LayersPDSCH, 4); confirmed against the gNB's own print
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
                              ptrs_unav /* 0 unless a PT-RS density was given; CSI-RM still excluded */,
                              cw->qamModOrder, cw->Nl);
  if (G == 0) {
    return out->status;
  }
/* TBPARM probe (ISAC_PDSCH_TBPARM=1): every transport-block parameter the gNB also prints on its
   * own PDSCH line, so they can be compared one-for-one instead of inferred from a CRC failure.
   * gNB prints: mcs_index / mod / tbs / tb_size_lbrm / ldpc_base_graph / vrbs=[start..end). */
  {
    static int s_tbp = -1;
    if (s_tbp < 0)
      s_tbp = (getenv("ISAC_PDSCH_TBPARM") != NULL) ? 1 : 0;
    if (s_tbp)
      LOG_I(PHY,
            "SENSING: TBPARM rnti=0x%x nl=%u mcs=%u tbl=%u Qm=%u R=%u tbs=%u G=%u lbrm=%u bg=%u "
            "prb=%u+%u nsym=%u dmrs_len=%u nb_re_dmrs=%u\n",
            grant->rnti, (unsigned)cw->Nl, (unsigned)grant->mcs, (unsigned)grant->mcs_table,
            (unsigned)cw->qamModOrder, (unsigned)cw->targetCodeRate, (unsigned)cw->TBS, G,
            (unsigned)dlsch_config->tbslbrm, (unsigned)cw->ldpcBaseGraph, (unsigned)freq_alloc->first_rb,
            (unsigned)freq_alloc->num_rbs, (unsigned)dlsch_config->number_symbols, (unsigned)dmrs_len,
            (unsigned)nb_re_dmrs);
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
    return out->status;
  }

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
  const double fep_fo = isnan(nr_slot_fep_fo_override_hz)
      ? (ue->cont_fo_comp ? ue->dl_Doppler_shift + ue->freq_offset : 0.0)
      : nr_slot_fep_fo_override_hz;
  if (fp->nb_antennas_rx > 1) {
    nr_slot_fep_ant_task_t fep_tasks[fp->nb_antennas_rx];
    task_ans_t fep_ans;
    init_task_ans(&fep_ans, fp->nb_antennas_rx);
    for (unsigned int ant = 0; ant < (unsigned int)fp->nb_antennas_rx; ant++) {
      fep_tasks[ant] = (nr_slot_fep_ant_task_t){.fo_hz = fep_fo, .ue = ue,
                                                .fp = fp,
                                                .slot = proc->nr_slot_rx,
                                                .start_symbol = dlsch_config->start_symbol,
                                                .number_symbols = dlsch_config->number_symbols,
                                                .ant = ant,
                                                .rxdataF_flat = &rxdataF[0][0],
                                                .stride = fp->samples_per_slot_wCP,
                                                .rxdata = ue->common_vars.rxdata,
                                                .ans = &fep_ans};
      task_t t = {.func = nr_slot_fep_ant_task, .args = &fep_tasks[ant]};
      pushTpool(&get_nrUE_params()->Tpool, t);
    }
    join_task_ans(&fep_ans);
  } else {
    for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
      nr_slot_fep(ue, fp, proc->nr_slot_rx, m, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
    }
  }
  pdtim_add(PDTIM_FEP, pdt_fep);

  if (grant->check_sample_lifetime && !nr_passive_samples_valid(
          atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed),
          grant->source_absolute_slot, fp->slots_per_frame)) {
    out->status = NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED;
    return out->status; /* overwritten IQ is not CRC evidence */
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

  uint32_t nvar = 0;
  int n_dmrs_sym = 0;
  int dmrs_first = -1, dmrs_last = -1; // for the per-branch phase-slope estimator below
  for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
    if (!((dlsch_config->dlDmrsSymbPos >> m) & 1)) {
      continue;
    }
    if (dmrs_first < 0) {
      dmrs_first = m;
    }
    dmrs_last = m;
    for (int nl = 0; nl < cw->Nl; nl++) { // mirrors nr_ue_pdsch_procedures()'s per-layer loop
      uint32_t nvar_tmp = 0;
      nr_pdsch_channel_estimation(ue, proc, dlsch_config, freq_alloc, nl,
                                  get_dmrs_port(nl, dlsch_config->dmrs_ports), (unsigned char)m, pdsch_est_size,
                                  pdsch_dl_ch_estimates, fp->samples_per_slot_wCP, rxdataF, &nvar_tmp);
      nvar += nvar_tmp;
    }
    n_dmrs_sym++;
  }
  if (n_dmrs_sym == 0) {
    return out->status; // no DM-RS in the allocation: nothing to equalise against
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
  uint32_t nvar_den = 1u; // hoisted: the per-branch substitution below must reuse the SAME divisor
  {
    static int s_nvfix = -1;
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
    static int s_pb = -1;
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
  if (dmrs_first >= 0 && dmrs_last > dmrs_first) {
    static _Atomic uint64_t s_dfo_n = 0;
    /* SFO is counted separately from CFO: a grant too narrow to fit two slope groups yields a CFO
     * but NO SFO, and the two populations are not the same size (measured 46 % narrow on
     * captures/sfooff_r1_191752). Sharing one counter is what let an unmeasured SFO be folded in
     * as if it were a measurement -- see the gate below. */
    static _Atomic uint64_t s_sfo_n = 0;
    /* The EMA updates are READ-MODIFY-WRITE and this function runs on N consumer threads
     * (nr_pdsch_passive_queue.c starts several). Plain statics raced: a capture showed
     * "cfo=-41.3 Hz (ema +248.2)", an average nowhere near the samples feeding it. The comment on
     * g_sfo_ppm_ema justifies a torn READ by a consumer, which is fine and unchanged; it does not
     * justify a torn update. The critical section is a few flops. */
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
        g_sfo_ppm_ema = s_sfo_ema;  // published for the SFO correction stage
      }
      sfo_pub = s_sfo_ema;
      const double cfo_pub = s_cfo_ema;
      pthread_mutex_unlock(&s_dfo_lock);
      static int s_apply = -1;
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

  if (fp->nb_antennas_rx > 1 && dmrs_first >= 0 && dmrs_last > dmrs_first) {
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
    for (int a = 0; a < fp->nb_antennas_rx && a < NR_DL_CHEST_MAX_ANT; a++) {
      // Differential against branch 0: the common part is the shared sync loop's job, not ours.
      const double d = fo[a] - fo[0];
      s_fo_ema[a] = (n == 0) ? d : (0.99 * s_fo_ema[a] + 0.01 * d);
      if (getenv("ISAC_RX_BRANCH_FO") != NULL && atoi(getenv("ISAC_RX_BRANCH_FO")) != 0) {
        nr_ue_set_branch_fo_hz(a, -s_fo_ema[a]); // de-rotate by the negative of the observed drift
      }
    }
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
    static int s_cd = -1;
    if (s_cd < 0)
      s_cd = (getenv("ISAC_PDSCH_TBPARM") != NULL) ? 1 : 0;
    static int s_cd_left = 12;
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

  if (ue->chest_time == 1) {
    nr_chest_time_domain_avg(fp, (int32_t **)pdsch_dl_ch_estimates, dlsch_config->number_symbols,
                             dlsch_config->start_symbol, dlsch_config->dlDmrsSymbPos, freq_alloc->num_rbs, cw->Nl,
                             fp->nb_antennas_rx);
  }

  // ---- Demodulate to LLRs, symbol by symbol. nr_rx_pdsch() reads its transport-block parameters
  // out of a NR_UE_DLSCH_t and a NR_DL_UE_HARQ_t; both are stack-local here, deliberately (see the
  // header). `status = NR_ACTIVE` is what makes it apply the PTRS/symbol-span branch consistently
  // with the attached path -- with pduBitmap==0 it only selects the symbol bookkeeping. ----
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
  const uint32_t rx_llr_buf_sz = ALIGNARRAYSIZE(G, 32);
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
  uint32_t dmrs_data_re = (dlsch_config->dmrsConfigType == NFAPI_NR_DMRS_TYPE1)
                              ? 12 - 6 * dlsch_config->n_dmrs_cdm_groups
                              : 12 - 4 * dlsch_config->n_dmrs_cdm_groups;
  int first_symbol_with_data = dlsch_config->start_symbol;
  while (dmrs_data_re == 0 && (dlsch_config->dlDmrsSymbPos & (1 << first_symbol_with_data))) {
    first_symbol_with_data++;
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
  static int s_sfo_corr = -1;
  if (s_sfo_corr < 0) {
    const char *e = getenv("ISAC_SFO_CORRECT");
    s_sfo_corr = (e != NULL && atoi(e) != 0) ? 1 : 0;
  }
  double sfo_applied[NR_SYMBOLS_PER_SLOT] = {0};  // symbol-periods of rotation already applied, per slot
  const double sfo_eps = s_sfo_corr ? (nr_pdsch_passive_sfo_ppm() * 1.0e-6) : 0.0;
  const double sfo_tsym = (1.0e-3 / (double)fp->slots_per_subframe) / (double)fp->symbols_per_slot;

  const uint64_t pdt_dem = pdtim_on ? pdtim_now() : 0;
  bool demod_ok = true;
  for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
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
            const double ph = c * (double)k;
            const double cs = cos(ph), sn = sin(ph);
            const double hr = (double)h[k].r, hi = (double)h[k].i;
            h[k].r = (int16_t)lround(hr * cs - hi * sn);
            h[k].i = (int16_t)lround(hr * sn + hi * cs);
          }
        }
      }
    }
    if (nr_rx_pdsch(ue, proc, &dlsch, freq_alloc, dlsch_config, &harq, (unsigned char)m,
                    m == first_symbol_with_data, (unsigned char)dlsch_config->harq_process_nbr, pdsch_est_size,
                    pdsch_dl_ch_estimates, llr, dl_valid_re, rxdataF, &log2_maxh, rx_size_symbol,
                    fp->nb_antennas_rx, rxdataF_comp, dl_ch_mag, dl_ch_magb, dl_ch_magr, ptrs_phase_per_slot,
                    ptrs_re_per_slot, nvar, &scope_req, NULL /* rho_dl: single layer */)
        < 0) {
      demod_ok = false;
      break;
    }
  }

  pdtim_add(PDTIM_DEMOD, pdt_dem);

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
    static int s_evm = -1;
    if (s_evm < 0)
      s_evm = (getenv("ISAC_PDSCH_EVM") != NULL) ? 1 : 0;
    static __thread unsigned long s_evm_n = 0;
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
        const int start_re_abs =
            (fp->first_carrier_offset + start_rb_abs * NR_NB_SC_PER_RB) % fp->ofdm_symbol_size;
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
      static int s_llr_scale = -1;
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
    bool ldpc_ok = passive_ldpc_decode(ue, proc, &g_harq, cw, dlsch_config, llr, freq_alloc->num_rbs, G);
    pdtim_add(PDTIM_LDPC, pdt_ldp);

    /* ---- SELECTION DIVERSITY across receive branches (2026-09-03) -------------------------------
     * If the default branch selection failed CRC, re-demodulate and re-decode the SAME transport
     * block from each OTHER receive branch in turn, and keep the first one that passes.
     *
     * WHY THIS SHAPE, and why it is safe: the default path runs FIRST and unchanged, so this can
     * only convert a failure into a success -- never the reverse. `nb_antennas_rx == 1` and a
     * successful first attempt both skip the whole block, leaving those cases bit-identical.
     *
     * WHY SELECTION AND NOT COMBINING: OAI's fixed-point MRC accumulator overflows at four RX
     * (the reason ISAC_RX_MRC_MODE defaults to 0, branch 0 only), and on this rig an alternated
     * 2x4 sweep measured every combining mode at 0.0 % CRC against 48.9-82.6 % for branch 0 alone.
     * Decoding one branch at a time keeps each attempt in exactly the configuration that works.
     *
     * WHY IT COSTS NOTHING FOR SENSING: the decode exists only to recover X. X is a single physical
     * truth -- whichever branch recovers it, the data-aided tap then forms H = Y_a/X against the RAW
     * per-antenna Y of EVERY antenna (nr_pdsch_data_aided.c loops rxdataF[a]), so AoA and per-antenna
     * CFR are unaffected by which branch happened to decode.
     *
     * WHY BY INDEX AND NOT BY POWER: mode 1 already picks the strongest branch, and it measured
     * 0.0 % while branch 3 held the highest |h| -- power does not predict decodability on this rig
     * (the standing hypothesis is a per-daughterboard frequency offset: X410 puts ch0/1 on board A
     * and ch2/3 on board B). So walk by index and let the CRC be the judge. The per-branch counters
     * below are the measurement that turns that hypothesis into data. */
    if (!ldpc_ok && fp->nb_antennas_rx > 1 && cw->Nl == 1 && g_branch_retry_enabled()) {
      const int first_branch = nr_dlsch_last_branch(); // -1 if the first attempt combined
      for (int b = 0; b < fp->nb_antennas_rx && !ldpc_ok; b++) {
        if (b == first_branch) {
          continue; // already tried, and it failed
        }
        atomic_fetch_add(&g_branch_try[b], 1);
        nr_dlsch_force_branch(b);
        /* Per-branch nvar: the equaliser is about to work on branch b ALONE, so hand it branch b's
         * own noise rather than the mean across all four. The mean is dominated by the weak
         * branches here (8-15 dB down), which mis-states the confidence for whichever single branch
         * is actually being decoded. Falls back to the mean if this branch produced no estimate. */
        const uint32_t nvar_saved = nvar;
        if (b < NR_DL_CHEST_MAX_ANT && nr_dl_chest_nvar_ant[b] > 0) {
          const uint64_t scaled = (uint64_t)nr_dl_chest_nvar_ant[b] * (uint64_t)(n_dmrs_sym * cw->Nl);
          const uint32_t nvar_branch = (uint32_t)(scaled / nvar_den);
          if (nvar_branch > 0) {
            nvar = nvar_branch;
          }
        }
        memset(llr, 0, rx_llr_buf_sz * sizeof(*llr));
        bool redemod_ok = true;
        for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
          if (nr_rx_pdsch(ue, proc, &dlsch, freq_alloc, dlsch_config, &harq, (unsigned char)m,
                          m == first_symbol_with_data, (unsigned char)dlsch_config->harq_process_nbr, pdsch_est_size,
                          pdsch_dl_ch_estimates, llr, dl_valid_re, rxdataF, &log2_maxh, rx_size_symbol,
                          fp->nb_antennas_rx, rxdataF_comp, dl_ch_mag, dl_ch_magb, dl_ch_magr, ptrs_phase_per_slot,
                          ptrs_re_per_slot, nvar, &scope_req, NULL /* rho_dl: single layer */)
              < 0) {
            redemod_ok = false;
            break;
          }
        }
        /* DESCRAMBLE. nr_dlsch_unscrambling() mutates llr IN PLACE and is applied exactly once on
         * the primary path, BEFORE the first decode -- so any path that regenerates llr by
         * re-running nr_rx_pdsch() must descramble it again or it hands the LDPC decoder scrambled
         * soft bits and fails 100 %% of the time, whatever the antennas are doing.
         * Found 2026-09-08 by a no-mask REPLAY CONTROL in the subset scan: forcing nothing at all,
         * i.e. reproducing the primary path exactly, still read 0 %% on TBs the primary decoded at
         * 49.7 %%. That isolated the fault to the REPLAY rather than to branch selection, after two
         * earlier fixes (per-subset nvar, the mask-aware shift) had been aimed at the wrong thing. */
        if (redemod_ok) {
          nr_dlsch_unscrambling(llr, G, 0 /* codeword */, dlsch_config->dlDataScramblingId, grant->rnti);
        }
        if (redemod_ok && passive_ldpc_decode(ue, proc, &g_harq, cw, dlsch_config, llr, freq_alloc->num_rbs, G)) {
          ldpc_ok = true;
          atomic_fetch_add(&g_branch_ok[b], 1);
        }
        nvar = nvar_saved; // restore: the next retry (and anything downstream) expects the mean
      }
      nr_dlsch_force_branch(-1); // never leave a pin set: the next TB must re-decide normally
    }

    /* ---- Subset scan. Runs AFTER the normal decode so it can never change this TB's own result:
     * ldpc_ok is saved and restored, and the pin is always cleared. Sampled (1 in N) because it
     * costs 15 extra demod+decode passes per scanned TB, which is far beyond the RT budget if run
     * on every grant. Deliberately NOT restricted to TBs that some subset decoded -- selecting on
     * success would bias every rate it reports. */
    {
      static int s_subset_n = -1;
      if (s_subset_n < 0) {
        const char *e = getenv("ISAC_SUBSET_SCAN");
        s_subset_n = (e != NULL) ? atoi(e) : 0;
      }
      static __thread unsigned long s_subset_seen = 0;
      if (s_subset_n > 0 && fp->nb_antennas_rx == 4 && cw->Nl == 1
          && (s_subset_seen++ % (unsigned long)s_subset_n) == 0) {
        const bool ldpc_ok_saved = ldpc_ok;
        /* The diagnostic decoder reuses g_harq.b. Preserving only ldpc_ok would publish
         * the final subset's bytes as if they were the primary CRC-verified TB. */
        const size_t saved_tb_size = (lenWithCrc(1, cw->TBS) + 7u) / 8u;
        uint8_t *saved_tb = ldpc_ok_saved ? malloc(saved_tb_size) : NULL;
        if (saved_tb)
          memcpy(saved_tb, g_harq.b, saved_tb_size);
        /* On allocation failure skip the diagnostic, never risk the production payload. */
        for (int k = 0; (!ldpc_ok_saved || saved_tb) && k < NR_PDSCH_SUBSET_N; k++) {
          /* mask 0 is the control: force nothing, so selection follows the normal mode-0 path. */
          nr_dlsch_force_mask(kSubsetMask[k] ? kSubsetMask[k] : -1);
          memset(llr, 0, rx_llr_buf_sz * sizeof(*llr));
          bool ok = true;
          for (int m = dlsch_config->start_symbol;
               m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
            if (nr_rx_pdsch(ue, proc, &dlsch, freq_alloc, dlsch_config, &harq, (unsigned char)m,
                            m == first_symbol_with_data, (unsigned char)dlsch_config->harq_process_nbr,
                            pdsch_est_size, pdsch_dl_ch_estimates, llr, dl_valid_re, rxdataF,
                            &log2_maxh, rx_size_symbol, fp->nb_antennas_rx, rxdataF_comp, dl_ch_mag,
                            dl_ch_magb, dl_ch_magr, ptrs_phase_per_slot, ptrs_re_per_slot, nvar,
                            &scope_req, NULL) < 0) {
              ok = false;
              break;
            }
          }
          if (ok) {
            // Same reason as the retry loop above: llr has just been regenerated, so it is
            // scrambled again and must be descrambled before the decoder sees it.
            nr_dlsch_unscrambling(llr, G, 0 /* codeword */, dlsch_config->dlDataScramblingId,
                                  grant->rnti);
          }
          atomic_fetch_add(&g_subset_try[k], 1);
          if (ok && passive_ldpc_decode(ue, proc, &g_harq, cw, dlsch_config, llr,
                                        freq_alloc->num_rbs, G)) {
            atomic_fetch_add(&g_subset_ok[k], 1);
          }
        }
        nr_dlsch_force_mask(-1);  // never leave a mask pinned
        if (saved_tb) {
          memcpy(g_harq.b, saved_tb, saved_tb_size);
          free(saved_tb);
        }
        ldpc_ok = ldpc_ok_saved;  // both outcome and payload now match the primary/retry result
      }
    }

    {
      /* 1 = decoded with data, 0 = zero_tb, 2 = seg_fail. `out->status` is not set yet here, so the
       * zero/seg distinction comes from the counters passive_ldpc_decode just bumped. */
      const int sk = ldpc_ok ? 1 : ((atomic_load(&g_ldpc_zero_tb) != zero_before) ? 0 : 2);
      {
        /* llr_have: index one past the last NON-ZERO LLR. An exactly-zero LLR is possible but
         * vanishingly rare in real soft output, so the last nonzero is a good proxy for how far
         * nr_rx_pdsch() actually filled the buffer -- and a shortfall against G is the thing being
         * hunted. Scanned backwards so a full buffer costs one comparison. */
        uint32_t llr_have = 0;
        for (int i = (int)G - 1; i >= 0; i--) {
          if (llr[i] != 0) { llr_have = (uint32_t)i + 1; break; }
        }
        uint32_t vre = 0;
        for (int m = dlsch_config->start_symbol;
             m < dlsch_config->start_symbol + dlsch_config->number_symbols && m < NR_SYMBOLS_PER_SLOT; m++) {
          vre += dl_valid_re[m];
        }
        const int pk = (sk == 1) ? 1 : 0;  // 1 = decoded, 0 = did not decode (zero_tb or seg_fail)
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
      out->status = NR_PDSCH_PASSIVE_DECODE_CRC_OK;
      out->tb     = g_harq.b;
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
    static int s_tbp2 = -1;
    if (s_tbp2 < 0)
      s_tbp2 = (getenv("ISAC_PDSCH_TBPARM") != NULL) ? 1 : 0;
    if (s_tbp2)
      /* Every field needed to attribute a failure is on THIS line. Do NOT reconstruct it by pairing
       * against the preceding TBPARM line: decodes for different slots interleave in the log, so
       * adjacency-based pairing silently mis-attributes (it produced two mutually contradictory
       * breakdowns before this was fixed). Same class of error as the retracted "20 % dt bias". */
      LOG_I(PHY,
            "SENSING: TBRESULT rnti=0x%x nl=%u mcs=%u Qm=%u R=%u tbs=%u bg=%u prb=%u+%u status=%s\n",
            grant->rnti, (unsigned)cw->Nl, (unsigned)grant->mcs, (unsigned)cw->qamModOrder,
            (unsigned)cw->targetCodeRate, (unsigned)cw->TBS, (unsigned)cw->ldpcBaseGraph,
            (unsigned)freq_alloc->first_rb, (unsigned)freq_alloc->num_rbs,
            out->status == NR_PDSCH_PASSIVE_DECODE_CRC_OK ? "CRC_OK"
              : (out->status == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL ? "CRC_FAIL" : "ERROR"));
  }

  pdtim_report();

  /* Nothing is freed here any more: every buffer above persists for the life of this thread and is
   * reused by the next grant. See the note at the chest allocation for the measurement. */
  return out->status;
}
