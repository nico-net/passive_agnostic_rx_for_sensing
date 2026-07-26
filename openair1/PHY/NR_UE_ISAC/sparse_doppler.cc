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

#include "sparse_doppler.h"

#include <cmath>

namespace nr_isac {

namespace {

// Complex soft-threshold (proximal operator of t*||.||_1): shrink magnitude by t, keep phase, floor
// at zero magnitude.
inline icf_t soft_threshold(const icf_t& v, float t)
{
  const float mag = std::abs(v);
  if (mag <= t || mag <= 1e-20f) {
    return icf_t(0.0f, 0.0f);
  }
  const float scale = 1.0f - t / mag;
  return v * scale;
}

} // namespace

void sparse_doppler_prepare(const double*        row_time_slots,
                            uint32_t              nof_slow,
                            float                 period_slots,
                            uint32_t              nof_dopp,
                            sparse_doppler_ctx&   ctx)
{
  ctx.nof_slow = nof_slow;
  ctx.nof_dopp = nof_dopp;
  ctx.A.assign((size_t)nof_slow * nof_dopp, icf_t(0.0f, 0.0f));

  const double invN = 1.0 / (double)nof_dopp;
  const double norm = 1.0 / std::sqrt((double)nof_dopp);
  const double per  = (period_slots > 0.0f) ? (double)period_slots : 1.0;
  for (uint32_t k = 0; k < nof_slow; k++) {
    const double tau = row_time_slots[k] / per;
    icf_t* Ak = &ctx.A[(size_t)k * nof_dopp];
    for (uint32_t n = 0; n < nof_dopp; n++) {
      const double ph = -2.0 * M_PI * (double)n * tau * invN;
      Ak[n] = icf_t((float)(std::cos(ph) * norm), (float)(std::sin(ph) * norm));
    }
  }

  // Lipschitz constant = largest eigenvalue of G = A^H A (nof_dopp x nof_dopp), via power iteration
  // directly on v -> G v = A^H (A v), never forming G explicitly (cheap: O(nof_slow*nof_dopp) per
  // iteration, same cost as one FISTA gradient step, done a handful of times once per CPI).
  std::vector<icf_t> v(nof_dopp), Av(nof_slow), Gv(nof_dopp);
  uint32_t seed = 0x2545F491u;
  for (uint32_t n = 0; n < nof_dopp; n++) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    const float re = ((float)(seed & 0xFFFFu) / 32768.0f) - 1.0f;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    const float im = ((float)(seed & 0xFFFFu) / 32768.0f) - 1.0f;
    v[n] = icf_t(re, im);
  }
  double lipschitz = 1.0;
  for (int it = 0; it < 40; it++) {
    // Av = A v
    for (uint32_t k = 0; k < nof_slow; k++) {
      const icf_t* Ak = &ctx.A[(size_t)k * nof_dopp];
      icf_t acc(0.0f, 0.0f);
      for (uint32_t n = 0; n < nof_dopp; n++) acc += Ak[n] * v[n];
      Av[k] = acc;
    }
    // Gv = A^H Av
    for (uint32_t n = 0; n < nof_dopp; n++) Gv[n] = icf_t(0.0f, 0.0f);
    for (uint32_t k = 0; k < nof_slow; k++) {
      const icf_t* Ak = &ctx.A[(size_t)k * nof_dopp];
      const icf_t  akv = Av[k];
      for (uint32_t n = 0; n < nof_dopp; n++) Gv[n] += std::conj(Ak[n]) * akv;
    }
    double nrm2 = 0.0;
    for (uint32_t n = 0; n < nof_dopp; n++) nrm2 += (double)std::norm(Gv[n]);
    const double nrm = std::sqrt(nrm2);
    if (nrm <= 1e-20) break;
    // Rayleigh quotient v^H G v / v^H v with the NEW (unnormalised) Gv as the Rayleigh estimate proxy:
    // since v is already unit-norm from the previous normalisation, ||Gv|| itself converges to the
    // dominant eigenvalue for a Hermitian PSD operator under power iteration.
    lipschitz = nrm;
    const float inv = (float)(1.0 / nrm);
    for (uint32_t n = 0; n < nof_dopp; n++) v[n] = Gv[n] * inv;
  }
  ctx.lipschitz = (float)std::max(lipschitz, 1e-6);
}

void sparse_doppler_solve(const sparse_doppler_ctx& ctx,
                          const icf_t*              row,
                          uint32_t                  iters,
                          float                     lambda,
                          std::vector<icf_t>&       x_out)
{
  const uint32_t K = ctx.nof_slow, N = ctx.nof_dopp;
  x_out.assign(N, icf_t(0.0f, 0.0f));
  if (K == 0 || N == 0 || ctx.A.size() != (size_t)K * N) {
    return;
  }
  const float step = 1.0f / ctx.lipschitz;
  const float thr   = lambda * step;

  std::vector<icf_t> x_prev(N, icf_t(0.0f, 0.0f));
  std::vector<icf_t> z(N, icf_t(0.0f, 0.0f));
  std::vector<icf_t> Az(K);
  std::vector<icf_t> grad(N);
  float t_k = 1.0f;

  for (uint32_t it = 0; it < iters; it++) {
    // Az = A z
    for (uint32_t k = 0; k < K; k++) {
      const icf_t* Ak = &ctx.A[(size_t)k * N];
      icf_t acc(0.0f, 0.0f);
      for (uint32_t n = 0; n < N; n++) acc += Ak[n] * z[n];
      Az[k] = acc - row[k]; // residual r = A z - y
    }
    // grad = A^H r
    for (uint32_t n = 0; n < N; n++) grad[n] = icf_t(0.0f, 0.0f);
    for (uint32_t k = 0; k < K; k++) {
      const icf_t* Ak = &ctx.A[(size_t)k * N];
      const icf_t  rk = Az[k];
      for (uint32_t n = 0; n < N; n++) grad[n] += std::conj(Ak[n]) * rk;
    }
    // x = soft_threshold(z - step*grad, thr)
    std::vector<icf_t> x(N);
    for (uint32_t n = 0; n < N; n++) {
      x[n] = soft_threshold(z[n] - grad[n] * step, thr);
    }
    // FISTA momentum update
    const float t_next = 0.5f * (1.0f + std::sqrt(1.0f + 4.0f * t_k * t_k));
    const float mom     = (t_k - 1.0f) / t_next;
    for (uint32_t n = 0; n < N; n++) {
      z[n] = x[n] + mom * (x[n] - x_prev[n]);
    }
    x_prev.swap(x);
    t_k = t_next;
  }
  x_out = x_prev;
}

} // namespace nr_isac
