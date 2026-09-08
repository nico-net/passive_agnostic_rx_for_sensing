/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "motion_tracker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace nr_isac {
namespace {

Matrix transition(double dt)
{
  Matrix f = Matrix::identity(3);
  f(0,1)=dt; f(0,2)=0.5*dt*dt; f(1,2)=dt;
  return f;
}

Matrix process_noise(double dt, double q)
{
  Matrix out(3,3);
  const double t2=dt*dt, t3=t2*dt, t4=t3*dt, t5=t4*dt;
  out(0,0)=q*t5/20.0; out(0,1)=out(1,0)=q*t4/8.0;
  out(0,2)=out(2,0)=q*t3/6.0; out(1,1)=q*t3/3.0;
  out(1,2)=out(2,1)=q*t2/2.0; out(2,2)=q*dt;
  return out;
}

std::vector<std::pair<size_t,size_t>> hungarian(const Matrix& costs)
{
  if (!costs.rows() || !costs.cols()) return {};
  bool transposed = costs.rows() > costs.cols();
  const size_t n = transposed ? costs.cols() : costs.rows();
  const size_t m = transposed ? costs.rows() : costs.cols();
  auto cost = [&](size_t i, size_t j) { return transposed ? costs(j-1,i-1) : costs(i-1,j-1); };
  std::vector<double> u(n+1), v(m+1);
  std::vector<size_t> p(m+1), way(m+1);
  for (size_t i=1;i<=n;++i) {
    p[0]=i; size_t j0=0;
    std::vector<double> minv(m+1,std::numeric_limits<double>::infinity());
    std::vector<uint8_t> used(m+1);
    do {
      used[j0]=1; const size_t i0=p[j0]; double delta=std::numeric_limits<double>::infinity(); size_t j1=0;
      for (size_t j=1;j<=m;++j) if(!used[j]) {
        const double cur=cost(i0,j)-u[i0]-v[j];
        if(cur<minv[j]) {minv[j]=cur;way[j]=j0;}
        if(minv[j]<delta) {delta=minv[j];j1=j;}
      }
      for(size_t j=0;j<=m;++j) if(used[j]) {u[p[j]]+=delta;v[j]-=delta;} else minv[j]-=delta;
      j0=j1;
    } while(p[j0]!=0);
    do { const size_t j1=way[j0]; p[j0]=p[j1]; j0=j1; } while(j0);
  }
  std::vector<std::pair<size_t,size_t>> result;
  for(size_t j=1;j<=m;++j) if(p[j]) {
    const size_t i=p[j]-1, jj=j-1;
    result.emplace_back(transposed?jj:i,transposed?i:jj);
  }
  std::sort(result.begin(),result.end());
  return result;
}

int status_priority(const std::string& status)
{
  if(status=="confirmed") return 3;
  if(status=="coasting") return 2;
  if(status=="tentative") return 1;
  return 0;
}

} // namespace

double range_psf_envelope(double delta, double resolution, double margin_db)
{
  if (!(resolution > 0.0)) throw std::invalid_argument("range PSF resolution must be positive");
  const double xi=std::abs(delta)/resolution;
  if(xi<1.0) return 1.0;
  return std::min(1.0,std::pow(10.0,margin_db/10.0)/(PI*PI*xi*xi));
}

double rate_psf_envelope(double delta, double resolution, double margin_db)
{
  return range_psf_envelope(delta,resolution,margin_db);
}

bool is_aperture_sidelobe(const Detection& c, const ConfirmedTrackView& p,
                          double rr, double vr, double margin_db,
                          const std::optional<double>& comb)
{
  if (!(p.last_score>0.0) || c.score>p.last_score) return false;
  const double dr=std::abs(c.range_m-p.range_m), dv=std::abs(c.range_rate_mps-p.rate_mps);
  if(dr<=0.8*rr && dv<=0.8*vr) return false;
  if(dr<=1.5*rr) {
    if(dv>1.2*vr && c.score<=p.last_score*rate_psf_envelope(dv,vr,margin_db)) return true;
    if(comb && *comb>0.0) {
      const long k=std::lround(dv/ *comb);
      if(k>=1 && std::abs(dv-k* *comb)<=1.5*vr) {
        const double bound=std::min(0.5,std::pow(10.0,margin_db/10.0)/(PI*k));
        if(c.score<=p.last_score*bound) return true;
      }
    }
  }
  return c.score<=p.last_score*range_psf_envelope(dr,rr,margin_db)*rate_psf_envelope(dv,vr,margin_db);
}

bool is_multipath_shadow(const Detection& c, const ConfirmedTrackView& p,
                         double rr, double vr, double maximum_delay)
{
  if(!(p.last_score>0.0)||c.score>p.last_score) return false;
  const double dr=c.range_m-p.range_m, dv=std::abs(c.range_rate_mps-p.rate_mps);
  return dr>0.0 && dr<=std::max(4.0*rr,maximum_delay) && dv<=1.5*vr
         && c.score<=p.last_score*std::pow(10.0,-3.0/10.0);
}

