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
#include "nr_pdsch_da_snap.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.h" // nr_bgq_prepare / nr_bgq_start
#include "common/utils/nr_cell.h"

/* Set by a DEFERRED caller to this job's monotonic absolute slot; 0 = derive from proc. */
__thread uint64_t nr_isac_abs_slot_override = 0;

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"

#include "PHY/CODING/coding_defs.h"
#include "PHY/NR_REFSIG/dmrs_nr.h" // get_num_dmrs_re_per_rb, nr_chest_time_domain_avg
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/MODULATION/nr_modulation.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/NR_UE_ISAC/nr_isac.h"
#include "nr_pdsch_mimo_ls.h"
#include "nr_sense_api.h"
#include "PHY/NR_UE_TRANSPORT/nr_sense_records.h"
#include "executables/nr-uesoftmodem.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"

static _Atomic uint64_t g_data_aided_g_refused = 0;
/* Large per-thread scratch lives on the heap, not in static TLS: every thread's static TLS is carved out of its
 * 8 MB stack, and 5.6 MB of it left the SYNC actor too little stack for the fast re-sync (OTA SIGSEGV in
 * nr_pbch_detection, 2026-10-11). Allocated once per thread on first use, kept for the thread's lifetime. */
static void *da_tls_buf(void **p, size_t n)
{
  if (!*p) *p = aligned_alloc(64, (n + 63) / 64 * 64);
  return *p;
}

/* Submit the rows [off, off+cnt) of the estimate: one frame at Nl = 1 (exactly the original call), one frame per layer at Nl > 1.
 * Layer l's plane block starts nant*stride complex entries after layer l-1's; its frame carries the single DM-RS port the layer
 * is transmitted on (the l-th lowest set bit of dmrs_ports, TS 38.212 7.3.1.1/7.3.1.2 order) plus layer/n_layers. */
static void da_emit(uint32_t slot_idx, float frac, const nr_isac_carrier_t *carrier, const nr_sense_rs_meta_t *rs_meta, uint16_t dmrs_ports,
                    uint32_t nl, const float *h, uint32_t nant, uint32_t stride, const uint32_t *k, const uint32_t *l, uint32_t off,
                    uint32_t cnt, float nvar)
{
  if (nl <= 1) {
    nr_isac_submit_cfr_multi_meta(slot_idx, frac, NR_ISAC_SRC_PDSCH_DATA, carrier, &h[2 * (size_t)off], nant, stride, &k[off], &l[off],
                                  cnt, nvar, rs_meta);
    return;
  }
  uint32_t ports = dmrs_ports;
  const bool port_known = (uint32_t)__builtin_popcount(ports) == nl;
  for (uint32_t lay = 0; lay < nl; lay++) {
    nr_sense_rs_meta_t m = *rs_meta;
    m.ports = 1;
    m.port_mask = port_known ? (int)(ports & -ports) : -1;
    if (port_known) ports &= ports - 1;
    m.layer_idx1 = (int)lay + 1;
    m.n_layers = (int)nl;
    nr_isac_submit_cfr_multi_meta(slot_idx, frac, NR_ISAC_SRC_PDSCH_DATA, carrier, &h[2 * ((size_t)lay * nant * stride + off)], nant,
                                  stride, &k[off], &l[off], cnt, nvar, &m);
  }
}

uint64_t nr_isac_pdsch_data_aided_g_refused(void)
{
  return atomic_load_explicit(&g_data_aided_g_refused, memory_order_relaxed);
}

/* the G of this tap's RE model (no SSB / CSI-RS / PT-RS hole) */
static uint32_t da_tap_G(const fapi_nr_dl_cw_info_t *cw, const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config, const freq_alloc_bitmap_t *freq_alloc)
{
  const uint8_t nb_re_dmrs = get_num_dmrs_re_per_rb(dlsch_config->dmrsConfigType, dlsch_config->n_dmrs_cdm_groups);
  const uint16_t dmrs_len = get_num_dmrs(dlsch_config->dlDmrsSymbPos);
  return nr_get_G(freq_alloc->num_rbs, dlsch_config->number_symbols, nb_re_dmrs, dmrs_len, 0, cw->qamModOrder, cw->Nl);
}

