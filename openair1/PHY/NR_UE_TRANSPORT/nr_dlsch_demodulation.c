/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief Top-level routines for demodulating the PDSCH physical channel from 38-211, V15.2 2018-06
 */

#include "PHY/NR_UE_TRANSPORT/nr_mrc_weights.h"
#include "PHY/NR_UE_TRANSPORT/nr_agnostic_v2.h"
#include "common/platform_constants.h"
#include "nr_phy_common.h"
#include "PHY/defs_nr_UE.h"
#include "nr_transport_proto_ue.h"
#include "PHY/sse_intrin.h"
#include "T.h"
#include "bits.h"
#include "openair1/PHY/NR_UE_ESTIMATION/nr_estimation.h"
#include "PHY/NR_REFSIG/nr_refsig.h"
#include "PHY/NR_REFSIG/dmrs_nr.h"
#include "PHY/NR_REFSIG/ptrs_nr.h" // is_ptrs_subcarrier (per-layer PT-RS RE compaction)
#include "common/utils/nr/nr_common.h"
#include <stdlib.h> // atoi (ISAC_RX_MRC_MODE)
#include <math.h>   // log10 (RXBRANCH probe)
#include <complex.h>
#include <math.h>
#include "nr_pdsch_evm_acc.h"
#include "nr_irc_cov.h"
#include "openair1/PHY/TOOLS/phy_scope_interface.h"
#include "nfapi/open-nFAPI/nfapi/public_inc/nfapi_nr_interface.h"

/* SELECTION DIVERSITY across receive branches (2026-09-03).
 *
 * `nr_dlsch_forced_branch` pins the branch the rank-1 four-RX path decodes from; -1 (default)
 * leaves the ISAC_RX_MRC_MODE logic in charge, so every caller that does not set it is
 * bit-identical to before. `nr_dlsch_used_branch` reports back which branch was actually used
 * (-1 = a combining mode, i.e. no single branch).
 *
 * Thread-local, not global: nr_pdsch_passive_queue runs several consumer threads, each decoding a
 * different transport block, and a shared pin would make one consumer's retry silently change
 * another's branch mid-TB. */
__thread int nr_dlsch_forced_branch = -1;
/* Forced receive-branch MASK for the same-capture subset scan. -1 = inactive (default), so every
 * existing path is bit-identical. When set, the combiner uses ALL branches as its contiguous slice
 * and the branches absent from the mask are zeroed, which is exactly how mode 3 already excludes a
 * branch -- MRC weights by h*, so h == 0 contributes precisely zero signal and zero noise.
 * This exists so 15 antenna subsets can be replayed against ONE captured transport block: comparing
 * separate live runs cannot answer whether four branches hurt, because propagation, gain state and
 * this rig's own 5-88 % CRC swing all change between runs. */
__thread int nr_dlsch_forced_mask = -1;
__thread int nr_dlsch_mrc_mode_override = -1;
__thread int nr_dlsch_irc_override = -1; /* T26d: per-grant ISAC_RX_IRC value (-1 = env) */ /* T26d ISAC_RX_MRC_PER_RNTI: per-grant combining mode (set by the passive decode) */
__thread nr_irc_cov_t nr_irc_dmrs_cov; /* ISAC_RX_IRC=4, nr_irc_cov.h */
__thread int nr_pdsch_chest_occ_override = -1; /* T26d ISAC_CHEST_OCC_AB, nr_pdsch_passive_decode.c */
__thread int nr_pdsch_ldpc_iters_override = -1; /* T26d ISAC_LDPC_ITERS_AB (set by the PDSCH queue, read by the passive decode) */
__thread bool nr_pdsch_decode_wide_mrc; /* CRC-validated passive recovery only */
__thread nr_mmse2_diag_t nr_mmse2_diag;
__thread int nr_dlsch_eq_override = -1; /* T26d ISAC_RX_EQ_AB: multi-layer equaliser per grant, 0 fixed MMSE, 1 float MMSE, 2 ML (nl 2, Qm<=6) */
__thread int nr_dlsch_qam_mode_override = -1; /* QAM-FIX: per-decode ISAC_PDSCH_MMSE_MODE (-1 = env), set by the passive decode A/B */
__thread nr_pdsch_evm_acc_t nr_pdsch_evm_acc; /* ISAC_PDSCH_EVM_GRANT, see nr_pdsch_evm_acc.h */
/* DECODE-FIX: compare the diagnostic float ZF's QPSK signs with the actual
 * fixed-point soft path. The EVM measurement precedes channel compensation. */
__thread uint64_t nr_pdsch_decode_signs, nr_pdsch_decode_sign_errors;
__thread int nr_dlsch_chest_per_symbol = 0;
extern __thread uint32_t nr_dl_chest_nvar_ant[]; // per-branch chest noise, published by the chest on this thread // set by nr_pdsch_passive_decode when it has time-interpolated the estimate
__thread int nr_dlsch_used_branch = -1;

void nr_dlsch_force_mask(int mask)
{
  nr_dlsch_forced_mask = mask;
}

void nr_dlsch_force_branch(int ant)
{
  nr_dlsch_forced_branch = ant;
}

int nr_dlsch_last_branch(void)
{
  return nr_dlsch_used_branch;
}

/* Which branch the rank-1 four-RX path WILL decode, known before the demodulator runs, or -1 if
 * that cannot be predicted here.
 *
 * Exists so the caller can hand the equaliser the noise variance of the branch actually being
 * decoded. nvar reaches nr_rx_pdsch() as an argument, i.e. it is fixed BEFORE the branch selection
 * inside the demodulator happens, so a caller that wants them to agree has to know the choice in
 * advance. Mode 0 is deterministic (branch 0) and is the default; mode 1 picks the strongest branch
 * from channel levels this function has not seen, so it honestly answers -1 there rather than
 * guessing, and the caller keeps the cross-antenna mean.
 *
 * Why it matters, measured: at 4 antennas DL decodes branch 0 alone and scored 51-64 %, while the
 * SAME branch 0 at --ue-nb-ant-rx 1 scored 76 %. The difference is that nvar is a mean over all
 * four branches, and on this rig three of them are noise -- so the equaliser is told the channel is
 * far noisier than the branch it is actually reading, and clips the LLRs. Same defect class as the
 * UL scale bug fixed in nr_ulsch_demodulation.c. */
int nr_dlsch_planned_branch(int nbRx, int nl)
{
  if (!(nl == 1 && nbRx == 4)) {
    return -1; // the branch-restriction logic does not engage outside rank-1 four-RX
  }
  if (nr_dlsch_forced_branch >= 0 && nr_dlsch_forced_branch < nbRx) {
    return nr_dlsch_forced_branch; // the retry has pinned one
  }
  if (nr_dlsch_forced_mask >= 0) {
    /* BUG FOUND 2026-09-08: this function did not know about the subset-scan mask at all, so it
     * fell through to the ISAC_RX_MRC_MODE env check below and returned BRANCH 0 for every one of
     * the 15 subsets regardless of which branches were actually selected -- {1} alone got branch
     * 0's nvar (too optimistic for a weaker branch), {0,1,2,3} got it too (far too small for a
     * 4-way coherent sum), and only {0} happened to get the RIGHT value by coincidence. Measured
     * consequence: SUBSET read 0% on all 15 subsets while the primary decode scored 87.7% on the
     * SAME transport blocks -- impossible if the scan were faithful.
     * The existing scale-preserving substitution only has a formula for ONE branch's raw nvar
     * standing in for the mean, so it is applied only when the mask selects exactly one branch;
     * a multi-branch mask keeps the cross-antenna mean, which is what production MRC mode 2/3
     * combining already uses (covariance-aware multi-branch nvar is a separate, unstarted piece
     * of work, not silently approximated here). */
    if ((nr_dlsch_forced_mask & (nr_dlsch_forced_mask - 1)) == 0) { // exactly one bit set
      for (int b = 0; b < nbRx; b++) {
        if (nr_dlsch_forced_mask & (1 << b)) {
          return b;
        }
      }
    }
    return -1;
  }
  const char *e = getenv("ISAC_RX_MRC_MODE");
  const int mode = (e != NULL) ? atoi(e) : 0;
  return (mode == 0) ? 0 : -1;
}

// #define DEBUG_HARQ(a...) printf(a)
#define DEBUG_HARQ(...)
//#define DEBUG_DLSCH_DEMOD
//#define DEBUG_PDSCH_RX

#define print_ints(s,x) printf("%s = %d %d %d %d\n",s,(x)[0],(x)[1],(x)[2],(x)[3])
#define print_shorts(s,x) printf("%s = [%d+j*%d, %d+j*%d, %d+j*%d, %d+j*%d]\n",s,(x)[0],(x)[1],(x)[2],(x)[3],(x)[4],(x)[5],(x)[6],(x)[7])

static bool overlap_csi_symbol(fapi_nr_dl_config_csirs_pdu_rel15_t *csi_pdu, int symbol)
{
  int num_l0 [18] = {1, 1, 1, 1, 2, 1, 2, 2, 1, 2, 2, 2, 2, 2, 4, 2, 2, 4};
  for (int s = 0; s < num_l0[csi_pdu->row - 1]; s++) {
    if (symbol == csi_pdu->symb_l0 + s)
      return true;
  }
  // check also l1 if relevant
  if (csi_pdu->row == 13 || csi_pdu->row == 14 || csi_pdu->row == 16 || csi_pdu->row == 17) {
    for (int s = 0; s < 2; s++) { // two consecutive symbols including l1
      if (symbol == csi_pdu->symb_l1 + s)
        return true;
    }
  }
  return false;
}

/* csi_resource_re_map: returns the 12-bit RE map of one CSI-RS resource inside one RB that carries it (no density or RB-range test).
 *   Row of the CSI-RS table selects k0 step, k' count and multiplier; freq_domain bits pick the RB pattern. AssertFatal if the bitmap is malformed.
 *   Callers: nr_dlsch_csi_overlap_bitmap() and nr_dlsch_csi_rb_mask().
 */
static uint32_t csi_resource_re_map(const fapi_nr_dl_config_csirs_pdu_rel15_t *csi_pdu)
{
  int num_k[18] = {1, 1, 1, 1, 1, 4, 2, 2, 6, 3, 4, 4, 3, 3, 3, 4, 4, 4};
  {

    int num_kp = 1;
    int mult = 1;
    int k0_step = 0;
    int num_k0 = 1;
    switch (csi_pdu->row) {
      case 1:
        k0_step = 4;
        num_k0 = 3;
        break;
      case 2:
        break;
      case 4:
        num_kp = 2;
        mult = 4;
        k0_step = 2;
        num_k0 = 2;
        break;
      default:
        num_kp = 2;
        mult = 2;
    }
    int found = 0;
    int bit = 0;
    uint32_t temp_res_map = 0;
    while (found < num_k[csi_pdu->row - 1]) {
      if ((csi_pdu->freq_domain >> bit) & 0x01) {
        for (int k0 = 0; k0 < num_k0; k0++) {
          for (int kp = 0; kp < num_kp; kp++) {
            int re = (bit * mult) + (k0 * k0_step) + kp;
            temp_res_map |= (1 << re);
          }
        }
        found++;
      }
      bit++;
      AssertFatal(bit < 13,
                  "Couldn't find %d positive bits in bitmap %d for CSI freq. domain\n",
                  num_k[csi_pdu->row - 1],
                  csi_pdu->freq_domain);
    }
    return temp_res_map;
  }
}

/* nr_dlsch_csi_overlap_bitmap: builds the CSI-RS RE exclusion bitmap of one OFDM symbol: low 16 bits = even RBs, high 16 bits = odd RBs (12 bits each).
 *   Ors the RE map of every rate-matching CSI-RS resource whose symbols include `symbol`, placed by its frequency density.
 *   No env flag (the RB-extent refinement is in nr_dlsch_csi_rb_mask()). Callers: nr_rx_pdsch(), nr_dlsch_csi_unav_res(), nr_ssb_rate_match.c, nr_pdsch_passive_decode.c.
 */
uint32_t nr_dlsch_csi_overlap_bitmap(fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config, int symbol)
{
  // LS 16 bits for even RBs, MS 16 bits for odd RBs
  uint32_t csi_res_bitmap = 0;
  for (int i = 0; i < dlsch_config->numCsiRsForRateMatching; i++) {
    fapi_nr_dl_config_csirs_pdu_rel15_t *csi_pdu = &dlsch_config->csiRsForRateMatching[i];
    if (!overlap_csi_symbol(csi_pdu, symbol))
      continue;
    const uint32_t temp_res_map = csi_resource_re_map(csi_pdu);
    if (csi_pdu->freq_density < 2)
      csi_res_bitmap |= (temp_res_map << (16 * csi_pdu->freq_density));
    else
      csi_res_bitmap |= (temp_res_map + (temp_res_map << 16));
  }
  return csi_res_bitmap;
}

/* LOW-MCS FIX ISAC_PDSCH_CSI_RB_EXTENT=1 (default off = the bitmap above applied to EVERY allocated RB).
 * TS 38.211 7.4.1.5.3: a CSI-RS resource occupies only the RBs of its CSI-FrequencyOccupation
 * (startingRB / nrofRBs, FAPI start_rb / nr_of_rbs relative to CRB 0) with RB parity per its density;
 * TS 38.214 5.1.4.1 rate-matches the PDSCH around those REs only. The legacy bitmap dropped REs in every
 * allocated RB, e.g. a 52-RB TRS (RB 0-51) removed 12 REs/RB from RB 52-175 of a 176-RB grant: G and the
 * extraction stayed consistent with each other but not with the gNB's mapping -> CRC 0/83 at the serving
 * cell's TRS occasions OTA. Returns the 12-bit excluded-RE map of common RB `crb` on `symbol`. */
__thread int nr_dlsch_csi_extent_override = -1; /* per-decode A/B (ISAC_PDSCH_CSI_EXTENT_AB), -1 = env */
/* nr_dlsch_csi_rb_extent: returns whether CSI-RS exclusion is limited to each resource's own RB range (ISAC_PDSCH_CSI_RB_EXTENT=1, cached; default 0 = every allocated RB).
 *   The per-decode override nr_dlsch_csi_extent_override (set by the ISAC_PDSCH_CSI_EXTENT_AB diagnostic) takes precedence when >= 0.
 *   Callers: nr_dlsch_csi_rb_mask(), nr_dlsch_csi_unav_res(), nr_dlsch_extract_rbs(), nr_rx_pdsch(), and gw_fingerprint() in the passive decoder.
 */
int nr_dlsch_csi_rb_extent(void)
{
  static int s = -1;
  if (s < 0) /* CSI-FULL: ISAC_CSIRS_FULL=1 exports per-cell 52-RB TRS sets, which need their extent */
    s = (getenv("ISAC_PDSCH_CSI_RB_EXTENT") && atoi(getenv("ISAC_PDSCH_CSI_RB_EXTENT")))
        || (getenv("ISAC_CSIRS_FULL") && atoi(getenv("ISAC_CSIRS_FULL"))) ? 1 : 0;
  return nr_dlsch_csi_extent_override >= 0 ? nr_dlsch_csi_extent_override : s;
}
/* CSI-FULL: a segmented/interleaved grant is demodulated as a VIRTUAL allocation (BWP RB v = data-order PRB v);
 * the caller sets this map (virtual BWP RB -> physical BWP PRB) so the extent/parity test sees the real CRB. */
__thread const uint16_t *nr_dlsch_csi_prb_map = NULL;
__thread int nr_dlsch_csi_prb_map_n = 0;
/* nr_dlsch_csi_rb_mask: returns the 12-bit CSI-RS RE exclusion mask of one common RB on one symbol, over all rate-matching resources.
 *   With the RB-extent flag on, a resource only counts for RBs inside its start_rb..start_rb+nr_of_rbs range; RB parity is checked per density.
 *   Callers: nr_dlsch_csi_unav_res(), nr_dlsch_extract_rbs(), and the CSI-RS rate-match test (tests/nr_csi_rb_extent_check.c).
 */
uint32_t nr_dlsch_csi_rb_mask(const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config, int symbol, int crb)
{
  uint32_t m = 0;
  if (nr_dlsch_csi_prb_map) {
    const int v = crb - dlsch_config->BWPStart;
    if (v >= 0 && v < nr_dlsch_csi_prb_map_n)
      crb = nr_dlsch_csi_prb_map[v] + dlsch_config->BWPStart;
  }
  for (int i = 0; i < dlsch_config->numCsiRsForRateMatching; i++) {
    const fapi_nr_dl_config_csirs_pdu_rel15_t *csi_pdu = &dlsch_config->csiRsForRateMatching[i];
    if (!overlap_csi_symbol((fapi_nr_dl_config_csirs_pdu_rel15_t *)csi_pdu, symbol))
      continue;
    if (nr_dlsch_csi_rb_extent() && (crb < csi_pdu->start_rb || crb >= csi_pdu->start_rb + csi_pdu->nr_of_rbs))
      continue;
    if (csi_pdu->freq_density < 2 && (crb & 1) != csi_pdu->freq_density)
      continue;
    m |= csi_resource_re_map(csi_pdu);
  }
  return m;
}
/* CSI-FULL erasure (ISAC_CSIRS_FULL_ERASE=1, filled by nr_csirs_full_apply()): OTHER cells' TRS/CSI-RS in this
 * slot. Their REs carry this grant's data (no rate matching), under a power-boosted foreign reference signal;
 * the extractor zeroes those received REs so their LLRs are erasures (0) instead of confident wrong bits. */
__thread fapi_nr_dl_config_csirs_pdu_rel15_t nr_dlsch_csi_erase[NFAPI_MAX_NUM_CSI_RATEMATCH];
__thread int nr_dlsch_csi_erase_n = 0;
void nr_dlsch_csi_erase_set(const fapi_nr_dl_config_csirs_pdu_rel15_t *list, int n)
{
  nr_dlsch_csi_erase_n = 0;
  for (int i = 0; list && i < n && i < NFAPI_MAX_NUM_CSI_RATEMATCH; i++)
    nr_dlsch_csi_erase[nr_dlsch_csi_erase_n++] = list[i];
}
uint32_t nr_dlsch_csi_erase_mask(const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config, int symbol, int crb)
{
  if (nr_dlsch_csi_erase_n <= 0)
    return 0;
  if (nr_dlsch_csi_prb_map) {
    const int v = crb - dlsch_config->BWPStart;
    if (v >= 0 && v < nr_dlsch_csi_prb_map_n)
      crb = nr_dlsch_csi_prb_map[v] + dlsch_config->BWPStart;
  }
  uint32_t m = 0;
  for (int i = 0; i < nr_dlsch_csi_erase_n; i++) {
    fapi_nr_dl_config_csirs_pdu_rel15_t *csi_pdu = &nr_dlsch_csi_erase[i];
    if (!overlap_csi_symbol(csi_pdu, symbol) || crb < csi_pdu->start_rb || crb >= csi_pdu->start_rb + csi_pdu->nr_of_rbs)
      continue;
    if (csi_pdu->freq_density < 2 && (crb & 1) != csi_pdu->freq_density)
      continue;
    m |= csi_resource_re_map(csi_pdu);
  }
  return m;
}
static bool csi_erase_symbol(int symbol)
{
  for (int i = 0; i < nr_dlsch_csi_erase_n; i++)
    if (overlap_csi_symbol(&nr_dlsch_csi_erase[i], symbol))
      return true;
  return false;
}

//==============================================================================================
// Pre-processing for LLR computation
//==============================================================================================

