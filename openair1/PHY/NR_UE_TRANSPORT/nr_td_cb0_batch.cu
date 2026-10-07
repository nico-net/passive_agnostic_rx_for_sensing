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
 * Unique LLR spans are explicitly staged into preallocated device storage. Kernels
 * never walk CPU-coherent host pages. All buffers are reserved before RX.
 */
#include <cuda_runtime.h>
#include <stdint.h>
#include <string.h>
#include "nr_td_cb0_batch.h"
#include "nr_dl_gpu_memory.h"
#include "nr_dl_gpu_budget.h"
#include <stdlib.h>

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
    acc += (uint32_t)(int32_t)(int16_t)(m.llr[(k % Eq) * m.Qm + k / Eq] >> m.shift); /* K38 k_h, as the full decode */
  return (uint16_t)acc;
}

__global__ void cb0_dematch_kernel(const nr_td_cb0_gpu_item_t *items, int8_t *l, uint32_t l_stride, int16_t *d)
{
  const nr_td_cb0_gpu_item_t m = items[blockIdx.y];
  if (m.E == 0)
    return; /* invalid item, or left to the CPU */
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= l_stride)
    return;
  const uint32_t Z = m.Z, K = m.K, KcZ = m.Kc * Z, Kprime = K - m.F;
  int32_t v;
  if (i >= KcZ || i < 2 * Z)
    v = 0; /* 2Z punctured systematic bits; past the decoder input */
  else if (i >= Kprime && i < K)
    v = 127; /* filler (memset 127 on int16 = 0x7F7F, saturated) */
  else
    v = (int16_t)ring_sum(m, i - 2 * Z);
  l[(size_t)blockIdx.y * l_stride + i] = (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
  if (d && i < m.N)
    d[(size_t)blockIdx.y * NR_TD_CB0_D_STRIDE + i] = (int16_t)ring_sum(m, i);
}

struct Ctx {
  int ok = -1, max_items = 0;
  cudaStream_t stream = nullptr;
  void *host[3] = {}, *dev[3] = {};
  size_t cap[3] = {};
  int16_t *llr_h = nullptr, *llr_d = nullptr;
  size_t llr_cap = 0;
  int output_slot = 0;
};
Ctx g;
nr_dl_gpu_budget_t budget;

int init()
{
  if (g.ok >= 0) return g.ok;
  g.ok = 0;
  const char *e = getenv("LDPC_CB0_MAX_ITEMS");
  g.max_items = e ? atoi(e) : 2048;
  if (g.max_items < 1) g.max_items = 1;
  if (g.max_items > 65536) g.max_items = 65536;
  g.cap[0] = (size_t)g.max_items * NR_TD_CB0_L_STRIDE;
  g.cap[1] = (size_t)g.max_items * NR_TD_CB0_D_STRIDE * sizeof(int16_t);
  g.cap[2] = (size_t)g.max_items * sizeof(nr_td_cb0_gpu_item_t);
  // Up to 16 distinct GrantWork signatures, full-band rank four 256-QAM.
  g.llr_cap = (size_t)16 * 275 * 12 * 14 * 4 * 8;
  if (cudaStreamCreateWithFlags(&g.stream, cudaStreamNonBlocking)) return 0;
  for (int k = 0; k < 3; k++) {
    if (cudaHostAlloc(&g.host[k], g.cap[k], cudaHostAllocDefault) || cudaMalloc(&g.dev[k], g.cap[k] * (k == 0 ? 8 : 1))) return 0;
    memset(g.host[k], 0, g.cap[k]);
  }
  if (cudaHostAlloc(&g.llr_h, g.llr_cap * sizeof(int16_t), cudaHostAllocDefault)
      || cudaMalloc(&g.llr_d, g.llr_cap * sizeof(int16_t))) return 0;
  memset(g.llr_h, 0, g.llr_cap * sizeof(int16_t));
  // Load the kernel before sealing; E=0 makes this an inactive item.
  if (cudaMemsetAsync(g.dev[2], 0, sizeof(nr_td_cb0_gpu_item_t), g.stream)) return 0;
  cb0_dematch_kernel<<<1, 256, 0, g.stream>>>((nr_td_cb0_gpu_item_t *)g.dev[2], (int8_t *)g.dev[0], NR_TD_CB0_L_STRIDE, nullptr);
  if (cudaStreamSynchronize(g.stream)) return 0;
  g.ok = 1;
  return 1;
}

void *scratch(int which, size_t bytes)
{
  return g.ok == 1 && which >= 0 && which < 3 && bytes <= g.cap[which] ? g.host[which] : nullptr;
}
// API legacy name: host pointers are accepted via explicit staging, never dereferenced by a kernel.
int pageable_ok() { return g.ok == 1; }
int ptr_ok(const void *p) { return g.ok == 1 && p != nullptr; }

