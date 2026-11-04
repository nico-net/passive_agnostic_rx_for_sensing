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
#include "nr_traffic_record.h" /* nr_layout_state_t */
#ifdef __cplusplus
extern "C" {
#endif

#define NR_PDSCH_SOLVER_MAX_LAYOUTS 65536 /* >= NR_DCI11_LAYOUT_MAX (65535; id 0xFFFF is "none") */
#define NR_PDSCH_SOLVER_MAX_VAR 24

typedef struct {
  const uint16_t *ids; ///< layout (resolver) indices reading this DCI identically
  int n_ids;
  uint16_t num_rb;     ///< PRB count of this reading (cost and search eligibility)
  uint16_t dmrs_ports; ///< parsed DCI 1_1 antenna-ports bitmap
  uint8_t n_layers;    ///< number of DM-RS ports / PDSCH layers in this reading
  uint8_t mcs;         ///< MCS parsed by this reading
  float fit;           ///< DM-RS evidence for this reading's PRBs in this slot, 0..1 (F1 against the PRBs carrying the RNTI's
                       ///< DM-RS); < 0 = not measured. Orders the search and weights its probe share; never a hard cut.
} nr_pdsch_solver_reading_t;

/* Readings are ranked only by evidence: the DM-RS fit (reading.fit) orders and weights the search, the CRC decides. There is no
 * width limit and no modulation preference (DCI11-RESOLVER 2026-10-24). Each scan consumer keeps its own grant
 * scrambling/CORESET/slot context, so concurrent plans cannot use another consumer's hypotheses. */
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

/** Slot-phase period (slots: 20, 40, 80 or 160) the cell's CRC passes of full-length allocations show; 20 until they conflict. Learnt, never set. */
int nr_pdsch_solver_phase_period(void);
/** ISAC_PDSCH_SOLVER_LAYOUT_RECOVER=N (default off): after N credited failed search probes,
 * reopen the active FDRA families' pruned layouts for this RNTI, including post-solve refinement. */
bool nr_pdsch_solver_reopen_layouts(uint16_t rnti);
/** True while an RNTI is unsolved: the caller sends this RNTI's grants to the solver instead of Technique D. */
bool nr_pdsch_solver_owns(uint16_t rnti);
/** E4 (traffic first DCI): while on, unsolved RNTIs are planned with the seeded variants only (no coverage search). Per thread; set for every grant. */
void nr_pdsch_solver_set_seeded_only(bool on);

/** Variants to decode for one DCI 1_1 grant (payload of `len` bits, MCS index `mcs`, DM-RS type-A position l0 2/3).
 * `budget` caps the probes; returns the number written to out[]. */
int nr_pdsch_solver_plan(uint16_t rnti, nr_dci_bits_t payload, uint16_t len, uint8_t mcs, int l0,
                         const nr_pdsch_solver_reading_t *rd, int n_rd, int budget, nr_pdsch_solver_var_t *out);
/** A SOLVED RNTI's layout set (the layouts consistent with its CRC passes, the representative among them), ascending, at most
 * `max`: the caller then parses only these instead of every live layout. -1 when the RNTI is not solved or the set exceeds max. */
int nr_pdsch_solver_layouts(uint16_t rnti, uint16_t *ids, int max);
/** As nr_pdsch_solver_plan(), for a slot that carries run-time-discovered CSI-RS resources (csirs_slot): whether the
 * gNB rate-matches this RNTI's PDSCH around them (TS 38.214 5.1.4: only resources configured for THIS UE) is a per-RNTI
 * hypothesis chosen by TB CRC -- both are probed until one passes. The search never starts on such a slot, so a
 * missing rate match cannot count against the true layout. */
int nr_pdsch_solver_plan_slot(uint16_t rnti, nr_dci_bits_t payload, uint16_t len, uint8_t mcs, int l0, bool csirs_slot,
                              const nr_pdsch_solver_reading_t *rd, int n_rd, int budget, nr_pdsch_solver_var_t *out);
/** T26c: absolute slot (signal time) of the grant about to be planned, for the per-RNTI solve-time report. */
void nr_pdsch_solver_set_now(uint64_t abs_slot);
/* SOLVERTDDEND: last DL symbol (0..13) of the planned grant's slot from the broadcast TDD pattern, -1 = unknown; set per grant */
void nr_pdsch_solver_set_slot_last(int last);
/** T26c: post-equalisation SNR (dB) of one full decode of `rnti` (EVMGRANT); kept as a per-RNTI EMA. */
void nr_pdsch_solver_note_snr(uint16_t rnti, float snr_db, int comb, bool crc_ok); /* comb: the receiver of that decode (EVMGRANT comb=), 0 unknown; crc_ok: MRCBUG, mode 2 keeps only passing decodes */
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
/** A3: identities (DM-RS scrambling ids, 0..65535) already seen in the cell, most frequent first (nr_dmrs_scan_pool_ids()), offered to an
 * RNTI that has none of its own yet: the solver tries them first in the identity form its cell's solved RNTIs use when those RNTIs do not
 * share one id (per-UE ids). Per scan consumer thread; cleared by nr_pdsch_solver_set_alt_scr*(), so call it after them. n <= NR_PDSCH_SOLVER_POOL_MAX. */
#define NR_PDSCH_SOLVER_POOL_MAX 32
void nr_pdsch_solver_set_pool_ids(const int32_t *ids, int n);
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
  NR_PDSCH_SOLVER_WHY_GATE,     ///< no defer / no decode wanted before planning
  NR_PDSCH_SOLVER_WHY_LIVE,     ///< too many live DCI layouts (ISAC_PDSCH_SOLVER_MAX_LIVE)
  NR_PDSCH_SOLVER_WHY_BUDGET0,  ///< probe token bucket empty / ring too full: not planned
  NR_PDSCH_SOLVER_WHY_NOREAD,   ///< no layout parsed this DCI
  NR_PDSCH_SOLVER_WHY_THROTTLE, ///< a planned probe refused by the token bucket
  NR_PDSCH_SOLVER_WHY_CAP,      ///< a planned variant cut by the per-occasion decode cap
  NR_PDSCH_SOLVER_WHY_WEAK,     ///< ISAC_PDSCH_SOLVER_DMRS_PRIO: contended, and no reading carries the RNTI's own DM-RS
  NR_PDSCH_SOLVER_WHY_GATE_RV,  ///< a solved RNTI's rv != 0 grant with no failed transmission of its TB held (first transmission not seen / evicted)
  NR_PDSCH_SOLVER_WHY_GATE_RVOK,///< a solved RNTI's rv != 0 grant whose TB was already decoded
  NR_PDSCH_SOLVER_WHY_GATE_CAP, ///< the per-occasion decode cap already reached before planning
  NR_PDSCH_SOLVER_WHY_CALLER_N
};
void nr_pdsch_solver_note_skip(uint16_t rnti, int why);
/** PARK (2026-10-30): one decode (probe or full) of the RNTI's grant at signal slot abs_slot with its DM-RS comb SNR (dB; <= -90 = no estimate, ignored).
 *  hyp: the decode's hypothesis class (any packing of DM-RS ports, CDM groups, layers, scrambling id). The cell's noise-floor reference is measured from the decodes of RNTIs that never solved; an RNTI whose every hypothesis class stays inside that
 *  noise (one sequential probability ratio test per hypothesis class, error rate NSE_BETA, nr_signal_evidence.h) is parked: unsolved RNTIs are searched on 1 grant in 2^k (k <= 4),
 *  solved RNTIs drop their probes (never their full decodes). gate_obs: an ungated real first-transmission decode of a dedicated grant; its margin to
 *  the MCS requirement feeds the cell-measured no-chance margin (nr_snr_gate_margin_db). Logs "SENSING: PARK". */
