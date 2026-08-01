/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include "range_doppler.h"
#include "detection_report.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

extern "C" {
#include "common/utils/LOG/log.h"
}

namespace nr_isac {

static constexpr double SPEED_OF_LIGHT = 299792458.0;

bool parse_selftest_los(const std::string& spec, selftest_los_impairment_t& out)
{
  out = selftest_los_impairment_t();
  if (spec.empty()) {
    return false;
  }
  double sto_us = 0, cfo_hz = 0, sfo_ppm = 0;
  if (std::sscanf(spec.c_str(), "%lf:%lf:%lf", &sto_us, &cfo_hz, &sfo_ppm) != 3) {
    LOG_W(PHY, "SENSING: ignoring malformed selftest_los '%s' (expected STO_US:CFO_HZ:SFO_PPM)\n", spec.c_str());
    return false;
  }
  out.sto_s   = sto_us * 1e-6;
  out.cfo_hz  = cfo_hz;
  out.sfo_ppm = sfo_ppm;
  out.present = true;
  return true;
}

icf_t selftest_tone(double amp, double m_times_df, double tau, double fd, double n_times_t_slow)
{
  const double phase = -2.0 * M_PI * m_times_df * tau + 2.0 * M_PI * fd * n_times_t_slow;
  return icf_t((float)(amp * std::cos(phase)), (float)(amp * std::sin(phase)));
}

// Builds a symmetric Dolph-Chebyshev window of length N with equiripple sidelobe attenuation
// @p sidelobe_db (positive dB, e.g. 60 => -60 dBc sidelobes), via the standard frequency-sampling
// construction (Antoniou; same algorithm as scipy.signal.windows.chebwin / MATLAB's chebwin).
// Reuses isac_fft's forward DFT (this module's established "own everything, no external DSP lib"
// practice -- see isac_sync.cc's leakage_model/HANN_ESTIMATOR_SCALE for the same pattern) rather
// than pulling in a dependency: isac_fft's uniform 1/sqrt(N) output scaling is identical on every
// bin, and this function's final peak-normalisation step divides it out exactly, so reusing the
// existing plan unmodified is safe.
//
// Applied only to the range/frequency-axis window (SYNC_NOISE_HANDOVER.md's diagnosed 55-100m
// LOS-skirt is fast-time sidelobe leakage from the strong LOS/direct-path tap); the slow-time
// (Doppler) window is untouched by this change and stays Hann.
static void build_chebyshev_window(uint32_t N, double sidelobe_db, std::vector<float>& out)
{
  out.assign(N, 1.0f);
  if (N < 2) {
    return;
  }
  const uint32_t order = N - 1;
  const double   r     = std::pow(10.0, std::abs(sidelobe_db) / 20.0);
  const double   beta  = std::cosh(std::acosh(r) / (double)order);

  std::vector<icf_t> p(N);
  for (uint32_t k = 0; k < N; k++) {
    const double x = beta * std::cos(M_PI * (double)k / (double)N);
    double       val;
    if (x > 1.0) {
      val = std::cosh((double)order * std::acosh(x));
    } else if (x < -1.0) {
      // (1 - 2*(order%2)): +1 for even order, -1 for odd order.
      val = ((order % 2 == 0) ? 1.0 : -1.0) * std::cosh((double)order * std::acosh(-x));
    } else {
      val = std::cos((double)order * std::acos(x));
    }
    p[k] = icf_t((float)val, 0.0f);
  }

  if (N % 2 == 1) {
    // Odd length: w = concat(reverse(w_half[1:]), w_half), w_half = real(fft(p))[:n], n=(N+1)/2.
    const uint32_t      n = (N + 1) / 2;
    std::vector<icf_t> w(N);
    fft_plan            plan(N, false /* forward DFT, matches numpy's fft(p) convention */);
    plan.run(p.data(), w.data());
    for (uint32_t i = 0; i < n; i++) {
      out[i]         = w[n - 1 - i].real();
      out[N - 1 - i] = w[n - 1 - i].real();
    }
  } else {
    // Even length: pre-rotate by exp(j*pi*k/N), then w = concat(reverse(w_half[1:n]), w_half[1:n]),
    // n = N/2+1.
    for (uint32_t k = 0; k < N; k++) {
      const double ph = M_PI * (double)k / (double)N;
      p[k]             = p[k] * icf_t((float)std::cos(ph), (float)std::sin(ph));
    }
    const uint32_t      n = N / 2 + 1;
    std::vector<icf_t> w(N);
    fft_plan            plan(N, false);
    plan.run(p.data(), w.data());
    for (uint32_t i = 0; i < n - 1; i++) {
      out[i]         = w[n - 1 - i].real();
      out[n - 1 + i] = w[1 + i].real();
    }
  }

  float mx = 0.0f;
  for (float v : out) {
    mx = std::max(mx, std::fabs(v));
  }
  if (mx > 0.0f) {
    for (float& v : out) {
      v /= mx;
    }
  }
}

range_doppler::range_doppler(const nr_isac_args_t& args_) : args(args_)
{
  // Parse "DELAY_US:DOPPLER_HZ:GAIN,DELAY_US:DOPPLER_HZ:GAIN,..." into synthetic targets.
  const std::string& spec = args.selftest_targets;
  size_t             pos  = 0;
  while (pos < spec.size()) {
    size_t            comma = spec.find(',', pos);
    const std::string item  = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    pos                     = (comma == std::string::npos) ? spec.size() : comma + 1;
    if (item.empty()) {
      continue;
    }
    double d_us = 0, fd = 0, g = 1.0;
    if (std::sscanf(item.c_str(), "%lf:%lf:%lf", &d_us, &fd, &g) == 3) {
      targets_.push_back({d_us * 1e-6, fd, g});
    } else {
      LOG_W(PHY, "SENSING: ignoring malformed target '%s' (expected DELAY_US:DOPPLER_HZ:GAIN)\n", item.c_str());
    }
  }

  // Phase 6a (ota_sync_passive_ue.md): known LOS-path impairment for the offline sync self-test.
  // NOTE: process() below does NOT consume this itself -- by the time a CPI reaches range_doppler,
  // it has already passed through Phases 1-4 in sensing_engine.cc's CPI-close block (before Stage-4b
  // interpolation), so injecting the impairment here would be downstream of the very corrections
  // Phase 6a needs to exercise. This is parsed and logged here (same config surface/style as
  // selftest_targets) purely for visibility; the actual raw-grid injection that drives Phases 1-4
  // lives in the offline test harness (tests/isac_sync_test.cc), which calls parse_selftest_los()
  // and selftest_tone() directly to build a synthetic pre-Stage-4b CPI. See
  // docs/NR_UE_ISAC_sync_gap_analysis.md section 12 for the full rationale.
  if (parse_selftest_los(args.selftest_los, los_impairment_)) {
    LOG_I(PHY, "SENSING: selftest_los parsed: sto=%.3f us cfo=%.2f Hz sfo=%.3f ppm (see gap-analysis doc sec. 12)\n",
          los_impairment_.sto_s * 1e6, los_impairment_.cfo_hz, los_impairment_.sfo_ppm);
  }

  // Clutter-removal method (ECA_CLUTTER_HANDOVER.md). Default "mean" preserves legacy behaviour;
  // "eca+" swaps in the CFR-domain ECA/ECA+ oblique projection (eca_clutter). Constructed once.
  if (args.clutter_removal == "eca+") {
    eca_.reset(new eca_clutter(args));
    LOG_I(PHY, "SENSING: clutter removal = ECA+ (delay_max=%.1f m, dopp_max=%.3f m/s)\n",
          (double)args.eca_delay_max_m, (double)args.eca_doppler_max_mps);
  } else if (!args.clutter_removal.empty() && args.clutter_removal != "mean") {
    LOG_W(PHY, "SENSING: unknown clutter_removal '%s'; falling back to mean subtraction\n",
          args.clutter_removal.c_str());
  }

  // Fast-time (range) window (SYNC_NOISE_HANDOVER.md's LOS-skirt finding). Default "hann" preserves
  // legacy behaviour; "chebyshev" swaps in the equiripple Dolph-Chebyshev window built above.
  if (args.range_window == "chebyshev") {
    LOG_I(PHY, "SENSING: range window = Dolph-Chebyshev (sidelobe=%.1f dB)\n", (double)args.range_window_sidelobe_db);
  } else if (!args.range_window.empty() && args.range_window != "hann") {
    LOG_W(PHY, "SENSING: unknown range_window '%s'; falling back to Hann\n", args.range_window.c_str());
  }
}

bool range_doppler::ensure_plans(uint32_t nof_range, uint32_t nof_slow)
{
  if (!range_plan || range_N != nof_range) {
    range_plan.reset(new fft_plan(nof_range, true /* inverse: range IFFT */));
    range_N = nof_range;
  }
  if (!dopp_plan || dopp_N != nof_slow) {
    dopp_plan.reset(new fft_plan(nof_slow, false /* forward: Doppler FFT */));
    dopp_N = nof_slow;
  }
  return range_plan && dopp_plan;
}

void range_doppler::inject_selftest(icf_t*    work_buf,
                                    uint32_t nof_slow,
                                    uint32_t nof_subc,
                                    double   df_comb,
                                    double   t_slow,
                                    double   fc)
{
  // Build the target list: explicit selftest_targets if given, else a default target at 25% of the
  // unambiguous range/velocity extents when selftest is set.
  std::vector<sensing_target_t> targets = targets_;
  if (targets.empty()) {
    // Bistatic conventions, matching rvm.range_res_m / rvm.vel_res_mps (2026-07-23 calibration fix):
    // dR = c*tau (no round-trip factor) and Ṙ = fd*lambda, so a target meant to land at 25% of each
    // axis is placed with tau = 0.25*range_max/c and fd = 0.25*vel_max*fc/c. Previously both carried
    // the erroneous monostatic 2, which put the DEFAULT self-test target at half the range and half
    // the velocity it advertised.
    const double range_max = SPEED_OF_LIGHT / df_comb;
    const double tau       = (0.25 * range_max) / SPEED_OF_LIGHT;
    double       fd        = 0.0;
    if (fc > 0.0 && t_slow > 0.0) {
      const double vel_max = SPEED_OF_LIGHT / (2.0 * fc * t_slow);
      // Negative fd for a positive (opening) range-rate -- matches the reporting sign above.
      fd                   = -(0.25 * vel_max) * fc / SPEED_OF_LIGHT;
    }
    targets.push_back({tau, fd, 3.0}); // strong default so it is unmistakable
  }

  // Reference amplitude: the CFR RMS (~ the LOS/direct-path level), so gain-1 target ~ direct path.
  double       power = 0.0;
  const size_t n_tot = (size_t)nof_slow * nof_subc;
  for (size_t i = 0; i < n_tot; i++) {
    power += (double)work_buf[i].real() * work_buf[i].real() + (double)work_buf[i].imag() * work_buf[i].imag();
  }
  const double rms = std::sqrt(power / (double)std::max<size_t>(1, n_tot)) + 1e-9;

  for (const sensing_target_t& t : targets) {
    const double tau = t.delay_s;
    const double fd  = t.doppler_hz;
    const double amp = t.gain * rms;
    for (uint32_t n = 0; n < nof_slow; n++) {
      for (uint32_t m = 0; m < nof_subc; m++) {
        work_buf[(size_t)n * nof_subc + m] += selftest_tone(amp, (double)m * df_comb, tau, fd, (double)n * t_slow);
      }
    }
    const int    r_bin = (int)std::lround(tau * (double)nof_subc * df_comb);
    const int    d_bin = (int)nof_slow / 2 + (int)std::lround(fd * (double)nof_slow * t_slow);
    const double range = SPEED_OF_LIGHT * tau / 2.0;
    const double vel   = (fc > 0.0) ? fd * SPEED_OF_LIGHT / (2.0 * fc) : 0.0;
    LOG_I(PHY,
          "SENSING: injected target delay=%.2f us dopp=%+.1f Hz gain=%.2f -> range=%.1f m vel=%.2f m/s "
          "(expected bins r=%d d=%d)\n",
          tau * 1e6, fd, t.gain, range, vel, r_bin, d_bin);
  }
}

/// Auto clutter-notch widths: how far the direct path / static clutter actually extends, measured
/// from this CPI's own map rather than declared.
///
/// The notch exists to remove ONE physical thing -- the direct path and the static return around it
/// -- and how many bins that occupies is a property of the window mainlobe, the residual sync error
/// and the clutter's own spread, all of which vary per deployment and per CPI. A hand-set width is
/// therefore either too narrow (clutter leaks into CFAR's noise estimate) or too wide (it eats real
/// slow/near targets). Both are measured here by walking outward from the clutter ridge until the
/// profile falls to its own robust floor:
///   zero-Doppler width : profile over Doppler, summed across range, walked out from the DC bin
///   zero-range width   : profile over range, summed across Doppler, walked out from bin 0
/// `fallback` is returned when nothing separates (a map with no dominant clutter), so a scene without
/// a direct path degrades to the configured behaviour instead of notching arbitrarily.
static int auto_notch_width(const std::vector<double>& prof, uint32_t centre, int fallback)
{
  const size_t n = prof.size();
  if (n < 16) {
    return fallback;
  }
  // Robust floor from the OUTER HALF, i.e. away from the clutter ridge the notch is meant to cover.
  std::vector<double> tail;
  tail.reserve(n / 2);
  for (size_t i = n / 2; i < n; i++) {
    tail.push_back(prof[i]);
  }
  std::nth_element(tail.begin(), tail.begin() + tail.size() / 2, tail.end());
  const double med = tail[tail.size() / 2];
  for (double& v : tail) {
    v = std::fabs(v - med);
  }
  std::nth_element(tail.begin(), tail.begin() + tail.size() / 2, tail.end());
  const double mad = std::max(tail[tail.size() / 2] * 1.4826, 1e-30);
  const double thr = med + 3.0 * mad;
  if (prof[centre] <= thr) {
    return fallback; // no dominant clutter here: nothing to size a notch against
  }
  // Walk out until the ridge has fallen into its own noise, capped so a pathological map cannot
  // notch the whole axis away.
  const int cap = (int)(n / 8);
  int       w   = 0;
  while (w < cap) {
    const size_t a = centre + (size_t)w + 1;
    const bool   hi = (a < n) ? (prof[a] > thr) : false;
    if (!hi) {
      break;
    }
    w++;
  }
  return w;
}

/// Auto far-range horizon: the largest range bin at which the range profile still stands above its
/// own far-end noise floor (see the caller). Returns nof_range (no notch) if nothing separates.
static int auto_range_horizon_bin(const sensing_rvm_t& rvm, uint32_t nof_range, uint32_t nof_dopp)
{
  if (rvm.power.empty() || nof_range < 8) {
    return (int)nof_range;
  }
  std::vector<double> prof(nof_range, 0.0);
  for (uint32_t r = 0; r < nof_range; r++) {
    const float* row = &rvm.power[(size_t)r * nof_dopp];
    double       acc = 0.0;
    for (uint32_t d = 0; d < nof_dopp; d++) {
      acc += (double)row[d];
    }
    prof[r] = acc;
  }
  // Robust floor from the FAR QUARTER, which is where an unreachable region must live if there is
  // one. Median/MAD so a couple of bright artifacts cannot set the floor.
  const uint32_t       lo = (nof_range * 3) / 4;
  std::vector<double>  tail(prof.begin() + lo, prof.end());
  std::vector<double>  t2 = tail;
  std::nth_element(t2.begin(), t2.begin() + t2.size() / 2, t2.end());
  const double med = t2[t2.size() / 2];
  for (double& v : t2) {
    v = std::fabs(v - med);
  }
  std::nth_element(t2.begin(), t2.begin() + t2.size() / 2, t2.end());
  const double mad = std::max(t2[t2.size() / 2] * 1.4826, 1e-30);
  // Walk in from the far end; the horizon is the first bin that clears the floor by a margin the
  // floor's OWN spread defines (3 robust sigma), not by a chosen power level.
  for (int r = (int)nof_range - 1; r >= 0; r--) {
    if (prof[(size_t)r] > med + 3.0 * mad) {
      return r;
    }
  }
  return (int)nof_range;
}


void range_doppler::process(const icf_t*                       h_cpi,
                            uint32_t                          nof_slow,
                            uint32_t                          nof_subc,
                            uint32_t                          comb_spacing,
                            const nr_isac_carrier_t&          carrier,
                            float                             period_slots,
                            const uint32_t*                   row_comb,
                            sensing_rvm_t&                    rvm,
                            std::vector<sensing_detection_t>& detections,
                            const double*                     row_time_slots,
                            const uint8_t*                    occ_mask)
{
  detections.clear();
  if (h_cpi == nullptr || nof_slow < 2 || nof_subc < 2 || comb_spacing == 0) {
    return;
  }

  const uint32_t nof_range = nof_subc;
  const uint32_t nof_dopp  = nof_slow;
  if (!ensure_plans(nof_range, nof_slow)) {
    LOG_E(PHY, "SENSING: DFT plan initialisation failed\n");
    return;
  }

  // Axis scaling (nominal). df_comb is the reference comb spacing in Hz.
  const double dfc      = (double)carrier.scs_hz;
  const double df_comb  = dfc * (double)comb_spacing;
  const double slots_sf = std::max(1.0, dfc / 15000.0);
  const double slot_dur = 1e-3 / slots_sf;
  const double t_slow   = (double)period_slots * slot_dur;
  const double fc       = (double)carrier.dl_center_hz;

  rvm.nof_range_bins   = nof_range;
  rvm.nof_doppler_bins = nof_dopp;
  // Range axis = DIFFERENTIAL BISTATIC RANGE (dR = R_tx→tgt→rx − R_los), NOT a monostatic one-way
  // range. Same 2026-07-23 calibration fix as the velocity axis below, and the same root cause: a
  // scatterer's CFR contribution is exp(-j2π·m·Δf·τ) with τ = dR/c (no round-trip factor — the
  // "there and back" is already inside the bistatic path length), so the range IFFT puts it at bin
  // k = N·Δf·dR/c and inverting gives dR = k·c/(N·Δf). The previous c/(2·N·Δf) reported HALF the
  // true differential range.
  //
  // This was invisible to tests/isac_sync_test.cc because that test is CIRCULAR on this point: it
  // synthesises its target with target_delay_s = 2·range/c and then checks range/range_res_m, so the
  // erroneous 2 cancels itself and the test passes under either convention. It was caught only by
  // comparing live sensing_sim RVMs against the harness's own physically-derived ground truth
  // (openair1/SIMULATION/TOOLS/sensing_channel.c uses tau = dR/c, the correct bistatic convention).
  //
  // Bin indices are UNCHANGED (a relabelling). Anything configured in absolute metres against this
  // axis must be re-tuned by 2x: `zero_range_guard` is in BINS and is unaffected, but `eca_delay_max_m`
  // is in metres (eca_clutter.cc converts it with the same formula, kept in sync) — and note that a
  // guard/notch specified in bins now masks TWICE the metric range it used to.
  rvm.range_res_m      = (float)(SPEED_OF_LIGHT / ((double)nof_range * df_comb));
  rvm.range_max_m      = (float)(SPEED_OF_LIGHT / df_comb);
  // Velocity axis = BISTATIC RANGE-RATE (dR/dt), NOT a monostatic radial velocity.
  //
  // CALIBRATION FIX (2026-07-23), found by comparing live sensing_sim detections against the
  // harness's own ground-truth log: these two lines previously carried the monostatic round-trip
  // factor 2 (vel = f_d·λ/2, i.e. c/(2·fc·N·T) and c/(4·fc·T)), which is wrong for this pipeline.
  // A bistatic scatterer's tap phase advances as exp(-j2π·R(t)/λ) where R is the FULL Tx→target→Rx
  // path (openair1/SIMULATION/TOOLS/sensing_channel.c uses exactly this), so its slow-time frequency
  // is f_d = Ṙ/λ with NO factor 2 — the "there and back" the monostatic factor accounts for is
  // already inside R. Inverting with the monostatic relation therefore reported HALF the true
  // bistatic range-rate. That also made the velocity axis inconsistent with the RANGE axis, which
  // already reports differential bistatic range (dR = R − R_los), and with the ground-truth log,
  // which prints range_rate = Ṙ — so GT-vs-detection velocity comparisons were off by 2x.
  // Correct inversion: Ṙ = f_d·λ, giving c/(fc·N·T) per bin and ±c/(2·fc·T) unambiguous.
  //
  // Bin indices are UNCHANGED by this fix (it is a relabelling of the same axis) — only the m/s
  // values reported in sensing_detection_t / sensing_rvm_t / DetectionReport change. Anything
  // configured in absolute m/s against this axis must be re-tuned by 2x: `eca_doppler_max_mps`
  // (eca_clutter.cc converts it to bins with the same formula, kept in sync below) and any
  // velocity-band analysis scripts. `zero_doppler_guard` is specified in BINS and is unaffected.
  rvm.vel_res_mps      = (fc > 0.0) ? (float)(SPEED_OF_LIGHT / (fc * (double)nof_dopp * t_slow)) : 0.0f;
  rvm.vel_max_mps      = (fc > 0.0) ? (float)(SPEED_OF_LIGHT / (2.0 * fc * t_slow)) : 0.0f;

  // Working copy of the CPI (so the caller's matrix and self-test injection stay isolated)
  work.assign(h_cpi, h_cpi + (size_t)nof_slow * nof_subc);

  if (args.selftest || !targets_.empty()) {
    inject_selftest(work.data(), nof_slow, nof_subc, df_comb, t_slow, fc);
  }

  // Clutter removal. Default: subtract the per-subcarrier slow-time mean (suppresses the
  // static/zero-Doppler LOS). ECA+ (opt-in): CFR-domain oblique projection that additionally removes
  // the near-zero-Doppler clutter *band* over a bounded delay window, killing the static/slow residual
  // that produces the conjugate mirror ghost — see eca_clutter.h / ECA_CLUTTER_HANDOVER.md.
  if (eca_) {
    eca_->remove(work.data(), nof_slow, nof_subc, df_comb, t_slow, fc);
  } else {
    for (uint32_t c = 0; c < nof_subc; c++) {
      double acc_re = 0.0, acc_im = 0.0;
      for (uint32_t n = 0; n < nof_slow; n++) {
        const icf_t v = work[(size_t)n * nof_subc + c];
        acc_re += v.real();
        acc_im += v.imag();
      }
      const icf_t mean((float)(acc_re / (double)nof_slow), (float)(acc_im / (double)nof_slow));
      for (uint32_t n = 0; n < nof_slow; n++) {
        work[(size_t)n * nof_subc + c] -= mean;
      }
    }
  }

  // Spectral whitening (opt-in, args.range_whiten): after clutter removal, attenuate subcarriers
  // whose slow-time RMS exceeds the across-subcarrier median. With irregular per-slot pdsch_data
  // occupancy a few subcarriers carry disproportionate energy; a near-impulse in the subcarrier
  // domain becomes a FLAT pedestal across the whole range axis after the IFFT (RVM-confirmed as a
  // strong target's smear at its own Doppler + harmonics -- see defs_nr_UE_ISAC.h). scale =
  // median/max(rms,median) is <=1 for hot subcarriers and exactly 1 at/below the median, so no weak
  // or empty subcarrier is ever amplified (would raise the noise floor). Phase is untouched, so a
  // real target's across-subcarrier phase ramp -- the thing that localises it in range -- is
  // preserved; only the disproportionate magnitude of the pedestal-driving subcarriers is capped.
  if (args.range_whiten && nof_subc > 1) {
    whiten_rms.assign(nof_subc, 0.0f);
    for (uint32_t c = 0; c < nof_subc; c++) {
      double acc = 0.0;
      for (uint32_t n = 0; n < nof_slow; n++) {
        const icf_t v = work[(size_t)n * nof_subc + c];
        acc += (double)v.real() * v.real() + (double)v.imag() * v.imag();
      }
      whiten_rms[c] = (float)std::sqrt(acc / (double)nof_slow);
    }
    // Median RMS across subcarriers (nth_element on a scratch copy; O(n), no full sort).
    whiten_sorted.assign(whiten_rms.begin(), whiten_rms.end());
    std::nth_element(whiten_sorted.begin(), whiten_sorted.begin() + nof_subc / 2, whiten_sorted.end());
    const float med_rms = whiten_sorted[nof_subc / 2];
    if (med_rms > 0.0f) {
      for (uint32_t c = 0; c < nof_subc; c++) {
        const float scale = (whiten_rms[c] > med_rms) ? (med_rms / whiten_rms[c]) : 1.0f;
        if (scale < 1.0f) {
          for (uint32_t n = 0; n < nof_slow; n++) {
            work[(size_t)n * nof_subc + c] *= scale;
          }
        }
      }
    }
  }

  // Frequency (range) window to suppress range sidelobes. Default Hann; opt-in equiripple
  // Dolph-Chebyshev (args.range_window) trades a wider mainlobe for much deeper, controlled
  // sidelobes -- see build_chebyshev_window() above and SYNC_NOISE_HANDOVER.md's LOS-skirt finding.
  if (freq_hann.size() != nof_subc) {
    freq_hann.resize(nof_subc);
    if (args.range_window == "chebyshev") {
      build_chebyshev_window(nof_subc, (double)args.range_window_sidelobe_db, freq_hann);
    } else {
      for (uint32_t c = 0; c < nof_subc; c++) {
        freq_hann[c] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)c / (float)(nof_subc - 1)));
      }
    }
  }

  // Range: windowed IFFT along frequency for each slow-time row -> range-major CIR [nof_range][nof_slow]
  //
  // Per-row de-aliasing: a row whose real reference samples are comb-N spaced physically supports
  // unambiguous range only out to nof_range/N bins (= c/(2*N*SCS)). Beyond that, the row's range
  // profile is aliased replicas of its near-range content — the "grating lobes" that a sparse CSI-RS
  // (comb-12) row otherwise scatters across the whole 0..range_max axis. Taper those replica bins to
  // zero per row, keeping only the row's valid window. A dense comb-1 row keeps the full range, so a
  // CSI-RS-only CPI collapses to its true ~416 m window while fused dense rows still reach far range.
  // Matched-filter mode (args.detector=="matched_filter"): build each row's range response over ONLY
  // its OCCUPIED subcarriers (no interpolated/held gap-fill -> no range pedestal) and energy-normalise
  // the row by its occupancy fraction so a sparsely-scheduled slot contributes as much SIGNAL as a
  // full one -- this removes the amplitude modulation irregular scheduling imposes on the slow-time
  // signal, i.e. removes the gating harmonics at the source. Requires the occupancy mask; falls back
  // to the plain windowed transform when unavailable. See defs_nr_UE_ISAC.h's `detector` comment.
  const bool matched_filter = (args.detector == "matched_filter") && (occ_mask != nullptr);

  // CLEAN deconvolution validity (see clean_deconv.h): needs a shift-invariant operator, i.e. NOT
  // matched_filter (per-row occupancy masking) and a uniform full-band comb (no per-row de-alias
  // taper). Under a fused multi-comb grid the range PSF differs per row -> non-separable -> skip.
  bool uniform_comb = true;
  if (row_comb != nullptr) {
    for (uint32_t n = 0; n < nof_slow; n++) {
      if (row_comb[n] > 1) {
        uniform_comb = false;
        break;
      }
    }
  }
  // Occupancy-aware forward-model CLEAN (clean_occ_aware): models each component's PSF through the real
  // per-row occupancy so the amplitude-gating harmonic replicas are reproduced and subtracted. Needs
  // the occupancy mask; runs its own range+Doppler+CLEAN+CFAR and returns early. Takes precedence over
  // the separable variant when both are set.
  const bool use_nudft_early = (args.doppler_nudft || matched_filter || (args.clean_deconv && args.clean_occ_aware)) &&
                               row_time_slots != nullptr && period_slots > 0.0f;
  if (args.clean_deconv && args.clean_occ_aware && occ_mask != nullptr) {
    clean_occ_aware(work.data(), nof_slow, nof_subc, nof_range, nof_dopp, occ_mask, row_comb,
                    row_time_slots, period_slots, use_nudft_early, rvm);
    cfar(rvm, detections, nof_slow, row_time_slots, period_slots);
    clean_prev_raw_det_ = (uint32_t)detections.size();
    return;
  }

  // Separable window-PSF CLEAN validity (see clean_deconv.h): needs a shift-invariant operator, i.e.
  // NOT matched_filter (per-row occupancy masking) and a uniform full-band comb (no per-row de-alias
  // taper). Under a fused multi-comb grid the range PSF differs per row -> non-separable -> skip.
  bool want_clean = args.clean_deconv && !args.clean_occ_aware && !matched_filter && uniform_comb;
  if (args.clean_deconv && !args.clean_occ_aware && !want_clean && !clean_warned_) {
    LOG_W(PHY, "SENSING: clean_deconv requested but operator is non-separable (%s%s) -- CLEAN skipped\n",
          matched_filter ? "matched_filter " : "", uniform_comb ? "" : "multi-comb-grid");
    clean_warned_ = true;
  }

  static constexpr uint32_t DEALIAS_TAPER = 4; // raised-cosine rolloff width (bins) before the cut
  cir_rm.assign((size_t)nof_range * nof_slow, icf_t(0.0f, 0.0f));
  range_in.resize(nof_subc);
  range_out.resize(nof_range);
  for (uint32_t n = 0; n < nof_slow; n++) {
    float row_norm = 1.0f;
    if (matched_filter) {
      uint32_t occ_count = 0;
      for (uint32_t c = 0; c < nof_subc; c++) {
        if (occ_mask[(size_t)n * nof_subc + c]) {
          range_in[c] = work[(size_t)n * nof_subc + c] * freq_hann[c];
          occ_count++;
        } else {
          range_in[c] = icf_t(0.0f, 0.0f); // occupied-only: unobserved subcarriers contribute nothing
        }
      }
      // Per-row occupancy normalisation: scale so every row's occupied energy is comparable, cancelling
      // the amplitude gating. sqrt(N/occ) keeps a full row at unit gain and boosts sparse rows.
      if (args.mf_per_row_norm && occ_count > 0) {
        row_norm = std::sqrt((float)nof_subc / (float)occ_count);
      }
    } else {
      for (uint32_t c = 0; c < nof_subc; c++) {
        range_in[c] = work[(size_t)n * nof_subc + c] * freq_hann[c];
      }
    }
    range_plan->run(range_in.data(), range_out.data());
    if (row_norm != 1.0f) {
      for (uint32_t r = 0; r < nof_range; r++) {
        range_out[r] *= row_norm;
      }
    }

    uint32_t valid = nof_range; // no clipping by default (row_comb absent or comb<=1)
    if (row_comb != nullptr && row_comb[n] > 1) {
      valid = nof_range / row_comb[n];
      if (valid < 1) {
        valid = 1;
      }
    }
    for (uint32_t r = 0; r < nof_range; r++) {
      float g = 1.0f;
      if (r >= valid) {
        g = 0.0f;
      } else if (valid > DEALIAS_TAPER && r >= valid - DEALIAS_TAPER) {
        g = 0.5f * (1.0f + std::cos((float)M_PI * (float)(r - (valid - DEALIAS_TAPER)) / (float)DEALIAS_TAPER));
      }
      cir_rm[(size_t)r * nof_slow + n] = (g == 1.0f) ? range_out[r] : range_out[r] * g;
    }
  }

  // ---- Measured gating spectrum (gating_reject) -------------------------------------------------
  // The scheduling-driven replicas are an AMPLITUDE-MODULATION product: the per-row received energy
  // rises and falls as the scheduler grants more or fewer REs, and multiplying a target's slow-time
  // tone by that envelope convolves its Doppler line with the envelope's spectrum -- putting copies at
  // fd +/- (each envelope frequency). The existing harmonic_reject assumes those copies land at
  // INTEGER MULTIPLES k*v, which is only true for a strictly periodic gate; on real traffic the gate
  // is irregular (T_slot measured wandering 1.0-3.7 slots), so the offsets move CPI to CPI and the
  // integer-ratio test catches only part of the family.
  //
  // Here the offsets are MEASURED instead of assumed: take the row-energy envelope of the very rows
  // about to be transformed, remove its mean (the DC term is the wanted signal path, not a replica
  // generator), transform it with the same length as the Doppler axis -- so its bin indices ARE
  // Doppler-bin offsets, no unit conversion -- and keep the strongest few peaks. cfar() then rejects a
  // detection sitting at one of those offsets from a stronger same-range detection.
  //
  // Using row energy rather than the occupancy mask is deliberate: it is the physical modulating
  // quantity, it is defined on exactly the rows being transformed (no correspondence problem if
  // slow-time resampling re-times the rows), and it needs nothing passed in from the engine.
  gating_offsets_.clear();
  if (args.gating_reject && nof_slow >= 8) {
    std::vector<float> env(nof_slow, 0.0f);
    for (uint32_t n = 0; n < nof_slow; n++) {
      double acc = 0.0;
      for (uint32_t c = 0; c < nof_subc; c++) {
        acc += std::norm(work[(size_t)n * nof_subc + c]);
      }
      env[n] = (float)std::sqrt(acc); // amplitude envelope
    }
    double mean = 0.0;
    for (float e : env) mean += e;
    mean /= (double)nof_slow;
    dopp_in.resize(nof_slow);
    for (uint32_t n = 0; n < nof_slow; n++) {
      dopp_in[n] = icf_t((float)((double)env[n] - mean), 0.0f);
    }
    dopp_out.resize(nof_slow);
    dopp_plan->run(dopp_in.data(), dopp_out.data());
    // Peak-pick over the positive half (the envelope is real, so the spectrum is symmetric).
    const uint32_t nhalf = nof_slow / 2;
    float peak = 0.0f;
    for (uint32_t k = 1; k < nhalf; k++) peak = std::max(peak, std::abs(dopp_out[k]));
    if (peak > 0.0f) {
      std::vector<std::pair<float, uint32_t>> cands;
      for (uint32_t k = 2; k + 1 < nhalf; k++) { // skip the lowest bins: near-DC drift is not a gate
        const float m = std::abs(dopp_out[k]);
        if (m >= args.gating_min_rel * peak && m >= std::abs(dopp_out[k - 1]) && m >= std::abs(dopp_out[k + 1])) {
          cands.emplace_back(m, k);
        }
      }
      std::sort(cands.begin(), cands.end(), [](auto& a, auto& b) { return a.first > b.first; });
      for (const auto& c : cands) {
        if (gating_offsets_.size() >= args.gating_max_offsets) break;
        gating_offsets_.push_back(c.second);
      }
    }
  }

  // Slow-time Hann window
  if (hann.size() != nof_slow) {
    hann.resize(nof_slow);
    for (uint32_t n = 0; n < nof_slow; n++) {
      hann[n] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(nof_slow - 1)));
    }
  }

  // Doppler transform along slow-time for each range bin, with fftshift so zero-Doppler is centred.
  // Two paths: (a) the default uniform forward FFT, and (b) a NON-UNIFORM DFT (args.doppler_nudft)
  // that evaluates the spectrum at each row's ACTUAL time so the harmonics irregular sampling would
  // otherwise generate never appear -- see defs_nr_UE_ISAC.h. The NUDFT matrix depends only on the
  // row times, so it is built once here and reused for every range bin.
  // Matched filter integrates coherently at the TRUE sample times -> its slow-time stage IS the NUDFT
  // (a uniform FFT here would re-introduce the very harmonics the occ-only range stage just avoided).
  const bool use_nudft = (args.doppler_nudft || matched_filter) && row_time_slots != nullptr && period_slots > 0.0f;
  if (use_nudft) {
    // W[d*N + n] = exp(-j*2*pi*d*tau_n/N)/sqrt(N), tau_n = t_n/T_mean (normalised sample index). For
    // uniform sampling tau_n = n and this is exactly the forward DFT matrix (matches dopp_plan's
    // 1/sqrt(N) normalisation), so enabling the mode is behaviour-preserving on evenly-spaced rows.
    nudft_mat.resize((size_t)nof_dopp * nof_slow);
    const double invN = 1.0 / (double)nof_slow;
    const double norm = 1.0 / std::sqrt((double)nof_slow);
    for (uint32_t d = 0; d < nof_dopp; d++) {
      for (uint32_t n = 0; n < nof_slow; n++) {
        const double tau = row_time_slots[n] / (double)period_slots;
        const double ph  = -2.0 * M_PI * (double)d * tau * invN;
        nudft_mat[(size_t)d * nof_slow + n] = icf_t((float)(std::cos(ph) * norm), (float)(std::sin(ph) * norm));
      }
    }
  }

  rvm.power.assign((size_t)nof_range * nof_dopp, 0.0f);
  if (want_clean) {
    cmap.assign((size_t)nof_range * nof_dopp, icf_t(0.0f, 0.0f));
  }
  dopp_in.resize(nof_slow);
  dopp_out.resize(nof_slow);
  const uint32_t half = nof_dopp / 2;
  for (uint32_t r = 0; r < nof_range; r++) {
    for (uint32_t n = 0; n < nof_slow; n++) {
      dopp_in[n] = cir_rm[(size_t)r * nof_slow + n] * hann[n];
    }
    if (use_nudft) {
      for (uint32_t d = 0; d < nof_dopp; d++) {
        const icf_t* w = &nudft_mat[(size_t)d * nof_slow];
        icf_t        acc(0.0f, 0.0f);
        for (uint32_t n = 0; n < nof_slow; n++) {
          acc += dopp_in[n] * w[n];
        }
        dopp_out[d] = acc;
      }
    } else {
      dopp_plan->run(dopp_in.data(), dopp_out.data());
    }
    for (uint32_t d = 0; d < nof_dopp; d++) {
      const uint32_t ds = (d + half) % nof_dopp; // fftshift
      const float    re = dopp_out[d].real();
      const float    im = dopp_out[d].imag();
      rvm.power[(size_t)r * nof_dopp + ds] = re * re + im * im;
      if (want_clean) {
        cmap[(size_t)r * nof_dopp + ds] = dopp_out[d];
      }
    }
  }

  // Notch static clutter: zero the zero-Doppler band (across all ranges) and the near-zero-range band
  // (direct-path / LOS). This removes the residual LOS and the zero-Doppler ghost that mean-subtraction
  // leaves at a moving target's range, and keeps the strong clutter out of the CFAR noise estimate.
  // Clutter-notch widths. `adaptive_clutter_guard` derives both from THIS CPI's own map (see
  // auto_notch_width): the notch covers one physical thing -- the direct path and the static return
  // around it -- and its extent is set by the window mainlobe, residual sync error and the clutter's
  // own spread, none of which a fixed number can follow across deployments. The configured values
  // remain the fallback when no dominant clutter is present.
  int zdg = (int)args.zero_doppler_guard;
  int zrg = (int)args.zero_range_guard;
  if (args.adaptive_clutter_guard) {
    std::vector<double> pd(nof_dopp, 0.0), pr(nof_range, 0.0);
    for (uint32_t r = 0; r < nof_range; r++) {
      const float* row = &rvm.power[(size_t)r * nof_dopp];
      for (uint32_t d = 0; d < nof_dopp; d++) {
        pd[d] += (double)row[d];
        pr[r] += (double)row[d];
      }
    }
    // Doppler profile is centred on the DC bin (`half`); the range profile starts at bin 0, so it is
    // already "centred" at its own origin.
    zdg = auto_notch_width(pd, half, zdg);
    zrg = auto_notch_width(pr, 0, zrg);
    if ((notch_log_++ % 20) == 0) {
      LOG_I(PHY, "SENSING: adaptive clutter guard -> zero_doppler=%d zero_range=%d bins\n", zdg, zrg);
    }
  }
  // Far-range notch (opt-in, max_range_m). The range axis routinely extends far beyond any range the
  // channel can physically deliver -- on tests/sensing_sim the axis spans ~10 km while the simulator's
  // CIR is capped at 255 taps (~622 m), and MEASURED on a 156-CPI capture, 39 % of all detections sat
  // beyond that cap. Those are artifacts by construction, not weak targets: no echo can exist there.
  // Notched HERE, with the other guards and before CFAR, rather than filtered afterwards, so they also
  // stop inflating the CFAR noise estimate -- which is what lets them cost real detections and not
  // merely add false ones. Set it to the deployment's true maximum observable range; 0 = disabled.
  // Far-range notch. > 0 = an explicit maximum observable range; < 0 = AUTO; 0 = disabled.
  // AUTO derives the horizon from the data instead of a surveyed number: integrate power over
  // Doppler for each range bin, then walk in from the far end while the profile stays statistically
  // indistinguishable from its own far-end floor. Where a real echo can still arrive the profile
  // lifts above that floor; beyond the channel's reach it does not. Uses the same robust
  // median/MAD estimator as det_quality, so "indistinguishable" means the same thing everywhere.
  int max_bin = (int)nof_range;
  if (args.max_range_m > 0.0f && rvm.range_res_m > 0.0f) {
    max_bin = (int)(args.max_range_m / rvm.range_res_m);
  } else if (args.max_range_m < 0.0f) {
    max_bin = auto_range_horizon_bin(rvm, nof_range, nof_dopp);
  }
  for (uint32_t r = 0; r < nof_range; r++) {
    const bool zero_range = (int)r <= zrg || (int)r >= (int)nof_range - 1 - zrg;
    const bool beyond_max = (int)r > max_bin;
    for (uint32_t d = 0; d < nof_dopp; d++) {
      const bool zero_dopp = std::abs((int)d - (int)half) <= zdg;
      if (zero_range || zero_dopp || beyond_max) {
        rvm.power[(size_t)r * nof_dopp + d] = 0.0f;
        if (want_clean) {
          cmap[(size_t)r * nof_dopp + d] = icf_t(0.0f, 0.0f);
        }
      }
    }
  }

  // CLEAN deconvolution (opt-in, clean_deconv.h): coherently strip each strong scatterer's separable
  // window PSF (range pedestal + Doppler sidelobes) from the COMPLEX map before CFAR, so its skirt
  // can't spawn ghost detections. The notch above already zeroed the LOS/zero-Doppler cells in both
  // rvm.power and cmap, keeping them out of CLEAN's peak search. Overwrites rvm.power with the cleaned
  // power; the notch is re-applied afterwards (a restored clean beam near the notch edge must not leak
  // back into the guarded band).
  if (want_clean) {
    build_clean_kernels(nof_range, nof_dopp, use_nudft);
    clean_deconv_params cp;
    cp.max_components = clean_components_budget(); // auto (prev-CPI det count) or the manual pin
    cp.loop_gain     = args.clean_loop_gain;
    cp.stop_db       = args.clean_stop_db;
    cp.restore_bins  = args.clean_restore_bins;
    uint32_t n_comp  = 0;
    clean_deconv_run(cmap.data(), nof_range, nof_dopp, clean_kr.data(), clean_kd.data(), cp,
                     rvm.power.data(), &n_comp);
    for (uint32_t r = 0; r < nof_range; r++) {
      const bool zero_range = (int)r <= zrg || (int)r >= (int)nof_range - 1 - zrg;
      for (uint32_t d = 0; d < nof_dopp; d++) {
        const bool zero_dopp = std::abs((int)d - (int)half) <= zdg;
        if (zero_range || zero_dopp) {
          rvm.power[(size_t)r * nof_dopp + d] = 0.0f;
        }
      }
    }
    clean_last_components_ = n_comp;
  }

  // 2D CA-CFAR detection + non-max suppression
  cfar(rvm, detections, nof_slow, row_time_slots, period_slots);

  // Auto-budget feedback: record this CPI's raw detection count so the next CPI can size CLEAN's
  // component budget from measured scene occupancy (same "measure, don't guess" pattern as mc_rank).
  clean_prev_raw_det_ = (uint32_t)detections.size();
}

