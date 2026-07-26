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

/*! \file openair1/PHY/NR_UE_ISAC/sparse_doppler.h
 * \brief L1-regularized (FISTA) sparse Doppler recovery over irregular slow-time samples.
 *
 * WHAT THIS IS FOR. A dense Doppler transform (uniform FFT or the non-uniform DFT in
 * range_doppler.cc's doppler_nudft path) evaluates ALL candidate frequencies against the observed,
 * irregularly-timed slow-time samples. When the occupancy is gappy (scheduler-driven), a single real
 * scatterer's tone is NOT perfectly orthogonal to the other candidate frequencies under that irregular
 * sampling, so the dense transform leaks real energy into "harmonic" ghost bins even though only one
 * true frequency exists (RVM-confirmed, PHASE2_MOT_MULTIUE_HANDOVER.md: dense NUDFT gave only ~5.5 dB
 * peak-to-harmonic ratio on a gappy single tone in an offline check).
 *
 * L1-regularized (basis-pursuit-denoising) recovery instead finds the SPARSEST spectrum consistent
 * with the observed samples: min_x 0.5||Ax-y||_2^2 + lambda||x||_1, solved with FISTA (Beck-Teboulle
 * 2009), a first-order proximal-gradient method -- no external solver library, same
 * dependency-free style as isac_fft/matrix_complete. Because using a SECOND (harmonic) frequency to
 * explain the same data costs an extra L1 penalty with no reduction in fit error (a single tone
 * already explains the gappy samples exactly), the optimum places all the energy on the true
 * frequency and drives the harmonic coefficients to exactly zero -- offline-validated
 * (numerically cross-checked in Python before porting, see the file's git history) at ~100+ dB
 * peak-to-harmonic ratio on the same gappy single-tone case that gave the dense transform only 5.5 dB,
 * and correctly preserves a second, weaker, genuinely-present target at its right relative power.
 *
 * SCOPE (why this is NOT a full RVM replacement). FISTA costs O(iters * nof_dopp * nof_slow) per
 * range bin -- for the full 3276-range-bin / 128-slot grid this is far too expensive to run on every
 * bin every CPI (~40x the cost of doppler_nudft's already-heavy O(N^2), which itself is only run on
 * one CPI's grid, not per range bin). Instead this is used as a VERIFICATION/refinement pass on only
 * the handful of range bins the (cheap) dense CFAR pass already flagged: re-solve just those rows,
 * and drop any detection whose reported Doppler bin does not survive as a significant peak in the
 * sparse solution -- i.e. it needed to borrow energy from a neighbouring bin to appear in the dense
 * transform, which sparse recovery reveals it didn't actually need. See range_doppler.cc's
 * doppler_sparse block.
 */

#ifndef NR_ISAC_SPARSE_DOPPLER_H
#define NR_ISAC_SPARSE_DOPPLER_H

#include <cstdint>
#include <vector>

#include "defs_nr_UE_ISAC.h" // icf_t

namespace nr_isac {

/// Dictionary + its Lipschitz constant, built once per CPI (depends only on the row times / nof_dopp,
/// not on any particular range bin's data) and reused across every verified range bin that CPI.
struct sparse_doppler_ctx {
  std::vector<icf_t> A;         ///< [nof_slow][nof_dopp] row-major dictionary, A[k,n] matches
                                ///< nudft_mat's convention (exp(-j2pi*n*tau_k/N)/sqrt(N))
  float               lipschitz = 1.0f; ///< largest eigenvalue of A^H A (FISTA step = 1/lipschitz)
  uint32_t            nof_slow  = 0;
  uint32_t            nof_dopp  = 0;
};

/**
 * @brief Builds the irregular-sampling dictionary and its Lipschitz constant for one CPI.
 * @param row_time_slots actual sample time of each slow-time row, in slots (row 0 need not be 0)
 * @param nof_slow       number of slow-time rows
 * @param period_slots   nominal mean row spacing (slots), used to normalise row_time_slots
 * @param nof_dopp       number of candidate Doppler bins (dictionary columns)
 */
void sparse_doppler_prepare(const double* row_time_slots,
                            uint32_t      nof_slow,
                            float         period_slots,
                            uint32_t      nof_dopp,
                            sparse_doppler_ctx& ctx);

/**
 * @brief FISTA-solves one range bin's slow-time CIR row against ctx's cached dictionary.
 * @param ctx     prepared dictionary (sparse_doppler_prepare), reused across range bins
 * @param row     [nof_slow] this range bin's slow-time CIR samples
 * @param iters   FISTA iteration count
 * @param lambda  absolute L1 threshold (caller derives this from a measured noise estimate --
 *                see range_doppler.cc's doppler_sparse block for the median-profile estimator)
 * @param[out] x_out [nof_dopp] recovered sparse spectrum, NATURAL (unshifted) bin order matching the
 *             dictionary's own index n -- apply the same fftshift convention as the main Doppler
 *             stage if comparing against rvm.power's storage order.
 */
void sparse_doppler_solve(const sparse_doppler_ctx& ctx,
                          const icf_t*              row,
                          uint32_t                  iters,
                          float                     lambda,
                          std::vector<icf_t>&       x_out);

} // namespace nr_isac

#endif // NR_ISAC_SPARSE_DOPPLER_H
