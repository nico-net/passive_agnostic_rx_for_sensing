#ifndef NR_PDSCH_GPU_RECORD_H
#define NR_PDSCH_GPU_RECORD_H

#include "nr_pdsch_gpu_fep.h"
#include <stdint.h>

/* On-disk format for ISAC_PDSCH_GPU_DUMP. Each record contains one CPU decode and
 * the complete slot IQ needed to replay its GPU front end independently. */
#define NR_PDSCH_GPU_RECORD_MAGIC 0x47504432u
typedef struct {
  uint32_t magic, bytes, version, absolute_slot;
  nr_gpu_fep_cfg_t cfg;
  nr_gpu_pdsch_job_t job;
  uint32_t iq_samples_per_ant, llr_count, rnti, data_scrambling_id;
  uint32_t crc_ok, evm_valid;
  float cpu_evm_pct;
  double fo_hz;
  uint32_t abs_sample, early;
  uint32_t rxF_symbol; /* version 2: first DM-RS symbol, CPU c16 rxdataF for every antenna */
  /* payload: rotation, IQ, CPU descrambled LLR, then rxdataF[ant][ofdm_symbol_size] c16. */
} nr_pdsch_gpu_record_t;

#endif
