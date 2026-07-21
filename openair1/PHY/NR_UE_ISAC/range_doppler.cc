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
    const double range_max = SPEED_OF_LIGHT / (2.0 * df_comb);
    const double tau       = 2.0 * (0.25 * range_max) / SPEED_OF_LIGHT;
    double       fd        = 0.0;
    if (fc > 0.0 && t_slow > 0.0) {
      const double vel_max = SPEED_OF_LIGHT / (4.0 * fc * t_slow);
      fd                   = 2.0 * (0.25 * vel_max) * fc / SPEED_OF_LIGHT;
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

void range_doppler::process(const icf_t*                       h_cpi,
                            uint32_t                          nof_slow,
                            uint32_t                          nof_subc,
                            uint32_t                          comb_spacing,
                            const nr_isac_carrier_t&          carrier,
                            float                             period_slots,
                            const uint32_t*                   row_comb,
                            sensing_rvm_t&                    rvm,
                            std::vector<sensing_detection_t>& detections)
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
  rvm.range_res_m      = (float)(SPEED_OF_LIGHT / (2.0 * (double)nof_range * df_comb));
  rvm.range_max_m      = (float)(SPEED_OF_LIGHT / (2.0 * df_comb));
  rvm.vel_res_mps      = (fc > 0.0) ? (float)(SPEED_OF_LIGHT / (2.0 * fc * (double)nof_dopp * t_slow)) : 0.0f;
  rvm.vel_max_mps      = (fc > 0.0) ? (float)(SPEED_OF_LIGHT / (4.0 * fc * t_slow)) : 0.0f;

  // Working copy of the CPI (so the caller's matrix and self-test injection stay isolated)
  work.assign(h_cpi, h_cpi + (size_t)nof_slow * nof_subc);

  if (args.selftest || !targets_.empty()) {
    inject_selftest(work.data(), nof_slow, nof_subc, df_comb, t_slow, fc);
  }

  // Clutter removal: subtract the per-subcarrier slow-time mean (suppresses the static/zero-Doppler LOS)
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

  // Frequency (range) Hann window to suppress range sidelobes
  if (freq_hann.size() != nof_subc) {
    freq_hann.resize(nof_subc);
    for (uint32_t c = 0; c < nof_subc; c++) {
      freq_hann[c] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)c / (float)(nof_subc - 1)));
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
  static constexpr uint32_t DEALIAS_TAPER = 4; // raised-cosine rolloff width (bins) before the cut
  cir_rm.assign((size_t)nof_range * nof_slow, icf_t(0.0f, 0.0f));
  range_in.resize(nof_subc);
  range_out.resize(nof_range);
  for (uint32_t n = 0; n < nof_slow; n++) {
    for (uint32_t c = 0; c < nof_subc; c++) {
      range_in[c] = work[(size_t)n * nof_subc + c] * freq_hann[c];
    }
    range_plan->run(range_in.data(), range_out.data());

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

  // Slow-time Hann window
  if (hann.size() != nof_slow) {
    hann.resize(nof_slow);
    for (uint32_t n = 0; n < nof_slow; n++) {
      hann[n] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(nof_slow - 1)));
    }
  }

  // Doppler: FFT along slow-time for each range bin, with fftshift so zero-Doppler is centred
  rvm.power.assign((size_t)nof_range * nof_dopp, 0.0f);
  dopp_in.resize(nof_slow);
  dopp_out.resize(nof_slow);
  const uint32_t half = nof_dopp / 2;
  for (uint32_t r = 0; r < nof_range; r++) {
    for (uint32_t n = 0; n < nof_slow; n++) {
      dopp_in[n] = cir_rm[(size_t)r * nof_slow + n] * hann[n];
    }
    dopp_plan->run(dopp_in.data(), dopp_out.data());
    for (uint32_t d = 0; d < nof_dopp; d++) {
      const uint32_t ds = (d + half) % nof_dopp; // fftshift
      const float    re = dopp_out[d].real();
      const float    im = dopp_out[d].imag();
      rvm.power[(size_t)r * nof_dopp + ds] = re * re + im * im;
    }
  }

  // Notch static clutter: zero the zero-Doppler band (across all ranges) and the near-zero-range band
  // (direct-path / LOS). This removes the residual LOS and the zero-Doppler ghost that mean-subtraction
  // leaves at a moving target's range, and keeps the strong clutter out of the CFAR noise estimate.
  const int zdg = (int)args.zero_doppler_guard;
  const int zrg = (int)args.zero_range_guard;
  for (uint32_t r = 0; r < nof_range; r++) {
    const bool zero_range = (int)r <= zrg || (int)r >= (int)nof_range - 1 - zrg;
    for (uint32_t d = 0; d < nof_dopp; d++) {
      const bool zero_dopp = std::abs((int)d - (int)half) <= zdg;
      if (zero_range || zero_dopp) {
        rvm.power[(size_t)r * nof_dopp + d] = 0.0f;
      }
    }
  }

  // 2D CA-CFAR detection + non-max suppression
  cfar(rvm, detections);
}

void range_doppler::cfar(const sensing_rvm_t& rvm, std::vector<sensing_detection_t>& detections)
{
  const uint32_t R   = rvm.nof_range_bins;
  const uint32_t D   = rvm.nof_doppler_bins;
  const int      g   = (int)args.cfar_guard;
  const int      t   = (int)args.cfar_train;
  const double   pfa = (args.cfar_pfa > 0.0f) ? (double)args.cfar_pfa : 1e-3;

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
      if (cell > thr) {
        sensing_detection_t det;
        det.range_bin   = r;
        det.doppler_bin = d;
        det.range_m     = (float)r * rvm.range_res_m;
        det.vel_mps     = ((float)d - half_d) * rvm.vel_res_mps;
        det.snr_db      = 10.0f * std::log10((float)(cell / noise));
        detections.push_back(det);
        if (detections.size() >= 8192) {
          break; // safety cap on raw detections before suppression
        }
      }
    }
  }

  // Non-max suppression: collapse each detection cluster to its strongest cell (greedy, strongest first).
  const int nms_r = (int)args.nms_range_bins;
  const int nms_d = (int)args.nms_doppler_bins;
  if ((nms_r > 0 || nms_d > 0) && detections.size() > 1) {
    std::sort(detections.begin(), detections.end(),
              [](const sensing_detection_t& a, const sensing_detection_t& b) { return a.snr_db > b.snr_db; });
    std::vector<sensing_detection_t> kept;
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
        if (kept.size() >= args.max_detections) {
          break;
        }
      }
    }
    detections.swap(kept);
  } else if (detections.size() > args.max_detections) {
    std::sort(detections.begin(), detections.end(),
              [](const sensing_detection_t& a, const sensing_detection_t& b) { return a.snr_db > b.snr_db; });
    detections.resize(args.max_detections);
  }
}

} // namespace nr_isac
