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

/*! \file openair1/PHY/NR_UE_ISAC/sensing_engine.h
 * \brief ISAC / passive-radar sensing engine (port of srsUE nr::sensing_engine).
 *
 * Owns a dedicated (non real-time) worker thread and a recycling double-queue. The RT
 * NR DL procedures call @ref submit with a per-slot CFR row; that does only cheap work
 * (recycle a snapshot, copy the CFR in) and hands the snapshot to the engine thread,
 * which accumulates a coherent processing interval and runs the range-Doppler DSP,
 * detection, and DetectionReport emission.
 */

#ifndef NR_ISAC_SENSING_ENGINE_H
#define NR_ISAC_SENSING_ENGINE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "defs_nr_UE_ISAC.h"
#include "isac_aoa.h"
#include "isac_sync.h"
#include "range_doppler.h"
#include "target_tracker.h"
#include "multi_target_tracker.h"
#include "matrix_complete.h"

namespace nr_isac {

/// Minimal blocking queue of pointers (mutex + condition variable). A null push is the
/// shutdown sentinel for the consumer. try_pop never blocks (used by the RT producer).
class ptr_queue
{
public:
  void push(sensing_slot_t* p)
  {
    {
      std::lock_guard<std::mutex> lk(m);
      q.push_back(p);
    }
    cv.notify_one();
  }
  bool try_pop(sensing_slot_t*& out)
  {
    std::lock_guard<std::mutex> lk(m);
    if (q.empty()) {
      return false;
    }
    out = q.front();
    q.pop_front();
    return true;
  }
  sensing_slot_t* wait_pop()
  {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [this] { return !q.empty(); });
    sensing_slot_t* p = q.front();
    q.pop_front();
    return p;
  }

private:
  std::mutex              m;
  std::condition_variable cv;
  std::deque<sensing_slot_t*> q;
};

class sensing_engine
{
public:
  sensing_engine(const nr_isac_args_t& args_, uint32_t max_prb_);
  ~sensing_engine();

  /// Launch the engine thread. Call once before the workers start producing slots.
  void start();

  /// Stop the engine thread and drain the queues.
  void stop();

  /**
   * @brief Real-time entry point: submit one per-slot CFR row (one slow-time sample).
   *
   * Best-effort; drops the occurrence if no snapshot buffer is free. @p h is the per-RE
   * CFR, @p k_abs / @p l the absolute subcarrier / OFDM-symbol index of each RE.
   */
  void submit(uint32_t                 slot_idx,
              float                    slot_frac,
              nr_isac_source_t         source,
              const nr_isac_carrier_t& carrier,
              const icf_t*              h,
              uint32_t                 nof_ant,
              const uint32_t*          k_abs,
              const uint32_t*          l,
              uint32_t                 nof_re,
              float                    noise_var);

private:
  void run_thread();
  void accumulate_cpi(const sensing_slot_t& s);
  void process_cpi();
  void interp_freq_row(icf_t* row, const uint8_t* mask, uint32_t n) const;
  void resample_slow_time();
  void write_outputs();
  void write_report_json();
  void report_bus_open();
  void report_bus_close();

  nr_isac_args_t args;
  uint32_t       max_prb = 0;

  // Recycling double-queue: pre-allocated snapshots cycle free_q -> (RT fill) -> ready_q -> (engine) -> free_q
  std::vector<sensing_slot_t> slot_pool;
  ptr_queue                   free_q;
  ptr_queue                   ready_q;

  std::thread       engine_thread;
  std::atomic<bool> running{false};