void nr_isac_pdsch_data_aided_submit(PHY_VARS_NR_UE *ue,
                                     const UE_nr_rxtx_proc_t *proc,
                                     const fapi_nr_dl_cw_info_t *cw,
                                     const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                     const freq_alloc_bitmap_t *freq_alloc,
                                     uint16_t rnti,
                                     const uint8_t *tb_bytes,
                                     uint32_t harq_pid_tag,
                                     const c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                     double nvar,
                                     uint32_t decode_G,
                                     float dmrs_snr_db)
{
  /* RE-MODEL GUARD. The reconstruction below maps the re-encoded TB onto EVERY non-DM-RS RE of the
   * allocation: its RE model has no hole for an SSB, a CSI-RS or a PT-RS. A decode that rate-matched
   * around any of them (G smaller than this tap's G) verified a TB whose E per code block, and hence
   * whose X, differs from what is re-encoded here -- and the enumeration is still self-consistent with
   * its own G, so the mod_idx invariant further down cannot catch it. Checked FIRST, before the
   * sensing gate, so the refusal is independent of the sensing configuration. */
  const uint32_t tap_G = da_tap_G(cw, dlsch_config, freq_alloc);
  if (decode_G != tap_G) {
    const uint64_t n = atomic_fetch_add_explicit(&g_data_aided_g_refused, 1, memory_order_relaxed) + 1;
    if (n == 1 || n % 200 == 0)
      LOG_W(NR_PHY,
            "SENSING: data-aided tap refused n=%lu rnti=0x%x: decode G %u != tap G %u (SSB/CSI-RS/PT-RS REs rate-matched "
            "around; the tap's RE model has no hole for them)\n",
            (unsigned long)n, rnti, decode_G, tap_G);
    return;
  }
  if (!nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DATA))
    return;

  /* MULTI-LAYER (Nl = 2..4): Ĥ[k] = Y[k]/X[k] is a SCALAR division and is only valid when one modulation symbol is transmitted
   * per RE. At Nl > 1 the receive sample is a superposition, y_a[k] = sum_l h_{a,l}[k] * x_l[k]; dividing by one layer's x_l
   * leaves the other layers in the "channel estimate" -- a CFR that is not a channel at all. The per-layer h_{a,l} is recovered by
   * a windowed least-squares solve over W adjacent data REs of one symbol (nr_pdsch_mimo_ls.h), one CFR frame per layer. Nl = 1
   * keeps the scalar path below, unchanged. Nl > 4 (two codewords) is not reconstructed: contributing nothing is always preferable
   * to corrupting the range profile. */
  if (cw->Nl < 1 || cw->Nl > NR_MIMO_LS_MAX_NL) {
    static __thread bool warned_ml = false;
    if (!warned_ml) {
      warned_ml = true;
      LOG_W(NR_PHY, "SENSING: data-aided CFR skipped -- grant has Nl=%u layers (only 1..%d are reconstructed)\n", cw->Nl,
            NR_MIMO_LS_MAX_NL);
    }
    return;
  }
  const uint32_t nl = cw->Nl;
  nr_sense_prod_scope_t prod __attribute__((cleanup(nr_sense_prod_scope_stop))) =
      {NR_SENSE_SRC_PDSCH_DATA,nr_sense_now_ns()};

  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;

  // --- TB CRC-included payload: tb_bytes is ALREADY B = A + TB-CRC bits (16 or 24-bit, matching
  // NR_MAX_PDSCH_TBS threshold) -- the exact same buffer format the TX-side encoder segments, so no
  // CRC attach is needed here; the decoder already reconstructed it. ---
  const uint32_t A = cw->TBS;
  const unsigned int B = A + (A > NR_MAX_PDSCH_TBS ? 24 : 16);

  // --- Segment into code blocks (must run again here to fill actual per-segment bytes: the decode
  // path only sized C/K/Z/F, passing NULL/NULL for input/output). ---
  static __thread uint8_t (*seg_storage)[8448];
  if (!da_tls_buf((void **)&seg_storage, sizeof(*seg_storage) * MAX_NUM_NR_DLSCH_SEGMENTS)) return;
  static __thread uint8_t *c_segs[MAX_NUM_NR_DLSCH_SEGMENTS];
  for (int r = 0; r < MAX_NUM_NR_DLSCH_SEGMENTS; r++)
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
  if (TB_parameters.C > MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER * nl) {
    LOG_W(NR_PHY, "SENSING: data-aided tap skipped -- too many segments C=%u\n", TB_parameters.C);
    return;
  }

  TB_parameters.nb_rb = freq_alloc->num_rbs;
  TB_parameters.Qm = cw->qamModOrder;
  TB_parameters.mcs = cw->mcs;
  TB_parameters.nb_layers = cw->Nl;
  TB_parameters.rv_index = cw->rv;
  TB_parameters.tbslbrm = dlsch_config->tbslbrm;

  TB_parameters.G = tap_G; // == the decode's G: checked on entry
  if (TB_parameters.G == 0)
    return;

  // 32-byte aligned: nr_modulation() / the LDPC encoder store through AVX2 intrinsics that fault on a
  // misaligned buffer. These are thread-local, so their addresses depend on the TLS block layout --
  // i.e. on every other __thread object in this file. That made the alignment ACCIDENTAL: adding the
  // sub-slot bookkeeping arrays below shifted the layout and produced an immediate GP fault inside
  // nr_modulation's `out128[i] = ...` store. Pin it explicitly so the layout can never break it again.
  static __thread uint8_t *coded_bits;
  const size_t coded_sz = (273 * 12 * 14 * 8 * NR_MIMO_LS_MAX_NL + 63) / 64 * 64 + 64; // G = REs * Qm * Nl
  if (!da_tls_buf((void **)&coded_bits, coded_sz)) return;
  memset(coded_bits, 0, coded_sz);
  TB_parameters.output = coded_bits;

  static __thread nrLDPC_segment_encoding_parameters_t segments[MAX_NUM_NR_DLSCH_SEGMENTS];
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
  static __thread uint32_t *scrambled; // heap, 64-byte aligned: G reaches REs * Qm * Nl
  if (!da_tls_buf((void **)&scrambled, sizeof(uint32_t) * ((273 * 12 * 14 * 8 * NR_MIMO_LS_MAX_NL + 31) / 32 + 1))) return;
  nr_codeword_scrambling(coded_bits, TB_parameters.G, 0 /* codeword index */, dlsch_config->dlDataScramblingId,
                        rnti, scrambled);

  static __thread c16_t *mod_syms;
  if (!da_tls_buf((void **)&mod_syms, sizeof(c16_t) * 273 * 12 * 14 * NR_MIMO_LS_MAX_NL)) return;
  nr_modulation(scrambled, TB_parameters.G, cw->qamModOrder, (int16_t *)mod_syms);

  // --- RE mapping: sweep the allocation symbol by symbol, increasing frequency within each, taking
  // every RE the transmitter mapped a data symbol to -- the same order (and, for DM-RS symbols, the
  // same per-RE bitmap) nr_dlsch_extract_rbs() uses, mirrored here in the forward
  // TX-reconstruction direction. Ĥ[k] = Y[k]/X[k] at every such RE. --
  const uint32_t base_sc  = nr_sense_crb_re(dlsch_config->BWPStart + freq_alloc->first_rb, 0);
  const int      num_sc   = (freq_alloc->n_prb_list ? freq_alloc->n_prb_list : freq_alloc->num_rbs) * NR_NB_SC_PER_RB;
  const int      start_re = (fp->first_carrier_offset + (dlsch_config->BWPStart + freq_alloc->first_rb) * NR_NB_SC_PER_RB)
                           % fp->ofdm_symbol_size;

  // Receive-array AoA (PHASE3_AOA_MULTISTATIC_HANDOVER 5.4): extract Ĥ = Y/X for every rx antenna,
  // not just antenna 0. X is the SAME reconstructed transport block for all of them, so this is a
  // pure inner loop over rxdataF[a] -- the per-element phase difference it captures IS the bearing.
  const uint32_t isac_max_re = 273 * 12 * 14;
  uint32_t       isac_nof_ant = (uint32_t)fp->nb_antennas_rx;
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
  /* Nl > 1 scratch (heap, lazily): the gathered Y planes the LS solve reads and the per-window output coordinates. At Nl > 1 the
   * output has one row per window of >= 2*Nl >= 4 REs, so isac_max_re / 4 rows per plane hold every window, and Nl*nant such planes
   * fit the isac_h allocation made for the Nl = 1 layout. */
  const uint32_t win_cap = isac_max_re / 4;
  static __thread c16_t *ygath;
  static __thread uint32_t *win_k;
  if (nl > 1) {
    if (!da_tls_buf((void **)&ygath, sizeof(c16_t) * NR_MIMO_LS_MAX_ANT * isac_max_re) ||
        !da_tls_buf((void **)&win_k, sizeof(uint32_t) * 2 * win_cap + sizeof(uint16_t) * win_cap))
      return;
  }
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
      uint32_t k_abs = base_sc + (uint32_t)j;
      if (freq_alloc->n_prb_list) { // non-contiguous grant: data RB j/12 sits on PRB prb_list[j/12]
        k_abs = nr_sense_crb_re(dlsch_config->BWPStart + freq_alloc->prb_list[j / NR_NB_SC_PER_RB],
                                j % NR_NB_SC_PER_RB);
        re = (int)((fp->first_carrier_offset + k_abs) % fp->ofdm_symbol_size);
      }
      if (nl > 1) {
        /* RE ordinal nof_re carries layer symbols mod_syms[nof_re*nl .. +nl) (TS 38.211 7.3.1.3: x_l(i) = d(nl*i + l)); no |X| guard:
         * the LS window judges the conditioning of the whole X. */
        mod_idx += nl;
        for (uint32_t a = 0; a < isac_nof_ant; a++) {
          const c16_t y = rxdataF[a][l * fp->ofdm_symbol_size + re];
          ygath[(size_t)a * isac_max_re + nof_re] = y;
          if (a == 0)
            ypow += (double)y.r * y.r + (double)y.i * y.i;
        }
        isac_k[nof_re] = k_abs;
        isac_l[nof_re] = (uint32_t)l;
        nof_re++;
        continue;
      }
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
      isac_k[nof_re]         = k_abs;
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

  nr_sense_prod_scope_stop(&prod);
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
  if (mod_idx != expected_syms * nl) { // mod_idx counts modulation symbols: nl per RE
    static __thread bool warned = false;
    if (!warned) {
      warned = true;
      LOG_W(NR_PHY,
            "SENSING: data-aided RE enumeration consumed %u modulation symbols but rate matching produced %u "
            "(nb_rb=%u nb_symb=%u dmrs_mask=0x%x cdm_groups=%u type=%u) -- the reconstructed X is MISALIGNED and "
            "every submitted CFR from this grant shape is invalid\n",
            mod_idx / nl, expected_syms, freq_alloc->num_rbs, dlsch_config->number_symbols, dlsch_config->dlDmrsSymbPos,
            dlsch_config->n_dmrs_cdm_groups, dlsch_config->dmrsConfigType);
    }
    return; // contribute nothing rather than corrupt the range profile
  }

  /* Estimate/export arrays the sub-slot tail below slices. Nl = 1: the Y/X rows just built. Nl > 1: one row per LS window
   * (planes [layer][antenna] of win_cap rows). sym_raw keeps the RE count per symbol for the SNR / coverage gates, which judge
   * the data actually pooled, whatever the number of rows it became. */
  const float    *e_h = isac_h;
  const uint32_t *e_k = isac_k, *e_l = isac_l;
  uint32_t        e_stride = isac_max_re, e_nre = nof_re;
  uint32_t        sym_raw[NR_SYMBOLS_PER_SLOT];
  for (uint32_t i = 0; i < nof_sym; i++)
    sym_raw[i] = sym_count[i];
  if (nl > 1) {
    uint32_t *win_l = win_k + win_cap;
    uint16_t *win_raw = (uint16_t *)(win_l + win_cap);
    nr_pdsch_mimo_ls_info_t li;
    const int nw = nr_pdsch_mimo_ls_cfr((int)nof_re, (int)nl, (int)isac_nof_ant, (const int16_t *)mod_syms, isac_k, isac_l,
                                        (const int16_t *)ygath, isac_max_re, (int)win_cap, win_cap, isac_h, win_k, win_l, win_raw, &li);
    if (nw <= 0) {
      static __thread bool warned_ls = false;
      if (!warned_ls) {
        warned_ls = true;
        LOG_W(NR_PHY, "SENSING: data-aided multi-layer CFR: no estimable LS window (Nl=%u, %u data REs)\n", nl, nof_re);
      }
      return;
    }
    e_h = isac_h;
    e_k = win_k;
    e_l = win_l;
    e_stride = win_cap;
    e_nre = (uint32_t)nw;
    uint32_t w = 0;
    for (uint32_t i = 0; i < nof_sym; i++) {
      sym_start[i] = w;
      while (w < e_nre && win_l[w] == sym_id[i])
        w++;
      sym_count[i] = w - sym_start[i];
    }
  }

  nr_isac_carrier_t carrier = {.nof_prb         = (uint32_t)fp->N_RB_DL,
                               .scs_hz          = fp->subcarrier_spacing,
                               .dl_center_hz    = fp->dl_CarrierFreq,
                               .pci             = fp->Nid_cell,
                               .slots_per_frame = fp->slots_per_frame};
  nr_sense_rs_meta_t rs_meta = {.rnti=rnti,
    .start_rb=dlsch_config->BWPStart+freq_alloc->first_rb, .nb_rb=freq_alloc->num_rbs,
    .ports=__builtin_popcount((unsigned)dlsch_config->dmrs_ports), .port_mask=(int)dlsch_config->dmrs_ports, .ssb_index=-1, .k_ssb=-1,
    .ta_est_samples=-1, .transform_precoding=-1,
    .dmrs_type=dlsch_config->dmrsConfigType == NFAPI_NR_DMRS_TYPE2 ? 2 : 1, .dmrs_add_pos=-1, .dmrs_n_scid=dlsch_config->nscid};
  nr_sense_meta_set_snr(&rs_meta, dmrs_snr_db);
  /* the demodulator's noise estimate in the units of the exported |Y/X|^2; 0 = none measured (prints null) */
  const float nvar_f = nvar > 0.0 ? (float)nvar : NAN;
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

  // --- Sub-slot sampling: split this slot's symbols into GROUPS and submit each
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
    da_emit(slot_idx, 0.0f, &carrier, &rs_meta, dlsch_config->dmrs_ports, nl, e_h, isac_nof_ant, e_stride, e_k, e_l, 0, e_nre,
            nvar_f);
    return;
  }

  const double nv = (nvar > 0.0) ? nvar : 1.0;
  uint32_t g_first = 0; // first symbol index of the group being assembled
  uint32_t emitted = 0;
  for (uint32_t i = 0; i < nof_sym; i++) {
    const uint32_t nsym = i - g_first + 1;
    uint32_t g_re = 0, g_out = 0; // g_re: data REs pooled (gates); g_out: rows to submit
    double   g_yp = 0.0;
    for (uint32_t t = g_first; t <= i; t++) {
      g_re += sym_raw[t];
      g_out += sym_count[t];
      g_yp += sym_ypow[t];
    }
    // Estimated coherent SNR of this group, in dB.
    const double per_re = (g_re > 0) ? ((g_yp / (double)g_re) - nv) / nv : -1.0;
    const double snr_db = (per_re > 0.0) ? 10.0 * log10(per_re * (double)g_re) : -99.0;
    const bool   gates_ok = (g_re >= sub_min_re) && (snr_db >= (double)sub_min_snr_db) && g_out > 0;
    const bool   is_last  = (i + 1 == nof_sym);

    if ((nsym >= sub_target && gates_ok) || (is_last && gates_ok)) {
      // Place the row at the group's centre symbol, in slots within this slot.
      const double centre = 0.5 * ((double)sym_id[g_first] + (double)sym_id[i]) + 0.5;
      const float  frac   = (float)(centre / (double)NR_SYMBOLS_PER_SLOT);
      // Slice, not copy: ant_stride_re stays the FULL buffer stride so antenna a's slice starts at
      // the same symbol offset within its own plane.
      da_emit(slot_idx, frac, &carrier, &rs_meta, dlsch_config->dmrs_ports, nl, e_h, isac_nof_ant, e_stride, e_k, e_l,
              sym_start[g_first], g_out, nvar_f);
      emitted++;
      g_first = i + 1;
    } else if (is_last) {
      // Tail that never passed the gates: merge it BACKWARDS by re-emitting from g_first to the end
      // as one row if nothing has been emitted yet, otherwise fold it into the whole-slot fallback.
      if (emitted == 0) {
        da_emit(slot_idx, 0.0f, &carrier, &rs_meta, dlsch_config->dmrs_ports, nl, e_h, isac_nof_ant, e_stride, e_k, e_l, 0, e_nre,
                nvar_f);
        emitted++;
      } else {
        const double centre = 0.5 * ((double)sym_id[g_first] + (double)sym_id[i]) + 0.5;
        const float  frac   = (float)(centre / (double)NR_SYMBOLS_PER_SLOT);
        if (g_out > 0)
          da_emit(slot_idx, frac, &carrier, &rs_meta, dlsch_config->dmrs_ports, nl, e_h, isac_nof_ant, e_stride, e_k, e_l,
                  sym_start[g_first], g_out, nvar_f);
      }
    }
  }
}

