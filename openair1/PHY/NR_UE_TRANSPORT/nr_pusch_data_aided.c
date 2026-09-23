/* Passive uplink data-aided CFR -- see nr_pusch_data_aided.h.
 *
 * Deliberately mirrors nr_pdsch_data_aided.c step for step (segment -> LDPC encode -> scramble ->
 * modulate -> map -> H = Y/X). The differences from the downlink are only these, and each one is a
 * place a silent wrong answer was available:
 *
 *  1. UCI. A downlink TB owns every data RE in its allocation. An uplink one does not: HARQ-ACK
 *     with O_ACK <= 2 PUNCTURES the ULSCH, overwriting REs whose positions depend on bits this
 *     receiver never decodes. Rather than reconstruct what we cannot know, the RE-ENCODE path runs
 *     ONLY on grants that decoded with no UCI hypothesis at all (uci_ack_re == 0 at the call site).
 *     Grants rescued by a UCI hypothesis, and CRC-failed grants, use the MASKED path instead: hard
 *     decisions of the grant's own LLRs, kept only where the learned confidence says they are right.
 *  2. Scrambling. TS 38.211 gives PUSCH the same Gold sequence as PDSCH when no UCI is present --
 *     nr_pusch_codeword_scrambling() itself delegates to nr_codeword_scrambling() for template ==
 *     NULL -- so the no-UCI case needs the public downlink function, not a UL-specific export.
 *  3. rxdataF is a RING of RU_RX_SLOT_DEPTH slots here, not a plain per-slot buffer.
 */
#include "nr_pusch_data_aided.h"
#include "nr_llr_confidence.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "PHY/CODING/nrLDPC_extern.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/MODULATION/nr_modulation.h"
#include "PHY/NR_REFSIG/nr_refsig.h"
#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "PHY/NR_UE_ISAC/nr_isac.h"
#include "common/utils/LOG/log.h"
#include "executables/nr-uesoftmodem.h"

#define UL_DA_MAX_RE (273 * 12 * 14)

static _Atomic uint64_t g_da_try, g_da_ok, g_da_rej_seg, g_da_rej_enc, g_da_rej_modidx, g_da_re;
static _Atomic uint64_t g_da_masked, g_da_nocal;

void nr_isac_pusch_data_aided_stats_dump(void)
{
  const uint64_t t = atomic_load_explicit(&g_da_try, memory_order_relaxed);
  if (t == 0) {
    return;
  }
  LOG_I(PHY,
        "SENSING: pusch_data_aided[try=%lu submitted=%lu (%.1f%%) re=%lu rej_seg=%lu rej_enc=%lu "
        "rej_modidx=%lu masked=%lu no_cal=%lu]\n",
        (unsigned long)t,
        (unsigned long)atomic_load_explicit(&g_da_ok, memory_order_relaxed),
        100.0 * (double)atomic_load_explicit(&g_da_ok, memory_order_relaxed) / (double)t,
        (unsigned long)atomic_load_explicit(&g_da_re, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&g_da_rej_seg, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&g_da_rej_enc, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&g_da_rej_modidx, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&g_da_masked, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&g_da_nocal, memory_order_relaxed));
}

/* nr_llr_confidence works on one byte per bit; the LDPC output, the scrambler and the modulator on
 * packed bits, bit i at byte i/8, bit i%8. */
static uint8_t *bit_buf(uint32_t G)
{
  static __thread uint8_t *buf = NULL;
  static __thread uint32_t cap = 0;
  if (cap < G) {
    free(buf);
    buf = malloc(G);
    cap = buf ? G : 0;
  }
  return buf;
}

