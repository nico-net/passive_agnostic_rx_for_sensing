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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_passive_mac_ta.c
 * \brief See nr_passive_mac_ta.h. Parsers are pure; only nr_passive_mac_report_ta() logs, and it is
 *        defined in nr_pdsch_passive_queue.c so this file keeps no dependency at all.
 */

#include "nr_passive_mac_ta.h"
/* The three DL-SCH LCIDs this walk needs, from TS 38.321 table 6.2.1-1. Deliberately NOT included
 * from openair2's nr_mac.h: that header pulls in the generated ASN.1 tree, which would make this
 * parser -- pure byte handling with no MAC state -- undeployable in a standalone unit test. */
#define DL_SCH_LCID_DRX        0x3C
#define DL_SCH_LCID_TA_COMMAND 0x3D
#define DL_SCH_LCID_CON_RES_ID 0x3E
#define DL_SCH_LCID_PADDING    0x3F

#include <stdlib.h>

/* TS 38.321 6.1.5: RAR subheaders are one octet.
 *   E(1) T(1) R(2) BI(4)      when T = 0  -- backoff indicator, no RAR payload follows
 *   E(1) T(1) RAPID(6)        when T = 1  -- followed by the 7-octet RAR payload
 * TS 38.321 6.2.3, the payload:
 *   R(1) TA(12) UL-grant(27) TC-RNTI(16) = 56 bits = 7 octets. */
#define RAR_PAYLOAD_OCTETS 7

bool nr_passive_mac_rar_ta(const uint8_t *pdu, uint32_t len, uint8_t *rapid_out, uint16_t *ta_out,
                           uint16_t *tc_rnti_out)
{
  if (pdu == NULL || len < 1 + RAR_PAYLOAD_OCTETS) {
    return false;
  }
  /* VALIDATE THE WHOLE PDU BEFORE TRUSTING ANY FIELD. This parser is fed every CRC-OK transport
   * block whose DCI merely CLAIMED RA-RNTI, so a false-accepted DCI hands it arbitrary bytes.
   * MEASURED on 2000 random payloads: field-range checks alone accepted 35 %, and adding a padding
   * rule that only applied to E=0 still accepted 12 % (a random E=1 bit skipped it). Requiring the
   * SUBHEADER CHAIN to terminate and the tail to be zero padding is what actually discriminates. */
  uint32_t i = 0;
  int32_t first_rar = -1;
  bool terminated = false;
  while (i < len) {
    const uint8_t sub = pdu[i];
    const bool E = (sub & 0x80) != 0;
    const bool T = (sub & 0x40) != 0;
    if (!T) {
      if ((sub & 0x30) != 0) {
        return false; /* reserved bits of a backoff-indicator subheader */
      }
      i += 1;
    } else {
      if (i + 1 + RAR_PAYLOAD_OCTETS > len) {
        return false; /* truncated RAR */
      }
      /* EVERY RAR in the chain must be self-consistent, not just the one we report. A random
       * payload that happens to split into two back-to-back RARs filling the PDU exactly has no
       * padding to fail on -- measured at 2.7 % of random draws when only the first was checked. */
      const uint8_t *q = &pdu[i + 1];
      const uint16_t q_ta = (uint16_t)(((uint16_t)(q[0] & 0x7F) << 5) | ((q[1] >> 3) & 0x1F));
      const uint16_t q_rnti = (uint16_t)(((uint16_t)q[5] << 8) | q[6]);
      if ((q[0] & 0x80) != 0 || q_ta > 3846 || q_rnti == 0 || q_rnti > 0xFFEF) {
        return false;
      }
      if (first_rar < 0) {
        first_rar = (int32_t)i;
      }
      i += 1 + RAR_PAYLOAD_OCTETS;
    }
    if (!E) {
      terminated = true;
      break;
    }
  }
  if (!terminated || first_rar < 0) {
    return false;
  }
  /* Whatever follows the last subheader must be zero padding -- what a gNB emits when the TBS
   * exceeds the RAR. Trade-off, stated: a gNB padding with NON-zero bytes would be rejected here;
   * none observed, and inventing a UE that does not exist is the worse failure. */
  for (uint32_t k = i; k < len; k++) {
    if (pdu[k] != 0) {
      return false;
    }
  }
  const uint8_t *p = &pdu[(uint32_t)first_rar + 1];
  /* R(1) then TA(12): TA = p[0][6:0] << 5 | p[1][7:3]. Written out rather than bitfield-cast: a
   * packed bitfield's layout is compiler/endian-dependent, and these are bytes off the air. */
  const uint16_t ta = (uint16_t)(((uint16_t)(p[0] & 0x7F) << 5) | ((p[1] >> 3) & 0x1F));
  const uint16_t tc_rnti = (uint16_t)(((uint16_t)p[5] << 8) | p[6]);
  if ((p[0] & 0x80) != 0) {
    return false; /* payload's leading R bit is reserved-zero (TS 38.321 6.2.3) */
  }
  if (ta > 3846) {
    return false; /* TS 38.213 4.2 */
  }
  if (tc_rnti == 0 || tc_rnti > 0xFFEF) {
    return false; /* TS 38.321 7.1: assignable RNTIs stop at 0xFFEF */
  }
  if (rapid_out != NULL) {
    *rapid_out = (uint8_t)(pdu[(uint32_t)first_rar] & 0x3F);
  }
  if (ta_out != NULL) {
    *ta_out = ta;
  }
  if (tc_rnti_out != NULL) {
    *tc_rnti_out = tc_rnti;
  }
  return true;
}

