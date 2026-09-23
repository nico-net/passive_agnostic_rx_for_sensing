/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/** Deterministic live-density detector timing diagnostic.
 *
 * Mirrors the observed four-channel, about-192-row, 3276-subcarrier CPI shape and exercises all
 * eight sequential CLEAN passes, including continuous refinement and local statistics. The first
 * call is deliberately excluded so CUDA context, cuFFT-plan, and code-loading startup do not count
 * against steady-state stream throughput.
 */
#include "clean_detector.h"
#include "detector_cuda.h"
#include "report_writer.h"
#include "sync_correction.h"
#include "sync_correction_cuda.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace nr_isac;

namespace {
CfrWindow dense_window()
{
  CfrWindow window;
  window.antennas = 4;
  window.rows = 192;
  window.subcarriers = 3276;
  window.scs_hz = 30000.0;
  window.fc_hz = 3499440000.0;
  const size_t cells = static_cast<size_t>(window.rows) * window.subcarriers;
  window.values.assign(window.antennas * cells, {});
  window.observed.assign(cells, 1);
  window.row_time_slots.resize(window.rows);
  window.row_slot_idx.resize(window.rows);
  window.row_slot_frac.resize(window.rows);
  window.row_source_mask.resize(window.rows);
  // 192 allocation rows distributed across the 32 ms CPI, as in the retained traffic capture.
  for (uint32_t row = 0; row < window.rows; ++row) {
    const double slot = row * (63.5 / (window.rows - 1));
    window.row_time_slots[row] = slot;
    window.row_slot_idx[row] = static_cast<uint32_t>(slot);
    window.row_slot_frac[row] = slot - std::floor(slot);
    window.row_source_mask[row] = row % 4 == 0
                                      ? 1u << NR_ISAC_SRC_PUSCH_DATA
                                      : 1u << NR_ISAC_SRC_PDSCH_DATA;
  }
  const double dwell = (window.row_time_slots.back() - window.row_time_slots.front())
                       * slot_duration_s(window.scs_hz);
  const double rate_resolution = C_MPS * (window.rows - 1.0)
                                 / (window.fc_hz * window.rows * dwell);
  const double ranges[8]{5.25, 13.40, 24.75, 36.15, 49.50, 63.20, 78.35, 94.10};
  const double dopplers[8]{84.2, 88.4, 91.6, 94.7, 97.8, 101.1, 104.3, 107.6};
  const double amplitudes[8]{1.0, .83, .72, .63, .55, .48, .42, .36};
  const double antenna_phase[4]{0.0, .23, -.41, .68};
  uint32_t random_state = 0x51A0AAu;
  for (uint32_t antenna = 0; antenna < window.antennas; ++antenna)
    for (uint32_t row = 0; row < window.rows; ++row) {
      const double time_s = (window.row_time_slots[row] - window.row_time_slots[0])
                            * slot_duration_s(window.scs_hz);
      for (uint32_t subcarrier = 0; subcarrier < window.subcarriers; ++subcarrier) {
        std::complex<double> value;
        for (uint32_t component = 0; component < 8; ++component) {
          const double rate = -(dopplers[component] - window.rows / 2.0) * rate_resolution;
          const double phase = -2.0 * PI * ranges[component] * subcarrier / window.subcarriers
                               - 2.0 * PI * rate * window.fc_hz * time_s / C_MPS
                               + antenna_phase[antenna] * (component + 1.0) / 8.0;
          value += std::polar(amplitudes[component], phase);
        }
        random_state = 1664525u * random_state + 1013904223u;
        const double noise_real = (static_cast<double>(random_state >> 8) / 16777216.0 - .5) * .002;
        random_state = 1664525u * random_state + 1013904223u;
        const double noise_imag = (static_cast<double>(random_state >> 8) / 16777216.0 - .5) * .002;
        value += std::complex<double>(noise_real, noise_imag);
        window.values[window.sample(antenna, row, subcarrier)] = {
            static_cast<float>(value.real()), static_cast<float>(value.imag())};
      }
    }
  return window;
}

// canonical CLEAN (clean_detector.h) accepts one independent receiver; split spatial RF channels
// first, mirroring sensing_engine.cc's independent_receiver_view().
CfrWindow receiver_view(const CfrWindow& input, uint32_t receiver)
{
  CfrWindow output = input;
  output.antennas = 1;
  const size_t cells = static_cast<size_t>(input.rows) * input.subcarriers;
  output.values.assign(input.values.begin() + receiver * cells,
                       input.values.begin() + (receiver + 1) * cells);
  return output;
}

CfrWindow dl_only_view(const CfrWindow& fused)
{
  CfrWindow dl = fused;
  for (uint32_t row = 0; row < dl.rows; ++row) {
    if ((dl.row_source_mask[row] & DL_SOURCE_BITS) != 0) continue;
    dl.row_source_mask[row] = 0;
    for (uint32_t subcarrier = 0; subcarrier < dl.subcarriers; ++subcarrier) {
      dl.observed[dl.cell(row, subcarrier)] = 0;
      for (uint32_t antenna = 0; antenna < dl.antennas; ++antenna)
        dl.values[dl.sample(antenna, row, subcarrier)] = {};
    }
  }
  return dl;
}

// P17(c): one persistent OS thread per receiver lane, reused for every repetition -- a minimal
// port of sensing_engine.cc's SpatialDetectorExecutor (same shape: one dedicated worker thread
// per lane, a mutex/condvar-guarded task queue, std::packaged_task -> std::future). A thread
// spawned fresh per repetition would not do: clean_detector.cc caches its CUDA
// backend/stream in `thread_local` storage (detect_clean(), ~line 1414), so only a genuinely
// persistent per-lane thread keeps that cache warm across repetitions the way the real spatial
// engine's long-lived workers do; a fresh std::thread per call would rebuild the CUDA context
// every time and measure cold-start cost, not steady-state throughput.
class FixedLaneExecutor {
public:
  explicit FixedLaneExecutor(size_t lanes)
  {
    workers_.reserve(lanes);
    while (workers_.size() < lanes) workers_.push_back(std::make_unique<Worker>());
  }

