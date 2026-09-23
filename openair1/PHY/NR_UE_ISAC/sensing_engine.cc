/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "sensing_engine.h"

#include "detector_cuda.h"

#include "aoa.h"
#include "cross_leg_fusion.h"
#include "cuda_support.h"
#include "clean_detector.h"
#include "sync_correction.h"
#ifdef NR_ISAC_FIXED_WORK_REPLAY
#include "diagnostic_clean_budget.h"
#include "ul_probe.h"
#endif
#ifdef NR_ISAC_CUDA_ACCELERATION
#include "sync_correction_cuda.h"
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <future>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <pthread.h>
#include <sched.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace nr_isac {

namespace {
constexpr int64_t ROW_TICKS_PER_SLOT = 1000000000LL;

PipelineConfig normalize_runtime_config(PipelineConfig config)
{
  if (config.spatial_receivers.configured) {
    // The multireceiver architecture has one declared, immutable coherent interval.  Tracker state
    // and detections may adapt, but neither is allowed to feed back into CPI selection.
    // (2026-09-21) NR_ISAC_SPATIAL_CPI_S overrides the declared dwell for the multi-dwell A/B
    // (longer coherent intervals for slow targets); still immutable within a run, never data-driven.
    double dwell = SPATIAL_CPI_DURATION_S;
    if (const char* v = std::getenv("NR_ISAC_SPATIAL_CPI_S")) { const double x = std::atof(v); if (x > 0.0) dwell = x; }
    config.duration_bank_s = {dwell};
    config.bootstrap_duration_index = 0;
    config.minimum_dwell_s = dwell;
    config.maximum_dwell_s = dwell;
  }
  return config;
}

int64_t row_key(int64_t absolute_slot, double fraction)
{
  const int64_t tick = std::clamp<int64_t>(
      std::llround(fraction * ROW_TICKS_PER_SLOT), 0, ROW_TICKS_PER_SLOT - 1);
  const long double expanded = static_cast<long double>(absolute_slot) * ROW_TICKS_PER_SLOT + tick;
  if (expanded > std::numeric_limits<int64_t>::max()
      || expanded < std::numeric_limits<int64_t>::min())
    throw std::overflow_error("slow-time slot counter exceeds native row-key range");
  return absolute_slot * ROW_TICKS_PER_SLOT + tick;
}

bool multi_dwell_enabled()
{
  static const bool value = [] { const char* v = std::getenv("NR_ISAC_MULTI_DWELL"); return v && *v == '1'; }();
  return value;
}

/** Concatenate the rows of the last n windows of a receiver (same antennas/subcarriers) into one
 *  coherent window; row times keep their absolute slot values so the plan measures the true dwell. */
// (2026-09-22) UL accumulation gate. Declared before evaluation, not fitted to any scene:
// NR_ISAC_UL_DWELL_ROWS defaults to 90, the measured DL row count per CPI at which the direct
// path's slow-time sidelobes fall ~45 dB (UL at 14 rows: 28 dB); NR_ISAC_UL_DWELL_MAX_S defaults
// to 0.30 s, the dwell over which a 5 m/s target stays inside one 3.05 m range cell.
uint32_t ul_dwell_target_rows()
{
  static const uint32_t value = [] { const char* v = std::getenv("NR_ISAC_UL_DWELL_ROWS");
    const double x = v ? std::atof(v) : 0.0; return x > 0.0 ? static_cast<uint32_t>(x) : 90u; }();
  return value;
}
double ul_dwell_max_s()
{
  static const double value = [] { const char* v = std::getenv("NR_ISAC_UL_DWELL_MAX_S");
    const double x = v ? std::atof(v) : 0.0; return x > 0.0 ? x : 0.30; }();
  return value;
}
bool ul_dwell_enabled()
{
  static const bool value = [] { const char* v = std::getenv("NR_ISAC_UL_DWELL"); return v && *v == '1'; }();
  return value;
}

CfrWindow merge_windows(const std::deque<CfrWindow>& ring, uint32_t n)
{
  CfrWindow out;
  const size_t first = ring.size() - n;
  const CfrWindow& ref = ring[first];
  out.session_id = ref.session_id; out.antennas = ref.antennas; out.subcarriers = ref.subcarriers;
  out.scs_hz = ref.scs_hz; out.fc_hz = ref.fc_hz; out.pci = ref.pci; out.start_utc_ns = ref.start_utc_ns;
  uint32_t rows = 0;
  for (size_t i = first; i < ring.size(); ++i) {
    if (ring[i].antennas != ref.antennas || ring[i].subcarriers != ref.subcarriers) return CfrWindow{};
    rows += ring[i].rows;
  }
  out.rows = rows;
  out.values.resize((size_t)out.antennas * rows * out.subcarriers);
  out.observed.resize((size_t)rows * out.subcarriers);
  out.row_time_slots.reserve(rows); out.row_slot_idx.reserve(rows); out.row_slot_frac.reserve(rows); out.row_source_mask.reserve(rows);
  uint32_t r0 = 0;
  for (size_t i = first; i < ring.size(); ++i) {
    const CfrWindow& w = ring[i];
    for (uint32_t a = 0; a < w.antennas; ++a)
      std::copy(w.values.begin() + (size_t)a * w.rows * w.subcarriers,
                w.values.begin() + (size_t)(a + 1) * w.rows * w.subcarriers,
                out.values.begin() + ((size_t)a * rows + r0) * out.subcarriers);
    std::copy(w.observed.begin(), w.observed.end(), out.observed.begin() + (size_t)r0 * out.subcarriers);
    out.row_time_slots.insert(out.row_time_slots.end(), w.row_time_slots.begin(), w.row_time_slots.end());
    out.row_slot_idx.insert(out.row_slot_idx.end(), w.row_slot_idx.begin(), w.row_slot_idx.end());
    out.row_slot_frac.insert(out.row_slot_frac.end(), w.row_slot_frac.begin(), w.row_slot_frac.end());
    out.row_source_mask.insert(out.row_source_mask.end(), w.row_source_mask.begin(), w.row_source_mask.end());
    for (size_t k = 0; k < w.source_occurrences.size(); ++k) out.source_occurrences[k] += w.source_occurrences[k];
    r0 += w.rows;
  }
  return out;
}

CfrWindow independent_receiver_view(const CfrWindow& input, uint32_t receiver)
{
  if (!input.valid() || receiver >= input.antennas)
    throw std::invalid_argument("independent receiver index is outside the CFR window");
  CfrWindow output;
  output.session_id = input.session_id;
  output.antennas = 1;
  output.rows = input.rows;
  output.subcarriers = input.subcarriers;
  output.scs_hz = input.scs_hz;
  output.fc_hz = input.fc_hz;
  output.pci = input.pci;
  output.start_utc_ns = input.start_utc_ns;
  output.observed = input.observed;
  output.row_time_slots = input.row_time_slots;
  output.row_slot_idx = input.row_slot_idx;
  output.row_slot_frac = input.row_slot_frac;
  output.row_source_mask = input.row_source_mask;
  output.source_occurrences = input.source_occurrences;
  const size_t cells = static_cast<size_t>(input.rows) * input.subcarriers;
  output.values.assign(input.values.begin() + receiver * cells,
                       input.values.begin() + (receiver + 1) * cells);
  return output;
}

// OUR ADAPTATION: clean_detector.cc's per-component Fisher-information covariance can be
// technically positive-definite but numerically near-singular, inverting to an unbounded value.
// Nothing previously caps a covariance that is too LARGE (MotionTracker's own noise floor only
// raises one that is too small), so an inflated value would reach any chi2-based consumer
// unmodified. Capping inside clean_detector.cc itself changes covariance_valid, which
// component_scale()/collapse_unresolved/tag_micro_doppler_families all consume for their
// grouping radius -- capping there would perturb grouping and cascade into later CLEAN
// iterations. Capping HERE instead, after clean_detector.cc has finished all of its own internal
// grouping, changes only what is reported. The bound is the same already-used, immutable
// quantity component_scale() clamps its own consumed doppler scale to (half the searched
// unambiguous rate axis), converted to physical units and squared for variance; the analogous
// bound is used for range (the full searched range axis). Not fit to any capture's outcome.
void cap_reported_covariance(Matrix& covariance, const Axes& axes)
{
  const double range_bound = std::pow(axes.range_res_m * axes.range_bins, 2.0);
  const double rate_bound = std::pow(axes.rate_res_mps * axes.rate_bins / 2.0, 2.0);
  bool clamped = false;
  if (!(covariance(0, 0) <= range_bound)) { covariance(0, 0) = range_bound; clamped = true; }
  if (!(covariance(1, 1) <= rate_bound)) { covariance(1, 1) = rate_bound; clamped = true; }
  if (clamped) covariance(0, 1) = covariance(1, 0) = 0.0;
}
} // namespace

/** Engine threads leave the PHY's SCHED_FIFO class and, when NR_ISAC_CPUS="3,12,13" is set, run
 *  only on those cores -- a std::thread created from a FIFO PHY thread inherits FIFO otherwise. */
void pin_current_thread_from_env()
{
  // Fix round 1 (P20): std::stoi throws std::invalid_argument/out_of_range on a garbage or
  // out-of-range NR_ISAC_CPUS token, and an uncaught exception at the top of an engine thread is
  // std::terminate -- the whole UE process aborts. Parse by hand instead; never throw.
  static std::atomic<bool> warned{false};
  sched_param sp{}; sp.sched_priority = 0;
  if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp) != 0 && !warned.exchange(true))
    std::fprintf(stderr, "SENSING: pthread_setschedparam(SCHED_OTHER) failed for an engine thread\n");
  const char* cpus = std::getenv("NR_ISAC_CPUS");
  if (!cpus || !*cpus) return;
  cpu_set_t set; CPU_ZERO(&set);
  bool any = false;
  std::stringstream s(cpus); std::string tok;
  while (std::getline(s, tok, ',')) {
    if (tok.empty()) continue;
    errno = 0;
    char* end = nullptr;
    const long cpu = std::strtol(tok.c_str(), &end, 10);
    if (end == tok.c_str() || *end != '\0' || errno == ERANGE || cpu < 0 || cpu >= CPU_SETSIZE) {
      if (!warned.exchange(true))
        std::fprintf(stderr,
                     "SENSING: NR_ISAC_CPUS token '%s' is not a valid core id (0..%d); ignored\n",
                     tok.c_str(), CPU_SETSIZE - 1);
      continue;
    }
    CPU_SET(static_cast<int>(cpu), &set);
    any = true;
  }
  if (any && pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0 && !warned.exchange(true))
    std::fprintf(stderr, "SENSING: pthread_setaffinity_np failed for an engine thread\n");
}

struct SpatialReceiverProduct {
  SpatialReceiverReport report;
  ReceiverDetectionBatch batch;
  std::optional<UlDifferentialReceiverBatch> ul_batch;
};

/** A bounded persistent lane per illumination/RX pair preserves thread-local FFT/CUDA state.
 *
 * Four DL lanes and four lanes for every configured PUSCH session let independent receiver and
 * illuminator views execute concurrently. The supported UE count is bounded at configuration
 * admission, so this never becomes an unbounded task/thread pool. Assigning a stable lane also
 * prevents one RNTI from reusing another RNTI's thread-local detector workspace mid-CPI.
 */
class SpatialDetectorExecutor {
public:
  explicit SpatialDetectorExecutor(size_t worker_count)
  {
    if (!worker_count) throw std::invalid_argument("spatial detector executor needs workers");
    workers_.reserve(worker_count);
    while (workers_.size() < worker_count)
      workers_.push_back(std::make_unique<Worker>());
  }

  std::future<SpatialReceiverProduct> submit(
      size_t lane, std::function<SpatialReceiverProduct()> function)
  {
    if (lane >= workers_.size())
      throw std::invalid_argument("spatial detector lane outside configured illumination count");
    return workers_[lane]->submit(std::move(function));
  }

  size_t size() const { return workers_.size(); }

private:
  using Task = std::packaged_task<SpatialReceiverProduct()>;

  struct Worker {
    Worker() : thread([this] { run(); }) {}
    ~Worker()
    {
      {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
      }
      condition.notify_one();
      if (thread.joinable()) thread.join();
    }

