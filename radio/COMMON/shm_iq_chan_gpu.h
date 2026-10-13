/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* ABI between shm_iq_chan (host) and libshm_iq_chan_gpu.so (shm_iq_chan_gpu.cu, built only with ENABLE_LDPC_CUDA=ON; dlopen'd
 * so the executable and the default build have no CUDA dependency). All functions return 0 / non-NULL on success. */
#ifndef SHM_IQ_CHAN_GPU_H
#define SHM_IQ_CHAN_GPU_H
#include "iq_ddc.h"
#define CHAN_GPU_ABI 1
#ifdef __cplusplus
extern "C" {
#endif
int chan_gpu_abi(void);
/* designs[cell]: an initialised iq_ddc_t (filters + rot table) per cell; block: raw samples per run (multiple of IQ_DDC_ROT_BLOCK) */
void *chan_gpu_create(double rate_in, int nch, int ncell, const iq_ddc_t *const *designs, int max_block);
/* take d->shift_hz / d->rot (call after iq_ddc_set_shift on the host design); the mixer phase is kept like iq_ddc_set_shift */
int chan_gpu_set_shift(void *g, int cell, const iq_ddc_t *d);
void chan_gpu_reset(void *g); /* filter history + mixer phase, like iq_ddc_reset on every cell and channel */
/* in[nch]: n raw sc16 samples each; out[cell * nch + ch]: n / decim sc16 samples. Blocking (one stream). */
int chan_gpu_run(void *g, const int16_t *const *in, int n, int16_t *const *out);
float chan_gpu_last_ms(void *g); /* GPU time of the last run (H2D + kernels + D2H, cudaEvent) */
void chan_gpu_destroy(void *g);
#ifdef __cplusplus
}
#endif
#endif