/* ---- the tap off the decode thread (nr_isac_pdsch_data_aided_submit_async) ---- */
#define DA_POOL 4
#define DA_TB_CAP 65536 /* bytes: one layer, 273 PRB x 12 x 14 REs x 8 bits / 8 = 45.9 kB + CRC */
typedef struct {
  PHY_VARS_NR_UE *ue;
  UE_nr_rxtx_proc_t proc;
  fapi_nr_dl_cw_info_t cw;
  fapi_nr_dl_config_dlsch_pdu_rel15_t dlsch;
  freq_alloc_bitmap_t fa;
  uint16_t rnti;
  uint32_t harq_tag, G;
  double nvar;
  float dmrs_snr_db;
  uint64_t abs_slot;
  c16_t *rx; /* nant x stride c16, the layout of the consumer's rxdataF; only the grant's region is written */
  uint8_t *tb;
  int idx;
} da_job_t;
typedef struct {
  da_job_t *job[DA_POOL];
  unsigned free_mask; /* bit i = job i is free */
  uint32_t stride;    /* samples_per_slot_wCP */
  int nant;
  _Atomic int ready;
  _Atomic uint64_t queued, inl, done;
  _Atomic uint64_t run_ns, snap_ns; /* worker time of the tap / consumer time of the snapshot of the queued grants: saved = run - snap */
} da_pool_t;
/* mpci: the pool, its queue and its worker are the cell's (the consumer calling _async is bound to it); the worker binds to the cell of each item (nr_gpu_bgq.h) */
NR_CELL_STATIC(da_pool_t, g_da)
#define g_da NR_CELL_REF(g_da)
NR_CELL_STATIC(nr_bgq_t, g_da_q)
#define g_da_q NR_CELL_REF(g_da_q)

