#ifndef NR_PDCCH_AL1_MAP_H
#define NR_PDCCH_AL1_MAP_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* AL1 CANDIDATE GEOMETRY, independent of the CORESET's CCE-to-REG mapping (TS 38.211 7.3.2.2).
 * An AL1 PDCCH candidate is one CCE = 6 REGs. Many mappings produce the SAME set of AL1 candidates:
 * with bundle size 6 the interleaver only renames which CCE a bundle is. So AL1 discovery needs only a
 * small COVER of the mapping catalogue (2-11 mappings for 81-1081, measured), and one verified AL1
 * decode pins the true mapping to the mappings whose AL1 family contains its REG set. */

typedef struct { uint8_t bundle; uint8_t interleaver; uint16_t shift; } nr_pdcch_al1_map_t; /* bundle 0 = non-interleaved */

#define NR_PDCCH_AL1_MAX_MAPS  1200 /* > 1081, the largest legal catalogue (270 RB x 2 symbols) */
#define NR_PDCCH_AL1_MAX_COVER 32   /* lane-loop bound for the cover lap; every legal shape stays below it */
#define NR_PDCCH_AL1_UNION_MAX 1088 /* >= 1080, the full-catalogue AL1 union of the largest shape (270 RB x 3 symbols) */

/** Every legal mapping of a span_rb x duration CORESET, in nr_pdcch_map_candidates()' rule order:
 *  non-interleaved, then L in {2,6} (D=1,2) / {3,6} (D=3), R in {2,3,6} with N_REG % (L*R) == 0, every
 *  shift 0..N_REG/L-1. Returns the count (capped by max_out); 0 for an illegal shape. */
int nr_pdcch_al1_enumerate(int span_rb, int duration, nr_pdcch_al1_map_t *out, int max_out);
/** The 6 REG indices (REGs numbered time-first: REG n = RB n/D, symbol n%D), ascending, of AL1 CCE
 *  `cce` under mapping m. Returns 6, or 0 when m or cce is illegal for the shape. */
int nr_pdcch_al1_regset(int span_rb, int duration, nr_pdcch_al1_map_t m, int cce, uint16_t out[6]);
/** Greedy minimal subset of nr_pdcch_al1_enumerate() whose AL1 families together contain every
 *  distinct AL1 REG set of the whole catalogue. Deterministic; non-interleaved first. */
int nr_pdcch_al1_cover(int span_rb, int duration, nr_pdcch_al1_map_t *out, int max_out);
/** In place: keep the mappings of cand[0..n) whose AL1 family contains every observed REG set.
 *  Returns the survivor count. */
int nr_pdcch_al1_narrow(int span_rb, int duration, const uint16_t (*obs)[6], int n_obs,
                        nr_pdcch_al1_map_t *cand, int n);
/** Number of distinct AL1 families among cand[0..n) (1 = AL1 decoding is exact with any of them). */
int nr_pdcch_al1_family_count(int span_rb, int duration, const nr_pdcch_al1_map_t *cand, int n);
/** In place: keep the FIRST mapping of each distinct AL1 family among cand[0..n), order preserved.
 *  Returns the kept count (= nr_pdcch_al1_family_count()). */
int nr_pdcch_al1_family_reps(int span_rb, int duration, nr_pdcch_al1_map_t *cand, int n);
/** Deduplicated union of the AL1 REG sets of cand[0..n), family-major: cand[0]'s sets first in CCE
 *  order, then each later family's NEW sets. Writes at most `max` sets (regsets may be NULL when
 *  max is 0) and returns the TOTAL distinct count, snprintf-style: > max means truncated. */
int nr_pdcch_al1_union(int span_rb, int duration, const nr_pdcch_al1_map_t *cand, int n, uint16_t (*regsets)[6], int max);
/** Demap one AL1 candidate given only its REG set: the e_rx nr_pdcch_demapping_deinterleaving() would
 *  produce for any (mapping, CCE) with that REG set (per symbol, its RBs ascending, 9 data REs each).
 *  llr/e_rx elements are 4 bytes (c16_t); llr rows are llr_stride elements per symbol.
 *  Returns 54 (REs written), or 0 when the set does not cover whole RBs. */
int nr_pdcch_al1_demap(int duration, const uint16_t regs[6], const void *llr, int llr_stride, void *e_rx);

#ifdef __cplusplus
}
#endif
#endif
