/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "clean_detector.h"

#include "adaptive_threshold.h"
#include "robust_stats.h"
#include "cuda_support.h"
#include "detector_cuda.h"
#include "fft.h"
#ifdef NR_ISAC_FIXED_WORK_REPLAY
#include "diagnostic_clean_budget.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <stdexcept>
#include <vector>
#if defined(__AVX2__) || defined(__AVX512F__)
#include <immintrin.h>
#endif

namespace nr_isac {
namespace {

constexpr double kTaperSidelobeDb = 80.0;   // declared design constant, see prepare()

// (2026-09-22) RANGE-WALK STEERING (NR_ISAC_RANGE_WALK=1). See detector_cuda.cu for the model.
// Declared threshold, not fitted: the correction is applied only when the dwell is long enough for
// the fastest searched target to migrate at least half a range cell, |v|max * T >= dR/2; below that
// the walk is inside the cell the matched filter already integrates and the map is left untouched.
// (2026-09-22) SKIRT RULE, REPLACED (NR_ISAC_SKIRT_SIDELOBE=1).
// The old rule ran a second OS-CFAR along the candidate's own range column and called the proposal
// a Doppler skirt whenever that statistic failed. Audited against ground truth on drone_car_car_det:
// a TRUE car component has row z = 350-430 against a row threshold of ~31, but column z = 14-15
// against a column threshold of ~36, so 72% of true car components and 89% of true drone components
// were discarded; clutter sat at column z = 7.7 and was discarded 97% of the time. The reason is
// physical: with 93 irregularly spaced rows the Doppler sidelobe floor of a REAL point target is
// only ~11 dB below its own peak, so the median of a column that contains a target is ~11 dB under
// the peak and no true object can ever reach a flat-noise threshold of 15 dB.
//
// The replacement tests the hypothesis the rule is actually for: "this cell is a Doppler sidelobe
// of the strongest cell in its own range column". That hypothesis PREDICTS a level -- the column
// peak times the measured peak sidelobe ratio of this CPI's own point-spread function. A cell above
// that bound cannot be explained by the sidelobe of the column peak and is therefore a distinct
// object; a cell below it is indistinguishable from one and is still suppressed. No new constant:
// the bound is the measured PSF, and the main-lobe guard is the measured Doppler half-width.
bool skirt_sidelobe_rule()
{
  static const bool value = [] { const char* v = std::getenv("NR_ISAC_SKIRT_SIDELOBE"); return v && *v == '1'; }();
  return value;
}

bool range_walk_enabled()
{
  static const bool value = [] { const char* v = std::getenv("NR_ISAC_RANGE_WALK"); return v && *v == '1'; }();
  return value;
}
double family_max_range_bins()
{
  static const double value = [] { const char* v = std::getenv("NR_ISAC_FAMILY_MAX_RANGE_BINS"); return v ? std::atof(v) : 0.0; }();
  return value;
}
bool row_taper_in_time()
{
  static const bool value = [] { const char* v = std::getenv("NR_ISAC_ROW_TAPER_TIME"); return v && *v == '1'; }();
  return value;
}

// Dolph-Chebyshev window of length n with equiripple sidelobes at -sidelobe_db, normalized to
// unit peak.  Closed form (frequency sampling of the Chebyshev polynomial T_{n-1}, then inverse
// DFT); computed once per window shape because Plan objects are cached by shape.
std::vector<double> dolph_chebyshev_uncached(uint32_t n, double sidelobe_db);

// NR_ISAC_CLEAN_FULL_MAP=1 restores the full likelihood download every iteration (A/B only).
static bool force_full_map()
{
  static const bool value = [] { const char* v = std::getenv("NR_ISAC_CLEAN_FULL_MAP"); return v && *v == '1'; }();
  return value;
}

// Shape-invariant: cache per (length, level).  Uncached this is O(n^2) (~0.15 s for n=3276) and
// prepare() runs every CPI, which pushed every CPI past the processing deadline (measured:
// 640/640 CPI x rx stopped on next_cpi_processing_deadline after one component).
std::vector<double> dolph_chebyshev(uint32_t n, double sidelobe_db)
{
  static std::mutex mutex;
  static std::map<std::pair<uint32_t, double>, std::vector<double>> cache;
  std::lock_guard<std::mutex> lock(mutex);
  auto found = cache.find({n, sidelobe_db});
  if (found != cache.end()) return found->second;
  return cache.emplace(std::make_pair(n, sidelobe_db), dolph_chebyshev_uncached(n, sidelobe_db)).first->second;
}

std::vector<double> dolph_chebyshev_uncached(uint32_t n, double sidelobe_db)
{
  std::vector<double> w(n, 1.0);
  if (n < 3) return w;
  const double ripple = std::pow(10.0, sidelobe_db / 20.0);
  const int order = static_cast<int>(n) - 1;
  const double beta = std::cosh(std::acosh(ripple) / order);
  auto cheb = [order](double x) {
    if (std::abs(x) <= 1.0) return std::cos(order * std::acos(x));
    const double v = std::cosh(order * std::acosh(std::abs(x)));
    return (x < 0.0 && (order & 1)) ? -v : v;
  };
  std::vector<double> spectrum(n);
  for (uint32_t k = 0; k < n; ++k)
    spectrum[k] = cheb(beta * std::cos(PI * k / n));
  for (uint32_t m = 0; m < n; ++m) {
    double acc = 0.0;
    for (uint32_t k = 0; k < n; ++k)
      acc += spectrum[k] * std::cos(2.0 * PI * k * (static_cast<double>(m) - 0.5 * order) / n);
    w[m] = acc;
  }
  const double peak = *std::max_element(w.begin(), w.end());
  for (double& v : w) v = std::max(0.0, v / peak);
  return w;
}


struct Plan {
  Axes axes;
  std::vector<double> times;
  std::vector<double> weights;
  std::vector<uint8_t> rate_allowed;
  std::vector<double> range_phase_rate;
  std::vector<double> doppler_phase_rate;
  std::vector<double> slow_steering_real;
  std::vector<double> slow_steering_imag;
  double denominator = 0.0;
  bool range_walk = false;              // (2026-09-22) see range_walk_enabled()
  double range_walk_midpoint_s = 0.0;   // slow-time origin the map's range coordinate refers to
};

Plan prepare(const CfrWindow& window, const PipelineConfig& config, const RateGate& gate)
{
  if (!window.valid() || window.rows < 2)
    throw std::invalid_argument("detector needs a valid coherent aperture");
  if (gate.center_mps.has_value() != gate.half_width_mps.has_value()
      || (gate.half_width_mps && !(*gate.half_width_mps > 0.0)))
    throw std::invalid_argument("invalid signed range-rate gate");
  Plan plan;
  plan.times.resize(window.rows);
  const double slot_s = slot_duration_s(window.scs_hz);
  for (uint32_t r = 0; r < window.rows; ++r)
    plan.times[r] = (window.row_time_slots[r] - window.row_time_slots[0]) * slot_s;
  plan.axes.dwell_s = plan.times.back();
  if (!(plan.axes.dwell_s > 0.0))
    throw std::invalid_argument("nonviable coherent aperture");
  plan.axes.range_res_m = C_MPS / (window.subcarriers * window.scs_hz);
  plan.axes.range_bins = std::min(window.subcarriers,
      static_cast<uint32_t>(std::floor(config.maximum_range_m / plan.axes.range_res_m)) + 1);
  plan.axes.rate_res_mps = C_MPS * (window.rows - 1.0)
                           / (window.fc_hz * window.rows * plan.axes.dwell_s);
  plan.axes.rate_bins = window.rows;
  plan.axes.rate_axis_mps.resize(window.rows);
  plan.rate_allowed.resize(window.rows, 1);
  for (uint32_t d = 0; d < window.rows; ++d) {
    const double rate = -(static_cast<double>(d) - static_cast<int>(window.rows / 2))
                        * plan.axes.rate_res_mps;
    plan.axes.rate_axis_mps[d] = rate;
    bool allowed = std::abs(rate) <= 2.0 * config.maximum_target_speed_mps;
    if (gate.half_width_mps)
      allowed = allowed && std::abs(rate - *gate.center_mps) <= *gate.half_width_mps;
    plan.rate_allowed[d] = allowed;
  }
  if (std::none_of(plan.rate_allowed.begin(), plan.rate_allowed.end(), [](uint8_t x) { return x != 0; }))
    throw std::invalid_argument("range-rate gate excludes every Doppler bin");
  plan.range_phase_rate.resize(window.subcarriers);
  for (uint32_t k = 0; k < window.subcarriers; ++k)
    plan.range_phase_rate[k] = -2.0 * PI * k / window.subcarriers;
  plan.doppler_phase_rate.resize(window.rows);
  plan.slow_steering_real.resize((size_t)window.rows * window.rows);
  plan.slow_steering_imag.resize((size_t)window.rows * window.rows);
  for (uint32_t r = 0; r < window.rows; ++r)
    plan.doppler_phase_rate[r] = 2.0 * PI * plan.axes.rate_res_mps * window.fc_hz
                                 * plan.times[r] / C_MPS;
  for (uint32_t d = 0; d < window.rows; ++d) {
    if (!plan.rate_allowed[d]) continue;
    const double doppler_bin = static_cast<double>(d) - static_cast<int>(window.rows / 2);
    for (uint32_t r = 0; r < window.rows; ++r) {
      const double phase = -plan.doppler_phase_rate[r] * doppler_bin;
      plan.slow_steering_real[(size_t)d * window.rows + r] = std::cos(phase);
      plan.slow_steering_imag[(size_t)d * window.rows + r] = std::sin(phase);
    }
  }
  // OUR ADAPTATION (stage-5 fix, 2026-09-18): Dolph-Chebyshev taper on both axes.  With the
  // previous rectangular weights the -13 dB sinc sidelobes of (a) the post-stage-3 static
  // residual (measured 55-70 dB above the floor) and (b) the target itself spread a skirt across
  // the whole map (measured +21 dB at +-1 Doppler bin, +10 dB at +-8, +4 dB at +-16; target range
  // sidelobes +39 dB at +-10 range bins), which is what the stage-6 local statistic was training
  // on.  Chebyshev gives the narrowest main lobe for a declared equiripple sidelobe level.  The
  // level is a declared design constant: 80 dB, i.e. below the strongest residual observed
  // (68 dB) with margin; it is not tied to any object class or capture.  PSF half-width,
  // guard, and hypothesis count are measured from these weights downstream, so they adapt.
  std::vector<double> row_taper = dolph_chebyshev(window.rows, kTaperSidelobeDb);
  // (2026-09-21, NR_ISAC_ROW_TAPER_TIME=1) Real captures fill only part of the CPI's slots and the
  // rows are irregular in time (grant pattern), so an index-domain taper does not realise its design
  // sidelobes along Doppler. Non-uniform-sampling form: evaluate the taper at each row's ACTUAL time
  // (Kaiser with the same 80 dB design level, beta = 0.1102 (A - 8.7)) and apply Voronoi density
  // compensation (each row weighted by the time interval it represents). No class/capture constant.
  if (row_taper_in_time()) {
    const double beta = 0.1102 * (kTaperSidelobeDb - 8.7);
    const double T = plan.axes.dwell_s;
    auto bessel_i0 = [](double x) { double s = 1.0, t = 1.0; for (int k = 1; k < 60; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; if (t < 1e-16 * s) break; } return s; };
    const double i0b = bessel_i0(beta);
    double mean_weight = 0.0;
    for (uint32_t r = 0; r < window.rows; ++r) {
      const double u = 2.0 * plan.times[r] / T - 1.0;                       // [-1, 1]
      const double taper = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - u * u))) / i0b;
      const double left = r ? plan.times[r - 1] : plan.times[r];
      const double right = r + 1 < window.rows ? plan.times[r + 1] : plan.times[r];
      const double density = 0.5 * (right - left) + ((r == 0 || r + 1 == window.rows) ? 0.5 * T / (window.rows - 1.0) : 0.0);
      row_taper[r] = taper * density;
      mean_weight += row_taper[r] / window.rows;
    }
    for (double& w : row_taper) w /= mean_weight;
  }
  const std::vector<double> subcarrier_taper = dolph_chebyshev(window.subcarriers, kTaperSidelobeDb);
  plan.weights.resize((size_t)window.rows * window.subcarriers);
  for (uint32_t r = 0; r < window.rows; ++r)
    for (uint32_t k = 0; k < window.subcarriers; ++k) {
      const size_t i = (size_t)r * window.subcarriers + k;
      plan.weights[i] = window.observed[i] ? row_taper[r] * subcarrier_taper[k] : 0.0;
      plan.denominator += plan.weights[i];
    }
  plan.axes.observed_re_count = static_cast<uint64_t>(plan.denominator);
  // Range-walk steering is a dwell-dependent decision, taken from measured quantities only:
  // enable it when the fastest searched rate migrates at least half a range cell over the dwell.
  if (range_walk_enabled()) {
    const double max_rate = 0.5 * plan.axes.rate_res_mps * static_cast<double>(window.rows);
    plan.range_walk = max_rate * plan.axes.dwell_s >= 0.5 * plan.axes.range_res_m;
    plan.range_walk_midpoint_s = 0.5 * (plan.times.front() + plan.times.back());
  }
  return plan;
}

