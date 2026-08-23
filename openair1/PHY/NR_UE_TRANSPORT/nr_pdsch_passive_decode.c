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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c
 * \brief See nr_pdsch_passive_decode.h for scope and for why this does not simply call
 * nr_dlsch_decoding().
 */

#include "nr_pdsch_passive_decode.h"

#include <stdlib.h>
#include <string.h>

#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"

#include "PHY/CODING/coding_defs.h"
#include "PHY/NR_REFSIG/dmrs_nr.h" // get_num_dmrs_re_per_rb, nr_chest_time_domain_avg
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/MODULATION/modulation_UE.h" // nr_slot_fep
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "PHY/TOOLS/tools_defs.h"
#include "executables/nr-uesoftmodem.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"

// Distinct from the attached path's 1000 + harq_pid (phy_procedures_nr_ue.c) and from
// nr_dlsch_decoding()'s 2*harq_pid + cw_idx, so a hardware LDPC accelerator's per-harq_unique_pid
// segment buffers can never be shared between a real HARQ process and an overheard one.
#define NR_PDSCH_PASSIVE_HARQ_TAG_BASE 2000

// ---------------------------------------------------------------------------------------------
// Private HARQ context. One per decoding thread: the RT tap runs from the UE's per-slot RX thread,
// and a thread never has two decodes in flight, so per-thread state needs no locking. Buffers are
// sized exactly as nr_init_dl_harq_processes() sizes the real ones (same segment-count formula), so
// a payload the real UE could hold fits here too.
// ---------------------------------------------------------------------------------------------
typedef struct {
  int      n_rb_dl;      // the N_RB_DL these buffers were sized for; a change forces a realloc
  uint32_t a_segments;
  uint8_t *b;
  uint8_t *c;
  int16_t *d;
  uint32_t processedSegments;
  int      llrLen;
  decode_abort_t abort_decode;
} passive_harq_t;

static __thread passive_harq_t g_harq; // zero-initialised per thread

static bool passive_harq_prepare(passive_harq_t *h, int n_rb_dl)
{
  if (h->b != NULL && h->n_rb_dl == n_rb_dl) {
    return true;
  }
  free(h->b);
  free(h->c);
  free(h->d);
  h->b = NULL;
  h->c = NULL;
  h->d = NULL;

  // Same formula as nr_init_dl_harq_processes(): the number of segments scales with bandwidth.
  uint32_t a_segments = MAX_NUM_NR_DLSCH_SEGMENTS;
  if (n_rb_dl != 273) {
    a_segments = (a_segments * (uint32_t)n_rb_dl) / 273 + 1;
  }

  h->b = (uint8_t *)malloc16_clear(a_segments * 1056);
  h->c = (uint8_t *)malloc16(a_segments * sizeof(*h->c) * 1056);
  h->d = (int16_t *)malloc16(a_segments * sizeof(*h->d) * 3 * 8448);
  if (h->b == NULL || h->c == NULL || h->d == NULL) {
    free(h->b);
    free(h->c);
    free(h->d);
    h->b = NULL;
    h->c = NULL;
    h->d = NULL;
    LOG_E(NR_PHY, "SENSING: passive PDSCH decode -- could not allocate the private HARQ buffers\n");
    return false;
  }
  init_abort(&h->abort_decode);
  h->a_segments = a_segments;
  h->n_rb_dl    = n_rb_dl;
  return true;
}

