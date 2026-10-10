/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPU-DEMOD host API, see nr_pdsch_gpu_demod_host.c. */
#ifndef NR_PDSCH_GPU_DEMOD_HOST_H
#define NR_PDSCH_GPU_DEMOD_HOST_H
#include "PHY/defs_nr_UE.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.h"

/* Every argument the passive decoder's symbol loop hands nr_rx_pdsch(), plus the loop's own controls. */
typedef struct {
  PHY_VARS_NR_UE *ue;
  const UE_nr_rxtx_proc_t *proc;
  NR_UE_DLSCH_t *dlsch;
  const freq_alloc_bitmap_t *freq_alloc;
  fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg;
  NR_DL_UE_HARQ_t *harq;
  int first_symbol;     /* the loop's first_symbol_with_data */
  int probe_last_sym;   /* the loop's probe horizon (-1 = full grant) */
  uint32_t est_size;    /* pdsch_est_size */
  int32_t *est;         /* pdsch_dl_ch_estimates */
  int16_t *llr;
  uint32_t llr_cap;     /* int16 entries in llr */
  uint32_t *dl_valid_re;
  c16_t *rxdataF;       /* rxdataF_dem */
  int32_t *log2_maxh;
  uint32_t buf;         /* rx_size_symbol of the caller (comp/mag row stride) */
  c16_t *comp, *mag, *magb, *magr;
  c16_t *ptrs_phase;    /* [nb_antennas_rx][NR_SYMBOLS_PER_SLOT] */
  int32_t *ptrs_re;     /* [nb_antennas_rx][NR_SYMBOLS_PER_SLOT] */
  uint32_t nvar;
  pdsch_scope_req_t *scope;
  void *rho;
  const nr_ssb_rm_mask_t *ssb_rm;
} nr_pdsch_gpu_demod_call_t;

/* Module warm-up (before the RX fence); 1 when ISAC_PDSCH_GPU_EXACT_DEMOD=1 is active. */
int nr_pdsch_gpu_demod_warmup(const NR_DL_FRAME_PARMS *fp);
/* 0: the grant is demodulated (exactly what the CPU loop would have produced); -1: the caller runs its CPU loop. */
int nr_pdsch_gpu_demod_grant(const nr_pdsch_gpu_demod_call_t *c);
/* GPU-DEMOD stage 2 (nr_dl_channel_estimation.c): every (DM-RS symbol in dmrs_mask, layer) estimate of a grant on the
 * GPU; nvar_out / nvar_ant_out = what each nr_pdsch_channel_estimation() call would return / publish. segs != NULL: the
 * PRG/VRB segmented path (estimate rows assembled in data order, width-weighted nvar). 0 = done. */
int nr_pdsch_gpu_chest_grant(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch,
                             const freq_alloc_bitmap_t *freq_alloc, const nr_prb_seg_t *segs, int nseg, int Nl, uint16_t dmrs_mask,
                             uint32_t pdsch_est_size,
                             int32_t dl_ch_estimates[][pdsch_est_size], int rxdataFsize, c16_t rxdataF[][rxdataFsize], int diag,
                             uint32_t nvar_out[14][4], uint32_t nvar_ant_out[14][4][8]);
#endif