void nr_pdsch_solver_note_dmrs(uint16_t rnti, uint64_t abs_slot, uint32_t hyp, float comb_db, bool crc_ok, bool probe, int mcs, int tbl, bool gate_obs);
bool nr_pdsch_solver_parked(uint16_t rnti);
double nr_snr_gate_margin_db_cell(int ul);
/** IDQUAL: the best class of the RNTI's DM-RS identities (2 proven by its decodes' DM-RS SNR, 1 untested, 0 all wrong, -1 none); steers the
 *  search back-off (never backed off on a proven identity, after 64 probes when all are wrong, else after 200). */
void nr_pdsch_solver_note_id_quality(uint16_t rnti, int q);
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

/* DCIRV (2026-10-12, ISAC_DCI11_RV_CONSISTENCY=1, default off). Layouts a few bits apart all read MCS 0 when those bits are zero,
 * so an RNTI whose CRC passes were all MCS 0 can solve to a layout whose NDI/RV/HARQ fields sit one bit off (OTA TIM PCI 923:
 * 255 + 473 RVRESCUE lines, all dci_rv=1 ok_rv=0). The caller reads (NDI, RV, HARQ pid) of a layout from a payload; the solver
 * keeps each RNTI's last 64 DCI payloads and scores a layout by TS 38.214 HARQ rules per pid: a new transmission (NDI toggled)
 * has rv 0, a retransmission follows rv 0,2,3,1 (<= 1 step missed). A layout is refuted when its violations exceed a binomial
 * bound (P(X >= bad | n, 0.15) < 1e-3) and VERIFIED when they refute a 0.5 violation rate. The representative is the best
 * unrefuted consistent layout; a solution seeds the cell/other RNTIs only after a CRC pass at MCS > 0 or HARQ verification;
 * >= 3 notes of an RV rescue (CRC only with another rv than the DCI's) re-select the RNTI's layout, or reopen its search. */
