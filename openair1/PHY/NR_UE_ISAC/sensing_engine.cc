/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "sensing_engine.h"

#include "detector_cuda.h"

#include "aoa.h"
#include "cuda_support.h"
#include "detector.h"
#include "sync_correction.h"
#ifdef NR_ISAC_CUDA_ACCELERATION
#include "sync_correction_cuda.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace nr_isac {

namespace {
constexpr int64_t ROW_TICKS_PER_SLOT = 1000000000LL;

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
} // namespace

struct SensingEngine::Snapshot {
  uint32_t slot = 0;
  int64_t absolute_slot = 0;
  float fraction = 0.0f;
  nr_isac_source_t source = NR_ISAC_SRC_CSI_RS;
  nr_isac_carrier_t carrier{};
  uint32_t antennas = 1;
  uint32_t resource_elements = 0;
  float noise_variance = 0.0f;
  int64_t utc_ns = 0;
  std::vector<std::complex<float>> cfr;
  std::vector<uint32_t> subcarrier;
  std::vector<uint32_t> symbol;
};

struct SensingEngine::PendingRow {
  double time_slots = 0.0;
  uint32_t raw_slot = 0;
  double slot_fraction = 0.0;
  uint32_t source_mask = 0;
  uint32_t dl_source_mask = 0;
  uint32_t ul_source_mask = 0;
  std::array<uint64_t, NR_ISAC_SRC_COUNT> source_occurrences{};
  std::array<uint64_t, NR_ISAC_SRC_COUNT> dl_source_occurrences{};
  std::array<uint64_t, NR_ISAC_SRC_COUNT> ul_source_occurrences{};
  int64_t first_utc_ns = 0;
  uint32_t available_antennas = 0;
  // Never average distinct illuminators into one complex CFR cell. cfr/weights is the DL view;
  // ul_cfr/ul_weights is the single-UE UL view. Their bistatic phase histories and geometries are
  // different even when their submissions have the same slot/fraction/resource element.
  std::vector<std::complex<float>> cfr; // DL [antenna][subcarrier]
  std::vector<float> weights;
  std::vector<std::complex<float>> ul_cfr;
  std::vector<float> ul_weights;
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
    : config_(std::move(config)), maximum_prb_(maximum_prb),
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
      || !(std::isfinite(config_.maximum_range_m) && config_.maximum_range_m > 0.0)
      || !config_.maximum_components || !config_.maximum_objects)
    throw std::invalid_argument("detector physical bounds and counts must be positive");
  if (!std::isfinite(config_.maximum_path_delay_m) || config_.maximum_path_delay_m < 0.0
      || !std::isfinite(config_.maximum_path_doppler_hz) || config_.maximum_path_doppler_hz < 0.0
      || !std::isfinite(config_.leading_significance_db) || config_.leading_significance_db > 0.0)
    throw std::invalid_argument("invalid measured-lattice multipath-collapse configuration");
  if (config_.adaptive_guard_range_bins > config_.adaptive_training_range_bins
      || config_.adaptive_guard_doppler_bins > config_.adaptive_training_doppler_bins
      || !(std::isfinite(config_.false_object_intensity_per_s)
           && config_.false_object_intensity_per_s > 0.0))
    throw std::invalid_argument("invalid CUT-excluded adaptive-threshold configuration");
  const bool has_uplink = (config_.sources_mask & UL_SOURCE_BITS) != 0;
  if (has_uplink && config_.num_ues != 1)
    throw std::invalid_argument(
        "UL sensing currently requires num_ues=1 because CFR submissions do not carry UE identity");
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
  if (config_.tracker_enable && config_.hierarchical_tracker_enable && !surveyed_baseline)
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
    if (config_.hierarchical_tracker_enable && norm(config_.tx_position - config_.rx_position) > 0.0)
      hierarchical_tracker_ = std::make_unique<HierarchicalEnuTracker>(
          BistaticGeometry{config_.tx_position, config_.rx_position}, HierarchicalTrackerConfig{}, inner);
    else {
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
                           uint32_t re, float noise)
{
  std::lock_guard<std::mutex> admission(submission_mutex_);
  if (!running_.load(std::memory_order_relaxed) || !cfr || !subcarrier || !symbol
      || !re || re > maximum_re_) return;
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
  if (!free_.try_pop(value) || !value) {
    const uint64_t dropped = dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (dropped == 1 || (dropped & (dropped - 1)) == 0)
      std::fprintf(stderr,
                   "SENSING: CFR snapshot pool exhausted; run is incomplete (total=%llu)\n",
                   static_cast<unsigned long long>(dropped));
    return;
  }
  value->slot = slot; value->absolute_slot = absolute_slot;
  value->fraction = fraction >= 0.0f && fraction < 1.0f ? fraction : 0.0f;
  value->source = source; value->carrier = carrier;
  value->antennas = std::min(std::max(1u, antennas), requested_antennas_);
  value->resource_elements = re; value->noise_variance = noise;
  value->utc_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  value->cfr.assign(cfr, cfr + (size_t)value->antennas * re);
  value->subcarrier.assign(subcarrier, subcarrier + re);
  value->symbol.assign(symbol, symbol + re);
  ready_.push(value);
}

