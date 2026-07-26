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

/*! \file openair1/PHY/NR_UE_ISAC/clean_deconv.cc
 * \brief Coherent CLEAN deconvolution with a separable shift-invariant PSF (see header).
 *
 * WHY SEPARABLE / SHIFT-INVARIANT: the caller (range_doppler.cc) enables CLEAN only for detector="fft"
 * on a uniform full-band comb. In that regime the range operator (windowed IFFT over all subcarriers)
 * and the Doppler operator (windowed FFT/NUDFT over all slow-time rows) are each linear and
 * translation-invariant, so a scatterer at (r0,d0) produces the outer product kr[.-r0]*kd[.-d0] --
 * one range kernel and one Doppler kernel, precomputed once per CPI, no per-component transform. Under
 * matched_filter (per-row occupancy masking) or a fused multi-comb grid the operator is NO LONGER
 * shift-invariant (each row masks different subcarriers), the PSF becomes a full 2-D forward model per
 * component, and this fast path is inapplicable -- range_doppler.cc gates CLEAN off in those modes.
 */

#include "clean_deconv.h"

#include <algorithm>
#include <cmath>

namespace nr_isac {

void clean_deconv_run(const icf_t*               cmap,
                      uint32_t                   R,
                      uint32_t                   D,
                      const icf_t*               kr,
                      const icf_t*               kd,
                      const clean_deconv_params& p,
                      float*                     out_pow,
                      uint32_t*                  n_components)
{
  const size_t N = (size_t)R * D;
  if (n_components != nullptr) {
    *n_components = 0;
  }
  if (cmap == nullptr || out_pow == nullptr || R == 0 || D == 0) {
    return;
  }
  if (kr == nullptr || kd == nullptr) {
    // No PSF: nothing to deconvolve, just pass the power through.
    for (size_t i = 0; i < N; i++) {
      out_pow[i] = std::norm(cmap[i]);
    }
    return;
  }

  // Residual starts as the full complex map; components are subtracted from it in place.
  std::vector<icf_t> res(cmap, cmap + N);

  // Banked clean components (complex amplitude at their cell). Kept sparse: a handful per CPI.
  std::vector<uint32_t> comp_r;
  std::vector<uint32_t> comp_d;
  std::vector<icf_t>    comp_a;
  comp_r.reserve(p.max_components);
  comp_d.reserve(p.max_components);
  comp_a.reserve(p.max_components);

  // Initial peak power sets the deconvolution stop floor.
  float peak0 = 0.0f;
  for (size_t i = 0; i < N; i++) {
    const float pw = std::norm(res[i]);
    if (pw > peak0) {
      peak0 = pw;
    }
  }
  if (peak0 <= 0.0f) {
    for (size_t i = 0; i < N; i++) {
      out_pow[i] = 0.0f;
    }
    return;
  }
  const float stop_pw = peak0 * std::pow(10.0f, -std::abs(p.stop_db) / 10.0f);
  const float gamma   = (p.loop_gain > 0.0f && p.loop_gain <= 1.0f) ? p.loop_gain : 0.8f;

  for (uint32_t it = 0; it < p.max_components; it++) {
    // 1. Locate the residual's brightest cell.
    float    best = 0.0f;
    uint32_t br = 0, bd = 0;
    for (uint32_t r = 0; r < R; r++) {
      const icf_t* row = &res[(size_t)r * D];
      for (uint32_t d = 0; d < D; d++) {
        const float pw = std::norm(row[d]);
        if (pw > best) {
          best = pw;
          br   = r;
          bd   = d;
        }
      }
    }
    if (best < stop_pw || best <= 0.0f) {
      break;
    }

    // 2. Coherently subtract loop_gain * amplitude * PSF (kr outer kd) from the whole residual. Since
    //    kr[0]=kd[0]=(1,0), the map value at (br,bd) already IS the scatterer amplitude at that cell.
    const icf_t c = res[(size_t)br * D + bd] * gamma;
    for (uint32_t r = 0; r < R; r++) {
      const uint32_t lr  = (r + R - br) % R; // range lag
      const icf_t    ckr = c * kr[lr];
      icf_t*         row = &res[(size_t)r * D];
      for (uint32_t d = 0; d < D; d++) {
        const uint32_t ld = (d + D - bd) % D; // Doppler lag
        row[d] -= ckr * kd[ld];
      }
    }

    // 3. Bank the component (accumulate onto an existing bank cell if we re-picked it).
    bool merged = false;
    for (size_t k = 0; k < comp_r.size(); k++) {
      if (comp_r[k] == br && comp_d[k] == bd) {
        comp_a[k] += c;
        merged = true;
        break;
      }
    }
    if (!merged) {
      comp_r.push_back(br);
      comp_d.push_back(bd);
      comp_a.push_back(c);
    }
  }

  if (n_components != nullptr) {
    *n_components = (uint32_t)comp_r.size();
  }

  // Restore: cleaned image = residual + each banked component convolved with a narrow real clean beam.
  // Work in the complex domain (coherent), then take |.|^2 for CFAR. The clean beam is a small
  // separable Gaussian of half-width restore_bins (0 => single-bin delta); it gives CFAR a compact,
  // artifact-free mainlobe to detect instead of the deconvolution's bare spike.
  std::vector<icf_t> cleaned(res); // start from the residual
  const int hw = (int)p.restore_bins;
  // Precompute 1-D Gaussian beam weights over [-hw, hw]; sigma chosen so the edge is ~ -8 dB.
  std::vector<float> bw(2 * hw + 1, 1.0f);
  if (hw > 0) {
    const float sigma = std::max(0.5f, (float)hw / 1.5f);
    for (int t = -hw; t <= hw; t++) {
      bw[t + hw] = std::exp(-0.5f * (float)(t * t) / (sigma * sigma));
    }
  }
  for (size_t k = 0; k < comp_r.size(); k++) {
    const int r0 = (int)comp_r[k];
    const int d0 = (int)comp_d[k];
    const icf_t a = comp_a[k];
    for (int dr = -hw; dr <= hw; dr++) {
      const int rr = ((r0 + dr) % (int)R + (int)R) % (int)R;
      const float wr = bw[dr + hw];
      for (int dd = -hw; dd <= hw; dd++) {
        const int dcol = ((d0 + dd) % (int)D + (int)D) % (int)D;
        cleaned[(size_t)rr * D + dcol] += a * (wr * bw[dd + hw]);
      }
    }
  }

  for (size_t i = 0; i < N; i++) {
    out_pow[i] = std::norm(cleaned[i]);
  }
}

} // namespace nr_isac
