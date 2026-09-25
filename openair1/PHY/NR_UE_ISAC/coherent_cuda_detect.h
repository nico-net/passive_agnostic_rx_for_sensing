/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/* GPU-resident coherent detect(): coherent_core.cc's detect() (the CPU oracle, kept intact and
 * selectable) with every per-round parallel piece on the device -- per-Doppler null scale, 4-D
 * local-maximum candidates, the per-candidate branch-and-bound channel choice, the per-channel path
 * fits, the residual cube and the per-item leakage re-score. The greedy pursuit's round loop (3-20
 * rounds) stays on the host and makes the same decisions in the same order as the oracle. */
#pragma once

#include "coherent_core.h"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace nr_isac::coherent {

struct GpuDetectTiming {
  double total_ms = 0, prep_ms = 0, scale_ms = 0, cand_ms = 0, choose_ms = 0, items_ms = 0, pursuit_ms = 0,
         pick_ms = 0, walk_ms = 0, fit_ms = 0, rebuild_ms = 0, rescore_ms = 0, choose2_ms = 0, refit_ms = 0, finish_ms = 0;
  uint32_t cands = 0, items = 0, rounds = 0, accepted = 0, fits = 0, sweeps = 0;
};

class GpuDetect {
public:
  GpuDetect();
  ~GpuDetect();
  GpuDetect(const GpuDetect&) = delete;
  GpuDetect& operator=(const GpuDetect&) = delete;
  /** Same contract as coherent::detect(E, R, g, geo, p). d_E: the device envelope ([t][voxel], float) or
   * null to upload E (E is then only a size placeholder). d_rd: unused -- the cube is always uploaded from
   * R.rd.v, the exact float values the oracle reads (see the .cu). stream: cudaStream_t (null = default). */
  std::vector<Detection> run(const std::vector<float>& E, const RdResult& R, const Grid& g, const Geometry& geo,
                             const DetectParams& p, const void* d_rd = nullptr, const float* d_E = nullptr,
                             void* stream = nullptr);
  const GpuDetectTiming& timing() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/** One detect() input set, for offline parity/timing replays (NR_ISAC_DETECT_DUMP=<dir> writes them). */
struct DetectCase {
  std::vector<float> E; RdResult R; Grid g; Geometry geo; DetectParams p;
};
namespace dump_io {
template <class T> void put(std::FILE* f, const T& v) { std::fwrite(&v, sizeof(T), 1, f); }
template <class T> void putv(std::FILE* f, const std::vector<T>& v)
{ const uint64_t n = v.size(); put(f, n); if (n) std::fwrite(v.data(), sizeof(T), n, f); }
template <class T> bool get(std::FILE* f, T* v) { return std::fread(v, sizeof(T), 1, f) == 1; }
template <class T> bool getv(std::FILE* f, std::vector<T>* v)
{ uint64_t n = 0; if (!get(f, &n) || n > (1ull << 32)) return false; v->resize(n); return !n || std::fread(v->data(), sizeof(T), n, f) == n; }
} // namespace dump_io
inline bool save_detect_case(const std::string& path, const std::vector<float>& E, const RdResult& R, const Grid& g,
                             const Geometry& geo, const DetectParams& p)
{
  using namespace dump_io;
  std::FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return false;
  const Axes& a = R.rd.axes; const auto& w = R.wf;
  put(f, (uint32_t)0x44455431);
  put(f, a.valid); put(f, a.fc_hz); put(f, a.lambda_m); put(f, a.scs_hz); put(f, a.subcarriers); put(f, a.n_fft);
  put(f, a.delay_step_s); put(f, a.n_range); put(f, a.b_eff_hz); put(f, a.n_dopp); put(f, a.dopp_step_hz); put(f, a.dopp0_hz);
  put(f, a.t_cpi_s); put(f, a.median_dt_s); put(f, a.notch_half_bins); put(f, a.v_max_mps); putv(f, a.tested_dopp); putv(f, a.row_t_s);
  putv(f, R.rd.v); put(f, R.los_tap); put(f, R.noise); put(f, R.los_found);
  putv(f, w.w); put(f, w.wsum); put(f, w.w2sum); putv(f, w.fc); putv(f, w.hh); putv(f, w.grp);
  put(f, (uint64_t)w.B.size()); for (size_t q = 0; q < w.B.size(); ++q) { putv(f, w.B[q]); putv(f, w.B2[q]); }
  put(f, w.X); put(f, w.sc); putv(f, w.mask); putv(f, w.lo); putv(f, w.hi); putv(f, w.Q);
  putv(f, E); put(f, g.origin); put(f, g.step); put(f, g.nx); put(f, g.ny); put(f, g.nz); put(f, geo); put(f, p.pfa);
  return std::fclose(f) == 0;
}
inline bool load_detect_case(const std::string& path, DetectCase* c)
{
  using namespace dump_io;
  std::FILE* f = std::fopen(path.c_str(), "rb"); if (!f) return false;
  Axes& a = c->R.rd.axes; auto& w = c->R.wf; uint32_t magic = 0; uint64_t ng = 0;
  bool ok = get(f, &magic) && magic == 0x44455431;
  ok = ok && get(f, &a.valid) && get(f, &a.fc_hz) && get(f, &a.lambda_m) && get(f, &a.scs_hz) && get(f, &a.subcarriers) && get(f, &a.n_fft)
       && get(f, &a.delay_step_s) && get(f, &a.n_range) && get(f, &a.b_eff_hz) && get(f, &a.n_dopp) && get(f, &a.dopp_step_hz) && get(f, &a.dopp0_hz)
       && get(f, &a.t_cpi_s) && get(f, &a.median_dt_s) && get(f, &a.notch_half_bins) && get(f, &a.v_max_mps) && getv(f, &a.tested_dopp) && getv(f, &a.row_t_s);
  ok = ok && getv(f, &c->R.rd.v) && get(f, &c->R.los_tap) && get(f, &c->R.noise) && get(f, &c->R.los_found);
  ok = ok && getv(f, &w.w) && get(f, &w.wsum) && get(f, &w.w2sum) && getv(f, &w.fc) && getv(f, &w.hh) && getv(f, &w.grp) && get(f, &ng) && ng < 100000;
  if (ok) { w.B.resize(ng); w.B2.resize(ng); for (size_t q = 0; ok && q < ng; ++q) ok = getv(f, &w.B[q]) && getv(f, &w.B2[q]); }
  ok = ok && get(f, &w.X) && get(f, &w.sc) && getv(f, &w.mask) && getv(f, &w.lo) && getv(f, &w.hi) && getv(f, &w.Q);
  ok = ok && getv(f, &c->E) && get(f, &c->g.origin) && get(f, &c->g.step) && get(f, &c->g.nx) && get(f, &c->g.ny) && get(f, &c->g.nz)
       && get(f, &c->geo) && get(f, &c->p.pfa);
  std::fclose(f);
  return ok;
}

} // namespace nr_isac::coherent
