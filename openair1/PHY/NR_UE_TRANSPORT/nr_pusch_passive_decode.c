/* See nr_pusch_passive_decode.h for why this file constructs a gNB by hand. */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#include "nr_pusch_passive_decode.h"

#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"
#include "PHY/defs_gNB.h"
#include "PHY/defs_RU.h"          // RU_RX_SLOT_DEPTH -- the gNB rxdataF ring depth
#include "PHY/MODULATION/modulation_UE.h"
#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "PHY/NR_TRANSPORT/nr_ulsch.h"
#include "executables/softmodem-common.h"
#include "executables/nr-uesoftmodem.h" // get_nrUE_params -- the UE thread pool this reuses
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"

/* nr_ulsch_decoding() has no declaration in any header this library exposes -- nr_transport_proto.h
 * declares nr_rx_pusch_group_tp() but not its decoder. Declared here against the definition read
 * from nr_ulsch_decoding.c rather than adding a prototype to a shared header, so that this passive
 * path cannot quietly change a signature the gNB build also depends on. */
extern int nr_ulsch_decoding(PHY_VARS_gNB *phy_vars_gNB,
                             NR_DL_FRAME_PARMS *frame_parms,
                             uint32_t frame,
                             uint8_t nr_tti_rx,
                             int *ULSCH_ids,
                             int nb_pusch);

/* ---- Two symbols the gNB PUSCH receive chain needs that live in files this library deliberately
 * does NOT compile. Providing them here costs four lines; pulling in their home files would drag
 * phy_init_nr_gNB() (with PRACH, SRS, PUCCH and the whole DLSCH side) and the entire gNB PHY
 * procedures TU into a passive receiver that calls none of it.
 *
 * Both are reproduced with their real semantics, not stubbed:
 *  - get_first_ant_idx() is the one-line expression from phy_procedures_nr_gNB.c:75 verbatim. With
 *    enable_analog_das = 0 (this receiver has no analog DAS) it returns fapi_start_port, which is
 *    the branch that would run anyway.
 *  - get_phy_stats() returns NULL, and that is CORRECT rather than a shortcut: its only caller here
 *    is nr_ulsch_decoding.c:142, which guards every use with . The structure it would
 *    otherwise hand back is the gNB's per-RNTI MAC-facing statistics array -- state a passive
 *    receiver has no business keeping, and which nothing in this path reads. This file keeps its
 *    own census instead (nr_pusch_passive_stats_dump).
 *
 * CONSTRAINT: these are global symbols, so this library must never be linked alongside PHY_NR.
 * It is not -- nr-uesoftmodem links PHY_NR_COMMON, PHY_NR_UE and this, and that is the whole
 * reason this library exists. */
uint16_t get_first_ant_idx(bool das, uint16_t num_ports_beams, uint16_t beam_id, uint16_t fapi_start_port)
{
  return ((das) ? (beam_id & 0x7fff) * num_ports_beams : fapi_start_port);
}

NR_gNB_PHY_STATS_t *get_phy_stats(PHY_VARS_gNB *gNB, uint16_t rnti)
{
  (void)gNB;
  (void)rnti;
  return NULL;
}

#define PASSIVE_UL_MAX_ANT 4
/* HARQ namespace. The DL path already uses 1000+ (attached UE), 2000+ (passive PDSCH decode) and
 * 3000+ (data-aided re-encode) on the SAME dlopen'd LDPC interface. A hardware accelerator keys its
 * internal state on this id, so an overlap would alias two unrelated transport blocks. */
#define PASSIVE_UL_HARQ_TAG_BASE 4000

static PHY_VARS_gNB *g_gnb;
static int           g_gnb_nant;
static _Atomic uint64_t g_try, g_crc_ok, g_rej_unsup, g_rej_setup;

/* ------------------------------------------------------------------------------------------
 * Minimal gNB context. Deliberately NOT phy_init_nr_gNB(): that allocates PRACH, SRS, PUCCH, the
 * DLSCH side and a set of queues, asserts on config this receiver has no business filling in, and
 * would drag most of PHY_NR into the link for buffers nothing here touches. Only the fields the
 * PUSCH receive chain actually reads are built, each one traceable to where it is read.
 * ------------------------------------------------------------------------------------------ */
