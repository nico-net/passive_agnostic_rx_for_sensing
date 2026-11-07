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
#include "nr_pdsch_da_recon.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.h" // nr_bgq_prepare / nr_bgq_start
#include "common/utils/nr_cell.h"

/* Set by a DEFERRED caller to this job's monotonic absolute slot; 0 = derive from proc. */
__thread uint64_t nr_isac_abs_slot_override = 0;
__thread const nr_da_cb_pass_t *nr_isac_da_cb_pass = NULL;
_Static_assert(NR_DA_CB_MAX >= MAX_NUM_NR_DLSCH_SEGMENTS, "the per-block pass bitmap must hold every code block of a TB");

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
#include "nr_sense_unit.h"
#include "PHY/NR_UE_TRANSPORT/nr_sense_noise_est.h"
#include "PHY/NR_UE_TRANSPORT/nr_sense_records.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.h" // nr_pdsch_prg_latched
#include "executables/nr-uesoftmodem.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"

static _Atomic uint64_t g_data_aided_g_refused = 0;
/* tpool with no threads: pushTpool runs a task on the pushing thread. The re-encode on the daBg worker uses it so the LDPC segments are encoded by the worker itself, not
 * queued on (and joined from) the shared decode Tpool. Read-only. */
static tpool_t da_inline_pool;
static __thread bool da_on_worker;
/* Large per-thread scratch lives on the heap, not in static TLS: every thread's static TLS is carved out of its
 * 8 MB stack, and 5.6 MB of it left the SYNC actor too little stack for the fast re-sync (OTA SIGSEGV in
 * nr_pbch_detection, 2026-10-11). Allocated once per thread on first use, kept for the thread's lifetime. */
static void *da_tls_buf(void **p, size_t n)
{
  if (!*p) *p = aligned_alloc(64, (n + 63) / 64 * 64);
  return *p;
}

/* The per-thread scratch of the tap (heap, 64-byte aligned). File-scope so the daBg worker can build and prefault it before RX start
 * (da_scratch_ensure(.., warm = true)): the first grant on a thread then allocates nothing. */
static __thread uint8_t (*seg_storage)[8448];
static __thread uint8_t *c_segs[MAX_NUM_NR_DLSCH_SEGMENTS];
static __thread uint8_t *coded_bits; /* 32-byte aligned: nr_modulation() / the LDPC encoder store through AVX2 intrinsics (see the note at its use) */
static __thread uint32_t *scrambled;
static __thread uint8_t *re_valid; /* partial TB: one flag per RE ordinal of the grant (DA_MAX_RE) */
static __thread c16_t *mod_syms;
static __thread float *isac_h;
static __thread uint32_t isac_h_nant;
static __thread c16_t *ygath;
static __thread nrLDPC_segment_encoding_parameters_t segments[MAX_NUM_NR_DLSCH_SEGMENTS];
static __thread uint32_t isac_k[273 * 12 * 14];
static __thread uint32_t isac_l[273 * 12 * 14];
static __thread uint32_t *win_k;
#define DA_MAX_RE (273 * 12 * 14)
#define DA_WIN_NV_OFF(cap) ((sizeof(uint32_t) * 2 * (cap) + sizeof(uint16_t) * (cap) + 7) / 8 * 8) /* byte offset of the per-window noise planes behind win_k/win_l/win_raw */
#define DA_WIN_BYTES(cap) (DA_WIN_NV_OFF(cap) + sizeof(float) * NR_MIMO_LS_MAX_NL * (cap))
#define DA_CODED_SZ ((size_t)(273 * 12 * 14 * 8 * NR_MIMO_LS_MAX_NL + 63) / 64 * 64 + 64) /* G = REs * Qm * Nl */