void nr_isac_pdsch_data_aided_async_prepare(int cell)
{
  if (!nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DATA))
    return;
  (void)nr_bgq_prepare(&g_da_q_cells[cell], DA_POOL, "daBg", cell);
}

void nr_isac_pdsch_data_aided_async_init(const PHY_VARS_NR_UE *ue)
{
  da_pool_t *P = &g_da;
  if (atomic_load(&P->ready) || !nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DATA))
    return;
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  P->stride = (uint32_t)fp->samples_per_slot_wCP;
  P->nant = fp->nb_antennas_rx;
  const size_t rx_b = (size_t)P->nant * P->stride * sizeof(c16_t);
  for (int i = 0; i < DA_POOL; i++) {
    da_job_t *j = calloc(1, sizeof(*j));
    if (j) {
      j->rx = aligned_alloc(64, (rx_b + 63) / 64 * 64);
      j->tb = aligned_alloc(64, DA_TB_CAP);
    }
    if (!j || !j->rx || !j->tb) {
      if (j) { free(j->rx); free(j->tb); free(j); }
      break;
    }
    memset(j->rx, 0, rx_b); /* prefault now, not on the decode path */
    memset(j->tb, 0, DA_TB_CAP);
    j->idx = i;
    P->job[i] = j;
    P->free_mask |= 1u << i;
  }
  if (!P->free_mask)
    return;
  if (nr_bgq_start(&g_da_q, DA_POOL, "daBg") != 0) {
    P->free_mask = 0; /* no worker: every submit stays inline */
    return;
  }
  atomic_store(&P->ready, 1);
  LOG_A(NR_PHY, "SENSING: data-aided tap off the decode thread: %u jobs, worker daBg\n", (unsigned)__builtin_popcount(P->free_mask));
}

