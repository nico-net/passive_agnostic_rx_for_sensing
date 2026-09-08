/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "sensing_engine.h"

#include "aoa.h"
#include "detector.h"
#include "sync_correction.h"

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
  std::array<uint64_t, NR_ISAC_SRC_COUNT> source_occurrences{};
  int64_t first_utc_ns = 0;
  uint32_t available_antennas = 0;
  std::vector<std::complex<float>> cfr; // [antenna][subcarrier]
  std::vector<float> weights;           // same shape; inverse-variance sum
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
  if (config_.aoa_ul_enable && !config_.aoa_enable)
    throw std::invalid_argument("AOA_UL_ENABLE cannot be active when AOA_ENABLE is off");
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
    else motion_tracker_ = std::make_unique<MotionTracker>(inner);
  }
  writer_ = std::make_unique<ReportWriter>(config_);
}

SensingEngine::~SensingEngine() { stop(); }

void SensingEngine::start()
{
  if (running_.exchange(true)) return;
  worker_ = std::thread(&SensingEngine::run, this);
}

void SensingEngine::stop()
{
  if (!running_.exchange(false)) return;
  ready_.push(nullptr);
  if (worker_.joinable()) worker_.join();
}

void SensingEngine::submit(uint32_t slot, float fraction, nr_isac_source_t source,
                           const nr_isac_carrier_t& carrier,
                           const std::complex<float>* cfr, uint32_t antennas,
                           const uint32_t* subcarrier, const uint32_t* symbol,
                           uint32_t re, float noise)
{
  if (!running_.load(std::memory_order_relaxed) || !cfr || !subcarrier || !symbol
      || !re || re > maximum_re_) return;
  Snapshot* value = nullptr;
  if (!free_.try_pop(value) || !value) { dropped_.fetch_add(1, std::memory_order_relaxed); return; }
  value->slot = slot; value->fraction = fraction >= 0.0f && fraction < 1.0f ? fraction : 0.0f;
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

void SensingEngine::run()
{
  for (;;) {
    Snapshot* value = ready_.wait_pop();
    if (!value) break;
    try { consume(*value); }
    catch (const std::exception& e) { std::fprintf(stderr, "SENSING: dropped CFR occurrence: %s\n", e.what()); }
    value->cfr.clear(); value->subcarrier.clear(); value->symbol.clear(); free_.push(value);
  }
  try { close_ready_windows(true); }
  catch (const std::exception& e) { std::fprintf(stderr, "SENSING: final window failed: %s\n", e.what()); }
}

int64_t SensingEngine::unwrap_slot(uint32_t raw, const nr_isac_carrier_t& carrier)
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
  have_slot_clock_ = false; latest_absolute_slot_ = 0; latest_raw_slot_ = 0;
  air_origin_slots_.reset(); last_closed_slots_.reset();
  clock_tracker_.reset(); planner_.reset();
  if (motion_tracker_) motion_tracker_->reset();
  if (hierarchical_tracker_) hierarchical_tracker_->reset();
}

void SensingEngine::consume(const Snapshot& s)
{
  const uint32_t subcarriers = s.carrier.nof_prb * 12u;
  if (!subcarriers || !s.carrier.scs_hz || !s.carrier.dl_center_hz) return;
  if (!have_geometry_ || carrier_.nof_prb != s.carrier.nof_prb
      || carrier_.scs_hz != s.carrier.scs_hz || carrier_.dl_center_hz != s.carrier.dl_center_hz
      || carrier_.pci != s.carrier.pci) begin_geometry(s.carrier);
  const int64_t absolute_slot = unwrap_slot(s.slot, s.carrier);
  const double fraction = std::clamp(static_cast<double>(s.fraction), 0.0,
                                     std::nextafter(1.0, 0.0));
  const int64_t key = row_key(absolute_slot, fraction);
  const double time_slots = absolute_slot + fraction;
  if (last_closed_slots_ && time_slots <= *last_closed_slots_ + 1e-9) { ++stale_; return; }
  auto found = rows_.find(key);
  if (found == rows_.end()) {
    PendingRow row;
    row.time_slots = time_slots; row.raw_slot = s.slot; row.slot_fraction = fraction;
    row.first_utc_ns = s.utc_ns;
    row.cfr.assign((size_t)requested_antennas_ * subcarriers, {});
    row.weights.assign((size_t)requested_antennas_ * subcarriers, 0.0f);
    found = rows_.emplace(key, std::move(row)).first;
  }
  PendingRow& row = found->second;
  row.source_mask |= 1u << static_cast<uint32_t>(s.source);
  ++row.source_occurrences[static_cast<uint32_t>(s.source)];
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
      const float old_weight = row.weights[index], total = old_weight + weight;
      row.cfr[index] = (row.cfr[index] * old_weight
                        + s.cfr[(size_t)a * s.resource_elements + i] * weight) / total;
      row.weights[index] = total;
    }
  }
  ensure_plan(); close_ready_windows(false);
}