double residual_energy(const std::vector<std::complex<double>>& residual,
                       uint32_t antennas, const Plan& plan)
{
  const size_t cells = plan.weights.size();
  double result = 0.0;
  for (uint32_t a = 0; a < antennas; ++a)
    for (size_t i = 0; i < cells; ++i)
      result += plan.weights[i] * std::norm(residual[(size_t)a * cells + i]);
  return result;
}

struct CpuMapWorkspace {
  std::vector<double> projected_real;
  std::vector<double> projected_imag;
  std::vector<std::complex<double>> fft_buffer;
};

std::complex<double> coherent_dot(const double* projected_real,
                                  const double* projected_imag,
                                  const double* steering_real,
                                  const double* steering_imag,
                                  uint32_t rows)
{
  uint32_t r = 0;
  double real = 0.0, imag = 0.0;
#if defined(__AVX512F__)
  __m512d real0 = _mm512_setzero_pd(), real1 = _mm512_setzero_pd();
  __m512d imag0 = _mm512_setzero_pd(), imag1 = _mm512_setzero_pd();
  for (; r + 16 <= rows; r += 16) {
    const __m512d xr0 = _mm512_loadu_pd(projected_real + r);
    const __m512d xi0 = _mm512_loadu_pd(projected_imag + r);
    const __m512d sr0 = _mm512_loadu_pd(steering_real + r);
    const __m512d si0 = _mm512_loadu_pd(steering_imag + r);
    const __m512d xr1 = _mm512_loadu_pd(projected_real + r + 8);
    const __m512d xi1 = _mm512_loadu_pd(projected_imag + r + 8);
    const __m512d sr1 = _mm512_loadu_pd(steering_real + r + 8);
    const __m512d si1 = _mm512_loadu_pd(steering_imag + r + 8);
    real0 = _mm512_add_pd(real0, _mm512_sub_pd(_mm512_mul_pd(xr0, sr0),
                                               _mm512_mul_pd(xi0, si0)));
    imag0 = _mm512_add_pd(imag0, _mm512_add_pd(_mm512_mul_pd(xr0, si0),
                                               _mm512_mul_pd(xi0, sr0)));
    real1 = _mm512_add_pd(real1, _mm512_sub_pd(_mm512_mul_pd(xr1, sr1),
                                               _mm512_mul_pd(xi1, si1)));
    imag1 = _mm512_add_pd(imag1, _mm512_add_pd(_mm512_mul_pd(xr1, si1),
                                               _mm512_mul_pd(xi1, sr1)));
  }
  alignas(64) double real_lanes[8], imag_lanes[8];
  _mm512_store_pd(real_lanes, _mm512_add_pd(real0, real1));
  _mm512_store_pd(imag_lanes, _mm512_add_pd(imag0, imag1));
  for (uint32_t lane = 0; lane < 8; ++lane) {
    real += real_lanes[lane];
    imag += imag_lanes[lane];
  }
#elif defined(__AVX2__)
  __m256d real0 = _mm256_setzero_pd(), real1 = _mm256_setzero_pd();
  __m256d imag0 = _mm256_setzero_pd(), imag1 = _mm256_setzero_pd();
  for (; r + 8 <= rows; r += 8) {
    const __m256d xr0 = _mm256_loadu_pd(projected_real + r);
    const __m256d xi0 = _mm256_loadu_pd(projected_imag + r);
    const __m256d sr0 = _mm256_loadu_pd(steering_real + r);
    const __m256d si0 = _mm256_loadu_pd(steering_imag + r);
    const __m256d xr1 = _mm256_loadu_pd(projected_real + r + 4);
    const __m256d xi1 = _mm256_loadu_pd(projected_imag + r + 4);
    const __m256d sr1 = _mm256_loadu_pd(steering_real + r + 4);
    const __m256d si1 = _mm256_loadu_pd(steering_imag + r + 4);
    real0 = _mm256_add_pd(real0, _mm256_sub_pd(_mm256_mul_pd(xr0, sr0),
                                               _mm256_mul_pd(xi0, si0)));
    imag0 = _mm256_add_pd(imag0, _mm256_add_pd(_mm256_mul_pd(xr0, si0),
                                               _mm256_mul_pd(xi0, sr0)));
    real1 = _mm256_add_pd(real1, _mm256_sub_pd(_mm256_mul_pd(xr1, sr1),
                                               _mm256_mul_pd(xi1, si1)));
    imag1 = _mm256_add_pd(imag1, _mm256_add_pd(_mm256_mul_pd(xr1, si1),
                                               _mm256_mul_pd(xi1, sr1)));
  }
  alignas(32) double real_lanes[4], imag_lanes[4];
  _mm256_store_pd(real_lanes, _mm256_add_pd(real0, real1));
  _mm256_store_pd(imag_lanes, _mm256_add_pd(imag0, imag1));
  for (uint32_t lane = 0; lane < 4; ++lane) {
    real += real_lanes[lane];
    imag += imag_lanes[lane];
  }
#endif
  for (; r < rows; ++r) {
    real += projected_real[r] * steering_real[r] - projected_imag[r] * steering_imag[r];
    imag += projected_real[r] * steering_imag[r] + projected_imag[r] * steering_real[r];
  }
  return {real, imag};
}

// Variant with pre-scaled slow-time coordinates. Kept explicit to make the physical sign identical
// to optimized_pipeline.py's steering_conjugate matrix.
void likelihood_map_scaled(const std::vector<std::complex<double>>& residual,
                           uint32_t antennas,
                           uint32_t rows,
                           uint32_t subcarriers,
                           const Plan& plan,
                           uint32_t minimum_range_bin,
                           CpuMapWorkspace& workspace,
                           std::vector<double>& result)
{
  const uint32_t range_bins = plan.axes.range_bins;
  workspace.projected_real.resize((size_t)antennas * rows * range_bins);
  workspace.projected_imag.resize((size_t)antennas * rows * range_bins);
  workspace.fft_buffer.resize(subcarriers);
  auto& buffer = workspace.fft_buffer;
  const size_t cells = (size_t)rows * subcarriers;
  for (uint32_t a = 0; a < antennas; ++a)
    for (uint32_t r = 0; r < rows; ++r) {
      for (uint32_t k = 0; k < subcarriers; ++k) {
        const size_t cell = (size_t)r * subcarriers + k;
        buffer[k] = plan.weights[cell] * residual[(size_t)a * cells + cell];
      }
      fft_inplace(buffer, true);
      for (uint32_t q = 0; q < range_bins; ++q) {
        const size_t projected = ((size_t)a * range_bins + q) * rows + r;
        workspace.projected_real[projected] = buffer[q].real() * subcarriers;
        workspace.projected_imag[projected] = buffer[q].imag() * subcarriers;
      }
  }
  const double denominator = plan.denominator * antennas;
  result.assign((size_t)range_bins * rows, -std::numeric_limits<double>::infinity());
  if (!(denominator > 0.0)) return;
  /* Projected samples are range-major so each Doppler dot product walks contiguous memory.
   * Slow-time steering is invariant within the CPI and is materialized once by prepare(). */
  const bool walk = plan.range_walk;
  const double t_mid = plan.range_walk_midpoint_s;
  for (uint32_t q = minimum_range_bin; q < range_bins; ++q)
    for (uint32_t d = 0; d < rows; ++d) {
      if (!plan.rate_allowed[d]) continue;
      double power = 0.0;
      const double walk_rate = walk
          ? -(static_cast<double>(d) - static_cast<double>(rows / 2)) * plan.axes.rate_res_mps
          : 0.0;
      for (uint32_t a = 0; a < antennas; ++a) {
        const size_t projected = ((size_t)a * range_bins + q) * rows;
        const size_t steering = (size_t)d * rows;
        std::complex<double> coherent;
        if (!walk) {
          coherent = coherent_dot(
              workspace.projected_real.data() + projected,
              workspace.projected_imag.data() + projected,
              plan.slow_steering_real.data() + steering,
              plan.slow_steering_imag.data() + steering, rows);
        } else {
          // Same matched filter, with each row read at the range the hypothesis puts it
          // (linear interpolation on the projected lattice; the lattice is the full IFFT grid).
          for (uint32_t r = 0; r < rows; ++r) {
            const double shift = walk_rate * (plan.times[r] - t_mid) / plan.axes.range_res_m;
            const double index = static_cast<double>(q) + shift;
            const double wrapped = index - std::floor(index / range_bins) * range_bins;
            const uint32_t q0 = static_cast<uint32_t>(wrapped);
            const double frac = wrapped - q0;
            const uint32_t q1 = (q0 + 1u) % range_bins;
            const size_t p0 = ((size_t)a * range_bins + q0) * rows + r;
            const size_t p1 = ((size_t)a * range_bins + q1) * rows + r;
            const double re = workspace.projected_real[p0]
                + frac * (workspace.projected_real[p1] - workspace.projected_real[p0]);
            const double im = workspace.projected_imag[p0]
                + frac * (workspace.projected_imag[p1] - workspace.projected_imag[p0]);
            const double sr = plan.slow_steering_real[steering + r];
            const double si = plan.slow_steering_imag[steering + r];
            coherent += std::complex<double>(re * sr - im * si, re * si + im * sr);
          }
        }
        power += std::norm(coherent);
      }
      const double score = power / denominator;
      result[(size_t)q * rows + d] = std::isfinite(score) && score > 0.0
                                        ? score : -std::numeric_limits<double>::infinity();
    }
}

struct Peak {
  uint32_t r = 0, d = 0; double score = 0.0; bool valid = false;
  // (2026-09-19) sub-cell start for refine(): quadratic interpolation of the map through the
  // 3x3 neighbourhood of the integer peak.  Cuts the Newton search from ~10 full-window GPU
  // evaluations to 2-3 (measured 3-4 ms -> ~1 ms per CLEAN iteration).  Same objective, same
  // optimum; only the starting point changes.
  double start_r = -1.0, start_d = -1.0;
};

// (2026-09-20) Sparse view of the likelihood map around the CLEAN proposal: the three Doppler
// columns d0-1, d0, d0+1 (each range_bins long) downloaded by CudaDetectorBackend::likelihood_peak.
// Values outside those columns are unavailable (-1), which the 3x3 interpolation and the
// OS-CFAR column statistic never need.
struct PeakColumns {
  uint32_t range_bins = 0, rows = 0, d0 = 0, r0 = 0;
  const std::vector<float>* columns = nullptr;
  const std::vector<float>* range_slice = nullptr;   // all Doppler cells at r0
  double value(size_t r, uint32_t d) const {
    if (r >= range_bins) return -1.0;
    if (range_slice && r == r0 && d < rows) return static_cast<double>((*range_slice)[d]);
    for (int j = -1; j <= 1; ++j) {
      const uint32_t dd = static_cast<uint32_t>(((static_cast<int64_t>(d0) + j) % static_cast<int64_t>(rows) + rows) % rows);
      if (dd == d) return static_cast<double>((*columns)[static_cast<size_t>(j + 1) * range_bins + r]);
    }
    return -1.0;
  }
};

