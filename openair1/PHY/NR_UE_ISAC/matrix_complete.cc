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

#include "matrix_complete.h"

#include <cmath>

namespace nr_isac {

namespace {

// Deterministic xorshift PRNG -> uniform in (-1,1). Fixed seed so completion is reproducible
// (no run-to-run variation from the random range-finder test matrix).
struct xorshift32 {
  uint32_t s;
  explicit xorshift32(uint32_t seed) : s(seed ? seed : 0x9e3779b9u) {}
  float next()
  {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return ((float)(s & 0xFFFFFFu) / 8388608.0f) - 1.0f; // 24-bit mantissa -> [-1,1)
  }
};

// Modified Gram-Schmidt orthonormalisation of an m x r column set stored row-major in Q[m*r]
// (column k = Q[i*r + k]). Degenerate columns (norm ~ 0) are zeroed.
void mgs_orthonormalize(icf_t* Q, uint32_t m, uint32_t r)
{
  for (uint32_t k = 0; k < r; k++) {
    // Subtract projections onto the already-orthonormal columns 0..k-1.
    for (uint32_t j = 0; j < k; j++) {
      icf_t dot(0.0f, 0.0f); // <q_j, v_k> = sum_i conj(Q[i,j]) * Q[i,k]
      for (uint32_t i = 0; i < m; i++) {
        dot += std::conj(Q[(size_t)i * r + j]) * Q[(size_t)i * r + k];
      }
      for (uint32_t i = 0; i < m; i++) {
        Q[(size_t)i * r + k] -= dot * Q[(size_t)i * r + j];
      }
    }
    double nrm2 = 0.0;
    for (uint32_t i = 0; i < m; i++) {
      const icf_t v = Q[(size_t)i * r + k];
      nrm2 += (double)v.real() * v.real() + (double)v.imag() * v.imag();
    }
    const float nrm = (float)std::sqrt(nrm2);
    if (nrm > 1e-12f) {
      const float inv = 1.0f / nrm;
      for (uint32_t i = 0; i < m; i++) {
        Q[(size_t)i * r + k] *= inv;
      }
    } else {
      for (uint32_t i = 0; i < m; i++) {
        Q[(size_t)i * r + k] = icf_t(0.0f, 0.0f);
      }
    }
  }
}

} // namespace

void complete_lowrank(icf_t*                    H,
                      const uint8_t*            mask,
                      uint32_t                  n_slow,
                      uint32_t                  n_subc,
                      uint32_t                  rank,
                      uint32_t                  iters,
                      uint32_t                  power_iters,
                      matrix_complete_scratch*  scratch)
{
  if (H == nullptr || mask == nullptr || n_slow < 2 || n_subc < 2) {
    return;
  }
  uint32_t r = rank;
  if (r < 1) r = 1;
  if (r > n_slow) r = n_slow;
  if (iters < 1) iters = 1;

  matrix_complete_scratch local;
  matrix_complete_scratch* w = (scratch != nullptr) ? scratch : &local;
  const size_t mn = (size_t)n_slow * n_subc;
  w->obs.assign(mn, icf_t(0.0f, 0.0f)); // P_Omega(H): observed entries, 0 elsewhere
  w->Y.assign((size_t)n_slow * r, icf_t(0.0f, 0.0f));
  w->Z.assign((size_t)n_subc * r, icf_t(0.0f, 0.0f));
  w->P.assign((size_t)r * n_subc, icf_t(0.0f, 0.0f));
  w->G.assign((size_t)n_subc * r, icf_t(0.0f, 0.0f));

  // Snapshot the observed entries and seed the working grid X (= H) so unobserved entries start at 0.
  for (size_t i = 0; i < mn; i++) {
    if (mask[i]) {
      w->obs[i] = H[i];
      H[i]      = H[i];
    } else {
      H[i] = icf_t(0.0f, 0.0f);
    }
  }

  // Deterministic random test matrix G (n_subc x r), fixed across iterations.
  xorshift32 rng(0x1234567u);
  for (size_t i = 0; i < w->G.size(); i++) {
    w->G[i] = icf_t(rng.next(), rng.next());
  }

  icf_t* X = H; // work in place on H

  for (uint32_t it = 0; it < iters; it++) {
    // --- Randomized range finder: Y = X * G  (n_slow x r) ---
    for (uint32_t i = 0; i < n_slow; i++) {
      const icf_t* xr = &X[(size_t)i * n_subc];
      for (uint32_t k = 0; k < r; k++) {
        icf_t acc(0.0f, 0.0f);
        for (uint32_t j = 0; j < n_subc; j++) {
          acc += xr[j] * w->G[(size_t)j * r + k];
        }
        w->Y[(size_t)i * r + k] = acc;
      }
    }
    // --- Power iterations: Y <- X (X^H Y) to sharpen toward the dominant subspace ---
    for (uint32_t p = 0; p < power_iters; p++) {
      // Z = X^H Y  (n_subc x r)
      for (uint32_t j = 0; j < n_subc; j++) {
        for (uint32_t k = 0; k < r; k++) {
          w->Z[(size_t)j * r + k] = icf_t(0.0f, 0.0f);
        }
      }
      for (uint32_t i = 0; i < n_slow; i++) {
        const icf_t* xr = &X[(size_t)i * n_subc];
        for (uint32_t k = 0; k < r; k++) {
          const icf_t yik = std::conj(w->Y[(size_t)i * r + k]);
          // Z[j,k] += conj(X[i,j]) * Y[i,k]  ==  conj( X[i,j] * conj(Y[i,k]) )
          for (uint32_t j = 0; j < n_subc; j++) {
            w->Z[(size_t)j * r + k] += std::conj(xr[j] * yik);
          }
        }
      }
      // Y = X Z  (n_slow x r)
      for (uint32_t i = 0; i < n_slow; i++) {
        const icf_t* xr = &X[(size_t)i * n_subc];
        for (uint32_t k = 0; k < r; k++) {
          icf_t acc(0.0f, 0.0f);
          for (uint32_t j = 0; j < n_subc; j++) {
            acc += xr[j] * w->Z[(size_t)j * r + k];
          }
          w->Y[(size_t)i * r + k] = acc;
        }
      }
    }
    // --- Q = orthonormal basis of Y (n_slow x r), in place ---
    mgs_orthonormalize(w->Y.data(), n_slow, r);

    // --- P = Q^H X   (r x n_subc) ---
    for (size_t i = 0; i < w->P.size(); i++) {
      w->P[i] = icf_t(0.0f, 0.0f);
    }
    for (uint32_t i = 0; i < n_slow; i++) {
      const icf_t* xr = &X[(size_t)i * n_subc];
      for (uint32_t k = 0; k < r; k++) {
        const icf_t qik = std::conj(w->Y[(size_t)i * r + k]);
        icf_t*      pk  = &w->P[(size_t)k * n_subc];
        for (uint32_t j = 0; j < n_subc; j++) {
          pk[j] += qik * xr[j];
        }
      }
    }
    // --- X = Q P   (n_slow x n_subc), then restore observed entries (data consistency) ---
    for (uint32_t i = 0; i < n_slow; i++) {
      icf_t* xr = &X[(size_t)i * n_subc];
      for (uint32_t j = 0; j < n_subc; j++) {
        icf_t acc(0.0f, 0.0f);
        for (uint32_t k = 0; k < r; k++) {
          acc += w->Y[(size_t)i * r + k] * w->P[(size_t)k * n_subc + j];
        }
        xr[j] = acc;
      }
    }
    for (size_t i = 0; i < mn; i++) {
      if (mask[i]) {
        X[i] = w->obs[i];
      }
    }
  }
}

} // namespace nr_isac