TrackSnapshot SensingEngine::planning_snapshot(double time) const
{
  if (hierarchical_tracker_) return hierarchical_tracker_->predict_to(time);
  if (motion_tracker_) return motion_tracker_->predict_to(time);
  TrackSnapshot value; value.status = "uninitialized"; value.air_time_s = time; return value;
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
    ensure_plan(); if (!active_plan_ || rows_.empty()) return;
    const double first = rows_.begin()->second.time_slots;
    const double cutoff = first + active_plan_->target_dwell_s / slot_duration_s(carrier_.scs_hz);
    auto after = rows_.upper_bound(static_cast<int64_t>(
        std::floor(cutoff * ROW_TICKS_PER_SLOT + 1e-6)));
    bool row_ceiling = rows_.size() > active_plan_->maximum_rows;
    if (!flush && after == rows_.end() && !row_ceiling) return;
    std::vector<int64_t> keys;
    if (row_ceiling) {
      auto it = rows_.begin();
      for (uint32_t n = 0; n < active_plan_->maximum_rows && it != rows_.end(); ++n, ++it)
        keys.push_back(it->first);
    } else if (flush) {
      for (const auto& item : rows_) keys.push_back(item.first);
    } else {
      for (auto it = rows_.begin(); it != after; ++it) keys.push_back(it->first);
    }
    if (keys.empty()) return;
    const double span = rows_.at(keys.back()).time_slots - rows_.at(keys.front()).time_slots;
    if (keys.size() >= active_plan_->minimum_rows && span > 0.0) {
      CfrWindow window = build_window(keys);
      const CpiPlan plan = *active_plan_;
      for (int64_t key : keys) rows_.erase(key);
      last_closed_slots_ = window.row_time_slots.back(); active_plan_.reset();
      process_window(std::move(window), plan);
    } else {
      const double last_time_slots = rows_.at(keys.back()).time_slots;
      for (int64_t key : keys) rows_.erase(key);
      last_closed_slots_ = last_time_slots;
      active_plan_.reset();
    }
  }
}

