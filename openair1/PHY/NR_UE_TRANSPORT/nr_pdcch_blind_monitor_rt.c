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

#include <string.h>
#include <time.h> // clock_gettime for the rnti_seen correlation line below

#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"

#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h" // nr_pdcch_demapping_deinterleaving/_unscrambling/_generate_llr
#include "PHY/MODULATION/modulation_UE.h"               // nr_slot_fep
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"          // nr_pdsch_channel_estimation
#include "PHY/TOOLS/tools_defs.h"                        // allocCast2D/fourDimArray_t
#include "PHY/NR_UE_ISAC/nr_isac.h"                      // nr_isac_submit_cfr/_enabled/_source_enabled
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.h"  // passive PDSCH decode (data-aided source)
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.h"      // shared re-encode + Ĥ=Y/X submit
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
static int         g_constdiag_left = 20; // TEMPORARY, see CONSTDIAG below
static uint64_t    g_accepts        = 0; // raw plausibility accepts (Step 1-4 of decode_and_extract),
                                         // BEFORE the noise-floor gates below -- unchanged meaning
                                         // from before 2026-07-28's gates, so old logs stay comparable
static const char *g_last_reject_reason = NULL; // TEMPORARY diagnostic, 2026-07-28 root-cause pass
static uint16_t     g_last_reject_rnti  = 0;
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
static uint64_t g_held_persist  = 0; // decoded+accepted but RNTI not yet seen rnti_persist_k times
static uint64_t g_held_snr      = 0; // decoded+accepted+persisted but post-estimation SNR too low
static uint64_t g_held_mismatch = 0; // migrated from NRSniffer: rejected by the adaptive mismatched-bits gate

// ---- Passive PDSCH decode counters (2026-07-30). g_dec_ok/g_dec_try IS the go/no-go measurement
// PASSIVE_PDSCH_DATA_AIDED_HANDOVER.md §B.5 asks for: a passive receiver sits somewhere the grant
// was not aimed at, so whether overheard transport blocks pass CRC at all is an open empirical
// question, and everything downstream of it is worthless if the answer is "almost never". ----
static uint64_t g_dec_try   = 0; // decodes actually attempted (i.e. reached the LDPC decoder)
static uint64_t g_dec_ok    = 0; // ... of which the transport-block CRC passed
static uint64_t g_dec_skip_rv = 0; // skipped: rv != 0 and rv0_only set (not self-decodable, see cfg)
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
  nr_pdcch_blind_result_t out; // OUTPUT
  bool         ok;             // OUTPUT
  task_ans_t  *ans;
} nr_pdcch_blind_cand_task_t;

static void nr_pdcch_blind_cand_worker(void *arg)
{
  nr_pdcch_blind_cand_task_t *t = (nr_pdcch_blind_cand_task_t *)arg;
  int16_t tmp_e[16 * 108];
  nr_pdcch_unscrambling((c16_t *)t->e_rx, t->scrambling_rnti, (uint32_t)(t->L * 108), t->dmrs_scrambling_id, tmp_e);
  t->ok = nr_pdcch_blind_decode_and_extract_ex(tmp_e, t->L, t->dci_length, t->bwp_size, t->dmrs_typeA_position,
                                               t->rnti_min, t->rnti_max, t->extract_opts, &t->out);
  {
    extern void nr_pdcch_llr_probe(const char *, int, int, int, int, uint32_t, const int16_t *, int);
    nr_pdcch_llr_probe("blind", t->frame, t->slot, t->cce, t->L, t->out.rnti, tmp_e, t->L * 108);
  }
  completed_task_ans(t->ans);
}

