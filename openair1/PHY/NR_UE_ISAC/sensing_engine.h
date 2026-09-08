/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "hierarchical_tracker.h"
#include "report_writer.h"

#include <atomic>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace nr_isac {

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
              float noise_variance);

private:
  struct Snapshot;
  struct PendingRow;
  struct WindowTask {
    CfrWindow window;
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
  void enqueue_window(CfrWindow window, const CpiPlan& plan);
  size_t pending_row_storage_bytes(const PendingRow& row) const;
  void make_pending_row_room(size_t incoming_bytes);
  void erase_rows(const std::vector<int64_t>& keys);
  void consume(const Snapshot& snapshot);
  int64_t unwrap_slot(uint32_t slot, const nr_isac_carrier_t& carrier);
  void begin_geometry(const nr_isac_carrier_t& carrier);
  void ensure_plan();
  void close_ready_windows(bool flush);
  CfrWindow build_window(const std::vector<int64_t>& keys) const;
  void process_window(CfrWindow window, const CpiPlan& plan, double air_origin_slots,
                      uint64_t sequence);
  TrackSnapshot planning_snapshot(double time_s) const;
  std::vector<ConfirmedTrackView> confirmed_tracks() const;
  AdaptiveClutterMap* clutter_map();

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
  std::atomic<uint64_t> discarded_pending_rows_{0};
  std::atomic<uint64_t> discarded_pending_intervals_{0};
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
  IndependentClockTracker clock_tracker_;
  CpiPlanner planner_;
  std::unique_ptr<MotionTracker> motion_tracker_;
  std::unique_ptr<HierarchicalEnuTracker> hierarchical_tracker_;
  mutable std::mutex tracker_mutex_;
  std::unique_ptr<ReportWriter> writer_;
};

} // namespace nr_isac