template<typename Value>
Peak interpolate_start_impl(Peak peak, Value value, uint32_t range_bins, uint32_t rows)
{
  peak.start_r = peak.r; peak.start_d = peak.d;
  if (!peak.valid) return peak;
  auto at = [&](int64_t r, int64_t d) -> double {
    if (r < 0 || r >= (int64_t)range_bins) return -1.0;
    d = ((d % (int64_t)rows) + rows) % rows;
    const double v = value((size_t)r, (uint32_t)d);
    return (std::isfinite(v) && v > 0.0) ? v : -1.0;
  };
  // log-power quadratic fit along each axis (a Gaussian-like main lobe is quadratic in log)
  auto axis_offset = [](double lo, double c, double hi) -> double {
    if (lo <= 0.0 || hi <= 0.0 || c <= 0.0) return 0.0;
    const double a = std::log(lo), b = std::log(c), e = std::log(hi);
    const double den = a - 2.0 * b + e;
    if (!(den < 0.0)) return 0.0;
    return std::clamp(0.5 * (a - e) / den, -0.5, 0.5);
  };
  peak.start_r += axis_offset(at((int64_t)peak.r - 1, peak.d), at(peak.r, peak.d), at((int64_t)peak.r + 1, peak.d));
  peak.start_d += axis_offset(at(peak.r, (int64_t)peak.d - 1), at(peak.r, peak.d), at(peak.r, (int64_t)peak.d + 1));
  return peak;
}

Peak interpolate_start(Peak peak, const std::vector<double>& map, uint32_t range_bins, uint32_t rows)
{
  return interpolate_start_impl(peak, [&](size_t r, uint32_t d) { return map[r * rows + d]; }, range_bins, rows);
}

Peak interpolate_start(Peak peak, const PeakColumns& view)
{
  return interpolate_start_impl(peak, [&](size_t r, uint32_t d) { return view.value(r, d); }, view.range_bins, view.rows);
}

Peak strongest(const std::vector<double>& map, uint32_t rows)
{
  Peak result;
  for (size_t i = 0; i < map.size(); ++i)
    if (std::isfinite(map[i]) && map[i] > 0.0 && (!result.valid || map[i] > result.score)) {
      result.valid = true; result.score = map[i]; result.r = i / rows; result.d = i % rows;
    }
  return result;
}

struct Refined {
  double range_bin = 0.0, doppler_bin = 0.0, score = 0.0;
  uint32_t evaluations = 0;
  std::vector<std::complex<double>> steering;
  std::vector<std::complex<double>> coherent;
  Localization localization;
};

Refined refine(const std::vector<std::complex<double>>& residual,
               const CfrWindow& window,
               const Plan& plan,
               Peak coarse,
               uint32_t minimum_range_bin,
               const RateGate& gate,
               double maximum_abs_rate_mps,
               CudaDetectorBackend*& cuda_backend)
{
  const uint32_t antennas = window.antennas, rows = window.rows, subcarriers = window.subcarriers;
  const size_t cells = (size_t)rows * subcarriers;
  // (2026-09-22) Walk-consistent CLEAN: the steered map found this proposal under the model
  // r(t) = r0 + v*(t - t_mid). Refinement and subtraction must use the same model, so the per-row
  // migration is frozen here at the proposal's COARSE Doppler bin (the refined bin moves by well
  // under one cell, and freezing keeps the analytic gradients exact).
  std::vector<double> component_walk;
  if (plan.range_walk) {
    const double walk_rate = -(static_cast<double>(coarse.d) - static_cast<double>(rows / 2))
                             * plan.axes.rate_res_mps;
    component_walk.resize(rows);
    for (uint32_t r = 0; r < rows; ++r)
      component_walk[r] = walk_rate * (plan.times[r] - plan.range_walk_midpoint_s)
                          / plan.axes.range_res_m;
  }
  if (cuda_backend) cuda_backend->set_component_walk(component_walk);
  std::array<double, 2> lower{std::max<double>(minimum_range_bin, coarse.r - 0.5),
                              std::max(0.0, coarse.d - 0.5)};
  std::array<double, 2> upper{std::min<double>(plan.axes.range_bins - 1, coarse.r + 0.5),
                              std::min<double>(rows - 1, coarse.d + 0.5)};
  double rate_lower = -maximum_abs_rate_mps, rate_upper = maximum_abs_rate_mps;
  if (gate.half_width_mps) {
    rate_lower = std::max(rate_lower, *gate.center_mps - *gate.half_width_mps);
    rate_upper = std::min(rate_upper, *gate.center_mps + *gate.half_width_mps);
  }
  if (std::isfinite(rate_upper))
    lower[1] = std::max(lower[1], rows / 2.0 - rate_upper / plan.axes.rate_res_mps);
  if (std::isfinite(rate_lower))
    upper[1] = std::min(upper[1], rows / 2.0 - rate_lower / plan.axes.rate_res_mps);
  if (lower[1] > upper[1]) lower[1] = upper[1] = coarse.d;

  struct Eval {
    double objective = 0.0;
    std::array<double, 2> gradient{};
    std::array<double, 4> hessian{};
    std::complex<double> coherent{};
  };
  uint32_t evaluations = 0;
  auto evaluate = [&](const std::array<double, 2>& point, bool derivatives) {
    ++evaluations;
    if (cuda_backend) {
      const auto accelerated = cuda_backend->evaluate(point[0], point[1]);
      if (accelerated.coherent.size() != 1)
        throw std::runtime_error("canonical CUDA refinement returned a non-scalar response");
      Eval e;
      e.objective = accelerated.objective;
      e.gradient = accelerated.gradient;
      e.hessian = accelerated.hessian;
      e.coherent = accelerated.coherent.front();
      return e;
    }
    Eval e;
    std::complex<double> first_r, first_d, second_r, second_d, second_cross;
    // At fixed slow time the continuous range phase is a geometric sequence.  One trigonometric
    // evaluation per row plus complex recurrence preserves the model.  Trial evaluations retain
    // only their sufficient statistics; materializing 12,800 steering samples for every rejected
    // line-search point was the dominant CPU refinement cost.
    for (uint32_t r = 0; r < rows; ++r) {
      const std::complex<double> range_step = std::polar(
          1.0, -2.0 * PI * (point[0] + (component_walk.empty() ? 0.0 : component_walk[r]))
                   / subcarriers);
      std::complex<double> steering = std::polar(
          1.0, plan.doppler_phase_rate[r] * (point[1] - static_cast<int>(rows / 2)));
      for (uint32_t k = 0; k < subcarriers; ++k) {
        const size_t cell = (size_t)r * subcarriers + k;
        const double sr = plan.range_phase_rate[k];
        const double sd = plan.doppler_phase_rate[r];
        const auto projected = plan.weights[cell] * residual[cell] * std::conj(steering);
        e.coherent += projected;
        if (derivatives) {
          first_r += std::complex<double>(0.0, -1.0) * projected * sr;
          first_d += std::complex<double>(0.0, -1.0) * projected * sd;
          second_r -= projected * sr * sr;
          second_d -= projected * sd * sd;
          second_cross -= projected * sr * sd;
        }
        steering *= range_step;
      }
    }
    e.objective = std::norm(e.coherent);
    if (!derivatives) return e;
    e.gradient[0] = 2.0 * std::real(std::conj(e.coherent) * first_r);
    e.gradient[1] = 2.0 * std::real(std::conj(e.coherent) * first_d);
    e.hessian[0] = 2.0 * std::real(std::conj(first_r) * first_r
                                    + std::conj(e.coherent) * second_r);
    const double cross_value = 2.0 * std::real(std::conj(first_d) * first_r
                                               + std::conj(e.coherent) * second_cross);
    e.hessian[1] = cross_value; e.hessian[2] = cross_value;
    e.hessian[3] = 2.0 * std::real(std::conj(first_d) * first_d
                                    + std::conj(e.coherent) * second_d);
    return e;
  };

  std::array<double, 2> point{std::clamp<double>(coarse.start_r >= 0.0 ? coarse.start_r : coarse.r, lower[0], upper[0]),
                              std::clamp<double>(coarse.start_d >= 0.0 ? coarse.start_d : coarse.d, lower[1], upper[1])};
  Eval current = evaluate(point, true);
  std::string convergence = "coarse_stationary";
  uint32_t iterations = 0;
  // The CFR enters through OAI as complex<float>; optimizing below that representation's
  // resolution only burns deadline while fitting quantization residue.
  constexpr uint32_t iteration_guard = std::numeric_limits<float>::digits;
  for (; iterations < iteration_guard; ++iterations) {
    const double info_a = -current.hessian[0], info_b = -0.5 * (current.hessian[1] + current.hessian[2]);
    const double info_d = -current.hessian[3];
    const auto eigen = eigenvalues_symmetric_2x2(info_a, info_b, info_d);
    // (2026-09-19) Declared numerical tolerance: 1e-3 cell (3 mm range, 1 mm/s rate), two
    // orders of magnitude below the measurement error, instead of float epsilon (1e-7 cell)
    // which cost 2 extra full-window GPU evaluations (0.42 ms each) per CLEAN iteration.
    const double tolerance = 1e-3;
    std::array<double, 2> step{};
    if (std::isfinite(eigen[0]) && eigen[0] > 0.0) {
      const double det = current.hessian[0] * current.hessian[3]
                         - current.hessian[1] * current.hessian[2];
      if (std::abs(det) <= std::numeric_limits<double>::min()) { convergence = "singular_local_likelihood"; break; }
      step[0] = (-current.gradient[0] * current.hessian[3]
                 + current.hessian[1] * current.gradient[1]) / det;
      step[1] = (current.hessian[2] * current.gradient[0]
                 - current.hessian[0] * current.gradient[1]) / det;
    } else {
      const double gn = std::hypot(current.gradient[0], current.gradient[1]);
      if (!std::isfinite(gn) || gn <= tolerance) { convergence = "nonconcave_stationary_likelihood"; break; }
      std::array<double, 2> direction{current.gradient[0] / gn, current.gradient[1] / gn};
      double feasible = std::numeric_limits<double>::infinity();
      for (size_t axis = 0; axis < 2; ++axis) {
        double value = std::numeric_limits<double>::infinity();
        if (direction[axis] > 0.0) value = (upper[axis] - point[axis]) / direction[axis];
        else if (direction[axis] < 0.0) value = (lower[axis] - point[axis]) / direction[axis];
        if (value > 0.0) feasible = std::min(feasible, value);
      }
      if (!std::isfinite(feasible)) { convergence = "nonconcave_box_stationary"; break; }
      step = {direction[0] * feasible, direction[1] * feasible};
    }
    if (!std::isfinite(step[0]) || !std::isfinite(step[1]) || std::hypot(step[0], step[1]) <= tolerance) {
      convergence = "likelihood_stationary"; break;
    }
    bool accepted = false;
    for (double scale = 1.0; scale >= std::numeric_limits<double>::epsilon(); scale *= 0.5) {
      const std::array<double, 2> candidate{
          std::clamp(point[0] + scale * step[0], lower[0], upper[0]),
          std::clamp(point[1] + scale * step[1], lower[1], upper[1])};
      if (candidate == point
          || std::hypot(candidate[0] - point[0], candidate[1] - point[1]) <= tolerance)
        break;
      // The full Hessian is useful only for an accepted point.  Preserve the common one-step
      // Newton fast path, while evaluating rejected backtracking points with their objective-only
      // sufficient statistic.
      const bool have_trial_derivatives = cuda_backend || scale == 1.0;
      Eval trial = evaluate(candidate, have_trial_derivatives);
      const double representable_improvement = std::numeric_limits<float>::epsilon()
                                               * std::max(std::abs(current.objective),
                                                          std::numeric_limits<double>::min());
      if (trial.objective > current.objective + representable_improvement) {
        if (!have_trial_derivatives)
          trial = evaluate(candidate, true);
        point = candidate; current = std::move(trial); accepted = true; break;
      }
    }
    if (!accepted) { convergence = "no_representable_likelihood_improvement"; ++iterations; break; }
  }
  if (iterations == iteration_guard) convergence = "floating_point_iteration_guard";

  Refined out;
  out.range_bin = point[0]; out.doppler_bin = point[1];
  out.coherent = {current.coherent};
  out.score = current.objective / std::max(plan.denominator * antennas, std::numeric_limits<double>::min());
  out.localization.iterations = iterations;
  out.evaluations = evaluations;
  out.localization.convergence = convergence;

  const uint64_t observed_cells = std::count_if(plan.weights.begin(), plan.weights.end(), [](double w){ return w > 0.0; });
  double fit_energy = 0.0, signal_scale = 0.0;
  if (cuda_backend && out.steering.empty()) {
    // Weighted least squares with unit-magnitude steering removes ||coherent||^2/sum(weights).
    // Keep the unweighted residual scale separately because unobserved cells can hold fitted
    // components after prior CLEAN iterations, exactly as in the Python CUDA implementation.
    fit_energy = std::max(0.0, cuda_backend->weighted_energy()
                                  - current.objective
                                        / std::max(plan.denominator,
                                                   std::numeric_limits<double>::min()));
    signal_scale = cuda_backend->unweighted_energy()
                   / std::max<uint64_t>(1, (uint64_t)antennas * observed_cells);
  } else {
    out.steering.resize(cells);
    const auto alpha = out.coherent.front()
        / std::max(plan.denominator, std::numeric_limits<double>::min());
    for (uint32_t r = 0; r < rows; ++r) {
      const std::complex<double> range_step = std::polar(
          1.0, -2.0 * PI * (point[0] + (component_walk.empty() ? 0.0 : component_walk[r]))
                   / subcarriers);
      std::complex<double> steering = std::polar(
          1.0, plan.doppler_phase_rate[r] * (point[1] - static_cast<int>(rows / 2)));
      for (uint32_t k = 0; k < subcarriers; ++k) {
        const size_t cell = (size_t)r * subcarriers + k;
        out.steering[cell] = steering;
        fit_energy += plan.weights[cell] * std::norm(residual[cell] - alpha * steering);
        signal_scale += std::norm(residual[cell]);
        steering *= range_step;
      }
    }
    signal_scale /= std::max<uint64_t>(1, (uint64_t)antennas * observed_cells);
  }
  const uint64_t dof = std::max<uint64_t>(1, (uint64_t)antennas * observed_cells - antennas - 2);
  double variance = fit_energy / dof;
  variance = std::max(variance, std::numeric_limits<float>::epsilon() * std::numeric_limits<float>::epsilon()
                                * std::max(signal_scale, std::numeric_limits<double>::min()));
  const double scale = std::max(plan.denominator * variance, std::numeric_limits<double>::min());
  const double ia = -current.hessian[0] / scale;
  const double ib = -0.5 * (current.hessian[1] + current.hessian[2]) / scale;
  const double id = -current.hessian[3] / scale;
  const auto ie = eigenvalues_symmetric_2x2(ia, ib, id);
  if (std::isfinite(ie[0]) && ie[0] > 0.0) {
    Matrix info(2, 2); info(0,0)=ia; info(0,1)=info(1,0)=ib; info(1,1)=id;
    out.localization.covariance_bins = inverse(info);
    Matrix jac(2,2); jac(0,0)=plan.axes.range_res_m; jac(1,1)=-plan.axes.rate_res_mps;
    out.localization.covariance_range_rate = jac * out.localization.covariance_bins * jac.transposed();
    // OUR ADAPTATION: the Cramer-Rao bound above is only valid for a CORRECTLY SPECIFIED model.
    // This one is not: after subtracting a single point component the residual is not white noise
    // over ~antennas*observed_cells independent samples, it is the rest of an extended object
    // (other scatterers, micro-Doppler) plus clutter -- structured and correlated. The bound is
    // consequently far too tight. Measured against ground truth on the frozen four-RX corpus, the
    // reported sigma was 2.7-4.1 mm while the actual range error was 0.34 m (drone), 0.74 m (bike)
    // and 1.51 m (car): overconfident by a factor of 76-216.
    //
    // No correction to the bound fixes a misspecified model (reducing the degrees of freedom to
    // the detector's own effective_hypotheses only inflates sigma ~17x, still 5-23x too tight), so
    // the value is floored here AT SOURCE rather than left for each consumer to clamp. The floor is
    // the resolution cell's own uniform-distribution variance, res^2/12 -- the same convention
    // measurement_quality() already applies downstream, derived from declared resolution only, not
    // fitted to any capture. An extended object's reference point cannot be known better than the
    // cell it occupies, however sharp the fitted peak happens to be.
    //
    // NOTE (OTA): a floor is a LOWER bound on uncertainty, not an estimate. Real channel
    // estimation noise, lower SNR and multipath bias can push the true error well past one cell,
    // and this floor will then be optimistic. The adaptive term for that is the per-receiver
    // temporal residual computed in the spatial stage, not anything available here.
    // Floor the BIN-domain covariance, not just the derived range/rate one: collapse_unresolved()
    // rebuilds a merged component's covariance from covariance_bins, so flooring only the derived
    // matrix would leave every merged component unfloored.
    {
      const double bin_floor = 1.0 / 12.0;   // one resolution cell, uniform, expressed in bins
      out.localization.covariance_bins(0, 0) =
          std::max(out.localization.covariance_bins(0, 0), bin_floor);
      out.localization.covariance_bins(1, 1) =
          std::max(out.localization.covariance_bins(1, 1), bin_floor);
      out.localization.covariance_range_rate =
          jac * out.localization.covariance_bins * jac.transposed();
    }
    out.localization.covariance_valid = true;
  }
  return out;
}