/* TS 38.321 6.1.2 DL-SCH subheader: R(1) F(1) LCID(6), plus L (1 or 2 octets) for variable-size
 * elements. Fixed-size CEs and padding carry no L. Only the sizes needed to WALK the PDU are
 * listed; anything else ends the walk rather than guessing a length. */
static int dl_ce_fixed_len(uint8_t lcid)
{
  switch (lcid) {
    case DL_SCH_LCID_TA_COMMAND: return 1;                  /* TAG ID(2) + TA command(6) */
    case DL_SCH_LCID_CON_RES_ID: return 6;
    case DL_SCH_LCID_DRX:        return 0;
    default:                     return -1;                 /* not a fixed-size CE we know */
  }
}

bool nr_passive_mac_dlsch_ta(const uint8_t *pdu, uint32_t len, uint8_t *tag_id_out, uint8_t *ta_cmd_out)
{
  if (pdu == NULL || len < 2) {
    return false;
  }
  uint32_t i = 0;
  while (i < len) {
    const uint8_t sub = pdu[i];
    const uint8_t lcid = sub & 0x3F;
    const bool F = (sub & 0x40) != 0; /* 0 = 8-bit L, 1 = 16-bit L (variable-size elements only) */
    i += 1;
    if (lcid == DL_SCH_LCID_PADDING) {
      return false; /* padding runs to the end of the PDU */
    }
    if (lcid == DL_SCH_LCID_TA_COMMAND) {
      if (i >= len) {
        return false;
      }
      if (tag_id_out != NULL) {
        *tag_id_out = (uint8_t)((pdu[i] >> 6) & 0x03);
      }
      if (ta_cmd_out != NULL) {
        *ta_cmd_out = (uint8_t)(pdu[i] & 0x3F);
      }
      return true;
    }
    const int fixed = dl_ce_fixed_len(lcid);
    if (fixed >= 0) {
      i += (uint32_t)fixed;
      continue;
    }
    /* A logical channel (or an unknown CE): its length field tells us how far to skip. */
    if (lcid > 32) {
      return false; /* unknown CE of unknown size: stop rather than walk into noise */
    }
    uint32_t l;
    if (F) {
      if (i + 2 > len) {
        return false;
      }
      l = ((uint32_t)pdu[i] << 8) | pdu[i + 1];
      i += 2;
    } else {
      if (i + 1 > len) {
        return false;
      }
      l = pdu[i];
      i += 1;
    }
    if (l > len - i) {
      return false; /* length runs past the PDU: this is not a valid MAC PDU */
    }
    i += l;
  }
  return false;
}

double nr_passive_mac_ta_metres(uint16_t ta_absolute, int mu)
{
  if (mu < 0 || mu > 4) {
    return -1.0;
  }
  /* N_TA = T_A * 16 * 64 / 2^mu, in units of T_c = 1 / (480000 * 4096) s. The advance is the ROUND
   * trip, so the one-way range is half of it. */
  const double Tc = 1.0 / (480000.0 * 4096.0);
  const double n_ta = (double)ta_absolute * 16.0 * 64.0 / (double)(1u << mu);
  return 299792458.0 * n_ta * Tc / 2.0;
}