    std::future<SpatialReceiverProduct> submit(
        std::function<SpatialReceiverProduct()> function)
    {
      Task task(std::move(function));
      auto future = task.get_future();
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) throw std::runtime_error("spatial detector worker is stopping");
        tasks.push_back(std::move(task));
      }
      condition.notify_one();
      return future;
    }

    void run()
    {
      pin_current_thread_from_env();
      for (;;) {
        Task task;
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
    std::deque<Task> tasks;
    bool stopping = false;
    std::thread thread;
  };

  std::vector<std::unique_ptr<Worker>> workers_;
};

struct SensingEngine::Snapshot {
  uint32_t slot = 0;
  int64_t absolute_slot = 0;
  float fraction = 0.0f;
  nr_isac_source_t source = NR_ISAC_SRC_CSI_RS;
  nr_isac_carrier_t carrier{};
  uint64_t session_id = 0;
  uint32_t antennas = 1;
  uint32_t resource_elements = 0;
  float noise_variance = 0.0f;
  int64_t utc_ns = 0;
  std::vector<std::complex<float>> cfr;
  std::vector<uint32_t> subcarrier;
  std::vector<uint32_t> symbol;
};

struct SensingEngine::PendingRow {
  struct UplinkView {
    uint32_t source_mask = 0;
    std::array<uint64_t, NR_ISAC_SRC_COUNT> source_occurrences{};
    std::vector<std::complex<float>> cfr;
    std::vector<float> weights;
  };
  double time_slots = 0.0;
  uint32_t raw_slot = 0;
  double slot_fraction = 0.0;
  uint32_t source_mask = 0;
  uint32_t dl_source_mask = 0;
  std::array<uint64_t, NR_ISAC_SRC_COUNT> source_occurrences{};
  std::array<uint64_t, NR_ISAC_SRC_COUNT> dl_source_occurrences{};
  std::array<uint64_t, NR_ISAC_SRC_COUNT> ul_source_occurrences{};
  int64_t first_utc_ns = 0;
  uint32_t available_antennas = 0;
  // Never average distinct illuminators into one complex CFR cell. Each RNTI/session owns an
  // independent view even when UEs share a slot on disjoint PUSCH allocations.
  std::vector<std::complex<float>> cfr; // DL [antenna][subcarrier]
  std::vector<float> weights;
  std::map<uint64_t, UplinkView> uplink;
};

void SensingEngine::PointerQueue::push(Snapshot* value)
{
  { std::lock_guard<std::mutex> lock(mutex_); queue_.push_back(value); }
  condition_.notify_one();
}

bool SensingEngine::PointerQueue::try_pop(Snapshot*& value)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) return false;
  value = queue_.front(); queue_.pop_front(); return true;
}

SensingEngine::Snapshot* SensingEngine::PointerQueue::wait_pop()
{
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [&] { return !queue_.empty(); });
  Snapshot* value = queue_.front(); queue_.pop_front(); return value;
}

void SensingEngine::WindowQueue::push(std::unique_ptr<WindowTask> value)
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_)
      throw std::logic_error("cannot enqueue a CPI after shutdown");
    if (!queue_.empty())
      throw std::logic_error("causal CPI queue already contains a window");
    queue_.push_back(std::move(value));
  }
  condition_.notify_one();
}

std::unique_ptr<SensingEngine::WindowTask> SensingEngine::WindowQueue::wait_pop()
{
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [&] { return closed_ || !queue_.empty(); });
  if (queue_.empty())
    return {};
  auto value = std::move(queue_.front());
  queue_.pop_front();
  return value;
}

void SensingEngine::WindowQueue::close()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }
  condition_.notify_all();
}

void SensingEngine::WindowQueue::reopen()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!queue_.empty())
    throw std::logic_error("cannot reopen a CPI queue before it drains");
  closed_ = false;
}

SensingEngine::SensingEngine(PipelineConfig config, uint32_t maximum_prb,
                             uint32_t requested_antennas)
    : config_(normalize_runtime_config(std::move(config))), maximum_prb_(maximum_prb),
      requested_antennas_(std::clamp(requested_antennas, 1u, 4u)),
      maximum_re_(std::max(1u, maximum_prb_) * 12u * 14u), planner_(config_)
{
  if (config_.num_ues < 1 || config_.num_ues > 4)
    throw std::invalid_argument("num_ues must be in the supported 1..4 range");
  const uint32_t known_sources = (1u << NR_ISAC_SRC_COUNT) - 1u;
  if (!maximum_prb_ || !(config_.sources_mask & known_sources)
      || (config_.sources_mask & ~known_sources))
    throw std::invalid_argument("carrier/source configuration is empty or invalid");
  if (!(std::isfinite(config_.maximum_target_speed_mps)
        && config_.maximum_target_speed_mps > 0.0)
      || !(std::isfinite(config_.maximum_range_m) && config_.maximum_range_m > 0.0))
    throw std::invalid_argument("detector physical surveillance bounds must be positive");
  if (!(std::isfinite(config_.false_object_intensity_per_s)
        && config_.false_object_intensity_per_s > 0.0)
      || !(config_.false_object_intensity_per_s * config_.maximum_dwell_s < 1.0))
    throw std::invalid_argument("false-object intensity must yield a CPI probability in (0,1)");
  const bool has_uplink = (config_.sources_mask & UL_SOURCE_BITS) != 0;
  if (config_.spatial_receivers.configured && requested_antennas_ != 4)
    throw std::invalid_argument("spatial receiver mode requires exactly four RF channels");
  if (config_.pending_row_budget_bytes < 64ULL * 1024ULL * 1024ULL)
    throw std::invalid_argument("pending-row backlog budget must be at least 64 MiB");
  if (config_.aoa_ul_enable && !config_.aoa_enable)
    throw std::invalid_argument("AOA_UL_ENABLE cannot be active when AOA_ENABLE is off");
  if (!(std::isfinite(config_.aoa_quality.maximum_relative_manifold_residual_energy)
        && config_.aoa_quality.maximum_relative_manifold_residual_energy >= 0.0
        && config_.aoa_quality.maximum_relative_manifold_residual_energy <= 1.0)
      || !(std::isfinite(config_.aoa_quality.maximum_phase_fit_residual_rms_rad)
           && config_.aoa_quality.maximum_phase_fit_residual_rms_rad > 0.0)
      || !(std::isfinite(config_.aoa_quality.maximum_azimuth_stddev_deg)
           && config_.aoa_quality.maximum_azimuth_stddev_deg > 0.0)
      || !(std::isfinite(config_.aoa_quality.maximum_elevation_stddev_deg)
           && config_.aoa_quality.maximum_elevation_stddev_deg > 0.0))
    throw std::invalid_argument("AoA quality limits are invalid");
  if (config_.array_calibration.configured) {
    if (requested_antennas_ != 4)
      throw std::invalid_argument("array calibration requires four receive channels");
    std::array<uint8_t, 4> seen{};
    for (size_t physical = 0; physical < 4; ++physical) {
      const uint32_t observed = config_.array_calibration.physical_to_observed[physical];
      if (observed >= 4 || seen[observed]++)
        throw std::invalid_argument("array calibration channel permutation is invalid");
      if (!(std::isfinite(config_.array_calibration.gain[physical])
            && config_.array_calibration.gain[physical] > 0.0)
          || !std::isfinite(config_.array_calibration.phase_rad[physical])
          || !std::isfinite(config_.array_calibration.delay_s[physical]))
        throw std::invalid_argument(
            "array calibration coefficients must be finite with positive gain");
    }
  }
  const bool surveyed_baseline = norm(config_.tx_position - config_.rx_position) > 0.0;
  if (config_.aoa_enable
      && (!config_.array.configured || requested_antennas_ != 4 || !surveyed_baseline))
    throw std::invalid_argument("AoA requires four channels, a rank-two array, and surveyed Tx/Rx");
  if (config_.tracker_enable && config_.hierarchical_tracker_enable
      && !config_.spatial_receivers.configured && !surveyed_baseline)
    throw std::invalid_argument("hierarchical ENU tracking requires surveyed noncoincident Tx/Rx");
  constexpr size_t pool_size = 64;
  pool_.reserve(pool_size);
  for (size_t i = 0; i < pool_size; ++i) {
    auto value = std::make_unique<Snapshot>();
    value->cfr.reserve((size_t)maximum_re_ * requested_antennas_);
    value->subcarrier.reserve(maximum_re_); value->symbol.reserve(maximum_re_);
    free_.push(value.get()); pool_.push_back(std::move(value));
  }
  if (config_.tracker_enable) {
    MotionTrackerConfig inner;
    if (config_.spatial_receivers.configured) {
      std::vector<BistaticGeometry> geometries;
      geometries.reserve(config_.spatial_receivers.positions.size());
      for (const Vec3 receiver : config_.spatial_receivers.positions)
        geometries.push_back({config_.tx_position, receiver});
      MultistaticImmTrackerConfig multistatic;
      multistatic.evidence_mode = config_.evidence_mode;
      multistatic.lifecycle_features = config_.lifecycle_features;
      multistatic.evidence_window_s = config_.evidence_window_s;
      multistatic.existence_threshold_scale = config_.existence_threshold_scale;
      multistatic.maximum_target_speed_mps = config_.maximum_target_speed_mps;
      multistatic.maximum_range_m = config_.maximum_range_m;
      multistatic.false_object_intensity_per_s = config_.false_object_intensity_per_s;
      multistatic.ue_position_known = config_.ue_position_known;
      multistatic.ue_position = {config_.ue_position.x, config_.ue_position.y, config_.ue_position.z};
      multistatic_tracker_ = std::make_unique<MultistaticImmTracker>(
          std::move(geometries), multistatic);
    } else if (config_.hierarchical_tracker_enable
               && norm(config_.tx_position - config_.rx_position) > 0.0) {
      HierarchicalTrackerConfig hierarchical;
      hierarchical.maximum_tangential_speed_mps = config_.maximum_target_speed_mps;
      hierarchical_tracker_ = std::make_unique<HierarchicalEnuTracker>(
          BistaticGeometry{config_.tx_position, config_.rx_position}, hierarchical, inner);
    } else {
      motion_tracker_ = std::make_unique<MotionTracker>(inner);
      if (has_uplink) ul_motion_tracker_ = std::make_unique<MotionTracker>(inner);
    }
  }
  writer_ = std::make_unique<ReportWriter>(config_);
}

SensingEngine::~SensingEngine() { stop(); }

void SensingEngine::start()
{
  std::lock_guard<std::mutex> lock(submission_mutex_);
  if (running_.load(std::memory_order_relaxed)) return;
  const bool require_cuda = cuda_required();
  const bool have_detector_cuda = detector_cuda_available();
  if (require_cuda && !have_detector_cuda)
    throw std::runtime_error("NR_ISAC_REQUIRE_CUDA=1 but no usable CUDA detector is available");
  if (config_.sync_enable) {
#ifdef NR_ISAC_CUDA_ACCELERATION
    if (require_cuda && environment_flag_enabled("NR_ISAC_DISABLE_CUDA_SYNC"))
      throw std::runtime_error("NR_ISAC_REQUIRE_CUDA=1 conflicts with NR_ISAC_DISABLE_CUDA_SYNC=1");
    if (!environment_flag_enabled("NR_ISAC_DISABLE_CUDA_SYNC")) {
      std::string error;
      const bool front_end_ready = warmup_sync_cuda(
          config_.maximum_rows, maximum_prb_ * 12u, &error);
      const bool correction_ready = front_end_ready && warmup_sync_correction_cuda(
          config_.maximum_rows, maximum_prb_ * 12u, requested_antennas_, &error);
      if (!front_end_ready || !correction_ready) {
        if (require_cuda)
          throw std::runtime_error("required CUDA sync warmup failed: " + error);
        std::fprintf(stderr, "SENSING: CUDA sync warmup failed (%s); CPU fallback remains available\n",
                     error.c_str());
      } else {
        std::fprintf(stderr,
                     "SENSING: CUDA sync context, plan, and correction workspace ready before "
                     "CFR admission (capacity_rows=%u, subcarriers=%u, antennas=%u)\n",
                     config_.maximum_rows, maximum_prb_ * 12u, requested_antennas_);
      }
    }
#else
    if (require_cuda)
      throw std::runtime_error("NR_ISAC_REQUIRE_CUDA=1 but CUDA sync support was not built");
#endif
  }
  if (have_detector_cuda) {
    bool warmup_complete = true;
    try {
      detector_cuda_warmup();
    } catch (const std::exception& error) {
      if (require_cuda) throw;
      warmup_complete = false;
      std::fprintf(stderr, "SENSING: CUDA detector warmup failed; CPU fallback remains available: %s\n",
                   error.what());
    }
    if (warmup_complete)
      std::fprintf(stderr, "SENSING: CUDA detector enabled; warm-up complete (%s mode)\n",
                   require_cuda ? "required" : "optional");
  }
  running_.store(true, std::memory_order_release);
  windows_.reopen();
  processing_worker_ = std::thread(&SensingEngine::processing_run, this);
  accumulation_worker_ = std::thread(&SensingEngine::accumulation_run, this);
}

