/* SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * Private contract between nr_polar_sc_cuda_host.c (gcc, OAI headers) and nr_polar_sc_cuda.cu
 * (nvcc, no OAI headers -- simde does not compile under nvcc). */
#ifndef NR_POLAR_SC_CUDA_INT_H
#define NR_POLAR_SC_CUDA_INT_H
#include <stdint.h>
#include "nr_polar_sc_cuda.h"

enum { NPC_OP_F = 0, NPC_OP_G = 1, NPC_OP_B = 2 };
typedef struct { uint8_t code, level, lfrozen, rfrozen; uint16_t fli; } npc_op_t;
typedef struct {
  int N, n, E, K, rm_mode; /* rm_mode: 0 repetition, 1 puncturing, 2 shortening */
  int nops;
  uint16_t rmp[NPC_MAX_E];
  npc_op_t ops[NPC_MAX_OPS];
} npc_dev_params_t;

#ifdef __cplusplus
extern "C" {
#endif
int npc_gpu_upload_params(int slot, const npc_dev_params_t *p);
/* llr: n x estride int16; u_out: n x nstride bytes. The strides are the batch max, not the
 * compile-time max -- at AL2 that is 216 vs 1024 int16, i.e. 4.7x less to copy.
 * Fills t_h2d/t_kernel/t_d2h (us). Returns 0 on success. */
int npc_gpu_decode(const int *pid, const int16_t *llr, const int *vidx, int n_vec, int n, int estride,
                   uint8_t *u_out, int nstride, double *t_h2d, double *t_kernel, double *t_d2h);
/* page-locked host staging buffers (pinned copies run ~2x faster and are what the eventual
 * async path needs anyway). Fall back to malloc if they fail. */
void *npc_gpu_host_alloc(size_t bytes);
void npc_gpu_host_free(void *p);
#ifdef __cplusplus
}
#endif
#endif
