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

/** Hand one slot to the blind CSI-RS search's OWN consumer (bounded ring, drop-oldest, priority 40
 *  -- see the consumer's comment in nr_csirs_blind_rt.c). Never blocks; the PDCCH scan consumer
 *  calls this instead of nr_csirs_blind_rt_slot() so the search can never delay a PDCCH occasion.
 *  absolute_slot_mono is the producer's monotonic slot counter for the staleness test; fo_hz the
 *  frequency offset sampled with these samples (replayed via nr_slot_fep_fo_override_hz). */
void nr_csirs_blind_rt_enqueue(PHY_VARS_NR_UE *ue, int slot, uint32_t absolute_slot, long absolute_slot_mono,
                               double fo_hz);

#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"
/** The confirmed cell CSI-RS resource as a FAPI PDU for PDSCH rate matching, if the blind search has
 *  converged and the resource occurs in `absolute_slot`. Returns true and fills *out; else false. */
bool nr_csirs_blind_rt_rate_match(uint32_t absolute_slot, fapi_nr_dl_config_csirs_pdu_rel15_t *out);
/** Same for the confirmed ZERO-POWER resource (csi_type 2): REs to rate-match around, nothing to
 *  estimate on. */
bool nr_csirs_blind_rt_rate_match_zp(uint32_t absolute_slot, fapi_nr_dl_config_csirs_pdu_rel15_t *out);
#endif /* __NR_CSIRS_BLIND_RT_H__ */