  template <typename F>
  auto submit(size_t lane, F function) -> std::future<decltype(function())>
  {
    return workers_.at(lane)->submit(std::move(function));
  }

private:
  struct Worker {
    Worker() : thread([this] { run(); }) {}
    ~Worker()
    {
      { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
      condition.notify_one();
      if (thread.joinable()) thread.join();
    }

    template <typename F>
    auto submit(F function) -> std::future<decltype(function())>
    {
      using R = decltype(function());
      auto task = std::make_shared<std::packaged_task<R()>>(std::move(function));
      auto future = task->get_future();
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) throw std::runtime_error("benchmark lane worker is stopping");
        tasks.push_back([task] { (*task)(); });
      }
      condition.notify_one();
      return future;
    }

    void run()
    {
      for (;;) {
        std::function<void()> task;
        {
          std::unique_lock<std::mutex> lock(mutex);
          condition.wait(lock, [&] { return stopping || !tasks.empty(); });
          if (stopping && tasks.empty()) return;
          task = std::move(tasks.front());
          tasks.pop_front();
        }
        task();
      }
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::function<void()>> tasks;
    bool stopping = false;
    std::thread thread;
  };

  std::vector<std::unique_ptr<Worker>> workers_;
};

double percentile(std::vector<double> values, double fraction)
{
  std::sort(values.begin(), values.end());
  const size_t index = std::min(values.size() - 1,
      static_cast<size_t>(std::ceil(fraction * values.size())) - 1);
  return values[index];
}
} // namespace