struct MotionTracker::Track {
  uint64_t id=0;
  MotionTrackerConfig config;
  std::vector<double> x{0.0,0.0,0.0};
  Matrix p{3,3};
  double time_s=0.0;
  std::string status="tentative";
  uint32_t coasts=0, confirmed_updates=0, total_updates=1;
  bool updated=true;
  std::vector<uint8_t> recent{1};
  std::optional<double> nis, nis_ewma;
  uint64_t source_sequence=0;
  double source_midpoint=0.0;
  double last_score=0.0;
  std::optional<size_t> associated_index;

  double effective_jerk() const {
    if(!config.adaptive_jerk||status!="confirmed"||!nis_ewma) return config.jerk_psd_m2_s5;
    const double scale=std::clamp(config.adaptive_jerk_floor+(1.0-config.adaptive_jerk_floor)*(*nis_ewma/2.0),
                                  config.adaptive_jerk_floor,1.0);
    return config.jerk_psd_m2_s5*scale;
  }
  void predict(double dt) {
    const Matrix f=transition(dt); x=f*x; p=f*p*f.transposed()+process_noise(dt,effective_jerk());
    time_s+=dt; updated=false; associated_index.reset();
  }
  std::tuple<double,std::vector<double>,Matrix,Matrix> innovation(const Detection& d, const Matrix& noise) const {
    Matrix h(2,3); h(0,0)=1.0; h(1,1)=1.0;
    Matrix s=h*p*h.transposed()+noise; Matrix inv=inverse(s);
    std::vector<double> y{d.range_m-x[0],d.range_rate_mps-x[1]};
    return {quadratic(y,inv),y,noise,inv};
  }
  void update_measurement(const Detection& d, const Matrix& noise,
                          const std::tuple<double,std::vector<double>,Matrix,Matrix>& in,
                          uint64_t sequence,size_t original) {
    Matrix h(2,3);h(0,0)=1.0;h(1,1)=1.0;
    const auto& y=std::get<1>(in); const auto& inv=std::get<3>(in);
    const Matrix gain=p*h.transposed()*inv;
    const auto dx=gain*y; for(size_t i=0;i<3;++i)x[i]+=dx[i];
    const Matrix factor=Matrix::identity(3)-gain*h;
    p=symmetrized(factor*p*factor.transposed()+gain*noise*gain.transposed());
    coasts=0;++total_updates;updated=true;nis=std::get<0>(in);
    nis_ewma=nis_ewma?(1.0-config.nis_ewma_alpha)* *nis_ewma+config.nis_ewma_alpha* *nis:*nis;
    recent.push_back(1);if(recent.size()>config.confirm_window)recent.erase(recent.begin());
    if(std::accumulate(recent.begin(),recent.end(),0u)>=config.confirm_updates){status="confirmed";++confirmed_updates;}
    else status="tentative";
    source_sequence=sequence;source_midpoint=time_s;last_score=d.score;associated_index=original;
  }
  bool coast(double rate_resolution) {
    ++coasts;recent.push_back(0);if(recent.size()>config.confirm_window)recent.erase(recent.begin());updated=false;associated_index.reset();
    const double guard=std::max(config.notch_guard_mps,1.5*rate_resolution);
    const uint32_t maximum=config.maximum_coasts+(std::abs(x[1])<=guard?config.notch_coast_extension:0);
    if(coasts>maximum){status="lost";return false;}status="coasting";return true;
  }
  TrackSnapshot snapshot() const {
    TrackSnapshot s;s.track_id=id;s.status=status;s.air_time_s=time_s;s.has_time=true;s.updated=updated;
    s.range_m=x[0];s.range_rate_mps=x[1];s.range_accel_mps2=x[2];
    s.sigma_range_m=std::sqrt(std::max(0.0,p(0,0)));s.sigma_rate_mps=std::sqrt(std::max(0.0,p(1,1)));
    s.sigma_accel_mps2=std::sqrt(std::max(0.0,p(2,2)));s.has_nis=nis.has_value();s.nis=nis.value_or(0.0);
    s.has_nis_ewma=nis_ewma.has_value();s.nis_ewma=nis_ewma.value_or(0.0);s.coast_count=coasts;
    s.confirmed_update_count=confirmed_updates;s.total_update_count=total_updates;
    s.source_cpi_sequence=source_sequence;s.source_cpi_midpoint_s=source_midpoint;return s;
  }
};

