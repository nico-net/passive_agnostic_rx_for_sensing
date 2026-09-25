#include "PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.h"
#include "nr_passive_sample_lifetime.h"
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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c
 * \brief RT receive-path tap for the blind PDCCH monitor (Phase 3 live wiring, Stage 1 of
 * /home/sens/.claude/plans/zesty-baking-thompson.md). See nr_pdcch_blind_monitor_rt.h for why this
 * is a separate translation unit from nr_pdcch_blind_monitor.c (the offline-tested pure decode
 * core + config parser): this file calls into PHY_NR_UE (nr_slot_fep, nr_pdcch_generate_llr,
 * nr_pdsch_channel_estimation) and NR_UE_ISAC (nr_isac_submit_cfr), so it is compiled into
 * PHY_NR_UE_SRC rather than the lean, offline-gtest-linked nr_pdcch_blind_monitor library.
 *
 * Builds a fully local, single-search-space nr_phy_data_t/fapi_nr_dl_config_dci_dl_pdu_rel15_t
 * every call, straight from [sensing] pdcch_blind_monitor_* config (via
 * nr_pdcch_blind_monitor_get_cfg()) -- NEVER the real MAC-driven phy_pdcch_config the caller's own
 * phy_data carries (that instance is untouched; see the call site in executables/nr-ue.c). This is
 * what makes the tap safe to run even in --passive-rx's UE_RECEIVING_SIB state, where the real
 * phy_pdcch_config.nb_search_space is always 0.
 */

#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_sib1_prior.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.h" // Phase 3 Technique C
#include "PHY/NR_UE_TRANSPORT/nr_passive_bwp.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_adaptive_config.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.h" // Phase 3 Technique D
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_dci11_layout_sweep.h" // DCI 1_1 layout, stage 1
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_dci01_layout_sweep.h" // DCI 0_1 layout, stage 1
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_ss_registry.h"        // CORESET/SS registry, observe-only
#include "PHY/NR_UE_TRANSPORT/nr_agnostic_v2.h"
#include "PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.h" // blind CSI-RS search, observe-only
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.h"      // Phase 3 Technique A cross-check (XCHECK diag)

#include <string.h>
#include <stdlib.h>
#include <limits.h>
bool nr_passive_rar_tc_seen(uint16_t rnti, uint32_t now_abs_slot, uint32_t window_slots, uint32_t *age_out); // nr_pdsch_passive_queue.c // getenv/atoi for the env-gated diagnostics in this file
#include <time.h> // clock_gettime for the rnti_seen correlation line below

#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"

#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h" // nr_pdcch_demapping_deinterleaving/_unscrambling/_generate_llr
#include "PHY/MODULATION/modulation_UE.h"               // nr_slot_fep
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"          // nr_pdsch_channel_estimation
#include "PHY/TOOLS/tools_defs.h"                        // allocCast2D/fourDimArray_t
#include "PHY/NR_UE_ISAC/nr_isac.h"                      // nr_isac_submit_cfr/_enabled/_source_enabled
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.h"  // passive PDSCH decode (data-aided source)
#include "PHY/NR_UE_TRANSPORT/nr_passive_mac_ta.h"        // timing advance out of an overheard MAC PDU
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.h"   // deferred decode off the RT thread
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_passive_queue.h"   // deferred SCAN off the RT thread
#include <stdatomic.h>

/* Published by the RF producer thread (executables/nr-ue.c:43/1148) immediately before it reads
 * each slot. The deferred-decode staleness check differences against it, so the enqueue side must
 * read the SAME counter rather than rebuild one from the wrapping frame number. */
extern _Atomic long nr_ue_diag_producer_absolute_slot;
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.h"      // shared re-encode + Ĥ=Y/X submit
#include "nr_pdcch_ul_discovery.h"
#include "nr_passive_acq_state.h" // explicit acquisition-state tracker (period-guarded)
#include "nr_pdsch_xoverhead.h"
#include <pthread.h>
#include "PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.h" // passive UPLINK PUSCH receive census
#include "PHY/NR_UE_TRANSPORT/nr_pusch_passive_monitor_rt.h" // UL grant book
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_constants.h" // FAPI_NR_CCE_REG_MAPPING_TYPE_*
#include "executables/nr-uesoftmodem.h"                   // get_nrUE_params()->Tpool
#include "common/utils/threadPool/thread-pool.h"          // tpool_t, pushTpool, task_t
#include "common/utils/threadPool/task_ans.h"             // task_ans_t, init/join/completed_task_ans
#include "nr_pdcch_discovery_replay.h"
#include "nr_pdcch_uss_tracker.h"
#include "nr_pdcch_joint_live.h"
#include <stdio.h>
#include "nr_polar_gpu.h"                                 // SWEEP GPU BATCH: nr_gpu_polar_load/decode_vec

#define NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS 9 // == dci_nr.c's file-local RE_PER_RB_OUT_DMRS #define
// Spec maxima for a CORESET: the frequency-domain bitmap addresses 6-PRB groups over the BWP, so at
// most floor(275/6) = 45 groups = 270 PRB; duration is 1..3 symbols (38.331 ControlResourceSet).
#define NR_PDCCH_BLIND_MAX_CORESET_RB 270
#define NR_PDCCH_BLIND_MAX_CORESET_DURATION 3
// 32 covers a full-BWP CORESET: 270 PRB / 6 = 45 CCEs -> 22 non-overlapping AL2 positions. The
// previous value of 8 was sized for the small rfsim CORESET and silently covered only CCE 0-14,
// i.e. ~36 % of a wideband CORESET -- MEASURED on a live 273 PRB cell, every one of the ~2200
// accepts was a random-CRC false positive and not one real grant was caught, because the gNB's
// UE-specific search space hashes its candidate to a per-slot position that mostly fell outside
// the scanned range. Bounded by fapi_nr_ue_interface.h's CCE[64].
#define NR_PDCCH_BLIND_AL2_MAX_CANDIDATES 32 // scan every non-overlapping AL2 CCE position -- a blind
                                            // receiver does not know which of these the real UE's own
                                            // RNTI hash landed on (unlike this SS's own al2_cand
                                            // config value, which describes ONE known UE's candidate
                                            // count -- see the plan's Stage-1 implementation note)
#define NR_PDCCH_BLIND_MAX_ANT 8 // matches csi_rx.c's NR_ISAC_CSIRS_MAX_ANT -- same reasoning, a
                                 // generous cap on the AoA receive array size this tap will extract
#define NR_PDCCH_BLIND_DATA_AIDED_TAG_BASE 3000 // nrLDPC_coding_interface harq_unique_pid namespace
/* Per-UE stride. harq_unique_pid must be unique across everything the LDPC accelerator holds at
 * once; with several UEs decoded simultaneously, BASE + harq_pid aliases the moment two of them use
 * the same HARQ process, silently mixing two receivers' contexts. 16 UEs x 16 processes = 3000-3255,
 * which stays clear of the 1000/2000 namespaces this file's header documents. */
#define NR_PDCCH_BLIND_UE_TAG_STRIDE 16
static uint32_t blind_harq_tag(uint32_t abs_slot, uint16_t rnti, uint8_t harq_pid)
{
  uint16_t known[NR_PDCCH_BLIND_MAX_UE];
  const int n = nr_pdcch_blind_monitor_confirmed_rnti_set(abs_slot, known, NR_PDCCH_BLIND_MAX_UE);
  int slot = 0;
  for (int i = 0; i < n; i++) {
    if (known[i] == rnti) { slot = i; break; }
  }
  return (uint32_t)(NR_PDCCH_BLIND_DATA_AIDED_TAG_BASE
                    + slot * NR_PDCCH_BLIND_UE_TAG_STRIDE + (harq_pid % NR_PDCCH_BLIND_UE_TAG_STRIDE));
}
                                                // for the passive data-aided RE-ENCODE; distinct from
                                                // the attached tap's 1000+pid and from
                                                // nr_pdsch_passive_decode.c's 2000+pid DECODE tag

static void build_coreset_bitmap(int num_groups, uint8_t bitmap[6])
{
  memset(bitmap, 0, 6);
  for (int g = 0; g < num_groups && g < 45; g++) {
    bitmap[g / 8] |= (uint8_t)(0x80 >> (g % 8));
  }
}

// Periodic INFO-level summary. The interesting per-candidate detail (LOG_D "blind PDCCH accept")
// is invisible at this project's usual phy_log_level=info -- confirmed live 2026-07-28 during
// Stage 1 validation (empty passive UE log despite the tap running). Every-slot occasions would
// flood at LOG_D's own level anyway, so this is a deliberate low-rate INFO counter, not a
// downgrade of the per-candidate line.
#define NR_PDCCH_BLIND_SUMMARY_PERIOD_OCC 1000
/* The periodic summary/BTIM lines used to fire every 1000 occasions, which on a 2026-09-17 capture
 * meant nothing for the first ~170 s (the summary block is only reached once the occasion runs to
 * completion) and then every 2 s: a 120 s validation capture printed no BTIM at all while a 600 s
 * one printed 204 near-identical lines. Gate them on wall-clock instead: once every 20 s, from the
 * first completed occasion. */
#define NR_PDCCH_BLIND_SUMMARY_PERIOD_NS 20000000000ull
static bool summary_due_now(void)
{
  static uint64_t s_last_ns = 0;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  if (s_last_ns != 0 && now - s_last_ns < NR_PDCCH_BLIND_SUMMARY_PERIOD_NS)
    return false;
  s_last_ns = now;
  return true;
}
static uint64_t    g_occasions_run  = 0;
static uint64_t    g_candidates_run = 0;
// Phase 3 autodiscover (2026-09-04): Technique C's dci_length sweep has succeeded, OR given up
// (see AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS). Guards nr_pdcch_dci_length_sweep_feed() from
// running every occasion forever -- see the autodiscover branch in
// nr_pdcch_blind_monitor_run_occasion() below.
static bool         g_length_swept  = false;
/* DISTINCT from g_length_swept, which is set by BOTH the success and the give-up branch and so
 * cannot tell them apart. Everything downstream of the DCI length is meaningless while the length
 * is wrong, because nothing genuine decodes.
 * MEASURED 2026-09-07, captures pdsch5_1_122600 and pdsch5_3_123646 -- two DEAF runs (49 and 113
 * genuine C-RNTI decodes against run 2's 217,838 in the same batch, this rig's documented bimodal
 * behaviour) in which the sweep was STARVED rather than wrong. The consequences were not confined
 * to the length: the extent verification then rejected BOTH extent candidates and reported "extent
 * NOT verified", blaming geometry for a signal outage, and Technique B confirmed a succession of
 * NOISE RNTIs (0xbaa8, then 0xa1bd / 0xf35e / 0xe2a6) which the acceptance-narrowing consumer
 * pinned to in turn. A deaf run must degrade to "no conclusion", never to a confident wrong one. */
static bool         g_length_found  = false;


/* Verified dedicated geometries remain operational while discovery continues for another UE's
 * CORESET. Entries are immutable after the release-store publishes them, so the receive producer
 * may test the count while the single scan consumer appends without a lock. */
#define NR_PDCCH_DISCOVERED_CORESETS 8
typedef struct {
  nr_pdcch_blind_monitor_cfg_t cfg;
  uint16_t owners[NR_PDCCH_BLIND_MAX_UE];
  uint8_t nowners;
} nr_pdcch_discovered_coreset_t;
static nr_pdcch_discovered_coreset_t g_coreset_bank[NR_PDCCH_DISCOVERED_CORESETS];
static _Atomic int g_coreset_bank_n;
/* Is this geometry already a verified bank entry? (stage 1-2 hand-off: a discovered CORESET the walk found
 * first must not be re-dwelled -- run s3live5 re-tested it, the alias rule retired it as "not verified",
 * and the walk resumed instead of pausing.) */
bool nr_pdcch_blind_monitor_bank_has_geometry(int rb_offset, int groups, int duration, int bundle, int interleaver,
                                               int shift, int nid)
{
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    const nr_pdcch_blind_monitor_cfg_t *b = &g_coreset_bank[i].cfg;
    if ((int)(b->bwp_start + b->coreset_rb_offset) == rb_offset && (int)b->coreset_freq_domain == groups
        && (int)b->coreset_duration == duration && (int)b->coreset_reg_bundle_size == bundle
        && (bundle == 0 || ((int)b->coreset_interleaver_size == interleaver && (int)b->coreset_shift_index == shift))
        && (int)b->coreset_pdcch_dmrs_scrambling_id == nid)
      return true;
  }
  return false;
}

static bool coreset_same_geometry(const nr_pdcch_blind_monitor_cfg_t *a,
                                  const nr_pdcch_blind_monitor_cfg_t *b)
{
  return a->bwp_start == b->bwp_start && a->bwp_size == b->bwp_size
      && a->coreset_rb_offset == b->coreset_rb_offset
      && a->coreset_freq_domain == b->coreset_freq_domain
      && a->coreset_duration == b->coreset_duration
      && a->coreset_reg_bundle_size == b->coreset_reg_bundle_size
      && a->coreset_interleaver_size == b->coreset_interleaver_size
      && a->coreset_shift_index == b->coreset_shift_index
      && a->coreset_pdcch_dmrs_scrambling_id == b->coreset_pdcch_dmrs_scrambling_id
      && a->ss_first_symbol == b->ss_first_symbol
      && a->dci_length_override == b->dci_length_override;
}

/* A banked operational scan may deliberately cover a wider RB interval than the observed
 * footprint. It covers any later hypothesis inside that interval when symbol, duration, DM-RS ID
 * and CCE-to-REG mapping agree. Skipping such hypotheses prevents discovery from repeatedly
 * rediscovering its first archived CORESET while still allowing a different mapping in the same
 * RBs to become a separate bank entry. */
static bool coreset_bank_covers(int rb_offset, int span_rb, int duration, int symbol,
                                int bundle, int interleaver, int shift, int dmrs_id)
{
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    const nr_pdcch_blind_monitor_cfg_t *b = &g_coreset_bank[i].cfg;
    const int bank_span = b->coreset_freq_domain * 6;
    if (b->coreset_rb_offset <= rb_offset
        && b->coreset_rb_offset + bank_span >= rb_offset + span_rb
        && b->coreset_duration == duration && b->ss_first_symbol == symbol
        && b->coreset_reg_bundle_size == bundle
        && b->coreset_interleaver_size == interleaver
        && b->coreset_shift_index == shift
        && b->coreset_pdcch_dmrs_scrambling_id == dmrs_id)
      return true;
  }
  return false;
}

static bool coreset_bank_has_owner(uint16_t rnti)
{
  if (!rnti)
    return false;
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < g_coreset_bank[i].nowners; ++j)
      if (g_coreset_bank[i].owners[j] == rnti)
        return true;
  return false;
}

/* A verified dedicated DCI length is a high-value cell prior for another CORESET, but not ground
 * truth: different UEs may have different BWPs/configurations. The fast catalog tests the modal
 * bank length for a bounded eight rounds; the exhaustive lap still tries every legal length. */
static int coreset_bank_length_hint(void)
{
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  int best = 0, best_count = 0;
  for (int i = 0; i < n; ++i) {
    const int len = g_coreset_bank[i].cfg.dci_length_override;
    if (len <= 0)
      continue;
    int count = 0;
    for (int j = 0; j < n; ++j)
      if (g_coreset_bank[j].cfg.dci_length_override == len)
        ++count;
    if (count > best_count) {
      best = len;
      best_count = count;
    }
  }
  return best;
}

static void coreset_bank_add(const nr_pdcch_blind_monitor_cfg_t *cfg, uint16_t owner)
{
  if (cfg == NULL || cfg->dci_length_override <= 0)
    return;
  int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  int at = -1;
  for (int i = 0; i < n; ++i)
    if (coreset_same_geometry(&g_coreset_bank[i].cfg, cfg)) { at = i; break; }
  if (at < 0) {
    /* A narrow CCE-compatible subset can decode the same UE as its already banked CORESET. It is
     * useful evidence but not a second independent configuration; retaining every such alias can
     * fill the bounded bank before another UE is reached. */
    if (coreset_bank_has_owner(owner))
      return;
    if (n >= NR_PDCCH_DISCOVERED_CORESETS) {
      LOG_W(PHY, "SENSING: multi-CORESET bank full (%d); verified geometry left unarchived\n", n);
      return;
    }
    at = n;
    memset(&g_coreset_bank[at], 0, sizeof(g_coreset_bank[at]));
    g_coreset_bank[at].cfg = *cfg;
    g_coreset_bank[at].cfg.autodiscover = 0;
    g_coreset_bank[at].cfg.ss_monitoring_slot_periodicity = 1;
    g_coreset_bank[at].cfg.ss_monitoring_slot_offset = 0;
    g_coreset_bank[at].cfg.ss_duration = 1;
    atomic_store_explicit(&g_coreset_bank_n, n + 1, memory_order_release);
    LOG_A(PHY, "SENSING: multi-CORESET bank add index=%d offset=%d span=%d symbol=%d mapping=%d/%d/%d len=%d\n",
          at, cfg->coreset_rb_offset, cfg->coreset_freq_domain * 6, cfg->ss_first_symbol,
          cfg->coreset_reg_bundle_size, cfg->coreset_interleaver_size, cfg->coreset_shift_index,
          cfg->dci_length_override);
  }
  nr_pdcch_discovered_coreset_t *e = &g_coreset_bank[at];
  for (int i = 0; i < e->nowners; ++i)
    if (e->owners[i] == owner) return;
  if (owner && e->nowners < NR_PDCCH_BLIND_MAX_UE)
    e->owners[e->nowners++] = owner;
}

/* Manual/auto is authoritative: disabled means no hypothesis application or scoring. */
static bool g_pdsch_sweep_on;

/* ---- DCI 1_1 LAYOUT CONSISTENCY (stage 1 of nr_pdcch_dci11_layout_sweep) ---------------------
 * The configured field layout is a set of ASSUMPTIONS. A wrong one still yields CRC-valid DCIs
 * with plausible-looking allocations -- the RNTI comes from the CRC and not the payload, and the
 * RIV precedes most of the fields that can be misplaced -- so nothing looks wrong except that no
 * transport block decodes. That is precisely how a whole campaign was lost to bwp_indicator 1->0
 * and time_domain_assignment 4->2 while the TOTAL length was right.
 *
 * Stage 1 costs NO decode: it reads the payload only. It enumerates every layout consistent with
 * the observed DCI length, drops the ones the payloads contradict, and reports whether the
 * CONFIGURED layout is still among the survivors. It cannot repair the layout on its own --
 * rotating the extractor across candidates is stage 2 and needs air to validate -- but it converts
 * a silent wrong assumption into a loud one, which is the expensive half of that failure.
 *
 * Lazily armed: bwp_size and the DCI length are not known until the monitor is configured. */
static nr_dci11_resolver_t g_dci11_resolver;
static int      g_dci11_state;   /* 0 = not tried, 1 = armed, -1 = unavailable */
static uint64_t g_dci11_seen;

static inline int nr_pdcch_ss_bucket(const nr_pdcch_blind_monitor_cfg_t *cfg);

/* STAGE 0: PDCCH data-scrambling n_RNTI as a HYPOTHESIS (TS 38.211 7.3.2.3, c_init = (n_RNTI << 16)
 * + n_ID). n_RNTI is the C-RNTI in a UE-specific search space whose CORESET carries
 * pdcch-DMRS-ScramblingID, else 0 -- and that RRC field is ciphered, so a passive receiver cannot
 * know which. Both are therefore tested, on USS/PDCCH-Config occasions only (callers gate that):
 *   H = {0} U {every RNTI the receiver itself has confirmed}; one hypothesis per occasion, chosen by a
 *   hash of the slot (a plain modulo aliases against TDD/monitoring periodicity and could starve a
 *   hypothesis forever).
 * Evidence, recorded per hypothesis and needing no ground truth:
 *   n_RNTI = R != 0 : accepts whose CRC-recovered RNTI == R. Under the right hypothesis the UE's own
 *                     DCIs decode with their own RNTI; under a wrong one nothing decodes consistently.
 *   n_RNTI = 0      : accepts whose RNTI is receiver-confirmed (recurrent).
 * ISAC_PDCCH_NRNTI=0 forces 0 (regression control only). There is deliberately no way to inject a
 * known C-RNTI: that was a test fixture and it is gone.
 * ponytail: uniform rotation, i.e. pure exploration; weight toward the winning hypothesis once the
 * evidence has been validated on a cell where n_RNTI != 0. */
#define STAGE0_MAXH 17
static struct {
  uint32_t slot;
  uint16_t nrnti;
} s_stage0_ring[1024];
static struct {
  uint16_t nrnti;
  uint64_t occ, acc, match;
} s_stage0_h[STAGE0_MAXH];
static pthread_mutex_t s_stage0_mu = PTHREAD_MUTEX_INITIALIZER;

static int stage0_slot_of(uint16_t nrnti) /* caller holds s_stage0_mu */
{
  for (int i = 0; i < STAGE0_MAXH; i++)
    if (s_stage0_h[i].occ && s_stage0_h[i].nrnti == nrnti)
      return i;
  for (int i = 0; i < STAGE0_MAXH; i++)
    if (!s_stage0_h[i].occ) {
      s_stage0_h[i].nrnti = nrnti;
      return i;
    }
  return -1;
}

static uint16_t nr_pdcch_nrnti_override(uint32_t abs_slot)
{
  static int s_force0 = -1;
  if (s_force0 < 0) {
    const char *e = getenv("ISAC_PDCCH_NRNTI");
    s_force0 = (e != NULL && strcmp(e, "0") == 0) ? 1 : 0;
    LOG_A(PHY, "SENSING: STAGE0 n_RNTI %s\n", s_force0 ? "forced to 0 (regression control)"
                                                       : "hypothesis test: {0} U confirmed RNTIs per USS occasion");
  }
  if (s_force0)
    return 0;
  /* Decided: once n_RNTI=0 has >= 200 confirmed accepts and every other hypothesis with >= 1000 occasions
   * self-matches at < 1/50 of that rate, n_RNTI=0 is the answer for this cell (a per-CORESET property, so
   * later UEs inherit it). Rotating on would descramble 1/(1+k) of all USS occasions with a wrong n_RNTI and
   * lose every grant in them (2 UEs: 2/3 of the occasions). */
  static int s_decided = -1;
  if (s_decided == 0)
    return 0;
  uint16_t h[STAGE0_MAXH];
  h[0] = 0;
  const int n = 1 + nr_pdcch_blind_monitor_confirmed_rnti_set(abs_slot, h + 1, STAGE0_MAXH - 1);
  const uint16_t pick = h[(uint32_t)(((uint64_t)abs_slot * 2654435761u) >> 16) % (uint32_t)n];
  pthread_mutex_lock(&s_stage0_mu);
  const int r = abs_slot & 1023;
  if (s_stage0_ring[r].slot != abs_slot + 1) { /* first call this slot (+1 so slot 0 != empty) */
    s_stage0_ring[r].slot = abs_slot + 1;
    s_stage0_ring[r].nrnti = pick;
    const int i = stage0_slot_of(pick);
    if (i >= 0)
      s_stage0_h[i].occ++;
    static uint64_t s_n;
    if ((s_n % 1000) == 999 && s_decided < 0) {
      int z = -1;
      for (int k = 0; k < STAGE0_MAXH; k++)
        if (s_stage0_h[k].occ && s_stage0_h[k].nrnti == 0) z = k;
      if (z >= 0 && s_stage0_h[z].match >= 200) {
        const double r0 = (double)s_stage0_h[z].match / (double)s_stage0_h[z].occ;
        bool decisive = true;
        for (int k = 0; k < STAGE0_MAXH; k++)
          if (k != z && s_stage0_h[k].occ >= 1000
              && 50.0 * (double)s_stage0_h[k].match / (double)s_stage0_h[k].occ >= r0)
            decisive = false;
        if (decisive) {
          s_decided = 0;
          LOG_A(PHY, "SENSING: STAGE0 DECIDED n_RNTI=0 for this cell (confirmed %lu / %lu occasions; every other "
                     "hypothesis self-matches at < 1/50 of that rate) -- rotation stopped\n",
                (unsigned long)s_stage0_h[z].match, (unsigned long)s_stage0_h[z].occ);
        }
      }
    }
    if ((++s_n % 4000) == 0) {
      char b[600];
      int u = 0;
      for (int k = 0; k < STAGE0_MAXH && u < (int)sizeof(b) - 60; k++)
        if (s_stage0_h[k].occ)
          u += snprintf(b + u, sizeof(b) - u, "n_RNTI=0x%04x:occ=%lu,acc=%lu,%s=%lu  ", s_stage0_h[k].nrnti,
                        (unsigned long)s_stage0_h[k].occ, (unsigned long)s_stage0_h[k].acc,
                        s_stage0_h[k].nrnti ? "self" : "confirmed", (unsigned long)s_stage0_h[k].match);
      LOG_A(PHY, "SENSING: STAGE0 %s\n", b);
    }
  }
  const uint16_t used = s_stage0_ring[r].nrnti; /* all three call sites of one slot agree */
  pthread_mutex_unlock(&s_stage0_mu);
  return used;
}

/* Credit an accept to the n_RNTI hypothesis its slot was descrambled with. */
static void stage0_note_accept(uint32_t abs_slot, uint16_t rnti, bool confirmed)
{
  pthread_mutex_lock(&s_stage0_mu);
  const int r = abs_slot & 1023;
  if (s_stage0_ring[r].slot == abs_slot + 1) {
    const int i = stage0_slot_of(s_stage0_ring[r].nrnti);
    if (i >= 0) {
      s_stage0_h[i].acc++;
      if (s_stage0_h[i].nrnti ? (rnti == s_stage0_h[i].nrnti) : confirmed)
        s_stage0_h[i].match++;
    }
  }
  pthread_mutex_unlock(&s_stage0_mu);
}


/* Every PDSCH field an RA-class grant is decoded with. Printed for the first 8 RA grants of a run so
 * that a working configuration and a broken one can be diffed field by field instead of guessed at
 * -- the failure mode here is a CRC that fails for ALL of them, which says nothing about WHICH
 * field is wrong. RA class only, 8 lines, so it costs nothing during a capture. */
static void ragrant_dump(const fapi_nr_dl_config_dlsch_pdu_rel15_t *pdu,
                         const nr_pdcch_blind_result_t *out, int mcs_table, bool css0, const char *path)
{
  if (out->rnti_class != NR_BLIND_RNTI_CLASS_RA) {
    return;
  }
  static _Atomic unsigned long s_n = 0;
  if (atomic_fetch_add_explicit(&s_n, 1, memory_order_relaxed) >= 8) {
    return;
  }
  LOG_A(PHY,
        "SENSING: RAGRANT %s css0=%d rnti=0x%x bwp=%u+%u rb=%u+%u sym=%u+%u dmrsmask=0x%x refpt=%u "
        "cdm=%u type=%d nscid=%u dmrs_scr=%u data_scr=%u ports=0x%x mcs=%u tbl=%d rv=%u tbscale=%u\n",
        path, (int)css0, (unsigned)out->rnti, (unsigned)pdu->BWPStart, (unsigned)pdu->BWPSize,
        (unsigned)pdu->start_rb, (unsigned)pdu->number_rbs, (unsigned)pdu->start_symbol,
        (unsigned)pdu->number_symbols, (unsigned)pdu->dlDmrsSymbPos, (unsigned)pdu->refPoint,
        (unsigned)pdu->n_dmrs_cdm_groups, (int)pdu->dmrsConfigType, (unsigned)pdu->nscid,
        (unsigned)pdu->dlDmrsScramblingId, (unsigned)pdu->dlDataScramblingId,
        (unsigned)pdu->dmrs_ports, (unsigned)out->mcs, mcs_table, (unsigned)out->rv,
        (unsigned)out->tb_scaling);
}


/* See the header note on GRANTDROP. Rate-limited per reason so a persistent gate logs once and then
 * every 500th time, which is enough to see it without flooding a capture. */
static void grantdrop(const nr_pdcch_blind_result_t *out, int frame, int slot, const char *why)
{
  if (out->rnti_class != NR_BLIND_RNTI_CLASS_SI && out->rnti_class != NR_BLIND_RNTI_CLASS_RA
      && out->rnti_class != NR_BLIND_RNTI_CLASS_TC) {
    return;
  }
  static _Atomic unsigned long s_n = 0;
  const unsigned long n = atomic_fetch_add_explicit(&s_n, 1, memory_order_relaxed) + 1;
  if (n <= 20 || (n % 500) == 0) {
    LOG_A(PHY, "SENSING: GRANTDROP (%d.%d) rnti=0x%x class=%d why=%s (n=%lu)\n",
          frame, slot, (unsigned)out->rnti, (int)out->rnti_class, why, n);
  }
}

static uint64_t g_pdsch_configuration;
/* ---- DCI 1_1 layout, STAGE 2 (ISAC_DCI11_STAGE2=1, default off). Every layout the stage-1
 * resolver still holds alive is turned into extract widths (nr_dci11_layout_to_field_bits, which is
 * offset-identical to the layout by construction -- nr_dci11_layout_apply_roundtrip) and parsed. Each
 * parse is a candidate allocation; the existing Technique-D machinery below gives every candidate its
 * own TB-CRC-scored context (key = configuration ^ layout id) and the settled/preferred/round-robin
 * selection promotes the one that decodes. TB CRC remains the only authority. The hand-picked
 * 3-family enumeration is the fallback while stage 1 is not armed. */
#define NR_DCI11_STAGE2_MAX_ALIVE 8   /* hand over once stage 1 is down to this many */
/* While the configured layout is still among stage 1's survivors, hand over only at 4 (the measured
 * dilution limit); once stage 1 has REFUTED it, waiting is pointless -- the hand-picked fallback
 * enumeration hard-codes antenna_ports=4 bits and can never contain the truth. OTA 2026-09-15 on the
 * rank-4 cell: 8 survivors, configured layout dead, 0/3793 TB CRC for the whole run. */
static int g_dci11_cfg_alive = 1;
static _Atomic int g_dl_layout_preferred; /* a DL layout family has >= 8 code-block CRC passes */
/* DL LINK HEALTH for FDRA staging: a CRC pass on a PDSCH decoded OUTSIDE the stage-2 layout search
 * (format 1_0 -- SIB1/RA/paging/C-RNTI fallback -- or the manual/fallback layouts), stamped with the
 * stage-2 trial count at which it arrived. Staging arms only while such a pass is recent, so a dead link
 * (CFO mis-lock, Technique D still searching) cannot cascade-arm every mode on its 0-pass reads. */
static _Atomic uint64_t g_dl_layout_trials;          /* stage-2 trials fed back so far */
static _Atomic uint64_t g_dl_link_pass_at = UINT64_MAX; /* g_dl_layout_trials at the last link pass */
static _Atomic int g_dl_type1_pass = -1;              /* a type-1 layout that passed: disarm (observer applies it) */
void nr_pdcch_dci11_layout_feedback(uint16_t layout_index, bool cb0_ok)
{
  if (layout_index >= NR_DCI11_LAYOUT_MAX) {
    if (cb0_ok) /* 0xFFFF: not a stage-2 trial -- a link-health pass */
      atomic_store_explicit(&g_dl_link_pass_at, atomic_load_explicit(&g_dl_layout_trials, memory_order_relaxed),
                            memory_order_relaxed);
    return;
  }
  atomic_fetch_add_explicit(&g_dl_layout_trials, 1, memory_order_relaxed);
  if (cb0_ok && layout_index < __atomic_load_n(&g_dci11_resolver.n_hyp, __ATOMIC_ACQUIRE)
      && g_dci11_resolver.off[layout_index].fdra_mode == NR_FDRA_TYPE1 && g_dci11_resolver.fdra_next > 1
      && g_dci11_resolver.fdra_next < NR_DCI11_FDRA_STAGES)
    atomic_store_explicit(&g_dl_type1_pass, (int)layout_index, memory_order_relaxed);
  /* consumer thread vs the receive thread's reads: plain counters, a torn read costs one tally */
  __atomic_fetch_add(&g_dci11_resolver.probe_tr[layout_index], 1u, __ATOMIC_RELAXED);
  if (cb0_ok)
    __atomic_fetch_add(&g_dci11_resolver.probe_ok[layout_index], 1u, __ATOMIC_RELAXED);
  const uint16_t fam = g_dci11_resolver.layout_fam[layout_index] % NR_DCI11_FAM_N;
  __atomic_fetch_add(&g_dci11_resolver.fam_tr[fam], 1u, __ATOMIC_RELAXED);
  if (cb0_ok)
    __atomic_fetch_add(&g_dci11_resolver.fam_ok[fam], 1u, __ATOMIC_RELAXED);
}
/* Evidence for a layout: its own probe passes or those of the interpretation family it last read. */
static inline uint32_t dci11_layout_evidence(const nr_dci11_resolver_t *r, int i, uint32_t *tr)
{
  const uint32_t fo = r->fam_ok[r->layout_fam[i] % NR_DCI11_FAM_N];
  if (tr) *tr = r->probe_tr[i] > r->fam_tr[r->layout_fam[i] % NR_DCI11_FAM_N] ? r->probe_tr[i] : r->fam_tr[r->layout_fam[i] % NR_DCI11_FAM_N];
  return r->probe_ok[i] > fo ? r->probe_ok[i] : fo;
}
static inline uint16_t dci11_family_key(const nr_pdcch_blind_result_t *p)
{
  uint32_t k = 2166136261u;
  const uint32_t v[] = {p->start_rb, p->num_rb, p->tda_index, p->mcs, p->rv, p->ndi, p->harq_pid, p->dmrs_ports, p->nscid, p->n_dmrs_cdm_groups,
                        p->ra_type0, p->rbg_bitmap};
  for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) k = (k ^ v[i]) * 16777619u;
  return (uint16_t)(k % NR_DCI11_FAM_N);
}
static int nr_pdcch_dci11_stage2_enabled(void)
{
  static int s_on = -1;
  if (s_on < 0) {
    const char *e = getenv("ISAC_DCI11_STAGE2");
    s_on = (e != NULL && atoi(e) != 0) ? 1 : 0;
  }
  return s_on;
}
static int nr_pdcch_dci11_stage2_candidates(const nr_pdcch_blind_raw_result_t *raw, uint16_t len,
                                            const nr_pdcch_blind_monitor_cfg_t *cfg,
                                            nr_pdcch_blind_result_t *out, uint16_t *ids, int max)
{
  const nr_dci11_resolver_t *r = &g_dci11_resolver;
  /* DILUTION GATE. Every candidate offered here gets its own Technique-D context, so N candidates
   * means each sees 1/N of the grants and none reaches min_trials -- the same failure the
   * layout-family preference below was built for, measured at N=14 (OTA 2026-09-12: DL CRC 0.3 %).
   * Stage 1 prunes on plausibility at no decode cost, so wait until it has: hand over only when the
   * live set is small enough to converge, and run the hand-picked enumeration until then. */
  /* Stage 2 drives at ANY live count now: above the hand-over limit the trials are first-code-block
   * PROBES (job.layout_probe, ~1/C of a full decode), so a wide set converges instead of waiting. */
  (void)g_dci11_cfg_alive;
  int count = 0;
  /* Per-layout scratch on the HEAP, one set per thread: NR_DCI11_LAYOUT_MAX-sized `static __thread`
   * arrays (128 kB at 8192) grow the static TLS block, and a shifted TLS layout is what produced this
   * codebase's AVX alignment fault (see the per-antenna CFR buffer). */
  static __thread int *order, *rot;
  static __thread double *sc;
  if (order == NULL) {
    order = malloc(NR_DCI11_LAYOUT_MAX * sizeof(*order));
    rot = malloc(NR_DCI11_LAYOUT_MAX * sizeof(*rot));
    sc = malloc(NR_DCI11_LAYOUT_MAX * sizeof(*sc));
    AssertFatal(order != NULL && rot != NULL && sc != NULL, "DCI 1_1 stage-2 scratch allocation failed\n");
  }
  int no = 0;
  const int nh = __atomic_load_n(&r->n_hyp, __ATOMIC_ACQUIRE); /* the observer appends and publishes with release */
  for (int i = 0; i < nh; i++)
    if (r->alive[i]) { sc[i] = nr_dci11_resolver_score(r, i); order[no++] = i; }
  if (no <= NR_DCI11_STAGE2_MAX_ALIVE) {
    for (int a = 1; a < no; a++)          /* insertion sort: a small set is offered best-scored first */
      for (int b = a; b > 0 && sc[order[b]] > sc[order[b - 1]]; b--) {
        const int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
      }
  } else {
    /* WIDE SET: ROTATE, do not rank. Measured on the rank-4 bed (883 live, constant phy-test DCI):
     * every offset reads a constant field, so the distributional score ranks nothing, and offering
     * the same top-8 by that score on every grant meant the true layout was never tried (12k probes,
     * 0 hits). A rotating window gives every live layout its probe within n_alive/max grants. */
    static _Atomic uint32_t s_rot;
    const uint32_t start = atomic_fetch_add_explicit(&s_rot, (uint32_t)max, memory_order_relaxed) % (uint32_t)no;
    for (int k = 0; k < no; k++) sc[order[k]] = 0.0; /* order[] rotated below; scores unused */
    /* EXPLOIT FIRST: a layout whose probes have already passed code block 0 goes to the head of
     * every window, so one lucky hit turns into a settled layout within seconds instead of waiting
     * for the rotation to come round again (809 live x ~6 PDSCH hypotheses: 1 hit per ~3000 probes
     * on the rank-4 bed, 2026-09-16). Everything else keeps rotating behind it. */
    int nh = 0;
    uint32_t hot_ok[NR_DCI11_STAGE2_MAX_ALIVE + 3];
    for (int k = 0; k < no; k++) {
      const int i = order[k];
      const uint32_t ok = dci11_layout_evidence(r, i, NULL); /* own or family evidence */
      if (ok == 0)
        continue;
      /* keep the `max` most-passed, sorted: the leader must be in every window once preferred */
      int pos = nh < max ? nh : max - 1;
      if (nh >= max && ok <= hot_ok[pos])
        continue;
      while (pos > 0 && hot_ok[pos - 1] < ok) { rot[pos] = rot[pos - 1]; hot_ok[pos] = hot_ok[pos - 1]; pos--; }
      rot[pos] = i; hot_ok[pos] = ok;
      if (nh < max) nh++;
    }
    for (int k = 0, w = nh; k < no && w < no; k++) {
      const int i = order[(start + k) % no];
      bool hot = false;
      for (int h = 0; h < nh; h++) hot |= (rot[h] == i);
      if (!hot) rot[w++] = i;
    }
    memcpy(order, rot, (size_t)no * sizeof(order[0]));
  }
  /* Above the hand-over limit only `max` of the live set fit one grant's trial list; rotate the
   * window over the score-sorted list so every hypothesis gets probed, best ones most often. */
  static __thread int s_rot = 0;
  /* The wide set was already rotated (hot layouts at its head): offer from 0 there. */
  const int start = (no > max && no <= NR_DCI11_STAGE2_MAX_ALIVE) ? (s_rot++ % (no - max + 1)) : 0;
  for (int oi = start; oi < no && count < max; oi++) {
    const int i = order[oi];
    nr_dci11_field_bits_t f;
    if (!nr_dci11_layout_to_field_bits(&r->hyp[i], &f))
      continue;
    nr_pdcch_blind_extract_opts_t o = {0};
    o.bwp_indicator_bits = f.bwp_indicator_bits;
    o.vrb_to_prb_bits    = f.vrb_to_prb_bits;
    o.prb_bundling_bits  = f.prb_bundling_bits;
    o.rate_matching_bits = f.rate_matching_bits;
    o.zp_csirs_bits      = f.zp_csirs_bits;
    o.tb2_bits           = f.tb2_bits;
    o.harq_pid_bits      = f.harq_pid_bits;
    o.dai_bits           = f.dai_bits;
    o.pdsch_to_harq_bits = f.pdsch_to_harq_bits;
    o.antenna_ports_bits = f.antenna_ports_bits;
    o.dmrs_config_type   = f.dmrs_config_type;
    o.tci_bits           = f.tci_bits;
    o.srs_request_bits   = f.srs_request_bits;
    o.cbg_bits           = f.cbg_bits;
    o.fdra_mode          = f.fdra_mode;       /* RA type 0 / dynamicSwitch move every later field */
    o.fdra_bwp_start     = cfg->bwp_start;    /* the grid the resolver's N_RBG was computed on */
    /* The TDA width is THIS hypothesis' (searched); a configured list still pins it. tda_count is
     * the largest count that width can express; S/L are the scaffold Technique D replaces. */
    const uint8_t htb = r->off[i].tda_bits;
    o.tda_count          = (cfg->extract.tda_count > 0) ? cfg->extract.tda_count : (htb == 0 ? 1 : (1 << htb));
    o.dmrs_add_pos       = 0;
    o.dmrs_max_length    = 1;
    for (int k = 0; k < o.tda_count && k < 16; ++k) {
      o.tda_start[k] = 1; o.tda_length[k] = 13; o.tda_mapping[k] = 0; // scaffold: Technique D replaces S/L
    }
    static __thread uint32_t s_sz_mismatch, s_rejected, s_diag_n;
    if (nr_pdcch_blind_dci_size_ex(cfg->bwp_size, &o) != len) {
      s_sz_mismatch++;
      continue; // the resolver and the extractor disagree on this layout's length: not a candidate
    }
    nr_pdcch_blind_result_t parsed;
    if (!nr_pdcch_blind_extract_11(raw, len, cfg->bwp_size, cfg->dmrs_typeA_position, &o, &parsed)) {
      s_rejected++;
      if ((++s_diag_n % 5000) == 1)
        LOG_A(PHY, "SENSING: STAGE2 reject: layout %d (tda_bits %u ap %u bwp %u) len_ex=%u: %s [size_mismatch=%u rejected=%u]\n", i,
              r->off[i].tda_bits, f.antenna_ports_bits, f.bwp_indicator_bits, nr_pdcch_blind_dci_size_ex(cfg->bwp_size, &o),
              parsed.reject_reason ? parsed.reject_reason : "?", s_sz_mismatch, s_rejected);
      continue;
    }
    g_dci11_resolver.layout_fam[i] = dci11_family_key(&parsed); /* what this layout reads on this DCI */
    out[count] = parsed;
    ids[count++] = (uint16_t)i; /* resolver index: up to 2048, a uint8_t wrapped it and merged
                                 * the sweep contexts / evidence of layouts 256 apart */
  }
  return count;
}

/* ---- DCI 0_1 layout, stage 1 (observe-only, mirrors the 1_1 observer below). The uplink grant's
 * field widths are set by RRC switches this receiver cannot read; every layout whose total equals
 * the observed 0_1 length is a hypothesis and each accepted payload prunes by plausibility. */
static nr_dci11_resolver_t g_dci01_resolver;
static int g_dci01_state = 0;   /* 0 = not armed, 1 = armed, -1 = no legal layout at this length */
static uint64_t g_dci01_seen = 0;
/* ---- DCI 0_1 FDRA MODE STAGING. Only type 1 is searched at first. Stage 1 cannot tell a type-0 cell
 * from a type-1 one on the uplink: a type-1 window that starts on the constant-zero identifier / UL-SUL /
 * BWP bits always reads a RIV inside the BWP, so type-1 layouts SURVIVE on a type-0 truth (measured by
 * Dci01Fdra.TypeZeroTruthWithConstantLeadingBits...: 144 of 208 alive at 106 PRB). The oracle is the TB
 * CRC of PUSCH decoded from DCI 0_1 grants read under converged, non-discovery widths only; DCI 0_0 and
 * discovery-hypothesis passes are LINK HEALTH (nr_dci01_fdra_evidence_t). With the link healthy and that
 * oracle 0/NR_DCI11_FDRA_ARM_MIN_TRIALS, the RA type 0 / dynamicSwitch 0_1 layouts are armed and oracle-class
 * 0_1 grants REFUSED (UL_FDRA_REFUSED): the UL extractor reads a RIV of the configured width, so they would
 * be decoded at the wrong PRBs and offsets, and RA type 0 PUSCH is not decodable here (nr_rx_pusch_group_tp
 * takes rb_start/rb_size only). 0_0 and discovery grants stay booked (UL DM-RS CFR continues); one refused
 * grant in NR_DCI01_FDRA_PROBE_EVERY is still booked, and one oracle pass ends the refusal for good.
 * KNOWN LIMIT: a type-1 cell whose 0_1 PUSCH fails for another reason (e.g. MCS-limited decode) while 0_0
 * decodes satisfies the same evidence and is refused too -- the probe keeps checking. ---- */
static pthread_mutex_t g_dci01_fdra_lock = PTHREAD_MUTEX_INITIALIZER;
static nr_dci01_fdra_evidence_t g_dci01_fdra_ev;  /* under g_dci01_fdra_lock */
static _Atomic int g_dci01_fdra_refuse;          /* 1 = refuse oracle-class 0_1 booking */
static _Atomic unsigned long g_ul_fdra_refused;
static int g_dci01_fdra_armed;
static uint16_t g_dci01_riv_bits, g_dci01_bwp_start, g_dci01_bwp_size;
static uint8_t g_dci01_tda_bits;
static nr_dci01_layout_t g_dci01_hyp[NR_DCI01_LAYOUT_MAX];
static nr_dci11_offsets_t g_dci01_off[NR_DCI01_LAYOUT_MAX];
static inline bool dci01_oracle_grant(const nr_pdcch_blind_ul_result_t *g)
{
  return nr_dci01_fdra_oracle_grant(g->ul_dci_format, g->width_hyp_class, g->interp_hyp_class);
}
void nr_pdcch_dci01_fdra_feedback(const nr_pdcch_blind_ul_result_t *g, bool tb_crc_ok)
{
  const bool oracle = dci01_oracle_grant(g);
  pthread_mutex_lock(&g_dci01_fdra_lock);
  nr_dci01_fdra_note(&g_dci01_fdra_ev, oracle, tb_crc_ok);
  pthread_mutex_unlock(&g_dci01_fdra_lock);
  if (oracle && tb_crc_ok)
    atomic_store_explicit(&g_dci01_fdra_refuse, 0, memory_order_relaxed); /* type 1 proven */
}
static void nr_pdcch_dci01_fdra_stage(bool periodic)
{
  pthread_mutex_lock(&g_dci01_fdra_lock);
  const nr_dci01_fdra_evidence_t ev = g_dci01_fdra_ev;
  pthread_mutex_unlock(&g_dci01_fdra_lock);
  int v = nr_dci01_fdra_verdict(&ev, g_dci01_fdra_armed, NULL);
  if (v == NR_DCI01_FDRA_ARM) {
    g_dci01_fdra_armed = 1;
    int n = 0;
    for (uint8_t m = NR_FDRA_TYPE0_CFG1; m <= NR_FDRA_DYN_CFG2 && n < NR_DCI01_LAYOUT_MAX; m++) {
      const int k = nr_dci01_layout_enumerate_mode(g_dci01_riv_bits, g_dci01_tda_bits, g_dci01_resolver.observed_len,
                                                   g_dci01_bwp_start, g_dci01_bwp_size, m, g_dci01_hyp + n,
                                                   g_dci01_off + n, NR_DCI01_LAYOUT_MAX - n);
      n += (k > 0) ? k : 0;
    }
    const int added = nr_dci_resolver_append_offsets(&g_dci01_resolver, g_dci01_off, n);
    LOG_A(PHY, "SENSING: DCI01_LAYOUT 0_1 type-1 PUSCH TB CRC 0/%u with the link healthy (%u other UL passes): armed "
               "RA type 0 / dynamicSwitch 0_1 layouts (%d added%s)\n",
          ev.t1_try, ev.link_ok, added, (added < n || n >= NR_DCI01_LAYOUT_MAX) ? ", TRUNCATED" : "");
    periodic = true;
  }
  if (!periodic)
    return; /* the alive scan below is O(n_hyp): run it every 256 observations, not per grant */
  v = nr_dci01_fdra_verdict(&ev, g_dci01_fdra_armed, &g_dci01_resolver);
  const int refuse = (v == NR_DCI01_FDRA_REFUSE);
  if (refuse != atomic_exchange_explicit(&g_dci01_fdra_refuse, refuse, memory_order_relaxed))
    LOG_A(PHY, "SENSING: DCI01_LAYOUT oracle-class 0_1 booking %s (0_1 type-1 PUSCH TB CRC %u/%u, other UL passes %u)\n",
          refuse ? "REFUSED -- a non-type-1 FDRA is the leading explanation" : "resumed", ev.t1_ok, ev.t1_try, ev.link_ok);
}
static void nr_pdcch_dci01_layout_observe(uint16_t ul_bwp_start, uint16_t ul_bwp_size, int ul_tda_count,
                                          uint16_t dci_length, uint64_t payload)
{
  if (g_dci01_state < 0 || dci_length == 0 || ul_bwp_size == 0) {
    return;
  }
  if (g_dci01_state == 0) {
    const double span = ((double)ul_bwp_size * (double)(ul_bwp_size + 1)) / 2.0;
    const uint16_t riv_bits = (uint16_t)ceil(log2(span));
    uint8_t tda_bits = 4;
    if (ul_tda_count > 0) {
      tda_bits = 0;
      while ((1 << tda_bits) < ul_tda_count) {
        tda_bits++;
      }
    }
    /* Type 1 only: the other FDRA modes are armed by nr_pdcch_dci01_fdra_stage() on TB-CRC evidence --
     * unless no type-1 layout fits this length at all, when the type-1 read is certainly wrong and they
     * are armed at once (and UL booking refused by the next verdict). */
    int n = nr_dci01_layout_enumerate(riv_bits, tda_bits, dci_length, g_dci01_hyp, g_dci01_off, NR_DCI01_LAYOUT_MAX);
    if (n == 0) {
      for (uint8_t m = NR_FDRA_TYPE0_CFG1; m <= NR_FDRA_DYN_CFG2 && n < NR_DCI01_LAYOUT_MAX; m++) {
        const int k = nr_dci01_layout_enumerate_mode(riv_bits, tda_bits, dci_length, ul_bwp_start, ul_bwp_size, m,
                                                     g_dci01_hyp + n, g_dci01_off + n, NR_DCI01_LAYOUT_MAX - n);
        n += (k > 0) ? k : 0;
      }
      g_dci01_fdra_armed = (n > 0);
    }
    if (n <= 0 || nr_dci_resolver_init_from_offsets(&g_dci01_resolver, ul_bwp_size, g_dci01_off, n) <= 0) {
      LOG_W(PHY, "SENSING: DCI01_LAYOUT no legal layout sums to dci_length=%u at ul_bwp_size=%u "
                 "tda_bits=%u -- one of those three is wrong for this cell\n",
            dci_length, (unsigned)ul_bwp_size, tda_bits);
      g_dci01_state = -1;
      return;
    }
    if (n >= NR_DCI01_LAYOUT_MAX)
      LOG_W(PHY, "SENSING: DCI01_LAYOUT initial set truncated at NR_DCI01_LAYOUT_MAX=%d\n", NR_DCI01_LAYOUT_MAX);
    g_dci01_riv_bits = riv_bits;
    g_dci01_tda_bits = tda_bits;
    g_dci01_bwp_start = ul_bwp_start;
    g_dci01_bwp_size = ul_bwp_size;
    if (ul_tda_count > 0 && ul_tda_count < 16)
      nr_dci11_resolver_set_tda_count(&g_dci01_resolver, (uint8_t)ul_tda_count);
    LOG_I(PHY, "SENSING: DCI01_LAYOUT armed: %d %s layouts consistent with dci_length=%u (riv=%u bits, tda=%u bits)\n",
          n, g_dci01_fdra_armed ? "RA type 0 / dynamicSwitch (no type-1 layout fits)" : "type-1", dci_length, riv_bits,
          tda_bits);
    g_dci01_state = 1;
  }
  nr_dci11_resolver_observe(&g_dci01_resolver, payload);
  const bool periodic = (++g_dci01_seen % 256) == 0 || g_dci01_seen == 1;
  nr_pdcch_dci01_fdra_stage(periodic);
  if ((g_dci01_seen % 4000) == 0) {
    pthread_mutex_lock(&g_dci01_fdra_lock);
    const nr_dci01_fdra_evidence_t ev = g_dci01_fdra_ev;
    pthread_mutex_unlock(&g_dci01_fdra_lock);
    LOG_A(PHY, "SENSING: DCI01_LAYOUT n=%llu observed | %d of %d layouts still plausible | 0_1 type-1 PUSCH TB CRC %u/%u, "
               "other UL passes %u | fdra %s | UL_FDRA_REFUSED=%lu\n",
          (unsigned long long)g_dci01_seen, g_dci01_resolver.n_alive, g_dci01_resolver.n_hyp, ev.t1_ok, ev.t1_try, ev.link_ok,
          atomic_load_explicit(&g_dci01_fdra_refuse, memory_order_relaxed) ? "refusing" : (g_dci01_fdra_armed ? "armed" : "type-1"),
          atomic_load_explicit(&g_ul_fdra_refused, memory_order_relaxed));
  }
}

/* ---- CORESET / search-space registry, observe-only. Each configuration the monitor scans is
 * registered on first sight; every occasion and accept is attributed to it; a CONFIRMED accept is
 * one whose RNTI repeated (noise does not repeat). The retire verdict is LOGGED, not acted on. */
static nr_pdcch_ss_registry_t g_ss_reg;
static int g_ss_reg_idx[2] = {-1, -1};
static uint16_t g_ss_recent_rnti[64];
static unsigned g_ss_recent_w = 0;
static int nr_pdcch_ss_registry_index(const nr_pdcch_blind_monitor_cfg_t *cfg)
{
  if (cfg == NULL || cfg->bwp_size == 0)
    return -1; // not configured yet (autoconf before MIB/SIB1)
  const int b = nr_pdcch_ss_bucket(cfg);
  if (g_ss_reg_idx[b] < 0) {
    const int nrb = cfg->coreset_freq_domain; // num_groups of 6 contiguous PRBs
    nr_pdcch_ss_entry_t e = {.coreset_id = (uint8_t)(b == 0 ? 0 : 1),
                             .coreset_duration = (uint8_t)cfg->coreset_duration,
                             .coreset_n_rbs = (uint16_t)(nrb * 6),
                             .ss_type = (uint8_t)(b == 0 ? 0 : 1),
                             .ss_first_symbol = (uint8_t)cfg->ss_first_symbol,
                             .ss_period_slots = 1,
                             .ss_offset_slots = 0,
                             .bwp_start = (uint16_t)cfg->bwp_start,
                             .bwp_size = (uint16_t)cfg->bwp_size,
                             .al_candidates = {0, 1, 1, 1, 0}}; // AL2/4/8 scanned; the census refines per SS
    g_ss_reg_idx[b] = nr_pdcch_ss_register(&g_ss_reg, &e);
  }
  return g_ss_reg_idx[b];
}
static void nr_pdcch_ss_registry_accept(const nr_pdcch_blind_monitor_cfg_t *cfg, uint16_t rnti)
{
  const int idx = nr_pdcch_ss_registry_index(cfg);
  if (idx < 0)
    return;
  bool repeated = false;
  for (int i = 0; i < 64; i++)
    if (g_ss_recent_rnti[i] == rnti) { repeated = true; break; }
  g_ss_recent_rnti[g_ss_recent_w++ % 64] = rnti;
  nr_pdcch_ss_observe(&g_ss_reg, idx, true, repeated);
}
static void nr_pdcch_ss_registry_occasion(const nr_pdcch_blind_monitor_cfg_t *cfg)
{
  const int idx = nr_pdcch_ss_registry_index(cfg);
  if (idx < 0)
    return;
  g_ss_reg.occasions[idx]++;
  static uint64_t n;
  if ((++n % 200000) == 0) {
    char b[400];
    int u = 0;
    for (int i = 0; i < g_ss_reg.n && u < (int)sizeof(b) - 60; i++)
      u += snprintf(b + u, sizeof(b) - u, "[%s cs%u dur%u %uRB sym%u: occ=%llu acc=%llu conf=%llu%s] ",
                    g_ss_reg.entry[i].ss_type ? "USS" : "CSS0", g_ss_reg.entry[i].coreset_id,
                    g_ss_reg.entry[i].coreset_duration, g_ss_reg.entry[i].coreset_n_rbs,
                    g_ss_reg.entry[i].ss_first_symbol, (unsigned long long)g_ss_reg.occasions[i],
                    (unsigned long long)g_ss_reg.accepts[i], (unsigned long long)g_ss_reg.confirmed[i],
                    g_ss_reg.retired[i] ? " RETIRED" : "");
    /* Observe-only by default: a copy is asked what it WOULD retire. Under V2 the live registry is
     * pruned (it never retires its last live entry), and the occasion loop above then probes a
     * retired entry 1 time in 64 instead of paying full scan cost on it. */
    int would;
    if (nr_agnostic_v2()) {
      would = nr_pdcch_ss_retire_barren(&g_ss_reg, 100000);
    } else {
      nr_pdcch_ss_registry_t probe = g_ss_reg;
      would = nr_pdcch_ss_retire_barren(&probe, 100000);
    }
    LOG_A(PHY, "SENSING: SS_REGISTRY live=%d/%d would_retire=%d %s\n", nr_pdcch_ss_live(&g_ss_reg), g_ss_reg.n, would, b);
  }
}

static void nr_pdcch_dci11_layout_observe(const nr_pdcch_blind_monitor_cfg_t *cfg,
                                          uint16_t dci_length, uint64_t payload)
{
  if (g_dci11_state < 0 || cfg == NULL || dci_length == 0 || cfg->bwp_size == 0) {
    return;
  }
  if (g_dci11_state == 0) {
    const double span = ((double)cfg->bwp_size * (double)(cfg->bwp_size + 1)) / 2.0;
    const uint16_t riv_bits = (uint16_t)ceil(log2(span));
    /* No configured TDRA list -> the field width is UNKNOWN and is searched (0..4 bits), not
     * assumed to be the 16-entry default. A configured list pins it, as before. */
    uint8_t tda_bits = NR_DCI11_TDA_UNKNOWN;
    if (cfg->extract.tda_count > 0) {
      tda_bits = 0;
      while ((1 << tda_bits) < cfg->extract.tda_count) {
        tda_bits++;
      }
    }
    const int n = nr_dci11_resolver_init_fdra(&g_dci11_resolver, cfg->bwp_start, cfg->bwp_size, riv_bits, tda_bits,
                                              dci_length);
    if (n <= 0) {
      /* NOT a resolver failure. It means no legal switch combination sums to the observed length,
       * so one of bwp_size / tda_count / dci_length disagrees with this cell -- which is itself
       * worth saying out loud, once. */
      LOG_W(PHY, "SENSING: DCI11_LAYOUT no legal layout sums to dci_length=%u at bwp_size=%u "
                 "tda_bits=%u -- one of those three is wrong for this cell\n",
            dci_length, (unsigned)cfg->bwp_size, tda_bits);
      g_dci11_state = -1;
      return;
    }
    if (cfg->extract.tda_count > 0 && cfg->extract.tda_count < 16)
      nr_dci11_resolver_set_tda_count(&g_dci11_resolver, (uint8_t)cfg->extract.tda_count);
    LOG_I(PHY, "SENSING: DCI11_LAYOUT armed: %d layouts (FDRA stages < %d) consistent with dci_length=%u "
               "(riv=%u bits, tda=%s; later RA type 0 / dynamicSwitch modes armed only on TB-CRC refutation)\n", n,
          g_dci11_resolver.fdra_next, dci_length,
          riv_bits, tda_bits == NR_DCI11_TDA_UNKNOWN ? "0..4 bits (searched)" : "configured");
    if (n >= NR_DCI11_LAYOUT_MAX)
      LOG_W(PHY, "SENSING: DCI11_LAYOUT initial set truncated at NR_DCI11_LAYOUT_MAX=%d\n", NR_DCI11_LAYOUT_MAX);
    g_dci11_state = 1;
  }
  nr_dci11_resolver_observe(&g_dci11_resolver, payload);
  if ((++g_dci11_seen % 4000) != 0) {
    return;
  }
  /* FDRA MODE STAGING (nr_dci11_resolver_init_fdra): stage 2 has refuted every live layout by TB CRC ->
   * arm the next RA type 0 / dynamicSwitch mode. Never on stage-1 evidence, which cannot refute them. */
  const int t1_pass = atomic_exchange_explicit(&g_dl_type1_pass, -1, memory_order_relaxed);
  if (t1_pass >= 0) {
    const int killed = nr_dci11_resolver_disarm(&g_dci11_resolver, t1_pass);
    LOG_A(PHY, "SENSING: DCI11_LAYOUT type-1 layout %d passed its CRC: FDRA staging DISARMED, %d RA type 0 / "
               "dynamicSwitch layouts killed\n", t1_pass, killed);
  }
  const uint64_t dl_tr = atomic_load_explicit(&g_dl_layout_trials, memory_order_relaxed);
  const uint64_t dl_link = atomic_load_explicit(&g_dl_link_pass_at, memory_order_relaxed);
  /* Link healthy = a non-stage-2 PDSCH passed within the span a refutation needs (64 trials per live layout). */
  const bool dl_link_ok = dl_link != UINT64_MAX
                          && dl_tr - dl_link <= (uint64_t)NR_DCI11_FDRA_ARM_MIN_TRIALS * (uint64_t)(g_dci11_resolver.n_alive + 1);
  if (dl_link_ok && nr_dci11_resolver_all_refuted(&g_dci11_resolver, NR_DCI11_FDRA_ARM_MIN_TRIALS)) {
    int added = 0;
    const int m = nr_dci11_resolver_arm_next_mode(&g_dci11_resolver, &added);
    if (m >= 0)
      LOG_A(PHY, "SENSING: DCI11_LAYOUT live set refuted by TB CRC (0 passes, >= %d trials per live layout in total, "
                 "link healthy): retired it, armed FDRA mode %d (1/2 = RA type 0, 3/4 = dynamicSwitch): %d layouts, "
                 "%d live of %d%s\n",
            NR_DCI11_FDRA_ARM_MIN_TRIALS, m, added, g_dci11_resolver.n_alive, g_dci11_resolver.n_hyp,
            g_dci11_resolver.n_hyp >= NR_DCI11_LAYOUT_MAX ? " -- TRUNCATED at NR_DCI11_LAYOUT_MAX" : "");
  }
  const nr_dci11_resolver_t *r = &g_dci11_resolver;
  const int cfg_bwp = (cfg->extract.bwp_indicator_bits >= 0) ? cfg->extract.bwp_indicator_bits : 1;
  const int cfg_ap  = (cfg->extract.antenna_ports_bits >= 0) ? cfg->extract.antenna_ports_bits : 4;
  int cfg_alive = 0;
  for (int i = 0; i < r->n_hyp; i++) {
    if (!r->alive[i]) {
      continue;
    }
    nr_dci11_field_bits_t f;
    if (nr_dci11_layout_to_field_bits(&r->hyp[i], &f)
        && f.bwp_indicator_bits == cfg_bwp && f.antenna_ports_bits == cfg_ap) {
      cfg_alive = 1;
      break;
    }
  }
  g_dci11_cfg_alive = cfg_alive;
  LOG_A(PHY, "SENSING: DCI11_LAYOUT n=%llu observed | %d of %d layouts still plausible | "
             "configured (bwp_ind=%d ant_ports=%d) %s\n",
        (unsigned long long)g_dci11_seen, r->n_alive, r->n_hyp, cfg_bwp, cfg_ap,
        cfg_alive ? "IS among the survivors"
                  : "IS NOT among the survivors -- the assumed widths contradict the air");
  if (nr_pdcch_dci11_stage2_enabled() || nr_agnostic_v2()) {
    /* Every live layout, compactly: i t<tda>b<bwp>m<pre_mcs>x<tb2+dai+p2h>a<ant><type>p<post_ant>:ok/trials.
     * The whole set, not a 400-byte prefix: whether the TRUE layout is still alive is the first
     * question when nothing decodes, and it cannot be answered from a truncated list. */
    static char eb[8192];
    int u = 0;
    for (int i = 0; i < r->n_hyp && u < (int)sizeof(eb) - 48; i++) {
      if (!r->alive[i])
        continue;
      nr_dci11_field_bits_t f;
      if (!nr_dci11_layout_to_field_bits(&r->hyp[i], &f))
        continue;
      const uint32_t ok = r->probe_ok[i], tr = r->probe_tr[i];
      u += snprintf(eb + u, sizeof(eb) - u, "[%d t%uf%ub%dm%dx%da%d%cp%d:%u/%u]", i, (unsigned)r->off[i].tda_bits,
                    (unsigned)r->off[i].fdra_mode, f.bwp_indicator_bits, f.vrb_to_prb_bits, f.tb2_bits, f.antenna_ports_bits,
                    f.dmrs_config_type ? 'B' : 'A', f.tci_bits, ok, tr);
    }
    LOG_A(PHY, "SENSING: DCI11_STAGE2 %s (alive=%d, hands over at <=%d) tb_crc ok/trials per live layout: %s\n",
          r->n_alive <= (g_dci11_cfg_alive ? 4 : NR_DCI11_STAGE2_MAX_ALIVE) ? "DRIVING the extractor" : "waiting for stage 1 to prune",
          r->n_alive, NR_DCI11_STAGE2_MAX_ALIVE, eb);
  }
}

/* Length evidence has two identities: physical CORESET geometry and addressed RNTI. The inner
 * bank owns per-RNTI state; the outer LRU keeps interleaved CORESET#0, discovered and archived
 * CORESETs independent. */
static nr_pdcch_dci_length_store_t g_dl_length_store;
static nr_pdcch_dci_length_store_t g_ul_length_store;
static pthread_mutex_t g_dl_length_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t length_coreset_key(const nr_pdcch_blind_monitor_cfg_t *cfg)
{
  uint64_t h = UINT64_C(1469598103934665603);
  const int fields[] = {
      cfg->bwp_start, cfg->bwp_size, cfg->coreset_rb_offset, cfg->coreset_freq_domain,
      cfg->coreset_duration, cfg->ss_first_symbol, cfg->coreset_type,
      cfg->coreset_reg_bundle_size, cfg->coreset_interleaver_size, cfg->coreset_shift_index,
      cfg->coreset_pdcch_dmrs_scrambling_id};
  for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
    h = (h ^ (uint32_t)fields[i]) * UINT64_C(1099511628211);
  return h ? h : 1;
}

static uint64_t length_lookahead_key(const nr_pdcch_blind_monitor_cfg_t *cfg,
                                     const nr_pdcch_lookahead_geom_t *g)
{
  uint64_t h = UINT64_C(1469598103934665603);
  const int fields[] = {
      cfg->bwp_start, cfg->bwp_size, g->rb_offset, g->freq_domain,
      cfg->coreset_duration, cfg->ss_first_symbol, cfg->coreset_type,
      g->reg_bundle_size, g->interleaver_size, g->shift_index,
      cfg->coreset_pdcch_dmrs_scrambling_id};
  for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
    h = (h ^ (uint32_t)fields[i]) * UINT64_C(1099511628211);
  return h ? h : 1;
}

/* Unanchored discovery cannot own a per-RNTI context yet. It remains isolated from the persistent
 * banks and is discarded when the geometry advances. */
static nr_pdcch_dci_length_sweep_state_t g_dl_length_state;

/* Lookahead lanes (nr_pdcch_blind_monitor.h). DCI length is a property of the payload width, not
 * the CORESET geometry, but a WRONG geometry's "candidates" are pure noise, so each lane needs its
 * OWN length-sweep accumulator -- sharing one across different geometries would mix real signal
 * from a right one with noise from a wrong one in the same significance test. Reset whenever a
 * lane's geometry changes (detected by comparing against g_lane_last_geom each occasion, since
 * nr_pdcch_blind_monitor.c owns lane advancement and has no reason to know this file's state). */
static nr_pdcch_dci_length_sweep_state_t g_lane_length_state[NR_PDCCH_LOOKAHEAD_MAX];
static bool                 g_lane_length_swept[NR_PDCCH_LOOKAHEAD_MAX];
static bool                 g_lane_length_found[NR_PDCCH_LOOKAHEAD_MAX];
static uint16_t             g_lane_dci_length[NR_PDCCH_LOOKAHEAD_MAX];
static uint16_t             g_lane_length_rnti[NR_PDCCH_LOOKAHEAD_MAX];
static nr_pdcch_lookahead_geom_t g_lane_last_geom[NR_PDCCH_LOOKAHEAD_MAX];

static void dl_discovery_invalidate(void)
{
  g_length_swept = g_length_found = false;
  atomic_store_explicit(&g_dl_layout_preferred, 0, memory_order_relaxed);
  nr_pdcch_dci_length_sweep_reset(&g_dl_length_state);
  nr_pdsch_config_sweep_reset_all();
  g_pdsch_sweep_on = false;
}

static void pdsch_sweep_maybe_enable(const nr_pdcch_blind_monitor_cfg_t *cfg)
{
  const bool ready = cfg->dl_full_auto && (!cfg->autodiscover
      || (g_length_found && nr_pdcch_blind_monitor_autodiscover_extent_verified()));
  uint64_t identity = UINT64_C(1469598103934665603);
  const int fields[] = {cfg->bwp_start, cfg->bwp_size, cfg->coreset_rb_offset,
      cfg->coreset_freq_domain, cfg->coreset_duration, cfg->coreset_reg_bundle_size,
      cfg->coreset_interleaver_size, cfg->coreset_shift_index,
      cfg->coreset_pdcch_dmrs_scrambling_id, cfg->dmrs_typeA_position, cfg->dci_length_override};
  for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
    identity = (identity ^ (uint32_t)fields[i]) * UINT64_C(1099511628211);
  /* Static configuration storage is zero-initialized; include every interpretation width/entry. */
  const unsigned char *bytes = (const unsigned char *)&cfg->extract;
  for (unsigned i = 0; i < sizeof(cfg->extract); ++i)
    identity = (identity ^ bytes[i]) * UINT64_C(1099511628211);
  identity ^= nr_pdcch_blind_monitor_autodiscover_generation();
  if (g_pdsch_sweep_on && (!ready || identity != g_pdsch_configuration))
    nr_pdsch_config_sweep_reset_all();
  if (ready && !g_pdsch_sweep_on)
    LOG_A(PHY, "SENSING: Technique D ARMED: independent RNTI/TDA contexts, TB-CRC scoring\n");
  g_pdsch_configuration = identity;
  g_pdsch_sweep_on = ready;
}
// Bounded give-up cap (2026-09-06) on accumulated sweep occasions -- see that branch's own comment
// for why this must accumulate across many occasions rather than fire once. Not derived from a
// rate (Technique A's own AUTODISCOVER_OBS_CALLS=1000 counts raw per-symbol scan calls, a
// different, faster-ticking counter than this one's real candidate-bearing occasions); a real
// length has been reached within tens of occasions in every live capture measured so far, so this
// is generous headroom, not a tuned minimum.
/* Occasions spent on one CORESET mapping hypothesis before moving to the next.
 *
 * 500 was sized for a sweep with NO GROUND TRUTH: the only way to judge a hypothesis was whether the
 * distribution of CRC passes looked non-random, which needs a large sample. That premise changed
 * 2026-09-20 -- the bootstrap now supplies a REAL RNTI (measured: bootstrap_rnti=0x15e1, from a
 * TC-RNTI harvested in the proven CORESET#0), so each hypothesis is a KNOWN-ANSWER test: the correct
 * mapping recovers that RNTI almost at once, a wrong one never does. 500 occasions to establish that
 * is enormously conservative.
 *
 * It matters because the sweep is serial over the catalogue and rate-limited by AIR TIME, not by
 * compute: MEASURED 2026-09-20, ~3 hypotheses/min, so 271 mappings take ~56 min per pass -- longer
 * than a TC-RNTI stays addressable in the dedicated search space, which is self-defeating.
 *
 * ISAC_SWEEP_OCCASIONS overrides it; default unchanged at 500 so no existing run behaves differently.
 * The structural fix is to test MANY hypotheses per occasion (same LLRs, 271 deinterleavings, batched
 * polar decodes -- a GPU job) which collapses a pass from 135,500 occasions to ~500. This knob is the
 * cheap approximation of that. */
static inline int autodiscover_sweep_budget(void)
{
  static int s_budget = -1;
  if (s_budget < 0) {
    const char *e = getenv("ISAC_SWEEP_OCCASIONS");
    const int v = (e != NULL) ? atoi(e) : 0;
    s_budget = (v > 0) ? v : 500;
  }
  return s_budget;
}
/* ISAC_DCI_LEN_MIN / ISAC_DCI_LEN_MAX: narrow the blind dci_length range (default 30..63, i.e. 34
 * lengths). Total sweep work is hypotheses x dwell x candidates x LENGTHS, so this is one of only
 * two knobs that cut TOTAL work rather than moving it in time (the other is the dwell above) --
 * batching and reordering cannot, because the search is throughput-bound, not launch-bound
 * (MEASURED 2026-09-20: 96 GPU calls -> 1 moved prepass by 5%).
 *
 * Narrowing is a PRIOR, not a fact: a real length outside the window becomes undiscoverable. Keep
 * the default wide and narrow only when the deployment's DCI 1_1 size is already known for the
 * bandwidth in use (e.g. ~47-48 at 273 PRB), and widen again if nothing converges. */
static inline int dci_len_min(void)
{
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("ISAC_DCI_LEN_MIN");
    const int x = (e != NULL) ? atoi(e) : 0;
    v = (x >= 1 && x <= 63) ? x : 30;
  }
  return v;
}
static inline int dci_len_max(void)
{
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("ISAC_DCI_LEN_MAX");
    const int x = (e != NULL) ? atoi(e) : 0;
    v = (x >= dci_len_min() && x <= 63) ? x : 63;
  }
  return v;
}
/* Widest aggregation level a lane may scan. Defined here because lane_als() validates against it
 * and is declared above the LANE BATCH block that sizes its vectors from it. */
#define LANE_BATCH_AL_MAX    8
/* Per-lane extracted-RE budget: 16 candidates at the widest AL (9 RE/RB * 8 * 6 = 432). */
#define LANE_RE_PER_LANE     (16 * NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * LANE_BATCH_AL_MAX * 6)
/* ISAC_LANE_ALS: aggregation levels the LOOKAHEAD LANES scan, comma-separated (default "2").
 *
 * The lanes were AL2-only, hardcoded, and they perform ~99 % of the CORESET geometry search -- the
 * primary walks one extent while 96 lanes walk the rest. As this file already notes further down,
 * "the gNB picks the aggregation level from the SERVED UE's link", so AL2-only hardcodes a
 * deployment. That was right for the lab srsRAN cell (dedicated traffic measured at AL2 -- note its
 * log prints log2(L), so its "AL1" IS AL2). For a commercial macro received at distance there is NO
 * such measurement, and cell-edge UEs are served at AL4/8/16, where an AL2-only scan can never find
 * a grant however long it dwells. Default stays "2" so nothing changes silently. */
static int lane_als(const uint8_t **out)
{
  static uint8_t v[5];
  static int n = -1;
  if (n < 0) {
    const char *e = getenv("ISAC_LANE_ALS");
    n = 0;
    if (e != NULL) {
      for (const char *q = e; *q && n < 5;) {
        const int x = atoi(q);
        if ((x == 1 || x == 2 || x == 4 || x == 8 || x == 16) && x <= LANE_BATCH_AL_MAX) {
          bool dup = false;
          for (int i = 0; i < n; i++)
            if (v[i] == (uint8_t)x) dup = true;
          if (!dup) v[n++] = (uint8_t)x;
        }
        while (*q && *q != ',') q++;
        if (*q == ',') q++;
      }
    }
    if (n == 0) {
      const nr_pdcch_sib1_prior_t *pr = nr_pdcch_sib1_prior_get();
      if (pr != NULL && pr->ss_valid) {
        for (int i = 0; i < NR_SIB1_PRIOR_NUM_AL && n < 5; i++)
          if (pr->al_candidates[i] > 0) v[n++] = (uint8_t)(1 << i);
        if (n > 0)
          LOG_A(PHY, "SENSING: lane ALs from SIB1 CSS: %d,%d,%d,%d,%d (n=%d)\n", v[0], n > 1 ? v[1] : 0,
                n > 2 ? v[2] : 0, n > 3 ? v[3] : 0, n > 4 ? v[4] : 0, n);
      }
      if (n == 0) {
        v[0] = 2; n = 1;   /* previous AL2-only default, until a SIB1 prior arrives */
        *out = v;
        const int r = n; n = -1;   /* do not cache: re-evaluate once SIB1 has been decoded */
        return r;
      }
    }
  }
  *out = v;
  return n;
}

static int lane_al_count(void)
{
  const uint8_t *unused = NULL;
  return lane_als(&unused);
}

static bool lane_has_bootstrap_hit(const nr_pdcch_dci_length_sweep_state_t *state)
{
  for (int len = 0; len < NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN; ++len)
    if (state->bootstrap_hits[len] > 0)
      return true;
  return false;
}

#define AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS autodiscover_sweep_budget()
/* ISAC_DCI_SWEEP_STRIDE: test every Nth dci_length per occasion instead of all 34 (the sweep's
 * `stride`, see nr_pdcch_dci_length_sweep.h). The budget above counts ROUNDS, so each length still
 * gets its 500 visits -- rotation only spreads them in time.
 *
 * DERIVATION of the value to use, from cs1_090638 (600 s, K=1, one active hypothesis): the scan
 * consumer took TOTAL 1890 us/occasion while its timed stages summed to ~135 us (fep_llr 24,
 * demap 4, prepass 3, decode 103), leaving ~1.75 ms in the two sweeps -- 34 lengths x ~6
 * candidates x ~8 us, i.e. ~51 us per length per occasion. Occasions arrive at ~1500/s = one per
 * 667 us, so the consumer was ~2.8x oversubscribed and dropped 68.5 % of them
 * (scanq queued=902224 done=283999 drop_full=618213). Both sweeps can run in the same occasion
 * (the DL one falls through to the UL one), so budget for two: 2 x (34/N) x 51 us + 135 us < 667 us
 * needs N >= 6.2. N = 8 gives 2 x 217 + 135 = 569 us at the pessimistic both-sweeps bound and
 * 135 + 217 = 352 us (53 % of the interval) when only one runs -- the headroom K>1 lookahead needs.
 * DEFAULT 0 (= off, bit-identical to the pre-rotation sweep) until a live capture confirms the
 * drops actually go away; 68.5 % occasion loss is the number that justifies flipping it on. */
static int dci_sweep_stride(void)
{
  static int s_stride = -1;
  if (s_stride < 0) {
    const char *e = getenv("ISAC_DCI_SWEEP_STRIDE");
    s_stride = (e != NULL) ? atoi(e) : 0;
    if (s_stride < 0) {
      s_stride = 0;
    }
    if (s_stride > 1) {
      LOG_A(PHY, "SENSING: dci_length sweep rotating -- every %dth length per occasion, %d rounds "
                 "to the give-up cap (per-length trials unchanged)\n",
            s_stride, AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS);
    }
  }
  return s_stride;
}
static int         g_constdiag_left = 20; // TEMPORARY, see CONSTDIAG below
/* Why these three exist (2026-09-09): the gNB's own log shows 746 format-1_1 and 147 format-0_1
 * DCIs in the same CORESET, same search space, same al=2, same rnti -- and this receiver converges
 * Technique D off the 1_1 stream while reporting dci01 accepts=0. "accepts" is measured AFTER the
 * discovery controller, so it cannot distinguish "the polar decode never recovered the RNTI" from
 * "it did, and the width search has not converged yet". These split that. */
static uint64_t    g_ul_sched       = 0; // DCI 0_1 candidates actually scheduled for decode
static uint64_t    g_ul_crc_hit     = 0; // ... whose polar CRC recovered the targeted RNTI
static uint64_t    g_ul_disc_call   = 0; // ... that reached the discovery controller
static uint64_t    g_ul00_accepts   = 0; // DCI 0_0 accepts (UL grants recovered off the 1_0 scan)
static uint64_t    g_ul00_rejects   = 0; // confirmed-RNTI 1_0 rejects that were not a valid 0_0
static uint64_t    g_ul_accepts     = 0; // DCI 0_1 accepts (UL grants recovered)
static uint64_t    g_ul_rejects     = 0; // DCI 0_1 candidates whose CRC was in range but whose
                                         // fields failed a plausibility check. Reported next to the
                                         // accepts because on an UNPINNED field layout a high
                                         // reject count is the FIRST symptom of wrong widths, and a
                                         // bare accept count cannot show it.
static uint64_t    g_accepts        = 0; // raw plausibility accepts (Step 1-4 of decode_and_extract),
                                         // BEFORE the noise-floor gates below -- unchanged meaning
                                         // from before 2026-07-28's gates, so old logs stay comparable
static const char *g_last_reject_reason = NULL; // TEMPORARY diagnostic, 2026-07-28 root-cause pass
static uint16_t     g_last_reject_rnti  = 0;
/* Per-DCI-format / per-RNTI-class accept census. Format 1_0 exists to reach SIB1, RAR and Msg4, so
 * "how many accepts, and of what" is the primary thing to look at when judging whether the 1_0 scan
 * is doing anything -- a single aggregate accept count cannot distinguish "found the RRCSetup" from
 * "found more C-RNTI fallback grants". Indexed by nr_blind_rnti_class_t. */
static uint64_t g_accepts_10     = 0;
static uint64_t g_accepts_class[NR_BLIND_RNTI_CLASS_COUNT] = {0};
static uint64_t g_cfr_submits    = 0; // final count that actually reached the ISAC engine, i.e. after
                                      // ALL gates (raw accept + energy + persistence + SNR)

// ---- Noise-floor gate counters (2026-07-28) -- how many raw accepts each gate held back, so the
// periodic summary shows where candidates are actually being lost, not just the final count. ----
static uint64_t g_held_energy   = 0; // skipped decode entirely, raw LLR energy below energy_min
static uint64_t g_held_dmrs     = 0; // skipped decode entirely, DM-RS coherence below this occasion's own per-AL median+MAD floor (5GSniffer-style gate, 2026-09-23)

// ---- Adaptive energy floor (cfg->energy_adapt_factor) ------------------------------------------
// Tracks the NOISE-FLOOR candidate energy so the gate threshold can be expressed as a dimensionless
// multiple of it rather than an absolute level. Motivation: the absolute `energy_min` is in
// receiver-dependent pdcch_e_rx units, so a value tuned at one bandwidth/gain does not transfer --
// it could not even be carried from the 106 PRB cell to 273 PRB, never mind to real OTA gain
// settings, which is exactly why it ended up disabled (0) rather than retuned per deployment.
//
// Why the MEDIAN and not the mean: at this scan's trial volume the candidate population is
// overwhelmingly unscheduled CCEs (8 candidates/slot, of which at most a couple are real grants),
// so the median IS the noise floor. Real grants are large outliers and would drag a mean upward,
// raising the threshold and progressively suppressing the very signals the gate is meant to keep --
// a feedback loop that gets worse the better the cell is loaded.
//
// The median is tracked with a "frugal" streaming update (step toward the sample by a fraction of
// the current estimate) rather than a histogram or reservoir: O(1) time, O(1) state, no allocation
// and no unbounded growth, which is what the RT path requires. The step is RELATIVE to the current
// estimate so convergence speed is scale-free -- it adapts equally fast whether the floor is 0.5 or
// 5000 units. ENERGY_FLOOR_STEP is a rate, not a magnitude: it sets how fast the estimate follows a
// changing environment, and is the only tuned number left, deliberately loose (anything in
// ~0.001-0.05 behaves the same on a stationary floor).
#define ENERGY_FLOOR_STEP        0.01f  // fractional step per candidate toward the running median
#define ENERGY_FLOOR_MIN         1e-6f  // keep strictly positive: the threshold is multiplicative
#define ENERGY_FLOOR_WARMUP      200    // candidates observed before the gate is allowed to reject
static float    g_energy_floor  = 0.0f;
static uint64_t g_energy_nseen  = 0;

static void energy_floor_update(float x)
{
  g_energy_nseen++;
  if (g_energy_floor <= 0.0f) {
    // Seed on the first sample rather than from 0, so the relative step has something to scale.
    g_energy_floor = (x > ENERGY_FLOOR_MIN) ? x : ENERGY_FLOOR_MIN;
    return;
  }
  const float step = g_energy_floor * ENERGY_FLOOR_STEP;
  g_energy_floor += (x > g_energy_floor) ? step : -step;
  if (g_energy_floor < ENERGY_FLOOR_MIN) {
    g_energy_floor = ENERGY_FLOOR_MIN;
  }
}
static uint64_t g_held_persist  = 0;
static uint64_t    g_held_rnti_set = 0;  // rejected: RNTI not among the confirmed UEs // decoded+accepted but RNTI not yet seen rnti_persist_k times
static uint64_t g_held_snr      = 0; // decoded+accepted+persisted but post-estimation SNR too low
static uint64_t g_held_mismatch = 0;
static unsigned long g_acc_slot[2][20], g_occ_slot[20]; // ACCSLOT census, see the accept path
/* [0]=deferred enqueue, [1]=normal enqueue, indexed by nr_blind_rnti_class_t. */
static _Atomic unsigned long g_enq_class[2][NR_BLIND_RNTI_CLASS_COUNT]; // migrated from NRSniffer: rejected by the adaptive mismatched-bits gate

// ---- Passive PDSCH decode counters (2026-07-30). g_dec_ok/g_dec_try IS the go/no-go measurement
// PASSIVE_PDSCH_DATA_AIDED_HANDOVER.md §B.5 asks for: a passive receiver sits somewhere the grant
// was not aimed at, so whether overheard transport blocks pass CRC at all is an open empirical
// question, and everything downstream of it is worthless if the answer is "almost never". ----
/* ---- RT-THREAD COST BREAKDOWN (ISAC_PDCCH_TIMING=1, default OFF) ------------------------------
 * PASSIVE_RX_ONLY_HANDOVER.md section 14 measures the SYMPTOM of running this tap on the PHY
 * receive thread (PBCH lock lost within ~2 s at ~1550 grants/s) but never measured WHERE the time
 * goes, so "move the PDSCH decode off the receive thread" is an assumption about which stage is
 * expensive, not a measurement. Sections 7.4/14.4 both name the PDSCH decode specifically; this
 * probe exists to confirm or refute that before any restructuring, because moving the wrong stage
 * costs the same effort and buys nothing.
 *
 * Deliberately CLOCK_MONOTONIC wall time, not CPU time: what starves the timing loop is elapsed
 * time on this thread, including the join_task_ans() wait for pool workers (which is wall time
 * this thread cannot use, but almost no CPU time). A getrusage/CLOCK_THREAD_CPUTIME view would
 * report the parallel decode as nearly free and point at the wrong stage.
 *
 * Cost when off: one already-resolved int test per call site. When on: two clock_gettime per
 * stage, which at ~8 stages/occasion is far below the microsecond-scale stages being measured --
 * but it is still a probe, so read it as R7 prescribes (characterise with it, score without it). */
#define BTIM_FEP_LLR 0
#define BTIM_DEMAP   1
#define BTIM_PREPASS 2
#define BTIM_DECODE  3
#define BTIM_CHEST   4
#define BTIM_PDSCH   5
#define BTIM_SUBMIT  6
#define BTIM_TOTAL   7
/* The untimed remainder, bucketed 2026-09-17: the four stages above summed to ~138 us while TOTAL
 * read 1980 us per occasion (cs1 capture), and that gap -- not the decode -- is what made one scan
 * consumer drop 68 % of occasions (scanq drop_full=618213 of 902224). Name every segment so the
 * next capture says where it goes instead of a reader guessing. Note that with
 * pdcch_blind_monitor_scan_thread set these are CONSUMER times: the receive thread only enqueues. */
#define BTIM_PRE     8   /* occasion entry -> first FEP (UL scan setup, hypothesis selection) */
#define BTIM_PBWP    9   /* passive-BWP CORESET observe (every 8th occasion) */
#define BTIM_CSIRS   10  /* nr_csirs_blind_rt_slot(): CSI-RS reference generation + FEP + correlate (until confirmed) */
#define BTIM_POST    11  /* after the candidate decodes: accepts, evidence, sweeps, submissions */
#define BTIM_RT      12  /* nr_pdcch_blind_monitor_process() on the PHY RECEIVE thread, per slot:
                          * the only blind-PDCCH work left there once scan/pdsch/ul consumers are on
                          * (Phase 3's own single-antenna FEP + window scan while undiscovered, the
                          * occasion gate, the enqueue). Written from the receive thread into its own
                          * array slots; the consumer never touches index 12. */
#define BTIM_DLSWEEP 13  /* DL dci_length autodiscover sweep: 34 lengths x every candidate, Polar+CRC each */
#define BTIM_ULSWEEP 14  /* UL per-RNTI dci_length sweep, same shape, under ul_length_lock */
#define BTIM_N       15
static const char *const kBtimName[BTIM_N] = {"fep_llr", "demap", "prepass", "decode",
                                              "chest",   "pdsch", "submit",  "TOTAL",
                                              "pre",     "pbwp",  "csirs",   "post", "rt",
                                              "dlsweep", "ulsweep"};
static uint64_t g_btim_ns[BTIM_N]  = {0};
static uint64_t g_btim_n[BTIM_N]   = {0};
static uint64_t g_btim_max[BTIM_N] = {0};
/* Per-occasion TOTAL, bucketed. The mean is not the interesting statistic here: the receive thread
 * is starved by the TAIL, so what matters is how often one occasion eats a large fraction of the
 * slot. Buckets are microseconds: <50 <100 <200 <400 <800 <1600 <3200 >=3200. */
static uint64_t g_btim_hist[8] = {0};
static uint64_t g_btim_over_slot = 0; // occasions whose total exceeded the slot duration
static int      g_btim_on        = -1;

static inline int btim_enabled(void)
{
  if (g_btim_on < 0) {
    g_btim_on = (getenv("ISAC_PDCCH_TIMING") != NULL) ? 1 : 0;
  }
  return g_btim_on;
}

static inline uint64_t btim_now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Opt-in work deadline; 350 us reserves 150 us of the 500-us mean target for
 * the ordinary decode/dispatch tail. Measured whole-occasion quantiles decide
 * whether the target is actually met; this is not a hard real-time guarantee. */
static unsigned discovery_budget_us(void)
{
  static int value=-1;
  if (value<0) {
    const char *e=getenv("ISAC_PDCCH_DISCOVERY_BUDGET_US");
    value=e?atoi(e):0;
    if(value<0 || value>1000000) value=0;
  }
  return (unsigned)value;
}
static uint64_t discovery_cpu_now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static bool discovery_trace_enabled(void)
{
  static int enabled=-1;
  if(enabled<0) enabled=getenv("ISAC_PDCCH_DISCOVERY_TRACE")!=NULL;
  return enabled;
}
typedef struct {
  uint64_t start, cpu_start, before_feed, after_feed, after_extent, after_ul, after_pre, after_decode;
  uint64_t trials;
  int width, budget, phase;
} discovery_latency_scope_t;
static struct { uint64_t n, ns, over_ms, hist[1002]; } discovery_latency[46];
static void discovery_latency_done(discovery_latency_scope_t *s)
{
  if(!s->start || s->width<6 || s->width>270 || s->width%6) return;
  const uint64_t end=btim_now(), ns=end-s->start;
  if(s->cpu_start) {
    const uint64_t cpu=discovery_cpu_now()-s->cpu_start;
    static unsigned slow, fast;
    if((ns>=1000000 && slow++<32) || (ns<1000000 && fast++<2)) {
      LOG_A(PHY,"DISCOVERYTRACE span=%d budget=%d phase=%d wall_us=%.2f cpu_us=%.2f "
            "before_feed_us=%.2f feed_us=%.2f extent_us=%.2f ul_us=%.2f pre_us=%.2f decode_us=%.2f tail_us=%.2f trials=%lu\n",
            s->width,s->budget,s->phase,ns/1000.0,cpu/1000.0,
            s->before_feed?(s->before_feed-s->start)/1000.0:0.0,
            s->after_feed?(s->after_feed-s->before_feed)/1000.0:0.0,
            s->after_extent&&s->after_feed?(s->after_extent-s->after_feed)/1000.0:0.0,
            s->after_ul&&s->after_extent?(s->after_ul-s->after_extent)/1000.0:0.0,
            s->after_pre&&s->after_ul?(s->after_pre-s->after_ul)/1000.0:0.0,
            s->after_decode&&s->after_pre?(s->after_decode-s->after_pre)/1000.0:0.0,
            s->after_decode?(end-s->after_decode)/1000.0:0.0,(unsigned long)s->trials);
    }
  }
  const int i=s->width/6;
  ++discovery_latency[i].n; discovery_latency[i].ns+=ns;
  discovery_latency[i].over_ms+=ns>=1000000;
  uint64_t bin=(ns+999)/1000; if(bin>1001) bin=1001;
  ++discovery_latency[i].hist[bin];
  if(discovery_latency[i].n%4096==0) {
    const uint64_t target=(99*discovery_latency[i].n+99)/100;
    uint64_t count=0; int p99=0;
    for(;p99<=1001;++p99) { count+=discovery_latency[i].hist[p99]; if(count>=target) break; }
    LOG_A(PHY,"DISCOVERYLAT span=%d n=%lu mean_us=%.2f p99_upper_us=%d over_1ms=%lu budget_us=%u\n",
          s->width,(unsigned long)discovery_latency[i].n,
          discovery_latency[i].ns/(1000.0*discovery_latency[i].n),p99,
          (unsigned long)discovery_latency[i].over_ms,discovery_budget_us());
  }
}

static inline void btim_add(int k, uint64_t t0)
{
  if (g_btim_on <= 0) {
    return;
  }
  const uint64_t d = btim_now() - t0;
  g_btim_ns[k] += d;
  g_btim_n[k]++;
  if (d > g_btim_max[k]) {
    g_btim_max[k] = d;
  }
}

static void btim_occasion_total(uint64_t d_ns, uint64_t slot_ns)
{
  const uint64_t us = d_ns / 1000;
  int b = 0;
  if (us >= 3200) b = 7;
  else if (us >= 1600) b = 6;
  else if (us >= 800) b = 5;
  else if (us >= 400) b = 4;
  else if (us >= 200) b = 3;
  else if (us >= 100) b = 2;
  else if (us >= 50) b = 1;
  g_btim_hist[b]++;
  if (slot_ns > 0 && d_ns > slot_ns) {
    g_btim_over_slot++;
  }
}

static uint64_t g_dec_try   = 0; // decodes actually attempted (i.e. reached the LDPC decoder)
static uint64_t g_dec_ok    = 0; // ... of which the transport-block CRC passed
static uint64_t g_dec_skip_rv = 0;
/* Accepted candidates abandoned BEFORE the FEP/channel-estimation because the occasion had already
 * queued pdsch_max_per_slot decodes. Counted rather than silent: a large value means the cap is
 * throwing away real grants and should be raised (or more consumers added), which is a capacity
 * decision, not a defect. */
static uint64_t g_dec_over_cap = 0;

/* ---- ADAPTIVE AGGREGATION-LEVEL ALLOCATION --------------------------------------------------
 * The candidate budget (64 candidates / NR_MAX_PDCCH_SIZE REs) cannot cover a full sweep of every
 * level: 45+23+11+5 = 84 candidates and 9450 REs on this CORESET, over both caps. Something must
 * be given up, and the previous rule gave up whatever came last in a FIXED order.
 *
 * That hardcodes a deployment. The gNB picks the aggregation level from the SERVED UE's link
 * quality -- a UE at cell edge gets AL8 -- so a fixed ladder tuned on one cell silently misses
 * every grant on another, or on the same cell after the UE moves. It is the same mistake as pinning
 * the MCS: the receiver does not choose, and must not assume.
 *
 * So allocate in proportion to where accepts are ACTUALLY observed, learned online, with two
 * guarantees that keep it adaptive rather than self-confirming:
 *   - a FLOOR of at least AL_MIN_PROBE candidates on every enabled level, so a level that has never
 *     produced an accept still gets looked at and can be discovered. Without this the split is a
 *     ratchet: a level starved to zero can never earn its way back.
 *   - Laplace smoothing (+AL_PRIOR) on the counts, so early noise cannot drive a level to nothing
 *     before there is evidence either way.
 * The floor's CCE position ROTATES between occasions, so a level held at its minimum still sweeps
 * its whole CCE space over time instead of probing the same spot forever. */
#define AL_MIN_PROBE 2
#define AL_PRIOR     1.0
static _Atomic uint64_t g_al_accepts[4]; // indexed as ss_al_candidates[]: AL 1, 2, 4, 8
static _Atomic uint64_t g_al_cand[2][4];    // [0]=CSS0 [1]=dedicated USS; candidates EXAMINED per AL
static _Atomic uint64_t g_al_confirmed[2][4]; // same split; accepts that PASSED the RNTI-persistence gate
static uint32_t         g_al_rotate[4];
/* ---- SEARCH-SPACE (aggregation-level) INFERENCE  (agnosticity gap #1) ---------------------------
 * The set of monitored CCE aggregation levels is a dedicated-search-space property that the air
 * does not carry, but it IS inferable: every accept counted in g_al_accepts[] above already passed
 * the DCI CRC (a recovered, real RNTI) AND the cross-occasion persistence gate, so those grants
 * land only on the levels the gNB actually schedules. Report the inferred set ONCE, when enough
 * confirmed grants have accumulated to name it. Honestly bounded and reject-only: a level with no
 * confirmed grants is reported absent, never assumed; and this is the OBSERVED monitored set with
 * its per-level share, NOT nrofCandidates or exact CCE positions (which need the SS config itself).*/
static _Atomic int g_ss_inferred_logged_ss[2]; // one verdict per search space (CSS0, USS)
#define SS_INFER_MIN_GRANTS 32   /* enough CRC-recovered grants before naming the set */
#define SS_INFER_SHARE_NUM  1    /* a level carrying >= 20% (1/5) of confirmed grants is "monitored" */
#define SS_INFER_SHARE_DEN  5
#define SS_INFER_MIN_CAND   200  /* candidates examined before a level's rate means anything */
/* CORESET#0 (common) and the dedicated UE-specific search space are DIFFERENT search spaces with
 * DIFFERENT aggregation levels -- measured on this cell: broadcast SIB1 at AL4 (gNB's own
 * dci_aggregation_level=2, i.e. log2), dedicated traffic overwhelmingly AL2. Pooling them into one
 * histogram produces a confidently wrong "monitored AL set", so every count is bucketed by which
 * search space produced it. cfg->coreset_type: 1 = MIB/SIB1 CORESET#0, 0 = dedicated. */
static inline int nr_pdcch_ss_bucket(const nr_pdcch_blind_monitor_cfg_t *cfg)
{
  return (cfg && cfg->dci10_ss_type == NR_BLIND_SS_COMMON) ? 0 : 1;
}
static void nr_pdcch_blind_infer_search_space(void)
{
  /* Reported PER SEARCH SPACE. CORESET#0 and the dedicated USS are different search spaces with
   * different aggregation levels (measured here: broadcast AL4, dedicated AL2), so a single pooled
   * "monitored AL set" is not a well-defined quantity -- it would be confidently wrong. Each bucket
   * is declared once, independently, as soon as IT has enough evidence. */
  static const char *kSsName[2] = {"CSS0(common)", "USS(dedicated)"};
  for (int ss = 0; ss < 2; ++ss) {
    if (atomic_load_explicit(&g_ss_inferred_logged_ss[ss], memory_order_relaxed))
      continue;
    uint64_t a[4], c[4], tot = 0;
    for (int i = 0; i < 4; ++i) {
      a[i] = atomic_load_explicit(&g_al_confirmed[ss][i], memory_order_relaxed);
      c[i] = atomic_load_explicit(&g_al_cand[ss][i], memory_order_relaxed);
      tot += a[i];
    }
    if (tot < SS_INFER_MIN_GRANTS)
      continue;
    /* RATE, not raw count: the adaptive allocator feeds budget to whichever level already produced
     * accepts, so raw counts are self-confirming. A level whose denominator is too small to support
     * a conclusion is reported UNDERSAMPLED, never silently declared absent. */
    double rate[4]; double rmax = 0.0;
    for (int i = 0; i < 4; ++i) {
      rate[i] = (c[i] > 0) ? ((double)a[i] / (double)c[i]) : 0.0;
      if (rate[i] > rmax) rmax = rate[i];
    }
    const int lvl[4] = {1, 2, 4, 8};
    char al_set[64]; int u = 0;
    char weak[64];   int w = 0;
    for (int i = 0; i < 4; ++i) {
      if (c[i] < SS_INFER_MIN_CAND)
        w += snprintf(weak + w, sizeof(weak) - w, "%s%d", w ? "," : "", lvl[i]);
      else if (rmax > 0.0 && rate[i] * SS_INFER_SHARE_DEN >= rmax * SS_INFER_SHARE_NUM)
        u += snprintf(al_set + u, sizeof(al_set) - u, "%s%d", u ? "," : "", lvl[i]);
    }
    if (!u) continue; /* nothing adequately sampled in this bucket yet */
    atomic_store_explicit(&g_ss_inferred_logged_ss[ss], 1, memory_order_relaxed);
    LOG_A(PHY, "SENSING: SEARCH_SPACE INFERRED [%s] by CRC-recovered grants: monitored AL={%s}%s%s "
               "confirmed/examined[AL1=%llu/%llu AL2=%llu/%llu AL4=%llu/%llu AL8=%llu/%llu] "
               "total_confirmed=%llu (accepts per candidate EXAMINED; levels below %d candidates are "
               "undersampled, not shown absent; nrofCandidates/CCE positions bounded, not exact)\n",
          kSsName[ss], al_set, w ? " undersampled=" : "", w ? weak : "",
          (unsigned long long)a[0], (unsigned long long)c[0], (unsigned long long)a[1], (unsigned long long)c[1],
          (unsigned long long)a[2], (unsigned long long)c[2], (unsigned long long)a[3], (unsigned long long)c[3],
          (unsigned long long)tot, SS_INFER_MIN_CAND);
  }
}
  // per-level rotating CCE start, advanced each occasion // skipped: rv != 0 and rv0_only set (not self-decodable, see cfg)
static uint64_t g_dec_unsup = 0; // skipped: grant outside the decode/reconstruction scope
static uint64_t g_data_submits = 0; // reconstructed CFRs submitted as NR_ISAC_SRC_PDSCH_DATA

// ---- RNTI persistence tracking (2026-07-28): a real UE's RNTI recurs across many grants; a noise
// accept is a one-off. Small ring buffer of recent (rnti, abs_slot) sightings -- linear scan is fine
// given the raw accept rate is on the order of ~1/s (measured), so the buffer holds at most a few
// seconds of history regardless of window size. See nr_pdcch_blind_monitor_rt.h's rnti_persist_k/
// rnti_persist_window_ms field comments. ----
#define NR_PDCCH_BLIND_PERSIST_MAX 64
static struct {
  uint16_t rnti;
  uint32_t abs_slot;
} g_recent[NR_PDCCH_BLIND_PERSIST_MAX];
static int g_recent_head  = 0;
static int g_recent_count = 0;

// Returns true once `rnti` has been sighted at least `min_k` times (including this one) within the
// last `window_slots` slots. Always records the current sighting regardless of the outcome, so a
// candidate that fails today can contribute toward tomorrow's threshold.
static bool rnti_persistence_check(uint16_t rnti, uint32_t abs_slot, uint32_t window_slots, int min_k)
{
  if (min_k <= 1) {
    return true; // gate disabled -- accept-on-first-sighting, matches pre-2026-07-28 behaviour
  }
  int seen = 0;
  for (int i = 0; i < g_recent_count; i++) {
    if (g_recent[i].rnti == rnti && (abs_slot - g_recent[i].abs_slot) <= window_slots) {
      seen++;
    }
  }
  g_recent[g_recent_head].rnti     = rnti;
  g_recent[g_recent_head].abs_slot = abs_slot;
  g_recent_head                    = (g_recent_head + 1) % NR_PDCCH_BLIND_PERSIST_MAX;
  if (g_recent_count < NR_PDCCH_BLIND_PERSIST_MAX) {
    g_recent_count++;
  }
  return (seen + 1) >= min_k; // +1 counts the sighting just recorded
}

// ---- Phase 3 Technique C scorer adapter (2026-09-04) --------------------------------------------
// nr_pdcch_dci_length_sweep() (nr_pdcch_dci_length_sweep.c) drives this synchronously: for each
// hypothesised length it calls the scorer up to TRIALS_PER_LENGTH times in a tight loop, so this
// adapter reads candidates from a SHORT-LIVED, occasion-local list (built by the autodiscover
// branch in nr_pdcch_blind_monitor_run_occasion() below, by replaying the SAME e_rx_cand_idx walk
// the pre-pass loop further down uses -- not a second, independently-wrong indexing scheme, per
// the plan brief's own Step 4 warning) rather than pulling in a whole extra candidate-iteration
// mechanism. `trial_idx % n_cand` cycles through whatever real candidates this ONE occasion
// offers -- a real occasion rarely has 64 distinct ones, so most lengths get each real candidate
// tried several times, which is fine: a genuinely wrong length still degenerates to "few/no passes"
// or "one fixed-point payload", exactly the two traps nr_pdcch_dci_length_sweep.h documents.

/* Opt-in observation only. No hypothesis, RNTI or acceptance decision comes from
 * this stream. The offline scorer runs after the receiver has exited. */
static __thread int discovery_evidence_frame = -1, discovery_evidence_slot = -1;
static void discovery_evidence(const char *stage, const char *direction, int frame, int slot,
                               int bits, uint16_t rnti, uint64_t payload, int al, int cce)
{
  static __thread int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("ISAC_DISCOVERY_EVIDENCE");
    enabled = e && !strcmp(e, "1");
  }
  if (!enabled) return;
  LOG_A(PHY, "DISCOVERY_EVIDENCE {\"stage\":\"%s\",\"direction\":\"%s\","
        "\"frame\":%d,\"slot\":%d,\"bits\":%d,\"rnti\":%u,"
        "\"payload\":\"%016llx\",\"al\":%d,\"cce\":%d}\n",
        stage, direction, frame, slot, bits, rnti, (unsigned long long)payload, al, cce);
}

typedef struct {
  const c16_t *e_rx;
  uint8_t      L;
  uint16_t     cce;
} nr_pdcch_autodiscover_cand_t;
static pthread_mutex_t ul_length_lock=PTHREAD_MUTEX_INITIALIZER;


typedef struct {
  const nr_pdcch_autodiscover_cand_t *cand;
  int      n_cand;
  uint16_t bwp_size;
  uint8_t  dmrs_typeA_position;
  uint16_t rnti_min;
  uint16_t rnti_max;
  const nr_pdcch_blind_extract_opts_t *extract_opts;
  uint16_t scrambling_rnti;
  uint16_t dmrs_scrambling_id;
  uint16_t known_rnti[NR_PDCCH_BLIND_MAX_UE];
  uint8_t  n_known;
  uint16_t bootstrap_alias;
  int lane, rb_offset, span_rb, bundle, interleaver, shift;
} nr_pdcch_autodiscover_sweep_ctx_t;

static uint16_t sweep_evidence_rnti(const nr_pdcch_autodiscover_sweep_ctx_t *ctx, uint16_t rnti,
                                    int dci_length, const nr_pdcch_autodiscover_cand_t *cand)
{
  for (int i = 0; i < ctx->n_known; ++i)
    if (ctx->known_rnti[i] == rnti) {
      static _Atomic uint64_t hits = 0;
      const uint64_t h = atomic_fetch_add_explicit(&hits, 1, memory_order_relaxed) + 1;
      if (h == 1 || h % 16 == 0)
        LOG_A(PHY, "SENSING: USS_BOOT_HIT count=%lu frame=%d slot=%d rnti=0x%04x len=%d "
                   "AL=%u CCE=%u lane=%d offset=%d span=%d bundle=%d interleaver=%d shift=%d "
                   "verified_set=%u\n",
              (unsigned long)h, discovery_evidence_frame, discovery_evidence_slot, rnti, dci_length,
              cand ? cand->L : 0, cand ? cand->cce : 0, ctx->lane, ctx->rb_offset, ctx->span_rb,
              ctx->bundle, ctx->interleaver, ctx->shift, ctx->n_known);
      return ctx->bootstrap_alias;
    }
  return rnti;
}

/* DMRS scheduling: 0 = old exhaustive order, 1 = bounded ranking + exploration (default),
 * 2 = ranked exhaustive control. A sub-1% cadence remains exhaustive in mode 1. No absolute
 * correlation cutoff and no changes to CRC validation, lengths or verified/CSS0 decoding. */
static int blind_dmrs_rank_mode(void)
{
  static int mode = -1;
  if (mode < 0) {
    const char *e = getenv("ISAC_PDCCH_DMRS_RANK");
    mode = e ? atoi(e) : 1;
    if (mode < 0 || mode > 2) mode = 1;
  }
  return mode;
}

static void blind_dmrs_rank_candidates(const nr_pdcch_dmrs_rank_grid_t *grid,
                                       fapi_nr_dl_config_dci_dl_pdu_rel15_t *pdu,
                                       int span, int offset, int slot, int first_symbol,
                                       uint32_t abs_slot, uint64_t visit)
{
  const int n = pdu->number_of_candidates;
  if (n < 1 || n > NR_PDCCH_RANK_MAX_CAND || !grid->n_rb) return;
  double score[NR_PDCCH_RANK_MAX_CAND], dmrs[NR_PDCCH_RANK_MAX_CAND];
  uint8_t order[NR_PDCCH_RANK_MAX_CAND], al[NR_PDCCH_RANK_MAX_CAND];
  uint16_t cce[NR_PDCCH_RANK_MAX_CAND], support[NR_PDCCH_RANK_MAX_CAND] = {0};
  const nr_pdcch_uss_geometry_t tracker_geometry = {
      .rb_offset = (uint16_t)offset, .span_rb = (uint16_t)span,
      .shift = pdu->coreset.ShiftIndex, .dmrs_id = pdu->coreset.pdcch_dmrs_scrambling_id,
      .duration = pdu->coreset.duration, .bundle = pdu->coreset.RegBundleSize,
      .interleaver = pdu->coreset.InterleaverSize, .first_symbol = (uint8_t)first_symbol};
  uint16_t known[NR_PDCCH_BLIND_MAX_UE];
  int n_known = nr_pdcch_blind_monitor_dedicated_rnti_set(abs_slot, known, NR_PDCCH_BLIND_MAX_UE);
  uint16_t tracked[NR_PDCCH_USS_TRACKER_TOP];
  const int n_tracked = nr_pdcch_uss_tracker_peek(&tracker_geometry, tracked, NR_PDCCH_USS_TRACKER_TOP);
  for (int t = 0; t < n_tracked && n_known < NR_PDCCH_BLIND_MAX_UE; ++t) {
    bool duplicate = false;
    for (int k = 0; k < n_known; ++k) duplicate |= known[k] == tracked[t];
    if (!duplicate) known[n_known++] = tracked[t];
  }
  if (n_known > 0)
    nr_pdcch_uss_candidate_supports((span * pdu->coreset.duration) / 6, slot, known, n_known,
                                    pdu->CCE, pdu->L, n, support);
  const double prior_den = n_known > 0 ? 21.0 * n_known : 1.0; /* 3 hash classes x 7 M values */
  for (int i = 0; i < n; ++i) {
    dmrs[i] = nr_pdcch_dmrs_candidate_score(grid, offset, span, pdu->coreset.RegBundleSize,
        pdu->coreset.InterleaverSize, pdu->coreset.ShiftIndex, pdu->CCE[i], pdu->L[i]);
    if (!isfinite(dmrs[i])) return; /* unsupported mapping: preserve exhaustive decode */
    /* DMRS measures occupancy; the normalized USS term is the prior probability under every
     * still-possible standard SearchSpace configuration. Neither term is an acceptance gate. */
    score[i] = dmrs[i] + 2.0 * (double)support[i] / prior_den;
  }
  /* Persist one strongest observation per AL. p_real and sigma remain separate; the
   * noise-debiased DMRS occupancy alone weights the independent USS identity scorer. */
  nr_pdcch_uss_observation_t observation[NR_PDCCH_USS_TRACKER_AL];
  int n_observation = 0;
  for (int level = 1; level <= 16; level *= 2) {
    int count = 0, strongest = -1;
    double sum = 0.0, sum2 = 0.0;
    for (int i = 0; i < n; ++i) if (pdu->L[i] == level) {
      ++count; sum += dmrs[i]; sum2 += dmrs[i] * dmrs[i];
      if (strongest < 0 || dmrs[i] > dmrs[strongest]) strongest = i;
    }
    if (count < 2 || strongest < 0) continue;
    const double mean = sum / count;
    const double variance = fmax(0.0, sum2 / count - mean * mean);
    const double sigma = sqrt(variance);
    observation[n_observation++] = (nr_pdcch_uss_observation_t){
        .cce = pdu->CCE[strongest], .al = (uint8_t)level, .p_real = (float)dmrs[strongest],
        .sigma = (float)sigma, .score = (float)fmax(dmrs[strongest], 0.0)};
  }
  if (n_observation > 0)
    nr_pdcch_uss_tracker_observe(&tracker_geometry, abs_slot, (uint16_t)slot,
                                 (uint16_t)((span * pdu->coreset.duration) / 6),
                                 observation, n_observation);

  /* Exploration already rotates every visit. Keep a sub-1% full audit so the p99 metric is
   * representative of normal bounded work while still measuring the complete list. */
  const bool full = blind_dmrs_rank_mode() == 2 || (visit % 128) == 0;
  const int used = nr_pdcch_dmrs_candidate_order(score, pdu->L, n, visit, full, order);
  if (used < 1) return;
  const int best = order[0];
  for (int i = 0; i < used; ++i) { cce[i] = pdu->CCE[order[i]]; al[i] = pdu->L[order[i]]; }
  memcpy(pdu->CCE, cce, used * sizeof(cce[0]));
  memcpy(pdu->L, al, used * sizeof(al[0]));
  pdu->number_of_candidates = used;
  static _Atomic uint64_t calls = 0, offered = 0, selected = 0, audits = 0, known_calls = 0;
  const uint64_t t = atomic_fetch_add(&calls, 1) + 1;
  atomic_fetch_add(&offered, n);
  atomic_fetch_add(&selected, used);
  if (full) atomic_fetch_add(&audits, 1);
  if (n_known > 0) atomic_fetch_add(&known_calls, 1);
  if (t == 1 || t % 4096 == 0)
    LOG_A(PHY, "SENSING: DMRSRANK mode=%d geometries=%lu offered=%lu selected=%lu full=%lu "
               "uss=%lu known=%d offset=%d span=%d best=%.4f dmrs=%.4f support=%u kept=%d/%d\n",
          blind_dmrs_rank_mode(), (unsigned long)t, (unsigned long)atomic_load(&offered),
          (unsigned long)atomic_load(&selected), (unsigned long)atomic_load(&audits),
          (unsigned long)atomic_load(&known_calls), n_known, offset, span, score[best], dmrs[best],
          support[best], used, n);
}

/* Bounded diagnostic recording. This run is NOT a latency benchmark: file I/O is
 * explicitly enabled only by ISAC_PDCCH_DISCOVERY_REPLAY. Accepted CSS0 controls
 * and unverified primary hypotheses have distinct record kinds. */
static void blind_discovery_replay(const NR_DL_FRAME_PARMS *fp, const UE_nr_rxtx_proc_t *proc,
    const fapi_nr_dl_config_dci_dl_pdu_rel15_t *pdu, int span, int offset, int first_symbol,
    const c16_t *grid, const c16_t *fft, long source_absolute_slot, int expected_index, uint16_t expected_rnti,
    uint16_t expected_length, uint64_t expected_payload, const c16_t *expected)
{
  static int initialized;
  static FILE *file;
  static unsigned controls, hypotheses;
  static uint64_t last_geometry = UINT64_MAX, last_control = UINT64_MAX;
  if (!initialized) {
    initialized = 1;
    const char *path = getenv("ISAC_PDCCH_DISCOVERY_REPLAY");
    if (!path || !*path) return;
    file = fopen(path, "wbx");
    LOG_A(PHY, "PDCCHREPLAY %s path=%s; capture timing VOID for performance comparison\n",
          file ? "ARMED" : "VOID(open)", path);
  }
  if (!file) return;
  const bool control = expected_index >= 0;
  const uint64_t geometry = (uint64_t)offset | ((uint64_t)span << 9)
      | ((uint64_t)pdu->coreset.duration << 18) | ((uint64_t)pdu->coreset.RegBundleSize << 20)
      | ((uint64_t)pdu->coreset.InterleaverSize << 24) | ((uint64_t)pdu->coreset.ShiftIndex << 28);
  const uint64_t slot_key = (uint64_t)source_absolute_slot;
  if (control ? (controls >= 16 || slot_key == last_control) : (hypotheses >= 64 || geometry == last_geometry)) return;
  if (span < 6 || span > 270 || pdu->coreset.duration < 1 || pdu->coreset.duration > 3
      || pdu->number_of_candidates > 64 || (control && expected_index >= pdu->number_of_candidates)) return;
  nr_pdcch_discovery_replay_t h = {
    .magic=NR_PDCCH_REPLAY_MAGIC, .version=2, .header_bytes=sizeof(h), .kind=control?1:2,
    .source_slot=slot_key, .expected_payload=expected_payload,
    .frame=proc->frame_rx, .slot=proc->nr_slot_rx, .pci=fp->Nid_cell,
    .span=span, .offset=offset, .duration=pdu->coreset.duration, .first_symbol=first_symbol,
    .bundle=pdu->coreset.RegBundleSize, .interleaver=pdu->coreset.InterleaverSize,
    .shift=pdu->coreset.ShiftIndex, .dmrs_id=pdu->coreset.pdcch_dmrs_scrambling_id,
    .scrambling_rnti=pdu->coreset.scrambling_rnti, .n_candidates=pdu->number_of_candidates,
    .grid_count=span*9*pdu->coreset.duration, .fft_size=fp->ofdm_symbol_size,
    .carrier_rb=fp->N_RB_DL, .first_carrier_offset=fp->first_carrier_offset,
    .expected_index=control?expected_index:UINT32_MAX, .expected_rnti=expected_rnti,
    .expected_length=expected_length, .expected_re=control?54*pdu->L[expected_index]:0,
  };
  memcpy(h.cce,pdu->CCE,h.n_candidates*sizeof(h.cce[0]));
  memcpy(h.al,pdu->L,h.n_candidates*sizeof(h.al[0]));
  bool ok=fwrite(&h,sizeof(h),1,file)==1
      && fwrite(grid,sizeof(c16_t),h.grid_count,file)==h.grid_count
      && fwrite(fft,sizeof(c16_t),h.fft_size*h.duration,file)==h.fft_size*h.duration
      && (!h.expected_re || fwrite(expected,sizeof(c16_t),h.expected_re,file)==h.expected_re)
      && fflush(file)==0;
  if (!ok) { LOG_E(PHY,"PDCCHREPLAY VOID(write)\n"); fclose(file); file=NULL; return; }
  if (control) { ++controls; last_control=slot_key; } else { ++hypotheses; last_geometry=geometry; }
  LOG_A(PHY,"PDCCHREPLAY record kind=%s source=%lu span=%u controls=%u hypotheses=%u\n",
        control?"CSS0_SI":"UNVERIFIED",(unsigned long)slot_key,h.span,controls,hypotheses);
}

/* ---- LANE BATCH ---------------------------------------------------------------------------------
 * ONE GPU call per OCCASION instead of one per LANE.
 *
 * MEASURED 2026-09-20 (Swisscom PCI 382, K=96, GPU on, 100 % cache hit, 0 misses):
 *     30,000,000 decodes / 2,866 occasions   = ~10,500 decodes per occasion
 *     96 calls/occasion x ~90 us per call    = ~8,640 us
 *     prepass measured                        =  8,756 us
 * i.e. the per-CALL overhead accounts for essentially all of prepass, and the decodes themselves are
 * nearly free. That is also why growing the batch 5.4x (238 -> 1292 items) changed the time by 0.6 %:
 * items are cheap, calls are not. At 238 items a call is actually SLOWER than decoding on the CPU
 * (~33 us), so the per-lane integration was a pessimisation; the device only pays above ~640 items.
 *
 * So all lanes' (candidate x length) grids are gathered into ONE batch per occasion. The lane loop is
 * split: phase A builds each lane's candidate list, then one flush decodes everything, then phase B
 * runs each lane's sweep against the cache.
 *
 * Lanes are AL2-only (ln_L = 2 in the lane loop), so one vector is 2*108 = 216 int16.
 * Bounded by BOTH item and vector caps; anything that does not fit is simply left for the CPU path,
 * which is always correct because the scorer falls back per item. */
#define LANE_BATCH_VSTRIDE   (LANE_BATCH_AL_MAX * 108)  /* AL8: 864 REs */
#define LANE_BATCH_MAX_VEC   2048           /* distinct candidates across all lanes */
#define LANE_BATCH_MAX_ITEMS 131072          /* (candidate x length) pairs; must be <= NPG_MAX_ITEMS */
#define LANE_BATCH_MAX_LEN   64

typedef struct {
  int      base;        /* first item index for this lane, -1 = not in the batch */
  int      n_cand;
  int      min_len, max_len;
} lane_batch_slot_t;

/* HEAP, not __thread: at AL8 this buffer is 2048 * 864 * 2 = 3.5 MB, and a TLS block that size is
 * the documented cause of an AVX alignment fault in this project (per-antenna CFR buffer, same
 * shape of bug). Only the pointer is thread-local; allocated once per thread, 32-byte aligned. */
static __thread int16_t *g_lb_vec = NULL;
/* HEAP, not __thread: at 131072 items these six total ~2.4 MB, and a TLS block that size is the
 * documented shape of the AVX alignment fault this project already hit. One allocation, sliced. */
static __thread uint16_t *g_lb_vidx = NULL;
static __thread uint16_t *g_lb_len  = NULL;
static __thread uint8_t  *g_lb_al   = NULL;
static __thread uint32_t *g_lb_crc  = NULL;
static __thread uint64_t *g_lb_pl   = NULL;
static __thread uint8_t  *g_lb_ok   = NULL;
static __thread void     *g_lb_pool = NULL;

static bool lane_batch_vec_ready(void)
{
  if (g_lb_vec != NULL)
    return true;
  void *m = NULL;
  if (posix_memalign(&m, 32, sizeof(int16_t) * (size_t)LANE_BATCH_MAX_VEC * LANE_BATCH_VSTRIDE) != 0)
    return false;
  const size_t n = (size_t)LANE_BATCH_MAX_ITEMS;
  const size_t need = n * (sizeof(uint16_t) * 2 + sizeof(uint8_t) * 2 + sizeof(uint32_t) + sizeof(uint64_t))
                      + 6 * 32;   /* slack so each slice can start 32-byte aligned */
  void *q = NULL;
  if (posix_memalign(&q, 32, need) != 0) {
    free(m);
    return false;
  }
  g_lb_pool = q;
  uintptr_t c = (uintptr_t)q;
  #define LB_SLICE(T, cnt) ({ c = (c + 31u) & ~(uintptr_t)31u; T *r_ = (T *)c; c += sizeof(T) * (cnt); r_; })
  g_lb_vidx = LB_SLICE(uint16_t, n);
  g_lb_len  = LB_SLICE(uint16_t, n);
  g_lb_al   = LB_SLICE(uint8_t,  n);
  g_lb_crc  = LB_SLICE(uint32_t, n);
  g_lb_pl   = LB_SLICE(uint64_t, n);
  g_lb_ok   = LB_SLICE(uint8_t,  n);
  #undef LB_SLICE
  g_lb_vec = (int16_t *)m;
  return true;
}
static __thread lane_batch_slot_t g_lb_slot[NR_PDCCH_LOOKAHEAD_MAX];
static __thread int g_lb_n_items, g_lb_n_vec;
static __thread int g_lb_flushed;   /* 1 once decode_vec has run for this occasion */

static void lane_batch_reset(void)
{
  g_lb_n_items = 0;
  g_lb_n_vec   = 0;
  g_lb_flushed = 0;
  for (int i = 0; i < NR_PDCCH_LOOKAHEAD_MAX; i++)
    g_lb_slot[i].base = -1;
}

/* Add one lane's (candidate x length) grid. Unscrambles each candidate ONCE -- the unscrambling
 * depends on (e_rx, L, dmrs_id) and not on dci_length, so doing it inside the length loop repeated
 * it 34x for identical output. Returns false when the lane does not fit; that lane then uses the
 * CPU path untouched. */
static bool lane_batch_add(int lane, const nr_pdcch_autodiscover_sweep_ctx_t *ctx, int min_len, int max_len)
{
  if (lane < 0 || lane >= NR_PDCCH_LOOKAHEAD_MAX || ctx == NULL || ctx->n_cand <= 0)
    return false;
  if (!lane_batch_vec_ready())
    return false;                           /* no buffer: whole lane uses the CPU path, unchanged */
  if (min_len < 0 || max_len >= LANE_BATCH_MAX_LEN || max_len < min_len)
    return false;
  const int n_len = max_len - min_len + 1;
  if (g_lb_n_vec + ctx->n_cand > LANE_BATCH_MAX_VEC)
    return false;
  if (g_lb_n_items + ctx->n_cand * n_len > LANE_BATCH_MAX_ITEMS)
    return false;

  const int base = g_lb_n_items;
  for (int c = 0; c < ctx->n_cand; c++) {
    const nr_pdcch_autodiscover_cand_t *cd = &ctx->cand[c];
    if ((int)cd->L * 108 > LANE_BATCH_VSTRIDE)
      return false;                       /* wider than LANE_BATCH_AL_MAX: CPU path */
    const int v = g_lb_n_vec + c;
    nr_pdcch_unscrambling((c16_t *)cd->e_rx, ctx->scrambling_rnti, (uint32_t)(cd->L * 108),
                          ctx->dmrs_scrambling_id, &g_lb_vec[v * LANE_BATCH_VSTRIDE]);
    for (int l = min_len; l <= max_len; l++) {
      const int i = g_lb_n_items + c * n_len + (l - min_len);
      g_lb_vidx[i] = (uint16_t)v;
      g_lb_len[i]  = (uint16_t)l;
      g_lb_al[i]   = cd->L;
    }
  }
  g_lb_slot[lane].base    = base;
  g_lb_slot[lane].n_cand  = ctx->n_cand;
  g_lb_slot[lane].min_len = min_len;
  g_lb_slot[lane].max_len = max_len;
  g_lb_n_vec   += ctx->n_cand;
  g_lb_n_items += ctx->n_cand * n_len;
  return true;
}

/* One device call for every lane gathered this occasion. */
static void lane_batch_flush(void)
{
  g_lb_flushed = 0;
  if (g_lb_vec == NULL || g_lb_ok == NULL)
    return;                                  /* nothing was ever added on this thread */
  { /* WHY-NOT diagnostic: a silent early return here costs double work (batch built, CPU still
     * decodes), so report the reason once per 5000 occasions rather than guessing. */
    static _Atomic uint64_t s_why = 0;
    const uint64_t w = atomic_fetch_add_explicit(&s_why, 1, memory_order_relaxed) + 1;
    if (w == 1 || (w % 5000) == 0) {
      const nr_gpu_polar_api_t *a = nr_gpu_polar_load();
      LOG_A(PHY, "SENSING: LANEBATCH-WHY items=%d vec=%d api=%s\n",
            g_lb_n_items, g_lb_n_vec, (a && a->decode_vec) ? "yes" : "NO");
    }
  }
  if (g_lb_n_items <= 0 || g_lb_n_vec <= 0)
    return;
  const nr_gpu_polar_api_t *api = nr_gpu_polar_load();
  if (api == NULL || api->decode_vec == NULL)
    return;                                  /* no GPU: every lane falls back to CPU, unchanged */
  memset(g_lb_ok, 0, (size_t)g_lb_n_items);
  const int m = api->decode_vec(g_lb_vec, LANE_BATCH_VSTRIDE, g_lb_n_vec, g_lb_vidx, g_lb_len,
                                g_lb_al, g_lb_n_items, g_lb_crc, g_lb_pl, g_lb_ok);
  if (m < 0)
    return;
  g_lb_flushed = 1;
  static _Atomic uint64_t s_calls = 0, s_items = 0;
  const uint64_t n = atomic_fetch_add_explicit(&s_calls, 1, memory_order_relaxed) + 1;
  atomic_fetch_add_explicit(&s_items, (uint64_t)g_lb_n_items, memory_order_relaxed);
  if (n == 1 || (n % 2000) == 0)
    LOG_A(PHY, "SENSING: LANEBATCH call #%llu: %d items, %d vectors, %llu items total (1 call/occasion)\n",
          (unsigned long long)n, g_lb_n_items, g_lb_n_vec,
          (unsigned long long)atomic_load_explicit(&s_items, memory_order_relaxed));
}

/* Per-lane sweep contexts live in an array so the scorer can recover its lane index by pointer
 * arithmetic -- O(1), and it keeps nr_pdcch_autodiscover_sweep_ctx_t unchanged. */
static __thread nr_pdcch_autodiscover_sweep_ctx_t g_lane_sweep_ctx[NR_PDCCH_LOOKAHEAD_MAX];
static __thread nr_pdcch_autodiscover_cand_t      g_lane_disc_cand[NR_PDCCH_LOOKAHEAD_MAX][45];
static __thread int                               g_lane_disc_n[NR_PDCCH_LOOKAHEAD_MAX];
static __thread uint8_t                           g_lane_needs_sweep[NR_PDCCH_LOOKAHEAD_MAX];
static __thread nr_pdcch_lookahead_geom_t         g_lane_geom_snap[NR_PDCCH_LOOKAHEAD_MAX];

/* Cached result for (lane, candidate, length); false = decode it on the CPU as before. */
static bool lane_batch_get(const void *ctx, int cand_idx, int dci_length, uint32_t *crc, uint64_t *payload)
{
  if (!g_lb_flushed || g_lb_ok == NULL)
    return false;
  const nr_pdcch_autodiscover_sweep_ctx_t *c = (const nr_pdcch_autodiscover_sweep_ctx_t *)ctx;
  if (c < &g_lane_sweep_ctx[0] || c >= &g_lane_sweep_ctx[NR_PDCCH_LOOKAHEAD_MAX])
    return false;                            /* not a lane context (e.g. the primary DL sweep) */
  const int lane = (int)(c - &g_lane_sweep_ctx[0]);
  const lane_batch_slot_t *sl = &g_lb_slot[lane];
  if (sl->base < 0 || cand_idx < 0 || cand_idx >= sl->n_cand)
    return false;
  if (dci_length < sl->min_len || dci_length > sl->max_len)
    return false;
  const int n_len = sl->max_len - sl->min_len + 1;
  const int i = sl->base + cand_idx * n_len + (dci_length - sl->min_len);
  if (i < 0 || i >= g_lb_n_items || !g_lb_ok[i])
    return false;
  *crc     = g_lb_crc[i];
  *payload = g_lb_pl[i];
  return true;
}

/* ---- SWEEP GPU BATCH ---------------------------------------------------------------------------
 * The DCI length sweep is 99 % of an occasion, and it was doing two things wrong per item.
 *
 * MEASURED 2026-09-20 (Swisscom PCI 382, K=64): prepass 12618us mean = 76 % of a 16.6 ms occasion,
 * with over_slot(500us) = 32393/32393 -- every occasion overran its slot ~33x, which is why K=64
 * froze the host and the sweep never left candidate 1/133. prepass SPANS the lane loop, and each
 * lane runs a full sweep: 34 lengths x up to 45 candidates. The `decode` bucket (81us) times only
 * Phase 1 of the primary config, so it hid this entirely.
 *
 * Two fixes, both here:
 *
 * 1. UNSCRAMBLE ONCE PER CANDIDATE. nr_pdcch_unscrambling() depends on (e_rx, L, dmrs_id) -- NOT on
 *    dci_length -- yet it sat inside the length loop and ran 34 times per candidate for identical
 *    output. Same redundancy class as the per-extent LLR hoist, which took fep_llr from ~98 % to
 *    0.2 % of the occasion.
 *
 * 2. ONE GPU BATCH PER SWEEP. Every (candidate, length) pair is an independent polar decode, so the
 *    whole grid goes to npg_decode_vec() in a single call: n_vec distinct unscrambled vectors,
 *    vidx[] selecting which one each item uses. That is exactly the shape this API was built for.
 *    It also avoids the failure mode of the earlier GPU A/B on this rig (20x SLOWER because
 *    consumers blocked and the batch degenerated to 1): here 1530 items are in hand at once.
 *
 * Falls back to the CPU path whenever the GPU is absent, NR_GPU_POLAR is unset, the grid does not
 * fit, or any item fails -- the scorer below checks `ok` per item and decodes that one on CPU. So
 * this can only be faster, never wrong. */
#define SWEEP_BATCH_MAX_CAND 45
#define SWEEP_BATCH_MAX_LEN  64
#define SWEEP_BATCH_VSTRIDE  (16 * 108)

typedef struct {
  const void *ctx;                 /* which sweep context this cache belongs to */
  int         n_cand, min_len, max_len;
  uint8_t     valid;
  uint8_t     ok[SWEEP_BATCH_MAX_CAND][SWEEP_BATCH_MAX_LEN];
  uint32_t    crc[SWEEP_BATCH_MAX_CAND][SWEEP_BATCH_MAX_LEN];
  uint64_t    payload[SWEEP_BATCH_MAX_CAND][SWEEP_BATCH_MAX_LEN];
} sweep_batch_cache_t;

static __thread sweep_batch_cache_t g_sweep_cache;

/* Unscramble each candidate once, then decode the whole (candidate x length) grid in one GPU call.
 * Returns true when the cache is populated; false leaves the scorer on its original CPU path. */
static bool sweep_gpu_prefill(const nr_pdcch_autodiscover_sweep_ctx_t *ctx, int min_len, int max_len)
{
  g_sweep_cache.valid = 0;
  if (ctx == NULL || ctx->n_cand <= 0 || ctx->n_cand > SWEEP_BATCH_MAX_CAND)
    return false;
  if (min_len < 0 || max_len >= SWEEP_BATCH_MAX_LEN || max_len < min_len)
    return false;
  const nr_gpu_polar_api_t *api = nr_gpu_polar_load();
  if (api == NULL || api->decode_vec == NULL)
    return false;

  const int n_len = max_len - min_len + 1;
  const int n_items = ctx->n_cand * n_len;
  static __thread int16_t  vec[SWEEP_BATCH_MAX_CAND * SWEEP_BATCH_VSTRIDE];
  static __thread uint16_t vidx[SWEEP_BATCH_MAX_CAND * SWEEP_BATCH_MAX_LEN];
  static __thread uint16_t lens[SWEEP_BATCH_MAX_CAND * SWEEP_BATCH_MAX_LEN];
  static __thread uint8_t  als[SWEEP_BATCH_MAX_CAND * SWEEP_BATCH_MAX_LEN];
  static __thread uint32_t crcs[SWEEP_BATCH_MAX_CAND * SWEEP_BATCH_MAX_LEN];
  static __thread uint64_t pls[SWEEP_BATCH_MAX_CAND * SWEEP_BATCH_MAX_LEN];
  static __thread uint8_t  oks[SWEEP_BATCH_MAX_CAND * SWEEP_BATCH_MAX_LEN];
  if (n_items > (int)(sizeof(vidx) / sizeof(vidx[0])))
    return false;

  /* (1) one unscramble per candidate, not per (candidate, length) */
  for (int c = 0; c < ctx->n_cand; c++) {
    const nr_pdcch_autodiscover_cand_t *cd = &ctx->cand[c];
    if ((int)cd->L * 108 > SWEEP_BATCH_VSTRIDE)
      return false;
    nr_pdcch_unscrambling((c16_t *)cd->e_rx, ctx->scrambling_rnti, (uint32_t)(cd->L * 108),
                          ctx->dmrs_scrambling_id, &vec[c * SWEEP_BATCH_VSTRIDE]);
  }
  /* (2) every (candidate, length) pair as one batch item */
  int n = 0;
  for (int c = 0; c < ctx->n_cand; c++)
    for (int l = min_len; l <= max_len; l++) {
      vidx[n] = (uint16_t)c;
      lens[n] = (uint16_t)l;
      als[n]  = ctx->cand[c].L;
      n++;
    }
  const int m = api->decode_vec(vec, SWEEP_BATCH_VSTRIDE, ctx->n_cand, vidx, lens, als, n, crcs, pls, oks);
  if (m < 0)
    return false;

  memset(g_sweep_cache.ok, 0, sizeof(g_sweep_cache.ok));
  for (int i = 0; i < n; i++) {
    const int c = vidx[i], l = lens[i];
    if (!oks[i])
      continue;
    g_sweep_cache.ok[c][l]      = 1;
    g_sweep_cache.crc[c][l]     = crcs[i];
    g_sweep_cache.payload[c][l] = pls[i];
  }
  g_sweep_cache.ctx     = ctx;
  g_sweep_cache.n_cand  = ctx->n_cand;
  g_sweep_cache.min_len = min_len;
  g_sweep_cache.max_len = max_len;
  g_sweep_cache.valid   = 1;

  static _Atomic uint64_t s_batches = 0, s_items = 0;
  const uint64_t b = atomic_fetch_add_explicit(&s_batches, 1, memory_order_relaxed) + 1;
  atomic_fetch_add_explicit(&s_items, (uint64_t)m, memory_order_relaxed);
  if (b == 1 || (b % 5000) == 0)
    LOG_A(PHY, "SENSING: sweep GPU batch #%llu: %d items (%d cand x %d len), %llu decoded total\n",
          (unsigned long long)b, n, ctx->n_cand, n_len,
          (unsigned long long)atomic_load_explicit(&s_items, memory_order_relaxed));
  return true;
}

/* UL length evidence is independent of both the DL sweep and field interpretation. */
typedef struct {
  const nr_pdcch_autodiscover_cand_t *cand;
  int count;
  uint16_t rnti, scrambling_rnti, dmrs_id;
  const nr_pdcch_autodiscover_sweep_ctx_t *gpu_ctx;  /* sweep_gpu_prefill()'d for these cands, or NULL */
} ul_length_ctx_t;
static bool ul_length_score(int len, int trial, uint16_t *rnti, uint32_t *hash, void *opaque)
{
  const ul_length_ctx_t *ctx=opaque;
  const int ci=trial%ctx->count;
  /* GPU SWEEP CACHE, same batch the DL sweep uses: the polar decode is format-agnostic, only the
   * admission differs -- mirror nr_pdcch_blind_decode_raw_01(): exact bootstrap RNTI (rnti_min ==
   * rnti_max == ctx->rnti) and format indicator 0. A miss falls through to the CPU path unchanged. */
  if (ctx->gpu_ctx && g_sweep_cache.valid && g_sweep_cache.ctx == ctx->gpu_ctx
      && ci < g_sweep_cache.n_cand && len >= g_sweep_cache.min_len && len <= g_sweep_cache.max_len
      && g_sweep_cache.ok[ci][len]) {
    const uint32_t crc=g_sweep_cache.crc[ci][len];
    const uint64_t pl=g_sweep_cache.payload[ci][len];
    if ((crc>>16)==0 && (uint16_t)crc==ctx->rnti && ((pl>>(len-1))&1)==0) {
      *rnti=(uint16_t)crc;
      *hash=(uint32_t)pl ^ (uint32_t)(pl>>32);
      discovery_evidence("sweep_crc", "UL", discovery_evidence_frame, discovery_evidence_slot,
                          len, (uint16_t)crc, pl, ctx->cand[ci].L, ctx->cand[ci].cce);
      return true;
    }
  }
  const nr_pdcch_autodiscover_cand_t *c=&ctx->cand[ci];
  int16_t llr[16*108];
  nr_pdcch_blind_ul_result_t out;
  /* TS 38.211 7.3.2.3: a USS under a CORESET with pdcch-DMRS-ScramblingID uses the
   * C-RNTI in the data scrambling initialization. The encrypted dedicated RRC leaves a passive
   * receiver unable to know whether that optional field is present, so try both legal modes once
   * an exact OTA-verified RNTI is available. An unanchored sweep still tries only zero. */
  nr_pdcch_unscrambling((c16_t *)c->e_rx,ctx->scrambling_rnti,c->L*108,ctx->dmrs_id,llr);
  bool ok=nr_pdcch_blind_decode_raw_01(llr,c->L,len,ctx->rnti,ctx->rnti,&out);
  bool used_alt = false;
  if(!ok && ci == 0 && ctx->rnti != 0 && ctx->rnti != ctx->scrambling_rnti) {
    nr_pdcch_unscrambling((c16_t *)c->e_rx,ctx->rnti,c->L*108,ctx->dmrs_id,llr);
    ok=nr_pdcch_blind_decode_raw_01(llr,c->L,len,ctx->rnti,ctx->rnti,&out);
    used_alt = ok;
  }
  if(!ok) return false;
  if (used_alt)
    LOG_A(PHY, "SENSING: RNTI_SCRAMBLE_HIT direction=UL rnti=0x%04x len=%d AL=%u CCE=%u\n",
          ctx->rnti, len, c->L, c->cce);
  *rnti=out.rnti;
  *hash=(uint32_t)out.raw_payload ^ (uint32_t)(out.raw_payload>>32);
  discovery_evidence("sweep_crc", "UL", discovery_evidence_frame, discovery_evidence_slot,
                      len, out.rnti, out.raw_payload, c->L, c->cce);
  return true;
}

static bool nr_pdcch_autodiscover_length_scorer(int dci_length, int trial_idx, uint16_t *rnti_out,
                                                uint32_t *payload_hash_out, void *user_ctx)
{
  const nr_pdcch_autodiscover_sweep_ctx_t *ctx = (const nr_pdcch_autodiscover_sweep_ctx_t *)user_ctx;
  if (ctx == NULL || ctx->n_cand <= 0) {
    return false;
  }
  const int cand_idx = trial_idx % ctx->n_cand;
  const nr_pdcch_autodiscover_cand_t *evidence_cand = &ctx->cand[cand_idx];
  /* LANE BATCH first: one device call per occasion covered this lane's whole grid. Same admission
   * test the CPU path applies, so a cached result can never be accepted on weaker evidence. */
  {
    uint32_t bcrc = 0;
    uint64_t bpl  = 0;
    if (lane_batch_get(ctx, cand_idx, dci_length, &bcrc, &bpl)) {
      static _Atomic uint64_t s_bh = 0;
      const uint64_t bh = atomic_fetch_add_explicit(&s_bh, 1, memory_order_relaxed) + 1;
      if ((bh % 20000000) == 0)
        LOG_A(PHY, "SENSING: LANEBATCH hit=%llu\n", (unsigned long long)bh);
      /* BOTH tests nr_pdcch_blind_decode_raw_11() applies, and it is the ground truth for what a
       * decode means here: CRC-recovered value in the plausible RNTI range, AND payload bit
       * (dci_length-1) set. That second one is its "format indicator=0 (UL grant, not DL)" reject.
       * A cached result must be judged on exactly the evidence a computed one is. */
      if ((bcrc >> 16) == 0 && bcrc >= ctx->rnti_min && bcrc <= ctx->rnti_max
          && ((bpl >> (dci_length - 1)) & 1) != 0) {
        *rnti_out         = sweep_evidence_rnti(ctx, (uint16_t)bcrc, dci_length, evidence_cand);
        *payload_hash_out = (uint32_t)bpl ^ (uint32_t)(bpl >> 32);
        discovery_evidence("sweep_crc", "DL", discovery_evidence_frame, discovery_evidence_slot,
                            dci_length, (uint16_t)bcrc, bpl, evidence_cand->L, evidence_cand->cce);
        return true;
      }
    }
  }
  /* SWEEPCACHE: is the GPU result actually being USED? A miss means we paid for the batch AND
   * still decode on CPU -- which would explain why prepass did not move. Counted, not assumed. */
  {
    static _Atomic uint64_t s_hit = 0, s_miss = 0;
    const bool usable = g_sweep_cache.valid && g_sweep_cache.ctx == ctx
                        && cand_idx < g_sweep_cache.n_cand
                        && dci_length >= g_sweep_cache.min_len && dci_length <= g_sweep_cache.max_len
                        && g_sweep_cache.ok[cand_idx][dci_length];
    const uint64_t h = usable ? atomic_fetch_add_explicit(&s_hit, 1, memory_order_relaxed) + 1
                              : atomic_load_explicit(&s_hit, memory_order_relaxed);
    const uint64_t m = usable ? atomic_load_explicit(&s_miss, memory_order_relaxed)
                              : atomic_fetch_add_explicit(&s_miss, 1, memory_order_relaxed) + 1;
    if (((h + m) % 2000000) == 0)
      LOG_A(PHY, "SENSING: SWEEPCACHE hit=%llu miss=%llu (%.1f%% hit)\n",
            (unsigned long long)h, (unsigned long long)m, 100.0 * (double)h / (double)(h + m));
  }
  /* SWEEP GPU BATCH: prefilled by sweep_gpu_prefill() for this exact ctx. A miss (GPU absent, item
   * rejected, length outside the batched range) falls through to the CPU path below unchanged. */
  if (g_sweep_cache.valid && g_sweep_cache.ctx == ctx && cand_idx < g_sweep_cache.n_cand
      && dci_length >= g_sweep_cache.min_len && dci_length <= g_sweep_cache.max_len
      && g_sweep_cache.ok[cand_idx][dci_length]) {
    const uint32_t crc = g_sweep_cache.crc[cand_idx][dci_length];
    const uint64_t pl  = g_sweep_cache.payload[cand_idx][dci_length];
    const bool crc_ok = (crc >> 16) == 0 && crc >= ctx->rnti_min && crc <= ctx->rnti_max;
    /* BUG (found 2026-09-20): this path claimed parity with nr_pdcch_blind_decode_raw_11() but
     * applied only the RNTI-range half of it, so it admitted format-indicator=0 candidates -- UL
     * grants -- that the CPU path rejects. Roughly a factor 2 of extra false accepts fed straight
     * into the sweep's bootstrap-hit statistics, which is exactly the noise the >1-hit threshold
     * is trying to stand above. */
    if (crc_ok && ((pl >> (dci_length - 1)) & 1) != 0) {
      *rnti_out         = sweep_evidence_rnti(ctx, (uint16_t)crc, dci_length, evidence_cand);
      *payload_hash_out = (uint32_t)pl ^ (uint32_t)(pl >> 32);
      discovery_evidence("sweep_crc", "DL", discovery_evidence_frame, discovery_evidence_slot,
                          dci_length, (uint16_t)crc, pl, evidence_cand->L, evidence_cand->cce);
      return true;
    }
  }
  const nr_pdcch_autodiscover_cand_t *c = &ctx->cand[cand_idx];
  // Same unscramble step nr_pdcch_blind_cand_worker_body() below uses on the identical cursor
  // (t->e_rx from the same pdcch_e_rx[]/e_rx_cand_idx walk), just with `dci_length` substituted
  // for the hypothesis under test instead of the (as yet unknown) real one.
  int16_t tmp_e[16 * 108];
  nr_pdcch_blind_raw_result_t out;
  nr_pdcch_unscrambling((c16_t *)c->e_rx, ctx->scrambling_rnti, (uint32_t)(c->L * 108), ctx->dmrs_scrambling_id,
                        tmp_e);
  bool ok=nr_pdcch_blind_decode_raw_11(tmp_e,c->L,(uint16_t)dci_length,ctx->rnti_min,ctx->rnti_max,&out);
  bool used_alt = false;
  if(!ok && cand_idx == 0 && ctx->bootstrap_alias != 0 && ctx->bootstrap_alias != ctx->scrambling_rnti) {
    nr_pdcch_unscrambling((c16_t *)c->e_rx, ctx->bootstrap_alias, (uint32_t)(c->L * 108),
                          ctx->dmrs_scrambling_id, tmp_e);
    ok=nr_pdcch_blind_decode_raw_11(tmp_e,c->L,(uint16_t)dci_length,ctx->rnti_min,ctx->rnti_max,&out);
    used_alt = ok;
  }
  if(!ok) return false;
  if (used_alt)
    LOG_A(PHY, "SENSING: RNTI_SCRAMBLE_HIT direction=DL rnti=0x%04x len=%d AL=%u CCE=%u\n",
          ctx->bootstrap_alias, dci_length, c->L, c->cce);
  /* Never ask a layout-dependent extractor to judge a length. An unknown TDA
   * width previously rejected genuine CRC-recovered 47-bit grants against a
   * presumed 51-bit field list, so discovery could never reach interpretation. */
  *rnti_out=sweep_evidence_rnti(ctx, out.rnti, dci_length, evidence_cand);
  *payload_hash_out=(uint32_t)out.payload ^ (uint32_t)(out.payload>>32);
  discovery_evidence("sweep_crc", "DL", discovery_evidence_frame, discovery_evidence_slot,
                      dci_length, out.rnti, out.payload, evidence_cand->L, evidence_cand->cce);

  return true;
}

// ---- Parallel per-candidate decode (2026-08-05) ------------------------------------------------
// MEASURED live: unscrambling + polar decode (Step 1's SCL search) + the mismatched-bits re-encode
// check are, per candidate, by far the most expensive work in this file, and ran strictly
// sequentially even though no candidate's decode depends on another's -- one core pegged at
// ~80-90% while 5 of 8 cores sat completely idle. This fans that independent work out across the
// UE's existing thread pool (get_nrUE_params()->Tpool, already used elsewhere on this RT path --
// see nr_initial_sync.c's GSCN scan for the same fork-join pattern this mirrors). Anything with a
// genuine sequential dependency (the dci_thres EMA, the RNTI persistence ring buffer, CFR/PDSCH
// decode submission) stays on the calling thread, in original candidate order, in a second pass
// AFTER the join -- those are cheap and only reached for the rare candidate that survives decode,
// so leaving them sequential costs nothing and avoids adding locking to genuinely shared state.
typedef struct {
  const c16_t *e_rx;
  uint8_t      L;
  uint16_t     dci_length;
  uint16_t     bwp_size;
  uint8_t      dmrs_typeA_position;
  uint16_t     rnti_min;
  uint16_t     rnti_max;
  const nr_pdcch_blind_extract_opts_t *extract_opts;
  uint16_t     scrambling_rnti;
  uint16_t     alternate_scrambling_rnti; /* exact verified C-RNTI, 0 when unavailable */
  uint16_t     dmrs_scrambling_id;
  int          frame;   /* LLRPROBE correlation only */
  int          slot;    /* LLRPROBE correlation only */
  int          cce;     /* LLRPROBE correlation only */
  /* Which DCI format to interpret this candidate's payload as. The two formats have DIFFERENT
     payload widths, so scanning both means two INDEPENDENT tasks over the same LLR slice -- the
     polar decoder is sized by dci_length and there is no way to share the decode. */
  uint8_t      format;  // nr_blind_dci_format_t
  const nr_pdcch_blind_dci10_ctx_t *dci10_ctx; // format 1_0 only; NULL for 1_1
  /* UPLINK. A separate flag rather than a third value of `format`, so that no existing switch or
     comparison over nr_blind_dci_format_t silently acquires a new reachable case -- the DL path
     must be unable to see this task kind at all. */
  bool         dl_auto;
  uint64_t     dl_layout_configuration;
  uint16_t     dl_layout_index; /* resolver index of the layout decoded under (0xFFFF = none) */
  nr_pdcch_blind_raw_result_t dl_raw;
  bool         ul_auto; // raw decode; sequential controller interprets the CRC-verified bits
  uint8_t      ul_scan; // 1 = interpret this candidate as DCI 0_1; `format` is then meaningless
  const nr_pdcch_blind_ul_opts_t *ul_opts;
  nr_pdcch_blind_ul_result_t ul_out; // OUTPUT when ul_scan
  nr_pdcch_blind_result_t out; // OUTPUT
  bool         ok;             // OUTPUT
  int8_t       bwp_entry;      // passive BWP entry this length belongs to (0 = the configured BWP)
  uint8_t      bwp_probe;      // 1 = raw decode only: BWP discovery / DM-RS scoring probe
  uint8_t      open_rnti;      // 1 = open RNTI range at a known length: finds UEs not yet resolved
  bool         is_lookahead;   // multi-candidate-per-occasion lookahead task (see the lookahead block)
  int8_t       lookahead_lane; // which lane; valid only when is_lookahead
  task_ans_t  *ans;
} nr_pdcch_blind_cand_task_t;

/* ---- Passive DL BWP tracking (nr_passive_bwp.h). ISAC_BWP_TRACK=1, or on under V2. The RT thread
 * registers entries and tracks RNTIs; decode consumers resolve (size, start) from the DM-RS, so every
 * access is under g_pbwp_lock and the scan works from a per-occasion snapshot. ---- */
static nr_pbwp_t g_pbwp;
static pthread_mutex_t g_pbwp_lock = PTHREAD_MUTEX_INITIALIZER;
static nr_pdcch_blind_extract_opts_t g_pbwp_opts[NR_PBWP_MAX];
static int nr_pbwp_enabled(void)
{
  static int s_on = -1;
  if (s_on < 0) {
    const char *e = getenv("ISAC_BWP_TRACK");
    s_on = (e != NULL) ? (atoi(e) != 0) : nr_agnostic_v2();
  }
  return s_on;
}
/* TB-CRC outcome of a grant decoded against a DISCOVERED BWP: 32 failures with no pass un-resolve it. */
void nr_pdcch_bwp_crc_result(int entry, bool crc_ok)
{
  pthread_mutex_lock(&g_pbwp_lock);
  const bool was = nr_pbwp_resolved(&g_pbwp, entry);
  nr_pbwp_feed_crc(&g_pbwp, entry, crc_ok);
  if (was && !nr_pbwp_resolved(&g_pbwp, entry))
    LOG_A(PHY, "SENSING: BWP UNRESOLVED entry=%d: 32 TB-CRC failures, no pass -- rescoring from the DM-RS\n", entry);
  pthread_mutex_unlock(&g_pbwp_lock);
}
void nr_pdcch_bwp_probe_result(int entry, uint64_t payload, const float *prb_coh)
{
  pthread_mutex_lock(&g_pbwp_lock);
  if (entry > 0 && entry < g_pbwp.n && nr_pbwp_score_grant(&g_pbwp, entry, payload, prb_coh))
    LOG_A(PHY, "SENSING: BWP RESOLVED entry=%d len=%u size=%u start=%d ind_bits=%u after %u grants\n", entry,
          g_pbwp.e[entry].dci_len, g_pbwp.e[entry].size, g_pbwp.e[entry].start, g_pbwp.e[entry].ind_bits,
          g_pbwp.e[entry].grants_scored);
  pthread_mutex_unlock(&g_pbwp_lock);
}

/* The candidate body WITHOUT the task_ans handshake, for the serial path. Split rather than passing
 * a flag so the parallel worker keeps exactly its previous shape and the pool contract (every task
 * must signal completion exactly once) cannot be broken by a wrong flag. */
static void nr_pdcch_blind_cand_worker_body(nr_pdcch_blind_cand_task_t *t)
{
  int16_t tmp_e[16 * 108];
  uint16_t data_scrambling_rnti = t->scrambling_rnti;
  bool used_alternate_scrambling = false;
retry_scrambling:
  nr_pdcch_unscrambling((c16_t *)t->e_rx, data_scrambling_rnti, (uint32_t)(t->L * 108), t->dmrs_scrambling_id, tmp_e);
  if (t->ul_scan) {
    t->ok = nr_pdcch_blind_decode_01_mode(t->ul_auto,tmp_e,t->L,t->dci_length,t->ul_opts,
                                          t->rnti_min,t->rnti_max,&t->ul_out);
  } else if (t->format == NR_BLIND_DCI_FORMAT_1_0) {
    nr_dci10_interpretation_report_t report;
    t->ok = nr_pdcch_blind_decode_10_mode(t->dl_auto, tmp_e, t->L, t->dci_length,
                                          t->dci10_ctx, t->rnti_min, t->rnti_max,
                                          t->extract_opts, &t->out, &report);
    if (t->dl_auto && report.attempted) {
      LOG_D(PHY, "DCI_INTERPRET format=1_0 frame=%d slot=%d cce=%d rnti=0x%04x "
                 "payload=0x%016lx bits=%u candidates=%u surviving=%u state=%s "
                 "unique=%d evidence=protocol_only scope=supplied_context\n",
            t->frame, t->slot, t->cce, t->out.rnti, (unsigned long)t->out.payload,
            t->dci_length, report.attempted, report.surviving,
            nr_dci_interpretation_state_name(report.state), report.unique_candidate);
      for (unsigned i = 0; i < report.attempted; ++i) {
        const nr_pdcch_blind_result_t *h = &report.candidates[i];
        LOG_D(PHY, "DCI_HYPOTHESIS rnti=0x%04x candidate=%u class=%u protocol=%s "
                   "reason=%s PRB=%u+%u symbols=%u+%u mcs=%u rv=%u harq=%u dmrs=0x%x\n",
              t->out.rnti, i, h->rnti_class, h->plausible ? "PASS" : "REJECT",
              h->reject_reason ? h->reject_reason : "independent_validation_pending",
              h->start_rb, h->num_rb, h->start_symbol, h->num_symbols,
              h->mcs, h->rv, h->harq_pid, h->dl_dmrs_symb_pos);
      }
    }
  } else if (t->dl_auto || t->bwp_probe) {
    t->ok = nr_pdcch_blind_decode_raw_11(tmp_e, t->L, t->dci_length,
                                        t->rnti_min, t->rnti_max, &t->dl_raw);
  } else {
    t->ok = nr_pdcch_blind_decode_and_extract_ex(tmp_e, t->L, t->dci_length, t->bwp_size, t->dmrs_typeA_position,
                                                 t->rnti_min, t->rnti_max, t->extract_opts, &t->out);
  }
  if (!t->ok && t->alternate_scrambling_rnti != 0
      && t->alternate_scrambling_rnti != data_scrambling_rnti) {
    data_scrambling_rnti = t->alternate_scrambling_rnti;
    used_alternate_scrambling = true;
    goto retry_scrambling;
  }
  /* UNKNOWN-RNTI FALLBACK (opt-in, ISAC_PDCCH_JOINT=1; NOT validated on air). Only the raw DL path: the format
   * 1_0 / UL / extract-and-interpret branches are left exactly as they were. tmp_e is what the LAST iteration
   * above descrambled with data_scrambling_rnti, which is what the solver's model must be told. */
  if (!t->ok && !t->ul_scan && t->format != NR_BLIND_DCI_FORMAT_1_0 && nr_pdcch_joint_live_enabled()) {
    nr_pdcch_joint_live_result_t jr;
    if (nr_pdcch_joint_live_decode_11(tmp_e, t->L, t->dci_length, t->dmrs_scrambling_id, (int)data_scrambling_rnti,
                                      t->rnti_min, t->rnti_max, &jr)) {
      nr_pdcch_blind_raw_result_t raw = {jr.payload, jr.rnti, jr.mismatched_bits, NULL};
      if (t->dl_auto || t->bwp_probe) {
        t->dl_raw = raw;
        t->ok = true;
      } else { /* the extract-and-interpret path: raw decode + nr_pdcch_blind_extract_11(), as blind_decode_and_extract does */
        t->ok = nr_pdcch_blind_extract_11(&raw, t->dci_length, t->bwp_size, t->dmrs_typeA_position, t->extract_opts, &t->out);
      }
      static _Atomic unsigned s_jhit;
      const unsigned h = atomic_fetch_add_explicit(&s_jhit, 1, memory_order_relaxed) + 1;
      if (h <= 20 || (h % 1000) == 0)
        LOG_A(PHY, "SENSING: JOINT_RNTI_HIT #%u rnti=0x%04x len=%u AL=%u CCE=%d mismatched_bits=%u path=%s ok=%d (unknown-RNTI solve)\n",
              h, jr.rnti, t->dci_length, t->L, t->cce, jr.mismatched_bits, (t->dl_auto || t->bwp_probe) ? "raw" : "extract", (int)t->ok);
    }
  }
  /* Same fallback for the UL grant (DCI 0_1) raw path: the same solve with the opposite format-indicator admission,
   * filled exactly as nr_pdcch_blind_decode_raw_01() fills its result. */
  if (!t->ok && t->ul_scan && t->ul_auto && nr_pdcch_joint_live_enabled()) {
    nr_pdcch_joint_live_result_t jr;
    if (nr_pdcch_joint_live_decode(tmp_e, t->L, t->dci_length, t->dmrs_scrambling_id, (int)data_scrambling_rnti,
                                   t->rnti_min, t->rnti_max, /*indicator=*/0, &jr)) {
      memset(&t->ul_out, 0, sizeof(t->ul_out));
      t->ul_out.width_hyp_class = t->ul_out.interp_hyp_class = -1;
      t->ul_out.plausible = false;
      t->ul_out.ul_dci_format = NR_BLIND_UL_DCI_FORMAT_0_1;
      t->ul_out.dci_length = t->dci_length;
      t->ul_out.raw_payload = jr.payload;
      t->ul_out.crc_rnti = jr.rnti;
      t->ul_out.rnti = jr.rnti;
      t->ul_out.mismatched_bits = jr.mismatched_bits;
      t->ok = true;
      static _Atomic unsigned s_jul;
      const unsigned h = atomic_fetch_add_explicit(&s_jul, 1, memory_order_relaxed) + 1;
      if (h <= 20 || (h % 1000) == 0)
        LOG_A(PHY, "SENSING: JOINT_RNTI_HIT_UL #%u rnti=0x%04x len=%u AL=%u CCE=%d mismatched_bits=%u (unknown-RNTI solve)\n",
              h, jr.rnti, t->dci_length, t->L, t->cce, jr.mismatched_bits);
    }
  }
  if (t->ok && used_alternate_scrambling)
    LOG_A(PHY, "SENSING: RNTI_SCRAMBLE_HIT direction=%s rnti=0x%04x len=%u AL=%u CCE=%d\n",
          t->ul_scan ? "UL" : "DL", t->alternate_scrambling_rnti,
          t->dci_length, t->L, t->cce);
  {
    extern void nr_pdcch_llr_probe(const char *, int, int, int, int, uint32_t, const int16_t *, int);
    nr_pdcch_llr_probe("blind", t->frame, t->slot, t->cce, t->L, t->out.rnti, tmp_e, t->L * 108);
  }
}

static void nr_pdcch_blind_cand_worker_serial(nr_pdcch_blind_cand_task_t *t)
{
  nr_pdcch_blind_cand_worker_body(t);
}

static void nr_pdcch_blind_cand_worker(void *arg)
{
  nr_pdcch_blind_cand_task_t *t = (nr_pdcch_blind_cand_task_t *)arg;
  nr_pdcch_blind_cand_worker_body(t);
  completed_task_ans(t->ans);
}

static void nr_pdcch_blind_monitor_process_body(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc);
void nr_pdcch_blind_monitor_process(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc)
{
  const uint64_t t_rt = btim_enabled() ? btim_now() : 0;
  nr_pdcch_blind_monitor_process_body(ue, proc);
  btim_add(BTIM_RT, t_rt);
}
static void nr_pdcch_blind_monitor_process_body(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc)
{
  /* The blind PDCCH monitor is the PASSIVE RECEIVER, not part of the sensing pipeline: it decodes
   * other UEs' DCIs and (optionally) their PDSCH. It used to be gated on nr_isac_enabled() as well,
   * which forced the whole ISAC stack -- CSI-RS monitor FEP, CFR submission, CPI accumulation,
   * range-Doppler, AoA -- to be running before a single DCI could be recovered.
   *
   * That coupling is expensive on real hardware. MEASURED on the X410 over 120 s, same binary,
   * only the [sensing] section differing: RXDISCONT (RF timestamp discontinuities caused by the PHY
   * receive thread missing its deadline) 10847 with sensing on versus 100 with it off, and 672
   * SIB1 NACKs versus 0. CLAUDE.md section 12 already flagged that the passive decode runs on the
   * PHY receive thread and "would be the first thing to break on real hardware".
   *
   * So the two are now independent: configure pdcch_blind_monitor_* and the passive receiver runs
   * with sensing.enable = 0. The ISAC-sourced paths below stay gated on nr_isac_enabled(). */
  if (!nr_pdcch_blind_monitor_enabled()) {
    return;
  }
  {
    // Moved to after the enabled() check above (was before it) so this doesn't fire for every
    // deployment with the monitor disabled entirely -- enabled is always 1 here now, so that
    // field is dropped from the message.
    static int s_entry_diag = -1;
    if (s_entry_diag < 0)
      s_entry_diag = (getenv("ISAC_DISCOVER_DIAG") != NULL) ? 1 : 0;
    static int s_entry_calls = 0;
    s_entry_calls++;
    if (s_entry_diag && s_entry_calls == 1) {
      printf("DISCOVERDIAG ENTRY\n"); fflush(stdout);
    }
  }
  const nr_pdcch_blind_monitor_cfg_t *cfg = nr_pdcch_blind_monitor_get_cfg();
  {
    static int s_cfg_diag = -1;
    if (s_cfg_diag < 0)
      s_cfg_diag = (getenv("ISAC_DISCOVER_DIAG") != NULL) ? 1 : 0;
    static int s_cfg_calls = 0;
    s_cfg_calls++;
    if (s_cfg_diag && s_cfg_calls == 1) {
      printf("DISCOVERDIAG CFG autodiscover=%d pdsch_decode=%d\n", cfg->autodiscover, cfg->pdsch_decode); fflush(stdout);
    }
  }

  // What this occasion is for. The tap used to run only for the DM-RS source; the passive
  // data-aided path (pdsch_decode) is a second, independent reason to scan the same candidates, and
  // `pdsch_decode == 1` (measure the CRC pass rate, submit nothing) must work with NO sensing source
  // enabled at all -- that is the whole point of having a measure-only level.
  const bool want_decode = cfg->pdsch_decode >= 1; // >=1 always decodes; only >=2 submits

  /* ---- Deferred decode (PASSIVE_RX_ONLY_HANDOVER.md §15). Started lazily here rather than from
   * the monitor's config parse because the consumer needs a live PHY_VARS_NR_UE (frame_parms sized,
   * rxdata allocated), which does not exist yet when [sensing] is parsed. One-shot; if the start is
   * REFUSED -- e.g. --cont-fo-comp makes a deferred FEP unsound, see nr_pdsch_passive_queue_start()
   * -- `defer` stays false and every decode runs in-line exactly as before. */
  if (want_decode && cfg->pdsch_thread) {
    static int s_queue_tried = 0;
    if (!s_queue_tried) {
      s_queue_tried = 1;
      /* pdsch_thread is the CONSUMER COUNT (1 = one thread). One consumer sustains ~1290
       * decodes/s and measurably could not keep up with this cell -- 31 % of accepts were dropped
       * at the ring. Depth defaults to 8 per consumer so worst-case job latency stays put as
       * consumers are added, rather than growing into the rxdata lifetime. */
      const int n_cons = cfg->pdsch_thread;
      const int depth  = (cfg->pdsch_queue_depth > 0) ? cfg->pdsch_queue_depth : (8 * n_cons);
      if (nr_pdsch_passive_queue_start(ue, depth, n_cons, cfg->pdsch_thread_core)) {
        LOG_I(PHY,
              "SENSING: passive PDSCH decode DEFERRED to %d consumer thread(s) (depth=%d core=%d) -- "
              "775us mean decode no longer runs on the PHY receive thread\n",
              n_cons, depth, cfg->pdsch_thread_core);
      }
    }
  }
  /* DCI discovery is itself a monitor consumer. PDSCH decode and ISAC DM-RS extraction are
   * optional work after an accepted grant, so disabling both must not disable PDCCH discovery. */

  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  /* Consult the SIB1 cache as soon as the cell is known: with OAI's own SIB1 failing at 4 RX, this
   * is what seeds the DL TDRA list (and the UL seed below) on a cell decoded before. One call per
   * PCI; the loader itself is idempotent. */
  {
    static uint16_t s_cache_pci = 0xFFFF;
    if (s_cache_pci != fp->Nid_cell) {
      s_cache_pci = (uint16_t)fp->Nid_cell;
      nr_pdcch_blind_common_config_t c;
      (void)nr_pdcch_blind_get_common((uint16_t)fp->Nid_cell, &c);
    }
  }

  /* ---- PHASE 3 (2026-09-04): recover the DEDICATED CORESET by search, Technique A -------------
   * Runs BEFORE the dedicated-SS occasion gate below, which is keyed on ss_monitoring_slot_* --
   * exactly the fields Technique A exists to find, so they cannot gate reaching it. Gated on
   * nr_pdcch_blind_monitor_autodiscover_done(), NOT g_cfg.bwp_size: CSS0 autoconf (required to be
   * on for this feature's bootstrap RNTI, see the autodiscover conf knob's own comment) very likely
   * already set g_cfg.bwp_size for CORESET#0 by the time this runs, so that field can no longer
   * tell "dedicated geometry still unknown" from "the common one is already known" -- see the
   * definition-site comment on nr_pdcch_blind_monitor_autodiscover_step() in
   * nr_pdcch_blind_monitor.c for the full reasoning.
   *
   * Does its OWN minimal single-symbol, whole-carrier FEP -- NOT run_occasion()'s CORESET-scoped
   * one further down, which needs coreset geometry (duration, frequency_domain_resource) this
   * function does not have until Technique A succeeds. */
  /* STAGE 1 blind nID sweep: one FEP'd WINDOW of nr_pdcch_coreset_map_idsweep_dur() consecutive
   * symbols per DL slot (rotating), handed to an idle-priority worker. Diagnostic; off unless
   * ISAC_COREMAP_IDSWEEP=1. dur=1 (default) is exactly the original single-symbol capture; dur>1 is
   * the "second capture pass" for a 2-3 symbol CORESET (ISAC_COREMAP_IDSWEEP_DUR). */
  {
    const int idsw_sym = nr_pdcch_coreset_map_idsweep_want(
        (uint32_t)proc->frame_rx * fp->slots_per_frame + (uint32_t)proc->nr_slot_rx, fp->symbols_per_slot);
    static c16_t *s_idsw_buf;
    if (idsw_sym >= 0 && s_idsw_buf == NULL)
      s_idsw_buf = malloc16(sizeof(c16_t) * fp->samples_per_slot_wCP);
    if (idsw_sym >= 0 && s_idsw_buf != NULL) {
      c16_t(*rxF_id)[fp->samples_per_slot_wCP] = (c16_t(*)[fp->samples_per_slot_wCP])s_idsw_buf;
      const int idsw_dur = nr_pdcch_coreset_map_idsweep_dur();
      for (int d = 0; d < idsw_dur; d++)
        nr_slot_fep_ant(ue, fp, proc->nr_slot_rx, idsw_sym + d, 0 /* ant */, rxF_id, link_type_dl, 0,
                        ue->common_vars.rxdata);
      nr_pdcch_coreset_map_idsweep_push(s_idsw_buf + idsw_sym * fp->ofdm_symbol_size, fp->ofdm_symbol_size,
                                        fp->first_carrier_offset, fp->N_RB_DL, proc->nr_slot_rx, idsw_sym,
                                        (uint16_t)fp->Nid_cell, fp->symbols_per_slot);
    }
  }
  if (cfg->autodiscover && !nr_pdcch_blind_monitor_autodiscover_done()
      && !nr_pdcch_blind_monitor_discovery_paused()) { /* paused: every discovered CORESET is banked */
    const uint32_t abs_slot_now = (uint32_t)proc->frame_rx * fp->slots_per_frame + (uint32_t)proc->nr_slot_rx;
    // ponytail: fixed at symbol 0 rather than rotating through the slot. This deployment's
    // dedicated CORESETs are always 1 symbol starting at 0 (see nr_pdcch_blind_monitor.c's
    // coreset_duration=1 comment); upgrade to a rotating symbol index if a future cell's dedicated
    // CORESET does not start at symbol 0.
    const int disc_symbol = 0;
    // Technique A only ever reads antenna 0 (nr_pdcch_coreset_map_scan() takes one c16_t* symbol
    // buffer), so FEP only antenna 0 -- nr_slot_fep_ant() rather than nr_slot_fep(), which would
    // needlessly FEP every antenna. Sized/indexed EXACTLY as nr_slot_fep_ant()'s own declared type
    // requires: `c16_t rxdataF[][frame_parms->samples_per_slot_wCP]`, one row (ant=0), a full
    // slot's row length (it writes at `rxdataF[ant][ofdm_symbol_size * symbol]`, not at a
    // single-symbol-sized offset 0 -- a smaller row here is a stack buffer overflow the moment
    // symbol/row indexing sees the real row stride, exactly as the review found).
    __attribute__((aligned(32))) c16_t rxdataF_disc[1][fp->samples_per_slot_wCP];
    nr_slot_fep_ant(ue, fp, proc->nr_slot_rx, disc_symbol, 0 /* ant */, rxdataF_disc, link_type_dl, 0,
                    ue->common_vars.rxdata);
    /* XCHECK follow-up (2026-09-06): nr_slot_fep_ant() applies common_fo_hz + a PER-BRANCH FO term
     * (nr_ue_get_branch_fo_hz(ant), added 2026-09-03 for the branch-coherence fix) that plain
     * nr_slot_fep() -- the path the XCHECK diagnostic above just proved gets 0.88-0.999 correlation
     * on this exact cell/config -- never applies. If branch 0's measured FO is nonzero/wrong here,
     * this residual per-subcarrier phase rotation would decorrelate Technique A's magnitude-of-
     * complex-sum measurement while leaving phase-insensitive measurements (RF power) unaffected --
     * exactly this handover's still-open symptom. */
    if (getenv("ISAC_DISCOVER_DIAG") != NULL) {
      extern double nr_ue_get_branch_fo_hz(int ant);
      static int s_fo_calls = 0;
      s_fo_calls++;
      if ((s_fo_calls % 200) == 1) {
        printf("BRANCHFO calls=%d branch0_fo_hz=%.2f\n", s_fo_calls, nr_ue_get_branch_fo_hz(0));
        fflush(stdout);
      }
    }
    if (getenv("ISAC_DISCOVER_DIAG") != NULL) {
      static uint32_t s_en_n;
      if ((s_en_n++ % 2000) == 0) {
        double e = 0; int nz = 0;
        for (int k = 0; k < fp->ofdm_symbol_size; k++) {
          const c16_t v = rxdataF_disc[0][disc_symbol * fp->ofdm_symbol_size + k];
          e += (double)v.r * v.r + (double)v.i * v.i; nz += (v.r | v.i) != 0;
        }
        printf("DISCOVERDIAG fep slot=%d sym=%d energy=%.3g nonzero_sc=%d\n", proc->nr_slot_rx, disc_symbol, e, nz);
        fflush(stdout);
      }
    }
    /* Symbol 1 as well, for the duration decision (one more symbol FEP per DL slot, pre-discovery only). */
    nr_slot_fep_ant(ue, fp, proc->nr_slot_rx, 1, 0 /* ant */, rxdataF_disc, link_type_dl, 0, ue->common_vars.rxdata);
    nr_pdcch_blind_monitor_autodiscover_observe_symbol1(rxdataF_disc[0] + 1 * fp->ofdm_symbol_size, fp->ofdm_symbol_size,
                                                        fp->N_RB_DL, fp->first_carrier_offset, (uint16_t)fp->Nid_cell,
                                                        proc->nr_slot_rx);
    nr_pdcch_blind_monitor_autodiscover_step(rxdataF_disc[0] + disc_symbol * fp->ofdm_symbol_size, fp->ofdm_symbol_size, fp->N_RB_DL,
                                             fp->first_carrier_offset, (uint16_t)fp->Nid_cell,
                                             proc->nr_slot_rx, disc_symbol, abs_slot_now);
    /* Previously a second discovery epoch made the receiver deaf: the unconditional return also
     * stopped every already-verified CORESET. Keep those immutable bank entries running. */
    if (atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire) == 0)
      return;
  }

  const uint32_t gate_slot = (uint32_t)proc->frame_rx * fp->slots_per_frame + (uint32_t)proc->nr_slot_rx;
  // BUG FIXED 2026-09-04 (found while investigating 0 blind SI-RNTI accepts on CORESET#0): this
  // gate checked ONLY the single slot at ss_monitoring_slot_offset, ignoring ss_duration entirely
  // -- ss_duration is set correctly by autoconf (2 for this cell's SS0, confirmed against the MAC's
  // own get_type0_PDCCH_CSS_config_parameters() computation and logged as "dur=2"), but was never
  // read anywhere in this file. A search space with duration > 1 spans MULTIPLE CONSECUTIVE slots
  // per occasion (TS 38.213 ch.13: SS0 is "over two consecutive slots"), and the real candidate can
  // land in any of them -- confirmed live: this cell's genuine SI-RNTI grant is at gate_slot%40==1,
  // the SECOND slot of the 2-slot window starting at offset=0, which the old single-slot check could
  // never reach. Duration 1 (the overwhelmingly common case -- every other search space this module
  // handles) makes this identical to the old check, so nothing else changes behavior.
  const uint32_t ss_dur = (cfg->ss_duration > 0) ? (uint32_t)cfg->ss_duration : 1;
  if (cfg->ss_monitoring_slot_periodicity <= 0) {
    return;
  }
  const uint32_t rem = gate_slot % (uint32_t)cfg->ss_monitoring_slot_periodicity;
  bool on_occasion = !(rem < (uint32_t)cfg->ss_monitoring_slot_offset
                       || rem >= (uint32_t)cfg->ss_monitoring_slot_offset + ss_dur);
  /* ---- RA SEARCH SPACE: its own occasions, in addition to the configured one ------------------
   * SIB1 and RA are DIFFERENT common search spaces with independent
   * monitoringSlotPeriodicityAndOffset -- on this cell sib1_ss=0 and ra_ss=1. CSS0 autoconf
   * configures this gate from SS#0 only (measured: period=40 offset=11 dur=2), so unless SS#1
   * happens to fall inside those 2 slots in 40 we never look at a single RAR or Msg4 occasion.
   * MEASURED 2026-09-21: RA=0 and TC=1 over 58,001 occasions on a cell the operator confirms is
   * busy -- which is what being blind to the RA window looks like, not an idle cell. Both windows
   * scan DCI 1_0 in a common search space, so widening the gate is sufficient; nothing downstream
   * needs to change. Inert until SIB1 supplies a period. */
  if (!on_occasion) {
    const nr_pdcch_sib1_prior_t *rp = nr_pdcch_sib1_prior_get();
    if (rp != NULL && rp->ra_ss_valid && rp->ra_ss_period > 0) {
      const uint32_t rdur = (rp->ra_ss_duration > 0) ? rp->ra_ss_duration : 1u;
      const uint32_t rrem = gate_slot % (uint32_t)rp->ra_ss_period;
      if (rrem >= (uint32_t)rp->ra_ss_offset && rrem < (uint32_t)rp->ra_ss_offset + rdur) {
        on_occasion = true;
        static _Atomic uint64_t s_ra_occ = 0;
        const uint64_t n = atomic_fetch_add_explicit(&s_ra_occ, 1, memory_order_relaxed) + 1;
        if (n == 1 || (n % 20000) == 0)
          LOG_A(PHY, "SENSING: RA-SS occasion %llu (period=%u offset=%u dur=%u) -- slots SS#0 never covered\n",
                (unsigned long long)n, rp->ra_ss_period, rp->ra_ss_offset, (unsigned)rdur);
      }
    }
  }
  if (!on_occasion) {
    return; // not a monitoring occasion this slot
  }

  /* ---- Deferred SCAN (nr_pdcch_passive_queue.h). Everything below this point -- full-slot FEP over
   * the CORESET symbols, PDCCH channel estimation + equalisation, demapping, and the per-candidate
   * polar decodes -- used to run HERE, on the PHY receive thread.
   *
   * BTIM, mean per occasion, measured on this deployment: fep_llr 54-62us, demap 3.2-3.5us,
   * prepass 2.6-3.0us, candidate decode 5.4-19.2us, for a TOTAL of 69us at 165 grants/s and
   * 80-102us at ~1530 grants/s. Against a 500us slot that is 13.9 % RT duty at low load and
   * 16-20 % at high load, and section 15 measured this receiver holding PBCH lock below ~18 % and
   * losing it above. That straddle IS the mechanism behind the grant-rate-driven CRC bimodality.
   * fep_llr alone is 60-79 % of the occasion, so deferring the body is what buys the margin.
   *
   * Started lazily here for the same reason the PDSCH pool is: the consumer needs a live
   * PHY_VARS_NR_UE with frame_parms sized and rxdata allocated, which does not exist when [sensing]
   * is parsed. One-shot, and RT-side so the guard itself stays single-threaded. If the start is
   * REFUSED (--cont-fo-comp makes a deferred FEP unsound) the scan runs in-line exactly as before. */
  if (cfg->scan_thread) {
    static int s_scan_tried = 0;
    if (!s_scan_tried) {
      s_scan_tried = 1;
      const int n_cons = cfg->scan_thread;
      const int depth  = (cfg->scan_queue_depth > 0) ? cfg->scan_queue_depth : 8;
      nr_pdcch_passive_queue_start(ue, depth, n_cons, cfg->scan_thread_core);
    }
  }

  /* The producer's MONOTONIC slot counter, used ONLY as the job's staleness reference -- the
   * consumer compares it against the producer's current value to decide whether this occasion's raw
   * IQ still exists in rxdata. It deliberately does NOT become the occasion's slot index: see the
   * epoch note in nr_pdcch_blind_monitor_run_occasion(). */
  const long mono_slot = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);

  if (nr_pdcch_passive_queue_running()) {
    const nr_pdcch_passive_job_t job = {.frame_rx      = proc->frame_rx,
                                        .nr_slot_rx    = proc->nr_slot_rx,
                                        .gNB_id        = proc->gNB_id,
                                        .absolute_slot = mono_slot,
                                        /* Sampled HERE, on the receive thread, with these samples. */
                                        .fo_hz         = ue->cont_fo_comp ? (ue->dl_Doppler_shift + ue->freq_offset) : 0.0};
    nr_pdcch_passive_queue_enqueue(&job);
    return; // the consumer runs the occasion; the receive thread is done here
  }

  nr_pdcch_blind_monitor_run_occasion(ue, proc, false /* on the RT thread: fan out as before */, mono_slot);
}

/* The occasion body. Runs on a scan consumer when the pool is up, and on the PHY receive thread
 * otherwise -- identical code either way, which is what makes the deferral A/B-able with one config
 * field. `abs_slot_monotonic` is the producer's un-wrapped slot counter for this occasion. */
static void nr_pdcch_blind_monitor_run_occasion_one(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc,
                                                     bool serial_candidates, long source_absolute_slot);

/* CROSS-PASS DEDUPE (2026-09-23). One slot is decoded by several passes (verified banks, CORESET#0-USS,
 * discovery). A DCI whose REs lie in CORESET#0's RBs decodes in BOTH the bank pass and the CORESET#0-USS
 * pass under different CCE numbering -- run s3live4: 1962 DL DCIs accepted twice (CCE 4 and CCE 6), i.e.
 * duplicate grants and duplicate PDSCH decodes. One accept per (slot, RNTI, direction). */
enum { PASS_OTHER = 0, PASS_C0USS = 1, PASS_BANK = 2 };
static __thread int t_pass_kind;
static pthread_mutex_t s_dedupe_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t slot; uint16_t rnti; uint8_t dir; } s_dedupe[256];
static unsigned s_dedupe_w;
static _Atomic uint64_t g_c0uss_unique, g_bank_accepts, g_dup_dropped;
static bool accept_dup(uint32_t abs_slot, uint16_t rnti, int dir)
{
  bool dup = false;
  pthread_mutex_lock(&s_dedupe_mu);
  for (int i = 0; i < 256 && !dup; i++)
    dup = s_dedupe[i].slot == abs_slot + 1 && s_dedupe[i].rnti == rnti && s_dedupe[i].dir == dir;
  if (!dup) {
    s_dedupe[s_dedupe_w & 255].slot = abs_slot + 1; /* +1: slot 0 never looks like an empty entry */
    s_dedupe[s_dedupe_w & 255].rnti = rnti;
    s_dedupe[s_dedupe_w & 255].dir = (uint8_t)dir;
    s_dedupe_w++;
  }
  pthread_mutex_unlock(&s_dedupe_mu);
  if (dup)
    atomic_fetch_add_explicit(&g_dup_dropped, 1, memory_order_relaxed);
  else if (t_pass_kind == PASS_C0USS)
    atomic_fetch_add_explicit(&g_c0uss_unique, 1, memory_order_relaxed);
  else if (t_pass_kind == PASS_BANK)
    atomic_fetch_add_explicit(&g_bank_accepts, 1, memory_order_relaxed);
  return dup;
}

void nr_pdcch_blind_monitor_run_occasion(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc,
                                         bool serial_candidates, long source_absolute_slot)
{
  const nr_pdcch_blind_monitor_cfg_t *root = nr_pdcch_blind_monitor_get_cfg();
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  if (!root->autodiscover) {
    nr_pdcch_blind_monitor_run_occasion_one(ue, proc, serial_candidates, source_absolute_slot);
    return;
  }

  /* A USS may legally reference CORESET#0. It is the cheapest exact geometry available OTA, so
   * search it before spending the occasion on unknown footprints. */
  nr_pdcch_blind_monitor_cfg_t c0_uss;
  uint16_t verified[NR_PDCCH_BLIND_MAX_UE];
  const uint32_t frame_slot = proc != NULL
      ? (uint32_t)proc->frame_rx * (uint32_t)ue->frame_parms.slots_per_frame + (uint32_t)proc->nr_slot_rx
      : 0;
  const int n_verified = nr_pdcch_blind_monitor_verified_rnti_set(
      frame_slot, verified, NR_PDCCH_BLIND_MAX_UE);
  /* ORDER + GATE (2026-09-23). Verified banks run FIRST, so the cross-pass dedupe keeps their copy and a
   * CORESET#0-USS accept only counts when it is UNIQUE. A USS may legally live on CORESET#0, so the pass is
   * never removed: it is skipped once a full window of occasions gives it no unique accept while the banks
   * are decoding, re-probed on 1 occasion in 64, and re-opened by its first unique accept. */
  if (n == 0) {
    t_pass_kind = PASS_C0USS;
    if (nr_pdcch_blind_monitor_coreset0_uss_cfg(&c0_uss)) {
      nr_pdcch_blind_monitor_cfg_override(&c0_uss);
      nr_pdcch_blind_monitor_run_occasion_one(ue, proc, serial_candidates, source_absolute_slot);
      nr_pdcch_blind_monitor_cfg_override(NULL);
    }
    t_pass_kind = PASS_OTHER;
    nr_pdcch_blind_monitor_run_occasion_one(ue, proc, serial_candidates, source_absolute_slot);
    return;
  }
  t_pass_kind = PASS_BANK;
  for (int i = 0; i < n; ++i) {
    nr_pdcch_blind_monitor_cfg_override(&g_coreset_bank[i].cfg);
    nr_pdcch_blind_monitor_run_occasion_one(ue, proc, serial_candidates, source_absolute_slot);
  }
  nr_pdcch_blind_monitor_cfg_override(NULL);
  {
    static uint64_t s_occ, s_u0, s_b0;
    static bool s_closed;
    const uint64_t u = atomic_load_explicit(&g_c0uss_unique, memory_order_relaxed);
    const uint64_t b = atomic_load_explicit(&g_bank_accepts, memory_order_relaxed);
    if (s_closed && u > s_u0) {
      s_closed = false;
      LOG_A(PHY, "SENSING: CORESET0_USS pass RE-OPENED: a probe found a DCI no other pass had\n");
    }
    if (++s_occ >= 20000) { /* ponytail: window/probe rates are compute budgets, not cell parameters */
      if (!s_closed && u == s_u0 && b > s_b0) {
        s_closed = true;
        LOG_A(PHY, "SENSING: CORESET0_USS pass GATED: 0 unique accepts in %lu occasions while the banks accepted %lu "
                   "-- probing 1 occasion in 64\n", (unsigned long)s_occ, (unsigned long)(b - s_b0));
      }
      s_occ = 0;
      s_u0 = u;
      s_b0 = b;
    }
    t_pass_kind = PASS_C0USS;
    if ((!s_closed || (s_occ & 63) == 0) && nr_pdcch_blind_monitor_coreset0_uss_cfg(&c0_uss)) {
      nr_pdcch_blind_monitor_cfg_override(&c0_uss);
      nr_pdcch_blind_monitor_run_occasion_one(ue, proc, serial_candidates, source_absolute_slot);
    }
    t_pass_kind = PASS_OTHER;
  }
  nr_pdcch_blind_monitor_cfg_override(NULL);

  if (nr_pdcch_blind_monitor_autodiscover_done()) {
    nr_pdcch_blind_monitor_run_occasion_one(ue, proc, serial_candidates, source_absolute_slot);
  } else {
    /* CSS0 remains alive while the discovery-only path has no candidate geometry to scan. */
    static uint64_t css_tick;
    const nr_pdcch_blind_monitor_cfg_t *css0 = nr_pdcch_blind_monitor_css0_cfg();
    if (css0 != NULL && ((css_tick++ & 7u) == 0)) {
      nr_pdcch_blind_monitor_cfg_override(css0);
      nr_pdcch_blind_monitor_run_occasion_one(ue, proc, serial_candidates, source_absolute_slot);
      nr_pdcch_blind_monitor_cfg_override(NULL);
    }
  }
}

static void nr_pdcch_blind_monitor_run_occasion_one(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc,
                                                     bool serial_candidates, long source_absolute_slot)
{
  discovery_evidence_frame = proc ? proc->frame_rx : -1;
  discovery_evidence_slot = proc ? proc->nr_slot_rx : -1;
  /* ISAC_TDD_SKIP=1: do not scan slots SIB1 says carry no downlink. Unknown pattern -> scan. */
  {
    static int s_tdd_skip = -1;
    if (s_tdd_skip < 0)
      s_tdd_skip = (getenv("ISAC_TDD_SKIP") != NULL) ? 1 : 0;
    static unsigned long s_skipped = 0, s_seen = 0;
    if (s_tdd_skip && proc != NULL) {
      s_seen++;
      /* Frame-aligned slot: the TDD pattern's phase is relative to the frame, and the producer's
       * ring counter is not (and can be -1 on this path). */
      const uint32_t frame_slot = (uint32_t)proc->frame_rx * (uint32_t)ue->frame_parms.slots_per_frame + (uint32_t)proc->nr_slot_rx;
      if (!nr_passive_acq_tdd_slot_has_downlink(frame_slot)) {
        if ((++s_skipped % 20000) == 1)
          LOG_A(PHY, "SENSING: TDD skip: %lu of %lu slots skipped as uplink-only (from SIB1)\n", s_skipped, s_seen);
        return;
      }
    }
  }
  const nr_pdcch_blind_monitor_cfg_t *cfg = nr_pdcch_blind_monitor_get_cfg();
  if (cfg->autodiscover && nr_pdcch_blind_monitor_autodiscover_done()
      && !nr_pdcch_blind_monitor_autodiscover_extent_verified()
      && coreset_bank_covers(cfg->coreset_rb_offset, cfg->coreset_freq_domain * 6,
                            cfg->coreset_duration, cfg->ss_first_symbol,
                            cfg->coreset_reg_bundle_size, cfg->coreset_interleaver_size,
                            cfg->coreset_shift_index, cfg->coreset_pdcch_dmrs_scrambling_id)) {
    nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start + cfg->coreset_rb_offset);
    dl_discovery_invalidate();
    return;
  }
  if (proc != NULL && proc->nr_slot_rx >= 0 && proc->nr_slot_rx < 20)
    g_occ_slot[proc->nr_slot_rx]++; // ACCSLOT census: occasions actually run per slot

  /* ---- CSS0 INTERLEAVE -------------------------------------------------------------------------
   * The snapshot taken in nr_pdcch_blind_monitor.c (g_css0_cfg) was built for this and NOTHING EVER
   * CONSUMED IT: nr_pdcch_blind_monitor_css0_cfg() was declared, defined, and called from nowhere.
   * So the two modes were mutually exclusive in practice, which is why (measured 2026-09-20,
   * Swisscom PCI 382):
   *   autodiscover=0 -> CSS0 gets every occasion: SI=10000, TC=16, seeds flow, no dedicated search
   *   autodiscover=1 -> CSS0 never runs at all:   SI=0,     TC=0,  no seeds, dedicated cannot verify
   * and the dedicated search needs BOTH at once -- CSS0 to harvest a verified RNTI, the dedicated
   * scan to use it. The single SIB1 seen in an autodiscover=1 run came from the autoconf phase
   * before the dedicated search took over, not from an interleave.
   *
   * Spending 1 occasion in ISAC_CSS0_EVERY on CSS0 is cheap: CSS0 yielded 10000 SI accepts when it
   * had 100 % of occasions, so a fraction still harvests plentifully, and the dedicated sweep keeps
   * the rest.
   *
   * SAFE AGAINST THE DOCUMENTED HAZARD: the snapshot carries autodiscover=0, and every dedicated
   * bookkeeping site (extent_step, the length sweep, the lookahead lanes) is gated on
   * cfg->autodiscover and only then writes g_cfg. Under the snapshot none of them run, so an
   * interleaved occasion cannot silently revert a hypothesis advance -- which is precisely what the
   * snapshot comment warned about. cfg is read-only for the occasion; nothing is restored because
   * nothing is mutated. */
  /* CORESET#0 can carry either a common or a UE-specific SearchSpace. Downstream behavior follows
   * the SearchSpace, while CoreSetType continues to control the physical DM-RS reference. */
  bool css0_occasion = (cfg->dci10_ss_type == NR_BLIND_SS_COMMON);
  {
    static int s_css0_every = -1;
    if (s_css0_every < 0) {
      const char *e = getenv("ISAC_CSS0_EVERY");
      s_css0_every = (e != NULL && atoi(e) >= 0) ? atoi(e) : 8;
    }
    if (s_css0_every > 0 && cfg->autodiscover) {
      const nr_pdcch_blind_monitor_cfg_t *css0 = nr_pdcch_blind_monitor_css0_cfg();
      if (css0 != NULL) {
        static _Atomic uint64_t s_occ = 0;
        const uint64_t n = atomic_fetch_add_explicit(&s_occ, 1, memory_order_relaxed);
        if ((n % (uint64_t)s_css0_every) == 0) {
          cfg = css0;
          css0_occasion = true;
          static _Atomic uint64_t s_css0_runs = 0;
          const uint64_t r = atomic_fetch_add_explicit(&s_css0_runs, 1, memory_order_relaxed) + 1;
          if (r == 1 || (r % 20000) == 0)
            LOG_A(PHY, "SENSING: CSS0 interleave: %llu occasions on CORESET#0 (1 in %d)\n",
                  (unsigned long long)r, s_css0_every);
        }
      }
    }
  }

  nr_pdcch_ss_registry_occasion(cfg);
  nr_pdsch_passive_queue_flush(); /* previous slot's grants go to the consumers together */
  if (nr_agnostic_v2()) {
    const int ssi = nr_pdcch_ss_registry_index(cfg);
    static _Atomic uint64_t s_probe = 0;
    if (ssi >= 0 && g_ss_reg.retired[ssi]
        && (atomic_fetch_add_explicit(&s_probe, 1, memory_order_relaxed) & 63) != 0)
      return; /* barren configuration: spend 1 occasion in 64 re-checking it, not every one */
  }
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  nr_pdcch_blind_ul_opts_t ul_opts = cfg->ul;
  ul_opts.phy_cell_id = fp->Nid_cell;
  ul_opts.numerology = fp->numerology_index;
  ul_opts.dmrs_typeA_position = cfg->dmrs_typeA_position;
  nr_pdcch_blind_common_config_t common;
  if (nr_pdcch_blind_get_common(fp->Nid_cell, &common) && common.ul_bwp_size) {
    ul_opts.numerology = common.ul_mu;
    /* JOINT UL INITIALISATION. The UL width/interpretation search cannot score anything until it
     * has a UL BWP (the RIV reference, and the frequency-domain field's width) and a TDA list --
     * and nothing on the air discloses the DEDICATED PUSCH-Config, which travels ciphered. Without
     * a seed the controller refuses every grant, which is exactly why autonomous UL sat at zero
     * attempts.
     *
     * SIB1's common initial UL BWP and pusch-TimeDomainAllocationList ARE decoded OTA facts. They
     * are seeded here as the search's STARTING HYPOTHESIS -- never asserted as dedicated
     * configuration. The transport-block CRC stays the authority: if the dedicated list differs,
     * the search simply fails to converge, which is the same outcome as not seeding at all. So the
     * seed can only add reach, never manufacture a false convergence.
     *
     * Only in full_auto, and only where the operator configured nothing. */
    if (cfg->dl_full_auto && ul_opts.bwp_size == 0) {
      ul_opts.bwp_start = common.ul_bwp_start;
      ul_opts.bwp_size  = common.ul_bwp_size;
      /* tda_count == 0 is NOT "unknown": nr_pdcch_blind_dci01_size() reads it as the TS 38.214
       * Table 6.1.2.1.1-2 default 16-entry table, i.e. a 4-bit field and a complete interpretation.
       * Only override it when SIB1 actually carried a list. */
      if (ul_opts.tda_count == 0 && common.ul_count > 0) {
        ul_opts.tda_count = common.ul_count;
        for (int i = 0; i < common.ul_count; i++) {
          ul_opts.tda_start[i]   = common.ul_start[i];
          ul_opts.tda_length[i]  = common.ul_length[i];
          ul_opts.tda_mapping[i] = common.ul_mapping[i];
          ul_opts.tda_k2[i]      = common.ul_k2[i];
        }
      }
      static bool ul_seed_logged;
      if (!ul_seed_logged) {
        ul_seed_logged = true;
        /* Print the ENTRIES, not just the count. The TDA list sets the TDA field width and hence the
         * whole DCI layout, so reproducing this seed in a manual configuration requires the actual
         * start/length/k2/mapping values -- and there is no other way to read them off the air. The
         * format matches pdcch_blind_monitor_ul_tda so the line can be pasted directly. */
        char tda[256];
        int n = 0;
        for (int i = 0; i < ul_opts.tda_count && n < (int)sizeof(tda) - 20; i++)
          n += snprintf(tda + n, sizeof(tda) - n, "%s%u:%u:%u:%u", i ? "," : "",
                        ul_opts.tda_start[i], ul_opts.tda_length[i],
                        ul_opts.tda_k2[i], ul_opts.tda_mapping[i]);
        LOG_A(PHY,
              "UL discovery seeded from SIB1: UL-BWP=%u+%u TDAs=%d ul_bwp=\"%u:%u\" ul_tda=\"%s\" "
              "-- HYPOTHESIS for the dedicated config, not a claim about it; TB CRC decides\n",
              ul_opts.bwp_start, ul_opts.bwp_size, ul_opts.tda_count,
              ul_opts.bwp_start, ul_opts.bwp_size, tda);
      }
    }
  }
  if (cfg->autodiscover)
    nr_pdcch_blind_monitor_discovered_poll(); /* stage 1-2 hand-off; bumps the generation when applied */
  static uint64_t previous_geometry;
  const uint64_t geometry = nr_pdcch_blind_monitor_autodiscover_generation();
  if (cfg->autodiscover && geometry != previous_geometry) {
    dl_discovery_invalidate();
    /* A lookahead lane commits its already-scored length together with the verified geometry.
     * Preserve that result across the generation handoff instead of immediately sweeping it again. */
    if (nr_pdcch_blind_monitor_autodiscover_extent_verified() && cfg->dci_length_override > 0)
      g_length_swept = g_length_found = true;
    previous_geometry = geometry;
  }
  /* Frame-derived, exactly as before the split -- NOT the producer's monotonic counter, even though
   * one is available here now. This value indexes the ISAC slow-time grid via
   * nr_isac_submit_cfr_multi(), and csi_rx.c's CSI-RS submissions are on the SAME grid using this
   * same frame-derived epoch. Only the deferred pdsch_data path uses the monotonic counter, and it
   * does so through nr_isac_abs_slot_override. Switching this one source to a different epoch would
   * silently scatter DM-RS rows away from the CSI-RS rows they are meant to fuse with.
   * The deferred consumer reconstructs proc from the job's own frame/slot, so this is identical
   * whether the occasion runs here or on the receive thread -- which is what keeps the deferral a
   * pure threading change and therefore A/B-able. */
  const uint32_t abs_slot = (uint32_t)proc->frame_rx * fp->slots_per_frame + (uint32_t)proc->nr_slot_rx;

  const bool isac_on     = nr_isac_enabled() != 0;
  const bool want_dmrs   = isac_on && nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DMRS_BLIND);
  const bool want_data   = isac_on && cfg->pdsch_decode >= 2 && nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DATA);
  const bool want_decode = cfg->pdsch_decode >= 1;
  const bool defer       = nr_pdsch_passive_queue_running();

  g_occasions_run++;
  const int      btim_on   = btim_enabled();
  const uint64_t btim_occ0 = btim_on ? btim_now() : 0;
  const bool budget_active=cfg->autodiscover && cfg->coreset_type!=1
      && !nr_pdcch_blind_monitor_autodiscover_extent_verified() && discovery_budget_us()>0;
  discovery_latency_scope_t discovery_scope __attribute__((cleanup(discovery_latency_done))) = {
      .start=(btim_on && cfg->autodiscover && cfg->coreset_type!=1
          && !nr_pdcch_blind_monitor_autodiscover_extent_verified()) ? btim_occ0 : 0,
      .cpu_start=discovery_trace_enabled()?discovery_cpu_now():0,
      .width=0, .budget=budget_active, .phase=0};
  const uint64_t sweep_deadline=budget_active ? (btim_on?btim_occ0:btim_now())+1000ull*discovery_budget_us() : 0;
  /* Occasions are consumed by several scan workers. A TLS counter made every worker restart the
   * rotation at lane zero, starving the high-numbered lanes indefinitely. */
  static uint64_t budget_visit;
  const uint64_t visit = __atomic_fetch_add(&budget_visit, 1, __ATOMIC_RELAXED);
  const int budget_owner = budget_active ? (int)(visit % (1 + nr_pdcch_blind_lookahead_count())) : 0;


  // ---- Build the local, single-search-space PDCCH config. ----
  nr_phy_data_t local_phy_data;
  memset(&local_phy_data, 0, sizeof(local_phy_data));
  local_phy_data.phy_pdcch_config.nb_search_space = 1;
  fapi_nr_dl_config_dci_dl_pdu_rel15_t *rel15 = &local_phy_data.phy_pdcch_config.pdcch_config[0];

  rel15->BWPStart = (uint16_t)cfg->bwp_start;
  rel15->BWPSize  = (uint16_t)cfg->bwp_size;
  rel15->coreset.CoreSetType =
      (cfg->coreset_type == 1) ? NFAPI_NR_CSET_CONFIG_MIB_SIB1 : NFAPI_NR_CSET_CONFIG_PDCCH_CONFIG;
  rel15->coreset.rb_offset   = (uint16_t)cfg->coreset_rb_offset;  // own frame; see the field comment
  rel15->coreset.duration    = (uint8_t)cfg->coreset_duration;
  build_coreset_bitmap(cfg->coreset_freq_domain, rel15->coreset.frequency_domain_resource);
  /* Kept CONSISTENT with nr_pdcch_demapping_deinterleaving() in dci_nr.c, which is what actually
   * demaps here and does NOT read this field: it switches on reg_bundle_size alone --
   *     interleaved   := (reg_bundle_size_L_in != 0)
   *     bundle size L := (reg_bundle_size_L_in != 0) ? reg_bundle_size_L_in : 6
   * i.e. reg_bundle_size = 0 MEANS non-interleaved and already implies L = 6. Setting this config
   * field to the CORESET's real bundle size (6) to "be accurate" therefore selects INTERLEAVED and
   * then divides by interleaver_size = 0 -- an FPE, observed 2026-08-03. Derived from the same
   * input as the demapper so the FAPI field and the demapping can never disagree.
   * Verified for this cell from the gNB's own debug log: the dedicated CORESET carrying every
   * DCI 1_1 is "NON INTERLEAVED reg_bundle_sz=6", so reg_bundle_size = 0 here is correct. */
  rel15->coreset.CceRegMappingType = (cfg->coreset_reg_bundle_size != 0)
                                         ? FAPI_NR_CCE_REG_MAPPING_TYPE_INTERLEAVED
                                         : FAPI_NR_CCE_REG_MAPPING_TYPE_NON_INTERLEAVED;
  rel15->coreset.RegBundleSize     = (uint8_t)cfg->coreset_reg_bundle_size;
  rel15->coreset.InterleaverSize   = (uint8_t)cfg->coreset_interleaver_size;
  rel15->coreset.ShiftIndex        = (uint16_t)cfg->coreset_shift_index;
  rel15->coreset.pdcch_dmrs_scrambling_id = cfg->coreset_pdcch_dmrs_scrambling_id;
  /* PDCCH data scrambling is c_init = (n_RNTI*2^16 + n_ID), where n_RNTI is the C-RNTI only when
   * the search space is UE-specific AND the CORESET carries pdcch-DMRS-ScramblingID; otherwise 0.
   * VERIFIED for this cell from the gNB debug log, on the very lines carrying the DCI 1_1 grants:
   * "nid_pdcch_data=2 nid_pdcch_dmrs=2 nrnti_pdcch_data=0" -- so n_RNTI = 0 and n_ID = PCI = 2,
   * which is what this module already assumed. (Had it been non-zero, blind decoding of the USS
   * would need the C-RNTI *before* it can descramble -- the very thing the scan is recovering --
   * i.e. a structural blocker rather than a tuning error. It is not the case here.) */
  rel15->coreset.scrambling_rnti   = (cfg->dci10_ss_type == NR_BLIND_SS_UE_SPECIFIC && cfg->coreset_type != 1
                                              ? nr_pdcch_nrnti_override(abs_slot) : 0); /* USS on a PDCCH-Config CORESET only */
  if (cfg->ss_first_symbol < 0 || cfg->ss_first_symbol >= fp->symbols_per_slot) {
    return;
  }
  rel15->coreset.StartSymbolBitmap = (uint16_t)(1u << (fp->symbols_per_slot - 1 - cfg->ss_first_symbol));

  int n_rb = 0, cset_start = 0;
  get_coreset_rballoc(rel15->coreset.frequency_domain_resource, &n_rb, &cset_start);
  /* Six RBs contain a legal AL1 candidate (and AL2/AL3 worth of CCEs at durations 2/3).
   * The old 12-RB floor returned before the retry/extent clock, freezing a 6-RB observation. */
  if (n_rb < 6 || n_rb % 6 || rel15->coreset.duration < 1 || rel15->coreset.duration > 3
      || cfg->ss_first_symbol + rel15->coreset.duration > fp->symbols_per_slot) {
    if (cfg->autodiscover) {
      nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start + cfg->coreset_rb_offset);
      dl_discovery_invalidate();
    }
    return;
  }
  discovery_scope.width=n_rb;
  const int num_cces = (n_rb * rel15->coreset.duration) / 6;

  /* Build the blind candidate set across AGGREGATION LEVELS.
   *
   * A blind receiver does not know the target UE's C-RNTI, so it cannot evaluate the search-space
   * hash and must scan every non-overlapping CCE position at each aggregation level it wants to
   * catch.
   *
   * WHICH LEVELS ACTUALLY CARRY TRAFFIC -- MEASURED 2026-08-20 against the gNB's own debug log.
   * This CORRECTS the reading that drove the previous version of this ladder:
   *
   *   srsRAN's FAPI line prints `dci_aggregation_level` as LOG2(L), NOT L. Verified by pairing
   *   every scheduler record with its FAPI record over a 449 MB debug log: 3071/3071 satisfy
   *   sched `al=` == 2^`dci_aggregation_level`, zero exceptions.
   *     [SCHED] - DL PDCCH: ... cce=36 al=4                       <- the real aggregation level
   *     [FAPI ] - PDCCH ... cce_index=36 dci_aggregation_level=2  <- log2 of it
   *
   *   So this cell's dedicated grants are:  AL2 = 3060 (99.6 %),  AL4 = 11 (0.4 %),  AL1 = 0.
   *   The previous comment read `dci_aggregation_level=1 : 774382 (99.997 %)` as "AL1" and made
   *   AL1 the exhaustively-scanned level. AL1 carries NO grants here, and because it has the most
   *   non-overlapping positions (one per CCE) it consumed 45 of the 64 candidate slots: the AL2
   *   sweep was truncated at CCE 36 -- missing the real grants at CCE 38/40, 12.6 % of traffic --
   *   and the AL4/AL8 loops could not run at all on the dedicated CORESET.
   *
   * BUDGET. Two independent limits bind, and the second was previously unchecked:
   *   - candidate count : rel15->CCE[64] / L[64]
   *   - LLR volume      : the demapper writes 54*L equalised REs per candidate into
   *                       pdcch_e_rx[NR_MAX_PDCCH_SIZE]. A full sweep of a 45-CCE CORESET at every
   *                       level needs 45+23+11+5 = 84 candidates and 9450 REs -- over BOTH caps --
   *                       so whichever levels are allocated first necessarily starve the rest.
   * Allocation order is therefore AL2, AL4, AL8, AL1: descending measured usage, AL1 last because
   * it is simultaneously the most numerous and (here) the least used. Per-deployment override via
   * `pdcch_blind_monitor_ss`'s al-candidate fields, using the house `0 = auto` convention:
   *   >0 = cap that level at N candidates,  0 = auto (full non-overlapping sweep),  <0 = disable.
   */
  const int max_cand = (int)(sizeof(rel15->CCE) / sizeof(rel15->CCE[0]));
  const int max_re   = NR_MAX_PDCCH_SIZE; // pdcch_e_rx holds this many c16_t REs
  static const int al_order[4] = {2, 4, 8, 1};
  int nc = 0;
  int used_re = 0;

  /* Per-level share of the budget, from observed accepts (see the ADAPTIVE block above). An
   * explicit cap in the config still wins -- >0 pins a level, <0 disables it -- so a deployment that
   * KNOWS its scheduler can say so; 0 (the house "auto") is what learns. */
  int al_cap[4];
  {
    double w[4] = {0, 0, 0, 0};
    double wsum = 0;
    int    n_auto = 0;
    for (int idx = 0; idx < 4; idx++) {
      if (cfg->ss_al_candidates[idx] != 0) {
        continue; // pinned or disabled: not part of the adaptive split
      }
      w[idx] = (double)atomic_load_explicit(&g_al_accepts[idx], memory_order_relaxed) + AL_PRIOR;
      wsum += w[idx];
      n_auto++;
    }
    /* Reserve the floor first, then share what is left by weight. With no accepts yet every weight
     * is the prior, so this starts as an EVEN split and converges on the cell's real distribution
     * -- it does not start from an assumption about which level matters. */
    const int reserved = n_auto * AL_MIN_PROBE;
    const int spare    = (max_cand > reserved) ? (max_cand - reserved) : 0;
    for (int idx = 0; idx < 4; idx++) {
      const int cfg_cap = cfg->ss_al_candidates[idx];
      if (cfg_cap != 0) {
        al_cap[idx] = cfg_cap; // <0 disabled, >0 pinned -- handled in the sweep below
        continue;
      }
      al_cap[idx] = AL_MIN_PROBE + (int)((wsum > 0) ? ((double)spare * w[idx] / wsum) : 0);
    }
  }

  for (int oi = 0; oi < 4; oi++) {
    const int L   = al_order[oi];
    const int idx = (L == 1) ? 0 : (L == 2) ? 1 : (L == 4) ? 2 : 3; // ss_al_candidates[] is AL 1,2,4,8
    int cap = al_cap[idx];
    if (cap < 0) {
      continue; // explicitly disabled for this deployment
    }
    const int need = NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * L * 6; // 54*L REs per candidate
    const int n_pos = (num_cces >= L) ? ((num_cces - L) / L + 1) : 0; // non-overlapping positions
    if (n_pos <= 0) {
      continue;
    }
    /* Start where the previous occasion left off, so a level held near its floor still sweeps every
     * CCE position over successive occasions rather than re-probing one spot forever. */
    const uint32_t start = g_al_rotate[idx] % (uint32_t)n_pos;
    int added = 0;
    for (int k = 0; k < n_pos && nc < max_cand && added < cap && used_re + need <= max_re; k++) {
      const int cce = (int)(((start + (uint32_t)k) % (uint32_t)n_pos) * (uint32_t)L);
      rel15->CCE[nc] = (uint16_t)cce;
      rel15->L[nc]   = (uint8_t)L;
      nc++;
      added++;
      used_re += need;
    }
    g_al_rotate[idx] = start + (uint32_t)added;
    atomic_fetch_add_explicit(&g_al_cand[nr_pdcch_ss_bucket(cfg)][idx], (uint64_t)added, memory_order_relaxed);
  }

  /* One-shot visibility. A ladder that silently fails to cover the level the deployment actually
   * uses produces a 100 % false-accept stream rather than an error -- which is precisely the
   * failure mode this ladder just had, undetected across several sessions. Print what was built. */
  {
    /* PERIODIC, not one-shot. With the allocation now ADAPTIVE, a one-shot line reports the initial
     * even split for the rest of the run and hides the very thing worth watching -- whether the
     * split actually converged on where the accepts are. Printed on the same cadence as the monitor
     * summary so the two can be read together. */
    static unsigned long s_ladder_n = 0;
    if ((s_ladder_n++ % NR_PDCCH_BLIND_SUMMARY_PERIOD_OCC) == 0) {
      int n_per_al[4] = {0, 0, 0, 0};
      for (int c = 0; c < nc; c++) {
        n_per_al[(rel15->L[c] == 1) ? 0 : (rel15->L[c] == 2) ? 1 : (rel15->L[c] == 4) ? 2 : 3]++;
      }
      LOG_I(PHY,
            "SENSING: blind PDCCH ladder: num_cces=%d ncand=%d/%d re=%d/%d (AL1=%d AL2=%d AL4=%d AL8=%d) "
            "accepts_per_al=[%lu %lu %lu %lu]\n",
            num_cces, nc, max_cand, used_re, max_re, n_per_al[0], n_per_al[1], n_per_al[2], n_per_al[3],
            (unsigned long)atomic_load_explicit(&g_al_accepts[0], memory_order_relaxed),
            (unsigned long)atomic_load_explicit(&g_al_accepts[1], memory_order_relaxed),
            (unsigned long)atomic_load_explicit(&g_al_accepts[2], memory_order_relaxed),
            (unsigned long)atomic_load_explicit(&g_al_accepts[3], memory_order_relaxed));
    }
  }

  if (nc < 1) {
    /* A geometry with no supported AL must advance even though no polar decode can run. */
    if (cfg->autodiscover) {
      nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start + cfg->coreset_rb_offset);
      dl_discovery_invalidate();
    }
    return;
  }
  rel15->number_of_candidates = (uint8_t)nc;
  const uint16_t dci_length =
      cfg->dci_length_override > 0 ? (uint16_t)cfg->dci_length_override : nr_pdcch_blind_dci_size((uint16_t)cfg->bwp_size);
  if (dci_length == 0) {
    return;
  }
  rel15->num_dci_options       = 1;
  rel15->dci_length_options[0] = dci_length;
  /* Passive BWP tracking: snapshot the resolved entries for this occasion and pick the probe. */
  const bool pbwp_on = nr_pbwp_enabled() != 0;
  struct { uint16_t len, size; int16_t start; } pbwp_snap[NR_PBWP_MAX];
  memset(pbwp_snap, 0, sizeof(pbwp_snap));
  int pbwp_n = 0, pbwp_probe_entry = 0;
  uint16_t pbwp_probe_len = 0;
  bool cs_have = false;
  int cs_start = 0, cs_n = 0, cs_dur = 1, cs_ref = 0;
  if (pbwp_on) {
    pthread_mutex_lock(&g_pbwp_lock);
    const uint8_t base_ind = cfg->extract.bwp_indicator_bits >= 0 ? (uint8_t)cfg->extract.bwp_indicator_bits : 1;
    if (g_pbwp.base_len != dci_length || g_pbwp.base_size != (uint16_t)cfg->bwp_size) {
      nr_pbwp_init(&g_pbwp, (uint16_t)ue->frame_parms.N_RB_DL, (uint16_t)cfg->bwp_start, (uint16_t)cfg->bwp_size,
                   dci_length, base_ind);
      LOG_A(PHY, "SENSING: BWP tracking armed: base len=%u size=%d start=%d ind_bits=%u, %d candidate lengths\n",
            dci_length, cfg->bwp_size, cfg->bwp_start, base_ind, g_pbwp.n_cand);
    }
    pbwp_n = g_pbwp.n;
    for (int bi = 0; bi < g_pbwp.n; bi++) {
      pbwp_snap[bi].len = g_pbwp.e[bi].dci_len;
      pbwp_snap[bi].size = g_pbwp.e[bi].size;
      pbwp_snap[bi].start = g_pbwp.e[bi].start;
      if (bi > 0 && g_pbwp.e[bi].start >= 0) {
        g_pbwp_opts[bi] = cfg->extract;
        g_pbwp_opts[bi].bwp_indicator_bits = g_pbwp.e[bi].ind_bits;
      } else if (bi > 0 && !pbwp_probe_len) {
        pbwp_probe_len = g_pbwp.e[bi].dci_len; /* unresolved: collect DM-RS-scored grants */
        pbwp_probe_entry = bi;
      }
    }
    cs_have = nr_pbwp_coreset_hypothesis(&g_pbwp, &cs_start, &cs_n, &cs_dur, &cs_ref);
    static uint32_t s_probe_tick;
    if (!pbwp_probe_len && (++s_probe_tick & 3) == 0) /* discovery: 1 occasion in 4 */
      pbwp_probe_len = nr_pbwp_next_probe_len(&g_pbwp);
    pthread_mutex_unlock(&g_pbwp_lock);
  }

  /* ---- DCI format 1_0 context (TS 38.212 7.3.1.0 / TS 38.214 5.1.2.2.2). Three things change with
   * the search-space kind and NONE of them is cosmetic: the frequency-domain field is sized from
   * CORESET#0 in a common search space and from the active DL BWP in a UE-specific one; the decoded
   * PRB start is counted from the CORESET's lowest RB rather than the BWP start; and the TDRA list
   * is pdsch-ConfigCommon's rather than the dedicated one. Resolved once per occasion, then shared
   * (read-only) by every candidate task. ---- */
  const bool scan_11 = (cfg->dci10_scan != 2);
  bool scan_10 = (cfg->dci10_scan >= 1);
  /* DISCOVERY vs DECODE VOLUME (V2). Scanning format 1_0 next to 1_1 doubles the polar decodes per
   * candidate, and the scan queue was dropping 34 % of occasions. Once the cell has shown what it
   * sends -- >= 10000 accepts with 1_0 under 0.5 % of them -- keep 1_0 on 1 occasion in 8: enough
   * to notice a change (fallback grants, a new UE in its common search space), not enough to cost
   * the 1_1 decode budget. Evidence-led and self-reversing: the share is re-evaluated every call. */
  if (scan_10 && scan_11 && nr_agnostic_v2() && g_accepts > 10000 && g_accepts_10 * 200 < g_accepts) {
    static _Atomic uint64_t s_occ10 = 0;
    scan_10 = (atomic_fetch_add_explicit(&s_occ10, 1, memory_order_relaxed) & 7) == 0;
  }
  const bool scan_01 = nr_pdcch_blind_monitor_ul_scan_enabled(cfg);
  uint16_t   dci01_length = 0;
  if (scan_01) {
    dci01_length = cfg->dci01_length_override > 0 ? (uint16_t)cfg->dci01_length_override
                                                  : nr_pdcch_blind_dci01_size(&ul_opts);
  }
  nr_pdcch_blind_dci10_ctx_t dci10_ctx;
  memset(&dci10_ctx, 0, sizeof(dci10_ctx));
  uint16_t dci10_length = 0;
  int      dci10_rb_base = cfg->bwp_start;
  if (scan_10) {
    dci10_ctx.ss_type = (uint8_t)cfg->dci10_ss_type;
    /* 0 = auto. In a common search space the frequency reference is the CORESET the DCI arrived in
     * -- which, when this monitor is pointed at CORESET#0 (coreset_type = 1), is exactly the
     * CORESET#0 size the spec asks for; n_rb here is that CORESET's own measured RB count. */
    dci10_ctx.n_rb_riv = (cfg->dci10_n_rb_riv > 0)
                             ? (uint16_t)cfg->dci10_n_rb_riv
                             : ((cfg->dci10_ss_type == NR_BLIND_SS_COMMON) ? (uint16_t)n_rb : (uint16_t)cfg->bwp_size);
    /* 0 or negative = auto. TS 38.214 5.1.2.2.2 counts a common-search-space format-1_0 grant's PRBs
     * from the LOWEST RB OF THE CORESET, which in the same frame of reference as bwp_start is
     * bwp_start + the CORESET's own offset within it -- NOT cset_start alone. Measured on this cell
     * 2026-08-21: the gNB places CORESET#0 at "bwp=[1..49)", i.e. CRB 1, so dropping bwp_start would
     * put every SIB1 allocation one RB low and quietly corrupt the channel estimate. For a
     * UE-specific search space the CORESET offset does not enter at all and this is bwp_start. */
    dci10_rb_base = (cfg->dci10_rb_offset >= 0)
                        ? cfg->dci10_rb_offset
                        : (cfg->bwp_start + ((cfg->dci10_ss_type == NR_BLIND_SS_COMMON) ? cset_start : 0));
    dci10_ctx.rb_offset           = (uint16_t)dci10_rb_base;
    dci10_ctx.dmrs_typeA_position = (uint8_t)cfg->dmrs_typeA_position;
    dci10_ctx.mux_pattern         = (uint8_t)cfg->dci10_mux_pattern;
    dci10_ctx.sib1                = (uint8_t)(cfg->dci10_sib1 ? 1 : 0);
    dci10_ctx.rnti_class_mask     = (uint32_t)cfg->dci10_class_mask;
    dci10_length = cfg->dci10_length_override > 0 ? (uint16_t)cfg->dci10_length_override
                                                  : nr_pdcch_blind_dci10_size(dci10_ctx.n_rb_riv);
  }
  {
    /* One-shot: a scan ladder that silently covers the wrong format/size finds nothing and reports
     * no error, which is the same failure mode the aggregation-level ladder already had. */
    /* Must report the DEDICATED config. The CSS0 interleave swaps cfg to the CORESET#0 snapshot on
     * every s_css0_every'th occasion INCLUDING occasion 0, and this log is one-shot -- so it fired
     * on the snapshot and printed "1_1=off (len=46 bwp=48)": 1_0-exclusive at CORESET#0's 48 RB.
     * Both fields were then read as evidence that format 1_1 was disabled and that the dedicated
     * sweep was sized against a 48 RB BWP. Neither was true (2026-09-20). The whole point of this
     * line is to make a wrong ladder visible at startup, so printing the wrong config defeats it. */
    static int s_fmt_logged = 0;
    if (!s_fmt_logged && !css0_occasion) {
      s_fmt_logged = 1;
      LOG_I(PHY,
            "SENSING: blind PDCCH formats: 1_1=%s (len=%u bwp=%u) 1_0=%s (len=%u n_rb_riv=%u rb_offset=%d "
            "ss=%s class_mask=0x%x mux=%u sib1=%u)\n",
            scan_11 ? "on" : "off", (unsigned)dci_length, (unsigned)cfg->bwp_size, scan_10 ? "on" : "off",
            (unsigned)dci10_length, (unsigned)dci10_ctx.n_rb_riv, dci10_rb_base,
            (cfg->dci10_ss_type == NR_BLIND_SS_COMMON) ? "common" : "ue-specific",
            (unsigned)dci10_ctx.rnti_class_mask, (unsigned)dci10_ctx.mux_pattern, (unsigned)dci10_ctx.sib1);
      if (scan_01) {
        const uint16_t derived = nr_pdcch_blind_dci01_size(&ul_opts);
        LOG_I(PHY,
              "SENSING: blind PDCCH formats: 0_1=on (len=%u derived=%u ul_bwp=%u+%u tda=%d tp=%d "
              "mcs_tbl=%d add_pos=%d)\n",
              (unsigned)dci01_length, (unsigned)derived, (unsigned)cfg->ul.bwp_start,
              (unsigned)cfg->ul.bwp_size, cfg->ul.tda_count, cfg->ul.transform_precoding,
              cfg->ul.mcs_table, cfg->ul.dmrs_add_pos);
        /* The DL path learned this the expensive way: a live-verified TOTAL length with wrong
         * per-field widths reads every field after the frequency-domain assignment from the wrong
         * offset, while the CRC still passes and the RNTI still looks right. Warn loudly when the
         * override and the configured widths disagree -- the override is ground truth, so a
         * mismatch means the widths are wrong, not the override. */
        if (derived > 0 && dci01_length > 0 && derived != dci01_length) {
          LOG_W(PHY,
                "SENSING: DCI 0_1 width MISMATCH: configured field widths sum to %u but "
                "dci01_length_override says %u. The override is ground truth, so %d bit(s) are "
                "misassigned across the RRC-derived fields (TDA / freq-hopping / SRI / precoding / "
                "CSI-request). Every field after the frequency-domain assignment is being read from "
                "the wrong offset. Pin the layout with ISAC_PDCCH_ULDCIGT=1 against the gNB log "
                "before trusting any UL grant.\n",
                (unsigned)derived, (unsigned)dci01_length, (int)dci01_length - (int)derived);
        }
      }
    }
  }
  if (scan_10 && dci10_length == 0) {
    return; // misconfigured n_rb_riv -- nothing to scan rather than a wrong-width sweep
  }

  // ---- FEP the CORESET's own symbol(s) + generate LLR (reuses the real RT PDCCH pipeline). ----
  const int llr_size_symbol    = n_rb * NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS;
  const int num_monitoring_occ = 1; // exactly one bit set in StartSymbolBitmap by construction above
  /* Sized from the SPEC MAXIMUM, not a guess. A CORESET's frequency-domain resource is a bitmap of
   * 6-PRB groups over the BWP, so it spans at most floor(275/6) = 45 groups = 270 PRB, and its
   * duration is at most 3 symbols (38.331 ControlResourceSet::duration). The previous 255-RB bound
   * was inherited from the small rfsim CORESET and is EXCEEDED by any real wideband dedicated
   * CORESET: measured on a live 273 PRB srsRAN cell, the dedicated CORESET is 45 groups = 270 PRB,
   * which tripped the bound check below and silently disabled blind PDCCH monitoring entirely
   * (blind=0, no accepts, no error other than this one line). ~30 kB on the stack at the maximum. */
  c16_t pdcch_llr[1][1][NR_PDCCH_BLIND_MAX_CORESET_RB * NR_PDCCH_BLIND_MAX_CORESET_DURATION
                        * NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS];
  if ((size_t)(rel15->coreset.duration * llr_size_symbol) > sizeof(pdcch_llr[0][0]) / sizeof(c16_t)) {
    LOG_E(PHY, "SENSING: blind PDCCH monitor CORESET too large for local LLR buffer (n_rb=%d)\n", n_rb);
    return;
  }

  const uint32_t rxdataF_sz = fp->samples_per_slot_wCP;
  __attribute__((aligned(32))) c16_t rxdataF[fp->nb_antennas_rx][rxdataF_sz];

  /* See nr_pdcch_blind_llr_autoscale's comment in dci_nr.c: the stock equaliser scale is derived
   * from the mean level over the whole CORESET, which for a blind full-BWP monitor is dominated by
   * empty REs and drives real PDCCH symbols past the LLR clip rail. Enabled only for this path. */
  extern int nr_pdcch_blind_llr_autoscale;
  nr_pdcch_blind_llr_autoscale = 1;
  extern int nr_pdcch_blind_dmrs_probe;
  nr_pdcch_blind_dmrs_probe = 1;
  extern int nr_pdcch_blind_capture;
  nr_pdcch_blind_capture = (getenv("ISAC_PDCCH_CAPTURE") != NULL);

  btim_add(BTIM_PRE, btim_occ0);
  const uint64_t btim_t_fep = btim_on ? btim_now() : 0;
  for (int symbol = cfg->ss_first_symbol; symbol < cfg->ss_first_symbol + rel15->coreset.duration; symbol++) {
    nr_slot_fep(ue, fp, proc->nr_slot_rx, symbol, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
    __attribute__((aligned(32))) c16_t rxdataF_symb[fp->nb_antennas_rx][((fp->ofdm_symbol_size + 7) / 8) * 8];
    for (int ant = 0; ant < fp->nb_antennas_rx; ant++) {
      memcpy(rxdataF_symb[ant], &rxdataF[ant][symbol * fp->ofdm_symbol_size], sizeof(c16_t) * fp->ofdm_symbol_size);
    }
    nr_pdcch_generate_llr(ue, proc, symbol, &local_phy_data, llr_size_symbol, num_monitoring_occ,
                         rel15->coreset.duration, rxdataF_symb, pdcch_llr);
  }
  btim_add(BTIM_FEP_LLR, btim_t_fep);
  /* ---- Passive BWP: CORESET discovery (nr_passive_bwp.h). A dedicated BWP's CORESET lives inside that
   * BWP, so this CORESET never carries its DCIs. Every 8th occasion, correlate each 6-RB window of the
   * CORESET symbols against the PDCCH DM-RS under the spec reference (CRB 0) and the OAI one (the BWP
   * start, within 5 RB below the window), and let the tracker find a second CORESET. ~6k MAC/symbol. */
  const uint64_t btim_t_pbwp = btim_on ? btim_now() : 0;
  if (pbwp_on) {
    static uint32_t s_cs_tick;
    static bool s_cs_logged;
    if ((++s_cs_tick & 7) == 0) {
      const int n_win = fp->N_RB_DL / 6 < NR_PBWP_CS_MAXWIN ? fp->N_RB_DL / 6 : NR_PBWP_CS_MAXWIN;
      const int base_lo = (cfg->bwp_start + cfg->coreset_rb_offset) / 6;
      const int base_hi = base_lo + cfg->coreset_freq_domain - 1;
      c16_t pilot[fp->N_RB_DL * 3];
      float corr[NR_PBWP_CS_MAXWIN];
      int16_t ref[NR_PBWP_CS_MAXWIN];
      for (int sym = 0; sym < 2; sym++) {
        const int symbol = cfg->ss_first_symbol + sym;
        if (sym >= rel15->coreset.duration)
          nr_slot_fep(ue, fp, proc->nr_slot_rx, symbol, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
        nr_pdcch_coreset_pilot(cfg->coreset_pdcch_dmrs_scrambling_id, proc->nr_slot_rx, symbol, fp->N_RB_DL, pilot);
        const c16_t *y = &rxdataF[0][symbol * fp->ofdm_symbol_size];
        for (int w = 0; w < n_win; w++) {
          corr[w] = (float)nr_pdcch_coreset_window_corr(y, fp->ofdm_symbol_size, fp->first_carrier_offset, pilot,
                                                        fp->N_RB_DL, w * 6, 0);
          ref[w] = 0;
          for (int d = 0; d <= 5 && w * 6 - d > 0; d++) {
            const float c = (float)nr_pdcch_coreset_window_corr(y, fp->ofdm_symbol_size, fp->first_carrier_offset,
                                                                pilot, fp->N_RB_DL, w * 6, w * 6 - d);
            if (c > corr[w]) { corr[w] = c; ref[w] = (int16_t)(w * 6 - d); }
          }
        }
        pthread_mutex_lock(&g_pbwp_lock);
        nr_pbwp_coreset_observe(&g_pbwp, n_win, base_lo, base_hi, cfg->bwp_start, corr, ref, sym, 0.8f);
        pthread_mutex_unlock(&g_pbwp_lock);
        /* ISAC_BWP_DIAG=1: every 256 observations, the 4 strongest windows of symbol 0 with their winning
         * reference -- what the tracker is actually seeing, when no CORESET gets declared. */
        static int s_bwp_diag = -1;
        if (s_bwp_diag < 0)
          s_bwp_diag = getenv("ISAC_BWP_DIAG") ? 1 : 0;
        static uint32_t s_diag_n;
        if (s_bwp_diag && sym == 0 && (++s_diag_n % 256) == 0) {
          char line[256];
          int u = 0;
          bool used[NR_PBWP_CS_MAXWIN] = {false};
          for (int k = 0; k < 4; k++) {
            int bw = -1;
            for (int w = 0; w < n_win; w++)
              if (!used[w] && (bw < 0 || corr[w] > corr[bw]))
                bw = w;
            if (bw < 0)
              break;
            used[bw] = true;
            u += snprintf(line + u, sizeof(line) - u, " RB%d:%.2f(ref %d, hits %u)", bw * 6, corr[bw], ref[bw],
                          g_pbwp.cs.hits[bw][0]);
          }
          LOG_A(PHY, "SENSING: BWP_DIAG coreset obs=%u base=[w%d..w%d ref %d]%s\n", g_pbwp.cs.occ, base_lo, base_hi,
                cfg->bwp_start, line);
        }
      }
      int cs_start, cs_n, cs_dur, cs_ref;
      pthread_mutex_lock(&g_pbwp_lock);
      const bool have = nr_pbwp_coreset_hypothesis(&g_pbwp, &cs_start, &cs_n, &cs_dur, &cs_ref);
      pthread_mutex_unlock(&g_pbwp_lock);
      if (have && !s_cs_logged) {
        s_cs_logged = true;
        LOG_A(PHY, "SENSING: BWP CORESET found: RB %d..%d (%d RB), %d symbol(s), DM-RS reference RB %d (%s)\n",
              cs_start, cs_start + cs_n - 1, cs_n, cs_dur, cs_ref, cs_ref ? "BWP start, OAI-style" : "CRB 0, 38.211");
      }
    }
  }
  btim_add(BTIM_PBWP, btim_t_pbwp);
  if (!nr_passive_samples_valid(
          atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed),
          source_absolute_slot, fp->slots_per_frame))
    return; /* No discovery/CRC evidence from an overwritten CORESET window. */

  /* Blind CSI-RS search, observe-only (ISAC_CSIRS_BLIND=1, default off). Placed HERE, after the
   * sample-lifetime check, deliberately: scoring a candidate against a window the producer has
   * already overwritten would feed the correlator next frame's samples and manufacture hits that
   * no periodicity test could distinguish from a real resource. It reads rxdataF and writes
   * nothing the decoder consumes, so it cannot affect decoding. */
  /* IN-LINE, ONCE PER OCCASION, UNTIL CONFIRMED -- then a free early return (see
   * nr_csirs_blind_rt_slot). A separate consumer thread was tried (2026-09-17) and starved:
   * unpinned at FIFO 40 it processed 3519 of 145k slots and never confirmed, because every slot
   * dropped stretches the round-robin search's time-to-confirm by the same factor. Correctness over
   * cleverness: the body costs the csirs bucket below only while unconfirmed. */
  const uint64_t btim_t_csirs = btim_on ? btim_now() : 0;
  nr_csirs_blind_rt_slot(ue, proc->nr_slot_rx,
                         source_absolute_slot >= 0 ? (uint32_t)source_absolute_slot : 0u,
                         rxdataF);
  btim_add(BTIM_CSIRS, btim_t_csirs);

  /* XCHECK diagnostic (2026-09-06, Task 5 follow-up): run Technique A's own correlation function
   * on THIS FEP output -- the manual, live-verified ground-truth config's own receive chain, which
   * genuinely decodes real DCIs here -- rather than the discovery tap's separate single-antenna
   * FEP call. If corr at rb_offset==0 is high here, Technique A's correlation math is fine and the
   * bug is specific to the discovery tap's own FEP/plumbing (nr_pdcch_blind_monitor_rt.c's
   * nr_slot_fep_ant call in the autodiscover_step path). If it's ALSO near-zero here, the bug is in
   * nr_pdcch_coreset_map_scan()/nr_pdcch_dmrs_ref() itself, isolated from any autodiscover-specific
   * config or wiring issue -- this path's config is proven correct by the FULLCRC decodes it
   * produces. Env-gated on the same ISAC_DISCOVER_DIAG flag as the discovery-side diagnostics. */
  static int s_xcheck_diag = -1;
  if (s_xcheck_diag < 0)
    s_xcheck_diag = (getenv("ISAC_DISCOVER_DIAG") != NULL) ? 1 : 0;
  if (s_xcheck_diag) {
#define XCHECK_MAX_CANDIDATE_WINDOWS (273 / 6) // generous headroom for a 273 PRB carrier, mirrors nr_pdcch_blind_monitor.c
    nr_pdcch_coreset_candidate_t xcheck_cand[XCHECK_MAX_CANDIDATE_WINDOWS];
    const int xcheck_n = nr_pdcch_coreset_map_scan(&rxdataF[0][cfg->ss_first_symbol * fp->ofdm_symbol_size],
                                                   fp->ofdm_symbol_size, fp->N_RB_DL, fp->first_carrier_offset,
                                                   cfg->coreset_pdcch_dmrs_scrambling_id, proc->nr_slot_rx,
                                                   cfg->ss_first_symbol, xcheck_cand,
                                                   XCHECK_MAX_CANDIDATE_WINDOWS);
    static int s_xcheck_calls = 0;
    s_xcheck_calls++;
    if (xcheck_n > 0 || (s_xcheck_calls % 50) == 1) {
      LOG_A(PHY, "XCHECK calls=%d n=%d best_corr=%.4f best_rb=%d (manual-conf FEP, scrambling_id=%u)\n",
           s_xcheck_calls, xcheck_n, xcheck_n > 0 ? xcheck_cand[0].corr : -1.0,
           xcheck_n > 0 ? xcheck_cand[0].rb_offset : -1, cfg->coreset_pdcch_dmrs_scrambling_id);
    }
  }

  // ---- Demapping/deinterleaving + per-candidate unscrambling/decode. Mirrors dci_nr.c's own
  // nr_pdcch_dci_indication()/nr_dci_decoding_procedure(), minus the own-RNTI equality gate --
  // that's the entire "blind" widening (nr_pdcch_blind_decode_and_extract() does its own range
  // check instead). ----
  const int llr_stride = llr_size_symbol; // duration==1 here -> llr_size == llr_size_symbol
  c16_t pdcch_e_rx[NR_MAX_PDCCH_SIZE];
  if (cfg->autodiscover && cfg->coreset_type != 1)
    blind_discovery_replay(fp,proc,rel15,n_rb,rel15->BWPStart+cset_start+rel15->coreset.rb_offset,cfg->ss_first_symbol,
        pdcch_llr[0][0],&rxdataF[0][cfg->ss_first_symbol*fp->ofdm_symbol_size],source_absolute_slot,-1,0,0,0,NULL);
  const uint64_t btim_t_dmp = btim_on ? btim_now() : 0;
  /* Grid is built once from this occasion's FFT and reused by all geometry lanes. It has no
   * mutable cross-occasion evidence and cannot associate a stale histogram with a current RNTI. */
  nr_pdcch_dmrs_rank_grid_t rank_grid;
  rank_grid.n_rb = 0;
  const bool rank_known_uss = cfg->coreset_type == 1
                                && cfg->dci10_ss_type == NR_BLIND_SS_UE_SPECIFIC;
  const bool rank_discovery = blind_dmrs_rank_mode() != 0
                                && ((cfg->autodiscover
                                     && !nr_pdcch_blind_monitor_autodiscover_extent_verified())
                                    || rank_known_uss);
  /* Keep the measurement grid available after extent verification while UL length is unresolved.
   * The regular candidate list remains exhaustive; only the UL length sweep uses its top candidate. */
  const bool build_rank_grid = blind_dmrs_rank_mode() != 0
                                 && (rank_discovery || scan_01);
  static __thread uint64_t rank_visit;
  static __thread uint64_t rank_lane_visit[NR_PDCCH_LOOKAHEAD_MAX];
  if (build_rank_grid && nr_pdcch_dmrs_rank_grid(&rank_grid, rxdataF[0], fp->ofdm_symbol_size,
      fp->first_carrier_offset, fp->N_RB_DL, rel15->coreset.pdcch_dmrs_scrambling_id,
      proc->nr_slot_rx, cfg->ss_first_symbol, rel15->coreset.duration, 0)
      && rank_discovery) {
    blind_dmrs_rank_candidates(&rank_grid, rel15, n_rb,
                               rel15->BWPStart + cset_start + rel15->coreset.rb_offset,
                               proc->nr_slot_rx, cfg->ss_first_symbol, abs_slot, rank_visit++);
  }
  nr_pdcch_demapping_deinterleaving((uint32_t)n_rb, pdcch_llr[0][0], pdcch_e_rx, rel15->coreset.duration,
                                    rel15->coreset.RegBundleSize, rel15->coreset.InterleaverSize,
                                    rel15->coreset.ShiftIndex, rel15->number_of_candidates, rel15->CCE, rel15->L,
                                    llr_stride);
  btim_add(BTIM_DEMAP, btim_t_dmp);

  /* ------------------------------------------------------------------------------------------
   * PURE INDEXING CHECK (2026-08-04). Does the CCE's equalised data-RE content actually land in
   * the LLR slice that polar consumes?
   *
   * This is a STRUCTURAL property, so it needs no occupied CCE and no real grant -- the demapper
   * is a permutation of its input, and noise makes a perfectly good fingerprint. Under
   * NON-interleaved mapping with duration 1, CCE c occupies CORESET REs [54c, 54c+54) of the
   * demapper INPUT (pdcch_llr, 9 data REs per RB x 6 RBs per CCE). So for a candidate at CCE c with
   * aggregation level L, the demapper OUTPUT slice pdcch_e_rx[off .. off + 54L) must be exactly
   * pdcch_llr[54c .. 54c + 54L).
   *
   *   match      -> demapper slicing is right; the fault is later (deinterleave ORDER within the
   *                 slice, LLR sign convention, unscrambling, rate recovery, polar).
   *   mismatch   -> the fault is the indexing/write offset itself.
   *   partial    -> REG-bundle / symbol-local offset problem.
   *
   * On mismatch it also SEARCHES the input for where the slice actually came from, which names the
   * offending offset directly instead of leaving it to be guessed. ---------------------------- */
  {
    static int s_idx_left = 6;
    if (s_idx_left > 0 && rel15->number_of_candidates > 0) {
      const c16_t *in = pdcch_llr[0][0];
      int probe_off = 0;
      char rep[600];
      int u = 0;
      for (int j = 0; j < rel15->number_of_candidates && j < 6 && u < (int)sizeof(rep) - 90; j++) {
        const int Lc = rel15->L[j];
        const int cc = rel15->CCE[j];
        const int n_re = NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * Lc * 6; /* 54*L */
        const int exp_base = 54 * cc;
        int same = 0;
        if (exp_base + n_re <= llr_stride) {
          for (int i = 0; i < n_re; i++) {
            if (pdcch_e_rx[probe_off + i].r == in[exp_base + i].r
                && pdcch_e_rx[probe_off + i].i == in[exp_base + i].i) {
              same++;
            }
          }
        }
        /* If it does not line up, find where this slice really lives in the input. */
        int found_at = -1;
        if (same != n_re) {
          const c16_t a = pdcch_e_rx[probe_off + 0];
          const c16_t b = pdcch_e_rx[probe_off + 1];
          const c16_t c2 = pdcch_e_rx[probe_off + 2];
          for (int q = 0; q + 2 < llr_stride; q++) {
            if (in[q].r == a.r && in[q].i == a.i && in[q + 1].r == b.r && in[q + 1].i == b.i
                && in[q + 2].r == c2.r && in[q + 2].i == c2.i) {
              found_at = q;
              break;
            }
          }
        }
        u += snprintf(rep + u, sizeof(rep) - u, "[j%d L%d cce%d exp%d %d/%d%s] ",
                      j, Lc, cc, exp_base, same, n_re,
                      (same == n_re) ? "" : (found_at >= 0 ? (snprintf(rep + u + 0, 0, "") , " src=?") : " src=none"));
        if (same != n_re && found_at >= 0 && u < (int)sizeof(rep) - 24) {
          u += snprintf(rep + u, sizeof(rep) - u, "src=%d(d%+d) ", found_at, found_at - exp_base);
        }
        probe_off += n_re;
      }
      LOG_W(PHY, "SENSING: IDXCHK n_rb=%d stride=%d ncand=%d %s\n",
            n_rb, llr_stride, rel15->number_of_candidates, rep);
      s_idx_left--;
    }
  }

  /* ---- PHASE 3 (2026-09-04, REWRITTEN 2026-09-06): Technique C, dci_length histogram sweep -----
   * ACCUMULATES across occasions (guarded by g_length_swept once it succeeds, and by a bounded
   * occasions-fed cap if it never does) after Technique A has confirmed the dedicated CORESET's
   * geometry but before its dci_length is known -- placed here, AFTER pdcch_e_rx[] is populated,
   * so the candidate list below walks the SAME e_rx_cand_idx cursor the pre-pass loop further down
   * uses (Step 4's own warning: not a second, independently-wrong indexing scheme). Does not
   * disturb the pre-pass loop's own walk -- this is a separate, throwaway replay of the identical
   * boundary arithmetic (a pure function of L[]/number_of_candidates, independent of dci_length).
   *
   * Live-measured 2026-09-06 why this must accumulate rather than fire once: this cell's real
   * accept rate is ~2% of occasions (measured on the proven-working manual-conf path,
   * 2801/138000), so a SINGLE occasion's ~20-40 candidates essentially never contains the real,
   * decodable candidates needed to reach significance without a bootstrap RNTI to anchor on --
   * and Technique B's bootstrap_rnti cannot be nonzero yet this early regardless (it can only
   * accumulate from run_occasion()'s own accept path, which needs the correct length to ever
   * accept anything -- see the handover doc's still-open item 5). Per-call cost (34 lengths x this
   * occasion's own real candidate count, typically ~20-40) is actually LOWER than the original
   * one-shot design's fixed 34*64 budget, so calling this every candidate-bearing occasion until
   * it converges is not a new order of magnitude of RT cost, just spread over more occasions. */
  /* DCI 1_1 length evidence is owned by (exact CORESET geometry, RNTI). This is also used for
   * CORESET#0 USS: a known physical CORESET does not make its dedicated payload width known. */
  uint16_t dl_ready_rnti[NR_PDCCH_BLIND_MAX_UE] = {0};
  uint16_t dl_ready_len[NR_PDCCH_BLIND_MAX_UE] = {0};
  int n_dl_ready = 0;
  uint16_t dl_known[NR_PDCCH_BLIND_MAX_UE] = {0};
  /* Expensive dedicated sweeps admit only recurring C-RNTIs or stable USS-hash tracks. A
   * RAR-proved TC-RNTI may be unrelated to the operational SCG C-RNTI under NSA CFRA. */
  int n_known_dl = nr_pdcch_blind_monitor_dedicated_rnti_set(
      abs_slot, dl_known, NR_PDCCH_BLIND_MAX_UE);
  /* USS CCE hashing narrows positions but cannot identify an exact 16-bit RNTI when
   * monitoring occasions repeat the same slot phase. Exact identities enter only through
   * multi-occasion CRC/payload recurrence below. */
  const uint64_t dl_geom = length_coreset_key(cfg);
  const bool dl_uss_auto = scan_11 && cfg->dl_full_auto
                           && cfg->dci10_ss_type != NR_BLIND_SS_COMMON;
  const bool dl_geometry_ready = !cfg->autodiscover
                                 || nr_pdcch_blind_monitor_autodiscover_done();

  /* Operator-forced length remains an explicit diagnostic for the active autodiscovery geometry. */
  if (cfg->autodiscover && dl_geometry_ready && !g_length_swept) {
    static int s_force_len = -1;
    if (s_force_len < 0) {
      const char *e = getenv("ISAC_FORCE_DCI_LEN");
      s_force_len = (e != NULL) ? atoi(e) : 0;
    }
    if (s_force_len > 0) {
      nr_pdcch_blind_monitor_autodiscover_set_dci_length(s_force_len);
      g_length_swept = true;
      g_length_found = true;
      LOG_A(PHY, "SENSING: Phase 3 autodiscover -- dci_length FORCED to %d (ISAC_FORCE_DCI_LEN)\n",
            s_force_len);
    }
  }

  const uint64_t btim_t_dlsw = btim_on ? btim_now() : 0;
  if (dl_uss_auto && dl_geometry_ready) {
    pthread_mutex_lock(&g_dl_length_lock);
    uint64_t evicted_geom = 0;
    nr_pdcch_dci_length_bank_t *dl_bank =
        nr_pdcch_dci_length_store_get(&g_dl_length_store, dl_geom, &evicted_geom);
    if (evicted_geom && getenv("ISAC_DISCOVER_DIAG") != NULL)
      LOG_A(PHY, "SENSING: DL length store evicted coreset_key=%llu for key=%llu\n",
            (unsigned long long)evicted_geom, (unsigned long long)dl_geom);

    uint16_t bootstrap_rnti = 0;
    nr_pdcch_dci_length_context_t *dlc = NULL;
    if (n_known_dl > 0) {
      uint64_t pick = (uint64_t)abs_slot + dl_geom + UINT64_C(0x9e3779b97f4a7c15);
      pick = (pick ^ (pick >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
      pick = (pick ^ (pick >> 27)) * UINT64_C(0x94d049bb133111eb);
      pick ^= pick >> 31;
      /* Prefer an unresolved UE, but rotate the starting point so periodic traffic cannot starve
       * one context. */
      for (int k = 0; k < n_known_dl; ++k) {
        const uint16_t r = dl_known[(pick + (uint64_t)k) % (uint64_t)n_known_dl];
        nr_pdcch_dci_length_context_t *c = nr_pdcch_dci_length_context(dl_bank, dl_geom, r);
        if (!c->found && !c->exhausted) {
          bootstrap_rnti = r;
          dlc = c;
          break;
        }
      }
    }

    /* Before an identity exists, a deadline-bounded anonymous state remains owned by this exact
     * CORESET bank. It jointly discovers (RNTI,length); it never promotes a hash alias. */
    const bool dl_anonymous = dlc == NULL && n_known_dl == 0 && dl_bank && !dl_bank->anonymous_exhausted;
    nr_pdcch_dci_length_sweep_state_t *dl_state =
        dlc ? &dlc->state : (dl_anonymous ? &dl_bank->anonymous : NULL);
    if (dl_state && (dlc || !dl_bank->anonymous_found)) {
      nr_pdcch_autodiscover_cand_t disc_cand[64];
      int disc_n_cand = 0, idx = 0;
      for (int c = 0; c < rel15->number_of_candidates && disc_n_cand < 64; ++c) {
        const int L = rel15->L[c];
        disc_cand[disc_n_cand++] = (nr_pdcch_autodiscover_cand_t){
            .e_rx = &pdcch_e_rx[idx], .L = (uint8_t)L, .cce = rel15->CCE[c]};
        idx += NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * L * 6;
      }
      if (disc_n_cand > 0) {
        nr_pdcch_autodiscover_sweep_ctx_t sweep_ctx = {
            .cand = disc_cand,
            .n_cand = disc_n_cand,
            .bwp_size = (uint16_t)cfg->bwp_size,
            .dmrs_typeA_position = (uint8_t)cfg->dmrs_typeA_position,
            .rnti_min = bootstrap_rnti ? bootstrap_rnti : cfg->rnti_min,
            .rnti_max = bootstrap_rnti ? bootstrap_rnti : cfg->rnti_max,
            .extract_opts = &cfg->extract,
            .scrambling_rnti = rel15->coreset.scrambling_rnti,
            .dmrs_scrambling_id = rel15->coreset.pdcch_dmrs_scrambling_id,
            .bootstrap_alias = bootstrap_rnti,
            .lane = -1,
            .rb_offset = rel15->BWPStart + cset_start + rel15->coreset.rb_offset,
            .span_rb = n_rb,
            .bundle = rel15->coreset.RegBundleSize,
            .interleaver = rel15->coreset.InterleaverSize,
            .shift = rel15->coreset.ShiftIndex,
        };
        sweep_ctx.n_known = (uint8_t)n_known_dl;
        memcpy(sweep_ctx.known_rnti, dl_known, (size_t)n_known_dl * sizeof(dl_known[0]));
        dl_state->excluded_len = dci10_length;
        dl_state->stride = dci_sweep_stride();
        if (dlc && dl_state->preferred_len == 0 && cfg->dci_length_override >= dci_len_min()
            && cfg->dci_length_override <= dci_len_max())
          dl_state->preferred_len = cfg->dci_length_override;

        if (budget_active)
          g_sweep_cache.valid = 0;
        else
          sweep_gpu_prefill(&sweep_ctx, dci_len_min(), dci_len_max());
        discovery_scope.before_feed = btim_on ? btim_now() : 0;
        discovery_scope.phase = 1;
        const uint64_t trace_decodes = dl_state->decodes;
        int found_len = (budget_active && budget_owner != 0) ? -1 :
            nr_pdcch_dci_length_sweep_feed_budget(
                dl_state, nr_pdcch_autodiscover_length_scorer, &sweep_ctx, disc_n_cand,
                dci_len_min(), dci_len_max(), bootstrap_rnti, sweep_deadline, 0);
        uint16_t locked_rnti = bootstrap_rnti;
        if (found_len > 0 && dl_anonymous) {
          locked_rnti = nr_pdcch_dci_length_sweep_winner_rnti(dl_state, found_len);
          if (!locked_rnti)
            found_len = -1;
        }
        discovery_scope.after_feed = btim_on ? btim_now() : 0;
        discovery_scope.trials = dl_state->decodes - trace_decodes;
        discovery_scope.phase = 2;
        if (found_len > 0) {
          if (dlc) {
            dlc->found = found_len;
            nr_pdcch_dci_length_bank_converged(dl_bank, locked_rnti, found_len);
          } else {
            dl_bank->anonymous_found = found_len;
            dl_bank->anonymous_rnti = locked_rnti;
            nr_pdcch_blind_rnti_bootstrap_record_corroborated(
                locked_rnti, NR_BLIND_RNTI_CLASS_C, abs_slot);
            nr_pdcch_dci_length_context_t *promoted =
                nr_pdcch_dci_length_context(dl_bank, dl_geom, locked_rnti);
            if (promoted)
              promoted->found = found_len;
          }
          LOG_A(PHY, "SENSING: DCI 1_1 length locked coreset=%llu rnti=0x%x len=%d "
                     "occasions=%d decodes=%llu evidence=distinct_ota\n",
                (unsigned long long)dl_geom, locked_rnti, found_len,
                dl_state->occasions_fed, (unsigned long long)dl_state->decodes);
          if (cfg->autodiscover) {
            nr_pdcch_blind_monitor_autodiscover_set_dci_length(found_len);
            g_length_swept = true;
            g_length_found = true;
          }
        } else if (dl_state->occasions_fed >= AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS) {
          if (dlc)
            dlc->exhausted = true;
          else if (dl_bank)
            dl_bank->anonymous_exhausted = true;
          if (cfg->autodiscover) {
            const int occasions = dl_state->occasions_fed;
            nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start + cfg->coreset_rb_offset);
            dl_discovery_invalidate();
            pthread_mutex_unlock(&g_dl_length_lock);
            LOG_W(PHY, "SENSING: DCI length unresolved after %d occasions for coreset=%llu rnti=0x%x; "
                       "next geometry\n", occasions, (unsigned long long)dl_geom, bootstrap_rnti);
            return;
          }
        }
      }
    }

    /* Snapshot every resolved RNTI for this CORESET. Candidate tasks below use exact RNTI bounds
     * and each UE's own length; no formula-default 1_1 decode is admitted in full-auto mode. */
    for (int i = 0; i < n_known_dl && n_dl_ready < NR_PDCCH_BLIND_MAX_UE; ++i) {
      nr_pdcch_dci_length_context_t *c =
          nr_pdcch_dci_length_context(dl_bank, dl_geom, dl_known[i]);
      if (c && c->found > 0) {
        dl_ready_rnti[n_dl_ready] = dl_known[i];
        dl_ready_len[n_dl_ready] = (uint16_t)c->found;
        ++n_dl_ready;
      }
    }
    if (cfg->autodiscover && n_dl_ready > 0)
      g_length_found = g_length_swept = true;
    pthread_mutex_unlock(&g_dl_length_lock);
  }
  btim_add(BTIM_DLSWEEP, btim_t_dlsw);

  // Persistence window in slots -- computed once per occasion (cheap, only used when the gate is
  // enabled). Standard NR: 10ms/frame regardless of numerology, so slots_per_frame slots = 10ms.
  const uint32_t persist_window_slots =
      (cfg->rnti_persist_k > 1)
          ? (uint32_t)(((int64_t)cfg->rnti_persist_window_ms * fp->slots_per_frame) / 10)
          : 0;

  // ---- Pre-pass (sequential, cheap): compute each candidate's offset into pdcch_e_rx, apply the
  // energy gate, run the CONSTDIAG diagnostic, and build the task list for every candidate that
  // survives -- i.e. everything that does NOT need the expensive decode is filtered out BEFORE
  // the parallel phase, exactly as it was filtered before this loop was split. ----
  // Two entries per candidate at most: one per DCI format scanned (see the task struct's `format`
  // comment for why the two cannot share a decode). rel15->CCE[64]/L[64] bounds the candidate count.
  nr_pdcch_blind_cand_task_t cand_task[128];
  int nof_tasks = 0;

  /* ---- Technique B CONSUMER (2026-09-07). Until now nothing downstream read the bootstrapped
   * C-RNTI: its only two consumers (the dci_length sweep and the CORESET-footprint log line) both
   * run BEFORE it can possibly latch, so it confirmed an RNTI and then sat inert. Once the live
   * C-RNTI is known, the plausibility RANGE check collapses to an EQUALITY check for the two
   * formats addressed to that UE (1_1 DL, 0_1 UL) -- which is what the live, non-blind path does.
   * That takes the false-accept rate from the "plausible" field-sanity heuristic over a 65518-wide
   * range down to a genuine CRC match.
   *
   * Format 1_0 is deliberately LEFT WIDE: it carries SI-/RA-/P-RNTI, which are not this UE's
   * C-RNTI and would be rejected outright by a narrowed range.
   *
   * SELF-HEALING, so it needs no escape hatch: only a real sighting refreshes the confirmation, so
   * when the UE re-attaches under a new C-RNTI the old one stops being seen, goes stale after
   * RNTI_BOOTSTRAP_STALE_SLOTS (~10 s), and the scan reverts to the configured wide range and
   * re-bootstraps. Never pinned. */
  /* Both geometry and length are hypotheses. Advance even while length is unresolved,
   * and never combine current LLRs with a newly applied geometry. */
  /* Before a length lock, only the budget owner actually feeds this geometry's sweep. Counting
   * the other lanes' visits retired the primary after 125 of the intended 500 rounds at K=4 and
   * stride=8. After lock, every visit carries verification evidence and counts normally. */
  if (cfg->autodiscover && (g_length_found || !budget_active || budget_owner == 0)
      && nr_pdcch_blind_monitor_autodiscover_extent_step(abs_slot)) {
    dl_discovery_invalidate();
    return;
  }
  /* The temporary CSS0 snapshot must not arm or replace the dedicated PDSCH
   * configuration. Its DCI 1_0 grants bypass this bank. */
  if (!css0_occasion)
    pdsch_sweep_maybe_enable(cfg);

  /* MULTI-UE: the confirmed SET, not one RNTI. Narrowing acceptance to a single C-RNTI discarded
   * every grant addressed to any other UE on the cell -- which for a passive receiver is most of
   * the traffic. The false-accept reduction that motivated the narrowing is kept: a noise decode
   * yields a random 24-bit CRC, so demanding membership in a set of at most 16 confirmed values is
   * still ~4000x tighter than the 65518-wide plausibility range.
   * The range itself stays WIDE so that an as-yet-unconfirmed UE can still be discovered; the set
   * is applied as a GATE after extraction (see g_held_rnti_set below), which is what makes
   * discovery and filtering coexist. */
  uint16_t boot_rnti = 0;
  uint16_t known_ul[NR_PDCCH_BLIND_MAX_UE];
  const int n_known_ul = nr_pdcch_blind_monitor_dedicated_rnti_set(abs_slot,
      known_ul, NR_PDCCH_BLIND_MAX_UE);
  /* Selection happens only on an eligible sweep below. It is keyed by a mixed absolute slot:
   * a plain round-robin counter aliases with periodic schedulers whenever the grant period is a
   * multiple of the confirmed-UE count, selecting the same UE on every occupied occasion. */

  /* full_auto=0 never feeds a search or replaces a manual UL option/length.
   * Auto has no silent fallback: unresolved searches do not emit guessed grants. */
  discovery_scope.after_extent=btim_on?btim_now():0;
  discovery_scope.phase=3;
  bool ul_ready=false;
#define dl_resolved(r_) ({ bool f_ = false; for (int q_ = 0; q_ < n_dl_ready; ++q_) f_ |= dl_ready_rnti[q_] == (r_); f_; })
  uint16_t ul_ready_rnti[NR_PDCCH_BLIND_MAX_UE], ul_ready_len[NR_PDCCH_BLIND_MAX_UE];
  int n_ul_ready = 0;
  const uint64_t btim_t_ulsw = btim_on ? btim_now() : 0;
  pthread_mutex_lock(&ul_length_lock);
  const uint64_t geom = length_coreset_key(cfg);
  uint64_t evicted_geom = 0;
  nr_pdcch_dci_length_bank_t *ul_bank =
      nr_pdcch_dci_length_store_get(&g_ul_length_store, geom, &evicted_geom);
  if (evicted_geom && getenv("ISAC_DISCOVER_DIAG") != NULL)
    LOG_A(PHY, "SENSING: UL length store evicted coreset_key=%llu for key=%llu\n",
          (unsigned long long)evicted_geom, (unsigned long long)geom);
  /* Interleaved CORESETs retain independent evidence. Disabling a scan pauses its bank; it must
   * not erase it, and the downstream field-layout discovery is already keyed by RNTI/length/options. */
  const bool ul_sweep_enabled = !css0_occasion && cfg->dl_full_auto && scan_01;
  /* DCI format 0_1 is monitored in a UE-specific search space only (TS 38.213 10.1); on a common
   * search space occasion (CORESET#0 interleave, or a cfg whose SS is common) there is nothing to
   * sweep for, so the whole block is skipped. dci01_length stays unset for this occasion. */
  const bool ul_ss_possible = !css0_occasion && cfg->dci10_ss_type != NR_BLIND_SS_COMMON;
  nr_pdcch_dci_length_context_t *ulc = NULL;
  if(ul_sweep_enabled && ul_ss_possible && n_known_ul > 0
      && (!cfg->autodiscover || nr_pdcch_blind_monitor_autodiscover_extent_verified())) {
    /* Stateless avalanche mixing breaks grant-period/UE-count phase locking while remaining
     * deterministic and independent of any deployment timing. */
    uint64_t pick = (uint64_t)abs_slot + UINT64_C(0x9e3779b97f4a7c15);
    pick = (pick ^ (pick >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    pick = (pick ^ (pick >> 27)) * UINT64_C(0x94d049bb133111eb);
    pick ^= pick >> 31;
    boot_rnti = known_ul[pick % (uint64_t)n_known_ul];
    /* Sweep target: a UE PROVEN by its own DL decodes (DL length resolved) whose UL length is unresolved.
     * Falsely confirmed noise RNTIs never resolve a DL length; letting them start 34-length UL sweeps cost
     * ~160 us per occasion at ~1400 DCIs/s (s3live7). No such UE -> no UL sweep this occasion. */
    bool sweep_target = false;
    for (int k = 0; k < n_known_ul; ++k) {
      const uint16_t r = known_ul[(pick + (uint64_t)k) % (uint64_t)n_known_ul];
      if (!dl_resolved(r))
        continue;
      const nr_pdcch_dci_length_context_t *cx = nr_pdcch_dci_length_context(ul_bank, geom, r);
      if (!cx->found && !cx->exhausted) {
        boot_rnti = r;
        sweep_target = true;
        break;
      }
    }
    ulc = nr_pdcch_dci_length_context(ul_bank, geom, boot_rnti);
    if(sweep_target && !ulc->found && !ulc->exhausted) {
      nr_pdcch_autodiscover_cand_t candidates[64];
      int count=0, offset=0;
      for(int c=0;c<rel15->number_of_candidates && count<64;++c) {
        const int L=rel15->L[c];
        candidates[count++]=(nr_pdcch_autodiscover_cand_t){.e_rx=&pdcch_e_rx[offset],.L=L,.cce=rel15->CCE[c]};
        offset+=NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS*L*6;
      }
      if(count) {
        /* Put this occasion's strongest measured DM-RS candidate first. The ordinary decoder below
         * still receives every candidate; this ordering only lets the bounded length sweep cover all
         * lengths for the likeliest live CCE. */
        if (rank_grid.n_rb && blind_dmrs_rank_mode() != 0) {
          double score[64];
          for (int i = 0; i < count; ++i)
            score[i] = nr_pdcch_dmrs_candidate_score(
                &rank_grid, rel15->BWPStart + cset_start + rel15->coreset.rb_offset, n_rb,
                rel15->coreset.RegBundleSize, rel15->coreset.InterleaverSize,
                rel15->coreset.ShiftIndex, candidates[i].cce, candidates[i].L);
          /* Keep several hot CCEs, in score order. One strongest CCE is enough for one UE but
           * permanently starves every other RNTI when several UE-specific CORESETs are active in
           * the same slot. Four remains a bounded GPU batch and needs no signal threshold. */
          for (int i = 1; i < count; ++i) {
            const nr_pdcch_autodiscover_cand_t c = candidates[i];
            const double sc = score[i];
            int j = i - 1;
            while (j >= 0 && (!isfinite(score[j]) || (isfinite(sc) && score[j] < sc))) {
              candidates[j + 1] = candidates[j];
              score[j + 1] = score[j];
              --j;
            }
            candidates[j + 1] = c;
            score[j + 1] = sc;
          }
          /* Scores from different ALs are not directly interchangeable: a real AL4 grant also
           * makes its two AL2 halves look hot. Keep the four strongest candidates PER AL, enough
           * for four simultaneous UEs without returning to a whole-CORESET sweep. */
          nr_pdcch_autodiscover_cand_t selected[16];
          int per_al[4] = {0}, nsel = 0;
          for (int i = 0; i < count && nsel < 16; ++i) {
            const int ai = candidates[i].L == 1 ? 0 : candidates[i].L == 2 ? 1
                         : candidates[i].L == 4 ? 2 : 3;
            if (per_al[ai] < 4) {
              selected[nsel++] = candidates[i];
              ++per_al[ai];
            }
          }
          memcpy(candidates, selected, (size_t)nsel * sizeof(candidates[0]));
          count = nsel;
        }
        /* The main candidate list was already ranked from this occasion's measured DM-RS. During
         * length discovery, one top-ranked candidate tests every length in about one candidate's
         * budget; testing every mostly-empty CCE multiplied the sweep by the CORESET width. Mode 0
         * retains the exhaustive control. The batch itself must use the same bound: pre-decoding all
         * offered CCEs before a one-candidate feed merely moved the old cost outside the timer. */
        const int sweep_trials = count; /* ranked mode already compacted to <=4 candidates per AL */
        const nr_pdcch_autodiscover_sweep_ctx_t gctx={.cand=candidates,.n_cand=sweep_trials,
                            .scrambling_rnti=rel15->coreset.scrambling_rnti,
                            .dmrs_scrambling_id=rel15->coreset.pdcch_dmrs_scrambling_id};
        const bool gpu=sweep_gpu_prefill(&gctx, dci_len_min(), dci_len_max());
        ul_length_ctx_t ctx={.cand=candidates,.count=sweep_trials,.rnti=boot_rnti,
                            .scrambling_rnti=rel15->coreset.scrambling_rnti,
                            .dmrs_id=rel15->coreset.pdcch_dmrs_scrambling_id,
                            .gpu_ctx=gpu?&gctx:NULL};
        ulc->state.stride = dci_sweep_stride();
        const uint64_t ul_feed_start = btim_now();
        const uint64_t ul_deadline = discovery_budget_us() > 0
                                         ? ul_feed_start + 1000ull * discovery_budget_us()
                                         : 0;
        const int found=nr_pdcch_dci_length_sweep_feed_budget(&ulc->state,ul_length_score,&ctx,
                                                     sweep_trials, dci_len_min(), dci_len_max(),
                                                     boot_rnti, ul_deadline, 0);
        if (getenv("ISAC_DISCOVER_DIAG") != NULL)
          LOG_A(PHY, "SENSING: ULSWEEPTIM us=%.2f rnti=0x%x ranked_trials=%d offered=%d decodes=%llu\n",
                (double)(btim_now() - ul_feed_start) / 1000.0, boot_rnti, sweep_trials, count,
                (unsigned long long)ulc->state.decodes);
        /* A single matching decode cannot rule out a degenerate polar fixed point.
         * Require distinct UL payloads before trusting the shared engine's shortcut. */
        int supported_lengths=0;
        for(int len=30;len<=63;++len)
          if(ulc->state.n_distinct[len]>1 && ulc->state.bootstrap_hits[len]>=3)
            ++supported_lengths;
        if(found>0 && supported_lengths==1 && ulc->state.n_distinct[found]>1 &&
           ulc->state.bootstrap_hits[found]>=3) {
          ulc->found=found;
          LOG_A(PHY,"UL automatic DCI length locked: %d rnti=0x%x (occasions=%d polar_decodes=%llu)\n",
                found,boot_rnti,ulc->state.occasions_fed,(unsigned long long)ulc->state.decodes);
          /* Publish to the bank. On agreement between two distinct RNTIs this becomes the cell-wide
           * prior and every later RNTI skips its own 34-length sweep -- the discovery cost stops
           * scaling with the number of UEs, which is what breaks the consumer at high grant rates. */
          nr_pdcch_dci_length_bank_converged(ul_bank, boot_rnti, found);
        } else if(ulc->state.occasions_fed>=AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS && dl_resolved(boot_rnti)) {
          /* A PROVEN UE (its own DL length resolved) keeps trying: s3live4's 0x4643 (~2 UL grants/s, and a UL
           * size of 43 where the peer had 45) was exhausted forever inside the occasion budget. Unproven RNTIs --
           * falsely confirmed noise -- still exhaust: letting them sweep forever cost ~180 us/occasion (s3live6). */
          ulc->state.occasions_fed=0;
        } else if(ulc->state.occasions_fed>=AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS) {
          ulc->exhausted=true;
          LOG_W(PHY,"UL automatic length unresolved after %d occasions\n",ulc->state.occasions_fed);
        }
      }
    }
    ul_ready=ulc->found>0;
    if(ul_ready) dci01_length=ulc->found;
    /* MULTI-RNTI UL (2026-09-23): every UE whose UL length is resolved gets its own task below. The old
     * path decoded 0_1 for ONE picked UE per occasion, so with 2 UEs at most ~half the UL grants could be
     * recovered (live s3live3: ~30 %). */
    for (int k = 0; k < n_known_ul && n_ul_ready < NR_PDCCH_BLIND_MAX_UE; ++k) {
      const nr_pdcch_dci_length_context_t *cx = nr_pdcch_dci_length_context(ul_bank, geom, known_ul[k]);
      if (cx->found > 0) {
        ul_ready_rnti[n_ul_ready] = known_ul[k];
        ul_ready_len[n_ul_ready++] = (uint16_t)cx->found;
      }
    }
    ul_ready = n_ul_ready > 0;
  }
  pthread_mutex_unlock(&ul_length_lock);
  (void)ulc;
  btim_add(BTIM_ULSWEEP, btim_t_ulsw);
  discovery_scope.after_ul=btim_on?btim_now():0;
  discovery_scope.phase=4;
  const uint64_t btim_t_pre = btim_on ? btim_now() : 0;
  {
    /* DM-RS COHERENCE GATE (2026-09-23, 5GSniffer-style: correlate_DMRS() + AL_corr_thresholds, but
     * the threshold is DERIVED per occasion from THIS occasion's own candidates (median + MAD per AL)
     * instead of copied fixed constants -- 5GSniffer's {0.9,0.8,0.7,0.15,0.15} were tuned on their own
     * rig and would silently misfire on a different SNR/CORESET config (the macro). Independent of the
     * existing raw-energy Gate 1 below: that gate reads post-equalisation candidate energy and cannot
     * tell strong noise/interference from a real PDCCH; this one requires coherent match to the
     * ACTUAL DM-RS Gold sequence for this CORESET's nID, which noise energy cannot fake. Fail-open
     * (gates nothing) whenever the DM-RS reference grid isn't available -- unverified/unknown geometry
     * during discovery, or the mode disabled -- so it can never block finding a CORESET in the first
     * place, only reduce cost/false-accepts once one is known. */
    static int s_dmrs_gate_env = -1;
    if (s_dmrs_gate_env < 0) {
      /* DEFAULT OFF (2026-09-23, measured, not guessed): live A/B on this cell (run s3live9 vs s3live8,
       * everything else identical) -- decoded grants 94.0%->86.8%, false accepts ~33->4, over_slot(500us)
       * 9.9%->3.0%, drop_full way down. The gate DOES what it is supposed to (cuts noise, cuts cost) but
       * ALSO rejects real weaker-SNR candidates at the current z=3.09 (~1e-3) threshold -- a net LOSS
       * against this project's explicit priority (minimize lost grants), not a bug to hunt. Needs a
       * threshold sweep (looser z, or only gate candidates far below the energy-gate floor rather than a
       * tight statistical bound) before defaulting on. ISAC_PDCCH_DMRS_GATE=1 to re-enable for that A/B. */
      s_dmrs_gate_env = (getenv("ISAC_PDCCH_DMRS_GATE") != NULL && atoi(getenv("ISAC_PDCCH_DMRS_GATE")) != 0) ? 1 : 0;
    }
    double cand_dmrs_score[256];
    float  cand_al_thresh[5] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY}; /* AL 1,2,4,8,16 */
    const bool dmrs_gate_active = s_dmrs_gate_env && rank_grid.n_rb > 0
                                   && rel15->number_of_candidates <= 256;
    if (dmrs_gate_active) {
      int bucket_idx[5][256], bucket_n[5] = {0};
      for (int c = 0; c < rel15->number_of_candidates; c++) {
        cand_dmrs_score[c] = nr_pdcch_dmrs_candidate_score(
            &rank_grid, rel15->BWPStart + cset_start + rel15->coreset.rb_offset, n_rb,
            rel15->coreset.RegBundleSize, rel15->coreset.InterleaverSize, rel15->coreset.ShiftIndex,
            rel15->CCE[c], rel15->L[c]);
        const int ai = (rel15->L[c] == 1) ? 0 : (rel15->L[c] == 2) ? 1 : (rel15->L[c] == 4) ? 2
                     : (rel15->L[c] == 8) ? 3 : 4;
        if (bucket_n[ai] < 256)
          bucket_idx[ai][bucket_n[ai]++] = c;
      }
      /* Need enough candidates per AL for a stable median/MAD; too few -> that AL is not gated this
       * occasion (fail-open per-AL, not a global disable). Small-n insertion sort, counts are <= a few
       * dozen per AL in every deployment seen so far. */
      for (int ai = 0; ai < 5; ai++) {
        const int m = bucket_n[ai];
        if (m < 8)
          continue;
        double v[256];
        for (int i = 0; i < m; i++)
          v[i] = cand_dmrs_score[bucket_idx[ai][i]];
        for (int i = 1; i < m; i++) { double x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
        const double med = v[m / 2];
        double d[256];
        for (int i = 0; i < m; i++)
          d[i] = fabs(v[i] - med);
        for (int i = 1; i < m; i++) { double x = d[i]; int j = i - 1; while (j >= 0 && d[j] > x) { d[j + 1] = d[j]; j--; } d[j + 1] = x; }
        const double mad = 1.4826 * d[m / 2] > 1e-6 ? 1.4826 * d[m / 2] : 1e-6;
        cand_al_thresh[ai] = (float)(med + 3.09 * mad); /* ~1e-3 one-sided, same z as the rest of this project */
      }
    }
    int e_rx_cand_idx = 0;
    for (int c = 0; c < rel15->number_of_candidates; c++) {
      const int L         = rel15->L[c];
      const int n_re_cand = NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * L * 6;

      // ---- Candidate energy. Computed UNCONDITIONALLY since 2026-08-21, and that matters:
      // the floor estimator and the ENERGYPROBE diagnostic used to live INSIDE the
      // `if (gate enabled)` block below, so turning the gate off in order to characterise it --
      // which is exactly what R7 prescribes -- also switched off the only instrument that measures
      // what the gate is doing. A characterisation run then returned zero probe lines and looked
      // like "no energy", which is indistinguishable from "no measurement". Same trap as
      // FULLCRC living downstream of the gate. Cost of always computing it is a sum over 108-216
      // REs, negligible beside the polar decode it guards. ----
      const c16_t *e_raw   = &pdcch_e_rx[e_rx_cand_idx];
      double       sum_abs = 0;
      for (int i = 0; i < n_re_cand; i++) {
        sum_abs += (e_raw[i].r < 0 ? -e_raw[i].r : e_raw[i].r) + (e_raw[i].i < 0 ? -e_raw[i].i : e_raw[i].i);
      }
      const float mean_abs = (float)(sum_abs / n_re_cand);
      // The estimator must see the WHOLE population to stay calibrated -- feeding it only survivors
      // would let it collapse -- so it is updated before any thresholding, gate enabled or not.
      energy_floor_update(mean_abs);

      /* ENERGYPROBE (ISAC_PDCCH_ENERGY=1): strongest candidate of this occasion PER AGGREGATION
       * LEVEL, as a ratio to the measured noise floor. Per-AL because that is the open question on
       * this cell: CORESET#0 SIB1 at AL4 (216 REs) decodes at 85-90 % while the dedicated CORESET's
       * AL2 grants (108 REs, 3 dB less) decode at ~3 %, so "is the energy there at AL2" decides
       * whether the fix is RF or DSP. A CORESET actually carrying PDCCH must show candidates well
       * above the floor. */
      {
        static int s_ep = -1;
        if (s_ep < 0)
          s_ep = (getenv("ISAC_PDCCH_ENERGY") != NULL) ? 1 : 0;
        if (s_ep) {
          static float occ_max[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // AL 1,2,4,8
          static int   occ_slot   = -1;
          const int    al_i       = (L == 1) ? 0 : (L == 2) ? 1 : (L == 4) ? 2 : 3;
          if (proc->nr_slot_rx != occ_slot) {
            if (occ_slot >= 0 && g_energy_nseen >= ENERGY_FLOOR_WARMUP && g_energy_floor > 0.0f) {
              LOG_I(PHY, "ENERGYPROBE slot=%d floor=%.2f al1=%.2f al2=%.2f al4=%.2f al8=%.2f\n",
                    occ_slot, g_energy_floor, occ_max[0] / g_energy_floor, occ_max[1] / g_energy_floor,
                    occ_max[2] / g_energy_floor, occ_max[3] / g_energy_floor);
            }
            occ_slot   = proc->nr_slot_rx;
            occ_max[0] = occ_max[1] = occ_max[2] = occ_max[3] = 0.0f;
          }
          if (mean_abs > occ_max[al_i]) {
            occ_max[al_i] = mean_abs;
          }
        }
      }

      // ---- Gate 1 (cheapest, runs first): raw pre-decode LLR energy. Unscheduled CCEs measured
      // exactly (0,0) live 2026-07-28; skips the polar decode entirely for those, not just the CFR
      // submission -- a real CPU saving alongside the false-accept reduction. Only the REJECTION is
      // conditional now; the measurement above always runs. ----
      if (cfg->energy_adapt_factor > 0.0f || cfg->energy_min > 0.0f) {
        float thresh;
        if (cfg->energy_adapt_factor > 0.0f) {
          // Until the estimate has converged, reject nothing: a not-yet-settled floor can sit far
          // above the true one and would throw away real grants during exactly the startup window
          // where the persistence gate is also still cold.
          thresh = (g_energy_nseen >= ENERGY_FLOOR_WARMUP) ? cfg->energy_adapt_factor * g_energy_floor : 0.0f;
        } else {
          thresh = cfg->energy_min;
        }
        if (mean_abs < thresh) {
          e_rx_cand_idx += n_re_cand;
          g_held_energy++;
          continue;
        }
      }

      // ---- Gate 0 (DM-RS coherence, see the setup block above this loop): survivors of the raw
      // energy gate still get checked against the ACTUAL PDCCH DM-RS sequence before a polar decode
      // is attempted for ANY UE or length at this candidate. ----
      if (dmrs_gate_active) {
        const int ai = (L == 1) ? 0 : (L == 2) ? 1 : (L == 4) ? 2 : (L == 8) ? 3 : 4;
        if (isfinite(cand_al_thresh[ai]) && cand_dmrs_score[c] < cand_al_thresh[ai]) {
          e_rx_cand_idx += n_re_cand;
          g_held_dmrs++;
          continue;
        }
      }

      /* TEMPORARY DIAGNOSTIC (2026-08-04): separate "channel estimation/equalisation is wrong" from
       * "descrambling/decode is wrong". At this point pdcch_e_rx holds the EQUALISED symbols for this
       * candidate, BEFORE any descrambling -- so if the channel estimate is good they must look like
       * QPSK: |I| ~= |Q| ~= a stable magnitude, and the per-symbol magnitude spread should be small.
       * Noise-like magnitudes here would put the fault upstream (extraction/estimation); clean QPSK
       * here would put it downstream (unscrambling/demapping/polar), which the energy gate has now
       * made worth distinguishing (real PDCCH power IS present -- 0.9% of candidates sit >3x the
       * measured noise floor). Only fires for candidates the energy gate already judged hot, and only
       * a handful of times, so it cannot flood the log or perturb timing meaningfully. */
      /* Fire ONLY on candidates the adaptive gate has judged clearly hot, and ONLY once its floor has
       * converged. The first version fired on the first 12 candidates full stop, which all land
       * during the floor warmup (where the gate passes everything) -- so it sampled cold, empty CCEs
       * and said nothing about real PDCCH. This version reproduces the intended measurement: the
       * equalised constellation of REs that actually carry a grant. */
      if (g_constdiag_left > 0 && g_energy_nseen >= ENERGY_FLOOR_WARMUP && g_energy_floor > 0.0f) {
        const c16_t *eqp = &pdcch_e_rx[e_rx_cand_idx];
        double probe_abs = 0.0;
        for (int i = 0; i < n_re_cand; i++) {
          probe_abs += (eqp[i].r < 0 ? -eqp[i].r : eqp[i].r) + (eqp[i].i < 0 ? -eqp[i].i : eqp[i].i);
        }
        probe_abs /= (double)n_re_cand;
        if (probe_abs < 5.0 * (double)g_energy_floor) {
          goto constdiag_done; // not a hot candidate -- say nothing rather than describe noise
        }
        const c16_t *eq = &pdcch_e_rx[e_rx_cand_idx];
        double sum_i = 0.0, sum_q = 0.0, sum_m = 0.0, sum_m2 = 0.0;
        for (int i = 0; i < n_re_cand; i++) {
          const double vi = (double)eq[i].r, vq = (double)eq[i].i;
          const double m  = sqrt(vi * vi + vq * vq);
          sum_i += (vi < 0 ? -vi : vi);
          sum_q += (vq < 0 ? -vq : vq);
          sum_m += m;
          sum_m2 += m * m;
        }
        const double n     = (double)n_re_cand;
        const double mean_m = sum_m / n;
        const double var_m  = (sum_m2 / n) - (mean_m * mean_m);
        /* QPSK on a good estimate: cv (magnitude coefficient of variation) is SMALL (all points on one
         * ring) and iq_bal ~= 1. Circularly-symmetric noise gives cv ~= 0.52 and iq_bal ~= 1 too, so
         * cv is the discriminator and iq_bal only catches a gross I/Q imbalance. */
        const double cv     = (mean_m > 0.0) ? sqrt(var_m > 0.0 ? var_m : 0.0) / mean_m : -1.0;
        const double iq_bal = (sum_q > 0.0) ? (sum_i / sum_q) : -1.0;
        LOG_W(PHY,
              "SENSING: CONSTDIAG L=%d cce=%u n_re=%d mean_mag=%.1f cv=%.3f iq_bal=%.3f hot=%.1fx "
              "(QPSK-on-good-estimate: cv<<0.5; circular noise: cv~0.52) s0=(%d,%d) s1=(%d,%d) s2=(%d,%d)\n",
              L, (unsigned)rel15->CCE[c], n_re_cand, mean_m, cv, iq_bal,
              probe_abs / (double)g_energy_floor,
              (int)eq[0].r, (int)eq[0].i, (int)eq[1].r, (int)eq[1].i, (int)eq[2].r, (int)eq[2].i);
        g_constdiag_left--;
      }
constdiag_done:;

      const nr_pdcch_blind_cand_task_t base_task = {
          .e_rx                = &pdcch_e_rx[e_rx_cand_idx],
          .L                   = (uint8_t)L,
          .dci_length          = dci_length,
          .bwp_size            = (uint16_t)cfg->bwp_size,
          .dmrs_typeA_position = (uint8_t)cfg->dmrs_typeA_position,
          .rnti_min            = cfg->rnti_min,
          .rnti_max            = cfg->rnti_max,
          .extract_opts        = &cfg->extract,
          .scrambling_rnti     = rel15->coreset.scrambling_rnti,
          .dmrs_scrambling_id  = rel15->coreset.pdcch_dmrs_scrambling_id,
          .frame               = proc->frame_rx,
          .slot                = proc->nr_slot_rx,
          .cce                 = rel15->CCE[c],
          .format              = NR_BLIND_DCI_FORMAT_1_1,
          .dci10_ctx           = NULL,
          .dl_auto             = cfg->dl_full_auto != 0,
      };
      if (scan_11 && !cfg->dl_full_auto
          && nof_tasks < (int)(sizeof(cand_task) / sizeof(cand_task[0]))) {
        cand_task[nof_tasks++] = base_task;
      } else if (scan_11 && cfg->dl_full_auto && n_dl_ready > 0) {
        /* One polar task per resolved UE, all through this single candidate pipeline. Exact CRC
         * bounds make the pair (CORESET,RNTI,length) operationally inseparable. Rotate UE order so
         * the fixed task cap cannot starve a later UE during a full-audit occasion. */
        const int first = (int)((abs_slot + (uint32_t)c) % (uint32_t)n_dl_ready);
        for (int k = 0; k < n_dl_ready
             && nof_tasks < (int)(sizeof(cand_task) / sizeof(cand_task[0])); ++k) {
          const int ri = (first + k) % n_dl_ready;
          cand_task[nof_tasks] = base_task;
          cand_task[nof_tasks].dci_length = dl_ready_len[ri];
          cand_task[nof_tasks].rnti_min = dl_ready_rnti[ri];
          cand_task[nof_tasks].rnti_max = dl_ready_rnti[ri];
          cand_task[nof_tasks].alternate_scrambling_rnti = c == 0 ? dl_ready_rnti[ri] : 0;
          nof_tasks++;
        }
        /* MULTI-RNTI (2026-09-23): the exact-bound tasks above can only ever decode UEs already resolved,
         * so a second UE on the same CORESET was invisible forever (live s3live: gNB scheduled 0x4626
         * 2033 times, receiver saw it 0 times). One OPEN-range task per distinct resolved length lets a new
         * RNTI be recovered from the CRC; it then goes through the same persistence + bootstrap
         * confirmation as any other before a grant is emitted. Known RNTIs are skipped in its results. */
        for (int ri = 0; ri < n_dl_ready && nof_tasks < (int)(sizeof(cand_task) / sizeof(cand_task[0])); ++ri) {
          bool dup = false;
          for (int rj = 0; rj < ri; ++rj) dup |= dl_ready_len[rj] == dl_ready_len[ri];
          if (dup) continue;
          cand_task[nof_tasks] = base_task;
          cand_task[nof_tasks].dci_length = dl_ready_len[ri];
          cand_task[nof_tasks].open_rnti = 1;
          nof_tasks++;
        }
      }
      /* Other resolved BWPs: their own length, RIV width and indicator width. ponytail: extracted with
       * the manual layout (cfg->extract); the V2 layout sweep is not run per BWP. */
      for (int bi = 1; scan_11 && pbwp_on && bi < pbwp_n; bi++) {
        if (pbwp_snap[bi].start < 0 || nof_tasks >= (int)(sizeof(cand_task) / sizeof(cand_task[0])))
          continue;
        cand_task[nof_tasks] = base_task;
        cand_task[nof_tasks].dci_length   = pbwp_snap[bi].len;
        cand_task[nof_tasks].bwp_size     = pbwp_snap[bi].size;
        cand_task[nof_tasks].extract_opts = &g_pbwp_opts[bi];
        cand_task[nof_tasks].dl_auto      = false;
        cand_task[nof_tasks].bwp_entry    = (int8_t)bi;
        nof_tasks++;
      }
      if (scan_11 && pbwp_on && pbwp_probe_len && nof_tasks < (int)(sizeof(cand_task) / sizeof(cand_task[0]))) {
        cand_task[nof_tasks] = base_task;
        cand_task[nof_tasks].dci_length = pbwp_probe_len;
        cand_task[nof_tasks].bwp_probe  = 1;
        cand_task[nof_tasks].bwp_entry  = (int8_t)pbwp_probe_entry;
        nof_tasks++;
      }
      if (scan_10 && nof_tasks < (int)(sizeof(cand_task) / sizeof(cand_task[0]))) {
        cand_task[nof_tasks]            = base_task;
        cand_task[nof_tasks].dci_length = dci10_length;
        cand_task[nof_tasks].format     = NR_BLIND_DCI_FORMAT_1_0;
        cand_task[nof_tasks].dci10_ctx  = &dci10_ctx;
        /* C-RNTI-dependent scrambling is attempted after CRC recurrence identifies the RNTI. */
        nof_tasks++;
      }
      if (scan_01 && !cfg->dl_full_auto && nof_tasks < (int)(sizeof(cand_task) / sizeof(cand_task[0]))) {
        cand_task[nof_tasks]            = base_task;
        cand_task[nof_tasks].dci_length = dci01_length;
        cand_task[nof_tasks].ul_scan    = 1;
        cand_task[nof_tasks].ul_opts    = &ul_opts;
        cand_task[nof_tasks].ul_auto    = 0;
        nof_tasks++;
      }
      for (int ri = 0; scan_01 && cfg->dl_full_auto && ri < n_ul_ready
           && nof_tasks < (int)(sizeof(cand_task) / sizeof(cand_task[0])); ++ri) {
        cand_task[nof_tasks]            = base_task;
        cand_task[nof_tasks].dci_length = ul_ready_len[ri];
        cand_task[nof_tasks].ul_scan    = 1;
        cand_task[nof_tasks].ul_opts    = &ul_opts;
        cand_task[nof_tasks].ul_auto    = 1;
        cand_task[nof_tasks].rnti_min   = ul_ready_rnti[ri];
        cand_task[nof_tasks].rnti_max   = ul_ready_rnti[ri];
        cand_task[nof_tasks].alternate_scrambling_rnti = c == 0 ? ul_ready_rnti[ri] : 0;
        nof_tasks++;
      }
      e_rx_cand_idx += n_re_cand;
      g_candidates_run++;
    }
  }

  /* ---- Passive BWP: second monitoring pass over the CORESET discovered for a dedicated BWP. Same
   * PDCCH pipeline (LLRs, demapping, the same scan tasks and result loop); only the CORESET geometry
   * differs, and only the lengths of discovered BWPs are tried there (the resolved ones + the probe).
   * DM-RS reference = the voted reference RB: the BWP start on OAI, 0 (CRB 0) per 38.211. ---- */
  static __thread c16_t s_pdcch_e_rx2[NR_MAX_PDCCH_SIZE];
  if (pbwp_on && cs_have && scan_11 && (pbwp_n > 1 || pbwp_probe_len) && cs_n >= 6
      && cs_n <= NR_PDCCH_BLIND_MAX_CORESET_RB && cs_dur <= NR_PDCCH_BLIND_MAX_CORESET_DURATION) {
    nr_phy_data_t phy_b = local_phy_data;
    fapi_nr_dl_config_dci_dl_pdu_rel15_t *rb = &phy_b.phy_pdcch_config.pdcch_config[0];
    rb->BWPStart = (uint16_t)cs_ref;
    rb->coreset.rb_offset = (uint16_t)(cs_start - cs_ref);
    rb->coreset.duration = (uint8_t)cs_dur;
    build_coreset_bitmap(cs_n / 6, rb->coreset.frequency_domain_resource);
    const int ncce_b = cs_n * cs_dur / 6;
    static const int al_b[4] = {2, 4, 1, 8};
    int nc_b = 0, re_b = 0;
    for (int oi = 0; oi < 4; oi++) {
      const int L = al_b[oi], need = NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * L * 6;
      for (int cce = 0; cce + L <= ncce_b && nc_b < 48 && re_b + need <= NR_MAX_PDCCH_SIZE; cce += L) {
        rb->CCE[nc_b] = (uint16_t)cce;
        rb->L[nc_b] = (uint8_t)L;
        nc_b++;
        re_b += need;
      }
    }
    rb->number_of_candidates = (uint8_t)nc_b;
    const int llr_sym_b = cs_n * NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS;
    c16_t pdcch_llr_b[1][1][NR_PDCCH_BLIND_MAX_CORESET_RB * NR_PDCCH_BLIND_MAX_CORESET_DURATION
                           * NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS];
    for (int symbol = cfg->ss_first_symbol; symbol < cfg->ss_first_symbol + cs_dur; symbol++) {
      if (symbol >= cfg->ss_first_symbol + rel15->coreset.duration)
        nr_slot_fep(ue, fp, proc->nr_slot_rx, symbol, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
      __attribute__((aligned(32))) c16_t rxdataF_symb[fp->nb_antennas_rx][((fp->ofdm_symbol_size + 7) / 8) * 8];
      for (int ant = 0; ant < fp->nb_antennas_rx; ant++)
        memcpy(rxdataF_symb[ant], &rxdataF[ant][symbol * fp->ofdm_symbol_size], sizeof(c16_t) * fp->ofdm_symbol_size);
      nr_pdcch_generate_llr(ue, proc, symbol, &phy_b, llr_sym_b, num_monitoring_occ, rb->coreset.duration,
                            rxdataF_symb, pdcch_llr_b);
    }
    nr_pdcch_demapping_deinterleaving((uint32_t)cs_n, pdcch_llr_b[0][0], s_pdcch_e_rx2, rb->coreset.duration,
                                      rb->coreset.RegBundleSize, rb->coreset.InterleaverSize, rb->coreset.ShiftIndex,
                                      rb->number_of_candidates, rb->CCE, rb->L, llr_sym_b);
    const int cap = (int)(sizeof(cand_task) / sizeof(cand_task[0]));
    int idx2 = 0;
    for (int c = 0; c < nc_b; c++) {
      const nr_pdcch_blind_cand_task_t t2 = {
          .e_rx = &s_pdcch_e_rx2[idx2],
          .L = rb->L[c],
          .dmrs_typeA_position = (uint8_t)cfg->dmrs_typeA_position,
          .rnti_min = cfg->rnti_min,
          .rnti_max = cfg->rnti_max,
          .scrambling_rnti = rb->coreset.scrambling_rnti,
          .dmrs_scrambling_id = rb->coreset.pdcch_dmrs_scrambling_id,
          .frame = proc->frame_rx,
          .slot = proc->nr_slot_rx,
          .cce = rb->CCE[c],
          .format = NR_BLIND_DCI_FORMAT_1_1,
          .dl_auto = false,
      };
      for (int bi = 1; bi < pbwp_n && nof_tasks < cap; bi++) {
        if (pbwp_snap[bi].start < 0)
          continue;
        cand_task[nof_tasks] = t2;
        cand_task[nof_tasks].dci_length = pbwp_snap[bi].len;
        cand_task[nof_tasks].bwp_size = pbwp_snap[bi].size;
        cand_task[nof_tasks].extract_opts = &g_pbwp_opts[bi];
        cand_task[nof_tasks].bwp_entry = (int8_t)bi;
        nof_tasks++;
      }
      if (pbwp_probe_len && nof_tasks < cap) {
        cand_task[nof_tasks] = t2;
        cand_task[nof_tasks].dci_length = pbwp_probe_len;
        cand_task[nof_tasks].bwp_probe = 1;
        cand_task[nof_tasks].bwp_entry = (int8_t)pbwp_probe_entry;
        nof_tasks++;
      }
      idx2 += NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * rb->L[c] * 6;
    }
  }

  /* ---- Lookahead lanes (nr_pdcch_blind_monitor.h): K-1 ADDITIONAL (extent, mapping) hypotheses
   * tested this same occasion, reusing the rxdataF this occasion already FEP'd above (every lane
   * shares g_cfg's coreset_duration, so the "only re-FEP a symbol beyond what's already covered"
   * guard below never fires -- mirrors the existing Passive-BWP second-pass block just above, which
   * established that pattern for a different geometry source). AL2-only (this cell's own dedicated
   * SS never schedules anything else -- see CLAUDE.md's "Multi-AL scanning" note): a lookahead
   * lane's job is finding the right GEOMETRY, not a complete scan of an unconfirmed one. Off by
   * default (ISAC_PDCCH_EXTENT_BATCH unset or 1). ---- */
  const int lookahead_k = (cfg->autodiscover && nr_pdcch_blind_monitor_autodiscover_done()
                                && !nr_pdcch_blind_monitor_autodiscover_extent_verified())
                               ? nr_pdcch_blind_lookahead_count() : 0;
  /* Per-lane candidate REs must survive until Phase 1's decode loop runs, much later in this same
   * occasion -- a stack array scoped to one loop iteration would leave cand_task[].e_rx dangling by
   * the time it's read. __thread (not a plain static) so two occasions running concurrently on
   * different consumer threads never share one buffer -- same convention this file's own pbwp
   * second-pass block already uses for s_pdcch_e_rx2. pdcch_llr_lane, by contrast, is fully
   * written-then-read within one lane's own iteration, so a single reused buffer is enough. */
  /* Was [.. * 2 * 45] = 810 REs/lane, i.e. ~7 AL2 candidates and only ONE AL8 -- sized when lanes
   * were AL2-only. A mixed-AL scan needs room for several positions at each level, so this is now
   * 16 AL8-equivalents and lives on the HEAP: as a __thread array it would be 127 * 6912 * 4 B =
   * 3.5 MB of TLS, the same shape as the large-TLS AVX alignment fault this project already hit. */
  static __thread c16_t *s_lane_re = NULL;
  if (s_lane_re == NULL) {
    void *m = NULL;
    if (posix_memalign(&m, 32, sizeof(c16_t) * (size_t)NR_PDCCH_LOOKAHEAD_MAX * LANE_RE_PER_LANE) != 0)
      return;
    s_lane_re = (c16_t *)m;
  }
  c16_t (*s_pdcch_e_rx_lane)[LANE_RE_PER_LANE] = (c16_t (*)[LANE_RE_PER_LANE])s_lane_re;
  static __thread c16_t pdcch_llr_lane[1][1][NR_PDCCH_BLIND_MAX_CORESET_RB * NR_PDCCH_BLIND_MAX_CORESET_DURATION
                                             * NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS];
  /* LLR CACHE, per occasion. nr_pdcch_generate_llr() depends on the frequency EXTENT (rb_offset and
   * the RB count implied by freq_domain) and the duration -- NOT on RegBundleSize, InterleaverSize
   * or ShiftIndex, which only nr_pdcch_demapping_deinterleaving() consumes. The catalogue walks all
   * 271 mapping variations WITHIN one extent, so consecutive lanes share the extent and each was
   * redoing the FEP, a full-symbol memcpy per antenna, and the channel estimation inside
   * generate_llr -- for bit-identical output.
   *
   * That repeated work IS the cost: polar decode is 4.3 us of a 208 us occasion (2 %, measured), so
   * ~98 % of an occasion is what is being duplicated K times. Removing it is what makes K > 16
   * lanes affordable on CPU, and it is also the precondition for a GPU version to be worth doing --
   * only once this is gone does the decode become the dominant term.
   *
   * Scoped to ONE occasion deliberately: these LLRs derive from this slot's samples, so the key is
   * reset on every call and never carried across occasions. */
  int llr_cache_rb = -1, llr_cache_off = -1;
  lane_batch_reset();   /* LANE BATCH is per-occasion: these LLRs belong to this slot only */

  /* UNION LLR: one FEP + channel estimate + LLR per occasion instead of one per lane.
   * lane_advance() puts every lane on its OWN extent (ext_idx + k + 1), so the per-lane LLR cache
   * below missed on every lane and nr_pdcch_generate_llr() ran K times per slot. For a dedicated
   * CORESET the DM-RS is absolute-indexed (dci_nr.c: pilots generated from BWPStart, RB n's pilot
   * does not depend on which extent contains it), so an LLR over the union of all lanes' extents
   * is the same per-RB quantity and each lane takes its slice. The one thing that is NOT identical:
   * the channel-estimation filter at an extent's first/last RB sees neighbours in the union that it
   * would not see in the sub-extent. ISAC_LANE_UNION_LLR=0 restores the per-lane path for an A/B. */
  static int s_union_llr = -1;
  if (s_union_llr < 0) { const char *e = getenv("ISAC_LANE_UNION_LLR"); s_union_llr = (e == NULL || atoi(e) != 0) ? 1 : 0; }
  static __thread c16_t pdcch_llr_union[1][1][NR_PDCCH_BLIND_MAX_CORESET_RB * NR_PDCCH_BLIND_MAX_CORESET_DURATION
                                              * NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS];
  int u_lo = 0, u_rb = 0;         /* union window: first RB and RB count; u_rb == 0 -> not built */
  if (s_union_llr) {
    int lo = INT_MAX, hi = -1;
    for (int lane = 0; lane < lookahead_k; lane++) {
      nr_pdcch_lookahead_geom_t g;
      if (!nr_pdcch_blind_lookahead_get(lane, &g))
        continue;
      if (g.rb_offset < lo) lo = g.rb_offset;
      if (g.rb_offset + g.freq_domain * 6 > hi) hi = g.rb_offset + g.freq_domain * 6;
    }
    if (hi > lo && (hi - lo) <= NR_PDCCH_BLIND_MAX_CORESET_RB && ((hi - lo) % 6) == 0) {
      nr_phy_data_t phy_u;
      memset(&phy_u, 0, sizeof(phy_u));
      phy_u.phy_pdcch_config.nb_search_space = 1;
      fapi_nr_dl_config_dci_dl_pdu_rel15_t *urel = &phy_u.phy_pdcch_config.pdcch_config[0];
      urel->coreset.CoreSetType   = rel15->coreset.CoreSetType;
      urel->coreset.rb_offset     = (uint16_t)lo;
      urel->coreset.duration      = rel15->coreset.duration;
      build_coreset_bitmap((hi - lo) / 6, urel->coreset.frequency_domain_resource);
      urel->coreset.CceRegMappingType        = FAPI_NR_CCE_REG_MAPPING_TYPE_NON_INTERLEAVED; /* LLR is mapping-independent */
      urel->coreset.pdcch_dmrs_scrambling_id = cfg->coreset_pdcch_dmrs_scrambling_id;
      urel->coreset.scrambling_rnti          = (cfg->dci10_ss_type == NR_BLIND_SS_UE_SPECIFIC && cfg->coreset_type != 1
                                              ? nr_pdcch_nrnti_override(abs_slot) : 0); /* USS on a PDCCH-Config CORESET only */
      urel->coreset.StartSymbolBitmap        = rel15->coreset.StartSymbolBitmap;
      const int u_sym = (hi - lo) * NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS;
      if ((size_t)(urel->coreset.duration * u_sym) <= sizeof(pdcch_llr_union[0][0]) / sizeof(c16_t)) {
        for (int symbol = cfg->ss_first_symbol; symbol < cfg->ss_first_symbol + urel->coreset.duration; symbol++) {
          if (symbol >= cfg->ss_first_symbol + rel15->coreset.duration)
            nr_slot_fep(ue, fp, proc->nr_slot_rx, symbol, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
          __attribute__((aligned(32))) c16_t rxdataF_symb_u[fp->nb_antennas_rx][((fp->ofdm_symbol_size + 7) / 8) * 8];
          for (int ant = 0; ant < fp->nb_antennas_rx; ant++)
            memcpy(rxdataF_symb_u[ant], &rxdataF[ant][symbol * fp->ofdm_symbol_size], sizeof(c16_t) * fp->ofdm_symbol_size);
          nr_pdcch_generate_llr(ue, proc, symbol, &phy_u, u_sym, num_monitoring_occ, urel->coreset.duration,
                                rxdataF_symb_u, pdcch_llr_union);
        }
        u_lo = lo;
        u_rb = hi - lo;
      }
    }
  }

  for (int lane = 0; lane < lookahead_k; lane++) {
    nr_pdcch_lookahead_geom_t geom;
    if (!nr_pdcch_blind_lookahead_get(lane, &geom))
      continue;
    if (coreset_bank_covers(geom.rb_offset, geom.freq_domain * 6, cfg->coreset_duration,
                            cfg->ss_first_symbol, geom.reg_bundle_size, geom.interleaver_size,
                            geom.shift_index, cfg->coreset_pdcch_dmrs_scrambling_id)) {
      nr_pdcch_blind_lookahead_retry(lane);
      continue;
    }
    if (memcmp(&geom, &g_lane_last_geom[lane], sizeof(geom)) != 0) {
      g_lane_last_geom[lane] = geom;
      nr_pdcch_dci_length_sweep_reset(&g_lane_length_state[lane]);
      /* SIB1 length seed: DCI 1_0 in a UE-specific search space is sized on the ACTIVE DL BWP, and
       * SIB1's initialDownlinkBWP is that BWP until RRC says otherwise -- so its 1_0 length is
       * KNOWN, not swept. Tried first for NR_PDCCH_LENGTH_PREFERRED_ROUNDS rounds, then the full
       * 30..63 sweep as before (the 1_1 length still depends on the dedicated config). */
      {
        const int bank_len = geom.fast_length_only ? coreset_bank_length_hint() : 0;
        if (bank_len >= dci_len_min() && bank_len <= dci_len_max())
          g_lane_length_state[lane].preferred_len = bank_len;
        const nr_pdcch_sib1_prior_t *pr = nr_pdcch_sib1_prior_get();
        if (g_lane_length_state[lane].preferred_len == 0
            && pr != NULL && pr->dl_bwp_valid && pr->dl_bwp_size > 0) {
          const int seed = (int)nr_pdcch_blind_dci10_size(pr->dl_bwp_size);
          if (seed >= dci_len_min() && seed <= dci_len_max())
            g_lane_length_state[lane].preferred_len = seed;
        }
      }
      g_lane_length_swept[lane] = false;
      g_lane_length_found[lane] = false;
      g_lane_length_rnti[lane] = 0;
    }

    nr_phy_data_t phy_lane;
    memset(&phy_lane, 0, sizeof(phy_lane));
    phy_lane.phy_pdcch_config.nb_search_space = 1;
    fapi_nr_dl_config_dci_dl_pdu_rel15_t *lrel = &phy_lane.phy_pdcch_config.pdcch_config[0];
    lrel->coreset.CoreSetType   = rel15->coreset.CoreSetType;
    lrel->coreset.rb_offset     = (uint16_t)geom.rb_offset;
    lrel->coreset.duration      = rel15->coreset.duration; // every lane shares g_cfg's duration
    build_coreset_bitmap(geom.freq_domain, lrel->coreset.frequency_domain_resource);
    lrel->coreset.CceRegMappingType = (geom.reg_bundle_size != 0)
                                          ? FAPI_NR_CCE_REG_MAPPING_TYPE_INTERLEAVED
                                          : FAPI_NR_CCE_REG_MAPPING_TYPE_NON_INTERLEAVED;
    lrel->coreset.RegBundleSize            = (uint8_t)geom.reg_bundle_size;
    lrel->coreset.InterleaverSize          = (uint8_t)geom.interleaver_size;
    lrel->coreset.ShiftIndex               = (uint16_t)geom.shift_index;
    lrel->coreset.pdcch_dmrs_scrambling_id = cfg->coreset_pdcch_dmrs_scrambling_id;
    lrel->coreset.scrambling_rnti          = (cfg->dci10_ss_type == NR_BLIND_SS_UE_SPECIFIC && cfg->coreset_type != 1
                                              ? nr_pdcch_nrnti_override(abs_slot) : 0); /* USS on a PDCCH-Config CORESET only */
    lrel->coreset.StartSymbolBitmap        = rel15->coreset.StartSymbolBitmap;

    int ln_rb = 0, ln_start = 0;
    get_coreset_rballoc(lrel->coreset.frequency_domain_resource, &ln_rb, &ln_start);
    if (ln_rb < 6) { // a 6-RB CORESET can carry AL1
      nr_pdcch_blind_lookahead_retry(lane);
      continue;
    }
    const int ln_num_cces = (ln_rb * lrel->coreset.duration) / 6;
    /* Every configured aggregation level, non-overlapping CCE positions at each. The RE budget is
     * tracked as a RUNNING TOTAL rather than (count * fixed_need): with mixed ALs the candidates no
     * longer have equal width, so the old uniform-stride check would under-count an AL8 entry and
     * overrun s_pdcch_e_rx_lane[]. This mirrors the disc_cand walk below, which already advances
     * its cursor by each candidate's own L. */
    const uint8_t *ln_als_cfg = NULL;
    const int ln_nal_all = lane_als(&ln_als_cfg);
    uint8_t ln_als_ord[5];
    const uint8_t *ln_als = ln_als_cfg;
    {
      const nr_pdcch_sib1_prior_t *pr = nr_pdcch_sib1_prior_get();
      if (pr != NULL && pr->ss_valid && ln_nal_all > 1) {
        int n = 0;
        for (int pass = 0; pass < 2; pass++)           /* pass 0: ALs the SIB1 CSS monitors; pass 1: the rest */
          for (int ai = 0; ai < ln_nal_all; ai++) {
            int idx = 0;
            while ((1 << idx) < ln_als_cfg[ai]) idx++;   /* AL -> al_candidates index (1,2,4,8,16 -> 0..4) */
            const bool known = (idx < NR_SIB1_PRIOR_NUM_AL) && pr->al_candidates[idx] > 0;
            if ((pass == 0) == known) ln_als_ord[n++] = ln_als_cfg[ai];
          }
        ln_als = ln_als_ord;
      }
    }
    /* One AL per visit. Decoding every AL at every geometry multiplied the lane-batch prepass by
     * four and measured >1 ms. The geometry dwell below is extended to two complete rotations, so
     * this changes scheduling latency only; every configured AL remains covered. Lane staggering
     * prevents all lanes from testing the same AL in an occasion. */
    const uint64_t lane_visit = rank_lane_visit[lane]++;
    uint8_t ln_al_active = ln_als[(lane_visit + (uint64_t)lane) % (uint64_t)ln_nal_all];
    ln_als = &ln_al_active;
    const int ln_nal = 1;
    const int ln_cap_re = (int)(sizeof(s_pdcch_e_rx_lane[lane]) / sizeof(s_pdcch_e_rx_lane[0][0]));
    int ln_nc = 0, ln_used_re = 0;
    /* Form every legal non-overlapping position before applying the extraction-buffer budget.
     * The previous position-major truncation silently removed high CCEs on wide mixed-AL
     * geometries before DMRS could score them. A verified RAR identity now contributes the exact
     * 38.213 USS hash prior; DMRS contributes current-slot occupancy. One rotating candidate per AL
     * gets the largest priority, so a bad prior or a quiet grant can delay but never permanently
     * exclude any CCE. */
    uint16_t all_cce[256], all_support[256] = {0};
    uint8_t all_al[256], all_order[256];
    double all_score[256] = {0};
    int all_n = 0;
    for (int pos = 0; all_n < 256; ++pos) {
      bool any = false;
      for (int ai = 0; ai < ln_nal && all_n < 256; ++ai) {
        const int L = (int)ln_als[ai];
        const int npos = ln_num_cces / L;
        if (pos >= npos)
          continue;
        all_cce[all_n] = (uint16_t)(pos * L);
        all_al[all_n] = (uint8_t)L;
        all_order[all_n] = (uint8_t)all_n;
        ++all_n;
        any = true;
      }
      if (!any)
        break;
    }
    uint16_t verified[NR_PDCCH_BLIND_MAX_UE];
    const int n_verified =
        nr_pdcch_blind_monitor_verified_rnti_set(abs_slot, verified, NR_PDCCH_BLIND_MAX_UE);
    if (n_verified > 0)
      nr_pdcch_uss_candidate_supports(ln_num_cces, proc->nr_slot_rx, verified, n_verified,
                                      all_cce, all_al, all_n, all_support);
    const bool prioritize = rank_grid.n_rb || n_verified > 0;
    if (prioritize) {
      const double prior_den = n_verified > 0 ? 21.0 * n_verified : 1.0;
      for (int i = 0; i < all_n; ++i) {
        const double d = rank_grid.n_rb
            ? nr_pdcch_dmrs_candidate_score(&rank_grid, geom.rb_offset, ln_rb,
                                            lrel->coreset.RegBundleSize,
                                            lrel->coreset.InterleaverSize,
                                            lrel->coreset.ShiftIndex, all_cce[i], all_al[i])
            : 0.0;
        all_score[i] = (isfinite(d) ? d : 0.0) + 2.0 * (double)all_support[i] / prior_den;
      }
      /* Reserve one cyclic exploration point per AL. Its bonus only changes scheduling order. */
      for (int ai = 0; ai < ln_nal; ++ai) {
        int count = 0;
        for (int i = 0; i < all_n; ++i)
          if (all_al[i] == ln_als[ai])
            ++count;
        if (count == 0)
          continue;
        int target = (int)(rank_lane_visit[lane] % (uint64_t)count);
        for (int i = 0; i < all_n; ++i)
          if (all_al[i] == ln_als[ai] && target-- == 0) {
            all_score[i] += 4.0;
            break;
          }
      }
      for (int i = 1; i < all_n; ++i) {
        const uint8_t v = all_order[i];
        int j = i;
        while (j > 0 && all_score[v] > all_score[all_order[j - 1]]) {
          all_order[j] = all_order[j - 1];
          --j;
        }
        all_order[j] = v;
      }
    }
    for (int oi = 0; oi < all_n && ln_nc < 45; ++oi) {
      const int i = all_order[oi];
      const int need = NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * all_al[i] * 6;
      if (ln_used_re + need > ln_cap_re)
        continue;
      lrel->CCE[ln_nc] = all_cce[i];
      lrel->L[ln_nc] = all_al[i];
      ++ln_nc;
      ln_used_re += need;
    }
    if (ln_nc < 1) {
      nr_pdcch_blind_lookahead_retry(lane);
      continue;
    }
    lrel->number_of_candidates = (uint8_t)ln_nc;
    if (rank_grid.n_rb) {
      blind_dmrs_rank_candidates(&rank_grid, lrel, ln_rb, geom.rb_offset,
                                  proc->nr_slot_rx, cfg->ss_first_symbol, abs_slot, lane_visit);
      ln_nc = lrel->number_of_candidates;
    }

    const int ln_llr_sym = ln_rb * NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS;
    if ((size_t)(lrel->coreset.duration * ln_llr_sym) > sizeof(pdcch_llr[0][0]) / sizeof(c16_t)) {
      nr_pdcch_blind_lookahead_step(lane);
      continue;
    }
    const bool in_union = (u_rb > 0) && ((int)lrel->coreset.rb_offset >= u_lo)
                          && ((int)lrel->coreset.rb_offset + ln_rb <= u_lo + u_rb);
    if (in_union) {
      /* UNION LLR slice: row r of the lane buffer = row r of the union buffer, offset by the lane's
       * RB distance from the union start. Same [symbol][RB*9] layout on both sides. */
      const int u_sym = u_rb * NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS;
      const int u_off = ((int)lrel->coreset.rb_offset - u_lo) * NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS;
      for (int r = 0; r < lrel->coreset.duration; r++)
        memcpy(&pdcch_llr_lane[0][0][r * ln_llr_sym], &pdcch_llr_union[0][0][r * u_sym + u_off],
               (size_t)ln_llr_sym * sizeof(c16_t));
      llr_cache_rb = -1;  /* the per-lane cache no longer describes pdcch_llr_lane */
    } else
    if (ln_rb != llr_cache_rb || (int)lrel->coreset.rb_offset != llr_cache_off) {
    llr_cache_rb  = ln_rb;
    llr_cache_off = (int)lrel->coreset.rb_offset;
    for (int symbol = cfg->ss_first_symbol; symbol < cfg->ss_first_symbol + lrel->coreset.duration; symbol++) {
      if (symbol >= cfg->ss_first_symbol + rel15->coreset.duration)
        nr_slot_fep(ue, fp, proc->nr_slot_rx, symbol, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
      __attribute__((aligned(32))) c16_t rxdataF_symb_lane[fp->nb_antennas_rx][((fp->ofdm_symbol_size + 7) / 8) * 8];
      for (int ant = 0; ant < fp->nb_antennas_rx; ant++)
        memcpy(rxdataF_symb_lane[ant], &rxdataF[ant][symbol * fp->ofdm_symbol_size], sizeof(c16_t) * fp->ofdm_symbol_size);
      nr_pdcch_generate_llr(ue, proc, symbol, &phy_lane, ln_llr_sym, num_monitoring_occ, lrel->coreset.duration,
                            rxdataF_symb_lane, pdcch_llr_lane);
    }
    }  /* end LLR CACHE guard: reuse pdcch_llr_lane when the extent is unchanged */
    nr_pdcch_demapping_deinterleaving((uint32_t)ln_rb, pdcch_llr_lane[0][0], s_pdcch_e_rx_lane[lane],
                                      lrel->coreset.duration, lrel->coreset.RegBundleSize,
                                      lrel->coreset.InterleaverSize, lrel->coreset.ShiftIndex,
                                      lrel->number_of_candidates, lrel->CCE, lrel->L, ln_llr_sym);

    if (!g_lane_length_found[lane]) {
      nr_pdcch_autodiscover_cand_t disc_cand[45];
      int disc_n = 0, idx = 0;
      for (int c = 0; c < ln_nc && disc_n < 45; c++) {
        disc_cand[disc_n].e_rx = &s_pdcch_e_rx_lane[lane][idx];
        disc_cand[disc_n].L    = lrel->L[c];
        disc_cand[disc_n].cce = lrel->CCE[c];
        disc_n++;
        idx += NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * lrel->L[c] * 6;
      }
      if (disc_n > 0) {
        /* PHASE A: record this lane's grid and DEFER the sweep. One device call covers every lane
         * (see LANE BATCH): per-lane calls cost ~90 us each and dominated the occasion at K=96.
         * nr_pdcch_blind_lookahead_step() is deferred with it -- it advances the lane's geometry
         * cursor, so stepping before the sweep would attribute a lock to the wrong geometry. */
        memcpy(g_lane_disc_cand[lane], disc_cand, (size_t)disc_n * sizeof(disc_cand[0]));
        g_lane_disc_n[lane] = disc_n;
        g_lane_sweep_ctx[lane] = (nr_pdcch_autodiscover_sweep_ctx_t){
            .cand                = g_lane_disc_cand[lane],
            .n_cand              = disc_n,
            .bwp_size            = (uint16_t)cfg->bwp_size,
            .dmrs_typeA_position = (uint8_t)cfg->dmrs_typeA_position,
            .rnti_min            = cfg->rnti_min,
            .rnti_max            = cfg->rnti_max,
            .extract_opts        = &cfg->extract,
            .scrambling_rnti     = lrel->coreset.scrambling_rnti,
            .dmrs_scrambling_id  = lrel->coreset.pdcch_dmrs_scrambling_id,
            .lane                = lane,
            .rb_offset           = geom.rb_offset,
            .span_rb             = geom.freq_domain * 6,
            .bundle              = geom.reg_bundle_size,
            .interleaver         = geom.interleaver_size,
            .shift               = geom.shift_index,
        };
        g_lane_geom_snap[lane]   = geom;
        g_lane_needs_sweep[lane] = 1;
        if (!budget_active)
          lane_batch_add(lane, &g_lane_sweep_ctx[lane], dci_len_min(), dci_len_max());
        continue;   /* phase B runs the anchored sweep AND the step for this lane */
      }
    } else {
      const int cap = (int)(sizeof(cand_task) / sizeof(cand_task[0]));
      int idx = 0;
      for (int c = 0; c < ln_nc && nof_tasks < cap; c++) {
        cand_task[nof_tasks] = (nr_pdcch_blind_cand_task_t){
            .e_rx               = &s_pdcch_e_rx_lane[lane][idx],
            .L                  = lrel->L[c],
            .dci_length         = g_lane_dci_length[lane],
            .rnti_min           = g_lane_length_rnti[lane] ? g_lane_length_rnti[lane] : cfg->rnti_min,
            .rnti_max           = g_lane_length_rnti[lane] ? g_lane_length_rnti[lane] : cfg->rnti_max,
            .scrambling_rnti    = lrel->coreset.scrambling_rnti,
            .alternate_scrambling_rnti = c == 0 ? g_lane_length_rnti[lane] : 0,
            .dmrs_scrambling_id = lrel->coreset.pdcch_dmrs_scrambling_id,
            .frame              = proc->frame_rx,
            .slot               = proc->nr_slot_rx,
            .cce                = lrel->CCE[c],
            .format             = NR_BLIND_DCI_FORMAT_1_1,
            .dl_auto            = true, // raw decode (nr_pdcch_blind_decode_raw_11), same as the primary
            .is_lookahead       = true,
            .lookahead_lane     = (int8_t)lane,
        };
        nof_tasks++;
        idx += NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * lrel->L[c] * 6;
      }
    }
    nr_pdcch_blind_lookahead_step(lane);
  }

  /* ---- PHASE B: one device call for every lane, then each lane's sweep against the cache -------
   * Splitting the loop is what makes the single call possible: phase A needed every lane's
   * candidate list to exist before the batch could be assembled. Lanes that did not fit the batch
   * are not special-cased -- the scorer simply misses and decodes them on the CPU exactly as
   * before, so correctness never depends on the batch succeeding. */
  if (!budget_active) lane_batch_flush();
  for (int lane = 0; lane < lookahead_k; lane++) {
    if (!g_lane_needs_sweep[lane])
      continue;
    g_lane_needs_sweep[lane] = 0;
    const int disc_n = g_lane_disc_n[lane];
    if (disc_n <= 0) {
      nr_pdcch_blind_lookahead_step(lane);
      continue;
    }
    uint16_t lane_boot = 0;
    nr_pdcch_autodiscover_sweep_ctx_t *lane_ctx = &g_lane_sweep_ctx[lane];
    uint16_t lane_known[NR_PDCCH_BLIND_MAX_UE];
    int lane_n_known = nr_pdcch_blind_monitor_dedicated_rnti_set(
        abs_slot, lane_known, NR_PDCCH_BLIND_MAX_UE);
    if (lane_n_known > 0) {
      uint64_t pick = length_lookahead_key(cfg, &g_lane_geom_snap[lane])
                      + (uint64_t)abs_slot + (uint64_t)lane;
      pick = (pick ^ (pick >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
      pick = (pick ^ (pick >> 27)) * UINT64_C(0x94d049bb133111eb);
      pick ^= pick >> 31;
      lane_boot = lane_known[pick % (uint64_t)lane_n_known];
      lane_ctx->n_known = 1;
      lane_ctx->known_rnti[0] = lane_boot;
      lane_ctx->bootstrap_alias = lane_boot;
      lane_ctx->rnti_min = lane_boot;
      lane_ctx->rnti_max = lane_boot;
    } else {
      lane_ctx->n_known = 0;
      lane_ctx->bootstrap_alias = 0;
    }

    pthread_mutex_lock(&g_dl_length_lock);
    const uint64_t lane_geom_key = length_lookahead_key(cfg, &g_lane_geom_snap[lane]);
    nr_pdcch_dci_length_bank_t *lane_bank =
        nr_pdcch_dci_length_store_get(&g_dl_length_store, lane_geom_key, NULL);
    nr_pdcch_dci_length_context_t *lane_len_ctx =
        lane_boot ? nr_pdcch_dci_length_context(lane_bank, lane_geom_key, lane_boot) : NULL;
    const bool lane_anonymous = !lane_boot && lane_bank && !lane_bank->anonymous_exhausted;
    nr_pdcch_dci_length_sweep_state_t *lane_state =
        lane_len_ctx ? &lane_len_ctx->state
                     : (lane_anonymous ? &lane_bank->anonymous : &g_lane_length_state[lane]);
    if (!lane_boot && !lane_anonymous) {
      pthread_mutex_unlock(&g_dl_length_lock);
      nr_pdcch_blind_lookahead_retry(lane);
      nr_pdcch_blind_lookahead_step(lane);
      continue;
    }
    if (lane_state->preferred_len == 0
        && g_lane_length_state[lane].preferred_len >= dci_len_min()
        && g_lane_length_state[lane].preferred_len <= dci_len_max())
      lane_state->preferred_len = g_lane_length_state[lane].preferred_len;
    lane_state->excluded_len = dci10_length;
    lane_state->stride = dci_sweep_stride();

    /* Only one owner spends discovery budget per occasion. Anonymous and anchored evidence use
     * the same bounded scorer; a fast one-length prior may finish without suspension. */
    const bool lane_needs_deadline = budget_active
        && (!g_lane_geom_snap[lane].fast_length_only || lane_state->preferred_len == 0);
    const uint64_t lane_deadline = lane_needs_deadline ? btim_now() + 150000ull : 0;
    int found_len = lane_len_ctx ? lane_len_ctx->found
                                 : (lane_anonymous ? lane_bank->anonymous_found : 0);
    if (found_len <= 0)
      found_len = (budget_active && budget_owner != lane + 1) ? -1 :
          nr_pdcch_dci_length_sweep_feed_budget(lane_state,
              nr_pdcch_autodiscover_length_scorer, lane_ctx, disc_n,
              dci_len_min(), dci_len_max(), lane_boot, lane_deadline, 0);
    uint16_t lane_locked_rnti = lane_boot;
    if (found_len > 0 && lane_anonymous) {
      lane_locked_rnti = nr_pdcch_dci_length_sweep_winner_rnti(lane_state, found_len);
      if (!lane_locked_rnti)
        found_len = -1;
    }
    if (found_len > 0) {
      if (lane_len_ctx && lane_len_ctx->found <= 0) {
        lane_len_ctx->found = found_len;
        nr_pdcch_dci_length_bank_converged(lane_bank, lane_locked_rnti, found_len);
      } else if (lane_anonymous) {
        lane_bank->anonymous_found = found_len;
        lane_bank->anonymous_rnti = lane_locked_rnti;
        nr_pdcch_blind_rnti_bootstrap_record_corroborated(
            lane_locked_rnti, NR_BLIND_RNTI_CLASS_C, abs_slot);
        nr_pdcch_dci_length_context_t *promoted =
            nr_pdcch_dci_length_context(lane_bank, lane_geom_key, lane_locked_rnti);
        if (promoted)
          promoted->found = found_len;
      }
      g_lane_dci_length[lane] = (uint16_t)found_len;
      g_lane_length_rnti[lane] = lane_locked_rnti;
      g_lane_length_found[lane] = true;
      g_lane_length_swept[lane] = true;
      LOG_I(PHY, "SENSING: lookahead lane %d dci_length locked at %d rnti=0x%x "
                 "(offset=%d span=%d) evidence=distinct_ota\n",
            lane, found_len, lane_locked_rnti, g_lane_geom_snap[lane].rb_offset,
            g_lane_geom_snap[lane].freq_domain * 6);
    } else if (g_lane_geom_snap[lane].fast_length_only
               && lane_state->preferred_rounds >= 2 * lane_al_count()
               && !lane_has_bootstrap_hit(lane_state)) {
      nr_pdcch_blind_lookahead_retry(lane);
    } else if (lane_state->occasions_fed >= AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS) {
      if (lane_len_ctx)
        lane_len_ctx->exhausted = true;
      else if (lane_bank)
        lane_bank->anonymous_exhausted = true;
      nr_pdcch_blind_lookahead_retry(lane);
    }
    pthread_mutex_unlock(&g_dl_length_lock);
    /* Until a length locks, only this lane's budget-owned visits add sweep evidence. Advancing on
     * every visit underexposed each geometry by (1 + K): 125 rounds instead of 500 at K=3. Once
     * locked, phase A owns the verification dwell and advances on every OTA visit. */
    if (!budget_active || budget_owner == lane + 1)
      nr_pdcch_blind_lookahead_step(lane);
  }

  // ---- Phase 1 (parallel): fan the independent unscramble+decode work out across the UE's thread
  // pool. pushTpool() runs the task inline if the pool has zero worker threads configured (its own
  // documented fallback), so this degrades to the original sequential behaviour rather than
  // breaking on a single-core/no-pool build. ----
  btim_add(BTIM_PREPASS, btim_t_pre);
  discovery_scope.after_pre=btim_on?btim_now():0;
  discovery_scope.phase=5;

  const uint64_t btim_t_dec = btim_on ? btim_now() : 0;
  if (nof_tasks > 0) {
    if (serial_candidates) {
      /* ALREADY off the PHY receive thread, so there is nothing to protect by fanning out -- and
       * fanning out here actively HURT. MEASURED, same work, same candidate count (38), same
       * binary, only the calling thread differing:
       *     BTIM decode   in-line on the RT thread : 18.4 us
       *     BTIM decode   on a scan consumer       : 271.1 us   (15x)
       * The consumer runs at priority 50 and then BLOCKS in join_task_ans() behind whatever else
       * the shared pool is serving, so the fan-out buys parallelism and pays for it in queueing.
       * At 363.5 us total per occasion against occasions arriving every 500 us, one consumer went
       * marginal and the ring evicted up to 29562 occasions per run.
       *
       * Running them serially here costs honest CPU instead of scheduling luck. The whole machine
       * is at ~1.5 of 12 cores, so there is no throughput reason to fan out -- only a latency one,
       * and latency is exactly what the deferral already bought. */
      for (int i = 0; i < nof_tasks; i++) {
        nr_pdcch_blind_cand_worker_serial(&cand_task[i]);
      }
    } else {
      task_ans_t ans;
      init_task_ans(&ans, nof_tasks);
      for (int i = 0; i < nof_tasks; i++) {
        cand_task[i].ans = &ans;
        task_t t = {.func = nr_pdcch_blind_cand_worker, .args = &cand_task[i]};
        pushTpool(&get_nrUE_params()->Tpool, t);
      }
      join_task_ans(&ans);
    }
  }
  btim_add(BTIM_DECODE, btim_t_dec);
  discovery_scope.after_decode=btim_on?btim_now():0;
  discovery_scope.phase=6;
  const uint64_t btim_t_post = btim_on ? btim_now() : 0;

  // ---- Phase 2 (sequential, in original candidate order): everything below has a genuine
  // sequential dependency (dci_thres EMA, RNTI persistence ring buffer) or is rare/expensive enough
  // (CFR submission, PDSCH decode) that parallelising it buys nothing. Unchanged from before the
  // split, just walking cand_task[] instead of decoding inline. ----
  int decodes_this_occasion = 0; // capped by cfg->pdsch_max_per_slot -- see that field's comment
  bool retired_lookahead[NR_PDCCH_LOOKAHEAD_MAX] = {false};
  for (int ti = 0; ti < nof_tasks; ti++) {
    if (cand_task[ti].is_lookahead) {
      /* Routed independently of the primary's dl_auto branch below on purpose: that branch updates
       * PRIMARY-only global state (ue->dci_thres EMA, RNTI persistence, AL census, DCI11 layout
       * stage 2) which must never see evidence from a lookahead lane's still-UNVERIFIED geometry --
       * noise from a wrong candidate mixed into those accumulators would corrupt them for everyone. */
      const int lane = cand_task[ti].lookahead_lane;
      if (lane < 0 || lane >= NR_PDCCH_LOOKAHEAD_MAX || retired_lookahead[lane])
        continue;
      if (cand_task[ti].ok && cand_task[ti].dl_raw.rnti) {
        if (coreset_bank_has_owner(cand_task[ti].dl_raw.rnti)) {
          /* This hypothesis is another decode-compatible view of an already operational UE.
           * Retire it once, immediately, so aliases cannot consume the search or the bank. */
          nr_pdcch_blind_lookahead_retry(lane);
          retired_lookahead[lane] = true;
          continue;
        }
        const long mono = source_absolute_slot;
        const bool just_verified = nr_pdcch_blind_lookahead_observe(lane,
            cand_task[ti].dl_raw.rnti, mono >= 0 ? (uint32_t)mono : abs_slot, cand_task[ti].dl_raw.payload);
        if (just_verified) {
          retired_lookahead[lane] = true;
          nr_pdcch_blind_monitor_autodiscover_set_dci_length(g_lane_dci_length[lane]);
          /* The lane already ran the same length scorer and fresh-payload verification as the
           * primary. Mark that result consumed so the newly committed geometry is not swept again. */
          g_length_swept = true;
          g_length_found = true;
          coreset_bank_add(nr_pdcch_blind_monitor_get_cfg(), cand_task[ti].dl_raw.rnti);
          nr_pdcch_blind_monitor_autodiscover_next();
        }
      }
      continue;
    }
    if (cand_task[ti].bwp_probe) {
      const nr_pdcch_blind_raw_result_t *pr = &cand_task[ti].dl_raw;
      if (cand_task[ti].ok && pr->rnti != 0) {
        const int be = cand_task[ti].bwp_entry;
        pthread_mutex_lock(&g_pbwp_lock);
        const bool proven = nr_pbwp_rnti_seen(&g_pbwp, pr->rnti);
        const int ne = (be == 0) ? nr_pbwp_probe_accept(&g_pbwp, pr->rnti, cand_task[ti].dci_length) : -1;
        const int ng = (ne > 0) ? g_pbwp.e[ne].ng : 0;
        pthread_mutex_unlock(&g_pbwp_lock);
        if (ne > 0)
          LOG_A(PHY, "SENSING: BWP NEW entry=%d len=%u from rnti 0x%x (%d indicator-width hypotheses) -- "
                     "resolving size/start from the DM-RS\n", ne, cand_task[ti].dci_length, pr->rnti, ng);
        if (be > 0 && proven && nr_pdsch_passive_queue_running()) {
          nr_pdsch_passive_job_t job;
          memset(&job, 0, sizeof(job));
          job.frame_rx = proc->frame_rx;
          job.nr_slot_rx = proc->nr_slot_rx;
          job.gNB_id = proc->gNB_id;
          job.absolute_slot = source_absolute_slot;
          job.rnti = pr->rnti;
          job.fo_hz = isnan(nr_slot_fep_fo_override_hz)
              ? (ue->cont_fo_comp ? ue->dl_Doppler_shift + ue->freq_offset : 0.0)
              : nr_slot_fep_fo_override_hz;
          job.bwp_probe_entry = (int8_t)be;
          job.bwp_probe_payload = pr->payload;
          nr_pdsch_passive_queue_enqueue(&job);
        }
      }
      continue;
    }
    /* ---- UPLINK candidates are handled here and nothing below runs for them: every line after
     * this point reads a DL result and would misinterpret a UL one. ---- */
    if (cand_task[ti].ul_scan) {
      g_ul_sched++;
      /* rnti is written only after the in-range CRC check, so non-zero IS that check. */
      if (cand_task[ti].ul_out.rnti != 0) g_ul_crc_hit++;
      if(cand_task[ti].ok && cand_task[ti].ul_auto) {
        g_ul_disc_call++;
        nr_passive_replay_ul(source_absolute_slot, cand_task[ti].rnti_min, cand_task[ti].dci_length,
                             cand_task[ti].ul_out.raw_payload);
        nr_pdcch_blind_ul_result_t discovered;
        cand_task[ti].ok=nr_pdcch_ul_discovery_grant(&ul_opts,cand_task[ti].dci_length,cand_task[ti].rnti_min,
                                                    cand_task[ti].ul_out.raw_payload,&discovered);
        if(cand_task[ti].ok) cand_task[ti].ul_out=discovered;
      }
      const nr_pdcch_blind_ul_result_t *u = &cand_task[ti].ul_out;
      if (cand_task[ti].ok && accept_dup(abs_slot, u->rnti, 1))
        cand_task[ti].ok = false; /* same UL DCI already accepted by another pass this slot */
      if (cand_task[ti].ok) {
        g_ul_accepts++;
        discovery_evidence("ul_admitted", "UL", cand_task[ti].frame, cand_task[ti].slot,
                            u->dci_length, u->rnti, u->raw_payload, cand_task[ti].L, cand_task[ti].cce);
        nr_pdcch_dci01_layout_observe(ul_opts.bwp_start, ul_opts.bwp_size, ul_opts.tda_count, cand_task[ti].dci_length,
                                      u->raw_payload);
        { /* grant-loss SCORING only: matchable to the gNB scheduler log */
          struct timespec uts;
          clock_gettime(CLOCK_REALTIME, &uts);
          LOG_I(PHY, "SENSING: blind PDCCH ul_seen utc_ns=%lld rnti=0x%x sfn=%d slot=%d fmt=0_1 cce=%d al=%d\n",
                (long long)uts.tv_sec * 1000000000LL + uts.tv_nsec, u->rnti, cand_task[ti].frame, cand_task[ti].slot,
                cand_task[ti].cce, (int)cand_task[ti].L);
        }
        /* Park it for the slot its PUSCH occupies. The DCI is in a DOWNLINK slot; the PUSCH is k2
         * slots later in an UPLINK one, where nothing runs today. */
        const int ul_verdict = atomic_load_explicit(&g_dci01_fdra_refuse, memory_order_relaxed) ? NR_DCI01_FDRA_REFUSE
                                                                                              : NR_DCI01_FDRA_BOOK;
        const bool ul_oracle = dci01_oracle_grant(u);
        unsigned long nref = 0;
        if (ul_verdict == NR_DCI01_FDRA_REFUSE && ul_oracle) {
          nref = atomic_fetch_add_explicit(&g_ul_fdra_refused, 1, memory_order_relaxed) + 1;
          if (nref == 1 || (nref % 10000) == 0)
            LOG_W(PHY, "SENSING: UL_FDRA_REFUSED n=%lu: the 0_1 FDRA is not a RIV -- 0_1 grant not booked (it would be "
                       "decoded at the wrong PRBs/offsets); 1 in %d still booked as a probe, 0_0 always booked\n",
                  nref, NR_DCI01_FDRA_PROBE_EVERY);
        }
        if (nr_dci01_fdra_book(ul_verdict, ul_oracle, nref))
          nr_pusch_grant_book_add(u, source_absolute_slot);
      } else {
        g_ul_rejects++;
      }
      /* ULDCIGT (ISAC_PDCCH_ULDCIGT=1): the reconciliation instrument, and the reason this commit
       * exists before any extraction does. The 0_1 field WIDTHS on this deployment are not pinned
       * -- 33 of the live 43 bits are spec-fixed and the remaining 10 admit more than one
       * assignment (see the UL section of nr_pdcch_blind_monitor.h) -- so the payload is dumped RAW
       * alongside the decoded fields, for EVERY candidate whose CRC landed in the plausible RNTI
       * range including rejected ones.
       *
       * The gNB logs h_id/ndi/rv/mcs/tpc/dai/mimo/ant on its own `UL PDCCH:` line for the same
       * grant. Solving offline for the width assignment that reproduces all EIGHT simultaneously is
       * a far stronger test than any total-length check -- and a total-length check is exactly what
       * let the DL path read every field after the frequency-domain assignment from the wrong
       * offset for a year while CRC still passed. Derive, then reconcile (R6). */
      {
        static int s_uldcigt = -1;
        if (s_uldcigt < 0)
          s_uldcigt = (getenv("ISAC_PDCCH_ULDCIGT") != NULL) ? 1 : 0;
        /* Print ACCEPTS plus every candidate whose FULL 24-bit CRC landed in the plausible RNTI
         * range -- `rnti` is written only after that check passes, so a non-zero value IS the
         * check. Gating on crc_rnti instead was wrong and expensive: crc_rnti is the low 16 bits of
         * a 24-bit CRC, so a pure-noise decode with a non-zero high byte still shows a plausible
         * looking value, and the probe fired on essentially every candidate -- 54,998 lines and
         * 21 MB in one 150 s run. Same probe-volume trap this file already records costing 163 MB
         * per run and causing the very timing runaway it was added to diagnose. */
        if (s_uldcigt && (cand_task[ti].ok || u->rnti != 0)) {
          LOG_I(PHY,
                "SENSING: ULDCIGT %d.%d cce=%d al=%u len=%u raw=0x%016llx crc_rnti=0x%x ok=%d "
                "mcs=%u rv=%u ndi=%u hid=%u tpc=%u dai=%u ant=%u sri_prec=%u srs=%u csi=%u "
                "tda=%u prb=%u+%u sym=%u+%u k2=%u cdm=%u ports=0x%x nscid=%u dmrsmask=0x%x fh=%u "
                "ulsch=%u mism=%u rej=%s\n",
                cand_task[ti].frame, cand_task[ti].slot, cand_task[ti].cce, (unsigned)cand_task[ti].L,
                (unsigned)u->dci_length, (unsigned long long)u->raw_payload, u->crc_rnti,
                cand_task[ti].ok ? 1 : 0,
                (unsigned)u->mcs, (unsigned)u->rv, (unsigned)u->ndi, (unsigned)u->harq_pid,
                (unsigned)u->tpc, (unsigned)u->dai, (unsigned)u->antenna_ports_field,
                (unsigned)u->precoding_info, (unsigned)u->srs_request, (unsigned)u->csi_request,
                (unsigned)u->tda_index, (unsigned)u->start_rb, (unsigned)u->num_rb,
                (unsigned)u->start_symbol, (unsigned)u->num_symbols, (unsigned)u->k2,
                (unsigned)u->n_dmrs_cdm_groups, (unsigned)u->dmrs_ports, (unsigned)u->nscid,
                (unsigned)u->ul_dmrs_symb_pos, (unsigned)u->frequency_hopping,
                (unsigned)u->ulsch_indicator, (unsigned)u->mismatched_bits,
                u->reject_reason ? u->reject_reason : "-");
        }
      }
      continue;
    }
    /* ---- DCI 0_0: the UL grants this scan was already decoding and throwing away. ----
     * TS 38.212 7.3.1.0 size-aligns 0_0 with 1_0, so the polar decode that just ran for this 1_0
     * candidate ALREADY produced the 0_0 payload -- an identifier bit of 0 is precisely what
     * nr_pdcch_blind_decode_and_extract_10() rejects with "DCI-1_0 identifier=0 (format 0_0 UL
     * grant, not a PDSCH DCI)". There is no second decode here and no new config knob.
     *
     * This is a genuinely different problem from 0_1, not a shortcut around it: format 0_0 has NO
     * RRC-derived field widths at all (identifier 1, FDRA=RIV, TDA 4, hopping 1, MCS 5, NDI 1,
     * RV 2, HARQ 4, TPC 2 -- all spec-fixed), so it needs no width hypothesis, no interpretation
     * hypothesis and no TB-CRC oracle to converge. It only needs a UL BWP for the RIV, which SIB1
     * supplies. On a cell scheduling with fallback formats -- this one accepts format 1_0 and
     * nothing else -- 0_0 is where the UL grants actually are.
     *
     * Two gates, both reusing what is already here: extract_00 re-reads the identifier bit itself,
     * so a real 1_0 that failed for any other reason is REJECTED rather than mis-parsed; and the
     * RNTI must be one the bootstrap has already confirmed, which is the same ~4000x-tighter test
     * the UL 0_1 scan applies, and keeps noise decodes out of the grant book. */
    if (scan_01 && !cand_task[ti].ok && cand_task[ti].format == NR_BLIND_DCI_FORMAT_1_0
        && ul_opts.bwp_size > 0 && cand_task[ti].out.rnti != 0) {
      bool confirmed = false;
      for (int k = 0; k < n_known_ul && !confirmed; k++)
        confirmed = (known_ul[k] == cand_task[ti].out.rnti);
      if (confirmed) {
        nr_pdcch_blind_ul_result_t ul00;
        if (nr_pdcch_blind_extract_00(cand_task[ti].out.payload, cand_task[ti].dci_length,
                                      cand_task[ti].out.rnti, &ul_opts, &ul00)) {
          g_ul00_accepts++;
          static uint64_t logged00;
          if (++logged00 <= 8)
            LOG_A(PHY, "SENSING: DCI 0_0 UL grant rnti=0x%x prb=%u+%u sym=%u+%u k2=%u mcs=%u\n",
                  ul00.rnti, (unsigned)ul00.start_rb, (unsigned)ul00.num_rb,
                  (unsigned)ul00.start_symbol, (unsigned)ul00.num_symbols,
                  (unsigned)ul00.k2, (unsigned)ul00.mcs);
          nr_pusch_grant_book_add(&ul00, source_absolute_slot);
        } else {
          g_ul00_rejects++;
        }
      }
    }
    if (cand_task[ti].dl_auto && cand_task[ti].format == NR_BLIND_DCI_FORMAT_1_1) {
      if (!cand_task[ti].ok || (cfg->autodiscover && !g_length_found))
        continue;
      const nr_pdcch_blind_raw_result_t *raw = &cand_task[ti].dl_raw;
      if (cand_task[ti].open_rnti) { /* resolved UEs are decoded by their own exact task */
        bool known = false;
        for (int ri = 0; ri < n_dl_ready && !known; ++ri) known = dl_ready_rnti[ri] == raw->rnti;
        if (known) continue;
      }
      ue->dci_thres = (ue->dci_thres + raw->mismatched_bits) / 2;
      if (raw->mismatched_bits > ue->dci_thres + 30
          || !rnti_persistence_check(raw->rnti, abs_slot, persist_window_slots, cfg->rnti_persist_k))
        continue;
      nr_pdcch_blind_rnti_bootstrap_record(raw->rnti, NR_BLIND_RNTI_CLASS_C, abs_slot);
      /* rnti_persistence_check() establishes a repeated raw candidate; the bootstrap table then
       * requires its own second admitted sighting. Until that happens this is discovery evidence,
       * not an operational dedicated grant. Emitting/decoding it here admitted a one-off 0x2a8a
       * noise candidate in the hidden-truth replay. */
      if (!nr_pdcch_blind_monitor_rnti_confirmed(abs_slot, raw->rnti))
        continue;
      if (accept_dup(abs_slot, raw->rnti, 0)) /* same DCI already accepted by another pass this slot */
        continue;
      if (cfg->autodiscover && !nr_pdcch_blind_monitor_autodiscover_extent_verified()
          && coreset_bank_has_owner(raw->rnti)) {
        /* Same alias rule as the lookahead path. The already banked geometry continues decoding
         * this UE through the dispatcher; the discovery cursor must keep looking for a new owner. */
        nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start + cfg->coreset_rb_offset);
        dl_discovery_invalidate();
        continue;
      }
      { /* Corroborated AL census for DCI 1_1 -- the dominant traffic. This branch does its OWN
         * persistence check above and then continues on its own path, so accepts here never reach
         * the generic Gate 2 where the census was originally placed. MEASURED 2026-09-13: 51,498
         * 1_1 grants produced ZERO confirmed counts while only stray non-1_1 accepts were tallied,
         * so the search-space inference never reached its 32-grant threshold and never fired. */
        const int Lc3 = cand_task[ti].L;
        const int li3 = (Lc3 == 1) ? 0 : (Lc3 == 2) ? 1 : (Lc3 == 4) ? 2 : 3;
        atomic_fetch_add_explicit(&g_al_confirmed[nr_pdcch_ss_bucket(cfg)][li3], 1, memory_order_relaxed);
      }
      discovery_evidence("dl_raw_admitted", "DL", cand_task[ti].frame, cand_task[ti].slot,
                          cand_task[ti].dci_length, raw->rnti, raw->payload, cand_task[ti].L, cand_task[ti].cce);
      stage0_note_accept((uint32_t)cand_task[ti].frame * fp->slots_per_frame + (uint32_t)cand_task[ti].slot,
                         raw->rnti, true);
      nr_pdcch_ss_registry_accept(cfg, raw->rnti);
      if (cfg->autodiscover) {
        const bool was_verified = nr_pdcch_blind_monitor_autodiscover_extent_verified();
        const long mono = source_absolute_slot;
        nr_pdcch_blind_monitor_autodiscover_observe(raw->rnti,
            mono >= 0 ? (uint32_t)mono : abs_slot, raw->payload);
        if (!was_verified && nr_pdcch_blind_monitor_autodiscover_extent_verified()) {
          coreset_bank_add(nr_pdcch_blind_monitor_get_cfg(), raw->rnti);
          nr_pdcch_blind_monitor_autodiscover_next();
        }
      }
      static uint64_t raw_dl_count;
      if (++raw_dl_count <= 12 || raw_dl_count % 2000 == 0)
        LOG_I(PHY, "SENSING: DL raw evidence n=%lu len=%u rnti=0x%x payload=0x%lx mm=%u; "
                   "layout requires TB-CRC evidence\n",
              (unsigned long)raw_dl_count, cand_task[ti].dci_length, raw->rnti,
              (unsigned long)raw->payload, raw->mismatched_bits);
      nr_pdcch_dci11_layout_observe(cfg, cand_task[ti].dci_length, raw->payload);
      if (!g_pdsch_sweep_on) continue;
      /* Sized by the hand-over, NOT by the resolver's 512-entry capacity: this runs on a scan
       * consumer's stack, and 512 results there overflowed it on the first DL grant (OTA 2026-09-15). */
      nr_pdcch_blind_result_t layouts[NR_DCI11_STAGE2_MAX_ALIVE + 3];
      uint16_t layout_ids[NR_DCI11_STAGE2_MAX_ALIVE + 3];
      int n = 0;
      if ((nr_pdcch_dci11_stage2_enabled() || nr_agnostic_v2()) && g_dci11_state == 1)
        n = nr_pdcch_dci11_stage2_candidates(raw, cand_task[ti].dci_length, cfg, layouts, layout_ids, NR_DCI11_STAGE2_MAX_ALIVE);
      const bool from_stage2 = (n > 0);
      if (!n) {
        uint8_t ids8[3];
        n = nr_pdcch_blind_dl_layout_candidates(raw, cand_task[ti].dci_length,
            cfg->bwp_size, cfg->dmrs_typeA_position, layouts, ids8);
        for (int i = 0; i < n && i < 3; i++) layout_ids[i] = ids8[i];
      }
      if (!n) continue;
      {
        static uint32_t s_cand_n;
        if ((s_cand_n++ % 2000) == 0) {
          char cb[160];
          int u = 0;
          for (int i = 0; i < n && u < (int)sizeof(cb) - 12; i++)
            u += snprintf(cb + u, sizeof(cb) - u, "%u:%u/%u/%u ", layout_ids[i], __builtin_popcount(layouts[i].dmrs_ports), layouts[i].mcs, layouts[i].rv);
          LOG_A(PHY, "SENSING: DL layout candidates n=%d %s ids(ports/mcs/rv)= %s\n", n, from_stage2 ? "stage2" : "fallback", cb);
        }
      }
      uint64_t keys[NR_DCI11_STAGE2_MAX_ALIVE + 3];
      int settled=-1, n_settled=0;
      for (int i=0;i<n;++i) {
        keys[i]=(g_pdsch_configuration ^ (uint64_t)(layout_ids[i]+1)) * UINT64_C(1099511628211);
        if (nr_pdsch_config_sweep_is_settled(keys[i],raw->rnti,layouts[i].tda_index,cfg->dmrs_typeA_position)) {
          settled=i; ++n_settled;
        }
      }
      if (n_settled>1) continue; // ambiguous layouts are not a unique operational solution
      static uint32_t layout_cursor[65536]; // independent RR cursors, indexed by recovered RNTI
      /* LAYOUT-FAMILY ELIMINATION (OTA 2026-09-12). With several candidate 1_1 field layouts at one
       * DCI length, pure round-robin gave the family that actually decodes (8/9 CRC in its own
       * context) a small, payload-dependent share of the grants -- ~1/14 measured -- so its
       * Technique-D context never reached min_trials and nothing ever settled; overall DL CRC read
       * 0.3 %, 15 % or 26 % depending only on that share. A family that has passed CRC is preferred
       * over one that never has: reject-only, evidence-led, and it collapses to the old behaviour
       * while no family has evidence. Ties (both decoding) still rotate. */
      int preferred=-1; uint32_t preferred_ok=0;
      uint32_t ts_ok[NR_DCI11_STAGE2_MAX_ALIVE + 3], ts_tr[NR_DCI11_STAGE2_MAX_ALIVE + 3];
      for (int i=0;i<n;++i) {
        uint32_t ok=0,tr=0;
        if (from_stage2 && layout_ids[i] < NR_DCI11_LAYOUT_MAX) {
          /* wide search: the resolver's own probe tallies (a sweep context can be evicted between
           * two probes of the same layout; these cannot) */
          ok = dci11_layout_evidence(&g_dci11_resolver, layout_ids[i], &tr);
        } else
          nr_pdsch_config_sweep_context_stats(keys[i],raw->rnti,layouts[i].tda_index,cfg->dmrs_typeA_position,&ok,&tr);
        ts_ok[i]=ok; ts_tr[i]=tr;
        if (ok>preferred_ok) { preferred_ok=ok; preferred=i; }
        else if (ok==preferred_ok && preferred>=0 && ok>0) preferred=-1; // tie: no preference
      }
      if (preferred>=0 && preferred_ok>=8) {
        static uint8_t s_pref_logged[65536];
        atomic_store_explicit(&g_dl_layout_preferred, 1, memory_order_relaxed); /* probing is over: decode */
        if (!s_pref_logged[raw->rnti]) {
          s_pref_logged[raw->rnti]=1;
          LOG_A(PHY,"SENSING: DL layout family PREFERRED by TB CRC: rnti=0x%x layout_id=%u (%d candidates) passes=%u\n",
                raw->rnti,(unsigned)layout_ids[preferred],n,preferred_ok);
        }
      } else preferred=-1;
      int fallback;
      if (nr_agnostic_v2() && n > 1) {
        /* Thompson over the candidates' cell-wide TB-CRC evidence (contexts are keyed without the
         * RNTI, so every UE's grants inform every other's). Stage-2 candidates arrive best-scored
         * first; that order becomes a small prior so the first grants go to the stage-1 favourite. */
        static __thread uint64_t s_rng = 0;
        if (s_rng == 0) s_rng = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(uintptr_t)&s_rng;
        double prior[NR_DCI11_STAGE2_MAX_ALIVE + 3];
        for (int i=0;i<n;++i) prior[i] = from_stage2 ? 2.0 * (double)(n - i) / (double)n : 0.0;
        fallback = nr_dci11_thompson_pick(ts_ok, ts_tr, prior, n, &s_rng);
        if (fallback < 0) fallback = 0;
      } else {
        fallback = layout_cursor[raw->rnti]++ % n;
      }
      const int selected=settled>=0 ? settled : preferred>=0 ? preferred : fallback;
      cand_task[ti].out=layouts[selected];
      cand_task[ti].dl_layout_configuration=keys[selected];
      cand_task[ti].dl_layout_index=from_stage2 ? layout_ids[selected] : 0xFFFF;
    }
    const nr_pdcch_blind_result_t out = cand_task[ti].out;
    if (!cand_task[ti].ok) {
      g_last_reject_reason = out.reject_reason; // TEMPORARY diagnostic, see periodic summary below
      g_last_reject_rnti   = out.rnti;
      /* A SELF-VERIFYING class (SI/RA/TC) that decodes and is then rejected downstream is the one
       * case worth a line each: those RNTIs cannot be chance hits, so the reject reason IS the
       * defect. Chatty C-RNTI rejects stay in the periodic summary. */
      if (out.rnti_class == NR_BLIND_RNTI_CLASS_RA || out.rnti_class == NR_BLIND_RNTI_CLASS_TC || out.rnti_class == NR_BLIND_RNTI_CLASS_P) {
        LOG_A(PHY, "SENSING: DCIREJECT (%d.%d) rnti=0x%x class=%d L=%d reason=%s\n",
              proc->frame_rx, proc->nr_slot_rx, (unsigned)out.rnti, (int)out.rnti_class, cand_task[ti].L,
              out.reject_reason ? out.reject_reason : "(none)");
      }
      continue;
    }
    if (cfg->coreset_type == 1 && out.rnti == 0xffff
        && cand_task[ti].format == NR_BLIND_DCI_FORMAT_1_0) {
      int cursor=0;
      for (int ci=0;ci<rel15->number_of_candidates;++ci) {
        if (cand_task[ti].e_rx == &pdcch_e_rx[cursor]) {
          blind_discovery_replay(fp,proc,rel15,n_rb,rel15->BWPStart+cset_start+rel15->coreset.rb_offset,cfg->ss_first_symbol,
              pdcch_llr[0][0],&rxdataF[0][cfg->ss_first_symbol*fp->ofdm_symbol_size],source_absolute_slot,ci,out.rnti,
              cand_task[ti].dci_length,out.payload,cand_task[ti].e_rx);
          break;
        }
        cursor += 54*rel15->L[ci];
      }
    }
    stage0_note_accept((uint32_t)cand_task[ti].frame * fp->slots_per_frame + (uint32_t)cand_task[ti].slot,
                       out.rnti, nr_pdcch_blind_monitor_rnti_confirmed(abs_slot, out.rnti));
    g_accepts++;
    /* ACCSLOT: per-slot census of CRC passes, SI vs everything else. Noise passes are uniform over
     * slots if every slot is processed identically; a slot with zero passes of EITHER kind is a slot
     * the scan never really looks at (2026-09-21: SIB1 decodes at slots 11/12 and RAR at slot 5 and
     * nothing else does, on a cell that must be paging and sending Msg4 in the other DL slots). */
    if (proc->nr_slot_rx >= 0 && proc->nr_slot_rx < 20)
      g_acc_slot[out.rnti_class == NR_BLIND_RNTI_CLASS_SI ? 0 : 1][proc->nr_slot_rx]++;
    if (pbwp_on && out.rnti != 0) {
      pthread_mutex_lock(&g_pbwp_lock);
      nr_pbwp_mark_seen(&g_pbwp, out.rnti);
      const bool sw = out.dci_format == NR_BLIND_DCI_FORMAT_1_1
                      && nr_pbwp_on_accept(&g_pbwp, out.rnti, cand_task[ti].bwp_entry);
      const uint32_t nsw = g_pbwp.switches;
      pthread_mutex_unlock(&g_pbwp_lock);
      if (sw)
        LOG_A(PHY, "SENSING: BWP SWITCH rnti=0x%x -> entry %d (len %u, start %d, size %u), switches=%u\n",
              out.rnti, cand_task[ti].bwp_entry, cand_task[ti].dci_length,
              pbwp_snap[cand_task[ti].bwp_entry].start, cand_task[ti].bwp_size, nsw);
    }
    {
      /* Feeds the adaptive ladder above. Counted per AGGREGATION LEVEL of the candidate that
       * produced the accept, which is the quantity the allocation needs -- not per candidate index,
       * which changes meaning as the allocation itself changes. */
      const int Lc = cand_task[ti].L;
      const int li = (Lc == 1) ? 0 : (Lc == 2) ? 1 : (Lc == 4) ? 2 : 3;
      atomic_fetch_add_explicit(&g_al_accepts[li], 1, memory_order_relaxed);
    }
    /* ---- Per-format resolution, hoisted here because everything below -- the DCIGT probe, the
     * PDSCH allocation, the TBS -- depends on it. THREE quantities differ between DCI 1_1 and DCI
     * 1_0 and each is silently wrong rather than loudly wrong if mixed up:
     *   rb_origin        TS 38.214 5.1.2.2.2 -- a 1_0 grant in a COMMON search space numbers its RBs
     *                    from the CORESET's lowest RB, not the BWP start.
     *   BWP framing      the same distinction, for the frequency reference the allocation sits in.
     *   grant_mcs_table  TS 38.214 5.1.3.1 -- format 1_0 is ALWAYS Table 5.1.3.1-1 (qam64); a
     *                    deployment-wide qam256 must not leak onto it.
     * For format 1_1, and for 1_0 in a UE-specific search space, all three resolve to what this
     * function used before. ---- */
    const bool    is_dci10        = (out.dci_format == NR_BLIND_DCI_FORMAT_1_0);
    const int     rb_origin       = is_dci10 ? dci10_rb_base
                                  : (cand_task[ti].bwp_entry > 0 ? pbwp_snap[cand_task[ti].bwp_entry].start : cfg->bwp_start);
    uint8_t grant_mcs_table = is_dci10 ? out.mcs_table : (uint8_t)cfg->pdsch_mcs_table;
    if (is_dci10) {
      g_accepts_10++;
      if (out.rnti_class < NR_BLIND_RNTI_CLASS_COUNT) {
        g_accepts_class[out.rnti_class]++;
      }
    }

    /* DEADLOCK 3, measured 2026-09-20 on Swisscom PCI 382 -- the third gate to starve the seed it
     * depends on. With a CSS0-only scan the monitor accepted TC=16 real TC-RNTIs (and SI=10000
     * SIB1s, so the chain is demonstrably healthy), yet BOOTTABLE stayed used=0. The record sat
     * BELOW Gate 1.5, the adaptive mismatched-bits test, and held[mismatch=18] is those accepts:
     * ue->dci_thres is an EMA driven here by ten thousand pristine SIB1 decodes, so it sits very
     * low and a slightly noisier TC-RNTI decode exceeds dci_thres+30 and is dropped.
     *
     * A TC-RNTI is the one identity this receiver can VERIFY -- it is handed out during random
     * access, so it belongs to a real UE and becomes that UE C-RNTI. It is exactly the seed the
     * dedicated CORESET search needs, and it was being thrown away by a threshold tuned on
     * broadcast traffic.
     *
     * Recorded here, above every heuristic gate, for the same reason the Gate 2 move was safe: the
     * bootstrap does not trust one sighting either -- boot_entry_live() still demands sightings >= 2
     * within RNTI_BOOTSTRAP_STALE_SLOTS, so a false accept that never recurs enters at n=1 and is
     * never confirmed. The gates still govern what gets DECODED and reported; they no longer govern
     * what gets REMEMBERED. */
    {
      const int ss_bucket = nr_pdcch_ss_bucket(cfg);
      /* Never let an unverified geometry bootstrap itself. Salt produced thousands of raw 1_0
       * chance accepts; over millions of trials several 16-bit values repeated and filled the
       * dedicated set with false RNTIs. CORESET#0 USS is also only a search hypothesis despite its
       * known physical geometry. Exact identities enter from a common SS, an already verified
       * dedicated CORESET, or record_corroborated() after the joint five-payload lock. */
      if (ss_bucket == 0) {
        nr_pdcch_blind_rnti_bootstrap_record_trusted(out.rnti, out.rnti_class, abs_slot);
      } else if (cfg->coreset_type == 0 && (!cfg->autodiscover || g_length_found)) {
        nr_pdcch_blind_rnti_bootstrap_record(out.rnti, out.rnti_class, abs_slot);
      }
    }

    // ---- Gate 1.5: mismatched-bits adaptive threshold. Migrated from NRSniffer's dci_nr.c
    // (nr_dci_false_detection + ue->dci_thres), 2026-08-05. out.mismatched_bits (computed in
    // nr_pdcch_blind_decode_and_extract_ex) counts bit disagreements between the re-encoded
    // payload and the original LLR polarity -- a CRC-plausible candidate that's actually a random
    // false accept will typically mismatch far more bits than a genuine decode. ue->dci_thres is a
    // simple EMA of recent mismatch counts (self-calibrating to this receiver's own noise floor,
    // not a fixed constant); a candidate mismatching more than dci_thres+30 bits is rejected.
    // Default ON (NRSniffer runs this unconditionally); no new config knob added given time
    // constraints -- flag for follow-up if it needs to be independently disable-able.
    if (!cand_task[ti].dl_auto || is_dci10) {
    ue->dci_thres = (ue->dci_thres + out.mismatched_bits) / 2;
    {
      /* ISAC_PDCCH_NO_MISMATCH_GATE=1 bypasses this gate. It has no config knob (see the note above),
       * and for DIAGNOSIS it must be removable: it is an adaptive EMA, so a genuine decode whose
       * mismatch count sits above the running mean is dropped before it can ever be examined. */
      static int s_no_mm = -1;
      if (s_no_mm < 0)
        s_no_mm = (getenv("ISAC_PDCCH_NO_MISMATCH_GATE") != NULL) ? 1 : 0;
      /* DCIQUAL: report decode QUALITY for the non-SI classes. The DCI CRC passing only means a
       * 16-bit RNTI was recovered; the PAYLOAD can still carry bit errors, which would give a
       * subtly wrong allocation and a PDSCH that can never decode. mismatched_bits counts
       * re-encode disagreements against the LLR polarity, so a genuine decode sits near the EMA
       * and a marginal one well above it. Printed against dci_thres because the gate is adaptive:
       * the absolute number means nothing without the running mean it is judged against. */
      if (out.rnti_class != NR_BLIND_RNTI_CLASS_SI) {
        static _Atomic uint64_t s_dq = 0;
        const uint64_t dq = atomic_fetch_add_explicit(&s_dq, 1, memory_order_relaxed) + 1;
        if (dq <= 40 || (dq % 50) == 0)
          LOG_A(PHY, "SENSING: DCIQUAL rnti=0x%x class=%u mismatched_bits=%u dci_thres=%u %s\n",
                out.rnti, (unsigned)out.rnti_class, (unsigned)out.mismatched_bits,
                (unsigned)ue->dci_thres,
                (out.mismatched_bits > (ue->dci_thres + 30)) ? "HELD" : "pass");
      }
      /* MSG4 BY EXACT RNTI (2026-09-21). A TC-class accept whose RNTI was issued by a RAR we decoded
       * in the last ~2 s is Msg4 (or its retransmission) -- an exact 16-bit match against a known
       * value, stronger than any heuristic gate. Logged unconditionally so the RAR -> Msg4 link is
       * MEASURED (harv3: 46 RARs, 0 matching accepts -- this line settles whether Msg4 ever reaches
       * the CRC stage), and exempt from the mismatch hold below. */
      uint32_t msg4_age = 0;
      const bool msg4_exact = (out.rnti_class == NR_BLIND_RNTI_CLASS_TC)
                              && nr_passive_rar_tc_seen(out.rnti, abs_slot, 4 * fp->slots_per_frame * 10, &msg4_age);
      if (msg4_exact)
        LOG_A(PHY, "SENSING: MSG4 CANDIDATE (%d.%d) tc_rnti=0x%x %u slots after its RAR, mismatched_bits=%u thres=%u L=%d\n",
              proc->frame_rx, proc->nr_slot_rx, out.rnti, msg4_age, (unsigned)out.mismatched_bits,
              (unsigned)ue->dci_thres, cand_task[ti].L);
      if (!msg4_exact && !s_no_mm && out.mismatched_bits > (ue->dci_thres + 30)) {
        g_held_mismatch++;
        continue;
      }
    }

    /* DEADLOCK 2, measured 2026-09-20 on Swisscom PCI 382: this call used to sit BELOW Gate 2, so
     * an accept held by the persistence gate never recorded a sighting. But the bootstrap's own
     * confirmation rule is "2 sightings in the window" -- the counter that would have released the
     * gate was itself behind the gate. Measured: occasions=5377 accepts=164, held[persist=127
     * mismatch=37] = 164, i.e. EVERY accept held, and BOOTTABLE used=0 -- the table never received
     * a single entry, so bootstrap_rnti stayed 0x0 and with it the dedicated CORESET verification,
     * the DL length sweep and the UL PUSCH scan (all three gate on the confirmed-RNTI set).
     *
     * This is the same deadlock the 1_0-seeding fix below addressed one layer down, and the file's
     * own comment there already states the intent: record new-UE evidence BEFORE membership gating.
     * Recording pre-gate is safe because the bootstrap does NOT trust a single sighting either --
     * boot_entry_live() still demands sightings >= 2 within RNTI_BOOTSTRAP_STALE_SLOTS, so a noise
     * RNTI that never recurs enters at n=1 and is never confirmed, exactly as before. */
    // ---- Gate 2: RNTI persistence. A real UE's RNTI recurs across many grants; a noise accept is
    // (almost always) a one-off. See rnti_persistence_check()'s own comment. ----
    /* SAME EXEMPTION AS GATE 3 (2026-09-21, Swisscom PCI 382): a TC-RNTI addresses exactly ONE
     * Msg4, so "recur within 500 ms or be held" holds every Msg4 by construction -- MEASURED
     * TC accepts 17..322 per run, TC PDSCH decodes 0, held[persist] the only non-zero hold. RA-RNTI
     * is verified by PRACH decomposition, SI is a constant; recurrence adds nothing for any of the
     * three, and Msg4 is the ONLY carrier of the dedicated config (nr_passive_rrc_harvest.c). */
    const bool gate2_exempt = (out.rnti_class == NR_BLIND_RNTI_CLASS_SI)
                              || (out.rnti_class == NR_BLIND_RNTI_CLASS_TC)
                              || (out.rnti_class == NR_BLIND_RNTI_CLASS_RA);
    if (!gate2_exempt && !rnti_persistence_check(out.rnti, abs_slot, persist_window_slots, cfg->rnti_persist_k)) {
      g_held_persist++;
      continue;
    }

    { /* Corroborated AL census: this accept's RNTI recurred, so it is not a one-off false accept.
       * The RAW census above is fed by every accept and is therefore dominated by the blind
       * false-accept floor, which scales with how many candidates a level is given -- useless for
       * inferring what the CELL does. This one only counts accepts that survived the gate. */
      const int Lc2 = cand_task[ti].L;
      const int li2 = (Lc2 == 1) ? 0 : (Lc2 == 2) ? 1 : (Lc2 == 4) ? 2 : 3;
      atomic_fetch_add_explicit(&g_al_confirmed[nr_pdcch_ss_bucket(cfg)][li2], 1, memory_order_relaxed);
    }

    { /* ---- PDCCH SCRAMBLING IDENTITY VERDICT  (agnosticity #5) ----------------------------
       * pdcch-DMRS-ScramblingID is assumed = PCI (mandated for CORESET#0, but DEDICATED CORESETs
       * may carry a configured value, and nothing verified ours). A CRC-recovered RNTI IS the
       * proof: the DM-RS sequence generated from this identity is what de-scrambles the candidate,
       * so a WRONG identity yields no accepts at all rather than degraded ones. State it once per
       * search space instead of leaving it an unexamined assumption -- and say plainly that this
       * confirms the value IN USE, it does not search the 1024-value domain for an override. */
      const int ssb_ = nr_pdcch_ss_bucket(cfg);
      static _Atomic int s_scr_logged[2];
      int expect_ = 0;
      if (atomic_compare_exchange_strong_explicit(&s_scr_logged[ssb_], &expect_, 1,
                                                  memory_order_relaxed, memory_order_relaxed)) {
        LOG_A(PHY, "SENSING: PDCCH_SCRAMBLING_ID CONFIRMED [%s] n_id=%u (assumed = PCI %u) by "
                   "CRC-recovered RNTI 0x%x -- a wrong identity yields zero accepts, so this is "
                   "proof of the value in use, not a search of the identity domain\n",
              ssb_ ? "USS(dedicated)" : "CSS0(common)",
              (unsigned)cfg->coreset_pdcch_dmrs_scrambling_id,
              (unsigned)ue->frame_parms.Nid_cell, out.rnti);
      }
    }

    /* Record new-UE evidence before membership gating, otherwise a confirmed UE prevents
     * every later UE from ever acquiring a context. Only resolved-length accepts may seed it. */
    /* DEADLOCK, measured 2026-09-19 on two commercial cells: the bootstrap only seeded AFTER the
     * DCI 1_1 length was found, but the length sweep needs a bootstrapped RNTI to reach significance
     * on a sparsely loaded cell -- so on a cell where we decode 1_0 grants and nothing else,
     * bootstrap_rnti stayed 0x0 for entire 25-minute runs and no mapping could ever be verified.
     * Format 1_0's length is DERIVED (CORESET/BWP), never guessed, so a 1_0 accept is exactly as
     * trustworthy a sighting as a post-lock one -- and it still has to clear the same persistence
     * gate (2 sightings in the window) before it counts as confirmed. */
    if (cfg->autodiscover && g_length_found && !is_dci10) {
      const uint64_t fingerprint = (uint64_t)out.start_rb | ((uint64_t)out.num_rb << 9)
          | ((uint64_t)out.mcs << 18) | ((uint64_t)out.rv << 23) | ((uint64_t)out.ndi << 25)
          | ((uint64_t)out.harq_pid << 26) | ((uint64_t)out.tda_index << 30);
      const long mono = source_absolute_slot;
      nr_pdcch_blind_monitor_autodiscover_observe(out.rnti, mono >= 0 ? (uint32_t)mono : abs_slot, fingerprint);
    }
    }

    /* ---- Gate 3: confirmed-UE set. Once ANY UE is confirmed, a candidate whose CRC-recovered RNTI
     * is not one of them is a false accept: real grants are addressed to UEs that recur, noise is
     * not. Inert until the first confirmation, so discovery is never blocked by its own output. */
    {
      /* The self-verifying classes are EXEMPT. This gate's premise -- "real grants are addressed to
       * UEs that recur" -- holds only for C-RNTI. It is wrong, and in two cases circular, for:
       *   SI-RNTI  the fixed constant 0xFFFF. Unfakeable, and never a member of the confirmed set,
       *            so once ANY UE was confirmed EVERY SI accept was held. MEASURED 2026-09-21:
       *            SIB1 PDSCH decoding stopped dead at 188 of 14450 accepts for exactly this
       *            reason (held[rnti_set]=11228, and persist+mismatch+rnti_set = accepts - 188
       *            to the unit).
       *   TC-RNTI  single-use by construction (one random-access contention resolution). Demanding
       *            that it recur before it may be decoded is circular: Msg4 is what would confirm
       *            it, and Msg4 is what this gate was blocking. 41 accepts, 0 decoded.
       *   RA-RNTI  a structured value now checked against the cell's own PRACH configuration
       *            (nr_pdcch_sib1_prior_ra_rnti_valid), which is stronger evidence than recurrence.
       * C-RNTI keeps the gate, which is where it actually discriminates. */
      const bool self_verifying = (out.rnti_class == NR_BLIND_RNTI_CLASS_SI)
                                  || (out.rnti_class == NR_BLIND_RNTI_CLASS_TC)
                                  || (out.rnti_class == NR_BLIND_RNTI_CLASS_RA);
      uint16_t known[NR_PDCCH_BLIND_MAX_UE];
      const int n_known = nr_pdcch_blind_monitor_confirmed_rnti_set(abs_slot, known, NR_PDCCH_BLIND_MAX_UE);
      if (!self_verifying && n_known > 0 && !nr_pdcch_blind_monitor_rnti_confirmed(abs_slot, out.rnti)) {
        g_held_rnti_set++;
        continue;
      }
    }

    LOG_D(PHY,
         "SENSING: blind PDCCH accept (%d.%d) rnti=0x%x prb=[%u..%u) sym=[%u..%u) dmrs_mask=0x%x\n",
         proc->frame_rx, proc->nr_slot_rx, out.rnti, out.start_rb, out.start_rb + out.num_rb, out.start_symbol,
         out.start_symbol + out.num_symbols, out.dl_dmrs_symb_pos);

    /* Do NOT let an unresolved DCI length seed the C-RNTI bootstrap: at the formula-default length
     * the accepts are noise, two of them agreeing is enough to CONFIRM, and the consumer below then
     * narrows acceptance to a value that was never on the air. */

    // ---- Cross-receiver RNTI consistency (offline, post-hoc -- see tests/passive_rx/rnti_gate.py):
    // this line's sole purpose is a wall-clock anchor to correlate accepts across INDEPENDENT
    // receiver PROCESSES that share no RT state. A real active UE's RNTI is legitimately accepted by
    // EVERY receiver that can hear it (same air, same grants); a noise/false accept is
    // receiver-local (wrong CRC-recovered RNTI from that receiver's own decode error) and will not
    // recur on a SECOND receiver at the same wall-clock time. LOG_I (not LOG_D) and deliberately
    // separate from the line above -- that one is for interactive debugging (PRB/symbol detail, gets
    // noisy fast), this one is a fixed, parseable schema meant to be grepped by tooling. ----
    /* ---- DCIGT probe (ISAC_PDCCH_DCIGT=1): a fixed, parseable schema carrying EVERYTHING that
     * can be checked against the gNB's own scheduler log for the same grant --
     *   [SFN.slot] cce/al   -> did we look in the right place, and is our slot clock aligned?
     *   mcs/rv/ndi/hid      -> are the DCI FIELD OFFSETS right? A CRC pass only proves the payload
     *                          BITS are right; it says nothing about where the fields sit in them,
     *                          and section 12 of CLAUDE.md records two widths that were wrong for
     *                          exactly this reason while CRC still passed.
     *   prb/sym             -> does the PDSCH allocation we derive match what the gNB scheduled?
     * Off by default (rule R7: no full-rate logging during a capture). */
    {
      static int s_dcigt = -1;
      if (s_dcigt < 0)
        s_dcigt = (getenv("ISAC_PDCCH_DCIGT") != NULL) ? 1 : 0;
      if (s_dcigt) {
        static const char *const kClassName[NR_BLIND_RNTI_CLASS_COUNT] = {"C", "TC", "SI", "RA", "P"};
        LOG_I(PHY,
              "SENSING: DCIGT %d.%d fmt=%s class=%s rnti=0x%x cce=%d al=%u mcs=%u/tbl%u rv=%u ndi=%u hid=%u "
              "tda=%u prb=%u+%u(org%d) sym=%u+%u cdm=%u ports=0x%x nscid=%u dmrsmask=0x%x vrb=%u tbs_scal=%u\n",
              cand_task[ti].frame, cand_task[ti].slot,
              (out.dci_format == NR_BLIND_DCI_FORMAT_1_0) ? "1_0" : "1_1",
              (out.rnti_class < NR_BLIND_RNTI_CLASS_COUNT) ? kClassName[out.rnti_class] : "?",
              out.rnti, cand_task[ti].cce,
              (unsigned)cand_task[ti].L, (unsigned)out.mcs, (unsigned)grant_mcs_table, (unsigned)out.rv,
              (unsigned)out.ndi, (unsigned)out.harq_pid, (unsigned)out.tda_index, (unsigned)out.start_rb,
              (unsigned)out.num_rb, rb_origin, (unsigned)out.start_symbol, (unsigned)out.num_symbols,
              (unsigned)out.n_dmrs_cdm_groups, (unsigned)out.dmrs_ports, (unsigned)out.nscid,
              (unsigned)out.dl_dmrs_symb_pos, (unsigned)out.vrb_to_prb, (unsigned)out.tb_scaling);
      }
    }

    struct timespec rnti_ts;
    clock_gettime(CLOCK_REALTIME, &rnti_ts);
    const long long rnti_utc_ns = (long long)rnti_ts.tv_sec * 1000000000LL + (long long)rnti_ts.tv_nsec;
    /* sfn/slot/cce make each accept matchable to the gNB scheduler log -- for grant-loss SCORING only */
    LOG_I(PHY, "SENSING: blind PDCCH rnti_seen utc_ns=%lld rnti=0x%x sfn=%d slot=%d fmt=%s cce=%d al=%d\n", rnti_utc_ns,
          out.rnti, cand_task[ti].frame, cand_task[ti].slot,
          cand_task[ti].ul_scan ? "0_x" : (out.dci_format == NR_BLIND_DCI_FORMAT_1_0 ? "1_0" : "1_1"), cand_task[ti].cce,
          (int)cand_task[ti].L);
    /* Tie this accept to whichever CORESET windows are currently lit -- see COREMAPLT. */
    if (out.rnti_class == NR_BLIND_RNTI_CLASS_C || out.rnti_class == NR_BLIND_RNTI_CLASS_TC)
      nr_pdcch_blind_monitor_note_rnti_for_windows(out.rnti);

    /* ON-ACCEPT DM-RS PROBE (ISAC_COREMAP_ONACCEPT=1, default off). This DCI passed CRC from THIS
     * buffer, at THIS symbol -- so the CORESET provably carried a PDCCH here. Correlating now
     * removes the duty-cycle dilution that every all-calls average suffers, and is the paired
     * control for COREMAPCTL. Uses the cfg's own CORESET geometry, so it asks about the CORESET
     * the accept actually came from rather than an assumed one. */
    {
      int probe_n_rb = 0, probe_start = 0;
      get_coreset_rballoc(rel15->coreset.frequency_domain_resource, &probe_n_rb, &probe_start);
      nr_pdcch_coreset_map_accept_probe(&rxdataF[0][cfg->ss_first_symbol * fp->ofdm_symbol_size],
                                        fp->ofdm_symbol_size, fp->N_RB_DL, fp->first_carrier_offset,
                                        rel15->coreset.pdcch_dmrs_scrambling_id, proc->nr_slot_rx,
                                        cfg->ss_first_symbol,
                                        probe_start + rel15->coreset.rb_offset, probe_n_rb);
    }

    // ---- CFR extraction: nr_pdsch_channel_estimation() on the blind-decoded allocation/DMRS config
    // -- mirrors phy_procedures_nr_ue.c's existing pdsch_dmrs ISAC tap exactly (same function, same
    // pdsch_est_size formula, same comb-2 packing), just fed from a blind decode instead of the UE's
    // own real DLSCH config. ----
    /* Accepted DCI may still be discovery evidence. Do not emit PDSCH/CFR from
     * unverified geometry or silently use manual interpretation while full-auto is waiting. */
    if (cfg->autodiscover && (!g_length_found || !nr_pdcch_blind_monitor_autodiscover_extent_verified())) {
      grantdrop(&out, proc->frame_rx, proc->nr_slot_rx, "autodiscover-unverified");
      continue;
    }
    if (cfg->dl_full_auto && !is_dci10 && !g_pdsch_sweep_on) {
      grantdrop(&out, proc->frame_rx, proc->nr_slot_rx, "full_auto-no-sweep");
      continue;
    }

    int dmrs_sym = -1;
    uint8_t hy_k0 = 0; /* PDSCH slot offset from this DCI's slot, from the Technique D hypothesis */
    for (int m = out.start_symbol; m < out.start_symbol + out.num_symbols; m++) {
      if (out.dl_dmrs_symb_pos & (1u << m)) {
        dmrs_sym = m;
        break;
      }
    }
    if (dmrs_sym < 0) {
      grantdrop(&out, proc->frame_rx, proc->nr_slot_rx, "no-dmrs-symbol-in-tda");
      continue;
    }

    fapi_nr_dl_config_dlsch_pdu_rel15_t dlsch_pdu;
    memset(&dlsch_pdu, 0, sizeof(dlsch_pdu));
    dlsch_pdu.BWPStart           = (uint16_t)rb_origin;
    dlsch_pdu.BWPSize            = is_dci10 ? dci10_ctx.n_rb_riv : cand_task[ti].bwp_size;
    /* resource_alloc stays 1 for RA type 0 too: nr_dl_channel_estimation.c AssertFatal()s on 0 when it
     * PRB-averages, and rb_bitmap is never filled. A type-0 grant travels as a Task 9 PRB list in
     * freq_alloc (below), which is what the passive decode and every chest here actually consume. */
    dlsch_pdu.resource_alloc     = 1;
    /* DM-RS SEQUENCE reference point, TS 38.211 7.4.1.1.2. NOT cosmetic and NOT unread:
     * nr_dl_channel_estimation.c:1249 computes the gold-sequence offset as
     *     first_rb + (refPoint ? 0 : BWPStart)
     * so refPoint = 0 references the sequence to CRB 0 and refPoint = 1 references it to the
     * BWP/CORESET start. The spec's rule is "subcarrier 0 of CRB 0 in general, EXCEPT for a PDSCH
     * scheduled by DCI format 1_0 with CRC scrambled by SI-RNTI in a Type0-PDCCH common search
     * space, where it is subcarrier 0 of the lowest-numbered RB of the CORESET" -- i.e. exactly
     * OAI's own `mac->get_sib1 ? 1 : 0`.
     *
     * MEASURED 2026-08-21: this cell puts CORESET#0 at CRB 1, the gNB logs `ref_point=1` on every
     * SIB1 PDSCH, and hardcoding 0 here offset our DM-RS sequence by one RB -- every DCI field
     * decoded perfectly and the PDSCH still failed CRC 32/32. A wrong sequence looks like a dead
     * channel, not like a wrong parameter.
     *
     * Scoped to SI-RNTI: RA-/TC-/P-RNTI and every format 1_1 grant take the general CRB-0 rule.
     * KNOWN GAP: the spec scopes the exception to Type0-PDCCH CSS specifically, and this monitor
     * cannot tell Type0 from Type0A (other SI messages, si_indicator = 1) -- if Type0A SI ever needs
     * decoding here, gate this on out.si_indicator == 0 as well. */
    /* RETRACTED 2026-09-21: the RA/TC extension above was wrong on BOTH counts. TS 38.211 7.4.1.1.2
     * scopes the CORESET reference point to SI-RNTI in Type0-PDCCH CSS only (which is exactly OAI's
     * own `mac->get_sib1 ? 1 : 0`), and it did not fix anything -- genuine RARs fail CRC identically
     * at refPoint 0 and 1, so refpt was never the differing field the comment above claimed. */
    dlsch_pdu.refPoint           = (is_dci10 && out.rnti_class == NR_BLIND_RNTI_CLASS_SI) ? 1 : 0;
    dlsch_pdu.dmrsConfigType     = out.dmrs_config_type ? NFAPI_NR_DMRS_TYPE2 : NFAPI_NR_DMRS_TYPE1; // the table the ports were read under
    dlsch_pdu.n_dmrs_cdm_groups  = out.n_dmrs_cdm_groups;
    dlsch_pdu.dlDmrsScramblingId = fp->Nid_cell;
    dlsch_pdu.nscid              = out.nscid;
    dlsch_pdu.start_symbol       = out.start_symbol;
    dlsch_pdu.number_symbols     = out.num_symbols;
    dlsch_pdu.dlDmrsSymbPos      = out.dl_dmrs_symb_pos;
    dlsch_pdu.dmrs_ports         = out.dmrs_ports;
    // Only the passive PDSCH decode below reads these; harmless for the DM-RS-only path, which
    // never looks past the allocation. dlDataScramblingId = PCI because this gNB leaves
    // dataScramblingIdentityPDSCH unset (nr_radio_config.c:1745) -- re-verify per deployment, a
    // wrong value descrambles to noise exactly like a wrong csirs_monitor scramb_id does.
    dlsch_pdu.dlDataScramblingId = fp->Nid_cell;
    dlsch_pdu.harq_process_nbr   = out.harq_pid;
    dlsch_pdu.number_rbs         = out.num_rb;
    dlsch_pdu.start_rb           = out.start_rb;
    dlsch_pdu.mcs_table          = grant_mcs_table;

    /* Apply only to dedicated format 1_1. Common/SI/RA/Paging semantics stay manual/spec-derived.
     * The key includes RNTI and the observed TDA index; tickets carry a generation across threads. */
    nr_pdsch_sweep_ticket_t sweep_ticket = {0};
    /* 0 is layout 0, not "none": a format 1_0 / manual-layout decode used to credit its CRC (SIB1 passes
     * included) to stage-2 layout 0. 0xFFFF = none, which the feedback counts as DL link health. */
    sweep_ticket.layout_index = 0xFFFF;
    int8_t grant_mcs_table_lbrm = (int8_t)cfg->pdsch_mcs_table;
    if (g_pdsch_sweep_on && !is_dci10) {
      /* Rotate only grants eligible for a CRC attempt, not deterministic RV/cap drops. */
      if (!want_decode || (cfg->pdsch_rv0_only && out.rv != 0)
          || decodes_this_occasion >= cfg->pdsch_max_per_slot)
        continue;
      nr_pdsch_cfg_hypothesis_t hy;
      if (!nr_pdsch_config_sweep_select(cand_task[ti].dl_auto ? cand_task[ti].dl_layout_configuration : g_pdsch_configuration,
                                       out.rnti, out.tda_index, cand_task[ti].dl_auto ? 0 : cfg->extract.tda_count, cfg->dmrs_typeA_position,
                                       nr_pdcch_blind_dmrs_mask, &sweep_ticket, &hy))
        continue; /* Unsupported auto context is not a guessed manual success. */
      nr_pdsch_adaptive_apply(&hy, &dlsch_pdu, &grant_mcs_table, &grant_mcs_table_lbrm);
      hy_k0 = hy.k0;
      sweep_ticket.layout_index = cand_task[ti].dl_auto ? cand_task[ti].dl_layout_index : 0xFFFF;
      dmrs_sym = __builtin_ctz((unsigned)hy.dmrs_mask);
    }
    dlsch_pdu.pduBitmap          = 0; // no PTRS: format 1_1 with no dedicated PTRS config
    dlsch_pdu.numCsiRsForRateMatching = 0;
    /* CSI-RS rate matching from the blind CSI-RS search's confirmed resources (NZP and ZP), on the
     * slots they occur. A hypothesis that lands on one of this grant's DM-RS symbols is wrong for
     * this grant (the standard forbids the overlap and nr_dlsch_extract_rbs() ASSERTS on it -- which
     * killed the 4-RX OTA run r4a_223725): applied only when it touches no DM-RS symbol. */
    for (int zp = 0; zp < 2; zp++) {
      fapi_nr_dl_config_csirs_pdu_rel15_t *c = &dlsch_pdu.csiRsForRateMatching[dlsch_pdu.numCsiRsForRateMatching];
      const bool have = zp ? nr_csirs_blind_rt_rate_match_zp(abs_slot, c) : nr_csirs_blind_rt_rate_match(abs_slot, c);
      if (!have)
        continue;
      static const uint8_t num_l0[18] = {1, 1, 1, 1, 2, 1, 2, 2, 1, 2, 2, 2, 2, 2, 4, 2, 2, 4};
      bool clash = (c->row < 1 || c->row > 18);
      for (int k = 0; !clash && k < num_l0[c->row - 1]; k++)
        if ((dlsch_pdu.dlDmrsSymbPos >> (c->symb_l0 + k)) & 1) clash = true;
      if (!clash && (c->row == 13 || c->row == 14 || c->row == 16 || c->row == 17))
        for (int k = 0; !clash && k < 2; k++)
          if ((dlsch_pdu.dlDmrsSymbPos >> (c->symb_l1 + k)) & 1) clash = true;
      static _Atomic uint32_t s_clash_n;
      if (clash) {
        if ((atomic_fetch_add(&s_clash_n, 1) % 500) == 0)
          LOG_W(PHY, "SENSING: %s CSI-RS hypothesis (row %u l0=%u) overlaps this grant's DM-RS symbols (mask 0x%x): not applied\n",
                zp ? "ZP" : "NZP", c->row, c->symb_l0, dlsch_pdu.dlDmrsSymbPos);
        memset(c, 0, sizeof(*c));
      } else {
        dlsch_pdu.numCsiRsForRateMatching++;
      }
    }

    freq_alloc_bitmap_t freq_alloc = set_bitmap_from_start_size(out.start_rb, out.num_rb);
    /* RA type 0 (resolved by the DCI 1_1 layout search): the allocation is an RBG bitmap. Hand it on as a
     * DATA-ORDERED PRB list, normalised here -- the one place every path below (fast enqueue, deferred,
     * in-line decode, data-aided tap) takes it from -- so first_rb/num_rbs/bitmap agree with the list. */
    /* The stage-2 extraction that produced a type-0 read sized its RBG grid on the DEDICATED BWP
     * (cfg->bwp_start/bwp_size); a grant framed on another BWP entry cannot be expanded on that grid. */
    if (out.ra_type0 && (cand_task[ti].bwp_entry > 0 || out.rbg_bwp_start != cfg->bwp_start
                         || dlsch_pdu.BWPSize != cfg->bwp_size)) {
      grantdrop(&out, proc->frame_rx, proc->nr_slot_rx, "ra-type0-foreign-bwp");
      continue;
    }
    if (out.ra_type0) {
      freq_alloc.n_prb_list = (uint16_t)nr_ra_type0_prbs(out.rbg_bitmap, out.rbg_bwp_start, dlsch_pdu.BWPSize,
                                                         out.rbg_size, freq_alloc.prb_list, NR_PRB_SET_MAX);
      if (freq_alloc.n_prb_list == 0 || !nr_pdsch_passive_alloc_normalise(&freq_alloc, dlsch_pdu.BWPSize)) {
        grantdrop(&out, proc->frame_rx, proc->nr_slot_rx, "ra-type0-prb-list-invalid");
        continue;
      }
      dlsch_pdu.start_rb   = (uint16_t)freq_alloc.first_rb;
      dlsch_pdu.number_rbs = (uint16_t)freq_alloc.num_rbs; /* the PRB COUNT: TBS/G read it */
    }
    /* The per-accept channel estimate below (DM-RS CFR tap, SNR gate) is contiguous: for a PRB-list
     * grant it covers the list's FIRST contiguous run only (nr_pdsch_channel_estimation walks bitmap
     * blocks but restarts its output at index 0 per block). */
    freq_alloc_bitmap_t chest_alloc = freq_alloc;
    if (freq_alloc.n_prb_list > 0) {
      int run = 1;
      while (run < freq_alloc.n_prb_list && freq_alloc.prb_list[run] == freq_alloc.prb_list[run - 1] + 1)
        run++;
      chest_alloc = set_bitmap_from_start_size(freq_alloc.prb_list[0], run);
    }

    /* ---- Is the per-accept channel estimate needed AT ALL on this thread? -----------------------
     * It exists for exactly two consumers: the DM-RS CFR tap (`want_dmrs`) and the post-estimation
     * SNR gate (`min_snr_lin`). When neither is active it is computed and thrown away -- and it is
     * NOT cheap: 96.7 us measured, which at ~1500 accepts/s is ~14.5 % of the receive thread, on top
     * of fep_llr's ~9 %.
     *
     * That matters because it is now the thing costing us the capture. MEASURED 2026-08-24: every
     * short run died `RFSTALL PBCH lock lost (timing runaway)` with rf_pow HEALTHY (405-476) and
     * max_pos_acc running away to 606-1699, while the one long run held `pbch_ok=50 pbch_fail=0`
     * with max_pos_acc flat at ~545. The 0 %-CRC runs and the 3-6 s runs are the SAME fault -- the
     * timing loop losing lock -- not two separate problems, and section 15's ablation already showed
     * this receiver holds lock at ~18 % RT duty and loses it above that.
     *
     * The DEFERRED decode does its own FEP and channel estimation in the consumer, so skipping here
     * costs it nothing. `sources = "pdsch_data"` makes want_dmrs false, and this deployment runs the
     * SNR gate off, so on the sensing config this skips the whole block. */
    const bool need_chest = want_dmrs || (cfg->min_snr_lin > 0.0f);
    if (want_decode && (cfg->pdsch_rv0_only && out.rv != 0))
      grantdrop(&out, proc->frame_rx, proc->nr_slot_rx, "rv!=0");
    if (want_decode && decodes_this_occasion >= cfg->pdsch_max_per_slot)
      grantdrop(&out, proc->frame_rx, proc->nr_slot_rx, "max-per-slot");
    if (!want_decode)
      grantdrop(&out, proc->frame_rx, proc->nr_slot_rx, "pdsch_decode=0");
    if (!need_chest && defer && want_decode && !(cfg->pdsch_rv0_only && out.rv != 0)
        && decodes_this_occasion < cfg->pdsch_max_per_slot) {
      /* Nothing on this thread needs the estimate, and the consumer makes its own -- so enqueue
       * straight from the decoded DCI and skip the FEP + channel estimation entirely. Mirrors the
       * job built on the normal path below; kept explicit rather than reached by a goto, which
       * cannot legally jump into that nested block. */
      decodes_this_occasion++;
      const nr_pdsch_passive_grant_t grant_q = {.rnti           = out.rnti,
                                                .mcs            = out.mcs,
                                                .rv             = out.rv,
                                                .ndi            = out.ndi,
                                                .harq_pid       = out.harq_pid,
                                                .mcs_table      = grant_mcs_table,
                                                .nb_rb_oh       = (uint16_t)cfg->pdsch_xoverhead,
                                                .tb_scaling     = out.tb_scaling,
                                                .bw_tbslbrm     = (uint16_t)fp->N_RB_DL,
                                                .mcs_table_lbrm = grant_mcs_table_lbrm};
      nr_pdsch_passive_job_t job;
      memset(&job, 0, sizeof(job));
      job.dlsch_pdu     = dlsch_pdu;
      job.freq_alloc    = freq_alloc;
      job.grant         = grant_q;
      /* k0: the PDSCH is k0 slots after the DCI. The consumer waits for that slot's samples. */
      job.frame_rx      = (proc->frame_rx + (proc->nr_slot_rx + hy_k0) / fp->slots_per_frame) % 1024;
      job.nr_slot_rx    = (proc->nr_slot_rx + hy_k0) % fp->slots_per_frame;
      job.gNB_id        = proc->gNB_id;
      job.absolute_slot = source_absolute_slot + hy_k0;
      job.rnti          = out.rnti;
      job.rnti_class    = out.rnti_class;
      job.harq_pid_tag  = blind_harq_tag(abs_slot, out.rnti, out.harq_pid);
      job.want_data     = want_data;
      job.fo_hz         = isnan(nr_slot_fep_fo_override_hz)
                  ? (ue->cont_fo_comp ? ue->dl_Doppler_shift + ue->freq_offset : 0.0)
                  : nr_slot_fep_fo_override_hz;  /* receive-thread sample; see nr_slot_fep_fo_override_hz */
      job.sweep_ticket  = sweep_ticket;
      job.bwp_entry     = cand_task[ti].bwp_entry;
      /* Wide layout set: this trial is a first-code-block probe, not a full decode. */
      /* ... until a layout family is PREFERRED by its own code-block CRCs: from then on every
       * trial is a full decode (the rank-4 bed converged at 22k grants and then sat at 0 % CRC
       * because it kept probing -- a probe never reports a TB). */
      job.layout_probe  = (cand_task[ti].dl_auto && g_dci11_state == 1
                           && g_dci11_resolver.n_alive > NR_DCI11_STAGE2_MAX_ALIVE
                           && !atomic_load_explicit(&g_dl_layout_preferred, memory_order_relaxed)) ? 1 : 0;
      atomic_fetch_add_explicit(&g_enq_class[0][out.rnti_class], 1, memory_order_relaxed);
      ragrant_dump(&dlsch_pdu, &out, grant_mcs_table, css0_occasion, "deferred");
      nr_pdsch_passive_queue_enqueue(&job);
      continue;
    }

    /* ---- Nothing below this point can be USED once the per-occasion decode cap is reached.
     * MEASURED: BTIM reports chest[n=61..2147 mean~100us] on runs configured with
     * sources = "pdsch_data", where want_dmrs is FALSE and the estimate therefore has no consumer
     * at all. The cause is the cap: the fast enqueue path above requires
     * decodes_this_occasion < pdsch_max_per_slot, so once an occasion has queued its 16 grants every
     * FURTHER accepted candidate fell through to here, paid a full-slot FEP plus
     * nr_pdsch_channel_estimation (~100us EACH, on the PHY receive thread), and then hit the same
     * cap again at the decode below and threw the result away.
     *
     * That cost scales with the ACCEPT rate, i.e. with offered load, which is exactly the wrong
     * direction: it is largest in the regime where receive-thread duty is already at the ~18 % PBCH
     * lock threshold. Bail out before the expensive work instead. `want_dmrs` still gets its
     * estimate because the DM-RS CFR tap is not subject to the decode cap. */
    if (!want_dmrs && want_decode && decodes_this_occasion >= cfg->pdsch_max_per_slot) {
      g_dec_over_cap++;
      continue;
    }

    const uint32_t pdsch_est_size = ((fp->symbols_per_slot * fp->ofdm_symbol_size + 15) / 16) * 16;
    fourDimArray_t *toFree        = NULL;
    allocCast2D(pdsch_dl_ch_estimates, int32_t, toFree, fp->nb_antennas_rx, pdsch_est_size, false);

    // Full-slot FEP for the PDSCH's own DMRS symbol -- separate from the CORESET FEP above (a
    // different symbol in general; the PDSCH allocation starts after the PDCCH region).
    const uint64_t btim_t_che = btim_on ? btim_now() : 0;
    __attribute__((aligned(32))) c16_t rxdataF_pdsch[fp->nb_antennas_rx][rxdataF_sz];
    nr_slot_fep(ue, fp, proc->nr_slot_rx, dmrs_sym, rxdataF_pdsch, link_type_dl, 0, ue->common_vars.rxdata);

    uint32_t nvar = 0;
    nr_pdsch_channel_estimation(ue, proc, &dlsch_pdu, &chest_alloc, 0, get_dmrs_port(0, out.dmrs_ports),
                               (unsigned char)dmrs_sym, pdsch_est_size, pdsch_dl_ch_estimates,
                               fp->samples_per_slot_wCP, rxdataF_pdsch, &nvar);
    btim_add(BTIM_CHEST, btim_t_che);

    const int num_sc = chest_alloc.num_rbs * NR_NB_SC_PER_RB;
    if (num_sc >= 2) {
      const uint32_t base_sc = (uint32_t)(rb_origin + chest_alloc.first_rb) * NR_NB_SC_PER_RB;
      // ---- AoA (2026-07-28): nr_pdsch_channel_estimation() above already computed the estimate for
      // EVERY rx antenna (it loops aarx in [0,nb_antennas_rx) internally and writes
      // pdsch_dl_ch_estimates[a][...] for each) -- this was already true before today, nothing new
      // needed there. The only gap was HERE: extraction/submission only ever read antenna 0 and
      // called the single-antenna nr_isac_submit_cfr(). Mirrors csi_rx.c's nr_isac_submit_csirs_ls()
      // exactly (same nr_isac_aoa_antennas()/clamp/pack-then-submit-multi pattern, already
      // live-validated there for the attached-UE AoA path). Antenna 0 stays primary (feeds
      // range-Doppler + the SNR gate below); antennas 1..N-1 exist solely for isac_aoa.cc's bearing
      // estimate.
      uint32_t nof_ant = nr_isac_aoa_antennas();
      if (nof_ant > (uint32_t)fp->nb_antennas_rx) {
        nof_ant = (uint32_t)fp->nb_antennas_rx;
      }
      if (nof_ant == 0) {
        nof_ant = 1;
      }
      if (nof_ant > NR_PDCCH_BLIND_MAX_ANT) {
        nof_ant = NR_PDCCH_BLIND_MAX_ANT;
      }
      static __thread float    isac_h[NR_PDCCH_BLIND_MAX_ANT * 2 * 273 * NR_NB_SC_PER_RB];
      static __thread uint32_t isac_k[273 * NR_NB_SC_PER_RB];
      static __thread uint32_t isac_l[273 * NR_NB_SC_PER_RB];
      uint32_t nof_re  = 0;
      double   h_pow_sum = 0; // antenna 0 only -- feeds the post-estimation SNR gate below
      for (int j = 0; j < num_sc && nof_re < 273 * NR_NB_SC_PER_RB; j += 2) { // comb-2, matches pdsch_dmrs
        for (uint32_t a = 0; a < nof_ant; a++) {
          const c16_t *dl_ch_a = (const c16_t *)&pdsch_dl_ch_estimates[a][fp->ofdm_symbol_size * dmrs_sym];
          const size_t o       = 2 * ((size_t)a * (273 * NR_NB_SC_PER_RB) + nof_re);
          isac_h[o]            = (float)dl_ch_a[j].r;
          isac_h[o + 1]        = (float)dl_ch_a[j].i;
          if (a == 0) {
            h_pow_sum += (double)dl_ch_a[j].r * dl_ch_a[j].r + (double)dl_ch_a[j].i * dl_ch_a[j].i;
          }
        }
        isac_k[nof_re] = base_sc + (uint32_t)j;
        isac_l[nof_re] = (uint32_t)dmrs_sym;
        nof_re++;
      }
      // ---- Gate 3 (last, most expensive to reach): the actual DMRS channel estimate itself looks
      // like noise even though every payload-level check passed by chance. mean|H|^2/nvar is
      // scale-invariant (both come from the SAME nr_pdsch_channel_estimation() call), so no absolute
      // unit conversion is needed. ----
      /* A hypothesis-dependent SNR gate would censor every wrong mask before CRC feedback,
       * leaving its trial count at zero forever. Exploration must reach the CRC oracle. */
      const bool snr_ok = (sweep_ticket.generation && !sweep_ticket.settled) ||
          !(cfg->min_snr_lin > 0.0f && nof_re > 0 && nvar > 0
            && (float)(h_pow_sum / nof_re) < cfg->min_snr_lin * (float)nvar);
      if (nof_re > 0 && snr_ok) {
        if (want_dmrs && (!sweep_ticket.generation || sweep_ticket.settled)) {
          /* Do not publish unverified hypothesis-derived DMRS rows into sensing. */
          nr_isac_carrier_t carrier = {.nof_prb         = (uint32_t)fp->N_RB_DL,
                                       .scs_hz          = fp->subcarrier_spacing,
                                       .dl_center_hz    = fp->dl_CarrierFreq,
                                       .pci             = fp->Nid_cell,
                                       .slots_per_frame = fp->slots_per_frame};
          nr_isac_submit_cfr_multi(abs_slot, 0.0f, NR_ISAC_SRC_PDSCH_DMRS_BLIND, &carrier, isac_h, nof_ant,
                                   273 * NR_NB_SC_PER_RB, isac_k, isac_l, nof_re, (float)nvar);
          g_cfr_submits++;
        }

        // ---- Passive data-aided PDSCH (PASSIVE_PDSCH_DATA_AIDED_HANDOVER.md Part B). Deliberately
        // the LAST thing in the chain: an LDPC decode is an order of magnitude more expensive than
        // everything above it, so it only ever runs for a candidate that already survived the raw
        // energy, RNTI-persistence and post-estimation SNR gates. ----
        /* ISAC_PDCCH_NO_PDSCH_DECODE=1 (diagnosis only, default off): run the whole PDCCH scan --
         * FEP, LLR, demap, polar decode, every gate, the CFR tap -- but skip ONLY the passive PDSCH
         * decode. This exists because `pdsch_decode = 0` in the config is NOT the same experiment:
         * that value makes want_decode false, and with sensing off the function then returns at the
         * `!want_dmrs && !want_decode` guard BEFORE any FEP, so it disables the PDCCH scan as well
         * and cannot separate the two costs. Needed to answer PASSIVE_RX_ONLY_HANDOVER.md section
         * 14's open question -- whether the timing runaway at high DL load is caused by the PDSCH
         * decode specifically or by the tap as a whole -- without first writing the consumer thread
         * that section 14.4 proposes. */
        static int s_no_pdsch = -1;
        if (s_no_pdsch < 0) {
          s_no_pdsch = (getenv("ISAC_PDCCH_NO_PDSCH_DECODE") != NULL) ? 1 : 0;
        }
        if (want_decode && !s_no_pdsch && decodes_this_occasion < cfg->pdsch_max_per_slot) {
          if (cfg->pdsch_rv0_only && out.rv != 0) {
            // A retransmission carries only an incremental-redundancy slice of the codeword and is
            // not self-decodable without the earlier round's soft bits -- which a receiver that
            // never saw the first grant does not have. Counted, not attempted.
            g_dec_skip_rv++;
            {
              /* DCIFIELDS (ISAC_DCI_FIELDS=1): dump what we decoded out of an ACCEPTED payload.
               * A high rv!=0 rate on a link without retransmissions is the KNOWN signature of
               * misaligned DCI field widths -- that is exactly what exposed the bwp_indicator/TDA
               * bug in 2026-07-30. Compare these against the gNB's own PDSCH line for the same
               * RNTI before touching any width. */
              static int s_df = -1;
              if (s_df < 0)
                s_df = (getenv("ISAC_DCI_FIELDS") != NULL) ? 1 : 0;
              if (s_df)
                LOG_I(PHY,
                      "DCIFIELDS rnti=0x%x rv=%u mcs=%u ndi=%u harq=%u tda=%u startrb=%u numrb=%u ssym=%u nsym=%u cdm=%u\n",
                      out.rnti, (unsigned)out.rv, (unsigned)out.mcs, (unsigned)out.ndi,
                      (unsigned)out.harq_pid, (unsigned)out.tda_index, (unsigned)out.start_rb,
                      (unsigned)out.num_rb, (unsigned)out.start_symbol, (unsigned)out.num_symbols,
                      (unsigned)out.n_dmrs_cdm_groups);
            }
          } else {
            decodes_this_occasion++;
            const nr_pdsch_passive_grant_t grant = {.rnti       = out.rnti,
                                                    .mcs        = out.mcs,
                                                    .rv         = out.rv,
                                                    .ndi        = out.ndi,
                                                    .harq_pid   = out.harq_pid,
                                                    .mcs_table  = grant_mcs_table,
                                                    .nb_rb_oh   = (uint16_t)cfg->pdsch_xoverhead,
                                                    .tb_scaling = out.tb_scaling,
                                                    // The carrier, never this grant's own frequency
                                                    // reference -- see nr_pdsch_passive_grant_t.
                                                    .bw_tbslbrm = (uint16_t)fp->N_RB_DL,
                                                    // The DEPLOYMENT's mcs-Table, never the
                                                    // format-1_0-forced one -- see the field comment.
                                                    .mcs_table_lbrm = grant_mcs_table_lbrm};
            nr_pdsch_passive_decode_result_t dec;
            // Reuses rxdataF_pdsch: nr_pdsch_passive_decode() FEPs the WHOLE allocation into it,
            // a superset of the single DM-RS symbol already transformed above, so the buffer is
            // simply refilled rather than duplicated (~900 kB at 273 PRB x 4 antennas).
            /* ---- DEFERRED PATH. Hands the job to the consumer and returns immediately: a
             * ~600 B copy plus one atomic store, against the 775 us it replaces. Deliberately does
             * NOT fall back to an in-line decode when the ring is full -- doing so would reintroduce
             * exactly the deadline overrun this exists to remove, and precisely under the load where
             * it hurts most. A full ring is counted (dropped_full) and reported. ---- */
            if (defer) {
              nr_pdsch_passive_job_t job;
              memset(&job, 0, sizeof(job));
              job.dlsch_pdu     = dlsch_pdu;
              job.freq_alloc    = freq_alloc;
              job.grant         = grant;
              job.frame_rx      = proc->frame_rx;
              job.nr_slot_rx    = proc->nr_slot_rx;
              job.gNB_id        = proc->gNB_id;
              /* Preserve the original RF slot across PDCCH -> PDSCH deferral. */
              job.absolute_slot = source_absolute_slot;
              job.rnti          = out.rnti;
              job.rnti_class    = out.rnti_class;
              job.harq_pid_tag  = blind_harq_tag(abs_slot, out.rnti, out.harq_pid);
              job.want_data     = want_data;
              job.fo_hz         = isnan(nr_slot_fep_fo_override_hz)
                  ? (ue->cont_fo_comp ? ue->dl_Doppler_shift + ue->freq_offset : 0.0)
                  : nr_slot_fep_fo_override_hz;  /* receive-thread sample */
              job.sweep_ticket  = sweep_ticket;
              job.bwp_entry     = cand_task[ti].bwp_entry;
              atomic_fetch_add_explicit(&g_enq_class[1][out.rnti_class], 1, memory_order_relaxed);
              ragrant_dump(&dlsch_pdu, &out, grant_mcs_table, css0_occasion, "normal");
              nr_pdsch_passive_queue_enqueue(&job);
              /* The per-candidate channel-estimate allocation is freed at the BOTTOM of this loop,
               * which `continue` skips -- ~917 kB per job at 273 PRB x 4 antennas, and mlockall()
               * makes every byte of it count against RLIMIT_MEMLOCK. Free it here. */
              free(toFree);
              continue; // nothing further to do on this thread for this candidate
            }

            const uint64_t btim_t_pds = btim_on ? btim_now() : 0;
            const nr_pdsch_passive_decode_status_t st =
                nr_pdsch_passive_decode(ue, proc, &dlsch_pdu, &freq_alloc, &grant, rxdataF_pdsch, &dec);
            btim_add(BTIM_PDSCH, btim_t_pds);
            /* Same feedback contract as deferred decoding: unsupported/internal errors are not CRC trials. */
            if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK || st == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL) {
              /* same evidence as the deferred consumer: layout tallies, or DL link health for 0xFFFF */
              nr_pdcch_dci11_layout_feedback(sweep_ticket.layout_index, st == NR_PDSCH_PASSIVE_DECODE_CRC_OK);
              nr_pdsch_cfg_hypothesis_t winner;
              if (nr_pdsch_config_sweep_feedback(&sweep_ticket, st == NR_PDSCH_PASSIVE_DECODE_CRC_OK, &winner))
                LOG_A(PHY, "SENSING: Technique D CONVERGED rnti=0x%x tda=%u S=%u L=%u mask=0x%x table=%u\n",
                      sweep_ticket.rnti, sweep_ticket.tda_index, winner.tda_start, winner.tda_length,
                      winner.dmrs_mask, winner.mcs_table);
            }
            if (st == NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED) {
              g_dec_unsup++;
            } else if (st != NR_PDSCH_PASSIVE_DECODE_ERROR) {
              g_dec_try++;
              if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK) {
                g_dec_ok++;
                LOG_D(PHY, "SENSING: passive PDSCH decode OK (%d.%d) rnti=0x%x mcs=%u rv=%u TBS=%u\n",
                      proc->frame_rx, proc->nr_slot_rx, out.rnti, out.mcs, out.rv, dec.cw.TBS);
                /* Same observation the deferred consumer makes: with deferral off nothing else ever
                 * looks inside the payload, so every overheard timing advance -- a range to the
                 * illuminator obtained without transmitting -- would be silently discarded. */
                if (dec.tb != NULL && dec.cw.TBS > 0)
                  nr_passive_mac_report_ta(out.rnti, out.rnti_class == NR_BLIND_RNTI_CLASS_RA,
                                           proc->frame_rx, proc->nr_slot_rx,
                                           (int)fp->numerology_index, (uint32_t)abs_slot, dec.tb,
                                           dec.cw.TBS / 8); /* TBS is in BITS; the parser walks octets */
                if (want_data) {
                  // The reconstruction chain the attached UE uses, unchanged -- the ONLY difference
                  // is where the verified transport block came from.
                  const uint64_t btim_t_sub = btim_on ? btim_now() : 0;
                  nr_isac_pdsch_data_aided_submit(ue, proc, &dec.cw, &dlsch_pdu, &freq_alloc, out.rnti, dec.tb,
                                                  blind_harq_tag(abs_slot, out.rnti, out.harq_pid), rxdataF_pdsch,
                                                  (double)dec.nvar);
                  btim_add(BTIM_SUBMIT, btim_t_sub);
                  g_data_submits++;
                }
              }
            }
          }
        }
      } else if (nof_re > 0) {
        g_held_snr++;
      }
    }
    free(toFree);
  }

  if (btim_on) {
    btim_add(BTIM_POST, btim_t_post);
    const uint64_t d = btim_now() - btim_occ0;
    g_btim_ns[BTIM_TOTAL] += d;
    g_btim_n[BTIM_TOTAL]++;
    if (d > g_btim_max[BTIM_TOTAL]) {
      g_btim_max[BTIM_TOTAL] = d;
    }
    /* Slot duration in ns from the frame parameters themselves, never a hardcoded 500 us -- the
     * whole point of the comparison is "what fraction of the receive thread's budget did this
     * occasion consume", and that budget is 1 ms / 2^numerology. */
    const uint64_t slot_ns = (fp->slots_per_frame > 0) ? (10000000ull / (uint64_t)fp->slots_per_frame) : 0;
    btim_occasion_total(d, slot_ns);
  }

  const bool sum_due = summary_due_now();
  if (sum_due) {
    /* scanq is all-zero when the scan runs in-line, which is what distinguishes "deferral off" from
     * "deferral on and keeping up" in a log without needing a second line. */
    nr_pdcch_passive_queue_stats_t scanq;
    memset(&scanq, 0, sizeof(scanq));
    nr_pdcch_passive_queue_get_stats(&scanq);
    /* The bootstrap table, raw. bootstrap_rnti=0x0 gates dedicated CORESET verification, the DL
     * length sweep AND the UL PUSCH scan at once, so when it stays zero the whole pipeline is dead
     * downstream -- and an empty table is indistinguishable from a table full of one-sighting
     * entries without printing it. live>0 means something confirmed; used>0 with live=0 means
     * accepts are arriving but never repeating. */
    {
      char boottab[512];
      boottab[0] = '\0';
      nr_pdcch_blind_rnti_bootstrap_dump((uint32_t)abs_slot, boottab, (int)sizeof(boottab));
      LOG_I(PHY, "SENSING: BOOTTABLE %s\n", boottab[0] ? boottab : "(empty)");
    }
    {
      char a0[256], a1[256], a2[256]; int u0 = 0, u1 = 0, u2 = 0;
      for (int k = 0; k < 20; k++) {
        u0 += snprintf(a0 + u0, sizeof(a0) - u0, "%lu%s", g_acc_slot[0][k], k < 19 ? "," : "");
        u1 += snprintf(a1 + u1, sizeof(a1) - u1, "%lu%s", g_acc_slot[1][k], k < 19 ? "," : "");
        u2 += snprintf(a2 + u2, sizeof(a2) - u2, "%lu%s", g_occ_slot[k], k < 19 ? "," : "");
      }
      LOG_I(PHY, "SENSING: ACCSLOT occasions=[%s] si=[%s] other=[%s]\n", a2, a0, a1);
      LOG_I(PHY,
            "SENSING: ENQCLASS deferred[C=%lu TC=%lu SI=%lu RA=%lu P=%lu] normal[C=%lu TC=%lu SI=%lu RA=%lu P=%lu]\n",
            atomic_load_explicit(&g_enq_class[0][0], memory_order_relaxed),
            atomic_load_explicit(&g_enq_class[0][1], memory_order_relaxed),
            atomic_load_explicit(&g_enq_class[0][2], memory_order_relaxed),
            atomic_load_explicit(&g_enq_class[0][3], memory_order_relaxed),
            atomic_load_explicit(&g_enq_class[0][4], memory_order_relaxed),
            atomic_load_explicit(&g_enq_class[1][0], memory_order_relaxed),
            atomic_load_explicit(&g_enq_class[1][1], memory_order_relaxed),
            atomic_load_explicit(&g_enq_class[1][2], memory_order_relaxed),
            atomic_load_explicit(&g_enq_class[1][3], memory_order_relaxed),
            atomic_load_explicit(&g_enq_class[1][4], memory_order_relaxed));
    }
    LOG_I(PHY,
         "SENSING: blind PDCCH monitor summary: occasions=%lu candidates=%lu accepts=%lu "
         "dci10[accepts=%lu C=%lu TC=%lu SI=%lu RA=%lu P=%lu] dci01[accepts=%lu rejects=%lu] "
         "dci00[accepts=%lu rejects=%lu] ulscan[sched=%lu crc_hit=%lu disc=%lu] "
         "held[energy=%lu dmrs=%lu persist=%lu snr=%lu mismatch=%lu rnti_set=%lu] efloor=%.2f cfr_submits=%lu "
         "pdsch_decode[try=%lu crc_ok=%lu (%.1f%%) skip_rv=%lu unsup=%lu over_cap=%lu data_submits=%lu] "
         "scanq[queued=%lu done=%lu drop_full=%lu drop_stale=%lu maxlag=%lu] "
         "last_reject=\"%s\" last_reject_rnti=0x%x\n",
         (unsigned long)g_occasions_run, (unsigned long)g_candidates_run, (unsigned long)g_accepts,
         (unsigned long)g_accepts_10,
         (unsigned long)g_accepts_class[NR_BLIND_RNTI_CLASS_C],
         (unsigned long)g_accepts_class[NR_BLIND_RNTI_CLASS_TC],
         (unsigned long)g_accepts_class[NR_BLIND_RNTI_CLASS_SI],
         (unsigned long)g_accepts_class[NR_BLIND_RNTI_CLASS_RA],
         (unsigned long)g_accepts_class[NR_BLIND_RNTI_CLASS_P],
         (unsigned long)g_ul_accepts, (unsigned long)g_ul_rejects,
         (unsigned long)g_ul00_accepts, (unsigned long)g_ul00_rejects,
         (unsigned long)g_ul_sched, (unsigned long)g_ul_crc_hit, (unsigned long)g_ul_disc_call,
         (unsigned long)g_held_energy, (unsigned long)g_held_dmrs, (unsigned long)g_held_persist, (unsigned long)g_held_snr,
         (unsigned long)g_held_mismatch, (unsigned long)g_held_rnti_set,
         g_energy_floor,
         (unsigned long)g_cfr_submits,
         (unsigned long)g_dec_try, (unsigned long)g_dec_ok,
         g_dec_try ? (100.0 * (double)g_dec_ok / (double)g_dec_try) : 0.0,
         (unsigned long)g_dec_skip_rv, (unsigned long)g_dec_unsup, (unsigned long)g_dec_over_cap,
         (unsigned long)g_data_submits,
         (unsigned long)scanq.queued, (unsigned long)scanq.processed, (unsigned long)scanq.dropped_full,
         (unsigned long)scanq.dropped_stale, (unsigned long)scanq.max_lag_slots,
         g_last_reject_reason ? g_last_reject_reason : "(none yet)",
         g_last_reject_rnti);
  }

    /* Distinct decode-parameter census. Printed with the periodic summary rather than only at
     * teardown, because a run that hits the RFSTALL watchdog leaves via exit(3) and would otherwise
     * take its census with it -- and the long, high-load runs are exactly the specimens worth
     * diffing (section 23.3). */
    /* PERIOD-GUARDED. These used to sit OUTSIDE the `% NR_PDCCH_BLIND_SUMMARY_PERIOD_OCC` block that
     * opens above, so they fired on EVERY monitoring occasion instead of every 1000 -- measured
     * 2026-08-24 as 790,798 PARMSET lines and a 163 MB log per 90 s run. That is not merely untidy:
     * it is ~1600 formatted LOG_I calls per second issued from the process whose PHY receive thread
     * must hit a 500 us deadline, i.e. an instrument heavy enough to cause the very timing runaways
     * it was added to diagnose. PARMSET is additionally throttled again on top, because it prints a
     * whole table and its content changes only when the gNB starts using a new configuration. */
    if (want_decode && (g_occasions_run % (NR_PDCCH_BLIND_SUMMARY_PERIOD_OCC * 50)) == 0) {
      nr_pdsch_passive_parmset_dump();
    }
    /* Which HALF of the decode is failing -- see §29.1. Cheap (one line) and period-guarded. */
    if (want_decode && sum_due) {
      nr_pdsch_passive_ldpc_stats_dump();
    }

    /* Uplink receive census. Also the reference that forces PHY_NR_PASSIVE_UL into the link: a
     * static library contributes nothing until something needs a symbol from it, so without a
     * caller a clean build proves only that the sources COMPILE, not that the gNB PUSCH receive
     * chain resolves inside this binary. */
    if (sum_due) {
      nr_pusch_grant_book_stats_dump();
    }
    if (nr_pdsch_passive_queue_running() && sum_due) {
      nr_pdsch_passive_queue_stats_t qs;
      nr_pdsch_passive_queue_get_stats(&qs);
      /* Every field here is a reason a queued job did NOT become a decode, so a shortfall in
       * `decoded` is attributable rather than merely visible. max_lag is the number that says
       * whether the configured depth was right: it must stay well under slots_per_frame. */
      LOG_I(PHY,
            "SENSING: PDSCHQ queued=%lu decoded=%lu crc_ok=%lu (%.1f%%) dropped[full=%lu stale=%lu] "
            "max_lag_slots=%lu/%d slot_groups=%lu/%lu multi-grant slots dropped_narrow=%lu\n",
            (unsigned long)qs.queued, (unsigned long)qs.decoded, (unsigned long)qs.crc_ok,
            qs.decoded ? (100.0 * (double)qs.crc_ok / (double)qs.decoded) : 0.0,
            (unsigned long)qs.dropped_full, (unsigned long)qs.dropped_stale,
            (unsigned long)qs.max_lag_slots, fp->slots_per_frame, (unsigned long)qs.slot_groups,
            (unsigned long)qs.batches_multi, (unsigned long)qs.dropped_narrow);
      char rc[256]; nr_pdsch_passive_queue_rnti_census(rc, sizeof(rc));
      LOG_I(PHY, "SENSING: PDSCHQ per-rnti%s\n", rc);
    }
    /* Explicit acquisition/discovery state (Gate 4/5 groundwork). Every input is a read of a
     * counter/boolean that ALREADY exists at this point -- no new measurement, no control-flow
     * change. Transitions log themselves at LOG_A inside nr_passive_acq_update(); the heartbeat
     * below is period-guarded like the census lines above (see the PARMSET note on why
     * per-occasion logging here is not merely untidy).
     *
     * The UPDATE is not tied to that log period any more. It used to be, and on a 4 s raw capture
     * that meant zero updates: the whole 1000-occasion summary never fired, the state never left
     * SEARCHING_PDCCH, and nothing was logged. Now three cheap milestone booleans are compared
     * EVERY occasion (3 compares), and a flip forces a full update -- costlier sweep snapshots
     * included -- immediately. Flips happen a handful of times per run, so RT cost stays
     * negligible. Convergence winners are still sampled at the period: they take tens of seconds
     * to form, so ~0.6 s resolution loses nothing. Unsynchronised statics follow this function's
     * existing single-caller assumption (g_occasions_run, raw_dl_count). */
    static bool acq_first_logged = false;
    if (!acq_first_logged) {
      acq_first_logged = true;
      LOG_I(PHY, "SENSING: ACQ first monitored occasion (occasions_run=%lu)\n", (unsigned long)g_occasions_run);
    }
    const bool acq_len = g_length_found;
    const bool acq_cs  = !cfg->autodiscover || nr_pdcch_blind_monitor_autodiscover_extent_verified();
    const bool acq_bwp = ul_opts.bwp_size > 0;
    static int acq_last_sig = -1;
    const int acq_sig = (int)acq_len | ((int)acq_cs << 1) | ((int)acq_bwp << 2);
    const bool acq_period = sum_due;
    if (acq_period || acq_sig != acq_last_sig) {
      acq_last_sig = acq_sig;
      const nr_pdcch_ul_discovery_snapshot_t uls = nr_pdcch_ul_discovery_snapshot();
      const nr_passive_acq_inputs_t acq_in = {
        .pdcch_length_found      = acq_len,
        .coreset_extent_verified = acq_cs,
        .ul_bwp_known            = acq_bwp,
        .dl_search_winners       = (uint64_t)nr_pdsch_config_sweep_settled_count(),
        .ul_width_winners        = (uint64_t)uls.width_winners,
        .ul_interp_winners       = (uint64_t)uls.interp_winners,
      };
      nr_passive_acq_update(&acq_in);
      nr_pdcch_blind_infer_search_space();
      const nr_passive_acq_snapshot_t acq = nr_passive_acq_snapshot();
      if (acq_period)
      {
      const nr_pdsch_xoverhead_state_t xo = nr_pdsch_xoverhead_snapshot();
      const nr_dmrs_id_state_t *dd = nr_pdsch_passive_dl_dmrs_id(), *du = nr_pusch_passive_ul_dmrs_id();
      LOG_I(PHY, "SENSING: ACQ state=%s time_in_state=%lu transitions=%lu regressions=%lu "
                 "in[len=%d coreset=%d ul_bwp=%d dl_win=%lu ul_win[w=%lu i=%lu]] "
                 "uldisc[gen=%lu raw=%d wcls=%d icls=%d wtrials=%lu itrials=%lu rejected_fb=%lu] "
                 "carrier=%s xoh[assumed=%u %s crc_ok=%u] dmrs_id[dl=%s%d/%u ul=%s%d/%u]\n",
            nr_passive_acq_state_name(acq.state), (unsigned long)acq.time_in_state,
            (unsigned long)acq.transitions, (unsigned long)acq.consecutive_regressions,
            acq_in.pdcch_length_found, acq_in.coreset_extent_verified, acq_in.ul_bwp_known,
            (unsigned long)acq_in.dl_search_winners, (unsigned long)acq_in.ul_width_winners,
            (unsigned long)acq_in.ul_interp_winners,
            (unsigned long)uls.generation, uls.raw_samples, uls.width_classes, uls.interp_classes,
            (unsigned long)uls.width_trials, (unsigned long)uls.interp_trials,
            (unsigned long)uls.rejected_feedback,
            acq.carrier_verified > 0 ? "CONFIRMED" : acq.carrier_verified < 0 ? "MISMATCH" : "unchecked",
            xo.assumed, xo.confirmed ? "CONFIRMED" : "unresolved", xo.crc_ok_seen,
            dd->decided ? (dd->best_id == dd->assumed_id ? "CONFIRMED:" : "MISMATCH:") : "pending:", dd->best_id, dd->grants,
            du->decided ? (du->best_id == du->assumed_id ? "CONFIRMED:" : "MISMATCH:") : "pending:", du->best_id, du->grants);
      }
    }

    if (btim_on && sum_due) {
      char rep[1300];
      int u = 0;
      for (int k = 0; k < BTIM_N && u < (int)sizeof(rep) - 90; k++) {
        u += snprintf(rep + u, sizeof(rep) - u, "%s[n=%lu mean=%.1fus max=%.1fus tot=%.2fs] ",
                      kBtimName[k], (unsigned long)g_btim_n[k],
                      g_btim_n[k] ? (double)g_btim_ns[k] / (double)g_btim_n[k] / 1000.0 : 0.0,
                      (double)g_btim_max[k] / 1000.0, (double)g_btim_ns[k] / 1e9);
      }
      const uint64_t slot_ns = (fp->slots_per_frame > 0) ? (10000000ull / (uint64_t)fp->slots_per_frame) : 0;
      LOG_I(PHY, "SENSING: BTIM %s\n", rep);
      LOG_I(PHY,
            "SENSING: BTIM occ_total_us_hist <50=%lu <100=%lu <200=%lu <400=%lu <800=%lu <1600=%lu "
            "<3200=%lu >=3200=%lu over_slot(%luus)=%lu/%lu\n",
            (unsigned long)g_btim_hist[0], (unsigned long)g_btim_hist[1], (unsigned long)g_btim_hist[2],
            (unsigned long)g_btim_hist[3], (unsigned long)g_btim_hist[4], (unsigned long)g_btim_hist[5],
            (unsigned long)g_btim_hist[6], (unsigned long)g_btim_hist[7],
            (unsigned long)(slot_ns / 1000), (unsigned long)g_btim_over_slot,
            (unsigned long)g_btim_n[BTIM_TOTAL]);
    }
}
