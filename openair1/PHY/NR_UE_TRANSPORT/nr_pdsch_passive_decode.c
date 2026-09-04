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
    nr_slot_fep_ant(a->ue, a->fp, a->slot, m, a->ant, rxdataF, link_type_dl, 0, a->rxdata);
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
  if (dlsch_config->pduBitmap & 0x1) {
    return out->status; // PTRS
  }
  if (dlsch_config->numCsiRsForRateMatching > 0) {
    return out->status; // CSI-RS rate matching
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
                              0 /* unav_res: PTRS/CSI-RM excluded above */, cw->qamModOrder, cw->Nl);
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
  if (fp->nb_antennas_rx > 1) {
    nr_slot_fep_ant_task_t fep_tasks[fp->nb_antennas_rx];
    task_ans_t fep_ans;
    init_task_ans(&fep_ans, fp->nb_antennas_rx);
    for (unsigned int ant = 0; ant < (unsigned int)fp->nb_antennas_rx; ant++) {
      fep_tasks[ant] = (nr_slot_fep_ant_task_t){.ue = ue,
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

  const uint64_t pdt_dem = pdtim_on ? pdtim_now() : 0;
  bool demod_ok = true;
  for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
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
        double errsum = 0.0;
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
          errsum += (vi - si) * (vi - si) + (vq - sq) * (vq - sq);
        }
        const double evm = sqrt((errsum / (double)n) / ideal_pow) * 100.0;
        LOG_I(NR_PHY,
              "SENSING: EQDIAG rnti=0x%x Qm=%u sym=%d n=%u evm=%.1f%% (ref: 29.7%% -> 90.7%% CRC, "
              "42.6%% -> ~0%%, PASSIVE_RX_ONLY_HANDOVER.md §12.1)\n",
              grant->rnti, (unsigned)cw->qamModOrder, best_m, n, evm);
      }
    }
  }

  if (demod_ok) {
    /* Measured BEFORE unscrambling: descrambling only flips signs, so magnitudes are identical
     * either side of it and taking them here keeps this independent of whether the scrambling
     * sequence is the suspect. */
    uint64_t llr_n = 0, llr_absum = 0, llr_zero = 0, llr_sat = 0, llr_pos = 0;
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
          nvar = nr_dl_chest_nvar_ant[b];
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
        if (redemod_ok && passive_ldpc_decode(ue, proc, &g_harq, cw, dlsch_config, llr, freq_alloc->num_rbs, G)) {
          ldpc_ok = true;
          atomic_fetch_add(&g_branch_ok[b], 1);
        }
        nvar = nvar_saved; // restore: the next retry (and anything downstream) expects the mean
      }
      nr_dlsch_force_branch(-1); // never leave a pin set: the next TB must re-decide normally
    }

    {
      /* 1 = decoded with data, 0 = zero_tb, 2 = seg_fail. `out->status` is not set yet here, so the
       * zero/seg distinction comes from the counters passive_ldpc_decode just bumped. */
      const int sk = ldpc_ok ? 1 : ((atomic_load(&g_ldpc_zero_tb) != zero_before) ? 0 : 2);
      atomic_fetch_add(&g_shape_n[sk], 1);
      atomic_fetch_add(&g_shape_tbs[sk], (uint64_t)cw->TBS);
      atomic_fetch_add(&g_shape_rb[sk], (uint64_t)freq_alloc->num_rbs);
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
