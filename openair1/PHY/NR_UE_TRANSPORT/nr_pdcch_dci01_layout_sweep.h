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
 */

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci01_layout_sweep.h
 * \brief The DCI 0_1 (uplink grant) counterpart of the DCI 1_1 layout sweep.
 *
 * STATE THIS CLOSES. `pdcch_blind_monitor_ul_*` widths are all -1, meaning "this module's
 * documented assumption" -- exactly where DCI 1_1 stood before nr_pdcch_dci11_layout_sweep.c.
 *
 * A NOTE ON WHAT WAS BELIEVED HERE. A long-standing record said UL 0_1 "accepts ZERO despite the
 * gNB sending it". That is SUPERSEDED: re-measured on captures/sfooff_r1_191752 the monitor shows
 * dci01[accepts=23933] with PUSCHDIAG decoding real grants partially (seg=3/7, SNR 17.9-55 dB).
 * So the uplink is not a dead path needing an investigation; it is a working path needing the same
 * layout derivation the downlink just got.
 *
 * DCI 0_1's field order is TS 38.212 7.3.1.1.2, and it differs from 1_1 in ways that matter: a
 * UL/SUL indicator and a frequency-hopping flag appear BEFORE the MCS, and SRS resource
 * indication, precoding-and-layers, CSI request, PT-RS/DM-RS association and beta-offset all sit
 * between the RV and the antenna ports. The group sums therefore cover different fields -- but the
 * fields extraction reads are the same five, so nr_dci11_offsets_t and the whole two-stage
 * resolver are reused rather than duplicated.
 */

#ifndef __NR_PDCCH_DCI01_LAYOUT_SWEEP_H__
#define __NR_PDCCH_DCI01_LAYOUT_SWEEP_H__

#include <stdbool.h>
#include <stdint.h>

#include "nr_pdcch_dci11_layout_sweep.h"   /* nr_dci11_offsets_t + the shared resolver */

/// One DCI 0_1 layout hypothesis, again in the only terms extraction can distinguish.
typedef struct {
  uint8_t pre_riv;    ///< ul_sul indicator + bwp indicator            (0..3)
  uint8_t pre_mcs;    ///< frequency hopping flag                      (0..1)
  uint8_t pre_ant;    ///< harq pid + dai1 + dai2 + tpc + sri + precoding + csi request
  uint8_t ant_ports;  ///< 2..5 (DM-RS type x maxLength, transform precoding)
  uint8_t post_ant;   ///< srs request + cbg + ptrs-dmrs + beta offset + dmrs seq init
  uint8_t fdra_mode;  ///< NR_FDRA_* (PUSCH resourceAllocation x rbg-Size); 0 = type 1
  uint8_t n_rbg;      ///< N_RBG of that RBG configuration (TS 38.214 Table 6.1.2.2.1-1); 0 for type 1
} nr_dci01_layout_t;

/** Offsets implied by a layout, in the SHARED struct so the DCI 1_1 resolver can score it.
 * `ul_sch_indicator` (1 bit) closes the payload and is included in `total`. */
bool nr_dci01_layout_offsets(const nr_dci01_layout_t *l, uint16_t riv_bits, uint8_t tda_bits,
                             nr_dci11_offsets_t *out);

/** Every layout whose total width equals `observed_len`. Returns the count, or -1 on bad
 * arguments. Fills the parallel `offsets` array when non-NULL, ready for
 * nr_dci_resolver_init_from_offsets(). */
int nr_dci01_layout_enumerate(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len,
                              nr_dci01_layout_t *out, nr_dci11_offsets_t *offsets, int max);

/** One FDRA mode's layouts (same rules as nr_dci11_layout_enumerate_mode(): 0 for a duplicate rbg-Size
 * config 2), and every mode in NR_FDRA_* order. The RT receiver searches type 1 first and appends the
 * others only once the booked type-1 PUSCH has failed its TB CRC (see nr_pdcch_blind_monitor_rt.c). */
int nr_dci01_layout_enumerate_mode(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len, uint16_t bwp_start,
                                   uint16_t bwp_size, uint8_t fdra_mode, nr_dci01_layout_t *out,
                                   nr_dci11_offsets_t *offsets, int max);
int nr_dci01_layout_enumerate_fdra(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len, uint16_t bwp_start,
                                   uint16_t bwp_size, nr_dci01_layout_t *out, nr_dci11_offsets_t *offsets, int max);

/** DCI 0_1 FDRA MODE STAGING verdict. Stage 1 cannot refute type 1 on a type-0 cell (a type-1 window that
 * starts on constant-zero leading bits always reads an in-range RIV), so the oracle is the TB CRC of the
 * PUSCH booked under the type-1 read: tb_try booked TBs, tb_ok passes.
 *   NR_DCI01_FDRA_BOOK   -- book UL grants (type 1 not refuted, or a type-1 TB has passed: type 1 is right)
 *   NR_DCI01_FDRA_ARM    -- not armed yet and 0 passes over >= NR_DCI11_FDRA_ARM_MIN_TRIALS: append the
 *                           type 0 / dynamicSwitch layouts (nr_dci01_layout_enumerate_mode +
 *                           nr_dci_resolver_append_offsets), then ask again
 *   NR_DCI01_FDRA_REFUSE -- armed, still 0 passes, and a non-type-1 layout alive: a non-type-1 FDRA is the
 *                           leading explanation, so the type-1 read would decode the grant wrong. Pure. */
enum { NR_DCI01_FDRA_BOOK = 0, NR_DCI01_FDRA_ARM = 1, NR_DCI01_FDRA_REFUSE = 2 };
int nr_dci01_fdra_verdict(uint32_t tb_try, uint32_t tb_ok, bool armed, const nr_dci11_resolver_t *r);

#endif /* __NR_PDCCH_DCI01_LAYOUT_SWEEP_H__ */