  // Coherent processing interval (CPI) accumulator — engine thread only.
  //
  // The grid is full per-subcarrier (column = absolute subcarrier relative to CRB0), so reference
  // combs from different sources (CSI-RS comb-12, PDSCH DM-RS comb-2, ...) land on one common axis
  // and fuse. nof_subc = carrier.nof_prb * 12. The grid column spacing is one subcarrier, so the
  // range DSP is fed comb_spacing = 1 (see cpi_grid_comb).
  std::vector<icf_t> h_cpi;            ///< row-major [cpi_slots][nof_subc]
  uint32_t          nof_subc         = 0;
  uint32_t          cpi_row          = 0;  ///< number of committed slow-time rows in the current CPI
  uint32_t          cpi_count        = 0;
  uint32_t          cpi_grid_comb    = 1;  ///< grid column spacing in subcarriers (always 1 for the fused grid)
  uint32_t          cpi_comb_spacing = 0;  ///< comb of the most recent contributing source (diagnostic only)
  double            cpi_period_slots = 0.0; ///< mean slow-time row spacing in slots -- FRACTIONAL (see process_cpi)
  double            prev_frac_       = 0.0; ///< previous submission's within-slot fraction (sub-slot sampling)
  double            cpi_prev_pos     = 0.0; ///< previous submission's absolute slow-time position (slots,
                                            ///< fractional once sub-slot sampling is on)
  uint32_t          cpi_prev_slot    = 0;
  double            cpi_slot_span    = 0.0; ///< span of this CPI's rows in slots -- FRACTIONAL, so sub-slot
                                            ///< rows land at their true slow-time position

  // Monotonic UNWRAPPED absolute-slot counter, advanced on every submission across CPI boundaries, so
  // the true inter-CPI time can be measured for the tracker's dt. cpi_slot_span (the span of a CPI's
  // OWN rows) undercounts dt by the gap between one CPI's last row and the next CPI's first row --
  // measured 0.105 s vs a true 0.147 s/CPI, a 29% bias that made the tracker lag. dt is now
  // (this CPI's anchor - previous CPI's anchor) in unwrapped slots.
  uint64_t          abs_slot_run_    = 0;   ///< running unwrapped slot count
  uint32_t          abs_prev_raw_    = 0;   ///< previous submission's raw (wrapped) slot_idx
  bool              abs_init_        = false;
  uint64_t          cpi_anchor_abs_  = 0;   ///< abs_slot_run_ at this CPI's first row
  uint64_t          prev_cpi_anchor_abs_ = 0;
  bool              have_prev_anchor_ = false;
  nr_isac_carrier_t cpi_carrier      = {};
  uint64_t          src_occ[NR_ISAC_SRC_COUNT] = {0}; ///< per-source occurrence count in the current CPI

  // Stage 4b interpolation state (engine thread only). Frequency interpolation is deferred to CPI
  // close (so a row hit by two sources in the same slot merges before it is gap-filled), hence a
  // full per-row occupancy grid rather than a single reused mask.
  std::vector<double>  cpi_row_time;  ///< Unwrapped slow-time position (in slots) of each accumulated row
  std::vector<icf_t>    h_cpi_uniform; ///< CPI matrix resampled onto a uniform slow-time grid (fed to DSP)
  std::vector<uint8_t> occ_all;       ///< Per-row occupied-subcarrier grid [cpi_slots][nof_subc]

  // Inverse-variance fusion weights: running Σ(1/σ²) accumulated per grid cell, parallel to occ_all.
  // When two sources (e.g. CSI-RS + PDSCH DM-RS) land on the same subcarrier in the same slot, h_cpi
  // holds their inverse-variance-weighted running mean and wsum_all its total weight (see
  // accumulate_cpi). Reset with each row alongside occ_all.
  std::vector<float>   wsum_all;      ///< Per-row accumulated inverse-variance weight [cpi_slots][nof_subc]

  // Per-row native comb (min real-sample subcarrier spacing), used by the DSP to de-alias each row's
  // range profile beyond its comb's unambiguous window (kills sparse-comb grating lobes).
  std::vector<uint32_t> cpi_row_comb;     ///< native comb of each accumulated row
  std::vector<uint32_t> row_comb_uniform; ///< native comb carried onto each uniform (resampled) row
  matrix_complete_scratch mc_scratch;     ///< reusable workspace for slow_time_complete (engine thread)
  double tslot_ema_ = -1.0;               ///< running EMA of per-CPI T_slot for the cpi_quality_gate (neg = unseeded)

