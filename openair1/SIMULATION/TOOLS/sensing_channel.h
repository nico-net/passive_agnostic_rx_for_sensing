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

/*! \file openair1/SIMULATION/TOOLS/sensing_channel.h
 * \brief Synthetic moving-target sensing channel for the rfsimulator downlink path.
 *
 * Generates a static direct path (LOS) plus one or more moving reflecting objects following
 * configured piecewise-linear trajectories, and synthesizes the resulting time-varying channel
 * impulse response (CIR) into channel_desc_t.ch[] once per rfsimulator RX block. This lets the
 * NR_UE_ISAC passive-sensing pipeline be exercised end-to-end (SSB sync -> CSI-RS/PDSCH channel
 * estimation -> CPI -> range-Doppler -> detection) against a controllable ground truth, with no SDR
 * and no over-the-air perturbations.
 *
 * Design (see the module .c for the full rationale):
 *  - Each object's tap carries a COMPLEX gain exp(-j 2*pi R(t)/lambda) at continuous delay
 *    tau = dR(t)/c * fs, spread across taps with a Hann-windowed sinc fractional-delay kernel (so a
 *    moving target slides smoothly across range bins instead of snapping, which would inject a false
 *    Doppler smear). Doppler emerges from the block-to-block phase progression -- channel_desc_t's
 *    single scalar Doppler_phase_inc is left at 0, so rxAddInput() is untouched.
 *  - Injected ONLY on the downlink (gNB->UE) channel model; the uplink stays clean so --do-ra
 *    RACH/attach is unaffected.
 *
 * This header intentionally exposes only the opaque lifecycle; the trajectory struct is private.
 */

#ifndef SENSING_CHANNEL_H
#define SENSING_CHANNEL_H

#include <stdint.h>

#include "sim.h" /* channel_desc_t */

