/* SSB rate matching for the passive PDSCH decoder (TS 38.214 5.1.4).
 * The SSB of the CURRENT slot is observed (PSS x SSS of the acquired PCI on that slot's own FFT
 * output); it is never projected from an acquisition event, a presumed period or SIB1. These are
 * the functions nr_pdsch_passive_decode() calls; tests/nr_ssb_rate_match_prod_check.c drives them
 * together with nr_rx_pdsch() end to end. */
#include <string.h>
#include "PHY/defs_nr_UE.h"
#include "PHY/INIT/nr_phy_init.h"
#include "nr_transport_proto_ue.h"

uint16_t nr_ssb_rm_candidates(const NR_DL_FRAME_PARMS *fp, int slot, int start_symbol, int nb_symbols)
{
  // Only same-SCS, slot-contained SSB candidates are represented by this PHY.
  if (fp->ssb_start_subcarrier < 0 || fp->ssb_start_subcarrier + 239 >= fp->N_RB_DL * NR_NB_SC_PER_RB
      || fp->slots_per_frame < 10 || fp->symbols_per_slot != 14
      || (fp->numerology_index != 0 && fp->numerology_index != 1))
    return 0;
  uint16_t candidates = 0;
  for (int i = 0; i < fp->Lmax; ++i) {
    const int start = nr_get_ssb_start_symbol(fp, i);
    const int s = start % 14;
    // Every candidate position the grant overlaps in TIME. Frequency is left to the mask, so the
    // observation (and its log line) also happens when the scheduler kept the grant off the SSB PRBs.
    if (start / 14 == slot % (fp->slots_per_frame / 2) && s <= 10 && s + 3 >= start_symbol
        && s < start_symbol + nb_symbols)
      candidates |= 1u << s;
  }
  return candidates;
}

nr_ssb_rm_event_t nr_ssb_rm_observe(const NR_DL_FRAME_PARMS *fp,
                                    int frame,
                                    int slot,
                                    uint16_t candidates,
                                    c16_t rxdataF[][fp->samples_per_slot_wCP])
{
  nr_ssb_rm_event_t e = nr_ssb_rm_event(frame, slot, fp->Nid_cell, fp->ssb_start_subcarrier, 0);
  for (int s = 0; s <= 10; ++s) {
    if (!((candidates >> s) & 1))
      continue;
    for (int ant = 0; ant < fp->nb_antennas_rx; ++ant) {
      int16_t pss[254], sss[254];
      for (int n = 0; n < 127; ++n) {
        // PSS/SSS occupy SSB subcarriers 56..182 (TS 38.211 Table 7.4.3.1-1).
        const int bin = (fp->first_carrier_offset + fp->ssb_start_subcarrier + 56 + n) % fp->ofdm_symbol_size;
        const c16_t p = rxdataF[ant][s * fp->ofdm_symbol_size + bin];
        const c16_t q = rxdataF[ant][(s + 2) * fp->ofdm_symbol_size + bin];
        pss[2 * n] = p.r;
        pss[2 * n + 1] = p.i;
        sss[2 * n] = q.r;
        sss[2 * n + 1] = q.i;
      }
      if (nr_ssb_rm_sync_present(pss, sss, fp->Nid_cell)) {
        e.symbols |= 0xfu << s;
        break;
      }
    }
  }
  return e;
}

bool nr_ssb_rm_plan(const nr_ssb_rm_event_t *e,
                    int frame,
                    int slot,
                    int pci,
                    uint16_t rnti,
                    fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg,
                    const freq_alloc_bitmap_t *fa,
                    const nr_prb_seg_t *seg,
                    int nseg,
                    nr_ssb_rm_plan_t *p)
{
  memset(p, 0, sizeof(*p));
  p->physical = nr_ssb_rm_map(e, frame, slot, pci, cfg->BWPStart, cfg->BWPSize, NULL);
  for (int m = cfg->start_symbol; m < cfg->start_symbol + cfg->number_symbols; ++m) {
    if (!((p->physical.symbols >> m) & 1))
      continue;
    const uint32_t csi = nr_dlsch_csi_overlap_bitmap(cfg, m);
    uint32_t n = 0;
    for (int rb = 0; rb < cfg->BWPSize; ++rb) {
      if (!((fa->bitmap[rb / 32] >> (rb % 32)) & 1))
        continue;
      const uint32_t csi_rb = ((rb + cfg->BWPStart) & 1) ? (csi >> 16) & 0xfff : csi & 0xfff;
      n += __builtin_popcount(nr_ssb_rm_excluded(&p->physical, m, rb) & ~csi_rb); // CSI-RS already in csi_unav
    }
    // SI-RNTI lacks its SI indicator here; a DM-RS or PT-RS RE on an SSB RE is not a valid PDSCH.
    if (n && (rnti == 0xffff || ((cfg->dlDmrsSymbPos >> m) & 1) || (cfg->pduBitmap & 1)))
      return false;
    p->unav += n;
  }
  p->dem = p->physical;
  if (p->unav && seg && nseg > 0) {
    // Segmented/interleaved grants are demodulated as a virtual allocation in DATA order.
    uint16_t physical[NR_PRB_SET_MAX] = {0};
    for (int s = 0; s < nseg; ++s) {
      if (seg[s].data_index + seg[s].n_prb > NR_PRB_SET_MAX)
        return false;
      for (int rb = 0; rb < seg[s].n_prb; ++rb)
        physical[seg[s].data_index + rb] = seg[s].prb_start + rb;
    }
    p->dem = nr_ssb_rm_map(e, frame, slot, pci, cfg->BWPStart, fa->num_rbs, physical);
  }
  return true;
}

int nr_ssb_rm_first_data_symbol(const fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg,
                                const freq_alloc_bitmap_t *fa,
                                const nr_ssb_rm_plan_t *p)
{
  // Same "first symbol carrying data" rule as nr_ue_pdsch_procedures(), plus symbols the SSB empties.
  const int dmrs_data_re = cfg->dmrsConfigType == NFAPI_NR_DMRS_TYPE1 ? 12 - 6 * cfg->n_dmrs_cdm_groups
                                                                      : 12 - 4 * cfg->n_dmrs_cdm_groups;
  bool ssb_all_prbs = p->unav > 0;
  for (int rb = 0; ssb_all_prbs && rb < cfg->BWPSize; ++rb)
    if (((fa->bitmap[rb / 32] >> (rb % 32)) & 1) && !p->physical.prb[rb])
      ssb_all_prbs = false;
  int m = cfg->start_symbol;
  while (m < cfg->start_symbol + cfg->number_symbols
         && ((dmrs_data_re == 0 && ((cfg->dlDmrsSymbPos >> m) & 1)) || (ssb_all_prbs && ((p->physical.symbols >> m) & 1))))
    m++;
  return m;
}
