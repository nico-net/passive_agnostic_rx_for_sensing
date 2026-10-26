/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Blind zero-power CSI-RS (ZP-CSI-RS) proposer.
 *
 * A ZP-CSI-RS resource carries nothing: it is a rate-matching pattern, the REs of a TS 38.211 Table 7.4.1.5.3-1 row at a periodic
 * slot offset on which the gNB sends no PDSCH. The NZP footprint detector (nr_csirs_disc_fp.c) looks for energy combs and cannot see it.
 * This unit looks for the opposite: resource elements that are persistently EMPTY while their neighbours in the same resource block
 * and symbol carry data, in slots that follow a legal period/offset lattice and not in the other slots.
 *
 * Features (one set per ring slot, CPU nr_csirs_zp_features_cpu() or the GPU op zp_features): the energy of every subcarrier class
 * of every 4-RB tile, per symbol and RB parity, summed over the receive antennas. 4 RB is the granularity of the CSI-RS frequency
 * band (TS 38.214 5.2.2.3.1); the parity split keeps a half-density (dot5) pattern visible.
 *
 * A subcarrier class of a tile is DARK when its energy is below a quarter of the 75th percentile class of the same tile (>= 4 of the
 * 12 classes carry data in every legal row, row 18 uses 8 of 12): idle tiles are not dark (all classes equal), loaded tiles with empty
 * REs are. Dark tile counts per (symbol, parity, subcarrier) are folded modulo 640 (every legal period divides 640, TS 38.331
 * CSI-ResourcePeriodicityAndOffset) and a lattice (period, offset) is tested by the contrast of the on-lattice slots against all the
 * others (a t-like statistic of the per-slot dark-tile count): structural holes (DM-RS of a second CDM group, CORESET gaps) are as
 * frequent off the lattice as on it and are rejected by the same test.
 */
#ifndef NR_CSIRS_DISC_ZP_H
#define NR_CSIRS_DISC_ZP_H
#include "nr_csirs_disc.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NR_CSIRS_ZP_TILE_RB 4
#define NR_CSIRS_ZP_FOLD 640   /* lcm of the legal periods 4..640 */

/* floats per slot of a feature set: [symbol 14][parity 2][tile][subcarrier 12] */
size_t nr_csirs_zp_feat_len(int n_rb);
/* CPU features of ring entry idx (host grid). out holds nr_csirs_zp_feat_len() floats. */
void nr_csirs_zp_features_cpu(const nr_csirs_disc_ring_t *r, int idx, float *out);

typedef struct nr_csirs_zp_det nr_csirs_zp_det_t;
nr_csirs_zp_det_t *nr_csirs_zp_new(int n_rb);
void nr_csirs_zp_free(nr_csirs_zp_det_t *);
void nr_csirs_zp_reset(nr_csirs_zp_det_t *);
/* Folds the features of one slot (abs_slot, nr_csirs_zp_feat_len floats) into the statistics. */
void nr_csirs_zp_feed(nr_csirs_zp_det_t *, uint32_t abs_slot, const float *feat);
/* Slots folded since reset (halving of saturated residues does not reduce it). */
uint64_t nr_csirs_zp_slots(const nr_csirs_zp_det_t *);
/* Footprints of the significant hole patterns, strongest first (at most max_out), in the format of the NZP detector: sym_mask,
 * sc_mask, density (0 even RB, 1 odd RB, 2 every RB), RB extent (multiples of 4 RB, clipped to the carrier), period, offset,
 * strength (the z of the weakest symbol). Same-lattice symbols with the same subcarriers are also offered as one multi-symbol
 * footprint, so that nr_csirs_disc_hypotheses() can match the two-symbol rows. */
int nr_csirs_zp_propose(nr_csirs_zp_det_t *, nr_csirs_disc_fp_t *out, int max_out);

/* Text table of the dark cells on the lattice (period, offset): every (symbol, RB parity, subcarrier) whose dark rate on the lattice slots is >= 0.2, as
 * "l<symbol><e|o><subcarrier>=<on-lattice rate>/<off-lattice rate>". Diagnostic of what the proposer saw behind a proposal. Returns the length written. */
int nr_csirs_zp_cells(const nr_csirs_zp_det_t *, int period, int offset, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