MotionTracker::MotionTracker(MotionTrackerConfig config)
    : config_(std::move(config)),
      clutter_map_(config_.clutter_alpha,config_.clutter_cfar_margin,
                   std::max(1.5,config_.direct_leakage_range_m/3.0),config_.static_clutter_guard_mps)
{
  if(!(config_.jerk_psd_m2_s5>0.0)||config_.confirm_updates<1||config_.confirm_updates>config_.confirm_window
     ||config_.maximum_tracks<1) throw std::invalid_argument("invalid motion tracker configuration");
}

MotionTracker::~MotionTracker() = default;

void MotionTracker::reset(){tracks_.clear();time_s_.reset();last_lost_=false;clutter_map_.reset();}

TrackSnapshot MotionTracker::snapshot() const
{
  if(tracks_.empty()){TrackSnapshot s;s.status=last_lost_?"lost":"uninitialized";if(time_s_){s.has_time=true;s.air_time_s=*time_s_;}return s;}
  const auto it=std::max_element(tracks_.begin(),tracks_.end(),[](const Track&a,const Track&b){
    return std::tuple(status_priority(a.status),a.confirmed_updates,a.last_score,-static_cast<int64_t>(a.id))
         < std::tuple(status_priority(b.status),b.confirmed_updates,b.last_score,-static_cast<int64_t>(b.id));});
  return it->snapshot();
}

TrackSnapshot MotionTracker::predict_to(double air_time_s) const
{
  if(tracks_.empty())return snapshot();
  const TrackSnapshot primary=snapshot();
  const auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](const Track&t){return t.id==primary.track_id;});
  const double dt=air_time_s-it->time_s;if(dt<0.0)throw std::invalid_argument("tracker time moved backwards");
  TrackSnapshot out=primary;out.air_time_s=air_time_s;out.propagation_age_s=dt;out.propagated=dt>0.0;
  if(dt>config_.maximum_propagation_s){out.status="stale";return out;}
  const Matrix f=transition(dt);const auto x=f*it->x;const Matrix p=f*it->p*f.transposed()+process_noise(dt,it->effective_jerk());
  out.range_m=x[0];out.range_rate_mps=x[1];out.range_accel_mps2=x[2];
  out.sigma_range_m=std::sqrt(std::max(0.0,p(0,0)));out.sigma_rate_mps=std::sqrt(std::max(0.0,p(1,1)));
  out.sigma_accel_mps2=std::sqrt(std::max(0.0,p(2,2)));return out;
}

std::vector<TrackSnapshot> MotionTracker::snapshots() const
{std::vector<TrackSnapshot> out;for(const auto&t:tracks_)out.push_back(t.snapshot());return out;}

std::vector<ConfirmedTrackView> MotionTracker::confirmed_tracks() const
{
  std::vector<ConfirmedTrackView> out;for(const auto&t:tracks_)if(t.status=="confirmed")
    out.push_back({t.id,t.x[0],t.x[1],std::sqrt(std::max(0.0,t.p(0,0))),std::sqrt(std::max(0.0,t.p(1,1))),t.last_score});
  return out;
}

std::vector<Stage1TrackView> MotionTracker::active_tracks() const
{
  std::vector<Stage1TrackView> out;
  out.reserve(tracks_.size());
  for (const auto& t : tracks_) out.push_back({t.snapshot(), t.associated_index, t.last_score});
  return out;
}

