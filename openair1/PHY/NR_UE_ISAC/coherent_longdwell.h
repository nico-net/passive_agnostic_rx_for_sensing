/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/* Long-dwell slow-target CPI (coherent_long_dwell). The short CPI (~75 ms) cannot test its own
 * zero-Doppler notch (+-notch_half_bins at 1/T_cpi, ~+-2.3-3.5 m/s of bistatic rate), where walking
 * people live. A second, LONG CPI runs alongside it and tests ONLY that slow band:
 *  - f_slow: the short CPI's first Doppler bin past its notch ((notch_half_bins+1) * dopp_step, the lowest
 *    |f| it tests; the median over the long CPI's short CPIs), so the two CPIs cover
 *    the Doppler axis without a gap (the notch EDGE itself leaves the half bin up to the first tested
 *    bin covered by neither).
 *  - T_L = (c / B_eff) / (lambda * f_slow): a target at the slow band's top rate migrates at most one
 *    range cell over the dwell. B_eff is the LONG CPI's own (median super-row bandwidth, what derive_axes
 *    gives it): super-rows are unions of a block's allocations, wider than the short CPI's median row.
 *  - super-rows: each short CPI's rows, after that CPI's own row sync and LOS referencing (exactly
 *    range_doppler()'s derotation), averaged per subcarrier in equal blocks of length <= T_s =
 *    1/(4 f_slow). Nyquist for |f| <= f_slow needs 1/(2 f_slow); the factor 2 of oversampling puts the
 *    folding frequency at 2 f_slow, so the band just above the slow band (f_slow..3 f_slow, the short
 *    CPI's first tested bins, where a walker becomes a jogger) cannot fold into it; only |f| > 3 f_slow
 *    can, attenuated by the block mean's sinc (<= -10.5 dB) and by its own range migration over T_L.
 *  - alignment: one common delay and one complex gain per short CPI against the running static profile
 *    (the same model as estimate_row_sync's per-row common gain), so the long CPI is phase-continuous
 *    across the per-CPI CFO phase and the per-CPI common LOS offset. The profile itself sits at whatever
 *    LOS offset its first CPI had; each short CPI's own LOS reference is zero-mean jitter, so the long
 *    window is re-centred by the mean of its CPIs' applied delays (range axis referenced to the LOS).
 *  - a data gap longer than T_L restarts the long window; a long CPI with fewer super-rows than tested
 *    Doppler bins (unknowns) is reported skipped.
 *  - cadence T_L/2: Hann windows at 50 % overlap sum to a constant (constant overlap-add), so every
 *    instant gets the same total slow-time weight.
 * The long CPI then runs the SAME chain (range_doppler / whitening / envelope / detect / refine) with
 * derive_axes limited to |f| <= f_slow; its own notch is its own +-notch_half_bins at 1/T_L. */
#pragma once
#include "coherent_core.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace nr_isac::coherent {

/** One short CPI reduced to super-rows. */
struct SuperRows {
  CfrWindow w;                        // super-rows, LOS-referenced / row-synced (range_doppler's derotation)
  std::array<bool, kCh> found{};      // channels with a LOS reference in this short CPI
  std::vector<cd> sum;                // [ch][k] sum of the derotated values over the CPI's rows
  std::vector<uint32_t> cnt;          // [k] number of rows observing k
  double f_slow_hz = 0, b_eff_hz = 0; // short CPI's first bin past its notch; median SUPER-row bandwidth
  double t0_slots = 0, t1_slots = 0;  // first / last super-row time (absolute slots)
  double t_air_s = 0, mid_slots = 0;  // short CPI's midpoint air time and its midpoint in slots
  double align_delay_s = 0;           // delay alignment applied against the running static profile
  bool valid() const { return w.rows > 0 && f_slow_hz > 0 && b_eff_hz > 0; }
};

/** Super-rows of one short CPI (rows flagged RowSync::bad are skipped). */
SuperRows make_super_rows(const CfrWindow& w, const Axes& a, const LosEstimate& L, const RowSync& s);

/** A long CPI ready for the chain: its window and derived parameters. */
struct LongCpi {
  CfrWindow w;
  std::array<bool, kCh> found{};
  double f_slow_hz = 0, t_l_s = 0, cadence_s = 0, b_eff_hz = 0;   // b_eff: median super-row bandwidth (T_L's range cell)
  double t_air_s = 0;                 // midpoint air time of the long CPI
  uint32_t n_short = 0;               // short CPIs contributing
  // Diagnostic: fraction of the residual (after this window's own per-subcarrier mean) that is constant
  // within each contributing short CPI -- the steps left by the one-gain-one-delay per-CPI alignment --
  // and the same fraction expected from white noise ((blocks - 1) / (rows - 1)).
  double step_frac = 0, step_frac_null = 0;
};

/** Sliding long window: aligns and buffers super-rows; add() returns true (and fills *out) when a long
 * CPI of span >= T_L is complete. Keeps the last T_L/2 for the next one. */
class LongDwell {
public:
  bool add(SuperRows sr, LongCpi* out);
  void reset() { q_.clear(); ref_sum_.clear(); ref_cnt_.clear(); start_slots_ = -1; }
  // alignment applied to the last add() (diagnostics / tests)
  double last_delay_s = 0; cd last_gain = 1;
private:
  std::deque<SuperRows> q_;
  std::vector<cd> ref_sum_; std::vector<uint32_t> ref_cnt_;   // [ch][k] running static profile over q_
  double start_slots_ = -1;                                   // start of the long window being filled
  void ref_add(const SuperRows& s, int sign);
};

/** Result of one long CPI through the chain. */
struct LongResult {
  LongCpi cpi;
  Axes a;
  Grid G;
  std::vector<Detection> D;
  StaticModelInfo smi;
  double tm_build = 0, tm_rd = 0, tm_env = 0, tm_detect = 0, tm_refine = 0, tm_total = 0;
  size_t queue_max = 0;
};

class CudaCoherent;
/** The long CPI on its own worker thread: push() takes a finished short CPI (moved in, never blocks),
 * the worker builds super-rows, aligns, and runs the chain when a long CPI completes; results go to
 * `sink` on the worker thread. */
class LongDwellRunner {
public:
  using Sink = std::function<void(LongResult&)>;
  LongDwellRunner(const CoherentConfig& cfg, bool use_cuda, Sink sink);
  ~LongDwellRunner();                       // drains the queue, joins the worker
  void push(CfrWindow&& w, const Axes& a, const LosEstimate& L, const RowSync& s, double t_air_s,
            const Geometry& geo, const Calibration& cal);
private:
  struct Job { CfrWindow w; Axes a; LosEstimate L; RowSync s; double t = 0; Geometry geo; Calibration cal; };
  void run();
  void process(Job& j);
  CoherentConfig cfg_;
  bool use_cuda_;
  Sink sink_;
  LongDwell ld_;
  std::unique_ptr<CudaCoherent> cuda_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> q_;
  size_t qmax_ = 0;
  bool stop_ = false;
  std::thread worker_;
};

} // namespace nr_isac::coherent
