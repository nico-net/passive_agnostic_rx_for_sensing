/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* CRC-free proof of the DCI 1_1 pre_ant split (E1 of docs/superpowers/plans/2026-11-04-param-extraction.md).
 *
 * The cell-wide resolver (nr_pdcch_dci11_layout_sweep.c) fixes the SUM pre_ant = TB2 + HARQ pid + DAI + TPC + 2nd TPC + PRI + K1 + the
 * Rel-16/17 extras. This module splits that sum into widths from bits alone. Tests per split, over one RNTI's bank of DCI payloads:
 *   HARQ pid : consecutive grants advance the pid by 1..3 (null 3 / 2^w per pair). RELATIVE: it refutes only when another split's pid
 *              reading is significant (a scheduler with arbitrary pids supports nobody, so nobody is refuted).
 *   K1       : there must be offsets c (per K1 index, per TDRA index: c = K0 + K1 absorbs the unknown K0) such that every DCI's feedback slot
 *              n + c is UL-capable (UL or MIXED) in the SIB1 TDD pattern. An empty feasible set refutes. Abstains when the pattern is unknown.
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

/* ---- per-cell banks (traffic mode only). Feed: any thread (a mutex and a copy). Prove: the scan consumer, never the RX thread. ---- */
void nr_dci11_bank_feed(uint16_t rnti, nr_dci_bits_t payload, uint16_t len, int64_t abs_slot);
/* Re-proves rnti's bank when it grew by >= 16 entries since the last proof (the first proof needs 16). The bank is copied under the cell's
 * lock and proved outside it. The layout/offsets are the RNTI's current representative. Returns true when the proven state changed. */
bool nr_dci11_bank_maybe_prove(uint16_t rnti, const nr_dci11_layout_t *l, const nr_dci11_offsets_t *o, nr_ul_slot_fn ul);
/* The split E1 proved for rnti under exactly this layout (pre_ant, ant_ports offset, dci length). False when nothing is proven. */
bool nr_dci11_bank_proven(uint16_t rnti, const nr_dci11_layout_t *l, const nr_dci11_offsets_t *o, nr_dci11_split_t *out);
/* New cell identity: forget every bank and proof of this cell. */
void nr_dci11_bank_reset(void);

#ifdef __cplusplus
}
#endif
#endif