typedef bool (*nr_pdsch_solver_hq_fn)(void *ctx, uint16_t layout, nr_dci_bits_t payload, uint8_t *ndi, uint8_t *rv, uint8_t *pid);
void nr_pdsch_solver_set_hq_fn(nr_pdsch_solver_hq_fn fn, void *ctx);
bool nr_pdsch_solver_rvc_on(void);
/** One DCI 1_1 payload of this C-RNTI at signal slot abs_slot (same slot twice is ignored). Call before any gate. */
void nr_pdsch_solver_note_dci(uint16_t rnti, nr_dci_bits_t payload, uint64_t abs_slot);
/** The full decode of `rnti` passed only with an rv other than its DCI's (RVRESCUE). */
void nr_pdsch_solver_note_rv_mismatch(uint16_t rnti);
/** The binomial bounds (exposed for tests): n evaluable grants, bad violations. */
bool nr_pdsch_solver_hq_refuted(int n, int bad);
bool nr_pdsch_solver_hq_verified(int n, int bad);

/* RNTIs the solver tracks per cell (was 64: PCI 923 OTA 2026-10-30 carries 109-132 active RNTIs; at 64 solved RNTIs were evicted 28-50 times
 * per 300 s and had to be solved again). The least recently active entry idle >= 2 s is reused when the table is full. */
#define NR_PDSCH_SOLVER_RNTIS 256

/* Fix A (ISAC_PDSCH_TDA_FIELD=1, default off): the DCI 1_1 time-domain-assignment field the layout solver aliases as pre-MCS bits
 * (tda_bits=0). Blind: W = ceil(log2(N)) of the SIB1 common TDRA list (else 1 or 2 bits learnt) at the position where the TDA
 * field would start; accepted only when its value depends on the TDD slot class (mixed slot) or on the (S,L) of CRC-passing
 * decodes. Rows: SIB1 common rows legal in the slot (S+L-1 <= last PDSCH symbol of the TDD pattern), the field value picks the
 * row; with no list, the (S,L) learnt for (value, slot class) from CRC passes. No per-cell constants. Thread-safe. */
bool nr_tda_field_on(void);
void nr_tda_field_reset(void);
void nr_tda_field_forget(void);
int nr_tda_field_width(int pos); /* the detected TDRA field width at bit `pos`, 0 = undetected */

/* PMEXPLORE: the DCI 1_1 bits between the TDRA field and MCS that the layout reads as one pre-MCS group (VRB-to-PRB, PRB bundling, rate
 * matching, ZP CSI-RS trigger). A value of that window whose full decodes never pass (>= 32, none) names a field the decode does not honour;
 * its grants try the hypotheses the receiver can apply instead (NR_PM_HYP_*: interleaved VRB-to-PRB mapping, bundle 2 or 4, TS 38.211
 * 7.3.1.6), round robin, and the first CRC pass is learnt for that value. Keyed by (window start, width); per cell; thread-safe. */