void nr_pdcch_blind_monitor_process(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc)
{
  if (!nr_pdcch_blind_monitor_enabled() || !nr_isac_enabled()) {
    return;
  }
  const nr_pdcch_blind_monitor_cfg_t *cfg = nr_pdcch_blind_monitor_get_cfg();

  // What this occasion is for. The tap used to run only for the DM-RS source; the passive
  // data-aided path (pdsch_decode) is a second, independent reason to scan the same candidates, and
  // `pdsch_decode == 1` (measure the CRC pass rate, submit nothing) must work with NO sensing source
  // enabled at all -- that is the whole point of having a measure-only level.
  const bool want_dmrs = nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DMRS_BLIND);
  const bool want_data = cfg->pdsch_decode >= 2 && nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DATA);
  const bool want_decode = cfg->pdsch_decode >= 1; // >=1 always decodes; only >=2 submits
  if (!want_dmrs && !want_decode) {
    return;
  }

  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const uint32_t abs_slot = (uint32_t)proc->frame_rx * fp->slots_per_frame + (uint32_t)proc->nr_slot_rx;
  if (cfg->ss_monitoring_slot_periodicity <= 0
      || (abs_slot % (uint32_t)cfg->ss_monitoring_slot_periodicity) != (uint32_t)cfg->ss_monitoring_slot_offset) {
    return; // not a monitoring occasion this slot
  }
  g_occasions_run++;

  // ---- Build the local, single-search-space PDCCH config. ----
  nr_phy_data_t local_phy_data;
  memset(&local_phy_data, 0, sizeof(local_phy_data));
  local_phy_data.phy_pdcch_config.nb_search_space = 1;
  fapi_nr_dl_config_dci_dl_pdu_rel15_t *rel15 = &local_phy_data.phy_pdcch_config.pdcch_config[0];

  rel15->BWPStart = (uint16_t)cfg->bwp_start;
  rel15->BWPSize  = (uint16_t)cfg->bwp_size;
  rel15->coreset.CoreSetType =
      (cfg->coreset_type == 1) ? NFAPI_NR_CSET_CONFIG_MIB_SIB1 : NFAPI_NR_CSET_CONFIG_PDCCH_CONFIG;
  rel15->coreset.rb_offset   = 0;
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
  for (int oi = 0; oi < 4; oi++) {
    const int L   = al_order[oi];
    const int idx = (L == 1) ? 0 : (L == 2) ? 1 : (L == 4) ? 2 : 3; // ss_al_candidates[] is AL 1,2,4,8
    int cap = cfg->ss_al_candidates[idx];
    if (cap < 0) {
      continue; // explicitly disabled for this deployment
    }
    if (cap == 0) {
      cap = max_cand; // auto: sweep every non-overlapping position the budget allows
    }
    const int need = NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * L * 6; // 54*L REs per candidate
    int added = 0;
    for (int cce = 0; cce + L - 1 < num_cces && nc < max_cand && added < cap && used_re + need <= max_re;
         cce += L) {
      rel15->CCE[nc] = (uint16_t)cce;
      rel15->L[nc]   = (uint8_t)L;
      nc++;
      added++;
      used_re += need;
    }
  }

  /* One-shot visibility. A ladder that silently fails to cover the level the deployment actually
   * uses produces a 100 % false-accept stream rather than an error -- which is precisely the
   * failure mode this ladder just had, undetected across several sessions. Print what was built. */
  {
    static int s_ladder_logged = 0;
    if (!s_ladder_logged) {
      s_ladder_logged = 1;
      int n_per_al[4] = {0, 0, 0, 0};
      for (int c = 0; c < nc; c++) {
        n_per_al[(rel15->L[c] == 1) ? 0 : (rel15->L[c] == 2) ? 1 : (rel15->L[c] == 4) ? 2 : 3]++;
      }
      LOG_I(PHY,
            "SENSING: blind PDCCH ladder: num_cces=%d ncand=%d/%d re=%d/%d (AL1=%d AL2=%d AL4=%d AL8=%d)\n",
            num_cces, nc, max_cand, used_re, max_re, n_per_al[0], n_per_al[1], n_per_al[2], n_per_al[3]);
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

  for (int symbol = cfg->ss_first_symbol; symbol < cfg->ss_first_symbol + rel15->coreset.duration; symbol++) {
    nr_slot_fep(ue, fp, proc->nr_slot_rx, symbol, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
    __attribute__((aligned(32))) c16_t rxdataF_symb[fp->nb_antennas_rx][((fp->ofdm_symbol_size + 7) / 8) * 8];
    for (int ant = 0; ant < fp->nb_antennas_rx; ant++) {
      memcpy(rxdataF_symb[ant], &rxdataF[ant][symbol * fp->ofdm_symbol_size], sizeof(c16_t) * fp->ofdm_symbol_size);
    }
    nr_pdcch_generate_llr(ue, proc, symbol, &local_phy_data, llr_size_symbol, num_monitoring_occ,
                         rel15->coreset.duration, rxdataF_symb, pdcch_llr);
  }

  // ---- Demapping/deinterleaving + per-candidate unscrambling/decode. Mirrors dci_nr.c's own
  // nr_pdcch_dci_indication()/nr_dci_decoding_procedure(), minus the own-RNTI equality gate --
  // that's the entire "blind" widening (nr_pdcch_blind_decode_and_extract() does its own range
  // check instead). ----
  const int llr_stride = llr_size_symbol; // duration==1 here -> llr_size == llr_size_symbol
  c16_t pdcch_e_rx[NR_MAX_PDCCH_SIZE];
  nr_pdcch_demapping_deinterleaving((uint32_t)n_rb, pdcch_llr[0][0], pdcch_e_rx, rel15->coreset.duration,
                                    rel15->coreset.RegBundleSize, rel15->coreset.InterleaverSize,
                                    rel15->coreset.ShiftIndex, rel15->number_of_candidates, rel15->CCE, rel15->L,
                                    llr_stride);

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
  nr_pdcch_blind_cand_task_t cand_task[64]; // bounded by rel15->CCE[64]/L[64] (fapi_nr_ue_interface.h)
  int nof_tasks = 0;
  {
    int e_rx_cand_idx = 0;
    for (int c = 0; c < rel15->number_of_candidates; c++) {
      const int L         = rel15->L[c];
      const int n_re_cand = NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * L * 6;

      // ---- Gate 1 (cheapest, runs first): raw pre-decode LLR energy. Unscheduled CCEs measured
      // exactly (0,0) live 2026-07-28; skips the polar decode entirely for those, not just the CFR
      // submission -- a real CPU saving alongside the false-accept reduction. ----
      if (cfg->energy_adapt_factor > 0.0f || cfg->energy_min > 0.0f) {
        const c16_t *e_raw   = &pdcch_e_rx[e_rx_cand_idx];
        double        sum_abs = 0;
        for (int i = 0; i < n_re_cand; i++) {
          sum_abs += (e_raw[i].r < 0 ? -e_raw[i].r : e_raw[i].r) + (e_raw[i].i < 0 ? -e_raw[i].i : e_raw[i].i);
        }
        const float mean_abs = (float)(sum_abs / n_re_cand);

        float thresh;
        if (cfg->energy_adapt_factor > 0.0f) {
          // Adaptive: threshold rides the measured noise floor. Update the floor estimate with EVERY
          // candidate (including this one) BEFORE thresholding -- the estimator must see the whole
          // population to stay calibrated, and feeding it only the survivors would let it collapse.
          energy_floor_update(mean_abs);
          // Until the estimate has converged, reject nothing: a not-yet-settled floor can sit far
          // above the true one and would throw away real grants during exactly the startup window
          // where the persistence gate is also still cold.
          thresh = (g_energy_nseen >= ENERGY_FLOOR_WARMUP)
                       ? cfg->energy_adapt_factor * g_energy_floor
                       : 0.0f;
        } else {
          thresh = cfg->energy_min;
        }

        /* ENERGYPROBE (ISAC_PDCCH_ENERGY=1): strongest candidate of this occasion vs the adaptive
         * noise floor. This separates "we cannot decode it" from "there is nothing to decode":
         * a CORESET actually carrying PDCCH must show candidates well ABOVE the floor. SIB1 slots
         * are the built-in positive control -- they are known-strong and decode reliably. */
        {
          static int s_ep = -1;
          if (s_ep < 0)
            s_ep = (getenv("ISAC_PDCCH_ENERGY") != NULL) ? 1 : 0;
          if (s_ep) {
            static float occ_max = 0.0f;
            static int   occ_slot = -1;
            if (proc->nr_slot_rx != occ_slot) {
              if (occ_slot >= 0 && g_energy_nseen >= ENERGY_FLOOR_WARMUP)
                LOG_I(PHY, "ENERGYPROBE slot=%d max_mean=%.2f floor=%.2f ratio=%.2f\n",
                      occ_slot, occ_max, g_energy_floor,
                      g_energy_floor > 0.0f ? occ_max / g_energy_floor : 0.0f);
              occ_slot = proc->nr_slot_rx;
              occ_max  = 0.0f;
            }
            if (mean_abs > occ_max)
              occ_max = mean_abs;
          }
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

      cand_task[nof_tasks] = (nr_pdcch_blind_cand_task_t){
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
      };
      nof_tasks++;
      e_rx_cand_idx += n_re_cand;
      g_candidates_run++;
    }
  }

  // ---- Phase 1 (parallel): fan the independent unscramble+decode work out across the UE's thread
  // pool. pushTpool() runs the task inline if the pool has zero worker threads configured (its own
  // documented fallback), so this degrades to the original sequential behaviour rather than
  // breaking on a single-core/no-pool build. ----
  if (nof_tasks > 0) {
    task_ans_t ans;
    init_task_ans(&ans, nof_tasks);
    for (int i = 0; i < nof_tasks; i++) {
      cand_task[i].ans = &ans;
      task_t t = {.func = nr_pdcch_blind_cand_worker, .args = &cand_task[i]};
      pushTpool(&get_nrUE_params()->Tpool, t);
    }
    join_task_ans(&ans);
  }

  // ---- Phase 2 (sequential, in original candidate order): everything below has a genuine
  // sequential dependency (dci_thres EMA, RNTI persistence ring buffer) or is rare/expensive enough
  // (CFR submission, PDSCH decode) that parallelising it buys nothing. Unchanged from before the
  // split, just walking cand_task[] instead of decoding inline. ----
  int decodes_this_occasion = 0; // capped by cfg->pdsch_max_per_slot -- see that field's comment
  for (int ti = 0; ti < nof_tasks; ti++) {
    const nr_pdcch_blind_result_t out = cand_task[ti].out;
    if (!cand_task[ti].ok) {
      g_last_reject_reason = out.reject_reason; // TEMPORARY diagnostic, see periodic summary below
      g_last_reject_rnti   = out.rnti;
      continue;
    }
    g_accepts++;

    // ---- Gate 1.5: mismatched-bits adaptive threshold. Migrated from NRSniffer's dci_nr.c
    // (nr_dci_false_detection + ue->dci_thres), 2026-08-05. out.mismatched_bits (computed in
    // nr_pdcch_blind_decode_and_extract_ex) counts bit disagreements between the re-encoded
    // payload and the original LLR polarity -- a CRC-plausible candidate that's actually a random
    // false accept will typically mismatch far more bits than a genuine decode. ue->dci_thres is a
    // simple EMA of recent mismatch counts (self-calibrating to this receiver's own noise floor,
    // not a fixed constant); a candidate mismatching more than dci_thres+30 bits is rejected.
    // Default ON (NRSniffer runs this unconditionally); no new config knob added given time
    // constraints -- flag for follow-up if it needs to be independently disable-able.
    ue->dci_thres = (ue->dci_thres + out.mismatched_bits) / 2;
    if (out.mismatched_bits > (ue->dci_thres + 30)) {
      g_held_mismatch++;
      continue;
    }

    // ---- Gate 2: RNTI persistence. A real UE's RNTI recurs across many grants; a noise accept is
    // (almost always) a one-off. See rnti_persistence_check()'s own comment. ----
    if (!rnti_persistence_check(out.rnti, abs_slot, persist_window_slots, cfg->rnti_persist_k)) {
      g_held_persist++;
      continue;
    }

    LOG_D(PHY,
         "SENSING: blind PDCCH accept (%d.%d) rnti=0x%x prb=[%u..%u) sym=[%u..%u) dmrs_mask=0x%x\n",
         proc->frame_rx, proc->nr_slot_rx, out.rnti, out.start_rb, out.start_rb + out.num_rb, out.start_symbol,
         out.start_symbol + out.num_symbols, out.dl_dmrs_symb_pos);

    // ---- Cross-receiver RNTI consistency (offline, post-hoc -- see tests/passive_rx/rnti_gate.py):
    // this line's sole purpose is a wall-clock anchor to correlate accepts across INDEPENDENT
    // receiver PROCESSES that share no RT state. A real active UE's RNTI is legitimately accepted by
    // EVERY receiver that can hear it (same air, same grants); a noise/false accept is
    // receiver-local (wrong CRC-recovered RNTI from that receiver's own decode error) and will not
    // recur on a SECOND receiver at the same wall-clock time. LOG_I (not LOG_D) and deliberately
    // separate from the line above -- that one is for interactive debugging (PRB/symbol detail, gets
    // noisy fast), this one is a fixed, parseable schema meant to be grepped by tooling. ----
    struct timespec rnti_ts;
    clock_gettime(CLOCK_REALTIME, &rnti_ts);
    const long long rnti_utc_ns = (long long)rnti_ts.tv_sec * 1000000000LL + (long long)rnti_ts.tv_nsec;
    LOG_I(PHY, "SENSING: blind PDCCH rnti_seen utc_ns=%lld rnti=0x%x\n", rnti_utc_ns, out.rnti);

    // ---- CFR extraction: nr_pdsch_channel_estimation() on the blind-decoded allocation/DMRS config
    // -- mirrors phy_procedures_nr_ue.c's existing pdsch_dmrs ISAC tap exactly (same function, same
    // pdsch_est_size formula, same comb-2 packing), just fed from a blind decode instead of the UE's
    // own real DLSCH config. ----
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
    dlsch_pdu.BWPStart           = (uint16_t)cfg->bwp_start;
    dlsch_pdu.BWPSize            = (uint16_t)cfg->bwp_size;
    dlsch_pdu.resource_alloc     = 1; // Type-1/RIV -- the only branch this module ever produces
    dlsch_pdu.refPoint           = 0;
    dlsch_pdu.dmrsConfigType     = NFAPI_NR_DMRS_TYPE1;
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
    dlsch_pdu.mcs_table          = (uint8_t)cfg->pdsch_mcs_table;
    dlsch_pdu.pduBitmap          = 0; // no PTRS: format 1_1 with no dedicated PTRS config
    dlsch_pdu.numCsiRsForRateMatching = 0;

    const freq_alloc_bitmap_t freq_alloc = set_bitmap_from_start_size(out.start_rb, out.num_rb);

    const uint32_t pdsch_est_size = ((fp->symbols_per_slot * fp->ofdm_symbol_size + 15) / 16) * 16;
    fourDimArray_t *toFree        = NULL;
    allocCast2D(pdsch_dl_ch_estimates, int32_t, toFree, fp->nb_antennas_rx, pdsch_est_size, false);

    // Full-slot FEP for the PDSCH's own DMRS symbol -- separate from the CORESET FEP above (a
    // different symbol in general; the PDSCH allocation starts after the PDCCH region).
    __attribute__((aligned(32))) c16_t rxdataF_pdsch[fp->nb_antennas_rx][rxdataF_sz];
    nr_slot_fep(ue, fp, proc->nr_slot_rx, dmrs_sym, rxdataF_pdsch, link_type_dl, 0, ue->common_vars.rxdata);

    uint32_t nvar = 0;
    nr_pdsch_channel_estimation(ue, proc, &dlsch_pdu, &freq_alloc, 0, get_dmrs_port(0, out.dmrs_ports),
                               (unsigned char)dmrs_sym, pdsch_est_size, pdsch_dl_ch_estimates,
                               fp->samples_per_slot_wCP, rxdataF_pdsch, &nvar);

    const int num_sc = out.num_rb * NR_NB_SC_PER_RB;
    if (num_sc >= 2) {
      const uint32_t base_sc = (uint32_t)(cfg->bwp_start + out.start_rb) * NR_NB_SC_PER_RB;
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
      const bool snr_ok =
          !(cfg->min_snr_lin > 0.0f && nof_re > 0 && nvar > 0
            && (float)(h_pow_sum / nof_re) < cfg->min_snr_lin * (float)nvar);
      if (nof_re > 0 && snr_ok) {
        if (want_dmrs) {
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
        if (want_decode && decodes_this_occasion < cfg->pdsch_max_per_slot) {
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
            const nr_pdsch_passive_grant_t grant = {.rnti      = out.rnti,
                                                    .mcs       = out.mcs,
                                                    .rv        = out.rv,
                                                    .mcs_table = (uint8_t)cfg->pdsch_mcs_table,
                                                    .nb_rb_oh  = (uint16_t)cfg->pdsch_xoverhead};
            nr_pdsch_passive_decode_result_t dec;
            // Reuses rxdataF_pdsch: nr_pdsch_passive_decode() FEPs the WHOLE allocation into it,
            // a superset of the single DM-RS symbol already transformed above, so the buffer is
            // simply refilled rather than duplicated (~900 kB at 273 PRB x 4 antennas).
            const nr_pdsch_passive_decode_status_t st =
                nr_pdsch_passive_decode(ue, proc, &dlsch_pdu, &freq_alloc, &grant, rxdataF_pdsch, &dec);
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
                  nr_isac_pdsch_data_aided_submit(ue, proc, &dec.cw, &dlsch_pdu, &freq_alloc, out.rnti, dec.tb,
                                                  NR_PDCCH_BLIND_DATA_AIDED_TAG_BASE + out.harq_pid, rxdataF_pdsch,
                                                  (double)dec.nvar);
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

  if (g_occasions_run % NR_PDCCH_BLIND_SUMMARY_PERIOD_OCC == 0) {
    LOG_I(PHY,
         "SENSING: blind PDCCH monitor summary: occasions=%lu candidates=%lu accepts=%lu "
         "held[energy=%lu persist=%lu snr=%lu mismatch=%lu] efloor=%.2f cfr_submits=%lu "
         "pdsch_decode[try=%lu crc_ok=%lu (%.1f%%) skip_rv=%lu unsup=%lu data_submits=%lu] "
         "last_reject=\"%s\" last_reject_rnti=0x%x\n",
         (unsigned long)g_occasions_run, (unsigned long)g_candidates_run, (unsigned long)g_accepts,
         (unsigned long)g_held_energy, (unsigned long)g_held_persist, (unsigned long)g_held_snr,
         (unsigned long)g_held_mismatch,
         g_energy_floor,
         (unsigned long)g_cfr_submits,
         (unsigned long)g_dec_try, (unsigned long)g_dec_ok,
         g_dec_try ? (100.0 * (double)g_dec_ok / (double)g_dec_try) : 0.0,
         (unsigned long)g_dec_skip_rv, (unsigned long)g_dec_unsup, (unsigned long)g_data_submits,
         g_last_reject_reason ? g_last_reject_reason : "(none yet)",
         g_last_reject_rnti);
  }
}
