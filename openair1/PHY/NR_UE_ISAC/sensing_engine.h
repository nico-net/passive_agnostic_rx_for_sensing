/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "hierarchical_tracker.h"
#include "multistatic_imm_tracker.h"
#include "report_writer.h"
#include "causal_clutter_filter.h"

#include <atomic>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace nr_isac {

class SpatialDetectorExecutor;

/** Engine threads leave the PHY's SCHED_FIFO class and, when NR_ISAC_CPUS="3,12,13" is set, run
 *  only on those cores -- a std::thread created from a FIFO PHY thread inherits FIFO otherwise.
 *  Declared here so non-engine threads (the gate watchdog in nr_isac.cc) can share it too. */
void pin_current_thread_from_env();

class SensingEngine {
public:
  SensingEngine(PipelineConfig config, uint32_t maximum_prb, uint32_t requested_antennas);
  ~SensingEngine();
  void start();
  void stop();
  void submit(uint32_t slot_idx, float slot_fraction, nr_isac_source_t source,
              const nr_isac_carrier_t& carrier, const std::complex<float>* cfr,
              uint32_t antennas, const uint32_t* absolute_subcarrier,
              const uint32_t* ofdm_symbol, uint32_t resource_elements,
              float noise_variance, uint64_t session_id = 0);
  // Offline-harness accounting (2026-09-20): CPIs are anchored at the first pending row, not at
  // 150-slot multiples, and non-viable windows emit nothing, so a replay must wait on what the
  // engine actually enqueued rather than on a slot-count prediction.
  uint64_t enqueued_cpis() const { return enqueued_cpis_.load(std::memory_order_relaxed); }
  uint64_t dropped_cpis() const { return dropped_cpis_.load(std::memory_order_relaxed); }
  // True once every accepted submit() has been consumed by the accumulation thread, so any window
  // closable from the submitted rows has already been enqueued (or rejected as non-viable).
  bool submissions_drained() const {
    return consumed_submissions_.load(std::memory_order_acquire) == accepted_submissions_.load(std::memory_order_acquire);
  }
  // Drained and no CPI in flight. A window closed by a consumed row marks itself in flight before
  // that row counts as consumed, so this never reports idle between the two.
  bool idle() const { return submissions_drained() && !processing_in_flight(); }

  /** Gate closed: drop every pending (not yet windowed) row once the accumulation thread has
   *  consumed everything that was already queued at request time -- a snapshot already sitting in
   *  ready_ belongs to the still-open interval and must survive the discard, not just the ones
   *  submitted after the request. Counted apart from discarded_pending_rows, which keeps meaning
   *  "loss". */
  void request_discard_pending()
  {
    discard_after_.store(accepted_submissions_.load(std::memory_order_acquire), std::memory_order_relaxed);
    discard_requested_.store(true, std::memory_order_release);
  }
  uint64_t gate_discarded_rows() const { return gate_discarded_rows_.load(std::memory_order_relaxed); }

private:
  struct Snapshot;
  struct PendingRow;
  struct WindowTask {
    CfrWindow dl_window;
    std::vector<CfrWindow> ul_windows;
    CpiPlan plan;
    double air_origin_slots = 0.0;
    uint64_t sequence = 0;
  };

  class PointerQueue {
  public:
    void push(Snapshot* value);
    bool try_pop(Snapshot*& value);
    Snapshot* wait_pop();
  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Snapshot*> queue_;
  };

  class WindowQueue {
  public:
    void push(std::unique_ptr<WindowTask> value);
    std::unique_ptr<WindowTask> wait_pop();
    void close();
    void reopen();
  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::unique_ptr<WindowTask>> queue_;
    bool closed_ = false;
  };

  void accumulation_run();
  void processing_run();
  bool processing_in_flight() const;
  void wait_for_processing();
  void finish_pending_windows();
  void enqueue_window(CfrWindow dl_window, std::vector<CfrWindow> ul_windows,
                      const CpiPlan& plan);
  size_t pending_row_storage_bytes(const PendingRow& row) const;
  void make_pending_row_room(size_t incoming_bytes);
  void erase_rows(const std::vector<int64_t>& keys);
  void discard_pending_rows();
  void maybe_discard_pending();
  void consume(const Snapshot& snapshot);
  int64_t unwrap_submission_slot(uint32_t slot, const nr_isac_carrier_t& carrier);
  void begin_geometry(const nr_isac_carrier_t& carrier);
  void ensure_plan();
  void close_ready_windows(bool flush);
  CfrWindow build_window(const std::vector<int64_t>& keys, bool uplink,
                         uint32_t forced_antennas = 0,
                         uint64_t selected_session = 0) const;
  void process_window(CfrWindow dl_window, std::vector<CfrWindow> ul_windows,
                      const CpiPlan& plan, double air_origin_slots,
                      uint64_t sequence);
  TrackSnapshot planning_snapshot(double time_s) const;