static void nr_dlsch_channel_level_median(uint32_t rx_size_symbol,
                                          int32_t dl_ch_estimates_ext[][rx_size_symbol],
                                          int32_t median[MAX_ANT][MAX_ANT],
                                          int n_tx,
                                          int n_rx,
                                          int length)
{
  for (int aatx = 0; aatx < n_tx; aatx++) {
    for (int aarx = 0; aarx < n_rx; aarx++) {
      int64_t max = median[aatx][aarx]; // initialize the med point for max
      int64_t min = median[aatx][aarx]; // initialize the med point for min
      simde__m128i *dl_ch128 = (simde__m128i *)dl_ch_estimates_ext[aatx * n_rx + aarx];

      const int length2 = length >> 2; // length = number of REs, hence length2=nb_REs*(32/128) in SIMD loop

      for (int ii = 0; ii < length2; ii++) {
        simde__m128i norm128D =
            simde_mm_srai_epi32(simde_mm_madd_epi16(*dl_ch128, *dl_ch128), 2); //[|H_0|²/4 |H_1|²/4 |H_2|²/4 |H_3|²/4]
        int32_t *tmp = (int32_t *)&norm128D;
        int64_t norm_pack = (int64_t)tmp[0] + tmp[1] + tmp[2] + tmp[3];

        if (norm_pack > max)
          max = norm_pack;
        if (norm_pack < min)
          min = norm_pack;
        dl_ch128+=1;
      }

      median[aatx][aarx] = (max + min) >> 1;
      LOG_D(PHY, "Channel level  median [%d][%d]: %d max = %ld min = %ld\n", aatx, aarx, median[aatx][aarx], max, min);
    }
  }
}

/* nr_dlsch_csi_unav_res: counts the CSI-RS REs that nr_dlsch_extract_rbs() removes for this grant, summed over the grant's symbols and allocated RBs.
 *   Uses the same bitmaps as the extractor, so the rate-matching RE budget G and the extracted RE count agree by construction.
 *   Callers: nr_pdsch_passive_decode.c (G computation, passive_decode_body and passive_decode_retry); tests/nr_csi_rb_extent_check.c.
 */
uint32_t nr_dlsch_csi_unav_res(fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config, const freq_alloc_bitmap_t *freq_alloc)
{
  uint32_t n = 0;
  for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
    const uint32_t csi = nr_dlsch_csi_overlap_bitmap(dlsch_config, m);
    if (csi == 0)
      continue;
    const int even = __builtin_popcount(csi & 0xfff), odd = __builtin_popcount((csi >> 16) & 0xfff);
    for (int rb = 0; rb < dlsch_config->BWPSize; rb++) {
      if ((freq_alloc->bitmap[rb / 32] >> (rb % 32)) & 1)
        n += nr_dlsch_csi_rb_extent() ? __builtin_popcount(nr_dlsch_csi_rb_mask(dlsch_config, m, rb + dlsch_config->BWPStart))
                                      : ((rb + dlsch_config->BWPStart) & 1) ? odd : even;
    }
  }
  return n;
}

//==============================================================================================
// Extraction functions
//==============================================================================================

/* nr_dlsch_extract_rbs: copies the PDSCH data REs of one symbol from the FEP output into the compensation buffers, skipping DM-RS, CSI-RS and SSB REs.
 *   Per allocated RB block and antenna: data symbols use a bulk copy; otherwise an RE loop removes the overlap map (DM-RS | CSI-RS | SSB).
 *   Also fills the channel estimates for the same REs and returns the packed number of REs (offset) written. Caller: nr_rx_pdsch().
 */
static uint32_t nr_dlsch_extract_rbs(uint32_t rxdataF_sz,
                                 c16_t rxdataF[][rxdataF_sz],
                                 uint32_t rx_size_symbol,
                                 uint32_t pdsch_est_size,
                                 int32_t dl_ch_estimates[][pdsch_est_size],
                                 c16_t rxdataF_ext[][rx_size_symbol],
                                 int32_t dl_ch_estimates_ext[][rx_size_symbol],
                                 unsigned char symbol,
                                 uint8_t pilots,
                                 const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                 const freq_alloc_bitmap_t *freq_alloc,
                                 uint8_t Nl,
                                 NR_DL_FRAME_PARMS *fp,
                                 uint32_t csi_res_bitmap,
                                 int chest_time_type,
                                 const nr_ssb_rm_mask_t *ssb_rm)
{
  int config_type = dlsch_config->dmrsConfigType;
  int n_dmrs_cdm_groups = dlsch_config->n_dmrs_cdm_groups;
  if (config_type == NFAPI_NR_DMRS_TYPE1)
    AssertFatal(n_dmrs_cdm_groups == 1
                || n_dmrs_cdm_groups == 2,
                "n_dmrs_cdm_groups %d is illegal\n",
                n_dmrs_cdm_groups);
  else
    AssertFatal(n_dmrs_cdm_groups == 1
                || n_dmrs_cdm_groups == 2 
                || n_dmrs_cdm_groups == 3,
                "n_dmrs_cdm_groups %d is illegal\n",
                n_dmrs_cdm_groups);

  uint32_t dmrs_rb_bitmap = 0;
  if (pilots) {
    dmrs_rb_bitmap = 0xfff; // all REs taken by dmrs
    if (config_type == NFAPI_NR_DMRS_TYPE1 && n_dmrs_cdm_groups == 1)
      dmrs_rb_bitmap = 0x555; // alternating REs starting from 0
    if (config_type == NFAPI_NR_DMRS_TYPE2 && n_dmrs_cdm_groups == 1)
      dmrs_rb_bitmap = 0xc3;  // REs 0,1 and 6,7
    if (config_type == NFAPI_NR_DMRS_TYPE2 && n_dmrs_cdm_groups == 2)
      dmrs_rb_bitmap = 0x3cf;  // REs 0,1,2,3 and 6,7,8,9
  }

  // csi_res_bitmap LS 16 bits for even RBs, MS 16 bits for odd RBs
  uint32_t csi_res_even = csi_res_bitmap & 0xfff;
  uint32_t csi_res_odd = (csi_res_bitmap >> 16) & 0xfff;
  AssertFatal((dmrs_rb_bitmap & csi_res_even) == 0, "DMRS RE overlapping with CSI RE, it shouldn't happen\n");
  AssertFatal((dmrs_rb_bitmap & csi_res_odd) == 0, "DMRS RE overlapping with CSI RE, it shouldn't happen\n");
  uint32_t dmrs_csi_overlap_even = csi_res_even | dmrs_rb_bitmap;
  uint32_t dmrs_csi_overlap_odd = csi_res_odd | dmrs_rb_bitmap;
  int8_t validDmrsEst;
  if (nr_dlsch_chest_per_symbol)
    validDmrsEst = symbol; // passive path filled every data symbol's slot by time interpolation
  else if (chest_time_type == 0)
    validDmrsEst = get_valid_dmrs_idx_for_channel_est(dlsch_config->dlDmrsSymbPos, symbol);
  else
    validDmrsEst = get_next_dmrs_symbol_in_slot(dlsch_config->dlDmrsSymbPos, 0, 14); // get first dmrs symbol index

  int pos = 0;
  int block_start, block_end;
  int offset = 0;
  while (find_next_rb_block(freq_alloc->bitmap, dlsch_config->BWPSize, &pos, &block_start, &block_end)) {
    int start_rb = block_start + dlsch_config->BWPStart;
    int nb_rb = block_end - block_start + 1;
    const int start_re = (fp->first_carrier_offset + start_rb * NR_NB_SC_PER_RB) % fp->ofdm_symbol_size;
    for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++) {
      c16_t *rxF_ext = rxdataF_ext[aarx] + offset;
      c16_t *rxF = &rxdataF[aarx][symbol * fp->ofdm_symbol_size];
      for (int l = 0; l < Nl; l++) {
        int32_t *dl_ch0 = &dl_ch_estimates[(l * fp->nb_antennas_rx) + aarx][validDmrsEst * fp->ofdm_symbol_size];
        int32_t *dl_ch0_ext = dl_ch_estimates_ext[(l * fp->nb_antennas_rx) + aarx] + offset;
        if (pilots == 0 && csi_res_bitmap == 0 && (!ssb_rm || !(((ssb_rm->symbols | ssb_rm->symbols2) >> symbol) & 1))
            && !csi_erase_symbol(symbol)) { // data symbol only
          if (l == 0) {
            if (start_re + nb_rb * NR_NB_SC_PER_RB <= fp->ofdm_symbol_size) {
              memcpy(rxF_ext, &rxF[start_re], nb_rb * NR_NB_SC_PER_RB * sizeof(int32_t));
            } else {
              int neg_length = fp->ofdm_symbol_size - start_re;
              int pos_length = nb_rb * NR_NB_SC_PER_RB - neg_length;
              memcpy(rxF_ext, &rxF[start_re], neg_length * sizeof(int32_t));
              memcpy(&rxF_ext[neg_length], rxF, pos_length * sizeof(int32_t));
            }
          }
          memcpy(dl_ch0_ext, dl_ch0, nb_rb * NR_NB_SC_PER_RB * sizeof(int32_t));
        } else {
          int j = 0;
          int k = start_re;
          for (int rb = start_rb; rb < start_rb + nb_rb; rb++) {
            uint32_t overlap_map = rb % 2 ?  dmrs_csi_overlap_odd : dmrs_csi_overlap_even;
            if (csi_res_bitmap && nr_dlsch_csi_rb_extent())
              overlap_map = dmrs_rb_bitmap | nr_dlsch_csi_rb_mask(dlsch_config, symbol, rb);
            overlap_map |= nr_ssb_rm_excluded(ssb_rm, symbol, rb - dlsch_config->BWPStart);
            const uint32_t erase_map = nr_dlsch_csi_erase_mask(dlsch_config, symbol, rb) & ~overlap_map; /* CSI-FULL */
            for (int re = 0; re < NR_NB_SC_PER_RB; re++) {
              if (((overlap_map >> re) & 0x01) == 0) {
                // DATA RE
                if (l == 0)
                  rxF_ext[j] = ((erase_map >> re) & 1) ? (c16_t){0, 0} : rxF[k];
                dl_ch0_ext[j] = dl_ch0[re];
                j++;
              }
              k++;
              if (k >= fp->ofdm_symbol_size)
                k -= fp->ofdm_symbol_size;
            }
            dl_ch0 += 12;
          }
        }
      }
    }
    if (ssb_rm) {
      /* Pack across allocation blocks as well as within each block. */
      for (int rb = start_rb; rb < start_rb + nb_rb; ++rb) {
        uint32_t excluded = ((csi_res_bitmap && nr_dlsch_csi_rb_extent())
                                 ? (dmrs_rb_bitmap | nr_dlsch_csi_rb_mask(dlsch_config, symbol, rb))
                                 : (rb % 2 ? dmrs_csi_overlap_odd : dmrs_csi_overlap_even))
                          | nr_ssb_rm_excluded(ssb_rm, symbol, rb - dlsch_config->BWPStart);
        offset += 12 - __builtin_popcount(excluded);
      }
    } else {
      offset += nb_rb * NR_NB_SC_PER_RB;
    }
  }
  return offset;
}

/* GPU-DEMOD (nr_pdsch_gpu_demod_host.c): the RE geometry nr_dlsch_extract_rbs() packs for one symbol, from index-coded
 * inputs (rx[s * N + k] = {k, 1}, est[s * N + j] = {j, s + 1}) on one antenna and one layer. */
uint32_t nr_dlsch_extract_geometry(const NR_DL_FRAME_PARMS *fp, int chest_time, c16_t *rx_idx, uint32_t est_sz, int32_t *est_idx,
                                   uint32_t ext_sz, c16_t *out_y, int32_t *out_h, unsigned char symbol,
                                   fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg, const freq_alloc_bitmap_t *fa, const nr_ssb_rm_mask_t *ssb_rm)
{
  static __thread NR_DL_FRAME_PARMS *f1; /* the extractor reads only these three fields (the full copy is ~700 kB) */
  if (!f1 && !(f1 = calloc(1, sizeof(*f1))))
    return UINT32_MAX;
  f1->first_carrier_offset = fp->first_carrier_offset;
  f1->ofdm_symbol_size = fp->ofdm_symbol_size;
  f1->nb_antennas_rx = 1;
  return nr_dlsch_extract_rbs(fp->samples_per_slot_wCP, (c16_t(*)[fp->samples_per_slot_wCP])rx_idx, ext_sz, est_sz, (int32_t(*)[est_sz])est_idx,
                              (c16_t(*)[ext_sz])out_y, (int32_t(*)[ext_sz])out_h, symbol, (cfg->dlDmrsSymbPos >> symbol) & 1, cfg, fa, 1, f1,
                              nr_dlsch_csi_overlap_bitmap(cfg, symbol), chest_time, ssb_rm);
}

/* Zero Forcing Rx function: nr_a_sum_b()
 * Compute the complex addition x=x+y
 *
 * */
void nr_a_sum_b(c16_t *input_x, c16_t *input_y, unsigned short nb_rb)
{
  unsigned short rb;
  simde__m128i *x = (simde__m128i *)input_x;
  simde__m128i *y = (simde__m128i *)input_y;

  for (rb=0; rb<nb_rb; rb++) {
    x[0] = simde_mm_adds_epi16(x[0], y[0]);
    x[1] = simde_mm_adds_epi16(x[1], y[1]);
    x[2] = simde_mm_adds_epi16(x[2], y[2]);
    x += 3;
    y += 3;
  }
}

/* Zero Forcing Rx function: nr_element_sign()
 * Compute b=sign*a */
static inline void nr_element_sign(c16_t *a, c16_t *b, unsigned short nb_rb, int32_t sign)
{
  const int16_t nr_sign[8] __attribute__((aligned(16))) = {-1, -1, -1, -1, -1, -1, -1, -1};
  simde__m128i *a_128,*b_128;

  a_128 = (simde__m128i *)a;
  b_128 = (simde__m128i *)b;

  for (int rb = 0; rb < 3 * nb_rb; rb++) {
    if (sign < 0)
      b_128[rb] = simde_mm_sign_epi16(a_128[rb], ((simde__m128i *)nr_sign)[0]);
    else
      b_128[rb] = a_128[rb];

#ifdef DEBUG_DLSCH_DEMOD
    print_shorts("b:", (int16_t *)b_128);
#endif
  }
}

/* Zero Forcing Rx function: nr_det_4x4()
 * Compute the matrix determinant for 4x4 Matrix
 *
 * */
static void nr_determin(int size,
                        c16_t *a44[][size], //
                        c16_t *ad_bc, // ad-bc
                        unsigned short nb_rb,
                        int32_t sign,
                        int32_t shift0)
{
  AssertFatal(size > 0, "impossible null size in nr_determin");

  if(size==1) {
    nr_element_sign(a44[0][0], // a
                    ad_bc, // b
                    nb_rb,
                    sign);
  } else {
    int16_t k, rr[size - 1], cc[size - 1];
    c16_t outtemp[12 * nb_rb] __attribute__((aligned(32)));
    c16_t outtemp1[12 * nb_rb] __attribute__((aligned(32)));
    c16_t *sub_matrix[size - 1][size - 1];
    for (int rtx=0;rtx<size;rtx++) {//row calculation for determin
      int ctx=0;
      //find the submatrix row and column indices
      k=0;
      for(int rrtx=0;rrtx<size;rrtx++)
        if(rrtx != rtx) rr[k++] = rrtx;
      k=0;
      for(int cctx=0;cctx<size;cctx++)
        if(cctx != ctx) cc[k++] = cctx;
      // fill out the sub matrix corresponds to this element

      for (int ridx = 0; ridx < (size - 1); ridx++)
        for (int cidx = 0; cidx < (size - 1); cidx++)
          sub_matrix[cidx][ridx] = a44[cc[cidx]][rr[ridx]];

      nr_determin(size - 1,
                  sub_matrix, // a33
                  outtemp,
                  nb_rb,
                  ((rtx & 1) == 1 ? -1 : 1) * ((ctx & 1) == 1 ? -1 : 1) * sign,
                  shift0);
      mult_complex_vectors(a44[ctx][rtx], outtemp, rtx == 0 ? ad_bc : outtemp1, sizeofArray(outtemp1), shift0);

      if (rtx != 0)
        nr_a_sum_b(ad_bc, outtemp1, nb_rb);
    }
  }
}

static double complex nr_determin_cpx(int32_t size, // size
                                      double complex a44_cpx[][size], //
                                      int32_t sign)
{
  double complex outtemp, outtemp1;
  //Allocate the submatrix elements
  DevAssert(size > 0);
  if(size==1) {
    return (a44_cpx[0][0] * sign);
  }else {
    double complex sub_matrix[size - 1][size - 1];
    int16_t k, rr[size - 1], cc[size - 1];
    outtemp1 = 0;
    for (int rtx=0;rtx<size;rtx++) {//row calculation for determin
      int ctx=0;
      //find the submatrix row and column indices
      k=0;
      for(int rrtx=0;rrtx<size;rrtx++)
        if(rrtx != rtx) rr[k++] = rrtx;
      k=0;
      for(int cctx=0;cctx<size;cctx++)
        if(cctx != ctx) cc[k++] = cctx;
      //fill out the sub matrix corresponds to this element
       for (int ridx=0;ridx<(size-1);ridx++)
         for (int cidx=0;cidx<(size-1);cidx++)
           sub_matrix[cidx][ridx] = a44_cpx[cc[cidx]][rr[ridx]];

       outtemp = nr_determin_cpx(size - 1,
                                 sub_matrix, // a33
                                 ((rtx & 1) == 1 ? -1 : 1) * ((ctx & 1) == 1 ? -1 : 1) * sign);
       outtemp1 += a44_cpx[ctx][rtx] * outtemp;
    }

    return((double complex)outtemp1);
  }
}

/* Zero Forcing Rx function: nr_matrix_inverse()
 * Compute the matrix inverse and determinant up to 4x4 Matrix
 *
 * */
