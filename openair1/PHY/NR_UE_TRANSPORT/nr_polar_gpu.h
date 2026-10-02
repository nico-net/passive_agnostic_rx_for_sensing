/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 *
 * GPU polar SC decode for the blind PDCCH candidate scan: submit a whole occasion's candidates as
 * ONE batch instead of decoding them one at a time on the scan thread. Measured motivation: a live
 * capture offered 6.17M candidates in 450 s and decoded 112k of them (86.6% dropped), which is what
 * stalls the DCI-layout sweep.
 *
 * Same shape as nr_pdsch_gpu_fep.h and for the same reason: the CUDA side is built as a MODULE
 * (libpolar_gpu.so) and dlopen'd at run time when NR_GPU_POLAR=1, so a build without CUDA -- or a
 * run without the knob -- is the CPU path, byte for byte. This header is the whole contract between
 * the receiver and the CUDA decoder; nothing in NR_UE_TRANSPORT includes nr_polar_sc_cuda.h.
 */
#ifndef NR_POLAR_GPU_H
#define NR_POLAR_GPU_H

#include <stdint.h>
#include "nr_dci_bits.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  /** Decode n candidates as one batch. llr is n vectors of `stride` int16 LLRs (the same vector
   *  polar_decoder_int16() gets, already unscrambled); item i uses len[i] payload bits at
   *  aggregation level al[i] and occupies the first al[i]*108 entries of its slot.
   *  Per item: crc[i] = what polar_decoder_int16() returns, payload[i] = its three right-aligned payload words,
   *  ok[i] = 1 when that item was actually decoded. An item the GPU cannot take (E beyond the
   *  kernel's cap, or the params table full) comes back ok[i]=0 and MUST be decoded on the CPU --
   *  mixing the two within an occasion is normal, not an error.
   *  Returns the number decoded (>= 0), or < 0 if the whole batch failed. */
  int (*decode)(const int16_t *llr, int stride, const uint16_t *len, const uint8_t *al, int n,
                uint32_t *crc, nr_dci_bits_t *payload, uint8_t *ok);
  /** Same contract, but items SHARE LLR vectors: item i decodes vec + vidx[i]*vstride. Only the
   *  n_vec distinct vectors are copied and uploaded (a dci_length sweep decodes each candidate at
   *  every length, so this skips ~97 % of the copying). NULL in a module that predates it. */
  int (*decode_vec)(const int16_t *vec, int vstride, int n_vec, const uint16_t *vidx, const uint16_t *len,
                    const uint8_t *al, int n, uint32_t *crc, nr_dci_bits_t *payload, uint8_t *ok);
} nr_gpu_polar_api_t;

#ifndef NR_GPU_POLAR_NO_LOADER
#include <dlfcn.h>
#include <stdlib.h>
/** dlopen libpolar_gpu.so next to the executable when NR_GPU_POLAR=1; NULL = CPU path. */
static inline const nr_gpu_polar_api_t *nr_gpu_polar_load(void)
{
  static const nr_gpu_polar_api_t *api = NULL;
  static int tried = 0;
  if (tried)
    return api;
  tried = 1;
  const char *e = getenv("NR_GPU_POLAR");
  if (e == NULL || atoi(e) == 0)
    return NULL;
  void *h = dlopen("libpolar_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (h == NULL)
    h = dlopen("./libpolar_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (h == NULL)
    return NULL;
  const nr_gpu_polar_api_t *(*get)(void) = (const nr_gpu_polar_api_t *(*)(void))dlsym(h, "nr_gpu_polar_api_v2");
  api = get ? get() : NULL;
  return api;
}
#endif

#ifdef __cplusplus
}
#endif

#endif