uint32_t first_psf_minimum(const std::vector<double>& values)
{
  if (values.size() < 2)
    throw std::invalid_argument("PSF slice needs at least two samples");
  const double numerical_zero = std::numeric_limits<double>::epsilon()
                                * std::max(1.0, std::abs(values.front()))
                                * values.size();
  // A full rectangular allocation has analytical zeros at integer-bin offsets. Roundoff can make
  // later zeros microscopically smaller and defeat a strict local-minimum comparison, inflating
  // the measured direct-path exclusion support. Prefer the first numerical zero.
  // (2026-09-18) With a tapered PSF there is no exact zero near the main lobe; the first
  // numerical zero can then lie far out in the underflowing sidelobes (measured: Doppler
  // half-width 75 = whole axis).  Take whichever comes first: numerical zero or local minimum.
  uint32_t zero_index = 0, minimum_index = 0;
  for (size_t i = 1; i < values.size(); ++i)
    if (std::abs(values[i]) <= numerical_zero) { zero_index = static_cast<uint32_t>(i); break; }
  for (size_t i = 1; i + 1 < values.size(); ++i)
    if (values[i] <= values[i - 1] && values[i] <= values[i + 1]) {
      minimum_index = static_cast<uint32_t>(i); break;
    }
  if (zero_index && minimum_index) return std::min(zero_index, minimum_index);
  if (zero_index) return zero_index;
  if (minimum_index) return minimum_index;
  return static_cast<uint32_t>(
      std::min_element(values.begin() + 1, values.end()) - values.begin());
}

struct PsfSupport {
  uint32_t range_halfwidth = 0;
  uint32_t doppler_halfwidth = 0;
  // (2026-09-22) MEASURED Doppler point-spread profile, normalised to its own main lobe:
  // doppler_psf_ratio[k] is the fraction of a point target's power that appears k Doppler bins
  // away from it, at the SAME range. This is what a Doppler skirt of an already-accepted component
  // is predicted to look like (see the skirt rule in the CLEAN loop). Index 0 is the main lobe.
  std::vector<double> doppler_psf_ratio;
  double doppler_sidelobe_ratio = 1.0;   // max of doppler_psf_ratio outside the main lobe
  uint64_t searched_cells = 0;
  uint64_t resolution_cells = 0;
  uint64_t effective_hypotheses = 0;
};

PsfSupport measured_psf_support_uncached(const Plan& plan, uint32_t rows, uint32_t subcarriers);

// (2026-09-19) The PSF depends only on the weights (taper x observed mask) and the row times;
// cache by a checksum of those so an unchanged allocation shape does not pay ~20 ms per CPI.
PsfSupport measured_psf_support(const Plan& plan, uint32_t rows, uint32_t subcarriers)
{
  static std::mutex mutex;
  static std::map<std::array<double, 4>, PsfSupport> cache;
  double wsum = 0.0, wsq = 0.0, tsum = 0.0;
  for (size_t i = 0; i < plan.weights.size(); ++i) { wsum += plan.weights[i]; wsq += plan.weights[i] * (double)(i % 977); }
  for (double t : plan.times) tsum += t;
  const std::array<double, 4> key{(double)rows * 1e6 + subcarriers, wsum, wsq, tsum + plan.axes.range_bins * 1e3};
  std::lock_guard<std::mutex> lock(mutex);
  auto found = cache.find(key);
  if (found != cache.end()) return found->second;
  if (cache.size() > 64) cache.clear();
  return cache.emplace(key, measured_psf_support_uncached(plan, rows, subcarriers)).first->second;
}