#ifdef __cplusplus
extern "C" {
#endif

#define SENSING_CHANNEL_SECTION "sensing_channel"

/**
 * @brief Parse the [sensing_channel] config section and build trajectory state for a DL channel.
 *
 * Thin wrapper: reads the config values, then calls sensing_channel_make(). Uses @p cd 's
 * center_freq / sampling_rate / nb_tx / nb_rx / channel_length.
 *
 * @return heap sensing_traj_t* to store in cd->sensing_traj, or NULL if the section is absent /
 *         disabled / malformed (leaving @p cd otherwise untouched).
 */
void *sensing_channel_parse(channel_desc_t *cd);

/**
 * @brief Build + configure a sensing channel programmatically (no config module). Also used by the
 * unit test so the tap-synthesis math can be exercised without a config file.
 *
 * If the required tap count (largest object delay + fractional-kernel half-width, then @p
 * channel_length as a floor) exceeds the descriptor's current channel_length, GROWS cd->ch[] (and
 * channel_length) so object delays fit -- the base rfsim model type (e.g. AWGN) may allocate too few.
 *
 * @param objects_spec  '|'-separated objects, each "refl;t,x,y;t,x,y;..." (same encoding as the
 *                      config 'objects' field); may be NULL/empty for a LOS-only channel.
 * @return heap sensing_traj_t* (store in cd->sensing_traj), or NULL on invalid @p cd.
 */
void *sensing_channel_make(channel_desc_t *cd,
                           double          tx_x,
                           double          tx_y,
                           double          rx_x,
                           double          rx_y,
                           double          los_gain_db,
                           double          los_delay_samples,
                           int             frac_delay_taps,
                           int             channel_length,
                           const char     *objects_spec);

/**
 * @brief Attach an RX antenna-array geometry so per-antenna taps carry the correct inter-element
 * phase (this is what makes AoA observable in simulation — see PHASE3_AOA_MULTISTATIC_HANDOVER §5.5).
 *
 * Without this the descriptor's rx antennas all receive the identical CIR and any AoA estimator
 * reads exactly zero phase difference. With it, element @c i at offset @c d_i gets the extra factor
 * @c exp(+j*2*pi*(d_i·û)/lambda), where @c û points FROM the receiver TOWARDS the scatterer (the
 * element is closer to the source by @c d_i·û, hence a phase ADVANCE).
 *
 * Narrowband array assumption, stated so it isn't re-derived: the across-aperture delay spread
 * (~4 cm / c = 0.13 ns) is two orders of magnitude below one sample at any rate this simulator runs
 * at (8.1 ns at 122.88 MHz), so the element offset changes the tap PHASE only, never its delay
 * @c tau. All elements therefore share one fractional-delay kernel.
 *
 * @param traj          the handle returned by sensing_channel_make()/_parse().
 * @param spec          element offsets, "x,y;x,y;..." in metres, in the ARRAY frame. NULL/empty (or
 *                      a single element) restores the co-located behaviour exactly.
 * @param boresight_deg rotation of the array frame into ENU (degrees CCW from +x/east); the offsets
 *                      are rotated by this once, here, so everything downstream is ENU-aligned.
 * @return number of elements accepted (0 on a NULL/empty/malformed spec).
 */
int sensing_channel_set_rx_array(void *traj, const char *spec, double boresight_deg);

/**
 * @brief Inject a RECEIVER CLOCK error (STO / CFO / SFO) into the sensing channel.
 *
 * WHY THIS EXISTS. rfsimulator runs both ends as processes on one machine clock, so the impairments
 * the UE's Phase 1-3 sync stack exists to correct are almost absent: measured on the 4-object scene,
 * CFO rms 0.42 Hz and STO drift 0.12 bins/CPI, with the SFO correction correctly withheld by its own
 * linearity gate because there is no real drift to fit. A sync-on/off A/B on that harness therefore
 * measures "correcting nothing changes nothing" and can neither validate nor condemn the algorithm.
 * This function gives the harness a KNOWN impairment to recover, so the sync stack can be scored
 * against ground truth without an SDR.
 *
 * All three are applied COMMON-MODE to every path (LOS and every target), because a clock error
 * belongs to the receiver, not to any propagation path -- and that common-mode structure is exactly
 * what the sync stack assumes when it estimates from the LOS tap and corrects the whole grid.
 *
 * @param cd            channel descriptor; may have its CIR GROWN to fit the shifted taps.
 * @param traj          handle from sensing_channel_make()/_parse().
 * @param sto_us        constant timing offset, microseconds.
 * @param cfo_hz        carrier frequency offset, Hz (a common phase rotation 2*pi*f*t).
 * @param sfo_ppm       sample-clock offset, ppm; the injected delay RAMPS at this rate.
 * @param wrap_samples  sawtooth limit for that ramp, samples. Models the coarse time-tracking loop a
 *                      real receiver runs, and is load-bearing: at 1 ppm / 61.44 Msps a free ramp
 *                      passes the 255-tap CIR cap in ~4 s and the whole scene goes dark. Set it
 *                      ABOVE the drift expected within one CPI to leave Phase 3 an unbroken ramp.
 *                      <=0 disables wrapping (warned about).
 * @return 0 on success (including the all-zero no-op case), -1 on a NULL argument.
 *
 * MEASURED LIMITATION -- READ BEFORE USING THIS TO VALIDATE PHASE 3 (2026-07-27). Injecting SFO here
 * does NOT deliver a delay ramp to the sensing pipeline, because the UE's OWN receive timing loop
 * tracks and removes it upstream of the ISAC CFR tap. Verified two independent ways on a 0.5 ppm /
 * 512-slot (256 ms) CPI run, where the ramp should present ~12.6 range bins of drift per CPI:
 *   - Phase 1's STO tracker reported `walk` of only 0.3-2.1 bins/CPI, not ~12.6.
 *   - Phase 3's own accepted-row delays had ~1.3-bin RMS scatter about a flat line (r2 ~ 0.00-0.01),
 *     i.e. clustered, not ramping; a genuine 12.6-bin ramp would show ~3.6-bin std and r2 ~ 0.99.
 * So `SFO_MIN_R_SQUARED` withholding the correction on 0% of CPIs here is the gate being CORRECT --
 * there genuinely is no residual linear drift left in the grid to correct. Do not "fix" that gate on
 * the strength of this harness. This is the same physical effect the wrap_samples comment above
 * describes (a real receiver's coarse loop removes whole-sample drift); it simply turns out to remove
 * essentially all of it here rather than leaving a sensing-grade residual.
 *
 * CFO is only PARTIALLY removed the same way (by --ue-fo-compensation), so cfo_hz does reach the
 * pipeline attenuated -- a 50 Hz injection was recovered as ~30 Hz -- which makes it usable as a
 * qualitative check but not as an absolute-accuracy one.
 *
 * To actually exercise Phases 1-3 end-to-end against known impairments, inject into the CFR grid
 * INSIDE the sensing engine (after the UE's timing/frequency loops, before the sync stack), not into
 * the rfsim channel. Not implemented; `tests/isac_sync_test.cc` already covers the estimators
 * directly on synthetic grids, which is why this was not pursued further.
 */
int sensing_channel_set_rx_clock(channel_desc_t *cd,
                                 void           *traj,
                                 double          sto_us,
                                 double          cfo_hz,
                                 double          sfo_ppm,
                                 double          wrap_samples);

/**
 * @brief Rebuild cd->ch[] from the object trajectories at this block's (midpoint) time.
 *
 * Called from update_channel_model() once per rfsimulator RX block when cd->sensing_traj != NULL.
 * @p nbSamples is the block length (used to evaluate the trajectory at the block midpoint);
 * @p TS is the block-start timestamp in samples.
 */
void sensing_channel_update(channel_desc_t *cd, int nbSamples, uint64_t TS);

/// Frees a sensing_traj_t previously returned by sensing_channel_parse().
void sensing_channel_free(void *traj);

#ifdef __cplusplus
}
#endif

#endif // SENSING_CHANNEL_H
