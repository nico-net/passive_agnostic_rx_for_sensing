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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.c
 * \brief See nr_pdsch_data_aided.h. Body lifted from phy_procedures_nr_ue.c's former static
 * nr_isac_pdsch_data_aided_tap() so the attached-UE and passive receivers share ONE reconstruction
 * chain -- the comments below are the original ones, kept because they record decisions (the
 * explicit 32-byte alignment, the heap-not-TLS per-antenna buffer, the sub-slot grouping gates)
 * that were each paid for with a real bug.
 */

#include "nr_pdsch_data_aided.h"

/* Set by a DEFERRED caller to this job's monotonic absolute slot; 0 = derive from proc. */
__thread uint64_t nr_isac_abs_slot_override = 0;

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"

#include "PHY/CODING/coding_defs.h"
#include "PHY/NR_REFSIG/dmrs_nr.h" // get_num_dmrs_re_per_rb, nr_chest_time_domain_avg
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/MODULATION/nr_modulation.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/NR_UE_ISAC/nr_isac.h"
#include "executables/nr-uesoftmodem.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"

void nr_isac_pdsch_data_aided_submit(PHY_VARS_NR_UE *ue,
                                     const UE_nr_rxtx_proc_t *proc,
                                     const fapi_nr_dl_cw_info_t *cw,
                                     const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                     const freq_alloc_bitmap_t *freq_alloc,
                                     uint16_t rnti,
                                     const uint8_t *tb_bytes,
                                     uint32_t harq_pid_tag,
                                     const c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                     double nvar)
{
  if (!nr_isac_enabled() || !nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DATA))
    return;
  if (!nr_isac_flow_admit(rnti))
    return;

  /* SINGLE LAYER ONLY -- and this is a correctness guard, not a scope preference.
   *
   * The extraction below computes Ĥ[k] = Y[k]/X[k] as a SCALAR division per (RE, rx antenna). That
   * is only valid when one modulation symbol is transmitted per RE. At Nl > 1 the receive sample is
   * a superposition, y_a[k] = sum_l h_{a,l}[k] * x_l[k], so dividing by any single layer's x_l
   * leaves the other layers' contributions in the "channel estimate" -- it does not merely add
   * noise, it produces a CFR that is not a channel at all, and a plausible-looking but wrong range
   * profile. Recovering the per-layer h_{a,l} needs a LEAST-SQUARES SOLVE over a window of REs
   * (4 rx antennas give 4 equations per RE against Nl*nb_ant unknowns, so W >= Nl REs must be
   * pooled assuming H is flat across them) -- deliberately NOT attempted here.
   *
   * The passive DECODE path now supports Nl > 1 (nr_pdsch_passive_decode.c), so a rank-4 grant will
   * decode and its CRC pass rate is measurable; it just cannot contribute a CFR row until that
   * solve exists. Contributing nothing is always preferable to corrupting the range profile -- the
   * same rule the DM-RS-symbol RE-enumeration bug below is written under. */
  if (cw->Nl != 1) {
    static __thread bool warned_ml = false;
    if (!warned_ml) {
      warned_ml = true;
      LOG_W(NR_PHY,
            "SENSING: data-aided CFR skipped -- grant has Nl=%u layers and Ĥ=Y/X is only valid at "
            "Nl=1 (needs a windowed least-squares MIMO solve). The TB still decodes; only the CFR "
            "submission is suppressed.\n",
            cw->Nl);
    }
    return;
  }

  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;

  // --- TB CRC-included payload: tb_bytes is ALREADY B = A + TB-CRC bits (16 or 24-bit, matching
  // NR_MAX_PDSCH_TBS threshold) -- the exact same buffer format the TX-side encoder segments, so no
  // CRC attach is needed here; the decoder already reconstructed it. ---
  const uint32_t A = cw->TBS;
  const unsigned int B = A + (A > NR_MAX_PDSCH_TBS ? 24 : 16);

  // --- Segment into code blocks (must run again here to fill actual per-segment bytes: the decode
  // path only sized C/K/Z/F, passing NULL/NULL for input/output). ---
  static __thread uint8_t seg_storage[MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER][8448];
  static __thread uint8_t *c_segs[MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER];
  for (int r = 0; r < MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER; r++)
    c_segs[r] = seg_storage[r];

  nrLDPC_TB_encoding_parameters_t TB_parameters = {0};
  // Offset well clear of real harq_pid / 2*harq_pid+cw_idx ranges used by concurrent PDSCH decode
  // and PUSCH encode on this same nrLDPC_coding_interface, to avoid any id collision. The caller
  // supplies the tag so the attached and passive paths cannot collide with each other either.
  TB_parameters.harq_unique_pid = harq_pid_tag;
  TB_parameters.BG = cw->ldpcBaseGraph;
  TB_parameters.A = A;
  TB_parameters.Kb = nr_segmentation((unsigned char *)tb_bytes, c_segs, B, &TB_parameters.C, &TB_parameters.K,
                                     &TB_parameters.Z, &TB_parameters.F, TB_parameters.BG);
  if (TB_parameters.C > MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER) {
    LOG_W(NR_PHY, "SENSING: data-aided tap skipped -- too many segments C=%u\n", TB_parameters.C);
    return;
  }

  TB_parameters.nb_rb = freq_alloc->num_rbs;
  TB_parameters.Qm = cw->qamModOrder;
  TB_parameters.mcs = cw->mcs;
  TB_parameters.nb_layers = cw->Nl;
  TB_parameters.rv_index = cw->rv;
  TB_parameters.tbslbrm = dlsch_config->tbslbrm;

  const uint8_t  nb_re_dmrs = get_num_dmrs_re_per_rb(dlsch_config->dmrsConfigType, dlsch_config->n_dmrs_cdm_groups);
  const uint16_t dmrs_len   = get_num_dmrs(dlsch_config->dlDmrsSymbPos);
  TB_parameters.G = nr_get_G(freq_alloc->num_rbs, dlsch_config->number_symbols, nb_re_dmrs, dmrs_len,
                            0 /* unav_res: PTRS/CSI-RM already excluded by the caller */, cw->qamModOrder, cw->Nl);
  if (TB_parameters.G == 0)
    return;

  // 32-byte aligned: nr_modulation() / the LDPC encoder store through AVX2 intrinsics that fault on a
  // misaligned buffer. These are thread-local, so their addresses depend on the TLS block layout --
  // i.e. on every other __thread object in this file. That made the alignment ACCIDENTAL: adding the
  // sub-slot bookkeeping arrays below shifted the layout and produced an immediate GP fault inside
  // nr_modulation's `out128[i] = ...` store. Pin it explicitly so the layout can never break it again.
  static __thread uint8_t coded_bits[(273 * 12 * 14 * 8 + 63) / 64 * 64 + 64] __attribute__((aligned(32)));
  memset(coded_bits, 0, sizeof(coded_bits));
  TB_parameters.output = coded_bits;

  static __thread nrLDPC_segment_encoding_parameters_t segments[MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER];
  memset(segments, 0, sizeof(segments));
  TB_parameters.segments = segments;
  for (uint32_t r = 0; r < TB_parameters.C; r++) {
    segments[r].c = c_segs[r];
    segments[r].E = nr_get_E(TB_parameters.G, TB_parameters.C, TB_parameters.Qm, TB_parameters.nb_layers, r);
    reset_meas(&segments[r].ts_interleave);
    reset_meas(&segments[r].ts_rate_match);
    reset_meas(&segments[r].ts_ldpc_encode);
  }

  nrLDPC_slot_encoding_parameters_t slot_parameters = {.frame = proc->frame_rx,
                                                       .slot = proc->nr_slot_rx,
                                                       .nb_TBs = 1,
                                                       .threadPool = &get_nrUE_params()->Tpool,
                                                       .tinput = NULL,
                                                       .tinput_memcpy = NULL,
                                                       .tprep = NULL,
                                                       .tparity = NULL,
                                                       .toutput = NULL,
                                                       .tconcat = NULL,
                                                       .TBs = &TB_parameters};
  if (ue->nrLDPC_coding_interface.nrLDPC_coding_encoder(&slot_parameters) != 0) {
    LOG_W(NR_PHY, "SENSING: data-aided LDPC re-encode failed\n");
    return;
  }

  // --- Scramble (same Gold-sequence XOR the gNB TX side uses) + modulate. ---
  static __thread uint32_t scrambled[(273 * 12 * 14 * 8 + 31) / 32 + 1] __attribute__((aligned(32)));
  nr_codeword_scrambling(coded_bits, TB_parameters.G, 0 /* codeword index */, dlsch_config->dlDataScramblingId,
                        rnti, scrambled);

  static __thread c16_t mod_syms[273 * 12 * 14] __attribute__((aligned(32)));
  nr_modulation(scrambled, TB_parameters.G, cw->qamModOrder, (int16_t *)mod_syms);

  // --- RE mapping: sweep the allocation symbol by symbol, increasing frequency within each, taking
  // every RE the transmitter mapped a data symbol to -- the same order (and, for DM-RS symbols, the
  // same per-RE bitmap) nr_dlsch_extract_rbs() uses, mirrored here in the forward
  // TX-reconstruction direction. Ĥ[k] = Y[k]/X[k] at every such RE. --
  const uint32_t base_sc  = (uint32_t)(dlsch_config->BWPStart + freq_alloc->first_rb) * NR_NB_SC_PER_RB;
  const int      num_sc   = freq_alloc->num_rbs * NR_NB_SC_PER_RB;
  const int      start_re = (fp->first_carrier_offset + (dlsch_config->BWPStart + freq_alloc->first_rb) * NR_NB_SC_PER_RB)
                           % fp->ofdm_symbol_size;

  // Receive-array AoA (PHASE3_AOA_MULTISTATIC_HANDOVER 5.4): extract Ĥ = Y/X for every rx antenna,
  // not just antenna 0. X is the SAME reconstructed transport block for all of them, so this is a
  // pure inner loop over rxdataF[a] -- the per-element phase difference it captures IS the bearing.
  const uint32_t isac_max_re = 273 * 12 * 14;
  uint32_t       isac_nof_ant = nr_isac_rx_channels();
  if (isac_nof_ant > (uint32_t)fp->nb_antennas_rx)
    isac_nof_ant = (uint32_t)fp->nb_antennas_rx;
  if (isac_nof_ant == 0)
    isac_nof_ant = 1;

  // Heap + thread-local pointer rather than a __thread array: with AoA on this is nof_ant x 367 kB,
  // which would bloat every DL worker's TLS block even in the far more common single-antenna case --
  // and a shifted TLS layout is exactly what produced the AVX alignment fault documented above.
  // Allocated once per thread, grown if the antenna count ever rises.
  static __thread float*   isac_h        = NULL;
  static __thread uint32_t isac_h_nant   = 0;
  if (isac_h == NULL || isac_h_nant < isac_nof_ant) {
    free(isac_h);
    isac_h = (float *)aligned_alloc(32, (size_t)isac_nof_ant * 2 * isac_max_re * sizeof(float));
    if (isac_h == NULL) {
      isac_h_nant = 0;
      LOG_W(NR_PHY, "SENSING: data-aided tap skipped -- could not allocate the per-antenna CFR buffer\n");
      return;
    }
    isac_h_nant = isac_nof_ant;
  }
  static __thread uint32_t isac_k[273 * 12 * 14];
  static __thread uint32_t isac_l[273 * 12 * 14];
  // Per-contributing-symbol slice bookkeeping, for sub-slot sampling: REs are emitted symbol by
  // symbol below, so each symbol owns a contiguous [start, start+count) span of the arrays.
  static __thread uint32_t sym_id[NR_SYMBOLS_PER_SLOT];
  static __thread uint32_t sym_start[NR_SYMBOLS_PER_SLOT];
  static __thread uint32_t sym_count[NR_SYMBOLS_PER_SLOT];
  static __thread double   sym_ypow[NR_SYMBOLS_PER_SLOT]; // summed |Y|^2, for the per-group SNR gate
  uint32_t nof_sym = 0;
  uint32_t nof_re  = 0;
  uint32_t mod_idx = 0;
  const uint32_t max_re = sizeofArray(isac_k);

  for (int l = dlsch_config->start_symbol; l < dlsch_config->start_symbol + dlsch_config->number_symbols; l++) {
    // --- Which REs of this symbol carry DATA. A DM-RS symbol is NOT necessarily data-free: with
    // numDmrsCdmGrpsNoData == 1 only one CDM group is reserved and the rest of the symbol is
    // ordinary PDSCH, which TS 38.211 7.3.1.6 maps in the same increasing-k-then-l sweep as every
    // other data RE. Bitmaps taken verbatim from nr_dlsch_extract_rbs() (bit set = DM-RS), so the
    // reconstruction and the receiver's own RE extraction cannot drift apart.
    //
    // THIS IS LOAD-BEARING, not a refinement (found 2026-07-30): simply `continue`-ing on a DM-RS
    // symbol without consuming its data REs desynchronises `mod_idx` from the transmitter's mapping
    // for EVERY later symbol, so the reconstructed X -- and therefore Ĥ = Y/X -- is wrong from the
    // first DM-RS symbol onward.
    //
    // Which case a deployment lands in is decided by gNB_scheduler_primitives.c's
    // nr_set_pdsch_semi_static()-equivalent: DCI 1_0 takes numDmrsCdmGrpsNoData = (nrOfSymbols <= 2
    // ? 1 : 2), so an ordinary-length 1_0 grant gets 2 -- the whole-symbol case, which behaves
    // identically with or without this fix. DCI 1_1 at ONE layer takes 1 unconditionally
    // (:315), i.e. always the broken case. A gNB switches a UE to 1_1 as soon as its
    // dedicated ue-Specific search space is configured with dci_Formats = formats0_1_And_1_1
    // (:3062), so any RRC-connected UE on this codebase is in the broken case.
    //
    // Arithmetic check, worth redoing if this is ever touched: nr_get_G() budgets
    // (12*nb_symb - nb_re_dmrs*len_dmrs) data REs per RB. Type 1 / one CDM group, 13 symbols, 2
    // DM-RS symbols -> 12*13 - 6*2 = 144, which is 11 full symbols (132) PLUS 6 REs in each of the
    // 2 DM-RS symbols. Enumerating only the 11 full symbols yields 132 -- both short AND misaligned.
    const bool pilots = ((dlsch_config->dlDmrsSymbPos >> l) & 1) != 0;
    uint16_t   dmrs_re_bitmap = 0;
    if (pilots) {
      dmrs_re_bitmap = 0xfff; // default: the whole symbol is DM-RS
      if (dlsch_config->dmrsConfigType == NFAPI_NR_DMRS_TYPE1 && dlsch_config->n_dmrs_cdm_groups == 1)
        dmrs_re_bitmap = 0x555; // alternating from RE 0
      else if (dlsch_config->dmrsConfigType == NFAPI_NR_DMRS_TYPE2 && dlsch_config->n_dmrs_cdm_groups == 1)
        dmrs_re_bitmap = 0x0c3; // REs 0,1 and 6,7
      else if (dlsch_config->dmrsConfigType == NFAPI_NR_DMRS_TYPE2 && dlsch_config->n_dmrs_cdm_groups == 2)
        dmrs_re_bitmap = 0x3cf; // REs 0,1,2,3 and 6,7,8,9
      if (dmrs_re_bitmap == 0xfff)
        continue; // no data in this symbol at all: nothing to consume, nothing to measure
    }

    const uint32_t sym_re0 = nof_re;
    double         ypow    = 0.0;
    for (int j = 0; j < num_sc && nof_re < max_re && mod_idx < TB_parameters.G / cw->qamModOrder; j++) {
      // j counts from the first allocated subcarrier, which is RB-aligned, so j % 12 is the RE index
      // within the RB -- the same index nr_dlsch_extract_rbs() tests its bitmap against.
      if (pilots && ((dmrs_re_bitmap >> (j % NR_NB_SC_PER_RB)) & 1))
        continue; // DM-RS RE: the transmitter placed no data symbol here, so do not consume one
      int re = start_re + j;
      if (re >= fp->ofdm_symbol_size)
        re -= fp->ofdm_symbol_size;
      const c16_t x = mod_syms[mod_idx++];
      const float xr = (float)x.r, xi = (float)x.i;
      const float xmag2 = xr * xr + xi * xi;
      if (xmag2 < 1e-6f)
        continue; // shouldn't happen for a QAM point, but guard the division
      for (uint32_t a = 0; a < isac_nof_ant; a++) {
        const c16_t y  = rxdataF[a][l * fp->ofdm_symbol_size + re];
        const float yr = (float)y.r, yi = (float)y.i;
        // Ĥ = Y / X = Y * conj(X) / |X|^2
        const size_t o = (size_t)2 * ((size_t)a * isac_max_re + nof_re);
        isac_h[o]     = (yr * xr + yi * xi) / xmag2;
        isac_h[o + 1] = (yi * xr - yr * xi) / xmag2;
        if (a == 0)
          ypow += (double)yr * yr + (double)yi * yi; // gates judge the primary antenna
      }
      isac_k[nof_re]         = base_sc + (uint32_t)j;
      isac_l[nof_re]         = (uint32_t)l;
      nof_re++;
    }
    if (nof_re > sym_re0 && nof_sym < sizeofArray(sym_id)) {
      sym_id[nof_sym]    = (uint32_t)l;
      sym_start[nof_sym] = sym_re0;
      sym_count[nof_sym] = nof_re - sym_re0;
      sym_ypow[nof_sym]  = ypow;
      nof_sym++;
    }
  }

  if (nof_re == 0)
    return;

  // --- Invariant: the enumeration above must consume EXACTLY the modulation symbols the rate
  // matcher produced. nr_get_G() budgets (12*nb_symb - nb_re_dmrs*len_dmrs)*nb_rb data REs, and this
  // loop takes one symbol per data RE, so `mod_idx` must land on G/(Qm*Nl). Any mismatch means the
  // RE model and the transmitter's mapping disagree -- which does not merely lose REs, it MISALIGNS
  // every RE after the divergence and makes Ĥ = Y/X meaningless. This is exactly the failure the
  // DM-RS-symbol data REs caused (see the bitmap comment above); checked from now on rather than
  // trusted. One-shot LOG_W, not an assert: this runs on the RT path and contributing nothing is
  // always preferable to killing the receiver.
  const uint32_t expected_syms = TB_parameters.G / (cw->qamModOrder * cw->Nl);
  if (mod_idx != expected_syms) {
    static __thread bool warned = false;
    if (!warned) {
      warned = true;
      LOG_W(NR_PHY,
            "SENSING: data-aided RE enumeration consumed %u modulation symbols but rate matching produced %u "
            "(nb_rb=%u nb_symb=%u dmrs_mask=0x%x cdm_groups=%u type=%u) -- the reconstructed X is MISALIGNED and "
            "every submitted CFR from this grant shape is invalid\n",
            mod_idx, expected_syms, freq_alloc->num_rbs, dlsch_config->number_symbols, dlsch_config->dlDmrsSymbPos,
            dlsch_config->n_dmrs_cdm_groups, dlsch_config->dmrsConfigType);
    }
    return; // contribute nothing rather than corrupt the range profile
  }

  nr_isac_carrier_t carrier = {.nof_prb         = (uint32_t)fp->N_RB_DL,
                               .scs_hz          = fp->subcarrier_spacing,
                               .dl_center_hz    = fp->dl_CarrierFreq,
                               .pci             = fp->Nid_cell,
                               .slots_per_frame = fp->slots_per_frame};
  /* Slow-time index for the CPI grid. `proc->frame_rx` WRAPS at 1024, so the obvious
   * frame*slots_per_frame + slot wraps every 20480 slots. In-line that was benign: submissions
   * arrived in strict slot order and a wrap was just a discontinuity between CPIs. Once the decode
   * is DEFERRED to a consumer pool they arrive concurrently and can straddle a wrap, and the CPI's
   * slot SPAN then explodes -- measured 2026-08-24 as T_slot = 1942.6 slots between rows on a run
   * producing ~670 rows/s (true spacing ~3), which drove vel[max] to 0.0 m/s.
   * A deferred caller publishes the producer's MONOTONIC absolute slot here before calling;
   * 0 means "not set", i.e. the previous behaviour for the in-order attached-UE path. */
  const uint32_t slot_idx = (nr_isac_abs_slot_override != 0)
                                ? (uint32_t)nr_isac_abs_slot_override
                                : (uint32_t)(proc->frame_rx * fp->slots_per_frame + proc->nr_slot_rx);

  // --- Sub-slot sampling (defs_nr_UE_ISAC.h): split this slot's symbols into GROUPS and submit each
  // as its own slow-time row, multiplying the effective PRF (and hence the unambiguous velocity) by
  // the number of groups. Grouping is ADAPTIVE because a short row is both sparser and noisier:
  //   * SPARSITY gate -- a group must carry >= min_re REs (fewer REs = less frequency coverage =
  //     a poorer, more sidelobe-prone range profile).
  //   * SNR gate -- estimated post-integration SNR must clear min_snr_db. Per-RE SNR is
  //     (mean|Y|^2 - nvar)/nvar from the demodulator's own noise estimate, and coherent integration
  //     over N REs adds a factor N; both are in the SAME received-signal units, so the threshold is
  //     a real dB figure rather than a scale-dependent fudge.
  // A group that fails either gate absorbs the next symbol and retries; a TAIL group that can never
  // pass is merged backwards into its predecessor rather than emitted, so we never inject a weak row
  // into the slow-time sequence (which would just feed CFAR false alarms and undo the PRF gain).
  uint32_t sub_min_re = 0;
  float    sub_min_snr_db = 0.0f;
  const uint32_t sub_target = nr_isac_subslot_config(&sub_min_re, &sub_min_snr_db);

  if (sub_target == 0 || nof_sym <= 1) {
    nr_isac_submit_cfr_multi(slot_idx, 0.0f, NR_ISAC_SRC_PDSCH_DATA, &carrier, isac_h, isac_nof_ant, isac_max_re,
                             isac_k, isac_l, nof_re, (float)nvar);
    return;
  }

  const double nv = (nvar > 0.0) ? nvar : 1.0;
  uint32_t g_first = 0; // first symbol index of the group being assembled
  uint32_t emitted = 0;
  for (uint32_t i = 0; i < nof_sym; i++) {
    const uint32_t nsym = i - g_first + 1;
    uint32_t g_re = 0;
    double   g_yp = 0.0;
    for (uint32_t t = g_first; t <= i; t++) {
      g_re += sym_count[t];
      g_yp += sym_ypow[t];
    }
    // Estimated coherent SNR of this group, in dB.
    const double per_re = (g_re > 0) ? ((g_yp / (double)g_re) - nv) / nv : -1.0;
    const double snr_db = (per_re > 0.0) ? 10.0 * log10(per_re * (double)g_re) : -99.0;
    const bool   gates_ok = (g_re >= sub_min_re) && (snr_db >= (double)sub_min_snr_db);
    const bool   is_last  = (i + 1 == nof_sym);

    if ((nsym >= sub_target && gates_ok) || (is_last && gates_ok)) {
      // Place the row at the group's centre symbol, in slots within this slot.
      const double centre = 0.5 * ((double)sym_id[g_first] + (double)sym_id[i]) + 0.5;
      const float  frac   = (float)(centre / (double)NR_SYMBOLS_PER_SLOT);
      // Slice, not copy: ant_stride_re stays the FULL buffer stride so antenna a's slice starts at
      // the same symbol offset within its own plane.
      nr_isac_submit_cfr_multi(slot_idx, frac, NR_ISAC_SRC_PDSCH_DATA, &carrier, &isac_h[2 * sym_start[g_first]],
                               isac_nof_ant, isac_max_re, &isac_k[sym_start[g_first]], &isac_l[sym_start[g_first]],
                               g_re, (float)nvar);
      emitted++;
      g_first = i + 1;
    } else if (is_last) {
      // Tail that never passed the gates: merge it BACKWARDS by re-emitting from g_first to the end
      // as one row if nothing has been emitted yet, otherwise fold it into the whole-slot fallback.
      if (emitted == 0) {
        nr_isac_submit_cfr_multi(slot_idx, 0.0f, NR_ISAC_SRC_PDSCH_DATA, &carrier, isac_h, isac_nof_ant, isac_max_re,
                                 isac_k, isac_l, nof_re, (float)nvar);
        emitted++;
      } else {
        const double centre = 0.5 * ((double)sym_id[g_first] + (double)sym_id[i]) + 0.5;
        const float  frac   = (float)(centre / (double)NR_SYMBOLS_PER_SLOT);
        nr_isac_submit_cfr_multi(slot_idx, frac, NR_ISAC_SRC_PDSCH_DATA, &carrier, &isac_h[2 * sym_start[g_first]],
                                 isac_nof_ant, isac_max_re, &isac_k[sym_start[g_first]], &isac_l[sym_start[g_first]],
                                 g_re, (float)nvar);
      }
    }
  }
}