PsfSupport measured_psf_support_uncached(const Plan& plan, uint32_t rows, uint32_t subcarriers)
{
  std::vector<double> column_weight(subcarriers), row_weight(rows);
  for (uint32_t r = 0; r < rows; ++r)
    for (uint32_t k = 0; k < subcarriers; ++k) {
      const double weight = plan.weights[(size_t)r * subcarriers + k];
      column_weight[k] += weight;
      row_weight[r] += weight;
    }
  if (!(plan.denominator > 0.0))
    throw std::invalid_argument("measured PSF needs positive observed weight");

  std::vector<double> range_psf(std::max<uint32_t>(2, plan.axes.range_bins));
  for (uint32_t offset = 0; offset < range_psf.size(); ++offset) {
    std::complex<double> coherent;
    for (uint32_t k = 0; k < subcarriers; ++k) {
      const double phase = 2.0 * PI * k * offset / subcarriers;
      coherent += column_weight[k] * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    range_psf[offset] = std::norm(coherent) / (plan.denominator * plan.denominator);
  }

  const uint32_t maximum_doppler_offset = std::max<uint32_t>(1, rows / 2);
  std::vector<double> doppler_psf(maximum_doppler_offset + 1);
  for (uint32_t offset = 0; offset <= maximum_doppler_offset; ++offset) {
    std::complex<double> coherent;
    const double rate_difference = offset * plan.axes.rate_res_mps;
    for (uint32_t r = 0; r < rows; ++r) {
      const double phase = 2.0 * PI * rate_difference * plan.times[r] / plan.axes.rate_res_mps
                           * (rows - 1.0) / (rows * plan.axes.dwell_s);
      coherent += row_weight[r] * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    doppler_psf[offset] = std::norm(coherent) / (plan.denominator * plan.denominator);
  }

  PsfSupport result;
  result.range_halfwidth = first_psf_minimum(range_psf);
  result.doppler_halfwidth = first_psf_minimum(doppler_psf);
  {   // normalised Doppler PSF profile and its peak sidelobe
    const double peak = doppler_psf.empty() ? 0.0 : doppler_psf[0];
    result.doppler_psf_ratio.assign(doppler_psf.size(), 1.0);
    double sidelobe = 0.0;
    if (peak > 0.0) {
      for (size_t offset = 0; offset < doppler_psf.size(); ++offset)
        result.doppler_psf_ratio[offset] = std::min(1.0, doppler_psf[offset] / peak);
      for (uint32_t offset = result.doppler_halfwidth + 1; offset < doppler_psf.size(); ++offset)
        sidelobe = std::max(sidelobe, result.doppler_psf_ratio[offset]);
    }
    result.doppler_sidelobe_ratio = peak > 0.0 ? sidelobe : 1.0;
  }
  const uint64_t searched_rate_bins = std::count_if(
      plan.rate_allowed.begin(), plan.rate_allowed.end(), [](uint8_t allowed) { return allowed != 0; });
  result.searched_cells = static_cast<uint64_t>(plan.axes.range_bins) * searched_rate_bins;
  result.resolution_cells = static_cast<uint64_t>(2 * result.range_halfwidth + 1)
                            * (2 * result.doppler_halfwidth + 1);
  result.effective_hypotheses = std::max<uint64_t>(
      1, (result.searched_cells + result.resolution_cells - 1) / result.resolution_cells);
  return result;
}

LocalStatistic adaptive_local_statistic(const std::vector<double>& likelihood,
                                        const Plan& plan,
                                        const PsfSupport& psf,
                                        uint32_t cut_range,
                                        uint32_t cut_doppler)
{
  /* The guard is the measured main-lobe support, rather than a configured number of bins.  Grow
   * the local training rectangle until it contains a data-sized robust sample (sqrt of the valid
   * search lattice), or until the complete lattice is reached.  This preserves the Python CLEAN
   * pipeline's CUT-excluded log-median/MAD statistic without its old fixed 12/2 bin radii. */
  const uint64_t valid_cells = std::count_if(
      likelihood.begin(), likelihood.end(), [](double value) {
        return std::isfinite(value) && value > 0.0;
      });
  // A two-dimensional robust background needs more than a one-dimensional sqrt(N) slice. N^(2/3)
  // grows with the observed lattice while leaving an asymptotically vanishing local fraction.
  const uint64_t desired = std::max<uint64_t>(
      2 * 2 * 2 * 2,
      static_cast<uint64_t>(std::ceil(std::pow(static_cast<double>(valid_cells), 2.0 / 3.0))));
  // The first-minimum halfwidth resolves objects.  Twice that measured support keeps the complete
  // off-grid main-lobe diameter (including fractional-bin leakage) out of nuisance training.
  const uint32_t guard_range = std::min<uint32_t>(
      plan.axes.range_bins - 1, 2 * psf.range_halfwidth);
  const uint32_t guard_doppler = std::min<uint32_t>(
      plan.axes.rate_bins / 2, 2 * psf.doppler_halfwidth);
  uint32_t train_range = guard_range;
  uint32_t train_doppler = guard_doppler;
  LocalStatistic result;
  for (;;) {
    // (2026-09-18) Training exclusions of the zero-Doppler bin / CUT range bin were tried and
    // measured NOT to remove the ring contamination (true-cell z 2.5 -> 2.8 only); the
    // contamination is sidelobe leakage, addressed at the source by the taper in prepare().
    // The API keeps the optional exclusions; production leaves them off (one change at a time).
    result = cut_excluded_local_statistic(
        likelihood, plan.axes.range_bins, plan.axes.rate_bins, cut_range, cut_doppler,
        train_range, train_doppler, guard_range, guard_doppler);
    if (!result.fallback_global && result.training_cells >= desired)
      return result;
    const bool range_complete = train_range >= plan.axes.range_bins - 1;
    const bool doppler_complete = train_doppler >= plan.axes.rate_bins / 2;
    if (range_complete && doppler_complete) return result;
    // Expand the axis with fewer PSF-resolution cells first, keeping the neighborhood local.
    const double normalized_range = static_cast<double>(train_range + 1)
                                    / std::max<uint32_t>(1, guard_range + 1);
    const double normalized_doppler = static_cast<double>(train_doppler + 1)
                                      / std::max<uint32_t>(1, guard_doppler + 1);
    if (!range_complete && (doppler_complete || normalized_range <= normalized_doppler))
      ++train_range;
    else
      ++train_doppler;
  }
}


/* OUR ADAPTATION (stage-6 replacement, 2026-09-18): per-Doppler, range-trained cell-averaging
 * statistic with the exponential (Rayleigh-power) null.
 *
 * Measured facts that drove this: after stage 3 and the Chebyshev taper the map background is
 * a Doppler-dependent skirt (+39 dB at 0, +23 at +-4 bins, +12 at +-12, 0 at +-48; identical on
 * all receivers), i.e. a slowly varying texture in Doppler.  A 2-D training ring straddles that
 * gradient and inflates the log-MAD sigma to ~2 nats, so a target 30-45 dB above its own Doppler
 * row read as z~2-4.  The physically standard model is compound-Gaussian: texture x Gaussian
 * speckle.  Normalising by the local mean AT THE SAME DOPPLER (estimated across range) leaves an
 * exponential ratio, so the decision is  cell / mean > ln(N_hyp / budget)  -- no log domain, no
 * Gaussian, no fitted constant.  Background mean = median / ln 2 (exponential property) so that a
 * second object in the same row does not bias it.  The Gaussian-on-log-power rule it replaces was
 * ~10 dB conservative even on pure noise (191 hyp: 19.7 dB demanded vs 9.9 dB physical).
 * Fields reused: z = ratio, log_background_median = log(mean) so null_scale stays consistent. */
template<typename Value>
LocalStatistic doppler_row_ca_statistic_impl(Value value, const Plan& plan, const PsfSupport& psf,
                                             uint32_t cut_range, uint32_t cut_doppler)
{
  LocalStatistic result;
  const uint32_t guard = 2 * psf.range_halfwidth;
  std::vector<double> values;
  values.reserve(plan.axes.range_bins);
  for (uint32_t q = 0; q < plan.axes.range_bins; ++q) {
    if ((q > cut_range ? q - cut_range : cut_range - q) <= guard) continue;
    const double v = value((size_t)q, cut_doppler);
    if (std::isfinite(v) && v > 0.0) values.push_back(v);
  }
  result.cut_score = value((size_t)cut_range, cut_doppler);
  result.training_cells = static_cast<uint32_t>(values.size());
  if (values.size() < 16 || !(std::isfinite(result.cut_score) && result.cut_score > 0.0))
    return result;   // valid=false: no decision without a data-sized background
  const double order_statistic = median(values);          // k = N/2 order statistic (OS-CFAR)
  if (!(order_statistic > 0.0)) return result;
  result.log_background_median = std::log(order_statistic / std::log(2.0));   // exponential mean
  result.log_background_sigma = 1.0;
  result.z = result.cut_score / order_statistic;     // compared with os_cfar_multiplier(N, alpha)
  result.valid = true;
  return result;
}

// (2026-09-20) Range-column background: the same statistic along DOPPLER at the CUT's range bin,
// guard = 2 x PSF Doppler half-width.  The Doppler skirt of a strong component (its residue after
// CLEAN subtraction spreads along Doppler at fixed range) is invisible to the Doppler-row test,
// whose training cells at other ranges are clean; it dominates the column.  Measured (33 m car,
// UL): 64 % of false detections lie within 4 range cells of the strongest component of the same
// receiver-CPI, at Doppler offsets of 7-35 cells.  Greatest-of CFAR = the CUT must exceed both
// backgrounds (conventional GO-CFAR, same alpha, no new constant).
template<typename Value>
LocalStatistic range_column_statistic_impl(Value value, const Plan& plan, const PsfSupport& psf,
                                           uint32_t cut_range, uint32_t cut_doppler)
{
  LocalStatistic result;
  const uint32_t nd = plan.axes.rate_bins;
  const uint32_t guard = 2 * psf.doppler_halfwidth;
  std::vector<double> values; values.reserve(nd);
  for (uint32_t d = 0; d < nd; ++d) {
    const uint32_t delta = std::min((d + nd - cut_doppler) % nd, (cut_doppler + nd - d) % nd);
    if (delta <= guard) continue;
    const double v = value((size_t)cut_range, d);
    if (std::isfinite(v) && v > 0.0) values.push_back(v);
  }
  result.cut_score = value((size_t)cut_range, cut_doppler);
  result.training_cells = static_cast<uint32_t>(values.size());
  {   // strongest cell anywhere in this range column: the candidate parent of a Doppler sidelobe
    double column_peak = 0.0;
    for (uint32_t d = 0; d < nd; ++d) {
      const double v = value((size_t)cut_range, d);
      if (std::isfinite(v) && v > column_peak) column_peak = v;
    }
    result.column_peak = column_peak;
  }
  if (values.size() < 16 || !(std::isfinite(result.cut_score) && result.cut_score > 0.0)) return result;
  const double order_statistic = median(values);
  if (!(order_statistic > 0.0)) return result;
  result.log_background_median = std::log(order_statistic / std::log(2.0));
  result.log_background_sigma = 1.0;
  result.z = result.cut_score / order_statistic;
  result.valid = true;
  return result;
}

static bool greatest_of_cfar()
{
  static const bool value = [] { const char* v = std::getenv("NR_ISAC_CFAR_GREATEST_OF"); return !(v && *v == '0'); }();
  return value;
}

LocalStatistic range_column_statistic(const std::vector<double>& likelihood, const Plan& plan,
                                      const PsfSupport& psf, uint32_t cut_range, uint32_t cut_doppler)
{
  const uint32_t nd = plan.axes.rate_bins;
  return range_column_statistic_impl([&](size_t r, uint32_t d) { return likelihood[r * nd + d]; }, plan, psf, cut_range, cut_doppler);
}

LocalStatistic range_column_statistic(const PeakColumns& view, const Plan& plan,
                                      const PsfSupport& psf, uint32_t cut_range, uint32_t cut_doppler)
{
  return range_column_statistic_impl([&](size_t r, uint32_t d) { return view.value(r, d); }, plan, psf, cut_range, cut_doppler);
}

LocalStatistic doppler_row_ca_statistic(const std::vector<double>& likelihood, const Plan& plan,
                                        const PsfSupport& psf, uint32_t cut_range, uint32_t cut_doppler)
{
  const uint32_t nd = plan.axes.rate_bins;
  return doppler_row_ca_statistic_impl([&](size_t r, uint32_t d) { return likelihood[r * nd + d]; }, plan, psf, cut_range, cut_doppler);
}

LocalStatistic doppler_row_ca_statistic(const PeakColumns& view, const Plan& plan,
                                        const PsfSupport& psf, uint32_t cut_range, uint32_t cut_doppler)
{
  return doppler_row_ca_statistic_impl([&](size_t r, uint32_t d) { return view.value(r, d); }, plan, psf, cut_range, cut_doppler);
}

/* Exact finite-sample OS-CFAR multiplier (Rohling 1983) for exponential cells: with N reference
 * cells and the k-th order statistic, P_fa(T) = prod_{i=0}^{k-1} (N-i)/(N-i+T).  Solved for T by
 * bisection at the declared per-cell alpha.  This replaces the asymptotic ln(1/alpha), which
 * measured 7.8% instead of 1.2% on pure noise because the reference is estimated from ~95 cells. */
double os_cfar_multiplier(uint32_t n, double alpha)
{
  const uint32_t k = std::max<uint32_t>(1, n / 2);
  auto pfa = [&](double t) {
    double p = 1.0;
    for (uint32_t i = 0; i < k; ++i) p *= static_cast<double>(n - i) / (static_cast<double>(n - i) + t);
    return p;
  };
  double lo = 0.0, hi = 1.0;
  while (pfa(hi) > alpha && hi < 1e12) hi *= 2.0;
  for (int it = 0; it < 200 && hi - lo > 1e-9 * std::max(1.0, hi); ++it) {
    const double mid = 0.5 * (lo + hi);
    (pfa(mid) > alpha ? lo : hi) = mid;
  }
  return hi;
}

double circular_bin_difference(double left, double right, uint32_t period)
{
  const double difference = std::abs(left - right);
  return std::min(difference, period - difference);
}

std::array<double, 2> component_scale(const CleanComponent& component,
                                      const PsfSupport& psf,
                                      uint32_t rate_bins)
{
  double range = psf.range_halfwidth;
  double doppler = psf.doppler_halfwidth;
  if (component.localization.covariance_valid) {
    range = std::max(range, std::sqrt(std::max(0.0, component.localization.covariance_bins(0, 0))));
    doppler = std::max(doppler, std::sqrt(std::max(0.0, component.localization.covariance_bins(1, 1))));
  }
  const double epsilon = std::numeric_limits<double>::epsilon();
  return {std::max(range, epsilon), std::min(std::max(doppler, epsilon), rate_bins / 2.0)};
}

std::vector<CleanComponent> collapse_unresolved(std::vector<CleanComponent> remaining,
                                                const Plan& plan,
                                                const PsfSupport& psf)
{
  std::stable_sort(remaining.begin(), remaining.end(),
                   [](const auto& left, const auto& right) { return left.score > right.score; });
  std::vector<CleanComponent> objects;
  while (!remaining.empty()) {
    const CleanComponent anchor = remaining.front();
    const auto anchor_scale = component_scale(anchor, psf, plan.axes.rate_bins);
    std::vector<size_t> group;
    std::vector<CleanComponent> next;
    for (size_t i = 0; i < remaining.size(); ++i) {
      const auto scale = component_scale(remaining[i], psf, plan.axes.rate_bins);
      const double range_scale = std::hypot(anchor_scale[0], scale[0]);
      const double doppler_scale = std::hypot(anchor_scale[1], scale[1]);
      const double dr = std::abs(remaining[i].range_bin - anchor.range_bin);
      const double dd = circular_bin_difference(
          remaining[i].doppler_bin, anchor.doppler_bin, plan.axes.rate_bins);
      if (std::pow(dr / range_scale, 2.0) + std::pow(dd / doppler_scale, 2.0) <= 1.0)
        group.push_back(i);
      else
        next.push_back(std::move(remaining[i]));
    }
    CleanComponent object = anchor;
    double energy = 0.0;
    for (size_t index : group)
      energy += remaining[index].score;
    if (energy > 0.0) {
      object.range_bin = 0.0;
      double doppler = 0.0;
      for (size_t index : group) {
        const double probability = remaining[index].score / energy;
        object.range_bin += probability * remaining[index].range_bin;
        double unwrapped = remaining[index].doppler_bin;
        const double delta = unwrapped - anchor.doppler_bin;
        if (delta > plan.axes.rate_bins / 2.0) unwrapped -= plan.axes.rate_bins;
        else if (delta < -static_cast<double>(plan.axes.rate_bins) / 2.0) unwrapped += plan.axes.rate_bins;
        doppler += probability * unwrapped;
      }
      object.doppler_bin = std::fmod(doppler + plan.axes.rate_bins, plan.axes.rate_bins);
      std::complex<double> coefficient;
      object.component_lineage.clear();
      for (size_t index : group) {
        const double probability = remaining[index].score / energy;
        coefficient += probability * remaining[index].complex_coefficient;
        object.component_lineage.insert(object.component_lineage.end(),
                                        remaining[index].component_lineage.begin(),
                                        remaining[index].component_lineage.end());
      }
      object.complex_coefficient = coefficient;
      object.amplitude_abs = std::abs(coefficient);
      object.amplitude_phase_rad = std::arg(coefficient);
      bool covariance = !group.empty();
      for (size_t index : group)
        covariance = covariance && remaining[index].localization.covariance_valid;
      if (covariance) {
        Matrix mixture(2, 2);
        for (size_t index : group) {
          const double probability = remaining[index].score / energy;
          const double dr = remaining[index].range_bin - object.range_bin;
          double dd = remaining[index].doppler_bin - object.doppler_bin;
          if (dd > plan.axes.rate_bins / 2.0) dd -= plan.axes.rate_bins;
          else if (dd < -static_cast<double>(plan.axes.rate_bins) / 2.0) dd += plan.axes.rate_bins;
          mixture(0, 0) += probability * (remaining[index].localization.covariance_bins(0, 0) + dr * dr);
          mixture(0, 1) += probability * (remaining[index].localization.covariance_bins(0, 1) + dr * dd);
          mixture(1, 0) += probability * (remaining[index].localization.covariance_bins(1, 0) + dd * dr);
          mixture(1, 1) += probability * (remaining[index].localization.covariance_bins(1, 1) + dd * dd);
        }
        object.localization.covariance_bins = mixture;
        Matrix jacobian(2, 2);
        jacobian(0, 0) = plan.axes.range_res_m;
        jacobian(1, 1) = -plan.axes.rate_res_mps;
        object.localization.covariance_range_rate = jacobian * mixture * jacobian.transposed();
        object.localization.covariance_valid = true;
      }
    }
    object.object_component_count = static_cast<uint32_t>(group.size());
    objects.push_back(std::move(object));
    remaining = std::move(next);
  }
  return objects;
}

// OUR ADAPTATION: collapse_unresolved above already merges components that are close in BOTH
// range and Doppler (its existing covariance/PSF ellipse test) -- that behavior is unchanged.
// What it does not do is relate components that share range support but do NOT share Doppler
// support: those already survive collapse_unresolved as fully independent objects, with no
// record that they came from the same range cell as a stronger component there. That is exactly
// the shape of a genuine rigid-body-plus-micro-Doppler-sideband pair (same physical location,
// different instantaneous velocity component) -- and, with no marker, a downstream consumer
// (e.g. a tracker) cannot tell that apart from two genuinely different, co-range objects.
// This pass adds that marker without changing any component's own range/Doppler/amplitude/
// covariance and without changing which components collapse_unresolved already merged: for each
// object, in descending score order, objects whose RANGE (not Doppler) falls within the
// stronger object's own already-computed range PSF/covariance scale are tagged as sharing that
// object's range family via bulk_component_iteration = the stronger object's `iteration`. The
// strongest object in a family is left untagged (bulk_component_iteration stays -1). A tagged
// component is not removed, merged, or otherwise altered -- this is metadata only.
void tag_micro_doppler_families(std::vector<CleanComponent>& objects,
                                const Plan& plan, const PsfSupport& psf)
{
  std::vector<size_t> order(objects.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return objects[a].score > objects[b].score;
  });
  std::vector<uint8_t> claimed(objects.size(), 0);
  for (size_t oi : order) {
    if (claimed[oi]) continue;
    const auto anchor_scale = component_scale(objects[oi], psf, plan.axes.rate_bins);
    for (size_t oj : order) {
      if (oj == oi || claimed[oj] || objects[oj].bulk_component_iteration >= 0) continue;
      const auto scale = component_scale(objects[oj], psf, plan.axes.rate_bins);
      double range_scale = std::hypot(anchor_scale[0], scale[0]);
      // (2026-09-22, NR_ISAC_FAMILY_MAX_RANGE_BINS) The tagging radius comes from the components'
      // localisation covariance, which for weak/partial-band components spans several bins: measured
      // in the 3-persons scene, a person 24-30 m (8-10 bins) from the strongest object was tagged as
      // its bulk component in 42 of 75 multi-receiver CPIs and vanished from stage 8. A family is a
      // physical object, so its range extent is bounded by the largest object length (car 4.5 m ->
      // 1.5 cells); the bound is a declared physical limit, not a fitted threshold.
      if (family_max_range_bins() > 0.0) range_scale = std::min(range_scale, family_max_range_bins());
      const double dr = std::abs(objects[oj].range_bin - objects[oi].range_bin);
      if (dr <= range_scale) {
        objects[oj].bulk_component_iteration = static_cast<int32_t>(objects[oi].iteration);
        claimed[oj] = 1;
      }
    }
  }
}

void validate_cross_fold_support(std::vector<CleanComponent>& objects,
                                 DetectorResult& result,
                                 const CfrWindow& window,
                                 const Plan& plan,
                                 double false_object_budget)
{
  if (objects.empty()) return;
  std::vector<uint8_t> fold(window.rows);
  std::map<std::string, uint32_t> occurrences;
  for (uint32_t row = 0; row < window.rows; ++row) {
    std::string key;
    key.resize(sizeof(uint32_t) + sizeof(int32_t) + window.subcarriers);
    const int32_t fraction = static_cast<int32_t>(
        std::llround(window.row_slot_frac[row] * 28.0));
    std::memcpy(key.data(), &window.row_source_mask[row], sizeof(uint32_t));
    std::memcpy(key.data() + sizeof(uint32_t), &fraction, sizeof(int32_t));
    std::memcpy(key.data() + sizeof(uint32_t) + sizeof(int32_t),
                window.observed.data() + window.cell(row, 0), window.subcarriers);
    fold[row] = occurrences[key]++ & 1U;
  }
  // Count every searched lattice point. The PSF guard area is an object-resolution support,
  // not a divisor for the number of noise trials (full rectangular DFT cells are orthogonal).
  // This remains a plug-in consistency test: coordinates were selected on both folds and the
  // scale is estimated. Do not advertise a calibrated end-to-end false-track probability.
  const double threshold = -0.5 * std::log(
      false_object_budget / std::max<uint64_t>(1, result.searched_cells));
  const double noise_scale = result.null_scale;
  std::vector<CleanComponent> accepted;
  accepted.reserve(objects.size());
  for (CleanComponent& object : objects) {
    std::array<std::vector<std::complex<double>>, 2> coherent{
        std::vector<std::complex<double>>(window.antennas),
        std::vector<std::complex<double>>(window.antennas)};
    std::array<uint64_t, 2> observed{};
    std::array<double, 2> weight_sum{};
    const std::complex<double> range_step = std::polar(
        1.0, -2.0 * PI * object.range_bin / window.subcarriers);
    for (uint32_t row = 0; row < window.rows; ++row) {
      std::complex<double> steering = std::polar(
          1.0, plan.doppler_phase_rate[row]
                   * (object.doppler_bin - static_cast<int>(window.rows / 2)));
      // (2026-09-19 fix) Weight the fold matched filter with the SAME plan.weights (taper) the
      // raster and the fit use.  Unweighted, this filter has a rectangular main lobe with nulls
      // at integer cell offsets, while object coordinates come from the tapered (4-cell) lobe:
      // a merged pair 2 or 4 cells apart put the centroid exactly on a null and was rejected
      // 20/20 (synthetic), i.e. real unresolved body+limb objects were being deleted.
      for (uint32_t subcarrier = 0; subcarrier < window.subcarriers; ++subcarrier) {
        const double weight = plan.weights[window.cell(row, subcarrier)];
        if (weight > 0.0) {
          weight_sum[fold[row]] += weight;
          ++observed[fold[row]];
          for (uint32_t antenna = 0; antenna < window.antennas; ++antenna)
            coherent[fold[row]][antenna] +=
                weight * static_cast<std::complex<double>>(
                    window.values[window.sample(antenna, row, subcarrier)])
                * std::conj(steering);
        }
        steering *= range_step;
      }
    }
    std::array<double, 2> z{};
    for (size_t side = 0; side < 2; ++side) {
      double power = 0.0;
      for (const auto value : coherent[side]) power += std::norm(value);
      // Same normalisation as the map: |sum w x|^2 / (sum w) -> exponential with mean = noise
      // variance under the null, so noise_scale (the map's null mean) stays the right reference.
      const double denominator = weight_sum[side] * window.antennas;
      if (denominator > 0.0 && noise_scale > 0.0)
        z[side] = power / denominator / noise_scale;
    }
    object.split_minimum_z = std::min(z[0], z[1]);
    object.split_threshold = threshold;
    object.split_validated = observed[0] && observed[1]
                             && object.split_minimum_z > threshold;
    if (object.split_validated) accepted.push_back(std::move(object));
    else ++result.split_validation_rejections;
  }
  objects = std::move(accepted);
}

struct CachedDiagnosticBackend {
  uint32_t antennas = 0, rows = 0, subcarriers = 0, range_bins = 0;
  std::unique_ptr<CudaDetectorBackend> backend;
};

CudaDetectorBackend& prepare_diagnostic_cuda_backend(const CfrWindow& window,
                                                     const Plan& plan)
{
  static thread_local CachedDiagnosticBackend cached;
  const bool shape_matches = cached.backend && cached.antennas == window.antennas
      && cached.rows == window.rows && cached.subcarriers == window.subcarriers
      && cached.range_bins == plan.axes.range_bins;
  if (!shape_matches) {
    cached = {};
    cached.antennas = window.antennas;
    cached.rows = window.rows;
    cached.subcarriers = window.subcarriers;
    cached.range_bins = plan.axes.range_bins;
    cached.backend = std::make_unique<CudaDetectorBackend>(
        window.antennas, window.rows, window.subcarriers, plan.axes.range_bins,
        window.fc_hz, plan.denominator, plan.weights, plan.times, plan.axes.rate_axis_mps,
        plan.rate_allowed, plan.axes.rate_res_mps, true);
  } else {
    cached.backend->reset_diagnostic_cpi(
        window.fc_hz, plan.denominator, plan.weights, plan.times,
        plan.axes.rate_axis_mps, plan.rate_allowed, plan.axes.rate_res_mps);
  }
  return *cached.backend;
}

DiagnosticLikelihoodMap diagnostic_likelihood_map_cpu(const CfrWindow& window,
                                                       const Plan& plan,
                                                       uint32_t minimum_range_bin)
{
  const size_t cells = (size_t)window.rows * window.subcarriers;
  std::vector<std::complex<double>> residual((size_t)window.antennas * cells);
  for (uint32_t a = 0; a < window.antennas; ++a)
    for (size_t i = 0; i < cells; ++i)
      residual[(size_t)a * cells + i] = window.observed[i]
          ? window.values[(size_t)a * cells + i] : std::complex<float>();
  CpuMapWorkspace workspace;
  std::vector<double> likelihood;
  likelihood_map_scaled(residual, window.antennas, window.rows, window.subcarriers, plan,
                        minimum_range_bin, workspace, likelihood);
  return {plan.axes, std::move(likelihood)};
}

} // namespace

