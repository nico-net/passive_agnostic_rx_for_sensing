/* SPDX-License-Identifier: OAI-Public-License-1.1 */
// Included after Track: shares the existing geometry solver and IMM/EKF implementation.
// The legacy update remains available solely as ablation A until qualification succeeds.
namespace {
#ifdef NR_ISAC_EXPLANATION_BUNDLE
constexpr bool explanation_bundle = true;
#else
constexpr bool explanation_bundle = false;
#endif
struct TimedHardObservation {
  Observation value;
  double time_s = 0.0;
  uint64_t sequence = 0;
};

Observation with_reflector_uncertainty(Observation observation,
    const std::array<double,6>& state, const ReflectorPlane* plane)
{
  if (!plane) return observation;
  const std::array<double,3> parameters{std::atan2(plane->normal.y,plane->normal.x),
      std::asin(std::clamp(plane->normal.z,-1.0,1.0)),plane->offset_m};
  Matrix jacobian(2,3);
  for(size_t axis=0;axis<3;++axis) {
    const double step=std::cbrt(std::numeric_limits<double>::epsilon())*std::max(1.0,std::abs(parameters[axis]));
    auto prediction=[&](double sign) {
      auto p=parameters; p[axis]+=sign*step;
      auto perturbed=*plane;
      perturbed.normal={std::cos(p[1])*std::cos(p[0]),std::cos(p[1])*std::sin(p[0]),std::sin(p[1])};
      perturbed.offset_m=p[2];
      return reflected_dl_model(state,observation.geometry,perturbed);
    };
    const auto plus=prediction(1),minus=prediction(-1);
    for(size_t row=0;row<2;++row) jacobian(row,axis)=(plus[row]-minus[row])/(2*step);
  }
  observation.noise=observation.noise+jacobian*plane->covariance*jacobian.transposed();
  return observation;
}

std::pair<double, Matrix> asynchronous_cost(
    const std::vector<double>& state, double epoch,
    const std::vector<TimedHardObservation>& input, std::vector<double>* rhs = nullptr)
{
  Matrix information(6, 6);
  std::vector<double> response(6);
  double cost = 0.0;
  for (const auto& sample : input) {
    const double dt = sample.time_s - epoch;
    auto propagated = state;
    for (size_t axis = 0; axis < 3; ++axis) propagated[axis] += dt * state[axis + 3];
    const auto prediction = enu_measurement_model(propagated, sample.value.geometry, false);
    Matrix jacobian = enu_measurement_jacobian(propagated, sample.value.geometry, false);
    for (size_t row = 0; row < 2; ++row)
      for (size_t axis = 0; axis < 3; ++axis)
        jacobian(row, axis + 3) += dt * jacobian(row, axis);
    const Matrix weight = inverse(sample.value.noise);
    const std::vector<double> error{sample.value.value.range_m - prediction[0],
                                  sample.value.value.range_rate_mps - prediction[1]};
    cost += quadratic(error, weight);
    information = information + jacobian.transposed() * weight * jacobian;
    const auto increment = jacobian.transposed() * (weight * error);
    for (size_t axis = 0; axis < 6; ++axis) response[axis] += increment[axis];
  }
  if (rhs) *rhs = std::move(response);
  return {cost, information};
}

TemporalStateFit fit_asynchronous(const std::vector<TimedHardObservation>& input,
                                  double epoch, const MultistaticImmTrackerConfig& config,
                                  std::vector<TemporalStateFit>* branches = nullptr,
                                  uint64_t* multimodal_augmentations = nullptr)
{
  TemporalStateFit result;
  if (input.size() < 3) return result;
  std::vector<Observation> seeds;
  for (const auto& sample : input) {
    Observation seed = sample.value;
    seed.value.range_m += (epoch - sample.time_s) * seed.value.range_rate_mps;
    seeds.push_back(seed);
  }
  double best = std::numeric_limits<double>::infinity();
  // OUR ADAPTATION: algebraic_three_surface_seeds() can return up to two real roots of its
  // quadratic (a genuine, discrete two-mode ambiguity, not a smooth flat direction). The
  // Gauss-Newton covariance below is a LOCAL Laplace approximation valid only around whichever
  // single root it was computed at -- it correctly describes how much that one root could wobble
  // from measurement noise, but says nothing about the possibility that the OTHER root was
  // actually correct. Reporting it alone is a genuine miscalibration (confirmed by inspecting
  // held-out track output: a track sitting 10-20m from where any declared class can physically
  // be nonetheless reports ~1-2m of position std). All valid roots (not just the best) are kept
  // here so the returned covariance can be widened to honestly reflect the full multi-root
  // spread, using the SAME cost-based relative likelihood weighting already used throughout this
  // codebase (w_i ~ exp(-0.5*cost_i)) -- not a new tuned constant.
  std::vector<TemporalStateFit> all_candidates;
  for (const Vec3 position : algebraic_three_surface_seeds(seeds)) {
    try {
      const Vec3 velocity = fit_velocity(position, seeds).first;
      if (norm(velocity) > config.maximum_target_speed_mps) continue;
      std::vector<double> state{position.x, position.y, position.z,
                                velocity.x, velocity.y, velocity.z};
      double damping = 1e-6;
      for (uint32_t iteration = 0; iteration < config.birth_maximum_iterations; ++iteration) {
        std::vector<double> rhs;
        const auto [cost, info] = asynchronous_cost(state, epoch, input, &rhs);
        Matrix system = info;
        for (size_t axis = 0; axis < 6; ++axis)
          system(axis, axis) += damping * std::max(1.0, info(axis, axis));
        const auto step = inverse(system) * rhs;
        auto next = state;
        for (size_t axis = 0; axis < 6; ++axis) next[axis] += step[axis];
        const double next_cost = asynchronous_cost(next, epoch, input).first;
        if (next_cost < cost) {
          state = next;
          damping *= 0.25;
          if (std::abs(cost - next_cost) <= 1e-9 * std::max(1.0, cost)) break;
        } else damping *= 10.0;
      }
      const auto [cost, info] = asynchronous_cost(state, epoch, input);
      // Invert the full information: a weak/unobservable velocity is not silently zero variance.
      Matrix covariance = inverse(info);
      if (!std::isfinite(cost)) continue;
      const Vec3 speed{state[3], state[4], state[5]};
      if (norm(speed) > config.maximum_target_speed_mps) continue;
      bool valid = true;
      for (size_t axis = 0; axis < 6; ++axis)
        valid = valid && std::isfinite(covariance(axis, axis)) && covariance(axis, axis) > 0.0;
      for (const auto& sample : input) {
        const auto z = enu_measurement_model(state, sample.value.geometry, false);
        valid = valid && z[0] >= 0.0 && z[0] <= config.maximum_range_m;
      }
      if (!valid) continue;
      TemporalStateFit candidate;
      candidate.valid = true;
      candidate.position = {state[0], state[1], state[2]};
      candidate.velocity = speed;
      candidate.covariance = positive_semidefinite(symmetrized(covariance), 1e-12);
      candidate.normalized_rms = std::sqrt(cost / std::max<size_t>(1, 2 * input.size() - 6));
      Matrix positional(3, 3);
      for (size_t r = 0; r < 3; ++r)
        for (size_t c = 0; c < 3; ++c) positional(r, c) = info(r, c);
      candidate.condition = position_condition(positional);
      if (branches) {
        // Only collapse numerically identical solver roots, never nearby objects.
        const double tolerance = std::sqrt(std::numeric_limits<double>::epsilon());
        const bool repeated = std::any_of(branches->begin(), branches->end(), [&](const auto& old) {
          return norm(old.position-candidate.position) <= tolerance*std::max(1.0,norm(candidate.position))
              && norm(old.velocity-candidate.velocity) <= tolerance*std::max(1.0,norm(candidate.velocity));
        });
        if (!repeated) branches->push_back(candidate);
      }
      // Only distinct roots contribute to the multimodal covariance widening below; a repeated
      // numerically-identical solution from a different receiver triple would double-count.
      const double tolerance = std::sqrt(std::numeric_limits<double>::epsilon());
      const bool duplicate_root = std::any_of(
          all_candidates.begin(), all_candidates.end(), [&](const auto& old) {
        return norm(old.position-candidate.position) <= tolerance*std::max(1.0,norm(candidate.position))
            && norm(old.velocity-candidate.velocity) <= tolerance*std::max(1.0,norm(candidate.velocity));
      });
      if (!duplicate_root) all_candidates.push_back(candidate);
      if (cost < best) { best = cost; result = candidate; }
    } catch (...) { /* Unidentifiable geometry remains an unlocalized candidate. */ }
  }
  if (result.valid && all_candidates.size() > 1) {
    // Moment-match the full multi-root mixture's second moment ABOUT THE REPORTED (best-cost)
    // MEAN: E[(X - best_mean)(X - best_mean)^T] under the mixture = sum_i w_i * (Sigma_i +
    // outer(mean_i - best_mean, mean_i - best_mean)). This is deliberately more conservative than
    // moment-matching about the mixture's own (weighted) mean: the point estimate stays exactly
    // the best-supported root (unchanged from today), but the reported spread around it honestly
    // includes the possibility that a materially-different root was actually correct, instead of
    // only the local curvature at the chosen root.
    const std::vector<double> best_state{result.position.x, result.position.y, result.position.z,
                                         result.velocity.x, result.velocity.y, result.velocity.z};
    double min_rms2 = std::numeric_limits<double>::infinity();
    for (const auto& candidate : all_candidates)
      min_rms2 = std::min(min_rms2, candidate.normalized_rms * candidate.normalized_rms);
    std::vector<double> weights(all_candidates.size());
    double weight_sum = 0.0;
    for (size_t i = 0; i < all_candidates.size(); ++i) {
      // normalized_rms^2 is proportional to cost for a fixed input set (same degrees of freedom
      // across roots of the SAME triple), so this is the same cost-based relative likelihood
      // weighting (w ~ exp(-0.5*cost)) already used throughout this codebase.
      const double relative_cost = all_candidates[i].normalized_rms * all_candidates[i].normalized_rms
          - min_rms2;
      weights[i] = std::exp(-0.5 * relative_cost);
      weight_sum += weights[i];
    }
    if (std::isfinite(weight_sum) && weight_sum > 0.0) {
      Matrix augmented(6, 6);
      for (size_t i = 0; i < all_candidates.size(); ++i) {
        const double w = weights[i] / weight_sum;
        const std::vector<double> mean_i{
            all_candidates[i].position.x, all_candidates[i].position.y, all_candidates[i].position.z,
            all_candidates[i].velocity.x, all_candidates[i].velocity.y, all_candidates[i].velocity.z};
        for (size_t r = 0; r < 6; ++r)
          for (size_t c = 0; c < 6; ++c)
            augmented(r, c) += w * (all_candidates[i].covariance(r, c)
                + (mean_i[r] - best_state[r]) * (mean_i[c] - best_state[c]));
      }
      result.covariance = positive_semidefinite(symmetrized(augmented), 1e-12);
      if (multimodal_augmentations) ++*multimodal_augmentations;
    }
  }
  return result;
}

} // namespace

