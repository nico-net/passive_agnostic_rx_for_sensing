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

/*! \file openair1/PHY/NR_UE_ISAC/nr_isac.h
 * \brief Public C API of the OAI-UE ISAC / passive-radar sensing pipeline.
 *
 * This is a faithful port of the srsUE sensing pipeline (repos/srs-ue-isac-dmd,
 * srsue/src/phy/nr/sensing/) into the OAI UE. The real-time NR DL procedures tap
 * the per-RE channel-frequency-response (Ĥ = Y/X) at reference REs (CSI-RS LS, PDSCH
 * DM-RS, or data-aided full-allocation estimate) and hand it, best-effort and off the
 * RT path, to a dedicated engine that turns the reused CFR into a range-velocity map
 * (clutter removal -> range IFFT -> Doppler FFT -> 2D CA-CFAR -> NMS) and emits
 * DetectionReport JSON-lines / a ZeroMQ PUB bus compliant with the central node
 * (repos/isac crates/isac-core/src/report.rs).
 *
 * Only plain-C types cross this boundary so the header is includable from the C PHY
 * procedure files (phy_procedures_nr_ue.c, csi_rx.c); the engine internals are C++.
 */

#ifndef NR_ISAC_H
#define NR_ISAC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Downlink reference used as the sensing illuminator (mirrors srsUE sensing_source_t).
typedef enum nr_isac_source_e {
  NR_ISAC_SRC_CSI_RS = 0,     ///< NZP-CSI-RS LS estimates (periodic, low PRF)
  NR_ISAC_SRC_PDSCH_DMRS = 1, ///< PDSCH DM-RS positions (comb-2, one per scheduled DL slot -> high PRF)
  NR_ISAC_SRC_PDSCH_DATA = 2, ///< Full-allocation interpolated CFR (dense comb-1 -> finest range resolution)
  NR_ISAC_SRC_COUNT      = 3  ///< Number of distinct sources (for the enabled-set bitmask)
} nr_isac_source_t;

/// Carrier geometry valid for one submitted CFR snapshot. All axis scaling derives from this.
typedef struct nr_isac_carrier_s {
  uint32_t nof_prb;         ///< PRBs spanned by the submitted lattice (subcarrier grid = nof_prb*12)
  uint32_t scs_hz;          ///< Subcarrier spacing (Hz)
  uint64_t dl_center_hz;    ///< Carrier centre frequency (Hz)
  uint16_t pci;             ///< Physical cell id (observed)
  uint16_t slots_per_frame; ///< Slots per 10 ms radio frame at this numerology
} nr_isac_carrier_t;

/**
 * @brief Initialise the sensing pipeline from the OAI "sensing" config section.
 *
 * Reads the [sensing] parameters (enable, source, cpi_slots, CFAR/NMS, geometry, report
 * sinks, ...) via the OAI config module. A no-op leaving the pipeline disabled when
 * sensing.enable is not set. Safe to call once at UE start-up before the workers run.
 */
void nr_isac_init(void);

/// Launch the engine consumer thread. Call once after nr_isac_init(), before DL processing.
void nr_isac_start(void);

/// Stop the engine thread, flush the final CPI accumulator and close the report sinks.
void nr_isac_stop(void);

/// Non-zero when sensing is enabled (master switch). Cheap; safe on the RT path.
int nr_isac_enabled(void);

/// Primary reference source (nr_isac_source_t): the lowest-numbered enabled source. Valid only
/// when nr_isac_enabled(). Kept for callers that need a single representative source.
int nr_isac_source(void);

/**
 * @brief Non-zero when @p source (an nr_isac_source_t) is in the enabled set (sensing.sources).
 *
 * The RT taps gate on this rather than on a single selected source, so several sources can feed
 * one fused CFR grid. Cheap (a bitmask test); safe on the RT path.
 */
int nr_isac_source_enabled(int source);

/**
 * @brief Real-time tap: submit one per-slot CFR row (one slow-time sample) to the engine.
 *
 * Best-effort: if no snapshot buffer is free the occurrence is dropped (sensing never
 * blocks or allocates on the RT path). @p h is interleaved float [re0,im0,re1,im1,...] of
 * length 2*nof_re; @p k_abs / @p l_sym give the absolute subcarrier and OFDM symbol index of
 * each RE. The comb spacing is inferred from the smallest positive subcarrier gap.
 *
 * @p source (an nr_isac_source_t) tags which reference produced this row so the engine can fuse
 * several sources onto one grid and report per-source diagnostics. For fusion to be coherent, all
 * sources MUST submit @p k_abs on the same absolute subcarrier axis (relative to CRB0 / point A)
 * and the same full-carrier @p carrier.nof_prb.
 *
 * @p noise_var is this estimate's per-RE noise power (linear, same amplitude units as @p h). When
 * two sources land on the same subcarrier in the same slot, the engine fuses them by inverse-variance
 * weighting (ĥ = Σ ĥ_i/σ²_i / Σ 1/σ²_i) instead of last-write-wins, so the lower-noise estimate
 * dominates. Pass 0 (or a negative value) if unknown -> the engine falls back to equal weighting.
 */
void nr_isac_submit_cfr(uint32_t                 slot_idx,
                        int                      source,
                        const nr_isac_carrier_t* carrier,
                        const float*             h,
                        const uint32_t*          k_abs,
                        const uint32_t*          l_sym,
                        uint32_t                 nof_re,
                        float                    noise_var);

/**
 * @brief As nr_isac_submit_cfr(), but places the estimate at a SUB-SLOT slow-time position.
 *
 * @p slot_frac is the offset within the slot in slots, i.e. [0,1) -- typically
 * (group centre symbol + 0.5)/symbols_per_slot. Submissions sharing the same (slot_idx, slot_frac)
 * still merge into one slow-time row (multi-source fusion); different fractions open separate rows,
 * which is what raises the effective PRF. nr_isac_submit_cfr() is exactly this with slot_frac = 0,
 * so existing callers are unaffected. See defs_nr_UE_ISAC.h's sub-slot section for why this exists.
 */
void nr_isac_submit_cfr_at(uint32_t                 slot_idx,
                           float                    slot_frac,
                           int                      source,
                           const nr_isac_carrier_t* carrier,
                           const float*             h,
                           const uint32_t*          k_abs,
                           const uint32_t*          l_sym,
                           uint32_t                 nof_re,
                           float                    noise_var);

/**
 * @brief Sub-slot sampling configuration for the RT taps (see defs_nr_UE_ISAC.h).
 * Returns the target symbols per row (0 = disabled) and fills the sparsity / SNR gates.
 */
uint32_t nr_isac_subslot_config(uint32_t* min_re, float* min_snr_db);

#ifdef __cplusplus
}
#endif

#endif // NR_ISAC_H