  PipelineConfig config_;
  uint32_t maximum_prb_ = 0;
  uint32_t requested_antennas_ = 1;
  uint32_t maximum_re_ = 0;
  std::vector<std::unique_ptr<Snapshot>> pool_;
  PointerQueue free_, ready_;
  WindowQueue windows_;
  std::thread accumulation_worker_;
  std::thread processing_worker_;
  std::atomic<bool> running_{false};
  std::mutex submission_mutex_;
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> dropped_cpis_{0};
  std::atomic<uint64_t> enqueued_cpis_{0};
  std::atomic<uint64_t> accepted_submissions_{0};
  std::atomic<uint64_t> consumed_submissions_{0};
  std::atomic<uint64_t> discarded_pending_rows_{0};
  std::atomic<uint64_t> discarded_pending_intervals_{0};
  std::atomic<bool> discard_requested_{false};
  std::atomic<uint64_t> discard_after_{0};
  std::atomic<uint64_t> gate_discarded_rows_{0};
  std::atomic<uint64_t> stale_{0};
  mutable std::mutex processing_mutex_;
  std::condition_variable processing_condition_;
  bool processing_in_flight_ = false;

  nr_isac_carrier_t carrier_{};
  bool have_geometry_ = false;
  // A nanosecond-of-slot key merges genuinely co-timed submissions without rounding measured
  // sub-slot positions onto the 1/28-slot allocation-family lattice.  The latter rounding belongs
  // only in the family key, exactly as in the Python pipeline.
  std::map<int64_t, PendingRow> rows_;
  size_t pending_row_bytes_ = 0;
  bool have_slot_clock_ = false;
  int64_t latest_absolute_slot_ = 0;
  uint32_t latest_raw_slot_ = 0;
  std::optional<double> air_origin_slots_;
  std::optional<double> last_closed_slots_;
  std::optional<CpiPlan> active_plan_;
  uint64_t cpi_sequence_ = 0;
  IndependentClockTracker dl_clock_tracker_;
  IndependentClockTracker ul_clock_tracker_;
  std::array<CausalClutterFilter, 4> spatial_clutter_filters_;
  // (2026-09-22) per-receiver ring of the last corrected (post-clutter) DL windows for nested dwells
  std::array<std::deque<CfrWindow>, 4> dwell_ring_;
  // (2026-09-22) UL accumulation gate: per (PUSCH session, receiver) ring of corrected UL windows.
  // The UL leg gets 14-31 PUSCH DMRS rows per 75 ms CPI against 84-96 on DL, so the direct path's
  // slow-time sidelobes sit only ~28 dB down and cover range bins 0-3 -- exactly where a target
  // passing near the UE lands. The gate accumulates consecutive UL CPIs until a declared row count
  // or a declared maximum dwell is reached, whichever comes first.
  std::map<std::pair<uint64_t, uint32_t>, std::deque<CfrWindow>> ul_dwell_ring_;
  // PUSCH clutter state is transmitter-specific. Sharing it across RNTIs would subtract one UE's
  // channel from another and manufacture DTD/DFS residuals.
  std::map<uint64_t, std::array<CausalClutterFilter, 4>> spatial_ul_clutter_filters_;
  std::map<uint64_t, uint64_t> spatial_ul_filter_last_used_;
  CpiPlanner planner_;
  std::unique_ptr<MotionTracker> motion_tracker_;
  std::unique_ptr<MotionTracker> ul_motion_tracker_;
  std::unique_ptr<HierarchicalEnuTracker> hierarchical_tracker_;
  std::unique_ptr<MultistaticImmTracker> multistatic_tracker_;
  std::unique_ptr<SpatialDetectorExecutor> spatial_detector_executor_;
  mutable std::mutex tracker_mutex_;
  std::unique_ptr<ReportWriter> writer_;
};

} // namespace nr_isac
