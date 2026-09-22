/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstdint>
#include <deque>
#include <map>
#include <tuple>
#include <vector>

namespace nr_isac {
// Conditional component evidence, not a calibrated object probability. With
// l=log(p_signal/p_clutter), the null is (1-q)*p_clutter+q*p_signal
// when a frozen existing-object explanation covers this component. This is a
// conservative unresolved-component alternative, NOT subtraction of coherent
// powers and NOT a complete two-scatterer coherent likelihood. Distinct local
// components remain available; exact shared support supplies no new evidence.
inline double unexplained_component_evidence(double l, double q)
{
  if (!std::isfinite(l) || !std::isfinite(q) || q < 0.0 || q > 1.0)
    throw std::invalid_argument("invalid component explanation");
  if (q == 0.0) return l;
  if (q == 1.0) return 0.0;
  if (l >= 0.0) return -std::log(q + (1.0 - q) * std::exp(-l));
  return l - std::log1p(q * std::expm1(l));
}

class ComponentOwnership {
  using Key = std::tuple<uint64_t,uint32_t,bool,uint64_t>;
  struct Owner { double time; uint64_t track; };
  std::map<Key,Owner> owners_;
public:
  bool contains(uint64_t sequence,uint32_t rx,size_t index,
                const std::vector<uint32_t>& lineage) const {
    if(lineage.empty()) return owners_.count({sequence,rx,false,index});
    for(auto component:lineage) if(owners_.count({sequence,rx,true,component})) return true;
    return false;
  }
  void remember(uint64_t sequence,uint32_t rx,size_t index,
                const std::vector<uint32_t>& lineage,double time,uint64_t track) {
    if(lineage.empty()) owners_.emplace(Key{sequence,rx,false,index},Owner{time,track});
    for(auto component:lineage) owners_.emplace(Key{sequence,rx,true,component},Owner{time,track});
  }
  void prune(double time,double window) {
    for(auto it=owners_.begin();it!=owners_.end();)
      if(time-it->second.time>window) it=owners_.erase(it); else ++it;
  }
};

// Proof of redundancy requires repeated shared measurements, and no recent
// independent support. Close positions alone are deliberately absent here.
class DuplicateEvidence {
  struct Epoch { double time; uint32_t shared; bool distinct; };
  std::deque<Epoch> epochs_;
public:
  bool observe(double time,uint32_t shared,bool distinct,double window,double memory) {
    if(!epochs_.empty() && time<=epochs_.back().time)
      throw std::invalid_argument("duplicate evidence acquisition repeated");
    epochs_.push_back({time,shared,distinct});
    while(!epochs_.empty() && time-epochs_.front().time>memory) epochs_.pop_front();
    size_t repeated=0;
    for(const auto& epoch:epochs_) {
      if(epoch.distinct) return false;
      if(time-epoch.time<=window && __builtin_popcount(epoch.shared)>=3) ++repeated;
    }
    return __builtin_popcount(shared)>=3 && repeated>=2;
  }
};
} // namespace nr_isac
