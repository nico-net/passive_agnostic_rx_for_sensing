/*
 * Passive DL BWP tracking (2026-09-15).
 *
 * A passive receiver learns the cell's INITIAL BWP from SIB1, but a connected UE is usually moved to
 * a dedicated BWP (RRC, invisible here), and can be switched between up to four of them. Every
 * quantity the blind DCI 1_1 path uses depends on the active BWP:
 *   - the payload LENGTH: L = K + riv_bits(N) + d, where riv_bits(N) = ceil(log2(N(N+1)/2)) is the
 *     FDRA width for a BWP of N PRBs, d the BWP-indicator width (0..2, set by how many dedicated
 *     BWPs RRC configured -- also invisible), and K everything else in the layout;
 *   - the RIV's bit position (it follows the indicator), its decode (N) and the PRB origin (start).
 *
 * Discovery, without any RRC:
 *   1. A length L != L0 that decodes to an RNTI ALREADY PROVEN by an accepted DCI is a new BWP. L
 *      admits one group of hypotheses per indicator width d, each a range of sizes N.
 *   2. (d, N, start) is resolved from the PDSCH DM-RS: its sequence is referenced to CRB 0
 *      (refPoint 0), so a per-PRB coherence vector over the whole carrier, computed once per grant,
 *      scores every hypothesis: the true one puts EVERY grant's decoded RIV on PRBs that carry DM-RS.
 *      A RIV that is impossible for a size refutes that size outright.
 *   3. Per RNTI, the active BWP follows the length its DCIs arrive at (RRC-triggered switches), or
 *      the DCI's BWP indicator (DCI-triggered switches, TS 38.212 7.3.1.1.2: the FDRA field keeps the
 *      CURRENT BWP's width -- zeros prepended if the target needs more bits, LSBs kept if fewer).
 *
 * Scope, stated: assumes the dedicated BWPs share one pdsch-Config (so only the FDRA and the
 * indicator change the length). A cell whose per-BWP pdsch-Config differs needs the V2 layout
 * sweep run per BWP. Pure C, no OAI dependencies: unit-tested in tests/nr_passive_bwp_test.cc.
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
#define NR_PBWP_NEW_HITS   3    /* proven-RNTI accepts at an unregistered length to register it */
#define NR_PBWP_MIN_GRANTS 8    /* grants scored before a hypothesis may be declared */
#define NR_PBWP_MARGIN     2.0  /* summed-score lead over the runner-up to declare */
#define NR_PBWP_IND_LEARN  8    /* steady-state DCIs binding a BWP-indicator value to an entry */
#define NR_PBWP_MAX_CAND   32
#define NR_PBWP_CS_MAXWIN  46   /* 6-RB windows on a 275-PRB carrier */
#define NR_PBWP_CS_MIN_OCC 64   /* observed occasions before a CORESET may be declared */
#define NR_PBWP_CS_MIN_HITS 8   /* above-threshold hits for a window to count as lit */

typedef struct {
  uint8_t  d;                /* BWP-indicator width of this hypothesis group */
  uint16_t lo, hi;           /* sizes sharing this group's FDRA width */
  uint16_t n_starts;
  uint64_t dead_lo, dead_hi; /* bit (N - lo): size refuted by an impossible RIV */
  float   *score;            /* [(N - lo) * n_starts + s] */
} nr_pbwp_group_t;

typedef struct {
  uint16_t dci_len;          /* DCI 1_1 payload length on this BWP */
  int16_t  start;            /* first PRB (CRB index); -1 until resolved */
  uint16_t size;             /* valid once resolved */
  uint8_t  ind_bits;         /* BWP-indicator width; valid once resolved */
  uint32_t hits, grants_scored, crc_try, crc_ok;
  nr_pbwp_group_t g[3];
  int ng;
} nr_pbwp_entry_t;

/* CORESET discovery for dedicated BWPs: a dedicated BWP's CORESET lives inside that BWP, so the
 * configured one never sees its DCIs. PDCCH DM-RS is present only where a PDCCH is sent, so each
 * occasion lights up the 6-RB windows carrying DCIs; over many occasions a CORESET appears as a run of
 * lit windows. The DM-RS reference (CRB 0 per 38.211, the BWP start on OAI) is voted on as an absolute
 * RB -- on OAI it IS the BWP start. */
typedef struct {
  uint32_t occ;
  uint32_t hits[NR_PBWP_CS_MAXWIN][2];      /* per window, CORESET symbol 0 / 1 */
  uint32_t base_hits[NR_PBWP_CS_MAXWIN][2]; /* same, inside the configured CORESET at its own reference */
  int16_t base_lo, base_hi, base_ref;       /* configured CORESET of the last observation */
  uint32_t ref_votes[276];                  /* winning reference RB of each lit window */
} nr_pbwp_coreset_t;

typedef struct {
  uint16_t carrier_rbs, base_len, base_size;
  uint8_t base_ind_bits;
  nr_pbwp_entry_t e[NR_PBWP_MAX];
  int n;
  uint16_t cand_len[NR_PBWP_MAX_CAND];
  uint32_t cand_hits[NR_PBWP_MAX_CAND];
  uint16_t cand_rnti[NR_PBWP_MAX_CAND][4];  /* repetition proof: recent RNTIs per candidate length */
  uint8_t  cand_rnti_n[NR_PBWP_MAX_CAND][4];
  int n_cand, probe_cursor;
  uint8_t ind_map[4];          /* BWP-indicator value -> entry index + 1 (0 = unknown) */
  uint16_t ind_votes[4][NR_PBWP_MAX];
  uint32_t switches;
  uint8_t rnti_bwp[65536];     /* per-RNTI active entry + 1 (0 = never seen on a 1_1 entry) */
  uint8_t rnti_seen[65536 / 8];/* RNTI proven by an accepted DCI: the new-length evidence */
  nr_pbwp_coreset_t cs;
} nr_pbwp_t;

