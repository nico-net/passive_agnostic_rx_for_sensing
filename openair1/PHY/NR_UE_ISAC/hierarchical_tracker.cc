/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "hierarchical_tracker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace nr_isac {

HierarchicalEnuTracker::HierarchicalEnuTracker(BistaticGeometry geometry,
                                               HierarchicalTrackerConfig config,
                                               MotionTrackerConfig inner)
    : geometry_(geometry), config_(config), inner_config_(std::move(inner)),
      motion_tracker_(inner_config_), auxiliary_motion_tracker_(inner_config_)
{
  geometry_.validate();
  if (!(config_.gate_chi2_2d > 0.0) || !(config_.gate_chi2_4d > 0.0)
      || !(config_.adaptation_alpha > 0.0 && config_.adaptation_alpha <= 1.0)
      || !(config_.maximum_acceleration_variance > 0.0)
      || !(config_.initial_maximum_velocity_variance > 0.0)
      || !(config_.maximum_tangential_speed_mps > 0.0)
      || !(config_.aoa_temporal_sigma > 0.0)
      || !(config_.minimum_aoa_temporal_stddev_deg > 0.0)
      || config_.ul_confirmation_hits < 1
      || config_.ul_confirmation_hits > config_.ul_confirmation_window)
    throw std::invalid_argument("invalid hierarchical tracker configuration");
}

void HierarchicalEnuTracker::reset()
{
  motion_tracker_.reset(); auxiliary_motion_tracker_.reset(); global_tracks_.clear();
  stage1_to_global_.clear();
  valid_aoa_cache_.clear(); ul_confirmation_history_.clear(); next_global_id_ = 1;
}

void HierarchicalEnuTracker::update_auxiliary(
    double time, const std::vector<Detection>& detections, double range_res,
    double rate_res, uint64_t sequence)
{
  // The passive receiver does not know a UE transmitter position a priori. Keep UL measurements
  // as independent Stage-1 range/rate tracks; never inject oracle geometry into the DL ENU EKFs.
  auxiliary_motion_tracker_.update(time, detections, range_res, rate_res, sequence);
}

TrackSnapshot HierarchicalEnuTracker::predict_to(double time) const
{
  return motion_tracker_.predict_to(time);
}

