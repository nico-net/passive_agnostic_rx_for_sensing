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

/*! \file openair1/PHY/NR_UE_ISAC/isac_fft.h
 * \brief Arbitrary-length complex DFT for the sensing DSP.
 *
 * OAI's built-in DFT (PHY/TOOLS) only supports the fixed OFDM sizes, but the sensing
 * range IFFT is sized by the CSI-RS/DM-RS comb (arbitrary) and the Doppler FFT by the
 * CPI length. This standalone plan runs entirely off the real-time path, so it favours
 * clarity and generality: a radix-2 fast path for power-of-two sizes and Bluestein's
 * chirp-z algorithm for any other length. It reproduces srsran_dft's normalised mode
 * (both directions scaled by 1/sqrt(N)) so the ported pipeline behaves identically.
 */

#ifndef NR_ISAC_FFT_H
#define NR_ISAC_FFT_H

#include <complex>
#include <cstdint>
#include <vector>

namespace nr_isac {

/// A reusable complex-DFT plan for one (size, direction). Precomputes twiddles / Bluestein
/// chirp on construction; run() has no allocations after the first call of a given size.
class fft_plan
{
public:
  /// @param n        transform length (>= 1)
  /// @param inverse  true for the inverse (backward, +j) transform, false for forward (-j)
  fft_plan(uint32_t n, bool inverse);

  /// Transform @p in (length N) into @p out (length N), scaled by 1/sqrt(N). May alias-safe:
  /// @p in and @p out must not overlap.
  void run(const std::complex<float>* in, std::complex<float>* out);

  uint32_t size() const { return N; }

private:
  using cd = std::complex<double>;

  // In-place iterative radix-2 FFT of @p a (length is a power of two). @p invert selects sign.
  static void fft_pow2(std::vector<cd>& a, bool invert);

  uint32_t N       = 0;
  bool     inverse = false;
  bool     is_pow2 = false;

  // Bluestein state (used when N is not a power of two): M is the next power of two >= 2N-1.
  uint32_t        M = 0;
  std::vector<cd> chirp;   ///< w[n] = exp(-/+ j*pi*n^2/N), n = 0..N-1
  std::vector<cd> b_fft;   ///< FFT of the zero-padded conjugate chirp kernel (length M)

  // Scratch reused across run() calls (single-threaded engine use only).
  std::vector<cd> scratch;
};

} // namespace nr_isac

#endif // NR_ISAC_FFT_H