void nr_isac_pdsch_data_aided_async_counts(uint64_t *queued, uint64_t *inl, uint64_t *done, uint64_t *saved_us)
{
  *queued = atomic_load(&g_da.queued);
  *inl = atomic_load(&g_da.inl);
  *done = atomic_load(&g_da.done);
  const uint64_t r = atomic_load(&g_da.run_ns), c = atomic_load(&g_da.snap_ns);
  *saved_us = r > c ? (r - c) / 1000 : 0;
}

static uint64_t da_now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void da_run(void *arg)
{
  da_job_t *j = arg;
  da_pool_t *P = &g_da; /* the worker was bound to the item's cell by nr_bgq_run_one */
  const uint64_t t0 = da_now_ns();
  nr_isac_abs_slot_override = j->abs_slot;
  nr_isac_pdsch_data_aided_submit(j->ue, &j->proc, &j->cw, &j->dlsch, &j->fa, j->rnti, j->tb, j->harq_tag, (const c16_t(*)[P->stride])j->rx, j->nvar, j->G,
                                  j->dmrs_snr_db);
  nr_isac_abs_slot_override = 0;
  atomic_fetch_add(&P->run_ns, da_now_ns() - t0);
  const uint64_t n = atomic_fetch_add(&P->done, 1) + 1;
  if (n == 1 || n % 2000 == 0) {
    uint64_t q, i, d, sv;
    nr_isac_pdsch_data_aided_async_counts(&q, &i, &d, &sv);
    LOG_A(NR_PHY, "SENSING: DATA_AIDED_ASYNC done=%lu queued=%lu inline=%lu consumer_us_saved=%lu\n", (unsigned long)d, (unsigned long)q, (unsigned long)i, (unsigned long)sv);
  }
  __atomic_fetch_or(&P->free_mask, 1u << j->idx, __ATOMIC_RELEASE);
}

