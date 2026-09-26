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
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */
#ifndef NR_DMRS_ID_ESTIMATE_H
#define NR_DMRS_ID_ESTIMATE_H
#include <stdbool.h>
#include <stdint.h>
#include "PHY/TOOLS/tools_defs.h" // c16_t only; the heavier PHY headers are not C++-clean and stay in the .c
#ifdef __cplusplus
extern "C" {
#endif

/* Blind estimation of the DM-RS scrambling identity (N_ID^{n_SCID}, 0..1023) of a PDSCH or
 * CP-OFDM PUSCH from received DM-RS resource elements. This is the one dedicated (RRC-configured,
 * ciphered) parameter the passive receiver has been ASSUMING equals the PCI: on this cell that
 * assumption is true, on another cell nothing would say it broke except the CRC rate collapsing.
 *
 * Method: for every candidate N_ID the type-1, port-0 pilot sequence is regenerated with the same
 * primitives the receiver's own channel estimator uses (nr_pdsch_dmrs_rx over a Gold sequence),
 * an LS estimate h_c[m] = pilot_c[m] * y[m] is formed on the comb pilots, and the score is the
 * adjacent-pilot coherence  |sum_m h_c[m] conj(h_c[m+1])| / sum_m |h_c[m]|^2.  Adjacent pilots are
 * two subcarriers apart, over which any physical channel is highly correlated, so the TRUE identity
 * sums coherently (score -> 1) while a wrong identity rotates each term by a pseudo-random QPSK
 * phase (score ~ 1/sqrt(M)). Scores accumulate across grants; a decision needs both a minimum number
 * of grants and a minimum margin, in dB, of the best candidate over the median of the current
 * candidate window (see nr_dmrs_id_set_range below) -- a relative gate, so it does not depend on
 * gain, SNR or allocation size.
 *
 * Cost: ~1024 x (gold generation + 6*nb_rb complex MACs) per grant, single-threaded, no allocation
 * after init. Never call this on the PHY receive thread; the deferred decode consumers are the
 * intended call sites.
 *
 * RANGE. scramblingID0/scramblingID1 (TS 38.211 7.4.1.1.2) are 16-bit RRC fields, 0..65535 --
 * four times NR_DMRS_ID_CANDIDATES. The state sweeps a WINDOW of the full space at a time
 * (nr_dmrs_id_set_range, default 0..1023, matching every deployment seen so far); num_r/num_i/den
 * are allocated to fit the current window rather than sized to the full range, since a 65536-wide
 * window costs 64x a 1024-wide one per grant (see nr_dmrs_id_set_range's own comment) and almost
 * every cell needs only the default window. */
#define NR_DMRS_ID_CANDIDATES 1024
#define NR_DMRS_ID_SPACE 65536 // full scramblingID0/1 range; stage 2 sweeps [NR_DMRS_ID_CANDIDATES, NR_DMRS_ID_SPACE)

typedef struct {
  /* Complex numerator sum h[m]conj(h[m-1]) and real denominator sum |h|^2, accumulated ACROSS
   * grants per candidate. Kept separate on purpose: accumulating per-grant |num|/den would be
   * biased positive (the magnitude of a random walk is never zero) and the margin would stop
   * growing with evidence; with complex accumulation a wrong candidate's numerator keeps
   * random-walking down as 1/sqrt(total pilots) while the true one adds coherently.
   * Sized to range_count (nr_dmrs_id_set_range), NOT NR_DMRS_ID_CANDIDATES -- index i here is
   * candidate id (range_first + i), never a raw id, everywhere in the .c file. */
  double  *num_r, *num_i, *den;
  uint32_t range_first;                  // physical id represented by index 0 of the arrays above
  uint32_t range_count;                  // number of candidates currently swept
  uint32_t grants;                       // grants accumulated so far IN THE CURRENT RANGE
  int      best_id;                      // -1 until decided; a PHYSICAL id (range_first + local)
  double   margin_db;                    // best over median, at decision time
  bool     decided;
  int      assumed_id;                   // what the receiver is currently using (PCI by default)
  const char *label;                     // "PDSCH" / "PUSCH", for logging only
} nr_dmrs_id_state_t;

void nr_dmrs_id_init(nr_dmrs_id_state_t *st, const char *label, int assumed_id);

/* (Re)size the sweep window to [first, first+count) and reset accumulation (grants/decided/best_id
 * all clear -- a range change is a fresh estimation problem). Called by nr_dmrs_id_init() for the
 * default window (0, NR_DMRS_ID_CANDIDATES); callers escalate to a wider/shifted window (typically
 * stage 2 = NR_DMRS_ID_CANDIDATES..NR_DMRS_ID_SPACE) only after the default window's margin gate
 * has failed with enough evidence -- see nr_pdsch_passive_queue.c's stage-2 trigger, which also
 * THROTTLES stage-2 accumulate() calls: cost scales with range_count (one gold sequence + pilot
 * regen per candidate), so a 64512-wide sweep is roughly 64x a 1024-wide one and must not run on
 * every grant. count == 0 is a no-op (a state must always have at least one candidate). */
void nr_dmrs_id_set_range(nr_dmrs_id_state_t *st, uint32_t first, uint32_t count);

/* Accumulate one DM-RS symbol of one grant.
 *   rx_symbol        : frequency-domain samples of the DM-RS OFDM symbol, ofdm_symbol_size long
 *                      (i.e. &rxdataF[ant][symbol * ofdm_symbol_size]); delta of CDM group 0 is 0
 *   start_subcarrier : absolute subcarrier index of the allocation's first RB in that array,
 *                      already including first_carrier_offset (the estimator's bwp_start_subcarrier)
 *   rb_offset        : allocation's first RB relative to the DM-RS sequence reference point
 *                      (the estimator's rb_offset: first_rb + BWPStart unless refPoint says CORESET0)
 *   nb_rb            : allocation width in RBs
 *   N_RB, symbols_per_slot, slot, symbol, nscid : as passed to nr_gold_pdsch/nr_pdsch_dmrs_rx
 *   normal_cp        : 1 for normal CP (extended CP is not supported by the pilot generator either)
 * Returns the number of candidates scored (the current window's range_count) or 0 on invalid
 * input. Works WITHOUT a CRC precondition on purpose -- the DM-RS sequence is fully determined by
 * the (nid, slot, symbol) tuple regardless of whether the payload later decodes, so requiring a
 * CRC pass first would be circular: under a wrong id the CRC never passes. */
int nr_dmrs_id_accumulate(nr_dmrs_id_state_t *st, const c16_t *rx_symbol, int ofdm_symbol_size,
                          int start_subcarrier, int rb_offset, int nb_rb, int N_RB,
                          int symbols_per_slot, int slot, int symbol, int nscid, int normal_cp);

/* Decide once enough evidence exists. Returns true exactly when the decision is first made.
 * min_margin_db is the best-over-median gate; 10 dB is comfortably above what 1023 wrong
 * candidates ever reach on a real allocation (measured: see the handover doc) and far below what
 * the true identity reaches with >= 50 pilots. */
bool nr_dmrs_id_decide(nr_dmrs_id_state_t *st, uint32_t min_grants, double min_margin_db);

/* RANK PROBE. With DM-RS type 1, ports 0 and 1 share CDM group 0 and are separated by the
 * frequency-domain OCC w_f = [+1,+1] / [+1,-1] over each comb pair (k' = 0, 1). Under the port-0
 * pilot, the pair reads (h0 + h1, h0 - h1) when both ports are present and (h0, h0) when only port
 * 0 is. The even/odd pair coherence |sum_n h[2n] conj(h[2n+1])| / sum |h|^2 is therefore ~1 for a
 * single-layer grant and collapses for a two-layer one -- a per-grant rank indicator that needs no
 * decode. Same index conventions as nr_dmrs_id_accumulate; `nid` is the (confirmed) identity. */
double nr_dmrs_port_pair_coherence(const c16_t *rx_symbol, int ofdm_symbol_size, int start_subcarrier,
                                   int rb_offset, int nb_rb, int N_RB, int symbols_per_slot, int slot,
                                   int symbol, int nscid, int nid, int normal_cp);
/// Per-PRB DM-RS coherence over the whole carrier (CRB0-referenced, port 1000, type 1): out[N_RB].
void nr_dmrs_prb_coherence(const c16_t *rx_symbol, int ofdm_symbol_size, int first_carrier_offset, int N_RB,
                           int symbols_per_slot, int slot, int symbol, int nscid, int nid, int normal_cp,
                           float *out);

/* Coherence score of one candidate, and its margin over the median of the current window, in dB.
 * `id` is a PHYSICAL candidate id; either returns 0.0 / -inf if id falls outside the current
 * window (nr_dmrs_id_set_range). */
double nr_dmrs_id_score(const nr_dmrs_id_state_t *st, int id);
double nr_dmrs_id_margin_db(const nr_dmrs_id_state_t *st, int id);

/* ---- TWO-WINDOW DRIVER (final review I5) ----------------------------------------------------------
 * Stage 1 (0..1023, every deployment seen so far) accumulates on EVERY call until an identity is
 * decided -- it is never switched off, so evidence gathered while escalation was premature (acquisition
 * garbage, CFO mis-lock, false-accept DCIs, a wrong Technique D mask reading data as DM-RS) cannot make
 * a true identity in 0..1023 undecidable, as the former one-way switch to 1024..65535 did.
 * Stage 2 (1024..65535) is armed once stage 1 has s1_grants grants without deciding, then evaluated on
 * one call in s2_throttle and at most s2_max_evals times IN TOTAL: at ~170 ms per stage-2 accumulate
 * (measured, FindsAnIdAboveTheOldRange) the cap bounds its lifetime cost to ~11 s of one consumer's CPU
 * instead of ~83 us/job forever. 64 evaluations = 4x the decide floor (16 grants); a stage-2 id that
 * cannot clear the margin gate in 64 grants is not going to. Once either stage decides, all work stops
 * (and the stage-2 arrays are freed if its budget is spent undecided). Caller-serialised (a trylock
 * around it, as before); nr_dmrs_id_2stage_decided() is safe to call unlocked from any thread. */
#define NR_DMRS_ID_STAGE1_GRANTS 64
#define NR_DMRS_ID_STAGE2_THROTTLE 2048
#define NR_DMRS_ID_STAGE2_MAX_EVALS 64
typedef struct {
  nr_dmrs_id_state_t s1;   ///< 0..1023
  nr_dmrs_id_state_t s2;   ///< 1024..65535, allocated when armed
  uint32_t s1_grants;      ///< stage-1 grants before stage 2 is armed (default NR_DMRS_ID_STAGE1_GRANTS)
  uint32_t s2_throttle;    ///< one stage-2 evaluation per this many calls (default NR_DMRS_ID_STAGE2_THROTTLE)
  uint32_t s2_max_evals;   ///< total stage-2 evaluations allowed (default NR_DMRS_ID_STAGE2_MAX_EVALS)
  uint32_t s2_tick;        ///< calls since stage 2 was armed
  uint32_t s2_evals;       ///< stage-2 evaluations made
  bool     s2_armed;
  int      decided_p1;     ///< decided identity + 1; 0 = undecided, so a zero-initialised (static,
                           ///< never-init'd) state reads as undecided. Release-published; read with _decided().
} nr_dmrs_id_2stage_t;

void nr_dmrs_id_2stage_init(nr_dmrs_id_2stage_t *t, const char *label, int assumed_id);
/* Same inputs as nr_dmrs_id_accumulate(). Returns true on the call that decides. */
bool nr_dmrs_id_2stage_accumulate(nr_dmrs_id_2stage_t *t, const c16_t *rx_symbol, int ofdm_symbol_size,
                                  int start_subcarrier, int rb_offset, int nb_rb, int N_RB, int symbols_per_slot,
                                  int slot, int symbol, int nscid, int normal_cp);
/* The decided identity, or -1 (acquire load: never exposes a half-published decision, final review M1).
 * Inline so a pure library reading it (nr_pdcch_blind_monitor.c) needs no estimator symbol. */
static inline int nr_dmrs_id_2stage_decided(const nr_dmrs_id_2stage_t *t)
{
  return t ? __atomic_load_n(&t->decided_p1, __ATOMIC_ACQUIRE) - 1 : -1;
}

#ifdef __cplusplus
}
#endif
#endif