void HierarchicalEnuTracker::update(double time, const std::vector<Detection>& detections,
                                    double range_res, double rate_res, uint64_t sequence,
                                    double dwell, bool ul_motion_active,
                                    const std::vector<AoaEstimate>& auxiliary_aoa_measurements,
                                    bool require_ul_confirmation_for_birth,
                                    bool require_candidate_ul_confirmation_for_birth)
{
  motion_tracker_.update(time, detections, range_res, rate_res, sequence, ul_motion_active);
  for (auto& item : global_tracks_) item.second->predict(time);
  const auto active = motion_tracker_.active_tracks();
  std::set<uint64_t> active_ids;
  for (const auto& view : active) {
    const auto& s = view.snapshot; active_ids.insert(s.track_id);
    if (s.updated && view.associated_index && *view.associated_index < detections.size()) {
      const Detection& d = detections[*view.associated_index];
      if (d.aoa.valid && d.aoa.covariance_rad2.rows() == 2)
        valid_aoa_cache_[s.track_id] = d;
      std::optional<uint8_t> confirmation;
      if (d.ul_confirmation_candidate_specific)
        confirmation = d.ul_confirmation_supported ? 1 : 0;
      else if (d.ul_confirmation_status == "no_cross_leg_bearing_match_preserved")
        confirmation = 0;
      if (confirmation) {
        auto& history = ul_confirmation_history_[s.track_id];
        history.push_back(*confirmation);
        if (history.size() > config_.ul_confirmation_window) history.erase(history.begin());
      }
    }
  }

  std::set<uint64_t> updated_global;
  for (const auto& view : active) {
    const auto& s = view.snapshot;
    if (!s.updated || !view.associated_index || *view.associated_index >= detections.size()) continue;
    auto binding = stage1_to_global_.find(s.track_id);
    if (binding == stage1_to_global_.end() || !global_tracks_.count(binding->second)
        || updated_global.count(binding->second)) continue;
    global_tracks_.at(binding->second)->update(detections[*view.associated_index], range_res,
                                               rate_res, view.associated_index,
                                               s.range_m, s.range_rate_mps);
    updated_global.insert(binding->second);
  }

  EnuTrackerConfig enu_config;
  enu_config.gate_chi2_2d = config_.gate_chi2_2d;
  enu_config.gate_chi2_4d = config_.gate_chi2_4d;
  enu_config.confirm_updates = config_.confirm_updates;
  enu_config.confirm_window = config_.confirm_window;
  enu_config.adaptation_alpha = config_.adaptation_alpha;
  enu_config.adapt_acceleration_variance = false;
  enu_config.maximum_tangential_speed_mps = config_.maximum_tangential_speed_mps;
  enu_config.aoa_temporal_sigma = config_.aoa_temporal_sigma;
  enu_config.minimum_aoa_temporal_stddev_deg = config_.minimum_aoa_temporal_stddev_deg;

  for (const auto& view : active) {
    const TrackSnapshot& s = view.snapshot;
    auto binding = stage1_to_global_.find(s.track_id);
    const uint64_t bound = binding == stage1_to_global_.end() ? 0 : binding->second;
    if (bound && updated_global.count(bound)) continue;
    const bool confirmed = s.status == "confirmed" || s.confirmed_update_count >= 1
                           || s.total_update_count >= config_.confirm_updates;
    if (!confirmed) continue;

    const Detection* current = nullptr;
    if (s.updated && view.associated_index && *view.associated_index < detections.size())
      current = &detections[*view.associated_index];
    const Detection* test = current;
    if (!test) {
      const auto cache = valid_aoa_cache_.find(s.track_id);
      if (cache != valid_aoa_cache_.end()) test = &cache->second;
    }
    uint64_t best_id = 0;
    double best_nis = std::numeric_limits<double>::infinity();
    if (test)
      for (const auto& candidate : global_tracks_) {
        if (updated_global.count(candidate.first)) continue;
        const EnuInnovation fit = candidate.second->innovation(*test, range_res, rate_res, true);
        if (fit.valid && fit.nis <= config_.gate_chi2_2d && fit.nis < best_nis)
          best_nis = fit.nis, best_id = candidate.first;
      }
    if (best_id) {
      stage1_to_global_[s.track_id] = best_id;
      if (current) {
        global_tracks_.at(best_id)->update(*current, range_res, rate_res,
                                           view.associated_index, s.range_m,
                                           s.range_rate_mps);
        updated_global.insert(best_id);
      }
      continue;
    }
    if (bound) continue;
    const Detection* birth = current && current->aoa.valid ? current : nullptr;
    if (!birth) {
      const auto cache = valid_aoa_cache_.find(s.track_id);
      if (cache != valid_aoa_cache_.end() && cache->second.aoa.valid) birth = &cache->second;
    }
    if (!birth) continue;
    if (require_ul_confirmation_for_birth && !ul_motion_active) continue;
    const auto confirmation = ul_confirmation_history_.find(s.track_id);
    const uint32_t confirmation_hits = confirmation == ul_confirmation_history_.end()
        ? 0 : std::accumulate(confirmation->second.begin(), confirmation->second.end(), 0u);
    if (require_candidate_ul_confirmation_for_birth
        && confirmation_hits < config_.ul_confirmation_hits)
      continue;
    try {
      const uint64_t id = next_global_id_++;
      auto track = std::make_unique<EnuTrack>(id, time, *birth, geometry_, range_res,
                                              rate_res, birth->dwell_s > 0.0 ? birth->dwell_s : dwell,
                                              enu_config, view.associated_index,
                                              s.range_m, s.range_rate_mps);
      track->cap_birth_uncertainty(config_.initial_maximum_velocity_variance,
                                   config_.maximum_acceleration_variance);
      global_tracks_[id] = std::move(track); stage1_to_global_[s.track_id] = id;
      updated_global.insert(id);
    } catch (...) {
      // Python reference refuses nonphysical 3-D births and leaves Stage 1 alive.
    }
  }

  // An UL reflection reaches the same receive array, so its admitted direction is a bearing-only
  // observation of an existing DL track. UL bistatic range/rate is deliberately excluded because
  // its measurement model would require the unknown UE transmitter position.
  std::vector<std::tuple<double, uint64_t, size_t>> auxiliary_pairs;
  for (const auto& item : global_tracks_) {
    const auto& track = *item.second;
    if (track.last_update_used_angles() || !track.auxiliary_aoa_allowed()) continue;
    for (size_t index = 0; index < auxiliary_aoa_measurements.size(); ++index) {
      const EnuInnovation fit = track.angle_innovation(auxiliary_aoa_measurements[index]);
      if (fit.valid && fit.nis <= config_.gate_chi2_2d)
        auxiliary_pairs.emplace_back(fit.nis, item.first, index);
    }
  }
  std::sort(auxiliary_pairs.begin(), auxiliary_pairs.end());
  std::set<uint64_t> used_global;
  std::set<size_t> used_auxiliary;
  for (const auto& [nis, global_id, auxiliary_index] : auxiliary_pairs) {
    (void)nis;
    if (used_global.count(global_id) || used_auxiliary.count(auxiliary_index)) continue;
    if (global_tracks_.at(global_id)->update_angles(
            auxiliary_aoa_measurements[auxiliary_index])) {
      used_global.insert(global_id);
      used_auxiliary.insert(auxiliary_index);
      updated_global.insert(global_id);
    }
  }

  const uint32_t maximum_coasts = config_.maximum_3d_coasts + (ul_motion_active ? 4 : 0);
  for (auto it = global_tracks_.begin(); it != global_tracks_.end();) {
    if (!updated_global.count(it->first)) it->second->coast(ul_motion_active);
    if (it->second->coasts() > maximum_coasts) it = global_tracks_.erase(it);
    else ++it;
  }
  for (auto it = stage1_to_global_.begin(); it != stage1_to_global_.end();) {
    if (!active_ids.count(it->first) || !global_tracks_.count(it->second)) it = stage1_to_global_.erase(it);
    else ++it;
  }
  for (auto it = valid_aoa_cache_.begin(); it != valid_aoa_cache_.end();) {
    if (!active_ids.count(it->first)) it = valid_aoa_cache_.erase(it); else ++it;
  }
  for (auto it = ul_confirmation_history_.begin(); it != ul_confirmation_history_.end();) {
    if (!active_ids.count(it->first)) it = ul_confirmation_history_.erase(it); else ++it;
  }
}

std::vector<TrackSnapshot> HierarchicalEnuTracker::snapshots() const
{
  std::vector<TrackSnapshot> out;
  out.reserve(global_tracks_.size());
  for (const auto& item : global_tracks_) out.push_back(item.second->snapshot());
  return out;
}

TrackSnapshot HierarchicalEnuTracker::snapshot() const
{
  const auto values = snapshots();
  if (values.empty()) return motion_tracker_.snapshot();
  auto priority = [](const std::string& s) { return s == "confirmed" ? 2 : (s == "coasting" ? 1 : 0); };
  return *std::max_element(values.begin(), values.end(), [&](const TrackSnapshot& a,
                                                             const TrackSnapshot& b) {
    const double ua = trace(a.position_covariance), ub = trace(b.position_covariance);
    return std::tuple(priority(a.status), -ua, -static_cast<int64_t>(a.track_id))
           < std::tuple(priority(b.status), -ub, -static_cast<int64_t>(b.track_id));
  });
}

} // namespace nr_isac
