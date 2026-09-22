/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <vector>

namespace nr_isac {
enum LifecycleFeature : uint32_t { grouped_birth = 1, object_path_grouping = 2,
                                   explanatory_retirement = 4 };
inline double log_density_union(const std::vector<double>& alternatives) {
  // Clutter has relative density one. Sum mutually exclusive model densities,
  // NOT products of correlated receiver likelihoods. Equal model prior odds
  // charge alternative-model multiplicity conservatively.
  double largest=0;
  for(double value:alternatives) {
    if(!std::isfinite(value)) throw std::invalid_argument("invalid model evidence");
    largest=std::max(largest,value);
  }
  double sum=std::exp(-largest);
  for(double value:alternatives) sum+=std::exp(value-largest);
  return largest+std::log(sum);
}
inline double branch_increment(size_t branch,const std::vector<double>& evidence) {
  if(branch>=evidence.size()) throw std::invalid_argument("invalid branch");
  std::vector<double> alternatives;
  for(size_t i=0;i<evidence.size();++i) if(i!=branch) alternatives.push_back(evidence[i]);
  return evidence[branch]-log_density_union(alternatives);
}
class CausalModelPreference {
  struct Epoch { double time, gain; };
  std::deque<Epoch> history_;
public:
  void observe(double time,double gain,double window) {
    if(!std::isfinite(time+gain) || !(window>0) || !std::isfinite(window)
        || (!history_.empty() && time<=history_.back().time))
      throw std::invalid_argument("invalid causal model preference");
    history_.push_back({time,gain});
    while(!history_.empty() && time-history_.front().time>window) history_.pop_front();
  }
  double score(double time,double window) const {
    if(!std::isfinite(time) || !(window>0) || !std::isfinite(window)
        || (!history_.empty() && time<history_.back().time))
      throw std::invalid_argument("invalid preference query");
    double value=0;
    for(const auto& epoch:history_) if(time-epoch.time<=window)
      value+=std::exp(-(time-epoch.time)/window)*epoch.gain;
    return value;
  }
  size_t epochs(double time,double window) const {
    if(!std::isfinite(time) || !(window>0) || !std::isfinite(window)
        || (!history_.empty() && time<history_.back().time))
      throw std::invalid_argument("invalid preference query");
    return std::count_if(history_.begin(),history_.end(),[&](const auto& e){return time-e.time<=window;});
  }
};
} // namespace nr_isac