void nr_isac_pusch_data_aided_submit(PHY_VARS_NR_UE *ue,
                                     PHY_VARS_gNB *gnb,
                                     const nfapi_nr_pusch_pdu_t *pdu,
                                     const nr_pdcch_blind_ul_result_t *g,
                                     const uint8_t *tb_bytes,
                                     const int16_t *llr,
                                     uint32_t llr_G,
                                     uint32_t harq_pid_tag,
                                     uint32_t ul_slot_idx,
                                     uint32_t nof_ant,
                                     int slot)
{
  if (!nr_isac_enabled() || !nr_isac_source_enabled(NR_ISAC_SRC_PUSCH_DATA)) {
    return;
  }
  if (ue == NULL || gnb == NULL || pdu == NULL || g == NULL) {
    return;
  }
  if (!nr_isac_flow_admit(g->rnti)) {
    return;
  }
  if (g->nrOfLayers != 1) {
    return; // single layer only, same scope guard the decode itself applies
  }
  atomic_fetch_add_explicit(&g_da_try, 1, memory_order_relaxed);

  const NR_DL_FRAME_PARMS *fp    = &gnb->frame_parms;
  const int                symsz = fp->ofdm_symbol_size;
  const int                sps   = fp->symbols_per_slot;
  const uint32_t           Qm    = pdu->qam_mod_order;

  const int n_dmrs_sym = __builtin_popcount((unsigned)g->ul_dmrs_symb_pos
                                            & (((1u << g->num_symbols) - 1u) << g->start_symbol));
  const int nb_dmrs_re_per_rb = ((g->dmrs_config_type == 0) ? 6 : 4) * g->n_dmrs_cdm_groups;
  nrLDPC_TB_encoding_parameters_t TB = {0};
  TB.Qm        = Qm;
  TB.nb_layers = 1;
  TB.G = nr_get_G(g->num_rb, g->num_symbols, nb_dmrs_re_per_rb, n_dmrs_sym, 0, Qm, 1);
  if (TB.G == 0 || TB.G > UL_DA_MAX_RE * 8) {
    atomic_fetch_add_explicit(&g_da_rej_seg, 1, memory_order_relaxed);
    return;
  }

  /* 32-byte aligned for the same reason the downlink twin documents: nr_modulation() and the LDPC
   * encoder store through AVX2 intrinsics that fault on a misaligned buffer, and a __thread buffer's
   * address depends on the whole TLS layout, which makes natural alignment accidental. */
  static __thread uint8_t coded_bits[(UL_DA_MAX_RE * 8 + 63) / 64 * 64 + 64] __attribute__((aligned(32)));
  static __thread uint8_t keep[UL_DA_MAX_RE];
  memset(coded_bits, 0, sizeof(coded_bits));
  const bool masked = (tb_bytes == NULL);
  const bool have_llr = (llr != NULL && llr_G == TB.G);

  if (masked) {
    float tau = 0.0f;
    uint8_t *hard = bit_buf(TB.G);
    if (!have_llr || hard == NULL || !nr_llrconf_threshold((uint8_t)Qm, &tau)) {
      atomic_fetch_add_explicit(&g_da_nocal, 1, memory_order_relaxed);
      return; /* no calibration for this Qm yet: DM-RS only (spec §6) */
    }
    nr_llrconf_hard((uint8_t)Qm, llr, TB.G, tau, hard, keep);
    /* Hard decisions of DESCRAMBLED LLRs; the scramble below restores the transmitted bits. */
    for (uint32_t i = 0; i < TB.G; i++)
      coded_bits[i >> 3] |= (uint8_t)(hard[i] << (i & 7));
  } else {
    /* ---- Segment + LDPC encode. Re-segmented here because the decode path only sized C/K/Z/F. ---- */
    static __thread uint8_t  seg_storage[MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER][8448];
    static __thread uint8_t *c_segs[MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER];
    for (int r = 0; r < MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER; r++) {
      c_segs[r] = seg_storage[r];
    }

    const uint32_t A = pdu->pusch_data.tb_size * 8;
    TB.harq_unique_pid = harq_pid_tag;
    TB.BG              = pdu->maintenance_parms_v3.ldpcBaseGraph;
    TB.A               = A;
    const uint32_t B   = A + 24; // TB CRC, as nr_segmentation expects
    TB.Kb = nr_segmentation((unsigned char *)tb_bytes, c_segs, B, &TB.C, &TB.K, &TB.Z, &TB.F, TB.BG);
    if (TB.C > MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER) {
      atomic_fetch_add_explicit(&g_da_rej_seg, 1, memory_order_relaxed);
      return;
    }

    TB.nb_rb      = g->num_rb;
    TB.mcs        = g->mcs;
    TB.rv_index   = g->rv;
    TB.tbslbrm    = pdu->maintenance_parms_v3.tbSizeLbrmBytes;
    TB.output     = coded_bits;

    static __thread nrLDPC_segment_encoding_parameters_t segs[MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER];
    memset(segs, 0, sizeof(segs));
    TB.segments = segs;
    for (uint32_t r = 0; r < TB.C; r++) {
      segs[r].c = c_segs[r];
      segs[r].E = nr_get_E(TB.G, TB.C, TB.Qm, TB.nb_layers, r);
      reset_meas(&segs[r].ts_interleave);
      reset_meas(&segs[r].ts_rate_match);
      reset_meas(&segs[r].ts_ldpc_encode);
    }

    nrLDPC_slot_encoding_parameters_t sp = {.frame      = 0,
                                            .slot       = slot,
                                            .nb_TBs     = 1,
                                            .threadPool = &get_nrUE_params()->Tpool,
                                            .tinput     = NULL,
                                            .tprep      = NULL,
                                            .tparity    = NULL,
                                            .toutput    = NULL,
                                            .TBs        = &TB};
    if (ue->nrLDPC_coding_interface.nrLDPC_coding_encoder(&sp) != 0) {
      atomic_fetch_add_explicit(&g_da_rej_enc, 1, memory_order_relaxed);
      return;
    }

    uint8_t *truth = have_llr ? bit_buf(TB.G) : NULL;
    if (truth != NULL) { /* CRC-OK grant: this IS the calibration ground truth */
      for (uint32_t i = 0; i < TB.G; i++)
        truth[i] = (coded_bits[i >> 3] >> (i & 7)) & 1;
      nr_llrconf_observe((uint8_t)Qm, llr, truth, TB.G);
      nr_llrconf_agreement((uint8_t)Qm, llr, truth, TB.G);
    }
  }

  /* ---- Scramble + modulate. The re-encode path has no UCI by construction (see the file header),
   * exactly the case where PUSCH scrambling reduces to the plain codeword scrambling. The masked path
   * scrambles bits that the receiver DESCRAMBLED with this same c(i), so it also reproduces UCI and
   * placeholder bits. ---- */
  static __thread uint32_t scrambled[(UL_DA_MAX_RE * 8 + 31) / 32 + 1] __attribute__((aligned(32)));
  nr_codeword_scrambling(coded_bits, TB.G, 0, pdu->data_scrambling_id, g->rnti, scrambled);

  static __thread c16_t mod_syms[UL_DA_MAX_RE] __attribute__((aligned(32)));
  nr_modulation(scrambled, TB.G, Qm, (int16_t *)mod_syms);

  /* ---- Map REs and form H = Y/X. ---- */
  static __thread float    h_buf[4 * 2 * UL_DA_MAX_RE];
  static __thread uint32_t k_buf[UL_DA_MAX_RE];
  static __thread uint32_t l_buf[UL_DA_MAX_RE];

  const uint32_t cap      = UL_DA_MAX_RE;
  const uint32_t nant     = (nof_ant > 4) ? 4 : (nof_ant ? nof_ant : 1);
  const int      slot_off = (slot % RU_RX_SLOT_DEPTH) * sps * symsz;
  const uint32_t expected = TB.G / (Qm * TB.nb_layers);

  uint32_t mod_idx = 0, nof_re = 0;
  for (int l = g->start_symbol; l < g->start_symbol + g->num_symbols; l++) {
    const bool is_dmrs = (g->ul_dmrs_symb_pos >> l) & 1u;
    /* A DM-RS symbol is NOT data-free. With numDmrsCdmGrpsNoData == 1 only one CDM group is
     * reserved and the remaining REs carry ordinary PUSCH, mapped in the same increasing-k sweep.
     * Skipping them WITHOUT consuming their modulation symbols desynchronises mod_idx from the
     * transmitter for every later symbol -- the exact bug that corrupted the downlink twin, caught
     * there by the invariant below. Type 1 with one CDM group leaves the odd subcarriers. */
    const bool dmrs_has_data = is_dmrs && (g->n_dmrs_cdm_groups == 1) && (g->dmrs_config_type == 0);
    if (is_dmrs && !dmrs_has_data) {
      continue;
    }

    for (int rb = 0; rb < g->num_rb; rb++) {
      for (int sc = 0; sc < 12; sc++) {
        if (is_dmrs && ((sc % 2) == 0)) {
          continue; // even subcarriers are the reserved CDM group
        }
        if (mod_idx >= expected || nof_re >= cap) {
          break;
        }
        /* TWO different indices, and collapsing them into one is silently wrong: rxdataF is the
         * FFT-ordered grid (nr_symbol_fep_ul writes DC at bin 0, the carrier starting at
         * first_carrier_offset and wrapping) -- which is why nr_rx_pusch, reading this same buffer,
         * adds first_carrier_offset -- while the ISAC grid column is CRB0-absolute. The DL twin
         * (nr_pdsch_data_aided.c) keeps them apart as start_re/base_sc; this used to use the
         * CRB-absolute value for BOTH, so Y was read ~first_carrier_offset subcarriers away from X
         * and H = Y/X was noise. */
        const int k_crb = (g->start_rb + g->bwp_start) * 12 + rb * 12 + sc;
        int       k_abs = fp->first_carrier_offset + k_crb;
        if (k_abs >= symsz)
          k_abs -= symsz;

        const uint32_t m  = mod_idx;
        const c16_t    xs = mod_syms[mod_idx++];
        if (masked && !keep[m]) {
          continue; // consumed (keeps the mapping aligned), not measured: low-confidence decision
        }
        const double xr = (double)xs.r, xi = (double)xs.i;
        const double p  = xr * xr + xi * xi;
        if (p <= 0.0) {
          continue; // a zero constellation point carries no channel information
        }

        for (uint32_t a = 0; a < nant; a++) {
          const c16_t *rf = &gnb->common_vars.rxdataF[a][slot_off + l * symsz];
          const double yr = (double)rf[k_abs].r, yi = (double)rf[k_abs].i;
          /* H = Y/X = Y * conj(X) / |X|^2 */
          const size_t o = 2 * ((size_t)a * cap + nof_re);
          h_buf[o]       = (float)((yr * xr + yi * xi) / p);
          h_buf[o + 1]   = (float)((yi * xr - yr * xi) / p);
        }
        k_buf[nof_re] = (uint32_t)k_crb;
        l_buf[nof_re] = (uint32_t)l;
        nof_re++;
      }
    }
  }

  /* Invariant, not a sanity check: nr_get_G() budgets a known data-RE count and this loop consumes
   * exactly one modulation symbol per data RE, so mod_idx MUST land on G/(Qm*Nl). A mismatch means
   * the RE enumeration and the transmitter's mapping have drifted apart, which yields a confidently
   * WRONG H rather than a noisy one. Submit nothing. */
  if (mod_idx != expected) {
    atomic_fetch_add_explicit(&g_da_rej_modidx, 1, memory_order_relaxed);
    static _Atomic int once = 0;
    if (atomic_exchange(&once, 1) == 0) {
      LOG_W(PHY,
            "SENSING: pusch data-aided RE enumeration disagrees with nr_get_G: mod_idx=%u expected=%u "
            "(rb=%u sym=%u+%u dmrs_pos=0x%x cdm_grps=%u type=%u) -- submitting nothing\n",
            mod_idx, expected, (unsigned)g->num_rb, (unsigned)g->start_symbol,
            (unsigned)g->num_symbols, (unsigned)g->ul_dmrs_symb_pos,
            (unsigned)g->n_dmrs_cdm_groups, (unsigned)g->dmrs_config_type);
    }
    return;
  }
  if (nof_re == 0) {
    return;
  }

  /* ul_CarrierFreq for the same reason the DM-RS path uses it: the range axis scales with the
   * wavelength actually observed, and on TDD the two coincide only because the duplex spacing is 0. */
  nr_isac_carrier_t carrier = {.nof_prb         = (uint32_t)fp->N_RB_UL,
                               .scs_hz          = fp->subcarrier_spacing,
                               .dl_center_hz    = fp->ul_CarrierFreq,
                               .pci             = fp->Nid_cell,
                               .slots_per_frame = fp->slots_per_frame};
  nr_isac_submit_cfr_multi_session(ul_slot_idx, 0.0f, NR_ISAC_SRC_PUSCH_DATA, &carrier, h_buf, nant, cap,
                                   k_buf, l_buf, nof_re, 1.0f, (uint64_t)g->rnti);
  atomic_fetch_add_explicit(&g_da_ok, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_da_re, nof_re, memory_order_relaxed);
  if (masked) {
    atomic_fetch_add_explicit(&g_da_masked, 1, memory_order_relaxed);
  }
}
