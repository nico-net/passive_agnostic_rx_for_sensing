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
#include <stdlib.h> // getenv/atoi for the env-gated diagnostics in this file
#include <time.h> // clock_gettime for the rnti_seen correlation line below

#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"

#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h" // nr_pdcch_demapping_deinterleaving/_unscrambling/_generate_llr
#include "PHY/MODULATION/modulation_UE.h"               // nr_slot_fep
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"          // nr_pdsch_channel_estimation
#include "PHY/TOOLS/tools_defs.h"                        // allocCast2D/fourDimArray_t
#include "PHY/NR_UE_ISAC/nr_isac.h"                      // nr_isac_submit_cfr/_enabled/_source_enabled
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.h"  // passive PDSCH decode (data-aided source)
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
                                            nr_pdcch_blind_result_t *out, uint8_t *ids, int max)
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
  static __thread int order[NR_DCI11_LAYOUT_MAX];
  static __thread double sc[NR_DCI11_LAYOUT_MAX];
  int no = 0;
  for (int i = 0; i < r->n_hyp; i++)
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
    static __thread int rot[NR_DCI11_LAYOUT_MAX];
    for (int k = 0; k < no; k++) rot[k] = order[(start + k) % no];
    memcpy(order, rot, (size_t)no * sizeof(order[0]));
  }
  /* Above the hand-over limit only `max` of the live set fit one grant's trial list; rotate the
   * window over the score-sorted list so every hypothesis gets probed, best ones most often. */
  static __thread int s_rot = 0;
  const int start = (no > max) ? (s_rot++ % (no - max + 1)) : 0;
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
    out[count] = parsed;
    ids[count++] = (uint8_t)i;
  }
  return count;
}

/* ---- DCI 0_1 layout, stage 1 (observe-only, mirrors the 1_1 observer below). The uplink grant's
 * field widths are set by RRC switches this receiver cannot read; every layout whose total equals
 * the observed 0_1 length is a hypothesis and each accepted payload prunes by plausibility. */
static nr_dci11_resolver_t g_dci01_resolver;
static int g_dci01_state = 0;   /* 0 = not armed, 1 = armed, -1 = no legal layout at this length */
static uint64_t g_dci01_seen = 0;
static void nr_pdcch_dci01_layout_observe(uint16_t ul_bwp_size, int ul_tda_count,
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
    static nr_dci01_layout_t hyp[NR_DCI11_LAYOUT_MAX];
    static nr_dci11_offsets_t off[NR_DCI11_LAYOUT_MAX];
    const int n = nr_dci01_layout_enumerate(riv_bits, tda_bits, dci_length, hyp, off, NR_DCI11_LAYOUT_MAX);
    if (n <= 0 || nr_dci_resolver_init_from_offsets(&g_dci01_resolver, ul_bwp_size, off, n) <= 0) {
      LOG_W(PHY, "SENSING: DCI01_LAYOUT no legal layout sums to dci_length=%u at ul_bwp_size=%u "
                 "tda_bits=%u -- one of those three is wrong for this cell\n",
            dci_length, (unsigned)ul_bwp_size, tda_bits);
      g_dci01_state = -1;
      return;
    }
    if (ul_tda_count > 0 && ul_tda_count < 16)
      nr_dci11_resolver_set_tda_count(&g_dci01_resolver, (uint8_t)ul_tda_count);
    LOG_I(PHY, "SENSING: DCI01_LAYOUT armed: %d layouts consistent with dci_length=%u (riv=%u bits, tda=%u bits)\n",
          n, dci_length, riv_bits, tda_bits);
    g_dci01_state = 1;
  }
  nr_dci11_resolver_observe(&g_dci01_resolver, payload);
  if ((++g_dci01_seen % 4000) == 0) {
    LOG_A(PHY, "SENSING: DCI01_LAYOUT n=%llu observed | %d of %d layouts still plausible\n",
          (unsigned long long)g_dci01_seen, g_dci01_resolver.n_alive, g_dci01_resolver.n_hyp);
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
    const int n = nr_dci11_resolver_init(&g_dci11_resolver, cfg->bwp_size, riv_bits, tda_bits,
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
    LOG_I(PHY, "SENSING: DCI11_LAYOUT armed: %d layouts consistent with dci_length=%u "
               "(riv=%u bits, tda=%s)\n", n, dci_length, riv_bits,
          tda_bits == NR_DCI11_TDA_UNKNOWN ? "0..4 bits (searched)" : "configured");
    g_dci11_state = 1;
  }
  nr_dci11_resolver_observe(&g_dci11_resolver, payload);
  if ((++g_dci11_seen % 4000) != 0) {
    return;
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
      uint32_t ok = 0, tr = 0;
      const uint64_t key = (g_pdsch_configuration ^ (uint64_t)(i + 1)) * UINT64_C(1099511628211);
      nr_pdsch_config_sweep_context_stats(key, 0 /* any rnti */, 0, cfg->dmrs_typeA_position, &ok, &tr);
      u += snprintf(eb + u, sizeof(eb) - u, "[%d t%ub%dm%dx%da%d%cp%d:%u/%u]", i, (unsigned)r->off[i].tda_bits,
                    f.bwp_indicator_bits, f.vrb_to_prb_bits, f.tb2_bits, f.antenna_ports_bits,
                    f.dmrs_config_type ? 'B' : 'A', f.tci_bits, ok, tr);
    }
    LOG_A(PHY, "SENSING: DCI11_STAGE2 %s (alive=%d, hands over at <=%d) tb_crc ok/trials per live layout: %s\n",
          r->n_alive <= (g_dci11_cfg_alive ? 4 : NR_DCI11_STAGE2_MAX_ALIVE) ? "DRIVING the extractor" : "waiting for stage 1 to prune",
          r->n_alive, NR_DCI11_STAGE2_MAX_ALIVE, eb);
  }
}