static bool da_scratch_ensure(uint32_t nant, bool want_ml, bool warm)
{
  if (!da_tls_buf((void **)&seg_storage, sizeof(*seg_storage) * MAX_NUM_NR_DLSCH_SEGMENTS) || !da_tls_buf((void **)&coded_bits, DA_CODED_SZ) ||
      !da_tls_buf((void **)&scrambled, sizeof(uint32_t) * ((273 * 12 * 14 * 8 * NR_MIMO_LS_MAX_NL + 31) / 32 + 1)) ||
      !da_tls_buf((void **)&mod_syms, sizeof(c16_t) * 273 * 12 * 14 * NR_MIMO_LS_MAX_NL) || !da_tls_buf((void **)&re_valid, DA_MAX_RE))
    return false;
  if (nant == 0)
    nant = 1;
  if (isac_h == NULL || isac_h_nant < nant) {
    free(isac_h);
    isac_h = (float *)aligned_alloc(32, (size_t)nant * 2 * DA_MAX_RE * sizeof(float));
    if (isac_h == NULL) {
      isac_h_nant = 0;
      LOG_W(NR_PHY, "SENSING: data-aided tap skipped -- could not allocate the per-antenna CFR buffer\n");
      return false;
    }
    isac_h_nant = nant;
  }
  const uint32_t win_cap = DA_MAX_RE / 4;
  if (want_ml &&
      (!da_tls_buf((void **)&ygath, sizeof(c16_t) * NR_MIMO_LS_MAX_ANT * DA_MAX_RE) || !da_tls_buf((void **)&win_k, DA_WIN_BYTES(win_cap))))
    return false;
  if (warm) { /* touch every page now, not on the first grant */
    memset(segments, 0, sizeof(segments));
    memset(isac_k, 0, sizeof(isac_k));
    memset(isac_l, 0, sizeof(isac_l));
    memset(seg_storage, 0, sizeof(*seg_storage) * MAX_NUM_NR_DLSCH_SEGMENTS);
    memset(coded_bits, 0, DA_CODED_SZ);
    memset(scrambled, 0, sizeof(uint32_t) * ((273 * 12 * 14 * 8 * NR_MIMO_LS_MAX_NL + 31) / 32 + 1));
    memset(mod_syms, 0, sizeof(c16_t) * 273 * 12 * 14 * NR_MIMO_LS_MAX_NL);
    memset(isac_h, 0, (size_t)isac_h_nant * 2 * DA_MAX_RE * sizeof(float));
    if (want_ml) {
      memset(ygath, 0, sizeof(c16_t) * NR_MIMO_LS_MAX_ANT * DA_MAX_RE);
      memset(win_k, 0, DA_WIN_BYTES(win_cap));
    }
  }
  return true;
}

/* Submit the rows [off, off+cnt) of the estimate: one frame at Nl = 1 (exactly the original call), one frame per layer at Nl > 1.
 * Layer l's plane block starts nant*stride complex entries after layer l-1's; its frame carries the single DM-RS port the layer
 * is transmitted on (the l-th lowest set bit of dmrs_ports, TS 38.212 7.3.1.1/7.3.1.2 order) plus layer/n_layers. */