LocalMapEvidence local_map_evidence(const ReceiverDetectionBatch& batch,
                                    const std::array<double, 6>& state,
                                    const Matrix& covariance, uint32_t maximum_cells,
                                    const std::vector<MapExplanation>& explanations)
{
  LocalMapEvidence result;
  if (!batch.soft_evidence || !batch.soft_evidence->valid()
      || !batch.soft_evidence->search_complete || !batch.reference_valid) return result;
  const auto& map = *batch.soft_evidence;
  try {
    const std::vector<double> x(state.begin(), state.end());
    const auto z = enu_measurement_model(x, batch.geometry, false);
    const Matrix h = enu_measurement_jacobian(x, batch.geometry, false);
    Matrix variance = h * covariance * h.transposed();
    const double rr = map.axes.range_res_m, vr = map.axes.rate_res_mps;
    variance(0, 0) += rr * rr / 12.0;
    variance(1, 1) += vr * vr / 12.0;
    const double cr = z[0] / rr, cd = map.axes.rate_bins / 2.0 - z[1] / vr;
    const int pr = std::max(1u, map.psf_range_halfwidth_bins);
    const int pd = std::max(1u, map.psf_doppler_halfwidth_bins);
    if (!std::isfinite(cr + cd) || cr < pr || cd < pd
        || cr + pr >= map.axes.range_bins || cd + pd >= map.axes.rate_bins) return result;
    // Six standard deviations bound local numerical quadrature, not a detection gate.
    const double r_extent = pr + std::ceil(6.0 * std::sqrt(variance(0, 0)) / rr);
    const double d_extent = pd + std::ceil(6.0 * std::sqrt(variance(1, 1)) / vr);
    if (!std::isfinite(r_extent + d_extent)
        || (4.0 * r_extent + 1) * (4.0 * d_extent + 1) > maximum_cells)
      return result; // broad geometry is unavailable; never scan the whole map
    const int ar = static_cast<int>(r_extent), ad = static_cast<int>(d_extent);
    const int centre_r = std::lround(cr), centre_d = std::lround(cd);
    std::vector<double> background;
    struct Cell { double value, weight, explained; };
    std::vector<Cell> signal;
    const Matrix weight = inverse(variance);
    struct ExplanationSupport { std::array<double, 2> z; Matrix weight; };
    std::vector<ExplanationSupport> supports;
    for (const auto& explanation : explanations) {
      Matrix support = explanation.covariance;
      support(0, 0) += pr * pr * rr * rr;
      support(1, 1) += pd * pd * vr * vr;
      supports.push_back({explanation.measurement, inverse(support)});
    }
    // Non-periodic search boundaries: aliased/unobserved edge cells provide no evidence.
    for (int r = centre_r - 2 * ar; r <= centre_r + 2 * ar; ++r)
      for (int d = centre_d - 2 * ad; d <= centre_d + 2 * ad; ++d) {
        if (r < 0 || r >= static_cast<int>(map.axes.range_bins)
            || d < 0 || d >= static_cast<int>(map.axes.rate_bins)) continue;
        ++result.lookups;
        const double value = map.values[static_cast<size_t>(r) * map.axes.rate_bins + d];
        if (!(std::isfinite(value) && value >= 0.0)) return result;
        if (std::abs(r - cr) <= ar && std::abs(d - cd) <= ad) {
          const double q = quadratic({(r - cr) * rr, -(d - cd) * vr}, weight);
          double explained = 0.0;
          for (const auto& support : supports) {
            const double distance = quadratic({r * rr - support.z[0],
                (map.axes.rate_bins / 2.0 - d) * vr - support.z[1]}, support.weight);
            // Maximum, not a product of correlated alternative explanations.
            // Use the same finite six-sigma quadrature support as the local
            // query. An infinitesimal Gaussian tail is not a remote echo owner.
            if (distance <= 36.0)
              explained = std::max(explained, std::exp(-0.5 * std::max(0.0, distance)));
          }
          signal.push_back({value, -0.5 * q, explained});
        } else background.push_back(value);
      }
    if (signal.empty() || background.size() < 16) return result;
    const double noise = sample_median(background) / std::log(2.0);
    if (!(noise > 0.0)) return result;
    // p0(y)=exp(-y), p1(y)=(1+y)^-2: a normalized heavy-tail alternative.
    // This is a declared evidence model, not a calibrated target probability.
    auto integrated = [&](bool condition) {
      double numerator_max = -INFINITY, denominator_max = -INFINITY;
      auto evidence = [&](const Cell& cell) {
        const double y = cell.value / noise;
        const double raw = y - 2.0 * std::log1p(y);
        return condition ? unexplained_component_evidence(raw, cell.explained) : raw;
      };
      for (const auto& cell : signal) {
        numerator_max = std::max(numerator_max, cell.weight + evidence(cell));
        denominator_max = std::max(denominator_max, cell.weight);
      }
      double numerator = 0.0, denominator = 0.0;
      for (const auto& cell : signal) {
        numerator += std::exp(cell.weight + evidence(cell) - numerator_max);
        denominator += std::exp(cell.weight - denominator_max);
      }
      return numerator_max + std::log(numerator) - denominator_max - std::log(denominator);
    };
    result.unconditioned_score = integrated(false);
    result.score = explanations.empty() ? result.unconditioned_score : integrated(true);
    // An existing object explaining a positive peak is ambiguity, not measured
    // absence of a second unresolved object. Do not manufacture a visibility miss.
    if (!explanations.empty() && result.unconditioned_score > 0.0)
      result.score = std::max(0.0, result.score);
    // Measured aperture quality and projected uncertainty, no class/GT visibility.
    const double uncertainty = 1.0 / std::sqrt(
        (1.0 + variance(0, 0) / (pr * pr * rr * rr))
        * (1.0 + variance(1, 1) / (pd * pd * vr * vr)));
    result.visibility = std::clamp(batch.occupied_bandwidth_fraction, 0.0, 1.0)
        * std::clamp(batch.valid_re_fraction, 0.0, 1.0)
        * std::clamp(batch.measured_dwell_s / SPATIAL_CPI_DURATION_S, 0.0, 1.0)
        * uncertainty;
    result.available = std::isfinite(result.score);
  } catch (...) {}
  return result;
}

struct MultistaticImmTracker::EvidenceHistory {
  uint64_t next_family = 1, next_branch = uint64_t{1} << 63;
  std::map<std::tuple<uint64_t,uint64_t,uint64_t>, CausalModelPreference> model_preferences;
  ComponentOwnership owners;
  std::map<std::pair<uint64_t,uint64_t>, DuplicateEvidence> pairs;
  std::deque<TimedHardObservation> seeds;
  uint64_t sequence = 0;
  bool have_sequence = false;
  std::array<double, 4> last_acquisition{{-INFINITY, -INFINITY, -INFINITY, -INFINITY}};
};

