#ifndef NR_PDSCH_PRB_SET_H
#define NR_PDSCH_PRB_SET_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Frequency-domain resource arithmetic for non-contiguous allocations. All PRB indices are
 * BWP-relative; bwp_start is the BWP's first CRB (RBG, VRB-bundle and PRG boundaries are aligned to
 * the COMMON RB grid, which is why it is needed). */

#define NR_PRB_SET_MAX 275

/** RBG size P (TS 38.214 Table 5.1.2.2.1-1; UL Table 6.1.2.2.1-1 is identical). 0 if out of range. */
int nr_rbg_size(int bwp_size, int rbg_config2);
/** N_RBG = ceil((N_size + (N_start mod P)) / P). */
int nr_rbg_count(int bwp_start, int bwp_size, int P);
/** PRBs of an RA type-0 bitmap (N_RBG bits, MSB = RBG 0), increasing. Returns the count. */
int nr_ra_type0_prbs(uint32_t bitmap, int bwp_start, int bwp_size, int P, uint16_t *prb, int max);
/** dynamicSwitch FDRA field (1 + max(N_RBG, riv_bits) bits): MSB 0 -> type 0, bitmap = N_RBG LSBs,
 *  returns 0; MSB 1 -> type 1, RIV = riv_bits LSBs, returns 1 (TS 38.212 7.3.1.2.2). */
int nr_fdra_dynamic_split(uint32_t field, int n_rbg, int riv_bits, uint32_t *type0_bitmap, uint32_t *riv);
/** Interleaved VRB-to-PRB mapping (TS 38.211 7.3.1.6), bundle size L in {2,4}. prb[i] is the PRB of
 *  VRB vrb_start+i, i.e. the output is in DATA order (PDSCH maps to VRBs in increasing order).
 *  For DCI 1_0 in a common search space pass bwp_start = 0, the initial-BWP size, L = 2. */
int nr_vrb_to_prb_interleaved(int bwp_start, int bwp_size, int L, int vrb_start, int n_vrb, uint16_t *prb);

/** One contiguous piece of an allocation, in data order: channel estimation and RE extraction run per
 *  segment and the segments' REs are concatenated in array order. */
typedef struct { uint16_t prb_start; uint16_t n_prb; uint16_t data_index; } nr_prb_seg_t;
/** Split a data-ordered PRB list into contiguous segments; prg > 0 also splits at CRB multiples of prg
 *  (PRB bundling: precoding may change there, so a channel estimate must not interpolate across it).
 *  prg = 0 = wideband. Returns the segment count, -1 if more than max. */
int nr_prb_segments(const uint16_t *prb, int n, int bwp_start, int prg, nr_prb_seg_t *seg, int max);

#ifdef __cplusplus
}
#endif
#endif