static void da_emit(uint32_t slot_idx, float frac, const nr_isac_carrier_t *carrier, const nr_sense_rs_meta_t *rs_meta, uint16_t dmrs_ports,
                    uint32_t nl, const float *h, uint32_t nant, uint32_t stride, const uint32_t *k, const uint32_t *l, uint32_t off,
                    uint32_t cnt, float nvar, const float *wnv)
{
  /* The demodulator's nvar is in rxdataF units; H = Y/X is not (|X| is the constellation scale, ~16 at QPSK), so the noise handed to the quality is the
   * one measured on H itself (second difference over consecutive subcarriers, as the PUSCH data tap). NaN/0 = none: the quality uses the record's residual. */
  if (nl <= 1) {
    if (nr_isac_mode() == NR_ISAC_MODE_SENSING) /* legacy/traffic records keep the demodulator's figure */
      nvar = nr_sense_h_noise_var(&h[2 * (size_t)off], &k[off], &l[off], cnt, stride, nant);
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
    const float *hl = &h[2 * ((size_t)lay * nant * stride + off)];
    /* LS-window rows: the layer's noise reference and the heterogeneity of the rows' variances come from the LS solve itself (sigma_y^2 (G^-1)_ll per window), not from H: the quality
     * tests the rows' power sum against Gamma(n_ant R eta), eta = (sum v)^2 / (R sum v^2) (nr_sense_quality.c) */
    float nv_layer = NAN;
    if (wnv && cnt > 0) {
      const float *v = &wnv[(size_t)lay * stride + off];
      double s1 = 0, s2 = 0;
      for (uint32_t i = 0; i < cnt; i++) s1 += v[i], s2 += (double)v[i] * v[i];
      if (s1 > 0 && s2 > 0) {
        nv_layer = (float)(s1 / cnt);
        m.win_het = (float)(s1 * s1 / ((double)cnt * s2));
      }
    }
    nr_isac_submit_cfr_multi_meta(slot_idx, frac, NR_ISAC_SRC_PDSCH_DATA, carrier, hl, nant, stride, &k[off], &l[off], cnt,
                                  nr_isac_mode() == NR_ISAC_MODE_SENSING ? (wnv ? nv_layer : nr_sense_h_noise_var(hl, &k[off], &l[off], cnt, stride, nant)) : nvar, &m);
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

static bool da_recon_backlogged(void);
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
  /* SENSING mode unit gate (spec 5.6): a disabled (RNTI, PDSCH_DATA) unit is skipped before any work and counted as a disabled skip. The daBg worker re-enters here with a
   * message that was admitted when the unit was enabled: it is finished, never re-gated. */
  if (!da_on_worker && !nr_sense_unit_gate_deferred(NR_UNIT_PDSCH_DATA, rnti, (uint16_t)freq_alloc->num_rbs)) /* called under g_phase2_mu by the monitor: the skip is booked after its unlock */
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
  const size_t tb_n = ((size_t)B + 7) / 8;

  // --- Segment into code blocks (must run again here to fill actual per-segment bytes: the decode
  // path only sized C/K/Z/F, passing NULL/NULL for input/output). ---
  if (!da_scratch_ensure((uint32_t)ue->frame_parms.nb_antennas_rx, nl > 1, false)) return;
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
  const size_t coded_sz = DA_CODED_SZ;
  memset(coded_bits, 0, coded_sz);
  TB_parameters.output = coded_bits;

  memset(segments, 0, sizeof(segments));
  TB_parameters.segments = segments;
  for (uint32_t r = 0; r < TB_parameters.C; r++) {
    segments[r].c = c_segs[r];
    segments[r].E = nr_get_E(TB_parameters.G, TB_parameters.C, TB_parameters.Qm, TB_parameters.nb_layers, r);
    reset_meas(&segments[r].ts_interleave);
    reset_meas(&segments[r].ts_rate_match);
    reset_meas(&segments[r].ts_ldpc_encode);
  }
  /* Partial TB: which code blocks passed (the decoder's per-block CRC24B, not the 64-bit EVMGRANT map), and from that which REs they pin down. A block whose payload is all zero is not counted:
   * an all-zero code block has a zero CRC24B, the same false pass the whole-TB path guards against. */
  nr_da_cb_pass_t part = {0};
  const bool part_on = nr_isac_da_cb_pass != NULL && nr_isac_mode() == NR_ISAC_MODE_SENSING;
  if (part_on) {
    part = *nr_isac_da_cb_pass;
    const uint32_t seg_bytes = (TB_parameters.K >> 3) - (TB_parameters.F >> 3) - 3;
    if (TB_parameters.C < 2 || part.C != TB_parameters.C)
      return; /* a one-block TB has no block CRC; a mismatched block count means the result is not this TB's */
    nr_da_cb_clear_zero(&part, tb_bytes, seg_bytes, seg_bytes, tb_n);
    const uint16_t ok = part.ok;

    if (!ok) return;
    uint32_t E[MAX_NUM_NR_DLSCH_SEGMENTS];
    for (uint32_t r = 0; r < TB_parameters.C; r++) E[r] = (uint32_t)segments[r].E;
    (void)nr_da_re_valid(E, &part, cw->qamModOrder, nl, TB_parameters.G / (cw->qamModOrder * nl), re_valid);
  }

  nrLDPC_slot_encoding_parameters_t slot_parameters = {.frame = proc->frame_rx,
                                                       .slot = proc->nr_slot_rx,
                                                       .nb_TBs = 1,
                                                       .threadPool = da_on_worker ? &da_inline_pool : &get_nrUE_params()->Tpool,
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
  nr_codeword_scrambling(coded_bits, TB_parameters.G, 0 /* codeword index */, dlsch_config->dlDataScramblingId,
                        rnti, scrambled);

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
  /* Nl > 1 scratch (heap, lazily): the gathered Y planes the LS solve reads and the per-window output coordinates. At Nl > 1 the
   * output has one row per window of >= 2*Nl >= 4 REs, so isac_max_re / 4 rows per plane hold every window, and Nl*nant such planes
   * fit the isac_h allocation made for the Nl = 1 layout. */
  const uint32_t win_cap = isac_max_re / 4;
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
        const uint32_t s0 = mod_idx;
        mod_idx += nl;
        if (!nr_da_re_take(re_valid, part_on, s0, nl, nof_re, (int16_t *)mod_syms)) continue; /* a failed or untried block carries this RE: invalid, not exported */
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
      const uint32_t s0 = mod_idx;
      const c16_t x = mod_syms[mod_idx++];
      if (!nr_da_re_take(re_valid, part_on, s0, 1, nof_re, (int16_t *)mod_syms)) continue; /* a failed or untried block carries this RE */
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
  const float    *e_nv = NULL; /* Nl > 1: per-window noise variance planes [layer][win_cap] (nr_pdsch_mimo_ls_cfr_nv) */
  uint32_t        sym_raw[NR_SYMBOLS_PER_SLOT];
  for (uint32_t i = 0; i < nof_sym; i++)
    sym_raw[i] = sym_count[i];
  if (nl > 1) {
    uint32_t *win_l = win_k + win_cap;
    uint16_t *win_raw = (uint16_t *)(win_l + win_cap);
    nr_pdsch_mimo_ls_info_t li;
    float *win_nv = (float *)((uint8_t *)win_k + DA_WIN_NV_OFF(win_cap));
    const int nw = nr_pdsch_mimo_ls_cfr_nv((int)nof_re, (int)nl, (int)isac_nof_ant, (const int16_t *)mod_syms, isac_k, isac_l,
                                           (const int16_t *)ygath, isac_max_re, (int)win_cap, win_cap, isac_h, win_k, win_l, win_raw, win_nv, &li);
    e_nv = win_nv;
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
    .dmrs_type=dlsch_config->dmrsConfigType == NFAPI_NR_DMRS_TYPE2 ? 2 : 1, .dmrs_add_pos=-1, .dmrs_n_scid=dlsch_config->nscid,
    .tb_cb_ok = part_on ? part.ok : 0, .tb_cb_total = part_on ? part.C : 0, .qm = (uint8_t)cw->qamModOrder};
  nr_sense_meta_set_snr(&rs_meta, dmrs_snr_db);
  if (nr_isac_mode() == NR_ISAC_MODE_SENSING) {
    const int prg = nr_pdsch_prg_latched(rnti);
    rs_meta.prg_valid = prg >= 0;
    rs_meta.prg_size = prg >= 0 ? prg : 0;
    if (nl == 1 && mod_idx == nof_re) { /* RE ordinal i <-> mod_syms[i] only when no RE was skipped */
      if (da_recon_backlogged()) { /* bound by backlog only (no RE cap: big grants are the most valuable): recon_conf null, validity unchanged */
        nr_sense_recon_skip_note();
      } else {
        /* MRC-combined data Es/N0 = the DM-RS post-estimation SNR (itself the MRC sum over the antennas) minus the DM-RS power boost */
        const float boost_db = 20.0f * log10f(get_beta_dmrs(dlsch_config->n_dmrs_cdm_groups, dlsch_config->dmrsConfigType == NFAPI_NR_DMRS_TYPE2));
        rs_meta.q.recon_conf = nr_da_recon(isac_h, isac_max_re, isac_nof_ant, isac_k, isac_l, nof_re, (const int16_t *)mod_syms, cw->qamModOrder,
                                           rs_meta.snr_valid ? rs_meta.snr_db - boost_db : NAN, rs_meta.recon_prb_ok, NULL);
        rs_meta.recon_valid = rs_meta.q.recon_conf == rs_meta.q.recon_conf;
      }
    }
  }
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
            nvar_f, e_nv);
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
              sym_start[g_first], g_out, nvar_f, e_nv);
      emitted++;
      g_first = i + 1;
    } else if (is_last) {
      // Tail that never passed the gates: merge it BACKWARDS by re-emitting from g_first to the end
      // as one row if nothing has been emitted yet, otherwise fold it into the whole-slot fallback.
      if (emitted == 0) {
        da_emit(slot_idx, 0.0f, &carrier, &rs_meta, dlsch_config->dmrs_ports, nl, e_h, isac_nof_ant, e_stride, e_k, e_l, 0, e_nre,
                nvar_f, e_nv);
        emitted++;
      } else {
        const double centre = 0.5 * ((double)sym_id[g_first] + (double)sym_id[i]) + 0.5;
        const float  frac   = (float)(centre / (double)NR_SYMBOLS_PER_SLOT);
        if (g_out > 0)
          da_emit(slot_idx, frac, &carrier, &rs_meta, dlsch_config->dmrs_ports, nl, e_h, isac_nof_ant, e_stride, e_k, e_l,
                  sym_start[g_first], g_out, nvar_f, e_nv);
      }
    }
  }
}

