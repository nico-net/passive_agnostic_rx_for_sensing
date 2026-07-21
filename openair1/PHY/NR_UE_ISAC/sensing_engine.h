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
#include "isac_sync.h"
#include "range_doppler.h"

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
              nr_isac_source_t         source,
              const nr_isac_carrier_t& carrier,
              const icf_t*              h,
              const uint32_t*          k_abs,
              const uint32_t*          l,
              uint32_t                 nof_re);

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
  uint32_t          cpi_period_slots = 0;
  uint32_t          cpi_prev_slot    = 0;
  uint64_t          cpi_slot_span    = 0;
  nr_isac_carrier_t cpi_carrier      = {};
  uint64_t          src_occ[NR_ISAC_SRC_COUNT] = {0}; ///< per-source occurrence count in the current CPI

  // Stage 4b interpolation state (engine thread only). Frequency interpolation is deferred to CPI
  // close (so a row hit by two sources in the same slot merges before it is gap-filled), hence a
  // full per-row occupancy grid rather than a single reused mask.
  std::vector<double>  cpi_row_time;  ///< Unwrapped slow-time position (in slots) of each accumulated row
  std::vector<icf_t>    h_cpi_uniform; ///< CPI matrix resampled onto a uniform slow-time grid (fed to DSP)
  std::vector<uint8_t> occ_all;       ///< Per-row occupied-subcarrier grid [cpi_slots][nof_subc]

  // Per-row native comb (min real-sample subcarrier spacing), used by the DSP to de-alias each row's
  // range profile beyond its comb's unambiguous window (kills sparse-comb grating lobes).
  std::vector<uint32_t> cpi_row_comb;     ///< native comb of each accumulated row
  std::vector<uint32_t> row_comb_uniform; ///< native comb carried onto each uniform (resampled) row

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
  los_residual_t        last_los_residual;

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