  // Phase 1 (ota_sync_passive_ue.md): per-CPI fine-STO tracking/correction on the raw grid, run at
  // CPI close before Stage-4b interpolation. See isac_sync.h and docs/NR_UE_ISAC_sync_gap_analysis.md.
  cpi_sto_tracker  sto_tracker;
  sto_fit_result_t last_sto_fit;

  // Phase 2: residual-CFO estimation + per-row CPE de-rotation, reusing Phase 1's per-row LOS-tap
  // estimates. Runs immediately after sto_tracker.process(), same ordering constraint (before
  // Stage-4b interpolation).
  cpi_cfo_tracker  cfo_tracker;
  cfo_fit_result_t last_cfo_fit;

  // Phase 3: SFO delay-drift fit + correction. Runs its own per-row CIR/peak walk (does not reuse
  // Phase 1's estimates -- see cpi_sfo_tracker's class comment for why), independent of Phase 1/2's
  // corrections (all three commute). Same ordering constraint (before Stage-4b interpolation).
  cpi_sfo_tracker  sfo_tracker;
  sfo_fit_result_t last_sfo_fit;

  // Phase 4: closed-loop LOS pinning, wrapping range_doppler/detection_report's existing output
  // (no second RD/CFAR path). apply_bias_correction() runs alongside Phases 1-3 in the CPI-close
  // block; update_residual() runs in process_cpi(), after rd->process() produces detections/rvm.
  los_baseline_tracker los_tracker;

  // Per-CPI multi-object Kalman tracking over the detection stream (multi_target_tracker.h -> a bank
  // of per-track target_tracker filters + global association + M-of-N initiation). Purely additive:
  // raw detections are unchanged; this adds cross-CPI association + continuous (sub-bin) range/rate
  // estimates that coast through missed CPIs. Single-target operation is the special case
  // track_max_tracks=1, track_confirm_m=1. Gated on args.track_enable.
  std::unique_ptr<multi_target_tracker> tracker;
  std::vector<sensing_track_t>          last_tracks;
  int64_t                         prev_cpi_time_ns = 0;
  los_residual_t        last_los_residual;

  // ---- Receive-array AoA (isac_aoa.h) ----------------------------------------------------------
  // A SEPARATE, raw per-antenna grid running alongside the primary one. It is deliberately not the
  // same buffer: the primary grid is mutated in place by the sync trackers, matrix completion, gap
  // fill and (later) ECA/whitening, all of which are either data-dependent per antenna or irrelevant
  // to a phase difference. Keeping the AoA copy raw means the estimator's chain is provably identical
  // across antennas, which is the whole basis of the measurement (see isac_aoa.h). The occupancy mask
  // (occ_all) and row times (cpi_row_time) are SHARED -- every antenna observes the same REs.
  uint32_t                       aoa_ant_ = 0;  ///< 0/1 => AoA off; no aux grid is allocated
  std::vector<icf_t>             h_cpi_ant;     ///< [nof_ant][cpi_slots][nof_subc], raw
  std::unique_ptr<aoa_estimator> aoa;
  aoa_array_t                    aoa_array;
  bool                           aoa_array_ready_ = false; ///< parsed once the carrier fc is known
  std::vector<aoa_estimate_t>    aoa_out;
  uint32_t                       aoa_ant_seen_ = 0;     ///< max antennas any submission actually carried
  bool                           aoa_warned_   = false; ///< one-shot "configured but single-antenna" warning

  // Range-Doppler processor and its outputs (engine thread only)
  std::unique_ptr<range_doppler>   rd;
  sensing_rvm_t                    rvm;
  std::vector<sensing_detection_t> detections;

  // DetectionReport emission (engine thread only)
  std::string report_path;
  std::string report_endpoint;
  int64_t     cpi_start_time_utc_ns = 0;
  void*       zmq_ctx               = nullptr;
  void*       zmq_pub               = nullptr;

  // Diagnostics
  std::atomic<uint64_t> processed_slots{0};
  std::atomic<uint64_t> dropped_slots{0};
};

} // namespace nr_isac

#endif // NR_ISAC_SENSING_ENGINE_H