/* ---- the tap off the decode thread (nr_isac_pdsch_data_aided_submit_async) ---- */
/* Sizing. [MEASURED DGX 2026-11-01, PCI 415/923 day replays, sensing on]: <= 210 tap calls/s (9.5-20.7k per 100 s), 166-175 us of worker CPU per call, ~90 % of the
 * grants Nl > 1. The worker used to take ~2.9x that in wall time because the LDPC re-encode joined the shared decode Tpool (see da_inline_pool): with the encode on the
 * worker the load is rate x cost. Design point = 2x the measured peak rate (DA_RATE_PEAK) at that cost, one worker per DA_UTIL of a core, workers capped by the
 * background lane's cores; the queue holds the rate x DA_STALL_S jobs a descheduled nice-19 worker leaves behind. NR_BGQ_MAX is the queue's hard cap. */
#define DA_RATE_PEAK 420.0 /* grants/s */
#define DA_COST_S 200e-6   /* worker CPU per grant, measured mean 170 us plus margin */
#define DA_UTIL 0.25       /* a worker is not planned beyond this share of a core (nice 19 on a shared lane) */
#define DA_STALL_S 0.05    /* queued-work horizon of a worker stall */
#define DA_MIN_WORKERS 2   /* a nice-19 worker on the shared background lane is descheduled for 100 ms+ at a time [MEASURED DGX 2026-11-01: 415 shed 19 % with one worker at 2 % CPU]: a second worker covers the first's stalls */
#define DA_NW_MAX 4
#define DA_POOL 32 /* jobs per cell: >= DA_RATE_PEAK * DA_STALL_S = 21, plus headroom (the free mask is 32 bits) */
#define DA_TB_CAP (NR_MIMO_LS_MAX_NL * 65536) /* bytes: one layer, 273 PRB x 12 x 14 REs x 8 bits / 8 = 45.9 kB + CRC; Nl layers carry Nl times that */
_Static_assert(DA_POOL <= NR_BGQ_MAX && DA_POOL <= 32, "the daBg queue cannot hold the pool");
_Static_assert(DA_RATE_PEAK * DA_STALL_S <= DA_POOL, "pool shorter than the stall horizon");
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
  uint64_t t_enq_ns; /* queue-wait stamp */
  c16_t *rx; /* nant x stride c16, the layout of the consumer's rxdataF; only the grant's region is written */
  uint8_t *tb;
  int idx;
  bool part_on;          /* partial TB: part is this grant's per-block result */
  nr_da_cb_pass_t part;
} da_job_t;
typedef struct {
  da_job_t *job[DA_POOL];
  unsigned free_mask; /* bit i = job i is free */
  uint32_t stride;    /* samples_per_slot_wCP */
  int nant;
  _Atomic int ready;
  _Atomic uint64_t queued, dropped, done;
  _Atomic uint64_t wait_hist[24]; /* enqueue -> worker start, log2(us) buckets */
  _Atomic uint64_t drop_pool, drop_queue, drop_geom; /* why: no free job / queue full or busy / a grant the snapshot cannot hold (or no pool) */
  _Atomic uint64_t run_ns, snap_ns; /* worker time of the tap / consumer time of the snapshot of the queued grants: saved = run - snap */
} da_pool_t;
/* mpci: the pool, its queue and its worker are the cell's (the consumer calling _async is bound to it); the worker binds to the cell of each item (nr_gpu_bgq.h) */
NR_CELL_STATIC(da_pool_t, g_da)
#define g_da NR_CELL_REF(g_da)
typedef struct {
  nr_bgq_t q[DA_NW_MAX];
  int n; /* workers of this cell (set in prepare, before the fence) */
} da_qs_t;
NR_CELL_STATIC(da_qs_t, g_da_qs)
#define g_da_qs NR_CELL_REF(g_da_qs)
/* recon runs on a daBg worker only and is shed when the backlog (jobs in the pool = queued + running) is at least half the pool */
static bool da_recon_backlogged(void)
{
  if (!da_on_worker)
    return false;
  return nr_da_recon_skip((unsigned)(DA_POOL - __builtin_popcount(__atomic_load_n(&g_da.free_mask, __ATOMIC_RELAXED))), DA_POOL);
}
static const char *const da_wname[DA_NW_MAX] = {"daBg0", "daBg1", "daBg2", "daBg3"};

