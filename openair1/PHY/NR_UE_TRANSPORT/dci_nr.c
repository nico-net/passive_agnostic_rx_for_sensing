/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief Implements PDCCH physical channel TX/RX procedures (36.211) and DCI encoding/decoding (36.212/36.213). Current LTE
 * compliance V8.6 2009-03.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "executables/softmodem-common.h"
#include "nr_transport_proto_ue.h"
#include <math.h>
#include "PHY/CODING/nrPolar_tools/nr_polar_dci_defs.h"
#include "PHY/phy_extern.h"
#include "PHY/CODING/coding_extern.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "PHY/sse_intrin.h"
#include "common/utils/nr/nr_common.h"
#include <openair1/PHY/TOOLS/phy_scope_interface.h>
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"

#include "assertions.h"
#include "T.h"

// #define NR_PDCCH_DCI_DEBUG // activates NR_PDCCH_DCI_DEBUG logs
#ifdef NR_PDCCH_DCI_DEBUG
#define LOG_DDD(a, ...) printf("<-NR_PDCCH_DCI_DEBUG (%s)-> " a, __func__, ##__VA_ARGS__ )
#define LOG_DSYMB(b)                                                               \
  LOG_DDD("RB[c_rb %d] \t RE[re %d] => rxF_ext[%d]=(%d,%d)\t rxF[%d]=(%d,%d)\n" b, \
          c_rb,                                                                    \
          i,                                                                       \
          j,                                                                       \
          rxF_ext[j].r,                                                            \
          rxF_ext[j].i,                                                            \
          i,                                                                       \
          rxF[i].r,                                                                \
          rxF[i].i)
#else
#define LOG_DDD(a...)
#define LOG_DSYMB(a...)
#endif

#define NR_NBR_CORESET_ACT_BWP 3 // The number of CoreSets per BWP is limited to 3 (including initial CORESET: ControlResourceId 0)

#define RE_PER_RB 12
// after removing the 3 DMRS RE, the RB contains 9 RE with PDCCH
#define RE_PER_RB_OUT_DMRS 9

// Exported (was static) for openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c's blind-decode
// tap, which needs the exact same "already-demapped/already-unscrambled" e_rx/tmp_e shape this
// file's own nr_dci_decoding_procedure() consumes, but without going through that function's
// own-RNTI-only equality gate (line ~541 below). Pure linkage change, no logic touched -- see
// TOTAL_PASSIVE_UE_HANDOVER.md Phase 3 and /home/sens/.claude/plans/zesty-baking-thompson.md.
void nr_pdcch_demapping_deinterleaving(uint32_t coreset_nbr_rb,
                                              c16_t *llr,
                                              c16_t *e_rx,
                                              uint8_t coreset_time_dur,
                                              uint8_t reg_bundle_size_L_in,
                                              uint8_t coreset_interleaver_size_R,
                                              uint8_t n_shift,
                                              uint8_t number_of_candidates,
                                              uint16_t *CCE,
                                              uint8_t *L,
                                              int llr_stride_per_symbol)
{
  /*
   * This function will do demapping and deinterleaving from llr containing demodulated symbols
   * Demapping will regroup in REG and bundles
   * Deinterleaving will order the bundles
   *
   * In the following example we can see the process. The llr contains the demodulated IQs, but they are not ordered from
   REG 0,1,2,..
   * In e_rx (z) we will order the REG ids and group them into bundles.
   * Then we will put the bundles in the correct order as indicated in subclause 7.3.2.2
   *
   llr --------------------------> e_rx (z) ----> e_rx (z)
   |   ...
   |   ...
   |   REG 26
   symbol 2    |   ...
   |   ...
   |   REG 5
   |   REG 2

   |   ...
   |   ...
   |   REG 25
   symbol 1    |   ...
   |   ...
   |   REG 4
   |   REG 1

   |   ...
   |   ...                           ...              ...
   |   REG 24 (bundle 7)             ...              ...
   symbol 0    |   ...                           bundle 3         bundle 6
   |   ...                           bundle 2         bundle 1
   |   REG 3                         bundle 1         bundle 7
   |   REG 0  (bundle 0)             bundle 0         bundle 0

  */
  const int N_regs = coreset_nbr_rb * coreset_time_dur;
  /* interleaving will be done only if reg_bundle_size_L != 0 */
  const int coreset_C = (reg_bundle_size_L_in != 0) ? (uint32_t)(N_regs / (coreset_interleaver_size_R * reg_bundle_size_L_in)) : 0;
  const int coreset_interleaved = (reg_bundle_size_L_in != 0) ? 1 : 0;
  const int reg_bundle_size_L = (reg_bundle_size_L_in != 0) ? reg_bundle_size_L_in : 6;

  int B_rb = reg_bundle_size_L / coreset_time_dur; // nb of RBs occupied by each REG bundle
  int num_bundles_per_cce = 6 / reg_bundle_size_L;
  int n_cce = N_regs / 6;
  int max_bundles = n_cce * num_bundles_per_cce;
  int f_bundle_j_list[max_bundles];
  // for each bundle
  int c = 0, r = 0, f_bundle_j = 0;
  for (int nb = 0; nb < max_bundles; nb++) {
    if (coreset_interleaved == 0)
      f_bundle_j = nb;
    else {
      if (r == coreset_interleaver_size_R) {
        r = 0;
        c++;
      }
      f_bundle_j = ((r * coreset_C) + c + n_shift) % (N_regs / reg_bundle_size_L);
      r++;
    }
    f_bundle_j_list[nb] = f_bundle_j;
  }

  // Get cce_list indices by bundle index in ascending order
  int f_bundle_j_list_ord[number_of_candidates][max_bundles];
  for (int c_id = 0; c_id < number_of_candidates; c_id++) {
    int start_bund_cand = CCE[c_id] * num_bundles_per_cce;
    int max_bund_per_cand = L[c_id] * num_bundles_per_cce;
    int f_bundle_j_list_id = 0;
    for (int nb = 0; nb < max_bundles; nb++) {
      for (int bund_cand = start_bund_cand; bund_cand < start_bund_cand + max_bund_per_cand; bund_cand++) {
        if (f_bundle_j_list[bund_cand] == nb) {
          f_bundle_j_list_ord[c_id][f_bundle_j_list_id] = nb;
          f_bundle_j_list_id++;
        }
      }
    }
  }

  int rb_count = 0;
  for (int c_id = 0; c_id < number_of_candidates; c_id++) {
    for (int symbol_idx = 0; symbol_idx < coreset_time_dur; symbol_idx++) {
      for (int cce_count = 0; cce_count < L[c_id]; cce_count++) {
        for (int k = 0; k < NR_NB_REG_PER_CCE / reg_bundle_size_L; k++) { // loop over REG bundles
          int f = f_bundle_j_list_ord[c_id][k + NR_NB_REG_PER_CCE * cce_count / reg_bundle_size_L];
          c16_t *in = llr + f * B_rb * RE_PER_RB_OUT_DMRS + symbol_idx * llr_stride_per_symbol;
          // loop over the RBs of the bundle
          memcpy(e_rx + RE_PER_RB_OUT_DMRS * rb_count, in, B_rb * RE_PER_RB_OUT_DMRS * sizeof(*e_rx));
          rb_count += B_rb;
        }
      }
    }
  }
}

/* Opt-in headroom correction for BLIND PDCCH monitoring (0 = off = stock behaviour).
 *
 * nr_rx_pdcch_symbol() picks its equaliser output scale from
 *     log2_maxh = log2_approx(avgs)/2 + 5
 * where avgs is the mean channel level over the WHOLE CORESET. For a UE decoding its own grants
 * that is fine: its CORESET is small and largely occupied, so the mean is representative of the
 * REs that carry the DCI. A blind monitor is the opposite case -- it watches a full-BWP CORESET
 * (here 270 RB = 2430 REs) of which only a couple of CCEs are ever transmitted, so avgs is set by
 * EMPTY, noise-only REs. The resulting shift is far too small and the REs that do carry PDCCH
 * overshoot nr_pdcch_llr()'s [-32,31] clip by a wide margin.
 *
 * MEASURED on this cell (273 PRB, 4 rx, blind monitor): mean_mag 5-8 with peak 375-691 in the same
 * symbol -- i.e. real PDCCH REs sitting 12-21x above the clip rail. Clipping at that ratio does not
 * merely compress amplitude, it rotates the symbol (a QPSK point (203,-14) clips to (31,-14)), so
 * the soft bits handed to the polar decoder are wrong in PHASE, not just scale.
 *
 * When enabled, the scale is derived per symbol from the observed peak rather than from a mean that
 * the empty REs dominate -- no hand-tuned constant, and it self-adjusts with received level. */