/// LDPC decode of one transport block against the private HARQ context. Mirrors
/// nr_dlsch_decoding()'s parameter assembly; differs ONLY in that every piece of per-process state
/// it reads or writes lives in `h` instead of ue->dl_harq_processes[][], and in that this is always
/// a first (and only) HARQ round -- a passive receiver has no soft buffer from an earlier grant it
/// never saw, so there is nothing to combine and `d_to_be_cleared` is unconditionally true.
static bool passive_ldpc_decode(PHY_VARS_NR_UE *ue,
                                const UE_nr_rxtx_proc_t *proc,
                                passive_harq_t *h,
                                const fapi_nr_dl_cw_info_t *cw,
                                const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                int16_t *llr,
                                int number_rbs,
                                uint32_t G)
{
  nrLDPC_TB_decoding_parameters_t TB_parameters = {0};
  nrLDPC_slot_decoding_parameters_t slot_parameters = {.frame = proc->frame_rx,
                                                       .slot = proc->nr_slot_rx,
                                                       .nb_TBs = 1,
                                                       .threadPool = &get_nrUE_params()->Tpool,
                                                       .TBs = &TB_parameters};

  h->processedSegments = 0;

  TB_parameters.harq_unique_pid = NR_PDSCH_PASSIVE_HARQ_TAG_BASE + dlsch_config->harq_process_nbr;
  TB_parameters.G = G;
  TB_parameters.nb_rb = number_rbs;
  TB_parameters.Qm = cw->qamModOrder;
  TB_parameters.mcs = cw->mcs;
  TB_parameters.nb_layers = cw->Nl;
  TB_parameters.BG = cw->ldpcBaseGraph;
  TB_parameters.A = cw->TBS;
  TB_parameters.processedSegments = &h->processedSegments;

  nr_segmentation(NULL,
                  NULL,
                  lenWithCrc(1, TB_parameters.A), // max size, in case of a single segment
                  &TB_parameters.C,
                  &TB_parameters.K,
                  &TB_parameters.Z,
                  &TB_parameters.F,
                  TB_parameters.BG);
  if (TB_parameters.C > h->a_segments) {
    LOG_W(NR_PHY, "SENSING: passive PDSCH decode -- too many segments C=%u (cap %u)\n", TB_parameters.C, h->a_segments);
    return false;
  }

  TB_parameters.max_ldpc_iterations = 8; // same default init_nr_ue_dlsch() gives the real DLSCH
  TB_parameters.rv_index = cw->rv;
  TB_parameters.tbslbrm = dlsch_config->tbslbrm;
  TB_parameters.abort_decode = &h->abort_decode;
  set_abort(&h->abort_decode, false);

  TB_parameters.llr = llr;
  TB_parameters.c = h->c;
  TB_parameters.d = h->d;
  TB_parameters.E = nr_get_E(TB_parameters.G, TB_parameters.C, TB_parameters.Qm, TB_parameters.nb_layers, 0);
  TB_parameters.E2 = TB_parameters.E;
  TB_parameters.first_rE2 = TB_parameters.C;
  TB_parameters.R = nr_get_R_ldpc_decoder(TB_parameters.rv_index, TB_parameters.E, TB_parameters.BG, TB_parameters.Z,
                                          &h->llrLen, 0 /* DLround: always the first, see above */);
  for (uint32_t r = 1; r < TB_parameters.C; r++) {
    const int Er = nr_get_E(TB_parameters.G, TB_parameters.C, TB_parameters.Qm, TB_parameters.nb_layers, r);
    if (Er != TB_parameters.E) {
      TB_parameters.E2 = Er;
      TB_parameters.R2 = nr_get_R_ldpc_decoder(TB_parameters.rv_index, Er, TB_parameters.BG, TB_parameters.Z,
                                               &h->llrLen, 0);
      TB_parameters.first_rE2 = r;
      break;
    }
  }
  /* SEGDIAG (ISAC_PDSCH_TBPARM=1): the full rate-matching / segmentation parameter set, on ONE
   * line, so an MCS that fails deterministically can be diffed against one that succeeds without
   * pairing log lines. Everything the LDPC decoder is handed is here. */
  {
    static int s_sd = -1;
    if (s_sd < 0)
      s_sd = (getenv("ISAC_PDSCH_TBPARM") != NULL) ? 1 : 0;
    if (s_sd)
      LOG_I(PHY,
            "SENSING: SEGDIAG mcs=%u BG=%u A=%u G=%u C=%u K=%u Z=%u F=%u E=%u E2=%u frE2=%u R=%u R2=%u "
            "Qm=%u nl=%u rv=%u tbslbrm=%u nb_rb=%d llrLen=%d\n",
            (unsigned)cw->mcs, (unsigned)TB_parameters.BG, (unsigned)TB_parameters.A, (unsigned)TB_parameters.G,
            (unsigned)TB_parameters.C, (unsigned)TB_parameters.K, (unsigned)TB_parameters.Z,
            (unsigned)TB_parameters.F, (unsigned)TB_parameters.E, (unsigned)TB_parameters.E2,
            (unsigned)TB_parameters.first_rE2, (unsigned)TB_parameters.R, (unsigned)TB_parameters.R2,
            (unsigned)TB_parameters.Qm, (unsigned)TB_parameters.nb_layers, (unsigned)TB_parameters.rv_index,
            (unsigned)TB_parameters.tbslbrm, TB_parameters.nb_rb, h->llrLen);
  }

  TB_parameters.d_to_be_cleared = true;
  for (uint32_t r = 0; r < TB_parameters.C; r++) {
    TB_parameters.decodeSuccess[r] = false;
  }
  reset_meas(&TB_parameters.ts_deinterleave);
  reset_meas(&TB_parameters.ts_rate_unmatch);
  reset_meas(&TB_parameters.ts_seg_prep);
  reset_meas(&TB_parameters.ts_ldpc_decode);

  if (ue->nrLDPC_coding_interface.nrLDPC_coding_decoder(&slot_parameters) != 0) {
    LOG_W(NR_PHY, "SENSING: passive PDSCH decode -- nrLDPC_coding_decoder failed\n");
    return false;
  }

  for (uint32_t r = 0; r < TB_parameters.C; r++) {
    if (!TB_parameters.decodeSuccess[r]) {
      return false; // per-segment CRC failed: the expected outcome for a grant meant for someone else
    }
  }

  // Reassemble the transport block, exactly as nr_dlsch_decoding() does.
  uint32_t offset = 0, r_offset = 0;
  for (uint32_t r = 0; r < TB_parameters.C; r++) {
    const uint32_t seg_bytes =
        (TB_parameters.K >> 3) - (TB_parameters.F >> 3) - ((TB_parameters.C > 1) ? 3 : 0);
    memcpy(h->b + offset, h->c + r_offset, seg_bytes);
    offset += seg_bytes;
    r_offset += (TB_parameters.K >> 3);
  }

  if (TB_parameters.C > 1 && !check_crc(h->b, lenWithCrc(1, cw->TBS), crcType(1, cw->TBS))) {
    return false; // segment CRCs passed but the TB CRC did not
  }

  // The same all-zero-payload guard the real decoder applies: an all-zero TB with a zero CRC is a
  // known recurring false pass, and on this path it would inject a constant, information-free X.
  const uint32_t sz = cw->TBS / 8;
  if (h->b[sz] == 0 && h->b[sz + 1] == 0) {
    uint32_t i = 0;
    while (i < sz && h->b[i] == 0) {
      i++;
    }
    if (i == sz) {
      return false;
    }
  }
  return true;
}