enum { NR_PM_HYP_PLAIN = 0, NR_PM_HYP_ILV2, NR_PM_HYP_ILV4, NR_PM_HYP_N };
int nr_pm_hyp(int pos, int nbits, nr_dci_bits_t p, int len); /* the hypothesis this grant decodes with */
void nr_pm_note_result(int pos, int nbits, nr_dci_bits_t p, int len, int hyp, bool ok); /* its first-pass outcome (rv0) */
void nr_pm_forget(void); /* a new cell identity */ /* a new cell identity: the learnt field and rows (and the DCIBITS tally) go, the SIB1 rows stay */
void nr_tda_field_set_rows(const uint8_t *S, const uint8_t *L, int n); /* SIB1 pdsch-ConfigCommon list (n < 2: no field) */
/* One accepted C-RNTI DCI 1_1 (layout with tda_bits == 0, TDA window at bit `pos`); mixed = TDD slot with last PDSCH symbol < 13 */
void nr_tda_field_note_dci(int pos, nr_dci_bits_t p, int len, bool mixed);
/* A CRC-passing decode of such a DCI with (S, L) */
void nr_tda_field_note_ok(int pos, nr_dci_bits_t p, int len, bool mixed, int S, int L);
void nr_tda_field_note_ok_k0(int pos, nr_dci_bits_t p, int len, bool mixed, int S, int L, int k0); /* ... decoded K0 slots after the DCI */
/* DCIBITS: the first-pass outcome of every full decode of such a DCI, tallied per bit of the window [pos, pos + 8) (the TDRA field and the
 * pre-MCS fields the layout reads as one group: VRB-to-PRB, PRB bundling, rate matching, ZP CSI-RS trigger). A bit whose value changes the
 * CRC rate names a field the decode must honour. Periodic log line "SENSING: DCIBITS". */
void nr_tda_field_note_result(int pos, nr_dci_bits_t p, int len, bool mixed, bool ok);
/* Candidate (S,L) rows for a DCI, best first, at most `max`; 0 = no override (use the solver's reading).
 * last = last PDSCH symbol of the slot (13 when no TDD pattern is known). *detected: the field was accepted. */
int nr_tda_field_rows(int pos, nr_dci_bits_t p, int len, int last, int max, uint8_t *S, uint8_t *L, bool *detected);
/* The same with cross-slot rows: last[k] / dl[k] = last PDSCH-able symbol (-1: no DL) / leading DL symbols of the slot K0 = k after the DCI,
 * k = 0..2 (nr_passive_acq_tdd_pdsch_last_symbols / _dl_symbols). K0[i] is the slot offset of row i. A detected value whose grants never pass
 * (>= 32 decodes) gets the next TDD-legal (K0, S, L) candidate per grant until one passes (then learnt). */
int nr_tda_field_rows_k0(int pos, nr_dci_bits_t p, int len, const int8_t *last, const int8_t *dl, int max, uint8_t *S, uint8_t *L,
                         uint8_t *K0, bool *detected);

/* ISAC_RNTI_LAYOUT_CACHE=1 (default off): the layout set of a SOLVED RNTI for one grant context. `key` packs everything that changes
 * what a layout reads (DCI length, CORESET, slot class: TDD special/mixed slot vs full, CSI-RS slot, TDA-field mode); a context never
 * shares an entry with another. Returns n > 0 and writes the layout ids (ascending, the representative included) to ids[]; -1 = no cache
 * (flag off, RNTI new/unsolved/LOST, set larger than the cache, or max_fail > 0 and the RNTI has failed that many full TB CRCs in a row:
 * possible reconfiguration, the caller re-solves from every live layout). Layouts the resolver has since killed are the caller's to skip. */
bool nr_rnti_layout_cache_on(void);
int nr_pdsch_solver_layout_cache(uint16_t rnti, uint64_t key, int max_fail, uint16_t *ids, int max);
typedef struct { uint64_t hit, miss, stale, fail_resolve, bypass; } nr_pdsch_solver_lc_stats_t;
void nr_pdsch_solver_layout_cache_stats(nr_pdsch_solver_lc_stats_t *s);

/* SOLVERGATE (2026-10-24): the probe admission bucket as a reservation. A grant's planner used to read the bucket (whole tokens, ring
 * fill below the bound) and then each planned probe was admitted again against the live bucket and ring: the grant's own enqueues raise
 * the fill (a 20-deep ring moves 5 % per job) and concurrent producers spend the same tokens, so a plan the bucket allowed was then
 * refused probe by probe (273 PRB: 540 of 1189 planned probes). reserve() takes min(tokens, ring headroom not yet reserved) at plan
 * time, take() admits a probe from that reservation, release() returns what the grant did not use. Same rate, burst and fill bound.
 * Pure state; the caller serialises (one mutex). */
