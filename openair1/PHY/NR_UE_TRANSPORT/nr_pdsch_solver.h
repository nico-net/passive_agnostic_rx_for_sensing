/* Blind dedicated-PDSCH grant solver (T26b, ISAC_PDSCH_SOLVER=1, default off).
 *
 * Technique D learns (S, L, DM-RS, MCS table) per (DCI layout, TDA index) context one hypothesis per grant, and the
 * stage-2 DCI 1_1 layout search spreads grants over every live layout: on PCI 415 (224 live layouts after hard
 * refutation + HARQ-sequence pruning) neither converges inside 12 s. Measured on that capture (T26): small grants
 * decode at LDPC iteration 1 once the PRB set, S/L and DM-RS are right, and their CRC fails otherwise. This module
 * brute-forces exactly those few physical unknowns on small grants with first-code-block probes:
 *
 *   reading  = what one group of live layouts reads from THIS DCI (PRB set, DM-RS ports/CDM, MCS); layouts that
 *              read identically decode identically, so a DCI offers a handful of readings, not 224 layouts;
 *   S        = PDSCH start symbol 0..3 (type A, PDSCH ends at symbol 13: L = 14 - S);
 *   add_pos  = dmrs-AdditionalPosition 0..3 (DM-RS mask from TS 38.211 Table 7.4.1.1.2-3, single symbol);
 *   table    = MCS table 0/1/2, only where this MCS's (Qm, R) differ between tables.
 *
 * A probe pass makes a CANDIDATE (the layouts of that reading, add_pos, table, S); a second pass on another DCI from
 * the candidate's layouts SOLVES the RNTI (2 independent CRC passes: the false-accept bar). Solved RNTIs decode every
 * grant in full with the representative layout's reading; S is learnt per TDA index (as the representative reads it)
 * and the MCS table on the first table-discriminating grant, each by a few probes. The first solution is offered first
 * to every other RNTI (cell prior), never assumed. Thread-safe (one mutex): plan() runs on scan consumers, feedback()
 * on PDSCH consumers. */
#ifndef NR_PDSCH_SOLVER_H
#define NR_PDSCH_SOLVER_H
#include <stdbool.h>
#include <stdint.h>
#include "nr_dci_bits.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NR_PDSCH_SOLVER_MAX_LAYOUTS 32768 /* = NR_DCI11_LAYOUT_MAX */
#define NR_PDSCH_SOLVER_MAX_VAR 24

typedef struct {
  const uint16_t *ids; ///< layout (resolver) indices reading this DCI identically
  int n_ids;
  uint16_t num_rb;     ///< PRB count of this reading (cost and search eligibility)
  uint16_t dmrs_ports; ///< parsed DCI 1_1 antenna-ports bitmap
  uint8_t n_layers;    ///< number of DM-RS ports / PDSCH layers in this reading
  uint8_t mcs;         ///< MCS parsed by this reading
} nr_pdsch_solver_reading_t;

/* ISAC_PDSCH_SOLVER_NL_PREF=N (default off) probes one-layer QPSK readings first and waits up to N grants before
 * falling back when none is available. ISAC_PDSCH_SOLVER_THREAD_CTX=1 (default off) keeps each scan consumer's grant
 * scrambling/CORESET/slot context separate so concurrent plans cannot use another consumer's hypotheses. */
/* ISAC_PDSCH_SOLVER_COVERAGE=1 (default off) tracks search progress per stable layout, TDRA and scrambling identity,
 * so changes in the order/number of readings between grants cannot repeatedly skip a physical hypothesis. */

typedef struct {
  int reading;         ///< index into the caller's readings[]
  uint8_t S, L, add_pos, table;
  uint16_t dmrs_mask;  ///< dlDmrsSymbPos encoding (bit l = symbol l)
  uint8_t probe;       ///< 1 = first-code-block probe (search), 0 = full decode (solved parameters)
  uint8_t rm;          ///< 1 = rate-match around the run-time-discovered CSI-RS resources of this slot
  uint32_t token;      ///< hand back to nr_pdsch_solver_feedback()
  uint8_t comb;        ///< T26d ISAC_RX_MRC_PER_RNTI: combining mode for this variant (1 best branch, 3 MRC; 0 default)
  int32_t scr;         ///< T26c: -1 = default PDSCH scrambling ids; else DM-RS + data scrambling id = this run-time nID
} nr_pdsch_solver_var_t;