int dematch_impl(const nr_td_cb0_gpu_item_t *items, int n, int8_t *l, int16_t *d, const int8_t **device)
{
  if (g.ok != 1 || n <= 0 || n > g.max_items || !items || (!l && !device)) return -1;
  auto *m = (nr_td_cb0_gpu_item_t *)g.host[2];
  memcpy(m, items, (size_t)n * sizeof(*m));
  size_t used = 0;
  // Copy each unique LLR source once, at the largest E requested by its hypotheses.
  for (int i = 0; i < n; i++) {
    if (!items[i].E) continue;
    int first = i;
    for (int j = 0; j < i; j++) if (items[j].E && items[j].llr == items[i].llr) { first = j; break; }
    if (first != i) { m[i].llr = m[first].llr; continue; }
    size_t need = items[i].E;
    for (int j = i + 1; j < n; j++) if (items[j].llr == items[i].llr && items[j].E > need) need = items[j].E;
    if (need > g.llr_cap - used) return -1;
    // First pass only packs offsets, so budget refusal touches no LLR pages.
    m[i].llr = g.llr_d + used;
    used += need;
  }
  if (nr_dl_gpu_radio_mode && !nr_dl_gpu_budget_take(&budget,
      3 * used * sizeof(int16_t) + (size_t)n * sizeof(*m) * 3
      + (device ? 0 : (size_t)n * NR_TD_CB0_L_STRIDE * 3)
      + (d ? (size_t)n * NR_TD_CB0_D_STRIDE * sizeof(int16_t) * 3 : 0))) return -1;
  for (int i = 0; i < n; i++) {
    if (!items[i].E) continue;
    bool first = true;
    for (int j = 0; j < i; j++) if (items[j].E && items[j].llr == items[i].llr) { first = false; break; }
    if (!first) continue;
    size_t need = items[i].E;
    for (int j = i + 1; j < n; j++) if (items[j].llr == items[i].llr && items[j].E > need) need = items[j].E;
    memcpy(g.llr_h + (m[i].llr - g.llr_d), items[i].llr, need * sizeof(int16_t));
  }
  if (cudaMemcpyAsync(g.llr_d, g.llr_h, used * sizeof(int16_t), cudaMemcpyHostToDevice, g.stream)
      || cudaMemcpyAsync(g.dev[2], m, (size_t)n * sizeof(*m), cudaMemcpyHostToDevice, g.stream)) goto failed;
  for (int off = 0; off < n; off += 65535) {
    const int count = n - off < 65535 ? n - off : 65535;
    cb0_dematch_kernel<<<dim3((NR_TD_CB0_L_STRIDE + 255) / 256, count), 256, 0, g.stream>>>(
        (nr_td_cb0_gpu_item_t *)g.dev[2] + off, (int8_t *)g.dev[0] + (size_t)off * NR_TD_CB0_L_STRIDE,
        NR_TD_CB0_L_STRIDE, d ? (int16_t *)g.dev[1] + (size_t)off * NR_TD_CB0_D_STRIDE : nullptr);
  }
  if (cudaGetLastError()
      || (!device && cudaMemcpyAsync(g.host[0], g.dev[0], (size_t)n * NR_TD_CB0_L_STRIDE, cudaMemcpyDeviceToHost, g.stream))
      || (d && cudaMemcpyAsync(g.host[1], g.dev[1], (size_t)n * NR_TD_CB0_D_STRIDE * sizeof(int16_t), cudaMemcpyDeviceToHost, g.stream))
      || cudaStreamSynchronize(g.stream)) goto failed;
  if (device) { *device = (const int8_t *)g.dev[0]; return 0; }
  if (l != g.host[0]) memcpy(l, g.host[0], (size_t)n * NR_TD_CB0_L_STRIDE);
  if (d && d != g.host[1]) memcpy(d, g.host[1], (size_t)n * NR_TD_CB0_D_STRIDE * sizeof(int16_t));
  return 0;
failed:
  // Keep storage for any outstanding operation, but never reuse it after a CUDA error.
  g.ok = 0;
  return -2;
}

int dematch(const nr_td_cb0_gpu_item_t *items, int n, int8_t *l, int16_t *d)
{ return dematch_impl(items, n, l, d, nullptr); }
int dematch_device(const nr_td_cb0_gpu_item_t *items, int n, const int8_t **l)
{ return dematch_impl(items, n, nullptr, nullptr, l); }
extern "C" void nr_dl_gpu_transfer_stats(uint64_t *bytes, uint64_t *refused)
{ *bytes = __atomic_load_n(&budget.bytes, __ATOMIC_RELAXED); *refused = __atomic_load_n(&budget.refused, __ATOMIC_RELAXED); }

int retire_output()
{
  // Timed-out LDPC may still read this output on another stream. Eight buffers
  // were reserved at init; retire forever rather than waiting, freeing or growing.
  if (++g.output_slot >= 8) { g.ok = 0; return -1; }
  g.dev[0] = (char *)g.dev[0] + g.cap[0];
  return 0;
}

const nr_td_cb0_gpu_api_t api = {NR_TD_CB0_GPU_ABI, pageable_ok, ptr_ok, dematch, scratch, dematch_device, retire_output};

} // namespace

extern "C" const nr_td_cb0_gpu_api_t *nr_td_cb0_gpu_api(void)
{
  const int ready = init();
  nr_dl_gpu_sealed = true;
  return ready == 1 ? &api : nullptr;
}