CfrWindow SensingEngine::build_window(const std::vector<int64_t>& keys) const
{
  CfrWindow w;
  w.rows = keys.size(); w.subcarriers = carrier_.nof_prb * 12u;
  w.antennas = requested_antennas_;
  for (int64_t key : keys) w.antennas = std::min(w.antennas, rows_.at(key).available_antennas);
  w.antennas = std::max(1u, w.antennas); w.scs_hz = carrier_.scs_hz;
  w.fc_hz = carrier_.dl_center_hz; w.pci = carrier_.pci;
  w.start_utc_ns = rows_.at(keys.front()).first_utc_ns;
  const size_t cells = (size_t)w.rows * w.subcarriers;
  w.values.assign(cells * w.antennas, {}); w.observed.assign(cells, 0);
  w.row_time_slots.resize(w.rows); w.row_slot_idx.resize(w.rows);
  w.row_slot_frac.resize(w.rows); w.row_source_mask.resize(w.rows);
  for (uint32_t r = 0; r < w.rows; ++r) {
    const PendingRow& row = rows_.at(keys[r]);
    for (uint32_t i = 0; i < NR_ISAC_SRC_COUNT; ++i)
      w.source_occurrences[i] += row.source_occurrences[i];
    w.row_time_slots[r] = row.time_slots; w.row_slot_idx[r] = row.raw_slot;
    w.row_slot_frac[r] = row.slot_fraction; w.row_source_mask[r] = row.source_mask;
    for (uint32_t k = 0; k < w.subcarriers; ++k) {
      bool common = true;
      for (uint32_t a = 0; a < w.antennas; ++a)
        common = common && row.weights[(size_t)a * w.subcarriers + k] > 0.0f;
      w.observed[w.cell(r, k)] = common;
      if (common) for (uint32_t a = 0; a < w.antennas; ++a)
        w.values[w.sample(a, r, k)] = row.cfr[(size_t)a * w.subcarriers + k];
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

void SensingEngine::process_window(CfrWindow window, const CpiPlan& plan)
{
  PipelineReport report; report.cpi_sequence = ++cpi_sequence_; report.start_utc_ns = window.start_utc_ns;
  const double slot_ns = slot_duration_s(window.scs_hz) * 1e9;
  report.first_row_time_ns = std::llround(window.row_time_slots.front() * slot_ns);
  report.last_row_time_ns = std::llround(window.row_time_slots.back() * slot_ns);
  report.cpi_duration_ns = std::max<int64_t>(0, report.last_row_time_ns - report.first_row_time_ns);
  report.plan = plan; report.dropped_submissions = dropped_.load(std::memory_order_relaxed); report.stale_submissions = stale_;
  for (uint32_t mask : window.row_source_mask) {
    report.sources_mask |= mask;
  }
  report.source_occurrences = window.source_occurrences;
  const double midpoint_slots = 0.5 * (window.row_time_slots.front() + window.row_time_slots.back());
  report.midpoint_air_time_s = (midpoint_slots - *air_origin_slots_) * slot_duration_s(window.scs_hz);
  CfrWindow corrected = window;
  report.sync.rows = window.rows;
  if (config_.sync_enable && window.rows >= 3) {
    report.sync = estimate_sync(window);
    report.sync = clock_tracker_.update(report.sync, report.midpoint_air_time_s,
                                        window.subcarriers, window.scs_hz);
    std::optional<std::array<std::complex<double>, 4>> los;
    if (window.antennas == 4 && config_.array.configured
        && norm(config_.tx_position - config_.rx_position) > 0.0)
      los = surveyed_los_steering(config_.array, config_.tx_position, config_.rx_position, window.fc_hz);
    apply_sync_correction(corrected, report.sync, 0.0, los);
  }
  report.current_cpi_variance = estimate_current_cpi_variance(
      corrected, &report.covariance_family_count, &report.covariance_difference_count);
  CfrWindow detector_input = corrected;
  report.detector_alignment = align_allocation_families(detector_input, config_.family_static);
  const double dwell = (detector_input.row_time_slots.back() - detector_input.row_time_slots.front())
                       * slot_duration_s(detector_input.scs_hz);
  const RateGate gate = finalize_search_gate(plan, detector_input.rows, dwell,
                                              detector_input.fc_hz, config_);
  report.detector = detect_clean(detector_input, config_, gate, 0);
  const auto priors = confirmed_tracks();
  AdaptiveClutterMap* clutter = clutter_map();
  for (const CleanComponent& object : report.detector.objects) {
    Detection d;
    d.range_m = object.range_bin * report.detector.axes.range_res_m;
    d.range_rate_mps = -(object.doppler_bin - detector_input.rows / 2.0)
                       * report.detector.axes.rate_res_mps;
    if (std::abs(d.range_rate_mps) > config_.maximum_target_speed_mps) continue;
    d.score = object.score; d.decision_statistic = object.local.z;
    d.decision_threshold = object.local_threshold; d.effective_decision_threshold = object.local_threshold;
    d.source_component_iteration = object.iteration; d.object_component_count = object.object_component_count;
    d.dwell_s = report.detector.axes.dwell_s;
    if (object.localization.covariance_valid) {
      d.covariance_valid = true; d.range_rate_covariance = object.localization.covariance_range_rate;
    }
    bool near = false, artifact = false;
    for (const auto& track : priors) {
      const double sr = std::max(track.sigma_range_m, report.detector.axes.range_res_m);
      const double sv = std::max(track.sigma_rate_mps, report.detector.axes.rate_res_mps);
      const double dr = std::abs(d.range_m - track.range_m), dv = std::abs(d.range_rate_mps - track.rate_mps);
      if (std::pow(dr / (3.0 * sr), 2.0) + std::pow(dv / (3.0 * sv), 2.0) <= 1.0) near = true;
      else if (is_aperture_sidelobe(d, track, report.detector.axes.range_res_m,
                                    report.detector.axes.rate_res_mps)
               || is_multipath_shadow(d, track, report.detector.axes.range_res_m,
                                      report.detector.axes.rate_res_mps)) { artifact = true; break; }
    }
    if (artifact) continue;
    if (!priors.empty()) d.effective_decision_threshold += near ? -0.2 : 0.5;
    if (!near && clutter && clutter->is_static(d.range_m, d.range_rate_mps, d.score,
                                                report.detector.axes.range_res_m,
                                                report.detector.axes.rate_res_mps, false)) continue;
    if (!(d.decision_statistic > d.effective_decision_threshold)) continue;
    report.detections.push_back(std::move(d));
  }
  attach_aoa(corrected, report.detector.components, report.detector.axes, config_, report.detections);
  if (hierarchical_tracker_) {
    hierarchical_tracker_->update(report.midpoint_air_time_s, report.detections,
        report.detector.axes.range_res_m, report.detector.axes.rate_res_mps,
        report.cpi_sequence, report.detector.axes.dwell_s);
    report.tracks = hierarchical_tracker_->snapshots();
  } else if (motion_tracker_) {
    motion_tracker_->update(report.midpoint_air_time_s, report.detections,
        report.detector.axes.range_res_m, report.detector.axes.rate_res_mps, report.cpi_sequence);
    report.tracks = motion_tracker_->snapshots();
  }
  writer_->emit(report);
}

} // namespace nr_isac
