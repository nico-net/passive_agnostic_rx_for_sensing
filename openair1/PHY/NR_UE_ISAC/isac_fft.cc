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

#include "isac_fft.h"
#include <cmath>

namespace nr_isac {

static uint32_t next_pow2(uint32_t v)
{
  uint32_t p = 1;
  while (p < v) {
    p <<= 1;
  }
  return p;
}

void fft_plan::fft_pow2(std::vector<cd>& a, bool invert)
{
  const size_t n = a.size();
  // Bit-reversal permutation
  for (size_t i = 1, j = 0; i < n; i++) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(a[i], a[j]);
    }
  }
  // Danielson-Lanczos butterflies
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = (invert ? 2.0 : -2.0) * M_PI / (double)len;
    const cd     wlen(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      cd w(1.0, 0.0);
      for (size_t k = 0; k < len / 2; k++) {
        const cd u = a[i + k];
        const cd v = a[i + k + len / 2] * w;
        a[i + k]           = u + v;
        a[i + k + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
}

fft_plan::fft_plan(uint32_t n, bool inv) : N(n), inverse(inv)
{
  if (N < 1) {
    N = 1;
  }
  is_pow2 = (N & (N - 1)) == 0;
  if (is_pow2) {
    return;
  }

  // Bluestein: x_k = sum_n x_n exp(-/+ j*2*pi*k*n/N).
  // Using k*n = (k^2 + n^2 - (k-n)^2)/2, this becomes a convolution of (x_n * chirp_n) with
  // conj(chirp), evaluated by a length-M (power of two) FFT with M >= 2N-1.
  M = next_pow2(2 * N - 1);
  chirp.resize(N);
  const double sign = inverse ? +1.0 : -1.0;
  for (uint32_t k = 0; k < N; k++) {
    // exp(sign * j * pi * k^2 / N); use k^2 mod 2N to keep the argument small and accurate.
    const double phase = sign * M_PI * (double)((uint64_t)k * k % (2ULL * N)) / (double)N;
    chirp[k]           = cd(std::cos(phase), std::sin(phase));
  }

  // Kernel b_n = conj(chirp_n) for n in [0,N), symmetric-extended to negative indices at M-n.
  b_fft.assign(M, cd(0.0, 0.0));
  b_fft[0] = std::conj(chirp[0]);
  for (uint32_t n = 1; n < N; n++) {
    const cd v      = std::conj(chirp[n]);
    b_fft[n]        = v;
    b_fft[M - n]    = v;
  }
  fft_pow2(b_fft, false);

  scratch.resize(M);
}

void fft_plan::run(const std::complex<float>* in, std::complex<float>* out)
{
  const double norm = 1.0 / std::sqrt((double)N);

  if (N == 1) {
    out[0] = in[0];
    return;
  }

  if (is_pow2) {
    scratch.assign(N, cd(0.0, 0.0));
    for (uint32_t i = 0; i < N; i++) {
      scratch[i] = cd((double)in[i].real(), (double)in[i].imag());
    }
    fft_pow2(scratch, inverse);
    for (uint32_t i = 0; i < N; i++) {
      out[i] = std::complex<float>((float)(scratch[i].real() * norm), (float)(scratch[i].imag() * norm));
    }
    return;
  }

  // Bluestein path: a_n = x_n * chirp_n, zero-padded to M.
  std::vector<cd>& a = scratch;
  a.assign(M, cd(0.0, 0.0));
  for (uint32_t n = 0; n < N; n++) {
    a[n] = cd((double)in[n].real(), (double)in[n].imag()) * chirp[n];
  }
  fft_pow2(a, false);
  for (uint32_t i = 0; i < M; i++) {
    a[i] *= b_fft[i];
  }
  fft_pow2(a, true); // inverse length-M FFT (unnormalised)
  const double inv_m = 1.0 / (double)M;
  for (uint32_t k = 0; k < N; k++) {
    const cd conv = a[k] * inv_m;      // convolution result
    const cd val  = conv * chirp[k];   // multiply back by chirp_k
    out[k]        = std::complex<float>((float)(val.real() * norm), (float)(val.imag() * norm));
  }
}

} // namespace nr_isac