uint8_t nr_matrix_inverse(int32_t size,
                          c16_t *a44[][size], // Input matrix//conjH_H_elements[0]
                          c16_t *inv_H_h_H[][size], // Inverse
                          c16_t *ad_bc, // determin
                          unsigned short nb_rb,
                          int32_t flag, // fixed point or floating flag
                          int32_t shift0)
{
  DevAssert(size > 1);
  int16_t k,rr[size-1],cc[size-1];

  if(flag) {//fixed point SIMD calc.
    //Allocate the submatrix elements
    c16_t *sub_matrix[size - 1][size - 1];

    //Compute Matrix determinant
    nr_determin(size,
                a44, //
                ad_bc, // determinant
                nb_rb,
                +1,
                shift0);
    //print_shorts("nr_det_",(int16_t*)&ad_bc[0]);

    //Compute Inversion of the H^*H matrix
    /* For 2x2 MIMO matrix, we compute
     * *        |(conj_H_00xH_00+conj_H_10xH_10)   (conj_H_00xH_01+conj_H_10xH_11)|
     * * H_h_H= |                                                                 |
     * *        |(conj_H_01xH_00+conj_H_11xH_10)   (conj_H_01xH_01+conj_H_11xH_11)|
     * *
     * *inv(H_h_H) =(1/det)*[d  -b
     * *                     -c  a]
     * **************************************************************************/
    for (int rtx=0;rtx<size;rtx++) {//row
      k=0;
      for(int rrtx=0;rrtx<size;rrtx++)
        if(rrtx != rtx) rr[k++] = rrtx;
      for (int ctx=0;ctx<size;ctx++) {//column
        k=0;
        for(int cctx=0;cctx<size;cctx++)
          if(cctx != ctx) cc[k++] = cctx;

        //fill out the sub matrix corresponds to this element
        for (int ridx=0;ridx<(size-1);ridx++)
          for (int cidx=0;cidx<(size-1);cidx++)
            // To verify
            sub_matrix[cidx][ridx]=a44[cc[cidx]][rr[ridx]];

        nr_determin(size - 1, // size
                    sub_matrix,
                    inv_H_h_H[rtx][ctx], // out transpose
                    nb_rb,
                    ((rtx & 1) == 1 ? -1 : 1) * ((ctx & 1) == 1 ? -1 : 1),
                    shift0);
      }
    }
  }
  else {//floating point calc.
    //Allocate the submatrix elements
    double complex sub_matrix_cpx[size - 1][size - 1];
    //Convert the IQ samples (in Q15 format) to float complex
    double complex a44_cpx[size][size];
    double complex inv_H_h_H_cpx[size][size];
    double complex determin_cpx;
    for (int i=0; i<12*nb_rb; i++) {

      //Convert Q15 to floating point
      for (int rtx=0;rtx<size;rtx++) {//row
        for (int ctx=0;ctx<size;ctx++) {//column
          a44_cpx[ctx][rtx] =
              ((double)(a44[ctx][rtx])[i].r) / (1 << (shift0 - 1)) + I * ((double)(a44[ctx][rtx])[i].i) / (1 << (shift0 - 1));
        }
      }
      //Compute Matrix determinant (copy real value only)
      determin_cpx = nr_determin_cpx(size,
                                     a44_cpx, //
                                     +1);
      //if (i<4) printf("order %d nr_det_cpx = %lf+j%lf \n",log2_approx(creal(determin_cpx)),creal(determin_cpx),cimag(determin_cpx));

      //Round and convert to Q15 (Out in the same format as Fixed point).
      if (creal(determin_cpx)>0) {//determin of the symmetric matrix is real part only
        ((short *)ad_bc)[i << 1] = (short)((creal(determin_cpx) * (1 << (shift0))) + 0.5); //
      } else {
        ((short *)ad_bc)[i << 1] = (short)((creal(determin_cpx) * (1 << (shift0))) - 0.5); //
      }
      //Compute Inversion of the H^*H matrix (normalized output divide by determinant)
      for (int rtx=0;rtx<size;rtx++) {//row
        k=0;
        for(int rrtx=0;rrtx<size;rrtx++)
          if(rrtx != rtx) rr[k++] = rrtx;
        for (int ctx=0;ctx<size;ctx++) {//column
          k=0;
          for(int cctx=0;cctx<size;cctx++)
            if(cctx != ctx) cc[k++] = cctx;

          //fill out the sub matrix corresponds to this element
          for (int ridx=0;ridx<(size-1);ridx++)
            for (int cidx=0;cidx<(size-1);cidx++)
              sub_matrix_cpx[cidx][ridx] = a44_cpx[cc[cidx]][rr[ridx]];

          inv_H_h_H_cpx[rtx][ctx] = nr_determin_cpx(size - 1, // size,
                                                    sub_matrix_cpx, //
                                                    ((rtx & 1) == 1 ? -1 : 1) * ((ctx & 1) == 1 ? -1 : 1));
          //if (i==0) printf("H_h_H(r%d,c%d)=%lf+j%lf --> inv_H_h_H(%d,%d) = %lf+j%lf \n",rtx,ctx,creal(a44_cpx[ctx*size+rtx]),cimag(a44_cpx[ctx*size+rtx]),ctx,rtx,creal(inv_H_h_H_cpx[rtx*size+ctx]),cimag(inv_H_h_H_cpx[rtx*size+ctx]));

          if (creal(inv_H_h_H_cpx[rtx][ctx]) > 0)
            inv_H_h_H[rtx][ctx][i].r = (short)((creal(inv_H_h_H_cpx[rtx][ctx]) * (1 << (shift0 - 1))) + 0.5); // Convert to Q 18
          else
            inv_H_h_H[rtx][ctx][i].r = (short)((creal(inv_H_h_H_cpx[rtx][ctx]) * (1 << (shift0 - 1))) - 0.5); //

          if (cimag(inv_H_h_H_cpx[rtx][ctx]) > 0)
            inv_H_h_H[rtx][ctx][i].i = (short)((cimag(inv_H_h_H_cpx[rtx][ctx]) * (1 << (shift0 - 1))) + 0.5); //
          else
            inv_H_h_H[rtx][ctx][i].i = (short)((cimag(inv_H_h_H_cpx[rtx][ctx]) * (1 << (shift0 - 1))) - 0.5); //

          //if (i<4) printf("inv_H_h_H_FP(%d,%d)= %d+j%d \n",ctx,rtx, ((short *) inv_H_h_H[rtx*size+ctx])[i<<1],((short *) inv_H_h_H[rtx*size+ctx])[(i<<1)+1]);
        }
      }
    }
  }
  return(0);
}

/* Zero Forcing Rx function: nr_conjch0_mult_ch1()
 *
 *
 * */
// TODO: This function is just a wrapper, can be removed.
void nr_conjch0_mult_ch1(c16_t *ch0, c16_t *ch1, c16_t *ch0conj_ch1, unsigned short nb_rb, unsigned char output_shift0)
{
  //This function is used to compute multiplications in H_hermitian * H matrix
  mult_cpx_conj_vector(ch0, ch1, ch0conj_ch1, 12 * nb_rb, output_shift0);
}

/* nr_dlsch_mmse: multi-layer MMSE receiver for up to 4 layers: solves (H^H H + noise I) x = H^H y per RE and computes the per-layer LLR thresholds from the determinant.
 *   Env: ISAC_PDSCH_MMSE_MODE=2 (unbiased ZF with per-layer scale), ISAC_MMSE_FLOAT=1 (float solver), ISAC_MMSE_DIAG=1 (per-layer EVM log).
 *   Output is the layer-equalised symbols rxdataF_comp and the magnitude thresholds dl_ch_mag*. Caller: nr_rx_pdsch(), used when Nl > 2, or Nl == 2 without the ML LLR (ISAC_RX_ML_2L).
 */
static void nr_dlsch_mmse(uint32_t pdsch_buf_size_max,
                          uint32_t rx_size_symbol,
                          unsigned char n_rx,
                          unsigned char nl, // number of layer
                          c16_t rxdataF_comp[nl][pdsch_buf_size_max],
                          c16_t dl_ch_mag[][pdsch_buf_size_max],
                          c16_t dl_ch_magb[][pdsch_buf_size_max],
                          c16_t dl_ch_magr[][pdsch_buf_size_max],
                          int32_t dl_ch_estimates_ext[][rx_size_symbol],
                          unsigned char mod_order,
                          int shift,
                          int length,
                          uint32_t noise_var,
                          int qam_mode)
{
  uint32_t nb_rb_0 = (length + 11) / 12;
  c16_t determ_fin[12 * nb_rb_0] __attribute__((aligned(32)));

  ///Allocate H^*H matrix elements and sub elements
  c16_t conjH_H_elements_data[n_rx][nl][nl][12 * nb_rb_0];
  memset(conjH_H_elements_data, 0, sizeof(conjH_H_elements_data));
  c16_t *conjH_H_elements[n_rx][nl][nl];
  for (int aarx = 0; aarx < n_rx; aarx++)
    for (int rtx = 0; rtx < nl; rtx++)
      for (int ctx = 0; ctx < nl; ctx++)
        conjH_H_elements[aarx][rtx][ctx] = conjH_H_elements_data[aarx][rtx][ctx];

  //Compute H^*H matrix elements and sub elements:(1/2^log2_maxh)*conjH_H_elements
  for (int rtx = 0; rtx < nl; rtx++) {//row
    for (int ctx = 0; ctx < nl; ctx++) {//column
      for (int aarx = 0; aarx < n_rx; aarx++)  {
        c16_t *ch0r = (c16_t *)dl_ch_estimates_ext[rtx * n_rx + aarx];
        c16_t *ch0c = (c16_t *)dl_ch_estimates_ext[ctx * n_rx + aarx];
        nr_conjch0_mult_ch1(ch0r,
                            ch0c,
                            conjH_H_elements[aarx][ctx][rtx], // sic
                            nb_rb_0,
                            shift);
        if (aarx != 0)
          nr_a_sum_b(conjH_H_elements[0][ctx][rtx], conjH_H_elements[aarx][ctx][rtx], nb_rb_0);
      }
    }
  }

  // Add noise_var such that: H^h * H + noise_var * I
  if (noise_var != 0) {
    simde__m128i nvar_128i = simde_mm_set1_epi32(noise_var >> 3);
    for (int p = 0; p < nl; p++) {
      simde__m128i *conjH_H_128i = (simde__m128i *)conjH_H_elements[0][p][p];
      for (int k = 0; k < 3 * nb_rb_0; k++) {
        conjH_H_128i[0] = simde_mm_add_epi32(conjH_H_128i[0], nvar_128i);
        conjH_H_128i++;
      }
    }
  }

  /* QAM-FIX ISAC_PDSCH_MMSE_MODE=2: unbiased ZF per RE with PER-LAYER soft scale.
   * x_l = [G^-1 H^H y]_l is unit gain (E[x_l | x] = x_l, no MMSE shrinkage), its noise variance is
   * sigma^2 [G^-1]_ll, so the max-log LLR of layer l scales with w_l = 1/[G^-1]_ll (post-ZF SNR of
   * that layer). Output z_l = w_l x_l and thresholds w_l * n1/n2/n3 per layer (dl_ch_mag[l]), the
   * same form nr_channel_compensation() gives one layer (z = |h|^2 x, mag = |h|^2 n1). G and H^H y
   * carry the same 2^-shift, so x_l is in constellation units and w_l in the 1-layer |h|^2 units. */
  if (qam_mode == 2) {
    const int16_t qa = (mod_order == 4) ? QAM16_n1 : (mod_order == 6) ? QAM64_n1 : (mod_order == 8) ? QAM256_n1 : 0;
    const int16_t qb = (mod_order == 6) ? QAM64_n2 : (mod_order == 8) ? QAM256_n2 : 0;
    const int16_t qr = (mod_order == 8) ? QAM256_n3 : 0;
    /* LLRZERO ISAC_PDSCH_MMSE2_GAIN=N (default 0 = off): w and z are rounded to int16 in |h|^2 >> shift units, which sit at a few LSB
     * for the weak layer. A common gain 2^k, k = clamp(N - shift, 0, 12) (one value per grant: shift is per grant), scales z, mag, magb and magr
     * of ALL layers alike, so only the rounding grid changes, never the layer weighting. */
    static int s_g2 = -1;
    if (s_g2 < 0) s_g2 = getenv("ISAC_PDSCH_MMSE2_GAIN") ? atoi(getenv("ISAC_PDSCH_MMSE2_GAIN")) : 0;
    const int gk = s_g2 > 0 ? (s_g2 - shift < 0 ? 0 : s_g2 - shift > 12 ? 12 : s_g2 - shift) : 0;
    const double mmse2_gain = (double)(1 << gk);
    if (nr_mmse2_diag.on) {
      nr_mmse2_diag.shift = shift;
      nr_mmse2_diag.nl = nl;
      nr_mmse2_diag.qm = mod_order;
      nr_mmse2_diag.gain_k = gk;
    }
    for (int i = 0; i < length; i++) {
      double complex M[4][9]; /* [G | b | I] */
      const int nc = 2 * nl + 1;
      for (int r = 0; r < nl; r++) {
        for (int c = 0; c < nl; c++) {
          const c16_t g = conjH_H_elements[0][c][r][i]; /* (H^H H)[r][c] is stored at [c][r] */
          M[r][c] = (double)g.r + I * (double)g.i;
          M[r][nl + 1 + c] = (r == c) ? 1.0 : 0.0;
        }
        M[r][nl] = (double)rxdataF_comp[r][i].r + I * (double)rxdataF_comp[r][i].i;
      }
      bool singular = false;
      for (int k = 0; k < nl && !singular; k++) {
        int piv = k;
        for (int r = k + 1; r < nl; r++)
          if (cabs(M[r][k]) > cabs(M[piv][k]))
            piv = r;
        if (cabs(M[piv][k]) < 1e-9) {
          singular = true;
          break;
        }
        if (piv != k)
          for (int c = 0; c < nc; c++) {
            const double complex t = M[k][c];
            M[k][c] = M[piv][c];
            M[piv][c] = t;
          }
        const double complex inv = 1.0 / M[k][k];
        for (int c = 0; c < nc; c++)
          M[k][c] *= inv;
        for (int r = 0; r < nl; r++) {
          if (r == k)
            continue;
          const double complex f = M[r][k];
          if (f == 0.0)
            continue;
          for (int c = 0; c < nc; c++)
            M[r][c] -= f * M[k][c];
        }
      }
      for (int r = 0; r < nl; r++) {
        const double dinv = singular ? 0.0 : creal(M[r][nl + 1 + r]);
        double w = dinv > 1e-12 ? 1.0 / dinv : 0.0;
        w *= mmse2_gain; /* LLRZERO ISAC_PDSCH_MMSE2_GAIN: per-grant power-of-two common gain, 1.0 = off (exact) */
        const double complex o = w > 0.0 ? M[r][nl] * w : 0.0;
#define QF_S16(v) ((int16_t)((v) > 32767.0 ? 32767 : (v) < -32768.0 ? -32768 : lround(v)))
        rxdataF_comp[r][i].r = QF_S16(creal(o));
        rxdataF_comp[r][i].i = QF_S16(cimag(o));
        const double ma = w * qa / 32768.0, mb = w * qb / 32768.0, mr = w * qr / 32768.0;
        dl_ch_mag[r][i].r = dl_ch_mag[r][i].i = QF_S16(ma);
        dl_ch_magb[r][i].r = dl_ch_magb[r][i].i = QF_S16(mb);
        dl_ch_magr[r][i].r = dl_ch_magr[r][i].i = QF_S16(mr);
        if (nr_mmse2_diag.on && r < 4) {
          nr_mmse2_diag_t *d = &nr_mmse2_diag;
          d->n[r]++;
          d->sing[r] += singular;
          d->w0[r] += (w <= 0.0);
          d->z0[r] += (rxdataF_comp[r][i].r == 0 && rxdataF_comp[r][i].i == 0);
          d->mag0[r] += (dl_ch_mag[r][i].r == 0);
          d->sat[r] += (fabs(creal(o)) > 32767.0 || fabs(cimag(o)) > 32767.0);
          d->sumw[r] += w;
          d->sumabsz[r] += fabs((double)rxdataF_comp[r][i].r) + fabs((double)rxdataF_comp[r][i].i);
        }
#undef QF_S16
      }
    }
    return;
  }

  /* ISAC_MMSE_FLOAT=1 (default off = stock fixed-point path): solve (H^H H + nvar I) x = H^H y
   * per RE in double, output the unit-gain estimate at a fixed amplitude A and set the LLR
   * thresholds from that same A. Written while chasing the rank-4 0 % CRC (2026-09-16); the real
   * causes turned out to be the rfsim channel (nb_tx sized from the UE's own TX count, see
   * simulator.cpp) and an ill-conditioned scene -- on a well-conditioned 4x4 channel the fixed-point
   * adjugate path below decodes 100 % too. Kept opt-in as the A/B reference: it is immune to the
   * det(G) ~ (|h|^2 >> shift)^4 int16 saturation and puts the LLRs on a scale that does not clip
   * (fixed-point: 87 % of 256QAM LLRs on the int8 rail; here 20 %). Scalar per RE, a few ms per
   * 273-PRB rank-4 slot. */
  static int s_mmse_float = -1;
  if (s_mmse_float < 0)
    s_mmse_float = (getenv("ISAC_MMSE_FLOAT") != NULL) ? atoi(getenv("ISAC_MMSE_FLOAT")) : 0;
  if (nr_dlsch_eq_override >= 0 ? nr_dlsch_eq_override == 1 : s_mmse_float > 0) {
    const int16_t qa = (mod_order == 4) ? QAM16_n1 : (mod_order == 6) ? QAM64_n1 : (mod_order == 8) ? QAM256_n1 : 0;
    const int16_t qb = (mod_order == 6) ? QAM64_n2 : (mod_order == 8) ? QAM256_n2 : 0;
    const int16_t qr = (mod_order == 8) ? QAM256_n3 : 0;
    for (int i = 0; i < length; i++) {
      double complex M[nl][nl + 1]; // augmented [G | z]
      double diag = 0.0;
      for (int r = 0; r < nl; r++) {
        for (int c = 0; c < nl; c++) {
          const c16_t g = conjH_H_elements[0][c][r][i]; // (H^H H)[r][c] is stored at [c][r]
          M[r][c] = (double)g.r + I * (double)g.i;
        }
        M[r][nl] = (double)rxdataF_comp[r][i].r + I * (double)rxdataF_comp[r][i].i;
        diag += creal(M[r][r]);
      }
      bool singular = false;
      for (int k = 0; k < nl && !singular; k++) {
        int piv = k;
        for (int r = k + 1; r < nl; r++)
          if (cabs(M[r][k]) > cabs(M[piv][k]))
            piv = r;
        if (cabs(M[piv][k]) < 1e-9) {
          singular = true;
          break;
        }
        if (piv != k)
          for (int c = 0; c <= nl; c++) {
            const double complex t = M[k][c];
            M[k][c] = M[piv][c];
            M[piv][c] = t;
          }
        const double complex inv = 1.0 / M[k][k];
        for (int c = 0; c <= nl; c++)
          M[k][c] *= inv;
        for (int r = 0; r < nl; r++) {
          if (r == k)
            continue;
          const double complex f = M[r][k];
          if (f == 0.0)
            continue;
          for (int c = 0; c <= nl; c++)
            M[r][c] -= f * M[k][c];
        }
      }
      /* Output amplitude. The estimate is unit-gain, so A sets the LLR scale directly: the outermost
       * 16QAM LLR is 3A/sqrt(10) and the decoder input rail is int8, so A = 128 keeps every
       * constellation inside +-127 (256QAM outer bit 0.54A = 69, inner step 2A/sqrt(170) = 20 LSB).
       * tr(G)/nl (~1000 here) put 88 % of the 256QAM LLRs on the rail -> 7.6 % CRC at MCS 25.
       * ponytail: fixed scale, no per-layer SINR weighting (all layers share dl_ch_mag[0]); add
       * per-layer 1/[G^-1]_rr weighting only if a rank-4 link is measured to be SINR-limited. */
      const double A = 128.0;
      (void)diag;
      /* MMSEDIAG (ISAC_MMSE_DIAG=1): per-layer EVM of the unit-gain estimate against the nearest
       * 16/64/256-QAM grid point, one line per 200 calls. Tells "equaliser output is a constellation"
       * from "it is noise" without any downstream stage in the way. */
      {
        static __thread int s_md = -1;
        static __thread unsigned long s_mdn = 0;
        static __thread double s_err[4], s_pow[4];
        if (s_md < 0)
          s_md = (getenv("ISAC_MMSE_DIAG") != NULL) ? 1 : 0;
        if (s_md && !singular && mod_order >= 4) {
          const int lev = 1 << (mod_order / 2 - 1); // 16QAM: 2 levels per axis, 64: 4, 256: 8
          const double step = 2.0 / sqrt((2.0 / 3.0) * (double)((1 << mod_order) - 1)); // odd-integer grid spacing
          for (int r = 0; r < nl; r++) {
            const double re = creal(M[r][nl]), im = cimag(M[r][nl]);
            double best = 1e30;
            for (int a = -lev; a < lev; a++)
              for (int b = -lev; b < lev; b++) {
                const double dr = re - (2 * a + 1) * step / 2, di = im - (2 * b + 1) * step / 2;
                const double d = dr * dr + di * di;
                if (d < best) best = d;
              }
            s_err[r] += best;
            s_pow[r] += re * re + im * im;
          }
          if (i == length - 1 && (s_mdn++ % 200) == 0) {
            LOG_I(PHY, "SENSING: MMSEDIAG nl=%d Qm=%d A=%.0f evm%%=[%.1f %.1f %.1f %.1f] pow=[%.2f %.2f %.2f %.2f]\n", nl,
                  mod_order, A, 100.0 * sqrt(s_err[0] / (s_pow[0] + 1e-30)), 100.0 * sqrt(s_err[1] / (s_pow[1] + 1e-30)),
                  100.0 * sqrt(s_err[2] / (s_pow[2] + 1e-30)), 100.0 * sqrt(s_err[3] / (s_pow[3] + 1e-30)),
                  s_pow[0] / length, s_pow[1] / length, s_pow[2] / length, s_pow[3] / length);
          }
          if (i == length - 1)
            memset(s_err, 0, sizeof(s_err)), memset(s_pow, 0, sizeof(s_pow));
        }
      }
      for (int r = 0; r < nl; r++) {
        const double complex o = singular ? 0.0 : M[r][nl] * A;
        const double re = creal(o), im = cimag(o);
        rxdataF_comp[r][i].r = (int16_t)(re > 32767.0 ? 32767 : re < -32768.0 ? -32768 : lround(re));
        rxdataF_comp[r][i].i = (int16_t)(im > 32767.0 ? 32767 : im < -32768.0 ? -32768 : lround(im));
      }
      const int32_t a16 = (A > 32767.0) ? 32767 : (int32_t)lround(A);
      const int16_t ma = (int16_t)((a16 * qa + 16384) >> 15), mb = (int16_t)((a16 * qb + 16384) >> 15),
                    mr = (int16_t)((a16 * qr + 16384) >> 15);
      dl_ch_mag[0][i].r = dl_ch_mag[0][i].i = ma;
      dl_ch_magb[0][i].r = dl_ch_magb[0][i].i = mb;
      dl_ch_magr[0][i].r = dl_ch_magr[0][i].i = mr;
    }
    return;
  }

  //Compute the inverse and determinant of the H^*H matrix
  //Allocate the inverse matrix
  c16_t *inv_H_h_H[nl][nl];
  c16_t inv_H_h_H_data[nl][nl][12 * nb_rb_0];
  memset(inv_H_h_H_data, 0, sizeof(inv_H_h_H_data));
  for (int rtx = 0; rtx < nl; rtx++)
    for (int ctx = 0; ctx < nl; ctx++)
      inv_H_h_H[ctx][rtx] = inv_H_h_H_data[ctx][rtx];

  int fp_flag = 1;//0: float point calc 1: Fixed point calc
  nr_matrix_inverse(nl,
                    conjH_H_elements[0], // Input matrix
                    inv_H_h_H, // Inverse
                    determ_fin, // determin
                    nb_rb_0,
                    fp_flag, // fixed point flag
                    shift - (fp_flag == 1 ? 1 : 0)); // the out put is Q15

  // multiply Matrix inversion pf H_h_H by the rx signal vector
  c16_t outtemp[12 * nb_rb_0] __attribute__((aligned(32)));
  //Allocate rxdataF for zforcing out
  c16_t rxdataF_zforcing[nl][12 * nb_rb_0];
  memset(rxdataF_zforcing, 0, sizeof(rxdataF_zforcing));

  for (int rtx = 0; rtx < nl; rtx++) {//Output Layers row
    // loop over Layers rtx=0,...,N_Layers-1
    for (int ctx = 0; ctx < nl; ctx++) { // column multi
      // printf("Computing r_%d c_%d\n",rtx,ctx);
      // print_shorts(" H_h_H=",(int16_t*)&conjH_H_elements[ctx*nl+rtx][0][0]);
      // print_shorts(" Inv_H_h_H=",(int16_t*)&inv_H_h_H[ctx*nl+rtx][0]);
      mult_complex_vectors(inv_H_h_H[ctx][rtx],
                           rxdataF_comp[ctx],
                           outtemp,
                           sizeofArray(outtemp),
                           shift - (fp_flag == 1 ? 1 : 0));
      nr_a_sum_b(rxdataF_zforcing[rtx], outtemp, nb_rb_0); // a = a + b
    }
#ifdef DEBUG_DLSCH_DEMOD
    printf("Computing layer_%d \n", rtx);
    print_shorts(" Rx signal:=", (int16_t*)&rxdataF_zforcing[rtx][0]);
    print_shorts(" Rx signal:=", (int16_t*)&rxdataF_zforcing[rtx][4]);
    print_shorts(" Rx signal:=", (int16_t*)&rxdataF_zforcing[rtx][8]);
#endif
  }

  //Copy zero_forcing out to output array
  for (int rtx = 0; rtx < nl; rtx++)
    nr_element_sign(rxdataF_zforcing[rtx], rxdataF_comp[rtx], nb_rb_0, +1);

  //Update LLR thresholds with the Matrix determinant
  simde__m128i *dl_ch_mag128_0=NULL,*dl_ch_mag128b_0=NULL,*dl_ch_mag128r_0=NULL,*determ_fin_128;
  simde__m128i mmtmpD2,mmtmpD3;
  simde__m128i QAM_amp128={0},QAM_amp128b={0},QAM_amp128r={0};
  short nr_realpart[8]__attribute__((aligned(16))) = {1,0,1,0,1,0,1,0};
  determ_fin_128      = (simde__m128i *)&determ_fin[0];

  if (mod_order > 2) {
    if (mod_order == 4) {
      QAM_amp128 = simde_mm_set1_epi16(QAM16_n1);  //2/sqrt(10)
      QAM_amp128b = simde_mm_setzero_si128();
      QAM_amp128r = simde_mm_setzero_si128();
    } else if (mod_order == 6) {
      QAM_amp128  = simde_mm_set1_epi16(QAM64_n1); //4/sqrt{42}
      QAM_amp128b = simde_mm_set1_epi16(QAM64_n2); //2/sqrt{42}
      QAM_amp128r = simde_mm_setzero_si128();
    } else if (mod_order == 8) {
      QAM_amp128 = simde_mm_set1_epi16(QAM256_n1); //8/sqrt{170}
      QAM_amp128b = simde_mm_set1_epi16(QAM256_n2);//4/sqrt{170}
      QAM_amp128r = simde_mm_set1_epi16(QAM256_n3);//2/sqrt{170}
    }
    dl_ch_mag128_0 = (simde__m128i *)dl_ch_mag[0];
    dl_ch_mag128b_0 = (simde__m128i *)dl_ch_magb[0];
    dl_ch_mag128r_0 = (simde__m128i *)dl_ch_magr[0];

    for (int rb = 0; rb < 3 * nb_rb_0; rb++) {
      //for symmetric H_h_H matrix, the determinant is only real values
      mmtmpD2 = simde_mm_sign_epi16(determ_fin_128[0],*(simde__m128i*)&nr_realpart[0]);//set imag part to 0
      mmtmpD3 = simde_mm_shufflelo_epi16(mmtmpD2,SIMDE_MM_SHUFFLE(2,3,0,1));
      mmtmpD3 = simde_mm_shufflehi_epi16(mmtmpD3,SIMDE_MM_SHUFFLE(2,3,0,1));
      mmtmpD2 = simde_mm_add_epi16(mmtmpD2,mmtmpD3);

      dl_ch_mag128_0[0] = mmtmpD2;
      dl_ch_mag128b_0[0] = mmtmpD2;
      dl_ch_mag128r_0[0] = mmtmpD2;

      dl_ch_mag128_0[0] = simde_mm_mulhrs_epi16(dl_ch_mag128_0[0], QAM_amp128);
      dl_ch_mag128b_0[0] = simde_mm_mulhrs_epi16(dl_ch_mag128b_0[0],QAM_amp128b);
      dl_ch_mag128r_0[0] = simde_mm_mulhrs_epi16(dl_ch_mag128r_0[0],QAM_amp128r);

      determ_fin_128 += 1;
      dl_ch_mag128_0 += 1;
      dl_ch_mag128b_0 += 1;
      dl_ch_mag128r_0 += 1;
    }
  }
}