/// tda index the layout reads from the payload (the caller owns the layout offsets)
typedef int (*nr_pdsch_solver_tda_fn)(void *ctx, uint16_t layout, nr_dci_bits_t payload);

bool nr_pdsch_solver_enabled(void);                    ///< ISAC_PDSCH_SOLVER=1
void nr_pdsch_solver_reset(void);                      ///< tests / configuration epoch
void nr_pdsch_solver_set_tda_fn(nr_pdsch_solver_tda_fn fn, void *ctx);

/** Solved RNTI: its representative layout (>= 0), else -1. A solved caller parses only that layout. */
int nr_pdsch_solver_rep(uint16_t rnti);
/** ISAC_PDSCH_SOLVER_LAYOUT_RECOVER=N (default off): after N credited failed search probes,
 * reopen the active FDRA families' pruned layouts for this RNTI, including post-solve refinement. */
bool nr_pdsch_solver_reopen_layouts(uint16_t rnti);
/** True while an RNTI is unsolved: the caller sends this RNTI's grants to the solver instead of Technique D. */
bool nr_pdsch_solver_owns(uint16_t rnti);

/** Variants to decode for one DCI 1_1 grant (payload of `len` bits, MCS index `mcs`, DM-RS type-A position l0 2/3).
 * `budget` caps the probes; returns the number written to out[]. */
int nr_pdsch_solver_plan(uint16_t rnti, nr_dci_bits_t payload, uint16_t len, uint8_t mcs, int l0,
                         const nr_pdsch_solver_reading_t *rd, int n_rd, int budget, nr_pdsch_solver_var_t *out);
/** As nr_pdsch_solver_plan(), for a slot that carries run-time-discovered CSI-RS resources (csirs_slot): whether the
 * gNB rate-matches this RNTI's PDSCH around them (TS 38.214 5.1.4: only resources configured for THIS UE) is a per-RNTI
 * hypothesis chosen by TB CRC -- both are probed until one passes. The search never starts on such a slot, so a
 * missing rate match cannot count against the true layout. */
int nr_pdsch_solver_plan_slot(uint16_t rnti, nr_dci_bits_t payload, uint16_t len, uint8_t mcs, int l0, bool csirs_slot,
                              const nr_pdsch_solver_reading_t *rd, int n_rd, int budget, nr_pdsch_solver_var_t *out);
/** T26c: absolute slot (signal time) of the grant about to be planned, for the per-RNTI solve-time report. */
void nr_pdsch_solver_set_now(uint64_t abs_slot);
/** T26c: post-equalisation SNR (dB) of one full decode of `rnti` (EVMGRANT); kept as a per-RNTI EMA. */
void nr_pdsch_solver_note_snr(uint16_t rnti, float snr_db);
/** T26c: the RNTI's SNR EMA once >= 3 measurements exist; false otherwise. */
bool nr_pdsch_solver_snr(uint16_t rnti, float *snr_db);
/** T26c (ISAC_PDSCH_SOLVER_SCR=1): before planning, the RUN-TIME discovered identities (this DCI's CORESET PDCCH nID
 * first, then the other discovered CORESETs' nIDs) the PDSCH scrambling (DM-RS scramblingID0 + dataScramblingIdentity-
 * PDSCH) may use: a per-RNTI CRC-selected hypothesis {default, ids...}, searched grant by grant until a candidate fixes
 * it. Nothing configured; n = 0 restores default-only. coreset_key (the CORESET's PDCCH nID, or -1) groups RNTIs: once
 * one RNTI of a CORESET solved, the others of that CORESET try only its scrambling. The list may also carry the
 * scrambling ids of run-time discovered NZP CSI-RS (ISAC_PDSCH_SOLVER_SCR_CSIRS=1): neighbour-cell candidates. */
void nr_pdsch_solver_set_alt_scr(int coreset_key, const uint16_t *ids, int n, bool first_is_decided);
/** TIM-PDSCH (ISAC_PDSCH_DMRS_ID_SCAN): as nr_pdsch_solver_set_alt_scr() with 32-bit hypotheses (id | (data id + 1) << 16
 * = DM-RS id and a different data scrambling id). own = the hypotheses are this RNTI's own (per-UE identities): the
 * CORESET's first solved scrambling is then NOT preferred over them. Not gated by ISAC_PDSCH_SOLVER_SCR. */