uint8_t nr_pbwp_riv_bits(uint16_t n);
/** TS 38.214 5.1.2.2.2 Type-1 decode, same arithmetic as NRRIV2BW / NRRIV2PRBOFFSET. */
bool nr_pbwp_riv_decode(uint32_t riv, uint16_t n, uint16_t *start, uint16_t *len);
/** Length of a DCI 1_1 on a BWP of `n` PRBs with a `d`-bit indicator, same layout otherwise. */
uint16_t nr_pbwp_len_for(const nr_pbwp_t *t, uint16_t n, uint8_t d);
/** The FDRA field of a `len`-bit payload with a `d`-bit indicator and an `rb`-bit FDRA. */
uint32_t nr_pbwp_riv_field(uint64_t payload, uint16_t len, uint8_t d, uint8_t rb);

/** Entry 0 = the BWP the receiver was configured/derived with. */
void nr_pbwp_init(nr_pbwp_t *t, uint16_t carrier_rbs, uint16_t base_start, uint16_t base_size,
                  uint16_t base_len, uint8_t base_ind_bits);
void nr_pbwp_free(nr_pbwp_t *t);

int nr_pbwp_entry_for_len(const nr_pbwp_t *t, uint16_t rnti, uint16_t len);
bool nr_pbwp_resolved(const nr_pbwp_t *t, int idx);
static inline bool nr_pbwp_rnti_seen(const nr_pbwp_t *t, uint16_t rnti)
{
  return (t->rnti_seen[rnti >> 3] >> (rnti & 7)) & 1u;
}
/** An RNTI proven by any accepted DCI (1_0 or 1_1): enables new-length evidence from it. */
void nr_pbwp_mark_seen(nr_pbwp_t *t, uint16_t rnti);

/** An accept at a REGISTERED entry's length: tracks the RNTI's active BWP. Returns true when this
 *  moved the RNTI to a different entry (an RRC-triggered switch, seen as a length change). */
bool nr_pbwp_on_accept(nr_pbwp_t *t, uint16_t rnti, int idx);

/** Next unregistered candidate length to probe (round robin), 0 if none. */
uint16_t nr_pbwp_next_probe_len(nr_pbwp_t *t);
/** An accept at a probe length. Registers a new entry after NR_PBWP_NEW_HITS accepts from proven
 *  RNTIs, or after NR_PBWP_NEW_HITS accepts from the SAME unproven RNTI (repetition proof: when every UE
 *  has moved to a dedicated BWP none is ever proven on the base one, and noise accepts carry uniformly
 *  random RNTIs). Returns the new entry index, or -1. */
int nr_pbwp_probe_accept(nr_pbwp_t *t, uint16_t rnti, uint16_t len);

/** Score one grant of an unresolved entry from its raw payload. `prb_coh[p]` = DM-RS coherence of
 *  CRB p in [0,1] over the whole carrier. Returns true when this grant resolved the entry. */
bool nr_pbwp_score_grant(nr_pbwp_t *t, int idx, uint64_t payload, const float *prb_coh);

/** TB-CRC outcome of a grant decoded against a resolved entry. 32 tries with 0 passes un-resolves
 *  it (the DM-RS vote converged on the wrong hypothesis) and scoring restarts. */
void nr_pbwp_feed_crc(nr_pbwp_t *t, int idx, bool ok);

/** TS 38.212 7.3.1.1.2: FDRA sized for the CURRENT BWP, interpreted for the target one. */
uint32_t nr_pbwp_translate_riv(uint32_t value, uint8_t cur_bits, uint8_t tgt_bits);

/** DCI BWP-indicator handling. Steady-state DCIs (indicator value v received on entry idx) vote
 *  v -> idx; once bound, a DCI whose v points at ANOTHER resolved entry is a switch grant for it.
 *  Returns the entry the grant's allocation belongs to (idx when not a switch). */
int nr_pbwp_indicator(nr_pbwp_t *t, int idx, uint8_t ind_value);

/** One CORESET-symbol observation. corr[w] = best |corr| of window w (RBs 6w..6w+5) over the reference
 *  hypotheses, ref[w] = the reference RB that achieved it. A window inside the configured CORESET
 *  [base_lo, base_hi] is ignored only when its reference is the configured one (base_ref): a dedicated
 *  BWP's CORESET may sit INSIDE a wide configured CORESET, and on OAI it is told apart by its DM-RS
 *  reference (its own BWP start). Call with symbol 0 once per observed occasion, then symbol 1. */
void nr_pbwp_coreset_observe(nr_pbwp_t *t, int n_win, int base_lo, int base_hi, int base_ref,
                             const float *corr, const int16_t *ref, int symbol, float threshold);
/** The discovered CORESET, if any: first RB, size in RB (multiple of 6), duration (1 or 2 symbols) and
 *  the DM-RS reference RB (0 = CRB 0; on OAI the BWP start). */
bool nr_pbwp_coreset_hypothesis(const nr_pbwp_t *t, int *start_rb, int *n_rb, int *duration, int *ref_rb);

#ifdef __cplusplus
}
#endif
#endif
