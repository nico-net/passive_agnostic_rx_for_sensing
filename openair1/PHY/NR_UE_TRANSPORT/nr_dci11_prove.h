/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* CRC-free proof of the DCI 1_1 pre_ant split (E1 of docs/superpowers/plans/2026-11-04-param-extraction.md).
 *
 * The cell-wide resolver (nr_pdcch_dci11_layout_sweep.c) fixes the SUM pre_ant = TB2 + HARQ pid + DAI + TPC + 2nd TPC + PRI + K1 + the
 * Rel-16/17 extras. This module splits that sum into widths from bits alone. Tests per split, over one RNTI's bank of DCI payloads:
 *   HARQ pid : consecutive grants advance the pid by 1..3 (null 3 / 2^w per pair). RELATIVE and about the pid's START only: when some reading at one start
 *              (TB2 absent/present) is significant, the splits at the other start fall; the pid WIDTH (4/5) is left to the other tests (a 5-bit reading of a
 *              4-bit pid smears like a pid, and a lowest-free-process scheduler has no +1..3 habit at all). A scheduler with arbitrary pids supports nobody,
 *              so nobody is refuted.
 *   K1       : there must be offsets c (per K1 index, per TDRA index: c = K0 + K1 absorbs the unknown K0) such that the DCIs land on UL-capable slots (anything
 *              but pure DL in the SIB1 pattern). The best c per key is taken; a split is refuted only when the DCIs no c explains are significantly more than
 *              an outlier rate of 1 % (false alarms, SPS release, reused RNTI). Abstains when the pattern is unknown.
 *   counter DAI: of two consecutive grants whose HARQ-ACK slots CAN coincide (the feasible K1 offsets of their keys allow it), the later one reads the
 *              earlier + 1 mod 4 when they do (null 1/4 per pair). RELATIVE like the HARQ test: when some split's DAI position is significantly a counter,
 *              every split that does not put a DAI field exactly there falls (a DAI-less one calling those bits TPC, a longer HARQ field swallowing them).
 *              Abstains when K1 abstains (no windows without the TDD pattern).
 *   MCS table: MCS 28 on a new TB excludes table 2 (nr_dci11_mcs28_new_tb); evidence for E2, never refutes a split.
 * Pure: no allocation, no globals, except the per-cell bank at the end (guarded by a per-cell lock). */