// Builds the separable, shift-invariant CLEAN PSF kernels for the current CPI geometry: kr (range
// axis) = the range operator's response to a unit scatterer at range-bin 0, and kd (Doppler axis) =
// the Doppler operator's response to a zero-Doppler scatterer, each normalised to (1,0) at zero lag
// and expressed in the SAME fftshifted map coordinates the data uses. Rebuilt per CPI (one range +
// one Doppler transform -- negligible next to the nof_range Doppler transforms of the main path) so
// it always tracks the active window (hann/chebyshev) and Doppler mode (FFT/NUDFT).
void range_doppler::build_clean_kernels(uint32_t nof_range, uint32_t nof_dopp, bool use_nudft)
{
  // Range kernel: IFFT of the (windowed) unit scatterer at bin 0 -> range_in[m] = freq_hann[m].
  range_in.assign(nof_range, icf_t(0.0f, 0.0f));
  for (uint32_t m = 0; m < nof_range; m++) {
    range_in[m] = icf_t(freq_hann[m], 0.0f);
  }
  range_out.resize(nof_range);
  range_plan->run(range_in.data(), range_out.data());
  clean_kr.assign(nof_range, icf_t(0.0f, 0.0f));
  const icf_t kr0 = range_out[0];
  const float kr0n = std::norm(kr0);
  if (kr0n > 0.0f) {
    for (uint32_t r = 0; r < nof_range; r++) {
      clean_kr[r] = range_out[r] / kr0; // complex-normalise so kr[0] = (1,0)
    }
  } else {
    clean_kr[0] = icf_t(1.0f, 0.0f);
  }

  // Doppler kernel: transform of a zero-Doppler scatterer -> dopp_in[n] = hann[n]. Then fftshift so
  // zero Doppler maps to ds = half, and re-index by lag from ds=half. (nof_dopp == nof_slow here.)
  dopp_in.assign(nof_dopp, icf_t(0.0f, 0.0f));
  for (uint32_t n = 0; n < nof_dopp; n++) {
    dopp_in[n] = icf_t(hann[n], 0.0f);
  }
  dopp_out.resize(nof_dopp);
  if (use_nudft) {
    for (uint32_t d = 0; d < nof_dopp; d++) {
      const icf_t* w = &nudft_mat[(size_t)d * nof_dopp];
      icf_t        acc(0.0f, 0.0f);
      for (uint32_t n = 0; n < nof_dopp; n++) {
        acc += dopp_in[n] * w[n];
      }
      dopp_out[d] = acc;
    }
  } else {
    dopp_plan->run(dopp_in.data(), dopp_out.data());
  }
  // In fftshifted map coords the zero-Doppler peak sits at ds = half; a cell at map-lag `lag` from the
  // peak (ds = half+lag) is fed by dopp_out[lag mod N] (since (ds+half)%N picks bin `lag`). So the
  // lag-indexed kernel is simply dopp_out normalised by its DC bin dopp_out[0] (the peak).
  clean_kd.assign(nof_dopp, icf_t(0.0f, 0.0f));
  const icf_t kd0 = dopp_out[0];
  if (std::norm(kd0) > 0.0f) {
    for (uint32_t lag = 0; lag < nof_dopp; lag++) {
      clean_kd[lag] = dopp_out[lag] / kd0;
    }
  } else {
    clean_kd[0] = icf_t(1.0f, 0.0f);
  }
}