nr_pdsch_passive_decode_status_t nr_pdsch_passive_decode(PHY_VARS_NR_UE *ue,
                                                         const UE_nr_rxtx_proc_t *proc,
                                                         fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                                         const freq_alloc_bitmap_t *freq_alloc,
                                                         const nr_pdsch_passive_grant_t *grant,
                                                         c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                                         nr_pdsch_passive_decode_result_t *out)
{
  memset(out, 0, sizeof(*out));
  out->status = NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED;

  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;

  // ---- Scope: mirror nr_isac_pdsch_data_aided_submit()'s own guards. Decoding a grant whose
  // reconstruction we could not use anyway is pure CPU cost. ----
  if (dlsch_config->pduBitmap & 0x1) {
    return out->status; // PTRS
  }
  if (dlsch_config->numCsiRsForRateMatching > 0) {
    return out->status; // CSI-RS rate matching
  }
  int n_ports = 0;
  for (int i = 0; i < 12; i++) {
    if ((dlsch_config->dmrs_ports >> i) & 0x1) {
      n_ports++;
    }
  }
  /* Multi-layer is supported up to the receive-antenna count: separating Nl spatial streams needs
   * at least Nl receive antennas, and nr_rx_pdsch()'s MIMO equaliser is what does the separation.
   * MEASURED on the live srsRAN cell (4 DL antennas, commercial UE): num_layers=4 on 97 % of grants,
   * 3 on ~2 %, 1-2 on a handful -- so the previous "single layer only" guard rejected essentially
   * every real grant (`pdsch_decode[try=0 unsup=...]`) and the data-aided source could never fire.
   * The cell cannot be reconfigured to rank 1: its RU has 4 DL ports and the DU refuses
   * nof_antennas_dl < 4 ("RU number of downlink ports=4 must match the number of transmission
   * antennas"). */
  if (n_ports < 1 || n_ports > fp->nb_antennas_rx) {
    return out->status; // cannot separate more layers than we have receive antennas
  }

  // ---- Codeword parameters. get_cw_info()'s arithmetic (nr_ue_procedures.c), minus the HARQ
  // bookkeeping a passive receiver has no state for. ----
  fapi_nr_dl_cw_info_t *cw = &dlsch_config->cw_info[0];
  memset(cw, 0, sizeof(*cw));
  cw->mcs = grant->mcs;
  cw->rv  = grant->rv;
  cw->Nl  = (uint8_t)n_ports; // was pinned to 1; the DM-RS port count IS the layer count
  cw->new_data_indicator = true;
  cw->qamModOrder = nr_get_Qm_dl(grant->mcs, grant->mcs_table);
  const uint32_t R = nr_get_code_rate_dl(grant->mcs, grant->mcs_table);
  if (cw->qamModOrder == 0 || R == 0) {
    // MCS 28-31 (reserved-for-retransmission rows) have no modulation order/code rate of their own:
    // a real UE takes them from the initial transmission. A passive receiver has no such history,
    // so such a grant is simply not decodable here. nr_pdcch_blind_decode_and_extract() already
    // rejects those, so reaching this means the MCS table assumption is wrong.
    return out->status;
  }
  const uint8_t nb_re_dmrs = get_num_dmrs_re_per_rb(dlsch_config->dmrsConfigType, dlsch_config->n_dmrs_cdm_groups);
  const uint16_t dmrs_len  = get_num_dmrs(dlsch_config->dlDmrsSymbPos);
  cw->targetCodeRate = (uint16_t)R;
  cw->TBS = nr_compute_tbs(cw->qamModOrder, (uint16_t)R, freq_alloc->num_rbs, dlsch_config->number_symbols,
                           nb_re_dmrs * dmrs_len, grant->nb_rb_oh, grant->tb_scaling, cw->Nl);
  if (cw->TBS == 0) {
    return out->status;
  }
  cw->ldpcBaseGraph = get_BG(cw->TBS, cw->targetCodeRate);
  dlsch_config->n_codewords = 1;
  /* TBS_LBRM's layer term is n_L = min(maxMIMO-LayersPDSCH, 4) -- the UE's CAPABILITY (TS 38.212
   * 5.4.2.1), NOT the rank of this particular grant. It was hardcoded to 1, which is wrong for any
   * modern UE and matters because TBS_LBRM sets N_ref and hence the LDPC circular-buffer bound
   * N_cb: get it wrong and rate DE-matching reads a buffer of the wrong length, so the CRC can
   * never pass however clean the LLRs are.
   *
   * MEASURED, and it rules out deriving this from the observed rank: on this cell our own attached
   * UE is scheduled nl=1 while its capability file declares maxNumberMIMO-LayersPDSCH=fourLayers,
   * so the gNB uses n_L=4 for it too. A "largest rank seen" heuristic would give 1 and be wrong.
   * The gNB prints its own value: tb_size_lbrm=159749 bytes = 1277992 bits, which is n_L=4.
   *
   * A passive receiver cannot read the capability off the air, so this is the deployment fact the
   * module already takes on trust elsewhere (like csirs_monitor and dci_length_override). 4 is both
   * the spec ceiling and this deployment's value, so it is the default; re-derive per deployment if
   * a cell serves layer-limited UEs. */
  const int nl_tbslbrm = 4; // = min(maxMIMO-LayersPDSCH, 4); confirmed against the gNB's own print
  /* TS 38.212 5.4.2.1 sizes N_ref from TBS_LBRM over the carrier's LARGEST configured DL BWP, not
   * over whatever frequency reference this particular grant uses. Measured 2026-08-21: on a
   * CORESET#0 format-1_0 grant BWPSize is 48 and this produced lbrm=229576 against the gNB's own
   * 1277992 -- harmless there only because N_ref cannot bind on an 808-bit TB, and wrong the moment
   * a large TB is decoded. */
  const uint16_t bw_lbrm = grant->bw_tbslbrm > 0 ? grant->bw_tbslbrm : dlsch_config->BWPSize;
  /* ...and its MODULATION term is likewise a cell property, not this grant's table. MEASURED
   * 2026-08-21 on a SIB1 grant: passing the grant's own (format-1_0-forced) table 0 gave
   * lbrm=950984 against the gNB's 1277992, a clean Qm 6-vs-8 ratio. Fixing only the bandwidth left
   * this half wrong. */
  const uint8_t tbl_lbrm = grant->mcs_table_lbrm >= 0 ? (uint8_t)grant->mcs_table_lbrm : grant->mcs_table;
  dlsch_config->tbslbrm = nr_compute_tbslbrm(tbl_lbrm, bw_lbrm, (uint8_t)nl_tbslbrm);

  const uint32_t G = nr_get_G(freq_alloc->num_rbs, dlsch_config->number_symbols, nb_re_dmrs, dmrs_len,
                              0 /* unav_res: PTRS/CSI-RM excluded above */, cw->qamModOrder, cw->Nl);
  if (G == 0) {
    return out->status;
  }
  /* TBPARM probe (ISAC_PDSCH_TBPARM=1): every transport-block parameter the gNB also prints on its
   * own PDSCH line, so they can be compared one-for-one instead of inferred from a CRC failure.
   * gNB prints: mcs_index / mod / tbs / tb_size_lbrm / ldpc_base_graph / vrbs=[start..end). */
  {
    static int s_tbp = -1;
    if (s_tbp < 0)
      s_tbp = (getenv("ISAC_PDSCH_TBPARM") != NULL) ? 1 : 0;
    if (s_tbp)
      LOG_I(PHY,
            "SENSING: TBPARM rnti=0x%x nl=%u mcs=%u tbl=%u Qm=%u R=%u tbs=%u G=%u lbrm=%u bg=%u "
            "prb=%u+%u nsym=%u dmrs_len=%u nb_re_dmrs=%u\n",
            grant->rnti, (unsigned)cw->Nl, (unsigned)grant->mcs, (unsigned)grant->mcs_table,
            (unsigned)cw->qamModOrder, (unsigned)cw->targetCodeRate, (unsigned)cw->TBS, G,
            (unsigned)dlsch_config->tbslbrm, (unsigned)cw->ldpcBaseGraph, (unsigned)freq_alloc->first_rb,
            (unsigned)freq_alloc->num_rbs, (unsigned)dlsch_config->number_symbols, (unsigned)dmrs_len,
            (unsigned)nb_re_dmrs);
  }

  out->cw = *cw;
  out->G  = G;

  if (!passive_harq_prepare(&g_harq, fp->N_RB_DL)) {
    out->status = NR_PDSCH_PASSIVE_DECODE_ERROR;
    return out->status;
  }

  // ---- FEP every symbol of the allocation. The caller keeps this buffer: the data-aided submit
  // needs the SAME Y samples to form Ĥ = Y/X, and re-transforming them would be both wasteful and a
  // chance for the two views to diverge. ----
  for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
    nr_slot_fep(ue, fp, proc->nr_slot_rx, m, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
  }

  // ---- Channel estimation on the DM-RS symbols. ----
  const uint32_t pdsch_est_size = ((fp->symbols_per_slot * fp->ofdm_symbol_size + 15) / 16) * 16;
  fourDimArray_t *toFree = NULL;
  // One estimate per (layer, rx antenna) -- nr_rx_pdsch() indexes this as nl*nb_antennas_rx + aarx,
  // matching nr_ue_pdsch_procedures()'s own allocation. Estimating only layer 0 (as this used to)
  // gives the equaliser nothing to separate the other layers with.
  allocCast2D(pdsch_dl_ch_estimates, int32_t, toFree, fp->nb_antennas_rx * cw->Nl, pdsch_est_size, false);

  uint32_t nvar = 0;
  int n_dmrs_sym = 0;
  for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
    if (!((dlsch_config->dlDmrsSymbPos >> m) & 1)) {
      continue;
    }
    for (int nl = 0; nl < cw->Nl; nl++) { // mirrors nr_ue_pdsch_procedures()'s per-layer loop
      uint32_t nvar_tmp = 0;
      nr_pdsch_channel_estimation(ue, proc, dlsch_config, freq_alloc, nl,
                                  get_dmrs_port(nl, dlsch_config->dmrs_ports), (unsigned char)m, pdsch_est_size,
                                  pdsch_dl_ch_estimates, fp->samples_per_slot_wCP, rxdataF, &nvar_tmp);
      nvar += nvar_tmp;
    }
    n_dmrs_sym++;
  }
  if (n_dmrs_sym == 0) {
    free(toFree);
    return out->status; // no DM-RS in the allocation: nothing to equalise against
  }
  // nr_ue_pdsch_procedures() divides by number_symbols (not by the DM-RS symbol count) x layers x
  // antennas; mirrored so nvar carries the same scale the attached path's gates were tuned against.
  nvar /= (uint32_t)(dlsch_config->number_symbols * cw->Nl * fp->nb_antennas_rx);

  /* CHESTDIAG (ISAC_PDSCH_TBPARM=1): per-(layer,antenna) channel power, plus the layer-space Gram
   * matrix conditioning. This is the one remaining hypothesis for the rank-4 CRC failure that has
   * been asserted but never measured: separating Nl spatial streams requires the Nl x nbRx effective
   * channel to be well conditioned AT THIS RECEIVER, and the gNB chose its precoder from the SERVED
   * UE's PMI, not ours. If the layer columns are near-parallel here the MMSE inverse amplifies noise
   * without bound and no parameter fix can help; if they are well separated, the fault is in the
   * demodulation chain instead. Cheap: one pass over the already-computed estimates. */
  {
    static int s_cd = -1;
    if (s_cd < 0)
      s_cd = (getenv("ISAC_PDSCH_TBPARM") != NULL) ? 1 : 0;
    static int s_cd_left = 12;
    if (s_cd && s_cd_left > 0 && cw->Nl >= 1) {
      s_cd_left--;
      const int nsc = freq_alloc->num_rbs * NR_NB_SC_PER_RB;
      /* Estimates are written at ch_offset = ofdm_symbol_size * <DM-RS symbol>, NOT at
       * start_symbol -- reading start_symbol returns an untouched buffer (all zeros). */
      int sym = -1;
      for (int m2 = 0; m2 < NR_SYMBOLS_PER_SLOT; m2++) {
        if (dlsch_config->dlDmrsSymbPos & (1u << m2)) {
          sym = m2;
          break;
        }
      }
      if (sym < 0)
        sym = dlsch_config->start_symbol;
      char rep[420];
      int u = 0;
      double pw[8][8];
      for (int l = 0; l < cw->Nl && l < 8; l++) {
        for (int a = 0; a < fp->nb_antennas_rx && a < 8; a++) {
          const c16_t *h = (const c16_t *)&pdsch_dl_ch_estimates[l * fp->nb_antennas_rx + a][fp->ofdm_symbol_size * sym];
          double acc = 0;
          for (int k = 0; k < nsc; k++)
            acc += (double)h[k].r * h[k].r + (double)h[k].i * h[k].i;
          pw[l][a] = acc / (nsc > 0 ? nsc : 1);
        }
      }
      /* MIMO separability, done properly (2026-08-20). The previous statistic summed the layer
       * inner product over antennas AND subcarriers with POWER weighting, so a single dominant
       * branch (ch0 measured 11-17 dB above the others) made it return ~1 mechanically, whatever
       * the true spatial structure was. It could not distinguish "layers are parallel" from
       * "only one antenna is alive".
       *
       * Correct measure: per-subcarrier Gram matrix over LAYERS, G = H^H H with H[antenna][layer],
       * after normalising each ANTENNA branch by its own RMS so per-chain gain cannot bias the
       * geometry. Then the normalised determinant det(G) / prod(G_ii) in [0,1]:
       *   1  -> layers mutually orthogonal, full rank, 4 streams separable
       *   0  -> layers collinear, rank deficient, streams NOT separable
       * Computed by Cholesky (G is Hermitian positive semi-definite), which also yields the
       * per-layer residual L_ii^2 / G_ii: how much NEW information each layer adds beyond the
       * previous ones. That is the honest "effective rank" read-out. */
      double bn[8];
      for (int a = 0; a < fp->nb_antennas_rx && a < 8; a++) {
        double acc = 0;
        for (int l = 0; l < cw->Nl && l < 8; l++)
          acc += pw[l][a];
        bn[a] = (acc > 0) ? 1.0 / sqrt(acc) : 0.0; // per-branch gain normalisation
      }
      const int NL = (cw->Nl < 4) ? cw->Nl : 4;
      double gr[4][4] = {{0}}, gi[4][4] = {{0}};
      for (int x = 0; x < NL; x++) {
        for (int y = 0; y < NL; y++) {
          double ar = 0, ai = 0;
          for (int a = 0; a < fp->nb_antennas_rx && a < 8; a++) {
            const c16_t *hx = (const c16_t *)&pdsch_dl_ch_estimates[x * fp->nb_antennas_rx + a][fp->ofdm_symbol_size * sym];
            const c16_t *hy = (const c16_t *)&pdsch_dl_ch_estimates[y * fp->nb_antennas_rx + a][fp->ofdm_symbol_size * sym];
            const double w = bn[a] * bn[a];
            for (int k = 0; k < nsc; k++) {
              ar += w * ((double)hx[k].r * hy[k].r + (double)hx[k].i * hy[k].i);
              ai += w * ((double)hx[k].r * hy[k].i - (double)hx[k].i * hy[k].r);
            }
          }
          gr[x][y] = ar;
          gi[x][y] = ai;
        }
      }
      /* Cholesky of the Hermitian Gram; lr/li hold L. */
      double lr[4][4] = {{0}}, li[4][4] = {{0}}, resid[4] = {0};
      int pd = 1;
      for (int x = 0; x < NL && pd; x++) {
        for (int y = 0; y <= x && pd; y++) {
          double sr = gr[x][y], si = gi[x][y];
          for (int m2 = 0; m2 < y; m2++) {
            sr -= lr[x][m2] * lr[y][m2] + li[x][m2] * li[y][m2];
            si -= li[x][m2] * lr[y][m2] - lr[x][m2] * li[y][m2];
          }
          if (x == y) {
            if (sr <= 0) { pd = 0; break; }
            lr[x][x] = sqrt(sr);
            li[x][x] = 0;
            resid[x] = (gr[x][x] > 0) ? sr / gr[x][x] : 0.0; // fraction of layer x NOT explained by earlier layers
          } else {
            const double d = lr[y][y];
            lr[x][y] = sr / d;
            li[x][y] = si / d;
          }
        }
      }
      double detg = 1.0, prod_diag = 1.0;
      for (int x = 0; x < NL; x++) {
        detg *= pd ? (lr[x][x] * lr[x][x]) : 0.0;
        prod_diag *= gr[x][x];
      }
      const double orth = (prod_diag > 0) ? detg / prod_diag : 0.0;
      LOG_I(PHY,
            "SENSING: CHESTDIAG nl=%u nvar=%u orth=%.4f resid=[%.3f,%.3f,%.3f,%.3f] %s\n",
            (unsigned)cw->Nl, nvar, orth, resid[0], resid[1], resid[2], resid[3], rep);
    }
  }
  out->nvar = nvar;

  if (ue->chest_time == 1) {
    nr_chest_time_domain_avg(fp, (int32_t **)pdsch_dl_ch_estimates, dlsch_config->number_symbols,
                             dlsch_config->start_symbol, dlsch_config->dlDmrsSymbPos, freq_alloc->num_rbs, cw->Nl,
                             fp->nb_antennas_rx);
  }

  // ---- Demodulate to LLRs, symbol by symbol. nr_rx_pdsch() reads its transport-block parameters
  // out of a NR_UE_DLSCH_t and a NR_DL_UE_HARQ_t; both are stack-local here, deliberately (see the
  // header). `status = NR_ACTIVE` is what makes it apply the PTRS/symbol-span branch consistently
  // with the attached path -- with pduBitmap==0 it only selects the symbol bookkeeping. ----
  NR_UE_DLSCH_t dlsch = {0};
  dlsch.cw_info = *cw;
  dlsch.rnti = grant->rnti;
  dlsch.rnti_type = TYPE_C_RNTI_;
  dlsch.active = true;
  dlsch.max_ldpc_iterations = 8;

  NR_DL_UE_HARQ_t harq = {0};
  harq.status = NR_ACTIVE;
  harq.first_rx = 1;

  const uint32_t rx_llr_buf_sz = ALIGNARRAYSIZE(G, 32);
  int16_t *llr = (int16_t *)malloc16_clear(rx_llr_buf_sz * sizeof(int16_t));
  if (llr == NULL) {
    free(toFree);
    out->status = NR_PDSCH_PASSIVE_DECODE_ERROR;
    return out->status;
  }

  const uint32_t rx_size_symbol = (freq_alloc->num_rbs * NR_NB_SC_PER_RB + 15) & ~15;
  /* Middle dimension MUST be NR_MAX_NB_LAYERS, not cw->Nl: nr_rx_pdsch() declares these as
   * c16_t buf[][NR_MAX_NB_LAYERS][pdsch_buf_size_max], so it indexes symbol m with a COMPILE-TIME
   * stride of NR_MAX_NB_LAYERS * pdsch_buf_size_max. Allocating with the runtime layer count made
   * the per-symbol stride too small for any grant with Nl < NR_MAX_NB_LAYERS, and every write past
   * symbol 0 landed outside the buffer.
   * It never showed up before because this cell only ever scheduled Nl = 4, where the two happen to
   * be equal. The moment the gNB was reconfigured to max_rank = 1 it segfaulted on the first
   * full-band (273 PRB) grant -- big enough for the overrun to leave the mapping. */

  fourDimArray_t *toFree2 = NULL;
  allocCast3D(rxdataF_comp, c16_t, toFree2, fp->symbols_per_slot, NR_MAX_NB_LAYERS, rx_size_symbol, false);
  fourDimArray_t *toFree3 = NULL;
  allocCast3D(dl_ch_mag, c16_t, toFree3, NR_SYMBOLS_PER_SLOT, NR_MAX_NB_LAYERS, rx_size_symbol, false);
  fourDimArray_t *toFree4 = NULL;
  allocCast3D(dl_ch_magb, c16_t, toFree4, NR_SYMBOLS_PER_SLOT, NR_MAX_NB_LAYERS, rx_size_symbol, false);
  fourDimArray_t *toFree5 = NULL;
  allocCast3D(dl_ch_magr, c16_t, toFree5, NR_SYMBOLS_PER_SLOT, NR_MAX_NB_LAYERS, rx_size_symbol, false);

  c16_t ptrs_phase_per_slot[fp->nb_antennas_rx][NR_SYMBOLS_PER_SLOT];
  memset(ptrs_phase_per_slot, 0, sizeof(ptrs_phase_per_slot));
  int32_t ptrs_re_per_slot[fp->nb_antennas_rx][NR_SYMBOLS_PER_SLOT];
  memset(ptrs_re_per_slot, 0, sizeof(ptrs_re_per_slot));

  uint32_t dl_valid_re[NR_SYMBOLS_PER_SLOT] = {0};
  int32_t log2_maxh = 0;
  pdsch_scope_req_t scope_req = {.copy_chanest_to_scope = false, .copy_rxdataF_to_scope = false,
                                 .scope_rxdataF_offset = 0};

  // Same "first symbol carrying data" rule as nr_ue_pdsch_procedures().
  uint32_t dmrs_data_re = (dlsch_config->dmrsConfigType == NFAPI_NR_DMRS_TYPE1)
                              ? 12 - 6 * dlsch_config->n_dmrs_cdm_groups
                              : 12 - 4 * dlsch_config->n_dmrs_cdm_groups;
  int first_symbol_with_data = dlsch_config->start_symbol;
  while (dmrs_data_re == 0 && (dlsch_config->dlDmrsSymbPos & (1 << first_symbol_with_data))) {
    first_symbol_with_data++;
  }

  bool demod_ok = true;
  for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++) {
    if (nr_rx_pdsch(ue, proc, &dlsch, freq_alloc, dlsch_config, &harq, (unsigned char)m,
                    m == first_symbol_with_data, (unsigned char)dlsch_config->harq_process_nbr, pdsch_est_size,
                    pdsch_dl_ch_estimates, llr, dl_valid_re, rxdataF, &log2_maxh, rx_size_symbol,
                    fp->nb_antennas_rx, rxdataF_comp, dl_ch_mag, dl_ch_magb, dl_ch_magr, ptrs_phase_per_slot,
                    ptrs_re_per_slot, nvar, &scope_req, NULL /* rho_dl: single layer */)
        < 0) {
      demod_ok = false;
      break;
    }
  }

  if (demod_ok) {
    nr_dlsch_unscrambling(llr, G, 0 /* codeword */, dlsch_config->dlDataScramblingId, grant->rnti);
    if (passive_ldpc_decode(ue, proc, &g_harq, cw, dlsch_config, llr, freq_alloc->num_rbs, G)) {
      out->status = NR_PDSCH_PASSIVE_DECODE_CRC_OK;
      out->tb     = g_harq.b;
    } else {
      out->status = NR_PDSCH_PASSIVE_DECODE_CRC_FAIL;
    }
  } else {
    out->status = NR_PDSCH_PASSIVE_DECODE_ERROR;
  }

  /* Per-RNTI outcome (ISAC_PDSCH_TBPARM=1). Run inside an ATTACHED UE this splits the decode
   * population into grants addressed to US and grants addressed to ANOTHER UE, with everything
   * else -- code, rank machinery, config, radio, slot -- held identical. That is the controlled
   * test for whether a passive PDSCH failure is positional (precoder conditioned for the served
   * UE) or a defect in the receive path. */
  {
    static int s_tbp2 = -1;
    if (s_tbp2 < 0)
      s_tbp2 = (getenv("ISAC_PDSCH_TBPARM") != NULL) ? 1 : 0;
    if (s_tbp2)
      /* Every field needed to attribute a failure is on THIS line. Do NOT reconstruct it by pairing
       * against the preceding TBPARM line: decodes for different slots interleave in the log, so
       * adjacency-based pairing silently mis-attributes (it produced two mutually contradictory
       * breakdowns before this was fixed). Same class of error as the retracted "20 % dt bias". */
      LOG_I(PHY,
            "SENSING: TBRESULT rnti=0x%x nl=%u mcs=%u Qm=%u R=%u tbs=%u bg=%u prb=%u+%u status=%s\n",
            grant->rnti, (unsigned)cw->Nl, (unsigned)grant->mcs, (unsigned)cw->qamModOrder,
            (unsigned)cw->targetCodeRate, (unsigned)cw->TBS, (unsigned)cw->ldpcBaseGraph,
            (unsigned)freq_alloc->first_rb, (unsigned)freq_alloc->num_rbs,
            out->status == NR_PDSCH_PASSIVE_DECODE_CRC_OK ? "CRC_OK"
              : (out->status == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL ? "CRC_FAIL" : "ERROR"));
  }

  free(llr);
  free(toFree);
  free(toFree2);
  free(toFree3);
  free(toFree4);
  free(toFree5);
  return out->status;
}