static bool passive_gnb_prepare(PHY_VARS_NR_UE *ue)
{
  if (g_gnb != NULL) {
    return true;
  }
  const NR_DL_FRAME_PARMS *ufp = &ue->frame_parms;
  const int nant = (ufp->nb_antennas_rx < PASSIVE_UL_MAX_ANT) ? ufp->nb_antennas_rx : PASSIVE_UL_MAX_ANT;

  PHY_VARS_gNB *gnb = (PHY_VARS_gNB *)calloc(1, sizeof(PHY_VARS_gNB));
  if (gnb == NULL) {
    return false;
  }
  /* The two structs are the same type, so the uplink numerology, CP, FFT size and carrier offset
   * are inherited exactly from what the receiver is already synced to -- which is the point: a
   * passive receiver must demodulate the uplink on the SAME grid it demodulates the downlink on,
   * not on one derived independently. */
  gnb->frame_parms = *ufp;
  gnb->frame_parms.N_RB_UL = ufp->N_RB_DL;

  gnb->gNB_config.carrier_config.num_rx_ant.value = nant;
  gnb->gNB_config.cell_config.phy_cell_id.value   = ufp->Nid_cell;
  gnb->max_nb_pusch                = 1;
  gnb->max_ldpc_iterations         = 8;
  gnb->num_pusch_symbols_per_thread = 1;
  gnb->dmrs_num_antennas_per_thread = 1;
  gnb->chest_time                  = 0;
  gnb->chest_freq                  = 0;
  gnb->enable_analog_das           = 0;
  gnb->common_vars.num_beams_period = 1;
  gnb->pusch_thres                 = 0;
  /* The LDPC interface IS shared: it is a dlopen'd shared library and a second handle would not
   * share its internal state. The THREAD POOL is not -- tpool_t holds a pthread_barrier_t, so
   * copying the UE's by value would duplicate barrier state rather than share the pool. Give this
   * receiver its own small pool instead, which also keeps the decode off the UE's RT pool: this
   * tree has already measured blind PDSCH decoding on the receive thread costing PBCH lock. */
  gnb->nrLDPC_coding_interface     = ue->nrLDPC_coding_interface;
  initFloatingCoresTpool(2, &gnb->threadPool, false, "passiveUL-tpool");

  const int symsz = ufp->ofdm_symbol_size;
  const int sps   = ufp->symbols_per_slot;

  /* rxdataF is a RING of RU_RX_SLOT_DEPTH slots: the UL chest indexes it as
   * (Ns % RU_RX_SLOT_DEPTH) * symbols_per_slot * ofdm_symbol_size + symbol * ofdm_symbol_size. */
  gnb->common_vars.rxdataF = (c16_t **)calloc(nant, sizeof(c16_t *));
  if (gnb->common_vars.rxdataF == NULL) {
    free(gnb);
    return false;
  }
  for (int a = 0; a < nant; a++) {
    gnb->common_vars.rxdataF[a] = (c16_t *)calloc((size_t)RU_RX_SLOT_DEPTH * sps * symsz, sizeof(c16_t));
    if (gnb->common_vars.rxdataF[a] == NULL) {
      free(gnb);
      return false;
    }
  }

  const int max_layers = NR_MAX_NB_LAYERS;
  const int n_buf      = nant * max_layers;
  const int nb_re      = gnb->frame_parms.N_RB_UL * NR_NB_SC_PER_RB;
  const int nb_re2     = ((nb_re + 15) / 16) * 16;

  gnb->pusch_vars = (NR_gNB_PUSCH *)calloc(1, sizeof(NR_gNB_PUSCH));
  NR_gNB_PUSCH *pv = &gnb->pusch_vars[0];
  pv->ul_ch_estimates     = (int32_t **)calloc(n_buf, sizeof(int32_t *));
  pv->ptrs_phase_per_slot = (int32_t **)calloc(n_buf, sizeof(int32_t *));
  for (int i = 0; i < n_buf; i++) {
    pv->ul_ch_estimates[i]     = (int32_t *)calloc((size_t)symsz * sps, sizeof(int32_t));
    pv->ptrs_phase_per_slot[i] = (int32_t *)calloc(sps, sizeof(int32_t));
  }
  pv->rxdataF_comp = (c16_t **)calloc(max_layers, sizeof(c16_t *));
  for (int i = 0; i < max_layers; i++) {
    pv->rxdataF_comp[i] = (c16_t *)calloc((size_t)nb_re2 * sps, sizeof(c16_t));
  }
  /* Same size expression nr_init.c uses. It is not derived from anything here; copied deliberately
   * so the two cannot diverge. */
  pv->llr = (int16_t *)calloc(8 * ((3 * 8 * 6144) + 12), sizeof(int16_t));
  pv->ul_valid_re_per_slot = (int16_t *)calloc(sps, sizeof(int16_t));

  gnb->ulsch = (NR_gNB_ULSCH_t *)calloc(1, sizeof(NR_gNB_ULSCH_t));
  gnb->ulsch[0] = new_gNB_ulsch(gnb->max_ldpc_iterations, gnb->frame_parms.N_RB_UL);

  g_gnb       = gnb;
  g_gnb_nant  = nant;
  LOG_I(PHY,
        "SENSING: passive PUSCH receiver ready (N_RB_UL=%d ant=%d fft=%d sps=%d pci=%d)\n",
        gnb->frame_parms.N_RB_UL, nant, symsz, sps, ufp->Nid_cell);
  return true;
}

