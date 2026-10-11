/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUSHARE: nr_gpu_green.h. With ISAC_GPU_DISC_SMS=16 ISAC_GPU_RT_ISOLATE=1 a full-occupancy grid launched on the
 * discovery stream must run only on the discovery partition, a grid on the real-time stream only on the other SMs,
 * the two SM sets must be disjoint, and copies/events on those streams must work as on ordinary streams.
 * Prints GPU_GREEN_TEST ok=1 (exit 0), or exits 77 (skipped) without a usable device / green-context support. */
#include "../nr_gpu_green.h"
#include <cstdio>
#include <cstdlib>

__global__ static void where(unsigned *sm, int *x)
{
  unsigned id;
  asm("mov.u32 %0, %%smid;" : "=r"(id));
  if (threadIdx.x == 0) atomicOr(&sm[id / 32], 1u << (id % 32));
  x[blockIdx.x * blockDim.x + threadIdx.x] += 1;
}

static int count(const unsigned *m) { return __builtin_popcount(m[0]) + __builtin_popcount(m[1]) + __builtin_popcount(m[2]) + __builtin_popcount(m[3]); }

int main(void)
{
  setenv("ISAC_GPU_DISC_SMS", "16", 1);
  setenv("ISAC_GPU_RT_ISOLATE", "1", 1);
  int n = 0;
  if (cudaGetDeviceCount(&n) != cudaSuccess || n < 1) { printf("GPU_GREEN_TEST skipped (no device)\n"); return 77; }
  cudaStream_t disc, rt;
  if (!nr_gpu_green_stream(&disc, 0, false) || !nr_gpu_green_stream(&rt, -1, true)) {
    printf("GPU_GREEN_TEST skipped (no green contexts)\n");
    return 77;
  }
  int sms = 0;
  cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, 0);
  unsigned *m;
  int *x, host[256], back[256];
  cudaMallocManaged(&m, 8 * sizeof(unsigned));
  cudaMalloc(&x, 64 * sms * 128 * sizeof(int));
  for (int i = 0; i < 8; i++) m[i] = 0;
  for (int i = 0; i < 256; i++) host[i] = i;
  cudaMemsetAsync(x, 0, 64 * sms * 128 * sizeof(int), disc);
  cudaMemcpyAsync(x, host, sizeof(host), cudaMemcpyHostToDevice, disc);
  where<<<64 * sms, 128, 0, disc>>>(m, x);
  cudaEvent_t done;
  cudaEventCreateWithFlags(&done, cudaEventDisableTiming);
  cudaEventRecord(done, disc);
  cudaStreamWaitEvent(rt, done, 0);
  where<<<64 * sms, 128, 0, rt>>>(m + 4, x);
  cudaMemcpyAsync(back, x, sizeof(back), cudaMemcpyDeviceToHost, rt);
  const cudaError_t e = cudaStreamSynchronize(rt);
  bool values = true;
  for (int i = 0; i < 256; i++) values &= back[i] == i + 2;
  const bool disjoint = !((m[0] & m[4]) | (m[1] & m[5]) | (m[2] & m[6]) | (m[3] & m[7]));
  const bool ok = e == cudaSuccess && values && disjoint && count(m) > 0 && count(m) <= sms / 2 && count(m + 4) > 0
      && count(m) + count(m + 4) <= sms;
  printf("GPU_GREEN_TEST ok=%d sms=%d discovery=%d realtime=%d disjoint=%d values=%d cuda=%s\n", ok, sms, count(m),
         count(m + 4), disjoint, values, cudaGetErrorString(e));
  return ok ? 0 : 1;
}