void MultistaticImmTracker::update_evidence(
    double time, const std::vector<ReceiverDetectionBatch>& batches, uint64_t sequence,
    double processing_budget_s, const std::vector<UlDifferentialReceiverBatch>& ul_batches)
{
  using Clock = std::chrono::steady_clock;
  const auto started = Clock::now();
  if (!std::isfinite(time) || (have_time_ && time < time_s_)
      || !std::isfinite(processing_budget_s) || processing_budget_s < 0.0
      || processing_budget_s > SPATIAL_CPI_DURATION_S)
    throw std::invalid_argument("invalid evidence acquisition time/budget");
  if (evidence_history_->have_sequence && sequence <= evidence_history_->sequence)
    throw std::invalid_argument("duplicate/out-of-order evidence CPI");
  std::set<uint32_t> seen;
  for (const auto& batch : batches) {
    batch.geometry.validate();
    if (batch.receiver_index >= geometries_.size() || !seen.insert(batch.receiver_index).second
        || !std::isfinite(batch.time_offset_s) || batch.time_offset_s > 0.0
        || -batch.time_offset_s > config_.evidence_window_s
        || time + batch.time_offset_s <= evidence_history_->last_acquisition[batch.receiver_index]
        || norm(batch.geometry.tx - geometries_[batch.receiver_index].tx) > 1e-6
        || norm(batch.geometry.rx - geometries_[batch.receiver_index].rx) > 1e-6)
      throw std::invalid_argument("invalid receiver acquisition/geometry");
  }
  if (have_time_ && time - time_s_ > config_.maximum_propagation_s) reset();
  const double dt = have_time_ ? time - time_s_ : SPATIAL_CPI_DURATION_S;
  time_s_ = time; have_time_ = true;
  evidence_history_->have_sequence = true; evidence_history_->sequence = sequence;
  for (const auto& batch : batches)
    evidence_history_->last_acquisition[batch.receiver_index] = time + batch.time_offset_s;
  last_processing_stats_ = {};
  last_processing_stats_.processing_budget_s = processing_budget_s;
  const bool asynchronous = config_.evidence_mode >= 2;
  const bool soft_enabled = config_.evidence_mode >= 3;
  const bool visibility_enabled = config_.evidence_mode >= 4;
  const bool ul_enabled = config_.evidence_mode >= 5;
  const bool origin_enabled = config_.evidence_mode >= 6;
  std::vector<std::vector<Observation>> observations(geometries_.size());
  for (const auto& batch : batches) {
    // OUR ADAPTATION: tag_micro_doppler_families() (clean_detector.cc) groups same-range
    // components by CLEAN score, so its untagged "anchor" is whichever member reflects strongest.
    // For an object whose own bulk motion dominates its return (e.g. a bike/pedestrian) that is
    // already the representative we want, and it is also the most FRAME-STABLE choice (the
    // strongest reflector tends to stay the strongest one CPI to CPI), so it must not be replaced
    // gratuitously. But a rotating part (e.g. a rotor blade tip) can reflect strongly enough to win
    // "highest score" while its instantaneous rate is definitionally not the object's own bulk
    // motion: no real target in this configuration travels faster than
    // config_.maximum_target_speed_mps (an existing, non-tuned bound, not a new threshold). Only
    // when the anchor's own rate exceeds that bound is it swapped for the least Doppler-extreme
    // member of its family, which is the best available stand-in for the bulk motion. Only fold
    // when the family's untagged anchor actually survived as an observation this CPI -- a tag whose
    // anchor was separately filtered out below still stands on its own.
    std::map<uint32_t, size_t> anchor_index_by_family;
    for (size_t index = 0; index < batch.detections.size(); ++index) {
      const auto& detection = batch.detections[index];
      if (detection.bulk_component_iteration < 0)
        anchor_index_by_family[detection.source_component_iteration] = index;
    }
    std::map<uint32_t, size_t> kept_index_by_family;
    for (const auto& [family, anchor_index] : anchor_index_by_family) {
      size_t best_index = anchor_index;
      if (std::abs(batch.detections[anchor_index].range_rate_mps) > config_.maximum_target_speed_mps) {
        double best_rate = std::abs(batch.detections[anchor_index].range_rate_mps);
        for (size_t index = 0; index < batch.detections.size(); ++index) {
          const auto& detection = batch.detections[index];
          if (detection.bulk_component_iteration != static_cast<int32_t>(family)) continue;
          const double rate = std::abs(detection.range_rate_mps);
          if (rate < best_rate) { best_rate = rate; best_index = index; }
        }
      }
      kept_index_by_family[family] = best_index;
    }
    for (size_t index = 0; index < batch.detections.size(); ++index) {
      const auto& detection = batch.detections[index];
      if (!std::isfinite(detection.range_m) || !std::isfinite(detection.range_rate_mps)
          || detection.range_m < 0.0 || detection.range_m > config_.maximum_range_m
          || std::abs(detection.range_rate_mps) > 2.0 * config_.maximum_target_speed_mps)
        continue;
      const bool tagged = detection.bulk_component_iteration >= 0;
      if (tagged) ++last_processing_stats_.micro_doppler_sideband_candidates;
      const uint32_t family = tagged ? static_cast<uint32_t>(detection.bulk_component_iteration)
                                     : detection.source_component_iteration;
      const auto anchor_it = anchor_index_by_family.find(family);
      if (anchor_it != anchor_index_by_family.end() && kept_index_by_family.at(family) != index) {
        ++last_processing_stats_.micro_doppler_sideband_suppressed;
        continue;
      }
      const auto quality = measurement_quality(detection, batch.range_resolution_m,
          batch.rate_resolution_mps, config_.maximum_range_m, config_.maximum_target_speed_mps);
      observations[batch.receiver_index].push_back({batch.receiver_index, index, batch.geometry,
          detection, quality.noise, quality.range_informative, quality.rate_informative, batch.time_offset_s});
    }
  }
  for (auto& track : tracks_) track.predict(time);
  std::vector<std::vector<Observation>> assigned(tracks_.size());
  std::vector<std::array<int64_t,4>> preferred(tracks_.size(), {{-1,-1,-1,-1}});
  std::vector<bool> duplicate(tracks_.size(), false);
  std::set<uint64_t> redundant_ids;
  std::set<std::pair<uint32_t, size_t>> used;
  uint64_t hypotheses = 0;
  for (const auto& rx : observations) hypotheses += rx.size() * tracks_.size();
  const double gate = adaptive_chi2_2d_gate(config_.false_object_intensity_per_s,
                                          std::max(dt, 1e-9), hypotheses);
  last_processing_stats_.association_hypotheses = hypotheses;
  for (const auto& batch : batches) {
    const auto& rx = observations[batch.receiver_index];
    if (config_.lifecycle_features & grouped_birth) {
      // One assignment row per explanation family: alternative roots see the
      // SAME selected component, not mutually exclusive pieces of its RF data.
      std::map<std::pair<bool,uint64_t>,std::vector<size_t>> groups;
      for (size_t t=0;t<tracks_.size();++t)
        groups[{bool(tracks_[t].birth_family), tracks_[t].birth_family ? tracks_[t].birth_family : tracks_[t].id}].push_back(t);
      std::vector<std::vector<size_t>> rows;
      for (const auto& item:groups) rows.push_back(item.second);
      Matrix costs(rows.size(),rx.size(),gate+1.0);
      for(size_t row=0;row<rows.size();++row) for(size_t t:rows[row]) {
        double best=gate;
        for(size_t i=0;i<rx.size();++i) {
          auto observation=rx[i];
          if(explanation_bundle) observation.noise=observation.noise+tracks_[t].object_discrepancy[batch.receiver_index];
          const double nis=tracks_[t].innovation_nis(observation);
          costs(row,i)=std::min(costs(row,i),nis);
          if(nis<best) {best=nis;preferred[t][batch.receiver_index]=i;}
        }
      }
      for(const auto& [row,i]:minimum_cost_assignment(costs))
        if(std::isfinite(costs(row,i)) && costs(row,i)<=gate)
          for(size_t t:rows[row]) assigned[t].push_back(rx[i]);
      continue;
    }
    Matrix costs(tracks_.size(), rx.size(), gate + 1.0);
    for (size_t t = 0; t < tracks_.size(); ++t) {
      double best = gate;
      for (size_t i = 0; i < rx.size(); ++i) {
        Observation observation = rx[i];
        if (explanation_bundle)
          observation.noise = observation.noise + tracks_[t].object_discrepancy[batch.receiver_index];
        costs(t, i) = tracks_[t].innovation_nis(observation);
        if (costs(t, i) < best) {
          best = costs(t, i); preferred[t][batch.receiver_index] = static_cast<int64_t>(i);
        }
      }
    }
    for (const auto& [t, i] : minimum_cost_assignment(costs))
      if (std::isfinite(costs(t, i)) && costs(t, i) <= gate) {
        assigned[t].push_back(rx[i]);
      }
  }
  if (explanation_bundle) {
    // Shared keys refer to the actual detector component in this acquisition,
    // never to a Cartesian distance or a lineage number reused in another CPI.
    for (size_t a = 0; a < tracks_.size(); ++a) for (size_t b = a+1; b < tracks_.size(); ++b) {
      if (duplicate[a] || duplicate[b] || tracks_[a].birth_family || tracks_[b].birth_family) continue;
      uint32_t shared = 0;
      bool distinct = false;
      for (size_t rx = 0; rx < 4; ++rx)
        if (preferred[a][rx] >= 0 && preferred[a][rx] == preferred[b][rx]) shared |= 1u << rx;
      for (size_t t : {a,b}) for (const auto& observation : assigned[t]) {
        const auto rx = observation.receiver;
        if (!(shared & (1u << rx))) { distinct = true; continue; }
        const auto& common = observations[rx][preferred[a][rx]];
        if (observation.detection != common.detection) distinct = true;
      }
      auto& history = evidence_history_->pairs[{tracks_[a].id, tracks_[b].id}];
      if (!history.observe(time, shared, distinct, config_.evidence_window_s,
                           config_.maximum_propagation_s)) continue;
      // Preserve an established identity, otherwise the older candidate. Never
      // sum existence evidence or fuse correlated state covariances.
      size_t owner = a, redundant = b;
      if ((tracks_[b].confirmed && !tracks_[a].confirmed)
          || (tracks_[a].confirmed == tracks_[b].confirmed && tracks_[b].id < tracks_[a].id))
        std::swap(owner, redundant);
      duplicate[redundant] = true;
      redundant_ids.insert(tracks_[redundant].id);
      for (const auto& observation : assigned[redundant]) {
        const bool occupied = std::any_of(assigned[owner].begin(), assigned[owner].end(),
          [&](const auto& item){ return item.receiver == observation.receiver; });
        if (!occupied) assigned[owner].push_back(observation);
      }
      assigned[redundant].clear();
      ++last_processing_stats_.duplicate_consolidations;
    }
  }
  // Freeze all owner explanations before any track is updated. An unconfirmed
  // soft-only candidate cannot invent an explanation that suppresses its peers.
  std::vector<std::vector<std::pair<uint64_t,MapExplanation>>> explanations(4);
  if (explanation_bundle) for (size_t t = 0; t < tracks_.size(); ++t) {
    if (duplicate[t] || !tracks_[t].confirmed || tracks_[t].associated_object_path) continue;
    for (const auto& observation : assigned[t]) {
      MapExplanation item;
      item.measurement = {observation.value.range_m, observation.value.range_rate_mps};
      item.covariance = observation.noise;
      explanations[observation.receiver].push_back({tracks_[t].id, item});
    }
  }
  std::map<uint64_t, std::vector<UlDifferentialReceiverBatch>> sessions;
  if (ul_enabled) for (const auto& batch : ul_batches)
    if (batch.session_id) sessions[batch.session_id].push_back(batch);
  for (auto it = sessions.begin(); it != sessions.end();) {
    const auto& data = it->second;
    bool valid = !data.empty() && data.front().allocation_support_id;
    std::set<uint32_t> receivers;
    for (const auto& batch : data)
      valid = valid && receivers.insert(batch.receiver_index).second
          && batch.allocation_support_id == data.front().allocation_support_id;
    if (!valid) it = sessions.erase(it);
    else { ul_session_last_seen_s_[it->first] = time; ++it; }
  }
  if (getenv("ISAC_DEBUG_UL")) {
    std::fprintf(stderr, "UL t=%.3f valid_sessions=%zu\n", time, sessions.size());
    for (const auto& [session, data] : sessions) {
      const auto validator = ul_validators_.find(session);
      std::fprintf(stderr, "  session=%llu batches=%zu has_validator=%d has_causal=%d has_provisional=%d\n",
                   (unsigned long long)session, data.size(), validator != ul_validators_.end(),
                   validator != ul_validators_.end() && validator->second.has_causal_ue_state(),
                   validator != ul_validators_.end() && validator->second.has_provisional_ue_state());
    }
  }
  for (auto it = ul_session_last_seen_s_.begin(); it != ul_session_last_seen_s_.end();) {
    if (time - it->second > config_.maximum_propagation_s) {
      ul_validators_.erase(it->first); it = ul_session_last_seen_s_.erase(it);
    } else ++it;
  }
  const auto lifecycle_started=Clock::now();
  struct ObjectAlternative { uint64_t parent,plane; double score,gain; };
  std::vector<std::vector<ObjectAlternative>> object_alternatives(tracks_.size());
  if(config_.lifecycle_features & (object_path_grouping|explanatory_retirement)) {
    // All parent/child states, admitted planes and UE sessions are frozen before
    // this acquisition updates any of them. Compare identical hard components.
    auto likelihood=[&](const Track& model,const std::vector<Observation>& data,const ReflectorPlane* plane) -> double {
      std::vector<double> scores;
      for(const auto& sample:data) {
        auto state=model.target_state();
        Matrix propagation=Matrix::identity(6);
        for(size_t axis=0;axis<3;++axis) {
          state[axis]+=sample.time_offset_s*state[axis+3];
          propagation(axis,axis+3)=sample.time_offset_s;
        }
        auto observation=sample;
        if(explanation_bundle) observation.noise=observation.noise+model.object_discrepancy[sample.receiver];
        observation=with_reflector_uncertainty(observation,state,plane);
        const auto evidence=target_hard_proposal_evidence(state,
            propagation*model.target_covariance()*propagation.transposed(),{observation},config_,plane);
        if(evidence.available) scores.push_back(evidence.log_bayes_factor
            -std::log(std::max<size_t>(1,observations[sample.receiver].size())));
      }
      if(scores.size()<3) return -INFINITY;
      double score=std::accumulate(scores.begin(),scores.end(),0.0)/scores.size();
      std::vector<double> ul;
      for(const auto& [session,data]:sessions) {
        const auto validator=ul_validators_.find(session);
        if(validator==ul_validators_.end() || !validator->second.has_causal_ue_state()) continue;
        const auto evidence=plane ? validator->second.evaluate_reflected(time,model.target_state(),model.target_covariance(),data,*plane)
            : validator->second.evaluate(time,model.target_state(),model.target_covariance(),data);
        if(evidence.decision!=UlDifferentialDecision::unavailable) ul.push_back(evidence.log_bayes_factor);
      }
      if(!ul.empty()) score+=sample_median(ul);
      return score;
    };
    for(size_t child=0;child<tracks_.size();++child) {
      if(duplicate[child] || assigned[child].size()<3) continue;
      const double independent=likelihood(tracks_[child],assigned[child],nullptr);
      if(!std::isfinite(independent)) continue;
      for(size_t parent=0;parent<tracks_.size();++parent) {
        const auto& object=tracks_[parent];
        // Older established object is an acyclic anchor, not a distance merge.
        if(duplicate[parent] || !object.confirmed || object.birth_family || object.associated_object_path
            || object.id>=tracks_[child].id) continue;
        std::vector<const ReflectorPlane*> models{nullptr};
        for(const auto& plane:reflector_bank_.planes()) if(plane.admitted) models.push_back(&plane);
        for(const auto* plane:models) {
          ++last_processing_stats_.object_path_models_tested;
          const double score=likelihood(object,assigned[child],plane);
          if(!std::isfinite(score)) continue;
          object_alternatives[child].push_back({object.id,plane?plane->id:0,score,score-independent});
          if(config_.lifecycle_features & object_path_grouping)
            evidence_history_->model_preferences[{object.id,tracks_[child].id,plane?plane->id:0}]
                .observe(time,score-independent,config_.evidence_window_s);
        }
      }
    }
  }
  std::vector<ExistenceEvidence::Epoch> pending_epochs(tracks_.size());
#ifdef NR_ISAC_LIFECYCLE_TRACE
  std::vector<std::array<double,6>> pending_prior_states(tracks_.size());
#endif
  for (size_t index = 0; index < tracks_.size(); ++index) {
    auto& track = tracks_[index];
    if (duplicate[index]) continue;
    const bool previously_confirmed = track.confirmed;
    // Model errors learned below may only affect the NEXT acquisition.
    const auto frozen_state = track.state;
    const auto frozen_covariance = track.covariance;
    auto scoring_assignments = assigned[index];
    if (explanation_bundle) for (auto& observation : scoring_assignments)
      observation.noise = observation.noise + track.object_discrepancy[observation.receiver];
    ExistenceEvidence::Epoch epoch;
    epoch.time_s = time; epoch.sequence = sequence;
    std::vector<double> dl_scores;
    for (const auto& batch : batches) {
      auto state = track.target_state();
      Matrix propagation = Matrix::identity(6);
      for (size_t axis = 0; axis < 3; ++axis) {
        state[axis] += batch.time_offset_s * state[axis + 3];
        propagation(axis, axis + 3) = batch.time_offset_s;
      }
      const Matrix covariance = propagation * track.target_covariance() * propagation.transposed();
      std::vector<Observation> associated;
      for (const auto& assignment : scoring_assignments)
        if (assignment.receiver == batch.receiver_index) associated.push_back(assignment);
      // Hard detection and map describe the SAME RF data. Use the hard likelihood when assigned;
      // consult the soft map only in its absence. Seed observations are never scored retrospectively.
      if (!associated.empty()) {
        auto evidence = target_hard_proposal_evidence(state, covariance, associated, config_, nullptr);
        if (evidence.available) {
          // Association selected among all receiver proposals, not just the winning one.
          evidence.log_bayes_factor -= std::log(std::max<size_t>(1, observations[batch.receiver_index].size()));
          dl_scores.push_back(evidence.log_bayes_factor);
          if (evidence.log_bayes_factor > 0.0) epoch.rx_mask |= 1u << batch.receiver_index;
        }
      } else if (soft_enabled) {
        std::vector<MapExplanation> alternatives;
        for (const auto& [owner, explanation] : explanations[batch.receiver_index])
          if (owner != track.id) alternatives.push_back(explanation);
        const auto evidence = local_map_evidence(batch, state, covariance,
            config_.maximum_local_map_cells, alternatives);
        if (!alternatives.empty()) ++last_processing_stats_.conditioned_soft_queries;
        if (evidence.available && evidence.score < evidence.unconditioned_score)
          ++last_processing_stats_.soft_explanation_reductions;
        last_processing_stats_.local_map_cells += evidence.lookups;
        ++last_processing_stats_.soft_map_queries;
        if (!evidence.available) continue;
        double score = evidence.score;
        if (score < 0.0) {
          if (!visibility_enabled) continue;
          score = std::log1p(evidence.visibility * std::expm1(score));
          if (score < 0.0) ++last_processing_stats_.visibility_negative_epochs;
        } else {
          // Stable log(1-q+q*exp(score)); an unobservable map contributes no support.
          score = evidence.visibility > 0.0
              ? score + std::log(evidence.visibility + (1.0 - evidence.visibility) * std::exp(-score))
              : 0.0;
        }
        dl_scores.push_back(score);
        if (score > 0.0) {
          epoch.rx_mask |= 1u << batch.receiver_index;
          epoch.soft_rx_mask |= 1u << batch.receiver_index;
          ++last_processing_stats_.soft_supported_epochs;
        }
      }
    }
    // Arithmetic mean of evidence is deliberately conservative for correlated RX observations.
    if (!dl_scores.empty()) epoch.dl = std::accumulate(dl_scores.begin(), dl_scores.end(), 0.0) / dl_scores.size();
    if (getenv("ISAC_DEBUG_EPOCH")) {
      std::fprintf(stderr, "EPOCH t=%.3f id=%llu dl_scores=[", time, (unsigned long long)track.id);
      for (double v : dl_scores) std::fprintf(stderr, "%.3f,", v);
      std::fprintf(stderr, "] dl=%.3f n_assoc=%zu\n", epoch.dl, dl_scores.size());
    }
    const auto ul_started = Clock::now();
    std::vector<double> ul_scores;
    for (const auto& [session, data] : sessions) {
      const auto validator = ul_validators_.find(session);
      if (validator == ul_validators_.end() || !validator->second.has_causal_ue_state()) continue;
      const auto evidence = validator->second.evaluate(time, track.target_state(), track.target_covariance(), data);
#ifdef NR_ISAC_LIFECYCLE_TRACE
      ul_probe::validation(sequence,track.id,session,evidence);
#endif
      ++last_processing_stats_.ul_track_validation_attempts;
      if (evidence.decision != UlDifferentialDecision::unavailable)
        ul_scores.push_back(evidence.log_bayes_factor);
    }
    // EXPERIMENTAL, OPT-IN (config_.ue_position_known): score this track against the UL returns
    // under the assumed-known UE illuminator whenever the anchor-gated validator above produced
    // nothing -- which, measured on these captures, is always, because propose_shared_update needs
    // a confirmed DL anchor that a broken track never becomes.
    //
    // This exists because a track that has already diverged in elevation is structurally unable to
    // accept the correction that would fix it: measured on case_01, a track sitting at z~30 m
    // predicts a UL excess range of 52-58 m while the true target's return is at 0.8-3.1 m, roughly
    // 17 range cells out, so every correct measurement fails its association gate and the wrong
    // track survives untouched and outlives the right ones. Scoring the track against the UL
    // returns it CANNOT explain converts that same disagreement into negative existence evidence,
    // so the already-validated existence retirement path removes it. Deliberately no new threshold
    // or streak counter: this reuses target_hard_proposal_evidence (the identical scorer the DL
    // epoch uses) and the existing retirement rule.
    if (ul_scores.empty() && config_.ue_position_known) {
      const Vec3 ue{config_.ue_position[0], config_.ue_position[1], config_.ue_position[2]};
      std::vector<Observation> ul_observations;
      for (const auto& ul_batch : ul_batches) {
        if (ul_batch.receiver_index >= geometries_.size() || ul_batch.detections.empty()) continue;
        if (!(ul_batch.range_resolution_m > 0.0) || !(ul_batch.rate_resolution_mps > 0.0)) continue;
        const BistaticGeometry ul_geometry{ue, ul_batch.receiver_position};
        for (const auto& detection : ul_batch.detections) {
          const auto quality = measurement_quality(detection, ul_batch.range_resolution_m,
              ul_batch.rate_resolution_mps, config_.maximum_range_m, config_.maximum_target_speed_mps);
          if (!quality.range_informative && !quality.rate_informative) continue;
          ul_observations.push_back({ul_batch.receiver_index, 0, ul_geometry, detection,
                                     quality.noise, quality.range_informative,
                                     quality.rate_informative, ul_batch.target_time_offset_s});
        }
      }
      if (!ul_observations.empty()) {
        const auto evidence = target_hard_proposal_evidence(track.target_state(),
            track.target_covariance(), ul_observations, config_, nullptr);
        if (evidence.available) {
          // NO extra multiplicity subtraction here, unlike the DL epoch above. That difference is
          // deliberate and load-bearing: the DL epoch passes only the ASSOCIATED subset (usually a
          // single detection), so its -log(pool) term accounts for a selection effect the scorer
          // cannot see. This path passes the WHOLE UL pool as proposals, and the scorer already
          // charges exactly that multiplicity internally via
          // log_clutter_density = log(proposals.size()) - log(support_area). Subtracting it again
          // charged ~1.8 nats per CPI at the measured median of 6 UL candidates, which dragged
          // well-corroborated tracks negative until existence retired them -- it killed bike's
          // correct 131-state track at t=4.2s. Do not reinstate it.
          epoch.ul = evidence.log_bayes_factor;
          ++last_processing_stats_.ul_ue_geometry_validations;
        }
      }
    }
    if (!ul_scores.empty()) epoch.ul = sample_median(ul_scores);
    if (epoch.ul < 0.0) ++last_processing_stats_.ul_contradicted_track_updates;
    if (epoch.ul > 0.0) ++last_processing_stats_.ul_supported_track_updates;
    last_processing_stats_.ul_elapsed_s += std::chrono::duration<double>(Clock::now() - ul_started).count();
    if (origin_enabled) {
      std::vector<double> reflected_models;
      for (const auto& plane : reflector_bank_.planes()) {
        if (!plane.admitted) continue;
        std::vector<double> reflected;
        for (const auto& batch : batches) {
          std::vector<Observation> same_measurements;
          for (const auto& observation : scoring_assignments)
            if (observation.receiver == batch.receiver_index) same_measurements.push_back(observation);
          auto state = track.target_state();
          Matrix propagation = Matrix::identity(6);
          for (size_t axis = 0; axis < 3; ++axis) {
            state[axis] += batch.time_offset_s * state[axis + 3];
            propagation(axis, axis + 3) = batch.time_offset_s;
          }
          auto evidence = target_hard_proposal_evidence(state,
              propagation * track.target_covariance() * propagation.transposed(),
              same_measurements, config_, &plane);
          if (evidence.available)
            evidence.log_bayes_factor -= std::log(std::max<size_t>(1, observations[batch.receiver_index].size()));
          if (evidence.available) reflected.push_back(evidence.log_bayes_factor);
        }
        if (reflected.empty()) continue;
        double reflected_score = std::accumulate(reflected.begin(), reflected.end(), 0.0) / reflected.size();
        const auto reflected_ul_started = Clock::now();
        std::vector<double> reflected_ul;
        for (const auto& [session, data] : sessions) {
          const auto validator = ul_validators_.find(session);
          if (validator == ul_validators_.end() || !validator->second.has_causal_ue_state()) continue;
          const auto evidence = validator->second.evaluate_reflected(
              time, track.target_state(), track.target_covariance(), data, plane);
          if (evidence.decision != UlDifferentialDecision::unavailable)
            reflected_ul.push_back(evidence.log_bayes_factor);
        }
        if (!reflected_ul.empty()) reflected_score += sample_median(reflected_ul);
        reflected_models.push_back(reflected_score);
        last_processing_stats_.ul_elapsed_s += std::chrono::duration<double>(Clock::now() - reflected_ul_started).count();
      }
      if (!reflected_models.empty()) {
        // Competing clutter/reflection density, not a second copy of direct evidence.
        // Average admitted models so plane multiplicity does not multiply support.
        const double largest = *std::max_element(reflected_models.begin(), reflected_models.end());
        double total = 0.0;
        for (double score : reflected_models) total += std::exp(score - largest);
        const double log_reflection = largest + std::log(total / reflected_models.size());
        epoch.origin = -(std::max(0.0, log_reflection) + std::log1p(std::exp(-std::abs(log_reflection))));
      }
    }
    if(config_.lifecycle_features & explanatory_retirement) {
      std::vector<double> alternatives;
      for(const auto& model:object_alternatives[index]) alternatives.push_back(model.score);
      if(!alternatives.empty()) {
        const double penalty=-log_density_union(alternatives);
        // Do not double-charge the existing own-state reflector null model.
        if(penalty<epoch.origin) {epoch.origin=penalty; ++last_processing_stats_.explanation_negative_epochs;}
      }
    }
    if(track.birth_family) {
      pending_epochs[index]=epoch;
#ifdef NR_ISAC_LIFECYCLE_TRACE
      pending_prior_states[index]=track.target_state();
#endif
    }
    else {
#ifdef NR_ISAC_LIFECYCLE_TRACE
      ul_probe::epoch(track.id,track.target_state(),track.existence,epoch);
#endif
      track.existence.observe(epoch);
    }
    track.confirmed = !track.birth_family && track.existence.reportable();
    track.existence_log_evidence = track.existence.score();
    track.confirmation_log_threshold = track.existence.threshold();
    // The unchanged IMM/EKF maintains both a tentative prediction and confirmed kinematics.
    for (const auto& observation : scoring_assignments)
      if (track.update_measurement(observation)) {
        used.emplace(observation.receiver, observation.detection);
        if (explanation_bundle) evidence_history_->owners.remember(sequence, observation.receiver,
            observation.detection, observation.value.component_lineage, time + observation.time_offset_s, track.id);
      }
    // EXPERIMENTAL, OPT-IN (config_.ue_position_known, default false): continuous UL correction.
    // Measured cause of the elevation drift on ground targets: they are updated by only 1-2
    // receivers per CPI and get no measurement at all in ~48% of CPIs (drone: 3-4 receivers, 18%
    // unmeasured), so the update is underdetermined against 3 position unknowns and z wanders under
    // the motion model. The UL differential measurement has the SAME algebraic form as the DL one
    // with the UE substituted for the gNB as illuminator, so it needs no new model or Jacobian --
    // only a BistaticGeometry built on the assumed-known UE position. Adding every available UL
    // receiver roughly doubles the equations per CPI and, measured on this geometry, improves the
    // combined position conditioning about threefold versus either leg alone. These are physically
    // independent paths from the DL returns (different illuminator), so they are applied as an
    // ordinary sequential EKF update rather than competing with DL in the association assignment.
    if (config_.ue_position_known) {
      const Vec3 ue{config_.ue_position[0], config_.ue_position[1], config_.ue_position[2]};
      for (const auto& ul_batch : ul_batches) {
        if (ul_batch.receiver_index >= geometries_.size() || ul_batch.detections.empty()) continue;
        if (!(ul_batch.range_resolution_m > 0.0) || !(ul_batch.rate_resolution_mps > 0.0)) continue;
        const BistaticGeometry ul_geometry{ue, ul_batch.receiver_position};
        // One correction per receiver: the best-gating UL return, never several alternatives for
        // the same receiver, mirroring how DL assigns at most one detection per receiver.
        const Detection* best = nullptr;
        double best_nis = gate;
        for (const auto& detection : ul_batch.detections) {
          const auto quality = measurement_quality(detection, ul_batch.range_resolution_m,
              ul_batch.rate_resolution_mps, config_.maximum_range_m, config_.maximum_target_speed_mps);
          if (!quality.range_informative && !quality.rate_informative) continue;
          Observation candidate{ul_batch.receiver_index, 0, ul_geometry, detection, quality.noise,
                                quality.range_informative, quality.rate_informative,
                                ul_batch.target_time_offset_s};
          const double nis = track.innovation_nis(candidate);
          if (std::isfinite(nis) && nis < best_nis) { best_nis = nis; best = &detection; }
        }
        if (!best) continue;
        const auto quality = measurement_quality(*best, ul_batch.range_resolution_m,
            ul_batch.rate_resolution_mps, config_.maximum_range_m, config_.maximum_target_speed_mps);
        Observation correction{ul_batch.receiver_index, 0, ul_geometry, *best, quality.noise,
                               quality.range_informative, quality.rate_informative,
                               ul_batch.target_time_offset_s};
        if (track.update_measurement(correction)) ++last_processing_stats_.ul_track_corrections;
      }
    }
    if (explanation_bundle && previously_confirmed && assigned[index].size() >= 3) {
      const double learning = -std::expm1(-std::max(0.0, dt) / config_.evidence_window_s);
      for (const auto& observation : assigned[index]) {
        if (!used.count({observation.receiver, observation.detection})) continue;
        const auto predicted = acquisition_model(frozen_state, observation);
        const Matrix h = acquisition_jacobian(frozen_state, observation);
        const Matrix expected = h * frozen_covariance * h.transposed() + observation.noise;
        const std::vector<double> residual{observation.value.range_m-predicted[0],
                                         observation.value.range_rate_mps-predicted[1]};
        // Learn only innovations credible under detector + state uncertainty,
        // not ones admitted solely because discrepancy was previously enlarged.
        if (quadratic(residual, inverse(expected)) > gate) continue;
        Matrix excess(2,2);
        for (size_t r=0;r<2;++r) for(size_t c=0;c<2;++c)
          excess(r,c) = residual[r]*residual[c] - expected(r,c);
        auto& running = track.residual_excess[observation.receiver];
        running = (1.0-learning)*running + learning*excess;
        track.object_discrepancy[observation.receiver] = positive_semidefinite(symmetrized(running), 0.0);
        ++last_processing_stats_.object_discrepancy_updates;
      }
    }
    track.coasts = track.updated ? 0 : track.coasts + 1;
    if (track.updated) ++track.total_updates;
    if (track.confirmed && track.updated) ++track.confirmed_updates;
    track.source_sequence = sequence;
  }
  // Competing roots are a single object explanation family. Clutter and sibling
  // roots are alternatives, never independently confirmed physical tracks.
  std::map<uint64_t,std::vector<size_t>> families;
  for(size_t i=0;i<tracks_.size();++i) if(tracks_[i].birth_family) families[tracks_[i].birth_family].push_back(i);
  for(const auto& [family,indices]:families) {
    std::vector<double> scores;
    bool informative=false;
    for(size_t i:indices) {
      const auto& epoch=pending_epochs[i];
      scores.push_back(epoch.dl+epoch.ul+epoch.origin);
      informative=informative || scores.back()!=0.0;
    }
    size_t winner=tracks_.size();
    for(size_t branch=0;branch<indices.size();++branch) {
      auto& track=tracks_[indices[branch]];
      auto epoch=pending_epochs[indices[branch]];
      if(informative) epoch.origin+=branch_increment(branch,scores)-scores[branch];
#ifdef NR_ISAC_LIFECYCLE_TRACE
      ul_probe::epoch(track.id,pending_prior_states[indices[branch]],track.existence,epoch);
#endif
      track.existence.observe(epoch);
      track.existence_log_evidence=track.existence.score();
      ++last_processing_stats_.birth_branches_tested;
      if(track.existence.reportable() && (winner==tracks_.size() || track.existence.score()>tracks_[winner].existence.score()))
        winner=indices[branch];
    }
    if(winner!=tracks_.size()) {
      auto& track=tracks_[winner];
      const auto old_id=track.id;
      track.id=next_id_++; track.birth_family=0; track.confirmed=true;
#ifdef NR_ISAC_LIFECYCLE_TRACE
      ul_probe::family(sequence,family,old_id,track.id,track.target_state());
#endif
      track.confirmed_updates=track.updated ? 1 : 0;
      for(size_t i:indices) if(i!=winner) {duplicate[i]=true;redundant_ids.insert(tracks_[i].id);}
      // Old component keys remain reserved until their ordinary causal expiry;
      // they never become a new candidate merely because an identity was issued.
      (void)old_id;
      ++last_processing_stats_.birth_families_promoted;
    } else if(std::all_of(indices.begin(),indices.end(),[&](size_t i){return tracks_[i].existence.phase()==ExistencePhase::retired;}))
      ++last_processing_stats_.birth_families_expired;
  }
  if(config_.lifecycle_features & object_path_grouping) for(auto& track:tracks_) {
    track.associated_object_path=false; track.associated_parent_id=0; track.associated_plane_id=0;
    if(track.birth_family) continue;
    double best=track.existence.threshold();
    for(const auto& [key,history]:evidence_history_->model_preferences) {
      const auto [parent,child,plane]=key;
      if(child!=track.id || history.epochs(time,config_.evidence_window_s)<2) continue;
      const auto anchor=std::find_if(tracks_.begin(),tracks_.end(),[&](const Track& t){return t.id==parent
          && t.confirmed && !t.associated_object_path && !redundant_ids.count(t.id);});
      if(anchor==tracks_.end()) continue;
      const double gain=history.score(time,config_.evidence_window_s);
      if(gain>best) {
        best=gain;track.associated_object_path=true;
        track.associated_parent_id=parent;track.associated_plane_id=plane;
      }
    }
    if(track.associated_object_path) ++last_processing_stats_.grouped_path_epochs;
  }
  last_processing_stats_.lifecycle_elapsed_s=std::chrono::duration<double>(Clock::now()-lifecycle_started).count();
  // Learn environmental state after scoring: a newly fitted wall cannot explain the same epoch
  // used to fit it. Plane admission remains the existing multi-target, multi-CPI mechanism.
  if (origin_enabled) {
    for (size_t a = 0; a < tracks_.size(); ++a) for (size_t b = a + 1; b < tracks_.size(); ++b) {
      if (duplicate[a] || duplicate[b] || tracks_[a].birth_family || tracks_[b].birth_family) continue;
      if (assigned[a].size() < 3 || assigned[b].size() < 3) continue;
      const size_t parent = tracks_[a].existence.score() >= tracks_[b].existence.score() ? a : b;
      const size_t path = parent == a ? b : a;
      if (!tracks_[parent].confirmed || tracks_[parent].associated_object_path) continue;
      std::vector<ReflectorPathMeasurement> input;
      for (const auto& value : assigned[path]) {
        ReflectorPathMeasurement sample;
        sample.time_s = time; sample.receiver_index = value.receiver;
        sample.geometry = value.geometry; sample.detection = value.value;
        input.push_back(sample);
      }
      reflector_bank_.observe_pair(sequence, tracks_[parent].id, tracks_[path].id,
          tracks_[parent].target_state(), tracks_[path].target_state(), input);
      ++last_processing_stats_.reflector_pair_updates;
    }
  }
  // Select one already-confirmed anchor from the previous acquisition. No tentative candidate
  // gets to fit its own UE. Commit after every candidate consumed the frozen prior session state.
  const auto ul_update_started = Clock::now();
  const Track* anchor = nullptr;
  for (const auto& track : tracks_)
    if (!redundant_ids.count(track.id) && !track.associated_object_path && track.confirmed && track.confirmed_updates > 1
        && (!anchor || track.existence.score() > anchor->existence.score())) anchor = &track;
  if (getenv("ISAC_DEBUG_UL")) {
    std::fprintf(stderr, "UL-BOOTSTRAP t=%.3f anchor=%s sessions=%zu\n", time,
                 anchor ? std::to_string(anchor->id).c_str() : "none", sessions.size());
  }
  if (anchor) for (const auto& [session, data] : sessions) {
    auto& validator = ul_validators_[session];
    const auto proposal = validator.propose_shared_update(time, anchor->target_state(), anchor->target_covariance(), data);
    last_processing_stats_.ul_training_hypotheses += proposal.training_hypotheses;
    last_processing_stats_.ul_fitted_ue_states += proposal.fitted_ue_states;
    if (getenv("ISAC_DEBUG_UL")) {
      std::fprintf(stderr, "  proposal decision=%s reason=%s ue_state_valid=%d "
                   "establishment_eligible=%d direct_anchor_valid=%d\n",
                   ul_differential_decision_name(proposal.decision), proposal.reason.c_str(),
                   proposal.ue_state_valid, proposal.session_establishment_eligible,
                   proposal.direct_session_anchor_valid);
    }
    validator.commit(time, proposal);
#ifdef NR_ISAC_LIFECYCLE_TRACE
    ul_probe::bootstrap(sequence,anchor->id,session,proposal,validator.has_causal_ue_state());
#endif
  }
  last_processing_stats_.ul_elapsed_s += std::chrono::duration<double>(Clock::now() - ul_update_started).count();
  tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [&](const Track& track) {
    const bool retire = redundant_ids.count(track.id) || track.existence.phase() == ExistencePhase::retired
        || norm(Vec3{track.state[3], track.state[4], track.state[5]}) > config_.maximum_target_speed_mps;
    if (retire) ++last_processing_stats_.evidence_retirements;
    return retire;
  }), tracks_.end());
  std::set<uint64_t> active;
  for (const auto& track : tracks_) active.insert(track.id);
  reflector_bank_.retain_tracks(active);
  for(auto it=evidence_history_->model_preferences.begin();it!=evidence_history_->model_preferences.end();)
    if(!active.count(std::get<0>(it->first)) || !active.count(std::get<1>(it->first))
        || !it->second.epochs(time,config_.evidence_window_s)) it=evidence_history_->model_preferences.erase(it);
    else ++it;
  for (auto it = evidence_history_->pairs.begin(); it != evidence_history_->pairs.end();)
    if (!active.count(it->first.first) || !active.count(it->first.second))
      it = evidence_history_->pairs.erase(it);
    else ++it;
  evidence_history_->owners.prune(time, config_.evidence_window_s);

  const auto birth_started = Clock::now();
  auto& history = evidence_history_->seeds;
  const double roundoff = 16.0 * std::numeric_limits<double>::epsilon()
                          * std::max(1.0, std::abs(time));
  history.erase(std::remove_if(history.begin(), history.end(), [&](const auto& seed) {
    if (explanation_bundle && evidence_history_->owners.contains(
        seed.sequence, seed.value.receiver, seed.value.detection, seed.value.value.component_lineage)) {
      ++last_processing_stats_.owned_seed_exclusions;
      return true;
    }
    return time - seed.time_s > config_.evidence_window_s + roundoff;
  }), history.end());
  for (const auto& batch : batches)
    for (const auto& value : observations[batch.receiver_index])
      if (!used.count({value.receiver, value.detection}) && value.range_informative
          && -batch.time_offset_s <= config_.evidence_window_s)
        evidence_history_->seeds.push_back({value, time + batch.time_offset_s, sequence});
  // Three hard seed surfaces may come from DIFFERENT acquisition epochs. They only establish a
  // candidate geometry. Independent subsequent evidence is required for confirmation.
  std::array<std::vector<TimedHardObservation>, 4> by_receiver;
  for (const auto& seed : evidence_history_->seeds) by_receiver[seed.value.receiver].push_back(seed);
  for (size_t r = 0; r < 4; ++r)
    last_processing_stats_.birth_seeds_by_receiver[r] = by_receiver[r].size();
  uint64_t combinations = 0;
  for (size_t a = 0; a < 4; ++a) for (size_t b = a + 1; b < 4; ++b)
    for (size_t c = b + 1; c < 4; ++c)
      combinations += by_receiver[a].size() * by_receiver[b].size() * by_receiver[c].size();
  last_processing_stats_.birth_tuple_hypotheses = combinations;
  const double alpha = std::clamp(config_.false_object_intensity_per_s * config_.evidence_window_s, 1e-12, 0.49);
  const double threshold = config_.existence_threshold_scale * std::log(std::max<uint64_t>(2, combinations) / alpha);
  std::set<std::tuple<uint64_t, uint32_t, size_t>> reserved;
  for (size_t a = 0; a < 4; ++a) for (size_t b = a + 1; b < 4; ++b)
    for (size_t c = b + 1; c < 4; ++c)
      for (const auto& sa : by_receiver[a]) for (const auto& sb : by_receiver[b])
        for (const auto& sc : by_receiver[c]) {
          if (std::max({sa.sequence, sb.sequence, sc.sequence}) != sequence) continue;
          if (!asynchronous && (sa.sequence != sb.sequence || sa.sequence != sc.sequence)) continue;
          std::vector<TimedHardObservation> samples{sa, sb, sc};
          bool conflict = false;
          for (const auto& sample : samples)
            conflict = conflict || reserved.count({sample.sequence, sample.value.receiver, sample.value.detection});
          if (explanation_bundle) for (const auto& sample : samples)
            if (evidence_history_->owners.contains(sample.sequence, sample.value.receiver,
                sample.value.detection, sample.value.value.component_lineage)) {
              conflict = true; ++last_processing_stats_.owned_seed_exclusions;
            }
          if (conflict) continue;
          ++last_processing_stats_.birth_fit_attempts;
          std::vector<TemporalStateFit> branches;
          TemporalStateFit fit = fit_asynchronous(samples, time, config_,
              (config_.lifecycle_features & grouped_birth) ? &branches : nullptr,
              &last_processing_stats_.birth_multimodal_covariance_widenings);
          if (!fit.valid) continue;
          ++last_processing_stats_.birth_fit_valid;
          // EXPERIMENTAL, OPT-IN (config_.ue_position_known, default false -- see its declaration
          // in multistatic_imm_tracker.h): an assumed-known UE position supplies a genuinely
          // independent illuminator geometry from the gNB, so a root spurious under the DL
          // bistatic geometry is generically NOT also spurious under the UE-illuminated one. This
          // bypasses the existing UlDtdDfsValidator entirely -- that validator is anchor-gated
          // (propose_shared_update only runs once a DL track is already confirmed) and so cannot
          // help the very first birth, which is exactly where this ambiguity happens. Direct
          // counterfactual on the corrupted-receiver test that motivated the 4th-DL-receiver check
          // below: the UE-geometry margin between the correct and spurious root was ~50 m, versus a
          // few meters for the same-illuminator DL check, which is why this takes priority over it
          // when available.
          bool ue_geometry_resolved = false;
          if (config_.ue_position_known) {
            std::vector<TemporalStateFit> ue_roots;
            fit_asynchronous(samples, time, config_, &ue_roots, nullptr);
            if (ue_roots.empty()) ue_roots.push_back(fit);
            const UlDifferentialReceiverBatch* best_batch = nullptr;
            const Detection* best_detection = nullptr;
            for (const auto& batch : ul_batches) {
              if (batch.detections.empty()) continue;
              const auto& candidate = batch.detections.front();
              if (!best_detection || candidate.score > best_detection->score) {
                best_batch = &batch; best_detection = &candidate;
              }
            }
            if (best_batch) {
              const std::array<double, 6> ue_state{config_.ue_position[0], config_.ue_position[1],
                                                    config_.ue_position[2], 0.0, 0.0, 0.0};
              const double range_res = std::max(best_batch->range_resolution_m, 1e-6);
              const double rate_res = std::max(best_batch->rate_resolution_mps, 1e-6);
              const TemporalStateFit* winner = nullptr;
              double winner_cost = std::numeric_limits<double>::infinity();
              for (const auto& root : ue_roots) {
                const std::array<double, 6> state{root.position.x, root.position.y, root.position.z,
                                                  root.velocity.x, root.velocity.y, root.velocity.z};
                try {
                  const auto predicted = ul_dtd_dfs_model(state, ue_state, best_batch->receiver_position);
                  const double dr = (predicted[0] - best_detection->range_m) / range_res;
                  const double dv = (predicted[1] - best_detection->range_rate_mps) / rate_res;
                  const double cost = dr * dr + dv * dv;
                  if (std::isfinite(cost) && cost < winner_cost) { winner_cost = cost; winner = &root; }
                } catch (...) {}
              }
              if (getenv("ISAC_DEBUG_UE_GEOMETRY")) {
                std::fprintf(stderr, "UEGEO t=%.3f gate=%.3f roots=%zu winner_cost=%.3f resolved=%d\n",
                             time, gate, ue_roots.size(), winner_cost, winner && winner_cost <= gate);
              }
              if (winner && winner_cost <= gate) {
                fit = *winner;
                ++last_processing_stats_.birth_ue_geometry_disambiguations;
                ue_geometry_resolved = true;
              }
            }
          }
          // OUR ADAPTATION: algebraic_three_surface_seeds() solves a quadratic and can admit up to
          // two real roots for any one 3-receiver triple -- a genuine discrete ambiguity, not a
          // smooth ill-conditioning that local Gauss-Newton refinement can fix by itself. Direct
          // counterfactual test on this exact receiver footprint: a 0.3 m perturbation on just one
          // of the three ranges (an order of magnitude below the 3.05 m range resolution) is enough
          // to flip fit_asynchronous's own root selection -- based only on these three receivers --
          // from the true near-ground root to a root ~39 m up, near the transmitter's own height.
          // A 4th receiver's independent seed in the same evidence window is the only thing that
          // can tell these two roots apart: the true root must also satisfy it, a spurious root
          // generically will not. Re-fitting the SAME three samples with the fourth appended only
          // polishes whichever basin Gauss-Newton already started in -- it does not re-examine the
          // discrete choice made by algebraic_three_surface_seeds. So explicitly score every root
          // fit_asynchronous already found against the fourth receiver's own measurement, and let
          // that independent equation pick the winner directly, instead of only vetoing the
          // unchanged 3-receiver choice after the fact with a binary gate.
          if (!ue_geometry_resolved) {
            std::vector<TemporalStateFit> roots;
            fit_asynchronous(samples, time, config_, &roots, nullptr);
            if (roots.empty()) roots.push_back(fit);
            const TimedHardObservation* best_fourth = nullptr;
            for (size_t other = 0; other < by_receiver.size(); ++other) {
              if (other == a || other == b || other == c) continue;
              for (const auto& sample : by_receiver[other]) {
                if (std::abs(sample.time_s - time) > config_.evidence_window_s) continue;
                best_fourth = &sample;
              }
            }
            if (best_fourth) {
              const TemporalStateFit* winner = nullptr;
              double winner_chi2 = std::numeric_limits<double>::infinity();
              for (const auto& root : roots) {
                const std::vector<double> state{root.position.x, root.position.y, root.position.z,
                                                root.velocity.x, root.velocity.y, root.velocity.z};
                const double chi2 = asynchronous_cost(state, time, {*best_fourth}).first;
                if (std::isfinite(chi2) && chi2 < winner_chi2) { winner_chi2 = chi2; winner = &root; }
              }
              if (getenv("ISAC_DEBUG_CROSS")) {
                std::fprintf(stderr, "XC t=%.3f gate=%.3f roots=%zu winner_chi2=%.3f confirmed=%d\n",
                             time, gate, roots.size(), winner_chi2, winner && winner_chi2 <= gate);
              }
              if (!winner || winner_chi2 > gate) {
                ++last_processing_stats_.birth_cross_check_rejections;
                continue;
              }
              fit = *winner;
              ++last_processing_stats_.birth_four_receiver_refits;
            }
          }
          // OUR ADAPTATION: no declared target class can be below ground. The receiver hardware
          // (X410) is mounted 1 m above ground level, so a real target cannot be more than 1 m
          // below the receiver's own datum either way -- this is a deployment-derived physical
          // fact, not a threshold fitted to any capture's ghosts. A wrong-elevation trilateration
          // root is frequently the one that lands underground; reject it outright at birth.
          if (fit.position.z < -1.0) {
            ++last_processing_stats_.birth_ground_floor_rejections;
            continue;
          }
          if (!(config_.lifecycle_features & grouped_birth)) for (const Track& track : tracks_) {
            Matrix covariance(3, 3);
            for (size_t r = 0; r < 3; ++r) for (size_t c = 0; c < 3; ++c)
              covariance(r, c) = fit.covariance(r, c) + track.covariance(r, c);
            const Vec3 delta = fit.position - Vec3{track.state[0], track.state[1], track.state[2]};
            if (quadratic({delta.x, delta.y, delta.z}, pseudoinverse_symmetric(covariance)) <= gate)
              conflict = true;
          }
          if (conflict) continue;
          if (!(config_.lifecycle_features & grouped_birth)) branches.push_back(fit);
          const uint64_t family=(config_.lifecycle_features & grouped_birth) ? evidence_history_->next_family++ : 0;
          if(family) ++last_processing_stats_.birth_families_created;
          for(const auto& solution:branches) {
          fit=solution;
          Track track;
          track.birth_family=family;
          track.id = family ? evidence_history_->next_branch++ : next_id_++;
          track.config = config_; track.time_s = time;
          track.source_sequence = sequence;
          track.state = {fit.position.x, fit.position.y, fit.position.z,
                         fit.velocity.x, fit.velocity.y, fit.velocity.z, 0.0, 0.0, 0.0};
          for (size_t r = 0; r < 6; ++r) for (size_t c = 0; c < 6; ++c)
            track.covariance(r, c) = fit.covariance(r, c);
          for (size_t axis = 0; axis < 3; ++axis)
            track.covariance(axis + 6, axis + 6) = fit.covariance(axis + 3, axis + 3)
                / (config_.evidence_window_s * config_.evidence_window_s);
          track.probability = config_.initial_model_probability;
          const double total = std::accumulate(track.probability.begin(), track.probability.end(), 0.0);
          for (auto& p : track.probability) p /= total;
          for (size_t model = 0; model < MODEL_COUNT; ++model) {
            track.model_state[model] = track.state;
            track.model_covariance[model] = track.covariance;
          }
          track.birth_condition = fit.condition;
          // The evidence window sets how fast the confirmation statistic forgets old epochs; it
          // is not a physically justified bound on how long a confirmed target may coast through
          // a real detection gap. maximum_propagation_s is the existing, already-declared bound
          // on how long a state estimate may be trusted with no new information at all (used for
          // the same reason at the whole-tracker reset in this same file); reuse it here instead
          // of inventing a second constant for the same physical quantity.
          track.existence.initialize(time, config_.evidence_window_s, threshold,
                                      config_.maximum_propagation_s);
          track.confirmation_log_threshold = threshold;
#ifdef NR_ISAC_LIFECYCLE_TRACE
          ul_probe::family(sequence,family,track.id,0,track.target_state());
#endif
          if (getenv("ISAC_DEBUG_BIRTH")) {
            std::fprintf(stderr,
                "BIRTH t=%.3f id=%llu triple=(%zu,%zu,%zu) pos=(%.2f,%.2f,%.2f) vel=(%.2f,%.2f,%.2f) "
                "rms=%.3f samples=[rx%u@r%.2f,rx%u@r%.2f,rx%u@r%.2f]\n",
                time, (unsigned long long)track.id, a, b, c,
                fit.position.x, fit.position.y, fit.position.z,
                fit.velocity.x, fit.velocity.y, fit.velocity.z, fit.normalized_rms,
                sa.value.receiver, sa.value.value.range_m,
                sb.value.receiver, sb.value.value.range_m,
                sc.value.receiver, sc.value.value.range_m);
          }
          tracks_.push_back(std::move(track));
          if(family) ++last_processing_stats_.birth_branches_created;
          }
          ++last_processing_stats_.admitted_birth_solutions;
          if (sa.sequence != sb.sequence || sa.sequence != sc.sequence)
            ++last_processing_stats_.asynchronous_proposals;
          for (const auto& sample : samples)
            reserved.emplace(sample.sequence, sample.value.receiver, sample.value.detection);
          if (explanation_bundle) for (const auto& sample : samples)
            evidence_history_->owners.remember(sample.sequence, sample.value.receiver, sample.value.detection,
                sample.value.value.component_lineage, sample.time_s, tracks_.back().id);
        }
  // A consumed seed cannot repeatedly create fresh hypotheses in later CPIs.
  history.erase(std::remove_if(history.begin(), history.end(), [&](const auto& seed) {
    return reserved.count({seed.sequence, seed.value.receiver, seed.value.detection});
  }), history.end());
  last_processing_stats_.birth_elapsed_s = std::chrono::duration<double>(Clock::now() - birth_started).count();
  last_processing_stats_.active_internal_tracks = tracks_.size();
  std::set<uint64_t> pending;
  for (const auto& track : tracks_) {
    if (track.direct_emission_validated()) ++last_processing_stats_.reportable_tracks;
    if (track.birth_family) pending.insert(track.birth_family);
  }
  last_processing_stats_.pending_birth_families=pending.size();
  last_processing_stats_.processing_elapsed_s = std::chrono::duration<double>(Clock::now() - started).count();
  last_processing_stats_.processing_deadline_exhausted = last_processing_stats_.processing_elapsed_s > processing_budget_s;
}
