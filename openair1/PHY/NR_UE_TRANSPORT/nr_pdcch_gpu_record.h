#ifndef NR_PDCCH_GPU_RECORD_H
#define NR_PDCCH_GPU_RECORD_H
#include "nr_pdcch_gpu_fep.h"
#include <stdint.h>

#define NR_PDCCH_GPU_RECORD_MAGIC 0x47435032u
typedef struct {
  uint32_t magic, bytes, version, absolute_slot;
  nr_gpu_pdcch_fep_cfg_t cfg;
  nr_gpu_pdcch_job_t job;
  uint32_t llr_count;
  /* payload: IQ[ant][duration][N] c16, then CPU LLR[llr_count] int16. */
} nr_pdcch_gpu_record_t;

#ifndef NR_PDCCH_GPU_RECORD_FORMAT_ONLY
#include "PHY/defs_nr_UE.h"
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"
void nr_pdcch_gpu_record_cpu(const PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc,
                              const fapi_nr_dl_config_dci_dl_pdu_rel15_t *pdcch, int first_symbol,
                              int n_rb, int cset_start, const c16_t *rxF, int rxF_stride,
                              const c16_t *cpu_llr, uint32_t llr_re, uint32_t absolute_slot);
#endif
#endif