typedef struct {
  double tokens, rate, burst;
  long last_abs;
  int resv_jobs; ///< reserved and not yet taken or released (ring slots promised to planners)
} nr_probe_gate_t;
void nr_probe_gate_init(nr_probe_gate_t *g, double rate, double burst);
/** Refill to abs_slot, then reserve up to max whole tokens, bounded by headroom jobs (-1 = no ring bound) minus the reserved ones.
 *  Adds the reservation to *resv and returns it. */
int nr_probe_gate_reserve(nr_probe_gate_t *g, long abs_slot, int headroom, int max, int *resv);
/** Admit one probe: from *resv when > 0, else from the live bucket (refill to abs_slot, ring_ok = fill below the bound). */
bool nr_probe_gate_take(nr_probe_gate_t *g, long abs_slot, bool ring_ok, int *resv);
/** Return the unused reservation *resv to the bucket (capped at the burst); *resv = 0. */
void nr_probe_gate_release(nr_probe_gate_t *g, int *resv);

typedef struct { uint64_t planned, probes, fulls, probe_ok, full_ok, candidates, solved, dropped; } nr_pdsch_solver_stats_t;
void nr_pdsch_solver_stats(nr_pdsch_solver_stats_t *s);

/* ---- E3 (traffic mode only): CRC-free SOLVED path from DM-RS physics, per-cell layout families, Qm ledger. Nothing here is called in legacy mode. ----
 * The solver never computes a score: the monitor scores hypotheses with nr_dl_layout_score() (E2) on the scan consumer and passes the outcome in. */
/** What proved the RNTI's layout: NR_LAYOUT_EV_NONE (not solved), NR_LAYOUT_EV_PHYS (solved by DM-RS physics, no full TB passed yet) or NR_LAYOUT_EV_CRC.
 *  (NR_LAYOUT_EV_BITS is E1's and is never returned here.) */
int nr_pdsch_solver_layout_evidence(uint16_t rnti);

/** One scored hypothesis of a grant: the layouts of a reading (ids in any order), the scrambling hypothesis, the DM-RS additional position, and E2's outcome.
 *  rank = nr_dl_score_rank() (-inf = declaration inconsistent); sig = the hypothesis is CONFIRMABLE: n_signif == n_cells > 0 and the allocation meets
 *  E2's minimum PRB count. A hypothesis that is not sig can only lose. */
typedef struct {
  const uint16_t *ids;
  int n_ids;
  int32_t scr;
  uint8_t add_pos;
  float rank;
  bool sig;
} nr_pdsch_phys_hyp_t;
/** E1 consistency of a reading for this RNTI (its bank refutes none of the reading's class): the monitor's callback; NULL = not asked. */
typedef bool (*nr_pdsch_e1_fn)(void *ctx, const uint16_t *ids, int n_ids);
/** Phys acceptance. One grant (signal slot abs_slot, a slot is counted once) of an unsolved RNTI with the E2 outcome of each hypothesis. Per RNTI a sequential
 *  sign test runs between the tracked hypotheses (at most 16): a hypothesis is the winner when it was significant on >= 10 grants and, against EVERY other tracked
 *  hypothesis, outranked it on significantly more grants than it was outranked (one-sided binomial, alpha NBS_SIGNIF_ALPHA / rivals); hypotheses with identical
 *  rank are never separated, so footprint-twins keep the RNTI unsolved. The winner must also be E1-consistent. Then the RNTI is SOLVED with evidence PHYS:
 *  layouts = the winner's reading, add_pos and scr as scored, table / S / L / rate matching unknown (learnt by CRC as for any solved RNTI), no cell seeding until a
 *  full TB passes. Returns 1 when this call solved it. dci_len is the DCI length of the grant (families are per length). */
int nr_pdsch_solver_phys_note(uint16_t rnti, uint64_t abs_slot, const nr_pdsch_phys_hyp_t *h, int n, nr_pdsch_e1_fn e1, void *e1_ctx, uint16_t dci_len);
/** Score-drop reopen. For an RNTI SOLVED by physics (no full TB passed yet): one look at its solution on a grant, scorable = E2 could confirm it (meets the
 *  minimum PRB count), hit = it was confirmed. Over the last 16 scorable looks, misses with a binomial tail (p0 = 0.25) below alpha reopen the layout. Returns true
 *  when it reopened. Other RNTIs: ignored. */
