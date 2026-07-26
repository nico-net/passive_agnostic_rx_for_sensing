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

/*! \file openair1/PHY/NR_UE_ISAC/multi_target_tracker.h
 * \brief Multi-object tracker over the per-CPI detection stream (Phase 2, Part 1).
 *
 * WHAT THIS IS FOR. target_tracker is a single Kalman track: it assumes at most one target and does
 * its own nearest-detection association from the full CPI detection list. That is insufficient once a
 * scene has >1 mover -- two tracks would each greedily grab the same strongest detection, and there
 * is no notion of track identity, birth, or death of individual tracks. This class adds exactly the
 * three pieces the handover (PHASE2_MOT_MULTIUE_HANDOVER.md Part 1) calls for, and NOTHING in the
 * tuned single-track Kalman/gating/coasting logic changes:
 *
 *   1. MULTI-TRACK CONTAINER. A std::vector of independent per-track filters, each an unmodified
 *      target_tracker instance carrying its own Kalman state, adaptive-q, coast counter and a stable
 *      monotonic track_id.
 *   2. GLOBAL DATA ASSOCIATION. Instead of each filter picking "nearest to me", association is solved
 *      once across ALL tracks vs. ALL detections: greedy nearest-neighbour under a normalized-distance
 *      gate (sorted by cost, assign best pairs first, each track/detection used at most once). Greedy
 *      is chosen over Hungarian per the handover's open question #1 -- N is small (<= max_detections,
 *      16) and the crossing test validates it; Hungarian is only warranted if greedy demonstrably
 *      swaps identities at a crossing. Prediction is propagated forward by dt using each track's OWN
 *      velocity, so two tracks whose ranges momentarily coincide still separate by their differing
 *      range-rate -- the property that keeps identities stable through a range crossing.
 *   3. M-of-N TRACK INITIATION, WITH M AUTO-DERIVED. An unassociated detection does NOT immediately
 *      become a reported track (that would let a single CFAR false alarm or a conjugate-mirror ghost
 *      spawn one). It seeds a TENTATIVE track; only after M hits within the last N CPIs is the track
 *      CONFIRMED and reported. Tentative tracks that fail to confirm within the window are pruned.
 *      By default (track_confirm_m==0) M is NOT a hand-picked constant: solve_confirm_m() derives
 *      the smallest M for which a purely random/incoherent false-alarm stream would confirm a
 *      spurious track within N CPIs with probability <= track_confirm_target_pfa, using the MEASURED
 *      mean detections/CPI and the association gate's area as a fraction of the range-Doppler grid
 *      (needs the current CPI's sensing_rvm_t; see update()'s rvm parameter). This exists because a
 *      fixed M/N tuned to one scene silently mis-confirms once the false-alarm density changes --
 *      live-confirmed 2026-07-24 (PHASE2_MOT_MULTIUE_HANDOVER.md open question #2). Explicit
 *      track_confirm_m > 0 still pins a manual value and bypasses this entirely.
 *
 * Each assigned detection is handed to its track's target_tracker::update() as a single-element list
 * (or an empty list => that track coasts). The per-track filter therefore still applies its own tight
 * update gate, adaptive q, coast/drop -- the association here is a coarse routing layer on top, not a
 * replacement for it. Track deletion is the per-track target_tracker's existing max_coast drop,
 * observed via last().active; for tentative tracks a shorter confirm-window timeout applies.
 *
 * RT-safety: engine thread only, called from sensing_engine::process_cpi() after range_doppler.
 * Fixed small vectors, no per-CPI heap growth of consequence (bounded by track_max_tracks).
 */

#ifndef NR_ISAC_MULTI_TARGET_TRACKER_H
#define NR_ISAC_MULTI_TARGET_TRACKER_H

#include <cstdint>
#include <vector>

#include "defs_nr_UE_ISAC.h"
#include "target_tracker.h"

namespace nr_isac {

/**
 * @brief Multi-object tracker: a bank of per-track target_tracker filters + association + M-of-N init.
 * See the file comment for the design. Single-target operation is the special case of one confirmed
 * track (set track_max_tracks=1, track_confirm_m=1 to reproduce the legacy single-track behaviour).
 */
class multi_target_tracker
{
public:
  explicit multi_target_tracker(const nr_isac_args_t& args_) : args(args_) {}

  /**
   * @brief Advances all tracks by one CPI: predict + associate + update/coast + init + prune.
   * @param detections this CPI's CFAR detections (may be empty)
   * @param dt_s       elapsed time since the previous CPI, seconds (<=0 => per-track re-init dt)
   * @param rvm        this CPI's range-Doppler grid (for auto-M's grid-area term). Pass nullptr if
   *                   unavailable (e.g. offline tests) -- auto mode then falls back to a fixed M.
   * @return the CONFIRMED, active tracks after this CPI, each with its stable track_id.
   */
  const std::vector<sensing_track_t>& update(const std::vector<sensing_detection_t>& detections, double dt_s,
                                              const sensing_rvm_t* rvm = nullptr);

  const std::vector<sensing_track_t>& confirmed() const { return out_; }
  size_t                              num_tracks() const { return slots_.size(); } ///< incl. tentative
  void                                reset();

  // Diagnostics for the auto-derived M (see the file comment). Meaningful only when
  // args.track_confirm_m==0 AND a valid rvm was passed to the last update(); last_p_hit() returns
  // -1.0 otherwise (manual pin / no grid geometry yet) as a "not computed" sentinel.
  uint32_t last_confirm_m()        const { return last_confirm_m_; }
  double   last_p_hit()            const { return last_p_hit_; }
  double   mean_detections_ewma()  const { return mean_det_ewma_; }

private:
  /// One maintained track: its Kalman filter + identity + M-of-N confirmation bookkeeping.
  struct slot_t {
    uint32_t       id          = 0;
    target_tracker filter;                 ///< the unmodified single-track Kalman filter for this target
    bool           confirmed   = false;    ///< passed the M-of-N gate at least once
    uint32_t       age_cpis    = 0;        ///< CPIs since this slot was born
    uint32_t       hit_history = 0;        ///< bitmask of the last N "updated" flags (LSB = newest)

    // Flicker (power-continuity) state (track_flicker_reject). snr_jitter_ewma is the EWMA of
    // |Δsnr_dB| between consecutive ACCEPTED detections; a gated harmonic ghost's SNR swings hard
    // frame-to-frame (high jitter), a real reflector's is smooth (low). snr_updates gates the metric
    // in until enough samples exist. See defs_nr_UE_ISAC.h's track_flicker_reject comment.
    float          snr_prev_db      = 0.0f;
    float          snr_jitter_ewma  = 0.0f;
    uint32_t       snr_updates      = 0;
    bool           have_snr_prev    = false;

    explicit slot_t(uint32_t id_, const nr_isac_args_t& a) : id(id_), filter(a) {}
  };

  nr_isac_args_t               args;
  std::vector<slot_t>          slots_;
  std::vector<sensing_track_t> out_;         ///< confirmed tracks returned to the caller
  uint32_t                     next_id_ = 1; ///< monotonic id source (0 reserved for legacy path)

  // Auto-M state (see file comment): EWMA of raw detections/CPI, and the last computed diagnostics.
  double   mean_det_ewma_  = 0.0;
  bool     have_mean_det_  = false;
  uint32_t last_confirm_m_ = 0;
  double   last_p_hit_     = -1.0;
};

} // namespace nr_isac

#endif // NR_ISAC_MULTI_TARGET_TRACKER_H
