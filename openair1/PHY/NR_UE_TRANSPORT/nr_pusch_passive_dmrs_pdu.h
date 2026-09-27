#ifndef NR_PUSCH_PASSIVE_DMRS_PDU_H
#define NR_PUSCH_PASSIVE_DMRS_PDU_H

#include "nfapi_nr_interface_scf.h"
#include "nr_pdcch_blind_monitor.h"

/* TP support is restricted to the PCI-default nPUSCH-Identity hypothesis with
 * group/sequence hopping disabled (TS 38.211 6.4.1.1.1.2). PCI comes from cell
 * acquisition, never a gNB config. Explicit nPUSCH-Identity/hopping discovery
 * remains unsupported; CRC feedback must validate this default hypothesis.
 * CP-OFDM scramblingID0/1 is a separate identity and cannot seed the TP group.
 * Shared with the offline PDU-contract test; used by the production decoder. */
static inline void nr_pusch_passive_fill_dmrs_pdu(const nr_pdcch_blind_ul_result_t *g,
                                                 uint16_t measured_pci,
                                                 nfapi_nr_pusch_pdu_t *p)
{
  p->ul_dmrs_symb_pos = g->ul_dmrs_symb_pos;
  p->dmrs_config_type = g->dmrs_config_type;
  p->ul_dmrs_scrambling_id = g->ul_dmrs_scrambling_id;
  p->pusch_identity = measured_pci;
  p->scid = g->nscid;
  p->num_dmrs_cdm_grps_no_data = g->n_dmrs_cdm_groups;
  p->dmrs_ports = g->dmrs_ports;
  p->dfts_ofdm.low_papr_group_number = (uint8_t)(p->pusch_identity % 30);
  p->dfts_ofdm.low_papr_sequence_number = 0;
}

#endif