static nr_pdcch_dci_length_sweep_state_t g_dl_length_state;

static void dl_discovery_invalidate(void)
{
  g_length_swept = g_length_found = false;
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
#define AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS 500
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
static uint64_t g_held_mismatch = 0; // migrated from NRSniffer: rejected by the adaptive mismatched-bits gate

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
#define BTIM_N       8
static const char *const kBtimName[BTIM_N] = {"fep_llr", "demap", "prepass", "decode",
                                              "chest",   "pdsch", "submit",  "TOTAL"};
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
  return (cfg && cfg->coreset_type == 1) ? 0 : 1;
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
typedef struct {
  const c16_t *e_rx;
  uint8_t      L;
} nr_pdcch_autodiscover_cand_t;
/* UL length evidence is independent of both the DL sweep and field interpretation. */
typedef struct {
  const nr_pdcch_autodiscover_cand_t *cand;
  int count;
  uint16_t rnti, scrambling_rnti, dmrs_id;
} ul_length_ctx_t;
static bool ul_length_score(int len, int trial, uint16_t *rnti, uint32_t *hash, void *opaque)
{
  const ul_length_ctx_t *ctx=opaque;
  const nr_pdcch_autodiscover_cand_t *c=&ctx->cand[trial%ctx->count];
  int16_t llr[16*108];
  nr_pdcch_unscrambling((c16_t *)c->e_rx,ctx->scrambling_rnti,c->L*108,ctx->dmrs_id,llr);
  nr_pdcch_blind_ul_result_t out;
  /* Use a confirmed RNTI, not a DL-calibrated plausibility false-alarm floor. */
  if(!nr_pdcch_blind_decode_raw_01(llr,c->L,len,ctx->rnti,ctx->rnti,&out)) return false;
  *rnti=out.rnti;
  *hash=(uint32_t)out.raw_payload ^ (uint32_t)(out.raw_payload>>32);
  return true;
}
static nr_pdcch_dci_length_bank_t ul_lengths;
static uint64_t ul_geometry;
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
} nr_pdcch_autodiscover_sweep_ctx_t;

