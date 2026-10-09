#ifndef NR_PDCCH_SCAN_GPU_REF_H
#define NR_PDCCH_SCAN_GPU_REF_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The uncalibrated GPU FEP may only replace a CPU LLR if every soft bit
 * agrees. False means that cpu_llr remains untouched. */
static inline bool nr_pdcch_scan_gpu_reference_accept(void *cpu_llr, const void *gpu_llr,
                                                       size_t bytes, int64_t got_words,
                                                       uint32_t job_words)
{
  const size_t words = bytes / sizeof(int16_t);
  if (bytes % sizeof(int16_t) || got_words != (int64_t)words || job_words != words)
    return false;
  if (memcmp(cpu_llr, gpu_llr, bytes) != 0)
    return false;
  memcpy(cpu_llr, gpu_llr, bytes);
  return true;
}
#endif
