/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace nr_isac {

/** A C-RNTI is a flow while it produced a DL CFR AND a UL CFR within the last window_s.
 *  The sensing gate is open while at least one flow exists (spec §8). */
class FlowGate {
public:
  explicit FlowGate(double window_s) : window_s_(window_s) {}

  /** Returns 1 when this event opened the gate, else 0. Never closes the gate -- only poll() may
   *  do that, so a fast DL-only stream landing between polls can't silently flip open_ false with
   *  no close reported (the close would never be logged, discarded, or recorded). */
  int note(uint16_t rnti, bool uplink, double now_s)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Flow& f = flows_[rnti];
    (uplink ? f.last_ul_s : f.last_dl_s) = now_s;
    if (!open_ && is_flow(f, now_s)) {
      open_ = true;
      return 1;
    }
    return 0;
  }

  bool admit(uint16_t rnti, double now_s) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = flows_.find(rnti);
    return open_ && it != flows_.end() && is_flow(it->second, now_s);
  }

  /** Expires stale flows. Returns -1 when this poll closed the gate, else 0. */
  int poll(double now_s)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = flows_.begin(); it != flows_.end();)
      it = (now_s - std::max(it->second.last_dl_s, it->second.last_ul_s) > window_s_) ? flows_.erase(it) : std::next(it);
    const bool was_open = open_;
    open_ = any_flow(now_s);
    return (was_open && !open_) ? -1 : 0;
  }

  bool open() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
  }

private:
  struct Flow {
    double last_dl_s = -1e300;
    double last_ul_s = -1e300;
  };
  bool is_flow(const Flow& f, double now_s) const
  {
    return now_s - f.last_dl_s <= window_s_ && now_s - f.last_ul_s <= window_s_;
  }
  bool any_flow(double now_s) const
  {
    for (const auto& item : flows_)
      if (is_flow(item.second, now_s)) return true;
    return false;
  }
  double window_s_;
  mutable std::mutex mutex_;
  std::unordered_map<uint16_t, Flow> flows_;
  bool open_ = false;
};

} // namespace nr_isac