void MotionTracker::update(double air_time_s,const std::vector<Detection>& detections,
                           double range_res,double rate_res,uint64_t sequence)
{
  const double sigma_r=std::hypot(range_res/std::sqrt(12.0),config_.range_floor_m);
  const double sigma_v=std::hypot(rate_res/std::sqrt(12.0),config_.rate_floor_mps);
  std::vector<Detection> candidates;std::vector<size_t> original;
  std::vector<Matrix> noises;
  for(size_t i=0;i<detections.size();++i){const auto&d=detections[i];if(!std::isfinite(d.range_m)||!std::isfinite(d.range_rate_mps))continue;
    Matrix n(2,2);n(0,0)=sigma_r*sigma_r;n(1,1)=sigma_v*sigma_v;
    if(d.covariance_valid){Matrix c=symmetrized(d.range_rate_covariance);c(0,0)=std::max(c(0,0),n(0,0));c(1,1)=std::max(c(1,1),n(1,1));
      const auto ev=eigenvalues_symmetric_2x2(c(0,0),c(0,1),c(1,1));if(ev[0]>1e-9)n=c;}
    candidates.push_back(d);original.push_back(i);noises.push_back(n);
  }
  double dt=0.0; if(time_s_){dt=air_time_s-*time_s_;if(dt<0.0)throw std::invalid_argument("tracker time moved backwards");
    if(dt>config_.maximum_propagation_s){reset();dt=0.0;}}
  time_s_=air_time_s;if(dt>0.0)for(auto&t:tracks_)t.predict(dt);
  std::vector<size_t> order(candidates.size());std::iota(order.begin(),order.end(),0);
  std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return candidates[a].score>candidates[b].score;});
  std::vector<uint8_t> multipath(candidates.size()),sidelobe(candidates.size()),static_leak(candidates.size());
  for(size_t ip=0;ip<order.size();++ip){const size_t i=order[ip];if(multipath[i]||sidelobe[i])continue;
    ConfirmedTrackView primary{0,candidates[i].range_m,candidates[i].range_rate_mps,0,0,candidates[i].score};
    for(size_t jp=ip+1;jp<order.size();++jp){const size_t j=order[jp];
      if(config_.use_analytic_psf){if(is_multipath_shadow(candidates[j],primary,range_res,rate_res,config_.multipath_shadow_range_m))multipath[j]=1;
        if(is_aperture_sidelobe(candidates[j],primary,range_res,rate_res,config_.psf_margin_db,config_.comb_spacing_mps))sidelobe[j]=1;}
      else {const double dr=candidates[j].range_m-candidates[i].range_m,dv=std::abs(candidates[j].range_rate_mps-candidates[i].range_rate_mps);
        if(dr>0&&dr<=config_.multipath_shadow_range_m&&dv<=config_.multipath_shadow_rate_mps&&candidates[j].score<=candidates[i].score*.5)multipath[j]=1;
        if(std::abs(dr)<=std::max(config_.sidelobe_range_m,1.5*range_res)&&candidates[j].score<=candidates[i].score*config_.sidelobe_power_ratio)sidelobe[j]=1;}}
  }
  for(size_t i=0;i<candidates.size();++i){if(config_.use_adaptive_clutter_map)
      static_leak[i]=clutter_map_.is_static(candidates[i].range_m,candidates[i].range_rate_mps,candidates[i].score,range_res,rate_res);
    else static_leak[i]=(candidates[i].range_m<=std::max(config_.direct_leakage_range_m,1.5*range_res)
                         ||(candidates[i].score>0&&std::abs(candidates[i].range_rate_mps)<=std::min(config_.static_clutter_guard_mps,1.0)));}

  Matrix costs(tracks_.size(),candidates.size(),1e9);
  using Innovation=std::tuple<double,std::vector<double>,Matrix,Matrix>;
  std::map<std::pair<size_t,size_t>,Innovation> innovations;
  for(size_t t=0;t<tracks_.size();++t)for(size_t d=0;d<candidates.size();++d){auto in=tracks_[t].innovation(candidates[d],noises[d]);
    if(std::get<0>(in)<=config_.gate_chi2){costs(t,d)=std::get<0>(in);innovations[{t,d}]=in;}}
  std::set<size_t> assigned_t,assigned_d;
  for(auto [t,d]:hungarian(costs))if(costs(t,d)<=config_.gate_chi2){tracks_[t].update_measurement(candidates[d],noises[d],innovations.at({t,d}),sequence,original[d]);assigned_t.insert(t);assigned_d.insert(d);}
  std::vector<Track> survivors;bool lost=false;
  for(size_t t=0;t<tracks_.size();++t){if(!assigned_t.count(t)&&!tracks_[t].coast(rate_res)){lost=true;continue;}survivors.push_back(std::move(tracks_[t]));}
  last_lost_=lost&&survivors.empty();
  if(!lost)for(size_t d:order){if(assigned_d.count(d)||multipath[d]||sidelobe[d]||static_leak[d]||candidates[d].score<config_.birth_score_threshold)continue;
    if(survivors.size()>=config_.maximum_tracks)break;
    Track t;t.id=next_id_++;t.config=config_;t.x={candidates[d].range_m,candidates[d].range_rate_mps,0};
    t.p(0,0)=noises[d](0,0);t.p(1,1)=noises[d](1,1);t.p(2,2)=config_.initial_accel_sigma_mps2*config_.initial_accel_sigma_mps2;
    t.time_s=air_time_s;t.source_sequence=sequence;t.source_midpoint=air_time_s;t.last_score=candidates[d].score;t.associated_index=original[d];survivors.push_back(std::move(t));assigned_d.insert(d);}
  if(survivors.empty()&&!candidates.empty()&&!lost)for(size_t d:order){if(static_leak[d]||multipath[d]||sidelobe[d])continue;
    Track t;t.id=next_id_++;t.config=config_;t.x={candidates[d].range_m,candidates[d].range_rate_mps,0};t.p(0,0)=noises[d](0,0);t.p(1,1)=noises[d](1,1);t.p(2,2)=config_.initial_accel_sigma_mps2*config_.initial_accel_sigma_mps2;
    t.time_s=air_time_s;t.source_sequence=sequence;t.source_midpoint=air_time_s;t.last_score=candidates[d].score;t.associated_index=original[d];survivors.push_back(std::move(t));break;}
  tracks_=std::move(survivors);
}

} // namespace nr_isac
