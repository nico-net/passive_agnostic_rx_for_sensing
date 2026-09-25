/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include "coherent_types.h"
#include <vector>
namespace nr_isac::coherent {
struct TrackerParams {
  double max_speed_mps = 0; double false_object_intensity_per_s = 0; Volume volume; Vec3 array_centroid;
  double min_confirm_age_s = 0;   // a track is confirmed only once it has lived this long (operator rule; 0 = off)
};
std::vector<int> hungarian(const std::vector<std::vector<double>>& cost);
/** The bistatic range-rate band a scan tests (|rate| in [lo, hi], m/s, illuminator tx): a track whose
 * predicted rate lies outside it could not have been detected, so its miss is weighted by the probability
 * P_vis that its rate is inside (miss LLR ln(1 - P_D P_vis)). nullptr = the whole axis (P_vis = 1). */
struct RateBand { double lo = 0, hi = 0; Vec3 tx; };
class CoherentTracker {
public:
  explicit CoherentTracker(const TrackerParams& p);
  /** t_s earlier than the last scan (an out-of-sequence scan, e.g. the long dwell's midpoint): the tracks
   * are retrodicted to t_s, updated, and predicted forward again (process noise added both ways). */
  const std::vector<Track>& step(double t_s, double t_cpi_s, const std::vector<Detection>& dets, std::vector<int>* assoc = nullptr,
                                 const RateBand* band = nullptr);
  double pd() const { return (pd_hits_ + 1.0) / (pd_hits_ + pd_misses_ + 2.0); }   // Laplace prior 1/1
  /** A place a non-translating scatterer was found (a track whose Doppler says it moves but whose
   * position does not): new tracks may not be BORN inside it (existing tracks still pass through).
   * It expires when its own hit rate makes the silence since its last hit a 1 % event. */
  struct SuppressionCell { Vec3 pos; std::array<double, 9> cov{}; double t_first = 0, t_last = 0; uint32_t n = 0; };
  const std::vector<SuppressionCell>& suppression_cells() const { return cells_; }
  uint64_t kin_deleted() const { return kin_deleted_; }
  uint64_t births_blocked() const { return births_blocked_; }
private:
  void predict(Track& t, double dt) const;
  double position_nis(const Track& t, const Detection& d, double* logdet_s) const;
  void update_position(Track& t, const Detection& d, double* nis) const;
  void update_rate(Track& t, const Detection& d) const;
  TrackerParams p_;
  std::vector<Track> tracks_;
  uint64_t next_id_ = 1;
  double last_t_ = -1;
  double pd_hits_ = 0, pd_misses_ = 0;
  struct ClutterPoint { Vec3 pos; std::array<double, 9> cov; uint64_t owner; };   // owner = track it fed
  std::vector<ClutterPoint> clutter_hist_;   // unclaimed in-volume detections this epoch
  double clutter_t_ = 0;                      // dwell seconds this epoch
  std::vector<SuppressionCell> cells_;
  uint64_t kin_deleted_ = 0, births_blocked_ = 0;
  void kin_hit(Track& t, const Detection& d, double t_s);   // returns via t.llr = -inf when non-translating
};
} // namespace nr_isac::coherent