int nr_pdcch_blind_llr_autoscale = 1;
int nr_pdcch_blind_dmrs_probe = 0;
static int    g_dmrs_hot_cce = -1;
int nr_pdcch_blind_capture = 0;   /* 1 = write the replay fixture */
static FILE  *g_cap_fp = NULL;
static int    g_cap_left = 400;
static double g_dmrs_hot_nc = 0.0; /* TEMPORARY stage-split probe, see DMRSPROBE below */

static void nr_pdcch_llr(uint32_t sz, c16_t *rxF, c16_t *llr)
{
  for (int i = 0; i < sz; i++) {
    // We clip the signal
    c16_t res;
    res.r = min(rxF->r, 31);
    res.r = max(-32, res.r);
    res.i = min(rxF->i, 31);
    res.i = max(-32, res.i);
    *llr++ = res;
    LOG_DDD("llr logs: rb=%d i=%d rxF:%d,%d => pdcch_llr:%d,%d\n", i / 18, i, rxF->r, rxF->i, llr->r, llr->i);
    rxF++;
  }
}

// This function will extract the mapped DM-RS PDCCH REs as per 38.211 Section 7.4.1.3.2 (Mapping to physical resources)
static void nr_pdcch_extract_rbs_single(uint32_t rxdataF_sz,
                                        c16_t rxdataF[][rxdataF_sz],
                                        int32_t est_size,
                                        c16_t dl_ch_estimates[][est_size],
                                        int arraySz,
                                        c16_t rxdataF_ext[][arraySz],
                                        c16_t dl_ch_estimates_ext[][arraySz],
                                        NR_DL_FRAME_PARMS *frame_parms,
                                        uint8_t *coreset_freq_dom,
                                        uint32_t rb_offset,
                                        uint32_t coreset_nbr_rb,
                                        uint32_t n_BWP_start)
{
  /*
   * This function is demapping DM-RS PDCCH RE
   * Implementing 38.211 Section 7.4.1.3.2 Mapping to physical resources
   * PDCCH DM-RS signals are mapped on RE a_k_l where:
   * k = 12*n + 4*kprime + 1
   * n=0,1,..
   * kprime=0,1,2
   * According to this equations, DM-RS PDCCH are mapped on k where k%12==1 || k%12==5 || k%12==9
   *
   */

  for (int aarx = 0; aarx < frame_parms->nb_antennas_rx; aarx++) {
    c16_t *dl_ch0 = dl_ch_estimates[aarx];
    c16_t *rxFbase = rxdataF[aarx];
    LOG_DDD("dl_ch0 = &dl_ch_estimates[aarx = (%d)][0]\n", aarx);

    c16_t *dl_ch0_ext = dl_ch_estimates_ext[aarx];
    c16_t *rxF_ext = rxdataF_ext[aarx];

    /*
     * The following for loop handles treatment of PDCCH contained in table rxdataF (in frequency domain)
     * In NR the PDCCH IQ symbols are contained within RBs in the CORESET defined by higher layers which is located within the BWP
     * Lets consider that the first RB to be considered as part of the CORESET and part of the PDCCH is n_BWP_start
     * Several cases have to be handled differently as IQ symbols are situated in different parts of rxdataF:
     * 1. Number of RBs in the system bandwidth is even
     *    1.1 The RB is <  than the N_RB_DL/2 -> IQ symbols are in the second half of the rxdataF (from first_carrier_offset)
     *    1.2 The RB is >= than the N_RB_DL/2 -> IQ symbols are in the first half of the rxdataF (from element 0)
     * 2. Number of RBs in the system bandwidth is odd
     * (particular case when the RB with DC as it is treated differently: it is situated in symbol borders of rxdataF)
     *    2.1 The RB is <  than the N_RB_DL/2 -> IQ symbols are in the second half of the rxdataF (from first_carrier_offset)
     *    2.2 The RB is >  than the N_RB_DL/2 -> IQ symbols are in the first half of the rxdataF (from element 0 + 2nd half RB
     * containing DC) 2.3 The RB is == N_RB_DL/2          -> IQ symbols are in the upper border of the rxdataF for first 6 IQ
     * element and the lower border of the rxdataF for the last 6 IQ elements If the first RB containing PDCCH within the UE BWP
     * and within the CORESET is higher than half of the system bandwidth (N_RB_DL), then the IQ symbol is going to be found at
     * the position 0+c_rb-N_RB_DL/2 in rxdataF and we have to point the pointer at (1+c_rb-N_RB_DL/2) in rxdataF
     */

    c16_t middle_prb_buffer[RE_PER_RB];
    int start = rb_offset / 6;
    int size = coreset_nbr_rb / 6;
    for (int rb_group = start; rb_group < start + size; rb_group++) {
      if ((coreset_freq_dom[rb_group / 8] & (1 << (7 - (rb_group & 7)))) == 0) {
        continue;
      }
      for (int rb = 0; rb < 6; rb++) {
        int c_rb = rb_group * 6 + rb;
        c16_t *rxF = NULL;
        if ((frame_parms->N_RB_DL & 1) == 0) {
          if ((c_rb + n_BWP_start) < frame_parms->N_RB_DL / 2)
            // if RB to be treated is lower than middle system bandwidth then rxdataF pointed
            // at (offset + c_br + symbol * ofdm_symbol_size): even case
            rxF = rxFbase + frame_parms->first_carrier_offset + RE_PER_RB * (c_rb + n_BWP_start);
          else
            // number of RBs is even  and c_rb is higher than half system bandwidth (we don't skip DC)
            // if these conditions are true the pointer has to be situated at the 1st part of the rxdataF
            // we point at the 1st part of the rxdataF in symbol
            rxF = rxFbase + RE_PER_RB * (c_rb + n_BWP_start - frame_parms->N_RB_DL / 2);
        } else {
          if ((c_rb + n_BWP_start) <= frame_parms->N_RB_DL / 2)
            // if RB to be treated is lower than middle system bandwidth then rxdataF pointed
            //  at (offset + c_br + symbol * ofdm_symbol_size): odd case
            // Reassemble the middle PRB
            if (c_rb + n_BWP_start == frame_parms->N_RB_DL / 2) {
              memcpy(middle_prb_buffer, rxFbase + frame_parms->ofdm_symbol_size - RE_PER_RB / 2, sizeof(c16_t) * RE_PER_RB / 2);
              memcpy(middle_prb_buffer + RE_PER_RB / 2, rxFbase, sizeof(c16_t) * RE_PER_RB / 2);
              rxF = middle_prb_buffer;
            } else {
              rxF = rxFbase + frame_parms->first_carrier_offset + RE_PER_RB * (c_rb + n_BWP_start);
            }

          else
            // number of RBs is odd  and c_rb is higher than half system bandwidth + 1
            // if these conditions are true the pointer has to be situated at the 1st part of
            // the rxdataF just after the first IQ symbols of the RB containing DC
            // we point at the 1st part of the rxdataF in symbol
            rxF = rxFbase + RE_PER_RB * (c_rb + n_BWP_start - frame_parms->N_RB_DL / 2) - 6;
        }

        const int valid_re[RE_PER_RB_OUT_DMRS] = {0, 2, 3, 4, 6, 7, 8, 10, 11};
        for (int i = 0; i < sizeofArray(valid_re); i++) {
          *rxF_ext++ = rxF[valid_re[i]];
          *dl_ch0_ext++ = dl_ch0[valid_re[i]];
        }
        dl_ch0 += RE_PER_RB;
      }
    }
  }
}

static void nr_pdcch_channel_compensation(int arraySz,
                                          int sz2,
                                          c16_t rxdataF_ext[][sz2],
                                          c16_t dl_ch_estimates_ext[][sz2],
                                          c16_t rxdataF_comp[][arraySz],
                                          int antRx,
                                          uint8_t output_shift)
{
  for (int aarx = 0; aarx < antRx; aarx++) {
    // multiply by conjugated channel, this function require size in _m128i, else it doesn't process all samples
    mult_cpx_conj_vector(dl_ch_estimates_ext[aarx], rxdataF_ext[aarx], rxdataF_comp[aarx], arraySz, output_shift);
  }
}

