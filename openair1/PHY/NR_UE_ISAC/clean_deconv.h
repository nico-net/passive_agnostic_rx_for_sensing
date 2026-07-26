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

/*! \file openair1/PHY/NR_UE_ISAC/clean_deconv.h
 * \brief Coherent CLEAN deconvolution of a range-Doppler map with a separable, shift-invariant PSF.
 *
 * A strong scatterer is not a single bright cell: the range window (over subcarriers) and the
 * Doppler window (over slow-time) each convolve it with their transform, spreading its energy into a
 * deterministic sidelobe/pedestal skirt. CA-CFAR then reports the brighter skirt bins as separate
 * "detections" -- the dominant multi-object ghost source in this pipeline (PHASE2_MOT_MULTIUE_HANDOVER.md).
 *
 * CLEAN removes that skirt at its source. When the range and Doppler operators are shift-invariant
 * (uniform full-band comb + uniform/NUDFT Doppler, NO per-row occupancy masking -- i.e. detector="fft"),
 * the 2-D point-spread response of a unit scatterer at cell (r0,d0) is the outer product
 * PSF[r,d] = kr[r-r0] * kd[d-d0], with kr / kd the (complex) range- and Doppler-axis kernels, each
 * normalised to 1 at zero lag. The algorithm iterates:
 *   1. find the residual's brightest cell (r0,d0), amplitude a = residual[r0,d0];
 *   2. subtract loop_gain * a * kr[.-r0] * kd[.-d0] from the whole residual (coherent);
 *   3. bank the component (loop_gain*a at (r0,d0));
 * until the residual peak falls stop_db below the initial peak or the component budget is spent. The
 * cleaned image restored for CFAR = residual + each banked component convolved with a narrow clean
 * beam; its POWER (|.|^2) replaces the CFAR input. Real scatterers survive (restored as clean peaks);
 * a strong target's own coherent skirt is gone, so it can no longer spawn ghosts.
 *
 * Scope: removes the LINEAR window-sidelobe/pedestal ghost family. It does NOT remove amplitude-gating
 * Doppler harmonics -- those are a data-domain effect (the true CFR carries a residual amplitude
 * modulation at the scheduling rate) that a clean, shift-invariant window PSF cannot represent; a PSF
 * modelled through the actual per-row occupancy would, but that is non-separable (see the .cc note).
 */

#ifndef NR_ISAC_CLEAN_DECONV_H
#define NR_ISAC_CLEAN_DECONV_H

#include <cstdint>
#include <vector>

#include "defs_nr_UE_ISAC.h"

namespace nr_isac {

struct clean_deconv_params {
  uint32_t max_components   = 8;     ///< hard cap on CLEAN iterations (component budget)
  float    loop_gain        = 0.8f;  ///< gamma in (0,1]: fraction of the peak subtracted per iteration
  float    stop_db          = 25.0f; ///< stop when the residual peak is this many dB below the initial peak
  uint32_t restore_bins     = 1;     ///< clean-beam half-width (bins); 0 = single-bin delta
};

/**
 * @brief Coherent CLEAN on a complex range-Doppler map using a separable shift-invariant PSF.
 *
 * @param cmap    complex range-Doppler map [R*D], row-major (range-major), fftshifted, WITH the
 *                zero-Doppler/zero-range notches already applied (notched cells read (0,0) and are
 *                thereby excluded from the peak search).
 * @param R       number of range bins
 * @param D       number of Doppler bins
 * @param kr      complex range kernel [R], indexed by range lag (kr[0] = zero lag, circular). Must be
 *                normalised so kr[0] = (1,0).
 * @param kd      complex Doppler kernel [D], indexed by Doppler lag (kd[0] = zero lag, circular),
 *                normalised so kd[0] = (1,0).
 * @param p       CLEAN parameters
 * @param[out] out_pow  cleaned POWER map [R*D] for CFAR (= |residual|^2 + restored clean beams). May
 *                      alias neither cmap nor the kernels. Caller may re-apply notches afterwards.
 * @param[out] n_components  optional: number of components extracted (nullptr to ignore)
 */
void clean_deconv_run(const icf_t*               cmap,
                      uint32_t                   R,
                      uint32_t                   D,
                      const icf_t*               kr,
                      const icf_t*               kd,
                      const clean_deconv_params& p,
                      float*                     out_pow,
                      uint32_t*                  n_components = nullptr);

} // namespace nr_isac

#endif // NR_ISAC_CLEAN_DECONV_H
