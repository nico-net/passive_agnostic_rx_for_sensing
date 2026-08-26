/*
 * Passive PUSCH receive path.
 *
 * The downlink twin of this file (nr_pdsch_passive_decode.{h,c}) decodes another UE's PDSCH by
 * reusing the UE's own receive chain. That trick does NOT work for uplink: a UE has no PUSCH
 * receiver at all. The receiver for PUSCH is the gNB's -- nr_rx_pusch_group_tp() ->
 * nr_ulsch_decoding(), with nr_pusch_channel_estimation() underneath -- and those live in PHY_NR,
 * which nr-uesoftmodem does not link. Hence the PHY_NR_PASSIVE_UL library added alongside this
 * file, and hence the minimal PHY_VARS_gNB this file constructs: the gNB receive chain is reused
 * verbatim, it is simply given a context built by hand instead of by phy_init_nr_gNB().
 *
 * TWO THINGS ARE FUNDAMENTALLY DIFFERENT FROM THE DL CASE and both are handled here:
 *
 *  1. The grant does not schedule this slot. A DL DCI's PDSCH is in the same slot; an UL DCI's
 *     PUSCH is k2 slots later. The caller is responsible for presenting this function with the
 *     grant in its TARGET slot -- see nr_pusch_passive_monitor_rt.{h,c}.
 *
 *  2. The uplink is transmitted EARLY. A UE advances its transmission by N_TA_offset + N_TA so it
 *     lands aligned at the gNB, and a passive receiver synced to the downlink therefore sees the
 *     PUSCH ahead of its own slot boundary -- roughly one 30 kHz symbol of N_TA_offset in FR1,
 *     plus a per-UE N_TA that appears in no DCI. `ta_offset_samples` is that correction, applied
 *     to the FFT window. Getting it wrong does not fail loudly; it degrades the channel estimate
 *     into something that looks like a bad channel.
 */

#ifndef NR_PUSCH_PASSIVE_DECODE_H
#define NR_PUSCH_PASSIVE_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#include "PHY/defs_nr_UE.h"
#include "nr_pdcch_blind_monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  NR_PUSCH_PASSIVE_OK = 0,        ///< transport block decoded, CRC passed
  NR_PUSCH_PASSIVE_CRC_FAIL = 1,  ///< decoded, CRC failed
  NR_PUSCH_PASSIVE_UNSUPPORTED = 2, ///< grant outside this receiver's scope; nothing attempted
  NR_PUSCH_PASSIVE_ERROR = 3,     ///< setup/allocation failure
} nr_pusch_passive_status_t;

typedef struct {
  uint8_t  status;        ///< nr_pusch_passive_status_t
  uint32_t tbs_bytes;     ///< transport block size actually decoded
  const uint8_t *tb;      ///< decoded TB, valid only while the next call has not run; NULL unless OK
  uint32_t G;             ///< coded bits, for the data-aided reconstruction that follows
  uint8_t  qam_mod_order;
  uint16_t nb_rb;
  uint8_t  nb_symbols;
  float    snr_db;        ///< post-estimation SNR, per this path's own scale
  const char *reject_reason; ///< non-NULL when status != OK
} nr_pusch_passive_out_t;

/**
 * @brief Decode one passively-observed PUSCH.
 *
 * @param ue                the passive UE (supplies frame_parms, rxdata, thread pool, LDPC iface)
 * @param frame,slot        the slot the PUSCH occupies -- the DCI's slot PLUS k2, not the DCI slot
 * @param g                 the recovered UL grant
 * @param ta_offset_samples samples to advance the FFT window by (N_TA_offset + N_TA); 0 = none
 * @param[out] out          filled unconditionally; check out->status
 */
bool nr_pusch_passive_decode(PHY_VARS_NR_UE *ue,
                             uint32_t frame,
                             uint8_t  slot,
                             const nr_pdcch_blind_ul_result_t *g,
                             int32_t  ta_offset_samples,
                             nr_pusch_passive_out_t *out);

/// Release the minimal gNB context. Safe to call when it was never built.
void nr_pusch_passive_decode_free(void);

/// Periodic census: attempts / CRC pass / per-reason rejects. Printed by the caller's summary.
void nr_pusch_passive_stats_dump(void);

#ifdef __cplusplus
}
#endif

#endif // NR_PUSCH_PASSIVE_DECODE_H
