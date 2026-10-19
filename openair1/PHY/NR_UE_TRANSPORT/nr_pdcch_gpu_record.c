#include "nr_pdcch_gpu_record.h"

/* The PDCCH GPU dump is gone (diagnostic); the call site in nr_pdcch_blind_monitor_rt.c and this stub are to be removed together. */
void nr_pdcch_gpu_record_cpu(const PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc,
                              const fapi_nr_dl_config_dci_dl_pdu_rel15_t *pdcch, int first_symbol,
                              int n_rb, int cset_start, const c16_t *rxF, int rxF_stride,
                              const c16_t *cpu_llr, uint32_t llr_re, uint32_t absolute_slot)
{
  (void)ue; (void)proc; (void)pdcch; (void)first_symbol; (void)n_rb; (void)cset_start;
  (void)rxF; (void)rxF_stride; (void)cpu_llr; (void)llr_re; (void)absolute_slot;
}
