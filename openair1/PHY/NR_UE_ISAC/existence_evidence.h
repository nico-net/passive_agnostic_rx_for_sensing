/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>

namespace nr_isac {

enum class ExistencePhase { candidate, tentative, confirmed, coasting, retired };

inline const char* existence_phase_name(ExistencePhase phase)
{
  switch (phase) {
    case ExistencePhase::candidate: return "candidate";
    case ExistencePhase::tentative: return "tentative";
    case ExistencePhase::confirmed: return "confirmed";
    case ExistencePhase::coasting: return "coasting";
    default: return "retired";
  }
}

/** One acquisition contributes once. Hard and soft scores must be alternatives, not a sum.
 * Scores are model evidence, explicitly NOT calibrated existence probabilities.
 */
class ExistenceEvidence {
public:
  struct Epoch {
    double time_s = 0.0;
    uint64_t sequence = 0;
    double dl = 0.0, ul = 0.0, origin = 0.0;
    uint32_t rx_mask = 0, soft_rx_mask = 0;
  };

  // window_s sets the exponential forgetting time-constant for the confirmation statistic --
  // how fast old epochs stop counting toward score_. max_silence_s is a physically different
  // quantity: how long a track's predicted state may still be trusted with zero positive
  // evidence before it is declared lost. Conflating the two (both previously read from
  // window_s) forced every confirmed track to retire on any gap longer than one evidence
  // window (~150 ms default), far shorter than a realistic fade/occlusion. Defaulting
  // max_silence_s to window_s keeps every existing caller's behavior bit-identical.
  void initialize(double time_s, double window_s, double threshold, double max_silence_s = -1.0)
  {
    if (!std::isfinite(time_s) || !(window_s > 0.0)
        || !std::isfinite(window_s) || !(threshold > 0.0) || !std::isfinite(threshold))
      throw std::invalid_argument("invalid existence operating point");
    if (max_silence_s < 0.0) max_silence_s = window_s;
    if (!std::isfinite(max_silence_s) || max_silence_s < window_s)
      throw std::invalid_argument("invalid existence max silence duration");
    start_ = time_ = last_positive_ = time_s;
    window_ = window_s;
    max_silence_ = max_silence_s;
    threshold_ = threshold;
    epochs_.clear();
    score_ = 0.0;
    phase_ = ExistencePhase::tentative;
    last_sequence_ = 0;
    have_sequence_ = false;
    ever_confirmed_ = false;
  }

  void advance(double time_s)
  {
    if (!std::isfinite(time_s) || time_s < time_)
      throw std::invalid_argument("existence acquisition time moved backwards");
    time_ = time_s;
    const double roundoff = 16.0 * std::numeric_limits<double>::epsilon()
                            * std::max(1.0, std::abs(time_));
    while (!epochs_.empty() && time_ - epochs_.front().time_s > window_ + roundoff)
      epochs_.pop_front();
    score_ = 0.0;
    for (const Epoch& epoch : epochs_)
      score_ += std::exp(-(time_ - epoch.time_s) / window_)
                * (epoch.dl + epoch.ul + epoch.origin);
    if (phase_ == ExistencePhase::retired) return;
    if (score_ >= threshold_) {
      ever_confirmed_ = true;
      phase_ = ExistencePhase::confirmed;
    } else if (score_ <= -threshold_ || time_ - last_positive_ > max_silence_ + roundoff) {
      phase_ = ExistencePhase::retired;
    } else {
      phase_ = ever_confirmed_ ? ExistencePhase::coasting : ExistencePhase::tentative;
    }
  }

  bool observe(Epoch epoch)
  {
    if (!std::isfinite(epoch.time_s) || epoch.time_s < time_
        || !std::isfinite(epoch.dl) || !std::isfinite(epoch.ul) || !std::isfinite(epoch.origin))
      throw std::invalid_argument("invalid existence epoch");
    if (have_sequence_ && epoch.sequence <= last_sequence_) return false;
    advance(epoch.time_s);
    if (phase_ == ExistencePhase::retired) return false;
    last_sequence_ = epoch.sequence;
    have_sequence_ = true;
    // One acquisition, regardless of RX/UE multiplicity, cannot confirm a fresh candidate.
    const double raw = epoch.dl + epoch.ul + epoch.origin;
    if (raw > 0.0) last_positive_ = epoch.time_s;
    const double bounded = std::clamp(raw, -threshold_, 0.75 * threshold_);
    if (raw != 0.0) {
      const double scale = bounded / raw;
      epoch.dl *= scale; epoch.ul *= scale; epoch.origin *= scale;
    }
    epochs_.push_back(epoch);
    advance(epoch.time_s);
    return true;
  }

  double score() const { return score_; }
  double threshold() const { return threshold_; }
  ExistencePhase phase() const { return phase_; }
  bool reportable() const
  { return ever_confirmed_ && phase_ != ExistencePhase::retired && score_ > 0.0; }
  uint32_t receiver_mask() const
  {
    uint32_t mask = 0;
    for (const auto& epoch : epochs_) mask |= epoch.rx_mask;
    return mask;
  }
  size_t epochs() const { return epochs_.size(); }

private:
  std::deque<Epoch> epochs_;
  double start_ = 0.0, time_ = 0.0, last_positive_ = 0.0;
  double window_ = 0.15, max_silence_ = 0.15, threshold_ = 1.0, score_ = 0.0;
  uint64_t last_sequence_ = 0;
  bool have_sequence_ = false, ever_confirmed_ = false;
  ExistencePhase phase_ = ExistencePhase::candidate;
};
} // namespace nr_isac