DiagnosticLikelihoodMap diagnostic_likelihood_map(const CfrWindow& window,
                                                  const PipelineConfig& config,
                                                  const RateGate& rate_gate,
                                                  uint32_t minimum_range_bin)
{
  const Plan plan = prepare(window, config, rate_gate);
  if (minimum_range_bin >= plan.axes.range_bins)
    throw std::invalid_argument("diagnostic minimum range bin outside detector support");
  if (!(plan.denominator > 0.0))
    throw std::runtime_error("DL-only diagnostic RDM has no observed resource elements");
  const bool require_cuda = cuda_required();
  const bool have_cuda = detector_cuda_available();
  if (require_cuda && !have_cuda)
    throw std::runtime_error("NR_ISAC_REQUIRE_CUDA=1 but no usable CUDA detector is available");
  if (have_cuda) {
    try {
      auto& backend = prepare_diagnostic_cuda_backend(window, plan);
      return {plan.axes, backend.diagnostic_likelihood_map(window.values, minimum_range_bin)};
    } catch (...) {
      if (require_cuda) throw;
      std::fprintf(stderr, "NR_ISAC: DL-only diagnostic CUDA likelihood failed; using CPU fallback\n");
    }
  }
  return diagnostic_likelihood_map_cpu(window, plan, minimum_range_bin);
}