// CLEAN component budget: manual pin (clean_max_components > 0) or AUTO from the previous CPI's raw
// detection count plus a small margin, clamped to [1, clean_max_components_cap]. First CPI (no history)
// seeds at the cap so nothing is starved before a measurement exists.
uint32_t range_doppler::clean_components_budget() const
{
  if (args.clean_max_components > 0) {
    return args.clean_max_components;
  }
  uint32_t cap = args.clean_max_components_cap > 0 ? args.clean_max_components_cap : 16;
  if (clean_prev_raw_det_ == 0) {
    return cap; // no measurement yet
  }
  uint32_t budget = clean_prev_raw_det_ + 2; // margin so real targets aren't clipped by the budget
  return std::min(budget, cap);
}

// Pushes @p grid through the exact range+Doppler operator WITH per-row occupancy masking (occ-only
// subcarriers, NO per-row energy normalisation -- the gating must survive) and de-alias taper, writing
// the complex fftshifted map to @p cmap_out. Identical transform for the data grid and each CLEAN
// component's synthetic grid, which is what makes the coherent subtraction operator-consistent and
// lets a component's modelled amplitude-gating Doppler-harmonic replicas cancel the data's.
void range_doppler::occ_forward_transform(const icf_t* grid, uint32_t nof_slow, uint32_t nof_subc,
                                          uint32_t nof_range, uint32_t nof_dopp, const uint8_t* occ_mask,
                                          const uint32_t* row_comb, const double* row_time_slots,
                                          float period_slots, bool use_nudft, icf_t* cmap_out)
{
  static constexpr uint32_t DEALIAS_TAPER = 4;
  // Windows: freq_hann is already built by process() before the occ-aware branch; ensure the slow-time
  // Hann exists (process() builds it only later on the normal path).
  if (hann.size() != nof_slow) {
    hann.resize(nof_slow);
    for (uint32_t n = 0; n < nof_slow; n++) {
      hann[n] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(nof_slow - 1)));
    }
  }

  // Range: occ-masked windowed IFFT per slow-time row -> range-major CIR, with per-row de-alias taper.
  cir_rm.assign((size_t)nof_range * nof_slow, icf_t(0.0f, 0.0f));
  range_in.resize(nof_subc);
  range_out.resize(nof_range);
  for (uint32_t n = 0; n < nof_slow; n++) {
    for (uint32_t c = 0; c < nof_subc; c++) {
      range_in[c] = occ_mask[(size_t)n * nof_subc + c] ? (grid[(size_t)n * nof_subc + c] * freq_hann[c])
                                                       : icf_t(0.0f, 0.0f);
    }
    range_plan->run(range_in.data(), range_out.data());
    uint32_t valid = nof_range;
    if (row_comb != nullptr && row_comb[n] > 1) {
      valid = std::max(1u, nof_range / row_comb[n]);
    }
    for (uint32_t r = 0; r < nof_range; r++) {
      float g = 1.0f;
      if (r >= valid) {
        g = 0.0f;
      } else if (valid > DEALIAS_TAPER && r >= valid - DEALIAS_TAPER) {
        g = 0.5f * (1.0f + std::cos((float)M_PI * (float)(r - (valid - DEALIAS_TAPER)) / (float)DEALIAS_TAPER));
      }
      cir_rm[(size_t)r * nof_slow + n] = (g == 1.0f) ? range_out[r] : range_out[r] * g;
    }
  }

  // Doppler: NUDFT over the true row times (occ-aware always bypasses resampling, so rows are the raw
  // irregular occurrences) or uniform FFT. Matrix built once per call.
  if (use_nudft) {
    nudft_mat.resize((size_t)nof_dopp * nof_slow);
    const double invN = 1.0 / (double)nof_slow;
    const double norm = 1.0 / std::sqrt((double)nof_slow);
    for (uint32_t d = 0; d < nof_dopp; d++) {
      for (uint32_t n = 0; n < nof_slow; n++) {
        const double tau = row_time_slots[n] / (double)period_slots;
        const double ph  = -2.0 * M_PI * (double)d * tau * invN;
        nudft_mat[(size_t)d * nof_slow + n] = icf_t((float)(std::cos(ph) * norm), (float)(std::sin(ph) * norm));
      }
    }
  }
  dopp_in.resize(nof_slow);
  dopp_out.resize(nof_slow);
  const uint32_t half = nof_dopp / 2;
  for (uint32_t r = 0; r < nof_range; r++) {
    for (uint32_t n = 0; n < nof_slow; n++) {
      dopp_in[n] = cir_rm[(size_t)r * nof_slow + n] * hann[n];
    }
    if (use_nudft) {
      for (uint32_t d = 0; d < nof_dopp; d++) {
        const icf_t* w = &nudft_mat[(size_t)d * nof_slow];
        icf_t        acc(0.0f, 0.0f);
        for (uint32_t n = 0; n < nof_slow; n++) {
          acc += dopp_in[n] * w[n];
        }
        dopp_out[d] = acc;
      }
    } else {
      dopp_plan->run(dopp_in.data(), dopp_out.data());
    }
    for (uint32_t d = 0; d < nof_dopp; d++) {
      const uint32_t ds            = (d + half) % nof_dopp; // fftshift
      cmap_out[(size_t)r * nof_dopp + ds] = dopp_out[d];
    }
  }
}

