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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_passive_mac_ta.h
 * \brief Timing advance recovered from OVERHEARD MAC PDUs (TS 38.321 6.1.3.4, 6.2.3).
 *
 * The passive receiver already produces CRC-verified transport blocks for grants addressed to other
 * UEs, and then throws the payload away. Two MAC elements inside those payloads carry the gNB's own
 * timing-advance command, which is the round-trip propagation delay to THAT UE -- i.e. a direct
 * range measurement against the illuminator, obtained with no transmission of our own:
 *
 *   - the RAR (Msg2, addressed to RA-RNTI): a 12-bit T_A, the INITIAL advance, so it is an absolute
 *     range and it also marks the moment a new UE appears;
 *   - the Timing Advance Command MAC CE (LCID 0x3D) in a dedicated DL-SCH PDU: a 6-bit relative
 *     update around 31, i.e. a range DELTA for a UE already connected.
 *
 * Both parsers are pure: bytes in, fields out, no PHY or MAC state, so they are unit-testable and
 * cannot disturb the receive path. Range conversion is exact per TS 38.213 4.2 / 38.211 4.1:
 * N_TA = T_A * 16 * 64 / 2^mu samples of T_c = 1/(480000 * 4096) s, one-way distance = c * N_TA*T_c / 2.
 */

#ifndef __NR_PASSIVE_MAC_TA_H__
#define __NR_PASSIVE_MAC_TA_H__

#include <stdbool.h>
#include <stdint.h>

/** First RAR in an overheard Msg2. Walks the E/T/R/R/BI and E/T/RAPID subheaders of TS 38.321 6.1.5.
 * Returns false when the PDU is not a self-consistent RAR (which is the common case: this is called
 * on payloads whose RNTI class is only a hypothesis). */
bool nr_passive_mac_rar_ta(const uint8_t *pdu, uint32_t len, uint8_t *rapid_out, uint16_t *ta_out,
                           uint16_t *tc_rnti_out);

/** Timing Advance Command MAC CE (LCID 0x3D) in a DL-SCH PDU, if present. Walks the R/F/LCID(/L)
 * subheaders of TS 38.321 6.1.2. Returns false when the PDU carries no TA CE. */
bool nr_passive_mac_dlsch_ta(const uint8_t *pdu, uint32_t len, uint8_t *tag_id_out, uint8_t *ta_cmd_out);

/** One-way range in metres for an ABSOLUTE advance (RAR's 12-bit T_A) at numerology `mu`. */
double nr_passive_mac_ta_metres(uint16_t ta_absolute, int mu);

/** Try both parsers on one CRC-verified transport block and log whatever it turns out to carry.
 * Silent when the PDU holds no timing advance, which is the common case.
 *
 * DEFINED IN nr_pdsch_passive_queue.c, deliberately not here: this file stays free of log.h and of
 * every other dependency so the parsers above can be unit-tested on their own -- which matters,
 * because they run on payloads whose RNTI class is only a hypothesis. Declared here anyway because
 * BOTH passive decode paths (deferred consumer and in-line receive thread) must report identically,
 * and one implementation is the only way that stays true. */
void nr_passive_mac_report_ta(uint16_t rnti, bool is_ra_rnti, int frame, int slot, int mu,
                              const uint8_t *tb, uint32_t tb_bytes);

#endif /* __NR_PASSIVE_MAC_TA_H__ */