int main()
{
  const bool cpu_baseline = std::getenv("NR_ISAC_BENCHMARK_FORCE_CPU") != nullptr;
  if (cpu_baseline) {
    setenv("NR_ISAC_REQUIRE_CUDA", "0", 1);
    setenv("NR_ISAC_CUDA_DETECTOR", "0", 1);
    setenv("NR_ISAC_DISABLE_CUDA_SYNC", "1", 1);
  } else if (!detector_cuda_available()) {
    std::puts("CUDA detector benchmark skipped: no enabled CUDA device");
    return 77;
  }
  if (!cpu_baseline) {
    setenv("NR_ISAC_REQUIRE_CUDA", "1", 1);
    detector_cuda_warmup();
  }
  PipelineConfig config;
  config.maximum_range_m = 312.283810417;
  config.maximum_target_speed_mps = 50.0;
  // Declared false-alarm budget required by clean_detector.cc's admission gate
  // (config.false_object_intensity_per_s * dwell_s must lie in (0,1)); this fixture's dwell is
  // ~0.0317 s, so 1.0/s keeps the budget comfortably inside range.
  config.false_object_intensity_per_s = 1.0;
  config.capture_rvm = true;
  // P17(c): "everything real time" -- the per-CPI deadline the real spatial engine passes to
  // every per-receiver detect_clean() call (sensing_engine.cc process_receiver(), ~line 1187),
  // and the wall-clock budget one CPI has before the next one arrives.
  config.spatial_detector_deadline_s = SPATIAL_CPI_DURATION_S;
  const CfrWindow window = dense_window();
  const CfrWindow dl_window = dl_only_view(window);
  const std::optional<double> deadline = config.spatial_detector_deadline_s > 0.0
      ? std::optional<double>(config.spatial_detector_deadline_s) : std::nullopt;
  // P17(c): one worker thread per receiver, shared by every run()/run_full_cpi() call below
  // (warmup and timed repetitions alike) so each lane's CUDA backend/stream stays warm exactly
  // as it would in the real long-lived engine.
  FixedLaneExecutor lanes(window.antennas);
  // Canonical CLEAN accepts one independent receiver: keep the workload by running the four
  // per-receiver detections of this 4x192x3276 CPI CONCURRENTLY (P17(c) -- mirrors
  // SpatialDetectorExecutor: one lane's detection is gated by nothing but its own latency, not
  // by the other three finishing first) and timing the whole concurrent batch (P11's original
  // sequential timing is superseded here).
  auto run = [&]() {
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::future<DetectorResult>> futures;
    futures.reserve(window.antennas);
    for (uint32_t receiver = 0; receiver < window.antennas; ++receiver)
      futures.push_back(lanes.submit(receiver, [&, receiver] {
        return detect_clean(receiver_view(window, receiver), config, RateGate{}, 0,
                            std::nullopt, deadline);
      }));
    for (uint32_t receiver = 0; receiver < futures.size(); ++receiver) {
      const auto result = futures[receiver].get();
      if (result.components.empty() || result.initial_likelihood.empty()) {
        std::fprintf(stderr,
                     "dense CUDA detector returned %zu components and %zu map cells (rx%u)\n",
                     result.components.size(), result.initial_likelihood.size(), receiver);
        std::exit(EXIT_FAILURE);
      }
      for (const auto& component : result.components)
        if (!component.local.valid || !std::isfinite(component.local.z)) {
          std::fprintf(stderr, "dense CUDA detector lost a component local statistic (rx%u)\n",
                       receiver);
          std::exit(EXIT_FAILURE);
        }
    }
    const auto stopped = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(stopped - started).count();
  };
  if (!cpu_baseline) {
    const double warmup_ms = run();
    std::vector<double> samples;
    for (uint32_t repetition = 0; repetition < 5; ++repetition)
      samples.push_back(run());
    std::vector<double> sorted_samples = samples;
    std::sort(sorted_samples.begin(), sorted_samples.end());
    const double median_ms = sorted_samples[sorted_samples.size() / 2];
    const double p95_ms = percentile(samples, 0.95);
    const char* configured_limit = std::getenv("NR_ISAC_CUDA_BENCHMARK_MAX_MS");
    const double limit_ms = configured_limit ? std::strtod(configured_limit, nullptr)
                                             : SPATIAL_CPI_DURATION_S * 1000.0;
    std::printf("CUDA detector dense CPI (4 receivers concurrent): warmup=%.3f ms min=%.3f ms "
                "median=%.3f ms p95=%.3f ms max=%.3f ms limit=%.3f ms\n",
                warmup_ms, sorted_samples.front(), median_ms, p95_ms, sorted_samples.back(),
                limit_ms);
    if (!(median_ms < limit_ms)) {
      std::fprintf(stderr, "CUDA detector misses required steady-state CPI throughput\n");
      return EXIT_FAILURE;
    }
  }

  if (!cpu_baseline) {
    std::string sync_error;
    if (!warmup_sync_cuda(512, window.subcarriers, &sync_error)) {
      std::fprintf(stderr, "CUDA sync warmup failed before full-CPI benchmark: %s\n",
                   sync_error.c_str());
      return EXIT_FAILURE;
    }
  }
  struct FullSample {
    double total = 0.0, copy = 0.0, sync = 0.0, correction = 0.0;
    double variance = 0.0, alignment = 0.0, detector = 0.0, report = 0.0;
  };
  auto run_full_cpi = [&]() {
    const auto started = std::chrono::steady_clock::now();
    CfrWindow corrected = window;
    CfrWindow dl_corrected = dl_window;
    const auto copied = std::chrono::steady_clock::now();
    const SyncEstimate sync = estimate_sync(window);
    const auto synchronized = std::chrono::steady_clock::now();
    apply_sync_correction(corrected, sync, 0.0, std::nullopt);
    apply_sync_correction(dl_corrected, sync, 0.0, std::nullopt);
    const auto corrected_at = std::chrono::steady_clock::now();
    (void)estimate_current_cpi_variance(corrected);
    const auto variance_estimated = std::chrono::steady_clock::now();
    CfrWindow detector_input = corrected;
    align_allocation_families(detector_input, true);
    align_allocation_families(dl_corrected, true);
    const auto allocation_aligned = std::chrono::steady_clock::now();
    PipelineReport report;
    report.sync = sync;
    // Same concurrent per-receiver split as `run()` above (P17(c)), on the SAME persistent lanes
    // -- the timed span below covers all four receivers' detections running at once, gated by
    // the same spatial_detector_deadline_s. Uses plain detect_clean(), not
    // detect_clean_with_diagnostic(): that is what the real spatial per-receiver call
    // (sensing_engine.cc process_receiver(), ~line 1187) actually calls, and it is the only one
    // of the two that accepts a deadline at all. Mirrors that call site's own DL-only map
    // shortcut too (~line 1192): the fused map IS the DL-only map there, aliased rather than
    // computed by a second pass.
    std::vector<std::future<DetectorResult>> detector_futures;
    detector_futures.reserve(detector_input.antennas);
    for (uint32_t receiver = 0; receiver < detector_input.antennas; ++receiver)
      detector_futures.push_back(lanes.submit(receiver, [&, receiver] {
        return detect_clean(receiver_view(detector_input, receiver), config, RateGate{}, 0,
                            std::nullopt, deadline);
      }));
    for (uint32_t receiver = 0; receiver < detector_futures.size(); ++receiver) {
      auto rx_result = detector_futures[receiver].get();
      if (receiver == 0) {
        report.detector = std::move(rx_result);
        report.detector.initial_dl_likelihood = report.detector.initial_likelihood;
        report.detector.dl_observed_re_count = report.detector.axes.observed_re_count;
      }
    }
    const auto detected = std::chrono::steady_clock::now();
    const std::string json = build_report_json(report, config, config.capture_rvm);
    const auto stopped = std::chrono::steady_clock::now();
    if (json.find("\"dl_rvm_blob\":[") == std::string::npos)
      std::exit(EXIT_FAILURE);
    const auto elapsed = [](auto end, auto begin) {
      return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    return FullSample{elapsed(stopped, started), elapsed(copied, started),
                      elapsed(synchronized, copied), elapsed(corrected_at, synchronized),
                      elapsed(variance_estimated, corrected_at),
                      elapsed(allocation_aligned, variance_estimated),
                      elapsed(detected, allocation_aligned),
                      elapsed(stopped, detected)};
  };
  const FullSample full_warmup = cpu_baseline ? FullSample{} : run_full_cpi();
  std::vector<FullSample> full_samples;
  const uint32_t repetitions = cpu_baseline ? 1 : 5;
  for (uint32_t repetition = 0; repetition < repetitions; ++repetition)
    full_samples.push_back(run_full_cpi());
  const auto field_values = [&](double FullSample::*field) {
    std::vector<double> values;
    for (const auto& sample : full_samples) values.push_back(sample.*field);
    return values;
  };
  const auto median_field = [&](double FullSample::*field) {
    auto values = field_values(field);
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
  };
  const double full_median_ms = median_field(&FullSample::total);
  const double full_p95_ms = percentile(field_values(&FullSample::total), 0.95);
  const auto [minimum_full, maximum_full] = std::minmax_element(
      full_samples.begin(), full_samples.end(),
      [](const FullSample& left, const FullSample& right) { return left.total < right.total; });
  const char* configured_full_limit = std::getenv("NR_ISAC_CUDA_FULL_CPI_MAX_MS");
  const double full_limit_ms = configured_full_limit
                                   ? std::strtod(configured_full_limit, nullptr)
                                   : SPATIAL_CPI_DURATION_S * 1000.0;
  std::printf("%s full CPI (copy+sync+correction+variance+alignment+4-receiver-concurrent-"
              "detector+report): warmup=%.3f ms min=%.3f ms median=%.3f ms p95=%.3f ms "
              "max=%.3f ms limit=%.3f ms\n",
              cpu_baseline ? "CPU" : "CUDA", full_warmup.total,
              minimum_full->total, full_median_ms, full_p95_ms,
              maximum_full->total, full_limit_ms);
  std::printf("%s full CPI median stages: copy=%.3f sync=%.3f correction=%.3f "
              "variance=%.3f alignment=%.3f fused_plus_dl_detector=%.3f report=%.3f ms\n",
              cpu_baseline ? "CPU" : "CUDA", median_field(&FullSample::copy),
              median_field(&FullSample::sync),
              median_field(&FullSample::correction),
              median_field(&FullSample::variance), median_field(&FullSample::alignment),
              median_field(&FullSample::detector), median_field(&FullSample::report));
  if (!(full_median_ms < full_limit_ms)) {
    std::fprintf(stderr, "%s full pipeline misses required bounded-backlog CPI throughput\n",
                 cpu_baseline ? "CPU" : "CUDA");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