void SensingEngine::stop()
{
  {
    std::lock_guard<std::mutex> lock(submission_mutex_);
    if (!running_.exchange(false)) return;
    // Admission is closed while the FIFO sentinel is inserted, so a producer can never queue a
    // snapshot behind it and leave that snapshot unconsumed during shutdown.
    ready_.push(nullptr);
  }
  if (accumulation_worker_.joinable()) accumulation_worker_.join();
  if (processing_worker_.joinable()) processing_worker_.join();
}

void SensingEngine::submit(uint32_t slot, float fraction, nr_isac_source_t source,
                           const nr_isac_carrier_t& carrier,
                           const std::complex<float>* cfr, uint32_t antennas,
                           const uint32_t* subcarrier, const uint32_t* symbol,
                           uint32_t re, float noise, uint64_t session_id)
{
  std::lock_guard<std::mutex> admission(submission_mutex_);
  if (!running_.load(std::memory_order_relaxed) || !cfr || !subcarrier || !symbol
      || !re || re > maximum_re_ || antennas != requested_antennas_
      || !std::isfinite(fraction) || fraction < 0.0f || fraction >= 1.0f
      || !std::isfinite(noise) || noise < 0.0f) return;
  // Do this while holding submission_mutex_: the callbacks originate from several PHY workers,
  // whereas the old accumulator-only unwrap happens too late to prevent warm-up rows entering
  // the bounded snapshot FIFO.  The stamped value also preserves the absolute clock when the
  // first admitted raw SFN slot is after a 1024-frame wrap.
  const int64_t absolute_slot = unwrap_submission_slot(slot, carrier);
  if (config_.admission_window_enabled
      && (absolute_slot < static_cast<int64_t>(config_.admission_start_slot)
          || absolute_slot >= static_cast<int64_t>(config_.admission_end_slot)))
    return;
  Snapshot* value = nullptr;
#ifdef NR_ISAC_FIXED_WORK_REPLAY
  // Offline saved-input qualification must preserve every observation under host contention.
  // Do not enable this blocking admission policy in a live PHY callback.
  value = free_.wait_pop();
#else
  if (!free_.try_pop(value) || !value) {
    const uint64_t dropped = dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (dropped == 1 || (dropped & (dropped - 1)) == 0)
      std::fprintf(stderr,
                   "SENSING: CFR snapshot pool exhausted; run is incomplete (total=%llu)\n",
                   static_cast<unsigned long long>(dropped));
    return;
  }
#endif
  value->slot = slot; value->absolute_slot = absolute_slot;
  value->fraction = fraction;
  value->source = source; value->carrier = carrier; value->session_id = session_id;
  value->antennas = antennas;
  value->resource_elements = re; value->noise_variance = noise;
  value->utc_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  value->cfr.assign(cfr, cfr + (size_t)value->antennas * re);
  value->subcarrier.assign(subcarrier, subcarrier + re);
  value->symbol.assign(symbol, symbol + re);
  accepted_submissions_.fetch_add(1, std::memory_order_release);
  ready_.push(value);
}

// Single-threaded on the accumulation thread only (the watchdog thread that calls
// request_discard_pending() never touches rows_/active_plan_ directly). A snapshot already sitting
// in ready_ at request time was accepted while the gate was still open, so the discard must wait
// until the accumulation thread has actually consumed it -- discarding immediately would erase rows
// belonging to that still-open interval, not just the stale ones the close is meant to drop.
void SensingEngine::maybe_discard_pending()
{
  if (!discard_requested_.load(std::memory_order_acquire)) return;
  if (consumed_submissions_.load(std::memory_order_relaxed) < discard_after_.load(std::memory_order_relaxed))
    return;
  discard_requested_.store(false, std::memory_order_relaxed);
  discard_pending_rows();
}

void SensingEngine::accumulation_run()
{
  pin_current_thread_from_env();
  maybe_discard_pending();
  for (;;) {
    Snapshot* value = ready_.wait_pop();
    if (!value) break;
    try { consume(*value); }
    catch (const std::exception& e) { std::fprintf(stderr, "SENSING: dropped CFR occurrence: %s\n", e.what()); }
    value->cfr.clear(); value->subcarrier.clear(); value->symbol.clear(); free_.push(value);
    consumed_submissions_.fetch_add(1, std::memory_order_release);
    maybe_discard_pending();
  }
  // Fix round 1 (P20): honour a discard requested right at shutdown before the final flush, or
  // finish_pending_windows() would emit the partial CPI the close was meant to drop.
  maybe_discard_pending();
  try { finish_pending_windows(); }
  catch (const std::exception& e) { std::fprintf(stderr, "SENSING: final window failed: %s\n", e.what()); }
  windows_.close();
}

void SensingEngine::processing_run()
{
  pin_current_thread_from_env();
  while (auto task = windows_.wait_pop()) {
    try {
      process_window(std::move(task->dl_window), std::move(task->ul_windows), task->plan,
                     task->air_origin_slots,
                     task->sequence);
    }
    catch (const std::exception& e) {
      dropped_cpis_.fetch_add(1, std::memory_order_relaxed);
      std::fprintf(stderr, "SENSING: dropped complete CPI #%llu: %s\n",
                   static_cast<unsigned long long>(task->sequence), e.what());
    }
    {
      std::lock_guard<std::mutex> lock(processing_mutex_);
      processing_in_flight_ = false;
    }
    processing_condition_.notify_one();
  }
}

bool SensingEngine::processing_in_flight() const
{
  std::lock_guard<std::mutex> lock(processing_mutex_);
  return processing_in_flight_;
}

void SensingEngine::wait_for_processing()
{
  std::unique_lock<std::mutex> lock(processing_mutex_);
  processing_condition_.wait(lock, [&] { return !processing_in_flight_; });
}

void SensingEngine::finish_pending_windows()
{
  while (!rows_.empty()) {
    wait_for_processing();
    close_ready_windows(true);
  }
  wait_for_processing();
}

int64_t SensingEngine::unwrap_submission_slot(uint32_t raw, const nr_isac_carrier_t& carrier)
{
  const uint32_t slots_per_frame = carrier.slots_per_frame
      ? carrier.slots_per_frame
      : std::max(1u, static_cast<uint32_t>(std::llround(10.0 * carrier.scs_hz / 15000.0)));
  const int64_t cycle = (int64_t)slots_per_frame * 1024;
  if (!have_slot_clock_) {
    have_slot_clock_ = true; latest_raw_slot_ = raw; latest_absolute_slot_ = raw; return raw;
  }
  int64_t delta = static_cast<int64_t>(raw) - static_cast<int64_t>(latest_raw_slot_);
  while (delta > cycle / 2) delta -= cycle;
  while (delta < -cycle / 2) delta += cycle;
  const int64_t result = latest_absolute_slot_ + delta;
  if (result > latest_absolute_slot_) {
    latest_absolute_slot_ = result; latest_raw_slot_ = raw;
  }
  return result;
}

void SensingEngine::begin_geometry(const nr_isac_carrier_t& carrier)
{
  carrier_ = carrier; have_geometry_ = true; rows_.clear(); active_plan_.reset();
  pending_row_bytes_ = 0;
  // Slot unwrapping is deliberately retained across geometry construction.  Admission can start
  // after the raw SFN clock wraps; resetting here would relabel the first admitted row as slot 0.
  air_origin_slots_.reset(); last_closed_slots_.reset();
  dl_clock_tracker_.reset(); ul_clock_tracker_.reset(); planner_.reset();
  for (auto& filter : spatial_clutter_filters_) filter.reset();
  spatial_ul_clutter_filters_.clear();
  spatial_ul_filter_last_used_.clear();
  std::lock_guard<std::mutex> lock(tracker_mutex_);
  if (motion_tracker_) motion_tracker_->reset();
  if (ul_motion_tracker_) ul_motion_tracker_->reset();
  if (hierarchical_tracker_) hierarchical_tracker_->reset();
  if (multistatic_tracker_) multistatic_tracker_->reset();
}

size_t SensingEngine::pending_row_storage_bytes(const PendingRow& row) const
{
  size_t bytes = row.cfr.capacity() * sizeof(std::complex<float>)
                 + row.weights.capacity() * sizeof(float);
  for (const auto& [session, view] : row.uplink) {
    (void)session;
    bytes += view.cfr.capacity() * sizeof(std::complex<float>)
             + view.weights.capacity() * sizeof(float);
  }
  return bytes;
}

void SensingEngine::make_pending_row_room(size_t incoming)
{
  if (pending_row_bytes_ + incoming <= config_.pending_row_budget_bytes)
    return;
  const uint64_t low_watermark = config_.pending_row_budget_bytes * 3 / 4;
  uint64_t discarded = 0;
  double last_discarded = 0.0;
  while (!rows_.empty() && pending_row_bytes_ + incoming > low_watermark) {
    auto oldest = rows_.begin();
    const size_t bytes = pending_row_storage_bytes(oldest->second);
    pending_row_bytes_ = bytes <= pending_row_bytes_ ? pending_row_bytes_ - bytes : 0;
    last_discarded = oldest->second.time_slots;
    rows_.erase(oldest);
    ++discarded;
  }
  if (!discarded)
    throw std::runtime_error("one pending CFR row exceeds the configured backlog budget");
  last_closed_slots_ = std::max(last_closed_slots_.value_or(last_discarded), last_discarded);
  active_plan_.reset();
  const uint64_t rows = discarded_pending_rows_.fetch_add(discarded, std::memory_order_relaxed)
                        + discarded;
  const uint64_t intervals = discarded_pending_intervals_.fetch_add(1, std::memory_order_relaxed) + 1;
  std::fprintf(stderr,
               "SENSING: discarded %llu oldest unplanned rows at the backlog ceiling "
               "(rows=%llu intervals=%llu); run is incomplete\n",
               static_cast<unsigned long long>(discarded),
               static_cast<unsigned long long>(rows),
               static_cast<unsigned long long>(intervals));
}

void SensingEngine::erase_rows(const std::vector<int64_t>& keys)
{
  for (int64_t key : keys) {
    auto found = rows_.find(key);
    if (found == rows_.end())
      continue;
    const size_t bytes = pending_row_storage_bytes(found->second);
    pending_row_bytes_ = bytes <= pending_row_bytes_ ? pending_row_bytes_ - bytes : 0;
    rows_.erase(found);
  }
}

