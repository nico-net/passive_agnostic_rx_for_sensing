/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Traffic-mode configuration references: per cell, per RNTI version of the configuration a DCI is interpreted with. */
#ifndef NR_TRAFFIC_CFG_H
#define NR_TRAFFIC_CFG_H
#include "nr_pdcch_dci11_layout_sweep.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Callers must zero-initialise the whole struct first: the version hash covers its bytes (padding included). */
typedef struct {
  uint32_t identity_gen, dedicated_epoch;
  int16_t coreset_id, coreset_start_rb, coreset_n_rb; int8_t coreset_dur;
  int16_t bwp_start, bwp_size;
  int16_t layout_rep;              /* nr_pdsch_solver_rep(); -1 none */
  nr_dci11_layout_t layout;        /* zero when layout_rep < 0 */
  int8_t s_by_tda[16], l_by_tda[16], dmrs_add_pos, mcs_table, rm_set;
  int8_t layout_unproven;          /* E3: the representative is a layout solved by DM-RS physics and not hq_trusted / CRC-confirmed: layout_rep and layout print null with layout_evidence "phys" */
} nr_traffic_cfg_snap_t;
/* Per cell, per (RNTI, dci_format) (rnti 0 = cell-wide). Returns the version for this snapshot: a per-cell monotonic counter value (first = 1),
 * so (rnti, cfg_ref) is unique within a cell identity. On change takes a new version and emits one
 * {"type":"config","rnti","dci_format","cfg_ref", every snapshot field} record, before any DCI record that uses it. */
uint32_t nr_traffic_cfg_ref(uint16_t rnti, uint8_t dci_format, const nr_traffic_cfg_snap_t *s);
void nr_traffic_cfg_reset(void);   /* this cell's table, on HARD_RESET epochs */
#ifdef __cplusplus
}
#endif
#endif
