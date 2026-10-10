/* TIM-PDSCH (2026-10-10): decode-free blind PDSCH DM-RS identity scan over the FULL 16-bit space, per UE.
 *
 * WHY. On PCI 923 (TIM) every UE has its own scramblingID0 (measured on the capture: 0x4e32 -> 1677, 0x5954 -> 272,
 * 0x5098 -> 638), all above the 0..1023 window the per-CORESET estimator searches, and the data scrambling identity is
 * the PCI (dataScramblingIdentityPDSCH absent): a single per-CORESET identity can never be right there, and the
 * 0..1023 estimate "decided" a noise maximum (88, ~10 dB over the median of 1024 random walks).
 *
 * WHAT. (1) nr_dmrs_scan_slot(): on one type-A DM-RS symbol of a slot, find the RB segments that carry energy well above
 * the slot's floor, cut them into chunks of <= NR_DMRS_SCAN_CHUNK RBs and, per chunk, score every (N_ID 0..65535,
 * n_SCID 0/1) by the same-k' pilot coherence |sum_m P_m conj(r_{m+2}) r_m| / sum_m |P_m|, P_m = sum_ant y(m+2) y(m)^*
 * (type 1, CDM group 0; pilots m and m+2 share k' and therefore the port-1000/1001 OCC sign, so the score stays ~1 with
 * two ports in the CDM group, where the adjacent-pilot score of nr_dmrs_id_estimate collapses). The pilot sequence is
 * TS 38.211 7.4.1.1.1 referenced to CRB 0 (refPoint 0). The x2 Gold register is linear in c_init, so the window bits of
 * every candidate are an XOR of 31 precomputed basis rows: ~65 ms for 131072 candidates on 16 RBs (aarch64).
 * (2) nr_dmrs_scan_vote(): the identities found in a slot are voted for every C-RNTI whose PDSCH is in that slot; a UE's
 * own identity is in all of its slots, a co-scheduled UE's only in some. (3) nr_dmrs_scan_rnti_ids(): the best two
 * identities with >= NR_DMRS_SCAN_MIN_VOTES votes -- hypotheses for the PDSCH solver, the TB CRC decides. Identities
 * found by full scans form a cell-wide pool that nr_dmrs_scan_slot_pool() re-tests cheaply on the slots between scans.
 * (4) nr_dmrs_scan_prb_score(): the same score for ONE identity on a given CRB list -- cheap reading filter.
 * Pure C, no OAI state; thread-safe (vote table under a mutex). */
#ifndef NR_PDSCH_DMRS_SCAN_H
#define NR_PDSCH_DMRS_SCAN_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NR_DMRS_SCAN_CHUNK 16     ///< RBs per scanned chunk (pilot window 192 bits)
#define NR_DMRS_SCAN_MAX_HITS 8
#define NR_DMRS_SCAN_MIN_SCORE 0.6f
#define NR_DMRS_SCAN_MIN_RATIO 1.4f
#define NR_DMRS_SCAN_MIN_VOTES 2
#define NR_DMRS_SCAN_POOL_MIN_SCORE 0.75f

typedef struct {
  int nid, nscid, rb0, nrb;
  float score, second; ///< best and best other-N_ID score of the chunk
} nr_dmrs_scan_hit_t;

/** Score of the best (N_ID, n_SCID) over the whole 16-bit space on CRBs [rb0, rb0+nrb) (nrb <= NR_DMRS_SCAN_CHUNK).
 * rx[a] = the DM-RS OFDM symbol of antenna a (N samples, OAI layout: CRB 0 subcarrier 0 at fco). Returns true and fills
 * *hit when the best clears NR_DMRS_SCAN_MIN_SCORE and NR_DMRS_SCAN_MIN_RATIO x the best other N_ID. */
bool nr_dmrs_scan_chunk(const int16_t *const *rx, int nant, int N, int fco, int slot, int symbol, int symbols_per_slot,
                        int rb0, int nrb, nr_dmrs_scan_hit_t *hit);
/** Energy-segment the symbol over n_rb carrier RBs and scan every chunk (at most max_chunks). Returns the hits. */
int nr_dmrs_scan_slot(const int16_t *const *rx, int nant, int N, int fco, int n_rb, int slot, int symbol,
                      int symbols_per_slot, int max_chunks, nr_dmrs_scan_hit_t *hits, int max_hits);
/** As nr_dmrs_scan_slot(), but testing only the cell-wide pool of identities earlier full scans found (a few ms instead of
 * ~60 ms): a chunk is a hit for the pool identity scoring >= NR_DMRS_SCAN_POOL_MIN_SCORE and NR_DMRS_SCAN_MIN_RATIO x the
 * pool's runner-up. Lets every slot of an RNTI vote between the rate-limited full scans. */
int nr_dmrs_scan_slot_pool(const int16_t *const *rx, int nant, int N, int fco, int n_rb, int slot, int symbol,
                           int symbols_per_slot, int max_chunks, nr_dmrs_scan_hit_t *hits, int max_hits);
/** Same-k' coherence of ONE identity on a CRB list (any order; pairs within each RB). 0 when nothing to score. */
float nr_dmrs_scan_prb_score(const int16_t *const *rx, int nant, int N, int fco, int slot, int symbol, int symbols_per_slot,
                             int nid, int nscid, const uint16_t *crb, int n_crb);

/** Vote the hits of PDSCH slot abs_slot for rnti (once per (rnti, abs_slot)). */
void nr_dmrs_scan_vote(uint16_t rnti, int64_t abs_slot, const nr_dmrs_scan_hit_t *hits, int n);
/** Up to max identities of rnti with >= NR_DMRS_SCAN_MIN_VOTES votes, most voted first. */
int nr_dmrs_scan_rnti_ids(uint16_t rnti, int *ids, int max);
/** Slots voted for rnti so far (0 = unknown RNTI). */
int nr_dmrs_scan_rnti_slots(uint16_t rnti);
void nr_dmrs_scan_reset(void);
/** Reference TS 38.211 5.2.1 Gold bits c(n0 .. n0+n-1) of c_init, bit-serial (tests). */
void nr_dmrs_scan_gold_ref(uint32_t c_init, int n0, int n, uint8_t *out);

#ifdef __cplusplus
}
#endif
#endif
