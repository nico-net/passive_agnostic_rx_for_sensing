/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include "coherent_autofocus.h"
#include "coherent_core.h"
#include "coherent_cuda.h"
#include "coherent_report.h"
#include "coherent_tracker.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace nr_isac::coherent {

struct CoherentStats { uint64_t processed = 0, skipped = 0, overruns = 0, queue_waits = 0; double last_ms = 0; };

/** Coherent array fuser: one worker thread runs sync -> range-Doppler -> calibration -> envelope ->
 * detection -> refinement -> tracking -> autofocus per CPI and writes coherent_reports / coherent_tracks
 * / coherence JSONL next to report_path. submit() never drops: it waits while two CPIs are queued. */
class CoherentPipeline {
public:
  explicit CoherentPipeline(const CoherentConfig& cfg);
  ~CoherentPipeline();                      // drains the queue, joins the worker
  void submit(CfrWindow dl, std::vector<CfrWindow> ul, uint64_t sequence, double air_time_s);
  /** DL traffic state from the flow gate (called ~1/s). On close: tracks are dropped, no detections are
   * emitted, and a "traffic":false line is written to reports and tracks (repeated while closed, so
   * the UI can tell "no traffic" from "no data"). Never blocks. */
  void traffic(bool open);
  CoherentStats stats() const;

private:
  struct Job { CfrWindow dl; std::vector<CfrWindow> ul; uint64_t seq = 0; double t = 0; int kind = 0; bool traffic = true; };   // kind 1: traffic event
  void run();
  void process(Job& job);
  void write_coherence(uint64_t seq, double t, const Calibration& cal, const std::array<double, kCh>& los_delay_s, bool skipped);
  CoherentConfig cfg_;
  Calibrator cal_;
  std::array<std::vector<double>, kCh> los_resid_;   // per-channel LOS delay residual history (s)
  TrackerParams tp_;
  std::unique_ptr<CoherentTracker> tracker_;
  bool traffic_open_ = true;   // guarded by mu_
  double last_t_ = 0;          // last CPI air time, for traffic events (guarded by mu_)
  std::unique_ptr<Autofocus> af_;
  std::unique_ptr<CudaCoherent> cuda_;   // Task 10: GPU range_doppler()/envelope() when available
  bool cuda_fail_closed_ = false;        // NR_ISAC_REQUIRE_CUDA=1 and no device: skip every CPI
  JsonlSink reports_, tracks_, coherence_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> q_;
  bool stop_ = false;
  CoherentStats st_;
  std::chrono::steady_clock::time_point last_image_{};
  std::thread worker_;                      // last: started after every member above exists
};

} // namespace nr_isac::coherent
