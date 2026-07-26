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

/*! \file openair1/PHY/NR_UE_ISAC/matrix_complete.h
 * \brief Low-rank matrix completion of the slow-time x subcarrier CFR grid (SVP), dependency-free.
 *
 * WHAT THIS IS FOR. The passive receiver only observes a CFR entry H[slot][subcarrier] where the
 * scheduler actually allocated that PRB in that slot. The occupancy is sparse and varies slot to
 * slot, so the effective aperture is amplitude-gated -- and that gating (a {0,1} mask multiplying the
 * target's slow-time tone) convolves each target's Doppler line with the schedule mask's spectrum,
 * producing the "perfect echo" harmonic ghosts at f_d +/- k*f_schedule observed on the 2-target scene
 * (PHASE2_MOT_MULTIUE_HANDOVER.md). Per-row linear frequency interpolation (interp_freq_row) and the
 * non-uniform Doppler DFT each address only part of this; neither fills the gaps with a model that
 * knows the scene is a few coherent scatterers.
 *
 * THE MODEL. Across a CPI a point scatterer contributes a rank-1 term (its Doppler steering vector
 * over slots) x (its delay steering vector over subcarriers); the whole CFR grid is therefore
 * LOW-RANK, rank ~ (number of scatterers incl. LOS). So filling the unobserved entries is exactly
 * low-rank matrix completion: recover the matrix of minimum rank (nuclear norm) that matches the
 * observed entries. The completed grid has a CONSISTENT full aperture every slot, so the amplitude
 * gating -- and its Doppler harmonics -- is removed at the source, before any range/Doppler transform.
 *
 * THE ALGORITHM (Singular Value Projection, Jain-Meka-Dhillon 2010). Dependency-free: no SVD library.
 *   X <- P_Omega(H)                         (observed entries, zero elsewhere)
 *   repeat:
 *     Q  <- orthonormal basis of the top-r left subspace of X   (randomized range finder + power its)
 *     X  <- Q (Q^H X)                        (projection onto the rank-r manifold)
 *     X[Omega] <- H[Omega]                   (data consistency: restore observed entries)
 * The randomized range finder needs only complex GEMM + modified Gram-Schmidt on a slots x r matrix
 * (r ~ 4, slots ~ 128), which is far cheaper than a full SVD and easy to write from scratch. Engine
 * thread only (non-RT).
 */

#ifndef NR_ISAC_MATRIX_COMPLETE_H
#define NR_ISAC_MATRIX_COMPLETE_H

#include <cstdint>
#include <vector>

#include "defs_nr_UE_ISAC.h" // icf_t

namespace nr_isac {

/**
 * @brief In-place low-rank completion of a row-major [n_slow][n_subc] complex CFR grid.
 *
 * Unobserved entries (mask[i]==0) are overwritten with the rank-`rank` completion; observed entries
 * (mask[i]!=0) are preserved exactly. A row (slot) or column with no observed entries is left as-is.
 *
 * @param H       row-major [n_slow*n_subc] CFR grid, modified in place
 * @param mask    row-major [n_slow*n_subc] occupancy (nonzero = observed / known)
 * @param n_slow  number of slow-time rows (slots)
 * @param n_subc  number of subcarriers
 * @param rank    target rank r (~ expected number of scatterers incl. LOS; clamped to [1, n_slow])
 * @param iters   number of SVP iterations (data-consistency + rank-r projection)
 * @param power_iters extra subspace power iterations per projection (0 = plain range finder)
 * @param scratch optional reusable workspace (avoids per-CPI allocation); may be nullptr
 */
struct matrix_complete_scratch;
void complete_lowrank(icf_t*    H,
                      const uint8_t* mask,
                      uint32_t  n_slow,
                      uint32_t  n_subc,
                      uint32_t  rank,
                      uint32_t  iters,
                      uint32_t  power_iters,
                      matrix_complete_scratch* scratch);

/// Reusable scratch buffers for complete_lowrank (one per engine thread). Opaque to callers.
struct matrix_complete_scratch {
  std::vector<icf_t> obs;   ///< [n_slow*n_subc] observed grid (P_Omega(H))
  std::vector<icf_t> Y;     ///< [n_slow*r] range-finder samples / Q
  std::vector<icf_t> Z;     ///< [n_subc*r] power-iteration intermediate
  std::vector<icf_t> P;     ///< [r*n_subc] Q^H X
  std::vector<icf_t> G;     ///< [n_subc*r] deterministic random test matrix
};

} // namespace nr_isac

#endif // NR_ISAC_MATRIX_COMPLETE_H
