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

/* The 0_1 set is far smaller than 1_1's (type-1 stage <= 462, armed set <= 1830 over BWP 6..273 and
 * 28..62 bits), so its enumeration keeps the old cap even though the shared resolver type is larger. */
#define NR_DCI01_LAYOUT_MAX 8192

/* ---- DCI 0_1 FDRA MODE STAGING -----------------------------------------------------------------
 * Stage 1 cannot refute type 1 on a type-0 cell (a type-1 window that starts on constant-zero leading
 * bits always reads an in-range RIV: 144/208 type-1 layouts survive in the test), so the oracle is the TB
 * CRC of PUSCH decoded under the type-1 read -- but ONLY of DCI 0_1 grants read under converged,
 * non-discovery widths (the "oracle" class). A DCI 0_0 grant is always type 1, and a UL-discovery grant
 * is read under a width HYPOTHESIS: their CRC passes say the link works (LINK HEALTH), never that the
 * 0_1 FDRA is a RIV. Refusal requires link health, because on a dead link every read fails. */
typedef struct {
  uint32_t t1_try, t1_ok;      ///< oracle-class 0_1 TBs decoded / passed
  uint32_t link_ok;            ///< CRC passes on every other UL grant (0_0, discovery hypotheses)
  uint32_t t1_try_at_link;     ///< t1_try when the last link pass arrived
} nr_dci01_fdra_evidence_t;

/** True for a grant in the oracle class: format 0_1, no width or interpretation hypothesis owns it. */
static inline bool nr_dci01_fdra_oracle_grant(int ul_dci_format, int width_hyp_class, int interp_hyp_class)
{
  return ul_dci_format == 0 /* NR_BLIND_UL_DCI_FORMAT_0_1 */ && width_hyp_class < 0 && interp_hyp_class < 0;
}
/** Record one decoded UL TB. Not thread-safe: the caller serialises. */
void nr_dci01_fdra_note(nr_dci01_fdra_evidence_t *e, bool oracle_grant, bool tb_crc_ok);

/**   NR_DCI01_FDRA_BOOK   -- book everything: an oracle 0_1 TB has passed (type 1 is proven, permanently),
 *                           or type 1 is not refuted yet, or the link is not known to be healthy
 *   NR_DCI01_FDRA_ARM    -- not armed, link healthy and oracle 0/NR_DCI11_FDRA_ARM_MIN_TRIALS: append the
 *                           type 0 / dynamicSwitch layouts, then ask again
 *   NR_DCI01_FDRA_REFUSE -- armed, link healthy, oracle still 0 passes and a non-type-1 layout alive
 * LINK HEALTHY = a link pass arrived during the last NR_DCI11_FDRA_ARM_MIN_TRIALS oracle trials, i.e. the
 * link demonstrably worked while the type-1 reads were failing. Pure. */
enum { NR_DCI01_FDRA_BOOK = 0, NR_DCI01_FDRA_ARM = 1, NR_DCI01_FDRA_REFUSE = 2 };
int nr_dci01_fdra_verdict(const nr_dci01_fdra_evidence_t *e, bool armed, const nr_dci11_resolver_t *r);

/** Whether to book one UL grant under a verdict. REFUSE applies to oracle-class 0_1 grants only (0_0 and
 * discovery grants are always booked, so their UL DM-RS CFR and the discovery search continue), and one
 * refused grant in NR_DCI01_FDRA_PROBE_EVERY is still booked so a type-1 pass can end the refusal.
 * `refused_so_far` counts refused oracle grants including this one. `enforce` false (the DEFAULT, final
 * review I3): a REFUSE verdict is only counted/logged as would-refuse and the grant is booked -- "0_1
 * type-1 PUSCH 0/64 while 0_0 passes" is also exactly the signature of a 0_1-only data-scrambling-ID or
 * MCS limit, which refusing would hide rather than fix. Pure. */
#define NR_DCI01_FDRA_PROBE_EVERY 64
bool nr_dci01_fdra_book(int verdict, bool oracle_grant, unsigned long refused_so_far, bool enforce);
/** ISAC_UL_FDRA_REFUSE=1 enables the refusal (read once). Default off. */
bool nr_dci01_fdra_refuse_enforced(void);

#endif /* __NR_PDCCH_DCI01_LAYOUT_SWEEP_H__ */
