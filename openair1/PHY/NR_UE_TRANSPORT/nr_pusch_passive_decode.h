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
  NR_PUSCH_PASSIVE_ZERO_TB = 4,   ///< all-zero payload: CRC passes by construction, not a decode
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
  /// DM-RS CIR peak offset in samples, as nr_pusch_channel_estimation() measured it. This is the
  /// FFT-window error the applied timing advance did NOT remove, and it is the one number that says
  /// whether a failed decode is a timing problem: the receiver's own delay compensation spans only
  /// +/-MAX_DELAY_COMP (20) samples, so anything past that is uncorrected ISI.
  int      est_delay;
  /// est_delay measured on the FIRST pass, when the window was re-placed and the chain re-run.
  /// 0 when no refinement was needed. Kept separate so a refined grant is distinguishable.
  int      est_delay_pre;
  /// HARQ-ACK bit count that made the transport block decode, when the UCI reservation search
  /// found one. 0 = decoded with no reservation (no UCI, or O_ACK <= 2, which punctures instead).
  uint16_t uci_ack_re; ///< inferred ACK RE footprint; does not imply a known O_ACK/beta/alpha
  uint8_t  o_ack;
  int      n_segments;    ///< transport-block segments (C)
  int      segments_ok;   ///< of which the CRC passed. 0 vs C-1 are different failures.
  const char *reject_reason; ///< non-NULL when status != OK
} nr_pusch_passive_out_t;

/// Independent decode contexts. Each holds its own minimal PHY_VARS_gNB -- rxdataF ring, pusch_vars,
/// ULSCH HARQ and thread pool -- so two consumers never share one. Sized to the queue's consumer
/// cap; a caller decoding in-line uses context 0.
#define NR_PUSCH_PASSIVE_MAX_CTX 6

/**
 * @brief Decode one passively-observed PUSCH.
 *
 * @param ue                the passive UE (supplies frame_parms, rxdata, thread pool, LDPC iface)
 * @param ctx               decode-context index, < NR_PUSCH_PASSIVE_MAX_CTX. Every buffer the chain
 *                          writes lives in this context, so concurrent callers MUST pass distinct
 *                          values. 0 for the in-line path.
 * @param frame,slot        the slot the PUSCH occupies -- the DCI's slot PLUS k2, not the DCI slot
 * @param g                 the recovered UL grant
 * @param ta_offset_samples samples to advance the FFT window by (N_TA_offset + N_TA); 0 = none
 * @param abs_slot          CPI slow-time index for this PUSCH, captured on the RECEIVE thread.
 *                          Must not be derived here once deferred: a consumer reads a producer
 *                          counter that has moved on since these samples were taken. 0 = derive.
 * @param[out] out          filled unconditionally; check out->status
 */
/// Mode 2 (`pdcch_blind_monitor_ul_pusch = "2:.."`): estimate the channel and emit the CFR, but do
/// not run the LLR/LDPC half. `cfr_only` is that switch, plumbed rather than read from config here
/// so the decode stays a pure function of its arguments.
/// fo_hz is the frequency offset to de-rotate the FEP by (0.0 = none). It is a PARAMETER, not
/// read from ue inside, so a DEFERRED decode can pass the value that was live when its own samples
/// were captured -- reading ue->freq_offset here would use a newer one, which is exactly the hazard
/// that used to make nr_pusch_passive_queue refuse to start under --cont-fo-comp. See that
/// header's fo_hz field.
bool nr_pusch_passive_decode(PHY_VARS_NR_UE *ue,
                             int      ctx,
                             uint32_t frame,
                             uint8_t  slot,
                             const nr_pdcch_blind_ul_result_t *g,
                             int32_t  ta_offset_samples,
                             uint64_t abs_slot,
                             bool     cfr_only,
                             double   fo_hz,
                             nr_pusch_passive_out_t *out);

/* Actual production UL FFT, shared by immutable measurement diagnostics.
 * Positive sample_offset advances the window; fo_hz uses the receiver convention.
 * Output is before the symbol rotation applied by the full PUSCH decoder. */
void nr_pusch_passive_fep_symbol(const NR_DL_FRAME_PARMS *fp, const c16_t *rxdata, c16_t *rxdataF,
                                unsigned char symbol, unsigned char slot, int sample_offset, double fo_hz);

/// Release the minimal gNB context. Safe to call when it was never built.
void nr_pusch_passive_decode_free(void);

/// Periodic census: attempts / CRC pass / per-reason rejects. Printed by the caller's summary.
void nr_pusch_passive_stats_dump(void);

#ifdef __cplusplus
}
#endif

#include "PHY/NR_UE_TRANSPORT/nr_dmrs_id_estimate.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Read-only view of the UL DM-RS identity estimate for one nSCID (0 or 1; diagnostic, plain ints,
 * racy by design). */
const nr_dmrs_id_state_t *nr_pusch_passive_ul_dmrs_id(int nscid);
/* dataScramblingIdentityPUSCH sweep (Task 13), cell-wide (like the DM-RS estimate above -- this
 * deployment has one UL BWP, so there is nothing to key it by yet; unlike the DL side there is no
 * per-RNTI Technique D convergence signal to gate on, so nr_pusch_passive_ul_crc_stalled's own
 * try/ok counters (g_try/g_crc_ok) are the whole gate). `current()`/`feed()` follow the same
 * contract as the DL nr_pdsch_passive_data_id_* pair. */
uint16_t nr_pusch_passive_data_id_current(uint16_t pci, int dmrs_id, bool advance_ok);
void     nr_pusch_passive_data_id_feed(bool tb_crc_ok);
/* True when at least min_tries UL decodes have been attempted and NONE passed CRC. */
bool nr_pusch_passive_ul_crc_stalled(uint32_t min_tries);
#ifdef __cplusplus
}
#endif
#endif // NR_PUSCH_PASSIVE_DECODE_H
