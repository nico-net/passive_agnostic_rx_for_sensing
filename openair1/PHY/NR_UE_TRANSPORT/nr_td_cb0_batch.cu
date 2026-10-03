/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
/*! \file nr_td_cb0_batch.cu
 * \brief libtd_cb0_gpu.so: GPU rate de-matching + bit de-interleaving + LDPC input formatting of code block 0 for
 * many Technique D hypotheses at once (nr_td_cb0_batch.h). Bit-identical to the CPU chain
 *   nr_deinterleaving_ldpc -> nr_rate_matching_ldpc_rx(clear = 1) -> 2Z zeros / F fillers at 127 -> int8 saturation
 * (test_nr_td_cb0_batch GpuDematchBitIdenticalSweep).
 *
 * Formulation: a GATHER instead of the CPU's scatter. Excluding the F filler positions, the circular buffer is a ring
 * of L = Foffset + (Ncb - Foffset - F) positions; the CPU writes e[k] to ring position (p0 + k) mod L, p0 = the ring
 * position of k0 (k0 inside the filler -> first position after it; k0 past the ring -> 0, as the RX loops do). So
 * d[j] = sum of e[k] over k = (c - p0) mod L + m L < E, c = ring index of j, and e[k] = llr[(k mod E/Qm) Qm + k div
 * (E/Qm)] (the de-interleaver). int16 sums wrap like the CPU's `d[ind] += e[k]` (modular, order-independent).
 * One thread per decoder-input position: no atomics, every item in one launch (grid.y = item).
 *
 * LLRs are read IN PLACE: GB10 reads pageable host memory through the host page tables (cudaDevAttr
 * PageableMemoryAccess = 1), elsewhere they must be managed / device memory (ptr_ok). No copies.
 */
#include <cuda_runtime.h>
#include <stdint.h>
#include <string.h>
#include "nr_td_cb0_batch.h"

namespace {

/* d[j] after nr_rate_matching_ldpc_rx(clear = 1), as a uint16 (modular) sum */
__device__ __forceinline__ uint16_t ring_sum(const nr_td_cb0_gpu_item_t &m, uint32_t j)
{
  const uint32_t F = m.F, Fo = m.Foffset;
  if (j >= m.Ncb || (j >= Fo && j < Fo + F))
    return 0; /* never written: beyond N_cb, or a filler position */
  const uint32_t L = Fo + (m.Ncb > Fo + F ? m.Ncb - Fo - F : 0);
  uint32_t p0 = m.k0 < Fo ? m.k0 : (m.k0 < Fo + F ? Fo : m.k0 - F);
  if (p0 >= L)
    p0 = 0;
  const uint32_t c = j < Fo ? j : j - F;
  const uint32_t Eq = m.E / m.Qm;
  uint32_t acc = 0;
  for (uint32_t k = (c + L - p0) % L; k < m.E; k += L)
    acc += (uint32_t)(int32_t)m.llr[(k % Eq) * m.Qm + k / Eq];
  return (uint16_t)acc;
}

__global__ void cb0_dematch_kernel(const nr_td_cb0_gpu_item_t *items, int8_t *l, int16_t *d)
{
  const nr_td_cb0_gpu_item_t m = items[blockIdx.y];
  if (m.E == 0)
    return; /* invalid item, or left to the CPU */
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= NR_TD_CB0_L_STRIDE)
    return;
  const uint32_t Z = m.Z, K = m.K, KcZ = m.Kc * Z, Kprime = K - m.F;
  int32_t v;
  if (i >= KcZ || i < 2 * Z)
    v = 0; /* 2Z punctured systematic bits; past the decoder input */
  else if (i >= Kprime && i < K)
    v = 127; /* filler (memset 127 on int16 = 0x7F7F, saturated) */
  else
    v = (int16_t)ring_sum(m, i - 2 * Z);
  l[(size_t)blockIdx.y * NR_TD_CB0_L_STRIDE + i] = (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
  if (d && i < m.N)
    d[(size_t)blockIdx.y * NR_TD_CB0_D_STRIDE + i] = (int16_t)ring_sum(m, i);
}

struct Ctx {
  int ok = -1; /* -1 not initialised, 0 unusable, 1 ready */
  int pageable = 0;
  cudaStream_t stream = nullptr;
  void *scr[3] = {nullptr, nullptr, nullptr};
  size_t cap[3] = {0, 0, 0};
};
Ctx g;

int init()
{
  if (g.ok >= 0)
    return g.ok;
  g.ok = 0;
  int n = 0;
  if (cudaGetDeviceCount(&n) != cudaSuccess || n == 0)
    return 0;
  int v = 0;
  if (cudaDeviceGetAttribute(&v, cudaDevAttrPageableMemoryAccess, 0) == cudaSuccess)
    g.pageable = v;
  if (cudaStreamCreateWithFlags(&g.stream, cudaStreamNonBlocking) != cudaSuccess)
    return 0;
  g.ok = 1;
  return 1;
}

void *scratch(int which, size_t bytes)
{
  if (which < 0 || which > 2 || init() != 1)
    return nullptr;
  if (g.cap[which] < bytes) {
    if (g.scr[which])
      cudaFree(g.scr[which]);
    g.scr[which] = nullptr;
    g.cap[which] = 0;
    const size_t want = bytes + bytes / 4;
    if (cudaMallocManaged(&g.scr[which], want) != cudaSuccess)
      return nullptr;
    g.cap[which] = want;
  }
  return g.scr[which];
}

int pageable_ok()
{
  return init() == 1 && g.pageable;
}

int ptr_ok(const void *p)
{
  if (!p || init() != 1)
    return 0;
  if (g.pageable)
    return 1;
  cudaPointerAttributes a;
  if (cudaPointerGetAttributes(&a, p) != cudaSuccess) {
    cudaGetLastError();
    return 0;
  }
  return a.type == cudaMemoryTypeManaged || a.type == cudaMemoryTypeDevice;
}

int dematch(const nr_td_cb0_gpu_item_t *items, int n, int8_t *l, int16_t *d)
{
  if (init() != 1 || n <= 0)
    return -1;
  nr_td_cb0_gpu_item_t *m = (nr_td_cb0_gpu_item_t *)scratch(2, (size_t)n * sizeof(*m));
  if (!m)
    return -1;
  memcpy(m, items, (size_t)n * sizeof(*m));
  const int threads = 256;
  const unsigned bx = (NR_TD_CB0_L_STRIDE + threads - 1) / threads;
  for (int off = 0; off < n; off += 65535) {
    const int cnt = n - off < 65535 ? n - off : 65535;
    cb0_dematch_kernel<<<dim3(bx, cnt), threads, 0, g.stream>>>(m + off, l + (size_t)off * NR_TD_CB0_L_STRIDE,
                                                               d ? d + (size_t)off * NR_TD_CB0_D_STRIDE : nullptr);
  }
  if (cudaGetLastError() != cudaSuccess)
    return -2;
  if (cudaStreamSynchronize(g.stream) != cudaSuccess)
    return -3;
  return 0;
}

const nr_td_cb0_gpu_api_t api = {NR_TD_CB0_GPU_ABI, pageable_ok, ptr_ok, dematch, scratch};

} // namespace

extern "C" const nr_td_cb0_gpu_api_t *nr_td_cb0_gpu_api(void)
{
  return init() == 1 ? &api : nullptr;
}
