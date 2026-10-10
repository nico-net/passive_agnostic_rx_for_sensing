#ifndef NR_PDSCH_GPU_STAGE_H
#define NR_PDSCH_GPU_STAGE_H
#include <stdint.h>
/* v3 record trailer: row count, then (header, LS[count] c16, FIR[count] c16).
 * Data are before branch selection/alignment and before equalisation. */
typedef struct {
  uint32_t recovered, mrc_mode, mmse_mode, decoder, csi_count, ptrs, noise;
} nr_pdsch_gpu_stage_meta_t;
typedef struct {
  uint32_t symbol, layer, antenna, count;
  int32_t delay;
  uint32_t noise;
} nr_pdsch_gpu_stage_row_t;
#ifdef __cplusplus
extern "C" {
#endif
void nr_pdsch_gpu_stage_context(uint32_t epoch, int symbol, int layer, int antenna) __attribute__((weak));
void nr_pdsch_gpu_stage_chest(const void *ls, const void *fir, int count, int delay, uint32_t noise) __attribute__((weak));
#ifdef __cplusplus
}
#endif
#endif
