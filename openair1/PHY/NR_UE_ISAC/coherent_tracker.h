/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include "coherent_types.h"
#include <vector>
namespace nr_isac::coherent {
struct TrackerParams { double max_speed_mps = 0; double false_object_intensity_per_s = 0; Volume volume; Vec3 array_centroid; };
std::vector<int> hungarian(const std::vector<std::vector<double>>& cost);
class CoherentTracker {
public:
  explicit CoherentTracker(const TrackerParams& p);
  const std::vector<Track>& step(double t_s, double t_cpi_s, const std::vector<Detection>& dets, std::vector<int>* assoc = nullptr);
  double pd() const { return (pd_hits_ + 1.0) / (pd_hits_ + pd_misses_ + 2.0); }   // Laplace prior 1/1
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
};
} // namespace nr_isac::coherent
