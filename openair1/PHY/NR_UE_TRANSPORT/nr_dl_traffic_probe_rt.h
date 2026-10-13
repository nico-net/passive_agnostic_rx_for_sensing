/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* RT feed of the DL traffic probe (nr_dl_traffic_probe.h) and of the blind Point-A sweep.
 * ISAC_DL_TRAFFIC_PROBE=N (default off): every N-th DL slot the RX actor copies antenna 0 of the slot (CP stripped) to a
 *   one-slot mailbox; a SCHED_OTHER worker FEPs it (FO compensation, FFT, symbol rotation as nr_csirs_disc_td.c) and
 *   accumulates nr_dltp_slot(). A TRAFFIC line is logged every 256 probed slots.
 * ISAC_POINTA_SWEEP=1 (default off, needs the probe): on a cell whose carrier geometry is unknown (no SIB1, or an NSA
 *   SIB1 without servingCellConfigCommon) the worker correlates symbols 0-2 of every probed slot against the PDCCH DM-RS
 *   of nID = PCI (TS 38.211 7.4.1.3.1, the sequence index runs from CRB 0) for every Point-A hypothesis (grid RB of CRB 0 from
 *   -274 to n_rb - 6, i.e. CRB 0 below the grid or inside it), on the 6-RB group grid of that hypothesis, CORESET#0 and SSB RBs masked. A 6-RB window counts
 *   as lit above 4x the pure-noise mean |corr| (0.209 for 18 pilots; the nr_pdcch_coreset_map bar). The hypothesis with
 *   >= 6 lit windows and >= 4x the runner-up is published as the carrier hypothesis (Point A = 12 x that grid RB,
 *   carrier = the grid), which opens the GPU nID sweep and the dwell path. With the geometry known the same correlation
 *   runs at the known reference only and feeds the TRAFFIC line (pdcch_pci_windows). Requires the receiver grid aligned
 *   to the CRB grid (ISAC_KSSB_ALIGN=1; the advisory KSSB_GRID line says when it is not). */
#ifndef NR_DL_TRAFFIC_PROBE_RT_H
#define NR_DL_TRAFFIC_PROBE_RT_H
#include "PHY/defs_nr_UE.h"
void nr_dl_traffic_probe_push(PHY_VARS_NR_UE *ue, uint32_t abs_slot, uint16_t frame, uint8_t slot);
#endif
