/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
/* Internal to libldpc_cuda.so: the device base-graph tables of ldpc_decoder.cu, shared with the CB0 entry
 * (ldpc_cb0.cu). Same packing as the kernels: cn[row * cn_stride + k] = idx_col | s << 16, vn[col * vn_stride + k] =
 * idx_row | s << 16 (s is the raw shift V, the kernels apply it mod Z). */
#ifndef LDPC_CUDA_BG_H
#define LDPC_CUDA_BG_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  uint32_t num_rows, num_cols, num_edges;
  const uint32_t *cn_degree, *vn_degree, *cn, *vn; /* device pointers */
  uint32_t cn_stride, vn_stride;
} ldpc_cuda_bg_t;
/* Thread-safe; initialises the global tables on first use (cudaDeviceScheduleBlockingSync first, as
 * ldpc_decoder_init). Returns 0, or -1 if BG / Z is not a valid 38.212 pair or the tables are unavailable. */
int ldpc_cuda_basegraph(uint32_t BG, uint32_t Z, ldpc_cuda_bg_t *out);
#ifdef __cplusplus
}
#endif
#endif