// Occupancy-aware forward-model CLEAN: build the data map through the occ operator, then iteratively
// find the brightest cell, forward-model a unit scatterer at that (range,Doppler) THROUGH THE SAME occ
// operator (so its modelled response carries the true amplitude-gating harmonic replicas), and subtract
// a loop-gain fraction. The residual + restored clean beams -> |.|^2 -> rvm.power for CFAR.
void range_doppler::clean_occ_aware(const icf_t* work_grid, uint32_t nof_slow, uint32_t nof_subc,
                                    uint32_t nof_range, uint32_t nof_dopp, const uint8_t* occ_mask,
                                    const uint32_t* row_comb, const double* row_time_slots,
                                    float period_slots, bool use_nudft, sensing_rvm_t& rvm)
{
  const size_t   NRD  = (size_t)nof_range * nof_dopp;
  const uint32_t half = nof_dopp / 2;
  ensure_plans(nof_range, nof_slow);

  // Data map through the occ operator.
  clean_res.assign(NRD, icf_t(0.0f, 0.0f));
  occ_forward_transform(work_grid, nof_slow, nof_subc, nof_range, nof_dopp, occ_mask, row_comb,
                        row_time_slots, period_slots, use_nudft, clean_res.data());

  // Notch (same guards as the normal path) so LOS/zero-Doppler stays out of the peak search.
  const int zdg = (int)args.zero_doppler_guard;
  const int zrg = (int)args.zero_range_guard;
  auto apply_notch = [&](icf_t* m) {
    for (uint32_t r = 0; r < nof_range; r++) {
      const bool zero_range = (int)r <= zrg || (int)r >= (int)nof_range - 1 - zrg;
      for (uint32_t d = 0; d < nof_dopp; d++) {
        const bool zero_dopp = std::abs((int)d - (int)half) <= zdg;
        if (zero_range || zero_dopp) {
          m[(size_t)r * nof_dopp + d] = icf_t(0.0f, 0.0f);
        }
      }
    }
  };
  apply_notch(clean_res.data());

  // CLEAN loop.
  float peak0 = 0.0f;
  for (size_t i = 0; i < NRD; i++) {
    peak0 = std::max(peak0, std::norm(clean_res[i]));
  }
  const float stop_pw = peak0 * std::pow(10.0f, -std::abs(args.clean_stop_db) / 10.0f);
  const float gamma   = (args.clean_loop_gain > 0.0f && args.clean_loop_gain <= 1.0f) ? args.clean_loop_gain : 0.8f;
  const uint32_t budget = clean_components_budget();

  clean_psf.assign(NRD, icf_t(0.0f, 0.0f));
  clean_synth.assign((size_t)nof_slow * nof_subc, icf_t(0.0f, 0.0f));
  std::vector<uint32_t> comp_r, comp_d;
  std::vector<icf_t>    comp_a;
  uint32_t n_comp = 0;

  for (uint32_t it = 0; it < budget && peak0 > 0.0f; it++) {
    float    best = 0.0f;
    uint32_t br = 0, bd = 0;
    for (uint32_t r = 0; r < nof_range; r++) {
      const icf_t* row = &clean_res[(size_t)r * nof_dopp];
      for (uint32_t d = 0; d < nof_dopp; d++) {
        const float pw = std::norm(row[d]);
        if (pw > best) { best = pw; br = r; bd = d; }
      }
    }
    if (best < stop_pw || best <= 0.0f) {
      break;
    }
    // Synthesise a unit scatterer at (br,bd): subcarrier signature exp(-j2pi m br/nof_range) and
    // slow-time signature exp(+j2pi (bd-half) tau_n/nof_dopp). occ_forward_transform re-applies the
    // occupancy masking, so the synthetic scatterer picks up exactly the data's gating modulation.
    const int      d0u = ((int)bd - (int)half + (int)nof_dopp) % (int)nof_dopp;
    for (uint32_t n = 0; n < nof_slow; n++) {
      const double tau       = use_nudft ? (row_time_slots[n] / (double)period_slots) : (double)n;
      const double slow_ph   = 2.0 * M_PI * (double)d0u * tau / (double)nof_dopp;
      const double cs = std::cos(slow_ph), sn = std::sin(slow_ph);
      icf_t* srow = &clean_synth[(size_t)n * nof_subc];
      for (uint32_t m = 0; m < nof_subc; m++) {
        const double rp = -2.0 * M_PI * (double)m * (double)br / (double)nof_range;
        // exp(j*rp) * exp(j*slow_ph)
        const double cr = std::cos(rp), sr = std::sin(rp);
        srow[m] = icf_t((float)(cr * cs - sr * sn), (float)(cr * sn + sr * cs));
      }
    }
    occ_forward_transform(clean_synth.data(), nof_slow, nof_subc, nof_range, nof_dopp, occ_mask, row_comb,
                          row_time_slots, period_slots, use_nudft, clean_psf.data());
    apply_notch(clean_psf.data());
    const icf_t g0 = clean_psf[(size_t)br * nof_dopp + bd];
    if (std::norm(g0) <= 0.0f) {
      break;
    }
    const icf_t peak_val = clean_res[(size_t)br * nof_dopp + bd];
    const icf_t c        = (peak_val / g0) * gamma; // coefficient scaling the unit PSF
    for (size_t i = 0; i < NRD; i++) {
      clean_res[i] -= c * clean_psf[i];
    }
    // Bank the extracted map-amplitude (gamma fraction of the peak) for the clean-beam restore.
    bool merged = false;
    for (size_t k = 0; k < comp_r.size(); k++) {
      if (comp_r[k] == br && comp_d[k] == bd) { comp_a[k] += peak_val * gamma; merged = true; break; }
    }
    if (!merged) { comp_r.push_back(br); comp_d.push_back(bd); comp_a.push_back(peak_val * gamma); }
    n_comp++;
  }
  clean_last_components_ = n_comp;

  // Restore: cleaned = residual + each banked component as a narrow clean beam; power -> rvm.power.
  const int hw = (int)args.clean_restore_bins;
  std::vector<float> bw(2 * hw + 1, 1.0f);
  if (hw > 0) {
    const float sigma = std::max(0.5f, (float)hw / 1.5f);
    for (int t = -hw; t <= hw; t++) {
      bw[t + hw] = std::exp(-0.5f * (float)(t * t) / (sigma * sigma));
    }
  }
  for (size_t k = 0; k < comp_r.size(); k++) {
    const int   r0 = (int)comp_r[k], d0 = (int)comp_d[k];
    const icf_t a  = comp_a[k];
    for (int dr = -hw; dr <= hw; dr++) {
      const int rr = ((r0 + dr) % (int)nof_range + (int)nof_range) % (int)nof_range;
      for (int dd = -hw; dd <= hw; dd++) {
        const int dc = ((d0 + dd) % (int)nof_dopp + (int)nof_dopp) % (int)nof_dopp;
        clean_res[(size_t)rr * nof_dopp + dc] += a * (bw[dr + hw] * bw[dd + hw]);
      }
    }
  }

  rvm.power.assign(NRD, 0.0f);
  for (size_t i = 0; i < NRD; i++) {
    rvm.power[i] = std::norm(clean_res[i]);
  }
  // Re-apply the notch on the power map (a restored beam near the guard edge must not leak in).
  for (uint32_t r = 0; r < nof_range; r++) {
    const bool zero_range = (int)r <= zrg || (int)r >= (int)nof_range - 1 - zrg;
    for (uint32_t d = 0; d < nof_dopp; d++) {
      const bool zero_dopp = std::abs((int)d - (int)half) <= zdg;
      if (zero_range || zero_dopp) {
        rvm.power[(size_t)r * nof_dopp + d] = 0.0f;
      }
    }
  }
}