bool nr_pdsch_solver_phys_check(uint16_t rnti, bool scorable, bool hit);
/** True on every 16th call for an RNTI solved by physics (the cadence of the monitor's E1 re-check of its class); false otherwise. */
bool nr_pdsch_solver_phys_tick(uint16_t rnti);
/** The RNTI's current solution is phys-solved and not CRC-confirmed. */
bool nr_pdsch_solver_phys_unconfirmed(uint16_t rnti);

/** A layout family of the cell: the solution tuple of >= 1 CRC-confirmed promoted solved RNTIs whose DCIs have length dci_len, strongest (most members) first.
 *  Phys-solved RNTIs are not members until a full TB passed (a family of unconfirmed layouts would confirm itself). Returns the count written (<= max). */
typedef struct { int rep; uint8_t add_pos; int32_t scr; uint32_t members; } nr_pdsch_family_t;
int nr_pdsch_solver_families(uint16_t dci_len, nr_pdsch_family_t *out, int max);
/** A family exists for this DCI length and the RNTI is not solved: the monitor then checks E1 consistency and the first grant's E2 score. */
bool nr_pdsch_solver_family_match(uint16_t rnti, nr_dci_bits_t payload, uint16_t len);
/** Accept an unsolved RNTI as SOLVED (PHYS) on family f: ids = the reading of this DCI (any order) that contains f->rep, scr = the hypothesis E2 confirmed (same
 *  identity form as the family's). Re-checks that the family still exists, the RNTI is unsolved and e1 (if given) agrees. The table is NOT inherited. 1 = accepted. */
int nr_pdsch_solver_family_accept(uint16_t rnti, const nr_pdsch_family_t *f, const uint16_t *ids, int n_ids, int32_t scr, uint16_t dci_len, uint64_t abs_slot,
                                  nr_pdsch_e1_fn e1, void *e1_ctx);

/** The solution of a SOLVED RNTI as the monitor scores it: its scrambling hypothesis and DM-RS additional position. false when the RNTI is not solved. */
bool nr_pdsch_solver_solution(uint16_t rnti, int32_t *scr, uint8_t *add_pos);

/** Qm ledger (rank-1 grants of a SOLVED layout, distinct slots). qm = nr_qm_classify() (2/4/6/8); the caller passes only grants with n_signif == n_cells and
 *  at least E2's minimum PRB count; qm = 0 is no vote. A grant is informative when the MCS tables disagree on its Qm. Per MCS table (0..2): agree = distinct-slot
 *  grants whose (Qm of mcs in that table) equals qm, dissent = those where it does not. A Qm that no table gives marks the ledger poisoned for good. */
void nr_pdsch_solver_qm_note(uint16_t rnti, uint64_t abs_slot, int mcs, int qm);
/** The table the ledger fixes: exactly one table with >= 8 agreeing distinct-slot grants and ZERO dissent while every other table has dissent; else -1 (unknown).
 *  A hint only: it is a field only together with a CRC pass (nr_pdsch_solver_table_evidence). */
int nr_pdsch_solver_qm_table(uint16_t rnti);
/** Evidence of the RNTI's MCS table: 0 unknown, 1 = learnt by a CRC pass, 2 = Qm consensus AND a CRC pass with that table. */
int nr_pdsch_solver_table_evidence(uint16_t rnti);

/** Traffic mode: drop rnti's solution exactly as the 256-grant reopen does (search restarts). */
void nr_pdsch_solver_reopen(uint16_t rnti);
/** SOLVED / CANDIDATE / UNSOLVED (never FIXED); checks_ok / checks_fail (either may be NULL) = full-TB CRC passes / failures. */
nr_layout_state_t nr_pdsch_solver_layout_state(uint16_t rnti, uint32_t *checks_ok, uint32_t *checks_fail);
/** Read-and-clear: a HARQ/RV consistency violation was counted for rnti since the last take. */
bool nr_pdsch_solver_take_hrv_alert(uint16_t rnti);

#ifdef __cplusplus
}
#endif
#endif