void nr_pdsch_solver_set_alt_scr32(int coreset_key, const int32_t *ids, int n, bool first_is_decided, bool own);
/** Outcome of a variant: CRC of code block 0 (probe) or of the TB (full decode). credit=false: IQ expired, ignored. */
void nr_pdsch_solver_feedback(uint32_t token, bool crc_ok, bool credit);

/** DM-RS symbol mask, type A, single-symbol DM-RS, PDSCH S..S+L-1 (TS 38.211 Table 7.4.1.1.2-3). */
uint16_t nr_pdsch_solver_dmrs_mask(int l0, int S, int L, int add_pos);
/** (Qm, R x 1024 x 2) of MCS `mcs` in table 0/1/2; false if the index is reserved/out of range. */
bool nr_pdsch_solver_mcs_qr(int table, int mcs, int *qm, int *r2);

/* Per-RNTI search diagnostics (SOLVERCOVER 2026-10-11): why an unsolved RNTI's grant produced no credited probe. The counters
 * are always kept (a few increments under the existing mutex); ISAC_PDSCH_SOLVER_WHY=1 logs them with the per-rnti report.
 * Caller-side reasons (before nr_pdsch_solver_plan_slot()) are noted with nr_pdsch_solver_note_skip(). */
enum {
  NR_PDSCH_SOLVER_WHY_GATE,     ///< rv != 0 / no defer / no decode wanted / per-occasion cap reached before planning
  NR_PDSCH_SOLVER_WHY_LIVE,     ///< too many live DCI layouts (ISAC_PDSCH_SOLVER_MAX_LIVE)
  NR_PDSCH_SOLVER_WHY_BUDGET0,  ///< probe token bucket empty / ring too full: not planned
  NR_PDSCH_SOLVER_WHY_NOREAD,   ///< no layout parsed this DCI
  NR_PDSCH_SOLVER_WHY_THROTTLE, ///< a planned probe refused by the token bucket
  NR_PDSCH_SOLVER_WHY_CAP,      ///< a planned variant cut by the per-occasion decode cap
  NR_PDSCH_SOLVER_WHY_WEAK,     ///< ISAC_PDSCH_SOLVER_DMRS_PRIO: contended, and no reading carries the RNTI's own DM-RS
  NR_PDSCH_SOLVER_WHY_CALLER_N
};
void nr_pdsch_solver_note_skip(uint16_t rnti, int why);
/** ISAC_PDSCH_SOLVER_FAIRSHARE=1 (default off): fair per-RNTI share of the probe rate for unsolved RNTIs, and reuse of
 * the least recently active solver entry (idle >= 2 s) when the 64-RNTI table is full. */
bool nr_pdsch_solver_fairshare_on(void);
/** Probes an unsolved RNTI may plan now: its share rate_per_slot / n_active of the probe rate (capped at budget), applied
 * only while the global bucket is below its burst or the ring above its fill (contended); budget unchanged with the flag off. */
int nr_pdsch_solver_fair_budget(uint16_t rnti, double rate_per_slot, bool contended, int budget);

/* TIMSOLVE2 (2026-10-11, default off): ISAC_PDSCH_SOLVER_SIGNIF=1 solves an RNTI only on significant CRC evidence: the
 * candidate's consistent passes must have P(chance) < 1e-3 under a Poisson bound over all its credited probes (CRC16 code
 * blocks 2^-16, CRC24 2^-24; CRC16-only evidence needs >= 2 passes on distinct DCIs, one CRC24 pass can suffice).
 * ISAC_PDSCH_SOLVER_PRIOR_IDS=1 tries the CRC-proven solutions' layout dims with every own blind identity of an unsolved
 * RNTI (in the proven identity form) on each grant. ISAC_PDSCH_SOLVER_DMRS_PRIO=1 (nr_pdcch_blind_monitor_rt.c): under
 * probe contention, grants on which no reading carries the RNTI's own DM-RS are probed 1 time in 8. */
typedef struct { uint64_t planned, probes, fulls, probe_ok, full_ok, candidates, solved, dropped; } nr_pdsch_solver_stats_t;
void nr_pdsch_solver_stats(nr_pdsch_solver_stats_t *s);

#ifdef __cplusplus
}
#endif
#endif
