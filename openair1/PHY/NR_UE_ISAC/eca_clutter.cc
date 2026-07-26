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

#include "eca_clutter.h"

#include <algorithm>
#include <cmath>
#include <complex>

extern "C" {
#include "common/utils/LOG/log.h"
}

namespace nr_isac {

namespace {

constexpr double SPEED_OF_LIGHT = 299792458.0;
using cd_t                      = std::complex<double>; // double precision for the projector algebra

/**
 * Dense complex matrix inverse by Gauss-Jordan elimination with partial pivoting.
 * @p a is n×n row-major, overwritten with its inverse. Returns false if singular.
 * Used only on the small Gram matrices (ΦᴴΦ / ΨᴴΨ), so O(n³) here is negligible.
 */
bool invert(std::vector<cd_t>& a, uint32_t n)
{
  std::vector<cd_t> inv((size_t)n * n, cd_t(0.0, 0.0));
  for (uint32_t i = 0; i < n; i++) {
    inv[(size_t)i * n + i] = cd_t(1.0, 0.0);
  }
  for (uint32_t col = 0; col < n; col++) {
    // Partial pivot: largest-magnitude entry in this column at/below the diagonal.
    uint32_t piv  = col;
    double   best = std::abs(a[(size_t)col * n + col]);
    for (uint32_t r = col + 1; r < n; r++) {
      const double m = std::abs(a[(size_t)r * n + col]);
      if (m > best) {
        best = m;
        piv  = r;
      }
    }
    if (best < 1e-12) {
      return false; // singular
    }
    if (piv != col) {
      for (uint32_t k = 0; k < n; k++) {
        std::swap(a[(size_t)col * n + k], a[(size_t)piv * n + k]);
        std::swap(inv[(size_t)col * n + k], inv[(size_t)piv * n + k]);
      }
    }
    const cd_t d = a[(size_t)col * n + col];
    for (uint32_t k = 0; k < n; k++) {
      a[(size_t)col * n + k] /= d;
      inv[(size_t)col * n + k] /= d;
    }
    for (uint32_t r = 0; r < n; r++) {
      if (r == col) {
        continue;
      }
      const cd_t f = a[(size_t)r * n + col];
      if (f == cd_t(0.0, 0.0)) {
        continue;
      }
      for (uint32_t k = 0; k < n; k++) {
        a[(size_t)r * n + k] -= f * a[(size_t)col * n + k];
        inv[(size_t)r * n + k] -= f * inv[(size_t)col * n + k];
      }
    }
  }
  a.swap(inv);
  return true;
}

/**
 * Build the dense Hermitian orthogonal projector P = Φ(ΦᴴΦ)⁻¹Φᴴ onto span{atoms}, where
 * @p atoms is [natoms][dim] (row a = atom φ_a of length @p dim). Output @p P is [dim][dim].
 * For the integer-bin atom sets used here ΦᴴΦ is diagonal, but the general (ΦᴴΦ)⁻¹ is formed
 * so the code is a faithful least-squares projection (and tolerant of non-orthogonal atoms).
 */
bool build_projector(const std::vector<cd_t>& atoms, uint32_t natoms, uint32_t dim, std::vector<icf_t>& P)
{
  if (natoms == 0) {
    return false;
  }
  // Gram G = ΦᴴΦ  (natoms×natoms):  G[a][b] = Σ_i conj(atom_a[i]) atom_b[i]
  std::vector<cd_t> G((size_t)natoms * natoms, cd_t(0.0, 0.0));
  for (uint32_t a = 0; a < natoms; a++) {
    for (uint32_t b = a; b < natoms; b++) {
      cd_t acc(0.0, 0.0);
      for (uint32_t i = 0; i < dim; i++) {
        acc += std::conj(atoms[(size_t)a * dim + i]) * atoms[(size_t)b * dim + i];
      }
      G[(size_t)a * natoms + b] = acc;
      G[(size_t)b * natoms + a] = std::conj(acc);
    }
  }
  if (!invert(G, natoms)) {
    return false;
  }
  // M1[a][j] = Σ_b Ginv[a][b] conj(atom_b[j])   (natoms×dim)
  std::vector<cd_t> M1((size_t)natoms * dim, cd_t(0.0, 0.0));
  for (uint32_t a = 0; a < natoms; a++) {
    for (uint32_t b = 0; b < natoms; b++) {
      const cd_t g = G[(size_t)a * natoms + b];
      if (g == cd_t(0.0, 0.0)) {
        continue;
      }
      for (uint32_t j = 0; j < dim; j++) {
        M1[(size_t)a * dim + j] += g * std::conj(atoms[(size_t)b * dim + j]);
      }
    }
  }
  // P[i][j] = Σ_a atom_a[i] · M1[a][j]
  P.assign((size_t)dim * dim, icf_t(0.0f, 0.0f));
  for (uint32_t a = 0; a < natoms; a++) {
    for (uint32_t i = 0; i < dim; i++) {
      const cd_t ai = atoms[(size_t)a * dim + i];
      if (ai == cd_t(0.0, 0.0)) {
        continue;
      }
      for (uint32_t j = 0; j < dim; j++) {
        const cd_t v = ai * M1[(size_t)a * dim + j];
        P[(size_t)i * dim + j] += icf_t((float)v.real(), (float)v.imag());
      }
    }
  }
  return true;
}

} // namespace

void eca_clutter::rebuild(uint32_t nof_slow, uint32_t nof_subc, double df_comb, double t_slow, double fc)
{
  c_slow_ = nof_slow;
  c_subc_ = nof_subc;
  c_df_   = df_comb;
  c_ts_   = t_slow;
  c_fc_   = fc;

  // --- Delay (frequency) atom set Φ: delay bins 0..P-1 covering [0, eca_delay_max_m]. ---
  // Differential BISTATIC range per delay bin — kept bit-identical to range_doppler.cc's
  // rvm.range_res_m so `eca_delay_max_m` means the same thing on both (2026-07-23: both dropped the
  // erroneous monostatic factor 2; a given eca_delay_max_m now spans HALF as many delay bins).
  const double range_res = (nof_subc > 0 && df_comb > 0.0) ? SPEED_OF_LIGHT / ((double)nof_subc * df_comb) : 0.0;
  uint32_t     P         = nof_subc; // sentinel/full => whole delay subspace => P_Φ = I
  if (args.eca_delay_max_m > 0.0f && range_res > 0.0) {
    // integer nonneg delay bins {0..P-1} tiling [0, delay_max]; +1 to include bin 0
    const long nb = (long)std::floor((double)args.eca_delay_max_m / range_res) + 1;
    P             = (uint32_t)std::min<long>(std::max<long>(1, nb), (long)nof_subc);
  }
  freq_identity_ = (P >= nof_subc);
  nP_            = freq_identity_ ? nof_subc : P;

  if (freq_identity_) {
    Pf_.clear();
  } else {
    // φ_p[m] = exp(-j2π m p / M),  p=0..P-1,  m=0..M-1  (one-sided nonneg-delay DFT atoms)
    std::vector<cd_t> atoms((size_t)P * nof_subc);
    for (uint32_t p = 0; p < P; p++) {
      for (uint32_t m = 0; m < nof_subc; m++) {
        const double ph                    = -2.0 * M_PI * (double)m * (double)p / (double)nof_subc;
        atoms[(size_t)p * nof_subc + m]    = cd_t(std::cos(ph), std::sin(ph));
      }
    }
    if (!build_projector(atoms, P, nof_subc, Pf_)) {
      LOG_W(PHY, "SENSING: ECA delay-projector build failed (P=%u M=%u); falling back to identity\n", P, nof_subc);
      Pf_.clear();
      freq_identity_ = true;
      nP_            = nof_subc;
    }
  }

  // --- Doppler (slow-time) atom set Ψ: bins -Qh..+Qh covering [-eca_doppler_max_mps, +...]. ---
  // vel_res = c/(fc·nof_slow·t_slow) — BISTATIC RANGE-RATE per bin, kept bit-identical to
  // range_doppler.cc's rvm.vel_res_mps so `eca_doppler_max_mps` means the same thing on both axes.
  // (2026-07-23: both dropped the monostatic round-trip factor 2 together — see the long comment on
  // rvm.vel_res_mps in range_doppler.cc for why it was wrong here. Consequence: a given
  // eca_doppler_max_mps now removes HALF as many Doppler bins as before, so existing tuned values —
  // e.g. tests/sensing_sim's 0.15 — must be doubled to preserve their previous bin coverage.)
  // fc≤0 (no carrier) => Qh=0 (DC-only removal).
  uint32_t Qh = 0;
  if (fc > 0.0 && t_slow > 0.0 && args.eca_doppler_max_mps > 0.0f) {
    const double vel_res = SPEED_OF_LIGHT / (fc * (double)nof_slow * t_slow);
    if (vel_res > 0.0) {
      Qh = (uint32_t)std::lround((double)args.eca_doppler_max_mps / vel_res);
    }
  }
  // Keep the removed band strictly narrower than the CPI so P_Ψ ≠ I (I would cancel everything).
  const uint32_t qh_max = (nof_slow >= 3) ? (nof_slow - 1) / 2 - 1 : 0;
  Qh                    = std::min(Qh, qh_max);
  const uint32_t Q      = 2 * Qh + 1;
  nQ_                   = Q;

  // ψ_q[n] = exp(+j2π n q / nof_slow),  q = -Qh..+Qh,  n=0..nof_slow-1  (near-zero-Doppler DFT atoms)
  std::vector<cd_t> datoms((size_t)Q * nof_slow);
  for (uint32_t idx = 0; idx < Q; idx++) {
    const int q = (int)idx - (int)Qh;
    for (uint32_t n = 0; n < nof_slow; n++) {
      const double ph                 = 2.0 * M_PI * (double)n * (double)q / (double)nof_slow;
      datoms[(size_t)idx * nof_slow + n] = cd_t(std::cos(ph), std::sin(ph));
    }
  }
  if (!build_projector(datoms, Q, nof_slow, Pt_)) {
    LOG_E(PHY, "SENSING: ECA Doppler-projector build failed (Q=%u N=%u)\n", Q, nof_slow);
    Pt_.clear();
  }

  LOG_I(PHY,
        "SENSING: ECA projectors rebuilt: delay atoms=%u/%u (%s), Doppler atoms=%u (Qh=%u), "
        "delay_max=%.1f m dopp_max=%.3f m/s\n",
        nP_, nof_subc, freq_identity_ ? "identity" : "bounded", nQ_, Qh, (double)args.eca_delay_max_m,
        (double)args.eca_doppler_max_mps);
}

void eca_clutter::remove(icf_t* work, uint32_t nof_slow, uint32_t nof_subc, double df_comb, double t_slow, double fc)
{
  if (work == nullptr || nof_slow < 2 || nof_subc < 1) {
    return;
  }
  if (nof_slow != c_slow_ || nof_subc != c_subc_ || df_comb != c_df_ || t_slow != c_ts_ || fc != c_fc_) {
    rebuild(nof_slow, nof_subc, df_comb, t_slow, fc);
  }
  if (Pt_.empty()) {
    return; // Doppler projector unavailable => leave data untouched (already logged)
  }

  // Step A: Hf = P_Φ · H  (project each row's subcarrier vector onto the delay subspace). Always
  // staged into hf_ (a copy when P_Φ = I) so step B reads an *unmodified* buffer — reading from
  // `work` directly would alias, since step B overwrites `work` row-by-row as it goes.
  hf_.resize((size_t)nof_slow * nof_subc);
  if (freq_identity_ || Pf_.empty()) {
    std::copy(work, work + (size_t)nof_slow * nof_subc, hf_.begin());
  } else {
    for (uint32_t n = 0; n < nof_slow; n++) {
      const icf_t* hrow = &work[(size_t)n * nof_subc];
      icf_t*       frow = &hf_[(size_t)n * nof_subc];
      for (uint32_t m = 0; m < nof_subc; m++) {
        icf_t        acc(0.0f, 0.0f);
        const icf_t* prow = &Pf_[(size_t)m * nof_subc];
        for (uint32_t mp = 0; mp < nof_subc; mp++) {
          acc += prow[mp] * hrow[mp];
        }
        frow[m] = acc;
      }
    }
  }

  // Step B + C: clutter[n][m] = Σ_{n'} Pt[n][n'] · Hf[n'][m];  work -= clutter.
  // (The inner sum walks the slow-time axis n', so hf_ is accessed with a nof_subc stride.)
  for (uint32_t n = 0; n < nof_slow; n++) {
    const icf_t* prow = &Pt_[(size_t)n * nof_slow];
    icf_t*       wrow = &work[(size_t)n * nof_subc];
    for (uint32_t m = 0; m < nof_subc; m++) {
      icf_t acc(0.0f, 0.0f);
      for (uint32_t np = 0; np < nof_slow; np++) {
        acc += prow[np] * hf_[(size_t)np * nof_subc + m];
      }
      wrow[m] -= acc;
    }
  }
}

} // namespace nr_isac
