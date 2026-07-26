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

/*! \file openair1/PHY/NR_UE_ISAC/eca_clutter.h
 * \brief ECA / ECA+ clutter cancellation for the CFR-domain sensing pipeline.
 *
 * Replaces the per-subcarrier slow-time mean subtraction (range_doppler.cc) with an
 * Extensive Cancellation Algorithm (ECA/ECA+) oblique projection, per
 * ECA_CLUTTER_HANDOVER.md. Reference method: Wypich & Zielinski, Sensors 2026, 26, 1317
 * (their ECA+, refs [38,39] = Searle et al. 2018); original ECA: Colone et al.,
 * IEEE TAES 2009, 45, 698-722.
 *
 * WHY THIS FORM (design decision, so it is not re-litigated):
 *   The classic ECA builds a disturbance subspace from time/Doppler-shifted copies of the
 *   *raw reference* signal and least-squares-projects the *raw surveillance* signal onto its
 *   orthogonal complement. This pipeline is different: the sensing "sources" are already the
 *   channel estimate H = Y/X (Ĥ = Y/X, csi_rs / pdsch_dmrs / pdsch_data — see CLAUDE.md), so
 *   the data modulation has been divided out and the effective reference is *ideal*. In the
 *   CFR domain a scatterer at bistatic delay τ and Doppler f_d contributes a separable 2-D
 *   complex sinusoid H[n,m] = a·exp(-j2π m·Δf·τ)·exp(+j2π f_d·n·T_slow). The clutter
 *   (direct path + static/slow scatterers) is exactly the set of such atoms with delay in a
 *   bounded near-range window and Doppler in a narrow band around zero. That removal region is
 *   a *rectangle* in delay×Doppler, i.e. a tensor product of a delay atom set Φ (frequency
 *   axis) and a Doppler atom set Ψ (slow-time axis). The orthogonal projector onto a tensor-
 *   product subspace factorizes: P_{Φ⊗Ψ} = P_Φ ⊗ P_Ψ. So the ECA-cleaned CFR is
 *
 *        H_clean = H - P_Φ · H · P_Ψ ,
 *
 *   with P_Φ = Φ(ΦᴴΦ)⁻¹Φᴴ acting on the subcarrier (delay) index and P_Ψ = Ψ(ΨᴴΨ)⁻¹Ψᴴ acting
 *   on the slow-time (Doppler) index. This is the exact ECA oblique projection for this pipeline's
 *   ideal-reference case — no approximation — and it is far cheaper than a general N×N projection
 *   (N = nof_slow·nof_subc): both projectors are small dense matrices, no big matrix ever formed.
 *
 * WHY IT FIXES THE MIRROR GHOST (the motivating bug, ECA_CLUTTER_HANDOVER.md §"Why this"):
 *   The conjugate-image ghost at range bin (nof_range-1-r) is produced by the range IFFT acting
 *   on the near-*real*-valued residual that a static/slow scatterer leaves after mean subtraction
 *   (mean subtraction removes only the exact-DC slow-time component; CFO/SFO smear a static tap to
 *   ±1-2 Doppler bins, which it cannot touch). ECA removes the whole near-zero-Doppler clutter band
 *   *in the complex CFR domain, before the range IFFT*, so that real-valued residual never reaches
 *   the transform and the conjugate image never forms — the fix lives upstream of detection, not as
 *   a post-CFAR filter. (conj_image_reject stays as a defence-in-depth safety net; see the handover.)
 *
 * RELATION TO MEAN SUBTRACTION: mean subtraction is the exact special case P_Φ = I (delay_max
 * covers the full range) and Ψ = {DC only} (doppler_max = 0 ⇒ a single all-ones slow-time atom):
 * H - I·H·P_dc = H minus its per-subcarrier slow-time mean. eca_clutter reproduces it bit-for-bit
 * in that configuration (regression-tested in tests/eca_clutter_test.cc).
 */

#ifndef NR_ISAC_ECA_CLUTTER_H
#define NR_ISAC_ECA_CLUTTER_H

#include <cstdint>
#include <vector>

#include "defs_nr_UE_ISAC.h"

namespace nr_isac {

/**
 * @brief CFR-domain ECA/ECA+ clutter canceller.
 *
 * Builds the delay projector P_Φ and Doppler projector P_Ψ once per (dimension, geometry)
 * combination and caches them; @ref remove then applies H_clean = H - P_Φ·H·P_Ψ in place.
 * Engine-thread (non real-time) use only, like range_doppler.
 */
class eca_clutter
{
public:
  explicit eca_clutter(const nr_isac_args_t& args_) : args(args_) {}

  /**
   * @brief Removes the clutter subspace from @p work in place.
   *
   * @param work      CFR matrix, row-major [nof_slow][nof_subc] (row = slow-time occurrence).
   * @param nof_slow  Slow-time length of the CPI.
   * @param nof_subc  Number of (fused-grid) subcarriers per row.
   * @param df_comb   Subcarrier (column) spacing in Hz (SCS × grid comb; 1 for the fused grid).
   * @param t_slow    Slow-time row spacing in seconds (period_slots × slot duration).
   * @param fc        Carrier centre frequency in Hz (for the velocity→Doppler mapping). If ≤0 the
   *                  Doppler band collapses to DC-only (zero-velocity) removal.
   */
  void remove(icf_t* work, uint32_t nof_slow, uint32_t nof_subc, double df_comb, double t_slow, double fc);

  /// Effective atom counts from the last @ref remove (diagnostics/logging/tests).
  uint32_t last_delay_atoms() const { return nP_; }
  uint32_t last_doppler_atoms() const { return nQ_; }
  bool     freq_is_identity() const { return freq_identity_; }

private:
  // Rebuild the cached projectors if any dimension/geometry input changed since last call.
  void rebuild(uint32_t nof_slow, uint32_t nof_subc, double df_comb, double t_slow, double fc);

  nr_isac_args_t args;

  // Cache key (rebuild only when one of these changes).
  uint32_t c_slow_ = 0, c_subc_ = 0;
  double   c_df_ = 0.0, c_ts_ = 0.0, c_fc_ = 0.0;

  // Dense Hermitian projectors. Pf_ is [nof_subc][nof_subc] (delay), Pt_ is [nof_slow][nof_slow]
  // (Doppler). When the delay window spans the whole range, P_Φ = I: we skip building Pf_ and set
  // freq_identity_ so remove() applies only the Doppler projector (the common default path).
  std::vector<icf_t> Pf_;
  std::vector<icf_t> Pt_;
  bool               freq_identity_ = false;

  // Scratch: intermediate Hf = P_Φ·H, [nof_slow][nof_subc].
  std::vector<icf_t> hf_;

  uint32_t nP_ = 0, nQ_ = 0; // effective atom counts
};

} // namespace nr_isac

#endif // NR_ISAC_ECA_CLUTTER_H