void SensingEngine::consume(const Snapshot& s)
{
  static std::atomic<uint64_t> consume_calls{0};
  const uint64_t call_id = ++consume_calls;
  if (getenv("ISAC_DEBUG_HANG") && call_id % 200 == 0)
    std::fprintf(stderr, "HANG consume() call #%llu ENTER slot=%d\n",
                 (unsigned long long)call_id, s.slot);
  const uint32_t subcarriers = s.carrier.nof_prb * 12u;
  if (!subcarriers || !s.carrier.scs_hz || !s.carrier.dl_center_hz) return;
  if (!have_geometry_) {
    begin_geometry(s.carrier);
  } else if (carrier_.nof_prb != s.carrier.nof_prb
             || carrier_.scs_hz != s.carrier.scs_hz
             || carrier_.dl_center_hz != s.carrier.dl_center_hz
             || carrier_.pci != s.carrier.pci) {
    finish_pending_windows();
    begin_geometry(s.carrier);
  }
  const int64_t absolute_slot = s.absolute_slot;
  const double fraction = std::clamp(static_cast<double>(s.fraction), 0.0,
                                     std::nextafter(1.0, 0.0));
  const int64_t key = row_key(absolute_slot, fraction);
  const double time_slots = absolute_slot + fraction;
  if (last_closed_slots_ && time_slots <= *last_closed_slots_ + 1e-9) {
    stale_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  auto found = rows_.find(key);
  if (found == rows_.end()) {
    const size_t incoming = (size_t)requested_antennas_ * subcarriers
                            * (sizeof(std::complex<float>) + sizeof(float));
    make_pending_row_room(incoming);
    PendingRow row;
    row.time_slots = time_slots; row.raw_slot = s.slot; row.slot_fraction = fraction;
    row.first_utc_ns = s.utc_ns;
    row.cfr.assign((size_t)requested_antennas_ * subcarriers, {});
    row.weights.assign((size_t)requested_antennas_ * subcarriers, 0.0f);
    found = rows_.emplace(key, std::move(row)).first;
    pending_row_bytes_ += pending_row_storage_bytes(found->second);
  }
  PendingRow& row = found->second;
  row.source_mask |= 1u << static_cast<uint32_t>(s.source);
  ++row.source_occurrences[static_cast<uint32_t>(s.source)];
  const bool is_dl = (DL_SOURCE_BITS & (1u << static_cast<uint32_t>(s.source))) != 0;
  if (is_dl) {
    row.dl_source_mask |= 1u << static_cast<uint32_t>(s.source);
    ++row.dl_source_occurrences[static_cast<uint32_t>(s.source)];
  } else {
    if (s.session_id == 0) return;
    auto found_view = row.uplink.find(s.session_id);
    if (found_view == row.uplink.end()) {
      if (row.uplink.size() >= config_.num_ues) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      const size_t view_bytes = (size_t)requested_antennas_ * subcarriers
                                * (sizeof(std::complex<float>) + sizeof(float));
      // Do not evict the row whose reference is live. Backpressure is preferable to a dangling
      // reference and prevents a many-RNTI memory-exhaustion path.
      if (view_bytes > config_.pending_row_budget_bytes - std::min(
              pending_row_bytes_, static_cast<size_t>(config_.pending_row_budget_bytes))) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      PendingRow::UplinkView view;
      view.cfr.assign((size_t)requested_antennas_ * subcarriers, {});
      view.weights.assign((size_t)requested_antennas_ * subcarriers, 0.0f);
      found_view = row.uplink.emplace(s.session_id, std::move(view)).first;
      pending_row_bytes_ += view_bytes;
    }
    found_view->second.source_mask |= 1u << static_cast<uint32_t>(s.source);
    ++found_view->second.source_occurrences[static_cast<uint32_t>(s.source)];
  }
  row.available_antennas = std::max(row.available_antennas, s.antennas);
  const float weight = s.noise_variance > 0.0f ? 1.0f / s.noise_variance : 1.0f;
  for (uint32_t i = 0; i < s.resource_elements; ++i) {
    const uint32_t k = s.subcarrier[i]; if (k >= subcarriers) continue;
    bool finite = true;
    for (uint32_t a = 0; a < s.antennas; ++a) {
      const std::complex<float> value = s.cfr[(size_t)a * s.resource_elements + i];
      finite = finite && std::isfinite(value.real()) && std::isfinite(value.imag());
    }
    if (!finite) continue;
    for (uint32_t a = 0; a < s.antennas; ++a) {
      const size_t index = (size_t)a * subcarriers + k;
      const auto value = s.cfr[(size_t)a * s.resource_elements + i];
      auto accumulate = [&](std::vector<std::complex<float>>& cfr, std::vector<float>& weights) {
        const float old_weight = weights[index], total = old_weight + weight;
        cfr[index] = (cfr[index] * old_weight + value * weight) / total;
        weights[index] = total;
      };
      if (is_dl) accumulate(row.cfr, row.weights);
      else {
        auto& view = row.uplink.at(s.session_id);
        accumulate(view.cfr, view.weights);
      }
    }
  }
  if (getenv("ISAC_DEBUG_HANG") && call_id % 200 == 0)
    std::fprintf(stderr, "HANG consume() call #%llu before close_ready_windows\n",
                 (unsigned long long)call_id);
  close_ready_windows(false);
  if (getenv("ISAC_DEBUG_HANG") && call_id % 200 == 0)
    std::fprintf(stderr, "HANG consume() call #%llu EXIT\n", (unsigned long long)call_id);
}

TrackSnapshot SensingEngine::planning_snapshot(double time) const
{
  std::lock_guard<std::mutex> lock(tracker_mutex_);
  if (hierarchical_tracker_) return hierarchical_tracker_->predict_to(time);
  if (multistatic_tracker_) return multistatic_tracker_->planning_snapshot(time);
  if (motion_tracker_) return motion_tracker_->predict_to(time);
  TrackSnapshot value; value.status = "uninitialized"; value.air_time_s = time; return value;
}

void SensingEngine::enqueue_window(CfrWindow dl_window, std::vector<CfrWindow> ul_windows,
                                   const CpiPlan& plan)
{
  auto task = std::make_unique<WindowTask>();
  task->dl_window = std::move(dl_window);
  task->ul_windows = std::move(ul_windows);
  task->plan = plan;
  task->air_origin_slots = air_origin_slots_.value_or(0.0);
  task->sequence = ++cpi_sequence_;
  {
    std::lock_guard<std::mutex> lock(processing_mutex_);
    if (processing_in_flight_)
      throw std::logic_error("cannot plan a CPI before the prior tracker update");
    processing_in_flight_ = true;
  }
  try { windows_.push(std::move(task)); enqueued_cpis_.fetch_add(1, std::memory_order_relaxed); }
  catch (...) {
    {
      std::lock_guard<std::mutex> lock(processing_mutex_);
      processing_in_flight_ = false;
    }
    processing_condition_.notify_one();
    throw;
  }
}

void SensingEngine::ensure_plan()
{
  if (active_plan_ || rows_.empty()) return;
  if (!air_origin_slots_) air_origin_slots_ = rows_.begin()->second.time_slots;
  const double open = (rows_.begin()->second.time_slots - *air_origin_slots_) * slot_duration_s(carrier_.scs_hz);
  const uint32_t index = std::min<uint32_t>(planner_.current_index(), config_.duration_bank_s.size() - 1);
  const double horizon = config_.duration_bank_s[index];
  const TrackSnapshot snapshot = planning_snapshot(open + 0.5 * horizon);
  std::optional<double> baseline;
  if (norm(config_.tx_position - config_.rx_position) > 0.0)
    baseline = norm(config_.tx_position - config_.rx_position);
  active_plan_ = planner_.plan(snapshot, open, carrier_.dl_center_hz,
                               carrier_.nof_prb * 12u, carrier_.scs_hz,
                               false, baseline); // active Python baseline searches the full envelope
}

void SensingEngine::close_ready_windows(bool flush)
{
  for (;;) {
    if (processing_in_flight()) return;
    ensure_plan(); if (!active_plan_ || rows_.empty()) return;
    const double first = rows_.begin()->second.time_slots;
    const double cutoff = first + active_plan_->target_dwell_s / slot_duration_s(carrier_.scs_hz);
    auto after = config_.spatial_receivers.configured
        ? rows_.lower_bound(static_cast<int64_t>(
              std::ceil(cutoff * ROW_TICKS_PER_SLOT - 1e-6)))
        : rows_.upper_bound(static_cast<int64_t>(
              std::floor(cutoff * ROW_TICKS_PER_SLOT + 1e-6)));
    const bool row_ceiling = rows_.size() >= active_plan_->maximum_rows;
    const bool dwell_ready = after != rows_.end();
    if (!flush && !dwell_ready && !row_ceiling) return;
    if (config_.spatial_receivers.configured && flush && !dwell_ready && !row_ceiling) {
      const uint64_t discarded = rows_.size();
      std::vector<int64_t> incomplete;
      incomplete.reserve(rows_.size());
      for (const auto& item : rows_) incomplete.push_back(item.first);
      erase_rows(incomplete);
      discarded_pending_rows_.fetch_add(discarded, std::memory_order_relaxed);
      discarded_pending_intervals_.fetch_add(1, std::memory_order_relaxed);
      active_plan_.reset();
      return;
    }
    std::vector<int64_t> keys;
    if (dwell_ready) {
      for (auto it = rows_.begin(); it != after && keys.size() < active_plan_->maximum_rows; ++it)
        keys.push_back(it->first);
    } else if (row_ceiling) {
      auto it = rows_.begin();
      for (uint32_t n = 0; n < active_plan_->maximum_rows && it != rows_.end(); ++n, ++it)
        keys.push_back(it->first);
    } else {
      for (const auto& item : rows_) keys.push_back(item.first);
    }
    if (keys.empty()) return;
    auto viable = [&] {
      size_t count = 0;
      double first_time = 0.0, last_time = 0.0;
      for (int64_t key : keys) {
        const PendingRow& row = rows_.at(key);
        const uint32_t mask = row.dl_source_mask;
        if (!mask) continue;
        if (!count) first_time = row.time_slots;
        last_time = row.time_slots;
        ++count;
      }
      return count >= active_plan_->minimum_rows && last_time > first_time;
    };
    struct Span { size_t count = 0; double first = 0.0; double last = 0.0; };
    std::map<uint64_t, Span> session_spans;
    for (int64_t key : keys) {
      const PendingRow& row = rows_.at(key);
      for (const auto& [session, view] : row.uplink) {
        if (!view.source_mask) continue;
        auto& span = session_spans[session];
        if (!span.count) span.first = row.time_slots;
        span.last = row.time_slots;
        ++span.count;
      }
    }
    if (viable()) {
      CfrWindow dl_window = build_window(keys, false);
      std::vector<CfrWindow> ul_windows;
      for (const auto& [session, span] : session_spans)
        if (span.count >= active_plan_->minimum_rows && span.last > span.first)
          ul_windows.push_back(build_window(keys, true, dl_window.antennas, session));
      const CpiPlan plan = *active_plan_;
      const double last_time_slots = rows_.at(keys.back()).time_slots;
      erase_rows(keys);
      last_closed_slots_ = last_time_slots; active_plan_.reset();
      enqueue_window(std::move(dl_window), std::move(ul_windows), plan);
      return; // Python parity: plan the next CPI only after this tracker update finishes.
    } else {
      const double last_time_slots = rows_.at(keys.back()).time_slots;
      erase_rows(keys);
      last_closed_slots_ = last_time_slots;
      active_plan_.reset();
    }
  }
}

void SensingEngine::discard_pending_rows()
{
  std::vector<int64_t> keys;
  keys.reserve(rows_.size());
  for (const auto& item : rows_) keys.push_back(item.first);
  gate_discarded_rows_.fetch_add(keys.size(), std::memory_order_relaxed);
  erase_rows(keys);
  active_plan_.reset();
}

CfrWindow SensingEngine::build_window(const std::vector<int64_t>& keys, bool uplink,
                                      uint32_t forced_antennas,
                                      uint64_t selected_session) const
{
  if (uplink && selected_session == 0) {
    std::map<uint64_t, size_t> session_rows;
    for (int64_t key : keys) {
      const PendingRow& row = rows_.at(key);
      for (const auto& [session, view] : row.uplink)
        if (view.source_mask) ++session_rows[session];
    }
    for (const auto& [session, count] : session_rows)
      if (selected_session == 0 || count > session_rows.at(selected_session))
        selected_session = session;
  }
  std::vector<int64_t> selected_keys;
  selected_keys.reserve(keys.size());
  for (int64_t key : keys) {
    const PendingRow& row = rows_.at(key);
    if (uplink ? (row.uplink.count(selected_session)
                  && row.uplink.at(selected_session).source_mask)
               : row.dl_source_mask)
      selected_keys.push_back(key);
  }
  if (selected_keys.empty())
    throw std::runtime_error(uplink ? "UL measured window has no rows"
                                    : "DL measured window has no rows");
  CfrWindow w;
  w.session_id = uplink ? selected_session : 0;
  w.rows = selected_keys.size(); w.subcarriers = carrier_.nof_prb * 12u;
  if (forced_antennas) {
    w.antennas = forced_antennas;
  } else {
    w.antennas = requested_antennas_;
    for (int64_t key : selected_keys)
      w.antennas = std::min(w.antennas, rows_.at(key).available_antennas);
    w.antennas = std::max(1u, w.antennas);
  }
  w.scs_hz = carrier_.scs_hz;
  w.fc_hz = carrier_.dl_center_hz; w.pci = carrier_.pci;
  w.start_utc_ns = rows_.at(selected_keys.front()).first_utc_ns;
  const size_t cells = (size_t)w.rows * w.subcarriers;
  w.values.assign(cells * w.antennas, {}); w.observed.assign(cells, 0);
  w.row_time_slots.resize(w.rows); w.row_slot_idx.resize(w.rows);
  w.row_slot_frac.resize(w.rows); w.row_source_mask.resize(w.rows);
  for (uint32_t r = 0; r < w.rows; ++r) {
    const PendingRow& row = rows_.at(selected_keys[r]);
    const PendingRow::UplinkView* uplink_view = uplink
        ? &row.uplink.at(selected_session) : nullptr;
    for (uint32_t i = 0; i < NR_ISAC_SRC_COUNT; ++i)
      w.source_occurrences[i] += uplink ? uplink_view->source_occurrences[i]
                                        : row.dl_source_occurrences[i];
    w.row_time_slots[r] = row.time_slots; w.row_slot_idx[r] = row.raw_slot;
    w.row_slot_frac[r] = row.slot_fraction;
    w.row_source_mask[r] = uplink ? uplink_view->source_mask : row.dl_source_mask;
    const auto& row_weights = uplink ? uplink_view->weights : row.weights;
    const auto& row_cfr = uplink ? uplink_view->cfr : row.cfr;
    if (row_weights.size() != (size_t)requested_antennas_ * w.subcarriers
        || row_cfr.size() != (size_t)requested_antennas_ * w.subcarriers)
      throw std::runtime_error(uplink ? "UL CFR view is malformed" : "DL CFR view is malformed");
    for (uint32_t k = 0; k < w.subcarriers; ++k) {
      bool common = true;
      for (uint32_t a = 0; a < w.antennas; ++a)
        common = common && row_weights[(size_t)a * w.subcarriers + k] > 0.0f;
      w.observed[w.cell(r, k)] = common;
      if (common) for (uint32_t a = 0; a < w.antennas; ++a)
        w.values[w.sample(a, r, k)] = row_cfr[(size_t)a * w.subcarriers + k];
    }
  }
  if (!w.valid()) throw std::runtime_error("internal measured-window construction failed");
  return w;
}

void SensingEngine::process_window(CfrWindow dl_window, std::vector<CfrWindow> ul_windows,
                                   const CpiPlan& plan,
                                   double air_origin_slots, uint64_t sequence)
{
  if (getenv("ISAC_DEBUG_HANG"))
    std::fprintf(stderr, "HANG seq=%llu process_window ENTERED (dl_window.rows=%u)\n",
                 (unsigned long long)sequence, dl_window.rows);
  CfrWindow* primary_ul_window = ul_windows.empty() ? nullptr : &ul_windows.front();
  PipelineReport report;
  report.cpi_sequence = sequence;
  report.start_utc_ns = dl_window.start_utc_ns;
  const double slot_ns = slot_duration_s(dl_window.scs_hz) * 1e9;
  report.first_row_time_ns = std::llround(dl_window.row_time_slots.front() * slot_ns);
  report.last_row_time_ns = std::llround(dl_window.row_time_slots.back() * slot_ns);
  report.cpi_duration_ns = std::max<int64_t>(0, report.last_row_time_ns - report.first_row_time_ns);
  report.plan = plan;
  report.dropped_submissions = dropped_.load(std::memory_order_relaxed);
  report.dropped_cpis = dropped_cpis_.load(std::memory_order_relaxed);
  report.discarded_pending_rows = discarded_pending_rows_.load(std::memory_order_relaxed);
  report.discarded_pending_intervals = discarded_pending_intervals_.load(std::memory_order_relaxed);
  report.stale_submissions = stale_.load(std::memory_order_relaxed);
  auto record_sources = [&](const CfrWindow& view) {
    for (uint32_t mask : view.row_source_mask) report.sources_mask |= mask;
    for (uint32_t i = 0; i < NR_ISAC_SRC_COUNT; ++i)
      report.source_occurrences[i] += view.source_occurrences[i];
  };
  record_sources(dl_window);
  for (const CfrWindow& window : ul_windows) record_sources(window);
  for (const CfrWindow& window : ul_windows)
    report.uplink_session_ids.push_back(window.session_id);
  if (primary_ul_window) report.uplink_session_id = primary_ul_window->session_id;
  const double midpoint_slots = 0.5 * (dl_window.row_time_slots.front()
                                       + dl_window.row_time_slots.back());
  report.midpoint_air_time_s = (midpoint_slots - air_origin_slots)
                               * slot_duration_s(dl_window.scs_hz);
  if (config_.spatial_receivers.configured) {
    const auto spatial_processing_started = std::chrono::steady_clock::now();
    if (dl_window.antennas != 4
        || std::any_of(ul_windows.begin(), ul_windows.end(),
                       [](const CfrWindow& window) { return window.antennas != 4; }))
      throw std::runtime_error(
          "spatial receiver CPI requires four RF channels in every present illumination view");
    for (const CfrWindow& window : ul_windows) {
      if (!spatial_ul_clutter_filters_.count(window.session_id)
          && spatial_ul_clutter_filters_.size() >= config_.num_ues) {
        const auto oldest = std::min_element(
            spatial_ul_filter_last_used_.begin(), spatial_ul_filter_last_used_.end(),
            [](const auto& left, const auto& right) { return left.second < right.second; });
        if (oldest != spatial_ul_filter_last_used_.end()) {
          spatial_ul_clutter_filters_.erase(oldest->first);
          spatial_ul_filter_last_used_.erase(oldest);
        }
      }
      spatial_ul_clutter_filters_.try_emplace(window.session_id);
      spatial_ul_filter_last_used_[window.session_id] = sequence;
    }
    std::atomic<uint32_t> active_frontends{0};
    std::atomic<uint32_t> peak_active_frontends{0};
    auto process_receiver = [&](uint32_t receiver, const CfrWindow* uplink_source) {
#ifdef NR_ISAC_FIXED_WORK_REPLAY
      // Offline qualification shares the host with live jobs: serialize only this diagnostic
      // build's front ends. Production concurrency and acquisition timestamps are unchanged.
      static std::mutex replay_frontend_mutex;
      std::lock_guard<std::mutex> replay_frontend_lock(replay_frontend_mutex);
#endif
      const uint32_t active = active_frontends.fetch_add(1, std::memory_order_relaxed) + 1;
      uint32_t peak = peak_active_frontends.load(std::memory_order_relaxed);
      while (peak < active
             && !peak_active_frontends.compare_exchange_weak(
                 peak, active, std::memory_order_relaxed, std::memory_order_relaxed)) {
      }
      struct ActiveGuard {
        std::atomic<uint32_t>& count;
        ~ActiveGuard() { count.fetch_sub(1, std::memory_order_relaxed); }
      } active_guard{active_frontends};
      const bool uplink_only = uplink_source != nullptr;
      SpatialReceiverProduct product;
      auto& spatial = product.report;
      spatial.receiver_id = "rx" + std::to_string(receiver);
      spatial.receiver_position = config_.spatial_receivers.positions[receiver];
      // OFFLINE DIAGNOSTIC: dump every stage of the per-receiver CFR chain (raw -> sync -> pre ->
      // post, plus UL). getenv is cached in a function-local static (magic-static init is
      // thread-safe) so an unset NR_ISAC_DEBUG_DIR costs one pointer check per call, not a
      // getenv() per window per stage.
      auto dump_window = [&](const CfrWindow& w, const char* stage) {
        static const char* const dir = [] {
          const char* d = std::getenv("NR_ISAC_DEBUG_DIR");
          return d ? d : std::getenv("NR_ISAC_CLUTTER_DUMP_DIR");
        }();
        if (!dir) return;
        char path[512];
        std::snprintf(path, sizeof(path), "%s/seq%06llu_rx%u_%s.bin", dir,
                      (unsigned long long)sequence, (unsigned)receiver, stage);
        FILE* f = std::fopen(path, "wb");
        if (!f) return;
        uint32_t hdr[3] = {w.antennas, w.rows, w.subcarriers};
        std::fwrite(hdr, sizeof(uint32_t), 3, f);
        std::fwrite(w.row_time_slots.data(), sizeof(double), w.rows, f);
        std::fwrite(w.observed.data(), 1, w.observed.size(), f);
        std::fwrite(w.values.data(), sizeof(std::complex<float>), w.values.size(), f);
        std::fclose(f);
      };
      if (!uplink_only) {
      CfrWindow corrected = independent_receiver_view(dl_window, receiver);
      dump_window(corrected, "raw");
      spatial.sync.rows = corrected.rows;
      if (config_.sync_enable && corrected.rows >= 3) {
        // Current-CPI direct-path nuisance estimation is independent for each RF chain.  No
        // stored cable/phase correction and no target state enters this operation.
        spatial.sync = estimate_sync(corrected);
        // Excess bistatic range requires the measured direct path at zero even if its
        // fractional clock offset/drift is insignificant. Match the existing UL DTD policy;
        // this is a current-CPI reference, not a stored cable calibration.
        if (spatial.sync.admitted_rows >= 3 && std::isfinite(spatial.sync.los_bins))
          spatial.sync.sto_applied = true;
        // Reference the range axis to the surveyed gNB->receiver baseline (infrastructure
        // metadata), not to the estimated direct-path peak: environment paths inside one
        // resolution cell bias the interpolated LOS peak per receiver.  The LOS estimate is
        // kept and reported; (los_bins - baseline_bins) is the residual clock/estimator offset.
        // NOTE: valid only while the CFR delay origin is absolute (file-bank replay); an OTA
        // clock offset must be estimated on top of this reference.
        double delay_reference_bin = 0.0;
        const double baseline_m = norm(config_.tx_position - spatial.receiver_position);
        if (!config_.dl_reference_measured_los && baseline_m > 0.0 && corrected.subcarriers > 0
            && corrected.scs_hz > 0.0 && std::isfinite(spatial.sync.los_bins)) {
          const double range_res_m = 299792458.0 / (corrected.subcarriers * corrected.scs_hz);
          delay_reference_bin = spatial.sync.los_bins - baseline_m / range_res_m;
        }
        apply_sync_correction(corrected, spatial.sync, delay_reference_bin, std::nullopt);
        // Only reachable inside this `sync_enable && rows >= 3` block, so a "sync" dump exists
        // only for CPIs where sync correction actually ran -- a "raw" file with no matching
        // "sync" sibling is expected, not a bug, on a short/disabled-sync CPI.
        dump_window(corrected, "sync");
      }
      spatial.current_cpi_variance = estimate_current_cpi_variance(
          corrected, &spatial.covariance_family_count,
          &spatial.covariance_difference_count);
      // Align repeated scheduler allocations first, but do not self-subtract their current-CPI
      // mean.  The receiver-local filter subtracts only the preceding causal state and updates it
      // after producing this detector input.
      spatial.detector_alignment = align_allocation_families(corrected, false);
      dump_window(corrected, "pre");
      if (config_.family_static)
        spatial.causal_clutter = spatial_clutter_filters_[receiver].filter(
            corrected, spatial.current_cpi_variance);
      dump_window(corrected, "post");
      // Detector measurements are frozen before any cross-receiver or tracker consumer exists.
#ifdef NR_ISAC_FIXED_WORK_REPLAY
      diagnostic_clean::select(sequence, receiver, false);
#endif
      spatial.detector = detect_clean(
          corrected, config_, RateGate{}, 0, spatial.current_cpi_variance,
          config_.spatial_detector_deadline_s > 0.0
              ? std::optional<double>(config_.spatial_detector_deadline_s)
              : std::nullopt);
      spatial.detector.initial_dl_likelihood = spatial.detector.initial_likelihood;
      spatial.detector.dl_observed_re_count = spatial.detector.axes.observed_re_count;
      for (const CleanComponent& object : spatial.detector.objects) {
        Detection detection;
        detection.range_m = object.range_bin * spatial.detector.axes.range_res_m;
        detection.range_rate_mps = -(object.doppler_bin - corrected.rows / 2.0)
                                   * spatial.detector.axes.rate_res_mps;
        if (std::abs(detection.range_rate_mps) > 2.0 * config_.maximum_target_speed_mps)
          continue;
        detection.score = object.score;
        detection.decision_statistic = object.local.z;
        detection.decision_threshold = object.local_threshold;
        detection.effective_decision_threshold = object.local_threshold;
        detection.split_validated = object.split_validated;
        detection.split_minimum_z = object.split_minimum_z;
        detection.split_threshold = object.split_threshold;
        detection.source_component_iteration = object.iteration;
        detection.bulk_component_iteration = object.bulk_component_iteration;
        detection.object_component_count = object.object_component_count;
        detection.complex_coefficient = object.complex_coefficient;
        detection.component_lineage = object.component_lineage;
        detection.dwell_s = spatial.detector.axes.dwell_s;
        if (object.localization.covariance_valid) {
          detection.covariance_valid = true;
          detection.range_rate_covariance = object.localization.covariance_range_rate;
          cap_reported_covariance(detection.range_rate_covariance, spatial.detector.axes);
        }
        spatial.detections.push_back(std::move(detection));
      }
      // (2026-09-22) NESTED LONG DWELLS (NR_ISAC_MULTI_DWELL=1): the 75 ms CPI stays the clock; the
      // corrected, clutter-cancelled rows of the last 2 and 4 CPIs of this receiver are merged and
      // detected as 150 / 300 ms coherent windows. Measured need: at 75 ms two cars 3.5 m apart (rate
      // difference 1.9 m/s = 1.7 cells) merge inside the Doppler PSF (both resolved in 33% of CPIs, 62% at
      // 300 ms); slow walkers sit inside the +-0.57 m/s notch. The false-object budget is shared across
      // the dwell sets (Bonferroni: intensity / number of dwells). Ring content is receiver-local.
      if (multi_dwell_enabled() && !uplink_only && receiver < dwell_ring_.size()) {
        auto& ring = dwell_ring_[receiver];
        ring.push_back(corrected);
        while (ring.size() > 4) ring.pop_front();
        const uint64_t cpi_index = sequence;
        for (uint32_t n_merge : {2u, 4u}) {
          if (ring.size() < n_merge || (cpi_index % n_merge) != (n_merge - 1)) continue;
          CfrWindow merged = merge_windows(ring, n_merge);
          if (!merged.valid() || merged.rows < 3 || merged.rows > config_.maximum_rows) continue;
          PipelineConfig cfg = config_;
          cfg.false_object_intensity_per_s /= 3.0;   // three dwell sets (75/150/300 ms) share the budget
          DetectorResult ld;
          try {
            ld = detect_clean(merged, cfg, RateGate{}, 0, std::nullopt,
                              config_.spatial_detector_deadline_s > 0.0
                                  ? std::optional<double>(config_.spatial_detector_deadline_s) : std::nullopt);
          } catch (const std::exception& error) {
            std::fprintf(stderr, "SENSING: long dwell %u x CPI failed: %s\n", n_merge, error.what());
            continue;
          }
          SpatialReceiverReport::LongDwell out;
          out.dwell_s = ld.axes.dwell_s; out.rows = merged.rows;
          out.mid_time_slots = 0.5 * (merged.row_time_slots.front() + merged.row_time_slots.back());
          for (const CleanComponent& object : ld.objects) {
            Detection d;
            d.range_m = object.range_bin * ld.axes.range_res_m;
            d.range_rate_mps = -(object.doppler_bin - merged.rows / 2.0) * ld.axes.rate_res_mps;
            if (std::abs(d.range_rate_mps) > 2.0 * config_.maximum_target_speed_mps) continue;
            d.score = object.score; d.decision_statistic = object.local.z; d.decision_threshold = object.local_threshold;
            d.effective_decision_threshold = object.local_threshold; d.split_validated = object.split_validated;
            d.split_minimum_z = object.split_minimum_z; d.split_threshold = object.split_threshold;
            d.source_component_iteration = object.iteration; d.bulk_component_iteration = object.bulk_component_iteration;
            d.object_component_count = object.object_component_count; d.complex_coefficient = object.complex_coefficient;
            d.component_lineage = object.component_lineage; d.dwell_s = ld.axes.dwell_s;
            if (object.localization.covariance_valid) {
              d.covariance_valid = true; d.range_rate_covariance = object.localization.covariance_range_rate;
              cap_reported_covariance(d.range_rate_covariance, ld.axes);
            }
            out.detections.push_back(std::move(d));
          }
          spatial.long_dwells.push_back(std::move(out));
        }
      }
      auto& batch = product.batch;
      batch.receiver_index = receiver;
      batch.geometry = {config_.tx_position, spatial.receiver_position};
      batch.detections = spatial.detections;
      batch.range_resolution_m = spatial.detector.axes.range_res_m;
      batch.rate_resolution_mps = spatial.detector.axes.rate_res_mps;
      batch.measured_dwell_s = spatial.detector.axes.dwell_s;
      batch.valid_re_fraction = static_cast<double>(spatial.detector.axes.observed_re_count)
          / (static_cast<double>(corrected.rows) * corrected.subcarriers);
      uint32_t occupied_columns = 0;
      for (uint32_t k = 0; k < corrected.subcarriers; ++k) {
        bool occupied = false;
        for (uint32_t r = 0; r < corrected.rows; ++r)
          occupied = occupied || corrected.observed[corrected.cell(r, k)];
        occupied_columns += occupied;
      }
      batch.occupied_bandwidth_fraction = static_cast<double>(occupied_columns) / corrected.subcarriers;
      batch.reference_valid = spatial.sync.admitted_rows >= 3
          && std::isfinite(spatial.sync.los_bins);
      auto soft = std::make_shared<SoftRangeRateEvidence>();
      soft->axes = spatial.detector.axes;
      soft->values = spatial.detector.initial_likelihood;
      soft->null_scale = spatial.detector.null_scale;
      soft->psf_range_halfwidth_bins = spatial.detector.psf_range_halfwidth_bins;
      soft->psf_doppler_halfwidth_bins = spatial.detector.psf_doppler_halfwidth_bins;
      soft->effective_hypotheses = spatial.detector.effective_hypotheses;
      soft->search_complete = spatial.detector.stop_reason != "next_cpi_processing_deadline";
      batch.soft_evidence = std::move(soft);
      }

      if (uplink_only) {
        spatial.uplink_present = true;
        spatial.uplink_session_id = uplink_source->session_id;
        try {
          CfrWindow ul_corrected = independent_receiver_view(*uplink_source, receiver);
          dump_window(ul_corrected,
              (std::string("ul") + std::to_string(ul_corrected.session_id) + "_raw").c_str());
          spatial.uplink_sync.rows = ul_corrected.rows;
          if (!config_.sync_enable || ul_corrected.rows < 3)
            throw std::runtime_error(
                "path-relative UL requires a current-PUSCH direct-path reference");
          // Physical lead bound for the earliest-path search (2026-09-21): the UE direct path cannot
          // precede the dominant path by more than the range horizon; without it, real DMRS-comb
          // captures selected the interpolation alias image at N/2 (measured: +-1634 bins).
          const double ul_range_res_m = 299792458.0 / (ul_corrected.subcarriers * ul_corrected.scs_hz);
          const uint32_t ul_max_lead_bins = static_cast<uint32_t>(
              std::ceil(config_.maximum_range_m / ul_range_res_m));
          spatial.uplink_sync = estimate_sync(
              ul_corrected, SyncPathPolicy::earliest_persistent, ul_max_lead_bins);
          const uint32_t minimum_reference_rows = std::max(
              3, static_cast<int>(std::ceil(std::log2(
                     std::max(2u, spatial.uplink_sync.rows)))));
          if (spatial.uplink_sync.admitted_rows < minimum_reference_rows
              || !std::isfinite(spatial.uplink_sync.los_bins)
              || !std::isfinite(spatial.uplink_sync.sto_standard_error_bins))
            throw std::runtime_error(
                "PUSCH direct path is not statistically identifiable in the current CPI");
          // DTD/DFS coordinates require the measured direct path at the origin even when its
          // integer-bin delay happens to win the no-STO BIC model.  This is current-PUSCH
          // referencing, not a stored cable-delay calibration.
          spatial.uplink_sync.sto_applied = true;
          apply_sync_correction(ul_corrected, spatial.uplink_sync, 0.0, std::nullopt);
          spatial.uplink_current_cpi_variance = estimate_current_cpi_variance(
              ul_corrected, &spatial.uplink_covariance_family_count,
              &spatial.uplink_covariance_difference_count);
          spatial.uplink_detector_alignment = align_allocation_families(
              ul_corrected, false);
          if (config_.family_static)
            spatial.uplink_causal_clutter =
                spatial_ul_clutter_filters_.at(ul_corrected.session_id)[receiver].filter(
                ul_corrected, spatial.uplink_current_cpi_variance);
          dump_window(ul_corrected,   // OFFLINE DIAGNOSTIC (same env gate as DL)
              (std::string("ul") + std::to_string(ul_corrected.session_id) + "_post").c_str());
          PipelineConfig uplink_detector_config = config_;
          const double uplink_dwell_s = (ul_corrected.row_time_slots.back()
                                          - ul_corrected.row_time_slots.front())
                                         * slot_duration_s(ul_corrected.scs_hz);
          const double uplink_rate_resolution_mps = C_MPS * (ul_corrected.rows - 1.0)
              / (ul_corrected.fc_hz * ul_corrected.rows * uplink_dwell_s);
          const double uplink_alias_free_rate_mps = static_cast<double>(
              std::max(ul_corrected.rows / 2, ul_corrected.rows - ul_corrected.rows / 2 - 1))
              * uplink_rate_resolution_mps;
          // DFS contains both UE and target motion, so a target-speed bound is not a valid UL
          // search gate. Search the measured aperture's complete alias-free Doppler support.
          uplink_detector_config.maximum_target_speed_mps =
              0.5 * uplink_alias_free_rate_mps
              + std::numeric_limits<double>::epsilon()
                    * std::max(1.0, uplink_alias_free_rate_mps);
#ifdef NR_ISAC_FIXED_WORK_REPLAY
          diagnostic_clean::select(sequence, receiver, true);
#endif
          spatial.uplink_detector = detect_clean(
              ul_corrected, uplink_detector_config, RateGate{}, 0,
              spatial.uplink_current_cpi_variance,
              config_.spatial_detector_deadline_s > 0.0
                  ? std::optional<double>(config_.spatial_detector_deadline_s)
                  : std::nullopt);
          spatial.uplink_search_complete =
              spatial.uplink_detector.stop_reason != "next_cpi_processing_deadline";
          for (const CleanComponent& object : spatial.uplink_detector.objects) {
            // The measured direct path and its measured PSF support are a nuisance reference, not
            // a UE-target-RX reflection.  The guard is derived from this CPI's PSF.
            // (2026-09-19 fix) The direct-path nuisance occupies its PSF in BOTH range and
            // Doppler (a static UE: zero DFS).  Discarding on range alone threw away every UL
            // object within one range main lobe of the UE->rx path -- 12 m with the taper --
            // i.e. the whole person track (UL excess range 0.2-8.7 m, recall 0%).  An object
            // Doppler-separated from the direct path by more than its PSF is not the direct path.
            // The guard is centred on the MEASURED direct-path Doppler and applies only when
            // that Doppler is measured (moving UE).  For a static UE the direct path is at zero
            // DFS and stage 3 has already removed it; guarding a +-4-bin (4.6 m/s) Doppler zone
            // around zero would still discard every slow walker near the UE (person: 0% UL
            // recall with the 2-D guard around zero).
            if (spatial.uplink_direct_path_rate_valid) {
              const double direct_doppler_bin = ul_corrected.rows / 2.0
                  - spatial.uplink_direct_path_range_rate_mps
                        / spatial.uplink_detector.axes.rate_res_mps;
              const double doppler_offset = std::abs(object.doppler_bin - direct_doppler_bin);
              if (object.range_bin
                      <= static_cast<double>(spatial.uplink_detector.psf_range_halfwidth_bins)
                  && doppler_offset
                      <= static_cast<double>(spatial.uplink_detector.psf_doppler_halfwidth_bins))
                continue;
            }
            Detection detection;
            detection.range_m = object.range_bin
                                * spatial.uplink_detector.axes.range_res_m;
            detection.range_rate_mps = -(object.doppler_bin - ul_corrected.rows / 2.0)
                                       * spatial.uplink_detector.axes.rate_res_mps;
            detection.score = object.score;
            detection.decision_statistic = object.local.z;
            detection.decision_threshold = object.local_threshold;
            detection.effective_decision_threshold = object.local_threshold;
            detection.split_validated = object.split_validated;
            detection.split_minimum_z = object.split_minimum_z;
            detection.split_threshold = object.split_threshold;
            detection.source_component_iteration = object.iteration;
            detection.bulk_component_iteration = object.bulk_component_iteration;
            detection.object_component_count = object.object_component_count;
            detection.complex_coefficient = object.complex_coefficient;
            detection.component_lineage = object.component_lineage;
            detection.dwell_s = spatial.uplink_detector.axes.dwell_s;
            if (object.localization.covariance_valid) {
              detection.covariance_valid = true;
              detection.range_rate_covariance =
                  object.localization.covariance_range_rate;
              const double reference_range_sigma =
                  spatial.uplink_sync.sto_standard_error_bins
                  * spatial.uplink_detector.axes.range_res_m;
              detection.range_rate_covariance(0, 0) +=
                  reference_range_sigma * reference_range_sigma;
              if (std::isfinite(spatial.uplink_sync.cfo_resolution_hz)) {
                const double reference_rate_sigma = C_MPS
                    * spatial.uplink_sync.cfo_resolution_hz / ul_corrected.fc_hz;
                detection.range_rate_covariance(1, 1) +=
                    reference_rate_sigma * reference_rate_sigma;
              }
              detection.range_rate_covariance = positive_semidefinite(
                  symmetrized(detection.range_rate_covariance), 1e-12);
              cap_reported_covariance(detection.range_rate_covariance, spatial.uplink_detector.axes);
            }
            spatial.uplink_detections.push_back(std::move(detection));
          }
          spatial.uplink_valid = true;
          // (2026-09-22) UL ACCUMULATION GATE (NR_ISAC_UL_DWELL=1). Root cause it addresses: with
          // 14-31 PUSCH rows per 75 ms CPI the UE direct path leaks into every Doppler bin at
          // range bins 0-3 (measured pedestal 28 dB at 14 rows, 10 dB at 29), so a target passing
          // 4-8 m from the UE -- 3-7 m of differential range -- never clears its own range column
          // and every UL component is classified as a skirt (measured: 0 UL objects in 1272
          // receiver-CPIs). The gate concatenates the corrected, clutter-cancelled UL windows of
          // consecutive CPIs of the SAME session and receiver until either the declared row target
          // or the declared maximum dwell is reached, whichever comes first, and detects once on
          // the accumulated window. The 75 ms UL detection above is unchanged; the accumulated
          // result is reported separately with its own dwell and midpoint so stage 8 can time-align
          // it. The false-object budget is shared between the two dwell sets (Bonferroni /2).
          if (ul_dwell_enabled()) {
            auto& ring = ul_dwell_ring_[{ul_corrected.session_id, receiver}];
            ring.push_back(ul_corrected);
            const double slot_s = slot_duration_s(ul_corrected.scs_hz);
            auto span_s = [&] {
              return (ring.back().row_time_slots.back() - ring.front().row_time_slots.front()) * slot_s;
            };
            auto total_rows = [&] {
              uint32_t n = 0; for (const auto& w : ring) n += w.rows; return n;
            };
            // Drop from the front anything older than the maximum dwell: the window never covers
            // more time than declared, whatever the grant pattern does.
            while (ring.size() > 1 && span_s() > ul_dwell_max_s()) ring.pop_front();
            if (ring.size() > 1
                && (total_rows() >= ul_dwell_target_rows() || span_s() >= ul_dwell_max_s())) {
              CfrWindow merged = merge_windows(ring, static_cast<uint32_t>(ring.size()));
              if (merged.valid() && merged.rows >= 3 && merged.rows <= config_.maximum_rows) {
                PipelineConfig cfg = uplink_detector_config;
                cfg.false_object_intensity_per_s /= 2.0;   // two UL dwell sets share the budget
                const double merged_dwell_s = (merged.row_time_slots.back()
                                               - merged.row_time_slots.front()) * slot_s;
                const double merged_rate_res = C_MPS * (merged.rows - 1.0)
                    / (merged.fc_hz * merged.rows * merged_dwell_s);
                const double merged_alias_free = static_cast<double>(
                    std::max(merged.rows / 2, merged.rows - merged.rows / 2 - 1)) * merged_rate_res;
                cfg.maximum_target_speed_mps = 0.5 * merged_alias_free
                    + std::numeric_limits<double>::epsilon() * std::max(1.0, merged_alias_free);
                try {
                  const DetectorResult ld = detect_clean(
                      merged, cfg, RateGate{}, 0, spatial.uplink_current_cpi_variance,
                      config_.spatial_detector_deadline_s > 0.0
                          ? std::optional<double>(config_.spatial_detector_deadline_s) : std::nullopt);
                  SpatialReceiverReport::LongDwell out;
                  out.dwell_s = ld.axes.dwell_s; out.rows = merged.rows;
                  out.mid_time_slots = 0.5 * (merged.row_time_slots.front()
                                              + merged.row_time_slots.back());
                  for (const CleanComponent& object : ld.objects) {
                    Detection d;
                    d.range_m = object.range_bin * ld.axes.range_res_m;
                    d.range_rate_mps = -(object.doppler_bin - merged.rows / 2.0) * ld.axes.rate_res_mps;
                    d.score = object.score;
                    d.decision_statistic = object.local.z;
                    d.decision_threshold = object.local_threshold;
                    d.effective_decision_threshold = object.local_threshold;
                    d.source_component_iteration = object.iteration;
                    d.bulk_component_iteration = object.bulk_component_iteration;
                    d.object_component_count = object.object_component_count;
                    d.dwell_s = ld.axes.dwell_s;
                    out.detections.push_back(std::move(d));
                  }
                  spatial.uplink_long_dwells.push_back(std::move(out));
                } catch (const std::exception& error) {
                  std::fprintf(stderr, "SENSING: UL accumulated dwell failed: %s\n", error.what());
                }
              }
              ring.clear();   // non-overlapping accumulation windows
            }
            while (ring.size() > 8) ring.pop_front();
          }
          UlDifferentialReceiverBatch ul_batch;
          ul_batch.session_id = ul_corrected.session_id;
          uint64_t support = 1469598103934665603ULL;
          for (uint32_t r = 0; r < ul_corrected.rows; ++r) {
            support = (support ^ ul_corrected.row_slot_idx[r]) * 1099511628211ULL;
            support = (support ^ static_cast<uint64_t>(std::llround(
                ul_corrected.row_slot_frac[r] * ROW_TICKS_PER_SLOT))) * 1099511628211ULL;
            for (uint32_t k = 0; k < ul_corrected.subcarriers; ++k)
              support = (support ^ ul_corrected.observed[ul_corrected.cell(r, k)]) * 1099511628211ULL;
          }
          ul_batch.allocation_support_id = support ? support : 1;
          ul_batch.receiver_index = receiver;
          ul_batch.receiver_position = spatial.receiver_position;
          ul_batch.detections = spatial.uplink_detections;
          ul_batch.range_resolution_m = spatial.uplink_detector.axes.range_res_m;
          ul_batch.rate_resolution_mps = spatial.uplink_detector.axes.rate_res_mps;
          ul_batch.maximum_differential_range_m = config_.maximum_range_m;
          for (double rate : spatial.uplink_detector.axes.rate_axis_mps)
            ul_batch.maximum_abs_differential_rate_mps = std::max(
                ul_batch.maximum_abs_differential_rate_mps, std::abs(rate));
          const double ul_midpoint_slots = 0.5 * (
              ul_corrected.row_time_slots.front() + ul_corrected.row_time_slots.back());
          ul_batch.target_time_offset_s = (ul_midpoint_slots - midpoint_slots)
                                          * slot_duration_s(ul_corrected.scs_hz);
          if (spatial.uplink_sync.cfo_applied
              && std::isfinite(spatial.uplink_sync.cfo_hz)
              && std::isfinite(spatial.uplink_sync.cfo_resolution_hz)
              && spatial.uplink_sync.cfo_resolution_hz > 0.0
              && ul_corrected.fc_hz > 0.0) {
            const double rate_resolution = C_MPS
                * spatial.uplink_sync.cfo_resolution_hz / ul_corrected.fc_hz;
            ul_batch.direct_path_rate_valid = true;
            ul_batch.direct_path_range_rate_mps = -C_MPS
                * spatial.uplink_sync.cfo_hz / ul_corrected.fc_hz;
            ul_batch.direct_path_rate_variance_mps2 =
                rate_resolution * rate_resolution / 12.0;
          }
          spatial.uplink_direct_path_rate_valid = ul_batch.direct_path_rate_valid;
          spatial.uplink_direct_path_range_rate_mps =
              ul_batch.direct_path_range_rate_mps;
          spatial.uplink_direct_path_rate_variance_mps2 =
              ul_batch.direct_path_rate_variance_mps2;
          // The passive receiver's configured/swept FFT advance is not the serving gNB's per-UE
          // Timing Advance. serving_range_valid therefore remains false unless genuine protocol
          // telemetry is supplied by an integration boundary that owns that measurement.
          ul_batch.same_pusch_reference = true;
          ul_batch.observable = true;
          ul_batch.search_complete = spatial.uplink_search_complete;
          product.ul_batch = std::move(ul_batch);
        } catch (const std::exception& error) {
          spatial.uplink_error = error.what();
        }
      }
      return product;
    };

    std::array<SpatialReceiverProduct, 4> products;
    std::vector<std::array<SpatialReceiverProduct, 4>> ul_session_products(
        ul_windows.size());
    const size_t configured_lanes = 4 * (1 + static_cast<size_t>(config_.num_ues));
    if (!spatial_detector_executor_)
      spatial_detector_executor_ = std::make_unique<SpatialDetectorExecutor>(configured_lanes);
    if (spatial_detector_executor_->size() != configured_lanes)
      throw std::logic_error("spatial detector executor/configuration size changed at runtime");

    const auto detector_frontends_started = std::chrono::steady_clock::now();
    if (getenv("ISAC_DEBUG_HANG"))
      std::fprintf(stderr, "HANG seq=%llu submitting DL lanes\n", (unsigned long long)sequence);
    std::array<std::future<SpatialReceiverProduct>, 4> dl_futures;
    for (uint32_t receiver = 0; receiver < dl_futures.size(); ++receiver)
      dl_futures[receiver] = spatial_detector_executor_->submit(
          receiver, [&, receiver] { return process_receiver(receiver, nullptr); });
    std::vector<std::array<std::future<SpatialReceiverProduct>, 4>> ul_futures(
        ul_windows.size());
    for (size_t session = 0; session < ul_windows.size(); ++session) {
      const CfrWindow* const source = &ul_windows[session];
      for (uint32_t receiver = 0; receiver < ul_futures[session].size(); ++receiver) {
        const size_t lane = 4 * (session + 1) + receiver;
        ul_futures[session][receiver] = spatial_detector_executor_->submit(
            lane, [&, receiver, source] { return process_receiver(receiver, source); });
      }
    }
    if (getenv("ISAC_DEBUG_HANG"))
      std::fprintf(stderr, "HANG seq=%llu all lanes submitted, awaiting DL futures\n",
                   (unsigned long long)sequence);
    std::exception_ptr failure;
    for (uint32_t receiver = 0; receiver < dl_futures.size(); ++receiver) {
      if (getenv("ISAC_DEBUG_HANG"))
        std::fprintf(stderr, "HANG seq=%llu awaiting dl_futures[%u]\n",
                     (unsigned long long)sequence, receiver);
      try {
        products[receiver] = dl_futures[receiver].get();
      } catch (...) {
        if (!failure) failure = std::current_exception();
      }
      if (getenv("ISAC_DEBUG_HANG"))
        std::fprintf(stderr, "HANG seq=%llu got dl_futures[%u]\n",
                     (unsigned long long)sequence, receiver);
    }
    for (size_t session = 0; session < ul_futures.size(); ++session) {
      for (uint32_t receiver = 0; receiver < ul_futures[session].size(); ++receiver) {
        try {
          ul_session_products[session][receiver] = ul_futures[session][receiver].get();
        } catch (...) {
          if (!failure) failure = std::current_exception();
        }
      }
    }
    report.spatial_frontend_jobs = static_cast<uint32_t>(
        products.size() * (1 + ul_windows.size()));
    report.spatial_frontend_worker_count = static_cast<uint32_t>(
        spatial_detector_executor_->size());
    report.spatial_frontend_peak_concurrency = peak_active_frontends.load(
        std::memory_order_relaxed);
    report.spatial_frontend_wall_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - detector_frontends_started).count();
    if (failure) std::rethrow_exception(failure);

    std::vector<UlDifferentialReceiverBatch> ul_batches;
    ul_batches.reserve(ul_windows.size() * products.size());
    for (auto& session : ul_session_products)
      for (auto& product : session)
        if (product.ul_batch) ul_batches.push_back(std::move(*product.ul_batch));

    if (!ul_session_products.empty()) {
      for (uint32_t receiver = 0; receiver < products.size(); ++receiver) {
        auto& target = products[receiver];
        auto& source = ul_session_products.front()[receiver];
        target.report.uplink_present = source.report.uplink_present;
        target.report.uplink_session_id = source.report.uplink_session_id;
        target.report.uplink_valid = source.report.uplink_valid;
        target.report.uplink_search_complete = source.report.uplink_search_complete;
        target.report.uplink_error = source.report.uplink_error;
        target.report.uplink_sync = source.report.uplink_sync;
        target.report.uplink_covariance_family_count =
            source.report.uplink_covariance_family_count;
        target.report.uplink_covariance_difference_count =
            source.report.uplink_covariance_difference_count;
        target.report.uplink_current_cpi_variance =
            source.report.uplink_current_cpi_variance;
        target.report.uplink_detector_alignment = source.report.uplink_detector_alignment;
        target.report.uplink_causal_clutter = source.report.uplink_causal_clutter;
        target.report.uplink_detector = source.report.uplink_detector;
        target.report.uplink_detections = source.report.uplink_detections;
      }
    }

    report.uplink_session_receivers.reserve(ul_session_products.size());
    for (auto& session : ul_session_products) {
      std::vector<SpatialReceiverReport> receiver_reports;
      receiver_reports.reserve(session.size());
      for (auto& product : session)
        receiver_reports.push_back(std::move(product.report));
      report.uplink_session_receivers.push_back(std::move(receiver_reports));
    }

    std::vector<ReceiverDetectionBatch> batches;
    batches.reserve(products.size());
    report.spatial_receivers.reserve(products.size());
    for (auto& product : products) {
      batches.push_back(std::move(product.batch));
      report.spatial_receivers.push_back(std::move(product.report));
    }
    // Retain receiver zero in legacy scalar fields while the authoritative four receiver products
    // are serialized below.  No combining or cross-receiver admission occurs here.
    const auto& primary = report.spatial_receivers.front();
    report.sync = primary.sync;
    report.current_cpi_variance = primary.current_cpi_variance;
    report.covariance_family_count = primary.covariance_family_count;
    report.covariance_difference_count = primary.covariance_difference_count;
    report.detector_alignment = primary.detector_alignment;
    report.detector = primary.detector;
    report.detections = primary.detections;
    report.uplink_present = primary.uplink_present;
    report.uplink_session_id = primary.uplink_session_id;
    report.uplink_valid = primary.uplink_valid;
    report.uplink_error = primary.uplink_error;
    report.uplink_sync = primary.uplink_sync;
    report.uplink_covariance_family_count = primary.uplink_covariance_family_count;
    report.uplink_covariance_difference_count = primary.uplink_covariance_difference_count;
    report.uplink_current_cpi_variance = primary.uplink_current_cpi_variance;
    report.uplink_detector_alignment = primary.uplink_detector_alignment;
    report.uplink_detector = primary.uplink_detector;
    report.uplink_detections = primary.uplink_detections;
#ifdef NR_ISAC_FIXED_WORK_REPLAY
    ul_probe::capture(report.midpoint_air_time_s, report.cpi_sequence, config_, batches, ul_batches);
#endif
    if (multistatic_tracker_) {
      if (getenv("ISAC_DEBUG_HANG"))
        std::fprintf(stderr, "HANG seq=%llu awaiting tracker_mutex_\n",
                     (unsigned long long)sequence);
      std::lock_guard<std::mutex> tracker_lock(tracker_mutex_);
      if (getenv("ISAC_DEBUG_HANG"))
        std::fprintf(stderr, "HANG seq=%llu got tracker_mutex_, calling update()\n",
                     (unsigned long long)sequence);
      const double spatial_elapsed_s = std::chrono::duration<double>(
          std::chrono::steady_clock::now() - spatial_processing_started).count();
      const double tracker_budget_s = std::max(
          0.0, SPATIAL_CPI_DURATION_S - spatial_elapsed_s);
      multistatic_tracker_->update(
          report.midpoint_air_time_s, batches, report.cpi_sequence, tracker_budget_s,
          ul_batches);
      if (getenv("ISAC_DEBUG_HANG"))
        std::fprintf(stderr, "HANG seq=%llu update() returned\n", (unsigned long long)sequence);
      // Tentative hypotheses remain internal until their causal false-intensity evidence crosses
      // the adaptive confirmation threshold.
      report.tracks = multistatic_tracker_->reportable_snapshots();
      report.multistatic_tracker_processing =
          multistatic_tracker_->last_processing_stats();
    }
    if (getenv("ISAC_DEBUG_HANG"))
      std::fprintf(stderr, "HANG seq=%llu emitting report\n", (unsigned long long)sequence);
    writer_->emit(report);
    if (getenv("ISAC_DEBUG_HANG"))
      std::fprintf(stderr, "HANG seq=%llu report emitted, returning\n", (unsigned long long)sequence);
    return;
  }

  // Legacy co-located array operation remains separate from spatial receiver mode.
  apply_array_calibration(dl_window, config_.array_calibration);
  if (primary_ul_window)
    apply_array_calibration(*primary_ul_window, config_.array_calibration);
  CfrWindow dl_corrected = dl_window;
  report.sync.rows = dl_window.rows;
  if (config_.sync_enable && dl_window.rows >= 3) {
    report.sync = estimate_sync(dl_window);
    report.sync = dl_clock_tracker_.update(report.sync, report.midpoint_air_time_s,
                                           dl_window.subcarriers, dl_window.scs_hz);
    std::optional<std::array<std::complex<double>, 4>> los;
    if (dl_window.antennas == 4 && config_.array.configured
        && norm(config_.tx_position - config_.rx_position) > 0.0)
      los = surveyed_los_steering(
          config_.array, config_.tx_position, config_.rx_position, dl_window.fc_hz);
    apply_sync_correction(dl_corrected, report.sync, 0.0, los);
  }
  report.current_cpi_variance = estimate_current_cpi_variance(
      dl_corrected, &report.covariance_family_count, &report.covariance_difference_count);
  CfrWindow dl_detector_input = dl_corrected;
  report.detector_alignment = align_allocation_families(
      dl_detector_input, config_.family_static);
  const double dl_dwell = (dl_detector_input.row_time_slots.back()
                           - dl_detector_input.row_time_slots.front())
                          * slot_duration_s(dl_detector_input.scs_hz);
  const RateGate dl_gate = finalize_search_gate(
      plan, dl_detector_input.rows, dl_dwell, dl_detector_input.fc_hz, config_);
  report.detector = detect_clean(
      dl_detector_input, config_, dl_gate, 0, report.current_cpi_variance);
  // Backward-compatible capture fields now both name the same provenance-clean DL detector map.
  report.detector.initial_dl_likelihood = report.detector.initial_likelihood;
  report.detector.dl_observed_re_count = report.detector.axes.observed_re_count;

  auto accept = [&](const DetectorResult& detector, const CfrWindow& input) {
    std::vector<Detection> accepted;
    for (const CleanComponent& object : detector.objects) {
      Detection d;
      d.range_m = object.range_bin * detector.axes.range_res_m;
      d.range_rate_mps = -(object.doppler_bin - input.rows / 2.0)
                         * detector.axes.rate_res_mps;
      if (std::abs(d.range_rate_mps) > 2.0 * config_.maximum_target_speed_mps) continue;
      d.score = object.score;
      d.decision_statistic = object.local.z;
      d.decision_threshold = object.local_threshold;
      d.effective_decision_threshold = object.local_threshold;
      d.source_component_iteration = object.iteration;
      d.bulk_component_iteration = object.bulk_component_iteration;
      d.object_component_count = object.object_component_count;
      d.complex_coefficient = object.complex_coefficient;
      d.component_lineage = object.component_lineage;
      d.dwell_s = detector.axes.dwell_s;
      if (object.localization.covariance_valid) {
        d.covariance_valid = true;
        d.range_rate_covariance = object.localization.covariance_range_rate;
        cap_reported_covariance(d.range_rate_covariance, detector.axes);
      }
      if (d.decision_statistic > d.effective_decision_threshold)
        accepted.push_back(std::move(d));
    }
    return accepted;
  };

  report.detections = accept(report.detector, dl_detector_input);
  attach_aoa(dl_corrected, report.detector.components, report.detector.axes,
             config_, report.detections);

  std::optional<CfrWindow> ul_corrected;
  if (primary_ul_window) {
    report.uplink_present = true;
    try {
      ul_corrected = *primary_ul_window;
      report.uplink_sync.rows = primary_ul_window->rows;
      PipelineConfig ul_config = config_;
      ul_config.aoa_enable = config_.aoa_ul_enable;
      if (config_.sync_enable && primary_ul_window->rows >= 3) {
        report.uplink_sync = estimate_sync(*primary_ul_window);
        report.uplink_sync = ul_clock_tracker_.update(
            report.uplink_sync, report.midpoint_air_time_s,
            primary_ul_window->subcarriers, primary_ul_window->scs_hz);
        apply_sync_correction(*ul_corrected, report.uplink_sync, 0.0, std::nullopt);
      }
      report.uplink_current_cpi_variance = estimate_current_cpi_variance(
          *ul_corrected, &report.uplink_covariance_family_count,
          &report.uplink_covariance_difference_count);
      CfrWindow ul_detector_input = *ul_corrected;
      report.uplink_detector_alignment = align_allocation_families(
          ul_detector_input, config_.family_static);
      const double ul_dwell = (ul_detector_input.row_time_slots.back()
                               - ul_detector_input.row_time_slots.front())
                              * slot_duration_s(ul_detector_input.scs_hz);
      const RateGate ul_gate = finalize_search_gate(
          plan, ul_detector_input.rows, ul_dwell, ul_detector_input.fc_hz, config_);
      report.uplink_detector = detect_clean(
          ul_detector_input, config_, ul_gate, 0, report.uplink_current_cpi_variance);
      report.uplink_detections = accept(report.uplink_detector, ul_detector_input);
      attach_aoa(*ul_corrected, report.uplink_detector.components,
                 report.uplink_detector.axes, ul_config, report.uplink_detections, false);
      report.uplink_valid = true;
    } catch (const std::exception& error) {
      report.uplink_error = error.what();
    }
  }
  {
    std::lock_guard<std::mutex> tracker_lock(tracker_mutex_);
    if (hierarchical_tracker_) {
      if (report.uplink_valid) {
        hierarchical_tracker_->update_auxiliary(
            report.midpoint_air_time_s, report.uplink_detections,
            report.uplink_detector.axes.range_res_m,
            report.uplink_detector.axes.rate_res_mps, report.cpi_sequence);
        report.uplink_tracks = hierarchical_tracker_->auxiliary_snapshots();
      }
      CrossLegFusionResult fusion;
      if (report.uplink_valid)
        fusion = confirm_and_fuse_dl_with_ul(
            report.detections, report.uplink_detections, config_.aoa_ul_enable);
      else
        fusion.dl_measurements = report.detections;
      report.detections = std::move(fusion.dl_measurements);
      hierarchical_tracker_->update(
          report.midpoint_air_time_s, report.detections,
          report.detector.axes.range_res_m, report.detector.axes.rate_res_mps,
          report.cpi_sequence, report.detector.axes.dwell_s,
          fusion.diagnostics.ul_motion_active, fusion.auxiliary_ul_aoa,
          report.uplink_valid,
          report.uplink_valid && config_.aoa_ul_enable);
      report.tracks = hierarchical_tracker_->snapshots();
    } else if (motion_tracker_) {
      motion_tracker_->update(report.midpoint_air_time_s, report.detections,
          report.detector.axes.range_res_m, report.detector.axes.rate_res_mps, report.cpi_sequence);
      if (report.uplink_valid && ul_motion_tracker_) {
        ul_motion_tracker_->update(
            report.midpoint_air_time_s, report.uplink_detections,
            report.uplink_detector.axes.range_res_m,
            report.uplink_detector.axes.rate_res_mps, report.cpi_sequence);
        report.uplink_tracks = ul_motion_tracker_->snapshots();
      }
      report.tracks = motion_tracker_->snapshots();
    }
  }
  writer_->emit(report);
}

} // namespace nr_isac