static void nr_pdcch_detection_mrc(int nb_ant, int sz, c16_t rxdataF_comp[][sz])
{
  /* The four-RX "branch 0 only" skip that used to sit here is GONE (2026-09-15). Measured on the
   * X410: 1 RX decodes SIB1 on the 5th SI-RNTI candidate, 2 RX (MRC of the two WEAK antennas) on
   * the 47th, 4 RX single-branch -- branch 0 or the strongest -- 0 in 36k candidates. The loop
   * below halves both operands before every add, so it cannot overflow at any branch count; the
   * overflow argument belonged to nr_pbch's plain-adds loop, not to this one. */

  /* NOTE -- an "equal-gain, 32-bit accumulator, divide by nb_ant" rewrite of the loop below is a
   * REGRESSION: the output feeds nr_pdcch_llr(), which CLIPS at +/-31, so absolute amplitude --
   * not just relative branch weighting -- sets the soft-bit resolution. Dividing by nb_ant drops
   * the signal ~4x below the rail and the LLRs collapse to 0/+-1. The cascade below is lopsided
   * (a0/8 + a1/8 + a2/4 + a3/2 at four branches) but keeps the sum near the rail, which matters
   * more here. Do not "fix" the weighting without renormalising to the clip rail and re-measuring.
   *
   * The passive-mode carve-out that used to sit here (combine instead of skip when
   * IS_PASSIVE_RX_MODE) has been REVERTED to the baseline: its justification was the 817-vs-8 /
   * 3202-vs-11 recovered-RNTI counts, which X410_BLIND_PDCCH_HANDOVER.md section 5 retracted as
   * 0x5199, a degenerate polar-decoder fixed point on empty CCEs. Re-measure before reinstating. */

  c16_t *rx0 = rxdataF_comp[0];
  // MRC on each re of rb
  // input always aligned and accepting tail padding to process all actual samples
  for (int a = 1; a < nb_ant; a++) {
    c16_t *rx = rxdataF_comp[a];
    for (int i = 0; i < sz; i += 4) {
      *(simde__m128i *)(rx0 + i) = simde_mm_adds_epi16(simde_mm_srai_epi16(*(simde__m128i *)(rx0 + i), 1),
                                                       simde_mm_srai_epi16(*(simde__m128i *)(rx + i), 1));
    }
  }
}

