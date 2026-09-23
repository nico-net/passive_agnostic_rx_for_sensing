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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_tdd_pattern.h
 * \brief Slot direction from tdd-UL-DL-ConfigurationCommon.
 *
 * WHY. The passive receiver currently monitors every slot for PDCCH. On a TDD cell a large share
 * of them carry no downlink at all, so that work is spent scanning uplink slots -- pure CPU cost on
 * a receiver already measured at ~881 us per grant against a 500 us slot, AND a pure source of
 * false accepts, since a blind search over noise still occasionally produces an in-range RNTI.
 *
 * WHY IT IS DERIVABLE, unlike most of the dedicated configuration. tdd-UL-DL-ConfigurationCommon
 * is carried in SIB1, which this receiver already decodes on its way to CELL_CONFIGURED. Nothing
 * has to be guessed or swept; it only has to be read and applied. That is the whole reason this is
 * worth doing before the harder agnosticity items.
 *
 * The model is TS 38.213 11.1: one or two patterns, each a periodicity followed by
 * nrofDownlinkSlots whole DL slots, a mixed slot with nrofDownlinkSymbols at the start and
 * nrofUplinkSymbols at the end, then nrofUplinkSlots whole UL slots. Pattern 2, when present,
 * simply follows pattern 1 and the pair repeats.
 */

#ifndef __NR_TDD_PATTERN_H__
#define __NR_TDD_PATTERN_H__

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  NR_TDD_SLOT_DL = 0,   ///< every symbol downlink
  NR_TDD_SLOT_UL,       ///< every symbol uplink
  NR_TDD_SLOT_MIXED,    ///< DL symbols, then a guard, then UL symbols
} nr_tdd_slot_dir_t;

/// One pattern, exactly as tdd-UL-DL-ConfigurationCommon carries it.
typedef struct {
  uint16_t period_slots;    ///< dl-UL-TransmissionPeriodicity converted to slots at this SCS
  uint8_t  dl_slots;        ///< nrofDownlinkSlots
  uint8_t  dl_symbols;      ///< nrofDownlinkSymbols (in the mixed slot)
  uint8_t  ul_slots;        ///< nrofUplinkSlots
  uint8_t  ul_symbols;      ///< nrofUplinkSymbols (in the mixed slot)
} nr_tdd_pattern_t;

typedef struct {
  nr_tdd_pattern_t p1;
  nr_tdd_pattern_t p2;      ///< period_slots == 0 when absent
  bool valid;
} nr_tdd_config_t;

/** Periodicity in slots. `periodicity_x10_ms` is the value in tenths of a millisecond, so the
 * 0.5 / 0.625 / 1.25 / 2.5 ms entries stay exact in integers -- expressing them in whole
 * milliseconds would silently truncate three of the seven legal values to zero.
 * Returns 0 if the combination does not give a whole number of slots, which is not a legal
 * configuration and must not be rounded into one. */
uint16_t nr_tdd_period_slots(uint16_t periodicity_x10_ms, uint8_t mu);

/** Build a config, validating that the pattern fits its own periodicity. Returns false and leaves
 * `out->valid` false when it does not -- a pattern claiming more slots than its period would
 * otherwise alias and mark uplink slots as downlink. */
bool nr_tdd_config_init(nr_tdd_config_t *out, const nr_tdd_pattern_t *p1, const nr_tdd_pattern_t *p2);

/** Direction of an absolute slot. */
nr_tdd_slot_dir_t nr_tdd_slot_direction(const nr_tdd_config_t *cfg, uint32_t absolute_slot);

/** True if the slot can carry PDCCH -- i.e. it has at least one downlink symbol. A mixed slot
 * counts: the CORESET sits at the start of the slot, which is the downlink part. */
bool nr_tdd_slot_has_downlink(const nr_tdd_config_t *cfg, uint32_t absolute_slot);

#endif /* __NR_TDD_PATTERN_H__ */