#ifndef NR_DCI11_PROVE_H
#define NR_DCI11_PROVE_H
#include <stdbool.h>
#include <stdint.h>
#include "nr_dci_bits.h"
#ifdef __cplusplus
extern "C" {
#endif
#include "nr_pdcch_dci11_layout_sweep.h"

/* Widths of the pre_ant sub-fields in TS 38.212 7.3.1.2.2 order; the fixed TPC (2) and PUCCH-RI (3) are implied.
 * ext = the Rel-16/17 extras in the sweep's order (one-shot, PDSCH group index, NFI, n groups, HARQ-ACK retx, enhanced type 3): they are
 * never read, so the prover only knows their SUM; in nr_dci11_prove() ext is one representative decomposition. tpc/pri are the fixed 2/3. */
typedef struct { uint8_t tb2, harq, dai, tpc, tpc2, pri, k1, ext[6]; } nr_dci11_split_t;

/* Every spec-legal split of l->pre_ant under the sweep's switch tables (extras included). Returns the number of splits (may exceed max;
 * only min(count, max) are written), or -1 on bad arguments. */
int nr_dci11_splits(const nr_dci11_layout_t *l, nr_dci11_split_t *out, int max);

#define NR_DCI11_BANK 64      /* payloads kept per RNTI */
#define NR_DCI11_CORE_MAX 128 /* upper bound on distinct (tb2, harq, dai, tpc2, k1) cores of one pre_ant: 2*2*4*2*4 = 128 */
typedef struct { nr_dci_bits_t payload; uint16_t len; int64_t abs_slot; } nr_dci11_obs_t;

/* Slot direction callback: true = UL-capable (UL or MIXED). *known = 0: the TDD pattern is unknown, the answer must be ignored. */
typedef bool (*nr_ul_slot_fn)(int64_t abs_slot, int *known);

/* Proves the split of one RNTI. bank[0..n) = payloads in any order (only the last NR_DCI11_BANK are used; entries whose len differs from
 * o->total are ignored; abs_slot = the DCI's slot). `ul` may be NULL (= unknown pattern). alive[0..] gets the surviving splits, ONE per
 * distinct (tb2, harq, dai, tpc2, k1) (those fix every extracted field; the extras are never read), chance_p[i] = the probability that a
 * random reading survives as split i does (product of the supported tests' tails; 1 = no evidence). Returns the number of survivors
 * (<= min(max, NR_DCI11_CORE_MAX)), or -1 on bad arguments. A split is PROVEN when exactly one survives and chance_p[0] < NBS_SIGNIF_ALPHA. */
int nr_dci11_prove(const nr_dci11_layout_t *l, const nr_dci11_offsets_t *o, const nr_dci11_obs_t *bank, int n, nr_ul_slot_fn ul,
                   nr_dci11_split_t *alive, double *chance_p, int max);

/* MCS-table evidence of a split: the number of DCIs of the bank that carry MCS 28 on a NEW transport block (the NDI of their HARQ pid,
 * read with split s, toggled against the previous DCI of that pid). Each excludes table 2 (qam256: MCS 28..31 are reserved there). Informs E2;
 * a missed DCI can fake a toggle, so a consumer should want more than one. -1 on bad arguments. */
int nr_dci11_mcs28_new_tb(const nr_dci11_layout_t *l, const nr_dci11_offsets_t *o, const nr_dci11_obs_t *bank, int n,
                          const nr_dci11_split_t *s);

/* K1 feasibility of a split, for tests and diagnostics: for the key (tda index, K1 index) the bitmask of offsets c (bit c, 0..47) with every
 * DCI of that key landing on a UL-capable slot; 0 = refuted, UINT64_MAX = abstained (unknown pattern or key unused). */
uint64_t nr_dci11_k1_feasible(const nr_dci11_layout_t *l, const nr_dci11_offsets_t *o, const nr_dci11_obs_t *bank, int n, nr_ul_slot_fn ul,
                              const nr_dci11_split_t *s, int tda_idx, int k1_idx);

/* ---- layout classes: what the prover reads of a layout (start and length of pre_ant, TDRA field, MCS/NDI/RV block, DCI length). An RNTI whose layout is
 * not solved is proven over every class among the resolver's alive layouts. ---- */
typedef struct { nr_dci11_layout_t l; nr_dci11_offsets_t o; } nr_dci11_class_t;
#define NR_DCI11_CLASS_MAX 16
/* The distinct classes of the alive layouts of r (one representative each). Returns their number (0 when none is alive), -2 when r->n_truncated (incomplete set), or -1 when there are more than
 * max: the caller skips the round and counts it. Reads r->alive racily (a layout dying meanwhile only adds a class). */
int nr_dci11_classes(const nr_dci11_resolver_t *r, nr_dci11_class_t *out, int max);

bool nr_dci11_class_same(const nr_dci11_layout_t *l1, const nr_dci11_offsets_t *o1, const nr_dci11_layout_t *l2, const nr_dci11_offsets_t *o2);
/* A DCI's slot in the numbering of the SIB1 TDD pattern (frame * slots_per_frame + slot), unwrapped (frames wrap at 1024) to sit next to the monotonic
 * receiver slot `mono`: same difference between DCIs as mono. -1 on bad arguments. Pure. */
int64_t nr_dci11_tdd_slot(int64_t mono, int frame, int slot, int spf);

/* Cached class set of the cell's resolver, keyed by (resolver, n_hyp, n_alive, n_truncated): a scan of the alive layouts only when the set changed.
 * Returns the number of classes (>0, written to out when non-NULL), or NR_DCI11_CLS_NONE (no alive layout / not computed because allow_compute is false: the
 * receive thread never scans), NR_DCI11_CLS_TOO_MANY (more than NR_DCI11_CLASS_MAX), NR_DCI11_CLS_INCOMPLETE (n_truncated != 0: the stored set may lack the
 * true layout, so no reading is derived). On a computed result <= 0 every stored proof of the cell is dropped (it was made for a set that no longer exists). */
#define NR_DCI11_CLS_NONE 0
#define NR_DCI11_CLS_TOO_MANY (-1)
#define NR_DCI11_CLS_INCOMPLETE (-2)
#define NR_DCI11_CLASS_COUNT_MAX 64   /* classes counted for the histogram; a prove still takes at most NR_DCI11_CLASS_MAX */
/* total (nullable): the number of distinct classes, counted up to NR_DCI11_CLASS_COUNT_MAX + 1 (= "more than 64"); 0 for an incomplete set or when not computed. */
int nr_dci11_class_set(const nr_dci11_resolver_t *r, nr_dci11_class_t *out, bool allow_compute, int *total);
/* True when rnti's bank grew by >= 16 since its last look: the cheap test that precedes everything else. */
bool nr_dci11_bank_due(uint16_t rnti);

/* ---- per-cell banks (traffic mode only). Feed: any thread (a mutex and a copy). Prove: the scan consumer, never the RX thread. ---- */
typedef struct {
  uint64_t ident;          /* cell identity key (identity_gen, PCI, carrier): another value drops every bank of the cell */
  int64_t max_gap_slots;   /* an RNTI silent for longer than this loses its ring (the UE-context expiry) */
  int64_t max_back_slots;  /* a slot this far BEHIND the newest one of the RNTI also drops it (smaller reorder across consumers is tolerated) */
} nr_dci11_feed_t;
void nr_dci11_bank_feed(uint16_t rnti, nr_dci_bits_t payload, uint16_t len, int64_t abs_slot, const nr_dci11_feed_t *f);
/* Re-proves rnti's bank when it grew by >= 16 entries since the last proof (the first proof needs 16). The bank is copied under the cell's lock and
 * proved outside it. Every class is proved; the RNTI is PROVEN when exactly one distinct reading (absolute field positions) survives over all classes,
 * with chance_p < alpha / ncls. A proof stands (hysteresis) until a later look refutes that reading. Returns true when the proven state changed. */
bool nr_dci11_bank_maybe_prove(uint16_t rnti, const nr_dci11_class_t *cls, int ncls, nr_ul_slot_fn ul);
/* The proven reading with the class it was proved in. */
bool nr_dci11_bank_proven_any(uint16_t rnti, nr_dci11_class_t *cls, nr_dci11_split_t *out);
/* The same, only when its class reads like (l, o) (a solved RNTI's representative layout). */
bool nr_dci11_bank_proven(uint16_t rnti, const nr_dci11_layout_t *l, const nr_dci11_offsets_t *o, nr_dci11_split_t *out);
/* The same with the kind of proof: scoped = proved within a CRC-solved RNTI's own class (unique in that class only, not over the resolver's set). */
bool nr_dci11_bank_proven_scope(uint16_t rnti, nr_dci11_class_t *cls, nr_dci11_split_t *out, bool *scoped);
/* One due look of an RNTI on the scan consumer (everything E1 does per accept after the feed): cheap due test, budget test, class set (own == NULL: the
 * RNTI is unsolved, proved over the resolver's classes, unscoped) or own class (solved RNTI: scoped proof), prove, charge the time to the cell's budget.
 * Budget: 20 ms per second of wall time per cell; over budget the RNTI stays due. Returns NR_DCI11_SVC_* flags; *total_classes = alive classes counted (-1: no
 * unsolved look ran or the set is incomplete). */
#define NR_DCI11_SVC_CHANGED 1u    /* the proven state changed */
#define NR_DCI11_SVC_DEFERRED 2u   /* over budget, first time since the last look of this RNTI */
#define NR_DCI11_SVC_SKIPPED 4u    /* unsolved look without a usable class set (too many classes, incomplete) */
unsigned nr_dci11_bank_service(uint16_t rnti, const nr_dci11_resolver_t *r, const nr_dci11_class_t *own, nr_ul_slot_fn ul, int *total_classes);
/* May the stored proof be used to read this DCI? proven/scoped/pc as returned by nr_dci11_bank_proven_scope; rep_l/rep_o = the solver's representative layout
 * (both NULL: the RNTI is unsolved); set_usable = the cached class set is current and usable. */
bool nr_dci11_emit_ok(bool proven, bool scoped, uint16_t dci_len, const nr_dci11_class_t *pc, const nr_dci11_layout_t *rep_l, const nr_dci11_offsets_t *rep_o,
                      bool set_usable);
void nr_dci11_set_clock(uint64_t (*fn)(void));   /* test hook: monotonic nanoseconds, NULL = CLOCK_MONOTONIC */
void nr_dci11_budget_reset(void);
/* New cell identity: forget every bank and proof of this cell. */
void nr_dci11_bank_reset(void);

#ifdef __cplusplus
}
#endif
#endif