/* Produce LLRs from received PDCCH signal */
static void nr_rx_pdcch_symbol(PHY_VARS_NR_UE *ue,
                               const UE_nr_rxtx_proc_t *proc,
                               int symbol,
                               int ss_idx,
                               nr_phy_data_t *phy_data,
                               int llr_size_symbol,
                               c16_t rxdataF[ue->frame_parms.nb_antennas_rx][ue->frame_parms.ofdm_symbol_size],
                               c16_t llr[llr_size_symbol])
{
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  NR_UE_PDCCH_CONFIG *phy_pdcch_config = &phy_data->phy_pdcch_config;
  fapi_nr_coreset_t *coreset = &phy_pdcch_config->pdcch_config[ss_idx].coreset;
  int32_t pdcch_est_size = ceil_mod(fp->ofdm_symbol_size + LTE_CE_FILTER_LENGTH, 16);
  __attribute__((aligned(16))) c16_t pdcch_dl_ch_estimates[fp->nb_antennas_rx][pdcch_est_size];
  int n_rb, cset_start;
  unsigned short cs_sc_cap = 0; /* CORESET start subcarrier, copied out for the replay fixture */
  get_coreset_rballoc(coreset->frequency_domain_resource, &n_rb, &cset_start);
  int rb_offset = cset_start + coreset->rb_offset;
  unsigned short scrambling_id = coreset->pdcch_dmrs_scrambling_id;
  int dmrs_ref = 0;
  if (coreset->CoreSetType == NFAPI_NR_CSET_CONFIG_PDCCH_CONFIG)
    dmrs_ref = phy_pdcch_config->pdcch_config[ss_idx].BWPStart;
  /* CFGTRACE (ISAC_PDCCH_CFGTRACE=1): dump every DERIVED PDCCH parameter for each invocation.
   * The blind monitor and the normal MAC-driven path both call this function, so with an attached
   * UE both appear in one log and can be diffed line-by-line for the SAME slot -- which is the only
   * way to see which derived value the synthetic blind config gets wrong. Read-only, off by default. */
  {
    static int s_cfgtrace = -1;
    if (s_cfgtrace < 0)
      s_cfgtrace = (getenv("ISAC_PDCCH_CFGTRACE") != NULL) ? 1 : 0;
    static int s_cfgslot = -2;
    if (s_cfgslot == -2) {
      const char *e = getenv("ISAC_PDCCH_CFGTRACE_SLOT");
      s_cfgslot = e ? atoi(e) : -1; // -1 = every slot
    }
    if (s_cfgtrace && (s_cfgslot < 0 || proc->nr_slot_rx == s_cfgslot))
      LOG_I(PHY,
            "PDCCHCFG f=%d s=%d ss=%d type=%d n_rb=%d cset_start=%d rb_offset=%d dmrs_ref=%d "
            "scr_id=%d BWPStart=%d BWPSize=%d dur=%d bundle=%d ilv=%d shift=%d ncand=%d\n",
            proc->frame_rx, proc->nr_slot_rx, ss_idx, (int)coreset->CoreSetType, n_rb, cset_start,
            (int)rb_offset, dmrs_ref, (int)scrambling_id,
            (int)phy_pdcch_config->pdcch_config[ss_idx].BWPStart,
            (int)phy_pdcch_config->pdcch_config[ss_idx].BWPSize, (int)coreset->duration,
            (int)coreset->RegBundleSize, (int)coreset->InterleaverSize, (int)coreset->ShiftIndex,
            (int)phy_pdcch_config->pdcch_config[ss_idx].number_of_candidates);
  }

  // generate pilot
  c16_t pilot[(n_rb + rb_offset + dmrs_ref) * 3] __attribute__((aligned(16)));
  // Note: pilot returned by the following function is already the complex conjugate of the transmitted DMRS
  const uint32_t *gold = nr_gold_pdcch(fp->N_RB_DL, fp->symbols_per_slot, scrambling_id, proc->nr_slot_rx, symbol);
  nr_pdcch_dmrs_ref(gold, pilot, n_rb + rb_offset + dmrs_ref);

  /* ------------------------------------------------------------------------------------------
   * TEMPORARY STAGE-SPLIT PROBE (2026-08-04). Deterministic, not a parameter search.
   *
   * Correlates the RECEIVED PDCCH DM-RS against the LOCALLY REGENERATED sequence, per CCE:
   *     corr = sum(Y * conj(X)) / sqrt(sum|Y|^2 * sum|X|^2)
   * nr_pdcch_dmrs_ref() already returns conj(X), so Y*pil is Y*conj(X) directly.
   *
   * Indexing mirrors nr_pdcch_channel_estimation() exactly (same start subcarrier, DM-RS at RE
   * 1,5,9 of each RB, pilot advanced 3/RB) so the probe cannot disagree with the estimator about
   * where the pilots are. At duration 1 a REG is one RB and a CCE is 6 REGs, so CCE n covers
   * CORESET RBs [6n, 6n+6) under NON-interleaved mapping -- which the gNB log confirms for this
   * cell.
   *
   * Reading it:
   *   |corr| HIGH on some CCEs  -> DM-RS sequence, nid, slot/symbol, REG/CCE mapping and RE
   *                                extraction are all correct; any remaining failure is AFTER
   *                                equalisation (demap/descramble/rate-recover/polar/CRC).
   *   |corr| LOW on every CCE   -> the fault is at or before channel estimation.
   * The empty CCEs are the built-in control: they must sit at the noise level, and their spread
   * gives the significance bar for whatever the best CCE scores. ------------------------------ */
  if (nr_pdcch_blind_dmrs_probe) {
    /* ACCUMULATING version. The first cut fired on the first 6 CORESET symbols it saw and found
     * only noise -- but that proved nothing: this gNB sends ~1219 DCIs over ~900 s, i.e. roughly
     * 0.07 % of slots carry a grant, so 6 arbitrary slots are almost certainly all EMPTY. An empty
     * CORESET has no DM-RS to correlate against, so a noise-level result there is the expected
     * answer, not evidence about the receive chain.
     *
     * So instead: score EVERY CORESET symbol, keep running extremes, and report periodically. Over
     * a 100 s run this sees ~200k slots and therefore ~140 real grants. The population of empty
     * slots is its own control -- it fixes the noise distribution precisely, and any slot carrying
     * a real PDCCH must stand far outside it.
     *
     * Decision rule (unchanged): a real DM-RS gives |corr| ~ 0.8-0.95 over the 18 pilots of a CCE.
     * Pure noise gives sqrt(pi)/(2*sqrt(18)) = 0.209 per trial, and the max over many trials grows
     * only as sqrt(ln(trials)/18). If max_seen never leaves that envelope, the DM-RS is not being
     * recovered at all -> fault at/before channel estimation. If some symbols do reach ~0.8+, the
     * estimator input is fine and the fault is after equalisation. */
    static double s_max_corr = -1.0;
    static int    s_max_cce = -1, s_max_slot = -1;
    static long   s_symbols = 0, s_hits70 = 0, s_hits80 = 0;
    static double s_sum = 0.0, s_sum2 = 0.0;
    static long   s_n = 0;

    const int symb_sz = fp->ofdm_symbol_size;
    const unsigned short cs_sc =
        (fp->first_carrier_offset + (phy_pdcch_config->pdcch_config[ss_idx].BWPStart + rb_offset) * 12) % symb_sz;
    const int n_cce = n_rb / 6;
    double sym_best = -1.0;
    int    sym_best_cce = -1;

    for (int cce = 0; cce < n_cce; cce++) {
      double cr = 0.0, ci = 0.0, py = 0.0, px = 0.0;
      for (int rb = cce * 6; rb < (cce + 1) * 6; rb++) {
        for (int p = 0; p < 3; p++) {
          const int k = (cs_sc + rb * 12 + 1 + 4 * p) % symb_sz;
          const c16_t y = rxdataF[0][k];
          const c16_t x = pilot[(dmrs_ref + rb_offset + rb) * 3 + p];
          cr += (double)y.r * x.r - (double)y.i * x.i;
          ci += (double)y.r * x.i + (double)y.i * x.r;
          py += (double)y.r * y.r + (double)y.i * y.i;
          px += (double)x.r * x.r + (double)x.i * x.i;
        }
      }
      const double den = sqrt(py * px);
      const double a = (den > 0.0) ? sqrt(cr * cr + ci * ci) / den : 0.0;
      s_sum += a;
      s_sum2 += a * a;
      s_n++;
      if (a > 0.70) s_hits70++;
      if (a > 0.80) s_hits80++;
      if (a > sym_best) { sym_best = a; sym_best_cce = cce; }
    }
    if (sym_best > s_max_corr) {
      s_max_corr = sym_best;
      s_max_cce  = sym_best_cce;
      s_max_slot = proc->nr_slot_rx;
    }
    /* PER-RB (NON-COHERENT) DM-RS CORRELATION.
     *
     * The CCE-wide coherent metric above is not phase-ramp immune: it sums 18 pilots spanning 72
     * subcarriers, so ANY residual FFT-window timing error rotates the later pilots relative to the
     * earlier ones and the coherent sum collapses even when the DM-RS is present and perfectly
     * correct. OAI's own estimator does not have that weakness -- it forms per-pilot products and
     * interpolates -- so a low coherent score does NOT by itself prove the DM-RS is missing.
     *
     * Here each RB is correlated on its own 3 pilots (|corr| per RB), and the MAGNITUDES are
     * averaged over the 6 RBs of a CCE. That is immune to a phase ramp across the CCE. It also
     * reports the mean phase STEP between adjacent RBs of the best CCE: a real timing offset shows
     * up as a consistent per-RB rotation, and tau ~ (dphi/2pi) * (N_fft / 12) samples.
     *
     * Noise reference: |corr| over 3 complex pilots has mean sqrt(pi)/(2*sqrt(3)) = 0.512, so the
     * non-coherent average over 6 RBs sits near 0.51 for empty CORESETs. A real DM-RS should push
     * the per-CCE average well above that regardless of timing. */
    cs_sc_cap = cs_sc;
    {
      static double s_nc_max = 0.0;
      static long   s_nc_hits = 0, s_nc_n = 0;
      static double s_nc_sum = 0.0;
      static long   s_nc_cnt = 0;
      static double s_best_dphi = 0.0;
      double sym_best_nc = 0.0;
      double sym_best_dphi = 0.0;
      int    sym_best_cce_nc = -1;
      for (int cce = 0; cce < n_cce; cce++) {
        double mag_sum = 0.0;
        double ph[6];
        for (int j = 0; j < 6; j++) {
          const int rb = cce * 6 + j;
          double cr = 0.0, ci = 0.0, py = 0.0, px = 0.0;
          for (int p = 0; p < 3; p++) {
            const int k = (cs_sc + rb * 12 + 1 + 4 * p) % symb_sz;
            const c16_t y = rxdataF[0][k];
            const c16_t x = pilot[(dmrs_ref + rb_offset + rb) * 3 + p];
            cr += (double)y.r * x.r - (double)y.i * x.i;
            ci += (double)y.r * x.i + (double)y.i * x.r;
            py += (double)y.r * y.r + (double)y.i * y.i;
            px += (double)x.r * x.r + (double)x.i * x.i;
          }
          const double d = sqrt(py * px);
          mag_sum += (d > 0.0) ? sqrt(cr * cr + ci * ci) / d : 0.0;
          ph[j] = atan2(ci, cr);
        }
        const double nc = mag_sum / 6.0;
        s_nc_sum += nc;
        s_nc_cnt++;
        if (nc > 0.75) s_nc_hits++;
        if (nc > sym_best_nc) {
          sym_best_nc = nc;
          sym_best_cce_nc = cce;
          double dsum = 0.0;
          for (int j = 1; j < 6; j++) {
            double d = ph[j] - ph[j - 1];
            while (d > M_PI) d -= 2 * M_PI;
            while (d < -M_PI) d += 2 * M_PI;
            dsum += d;
          }
          sym_best_dphi = dsum / 5.0;
        }
      }
      if (sym_best_nc > s_nc_max) { s_nc_max = sym_best_nc; s_best_dphi = sym_best_dphi; }
      g_dmrs_hot_cce = (sym_best_nc > 0.85) ? sym_best_cce_nc : -1; /* a genuinely occupied CCE */
      g_dmrs_hot_nc  = sym_best_nc;
      s_nc_n++;
      if ((s_nc_n % 20000) == 0) {
        const double tau = (s_best_dphi / (2.0 * M_PI)) * ((double)symb_sz / 12.0);
        LOG_W(PHY,
              "SENSING: PERRB n=%ld cce_trials=%ld | pop mean=%.3f (noise ref 0.512) | MAX nc=%.3f "
              "n>0.75=%ld | best-CCE per-RB phase step=%+.3f rad => timing ~%+.1f samples "
              "(if MAX ~0.5 the DM-RS truly is absent; if MAX >0.8 it is present and the coherent "
              "metric was being killed by the ramp)\n",
              s_nc_n, s_nc_cnt, s_nc_sum / (double)s_nc_cnt, s_nc_max, s_nc_hits, s_best_dphi, tau);
      }
    }

    s_symbols++;
    if ((s_symbols % 20000) == 0) {
      const double mean_a = s_sum / (double)s_n;
      const double sd_a = sqrt((s_sum2 / (double)s_n) - mean_a * mean_a);
      LOG_W(PHY,
            "SENSING: DMRSSTAT symbols=%ld cce_trials=%ld | noise pop mean=%.3f sd=%.3f | "
            "MAX |corr|=%.3f (slot=%d cce=%d) | n>0.70=%ld n>0.80=%ld "
            "(real DM-RS ~0.8-0.95; if MAX stays ~0.6 the DM-RS is never recovered)\n",
            s_symbols, s_n, mean_a, sd_a, s_max_corr, s_max_slot, s_max_cce, s_hits70, s_hits80);
    }
  }

  nr_pdcch_channel_estimation(ue,
                              n_rb,
                              rb_offset,
                              dmrs_ref,
                              fp->first_carrier_offset,
                              phy_pdcch_config->pdcch_config[ss_idx].BWPStart,
                              pdcch_est_size,
                              pdcch_dl_ch_estimates,
                              rxdataF,
                              pilot);

  // TIME-TRACKING AUDIT (2026-08-04, instrumentation only -- no behavioural change, nothing below
  // this block reads these values). Reports, per PDCCH occasion, the DM-RS phase slope across the
  // CORESET and the resulting implied receive delay, plus a coherence figure.
  //
  //   z_k       = Y_k * pilot_k          (pilot is already conj(X), so z is the raw channel)
  //   coherence = |sum z_k conj(z_k+1)| / sum |z_k|^2
  //
  // Coherence is the load-bearing number: adjacent DM-RS REs are 4 subcarriers apart, so a smooth
  // channel gives ~1 and an incoherent one ~1/sqrt(n). It is delay- and per-RB-phase tolerant, unlike
  // a plain coherent sum. tau is reported too but is only meaningful WHEN coherence is high -- a phase
  // slope fitted to incoherent pilots is noise, and is ambiguous beyond +-ofdm_symbol_size/8.
  {
    static int audit = -1;
    if (audit < 0) {
      /* SAME DECIMATION FACTOR as the per-SSB trace in nr_adjust_synch_ue.c, and for a much
       * sharper reason: this probe fires per PDCCH CANDIDATE, not per SSB. Left undecimated while
       * the SSB trace was decimated, it alone produced 40 % of a 683 line/s (90 kB/s) log written
       * from the PHY receive thread, and 14837 lines in a single short run. That is the "overflow
       * and block" a live operator sees, and it is also the observer effect that made both traced
       * runs come out 100 % PBCH-dead while untraced runs held 164 healthy windows.
       *
       * "1" logs every candidate (only ever do this on a short, deliberate capture); "25" every
       * 25th. Unset is off. */
      const char *e_ = getenv("ISAC_TSYNC_AUDIT");
      audit = (e_ != NULL) ? atoi(e_) : 0;
      if (e_ != NULL && audit < 1) {
        audit = 1;
      }
    }
    static unsigned audit_n_ = 0;
    const int audit_now_ = audit && ((audit_n_++ % (unsigned)audit) == 0);
    if (audit_now_) {
      const int symb_sz = fp->ofdm_symbol_size;
      const unsigned short cs_sc =
          (fp->first_carrier_offset + (phy_pdcch_config->pdcch_config[ss_idx].BWPStart + rb_offset) * 12) % symb_sz;
      const c16_t *pil = &pilot[(dmrs_ref + rb_offset) * 3];
      const int npil = 3 * n_rb;
      double lr = 0.0, li = 0.0, e = 0.0;
      double pr = 0.0, pi = 0.0;
      for (int n = 0; n < npil; n++) {
        const int k = (cs_sc + 4 * n + 1) % symb_sz;
        const c16_t y = rxdataF[0][k];
        const c16_t x = pil[n];
        const double zr = (double)y.r * x.r - (double)y.i * x.i;
        const double zi = (double)y.r * x.i + (double)y.i * x.r;
        if (n > 0) { // accumulate z_{n-1} * conj(z_n)
          lr += pr * zr + pi * zi;
          li += pi * zr - pr * zi;
        }
        pr = zr; pi = zi;
        e += zr * zr + zi * zi;
      }
      const double coh = (e > 0.0) ? sqrt(lr * lr + li * li) / e : 0.0;
      const double dphi = -atan2(li, lr);
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      LOG_I(PHY,
            "TSYNC_PDCCH utc_ns=%lld frame=%d slot=%d symbol=%d ss=%d n_rb=%d coherence=%.4f "
            "tau_samples=%.1f tau_ambig=%d\n",
            (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec,
            proc->frame_rx, proc->nr_slot_rx, symbol, ss_idx, n_rb, coh,
            dphi * symb_sz / (2.0 * M_PI * 4.0), symb_sz / 8);
    }
  }

  const int32_t rx_size = ceil_mod(fp->N_RB_DL * 12, 32);
  __attribute__((aligned(32))) c16_t rxdataF_ext[fp->nb_antennas_rx][rx_size];
  __attribute__((aligned(32))) c16_t pdcch_dl_ch_estimates_ext[fp->nb_antennas_rx][rx_size];

  nr_pdcch_extract_rbs_single(fp->ofdm_symbol_size,
                              rxdataF,
                              pdcch_est_size,
                              pdcch_dl_ch_estimates,
                              rx_size,
                              rxdataF_ext,
                              pdcch_dl_ch_estimates_ext,
                              fp,
                              coreset->frequency_domain_resource,
                              rb_offset,
                              n_rb,
                              phy_pdcch_config->pdcch_config[ss_idx].BWPStart);

  LOG_D(NR_PHY_DCI, "in channel level function (dl_ch_estimates_ext -> dl_ch_estimates_ext)\n");
  int avg[fp->nb_antennas_rx];
  nr_channel_level(0, rx_size, pdcch_dl_ch_estimates_ext, fp->nb_antennas_rx, 1, avg, n_rb * RE_PER_RB_OUT_DMRS);
  int avgs = avg[0];
  // All branches are MRC-combined (see nr_pdcch_detection_mrc()), so the shift is the max over them.
  for (int i = 1; i < fp->nb_antennas_rx; i++)
    avgs = cmax(avgs, avg[i]); /* every branch is combined now, at 4 RX too */
  const int log2_maxh = (log2_approx(avgs) / 2) + 5; //+frame_parms->nb_antennas_rx;
  int rx_comp_sz = ceil_mod(llr_size_symbol, 4);
  __attribute__((aligned(32))) c16_t rxdataF_comp[fp->nb_antennas_rx][rx_comp_sz];
  memset(rxdataF_comp, 0, sizeof(rxdataF_comp));
  nr_pdcch_channel_compensation(rx_comp_sz,
                                rx_size,
                                rxdataF_ext,
                                pdcch_dl_ch_estimates_ext,
                                rxdataF_comp,
                                fp->nb_antennas_rx,
                                log2_maxh); // log2_maxh+I0_shift

  if (fp->nb_antennas_rx > 1) {
    nr_pdcch_detection_mrc(fp->nb_antennas_rx, rx_comp_sz, rxdataF_comp);
  }
  /* TEMPORARY DIAGNOSTIC (2026-08-04, blind-PDCCH bring-up): the PRE-CLIP equalised constellation.
   * nr_pdcch_llr() below deliberately clips to [-32,31] (6-bit soft values for the polar decoder),
   * so anything sampled AFTER it cannot answer "is the channel estimate good?" -- an earlier
   * attempt measured post-clip values and was misread as a saturation bug. Here rxdataF_comp holds
   * the equalised symbols, MRC-combined, before any clipping: on a good estimate a PDCCH candidate
   * is QPSK, so |I| ~= |Q| and the magnitude spread (cv) is small. Circularly-symmetric noise gives
   * cv ~= 0.52. Reports the symbol-level view plus how far the values sit from the clip rail, so a
   * genuinely over-driven front end stays distinguishable from a bad estimate.
   * Gated to a handful of shots and to the strongest symbols only; costs nothing once exhausted. */
  {
    static int s_preclip_left = 24;
    if (s_preclip_left > 0) {
      const c16_t *cp = rxdataF_comp[0];
      double sum_i = 0.0, sum_q = 0.0, sum_m = 0.0, sum_m2 = 0.0, peak = 0.0;
      int n_clip = 0;
      for (int i = 0; i < llr_size_symbol; i++) {
        const double vi = (double)cp[i].r, vq = (double)cp[i].i;
        const double m = sqrt(vi * vi + vq * vq);
        sum_i += (vi < 0 ? -vi : vi);
        sum_q += (vq < 0 ? -vq : vq);
        sum_m += m;
        sum_m2 += m * m;
        if (m > peak) peak = m;
        if (vi > 31.0 || vi < -32.0 || vq > 31.0 || vq < -32.0) n_clip++;
      }
      const double n = (double)llr_size_symbol;
      const double mean_m = sum_m / n;
      const double var_m = (sum_m2 / n) - (mean_m * mean_m);
      const double cv = (mean_m > 0.0) ? sqrt(var_m > 0.0 ? var_m : 0.0) / mean_m : -1.0;
      const double iq = (sum_q > 0.0) ? (sum_i / sum_q) : -1.0;
      /* Only report symbols with real energy -- an empty CORESET symbol says nothing. */
      if (mean_m > 4.0) {
        LOG_W(PHY,
              "SENSING: PRECLIP slot=%d symb=%d n_rb=%d n=%d mean_mag=%.1f peak=%.0f cv=%.3f iq=%.3f "
              "would_clip=%d/%d (QPSK+good-est: cv<<0.5 ; circular noise: cv~0.52) "
              "s=(%d,%d),(%d,%d),(%d,%d),(%d,%d)\n",
              proc->nr_slot_rx, symbol, n_rb, llr_size_symbol, mean_m, peak, cv, iq,
              n_clip, llr_size_symbol,
              (int)cp[0].r, (int)cp[0].i, (int)cp[1].r, (int)cp[1].i,
              (int)cp[2].r, (int)cp[2].i, (int)cp[3].r, (int)cp[3].i);
        s_preclip_left--;
      }
    }
  }
  UEscopeCopy(ue, pdcchRxdataF_comp, rxdataF_comp[0], sizeof(c16_t), 1, llr_size_symbol, 0);
  /* ------------------------------------------------------------------------------------------
   * REPLAY-FIXTURE CAPTURE. Writes every PDCCH stage for one CORESET symbol to a file so the rest
   * of the debugging is deterministic offline replay instead of repeated OTA runs (where the RF
   * conditions, the C-RNTI and even the gNB config move between hypotheses).
   *
   * Triggered on a CCE whose per-RB DM-RS correlation is high, i.e. a symbol that really does carry
   * PDCCH -- dumping arbitrary slots is useless here because only ~0.07 % of slots carry a grant.
   * frame/slot are recorded so the fixture can be cross-referenced against the gNB log's own
   * "[frame.slot] PDCCH: rnti=... cce=... al=..." lines and replayed against a KNOWN grant.
   *
   * Record layout (all little-endian, c16_t = 2x int16):
   *   magic 'PDCH', ver, frame, slot, symbol, n_rb, rb_offset, dmrs_ref, cs_sc, symb_sz,
   *   llr_size_symbol, pdcch_est_size, nb_rx, hot_cce, hot_nc(float), log2_maxh
   *   then: rxdataF[symb_sz]            (raw received symbol, antenna 0)
   *         pilot[(n_rb+rb_offset+dmrs_ref)*3]  (regenerated conj DM-RS)
   *         ch_est[pdcch_est_size]      (antenna 0)
   *         rxdataF_comp[llr_size_symbol]       (equalised, PRE-clip)
   * ------------------------------------------------------------------------------------------ */
  /* Trigger UNCONDITIONALLY on consecutive CORESET symbols. The first version triggered on
   * DM-RS corr > 0.88, but the noise tail of that statistic reaches 0.954, so it selected noise
   * peaks with no data energy -- the resulting fixture contained no verifiable grant. This cell
   * puts a DCI in ~6 %% of slots, so a few hundred consecutive symbols is certain to contain many. */
  /* Trigger on real CORESET OCCUPANCY, self-normalised per symbol. Blind consecutive capture was
   * wrong: measured against the gNB log, this cell runs at only ~2 grants/s, so a 0.24 s window of
   * consecutive slots contained ZERO grants and the fixture had nothing to decode. DM-RS
   * correlation is no good as a trigger either (its noise tail reaches 0.95). Occupancy is: some
   * CCE whose raw data-RE energy stands well above the median CCE of the SAME symbol -- scale-free,
   * so it needs no absolute threshold and cannot drift with gain. */
  int cap_occupied = 0;
  if (nr_pdcch_blind_capture && g_cap_left > 0) {
    const int ncce_c = n_rb / 6;
    if (ncce_c > 4) {
      double e[64];
      int ne = ncce_c > 64 ? 64 : ncce_c;
      for (int c = 0; c < ne; c++) {
        double t = 0.0; int cnt = 0;
        for (int j = 0; j < 6; j++) {
          const int rb = c * 6 + j;
          for (int sc = 0; sc < 12; sc++) {
            if ((sc & 3) == 1) continue;            /* 1,5,9 are DM-RS */
            const int k = (cs_sc_cap + rb * 12 + sc) % fp->ofdm_symbol_size;
            const c16_t y = rxdataF[0][k];
            t += fabs((double)y.r) + fabs((double)y.i);
            cnt++;
          }
        }
        e[c] = t / cnt;
      }
      double srt[64];
      memcpy(srt, e, sizeof(double) * ne);
      for (int a = 1; a < ne; a++) { double v = srt[a]; int b = a - 1; while (b >= 0 && srt[b] > v) { srt[b+1] = srt[b]; b--; } srt[b+1] = v; }
      const double med_e = srt[ne / 2];
      for (int c = 0; c < ne; c++) {
        if (med_e > 0.0 && e[c] > 6.0 * med_e) { cap_occupied = 1; break; }
      }
    }
  }
  if (nr_pdcch_blind_capture && g_cap_left > 0 && cap_occupied) {
    if (g_cap_fp == NULL) {
      g_cap_fp = fopen("/tmp/pdcch_fixture.bin", "wb");
    }
    if (g_cap_fp != NULL) {
      const int32_t hdr[14] = {0x48434450, 1, (int32_t)proc->frame_rx, (int32_t)proc->nr_slot_rx,
                               (int32_t)symbol, n_rb, rb_offset, dmrs_ref, (int32_t)cs_sc_cap,
                               (int32_t)fp->ofdm_symbol_size, llr_size_symbol, pdcch_est_size,
                               (int32_t)fp->nb_antennas_rx, g_dmrs_hot_cce};
      struct timespec cap_ts;
      clock_gettime(CLOCK_REALTIME, &cap_ts);
      const int64_t cap_utc_ns = (int64_t)cap_ts.tv_sec * 1000000000LL + cap_ts.tv_nsec;
      const float nc_f = (float)g_dmrs_hot_nc;
      const int32_t l2m = log2_maxh;
      fwrite(hdr, sizeof(hdr), 1, g_cap_fp);
      fwrite(&nc_f, sizeof(nc_f), 1, g_cap_fp);
      fwrite(&l2m, sizeof(l2m), 1, g_cap_fp);
      fwrite(&cap_utc_ns, sizeof(cap_utc_ns), 1, g_cap_fp);
      fwrite(rxdataF[0], sizeof(c16_t), fp->ofdm_symbol_size, g_cap_fp);
      fwrite(pilot, sizeof(c16_t), (n_rb + rb_offset + dmrs_ref) * 3, g_cap_fp);
      fwrite(pdcch_dl_ch_estimates[0], sizeof(c16_t), pdcch_est_size, g_cap_fp);
      fwrite(rxdataF_comp[0], sizeof(c16_t), llr_size_symbol, g_cap_fp);
      fflush(g_cap_fp);
      g_cap_left--;
      LOG_W(PHY, "SENSING: CAPTURE wrote record (frame=%d slot=%d cce=%d nc=%.3f) %d left\n",
            proc->frame_rx, proc->nr_slot_rx, g_dmrs_hot_cce, g_dmrs_hot_nc, g_cap_left);
    }
  }

  /* POST-EQUALISATION DATA-RE CHECK, triggered by a CCE whose DM-RS correlated at >0.85 -- i.e. a
   * PDCCH we KNOW is really there. The per-RB DM-RS result proves the DM-RS REs (subcarriers 1,5,9
   * of each RB) are extracted and sequenced correctly; it says nothing about the other 9 REs per RB
   * that actually carry the DCI. This looks at those, after channel compensation, for the very same
   * CCE: on a correct chain they must be QPSK, so the magnitude spread cv is small and |I|~|Q|.
   * Non-coherent-noise-like values here would place the fault in data-RE extraction / compensation
   * rather than in demapping/unscrambling/polar further downstream.
   * Under non-interleaved mapping with duration 1, CCE c covers CORESET RBs [6c,6c+6), i.e.
   * rxdataF_comp REs [54c, 54c+54). */
  if (nr_pdcch_blind_dmrs_probe && g_dmrs_hot_cce >= 0) {
    static int s_dq_left = 25;
    const int base = g_dmrs_hot_cce * 54;
    if (s_dq_left > 0 && base + 54 <= llr_size_symbol) {
      const c16_t *dq = &rxdataF_comp[0][base];
      double si = 0.0, sq = 0.0, sm = 0.0, sm2 = 0.0;
      for (int i = 0; i < 54; i++) {
        const double vi = (double)dq[i].r, vq = (double)dq[i].i;
        const double m = sqrt(vi * vi + vq * vq);
        si += (vi < 0 ? -vi : vi);
        sq += (vq < 0 ? -vq : vq);
        sm += m;
        sm2 += m * m;
      }
      const double mean_m = sm / 54.0;
      const double var_m = (sm2 / 54.0) - mean_m * mean_m;
      const double cv = (mean_m > 0.0) ? sqrt(var_m > 0.0 ? var_m : 0.0) / mean_m : -1.0;
      LOG_W(PHY,
            "SENSING: DATAQ slot=%d cce=%d dmrs_nc=%.3f | data REs: mean_mag=%.1f cv=%.3f iq=%.3f "
            "(QPSK on a correct chain: cv<<0.5 ; noise: cv~0.52) s=(%d,%d),(%d,%d),(%d,%d)\n",
            proc->nr_slot_rx, g_dmrs_hot_cce, g_dmrs_hot_nc, mean_m, cv,
            (sq > 0.0) ? si / sq : -1.0,
            (int)dq[0].r, (int)dq[0].i, (int)dq[1].r, (int)dq[1].i, (int)dq[2].r, (int)dq[2].i);
      s_dq_left--;
    }
  }

  if (nr_pdcch_blind_llr_autoscale) {
    /* Bring the strongest REs just inside nr_pdcch_llr()'s clip rail. Peak-referenced (not mean-),
     * because the mean here is the empty-CORESET noise floor -- the very thing that mis-scales the
     * stock path. A pure right shift keeps I and Q in the same ratio, so the constellation is
     * scaled rather than rotated. Peaks below the rail are left completely alone. */
    /* PERCENTILE, BOTH DIRECTIONS (2026-09-15). Measured on the X410 at 4 RX (PRECLIP): the run that
     * decoded SIB1 had mean_mag 14-22 with a peak of 66-101; the runs that never did had mean_mag
     * 4.6-5.6 (LLRs of 0/+-1) or mean 116 / peak 402 (half the REs clipped). One outlier RE set the
     * old peak-only right shift, and it never scaled UP. The 90th-percentile magnitude is put at
     * half the +/-31 rail: a shift either way, the top decile clips (nr_pdcch_llr() clips anyway). */
    uint16_t hist[256] = {0};
    int n = 0;
    for (int i = 0; i < llr_size_symbol; i++) {
      const int ar = rxdataF_comp[0][i].r < 0 ? -rxdataF_comp[0][i].r : rxdataF_comp[0][i].r;
      const int ai = rxdataF_comp[0][i].i < 0 ? -rxdataF_comp[0][i].i : rxdataF_comp[0][i].i;
      const int m = ar > ai ? ar : ai;
      hist[m > 255 ? 255 : m]++; /* magnitudes >= 255 share the top bin: they need a right shift anyway */
      n++;
    }
    int p90 = 0, acc = 0;
    for (int b = 0; b < 256; b++) {
      acc += hist[b];
      if (acc * 10 >= n * 9) { p90 = b; break; }
    }
    int sh = 0; /* >0 right shift, <0 left shift */
    if (p90 >= 255) {
      int peak = 0;
      for (int i = 0; i < llr_size_symbol; i++) {
        const int ar = rxdataF_comp[0][i].r < 0 ? -rxdataF_comp[0][i].r : rxdataF_comp[0][i].r;
        const int ai = rxdataF_comp[0][i].i < 0 ? -rxdataF_comp[0][i].i : rxdataF_comp[0][i].i;
        if (ar > peak) peak = ar;
        if (ai > peak) peak = ai;
      }
      while (peak > 31 && sh < 15) { peak >>= 1; sh++; }
    } else if (p90 > 24) {
      while ((p90 >> sh) > 24 && sh < 8) sh++;
    } else if (p90 > 0) {
      while ((p90 << (-sh + 1)) <= 24 && sh > -6) sh--;
    }
    if (sh != 0) {
      for (int i = 0; i < llr_size_symbol; i++) {
        int r = rxdataF_comp[0][i].r, q = rxdataF_comp[0][i].i;
        if (sh > 0) { r >>= sh; q >>= sh; } else { r <<= -sh; q <<= -sh; }
        rxdataF_comp[0][i].r = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        rxdataF_comp[0][i].i = (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
      }
      static int s_shift_log_left = 8;
      if (s_shift_log_left > 0) {
        LOG_W(PHY, "SENSING: PDCCH autoscale slot=%d symb=%d extra_shift=%d (p90 %d -> %d)\n",
              proc->nr_slot_rx, symbol, sh, p90, sh > 0 ? p90 >> sh : p90 << -sh);
        s_shift_log_left--;
      }
    }
  }
  nr_pdcch_llr(llr_size_symbol, rxdataF_comp[0], llr);
}

static bool is_start_symbol_in_ss(const fapi_nr_dl_config_dci_dl_pdu_rel15_t *ss, const int symbol, const int nb_symb_slot)
{
  return ((ss->coreset.StartSymbolBitmap >> (nb_symb_slot - 1 - symbol)) & 1);
}

static int get_pdcch_mon_occasions_slot(const fapi_nr_dl_config_dci_dl_pdu_rel15_t *ss,
                                        int nb_symb_slot,
                                        uint8_t start_symb[nb_symb_slot])
{
  int sum = 0;
  for (int s = 0; s < nb_symb_slot; s++) {
    if (is_start_symbol_in_ss(ss, s, nb_symb_slot)) {
      if (start_symb != NULL)
        start_symb[sum] = s;
      sum++;
    }
  }

  return sum;
}

int get_max_pdcch_monOcc(const NR_UE_PDCCH_CONFIG *phy_pdcch_config, int nb_symb_slot)
{
  int monOcc = 0;
  for (int ss = 0; ss < phy_pdcch_config->nb_search_space; ss++) {
    monOcc = max(monOcc, get_pdcch_mon_occasions_slot(&phy_pdcch_config->pdcch_config[ss], nb_symb_slot, NULL));
  }
  return monOcc;
}

void set_first_last_pdcch_symb(const NR_UE_PDCCH_CONFIG *phy_pdcch_config, int nb_symb_slot, int *first_symb, int *last_symb)
{
  *first_symb = nb_symb_slot; // max first pdcch symbol
  *last_symb = 0; // min last pdcch symbol
  for (int ss = 0; ss < phy_pdcch_config->nb_search_space; ss++) {
    for (int symb = 0; symb < nb_symb_slot; symb++) {
      if (is_start_symbol_in_ss(&phy_pdcch_config->pdcch_config[ss], symb, nb_symb_slot)) {
        const int duration = phy_pdcch_config->pdcch_config[ss].coreset.duration;
        *first_symb = min(*first_symb, symb);
        *last_symb = max(*last_symb, symb + duration - 1);
      }
    }
  }
}

/* Generates PDCCH LLRs from received symbol for each Search-Space */
void nr_pdcch_generate_llr(PHY_VARS_NR_UE *ue,
                           const UE_nr_rxtx_proc_t *proc,
                           int symbol,
                           nr_phy_data_t *phy_data,
                           int llr_size_symbol,
                           int num_monitoring_occ,
                           int max_symb,
                           c16_t rxdataF[ue->frame_parms.nb_antennas_rx][ue->frame_parms.ofdm_symbol_size],
                           c16_t pdcch_llr[phy_data->phy_pdcch_config.nb_search_space][num_monitoring_occ][max_symb * llr_size_symbol])
{
  const NR_UE_PDCCH_CONFIG *phy_pdcch_config = &phy_data->phy_pdcch_config;

  // Loop over search spaces
  for (int ss_idx = 0; ss_idx < phy_pdcch_config->nb_search_space; ss_idx++) {
    uint8_t start_symb[NR_SYMBOLS_PER_SLOT] = {0};
    const int num_monOcc = get_pdcch_mon_occasions_slot(&phy_pdcch_config->pdcch_config[ss_idx],
                                                        ue->frame_parms.symbols_per_slot,
                                                        start_symb);
    // Loop over monitoring occations within the slot in this ss
    for (int occ = 0; occ < num_monOcc; occ++) {
      const int first_symb = start_symb[occ];
      const int last_symb = first_symb + phy_pdcch_config->pdcch_config[ss_idx].coreset.duration;
      // Decode PDCCH and generate LLR for each ss in this OFDM symbol
      if ((symbol >= first_symb) && (symbol < last_symb)) {
        const int rel_symb_monOcc = symbol - first_symb;
        nr_rx_pdcch_symbol(ue,
                           proc,
                           symbol,
                           ss_idx,
                           phy_data,
                           llr_size_symbol,
                           rxdataF,
                           &pdcch_llr[ss_idx][occ][rel_symb_monOcc * llr_size_symbol]);
      }
    }
  }
}

// Exported (was static) -- see nr_pdcch_demapping_deinterleaving's comment above; same reasoning.
void nr_pdcch_unscrambling(c16_t *e_rx,
                                  uint16_t scrambling_RNTI,
                                  uint32_t length,
                                  uint16_t pdcch_DMRS_scrambling_id,
                                  int16_t *z2)
{
  uint32_t rnti = (uint32_t) scrambling_RNTI;
  uint16_t n_id = pdcch_DMRS_scrambling_id;
  uint32_t *seq = gold_cache(((rnti << 16) + n_id) % (1U << 31), length / 32); // this is c_init in 38.211 v15.1.0 Section 7.3.2.3
  LOG_D(NR_PHY_DCI, "PDCCH Unscrambling: scrambling_RNTI %x\n", rnti);
  int16_t *ptr = &e_rx[0].r;
  for (int i = 0; i < length; i++) {
    if (seq[i / 32] & (1UL << (i % 32)))
      z2[i] = -ptr[i];
    else
      z2[i] = ptr[i];
  }
}


/* LLRPROBE (ISAC_PDCCH_LLRPROBE=1): compact signature of the unscrambled soft bits a PDCCH
 * candidate feeds to polar_decoder_int16(), plus the CRC that decoder recovered.
 *
 * Why this exists: the normal path and the blind monitor call polar_decoder_int16() with IDENTICAL
 * arguments and advance their candidate index identically, so if their `tmp_e` agree for the same
 * (frame, slot, CCE, L) they MUST recover the same CRC. Printing the signature from both therefore
 * localises the defect to LLR PRODUCTION vs anything downstream, with no ground truth needed.
 * Read-only, off by default. */
void nr_pdcch_llr_probe(const char *path, int frame, int slot, int cce, int L, uint32_t crc, const int16_t *e, int n)
{
  static int s_on = -1;
  if (s_on < 0)
    s_on = (getenv("ISAC_PDCCH_LLRPROBE") != NULL) ? 1 : 0;
  if (!s_on)
    return;
  /* Slot filter: full-rate probing floods the log and has been MEASURED to break the attach that
   * provides the control arm. SIB1 sits in one slot index, so filtering to it costs nothing. */
  static int s_slot = -2;
  if (s_slot == -2) {
    const char *e2 = getenv("ISAC_PDCCH_LLRPROBE_SLOT");
    s_slot = e2 ? atoi(e2) : -1;
  }
  if (s_slot >= 0 && slot != s_slot)
    return;
  long sum = 0;
  int nz = 0;
  for (int i = 0; i < n; i++) {
    sum += e[i] < 0 ? -e[i] : e[i];
    if (e[i] != 0)
      nz++;
  }
  LOG_I(PHY, "LLRPROBE path=%s f=%d s=%d cce=%d L=%d crc=0x%x n=%d sum=%ld nz=%d e=%d,%d,%d,%d,%d,%d,%d,%d\n",
        path, frame, slot, cce, L, crc, n, sum, nz,
        e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
}

static void nr_dci_decoding_procedure(const UE_nr_rxtx_proc_t *proc,
                                      c16_t *pdcch_e_rx,
                                      fapi_nr_dl_config_dci_dl_pdu_rel15_t *rel15,
                                      fapi_nr_dci_indication_t *dci_ind)
{
  int e_rx_cand_idx = 0;
  // if DCI for SIB we don't break after finding 1st DCI with that RNTI
  // there might be SIB1 and otherSIB in the same slot with the same length
  bool is_SI = rel15->rnti == SI_RNTI;

  for (int j = 0; j < rel15->number_of_candidates; j++) {
    int CCEind = rel15->CCE[j];
    int L = rel15->L[j];

    // Loop over possible DCI lengths
    
    for (int k = 0; k < rel15->num_dci_options; k++) {
      // skip this candidate if we've already found one with the
      // same rnti and size at a different aggregation level
      int dci_length = rel15->dci_length_options[k];
      int ind;
      for (ind = 0; ind < dci_ind->number_of_dcis; ind++) {
        if (!is_SI && rel15->rnti == dci_ind->dci_list[ind].rnti && dci_length == dci_ind->dci_list[ind].payloadSize) {
          break;
        }
      }
      if (ind < dci_ind->number_of_dcis)
        continue;

      uint64_t dci_estimation[2] = {0};
      LOG_D(NR_PHY_DCI,
            "(%i.%i) Trying DCI candidate %d of %d number of candidates, CCE %d (%d), L %d, length %d, format %d\n",
            proc->frame_rx,
            proc->nr_slot_rx,
            j,
            rel15->number_of_candidates,
            CCEind,
            e_rx_cand_idx,
            L,
            dci_length,
            rel15->dci_format_options[k]);

      int16_t tmp_e[16 * 108];
      nr_pdcch_unscrambling(&pdcch_e_rx[e_rx_cand_idx],
                            rel15->coreset.scrambling_rnti,
                            L * 108,
                            rel15->coreset.pdcch_dmrs_scrambling_id,
                            tmp_e);

      const uint32_t crc = polar_decoder_int16(tmp_e, dci_estimation, 1, NR_POLAR_DCI_MESSAGE_TYPE, dci_length, L);
      nr_pdcch_llr_probe("normal", proc->frame_rx, proc->nr_slot_rx, CCEind, L, crc, tmp_e, L * 108);

      rnti_t n_rnti = rel15->rnti;
      /* SI-RNTI census: whether CORESET#0 candidates are being evaluated at all, and how often
       * they hit. Half of the 2026-09-15 X410 acquisitions never decoded SIB1 with PBCH at 50/50
       * and nothing said why. Every 500 SI candidates, plus the first hit. */
      if (is_SI) {
        static uint32_t s_si_try, s_si_hit;
        s_si_try++;
        if (crc == n_rnti)
          s_si_hit++;
        if ((s_si_try % 500) == 0 || (crc == n_rnti && s_si_hit == 1))
          LOG_W(NR_PHY_DCI, "SENSING: SICENSUS si_rnti candidates=%u hits=%u (last: %d.%d L=%d cce=%d len=%d crc=0x%x)\n",
                s_si_try, s_si_hit, proc->frame_rx, proc->nr_slot_rx, L, CCEind, dci_length, crc);
      }
      if (crc == n_rnti) {
        LOG_D(NR_PHY_DCI,
              "(%i.%i) Received dci indication (rnti %x,dci format %d,n_CCE %d,payloadSize %d,payload %llx)\n",
              proc->frame_rx,
              proc->nr_slot_rx,
              n_rnti,
              rel15->dci_format_options[k],
              CCEind,
              dci_length,
              *(unsigned long long *)dci_estimation);
        AssertFatal(dci_ind->number_of_dcis < sizeofArray(dci_ind->dci_list), "Fix allocation\n");
        fapi_nr_dci_indication_pdu_t *dci = dci_ind->dci_list + dci_ind->number_of_dcis;
        *dci = (fapi_nr_dci_indication_pdu_t){
            .rnti = n_rnti,
            .n_CCE = CCEind,
            .N_CCE = L,
            .dci_format = rel15->dci_format_options[k],
            .ss_type = rel15->ss_type_options[k],
            .coreset_type = rel15->coreset.CoreSetType,
        };
        int n_rb, cset_start;
        get_coreset_rballoc(rel15->coreset.frequency_domain_resource, &n_rb, &cset_start);
        dci->cset_start = rel15->BWPStart + cset_start + rel15->coreset.rb_offset;
        dci->payloadSize = dci_length;
        memcpy(dci->payloadBits, dci_estimation, (dci_length + 7) / 8);
        dci_ind->number_of_dcis++;
        break;    // If DCI is found, no need to check for remaining DCI lengths
      } else {
        LOG_D(NR_PHY_DCI,
              "(%i.%i) Decoded crc %x does not match rnti %x for DCI format %d\n",
              proc->frame_rx,
              proc->nr_slot_rx,
              crc,
              n_rnti,
              rel15->dci_format_options[k]);
      }
    }
    e_rx_cand_idx += RE_PER_RB_OUT_DMRS * L * 6; // e_rx index for next candidate (L CCEs, 6 REGs per CCE and 9 REs per REG )
  }
}

/* Decode DCI from LLRs for each Search-Space and send to MAC */
void nr_pdcch_dci_indication(const UE_nr_rxtx_proc_t *proc,
                             int llr_size,
                             int max_monOcc,
                             PHY_VARS_NR_UE *ue,
                             nr_phy_data_t *phy_data,
                             c16_t llr[phy_data->phy_pdcch_config.nb_search_space][max_monOcc][llr_size])
{
  NR_UE_PDCCH_CONFIG *phy_pdcch_config = &phy_data->phy_pdcch_config;

  fapi_nr_dci_indication_t dci_ind = {.SFN = proc->frame_rx, .slot = proc->nr_slot_rx};

  for (int ss_idx = 0; ss_idx < phy_pdcch_config->nb_search_space; ss_idx++) {
    fapi_nr_dl_config_dci_dl_pdu_rel15_t *rel15 = &phy_pdcch_config->pdcch_config[ss_idx];
    uint8_t unused_start_symb[NR_SYMBOLS_PER_SLOT] = {0};
    const int num_monitoring_occ = get_pdcch_mon_occasions_slot(rel15, ue->frame_parms.symbols_per_slot, unused_start_symb);
    const int llr_stride = llr_size / rel15->coreset.duration;
    int n_rb, cset_start;
    get_coreset_rballoc(rel15->coreset.frequency_domain_resource, &n_rb, &cset_start);

    for (int m = 0; m < num_monitoring_occ; m++) {
      /// PDCCH/DCI e-sequence (input to rate matching).
      c16_t pdcch_e_rx[NR_MAX_PDCCH_SIZE];


      nr_pdcch_demapping_deinterleaving(n_rb,
                                        llr[ss_idx][m],
                                        pdcch_e_rx,
                                        rel15->coreset.duration,
                                        rel15->coreset.RegBundleSize,
                                        rel15->coreset.InterleaverSize,
                                        rel15->coreset.ShiftIndex,
                                        rel15->number_of_candidates,
                                        rel15->CCE,
                                        rel15->L,
                                        llr_stride);

      nr_dci_decoding_procedure(proc, pdcch_e_rx, rel15, &dci_ind);
    }
  }

  for (int i = 0; i < dci_ind.number_of_dcis; i++) {
    LOG_D(PHY,
          "Frame.slot: %d.%d: DCI %i of %d total DCIs found --> rnti %x : format %d\n",
          proc->frame_rx,
          proc->nr_slot_rx,
          i + 1,
          dci_ind.number_of_dcis,
          dci_ind.dci_list[i].rnti,
          dci_ind.dci_list[i].dci_format);
  }

  /* Send to MAC */
  nr_downlink_indication_t dl_indication = (nr_downlink_indication_t){.gNB_index = proc->gNB_id,
                                                                      .module_id = ue->Mod_id,
                                                                      .cc_id = ue->CC_id,
                                                                      .hfn = proc->hfn_rx,
                                                                      .frame = proc->frame_rx,
                                                                      .slot = proc->nr_slot_rx,
                                                                      .phy_data = phy_data,
                                                                      .dci_ind = &dci_ind};
  ue->if_inst->dl_indication(&dl_indication);
  phy_pdcch_config->nb_search_space = 0;
}