DetectorResult detect_clean(const CfrWindow& window,
                            const PipelineConfig& config,
                            const RateGate& rate_gate,
                            uint32_t minimum_range_bin,
                            std::optional<double> current_cpi_noise_variance,
                            std::optional<double> processing_deadline_s)
{
#ifdef NR_ISAC_FIXED_WORK_REPLAY
  const int diagnostic_limit = diagnostic_clean::component_limit;
  if (diagnostic_limit == -2) throw std::runtime_error("diagnostic CLEAN context missing");
  processing_deadline_s.reset(); // Saved-work replay only; never a live-throughput claim.
#endif
  using DetectorClock = std::chrono::steady_clock;
  if (window.antennas != 1)
    throw std::invalid_argument(
        "canonical CLEAN accepts one independent receiver; split spatial RF channels first");
  if (processing_deadline_s &&
      (!std::isfinite(*processing_deadline_s) || *processing_deadline_s <= 0.0))
    throw std::invalid_argument("CLEAN processing deadline must be finite and positive");
  const bool timing_enabled = std::getenv("NR_ISAC_DETECTOR_TIMING") != nullptr;
  const auto detector_started = DetectorClock::now();
  const Plan plan = prepare(window, config, rate_gate);
  if (minimum_range_bin >= plan.axes.range_bins)
    throw std::invalid_argument("minimum range bin outside detector support");
  const size_t cells = (size_t)window.rows * window.subcarriers;
  std::vector<std::complex<double>> residual((size_t)window.antennas * cells);
  for (uint32_t a = 0; a < window.antennas; ++a)
    for (size_t i = 0; i < cells; ++i)
      residual[(size_t)a * cells + i] = window.observed[i] ? window.values[(size_t)a * cells + i]
                                                           : std::complex<float>();

  CudaDetectorBackend* cuda_backend = nullptr;
  const bool require_cuda = cuda_required();
  const bool have_cuda = detector_cuda_available();
  if (require_cuda && !have_cuda)
    throw std::runtime_error("NR_ISAC_REQUIRE_CUDA=1 but no usable CUDA detector is available");
  if (have_cuda) {
    try {
      struct CachedBackend {
        uint32_t antennas = 0, rows = 0, subcarriers = 0, range_bins = 0;
        uint64_t last_used = 0;
        std::unique_ptr<CudaDetectorBackend> backend;
      };
      static thread_local std::vector<CachedBackend> cache;
      static thread_local uint64_t use_sequence = 0;
      ++use_sequence;
      auto found = std::find_if(cache.begin(), cache.end(), [&](const CachedBackend& entry) {
        return entry.antennas == window.antennas && entry.rows == window.rows
               && entry.subcarriers == window.subcarriers
               && entry.range_bins == plan.axes.range_bins;
      });
      if (found == cache.end()) {
        // Live scheduler row counts vary slightly around each duration bank. Retain a bounded LRU
        // of plans/workspaces so common shapes reuse all cudaMalloc and cuFFT planning state.
        constexpr size_t maximum_cached_shapes = 8;
        if (cache.size() == maximum_cached_shapes) {
          found = std::min_element(cache.begin(), cache.end(), [](const auto& left, const auto& right) {
            return left.last_used < right.last_used;
          });
          cache.erase(found);
        }
        CachedBackend entry;
        entry.antennas = window.antennas;
        entry.rows = window.rows;
        entry.subcarriers = window.subcarriers;
        entry.range_bins = plan.axes.range_bins;
        entry.last_used = use_sequence;
        entry.backend = std::make_unique<CudaDetectorBackend>(
            window.antennas, window.rows, window.subcarriers, plan.axes.range_bins,
            window.fc_hz, plan.denominator, plan.weights, plan.times, plan.axes.rate_axis_mps,
            plan.rate_allowed, plan.axes.rate_res_mps);
        cache.push_back(std::move(entry));
        cuda_backend = cache.back().backend.get();
      } else {
        found->last_used = use_sequence;
        found->backend->reset_cpi(window.fc_hz, plan.denominator, plan.weights, plan.times,
                                  plan.axes.rate_axis_mps, plan.rate_allowed,
                                  plan.axes.rate_res_mps);
        cuda_backend = found->backend.get();
      }
      if (cuda_backend)
        cuda_backend->set_range_walk(plan.range_walk, plan.axes.range_res_m,
                                     plan.range_walk_midpoint_s);
    } catch (const std::exception& error) {
      if (require_cuda) throw;
      std::fprintf(stderr, "NR_ISAC: CUDA detector initialization failed; using CPU fallback: %s\n",
                   error.what());
    }
  }
  const auto initialized_at = DetectorClock::now();
  DetectorResult result;
  result.axes = plan.axes;
  const PsfSupport psf = measured_psf_support(plan, window.rows, window.subcarriers);
  result.psf_range_halfwidth_bins = psf.range_halfwidth;
  result.psf_doppler_halfwidth_bins = psf.doppler_halfwidth;
  result.searched_cells = psf.searched_cells;
  result.resolution_cells = psf.resolution_cells;
  result.effective_hypotheses = psf.effective_hypotheses;
  result.identifiability_guard = std::min<uint64_t>(
      psf.effective_hypotheses, std::max<uint64_t>(1, plan.axes.observed_re_count / 2));
  result.stop_reason = "identifiability_guard";
  const bool have_current_cpi_noise_variance = current_cpi_noise_variance
      && std::isfinite(*current_cpi_noise_variance) && *current_cpi_noise_variance > 0.0;
  if (have_current_cpi_noise_variance) {
    result.null_scale = *current_cpi_noise_variance;
    result.null_scale_source = "current_cpi_within_family_first_difference";
  } else {
    result.null_scale_source = "initial_current_cpi_map_median_fallback";
  }
  const double detector_denominator = plan.denominator * window.antennas;
  double cpu_weighted_energy = 0.0;
  if (!cuda_backend) {
    cpu_weighted_energy = residual_energy(residual, window.antennas, plan);
    result.initial_weighted_energy = cpu_weighted_energy;
    result.initial_residual_scale = result.initial_weighted_energy / detector_denominator;
  }
  const double budget = config.false_object_intensity_per_s * plan.axes.dwell_s;
  if (!(budget > 0.0 && budget < 1.0))
    throw std::invalid_argument("false-object intensity and CPI dwell yield invalid probability");
  // OUR ADAPTATION (stage-6 fix): the proposal is the maximum over effective_hypotheses
  // independent cells, so the per-decision tail must be divided by that count (Bonferroni on the
  // declared budget).  Measured before the fix: pure noise passed the one-proposal threshold in
  // 81.5% of CPIs against a declared 1.2%.
  // Exponential null: P(ratio > T) = e^-T per cell.  The multiplicity is the number of
  // independent NOISE cells searched (every DFT lattice point), not the PSF-resolution count:
  // a taper widens signal resolution but does not reduce the number of noise trials (measured:
  // 191-cell count let pure noise pass 55%; 15450-cell count gives the declared rate).
  const int64_t range_training_cells_available =
      (int64_t)plan.axes.range_bins - 4 * (int64_t)psf.range_halfwidth - 1;
  // DIAGNOSTIC ONLY, no algorithm/threshold change: doppler_row_ca_statistic_impl() (this file,
  // above) discards its own decision (valid=false, effectively "residual_compatible_with_
  // current_cpi_noise") whenever fewer than 16 range-axis cells survive its own
  // guard = 2*psf.range_halfwidth exclusion around the CUT -- the same
  // "range_bins - 4*range_halfwidth - 1" quantity the adaptive-threshold's reported nominal N
  // below already clamps to that same 16. A short range axis is silently undetectable; say so
  // once instead of leaving every CPI stopping at "noise" with no explanation.
  if (range_training_cells_available < 16) {
    static std::atomic<bool> warned_short_range_axis{false};
    if (!warned_short_range_axis.exchange(true))
      std::fprintf(stderr,
          "SENSING: range axis has only %u bins; after the CFAR training guard "
          "(4*psf_range_halfwidth_bins+1 = %lld) at most %lld cells remain, short of the "
          "16-cell minimum the detector needs to form a background estimate -- every CPI on "
          "this axis will read as noise before any component can be scored. Raise "
          "[sensing] maximum_range_m (range_bins = min(subcarriers, "
          "floor(maximum_range_m/range_res_m)+1)); further such warnings are suppressed.\n",
          plan.axes.range_bins, (long long)(4 * (int64_t)psf.range_halfwidth + 1),
          (long long)std::max<int64_t>(0, range_training_cells_available));
  }
  result.adaptive_threshold = os_cfar_multiplier(
      static_cast<uint32_t>(std::max<int64_t>(16, range_training_cells_available)),
      (budget / 2.0) / static_cast<double>(psf.searched_cells));   // reported: nominal N

  CpuMapWorkspace cpu_map_workspace;
  std::vector<double> map;
  bool skirt_proposal = false;
  for (uint64_t iteration = 0; iteration < result.identifiability_guard; ++iteration) {
    skirt_proposal = false;
    if (iteration != 0) {
      const double current_energy = cuda_backend
                                        ? cuda_backend->weighted_energy()
                                        : cpu_weighted_energy;
      const double storage_floor = result.initial_weighted_energy
          * std::numeric_limits<float>::epsilon() * std::numeric_limits<float>::epsilon();
      if (current_energy <= storage_floor) {
        result.stop_reason = "complex_float_storage_floor";
        break;
      }
    }
    const auto map_started = DetectorClock::now();
    // (2026-09-20) From the second iteration on, only the peak and its three Doppler columns
    // come back from the device (CudaDetectorBackend::likelihood_peak); the full map is needed
    // once, for result.initial_likelihood.  Same values, same decisions; ~50x less host traffic.
    CudaDetectorBackend::LikelihoodPeak device_peak;
    const bool peak_only = cuda_backend && iteration != 0 && !force_full_map();
    if (cuda_backend && peak_only) {
      device_peak = cuda_backend->likelihood_peak(residual, minimum_range_bin);
    } else if (cuda_backend) {
      try {
        map = cuda_backend->likelihood_map(residual, minimum_range_bin);
      } catch (const std::exception& error) {
        if (require_cuda || iteration != 0)
          throw; // Host residual is stale after a device-side CLEAN subtraction: never mix states.
        std::fprintf(stderr, "NR_ISAC: initial CUDA likelihood failed; using CPU fallback: %s\n",
                     error.what());
        cuda_backend = nullptr;
        cpu_weighted_energy = residual_energy(residual, window.antennas, plan);
        result.initial_weighted_energy = cpu_weighted_energy;
        result.initial_residual_scale = cpu_weighted_energy / detector_denominator;
      }
    }
    if (!cuda_backend)
      likelihood_map_scaled(residual, window.antennas, window.rows, window.subcarriers, plan,
                            minimum_range_bin, cpu_map_workspace, map);
    const auto map_finished = DetectorClock::now();
    if (!peak_only && force_full_map()) result.final_likelihood = map;   // last full residual map (offline integrator input)
    if (iteration == 0) {
      result.initial_likelihood = map;
      if (cuda_backend) {
        result.initial_weighted_energy = cuda_backend->weighted_energy();
        result.initial_residual_scale = result.initial_weighted_energy / detector_denominator;
      }
    }
    PeakColumns view;
    Peak peak;
    if (peak_only) {
      view.range_bins = device_peak.range_bins; view.rows = device_peak.rows; view.d0 = device_peak.d; view.columns = &device_peak.columns;
      view.r0 = device_peak.r; view.range_slice = &device_peak.range_slice;
      Peak raw; raw.valid = device_peak.valid; raw.r = device_peak.r; raw.d = device_peak.d; raw.score = device_peak.score;
      peak = interpolate_start(raw, view);
    } else {
      peak = interpolate_start(strongest(map, window.rows), map, plan.axes.range_bins, window.rows);
    }
    if (!peak.valid) {
      result.stop_reason = "empty_residual_map";
      break;
    }
    const double iteration_budget = budget
        / ((static_cast<double>(iteration) + 1.0) * (static_cast<double>(iteration) + 2.0));
    // Exactly one strongest proposal is exposed by each serial CLEAN search. Multiplicity over
    // the unbounded proposal sequence is already paid by alpha spending; the final cross-fold
    // validation separately pays the measured map-hypothesis count.
    const double per_cell_alpha = iteration_budget / static_cast<double>(psf.searched_cells);
    /* Preserve the canonical Python pipeline: every strongest CLEAN proposal is standardized
     * against the current residual map in log power.  Median/MAD makes the decision insensitive
     * to receiver gain and prevents thermal-noise variance from declaring structured multipath
     * thousands of sigma significant.  CUT/PSF cells never train their own decision. */
    LocalStatistic local = peak_only ? doppler_row_ca_statistic(view, plan, psf, peak.r, peak.d)
                                     : doppler_row_ca_statistic(map, plan, psf, peak.r, peak.d);
    if (iteration == 0 && local.valid) {
      // Exponential power median = mean * log(2). Fold likelihoods and downstream soft evidence
      // need the mean scale, not the raw median. CLEAN proposal z/threshold/refinement is unchanged.
      result.null_scale = std::exp(local.log_background_median);   // already the exponential mean
      result.null_scale_source = "current_residual_cut_excluded_exponential_mean";
    }
    const double decision_threshold = local.valid
        ? os_cfar_multiplier(local.training_cells, per_cell_alpha) : std::numeric_limits<double>::infinity();
    if (!local.valid || !(local.z > decision_threshold)) {
      result.stop_reason = "residual_compatible_with_current_cpi_noise";
      break;
    }
    double component_column_z = 0.0, component_column_threshold = std::numeric_limits<double>::infinity();
    uint32_t component_column_cells = 0;
    if (greatest_of_cfar()) {
      const LocalStatistic column = peak_only ? range_column_statistic(view, plan, psf, peak.r, peak.d)
                                              : range_column_statistic(map, plan, psf, peak.r, peak.d);
      double column_threshold = column.valid
          ? os_cfar_multiplier(column.training_cells, per_cell_alpha) : std::numeric_limits<double>::infinity();
      bool column_pass = column.valid && column.z > column_threshold;
      if (skirt_sidelobe_rule()) {
        // Sidelobe-bound rule, v2. The parent of a Doppler skirt is NOT in the current residual map
        // -- CLEAN already subtracted it -- so looking for a stronger cell in the column finds
        // nothing and flags no skirts at all (measured: 0 skirts, DL detections 780 -> 8384,
        // precision 59.4% -> 25.3%). The parent is a component ACCEPTED IN AN EARLIER ITERATION of
        // this same CPI, whose imperfectly cancelled sidelobe is what remains at the candidate cell.
        // Its predicted level is its own fitted power times the MEASURED Doppler PSF at the
        // candidate's Doppler offset -- both quantities this CPI already computed. A candidate above
        // every such prediction cannot be explained as anyone's sidelobe and is a distinct object.
        double predicted = 0.0;
        for (const CleanComponent& parent : result.components) {
          const double range_offset = std::abs(parent.range_bin - static_cast<double>(peak.r));
          if (range_offset > static_cast<double>(psf.range_halfwidth)) continue;
          const double raw = std::abs(parent.doppler_bin - static_cast<double>(peak.d));
          const double circular = std::min(raw, static_cast<double>(window.rows) - raw);
          const size_t offset = static_cast<size_t>(std::llround(circular));
          const double ratio = offset < psf.doppler_psf_ratio.size()
                                   ? psf.doppler_psf_ratio[offset]
                                   : psf.doppler_sidelobe_ratio;
          predicted = std::max(predicted, parent.raw_score * ratio);
        }
        column_pass = std::isfinite(local.cut_score) && local.cut_score > 0.0
                      && local.cut_score > predicted;
        column_threshold = predicted;   // the level the candidate had to beat to not be a skirt
      }
      component_column_z = column.z; component_column_threshold = column_threshold;
      component_column_cells = column.training_cells;
      if (!column_pass) {
        // A proposal that fails the column test is a skirt cell: it is NOT a target and must not
        // stop CLEAN either (the residue still has to be removed).  It is subtracted as a
        // component but flagged so the report excludes it from detections.
        skirt_proposal = true;
      } else {
        skirt_proposal = false;
      }
    }
    Refined refined = refine(residual, window, plan, peak, minimum_range_bin, rate_gate,
                             2.0 * config.maximum_target_speed_mps, cuda_backend);
    const auto refine_finished = DetectorClock::now();
    CleanComponent component;
    component.range_bin = refined.range_bin; component.doppler_bin = refined.doppler_bin;
    component.coarse_range_bin = peak.r; component.coarse_doppler_bin = peak.d;
    component.raw_score = refined.score;
    component.score = refined.score / std::max(result.initial_residual_scale, std::numeric_limits<double>::min());
    component.iteration = static_cast<uint32_t>(iteration + 1); component.local = local;
    component.skirt = skirt_proposal;
    component.local_threshold = decision_threshold;
    component.column_z = component_column_z;
    component.column_threshold = component_column_threshold;
    component.column_training_cells = component_column_cells;
    component.localization = refined.localization;
    component.array_response.resize(window.antennas);
    double alpha_power = 0.0;
    for (uint32_t a = 0; a < window.antennas; ++a) {
      const auto alpha = refined.coherent[a] / std::max(plan.denominator, std::numeric_limits<double>::min());
      component.array_response[a] = alpha;
      alpha_power += std::norm(alpha);
    }
    double before = 0.0, after = 0.0;
    if (cuda_backend) {
      const auto energy = cuda_backend->subtract_component(
          refined.range_bin, refined.doppler_bin, component.array_response);
      before = energy[0];
      after = energy[1];
    } else {
      before = cpu_weighted_energy;
      for (uint32_t a = 0; a < window.antennas; ++a)
        for (size_t i = 0; i < cells; ++i) {
          residual[(size_t)a * cells + i] -= component.array_response[a] * refined.steering[i];
          after += plan.weights[i] * std::norm(residual[(size_t)a * cells + i]);
        }
      cpu_weighted_energy = after;
    }
    const auto alpha0 = component.array_response.front();
    component.amplitude_abs = std::sqrt(alpha_power / window.antennas);
    component.amplitude_phase_rad = std::arg(alpha0);
    component.complex_coefficient = alpha0;
    component.component_lineage = {component.iteration};
    component.fitted_weighted_energy = alpha_power * plan.denominator;
    const auto update_finished = DetectorClock::now();
    component.weighted_energy_removed = std::max(0.0, before - after);
    result.components.push_back(std::move(component));
#ifdef NR_ISAC_FIXED_WORK_REPLAY
    if (diagnostic_limit > 0 && result.components.size() >= static_cast<size_t>(diagnostic_limit)) {
      result.stop_reason = "next_cpi_processing_deadline";
      break;
    }
#endif
    if (processing_deadline_s) {
      // Shed only work that cannot finish before the next causal CPI. The caller supplies the
      // acquisition cadence so this spatial runtime policy cannot leak into other OAI modes.
      const double elapsed_s = std::chrono::duration<double>(
          DetectorClock::now() - detector_started).count();
      const double mean_proposal_s = elapsed_s / static_cast<double>(iteration + 1);
      if (elapsed_s + mean_proposal_s >= *processing_deadline_s) {
        result.stop_reason = "next_cpi_processing_deadline";
        break;
      }
    }
    if (timing_enabled)
      std::fprintf(stderr, "NR_ISAC detector iteration %u: z=%.3g threshold=%.3g map=%.3f ms refine=%.3f ms update=%.3f ms evals=%u newton=%u\n",
                   static_cast<unsigned>(iteration + 1),
                   local.z, decision_threshold,
                   std::chrono::duration<double, std::milli>(map_finished - map_started).count(),
                   std::chrono::duration<double, std::milli>(refine_finished - map_finished).count(),
                   std::chrono::duration<double, std::milli>(update_finished - refine_finished).count(),
                   refined.evaluations, component.localization.iterations);
  }
  result.final_weighted_energy = cuda_backend
                                     ? cuda_backend->weighted_energy()
                                     : cpu_weighted_energy;
  {
    std::vector<CleanComponent> kept;
    for (const auto& c : result.components) if (!c.skirt) kept.push_back(c);
    result.skirt_components = static_cast<uint32_t>(result.components.size() - kept.size());
    result.objects = collapse_unresolved(kept, plan, psf);
  }
  tag_micro_doppler_families(result.objects, plan, psf);
  validate_cross_fold_support(result.objects, result, window, plan, budget);
  if (timing_enabled)
    std::fprintf(stderr, "NR_ISAC detector CPI: initialize=%.3f ms total=%.3f ms\n",
                 std::chrono::duration<double, std::milli>(initialized_at - detector_started).count(),
                 std::chrono::duration<double, std::milli>(DetectorClock::now() - detector_started).count());
  return result;
}