void SensingEngine::accumulation_run()
{
  for (;;) {
    Snapshot* value = ready_.wait_pop();
    if (!value) break;
    try { consume(*value); }
    catch (const std::exception& e) { std::fprintf(stderr, "SENSING: dropped CFR occurrence: %s\n", e.what()); }
    value->cfr.clear(); value->subcarrier.clear(); value->symbol.clear(); free_.push(value);
  }
  try { finish_pending_windows(); }
  catch (const std::exception& e) { std::fprintf(stderr, "SENSING: final window failed: %s\n", e.what()); }
  windows_.close();
}

void SensingEngine::processing_run()
{
  while (auto task = windows_.wait_pop()) {
    try {
      process_window(std::move(task->dl_window), std::move(task->ul_window), task->plan,
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
  std::lock_guard<std::mutex> lock(tracker_mutex_);
  if (motion_tracker_) motion_tracker_->reset();
  if (ul_motion_tracker_) ul_motion_tracker_->reset();
  if (hierarchical_tracker_) hierarchical_tracker_->reset();
}

size_t SensingEngine::pending_row_storage_bytes(const PendingRow& row) const
{
  return row.cfr.capacity() * sizeof(std::complex<float>)
         + row.weights.capacity() * sizeof(float)
         + row.ul_cfr.capacity() * sizeof(std::complex<float>)
         + row.ul_weights.capacity() * sizeof(float);
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
    const size_t view_bytes = (size_t)requested_antennas_ * subcarriers
                              * (sizeof(std::complex<float>) + sizeof(float));
    const size_t incoming = 2 * view_bytes;
    make_pending_row_room(incoming);
    PendingRow row;
    row.time_slots = time_slots; row.raw_slot = s.slot; row.slot_fraction = fraction;
    row.first_utc_ns = s.utc_ns;
    row.cfr.assign((size_t)requested_antennas_ * subcarriers, {});
    row.weights.assign((size_t)requested_antennas_ * subcarriers, 0.0f);
    row.ul_cfr.assign((size_t)requested_antennas_ * subcarriers, {});
    row.ul_weights.assign((size_t)requested_antennas_ * subcarriers, 0.0f);
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
    row.ul_source_mask |= 1u << static_cast<uint32_t>(s.source);
    ++row.ul_source_occurrences[static_cast<uint32_t>(s.source)];
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
      else accumulate(row.ul_cfr, row.ul_weights);
    }
  }
  close_ready_windows(false);
}

TrackSnapshot SensingEngine::planning_snapshot(double time) const
{
  std::lock_guard<std::mutex> lock(tracker_mutex_);
  if (hierarchical_tracker_) return hierarchical_tracker_->predict_to(time);
  if (motion_tracker_) return motion_tracker_->predict_to(time);
  TrackSnapshot value; value.status = "uninitialized"; value.air_time_s = time; return value;
}

void SensingEngine::enqueue_window(CfrWindow dl_window, std::optional<CfrWindow> ul_window,
                                   const CpiPlan& plan)
{
  auto task = std::make_unique<WindowTask>();
  task->dl_window = std::move(dl_window);
  task->ul_window = std::move(ul_window);
  task->plan = plan;
  task->air_origin_slots = air_origin_slots_.value_or(0.0);
  task->sequence = ++cpi_sequence_;
  {
    std::lock_guard<std::mutex> lock(processing_mutex_);
    if (processing_in_flight_)
      throw std::logic_error("cannot plan a CPI before the prior tracker update");
    processing_in_flight_ = true;
  }
  try { windows_.push(std::move(task)); }
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
    auto after = rows_.upper_bound(static_cast<int64_t>(
        std::floor(cutoff * ROW_TICKS_PER_SLOT + 1e-6)));
    const bool row_ceiling = rows_.size() >= active_plan_->maximum_rows;
    const bool dwell_ready = after != rows_.end();
    if (!flush && !dwell_ready && !row_ceiling) return;
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
    auto viable = [&](bool uplink) {
      size_t count = 0;
      double first_time = 0.0, last_time = 0.0;
      for (int64_t key : keys) {
        const PendingRow& row = rows_.at(key);
        const uint32_t mask = uplink ? row.ul_source_mask : row.dl_source_mask;
        if (!mask) continue;
        if (!count) first_time = row.time_slots;
        last_time = row.time_slots;
        ++count;
      }
      return count >= active_plan_->minimum_rows && last_time > first_time;
    };
    if (viable(false)) {
      CfrWindow dl_window = build_window(keys, false);
      std::optional<CfrWindow> ul_window;
      if (viable(true)) ul_window = build_window(keys, true, dl_window.antennas);
      const CpiPlan plan = *active_plan_;
      const double last_time_slots = rows_.at(keys.back()).time_slots;
      erase_rows(keys);
      last_closed_slots_ = last_time_slots; active_plan_.reset();
      enqueue_window(std::move(dl_window), std::move(ul_window), plan);
      return; // Python parity: plan the next CPI only after this tracker update finishes.
    } else {
      const double last_time_slots = rows_.at(keys.back()).time_slots;
      erase_rows(keys);
      last_closed_slots_ = last_time_slots;
      active_plan_.reset();
    }
  }
}

