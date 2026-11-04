/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Traffic mode: DAI / TPC / PUCCH resource indicator / PDSCH-to-HARQ timing (K1) out of a DL DCI payload. Pure. */
#ifndef NR_TRAFFIC_DCI_FIELDS_H
#define NR_TRAFFIC_DCI_FIELDS_H
#include <stdbool.h>
#include <stdint.h>
#include "nr_dci_bits.h"
#ifdef __cplusplus
extern "C" {
#endif
#include "nr_pdcch_dci11_layout_sweep.h"
#include "nr_dci11_prove.h"
typedef struct { int8_t dai, tpc, pri, k1; } nr_dci_harq_fields_t;  /* -1 unknown */
/* DCI 1_0 PDSCH-to-HARQ-ACK slot (TS 38.213 9.2.3: field 0..7 -> k = 1..8 slots after the PDSCH slot). -1 when either input is unknown. */
static inline int64_t nr_dci10_harq_fb_slot(int64_t pdsch_abs_slot, int k1_field)
{
  return pdsch_abs_slot < 0 || k1_field < 0 || k1_field > 7 ? -1 : pdsch_abs_slot + k1_field + 1;
}
/* DCI 1_0 C-RNTI/TC-RNTI (TS 38.212 7.3.1.2.1): identifier 1, FDRA(fdra_bits), TDRA 4, VRB 1, MCS 5, NDI 1, RV 2, HARQ 4,
 * DAI 2, TPC 2, PRI 3, K1 3. false (all -1) if dci_len is shorter than that. */
bool nr_dci10_harq_fields(nr_dci_bits_t payload, uint16_t dci_len, uint16_t fdra_bits, nr_dci_harq_fields_t *out);
/* DCI 1_1: pre_ant (tb2 + harq + dai + tpc 2 + 2nd tpc + pri 3 + k1) is a sum; the fields are returned only when exactly one split
 * exists under the sweep's switch tables counting the Rel-16/17 extras (one-shot, group idx, NFI, n groups, retx, enh. type 3) as free bits. Otherwise false and out all -1 (never a guess). */
bool nr_dci11_harq_fields(nr_dci_bits_t payload, uint16_t dci_len, const nr_dci11_layout_t *l,
                          const nr_dci11_offsets_t *off, nr_dci_harq_fields_t *out);
/* Same, but a split proven from bits by nr_dci11_prove() (E1) is read directly when `proven` is non-NULL (false and all -1 if it does not add up
 * to l->pre_ant); NULL = exactly the unique-split rule above. */
bool nr_dci11_harq_fields_proven(nr_dci_bits_t payload, uint16_t dci_len, const nr_dci11_layout_t *l,
                                 const nr_dci11_offsets_t *off, const nr_dci11_split_t *proven, nr_dci_harq_fields_t *out);
#ifdef __cplusplus
}
#endif
#endif
