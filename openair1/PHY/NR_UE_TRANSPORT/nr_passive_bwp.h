/*
 * Passive DL BWP tracking (2026-09-15).
 *
 * A passive receiver learns the cell's INITIAL BWP from SIB1, but a connected UE is usually moved to
 * a dedicated BWP (RRC, invisible here), and can be switched between up to four of them. Every
 * quantity the blind DCI 1_1 path uses depends on the active BWP:
 *   - the payload LENGTH, through the RIV width ceil(log2(N(N+1)/2)) -- nothing else in 1_1 depends
 *     on the BWP size, so   L(N) = L0 - riv_bits(N0) + riv_bits(N)   for the same RRC layout;
 *   - the RIV decode (N) and the PRB origin (start) of the allocation.
 *
 * Discovery, without any RRC:
 *   1. A length L != L0 that decodes to an RNTI ALREADY SEEN on a resolved BWP is a new BWP; L gives
 *      its size to within the range of N sharing that RIV width.
 *   2. Size and start are resolved from the PDSCH DM-RS: its sequence is referenced to CRB 0
 *      (refPoint 0), so a per-PRB coherence vector over the whole carrier, computed once per grant,
 *      scores every (size, start) hypothesis: the true one puts EVERY grant's decoded RIV on PRBs
 *      that carry DM-RS. A decoded RIV that is impossible for a size refutes that size outright.
 *   3. Per RNTI, the active BWP follows the length its DCIs arrive at (RRC-triggered switches), or
 *      the DCI's BWP indicator (DCI-triggered switches, TS 38.212 7.3.1.1.2: the FDRA field keeps the
 *      CURRENT BWP's width -- zeros prepended if the target needs more bits, LSBs kept if fewer).
 *
 * Pure C, no OAI dependencies: unit-tested in tests/nr_passive_bwp_test.cc.
 */
#ifndef NR_PASSIVE_BWP_H
#define NR_PASSIVE_BWP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_PBWP_MAX        5    /* initial/configured + up to 4 dedicated */
#define NR_PBWP_MIN_SIZE   20   /* smallest BWP a probe length is derived for */
#define NR_PBWP_NEW_HITS   3    /* known-RNTI accepts at an unregistered length to register it */
#define NR_PBWP_MIN_GRANTS 8    /* grants scored before a (size, start) may be declared */
#define NR_PBWP_MARGIN     2.0  /* summed-score lead over the runner-up to declare */
#define NR_PBWP_IND_LEARN  8    /* steady-state DCIs binding a BWP-indicator value to an entry */

typedef struct {
  uint16_t dci_len;          /* DCI 1_1 payload length on this BWP */
  uint16_t size_lo, size_hi; /* sizes consistent with dci_len; equal once resolved */
  int16_t  start;            /* first PRB (CRB index); -1 until resolved */
  uint32_t hits, grants_scored, crc_try, crc_ok;
  /* resolution state: score[(N - size_lo) * n_starts + s], NULL once resolved */
  float   *score;
  uint16_t n_starts;
  uint64_t dead_size_lo, dead_size_hi; /* bit (N - size_lo): size refuted by an impossible RIV */
} nr_pbwp_entry_t;

typedef struct {
  uint16_t carrier_rbs, base_len, base_size;
  nr_pbwp_entry_t e[NR_PBWP_MAX];
  int n;
  uint16_t cand_len[16];
  uint32_t cand_hits[16];
  int n_cand, probe_cursor;
  uint8_t ind_map[4];          /* BWP-indicator value -> entry index + 1 (0 = unknown) */
  uint16_t ind_votes[4][NR_PBWP_MAX];
  uint32_t switches;
  uint8_t rnti_bwp[65536];     /* per-RNTI active entry + 1 (0 = never seen) */
  uint8_t rnti_seen[65536 / 8];/* RNTI accepted on a RESOLVED entry: the new-length evidence */
} nr_pbwp_t;

uint8_t nr_pbwp_riv_bits(uint16_t n);
/** TS 38.214 5.1.2.2.2 Type-1 decode, same arithmetic as NRRIV2BW / NRRIV2PRBOFFSET. */
bool nr_pbwp_riv_decode(uint32_t riv, uint16_t n, uint16_t *start, uint16_t *len);
uint16_t nr_pbwp_len_for_size(const nr_pbwp_t *t, uint16_t n);

/** Entry 0 = the BWP the receiver was configured/derived with (start, size, DCI length). */
void nr_pbwp_init(nr_pbwp_t *t, uint16_t carrier_rbs, uint16_t base_start, uint16_t base_size,
                  uint16_t base_len);
void nr_pbwp_free(nr_pbwp_t *t);

/** Entry index whose length is `len` (RNTI's own entry preferred on a tie), or -1. */
int nr_pbwp_entry_for_len(const nr_pbwp_t *t, uint16_t rnti, uint16_t len);
bool nr_pbwp_resolved(const nr_pbwp_t *t, int idx);

/** An accept at a REGISTERED entry's length: tracks the RNTI's active BWP. Returns true when this
 *  moved the RNTI to a different entry (an RRC-triggered switch, seen as a length change). */
bool nr_pbwp_on_accept(nr_pbwp_t *t, uint16_t rnti, int idx);

/** Next unregistered candidate length to probe (round robin), 0 if none. */
uint16_t nr_pbwp_next_probe_len(nr_pbwp_t *t);
/** An accept at a probe length. Counts only RNTIs already seen on a resolved entry; registers a new
 *  entry after NR_PBWP_NEW_HITS. Returns the new entry index, or -1. */
int nr_pbwp_probe_accept(nr_pbwp_t *t, uint16_t rnti, uint16_t len);

/** Score one grant of an unresolved entry. `prb_coh[p]` = DM-RS coherence of CRB p in [0,1] over
 *  the whole carrier. Returns true when this grant resolved the entry's (size, start). */
bool nr_pbwp_score_grant(nr_pbwp_t *t, int idx, uint32_t riv, const float *prb_coh);

/** TB-CRC outcome of a grant decoded against a resolved entry. 32 tries with 0 passes un-resolves
 *  it (the DM-RS vote converged on the wrong hypothesis) and scoring restarts. */
void nr_pbwp_feed_crc(nr_pbwp_t *t, int idx, bool ok);

/** TS 38.212 7.3.1.1.2: FDRA sized for the CURRENT BWP, interpreted for the target one. */
uint32_t nr_pbwp_translate_riv(uint32_t value, uint8_t cur_bits, uint8_t tgt_bits);

/** DCI BWP-indicator handling. Steady-state DCIs (indicator value v received on entry idx) vote
 *  v -> idx; once bound, a DCI whose v points at ANOTHER entry is a switch grant for that entry.
 *  Returns the entry the grant's allocation belongs to (idx when not a switch). */
int nr_pbwp_indicator(nr_pbwp_t *t, uint16_t rnti, int idx, uint8_t ind_value);

#ifdef __cplusplus
}
#endif
#endif