/* workers: the load at the design point over a planned share of a core, capped by the background lane's cores shared by every cell of the process */
static int da_workers_planned(void)
{
  int nw = (int)ceil(DA_RATE_PEAK * DA_COST_S / DA_UTIL);
  const int per_cell = nr_bgq_lane_cores() / (nr_cell_count() > 0 ? nr_cell_count() : 1);
  if (nw > per_cell)
    nw = per_cell;
  nw = nw < DA_MIN_WORKERS ? DA_MIN_WORKERS : nw; /* stall cover, not CPU: still bounded by the lane's cores per cell below */
  if (nw > per_cell)
    nw = per_cell;
  return nw < 1 ? 1 : nw > DA_NW_MAX ? DA_NW_MAX : nw;
}

/* May run before or after _async_init (the receiver calls init from the queue start and prepare from the cell prealloc, in either order): prepare only parks the
 * workers; _async_init plans the same count itself and nr_bgq_start builds any worker that was not prepared. */
void nr_isac_pdsch_data_aided_async_prepare(int cell)
{
  if (!nr_isac_source_configured(NR_ISAC_SRC_PDSCH_DATA)) /* configured, not enabled: the cell's session is not open before the RX fence */
    return;
  if (atomic_load(&g_da_cells[cell].ready))
    return; /* the workers are already running: nr_bgq_prepare would re-initialise a live queue */
  const int nw = da_workers_planned();
  da_qs_t *Q = &g_da_qs_cells[cell];
  int ok = 0;
  for (int i = 0; i < nw; i++)
    if (nr_bgq_prepare(&Q->q[i], DA_POOL, da_wname[i], cell) == 0)
      ok++;
    else
      LOG_W(NR_PHY, "SENSING: daBg prepare: worker %d of cell %d not parked (the start builds it)\n", i, cell);
  Q->n = ok > Q->n ? ok : Q->n;
}