static void nr_dlsch_layer_demapping(const uint8_t Nl,
                                     const uint8_t mod_order,
                                     const int llrLayerSize,
                                     const int16_t llr_layers[NR_SYMBOLS_PER_SLOT][Nl][llrLayerSize],
                                     const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                     const uint32_t re_len[NR_SYMBOLS_PER_SLOT],
                                     int16_t *llr)
{
  const int s0 = dlsch_config->start_symbol;
  const int s1 = dlsch_config->number_symbols;
  int k = 0;

  for (int i = s0; i < (s0 + s1); i++) {
    int16_t *p_layer[Nl];
    for (int l = 0; l < Nl; l++)
      p_layer[l] = (int16_t *)llr_layers[i][l];
    nr_layer_demapping(Nl, mod_order, re_len[i], p_layer, llr + k);
    k += re_len[i] * mod_order * Nl;
  }
}

/* nr_dlsch_llr: computes the bit LLRs of one symbol for all layers from the equalised symbols, using the QAM order (QPSK, 16, 64, 256QAM).
 *   Magnitude thresholds are per layer when mag_per_layer is set (QAM-FIX mode 2), otherwise layer 0's are used for all layers.
 *   Caller: nr_rx_pdsch(). Layer-to-codeword demapping is done afterwards by nr_layer_demapping().
 */
static int nr_dlsch_llr(const NR_UE_DLSCH_t *dlsch,
                        const int len,
                        const int pdsch_buf_size_max,
                        const c16_t dl_ch_mag_l[][pdsch_buf_size_max],
                        const c16_t dl_ch_magb_l[][pdsch_buf_size_max],
                        const c16_t dl_ch_magr_l[][pdsch_buf_size_max],
                        const bool mag_per_layer, /* QAM-FIX mode 2: thresholds per layer, else layer 0's for all */
                        const int nb_antennas_rx,
                        const c16_t rxdataF_comp[dlsch->cw_info.Nl][pdsch_buf_size_max],
                        const int llrSize,
                        int16_t layer_llr[dlsch->cw_info.Nl][llrSize])
{
#define MAG_L(a, l) ((a)[mag_per_layer ? (l) : 0])
  switch (dlsch->cw_info.qamModOrder) {
    case 2 :
      for (int l = 0; l < dlsch->cw_info.Nl; l++)
        nr_qpsk_llr(rxdataF_comp[l], layer_llr[l], len);
      break;

    case 4 :
      for (int l = 0; l < dlsch->cw_info.Nl; l++)
        nr_16qam_llr(rxdataF_comp[l], MAG_L(dl_ch_mag_l, l), layer_llr[l], len);
      break;

    case 6 :
      for(int l=0; l < dlsch->cw_info.Nl; l++)
        nr_64qam_llr(rxdataF_comp[l], MAG_L(dl_ch_mag_l, l), MAG_L(dl_ch_magb_l, l), layer_llr[l], len);
      break;

    case 8:
      for(int l=0; l < dlsch->cw_info.Nl; l++)
        nr_256qam_llr(rxdataF_comp[l], MAG_L(dl_ch_mag_l, l), MAG_L(dl_ch_magb_l, l), MAG_L(dl_ch_magr_l, l), layer_llr[l], len);
      break;
#undef MAG_L

    default:
      AssertFatal(false, "Unknown mod_order!!!!\n");
      break;
  }

  return 0;
}
//==============================================================================================

/* Main Function */

/* nr_rx_pdsch: demodulates one OFDM symbol of a PDSCH grant: extract data REs, channel compensation (MRC or per-branch), MMSE or ML for multi-layer, LLRs.
 *   Env flags read here: ISAC_RX_MRC_MODE, ISAC_RX_BRANCH_MIN_DB, ISAC_RX_ML_2L, ISAC_PDSCH_MMSE_MODE, ISAC_RX_BEST_BY_SNR, ISAC_RX_BRANCH, ISAC_L2MAXH_*,
 *   ISAC_RX_MRC_ALIGN, ISAC_RX_IRC (+ _SHRINK), ISAC_RX_QAM_AGC, ISAC_PDSCH_EVM_GRANT, ISAC_PDSCH_DECODE_DIAG, ISAC_SUBSET_DIAG.
 *   Writes LLRs into llr and the valid-RE counts into dl_valid_re; the PTRS phase is estimated afterwards on the same symbols.
 *   Per-branch state (which receiver branch decoded) is kept in thread-locals. Callers: phy_procedures_nr_ue.c (regular PDSCH) and nr_pdsch_passive_decode.c (passive decode, once per symbol).
 */
