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