void nr_pusch_passive_decode_free(void)
{
  if (g_gnb == NULL) {
    return;
  }
  /* Deliberately a shallow teardown at process exit only: the buffers above are freed by the OS,
   * and free_gNB_ulsch()'s partner allocations are inside a struct this file did not fully build. */
  g_gnb = NULL;
}

/* Fill the FAPI PUSCH PDU from a recovered UL grant. Everything here either came from the DCI or
 * from the deployment config carried alongside it -- nothing is invented. */
static void fill_pusch_pdu(const nr_pdcch_blind_ul_result_t *g, int nant, nfapi_nr_pusch_pdu_t *p)
{
  memset(p, 0, sizeof(*p));
  p->pdu_bit_map        = PUSCH_PDU_BITMAP_PUSCH_DATA;
  p->rnti               = g->rnti;
  p->bwp_start          = g->bwp_start;
  p->bwp_size           = g->bwp_size;
  p->subcarrier_spacing = 1;   // mu = 1 (30 kHz); this monitor runs at one numerology
  p->cyclic_prefix      = 0;

  p->mcs_index          = g->mcs;
  p->mcs_table          = g->mcs_table;
  p->qam_mod_order      = nr_get_Qm_ul(g->mcs, g->mcs_table);
  p->target_code_rate   = nr_get_code_rate_ul(g->mcs, g->mcs_table);
  p->transform_precoding = g->transform_precoding ? 0 : 1; // enum: 0 = enabled, 1 = disabled
  p->data_scrambling_id = g->data_scrambling_id;
  p->nrOfLayers         = g->nrOfLayers;

  p->ul_dmrs_symb_pos   = g->ul_dmrs_symb_pos;
  p->dmrs_config_type   = g->dmrs_config_type;
  p->ul_dmrs_scrambling_id = g->ul_dmrs_scrambling_id;
  p->pusch_identity     = g->ul_dmrs_scrambling_id;
  p->scid               = g->nscid;
  p->num_dmrs_cdm_grps_no_data = g->n_dmrs_cdm_groups;
  p->dmrs_ports         = g->dmrs_ports;

  p->resource_alloc     = 1;   // type 1 -- the only type this deployment schedules
  p->rb_start           = g->start_rb;
  p->rb_size            = g->num_rb;
  p->vrb_to_prb_mapping = 0;
  p->frequency_hopping  = g->frequency_hopping;

  p->start_symbol_index = g->start_symbol;
  p->nr_of_symbols      = g->num_symbols;

  p->pusch_data.rv_index          = g->rv;
  p->pusch_data.harq_process_id   = g->harq_pid;
  p->pusch_data.new_data_indicator = g->ndi;

  p->maintenance_parms_v3.ldpcBaseGraph = 0; // filled below once the TBS is known
  p->param_v4.numSpatialStreamIndices   = 0;
  p->beamforming.num_prgs               = 0;
  p->beamforming.dig_bf_interface       = nant;
}