DetectorResult detect_clean_with_diagnostic(const CfrWindow& window,
                                            const CfrWindow& dl_window,
                                            const PipelineConfig& config,
                                            const RateGate& rate_gate,
                                            uint32_t minimum_range_bin)
{
  const Plan dl_plan = prepare(dl_window, config, rate_gate);
  if (minimum_range_bin >= dl_plan.axes.range_bins)
    throw std::invalid_argument("diagnostic minimum range bin outside detector support");
  if (!(dl_plan.denominator > 0.0))
    throw std::runtime_error("DL-only diagnostic RDM has no observed resource elements");

  const bool require_cuda = cuda_required();
  const bool have_cuda = detector_cuda_available();
  if (require_cuda && !have_cuda)
    throw std::runtime_error("NR_ISAC_REQUIRE_CUDA=1 but no usable CUDA detector is available");
  CudaDetectorBackend* diagnostic_backend = nullptr;
  if (have_cuda) {
    try {
      diagnostic_backend = &prepare_diagnostic_cuda_backend(dl_window, dl_plan);
      diagnostic_backend->begin_diagnostic_likelihood_map(dl_window.values,
                                                          minimum_range_bin);
    } catch (...) {
      if (require_cuda) throw;
      diagnostic_backend = nullptr;
      std::fprintf(stderr,
                   "NR_ISAC: asynchronous DL diagnostic launch failed; using CPU fallback\n");
    }
  }

  DetectorResult result;
  try {
    result = detect_clean(window, config, rate_gate, minimum_range_bin);
  } catch (...) {
    if (diagnostic_backend) {
      try {
        (void)diagnostic_backend->finish_diagnostic_likelihood_map();
      } catch (...) {
      }
    }
    throw;
  }

  DiagnosticLikelihoodMap dl;
  if (diagnostic_backend) {
    try {
      dl = {dl_plan.axes, diagnostic_backend->finish_diagnostic_likelihood_map()};
    } catch (...) {
      if (require_cuda) throw;
      std::fprintf(stderr,
                   "NR_ISAC: asynchronous DL diagnostic completion failed; using CPU fallback\n");
      dl = diagnostic_likelihood_map_cpu(dl_window, dl_plan, minimum_range_bin);
    }
  } else {
    dl = diagnostic_likelihood_map_cpu(dl_window, dl_plan, minimum_range_bin);
  }
  if (dl.axes.range_bins != result.axes.range_bins
      || dl.axes.rate_bins != result.axes.rate_bins
      || dl.likelihood.size() != result.initial_likelihood.size()
      || dl.axes.observed_re_count == 0)
    throw std::runtime_error("DL-only diagnostic RDM has incompatible axes or no observed DL samples");
  result.initial_dl_likelihood = std::move(dl.likelihood);
  result.dl_observed_re_count = dl.axes.observed_re_count;
  return result;
}

} // namespace nr_isac