/* worker side, once, before RX start: build and prefault the worker's own scratch so the first grant allocates nothing */
static void da_warm(void *arg)
{
  da_pool_t *P = &g_da;
  da_on_worker = true;
  (void)da_scratch_ensure((uint32_t)P->nant, true, true);
  __atomic_fetch_or(&P->free_mask, 1u << ((da_job_t *)arg)->idx, __ATOMIC_RELEASE);
}

void nr_isac_pdsch_data_aided_async_init(const PHY_VARS_NR_UE *ue)
{
  da_pool_t *P = &g_da;
  if (atomic_load(&P->ready))
    return;
  if (!nr_isac_source_configured(NR_ISAC_SRC_PDSCH_DATA)) /* configured, not enabled: no session is open before the RX fence */
    return; /* sensing without this source: nothing to build */
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
  if (!P->free_mask) {
    LOG_W(NR_PHY, "SENSING: data-aided tap: job pool allocation failed, every grant will be shed\n");
    return;
  }
  da_qs_t *Q = &g_da_qs;
  const int nw = da_workers_planned();
  int started = 0;
  unsigned warm_bits = 0;
  for (int w = 0; w < nw; w++) {
    if (nr_bgq_start(&Q->q[w], DA_POOL, da_wname[w]) != 0) {
      LOG_W(NR_PHY, "SENSING: daBg worker %d/%d failed to start\n", w, nw);
      break;
    }
    started++;
  }
  Q->n = started; /* no worker: every grant is shed (counted), none runs on the decode thread */
  if (!started) {
    P->free_mask = 0;
    LOG_W(NR_PHY, "SENSING: data-aided tap: no daBg worker started, every grant will be shed\n");
    return;
  }
  /* each worker builds its own scratch (a job's slot is borrowed for the hand-off, returned by da_warm), then the pool opens */
  for (int w = 0; w < started; w++) {
    const unsigned warm_bit = P->free_mask & (0u - P->free_mask);
    P->free_mask &= ~warm_bit;
    if (nr_bgq_push(&Q->q[w], da_warm, P->job[__builtin_ctz(warm_bit)]) == 0)
      warm_bits |= warm_bit;
    else
      __atomic_fetch_or(&P->free_mask, warm_bit, __ATOMIC_RELEASE);
  }
  for (int t = 0; t < 5000 && (warm_bits & ~__atomic_load_n(&P->free_mask, __ATOMIC_ACQUIRE)); t++) /* <= 5 s, before RX start */
    usleep(1000);
  if (warm_bits & ~__atomic_load_n(&P->free_mask, __ATOMIC_ACQUIRE))
    LOG_W(NR_PHY, "SENSING: daBg scratch warm-up did not finish; the first grants allocate on the worker\n");
  atomic_store(&P->ready, 1);
  LOG_A(NR_PHY, "SENSING: data-aided tap off the decode thread: %u jobs (Nl<=%d, TB<=%d B), %d daBg workers\n", (unsigned)__builtin_popcount(P->free_mask | warm_bits), NR_MIMO_LS_MAX_NL, DA_TB_CAP, started);
}

