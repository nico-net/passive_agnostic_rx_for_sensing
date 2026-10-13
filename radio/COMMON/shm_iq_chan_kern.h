/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Per-element arithmetic of the shared channelizer's GPU kernels (shm_iq_chan_gpu.cu). Compiled by nvcc for the device and by
 * the host compiler in shm_iq_chan --selftest, which runs the same data flow on the CPU and compares it with iq_ddc: the indexing
 * of the GPU path is thereby checkable without a GPU. The expressions mirror iq_ddc.c (mixer, halfband stage, output rounding). */
#ifndef SHM_IQ_CHAN_KERN_H
#define SHM_IQ_CHAN_KERN_H
#include <math.h>
#include <stdint.h>
#ifdef __CUDACC__
#define CK_HD static __forceinline__ __host__ __device__
#else
#define CK_HD static inline
#endif

/* x: sample i of the block (interleaved sc16), rot: exp(-j step nn) at nn = i % IQ_DDC_ROT_BLOCK, (sr, si): block-start phasor */
CK_HD void ck_mix(const int16_t *x, const float *rot, float sr, float si, float *yr, float *yi)
{
  const float wr = sr * rot[0] - si * rot[1], wi = sr * rot[1] + si * rot[0];
  const float a = x[0], b = x[1];
  *yr = a * wr - b * wi;
  *yi = a * wi + b * wr;
}

/* Halfband output m from the concatenated [history (H = 2c) | block] stage input, c = (ntaps-1)/2 (odd):
 * y[m] = 0.5 x[2m+c] + sum_k pair[k] (x[2m+c+j] + x[2m+c-j]), j = 2k+1. Same tap order as iq_ddc stage_run. */
CK_HD void ck_halfband(const float *xr, const float *xi, int m, int c, int npairs, const float *pair, float *yr, float *yi)
{
  const float *pr = xr + 2 * m + c, *pi = xi + 2 * m + c;
  float r = 0.5f * pr[0], i = 0.5f * pi[0];
  for (int k = 0; k < npairs; ++k) {
    const int j = 2 * k + 1;
    r += pair[k] * (pr[j] + pr[-j]);
    i += pair[k] * (pi[j] + pi[-j]);
  }
  *yr = r; *yi = i;
}

CK_HD int16_t ck_out(float a)
{
  const float ca = a > 32767.f ? 32767.f : a < -32768.f ? -32768.f : a;
  return (int16_t)(int32_t)rintf(ca); /* nearest even, like (ca + 1.5*2^23) - 1.5*2^23 in iq_ddc */
}
#endif
