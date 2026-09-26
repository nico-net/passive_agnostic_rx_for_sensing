#ifndef NR_PDSCH_PRB_SET_H
#define NR_PDSCH_PRB_SET_H
#include <stdbool.h>
#include <stdint.h>
#include "common/utils/bits.h" // freq_alloc_bitmap_t (nr_pdsch_passive_alloc_normalise())
#ifdef __cplusplus
extern "C" {
#endif

/* Frequency-domain resource arithmetic for non-contiguous allocations. All PRB indices are
 * BWP-relative; bwp_start is the BWP's first CRB (RBG, VRB-bundle and PRG boundaries are aligned to
 * the COMMON RB grid, which is why it is needed). */

#define NR_PRB_SET_MAX 275

/** Validate a data-ordered PRB list and derive its bitmap (bitmap_words 32-bit words, written only on
 *  success) and lowest/highest PRB: the pure core of nr_pdsch_passive_alloc_normalise(). false if the
 *  list is empty or longer than NR_PRB_SET_MAX, or a PRB is outside the BWP / the bitmap, or listed twice. */
bool nr_prb_list_normalise(const uint16_t *prb, int n, int bwp_size, uint32_t *bitmap, int bitmap_words, int *first,
                           int *last);

/** Make a PRB-list allocation self-consistent: with n_prb_list > 0, re-derive first_rb/last_rb/num_rbs
 *  (= n_prb_list, the PRB COUNT) and the bitmap from prb_list via nr_prb_list_normalise() above. No-op
 *  for a legacy (contiguous) allocation. false (fa untouched) if a listed PRB is outside the BWP,
 *  listed twice, or the list is longer than NR_PRB_SET_MAX. Every producer of a list grant must pass
 *  it through this before the grant reaches the decoder, the data-aided tap or the queue probes
 *  (nr_pdsch_passive_queue_enqueue() does it for queued grants). Moved here from
 *  nr_pdsch_passive_decode.{h,c} (2026-09-27): it depends on nothing else in that file, and living
 *  here lets it be unit-tested without linking PHY_VARS_NR_UE/NFAPI. */
bool nr_pdsch_passive_alloc_normalise(freq_alloc_bitmap_t *fa, int bwp_size);
/** RBG size P (TS 38.214 Table 5.1.2.2.1-1; UL Table 6.1.2.2.1-1 is identical). 0 if out of range. */
int nr_rbg_size(int bwp_size, int rbg_config2);
/** N_RBG = ceil((N_size + (N_start mod P)) / P). */
int nr_rbg_count(int bwp_start, int bwp_size, int P);
/** PRBs of an RA type-0 bitmap (N_RBG bits, MSB = RBG 0), increasing. Returns the count. */
int nr_ra_type0_prbs(uint32_t bitmap, int bwp_start, int bwp_size, int P, uint16_t *prb, int max);
/** dynamicSwitch FDRA field (1 + max(N_RBG, riv_bits) bits): MSB 0 -> type 0, bitmap = N_RBG LSBs,
 *  returns 0; MSB 1 -> type 1, RIV = riv_bits LSBs, returns 1 (TS 38.212 7.3.1.2.2). */
int nr_fdra_dynamic_split(uint32_t field, int n_rbg, int riv_bits, uint32_t *type0_bitmap, uint32_t *riv);
/** Frequency-domain resource assignment mode of a DCI 1_1 / 0_1 (resourceAllocation x rbg-Size). */
enum { NR_FDRA_TYPE1 = 0, NR_FDRA_TYPE0_CFG1 = 1, NR_FDRA_TYPE0_CFG2 = 2, NR_FDRA_DYN_CFG1 = 3, NR_FDRA_DYN_CFG2 = 4 };
/** RBG size P a mode uses (0 for type 1). */
int nr_fdra_rbg_size(int mode, int bwp_size);
/** FDRA field width: type 1 = riv_bits, type 0 = N_RBG, dynamicSwitch = 1 + max(N_RBG, riv_bits). */
int nr_fdra_bits(int mode, int n_rbg, int riv_bits);
/** Decode an FDRA field into a data-ordered PRB list (type 1 = the RIV's contiguous range). Returns the
 *  count; 0 = impossible for the true layout (empty bitmap, RIV outside the BWP). *type0 (may be NULL) is
 *  set to 1 when the field resolved to a type-0 bitmap. */
int nr_fdra_prbs(uint32_t field, int mode, int n_rbg, int riv_bits, int bwp_start, int bwp_size, uint16_t *prb,
                 int max, int *type0);
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
/** RE gather order: for each segment in array order, the BWP-relative RE indices
 *  prb_start*re_per_prb .. (prb_start+n_prb)*re_per_prb - 1. out[i] is the source RE of data RE i.
 *  Returns the count, -1 if more than max. */
int nr_prb_gather_index(const nr_prb_seg_t *seg, int nseg, int re_per_prb, int *out, int max);

/** BWP-relative PRB index -> absolute carrier CRB (CRB0-referenced), for probes that index a
 *  whole-carrier array such as nr_dmrs_prb_coherence()'s out[] (that function fills it by absolute
 *  CRB, per its own doc comment -- it is NOT BWP-relative like everything else in this file). Every
 *  such probe's rb0 must be converted with this before use; skipping it reads the wrong PRBs on any
 *  BWP that does not start at CRB 0. bwp_start = 0 is a no-op (identity). */
int nr_dmrs_oracle_crb(int bwp_start, int rb0);

#ifdef __cplusplus
}
#endif
#endif
