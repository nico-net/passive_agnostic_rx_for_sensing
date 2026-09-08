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

#ifndef NR_PDSCH_ADAPTIVE_CONFIG_H
#define NR_PDSCH_ADAPTIVE_CONFIG_H

#include "nr_pdsch_config_sweep.h"
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"

/* Keep allocation, decoder MCS and limited-buffer rate matching in one contract.
 * Only dedicated format 1_1 callers may apply this; common-format rules stay unchanged. */
static inline void nr_pdsch_adaptive_apply(const nr_pdsch_cfg_hypothesis_t *h,
                                          fapi_nr_dl_config_dlsch_pdu_rel15_t *pdu,
                                          uint8_t *grant_mcs_table, int8_t *grant_mcs_table_lbrm)
{
  pdu->start_symbol = h->tda_start;
  pdu->number_symbols = h->tda_length;
  pdu->dlDmrsSymbPos = h->dmrs_mask;
  pdu->mcs_table = *grant_mcs_table = h->mcs_table;
  *grant_mcs_table_lbrm = (int8_t)h->mcs_table;
}

#endif