bool nr_pusch_passive_decode(PHY_VARS_NR_UE *ue,
                             uint32_t frame,
                             uint8_t  slot,
                             const nr_pdcch_blind_ul_result_t *g,
                             int32_t  ta_offset_samples,
                             nr_pusch_passive_out_t *out)
{
  memset(out, 0, sizeof(*out));
  out->status = NR_PUSCH_PASSIVE_ERROR;

  if (ue == NULL || g == NULL || !g->plausible) {
    out->reject_reason = "no plausible grant";
    return false;
  }
  /* Scope guards, each a case this receiver cannot do correctly rather than one it merely has not
   * been tested on. Silently attempting any of them would produce a confident wrong answer. */
  if (g->nrOfLayers != 1) {
    atomic_fetch_add_explicit(&g_rej_unsup, 1, memory_order_relaxed);
    out->status = NR_PUSCH_PASSIVE_UNSUPPORTED;
    out->reject_reason = "multi-layer PUSCH";
    return false;
  }
  if (g->transform_precoding) {
    atomic_fetch_add_explicit(&g_rej_unsup, 1, memory_order_relaxed);
    out->status = NR_PUSCH_PASSIVE_UNSUPPORTED;
    out->reject_reason = "DFT-s-OFDM (transform precoding) not wired";
    return false;
  }
  if (g->rv != 0) {
    /* No HARQ history: a passive receiver has no earlier redundancy version to combine with, so an
     * rv != 0 transmission carries only incremental parity and cannot be decoded standalone. The
     * DM-RS-based CFR path is unaffected -- it needs only the allocation. */
    atomic_fetch_add_explicit(&g_rej_unsup, 1, memory_order_relaxed);
    out->status = NR_PUSCH_PASSIVE_UNSUPPORTED;
    out->reject_reason = "rv != 0 with no HARQ history to combine";
    return false;
  }
  if (!passive_gnb_prepare(ue)) {
    atomic_fetch_add_explicit(&g_rej_setup, 1, memory_order_relaxed);
    out->reject_reason = "gNB context allocation failed";
    return false;
  }

  PHY_VARS_gNB *gnb = g_gnb;
  NR_DL_FRAME_PARMS *fp = &gnb->frame_parms;
  const int nant = g_gnb_nant;
  const int symsz = fp->ofdm_symbol_size;
  const int sps   = fp->symbols_per_slot;

  /* ---- FEP the PUSCH's own symbols into the gNB grid, with the uplink timing advance. ----
   * nr_slot_fep()'s sample_offset is an unsigned quantity ADDED to the window position, but the
   * uplink arrives EARLY, so the correction is negative. Express it modulo the rxdata ring instead
   * of passing a negative number: the FEP wraps its reads against the same total, so an offset of
   * (total - advance) lands exactly where (-advance) would. */
  const uint32_t total_rx = (uint32_t)(fp->samples_per_frame);
  uint32_t sample_offset = 0;
  if (ta_offset_samples != 0) {
    int32_t off = ta_offset_samples % (int32_t)total_rx;
    sample_offset = (uint32_t)((off <= 0) ? (int32_t)total_rx + off : off);
    if (sample_offset == total_rx) {
      sample_offset = 0;
    }
  }
  const int slot_off = (slot % RU_RX_SLOT_DEPTH) * sps * symsz;
  {
    /* nr_slot_fep writes a [antenna][symbols_per_slot * samples_per_slot_wCP] shaped buffer; the
     * gNB grid is [antenna][RU_RX_SLOT_DEPTH * symbols_per_slot * ofdm_symbol_size]. Fill one
     * symbol at a time through a scratch view so the two layouts stay explicit rather than
     * assumed equal -- they are not. */
    c16_t (*scratch)[fp->samples_per_slot_wCP] =
        (c16_t (*)[fp->samples_per_slot_wCP])calloc(nant, sizeof(c16_t) * fp->samples_per_slot_wCP);
    if (scratch == NULL) {
      atomic_fetch_add_explicit(&g_rej_setup, 1, memory_order_relaxed);
      out->reject_reason = "scratch allocation failed";
      return false;
    }
    const int s0 = g->start_symbol;
    const int s1 = g->start_symbol + g->num_symbols;
    for (int sym = s0; sym < s1 && sym < sps; sym++) {
      nr_slot_fep(ue, fp, slot, sym, scratch, link_type_ul, sample_offset, ue->common_vars.rxdata);
      for (int a = 0; a < nant; a++) {
        memcpy(&gnb->common_vars.rxdataF[a][slot_off + sym * symsz], &scratch[a][sym * symsz],
               (size_t)symsz * sizeof(c16_t));
      }
    }
    free(scratch);
  }

  /* ---- TBS, then the receive chain, exactly as the gNB runs it. ---- */
  nfapi_nr_pusch_pdu_t pdu;
  fill_pusch_pdu(g, nant, &pdu);

  const int n_dmrs_sym = __builtin_popcount((unsigned)g->ul_dmrs_symb_pos
                                            & (((1u << g->num_symbols) - 1u) << g->start_symbol));
  const int nb_dmrs_re_per_rb = ((g->dmrs_config_type == 0) ? 6 : 4) * g->n_dmrs_cdm_groups;
  const uint32_t tbs = nr_compute_tbs(pdu.qam_mod_order, pdu.target_code_rate, g->num_rb, g->num_symbols,
                                      nb_dmrs_re_per_rb * n_dmrs_sym, 0, 0, g->nrOfLayers);
  if (tbs == 0) {
    out->status = NR_PUSCH_PASSIVE_UNSUPPORTED;
    out->reject_reason = "TBS computed as zero";
    return false;
  }
  pdu.pusch_data.tb_size = tbs >> 3;
  pdu.maintenance_parms_v3.ldpcBaseGraph = get_BG(tbs, pdu.target_code_rate);

  NR_gNB_ULSCH_t *ulsch = &gnb->ulsch[0];
  ulsch->rnti     = g->rnti;
  ulsch->frame    = frame;
  ulsch->slot     = slot;
  ulsch->harq_pid = 0;
  ulsch->active   = true;
  ulsch->harq_process->ulsch_pdu = pdu;
  ulsch->harq_process->harq_to_be_cleared = true;
  ulsch->unav_res = 0;

  NR_gNB_PUSCH *pvp = &gnb->pusch_vars[0];
  const nfapi_nr_pusch_pdu_t *pdup = &ulsch->harq_process->ulsch_pdu;
  uint32_t *unavp = &ulsch->unav_res;
  atomic_fetch_add_explicit(&g_try, 1, memory_order_relaxed);
  nr_rx_pusch_group_tp(gnb, &pvp, &pdup, &unavp, 1, frame, slot);

  int ulsch_id = 0;
  const int rc = nr_ulsch_decoding(gnb, fp, frame, slot, &ulsch_id, 1);

  out->G             = nr_get_G(g->num_rb, g->num_symbols, nb_dmrs_re_per_rb, n_dmrs_sym, 0,
                                pdu.qam_mod_order, g->nrOfLayers);
  out->qam_mod_order = pdu.qam_mod_order;
  out->nb_rb         = g->num_rb;
  out->nb_symbols    = g->num_symbols;
  out->tbs_bytes     = tbs >> 3;
  if (pvp->ulsch_noise_power_tot > 0) {
    out->snr_db = 10.0f * log10f((float)pvp->ulsch_power_tot / (float)pvp->ulsch_noise_power_tot);
  }

  if (rc == 0 && ulsch->harq_process->b != NULL) {
    out->status = NR_PUSCH_PASSIVE_OK;
    out->tb     = ulsch->harq_process->b;
    atomic_fetch_add_explicit(&g_crc_ok, 1, memory_order_relaxed);
    return true;
  }
  out->status = NR_PUSCH_PASSIVE_CRC_FAIL;
  out->reject_reason = "LDPC/CRC failed";
  return false;
}

void nr_pusch_passive_stats_dump(void)
{
  const uint64_t t = atomic_load_explicit(&g_try, memory_order_relaxed);
  const uint64_t k = atomic_load_explicit(&g_crc_ok, memory_order_relaxed);
  LOG_I(PHY,
        "SENSING: pusch_passive[try=%lu crc_ok=%lu (%.1f%%) unsup=%lu setup_fail=%lu]\n",
        (unsigned long)t, (unsigned long)k, t ? (100.0 * (double)k / (double)t) : 0.0,
        (unsigned long)atomic_load_explicit(&g_rej_unsup, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&g_rej_setup, memory_order_relaxed));
}
