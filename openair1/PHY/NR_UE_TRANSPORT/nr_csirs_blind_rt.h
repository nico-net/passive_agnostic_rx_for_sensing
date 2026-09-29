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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.h
 * \brief Live tap for the blind CSI-RS search: score ONE candidate per slot against rxdataF.
 *
 * Deliberately one candidate per slot, not a sweep inside the slot. Generating a reference grid
 * costs a full nr_generate_csi_rs() call, and this receiver is already measured at ~881 us of
 * decode against a 500 us slot -- adding a per-slot inner loop over hundreds of candidates would
 * turn a search into the thing that breaks the receiver it runs on. One per slot converges in
 * minutes, which is far below the dwell of any capture.
 *
 * Observe-only: it reads rxdataF and writes nothing the decoder consumes, so it cannot change
 * decoding. Enabled by ISAC_CSIRS_BLIND=1, default off.
 */

#ifndef __NR_CSIRS_BLIND_RT_H__
#define __NR_CSIRS_BLIND_RT_H__

#include "PHY/defs_nr_UE.h"

/** Score one candidate against this slot. Safe to call every slot; no-ops unless enabled.
 * `rxdataF` is the frequency-domain slot buffer the monitor already holds. */
/* `rxdataF` is the monitor's per-antenna slot buffer; only the CORESET symbols are populated when
 * this is called, so the candidate's own symbol is FFT'd here (antenna 0, one symbol per slot). */
void nr_csirs_blind_rt_slot(PHY_VARS_NR_UE *ue, int slot, uint32_t absolute_slot,
                            c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP]);

#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"
/** Every confirmed cell CSI-RS resource occurring in `absolute_slot`, as FAPI PDUs for PDSCH rate
 *  matching: NZP (csi_type 1) first, then ZERO-POWER (csi_type 2, REs to rate-match around, nothing
 *  to estimate on). Returns how many were written to out[0..max). */
int nr_csirs_blind_rt_rate_match_all(uint32_t absolute_slot, fapi_nr_dl_config_csirs_pdu_rel15_t *out, int max);
/** Decoded-grant evidence on a ZP (csi_type 2) rate-matching entry this module handed out: the grant decoded
 *  for PDSCH slot @p pdsch_absolute_slot scored @p score = nr_csirs_blind_zp_grant_score() on its own PRBs.
 *  Thread-safe (the PDSCH decode consumers call it); queued and applied by the next nr_csirs_blind_rt_slot(),
 *  which owns the search state. Entries that match no exported ZP geometry are dropped there. */
void nr_csirs_blind_rt_zp_grant_evidence(uint32_t pdsch_absolute_slot, const fapi_nr_dl_config_csirs_pdu_rel15_t *zp,
                                         double score);
#endif /* __NR_CSIRS_BLIND_RT_H__ */