int nr_isac_pdsch_data_aided_submit_async(PHY_VARS_NR_UE *ue,
                                          const UE_nr_rxtx_proc_t *proc,
                                          const fapi_nr_dl_cw_info_t *cw,
                                          const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                          const freq_alloc_bitmap_t *freq_alloc,
                                          uint16_t rnti,
                                          const uint8_t *tb_bytes,
                                          uint32_t harq_pid_tag,
                                          const c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                          double nvar,
                                          uint32_t decode_G,
                                          float dmrs_snr_db)
{
  da_pool_t *P = &g_da;
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const uint32_t A = cw->TBS;
  const size_t tb_n = ((size_t)A + (A > NR_MAX_PDSCH_TBS ? 24 : 16) + 7) / 8;
  /* eligibility = what the inline tap would do beyond its early returns: a single layer, the tap's own G, an enabled source (those early returns stay in submit and run inline) */
  int ok = atomic_load_explicit(&P->ready, memory_order_acquire) && cw->Nl == 1 && freq_alloc->n_prb_list == 0 && tb_n <= DA_TB_CAP && decode_G == da_tap_G(cw, dlsch_config, freq_alloc) &&
           (int)P->nant == fp->nb_antennas_rx && P->stride == (uint32_t)fp->samples_per_slot_wCP && nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DATA) && nr_bgq_room(&g_da_q);
  da_job_t *j = NULL;
  if (ok) {
    unsigned m = __atomic_load_n(&P->free_mask, __ATOMIC_ACQUIRE);
    while (m && !j)
      if (__atomic_compare_exchange_n(&P->free_mask, &m, m & (m - 1u), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        j = P->job[__builtin_ctz(m)];
    ok = j != NULL;
  }
  const uint64_t t_snap0 = ok ? da_now_ns() : 0;
  if (ok) {
    const int start_re = (fp->first_carrier_offset + (dlsch_config->BWPStart + freq_alloc->first_rb) * NR_NB_SC_PER_RB) % fp->ofdm_symbol_size;
    const int num_sc = freq_alloc->num_rbs * NR_NB_SC_PER_RB;
    ok = nr_da_snap_copy((uint32_t *)j->rx, (const uint32_t *)&rxdataF[0][0], P->nant, P->stride, fp->ofdm_symbol_size, dlsch_config->start_symbol, dlsch_config->number_symbols, start_re,
                         num_sc) == 0;
  }
  if (ok) {
    j->ue = ue;
    j->proc = *proc;
    j->cw = *cw;
    j->dlsch = *dlsch_config;
    j->fa = *freq_alloc;
    j->rnti = rnti;
    j->harq_tag = harq_pid_tag;
    j->G = decode_G;
    j->nvar = nvar;
    j->dmrs_snr_db = dmrs_snr_db;
    j->abs_slot = nr_isac_abs_slot_override;
    memcpy(j->tb, tb_bytes, tb_n);
    if (nr_bgq_push(&g_da_q, da_run, j) == 0) {
      atomic_fetch_add(&P->snap_ns, da_now_ns() - t_snap0);
      atomic_fetch_add(&P->queued, 1);
      return 1;
    }
  }
  if (j)
    __atomic_fetch_or(&P->free_mask, 1u << j->idx, __ATOMIC_RELEASE);
  atomic_fetch_add(&P->inl, 1);
  nr_isac_pdsch_data_aided_submit(ue, proc, cw, dlsch_config, freq_alloc, rnti, tb_bytes, harq_pid_tag, rxdataF, nvar, decode_G, dmrs_snr_db);
  return 0;
}