int nr_rx_pdsch(PHY_VARS_NR_UE *ue,
                const UE_nr_rxtx_proc_t *proc,
                NR_UE_DLSCH_t *dlsch,
                const freq_alloc_bitmap_t *freq_alloc,
                fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                NR_DL_UE_HARQ_t *dlsch_harq,
                unsigned char symbol,
                bool first_symbol_flag,
                unsigned char harq_pid,
                uint32_t pdsch_est_size,
                int32_t dl_ch_estimates[][pdsch_est_size],
                int16_t *llr,
                uint32_t dl_valid_re[NR_SYMBOLS_PER_SLOT],
                c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                int32_t *log2_maxh,
                uint32_t pdsch_buf_size_max,
                int nbRx,
                c16_t rxdataF_comp[][NR_MAX_NB_LAYERS][pdsch_buf_size_max],
                c16_t dl_ch_mag[][NR_MAX_NB_LAYERS][pdsch_buf_size_max],
                c16_t dl_ch_magb[][NR_MAX_NB_LAYERS][pdsch_buf_size_max],
                c16_t dl_ch_magr[][NR_MAX_NB_LAYERS][pdsch_buf_size_max],
                c16_t ptrs_phase_per_slot[][NR_SYMBOLS_PER_SLOT],
                int32_t ptrs_re_per_slot[][NR_SYMBOLS_PER_SLOT],
                uint32_t nvar,
                pdsch_scope_req_t *scope_req,
                c16_t rho_dl[][NR_MAX_NB_LAYERS * NR_MAX_NB_LAYERS][pdsch_buf_size_max],
                const nr_ssb_rm_mask_t *ssb_rm)
{
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const int nl = dlsch->cw_info.Nl;
  /* ---- Four-RX rank-one receive-branch selection (ISAC_RX_MRC_MODE) --------------------------
   * The original code decoded BRANCH 0 UNCONDITIONALLY ("four-RX rank-one compatibility mode ...
   * avoiding overflow in the fixed-point MRC accumulator"). On this passive rig that is a latent
   * ~6 dB trapdoor: PASSIVE_RX_ONLY_HANDOVER.md §12.3 measured the four receive branches at
   * 0 / -26.1 / -18.2 / -7.4 dB on one occasion and -6.2 / -17.2 / -7.9 / 0 dB on another -- i.e.
   * branch 0 is NOT always the live one. §12.1 measured rank 1 sitting only ~1 dB above the 16QAM
   * R=0.64 waterfall, so a run that happens to put branch 0 in the weak position falls off the
   * cliff entirely. That is the shape of the 0 % / ~90 % PDSCH bimodality §13 recorded as
   * "cause UNKNOWN".
   *   0 = branch 0 only (the previous behaviour, kept so the A/B is one env var)
   *   1 = strongest branch (removes the trapdoor; costs the MRC gain)
   *   2 = MRC across all branches (default)
   * The selection is decided once per transport block, at first_symbol_flag, and must persist
   * across the remaining symbols -- hence thread-local state rather than a local: the deferred
   * decode runs several consumer threads concurrently, each on a different TB. */
  /* Which branch this TB actually decoded from, for the caller's retry bookkeeping. Thread-local
   * for the same reason as the state below: several consumer threads decode different TBs at once. */
  static __thread int t_mrc_nb_rx = 0;
  static __thread int t_mrc_rx_index = 0;
  static __thread int t_mrc_live_mask = 0xF; // which receive branches feed the combiner (mode 3)
  static __thread int t_mrc_weighted = 0;    // V2: noise-weighted MRC active for this TB
  static __thread int16_t t_mrc_wq15[4] = {32767, 32767, 32767, 32767};
  static __thread double t_mrc_w[4] = {1, 1, 1, 1};
  static __thread int t_mrc_mode = -1;
  static __thread int t_mrc_env = -1; /* T26d: ISAC_RX_MRC_MODE as configured; t_mrc_mode = per-grant effective mode */
  static __thread double t_mrc_min_db = 12.0;
  if (t_mrc_env >= 0) /* already resolved: apply the per-grant override (nr_dlsch_mrc_mode_override, -1 = none) */
    t_mrc_mode = nr_dlsch_mrc_mode_override >= 0 ? nr_dlsch_mrc_mode_override : t_mrc_env;
  if (t_mrc_mode < 0) {
    const char *e = getenv("ISAC_RX_MRC_MODE");
    /* DEFAULT 0 -- MEASURED, not inherited. A 2x4 sweep on the live cell (modes alternated so
     * run-to-run drift is shared) gave: mode 0 = 48.9 % and 82.6 % PDSCH CRC; modes 1, 2 and 3 =
     * 0.0 % on every one of six runs. Post-equalisation EVM stayed flat at 47-60 % across ALL of
     * them, so the failures are not constellation quality.
     *
     * Mode 1 is the same code path as mode 0 with only a different antenna INDEX, and it decodes
     * nothing even though branch 3 reports the HIGHEST |h| -- so the other branches are not merely
     * weak, they are not coherently USABLE. The X410 puts channels 0/1 on RF daughterboard A and
     * 2/3 on board B; a residual per-board frequency offset would leave branch 3 with a strong
     * DM-RS-symbol estimate whose phase then rotates across the 13 data symbols -- exactly
     * "high |h|, zero decodes" -- and would equally corrupt any coherent sum containing it (modes 2
     * and 3). That makes §12.3's "+6 dB of MRC gain" UNAVAILABLE on this rig until the boards are
     * frequency-aligned or the antennas moved onto one board: hardware, not code. Keep the knob and
     * the RXBRANCH probe for that work; do not re-enable a combining mode by assumption. */
    t_mrc_mode = (e != NULL) ? atoi(e) : 0;
    t_mrc_env = t_mrc_mode;
    if (nr_dlsch_mrc_mode_override >= 0) t_mrc_mode = nr_dlsch_mrc_mode_override;
    const char *d = getenv("ISAC_RX_BRANCH_MIN_DB");
    if (d != NULL) {
      t_mrc_min_db = atof(d);
    }
  }
  if (!(nl == 1 && nbRx == 4)) {
    t_mrc_nb_rx = nbRx;
    t_mrc_rx_index = 0;
  } else if (t_mrc_nb_rx == 0) {
    t_mrc_nb_rx = 1; // until first_symbol_flag decides; never leave it 0
    t_mrc_rx_index = 0;
  }
  int mrc_nb_rx = t_mrc_nb_rx;
  int mrc_rx_index = t_mrc_rx_index;
  const int matrixSz = nbRx * nl;
  const uint32_t rx_size_symbol = (freq_alloc->num_rbs * NR_NB_SC_PER_RB + 15) & ~15;
  __attribute__((aligned(32))) int32_t dl_ch_estimates_ext[matrixSz][rx_size_symbol];

  // Use ML-based LLR for 2-layer MIMO with QPSK/16QAM/64QAM (nl==2, qamModOrder<=6).
  // Controlled by ue->do_ml (set via -E flag in dlsim, or ue->do_ml in the UE struct).
  // When false (default), MMSE equalization is used for all configurations.
  bool do_ml = ue->do_ml;
  { /* ISAC_RX_ML_2L=1 (T26d): joint ML LLRs for 2 layers (Qm <= 6) instead of MMSE; per grant via ISAC_RX_EQ_AB */
    static int s_ml = -1;
    if (s_ml < 0) s_ml = getenv("ISAC_RX_ML_2L") ? atoi(getenv("ISAC_RX_ML_2L")) : 0;
    if (nr_dlsch_eq_override >= 0) do_ml = nr_dlsch_eq_override == 2;
    else if (s_ml) do_ml = true;
  }
  if (rho_dl == NULL) do_ml = false; /* no inter-layer correlation buffer (passive decode allocates it on request) */
  /* QAM-FIX ISAC_PDSCH_MMSE_MODE (multi-layer MMSE path only; default 0 = stock):
   *   0 stock fixed-point MMSE: adj(G + nvar/8 I) H^H y against det(G + nvar/8 I) thresholds (biased for QAM)
   *   1 fixed-point ZF: same adjugate arithmetic without the diagonal loading (unbiased: z = det(G) x)
   *   2 float ZF with per-layer soft scale (see nr_dlsch_mmse) */
  int qam_mode = 0;
  {
    static int s_qm = -1;
    if (s_qm < 0) s_qm = getenv("ISAC_PDSCH_MMSE_MODE") ? atoi(getenv("ISAC_PDSCH_MMSE_MODE")) : 0;
    qam_mode = nr_dlsch_qam_mode_override >= 0 ? nr_dlsch_qam_mode_override : s_qm;
    if (qam_mode < 0 || qam_mode > 2) qam_mode = 0;
    /* Measured (QAMFIX.md): at 3-4 layers ZF noise enhancement costs low-SNR QPSK (16 -> 9 of 227 paired) and
     * the fixed-point ZF determinant underflows (kappa 0); keep the stock MMSE there. QAM at any rank and 2-layer
     * QPSK (1046 -> 1079 of 2110 paired) use the selected mode. */
    if (nl >= 3 && (qam_mode == 1 || dlsch->cw_info.qamModOrder == 2)) qam_mode = 0;
    if (nr_dlsch_eq_override == 1) qam_mode = 0; /* the ISAC_RX_EQ_AB float arm keeps its own path */
  }

  // Reinterpret flat dl_ch_estimates_ext as [nl][nbRx][rx_size_symbol]
  c16_t(*chFext)[nbRx][rx_size_symbol] = (void *)dl_ch_estimates_ext;

  c16_t *p_rxComp[nl];
  for (int l = 0; l < nl; l++)
    p_rxComp[l] = rxdataF_comp[symbol][l];

  NR_UE_COMMON *common_vars  = &ue->common_vars;
  const int frame = proc->frame_rx;
  const int nr_slot_rx = proc->nr_slot_rx;
  const int gNB_id = proc->gNB_id;
  uint8_t slot = 0;

  uint32_t nb_re_pdsch = -1;
  DevAssert(dlsch_harq);

  if (gNB_id > 2) {
    LOG_E(PHY, "Illegal gNB_id %d\n", gNB_id);
    return(-1);
  }

  if (!common_vars) {
    LOG_E(PHY, "dlsch_demodulation.c: Null common_vars\n");
    return(-1);
  }

  if(symbol > fp->symbols_per_slot >> 1)
    slot = 1;

  uint8_t pilots = (dlsch_config->dlDmrsSymbPos >> symbol) & 1;
  uint8_t config_type = dlsch_config->dmrsConfigType;

  const bool need_rho = do_ml ? (nl == 2 && dlsch_config->cw_info->qamModOrder <= 6) : false;

  //----------------------------------------------------------
  //--------------------- RBs extraction ---------------------
  //----------------------------------------------------------
  const bool meas_enabled = cpumeas(CPUMEAS_GETSTATE);
  int nb_rb_pdsch = freq_alloc->num_rbs;

  start_meas_nr_ue_phy(ue, DLSCH_EXTRACT_RBS_STATS);
  __attribute__((aligned(32))) c16_t rxdataF_ext[nbRx][rx_size_symbol];
  memset(rxdataF_ext, 0, sizeof(rxdataF_ext));

  uint32_t csi_res_bitmap = nr_dlsch_csi_overlap_bitmap(dlsch_config, symbol);
  LOG_D(PHY, "%d.%d symbol %d csi overlap bitmap %d\n", frame, nr_slot_rx, symbol, csi_res_bitmap);

  const uint32_t nb_re_ext = nr_dlsch_extract_rbs(fp->samples_per_slot_wCP,
                                                  rxdataF,
                                                  rx_size_symbol,
                                                  pdsch_est_size,
                                                  dl_ch_estimates,
                                                  rxdataF_ext,
                                                  dl_ch_estimates_ext,
                                                  symbol,
                                                  pilots,
                                                  dlsch_config,
                                                  freq_alloc,
                                                  nl,
                                                  fp,
                                                  csi_res_bitmap,
                                                  ue->chest_time,
                                                  ssb_rm);
  stop_meas_nr_ue_phy(ue, DLSCH_EXTRACT_RBS_STATS);
  if (scope_req->copy_chanest_to_scope) {
    size_t size = sizeof(c16_t) * nb_rb_pdsch * NR_NB_SC_PER_RB;
    int copy_index = symbol - dlsch_config->start_symbol;
    int offset = copy_index * size;
    UEscopeCopyUnsafe(ue, pdschChanEstimates, dl_ch_estimates_ext[0], size, offset, copy_index);
  }
  if (meas_enabled) {
    LOG_D(PHY,
          "[AbsSFN %u.%d] Slot%d Symbol %d: Pilot/Data extraction %5.2f \n",
          frame,
          nr_slot_rx,
          slot,
          symbol,
          ue->phy_cpu_stats.cpu_time_stats[DLSCH_EXTRACT_RBS_STATS].p_time / (cpuf * 1000.0));
  }
  if (ue->phy_sim_pdsch_rxdataF_ext)
    memcpy(ue->phy_sim_pdsch_rxdataF_ext + symbol * sizeof(rxdataF_ext), rxdataF_ext, sizeof(rxdataF_ext));

  nb_re_pdsch = (pilots == 1) ?
                ((config_type == NFAPI_NR_DMRS_TYPE1) ? nb_rb_pdsch * (12 - 6 * dlsch_config->n_dmrs_cdm_groups) :
                nb_rb_pdsch * (12 - 4 * dlsch_config->n_dmrs_cdm_groups)):
                (nb_rb_pdsch * 12);
  // Subtract CSI-RS REs from PDSCH RE count
  if (csi_res_bitmap != 0) {
    uint32_t csi_re_count = 0;
    uint32_t csi_res_even = csi_res_bitmap & 0xfff;
    uint32_t csi_res_odd = (csi_res_bitmap >> 16) & 0xfff;
    uint32_t count_even = count_bits(&csi_res_even, 1);
    uint32_t count_odd  = count_bits(&csi_res_odd, 1);
    int start = freq_alloc->first_rb + dlsch_config->BWPStart;
    int end = freq_alloc->last_rb + 1;
    if (nr_dlsch_csi_rb_extent()) {
      /* bitmap index is BWP-relative, the mask takes the CRB (the legacy loop below mixes both when BWPStart > 0) */
      for (int rb = freq_alloc->first_rb; rb <= freq_alloc->last_rb; rb++)
        if ((freq_alloc->bitmap[rb / 32] >> (rb % 32)) & 0x01)
          csi_re_count += __builtin_popcount(nr_dlsch_csi_rb_mask(dlsch_config, symbol, rb + dlsch_config->BWPStart));
    } else {
    for (int rb = start; rb < end; rb++) {
      if ((freq_alloc->bitmap[rb / 32] >> (rb % 32)) & 0x01)
        csi_re_count += (rb % 2 == 0) ? count_even : count_odd;
    }
    }
    nb_re_pdsch = (nb_re_pdsch > csi_re_count) ? (nb_re_pdsch - csi_re_count) : 0;
    if (csi_re_count > 0) {
      LOG_D(NR_PHY,
            "[CSI OVERLAP] Frame/Slot %d.%d Symbol %d: CSI-RS overlapping PDSCH - %d CSI-RS REs skipped, %d data REs extracted\n",
            frame,
            nr_slot_rx,
            symbol,
            csi_re_count,
            nb_re_pdsch);
    }
  }

  if (ssb_rm)
    nb_re_pdsch = nb_re_ext; // the extractor packed exactly the DM-RS | CSI-RS | SSB union out

  if (scope_req->copy_rxdataF_to_scope) {
    size_t size = sizeof(c16_t) * nb_re_pdsch;
    int copy_index = symbol - dlsch_config->start_symbol;
    UEscopeCopyUnsafe(ue, pdschRxdataF, rxdataF_ext[0], size, scope_req->scope_rxdataF_offset, copy_index);
    scope_req->scope_rxdataF_offset += size;
  }
  //----------------------------------------------------------
  //--------------------- Channel Scaling --------------------
  //----------------------------------------------------------
  start_meas_nr_ue_phy(ue, DLSCH_CHANNEL_SCALE_STATS);
  nr_scale_channel(rx_size_symbol, dl_ch_estimates_ext, 0, nb_re_pdsch, nl, nbRx, 0);
  stop_meas_nr_ue_phy(ue, DLSCH_CHANNEL_SCALE_STATS);
  if (meas_enabled) {
    LOG_D(PHY,
          "[AbsSFN %u.%d] Slot%d Symbol %d: Channel Scale  %5.2f \n",
          frame,
          nr_slot_rx,
          slot,
          symbol,
          ue->phy_cpu_stats.cpu_time_stats[DLSCH_CHANNEL_SCALE_STATS].p_time / (cpuf * 1000.0));
  }

  //----------------------------------------------------------
  //--------------------- Channel Level Calc. ----------------
  //----------------------------------------------------------
  start_meas_nr_ue_phy(ue, DLSCH_CHANNEL_LEVEL_STATS);
  if (first_symbol_flag) {
    int32_t avg[nl * nbRx];
    if (nb_re_pdsch)
      nr_channel_level(0, rx_size_symbol, (c16_t (*)[rx_size_symbol])dl_ch_estimates_ext, nbRx, nl, avg, nb_re_pdsch);
    else
      LOG_E(NR_PHY, "Average channel level is 0: nb_rb_pdsch = %d, nb_re_pdsch = %d\n", nb_rb_pdsch, nb_re_pdsch);
    int avgs = 0;
    int32_t median[MAX_ANT][MAX_ANT];
    for (int l = 0; l < nl; l++)
      for (int aarx = 0; aarx < nbRx; aarx++) {
        avgs = cmax(avgs, avg[l * nbRx + aarx]);
        LOG_D(PHY, "nb_rb %d avg_%d_%d Power per SC is %d\n", nb_rb_pdsch, aarx, l, avg[l * nbRx + aarx]);
        LOG_D(PHY, "avgs Power per SC is %d\n", avgs);
        median[l][aarx] = avg[l * nbRx + aarx];
      }

    /* Decide which receive branch(es) actually feed the equaliser, and derive the fixed-point
     * shift from the SAME set -- the two must agree or the compensation is scaled for energy the
     * decoder never sees. `avgs` already holds the max over all branches from the loop above,
     * which is what modes 1 and 2 want; only mode 0 overrides it back to branch 0. */
    if (nl == 1 && nbRx == 4) {
      int best = 0;
      for (int aarx = 1; aarx < nbRx; aarx++) {
        if (avg[aarx] > avg[best]) {
          best = aarx;
        }
      }
      /* SELECTION-DIVERSITY OVERRIDE (2026-09-03). When nr_dlsch_force_branch() has pinned a
       * branch, it wins over every mode below: the passive decoder uses it to RE-DECODE the same
       * transport block from a different branch after a CRC failure, so the branch identity has to
       * come from the caller rather than from this function's own power ranking. -1 (the default,
       * and the only value any other caller ever sees) leaves the mode logic untouched.
       *
       * Why the caller and not `best`: mode 1 already decodes the STRONGEST branch and this rig
       * measured it at 0.0 % across six runs while branch 3 held the highest |h| -- power ranking
       * does not predict decodability here, so the retry walks branches by index instead and lets
       * the CRC decide. */
      if (nr_dlsch_forced_mask >= 0) {
        /* Subset scan: combine exactly the masked branches. The shift still comes from the
         * strongest branch PRESENT IN THE MASK, not the strongest overall -- otherwise every subset
         * would inherit branch 0's scaling and the comparison would measure the shift, not the
         * subset. */
        t_mrc_nb_rx = nbRx;
        t_mrc_rx_index = 0;
        t_mrc_live_mask = nr_dlsch_forced_mask;
        int mbest = -1;
        for (int aarx = 0; aarx < nbRx; aarx++) {
          if ((nr_dlsch_forced_mask & (1 << aarx)) && (mbest < 0 || avg[aarx] > avg[mbest])) {
            mbest = aarx;
          }
        }
        avgs = (mbest >= 0) ? avg[mbest] : avg[best];
      } else if (nr_dlsch_forced_branch >= 0 && nr_dlsch_forced_branch < nbRx) {
        t_mrc_nb_rx = 1;
        t_mrc_rx_index = nr_dlsch_forced_branch;
        avgs = avg[nr_dlsch_forced_branch];
      } else if (t_mrc_mode == 0) {
        t_mrc_nb_rx = 1;
        t_mrc_rx_index = 0;
        avgs = avg[0];
      } else if (t_mrc_mode == 1) {
        /* ISAC_RX_BEST_BY_SNR=1 (T26d): pick the branch with the best |h|^2 / chest noise, not the highest |h|^2 --
         * a noisy branch has a large |h|^2 estimate (noise included) and was being selected, which also shows up as
         * a QAM amplitude ratio kappa << 1 (signal part of h* y small against the noise-inflated |h|^2). */
        static int s_bsnr = -1;
        if (s_bsnr < 0) s_bsnr = getenv("ISAC_RX_BEST_BY_SNR") && atoi(getenv("ISAC_RX_BEST_BY_SNR")) ? 1 : 0;
        if (s_bsnr) {
          double bq = -1.0;
          for (int aarx = 0; aarx < nbRx && aarx < 4; aarx++) {
            const double nv = nr_dl_chest_nvar_ant[aarx] > 0 ? (double)nr_dl_chest_nvar_ant[aarx] : 1.0;
            const double q = (double)avg[aarx] / nv;
            if (q > bq) { bq = q; best = aarx; }
          }
        }
        t_mrc_nb_rx = 1;
        t_mrc_rx_index = best;
        avgs = avg[best];
      } else if (t_mrc_mode == 2) {
        /* Full MRC over every branch. nr_channel_compensation() takes a CONTIGUOUS slice, so all
         * four go in -- INCLUDING the dead ones, which contribute ~no signal but do add noise,
         * because the combiner uses ONE scalar noise variance for all branches. That is why §12.7
         * measured the gain over the best single branch as only +0.8 dB rather than the +6 dB four
         * balanced branches would give. */
        t_mrc_nb_rx = nbRx;
        t_mrc_live_mask = 0xF;
        t_mrc_rx_index = 0;
      } else {
        /* Mode 3 (default): MRC over the LIVE branches only. Measured on this rig the four branches
         * sit at 0 / -28.5 / -17.0 / -3.3 dB -- two are noise and one is only 3.3 dB down, so
         * combining 0+3 while excluding 1+2 is worth ~+1.8 dB against §12.1's ~1 dB rank-1 margin,
         * whereas mode 2 spends most of that re-admitting the two dead branches' noise.
         *
         * Implemented by ZEROING the excluded branches' channel estimates rather than by permuting
         * rows into a contiguous slice: MRC weights by h*, so h == 0 contributes exactly zero
         * signal AND zero noise, which is the same result for a fraction of the work and no data
         * movement. (This is §12.7's ISAC_RX_BRANCH_MIN_DB, which the §13 revert removed.) */
        const double thr = pow(10.0, -t_mrc_min_db / 10.0) * (double)avg[best];
        int mask = 0;
        for (int aarx = 0; aarx < nbRx; aarx++) {
          if ((double)avg[aarx] >= thr) {
            mask |= (1 << aarx);
          }
        }
        if (mask == 0) {
          mask = (1 << best); // never exclude everything
        }
        t_mrc_live_mask = mask;
        t_mrc_nb_rx = nbRx; // the slice stays contiguous; excluded branches are zeroed instead
        t_mrc_rx_index = 0;
        /* V2: replace the power threshold with noise weighting. Power is the wrong criterion on this
         * rig -- branch 2 is the STRONGEST and the noisiest -- so weight by measured chest noise. */
        t_mrc_weighted = 0;
        if (nr_agnostic_v2() && nbRx <= 4) {
          uint32_t nv[4] = {0, 0, 0, 0};
          for (int aarx = 0; aarx < nbRx; aarx++) nv[aarx] = nr_dl_chest_nvar_ant[aarx];
          if (nr_mrc_noise_weights(nv, nbRx, t_mrc_w) > 0) {
            int wm = 0;
            for (int aarx = 0; aarx < nbRx; aarx++) {
              t_mrc_wq15[aarx] = (int16_t)lround(t_mrc_w[aarx] * 32767.0);
              if (t_mrc_w[aarx] > 0.0) wm |= 1 << aarx;
            }
            t_mrc_live_mask = wm ? wm : (1 << best);
            t_mrc_weighted = 1;
          }
        }
      }
      mrc_nb_rx = t_mrc_nb_rx;
      mrc_rx_index = t_mrc_rx_index;
      // Publish the branch actually used, so the passive decoder's retry knows which one to skip.
      nr_dlsch_used_branch = (t_mrc_nb_rx == 1) ? t_mrc_rx_index : -1;

      /* SUBSETDIAG (ISAC_SUBSET_DIAG=1): the state this decision actually resolved to, printed for
       * EVERY call so a force_mask={0} call and a primary mode-0 call on the same TB can be diffed
       * line by line. Added 2026-09-08 because {0} via force_mask reads 0 % CRC while primary mode
       * 0 (electrically identical in theory -- a zeroed channel estimate contributes exactly zero
       * to both the MRC signal and gain sums) reads 87.7 % on the SAME transport blocks, and static
       * reading of the zeroing/shift/accumulate path found no discrepancy. */
      if (getenv("ISAC_SUBSET_DIAG") != NULL) {
        LOG_I(PHY,
              "SENSING: SUBSETDIAG forced_mask=%d forced_branch=%d mode=%d nb_rx=%d rx_index=%d "
              "live_mask=0x%x avgs=%d best=%d shift_src=%s\n",
              nr_dlsch_forced_mask, nr_dlsch_forced_branch, t_mrc_mode, mrc_nb_rx, mrc_rx_index,
              t_mrc_live_mask, avgs, best,
              (nr_dlsch_forced_mask >= 0) ? "mask-branch" : (t_mrc_mode == 0) ? "mode0" : "other");
      }

      /* RXBRANCH: the per-branch powers this decision is made from. §12.7 records that CHESTDIAG's
       * equivalent field was declared, printed and never written, which is why the imbalance stayed
       * invisible for so long -- so this one prints the raw values, not a derived summary. dB are
       * relative to the strongest branch. */
      {
        static __thread int s_rxb = -1;
        static __thread unsigned long s_rxb_n = 0;
        if (s_rxb < 0) {
          s_rxb = (getenv("ISAC_RX_BRANCH") != NULL) ? atoi(getenv("ISAC_RX_BRANCH")) : 0;
        }
        /* ISAC_RX_BRANCH=2 prints EVERY transport block, not 1 in 500. A 1-in-500 sample of
         * `best` cannot distinguish "branch 0 is always strongest" from "branch 0 is strongest one
         * time in three", and that decides whether mode 1 is even selecting the branch it should. */
        if (s_rxb && ((s_rxb >= 2) || (s_rxb_n++ % 500) == 0)) {
          const double mx = (avg[best] > 0) ? (double)avg[best] : 1.0;
          LOG_I(PHY,
                "SENSING: RXBRANCH mode=%d best=%d used=[%d..%d) live_mask=0x%x pw=[%d %d %d %d] "
                "dB=[%.1f %.1f %.1f %.1f]\n",
                t_mrc_mode, best, mrc_rx_index, mrc_rx_index + mrc_nb_rx, t_mrc_live_mask,
                avg[0], avg[1], avg[2], avg[3],
                10.0 * log10((avg[0] > 0 ? avg[0] : 1) / mx), 10.0 * log10((avg[1] > 0 ? avg[1] : 1) / mx),
                10.0 * log10((avg[2] > 0 ? avg[2] : 1) / mx), 10.0 * log10((avg[3] > 0 ? avg[3] : 1) / mx));
        }
      }
    }

    if (nl > 1) {
      nr_dlsch_channel_level_median(rx_size_symbol, dl_ch_estimates_ext, median, nl, nbRx, nb_re_pdsch);
      for (int l = 0; l < nl; l++) {
        for (int aarx = 0; aarx < nbRx; aarx++) {
          avgs = cmax(avgs, median[l][aarx]);
        }
      }
    }
    /* ISAC_L2MAXH_PEAK=1 (default off): size the shift from the PEAK per-RB channel level over
     * the allocation instead of the MEAN. Measured OTA 2026-09-14 (EQDIAG, 273 PRB, X410 4-ch):
     * the front end has ~5 dB of in-band tilt (low edge 1.4-1.7x the band median), and every RE
     * whose |H| exceeds ~1.4x the allocation mean saturates in Y.H* -> int16: EVM 6-7 % below the
     * knee, 45-87 % above it, in 100 % of >=255-PRB grants and ~1 % of <=33-PRB ones (a narrow
     * grant's mean is local). The single guard bit covers the 256QAM constellation peak, not the
     * channel's own spread. Same RB-mean primitive as nr_channel_level so the two scale alike;
     * per-RB means rather than per-RE so one noisy RE cannot steal a bit from the whole grant. */
    static int s_l2_peak = -1;
    if (s_l2_peak < 0) {
      const char *e = getenv("ISAC_L2MAXH_PEAK");
      s_l2_peak = (e != NULL && atoi(e) != 0) ? 1 : 0;
    }
    if (s_l2_peak && nl == 1) {
      int32_t peak = 0;
      c16_t (*ext)[rx_size_symbol] = (c16_t (*)[rx_size_symbol])dl_ch_estimates_ext;
      for (int aarx = 0; aarx < nbRx; aarx++) {
        if (nbRx == 4 && (t_mrc_mode == 3 || nr_dlsch_forced_mask >= 0) && !(t_mrc_live_mask & (1 << aarx)))
          continue;
        for (uint32_t re = 0; re + 12 <= nb_re_pdsch; re += 12) {
          const int32_t rb = simde_mm_average((simde__m128i *)&ext[aarx][re], 12, 2, 3);
          if (rb > peak)
            peak = rb;
        }
      }
      static __thread unsigned long s_l2n = 0;
      if ((s_l2n++ % 500) == 0)
        LOG_A(PHY, "SENSING: L2PEAK nb_rb=%d avgs(mean)=%d peak_rb=%d ratio=%.2f shift_delta=%d\n",
              nb_rb_pdsch, avgs, peak, avgs > 0 ? (double)peak / avgs : 0.0,
              (log2_approx(peak > avgs ? peak : avgs) >> 1) - (log2_approx(avgs) >> 1));
      if (peak > avgs)
        avgs = peak;
    }
    // Output shift: half channel energy (log2|h|^2/2) + MRC antenna gain.
    // Single-layer adds +1 guard bit (raw peak); multi-layer uses median so no guard needed.
    /* MRC headroom. `log2_approx(n >> 1)` gives 1 for n = 4, but a coherent sum of 4 branches grows
     * by up to 4x and therefore needs 2 bits -- the formula was ONE BIT SHORT, which is the second
     * half of why multi-branch combining produced garbage (the first is the wrapping accumulator in
     * nr_channel_compensation.c). log2_approx(1) == log2_approx(0) == 0, so the single-branch case
     * -- the only one this deployment has been decoding with -- is BIT-IDENTICAL to before. */
    if (nl == 1) {
      /* Headroom must match the number of branches that actually CONTRIBUTE, not the width of the
       * slice. Mode 3 keeps the slice contiguous at nbRx and silences the dead branches by zeroing
       * their estimates, so charging it log2(nbRx) bits would spend 2 bits of LLR dynamic range to
       * protect a sum that only ever has popcount(live_mask) non-zero terms. */
      int contributing = mrc_nb_rx;
      /* The live-branch count must follow the mask WHOEVER set it. Gating this on t_mrc_mode == 3
       * alone meant the subset scan (which drives the same live_mask through nr_dlsch_force_mask()
       * while mode stays 0) got a shift sized for FOUR contributing branches even when one was
       * live -- over-scaling the equaliser output and destroying the decoder's soft input while
       * leaving the constellation itself intact, exactly the failure the L2MAXH comment below
       * describes. Measured 2026-09-08: every one of the 15 subsets read 0 % CRC, including {0},
       * on transport blocks the primary path decoded at 87.7 %. Same class as the nvar bug fixed
       * the same day -- a correction that knew about mode 3 but not about forced_mask. */
      if (t_mrc_weighted && nbRx <= 4 && t_mrc_mode == 3 && nr_dlsch_forced_mask < 0) {
        contributing = nr_mrc_effective_branches(t_mrc_w, nbRx);
      } else if (nbRx == 4 && (t_mrc_mode == 3 || nr_dlsch_forced_mask >= 0)) {
        contributing = 0;
        for (int aarx = 0; aarx < nbRx; aarx++) {
          if (t_mrc_live_mask & (1 << aarx)) {
            contributing++;
          }
        }
        if (contributing < 1) {
          contributing = 1;
        }
      }
      *log2_maxh = (log2_approx(avgs) >> 1) + 1 + log2_approx(contributing);
      /* ISAC_L2MAXH_HEADROOM=1 (default off): the shift above is derived from the MEAN |H|^2, which
       * leaves 2-4 bits of the int16 equaliser output unused on a flat channel and is therefore
       * 2-4 bits too COARSE once the channel has in-band spread -- the weak RBs then get too few
       * LSBs per 256QAM constellation step. Measured with OAI's own nr_channel_compensation() +
       * nr_256qam_llr(), no noise, exact H, 273 PRB, 3276 REs (see
       * openair1/PHY/NR_UE_ISAC/tests/dlsch_fixed_point_test.cc):
       *   |H|rms 480, flat        : 0.0000 % LLR sign errors at the mean-derived shift
       *   |H|rms 480, 21 dB spread: 2.14 %  at the mean-derived shift, 0.0000 % 2-4 bits lower
       *   |H|rms 2672, 21 dB      : 0.0000 % either way (that level already has the headroom)
       * Sizing the shift from the PEAK per-RB level instead makes the choice automatic and can only
       * LOWER it (cmin below), so a receiver already inside the exact window is unaffected.
       * NOT the root cause of the OTA wide-grant failure -- that rig measures |H|rms ~2672, where
       * both rules score 0.0000 % -- this is a robustness fix for lower-gain / wider-spread cases.
       * Per-RB means (not per-RE) so one noisy RE cannot steal a bit from the whole grant, same
       * primitive as nr_channel_level() so the two scale alike. */
      static int s_l2_hr = -1;
      if (s_l2_hr < 0) {
        const char *e = getenv("ISAC_L2MAXH_HEADROOM");
        s_l2_hr = (e != NULL && atoi(e) != 0) ? 1 : 0;
      }
      if (s_l2_hr) {
        int32_t peak_rb = 0;
        c16_t (*ext_hr)[rx_size_symbol] = (c16_t (*)[rx_size_symbol])dl_ch_estimates_ext;
        for (int aarx = 0; aarx < nbRx; aarx++) {
          if (nbRx == 4 && (t_mrc_mode == 3 || nr_dlsch_forced_mask >= 0) && !(t_mrc_live_mask & (1 << aarx)))
            continue;
          for (uint32_t re = 0; re + 12 <= nb_re_pdsch; re += 12) {
            const int32_t rb = simde_mm_average((simde__m128i *)&ext_hr[aarx][re], 12, 2, 3);
            if (rb > peak_rb)
              peak_rb = rb;
          }
        }
        const int hr = nr_log2_maxh_headroom((uint32_t)peak_rb, contributing);
        if (peak_rb > 0 && hr < *log2_maxh)
          *log2_maxh = hr;
      }
    }
    else
      *log2_maxh = (log2_approx(avgs) >> 1) + log2_approx(nbRx >> 1);
    /* ISAC_L2MAXH_DELTA (signed bits, default 0): the shift above makes Y.H* >> shift ~ |Y|/2, i.e.
     * the equaliser OUTPUT sits at a few LSB whatever the RX gain (measured OTA 2026-09-14: EVM floor
     * 7 % = ~25 dB at both 40 and 49 dB gain, rawmean 4 -> 11). Every LLR is quantised at that
     * resolution. A negative delta keeps more bits; pair it with ISAC_LLR_SCALE so the int8 pack
     * stays calibrated. Saturation guard: |H|^2 >> shift must stay well under 32767. */
    {
      static int s_l2d = -9999;
      if (s_l2d == -9999) {
        const char *e = getenv("ISAC_L2MAXH_DELTA");
        s_l2d = (e != NULL) ? atoi(e) : 0;
      }
      if (s_l2d != 0) {
        *log2_maxh += s_l2d;
        if (*log2_maxh < 0)
          *log2_maxh = 0;
      }
    }
    LOG_D(PHY, "[DLSCH] AbsSubframe %d.%d log2_maxh = %d (%d)\n", frame % 1024, nr_slot_rx, *log2_maxh, avgs);
    /* L2MAXH (ISAC_RX_BRANCH=1): the fixed-point shift the equaliser scales its output -- and hence
     * the QAM LLR decision thresholds -- by. This is the one quantity that can leave the CONSTELLATION
     * intact while destroying the DECODER's soft input, which is exactly the signature this rig shows:
     * post-equalisation EVM sits at 47-62 % whether the run decodes 82.6 % of transport blocks or
     * 0.0 % of them, so whatever separates those runs is downstream of the equaliser, and log2_maxh
     * is derived from the MEASURED channel level `avgs` and therefore free to move run to run with
     * gain/AGC. A self-normalising EVM probe is blind to it by construction. */
    {
      static __thread int s_l2 = -1;
      static __thread unsigned long s_l2n = 0;
      if (s_l2 < 0)
        s_l2 = (getenv("ISAC_RX_BRANCH") != NULL) ? 1 : 0;
      if (s_l2 && (s_l2n++ % 500) == 0)
        LOG_I(PHY, "SENSING: L2MAXH log2_maxh=%d avgs=%d nl=%d mrc_nb_rx=%d Qm=%u\n",
              *log2_maxh, avgs, nl, mrc_nb_rx, (unsigned)dlsch->cw_info.qamModOrder);
    }
#if T_TRACER
    T(T_UE_PHY_PDSCH_ENERGY,
      T_INT(gNB_id),
      T_INT(frame % 1024),
      T_INT(nr_slot_rx),
      T_INT(avg[0]), // layer 0, antenna 0
      T_INT(nbRx > 1 ? avg[1] : 0), // layer 0, antenna 1
      T_INT(nl > 1 ? avg[nbRx] : 0), // layer 1, antenna 0
      T_INT(nl > 1 && nbRx > 1 ? avg[nbRx + 1] : 0)); // layer 1, antenna 1
#endif
  }
  stop_meas_nr_ue_phy(ue, DLSCH_CHANNEL_LEVEL_STATS);
  if (meas_enabled) {
    LOG_D(PHY,
          "[AbsSFN %u.%d] Slot%d Symbol %d first_symbol_flag %d: Channel Level  %5.2f \n",
          frame,
          nr_slot_rx,
          slot,
          symbol,
          first_symbol_flag,
          ue->phy_cpu_stats.cpu_time_stats[DLSCH_CHANNEL_LEVEL_STATS].p_time / (cpuf * 1000.0));
  }

  /* Mode 3: silence the excluded branches. Applied EVERY symbol, not just at first_symbol_flag --
   * dl_ch_estimates_ext is rebuilt by nr_dlsch_extract_rbs() on each call, so a mask applied once
   * would be undone for every symbol after the first. The decision itself is made once (at
   * first_symbol_flag, from the true unzeroed powers) and carried in thread-local state. */
  if (nl == 1 && nbRx == 4 && (nr_dlsch_forced_mask >= 0 || t_mrc_mode == 3) && t_mrc_live_mask != 0xF) {
    for (int aarx = 0; aarx < nbRx; aarx++) {
      if (!(t_mrc_live_mask & (1 << aarx))) {
        memset(chFext[0][aarx], 0, rx_size_symbol * sizeof(c16_t));
      }
    }
  }
  /* V2 noise-weighted MRC: scale h AND y of each branch by w_a (Q15) every symbol, for the same
   * reason the mask above is re-applied every symbol (the extraction rebuilds both buffers). */
  if (nl == 1 && t_mrc_weighted && t_mrc_mode == 3 && nr_dlsch_forced_mask < 0 && nbRx <= 4) {
    for (int aarx = 0; aarx < nbRx; aarx++) {
      const int32_t w = t_mrc_wq15[aarx];
      if (w >= 32767)
        continue;
      c16_t *hq = (c16_t *)chFext[0][aarx], *yq = rxdataF_ext[aarx];
      for (int k = 0; k < rx_size_symbol; k++) {
        hq[k].r = (int16_t)(((int32_t)hq[k].r * w) >> 15); hq[k].i = (int16_t)(((int32_t)hq[k].i * w) >> 15);
        yq[k].r = (int16_t)(((int32_t)yq[k].r * w) >> 15); yq[k].i = (int16_t)(((int32_t)yq[k].i * w) >> 15);
      }
    }
  }

  /* ISAC_RX_MRC_ALIGN=1 (T26d): per-symbol phase alignment of the MRC branches. Each branch is equalised against its
   * own DM-RS estimate, but on this rig the branches' phases drift DIFFERENTLY between the DM-RS and the data symbols
   * (independent board LOs), so sum_a h_a* y_a adds partly incoherently. u_a = h_a* y_a ~ |h_a|^2 x e^{j th_a}; the
   * cross-correlation c_a = sum_i u_a conj(u_ref) ~ e^{j(th_a - th_ref)} needs no decisions. Rotating h_a by arg(c_a)
   * re-phases branch a onto the strongest live branch before nr_channel_compensation() combines. */
  if (nl == 1 && mrc_nb_rx > 1 && nb_re_pdsch >= 12) {
    static int s_align = -1;
    if (s_align < 0) s_align = getenv("ISAC_RX_MRC_ALIGN") ? atoi(getenv("ISAC_RX_MRC_ALIGN")) : 0;
    if (s_align) {
      const int a0 = mrc_rx_index, a1 = mrc_rx_index + mrc_nb_rx;
      double pw[NR_PDSCH_EVM_MAX_RX * 2] = {0};
      int ref = -1;
      for (int a = a0; a < a1 && a < NR_PDSCH_EVM_MAX_RX * 2; a++) {
        for (uint32_t i = 0; i < nb_re_pdsch; i++) {
          const c16_t h = chFext[0][a][i];
          pw[a] += (double)h.r * h.r + (double)h.i * h.i;
        }
        if (pw[a] > 0.0 && (ref < 0 || pw[a] > pw[ref])) ref = a;
      }
      /* ISAC_RX_MRC_ALIGN=2: one phase per chunk of 48 REs (4 RB) instead of per symbol -- follows a phase SLOPE
       * across frequency (different per-board timing / shared delay estimate) as well as the common rotation. */
      const uint32_t chunk = s_align >= 2 ? 48u : nb_re_pdsch;
      for (int a = a0; ref >= 0 && a < a1; a++) {
        if (a == ref || pw[a] <= 0.0) continue;
       for (uint32_t c0 = 0; c0 < nb_re_pdsch; c0 += chunk) {
        const uint32_t c1 = c0 + chunk < nb_re_pdsch ? c0 + chunk : nb_re_pdsch;
        double cr = 0.0, ci = 0.0;
        for (uint32_t i = c0; i < c1; i++) {
          const c16_t ha = chFext[0][a][i], ya = rxdataF_ext[a][i], hr = chFext[0][ref][i], yr = rxdataF_ext[ref][i];
          const double uar = (double)ha.r * ya.r + (double)ha.i * ya.i, uai = (double)ha.r * ya.i - (double)ha.i * ya.r;
          const double urr = (double)hr.r * yr.r + (double)hr.i * yr.i, uri = (double)hr.r * yr.i - (double)hr.i * yr.r;
          cr += uar * urr + uai * uri; /* u_a conj(u_ref) */
          ci += uai * urr - uar * uri;
        }
        const double mag = sqrt(cr * cr + ci * ci);
        if (mag <= 0.0) continue;
        const double c = cr / mag, sn = ci / mag; /* e^{j d}: rotating h_a by +d multiplies u_a by e^{-j d} */
        c16_t *h = chFext[0][a];
        const uint32_t e1 = (chunk == nb_re_pdsch) ? rx_size_symbol : c1;
        for (uint32_t i = c0; i < e1; i++) {
          const double hr2 = h[i].r * c - h[i].i * sn, hi2 = h[i].r * sn + h[i].i * c;
          h[i].r = (int16_t)lround(hr2); h[i].i = (int16_t)lround(hi2);
        }
       }
      }
    }
  }

  /* ISAC_RX_IRC=1 (T26d prototype): MMSE-IRC for 1 layer by spatial whitening. Per chunk of 48 REs (4 RB) of this
   * symbol the interference+noise covariance R (nbRx x nbRx) is estimated from decision-directed residuals
   * e = y - h s_hat (s_hat: MRC decision over all branches), shrunk toward its diagonal (ISAC_RX_IRC_SHRINK, default
   * 0.3) and loaded; with R = L L^H, y' = L^-1 y and h' = L^-1 h make the stock MRC combiner compute h^H R^-1 y and
   * the QAM thresholds h^H R^-1 h, i.e. IRC, with no change to the LLR code. All branches combine (mode-independent);
   * the whitened pair is rescaled so its combined power equals the original combined power of this mode's slice
   * (log2_maxh stays valid). Decision-directed: self-nulling at high symbol-error rates is the known risk. */
  if (nl >= 1 && nl <= 4 && nbRx >= 2 && nbRx <= 4 && nb_re_pdsch >= 48) {
    static int s_irc = -1;
    static double s_shrink = 0.3;
    if (s_irc < 0) {
      s_irc = getenv("ISAC_RX_IRC") ? atoi(getenv("ISAC_RX_IRC")) : 0;
      if (getenv("ISAC_RX_IRC_SHRINK")) s_shrink = atof(getenv("ISAC_RX_IRC_SHRINK"));
    }
    const int s_irc_env = s_irc;
    if (nr_dlsch_irc_override >= 0) s_irc = nr_dlsch_irc_override; /* per-grant (ISAC_RX_MRC_PER_RNTI) */
    if (s_irc && (nl == 1 || s_irc == 4)) { /* N layers: only the DM-RS covariance (4) -- no data decisions */
      const int qm = dlsch->cw_info.qamModOrder;
      const int lmax = (1 << (qm / 2)) - 1;
      double ms = 0.0; int nlev = 0;
      for (int l = 1; l <= lmax; l += 2) { ms += (double)l * l; nlev++; }
      const double ideal_pow = 2.0 * ms / nlev;
      const int nb = nbRx;
      /* combined power of the mode's original slice, for the rescale */
      double pslice = 0.0, pall = 0.0;
      for (uint32_t i = 0; i < nb_re_pdsch; i++)
        for (int a = 0; a < nb; a++) {
          const double p2 = (double)chFext[0][a][i].r * chFext[0][a][i].r + (double)chFext[0][a][i].i * chFext[0][a][i].i;
          pall += p2;
          if (a >= mrc_rx_index && a < mrc_rx_index + mrc_nb_rx) pslice += p2;
        }
      /* MRC decisions over all branches, symbol RMS scale */
      double complex *xs = s_irc == 4 ? NULL : malloc(sizeof(double complex) * nb_re_pdsch);
      double pw = 0.0; uint32_t nv = 0;
      for (uint32_t i = 0; xs && i < nb_re_pdsch; i++) {
        double complex num = 0; double g = 0;
        for (int a = 0; a < nb; a++) {
          const double complex h = chFext[0][a][i].r + I * chFext[0][a][i].i, y = rxdataF_ext[a][i].r + I * rxdataF_ext[a][i].i;
          num += conj(h) * y; g += creal(h) * creal(h) + cimag(h) * cimag(h);
        }
        xs[i] = g > 0 ? num / g : 0;
        if (g > 0) { pw += creal(xs[i]) * creal(xs[i]) + cimag(xs[i]) * cimag(xs[i]); nv++; }
      }
      if (((xs && nv >= 24 && pw > 0) || s_irc == 4) && pall > 0) {
        const double scale = xs ? sqrt(ideal_pow / (pw / nv)) : 1.0;
        for (uint32_t i = 0; xs && i < nb_re_pdsch; i++) { /* sliced decision back in channel units */
          double vi = creal(xs[i]) * scale, vq = cimag(xs[i]) * scale;
          double si = 2.0 * floor(vi / 2.0) + 1.0, sq = 2.0 * floor(vq / 2.0) + 1.0;
          si = si > lmax ? lmax : si < -lmax ? -lmax : si; sq = sq > lmax ? lmax : sq < -lmax ? -lmax : sq;
          xs[i] = (si + I * sq) / scale;
        }
        const double resc = sqrt(pslice / pall); /* applied together with the whitening normaliser below */
        for (uint32_t c0 = 0; c0 < nb_re_pdsch; c0 += 48) {
          const uint32_t c1 = c0 + 48 < nb_re_pdsch ? c0 + 48 : nb_re_pdsch;
          double complex R[4][4] = {{0}};
          /* ISAC_RX_IRC>=2: first correct each branch's estimate by its chunk LS gain alpha_a = <y_a, h_a s>/|h_a s|^2
           * (measured |alpha| ~0.5-0.85 < 1: the estimate is inflated by its own noise and its phase drifts), so the
           * residual does not carry (1-alpha) h s -- which made R see the signal and null it (self-nulling). */
          double complex alpha[4] = {1, 1, 1, 1};
          if (s_irc == 2 || s_irc == 3)
            for (int a = 0; a < nb; a++) {
              double complex num = 0; double den = 0;
              for (uint32_t i = c0; i < c1; i++) {
                const double complex hs = (chFext[0][a][i].r + I * chFext[0][a][i].i) * xs[i];
                num += (rxdataF_ext[a][i].r + I * rxdataF_ext[a][i].i) * conj(hs);
                den += creal(hs) * creal(hs) + cimag(hs) * cimag(hs);
              }
              if (den > 0) alpha[a] = num / den;
              for (uint32_t i = c0; i < c1; i++) {
                const double complex h2 = (chFext[0][a][i].r + I * chFext[0][a][i].i) * alpha[a];
                chFext[0][a][i].r = (int16_t)fmax(-32768.0, fmin(32767.0, lround(creal(h2))));
                chFext[0][a][i].i = (int16_t)fmax(-32768.0, fmin(32767.0, lround(cimag(h2))));
              }
            }
          for (uint32_t i = c0; xs && i < c1; i++) {
            double complex e[4];
            for (int a = 0; a < nb; a++)
              e[a] = (rxdataF_ext[a][i].r + I * rxdataF_ext[a][i].i) - (chFext[0][a][i].r + I * chFext[0][a][i].i) * xs[i];
            for (int a = 0; a < nb; a++)
              for (int b = 0; b < nb; b++) R[a][b] += e[a] * conj(e[b]);
          }
          if (s_irc == 4) { /* DM-RS residual covariance of this 4-RB group (chunks of 48 data REs ~ 4 RB) */
            const int g = (int)(c0 / 48u);
            if (!nr_irc_dmrs_cov.valid || g >= nr_irc_dmrs_cov.ng || nr_irc_dmrs_cov.n[g] < 8 || nr_irc_dmrs_cov.nb != nb)
              continue;
            for (int a = 0; a < nb; a++)
              for (int b = 0; b < nb; b++) R[a][b] = nr_irc_dmrs_cov.R[g][a][b] * (double)(c1 - c0) / (double)nr_irc_dmrs_cov.n[g];
          }
          double dmean = 0.0;
          for (int a = 0; a < nb; a++) { for (int b = 0; b < nb; b++) R[a][b] /= (double)(c1 - c0); dmean += creal(R[a][a]); }
          dmean /= nb;
          if (dmean <= 0) continue;
          for (int a = 0; a < nb; a++)
            for (int b = 0; b < nb; b++)
              R[a][b] = (a == b) ? creal(R[a][a]) + 1e-3 * dmean : (s_irc == 3 ? 0.0 : (1.0 - s_shrink) * R[a][b]); /* 3: diagonal = noise-weighted MRC */
          /* Cholesky R = L L^H */
          double complex L[4][4] = {{0}};
          bool ok = true;
          for (int j = 0; j < nb && ok; j++) {
            double d = creal(R[j][j]);
            for (int k = 0; k < j; k++) d -= creal(L[j][k] * conj(L[j][k]));
            if (d <= 0) { ok = false; break; }
            L[j][j] = sqrt(d);
            for (int i2 = j + 1; i2 < nb; i2++) {
              double complex v = R[i2][j];
              for (int k = 0; k < j; k++) v -= L[i2][k] * conj(L[j][k]);
              L[i2][j] = v / L[j][j];
            }
          }
          if (!ok) continue;
          /* forward substitution L z = v, normalised by sqrt(dmean) so the whitened noise has the mean branch level,
           * then scaled by resc (this mode's slice power / all-branch power) */
          const double nrm = sqrt(dmean) * resc;
          for (uint32_t i = c0; i < c1; i++) {
            double complex vy[4], vh[4][4], zy[4], zh[4][4];
            for (int a = 0; a < nb; a++) {
              vy[a] = rxdataF_ext[a][i].r + I * rxdataF_ext[a][i].i;
              for (int l = 0; l < nl; l++) vh[l][a] = chFext[l][a][i].r + I * chFext[l][a][i].i;
            }
            for (int a = 0; a < nb; a++) {
              double complex ty = vy[a];
              for (int k = 0; k < a; k++) ty -= L[a][k] * zy[k];
              zy[a] = ty / L[a][a];
              for (int l = 0; l < nl; l++) {
                double complex th = vh[l][a];
                for (int k = 0; k < a; k++) th -= L[a][k] * zh[l][k];
                zh[l][a] = th / L[a][a];
              }
            }
            for (int a = 0; a < nb; a++) {
              #define S16(v) ((int16_t)fmax(-32768.0, fmin(32767.0, lround(v))))
              rxdataF_ext[a][i].r = S16(creal(zy[a]) * nrm); rxdataF_ext[a][i].i = S16(cimag(zy[a]) * nrm);
              for (int l = 0; l < nl; l++) { chFext[l][a][i].r = S16(creal(zh[l][a]) * nrm); chFext[l][a][i].i = S16(cimag(zh[l][a]) * nrm); }
              #undef S16
            }
          }
        }
        mrc_rx_index = 0;
        mrc_nb_rx = nbRx;
      }
      free(xs);
    }
    s_irc = s_irc_env;
  }

  /* ISAC_PDSCH_EVM_GRANT (T26c): per-grant decision-directed EVM + per-branch LS SNR, nr_pdsch_evm_acc.h.
   * x = ZF over the combined branches (nl = 1: MRC), per layer; decisions per layer; branch fit uses sum_l h_l s_l. */
  int8_t *decode_ref_sign = NULL;
  static _Atomic int decode_diag = -1;
  if (decode_diag < 0) {
    const char *e = getenv("ISAC_PDSCH_DECODE_DIAG");
    decode_diag = e && atoi(e) != 0;
  }
  if (nr_pdsch_evm_acc.armed && nl >= 1 && nl <= 4 && nb_re_pdsch >= 12 && nb_re_pdsch <= rx_size_symbol) {
    nr_pdsch_evm_acc_t *ea = &nr_pdsch_evm_acc;
    const int qm = dlsch->cw_info.qamModOrder;
    const int lmax = (1 << (qm / 2)) - 1;
    double ms = 0.0;
    int nlev = 0;
    for (int l = 1; l <= lmax; l += 2) { ms += (double)l * l; nlev++; }
    const double ideal_pow = 2.0 * ms / nlev;
    const int nb = nbRx < NR_PDSCH_EVM_MAX_RX ? nbRx : NR_PDSCH_EVM_MAX_RX;
    const int a0 = nl == 1 ? mrc_rx_index : 0, a1 = nl == 1 ? mrc_rx_index + mrc_nb_rx : nbRx;
    double (*xr)[rx_size_symbol] = malloc(sizeof(double) * 2 * nl * rx_size_symbol);
    double (*xi)[rx_size_symbol] = xr ? (void *)((double *)xr + nl * rx_size_symbol) : NULL;
    double pw[4] = {0};
    uint32_t nv = 0;
    for (uint32_t i = 0; xr && i < nb_re_pdsch; i++) {
      /* G = H^H H (nl x nl), b = H^H y; solve G x = b by Gaussian elimination (complex, nl <= 4) */
      double complex G[4][5];
      for (int l = 0; l < nl; l++) {
        for (int k = 0; k < nl; k++) G[l][k] = 0;
        G[l][nl] = 0;
      }
      for (int a = a0; a < a1; a++) {
        const c16_t y = rxdataF_ext[a][i];
        const double complex yc = y.r + I * y.i;
        double complex h[4];
        for (int l = 0; l < nl; l++) { const c16_t hh = chFext[l][a][i]; h[l] = hh.r + I * hh.i; }
        for (int l = 0; l < nl; l++) {
          for (int k = 0; k < nl; k++) G[l][k] += conj(h[l]) * h[k];
          G[l][nl] += conj(h[l]) * yc;
        }
      }
      bool ok = true;
      if (nr_dlsch_csi_erase_n > 0) { /* CSI-FULL: an erased RE (all antennas zeroed) is not EVM evidence */
        bool z = true;
        for (int a = a0; a < a1 && z; a++) z = rxdataF_ext[a][i].r == 0 && rxdataF_ext[a][i].i == 0;
        ok = !z;
      }
      for (int c = 0; c < nl && ok; c++) {
        int piv = c;
        for (int r = c + 1; r < nl; r++) if (cabs(G[r][c]) > cabs(G[piv][c])) piv = r;
        if (cabs(G[piv][c]) <= 1e-9) { ok = false; break; }
        if (piv != c) for (int k = 0; k <= nl; k++) { const double complex t = G[c][k]; G[c][k] = G[piv][k]; G[piv][k] = t; }
        for (int r = 0; r < nl; r++) {
          if (r == c) continue;
          const double complex f = G[r][c] / G[c][c];
          for (int k = c; k <= nl; k++) G[r][k] -= f * G[c][k];
        }
      }
      for (int l = 0; l < nl; l++) {
        if (!ok) { xr[l][i] = xi[l][i] = NAN; continue; }
        const double complex x = G[l][nl] / G[l][l];
        xr[l][i] = creal(x); xi[l][i] = cimag(x);
        pw[l] += xr[l][i] * xr[l][i] + xi[l][i] * xi[l][i];
      }
      nv += ok;
    }
    bool good = xr && nv >= 12;
    for (int l = 0; l < nl && good; l++) good = pw[l] > 0.0;
    if (good) {
      if (decode_diag && qm == 2)
        decode_ref_sign = calloc((size_t)nl * nb_re_pdsch * 2, sizeof(*decode_ref_sign));
      double scale[4];
      for (int l = 0; l < nl; l++) scale[l] = sqrt(ideal_pow / (pw[l] / nv));
      double cr[NR_PDSCH_EVM_MAX_RX] = {0}, ci[NR_PDSCH_EVM_MAX_RX] = {0}, P[NR_PDSCH_EVM_MAX_RX] = {0}, Y[NR_PDSCH_EVM_MAX_RX] = {0};
      double qr[4] = {0}, qi[4] = {0}, qd[4] = {0}, qx[4] = {0}; /* per-layer LS fit x = beta d (CPE diagnostic) */
      for (uint32_t i = 0; i < nb_re_pdsch; i++) {
        if (isnan(xr[0][i])) continue;
        double sr[4], sq_[4];
        for (int l = 0; l < nl; l++) {
          const double vi = xr[l][i] * scale[l], vq = xi[l][i] * scale[l];
          if (decode_ref_sign) {
            decode_ref_sign[2 * (l * nb_re_pdsch + i)] = vi < 0 ? -1 : 1;
            decode_ref_sign[2 * (l * nb_re_pdsch + i) + 1] = vq < 0 ? -1 : 1;
          }
          double si = 2.0 * floor(vi / 2.0) + 1.0, sq = 2.0 * floor(vq / 2.0) + 1.0;
          si = si > lmax ? lmax : si < -lmax ? -lmax : si;
          sq = sq > lmax ? lmax : sq < -lmax ? -lmax : sq;
          ea->err += (vi - si) * (vi - si) + (vq - sq) * (vq - sq);
          ea->ref += ideal_pow;
          ea->err_layer[l] += (vi - si) * (vi - si) + (vq - sq) * (vq - sq);
          ea->ref_layer[l] += ideal_pow;
          if (symbol < 14) {
            const double e2 = (vi - si) * (vi - si) + (vq - sq) * (vq - sq);
            ea->err_sym[symbol] += e2;
            ea->ref_sym[symbol] += ideal_pow;
            ea->err_k[symbol][i % 12] += (float)(e2 / ideal_pow);
            ea->n_k[symbol][i % 12]++;
            if ((symbol == 0 || symbol == 1 || symbol == 3) && l == 0 && i / 12 < 276) { /* DECODE-RES per-RB map */
              const int row = symbol == 0 ? 0 : symbol == 1 ? 1 : 2;
              ea->err_rb[row][i / 12] += (float)(e2 / ideal_pow);
              ea->n_rb[row][i / 12]++;
            }
          }
          qr[l] += vi * si + vq * sq; qi[l] += vq * si - vi * sq; qd[l] += si * si + sq * sq; qx[l] += vi * vi + vq * vq;
          sr[l] = si / scale[l]; sq_[l] = sq / scale[l]; /* back in the channel's own units: one alpha per branch */
        }
        for (int a = 0; a < nb; a++) {
          const c16_t y = rxdataF_ext[a][i];
          double hsr = 0.0, hsi = 0.0;
          for (int l = 0; l < nl; l++) {
            const c16_t h = chFext[l][a][i];
            hsr += h.r * sr[l] - h.i * sq_[l];
            hsi += h.r * sq_[l] + h.i * sr[l];
          }
          cr[a] += y.r * hsr + y.i * hsi;
          ci[a] += y.i * hsr - y.r * hsi;
          P[a] += hsr * hsr + hsi * hsi;
          Y[a] += (double)y.r * y.r + (double)y.i * y.i;
        }
      }
      for (int l = 0; l < nl; l++) /* residual after the best per-symbol complex scale: |x|^2 - |<x,d>|^2/|d|^2 */
        if (qd[l] > 0.0) {
          const double r = qx[l] - (qr[l] * qr[l] + qi[l] * qi[l]) / qd[l];
          ea->err_cpe += r > 0.0 ? r : 0.0;
        }
      for (int a = 0; a < nb; a++) {
        if (P[a] <= 0.0) continue;
        const double sig = (cr[a] * cr[a] + ci[a] * ci[a]) / P[a];
        ea->sig[a] += sig;
        ea->res[a] += Y[a] > sig ? Y[a] - sig : 0.0;
        ea->alpha_abs[a] += sqrt(cr[a] * cr[a] + ci[a] * ci[a]) / P[a]; /* |alpha_a|: 1 = unbiased channel estimate */
        ea->alpha_n[a]++;
      }
      { /* T26d: cross-branch correlation of the LS residuals e_a = y_a - alpha_a h_a s (white noise: ~0) */
        double complex ar[NR_PDSCH_EVM_MAX_RX];
        for (int a = 0; a < nb; a++) ar[a] = P[a] > 0.0 ? (cr[a] + I * ci[a]) / P[a] : 0;
        double complex C[NR_PDSCH_EVM_MAX_RX][NR_PDSCH_EVM_MAX_RX] = {{0}};
        for (uint32_t i = 0; i < nb_re_pdsch; i++) {
          if (isnan(xr[0][i])) continue;
          double sr0[4], sq0[4];
          for (int l = 0; l < nl; l++) {
            const double vi = xr[l][i] * scale[l], vq = xi[l][i] * scale[l];
            double si = 2.0 * floor(vi / 2.0) + 1.0, sq = 2.0 * floor(vq / 2.0) + 1.0;
            si = si > lmax ? lmax : si < -lmax ? -lmax : si; sq = sq > lmax ? lmax : sq < -lmax ? -lmax : sq;
            sr0[l] = si / scale[l]; sq0[l] = sq / scale[l];
          }
          double complex e[NR_PDSCH_EVM_MAX_RX];
          for (int a = 0; a < nb; a++) {
            double complex hs = 0;
            for (int l = 0; l < nl; l++) hs += (chFext[l][a][i].r + I * chFext[l][a][i].i) * (sr0[l] + I * sq0[l]);
            e[a] = (rxdataF_ext[a][i].r + I * rxdataF_ext[a][i].i) - ar[a] * hs;
          }
          for (int a = 0; a < nb; a++)
            for (int b = a; b < nb; b++) C[a][b] += e[a] * conj(e[b]);
        }
        double rs = 0.0; int rn = 0;
        for (int a = 0; a < nb; a++)
          for (int b = a + 1; b < nb; b++)
            if (creal(C[a][a]) > 0 && creal(C[b][b]) > 0) { rs += cabs(C[a][b]) / sqrt(creal(C[a][a]) * creal(C[b][b])); rn++; }
        if (rn) { ea->rho_sum += rs / rn; ea->rho_n++; }
      }
      ea->n_re += nv;
      ea->n_sym++;
      ea->qm = qm;
      ea->nbrx = nb;
    }
    free(xr);
  }

  //----------------------------------------------------------
  //--------------------- channel compensation ---------------
  //----------------------------------------------------------
  start_meas_nr_ue_phy(ue, DLSCH_CHANNEL_COMPENSATION_STATS);
  nr_channel_compensation(rx_size_symbol,
                          pdsch_buf_size_max,
                          mrc_nb_rx,
                          nl,
                          &rxdataF_ext[mrc_rx_index],
                          (c16_t(*)[mrc_nb_rx][rx_size_symbol])&chFext[0][mrc_rx_index],
                          dl_ch_mag[symbol],
                          dl_ch_magb[symbol],
                          dl_ch_magr[symbol],
                          p_rxComp,
                          need_rho ? (c16_t(*)[nl][pdsch_buf_size_max])rho_dl[symbol] : NULL,
                          dlsch->cw_info.qamModOrder,
                          0, // symbol already baked into p_rxComp
                          *log2_maxh);
  /* Clamping every branch and partial sum can change the final sign when
   * branches cancel. Preserve the complete matched-filter sum before the
   * single output clamp. QPSK needs no corresponding magnitude thresholds. */
  if (nr_pdsch_decode_wide_mrc && nl == 1 && dlsch->cw_info.qamModOrder == 2) {
    for (uint32_t i = 0; i < nb_re_pdsch; i++) {
      int64_t re = 0, im = 0;
      for (int a = mrc_rx_index; a < mrc_rx_index + mrc_nb_rx; a++) {
        const c16_t h = chFext[0][a][i], y = rxdataF_ext[a][i];
        re += (int64_t)h.r * y.r + (int64_t)h.i * y.i;
        im += (int64_t)h.r * y.i - (int64_t)h.i * y.r;
      }
      re >>= *log2_maxh;
      im >>= *log2_maxh;
      p_rxComp[0][i].r = re > INT16_MAX ? INT16_MAX : re < INT16_MIN ? INT16_MIN : re;
      p_rxComp[0][i].i = im > INT16_MAX ? INT16_MAX : im < INT16_MIN ? INT16_MIN : im;
    }
  }
  stop_meas_nr_ue_phy(ue, DLSCH_CHANNEL_COMPENSATION_STATS);
  if (meas_enabled) {
    LOG_D(PHY,
          "[AbsSFN %u.%d] Slot%d Symbol %d log2_maxh %d Channel Comp  %5.2f \n",
          frame,
          nr_slot_rx,
          slot,
          symbol,
          *log2_maxh,
          ue->phy_cpu_stats.cpu_time_stats[DLSCH_CHANNEL_COMPENSATION_STATS].p_time / (cpuf * 1000.0));
  }
  // Please keep it: useful for debugging
#ifdef DEBUG_PDSCH_RX
  char filename[50];
  snprintf(filename, 50, "rxdataF0_symb_%d_nr_slot_rx_%d.m", symbol, nr_slot_rx);
  write_output(filename, "rxdataF0", &rxdataF[0][symbol * fp->ofdm_symbol_size], fp->ofdm_symbol_size, 1, 1);
  snprintf(filename, 50, "dl_ch_estimates0_symb_%d_nr_slot_rx_%d.m", symbol, nr_slot_rx);
  write_output(filename, "dl_ch_estimates0", &dl_ch_estimates[0][symbol * fp->ofdm_symbol_size], fp->ofdm_symbol_size, 1, 1);
  snprintf(filename, 50, "rxdataF_ext0_symb_%d_nr_slot_rx_%d.m", symbol, nr_slot_rx);
  write_output(filename, "rxdataF_ext0", &rxdataF_ext[0][0], rx_size_symbol, 1, 1);
  snprintf(filename, 50, "dl_ch_estimates_ext0_symb_%d_nr_slot_rx_%d.m", symbol, nr_slot_rx);
  write_output(filename, "dl_ch_estimates_ext0", &dl_ch_estimates_ext[0][0], rx_size_symbol, 1, 1);
  snprintf(filename, 50, "rxdataF_comp00_symb_%d_nr_slot_rx_%d.m", symbol, nr_slot_rx);
  write_output(filename, "rxdataF_comp00", &rxdataF_comp[0][0][symbol * pdsch_buf_size_max], pdsch_buf_size_max, 1, 1);
#endif

  /* ISAC_RX_QAM_AGC=1 (T26d): amplitude consistency of the QAM soft decisions. nr_channel_compensation() gives
   * z = sum_a h_a* y_a and the QAM thresholds mag = sum_a |h_a|^2 * n1; for a coherent combination the symbol mean of
   * |Re z|,|Im z| equals the mean of mag_a (n1 = mean |x| per dimension for 16/64/256QAM). Branches whose phase drifts
   * differently between the DM-RS and the data symbols (independent X410 board LOs) add partly incoherently in MRC,
   * shrinking |z| against mag, which biases every amplitude bit toward the inner points: QPSK is immune, QAM is not.
   * kappa = mean(|Re z|,|Im z|) / mean(mag_a) rescales the thresholds per symbol (kappa also logged via EVMGRANT). */
  if (nl == 1 && dlsch->cw_info.qamModOrder > 2 && nb_re_pdsch >= 24) {
    static int s_agc = -1;
    if (s_agc < 0) s_agc = getenv("ISAC_RX_QAM_AGC") ? atoi(getenv("ISAC_RX_QAM_AGC")) : 0;
    const c16_t *z = p_rxComp[0];
    const c16_t *ma = dl_ch_mag[symbol][0];
    double sz = 0.0, sm = 0.0;
    for (uint32_t i = 0; i < nb_re_pdsch; i++) {
      sz += abs(z[i].r) + abs(z[i].i);
      sm += 2.0 * ma[i].r;
    }
    const double kappa = sm > 0.0 ? sz / sm : 1.0;
    if (nr_pdsch_evm_acc.armed) { nr_pdsch_evm_acc.kappa_sum += kappa; nr_pdsch_evm_acc.kappa_n++; }
    if (s_agc && kappa > 0.2 && kappa < 1.5) {
      const int16_t kq = (int16_t)lround(kappa * 32767.0 > 32767.0 ? 32767.0 : kappa * 32767.0);
      const int qm = dlsch->cw_info.qamModOrder;
      if (kappa < 1.0)
        for (uint32_t i = 0; i < nb_re_pdsch; i++) {
          c16_t *a1 = &dl_ch_mag[symbol][0][i], *b1 = &dl_ch_magb[symbol][0][i], *r1 = &dl_ch_magr[symbol][0][i];
          a1->r = (int16_t)(((int32_t)a1->r * kq) >> 15); a1->i = (int16_t)(((int32_t)a1->i * kq) >> 15);
          if (qm > 4) { b1->r = (int16_t)(((int32_t)b1->r * kq) >> 15); b1->i = (int16_t)(((int32_t)b1->i * kq) >> 15); }
          if (qm > 6) { r1->r = (int16_t)(((int32_t)r1->r * kq) >> 15); r1->i = (int16_t)(((int32_t)r1->i * kq) >> 15); }
        }
      else if (s_agc >= 2) { /* =2 only: kappa >= 1 scales the thresholds UP (noise inflates mean |z|; off by default) */
        for (uint32_t i = 0; i < nb_re_pdsch; i++) {
          c16_t *a1 = &dl_ch_mag[symbol][0][i], *b1 = &dl_ch_magb[symbol][0][i], *r1 = &dl_ch_magr[symbol][0][i];
          const double f = kappa;
          #define SATK(v) ((int16_t)fmax(-32768.0, fmin(32767.0, (double)(v) * f)))
          a1->r = SATK(a1->r); a1->i = SATK(a1->i);
          if (qm > 4) { b1->r = SATK(b1->r); b1->i = SATK(b1->i); }
          if (qm > 6) { r1->r = SATK(r1->r); r1->i = SATK(r1->i); }
          #undef SATK
        }
      }
    }
  }

  // MRC is performed inline by nr_channel_compensation; apply MMSE for multi-layer
  start_meas_nr_ue_phy(ue, DLSCH_MRC_MMSE_STATS);
  if (nb_re_pdsch) {
    const uint8_t qamModOrder = dlsch->cw_info.qamModOrder;

    if ((nl > 2) || (nl == 2 && !do_ml)) {
      nr_dlsch_mmse(pdsch_buf_size_max,
                    rx_size_symbol,
                    nbRx,
                    nl,
                    rxdataF_comp[symbol],
                    dl_ch_mag[symbol],
                    dl_ch_magb[symbol],
                    dl_ch_magr[symbol],
                    dl_ch_estimates_ext,
                    qamModOrder,
                    *log2_maxh,
                    nb_re_pdsch,
                    qam_mode == 0 ? nvar : 0, /* modes 1/2: unbiased ZF, no diagonal loading */
                    qam_mode);
      /* QAM-FIX diagnostic: amplitude ratio of the multi-layer soft output against its own thresholds.
       * For an unbiased equaliser E|Re z| = mag (n1 = mean |x| per dimension of 16/64/256QAM), so
       * kappa ~ 1 at high SNR; a shrunk (biased) output reads kappa < 1. Logged by EVMGRANT. */
      if (nr_pdsch_evm_acc.armed && qamModOrder > 2 && nb_re_pdsch >= 24) {
        for (int l = 0; l < nl; l++) {
          const c16_t *z = rxdataF_comp[symbol][l], *ma = dl_ch_mag[symbol][qam_mode == 2 ? l : 0];
          double sz = 0.0, sm = 0.0;
          for (uint32_t i = 0; i < nb_re_pdsch; i++) {
            sz += abs(z[i].r) + abs(z[i].i);
            sm += 2.0 * ma[i].r;
          }
          if (sm > 0.0) {
            nr_pdsch_evm_acc.kappa_sum += sz / sm;
            nr_pdsch_evm_acc.kappa_n++;
          }
        }
      }
    } else if ((nl == 2) && (qamModOrder > 6) && do_ml) {
      nr_mmse_2layers(p_rxComp,
                      rx_size_symbol,
                      pdsch_buf_size_max,
                      nbRx,
                      nl,
                      dl_ch_mag[symbol],
                      dl_ch_magb[symbol],
                      dl_ch_magr[symbol],
                      chFext,
                      freq_alloc->num_rbs,
                      qamModOrder,
                      *log2_maxh,
                      0,
                      nb_re_pdsch,
                      nvar);
    }
  }
  stop_meas_nr_ue_phy(ue, DLSCH_MRC_MMSE_STATS);
  if (decode_ref_sign) {
    for (int l = 0; l < nl; l++)
      for (uint32_t i = 0; i < nb_re_pdsch; i++) {
        const int16_t v[2] = {rxdataF_comp[symbol][l][i].r, rxdataF_comp[symbol][l][i].i};
        for (int q = 0; q < 2; q++) {
          const int ref = decode_ref_sign[2 * (l * nb_re_pdsch + i) + q];
          if (!ref) continue;
          nr_pdsch_decode_signs++;
          nr_pdsch_decode_sign_errors += (v[q] < 0 ? -1 : 1) != ref;
        }
      }
    free(decode_ref_sign);
  }

  if (meas_enabled) {
    LOG_D(PHY,
          "[AbsSFN %u.%d] Slot%d Symbol %d: Channel Combine and MMSE %5.2f \n",
          frame,
          nr_slot_rx,
          slot,
          symbol,
          ue->phy_cpu_stats.cpu_time_stats[DLSCH_MRC_MMSE_STATS].p_time / (cpuf * 1000.0));
  }

  /* Store the valid DL RE's */
  dl_valid_re[symbol] = nb_re_pdsch;
  int startSymbIdx = 0;
  int nbSymb = 0;
  int pduBitmap = 0;

  if(dlsch_harq->status == NR_ACTIVE) {
    startSymbIdx = dlsch_config->start_symbol;
    nbSymb = dlsch_config->number_symbols;
    pduBitmap = dlsch_config->pduBitmap;
  }

  /* PTRS processing for multiple antenna ports is broken because the following
  function estimates phase offset from and applies compensation to rxdataF_comp
  for each antenna port but rxdataF_comp has MRCed data. */
  /* TODO: Move PTRS phase estimation before immediately after DMRS channels
  estimation and apply PTRS phase compensation in nr_channel_compensationi() */
  /* Check for PTRS bitmap and process it respectively */
  if((pduBitmap & 0x1) && (dlsch->rnti_type == TYPE_C_RNTI_)) {
    nr_pdsch_ptrs_processing(1, // rxdataF_comp is MRCed so no point in processing all antenna ports. Fixme.
                             ptrs_phase_per_slot,
                             ptrs_re_per_slot,
                             pdsch_buf_size_max,
                             nl,
                             rxdataF_comp,
                             fp,
                             dlsch_config,
                             nr_slot_rx,
                             symbol,
                             freq_alloc->num_rbs,
                             dlsch->rnti,
                             &dlsch->ptrs_symbols,
                             &dlsch->ptrs_symbol_index);
    dl_valid_re[symbol] -= ptrs_re_per_slot[0][symbol];
    /* nr_ptrs_cpe_estimation() compacts the PT-RS REs out of LAYER 0 only (the "1" above makes it
     * index rxdataF_comp[symbol][0]). PT-RS REs carry no PDSCH on ANY layer, and the LLR stage
     * reads dl_valid_re[symbol] contiguous REs from every layer -- so layers 1..nl-1 stayed
     * misaligned by one RE per PT-RS RE on every PT-RS symbol. Invisible at nl=1; at nl=4 it is
     * the difference between MCS 9 (no PT-RS) at 100 % and MCS 25 (PT-RS on) at 5 % CRC on a
     * channel with 1.5 % per-layer EVM (rfsim, 2026-09-16). Same RE rule, no phase rotation: the
     * PT-RS port is layer 0's, so there is nothing to estimate a CPE from on the other layers. */
    if (ptrs_re_per_slot[0][symbol] > 0) {
      const int nre = freq_alloc->num_rbs * NR_NB_SC_PER_RB;
      for (int l = 1; l < nl; l++) {
        c16_t *rx = rxdataF_comp[symbol][l];
        int cnt = 0;
        for (int re = 0; re < nre; re++)
          if (!is_ptrs_subcarrier(re, dlsch->rnti, dlsch_config->PTRSFreqDensity, freq_alloc->num_rbs,
                                  dlsch_config->PTRSReOffset, 0, fp->ofdm_symbol_size))
            rx[cnt++] = rx[re];
      }
    }
  }

  /* at last symbol in a slot calculate LLR's for whole slot */
  if (symbol == (startSymbIdx + nbSymb - 1)) {
    /* create LLR layer buffer */
    int max_symb_re = 0;
    GET_ARRAY_MAX(dl_valid_re, NR_SYMBOLS_PER_SLOT, max_symb_re);
    /* Row stride must stay 32-byte aligned: the QAM LLR kernels use aligned AVX2 stores, and a PT-RS
     * symbol count (e.g. 3276-137 = 3139 REs) times Qm is not a multiple of 16 int16. */
    const int llr_per_symbol = (max_symb_re * dlsch->cw_info.qamModOrder + 15) & ~15;
    __attribute__((aligned(32))) int16_t layer_llr[NR_SYMBOLS_PER_SLOT][nl][llr_per_symbol];

    // Generate LLR from PTRS compensated signal
    const uint8_t qamModOrder = dlsch->cw_info.qamModOrder;
    start_meas_nr_ue_phy(ue, DLSCH_LLR_STATS);
    for (int llr_sym = startSymbIdx; llr_sym < startSymbIdx + nbSymb; llr_sym++) {
      if (nl == 2 && qamModOrder <= 6 && do_ml) {
        // 2-layer QPSK/16QAM/64QAM: joint ML-LLR using inter-layer Tx correlation
        // rho_dl[llr_sym] is laid out as [nl*nl][rx_size_symbol]:
        // index 1 = rho[0][1], index nl (=2) = rho[1][0]
        nr_compute_ML_llr(rxdataF_comp[llr_sym][0],
                          rxdataF_comp[llr_sym][1],
                          dl_ch_mag[llr_sym][0],
                          dl_ch_mag[llr_sym][1],
                          layer_llr[llr_sym][0],
                          layer_llr[llr_sym][1],
                          rho_dl[llr_sym][1],
                          rho_dl[llr_sym][nl],
                          dl_valid_re[llr_sym],
                          qamModOrder);
      } else {
        nr_dlsch_llr(dlsch,
                     dl_valid_re[llr_sym],
                     pdsch_buf_size_max,
                     dl_ch_mag[llr_sym],
                     dl_ch_magb[llr_sym],
                     dl_ch_magr[llr_sym],
                     qam_mode == 2 && nl >= 2,
                     nbRx,
                     rxdataF_comp[llr_sym],
                     llr_per_symbol,
                     layer_llr[llr_sym]);
      }
    }
    stop_meas_nr_ue_phy(ue, DLSCH_LLR_STATS);
    start_meas_nr_ue_phy(ue, DLSCH_LAYER_DEMAPPING);
    nr_dlsch_layer_demapping(nl, dlsch->cw_info.qamModOrder, llr_per_symbol, layer_llr, dlsch_config, dl_valid_re, llr);
    stop_meas_nr_ue_phy(ue, DLSCH_LAYER_DEMAPPING);

    if (UEScopeHasTryLock(ue)) {
      metadata mt = {.frame = proc->frame_rx, .slot = proc->nr_slot_rx };
      int total_valid_res = 0;
      for (int i = startSymbIdx; i < startSymbIdx + nbSymb; i++) {
        total_valid_res += dl_valid_re[i];
      }
      if (UETryLockScopeData(ue, pdschRxdataF_comp, sizeof(c16_t), 1,  total_valid_res, &mt)) {
        size_t offset = 0;
        for (int i = startSymbIdx; i < startSymbIdx + nbSymb; i++) {
          size_t data_size = sizeof(c16_t) * dl_valid_re[i];
          UEscopeCopyUnsafe(ue, pdschRxdataF_comp, &rxdataF_comp[i][0][0], data_size, offset, i);
          offset += data_size;
        }
        UEunlockScopeData(ue, pdschRxdataF_comp)
      }
    } else {
      UEscopeCopy(ue, pdschRxdataF_comp, rxdataF_comp[0], sizeof(c16_t), nl, pdsch_buf_size_max, 0);
    }
  }

  if (meas_enabled) {
    LOG_D(PHY,
          "[AbsSFN %u.%d] Slot%d Symbol %d: LLR Computation  %5.2f \n",
          frame,
          nr_slot_rx,
          slot,
          symbol,
          ue->phy_cpu_stats.cpu_time_stats[DLSCH_LLR_STATS].p_time / (cpuf * 1000.0));
  }

#if T_TRACER
  T(T_UE_PHY_PDSCH_IQ,
    T_INT(gNB_id),
    T_INT(frame % 1024),
    T_INT(nr_slot_rx),
    T_INT(nb_rb_pdsch),
    T_INT(fp->N_RB_DL),
    T_INT(fp->symbols_per_slot),
    T_BUFFER(&rxdataF_comp[gNB_id][0], 2 * fp->N_RB_DL * 12 * fp->symbols_per_slot * 2));
#endif

  if (ue->phy_sim_pdsch_rxdataF_comp) {
    for (int a = 0; a < nbRx; a++) {
      memcpy((c16_t *)ue->phy_sim_pdsch_dl_ch_estimates + pdsch_est_size * a, dl_ch_estimates, pdsch_est_size * sizeof(c16_t));
    }
    for (int l = 0; l < nl; l++) {
      int offset = (void *)rxdataF_comp[symbol][l] - (void *)rxdataF_comp[0];
      memcpy(ue->phy_sim_pdsch_rxdataF_comp + offset, rxdataF_comp[symbol][l], sizeof(c16_t) * pdsch_buf_size_max);
    }
  }
  if (ue->phy_sim_pdsch_dl_ch_estimates_ext)
    memcpy(ue->phy_sim_pdsch_dl_ch_estimates_ext + symbol * sizeof(dl_ch_estimates_ext),
           dl_ch_estimates_ext,
           sizeof(dl_ch_estimates_ext));
  return 0;
}