void nr_isac_pdsch_data_aided_async_counts(uint64_t *queued, uint64_t *dropped, uint64_t *done, uint64_t *saved_us)
{
  *queued = atomic_load(&g_da.queued);
  *dropped = atomic_load(&g_da.dropped);
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

/* upper edge (us) of the log2 bucket holding quantile q of the queue waits */
static unsigned long da_wait_q(const da_pool_t *P, double q)
{
  uint64_t tot = 0, c = 0;
  for (int i = 0; i < 24; i++)
    tot += atomic_load(&P->wait_hist[i]);
  for (int i = 0; i < 24; i++) {
    c += atomic_load(&P->wait_hist[i]);
    if (tot && (double)c >= q * (double)tot)
      return i ? 1ul << i : 0;
  }
  return 0;
}

static void da_run(void *arg)
{
  da_job_t *j = arg;
  da_pool_t *P = &g_da; /* the worker was bound to the item's cell by nr_bgq_run_one */
  da_on_worker = true;
  const uint64_t t0 = da_now_ns();
  {
    const uint64_t w_us = (t0 - j->t_enq_ns) / 1000;
    int b = w_us ? 64 - __builtin_clzll(w_us) : 0; /* 0, 1, 2-3, 4-7, ... us */
    atomic_fetch_add(&P->wait_hist[b > 23 ? 23 : b], 1);
  }
  const uint64_t unit_c0 = nr_sense_unit_cpu_ns(); /* sensing mode: re-encode, recon, CFR export are the cost of the RNTI's PDSCH_DATA unit */
  nr_isac_abs_slot_override = j->abs_slot;
  nr_isac_da_cb_pass = j->part_on ? &j->part : NULL;
  nr_isac_pdsch_data_aided_submit(j->ue, &j->proc, &j->cw, &j->dlsch, &j->fa, j->rnti, j->tb, j->harq_tag, (const c16_t(*)[P->stride])j->rx, j->nvar, j->G,
                                  j->dmrs_snr_db);
  nr_isac_abs_slot_override = 0;
  nr_isac_da_cb_pass = NULL;
  const uint64_t t1 = da_now_ns();
  if (unit_c0)
  {
    const uint64_t cpu_da = nr_sense_unit_cpu_ns() - unit_c0;
    nr_sense_unit_job(NR_UNIT_PDSCH_DATA, j->rnti, cpu_da, 0 /* the daBg queue wait is the DA resource's own (violation path on a shed); QUEUE_DL is the decode queue */, t1 - j->t_enq_ns > NR_UNIT_LATE_NS, (uint16_t)j->fa.num_rbs, t1);
    nr_sense_unit_cost_da(NR_UNIT_PDSCH_DATA, j->rnti, cpu_da, t1); /* the daBg worker CPU is the unit's DA demand (it is also pool CPU: the worker leases a pool token) */
  }
  atomic_fetch_add(&P->run_ns, t1 - t0);
  const uint64_t n = atomic_fetch_add(&P->done, 1) + 1;
  if (n == 1 || n % 2000 == 0) {
    uint64_t q, d_, d, sv;
    nr_isac_pdsch_data_aided_async_counts(&q, &d_, &d, &sv);
    LOG_A(NR_PHY, "SENSING: DATA_AIDED_ASYNC done=%lu queued=%lu dropped=%lu(pool=%lu queue=%lu geom=%lu) qwait_us_p50<=%lu p99<=%lu consumer_us_saved=%lu\n", (unsigned long)d, (unsigned long)q, (unsigned long)d_,
          (unsigned long)atomic_load(&P->drop_pool), (unsigned long)atomic_load(&P->drop_queue), (unsigned long)atomic_load(&P->drop_geom), da_wait_q(P, 0.5), da_wait_q(P, 0.99), (unsigned long)sv);
  }
  __atomic_fetch_or(&P->free_mask, 1u << j->idx, __ATOMIC_RELEASE);
}

/* daBg workers of the calling thread's cell (0 before the pool is up): the capacity the DA resource of the sensing scheduler is built from */
int nr_isac_pdsch_data_aided_workers(void) { return atomic_load_explicit(&g_da.ready, memory_order_acquire) ? g_da_qs.n : 0; }

static int da_shed(_Atomic uint64_t *why, uint16_t rnti)
{
  da_pool_t *P = &g_da;
  atomic_fetch_add(why, 1);
  atomic_fetch_add(&P->dropped, 1);
  nr_sense_async_note(NR_SENSE_SRC_PDSCH_DATA, 0);
  nr_sense_unit_violation(NR_UNIT_PDSCH_DATA, rnti); /* a message of an enabled unit was dropped (counted only when the unit is enabled) */
  return 0;
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
  /* what the tap does without producing anything (G refusal + its count, source off, Nl out of range) costs nothing: it runs here, it is not sensing work */
  if (decode_G != da_tap_G(cw, dlsch_config, freq_alloc) || !nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DATA) || cw->Nl < 1 || cw->Nl > NR_MIMO_LS_MAX_NL) {
    nr_isac_pdsch_data_aided_submit(ue, proc, cw, dlsch_config, freq_alloc, rnti, tb_bytes, harq_pid_tag, rxdataF, nvar, decode_G, dmrs_snr_db);
    return 0;
  }
  if (!nr_sense_unit_gate(NR_UNIT_PDSCH_DATA, rnti, (uint16_t)freq_alloc->num_rbs)) /* SENSING mode: a disabled unit is skipped before the snapshot, counted once */
    return 0;
  /* Everything else is sensing work (re-encode, scramble, modulate, LS solve, CFR export): it never runs on this thread. A grant the worker cannot take
   * is shed and counted per source (nr_sense_async_note -> ISAC_METRICS sense_async_drop_pdsch_data); decode CRC and latency come first. */
  const uint32_t A = cw->TBS;
  const size_t tb_n = ((size_t)A + (A > NR_MAX_PDSCH_TBS ? 24 : 16) + 7) / 8;
  if (!atomic_load_explicit(&P->ready, memory_order_acquire) || tb_n > DA_TB_CAP || (int)P->nant != fp->nb_antennas_rx || P->stride != (uint32_t)fp->samples_per_slot_wCP)
    return da_shed(&P->drop_geom, rnti);
  da_qs_t *Q = &g_da_qs;
  const int nq = Q->n;
  int qi = -1;
  unsigned best = ~0u;
  for (int k = 0; k < nq; k++) { /* the worker with the shortest backlog: a descheduled worker's queue stops growing once the others are shorter */
    nr_bgq_t *bq = &Q->q[k];
    const unsigned depth = __atomic_load_n(&bq->n, __ATOMIC_RELAXED) + __atomic_load_n(&bq->run, __ATOMIC_RELAXED);
    if (nr_bgq_room(bq) && depth < best) {
      best = depth;
      qi = k;
    }
  }
  if (qi < 0)
    return da_shed(&P->drop_queue, rnti);
  da_job_t *j = NULL;
  unsigned m = __atomic_load_n(&P->free_mask, __ATOMIC_ACQUIRE);
  while (m && !j)
    if (__atomic_compare_exchange_n(&P->free_mask, &m, m & (m - 1u), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      j = P->job[__builtin_ctz(m)];
  if (!j)
    return da_shed(&P->drop_pool, rnti);
  const uint64_t t_snap0 = da_now_ns();
  /* a non-contiguous allocation reads PRBs anywhere in the carrier: snapshot the carrier's whole band (same index map as the tap's) */
  int start_re, num_sc;
  if (freq_alloc->n_prb_list) {
    start_re = fp->first_carrier_offset % fp->ofdm_symbol_size;
    num_sc = fp->N_RB_DL * NR_NB_SC_PER_RB;
  } else {
    start_re = (fp->first_carrier_offset + (dlsch_config->BWPStart + freq_alloc->first_rb) * NR_NB_SC_PER_RB) % fp->ofdm_symbol_size;
    num_sc = freq_alloc->num_rbs * NR_NB_SC_PER_RB;
  }
  int ok = nr_da_snap_copy((uint32_t *)j->rx, (const uint32_t *)&rxdataF[0][0], P->nant, P->stride, fp->ofdm_symbol_size, dlsch_config->start_symbol, dlsch_config->number_symbols,
                           start_re, num_sc) == 0;
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
    j->part_on = nr_isac_da_cb_pass != NULL;
    if (j->part_on) j->part = *nr_isac_da_cb_pass;
    memcpy(j->tb, tb_bytes, tb_n);
    j->t_enq_ns = da_now_ns();
    if (nr_bgq_push(&Q->q[qi], da_run, j) == 0) {
      atomic_fetch_add(&P->snap_ns, da_now_ns() - t_snap0);
      atomic_fetch_add(&P->queued, 1);
      nr_sense_async_note(NR_SENSE_SRC_PDSCH_DATA, 1);
      return 1;
    }
  }
  __atomic_fetch_or(&P->free_mask, 1u << j->idx, __ATOMIC_RELEASE);
  return da_shed(ok ? &P->drop_queue : &P->drop_geom, rnti);
}