static bool nr_pdcch_autodiscover_length_scorer(int dci_length, int trial_idx, uint16_t *rnti_out,
                                                uint32_t *payload_hash_out, void *user_ctx)
{
  const nr_pdcch_autodiscover_sweep_ctx_t *ctx = (const nr_pdcch_autodiscover_sweep_ctx_t *)user_ctx;
  if (ctx == NULL || ctx->n_cand <= 0) {
    return false;
  }
  const nr_pdcch_autodiscover_cand_t *c = &ctx->cand[trial_idx % ctx->n_cand];
  // Same unscramble step nr_pdcch_blind_cand_worker_body() below uses on the identical cursor
  // (t->e_rx from the same pdcch_e_rx[]/e_rx_cand_idx walk), just with `dci_length` substituted
  // for the hypothesis under test instead of the (as yet unknown) real one.
  int16_t tmp_e[16 * 108];
  nr_pdcch_unscrambling((c16_t *)c->e_rx, ctx->scrambling_rnti, (uint32_t)(c->L * 108), ctx->dmrs_scrambling_id,
                        tmp_e);
  nr_pdcch_blind_raw_result_t out;
  if(!nr_pdcch_blind_decode_raw_11(tmp_e,c->L,(uint16_t)dci_length,ctx->rnti_min,ctx->rnti_max,&out))
    return false;
  /* Never ask a layout-dependent extractor to judge a length. An unknown TDA
   * width previously rejected genuine CRC-recovered 47-bit grants against a
   * presumed 51-bit field list, so discovery could never reach interpretation. */
  *rnti_out=out.rnti;
  *payload_hash_out=(uint32_t)out.payload ^ (uint32_t)(out.payload>>32);

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
  nr_pdcch_blind_raw_result_t dl_raw;
  bool         ul_auto; // raw decode; sequential controller interprets the CRC-verified bits
  uint8_t      ul_scan; // 1 = interpret this candidate as DCI 0_1; `format` is then meaningless
  const nr_pdcch_blind_ul_opts_t *ul_opts;
  nr_pdcch_blind_ul_result_t ul_out; // OUTPUT when ul_scan
  nr_pdcch_blind_result_t out; // OUTPUT
  bool         ok;             // OUTPUT
  int8_t       bwp_entry;      // passive BWP entry this length belongs to (0 = the configured BWP)
  uint8_t      bwp_probe;      // 1 = raw decode only: BWP discovery / DM-RS scoring probe
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
  nr_pdcch_unscrambling((c16_t *)t->e_rx, t->scrambling_rnti, (uint32_t)(t->L * 108), t->dmrs_scrambling_id, tmp_e);
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

void nr_pdcch_blind_monitor_process(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc)
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
  const bool isac_on   = nr_isac_enabled() != 0;
  const bool want_dmrs = isac_on && nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DMRS_BLIND);
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
  if (!want_dmrs && !want_decode) {
    return;
  }

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
  if (cfg->autodiscover && !nr_pdcch_blind_monitor_autodiscover_done()) {
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
    nr_pdcch_blind_monitor_autodiscover_step(rxdataF_disc[0], fp->ofdm_symbol_size, fp->N_RB_DL,
                                             fp->first_carrier_offset, (uint16_t)fp->Nid_cell,
                                             proc->nr_slot_rx, disc_symbol, abs_slot_now);
    return;  // geometry not ready (or just became ready this call) -- no candidate decode this call
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
  if (rem < (uint32_t)cfg->ss_monitoring_slot_offset || rem >= (uint32_t)cfg->ss_monitoring_slot_offset + ss_dur) {
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
void nr_pdcch_blind_monitor_run_occasion(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc,
                                         bool serial_candidates, long source_absolute_slot)
{
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
  static uint64_t previous_geometry;
  const uint64_t geometry = nr_pdcch_blind_monitor_autodiscover_generation();
  if (cfg->autodiscover && geometry != previous_geometry) {
    dl_discovery_invalidate();
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
  rel15->coreset.ShiftIndex        = (uint8_t)cfg->coreset_shift_index;
  rel15->coreset.pdcch_dmrs_scrambling_id = cfg->coreset_pdcch_dmrs_scrambling_id;
  /* PDCCH data scrambling is c_init = (n_RNTI*2^16 + n_ID), where n_RNTI is the C-RNTI only when
   * the search space is UE-specific AND the CORESET carries pdcch-DMRS-ScramblingID; otherwise 0.
   * VERIFIED for this cell from the gNB debug log, on the very lines carrying the DCI 1_1 grants:
   * "nid_pdcch_data=2 nid_pdcch_dmrs=2 nrnti_pdcch_data=0" -- so n_RNTI = 0 and n_ID = PCI = 2,
   * which is what this module already assumed. (Had it been non-zero, blind decoding of the USS
   * would need the C-RNTI *before* it can descramble -- the very thing the scan is recovering --
   * i.e. a structural blocker rather than a tuning error. It is not the case here.) */
  rel15->coreset.scrambling_rnti   = 0;
  if (cfg->ss_first_symbol < 0 || cfg->ss_first_symbol >= fp->symbols_per_slot) {
    return;
  }
  rel15->coreset.StartSymbolBitmap = (uint16_t)(1u << (fp->symbols_per_slot - 1 - cfg->ss_first_symbol));

  int n_rb = 0, cset_start = 0;
  get_coreset_rballoc(rel15->coreset.frequency_domain_resource, &n_rb, &cset_start);
  if (n_rb < 12 || rel15->coreset.duration < 1) { // need >=2 AL2 candidates' worth of CCEs to bother
    return;
  }
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
    static int s_fmt_logged = 0;
    if (!s_fmt_logged) {
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
  if (!nr_passive_samples_valid(
          atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed),
          source_absolute_slot, fp->slots_per_frame))
    return; /* No discovery/CRC evidence from an overwritten CORESET window. */

  /* Blind CSI-RS search, observe-only (ISAC_CSIRS_BLIND=1, default off). Placed HERE, after the
   * sample-lifetime check, deliberately: scoring a candidate against a window the producer has
   * already overwritten would feed the correlator next frame's samples and manufacture hits that
   * no periodicity test could distinguish from a real resource. It reads rxdataF and writes
   * nothing the decoder consumes, so it cannot affect decoding. */
  nr_csirs_blind_rt_slot(ue, proc->nr_slot_rx,
                         source_absolute_slot >= 0 ? (uint32_t)source_absolute_slot : 0u,
                         rxdataF);

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
  const uint64_t btim_t_dmp = btim_on ? btim_now() : 0;
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
  /* DIAGNOSTIC (2026-09-06, env-gated ISAC_FORCE_DCI_LEN=<n>): pin dci_length instead of sweeping
   * for it. Separates two hypotheses that the "0 accepts under autodiscover" symptom cannot
   * distinguish on its own: (a) the rest of the autodiscover-derived config is sound and only the
   * SWEEP fails to find the right length, vs (b) something ELSE in that config breaks decode, in
   * which case even the known-correct length recovers nothing. The manual conf decodes this cell at
   * 48.6-82.8% PDSCH CRC with dci_length=47, so forcing 47 here is a like-for-like test. */
  if (cfg->autodiscover && nr_pdcch_blind_monitor_autodiscover_done() && !g_length_swept) {
    static int s_force_len = -1;
    if (s_force_len < 0) {
      const char *e = getenv("ISAC_FORCE_DCI_LEN");
      s_force_len = (e != NULL) ? atoi(e) : 0;
    }
    if (s_force_len > 0) {
      nr_pdcch_blind_monitor_autodiscover_set_dci_length(s_force_len);
      g_length_swept = true;
      g_length_found = true;  // an operator-supplied length is as trustworthy as a swept one
      LOG_A(PHY, "SENSING: Phase 3 autodiscover -- dci_length FORCED to %d (ISAC_FORCE_DCI_LEN), "
                 "sweep skipped\n", s_force_len);
    }
  }
  if (cfg->autodiscover && nr_pdcch_blind_monitor_autodiscover_done() && !g_length_swept) {
    nr_pdcch_autodiscover_cand_t disc_cand[64];
    int disc_n_cand = 0;
    {
      int idx = 0;
      for (int c = 0; c < rel15->number_of_candidates && disc_n_cand < 64; c++) {
        const int L         = rel15->L[c];
        const int n_re_cand = NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * L * 6;
        disc_cand[disc_n_cand].e_rx = &pdcch_e_rx[idx];
        disc_cand[disc_n_cand].L    = (uint8_t)L;
        disc_n_cand++;
        idx += n_re_cand;
      }
    }
    if (disc_n_cand > 0) {
      uint16_t bootstrap_rnti = 0;
      uint8_t  bootstrap_class = 0xFF;
      uint32_t age = 0;
      nr_pdcch_blind_monitor_confirmed_rnti(abs_slot, &bootstrap_rnti, &bootstrap_class, &age);
      // Only bootstrap_rnti feeds the sweep below; the function unconditionally writes through
      // all three out-params (see nr_pdcch_blind_rnti_bootstrap.c), so these two can't be NULL.
      (void)bootstrap_class;
      (void)age;

      nr_pdcch_autodiscover_sweep_ctx_t sweep_ctx = {
          .cand                = disc_cand,
          .n_cand              = disc_n_cand,
          .bwp_size            = (uint16_t)cfg->bwp_size,
          .dmrs_typeA_position = (uint8_t)cfg->dmrs_typeA_position,
          .rnti_min            = cfg->rnti_min,
          .rnti_max            = cfg->rnti_max,
          .extract_opts        = &cfg->extract,
          .scrambling_rnti     = rel15->coreset.scrambling_rnti,
          .dmrs_scrambling_id  = rel15->coreset.pdcch_dmrs_scrambling_id,
      };
      // DCI 1_1 lengths land in roughly 30-70 bits on any deployment this project has seen
      // (nr_pdcch_dci_length_sweep.h's own file comment), but this codebase itself rejects any
      // dci_length > 63 before ever decoding -- so the upper bound is 63, not 70: lengths 64-70
      // are guaranteed-wasted trials (7 of 41 hypotheses, ~17% of the sweep's budget, for zero
      // possible acceptance).
      const int found_len = nr_pdcch_dci_length_sweep_feed(&g_dl_length_state, nr_pdcch_autodiscover_length_scorer,
                                                            &sweep_ctx, disc_n_cand, 30, 63, bootstrap_rnti);
      if (found_len > 0) {
        nr_pdcch_blind_monitor_autodiscover_set_dci_length(found_len);
        g_length_swept = true;
        g_length_found = true;
        LOG_A(PHY, "SENSING: Phase 3 autodiscover -- dci_length locked at %d (bootstrap_rnti=0x%x, "
                   "occasions_fed=%d)\n", found_len, bootstrap_rnti, g_dl_length_state.occasions_fed);
        return; /* Rebuild the next occasion with the newly selected length. */
      } else if (g_dl_length_state.occasions_fed >= AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS) {
        // Bounded give-up (mirrors this file's other bounded-cost designs): a cell where the
        // sweep genuinely never reaches significance (e.g. real accept rate far below what even
        // this many occasions can establish) must not run this indefinitely.
        const int occasions = g_dl_length_state.occasions_fed;
        nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start + cfg->coreset_rb_offset);
        dl_discovery_invalidate();
        LOG_W(PHY, "SENSING: DCI length unresolved after %d occasions; next CORESET hypothesis, "
                   "no offset blacklist (bootstrap_rnti=0x%x)\n", occasions, bootstrap_rnti);
        return; /* Old LLRs must never be decoded against the new geometry. */
      }
    }
  }

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
  if (cfg->autodiscover && nr_pdcch_blind_monitor_autodiscover_extent_step(abs_slot)) {
    dl_discovery_invalidate();
    return;
  }
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
  const int n_known_ul = nr_pdcch_blind_monitor_confirmed_rnti_set(abs_slot,
      known_ul, NR_PDCCH_BLIND_MAX_UE);
  static uint32_t ul_ue_cursor;
  /* Bounded round-robin: one UL candidate set per occasion, fair across confirmed UEs.
   * Each UE owns its length and interpretation state; newest sighting cannot erase it. */
  if (n_known_ul > 0)
    boot_rnti = known_ul[(ul_ue_cursor++) % n_known_ul];

  /* full_auto=0 never feeds a search or replaces a manual UL option/length.
   * Auto has no silent fallback: unresolved searches do not emit guessed grants. */
  bool ul_ready=false;
  pthread_mutex_lock(&ul_length_lock);
  uint64_t geom=UINT64_C(1469598103934665603);
  const int geometry_fields[]={cfg->bwp_start,cfg->bwp_size,cfg->coreset_rb_offset,
      cfg->coreset_freq_domain,cfg->coreset_duration,cfg->coreset_reg_bundle_size,
      cfg->coreset_interleaver_size,cfg->coreset_shift_index,cfg->ul.phy_cell_id,
      rel15->coreset.scrambling_rnti,rel15->coreset.pdcch_dmrs_scrambling_id};
  for(unsigned i=0;i<sizeof(geometry_fields)/sizeof(geometry_fields[0]);++i)
    geom=(geom^(uint32_t)geometry_fields[i])*UINT64_C(1099511628211);
  geom ^= nr_pdcch_blind_monitor_autodiscover_generation();
  if (!cfg->dl_full_auto || !scan_01 || geom != ul_geometry) {
    memset(&ul_lengths, 0, sizeof(ul_lengths));
    nr_pdcch_ul_discovery_reset();
    ul_geometry=geom;
  }
  nr_pdcch_dci_length_context_t *ulc = boot_rnti
      ? nr_pdcch_dci_length_context(&ul_lengths, geom, boot_rnti) : NULL;
  if(cfg->dl_full_auto && scan_01 && boot_rnti && (!cfg->autodiscover || nr_pdcch_blind_monitor_autodiscover_extent_verified())) {
    if(!ulc->found && !ulc->exhausted) {
      nr_pdcch_autodiscover_cand_t candidates[64];
      int count=0, offset=0;
      for(int c=0;c<rel15->number_of_candidates && count<64;++c) {
        const int L=rel15->L[c];
        candidates[count++]=(nr_pdcch_autodiscover_cand_t){.e_rx=&pdcch_e_rx[offset],.L=L};
        offset+=NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS*L*6;
      }
      if(count) {
        ul_length_ctx_t ctx={.cand=candidates,.count=count,.rnti=boot_rnti,
                            .scrambling_rnti=rel15->coreset.scrambling_rnti,
                            .dmrs_id=rel15->coreset.pdcch_dmrs_scrambling_id};
        const int found=nr_pdcch_dci_length_sweep_feed(&ulc->state,ul_length_score,&ctx,
                                                     count,30,63,boot_rnti);
        /* A single matching decode cannot rule out a degenerate polar fixed point.
         * Require distinct UL payloads before trusting the shared engine's shortcut. */
        int supported_lengths=0;
        for(int len=30;len<=63;++len)
          if(ulc->state.n_distinct[len]>1 && ulc->state.bootstrap_hits[len]>=3)
            ++supported_lengths;
        if(found>0 && supported_lengths==1 && ulc->state.n_distinct[found]>1 &&
           ulc->state.bootstrap_hits[found]>=3) {
          ulc->found=found;
          LOG_A(PHY,"UL automatic DCI length locked: %d rnti=0x%x\n",found,boot_rnti);
        } else if(ulc->state.occasions_fed>=AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS) {
          ulc->exhausted=true;
          LOG_W(PHY,"UL automatic length unresolved after %d occasions\n",ulc->state.occasions_fed);
        }
      }
    }
    ul_ready=ulc->found>0;
    if(ul_ready) dci01_length=ulc->found;
  }
  pthread_mutex_unlock(&ul_length_lock);
  const uint64_t btim_t_pre = btim_on ? btim_now() : 0;
  {
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
      if (scan_11 && nof_tasks < (int)(sizeof(cand_task) / sizeof(cand_task[0]))) {
        cand_task[nof_tasks] = base_task;
        nof_tasks++;
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
        nof_tasks++;
      }
      if (scan_01 && (!cfg->dl_full_auto || ul_ready) && nof_tasks < (int)(sizeof(cand_task) / sizeof(cand_task[0]))) {
        cand_task[nof_tasks]            = base_task;
        cand_task[nof_tasks].dci_length = dci01_length;
        cand_task[nof_tasks].ul_scan    = 1;
        cand_task[nof_tasks].ul_opts    = &ul_opts;
        cand_task[nof_tasks].ul_auto    = cfg->dl_full_auto != 0;
        if(cfg->dl_full_auto) {
          cand_task[nof_tasks].rnti_min=boot_rnti;
          cand_task[nof_tasks].rnti_max=boot_rnti;
        }
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

  // ---- Phase 1 (parallel): fan the independent unscramble+decode work out across the UE's thread
  // pool. pushTpool() runs the task inline if the pool has zero worker threads configured (its own
  // documented fallback), so this degrades to the original sequential behaviour rather than
  // breaking on a single-core/no-pool build. ----
  btim_add(BTIM_PREPASS, btim_t_pre);

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

  // ---- Phase 2 (sequential, in original candidate order): everything below has a genuine
  // sequential dependency (dci_thres EMA, RNTI persistence ring buffer) or is rare/expensive enough
  // (CFR submission, PDSCH decode) that parallelising it buys nothing. Unchanged from before the
  // split, just walking cand_task[] instead of decoding inline. ----
  int decodes_this_occasion = 0; // capped by cfg->pdsch_max_per_slot -- see that field's comment
  for (int ti = 0; ti < nof_tasks; ti++) {
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
        nr_passive_replay_ul(source_absolute_slot, boot_rnti, dci01_length,
                             cand_task[ti].ul_out.raw_payload);
        nr_pdcch_blind_ul_result_t discovered;
        cand_task[ti].ok=nr_pdcch_ul_discovery_grant(&ul_opts,dci01_length,boot_rnti,
                                                    cand_task[ti].ul_out.raw_payload,&discovered);
        if(cand_task[ti].ok) cand_task[ti].ul_out=discovered;
      }
      const nr_pdcch_blind_ul_result_t *u = &cand_task[ti].ul_out;
      if (cand_task[ti].ok) {
        g_ul_accepts++;
        nr_pdcch_dci01_layout_observe(ul_opts.bwp_size, ul_opts.tda_count, dci01_length, u->raw_payload);
        /* Park it for the slot its PUSCH occupies. The DCI is in a DOWNLINK slot; the PUSCH is k2
         * slots later in an UPLINK one, where nothing runs today. */
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
      ue->dci_thres = (ue->dci_thres + raw->mismatched_bits) / 2;
      if (raw->mismatched_bits > ue->dci_thres + 30
          || !rnti_persistence_check(raw->rnti, abs_slot, persist_window_slots, cfg->rnti_persist_k))
        continue;
      nr_pdcch_blind_rnti_bootstrap_record(raw->rnti, NR_BLIND_RNTI_CLASS_C, abs_slot);
      { /* Corroborated AL census for DCI 1_1 -- the dominant traffic. This branch does its OWN
         * persistence check above and then continues on its own path, so accepts here never reach
         * the generic Gate 2 where the census was originally placed. MEASURED 2026-09-13: 51,498
         * 1_1 grants produced ZERO confirmed counts while only stray non-1_1 accepts were tallied,
         * so the search-space inference never reached its 32-grant threshold and never fired. */
        const int Lc3 = cand_task[ti].L;
        const int li3 = (Lc3 == 1) ? 0 : (Lc3 == 2) ? 1 : (Lc3 == 4) ? 2 : 3;
        atomic_fetch_add_explicit(&g_al_confirmed[nr_pdcch_ss_bucket(cfg)][li3], 1, memory_order_relaxed);
      }
      nr_pdcch_ss_registry_accept(cfg, raw->rnti);
      if (cfg->autodiscover) {
        const long mono = source_absolute_slot;
        nr_pdcch_blind_monitor_autodiscover_observe(raw->rnti,
            mono >= 0 ? (uint32_t)mono : abs_slot, raw->payload);
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
      uint8_t layout_ids[NR_DCI11_STAGE2_MAX_ALIVE + 3];
      int n = 0;
      if ((nr_pdcch_dci11_stage2_enabled() || nr_agnostic_v2()) && g_dci11_state == 1)
        n = nr_pdcch_dci11_stage2_candidates(raw, cand_task[ti].dci_length, cfg, layouts, layout_ids, NR_DCI11_STAGE2_MAX_ALIVE);
      const bool from_stage2 = (n > 0);
      if (!n)
        n = nr_pdcch_blind_dl_layout_candidates(raw, cand_task[ti].dci_length,
            cfg->bwp_size, cfg->dmrs_typeA_position, layouts, layout_ids);
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
        nr_pdsch_config_sweep_context_stats(keys[i],raw->rnti,layouts[i].tda_index,cfg->dmrs_typeA_position,&ok,&tr);
        ts_ok[i]=ok; ts_tr[i]=tr;
        if (ok>preferred_ok) { preferred_ok=ok; preferred=i; }
        else if (ok==preferred_ok && preferred>=0 && ok>0) preferred=-1; // tie: no preference
      }
      if (preferred>=0 && preferred_ok>=8) {
        static uint8_t s_pref_logged[65536];
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
    }
    const nr_pdcch_blind_result_t out = cand_task[ti].out;
    if (!cand_task[ti].ok) {
      g_last_reject_reason = out.reject_reason; // TEMPORARY diagnostic, see periodic summary below
      g_last_reject_rnti   = out.rnti;
      continue;
    }
    g_accepts++;
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
      if (!s_no_mm && out.mismatched_bits > (ue->dci_thres + 30)) {
        g_held_mismatch++;
        continue;
      }
    }

    // ---- Gate 2: RNTI persistence. A real UE's RNTI recurs across many grants; a noise accept is
    // (almost always) a one-off. See rnti_persistence_check()'s own comment. ----
    if (!rnti_persistence_check(out.rnti, abs_slot, persist_window_slots, cfg->rnti_persist_k)) {
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
    if (!cfg->autodiscover || g_length_found)
      nr_pdcch_blind_rnti_bootstrap_record(out.rnti, out.rnti_class, abs_slot);
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
      uint16_t known[NR_PDCCH_BLIND_MAX_UE];
      const int n_known = nr_pdcch_blind_monitor_confirmed_rnti_set(abs_slot, known, NR_PDCCH_BLIND_MAX_UE);
      if (n_known > 0 && !nr_pdcch_blind_monitor_rnti_confirmed(abs_slot, out.rnti)) {
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
    LOG_I(PHY, "SENSING: blind PDCCH rnti_seen utc_ns=%lld rnti=0x%x\n", rnti_utc_ns, out.rnti);

    // ---- CFR extraction: nr_pdsch_channel_estimation() on the blind-decoded allocation/DMRS config
    // -- mirrors phy_procedures_nr_ue.c's existing pdsch_dmrs ISAC tap exactly (same function, same
    // pdsch_est_size formula, same comb-2 packing), just fed from a blind decode instead of the UE's
    // own real DLSCH config. ----
    /* Accepted DCI may still be discovery evidence. Do not emit PDSCH/CFR from
     * unverified geometry or silently use manual interpretation while full-auto is waiting. */
    if (cfg->autodiscover && (!g_length_found || !nr_pdcch_blind_monitor_autodiscover_extent_verified()))
      continue;
    if (cfg->dl_full_auto && !is_dci10 && !g_pdsch_sweep_on)
      continue;

    int dmrs_sym = -1;
    for (int m = out.start_symbol; m < out.start_symbol + out.num_symbols; m++) {
      if (out.dl_dmrs_symb_pos & (1u << m)) {
        dmrs_sym = m;
        break;
      }
    }
    if (dmrs_sym < 0) {
      continue;
    }

    fapi_nr_dl_config_dlsch_pdu_rel15_t dlsch_pdu;
    memset(&dlsch_pdu, 0, sizeof(dlsch_pdu));
    dlsch_pdu.BWPStart           = (uint16_t)rb_origin;
    dlsch_pdu.BWPSize            = is_dci10 ? dci10_ctx.n_rb_riv : cand_task[ti].bwp_size;
    dlsch_pdu.resource_alloc     = 1; // Type-1/RIV -- the only branch this module ever produces
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
      dmrs_sym = __builtin_ctz((unsigned)hy.dmrs_mask);
    }
    dlsch_pdu.pduBitmap          = 0; // no PTRS: format 1_1 with no dedicated PTRS config
    dlsch_pdu.numCsiRsForRateMatching = 0;
    /* CSI-RS rate matching from the blind CSI-RS search's confirmed resource, on the slots it occurs. */
    if (nr_csirs_blind_rt_rate_match(abs_slot, &dlsch_pdu.csiRsForRateMatching[0])) {
      /* A CSI-RS hypothesis that lands on one of this grant's DM-RS symbols is wrong for this grant
       * (the standard forbids the overlap and nr_dlsch_extract_rbs() ASSERTS on it -- which killed
       * the 4-RX OTA run r4a_223725). Apply it only when it touches no DM-RS symbol. */
      const fapi_nr_dl_config_csirs_pdu_rel15_t *c = &dlsch_pdu.csiRsForRateMatching[0];
      static const uint8_t num_l0[18] = {1, 1, 1, 1, 2, 1, 2, 2, 1, 2, 2, 2, 2, 2, 4, 2, 2, 4};
      bool clash = (c->row < 1 || c->row > 18);
      for (int k = 0; !clash && k < num_l0[c->row - 1]; k++)
        if ((dlsch_pdu.dlDmrsSymbPos >> (c->symb_l0 + k)) & 1) clash = true;
      if (!clash && (c->row == 13 || c->row == 14 || c->row == 16 || c->row == 17))
        for (int k = 0; !clash && k < 2; k++)
          if ((dlsch_pdu.dlDmrsSymbPos >> (c->symb_l1 + k)) & 1) clash = true;
      dlsch_pdu.numCsiRsForRateMatching = clash ? 0 : 1;
      static _Atomic uint32_t s_clash_n;
      if (clash && (atomic_fetch_add(&s_clash_n, 1) % 500) == 0)
        LOG_W(PHY, "SENSING: CSI-RS hypothesis (row %u l0=%u) overlaps this grant's DM-RS symbols (mask 0x%x): not applied\n",
              c->row, c->symb_l0, dlsch_pdu.dlDmrsSymbPos);
    }

    const freq_alloc_bitmap_t freq_alloc = set_bitmap_from_start_size(out.start_rb, out.num_rb);

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
      job.frame_rx      = proc->frame_rx;
      job.nr_slot_rx    = proc->nr_slot_rx;
      job.gNB_id        = proc->gNB_id;
      job.absolute_slot = source_absolute_slot;
      job.rnti          = out.rnti;
      job.harq_pid_tag  = blind_harq_tag(abs_slot, out.rnti, out.harq_pid);
      job.want_data     = want_data;
      job.fo_hz         = isnan(nr_slot_fep_fo_override_hz)
                  ? (ue->cont_fo_comp ? ue->dl_Doppler_shift + ue->freq_offset : 0.0)
                  : nr_slot_fep_fo_override_hz;  /* receive-thread sample; see nr_slot_fep_fo_override_hz */
      job.sweep_ticket  = sweep_ticket;
      job.bwp_entry     = cand_task[ti].bwp_entry;
      /* Wide layout set: this trial is a first-code-block probe, not a full decode. */
      job.layout_probe  = (cand_task[ti].dl_auto && g_dci11_state == 1
                           && g_dci11_resolver.n_alive > NR_DCI11_STAGE2_MAX_ALIVE) ? 1 : 0;
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
    nr_pdsch_channel_estimation(ue, proc, &dlsch_pdu, &freq_alloc, 0, get_dmrs_port(0, out.dmrs_ports),
                               (unsigned char)dmrs_sym, pdsch_est_size, pdsch_dl_ch_estimates,
                               fp->samples_per_slot_wCP, rxdataF_pdsch, &nvar);
    btim_add(BTIM_CHEST, btim_t_che);

    const int num_sc = out.num_rb * NR_NB_SC_PER_RB;
    if (num_sc >= 2) {
      const uint32_t base_sc = (uint32_t)(rb_origin + out.start_rb) * NR_NB_SC_PER_RB;
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
              job.harq_pid_tag  = blind_harq_tag(abs_slot, out.rnti, out.harq_pid);
              job.want_data     = want_data;
              job.fo_hz         = isnan(nr_slot_fep_fo_override_hz)
                  ? (ue->cont_fo_comp ? ue->dl_Doppler_shift + ue->freq_offset : 0.0)
                  : nr_slot_fep_fo_override_hz;  /* receive-thread sample */
              job.sweep_ticket  = sweep_ticket;
              job.bwp_entry     = cand_task[ti].bwp_entry;
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

  if (g_occasions_run % NR_PDCCH_BLIND_SUMMARY_PERIOD_OCC == 0) {
    /* scanq is all-zero when the scan runs in-line, which is what distinguishes "deferral off" from
     * "deferral on and keeping up" in a log without needing a second line. */
    nr_pdcch_passive_queue_stats_t scanq;
    memset(&scanq, 0, sizeof(scanq));
    nr_pdcch_passive_queue_get_stats(&scanq);
    LOG_I(PHY,
         "SENSING: blind PDCCH monitor summary: occasions=%lu candidates=%lu accepts=%lu "
         "dci10[accepts=%lu C=%lu TC=%lu SI=%lu RA=%lu P=%lu] dci01[accepts=%lu rejects=%lu] "
         "dci00[accepts=%lu rejects=%lu] ulscan[sched=%lu crc_hit=%lu disc=%lu] "
         "held[energy=%lu persist=%lu snr=%lu mismatch=%lu rnti_set=%lu] efloor=%.2f cfr_submits=%lu "
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
         (unsigned long)g_held_energy, (unsigned long)g_held_persist, (unsigned long)g_held_snr,
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
    if (want_decode && (g_occasions_run % NR_PDCCH_BLIND_SUMMARY_PERIOD_OCC) == 0) {
      nr_pdsch_passive_ldpc_stats_dump();
    }

    /* Uplink receive census. Also the reference that forces PHY_NR_PASSIVE_UL into the link: a
     * static library contributes nothing until something needs a symbol from it, so without a
     * caller a clean build proves only that the sources COMPILE, not that the gNB PUSCH receive
     * chain resolves inside this binary. */
    if ((g_occasions_run % NR_PDCCH_BLIND_SUMMARY_PERIOD_OCC) == 0) {
      nr_pusch_grant_book_stats_dump();
    }
    if (nr_pdsch_passive_queue_running() && (g_occasions_run % NR_PDCCH_BLIND_SUMMARY_PERIOD_OCC) == 0) {
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
    const bool acq_period = (g_occasions_run % NR_PDCCH_BLIND_SUMMARY_PERIOD_OCC) == 0;
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

    if (btim_on && (g_occasions_run % NR_PDCCH_BLIND_SUMMARY_PERIOD_OCC) == 0) {
      char rep[700];
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
