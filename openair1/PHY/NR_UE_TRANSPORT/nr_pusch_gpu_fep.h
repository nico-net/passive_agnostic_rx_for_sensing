/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* ULFEP: host loader of the UL GPU front end (nr_pusch_gpu_fep.cuh, exported by libpdsch_gpu.so).
 * ISAC_UL_GPU_FEP=1 loads it; unset/0 or any failure leaves the CPU FEP untouched. */
#ifndef NR_PUSCH_GPU_FEP_H
#define NR_PUSCH_GPU_FEP_H
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>

/* Same layout as ulfep::Item (72 bytes on LP64). */
typedef struct {
  const int16_t *src[4]; /* window start per antenna */
  int16_t *dst[4];       /* N c16 output row per antenna */
  int16_t rot[2];        /* symbol_rotation c16 {r, i} */
  int rotate;            /* 1 = OAI RX symbol rotation after the DFT, 0 = bare DFT */
} nr_pusch_gpu_fep_item_t;

typedef struct {
  int (*init)(int nant, int N, int ncp, int offset_div, int pct);
  int (*run)(const void *items, int n, int nb_rb, int fco); /* 0 done; < 0: nothing written, run the CPU FEP */
  void (*stats)(uint64_t *o); /* calls items gpu_us max_us busy over_cap rt_defer late errors partitioned */
} nr_pusch_gpu_fep_api_t;

static inline const nr_pusch_gpu_fep_api_t *nr_pusch_gpu_fep_load(void)
{
  static nr_pusch_gpu_fep_api_t api;
  static int tried;
  if (tried) return api.run ? &api : NULL;
  tried = 1;
  const char *e = getenv("ISAC_UL_GPU_FEP");
  if (!e || atoi(e) != 1) return NULL;
  void *h = dlopen("libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h) h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h) return NULL;
  api.init = (int (*)(int, int, int, int, int))dlsym(h, "nr_pusch_gpu_fep_init");
  api.run = (int (*)(const void *, int, int, int))dlsym(h, "nr_pusch_gpu_fep_run");
  api.stats = (void (*)(uint64_t *))dlsym(h, "nr_pusch_gpu_fep_stats");
  if (!api.init || !api.run || !api.stats) { api.run = NULL; return NULL; }
  return &api;
}
#endif