void range_doppler::cfar(const sensing_rvm_t& rvm, std::vector<sensing_detection_t>& detections,
                         uint32_t nof_slow, const double* row_time_slots, float period_slots)
{
  const uint32_t R   = rvm.nof_range_bins;
  const uint32_t D   = rvm.nof_doppler_bins;
  const int      g   = (int)args.cfar_guard;
  const int      t   = (int)args.cfar_train;
  // Per-cell Pfa. args.cfar_pfa > 0 pins a manual value directly (legacy behaviour). Otherwise
  // (default) derive it from a grid-SIZE-INDEPENDENT target: cfar_pfa is a PER-CELL probability, so
  // the number of false alarms that actually appear scales with R*D (bandwidth * cpi_slots) -- a
  // fixed cfar_pfa tuned on one grid silently gives a different false-alarm COUNT on another. See
  // defs_nr_UE_ISAC.h's cfar_pfa/cfar_target_fa_per_cpi comment for the 2026-07-24 live finding this
  // responds to (a stale cfar_pfa=1e-4 predicted ~42 false alarms/CPI on a 420k-cell grid).
  // cfar_fa_adapt_enable: use the closed-loop state (seeded from args.cfar_target_fa_per_cpi, then
  // adjusted below from the MEASURED raw detection count) instead of the static config value. See
  // defs_nr_UE_ISAC.h's cfar_fa_adapt_enable comment.
  if (args.cfar_fa_adapt_enable && cfar_fa_state_ < 0.0f) {
    cfar_fa_state_ = args.cfar_target_fa_per_cpi;
  }
  const double   target_fa = args.cfar_fa_adapt_enable ? (double)cfar_fa_state_ : (double)args.cfar_target_fa_per_cpi;
  const double   cells = (double)R * (double)D;
  const double   pfa   = (args.cfar_pfa > 0.0f)
                             ? (double)args.cfar_pfa
                             : std::min(0.5, std::max(1e-12, target_fa / std::max(1.0, cells)));

  // Integral image of power (size (R+1)x(D+1)) for O(1) window sums
  integ.assign((size_t)(R + 1) * (D + 1), 0.0);
  const size_t stride = (size_t)(D + 1);
  for (uint32_t r = 0; r < R; r++) {
    double rowsum = 0.0;
    for (uint32_t d = 0; d < D; d++) {
      rowsum += (double)rvm.power[(size_t)r * D + d];
      integ[(size_t)(r + 1) * stride + (d + 1)] = integ[(size_t)r * stride + (d + 1)] + rowsum;
    }
  }

  auto rect_sum = [&](int r0, int d0, int r1, int d1) -> double {
    r0 = std::max(0, r0);
    d0 = std::max(0, d0);
    r1 = std::min((int)R - 1, r1);
    d1 = std::min((int)D - 1, d1);
    if (r1 < r0 || d1 < d0) {
      return 0.0;
    }
    return integ[(size_t)(r1 + 1) * stride + (d1 + 1)] - integ[(size_t)r0 * stride + (d1 + 1)] -
           integ[(size_t)(r1 + 1) * stride + d0] + integ[(size_t)r0 * stride + d0];
  };
  auto rect_cnt = [&](int r0, int d0, int r1, int d1) -> long {
    r0 = std::max(0, r0);
    d0 = std::max(0, d0);
    r1 = std::min((int)R - 1, r1);
    d1 = std::min((int)D - 1, d1);
    if (r1 < r0 || d1 < d0) {
      return 0;
    }
    return (long)(r1 - r0 + 1) * (long)(d1 - d0 + 1);
  };

  const float half_d = (float)D / 2.0f;
  for (uint32_t r = 0; r < R; r++) {
    for (uint32_t d = 0; d < D; d++) {
      const double outer_s = rect_sum((int)r - (g + t), (int)d - (g + t), (int)r + (g + t), (int)d + (g + t));
      const long   outer_c = rect_cnt((int)r - (g + t), (int)d - (g + t), (int)r + (g + t), (int)d + (g + t));
      const double guard_s = rect_sum((int)r - g, (int)d - g, (int)r + g, (int)d + g);
      const long   guard_c = rect_cnt((int)r - g, (int)d - g, (int)r + g, (int)d + g);
      const double train_s = outer_s - guard_s;
      const long   train_c = outer_c - guard_c;
      if (train_c <= 0) {
        continue;
      }
      const double noise = train_s / (double)train_c;
      if (noise <= 0.0) {
        continue;
      }
      const double alpha = (double)train_c * (std::pow(pfa, -1.0 / (double)train_c) - 1.0);
      const double thr   = alpha * noise;
      const double cell  = (double)rvm.power[(size_t)r * D + d];

      // Per-Doppler-column test (opt-in): the cell must also stand above the noise of its OWN velocity
      // lane, estimated from range-only training cells in the same column d (guard band excluded).
      // This is what rejects a strong target's range-wide pedestal, which uniformly elevates its own
      // Doppler column and would otherwise sail through the 2-D box test. See defs_nr_UE_ISAC.h.
      double thr_col   = 0.0;
      double col_noise = noise;
      if (args.cfar_per_column) {
        const double col_outer_s = rect_sum((int)r - (g + t), (int)d, (int)r + (g + t), (int)d);
        const long   col_outer_c = rect_cnt((int)r - (g + t), (int)d, (int)r + (g + t), (int)d);
        const double col_guard_s = rect_sum((int)r - g, (int)d, (int)r + g, (int)d);
        const long   col_guard_c = rect_cnt((int)r - g, (int)d, (int)r + g, (int)d);
        const long   col_train_c = col_outer_c - col_guard_c;
        if (col_train_c > 0) {
          col_noise = (col_outer_s - col_guard_s) / (double)col_train_c;
          if (col_noise > 0.0) {
            const double col_alpha = (double)col_train_c * (std::pow(pfa, -1.0 / (double)col_train_c) - 1.0);
            thr_col = col_alpha * col_noise;
          }
        }
      }

      // Per-RANGE-ROW test (opt-in): the mirror image of the column test above -- the cell must also
      // stand above the noise of its OWN RANGE row, estimated from Doppler-only training cells at the
      // same range r (guard band excluded). WHY (RVM-confirmed 2026-07-25, see
      // MULTISTATIC_FAST_TARGET_NOTES.md and the sub-slot RVM animation): the dominant surviving ghost
      // family is a horizontal RIDGE -- one scatterer's energy smeared along slow-time across its own
      // range row, spanning the whole velocity axis. Those ghosts are not independent false alarms,
      // they ARE the target's own signal mis-attributed to wrong velocities, which is why tightening
      // pfa never removed them. cfar_per_column trains along range (it kills the vertical, same-velocity
      // pedestal); a ridge runs the other way and sails straight through it. Training along DOPPLER
      // makes the ridge its own noise floor, so only a genuine peak standing above the ridge survives.
      // The two are complementary: enable both to bracket a strong target in each axis.
      double thr_row   = 0.0;
      double row_noise = noise;
      if (args.cfar_per_row) {
        const double row_outer_s = rect_sum((int)r, (int)d - (g + t), (int)r, (int)d + (g + t));
        const long   row_outer_c = rect_cnt((int)r, (int)d - (g + t), (int)r, (int)d + (g + t));
        const double row_guard_s = rect_sum((int)r, (int)d - g, (int)r, (int)d + g);
        const long   row_guard_c = rect_cnt((int)r, (int)d - g, (int)r, (int)d + g);
        const long   row_train_c = row_outer_c - row_guard_c;
        if (row_train_c > 0) {
          row_noise = (row_outer_s - row_guard_s) / (double)row_train_c;
          if (row_noise > 0.0) {
            const double row_alpha = (double)row_train_c * (std::pow(pfa, -1.0 / (double)row_train_c) - 1.0);
            thr_row = row_alpha * row_noise;
          }
        }
      }

      if (cell > thr && (!args.cfar_per_column || cell > thr_col) && (!args.cfar_per_row || cell > thr_row)) {
        sensing_detection_t det;
        det.range_bin   = r;
        det.doppler_bin = d;

        // Sub-bin peak interpolation (opt-in, args.subbin_interp): a target almost never sits exactly
        // on a bin centre, so reporting the bin index quantises range to range_res_m (3.05 m here) and
        // velocity to vel_res_mps, injecting a uniform +/-half-bin error into every measurement the
        // tracker and the multilateration then have to absorb. A 3-point parabolic fit through the
        // peak and its two neighbours recovers the fractional offset. Fitted in dB (log power) rather
        // than linear power: the mainlobe of a windowed peak is far closer to a parabola in log
        // domain, which is also why isac_sync.cc's LOS estimator needed its empirical
        // HANN_ESTIMATOR_SCALE correction for the linear-power version -- fitting in dB avoids that
        // whole bias class. Skipped at the array edges and whenever a neighbour is notched to zero
        // (log of 0), and the offset is clamped to +/-0.5 bin so a malformed fit can never move a
        // detection into a different bin.
        float dr_bin = 0.0f, dd_bin = 0.0f;
        if (args.subbin_interp) {
          if (r > 0 && r + 1 < R) {
            const double ym = (double)rvm.power[(size_t)(r - 1) * D + d];
            const double yp = (double)rvm.power[(size_t)(r + 1) * D + d];
            if (ym > 0.0 && yp > 0.0) {
              const double a = 10.0 * std::log10(ym);
              const double b = 10.0 * std::log10(cell);
              const double c = 10.0 * std::log10(yp);
              const double den = a - 2.0 * b + c;
              if (den < 0.0) { // concave => a genuine local maximum
                dr_bin = (float)std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den));
              }
            }
          }
          if (d > 0 && d + 1 < D) {
            const double ym = (double)rvm.power[(size_t)r * D + (d - 1)];
            const double yp = (double)rvm.power[(size_t)r * D + (d + 1)];
            if (ym > 0.0 && yp > 0.0) {
              const double a = 10.0 * std::log10(ym);
              const double b = 10.0 * std::log10(cell);
              const double c = 10.0 * std::log10(yp);
              const double den = a - 2.0 * b + c;
              if (den < 0.0) {
                dd_bin = (float)std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den));
              }
            }
          }
        }

        det.range_m     = ((float)r + dr_bin) * rvm.range_res_m;
        // NEGATED (2026-07-23): report BISTATIC RANGE-RATE, sign-consistent with the range axis
        // (positive = differential range increasing = target opening). A scatterer's slow-time term
        // is exp(-j2*pi*R(t)/lambda), so an OPENING target (Rdot>0) sits at NEGATIVE Doppler
        // frequency and thus a negative shifted bin -- reporting the raw bin offset therefore gave
        // -Rdot. Caught when the Kalman track seeded its rate from this field and converged to the
        // wrong sign against a ground truth of +6.4 m/s.
        det.vel_mps     = -(((float)d + dd_bin) - half_d) * rvm.vel_res_mps;
        // Report SNR against the STRONGER noise reference in use (per-column noise is the meaningful
        // one when a cell had to clear the column test), so a pedestal-adjacent survivor isn't
        // over-credited by the low 2-D-box noise estimate.
        double snr_noise = noise;
        if (args.cfar_per_column && col_noise > snr_noise) {
          snr_noise = col_noise;
        }
        if (args.cfar_per_row && row_noise > snr_noise) {
          snr_noise = row_noise;
        }
        det.snr_db      = 10.0f * std::log10((float)(cell / snr_noise));
        detections.push_back(det);
        if (detections.size() >= 8192) {
          break; // safety cap on raw detections before suppression
        }
      }
    }
  }

  // Raw (pre-NMS, pre-filter) detection count -- the direct, correctly-attributed signal for the
  // cfar_fa_adapt_enable feedback below (measures CFAR's own output, not the downstream filters').
  const uint32_t raw_detection_count = (uint32_t)detections.size();

  // Non-max suppression: collapse each detection cluster to its strongest cell (greedy, strongest
  // first). Deliberately NOT truncated to max_detections here -- that cap is now applied ONCE, at the
  // very end, AFTER conj_image_reject below. Truncating by SNR before mirror-pairing could cut away a
  // real detection's ghost partner (the ghost typically has HIGHER SNR -- see conj_image_reject's own
  // comment), orphaning the ghost so it survives unpaired with nothing left to match against.
  // Live-confirmed 2026-07-24 (PHASE2_MOT_MULTIUE_HANDOVER.md): with 2 real targets + their 2 mirror
  // ghosts + residual noise all competing for one small max_detections budget, this happened often
  // enough to dominate multi_target_tracker's track churn (mirror ghosts at range ~R-1-r, velocity
  // matching the real targets', surviving as confirmed spurious tracks).
  // NMS widths. 0 => AUTO-DERIVE from the transform's own mainlobe, which is what the suppression
  // radius physically IS: one scatterer occupies a mainlobe, so anything inside it is the same
  // scatterer and anything outside is a different one. There is nothing to tune here -- the width
  // follows from the window, which we chose:
  //   rectangular ~2 bins peak-to-null, Hann ~4, Dolph-Chebyshev ~4-5 (it trades mainlobe width for
  //   sidelobe level; at 60 dB it is close to Hann).
  // Half-width = mainlobe/2, floored at 1. Doppler uses a Hann slow-time window throughout, hence 2.
  const int nms_r = (args.nms_range_bins > 0)
                        ? (int)args.nms_range_bins
                        : ((args.range_window == "chebyshev") ? 2 : 2);
  const int nms_d = (args.nms_doppler_bins > 0) ? (int)args.nms_doppler_bins : 2;
  if ((nms_r > 0 || nms_d > 0) && detections.size() > 1) {
    std::sort(detections.begin(), detections.end(),
              [](const sensing_detection_t& a, const sensing_detection_t& b) { return a.snr_db > b.snr_db; });
    std::vector<sensing_detection_t> kept;
    kept.reserve(detections.size());
    for (const sensing_detection_t& cand : detections) {
      bool suppressed = false;
      for (const sensing_detection_t& k : kept) {
        if (std::abs((int)cand.range_bin - (int)k.range_bin) <= nms_r &&
            std::abs((int)cand.doppler_bin - (int)k.doppler_bin) <= nms_d) {
          suppressed = true;
          break;
        }
      }
      if (!suppressed) {
        kept.push_back(cand);
      }
    }
    detections.swap(kept);
  }

  // L1/FISTA sparse Doppler verification (opt-in, args.doppler_sparse). For each UNIQUE range bin
  // among the surviving detections, re-solve that bin's slow-time CIR row via L1-regularized
  // recovery over the SAME irregular sample times used by doppler_nudft (sparse_doppler.h): a genuine
  // scatterer is explained by one sparse tone; a dense-transform harmonic/leakage artifact needed to
  // borrow energy from a neighbouring bin and is revealed as spurious once solved sparsely (drops to
  // ~0). Scoped to only the CFAR-flagged bins (not the whole grid) to keep cost tractable -- see
  // sparse_doppler.h / defs_nr_UE_ISAC.h.
  if (args.doppler_sparse && row_time_slots != nullptr && period_slots > 0.0f && !detections.empty()) {
    // Seed the closed-loop lambda_scale from config on first use; thereafter it's adjusted below from
    // the MEASURED number of surviving detections each CPI, not held at a fixed constant.
    if (sparse_lambda_scale_state_ < 0.0f) {
      sparse_lambda_scale_state_ = args.doppler_sparse_lambda_scale;
    }
    const float lambda_scale = sparse_lambda_scale_state_;

    sparse_doppler_ctx sd_ctx;
    sparse_doppler_prepare(row_time_slots, nof_slow, period_slots, D, sd_ctx);

    std::vector<uint32_t> bins;
    bins.reserve(detections.size());
    for (const sensing_detection_t& det : detections) {
      bins.push_back(det.range_bin);
    }
    std::sort(bins.begin(), bins.end());
    bins.erase(std::unique(bins.begin(), bins.end()), bins.end());

    std::vector<sensing_detection_t> kept4;
    kept4.reserve(detections.size());
    std::vector<icf_t>  x_out;
    std::vector<float>  prof(D);
    // Diagnostics (Fix-2 verification plan): count how many detections the orphaned-harmonic probe
    // drops and the WORST (largest) sub-harmonic/peak ratio that triggered a drop -- a real orphaned
    // harmonic should reject on a ratio near/above harmonic_ratio (~0.25); frequent rejections on
    // tiny ratios would mean the probe is killing real targets via their own sidelobes.
    uint32_t n_probe_reject = 0;
    float    worst_reject_ratio = 0.0f;
    for (uint32_t r : bins) {
      // Noise floor for THIS range bin from the already-computed dense Doppler profile: most bins at
      // a fixed range are noise, so the median is a robust floor even though the row overall is
      // dominated by whatever scatterer(s) sit at their own bins (numerically validated: within-row
      // TIME-DOMAIN median is the wrong estimator here -- it reflects signal amplitude, not noise,
      // since every sample carries signal -- but the dense FREQUENCY-domain profile's median works
      // because most Doppler bins there truly are just noise).
      for (uint32_t d = 0; d < D; d++) {
        prof[d] = rvm.power[(size_t)r * D + d];
      }
      std::nth_element(prof.begin(), prof.begin() + D / 2, prof.end());
      const float noise_floor = prof[D / 2];
      // |Z|^2 for complex Gaussian Z~CN(0,sigma^2) is exponential with mean sigma^2; its median is
      // sigma^2*ln(2), so sigma = sqrt(median_power/ln2). Then the universal soft-threshold
      // sigma*sqrt(2*log(N)) (standard sparse-recovery choice for iid Gaussian noise).
      const float sigma  = std::sqrt(std::max(noise_floor, 1e-20f) / (float)M_LN2);
      const float lambda = sigma * std::sqrt(2.0f * std::log((float)D)) * lambda_scale;

      sparse_doppler_solve(sd_ctx, &cir_rm[(size_t)r * nof_slow], args.doppler_sparse_iters, lambda, x_out);
      float maxp = 0.0f;
      for (uint32_t n = 0; n < D; n++) {
        maxp = std::max(maxp, std::norm(x_out[n]));
      }
      for (const sensing_detection_t& det : detections) {
        if (det.range_bin != r) {
          continue;
        }
        // Undo the fftshift used for rvm.power's storage order (ds=(n+half)%D is self-inverse for
        // even D) to get back to the dictionary's natural bin index n.
        const uint32_t n = (det.doppler_bin + D / 2) % D;
        if (std::norm(x_out[n]) < args.doppler_sparse_peak_ratio * maxp) {
          continue; // already rejected by the base sparse-peak check
        }
        // Orphaned-harmonic check: a detection can survive the check above and STILL be a Doppler
        // harmonic of a real target whose FUNDAMENTAL never independently cleared CFAR that CPI (e.g.
        // its true bistatic rate briefly fell in zero_doppler_guard's notch, or its SNR dipped) --
        // track_harmonic_reject and the detection-level harmonic_reject above both require the
        // fundamental to ALSO be a detection/track that CPI to compare against, so an orphaned
        // harmonic with no co-detected fundamental slips through both. Since the full sparse spectrum
        // x_out is already computed for this range bin, directly PROBE the candidate sub-harmonic
        // bins (half/third this frequency) for meaningful energy, independent of whether anything was
        // separately detected there. Signed-frequency arithmetic: natural bin n represents frequency
        // n/D for n<=D/2 and (n-D)/D (negative) beyond it; dividing that SIGNED frequency by k and
        // mapping back to a natural bin is what "half/third the frequency" means here.
        const int  n_signed = ((int)n <= (int)D / 2) ? (int)n : (int)n - (int)D;
        bool       is_harmonic = false;
        // STRICTER, SEPARATE threshold for the reject-me-as-a-harmonic decision (harmonic_ratio,
        // ~0.25/-6 dB) than the lenient peak-survival check above (peak_ratio, ~0.05/-13 dB). A real
        // orphaned harmonic's fundamental sits within a few dB of it; a mere window sidelobe / SFO
        // jitter leak is 15-20 dB down, so the higher bar stops a strong target from flagging itself
        // as a harmonic of its own leakage (the "sidelobe self-destruction" that collapsed detections
        // to zero -- see defs_nr_UE_ISAC.h's doppler_sparse_harmonic_ratio comment).
        const float harm_thr = args.doppler_sparse_harmonic_ratio * maxp;
        for (int k = 2; k <= 3; k++) {
          if (std::abs(n_signed) < k) {
            continue; // too close to DC to meaningfully sub-divide
          }
          const int      sub_signed = (int)std::lround((double)n_signed / (double)k);
          const uint32_t sub_n = (sub_signed >= 0) ? (uint32_t)sub_signed : (uint32_t)(sub_signed + (int)D);
          if (sub_n == n) {
            continue;
          }
          if (std::norm(x_out[sub_n]) >= harm_thr) {
            is_harmonic = true;
            worst_reject_ratio = std::max(worst_reject_ratio,
                                          (maxp > 0.0f) ? (float)(std::norm(x_out[sub_n]) / maxp) : 0.0f);
            break;
          }
        }
        if (!is_harmonic) {
          kept4.push_back(det);
        } else {
          n_probe_reject++;
        }
      }
    }
    detections.swap(kept4);

    // Closed-loop update for the NEXT CPI: too few survivors (real targets likely missed) relaxes
    // the threshold; too many (ghosts likely leaking back through) tightens it. Clamped so it can't
    // run away in either direction. This is what makes lambda_scale adapt instead of sitting at a
    // fixed guess -- see defs_nr_UE_ISAC.h's doppler_sparse_lambda_scale comment.
    if (detections.size() < args.doppler_sparse_target_min_det) {
      sparse_lambda_scale_state_ *= args.doppler_sparse_adapt_rate; // relax (smaller threshold)
    } else if (detections.size() > args.doppler_sparse_target_max_det) {
      sparse_lambda_scale_state_ /= args.doppler_sparse_adapt_rate; // tighten (larger threshold)
    }
    sparse_lambda_scale_state_ = std::min(args.doppler_sparse_lambda_max,
                                          std::max(args.doppler_sparse_lambda_min, sparse_lambda_scale_state_));

    // Diagnostic dump (verification plan): per-CPI sparse-verify summary -- surviving detection count,
    // how many the orphaned-harmonic probe dropped and the worst ratio that triggered a drop (should
    // be >= harmonic_ratio; frequent drops at tiny ratios => probe killing real targets), the
    // adapted lambda_scale carried to the next CPI, and how many survivors sit at far range (>500 m)
    // together with the far survivors' velocities vs. the nearest-range survivor's (to check whether
    // far ghosts are k*v harmonics of a near target -- decides whether Fix 3 is needed).
    uint32_t n_far = 0;
    float    near_min_r = 1e30f, near_v = 0.0f;
    for (const sensing_detection_t& d : detections) {
      if (d.range_m < near_min_r) { near_min_r = d.range_m; near_v = d.vel_mps; }
    }
    for (const sensing_detection_t& d : detections) {
      if (d.range_m > 500.0f) { n_far++; }
    }
    LOG_I(PHY,
          "SENSING: sparse-verify surv=%zu probe_reject=%u worst_reject_ratio=%.3f lambda_scale=%.3f "
          "far>500m=%u near_r=%.1fm near_v=%.2f\n",
          detections.size(), n_probe_reject, worst_reject_ratio, sparse_lambda_scale_state_, n_far,
          (near_min_r < 1e29f) ? near_min_r : 0.0f, near_v);
  }

  // Conjugate-image ("mirror ghost") rejection. A range IFFT of a CFR carrying a real-valued
  // (conjugate-symmetric) component — the near-zero-Doppler residual a static/slow scatterer leaves
  // after clutter removal — produces a ghost at range bin (R-1-r) for a true scatterer at bin r. A
  // physical scatterer and its numerical mirror cannot both be real.
  //
  // Which member of the pair is the ghost is decided by RANGE, not SNR: a real bistatic target has a
  // SMALL differential range (dR >= 0, near bins), and its image lands in the far/upper half near
  // range_max. (SNR is the wrong discriminator: the image sits in the quiet far-range region and CFAR
  // gives it a *higher* SNR than the real target buried next to the LOS skirt — the opposite of what
  // "keep the stronger" would need.) So: drop a detection when another detection sits at its
  // range-mirror (R-1-r, within conj_image_guard) at a STRICTLY LOWER range bin — i.e. this one is the
  // upper/far member of the mirror pair. The lower (physical) member is always kept.
  if (args.conj_image_reject && detections.size() > 1) {
    const int guard = (int)args.conj_image_guard;
    std::vector<sensing_detection_t> kept2;
    kept2.reserve(detections.size());
    for (const sensing_detection_t& cand : detections) {
      const int mirror = (int)R - 1 - (int)cand.range_bin;
      bool is_image = false;
      for (const sensing_detection_t& other : detections) {
        // Another detection at this one's range-mirror, at a lower range bin => `cand` is the far image.
        if ((int)other.range_bin < (int)cand.range_bin &&
            std::abs((int)other.range_bin - mirror) <= guard) {
          is_image = true;
          break;
        }
      }
      if (!is_image) {
        kept2.push_back(cand);
      }
    }
    detections.swap(kept2);
  }

  // Doppler-harmonic rejection (opt-in). The Doppler analog of conj_image_reject above. Irregular,
  // scheduling-driven slow-time sampling of a moving target turns its slow-time tone into a
  // harmonic-rich signal, so besides the true peak at its bistatic range-rate v it also produces
  // strong peaks at k*v (k=2,3,...) AT THE SAME RANGE (RVM-confirmed 2026-07-24 once the range-wide
  // pedestal was removed -- see PHASE2_MOT_MULTIUE_HANDOVER.md). A physical second target at the very
  // same range and an exact integer-multiple velocity of another is astronomically unlikely, so a
  // detection is dropped when a STRONGER detection sits at the same range bin (within harmonic_guard)
  // whose velocity magnitude divides this one's by a near-integer k in [2, harmonic_max_k]. Keyed on
  // magnitude so both +k*v and -k*v images (the gate produces both) are caught; the stronger member
  // (the fundamental, i.e. the real target) is always kept.
  if (args.harmonic_reject && detections.size() > 1) {
    const int    guard = (int)args.harmonic_guard;
    const int    maxk  = (int)args.harmonic_max_k;
    // Fractional tolerance on the integer ratio. 0 => AUTO. The right tolerance is not a taste
    // parameter: it is set by how well THIS CPI can measure a range-rate. A ratio v_c/v_o inherits
    // the relative error of both terms, so with a per-detection rate uncertainty of ~sigma_v the
    // ratio's own 1-sigma is |k| * sigma_v/|v_o| * sqrt(1 + 1/k^2) ~ sigma_v/|v_o| for the k=2 case
    // that dominates. sigma_v is available per detection (rate_std_mps, SNR-derived) -- so the
    // tolerance is computed per candidate pair below rather than fixed here.
    // MEASURED justification for doing this at all: the Doppler estimate is unbiased but scatters
    // 3.8 bins, and 85 % of detections land within one bin -- a single fixed fraction cannot be
    // right simultaneously for a slow target (where one bin is a large fraction of v) and a fast one
    // (where it is a small one).
    const double tol   = (double)args.harmonic_tol; // 0 => per-pair adaptive, see below
    std::vector<sensing_detection_t> kept3;
    kept3.reserve(detections.size());
    for (const sensing_detection_t& cand : detections) {
      const double vc = std::abs((double)cand.vel_mps);
      bool is_harm = false;
      for (const sensing_detection_t& other : detections) {
        if (&other == &cand) {
          continue;
        }
        const double vo = std::abs((double)other.vel_mps);
        // `other` is the fundamental: same range, SMALLER |velocity| (the harmonic is always the
        // higher-order term). It need NOT be stronger -- irregular-sampling harmonics routinely rival
        // or exceed the fundamental in power (RVM-measured within ~1 dB, sometimes above) -- but it
        // must not be much WEAKER, so a random weak low-velocity blip can't knock out a strong real
        // detection: require it within harmonic_snr_margin dB of the candidate.
        if (vo < 1e-3 || vc <= vo || other.snr_db < cand.snr_db - (double)args.harmonic_snr_margin) {
          continue;
        }
        if (std::abs((int)cand.range_bin - (int)other.range_bin) > guard) {
          continue;
        }
        const double ratio = vc / vo;
        const double krnd  = std::round(ratio);
        // Per-pair tolerance when tol == 0: propagate each detection's OWN declared rate sigma into
        // the ratio. Falls back to a 1-bin-equivalent spread if a receiver declares no sigma.
        double tol_eff = tol;
        if (tol_eff <= 0.0) {
          // Each detection's own rate uncertainty from its own SNR -- the same estimator the report
          // emits (detection_report.h), so the gate and the wire cannot disagree about what sigma is.
          const double sc = nr_isac_detection_sigma((double)rvm.vel_res_mps, (double)cand.snr_db,
                                                    args.subbin_interp);
          const double so = nr_isac_detection_sigma((double)rvm.vel_res_mps, (double)other.snr_db,
                                                    args.subbin_interp);
          // d(ratio) = sqrt( (sc/vo)^2 + (vc*so/vo^2)^2 ), then a 3-sigma acceptance.
          const double dr = std::sqrt((sc / vo) * (sc / vo) + (vc * so / (vo * vo)) * (vc * so / (vo * vo)));
          tol_eff         = 3.0 * dr;
        }
        if (krnd >= 2.0 && krnd <= (double)maxk && std::abs(ratio - krnd) <= tol_eff) {
          is_harm = true;
          break;
        }
      }
      if (!is_harm) {
        kept3.push_back(cand);
      }
    }
    detections.swap(kept3);
  }

  // Measured-gating-offset rejection (opt-in, args.gating_reject). Same shape as harmonic_reject
  // above, but the offset list comes from THIS CPI's measured row-energy spectrum (see the estimator
  // in process()) instead of assuming integer velocity ratios: a detection is dropped when a
  // sufficiently strong detection sits at the same range and its Doppler bin differs by one of the
  // measured gating offsets. Strictly more general than the integer-ratio test -- it also catches the
  // asymmetric, non-integer replicas an irregular scheduler produces -- and it needs the SNR-margin
  // guard for the same reason: with several real targets crowded into a narrow Doppler span, one real
  // target can legitimately sit a gating offset away from another, so only a clearly stronger
  // neighbour may veto a detection.
  if (args.gating_reject && !gating_offsets_.empty() && detections.size() > 1) {
    const int guard = (int)args.harmonic_guard;
    const int tolb  = (int)args.gating_tol_bins;
    std::vector<sensing_detection_t> keptg;
    keptg.reserve(detections.size());
    for (const sensing_detection_t& cand : detections) {
      bool is_replica = false;
      for (const sensing_detection_t& other : detections) {
        if (&other == &cand) continue;
        // Only a clearly stronger, same-range neighbour may veto.
        if (other.snr_db < cand.snr_db + (double)args.gating_snr_margin) continue;
        if (std::abs((int)cand.range_bin - (int)other.range_bin) > guard) continue;
        const int dd = std::abs((int)cand.doppler_bin - (int)other.doppler_bin);
        for (uint32_t off : gating_offsets_) {
          if (std::abs(dd - (int)off) <= tolb) { is_replica = true; break; }
        }
        if (is_replica) break;
      }
      if (!is_replica) keptg.push_back(cand);
    }
    detections.swap(keptg);
  }

  // Far-range harmonic (range-smeared pedestal) rejection (opt-in). Unlike harmonic_reject above,
  // which needs the fundamental to be a SAME-RANGE detection, this finds the scene's dominant
  // NEAR-range Doppler component(s) directly from the range-integrated power profile -- so it catches
  // an amplitude-gating harmonic smeared out to far range even when its near parent isn't separately
  // detected that CPI (the "orphaned far ghost" the diagnostics showed at k*v_target, r>500 m). See
  // defs_nr_UE_ISAC.h's far_harmonic_reject comment.
  if (args.far_harmonic_reject && !detections.empty() && rvm.range_res_m > 0.0f) {
    // Near/far split. <= 0 => AUTO: this CPI's own ENERGY-WEIGHTED MEDIAN range bin, i.e. the split
    // between "where this scene's returns actually are" and "beyond them", measured rather than
    // surveyed. A pair of absolute metre values has to be re-derived for every deployment, and after
    // the auto far-range notch the usable axis is itself scene-dependent, so a fixed 250/500 m can
    // easily land entirely inside or entirely outside the populated band.
    int auto_split = -1;
    if (args.far_harmonic_near_m <= 0.0f || args.far_harmonic_far_m <= 0.0f) {
      double tot = 0.0;
      std::vector<double> cum(R, 0.0);
      for (uint32_t r = 0; r < R; r++) {
        const float* row = &rvm.power[(size_t)r * D];
        double       acc = 0.0;
        for (uint32_t d = 0; d < D; d++) {
          acc += (double)row[d];
        }
        tot += acc;
        cum[r] = tot;
      }
      if (tot > 0.0) {
        for (uint32_t r = 0; r < R; r++) {
          if (cum[r] >= 0.5 * tot) {
            auto_split = (int)r;
            break;
          }
        }
      }
    }
    const int near_bin = (args.far_harmonic_near_m > 0.0f)
                             ? std::min((int)R - 1, (int)(args.far_harmonic_near_m / rvm.range_res_m))
                             : ((auto_split >= 0) ? auto_split : (int)R - 1);
    const int zrg_f    = (int)args.zero_range_guard;
    // Range-integrated Doppler profile over the NEAR band only (skip the zero-range/LOS guard), so a
    // real near target's tone stands out as a strong column.
    std::vector<double> pnear(D, 0.0);
    for (int r = zrg_f + 1; r <= near_bin; r++) {
      const float* row = &rvm.power[(size_t)r * D];
      for (uint32_t d = 0; d < D; d++) {
        pnear[d] += (double)row[d];
      }
    }
    // Dominant near velocities = columns whose integrated power exceeds a robust floor (median x 8)
    // AND are a local max. Stored as SIGNED doppler offset from centre (= velocity sign/magnitude).
    std::vector<double> sorted_pn = pnear;
    std::nth_element(sorted_pn.begin(), sorted_pn.begin() + D / 2, sorted_pn.end());
    const double floor_pn = sorted_pn[D / 2] * 8.0;
    const int    half     = (int)D / 2;
    std::vector<int> near_off;
    for (uint32_t d = 1; d + 1 < D; d++) {
      if (pnear[d] > floor_pn && pnear[d] >= pnear[d - 1] && pnear[d] >= pnear[d + 1]) {
        const int off = (int)d - half; // shifted-bin index -> signed offset (skip DC/zero-Doppler)
        if (std::abs(off) >= (int)args.zero_doppler_guard + 1) {
          near_off.push_back(off);
        }
      }
    }
    if (!near_off.empty()) {
      const int    far_bin = (args.far_harmonic_far_m > 0.0f)
                                 ? (int)(args.far_harmonic_far_m / rvm.range_res_m)
                                 : ((auto_split >= 0) ? auto_split : (int)R);
      const int    maxk    = (int)args.harmonic_max_k;
      const double tol     = (double)args.harmonic_tol;
      std::vector<sensing_detection_t> kept5;
      kept5.reserve(detections.size());
      for (const sensing_detection_t& cand : detections) {
        bool drop = false;
        if ((int)cand.range_bin > far_bin) {
          const int voff = (int)cand.doppler_bin - half; // signed offset of the far candidate
          for (int no : near_off) {
            if (std::abs(no) < 1) {
              continue;
            }
            const double ratio = (double)std::abs(voff) / (double)std::abs(no);
            const double krnd  = std::round(ratio);
            if (krnd >= 2.0 && krnd <= (double)maxk && std::abs(ratio - krnd) <= tol) {
              drop = true; // far detection is a k*v harmonic of a dominant near-range component
              break;
            }
          }
        }
        if (!drop) {
          kept5.push_back(cand);
        }
      }
      detections.swap(kept5);
    }
  }

  // Final cap to max_detections, by SNR -- moved here (after NMS dedup + mirror-pairing have had the
  // FULL candidate set to work with) instead of happening mid-pipeline, so a real detection can't be
  // truncated away before conj_image_reject gets a chance to pair it with its ghost.
  if (detections.size() > args.max_detections) {
    std::sort(detections.begin(), detections.end(),
              [](const sensing_detection_t& a, const sensing_detection_t& b) { return a.snr_db > b.snr_db; });
    detections.resize(args.max_detections);
  }

  // Closed-loop update for the NEXT CPI: too few RAW detections (real targets likely starved by too
  // strict a budget) raises cfar_target_fa_per_cpi; too many (flooding the downstream filters again)
  // lowers it. Deliberately keyed on the RAW pre-filter count captured above, not the post-filter
  // count returned to the caller -- this measures CFAR's own behaviour, not the downstream filters'.
  // See defs_nr_UE_ISAC.h's cfar_fa_adapt_enable comment.
  if (args.cfar_fa_adapt_enable) {
    if (raw_detection_count < args.cfar_fa_target_min_det) {
      cfar_fa_state_ *= args.cfar_fa_adapt_rate; // raise the budget
    } else if (raw_detection_count > args.cfar_fa_target_max_det) {
      cfar_fa_state_ /= args.cfar_fa_adapt_rate; // lower the budget
    }
    cfar_fa_state_ = std::min(args.cfar_fa_max, std::max(args.cfar_fa_min, cfar_fa_state_));
    LOG_I(PHY, "SENSING: cfar-fa-adapt raw_det=%u target_fa=%.3f\n", raw_detection_count, cfar_fa_state_);
  }
}

} // namespace nr_isac