CfrWindow SensingEngine::build_window(const std::vector<int64_t>& keys, bool uplink,
                                      uint32_t forced_antennas) const
{
  std::vector<int64_t> selected_keys;
  selected_keys.reserve(keys.size());
  for (int64_t key : keys) {
    const PendingRow& row = rows_.at(key);
    if (uplink ? row.ul_source_mask : row.dl_source_mask) selected_keys.push_back(key);
  }
  if (selected_keys.empty())
    throw std::runtime_error(uplink ? "UL measured window has no rows"
                                    : "DL measured window has no rows");
  CfrWindow w;
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
    for (uint32_t i = 0; i < NR_ISAC_SRC_COUNT; ++i)
      w.source_occurrences[i] += uplink ? row.ul_source_occurrences[i]
                                        : row.dl_source_occurrences[i];
    w.row_time_slots[r] = row.time_slots; w.row_slot_idx[r] = row.raw_slot;
    w.row_slot_frac[r] = row.slot_fraction;
    w.row_source_mask[r] = uplink ? row.ul_source_mask : row.dl_source_mask;
    const auto& row_weights = uplink ? row.ul_weights : row.weights;
    const auto& row_cfr = uplink ? row.ul_cfr : row.cfr;
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

std::vector<ConfirmedTrackView> SensingEngine::confirmed_tracks() const
{
  if (hierarchical_tracker_) return hierarchical_tracker_->confirmed_tracks();
  return motion_tracker_ ? motion_tracker_->confirmed_tracks() : std::vector<ConfirmedTrackView>{};
}

AdaptiveClutterMap* SensingEngine::clutter_map()
{
  if (hierarchical_tracker_) return &hierarchical_tracker_->clutter_map();
  return motion_tracker_ ? &motion_tracker_->clutter_map() : nullptr;
}

void SensingEngine::process_window(CfrWindow dl_window, std::optional<CfrWindow> ul_window,
                                   const CpiPlan& plan,
                                   double air_origin_slots, uint64_t sequence)
{
  // The correction is hardware-specific and target-blind. Apply it before common-mode clock
  // correction, family alignment, noncoherent detection, and AoA so every downstream block sees
  // channels in physical-element order with the same calibrated gain/phase/delay convention.
  apply_array_calibration(dl_window, config_.array_calibration);
  if (ul_window) apply_array_calibration(*ul_window, config_.array_calibration);
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
  if (ul_window) record_sources(*ul_window);
  const double midpoint_slots = 0.5 * (dl_window.row_time_slots.front()
                                       + dl_window.row_time_slots.back());
  report.midpoint_air_time_s = (midpoint_slots - air_origin_slots)
                               * slot_duration_s(dl_window.scs_hz);
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
  report.detector = detect_clean(dl_detector_input, config_, dl_gate, 0);
  // Backward-compatible capture fields now both name the same provenance-clean DL detector map.
  report.detector.initial_dl_likelihood = report.detector.initial_likelihood;
  report.detector.dl_observed_re_count = report.detector.axes.observed_re_count;

  auto accept = [&](const DetectorResult& detector, const CfrWindow& input,
                    const std::vector<ConfirmedTrackView>& priors,
                    AdaptiveClutterMap* clutter) {
    std::vector<Detection> accepted;
    for (const CleanComponent& object : detector.objects) {
      Detection d;
      d.range_m = object.range_bin * detector.axes.range_res_m;
      d.range_rate_mps = -(object.doppler_bin - input.rows / 2.0)
                         * detector.axes.rate_res_mps;
      if (std::abs(d.range_rate_mps) > config_.maximum_target_speed_mps) continue;
      d.score = object.score;
      d.decision_statistic = object.local.z;
      d.decision_threshold = object.local_threshold;
      d.effective_decision_threshold = object.local_threshold;
      d.source_component_iteration = object.iteration;
      d.object_component_count = object.object_component_count;
      d.dwell_s = detector.axes.dwell_s;
      if (object.localization.covariance_valid) {
        d.covariance_valid = true;
        d.range_rate_covariance = object.localization.covariance_range_rate;
      }
      bool near = false, artifact = false;
      for (const auto& track : priors) {
        const double sr = std::max(track.sigma_range_m, detector.axes.range_res_m);
        const double sv = std::max(track.sigma_rate_mps, detector.axes.rate_res_mps);
        const double dr = std::abs(d.range_m - track.range_m);
        const double dv = std::abs(d.range_rate_mps - track.rate_mps);
        if (std::pow(dr / (3.0 * sr), 2.0) + std::pow(dv / (3.0 * sv), 2.0) <= 1.0)
          near = true;
        else if (is_aperture_sidelobe(d, track, detector.axes.range_res_m,
                                      detector.axes.rate_res_mps)
                 || is_multipath_shadow(d, track, detector.axes.range_res_m,
                                        detector.axes.rate_res_mps)) {
          artifact = true;
          break;
        }
      }
      if (artifact) continue;
      if (!priors.empty()) d.effective_decision_threshold += near ? -0.2 : 0.5;
      if (!near && clutter && clutter->is_static(
              d.range_m, d.range_rate_mps, d.score, detector.axes.range_res_m,
              detector.axes.rate_res_mps, false))
        continue;
      if (d.decision_statistic > d.effective_decision_threshold)
        accepted.push_back(std::move(d));
    }
    return accepted;
  };

  report.detections = accept(
      report.detector, dl_detector_input, confirmed_tracks(), clutter_map());
  attach_aoa(dl_corrected, report.detector.components, report.detector.axes,
             config_, report.detections);

  std::optional<CfrWindow> ul_corrected;
  if (ul_window) {
    report.uplink_present = true;
    try {
      ul_corrected = *ul_window;
      report.uplink_sync.rows = ul_window->rows;
      PipelineConfig ul_config = config_;
      ul_config.aoa_enable = config_.aoa_ul_enable;
      if (config_.sync_enable && ul_window->rows >= 3) {
        report.uplink_sync = estimate_sync(*ul_window);
        report.uplink_sync = ul_clock_tracker_.update(
            report.uplink_sync, report.midpoint_air_time_s,
            ul_window->subcarriers, ul_window->scs_hz);
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
      report.uplink_detector = detect_clean(ul_detector_input, config_, ul_gate, 0);
      const std::vector<ConfirmedTrackView> ul_priors = hierarchical_tracker_
          ? hierarchical_tracker_->auxiliary_confirmed_tracks()
          : (ul_motion_tracker_ ? ul_motion_tracker_->confirmed_tracks()
                                : std::vector<ConfirmedTrackView>{});
      AdaptiveClutterMap* ul_clutter = hierarchical_tracker_
          ? &hierarchical_tracker_->auxiliary_clutter_map()
          : (ul_motion_tracker_ ? &ul_motion_tracker_->clutter_map() : nullptr);
      report.uplink_detections = accept(
          report.uplink_detector, ul_detector_input, ul_priors, ul_clutter);
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
      hierarchical_tracker_->update(report.midpoint_air_time_s, report.detections,
          report.detector.axes.range_res_m, report.detector.axes.rate_res_mps,
          report.cpi_sequence, report.detector.axes.dwell_s);
      if (report.uplink_valid) {
        hierarchical_tracker_->update_auxiliary(
            report.midpoint_air_time_s, report.uplink_detections,
            report.uplink_detector.axes.range_res_m,
            report.uplink_detector.axes.rate_res_mps, report.cpi_sequence);
        report.uplink_tracks = hierarchical_tracker_->auxiliary_snapshots();
      }
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
