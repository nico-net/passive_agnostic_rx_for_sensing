/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_sweep_internal.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
/* Word-wise recurrence from PHY/gold.h; skip Nc=1600. The frozen idsweep
 * example uses the same recurrence, but its DM-RS c_init MUST NOT be reused. */
struct Gold {
  uint32_t x1, x2;
  __device__ void step()
  {
    x1 = (x1 >> 1) ^ (x1 >> 4);
    x1 ^= (x1 << 31) ^ (x1 << 28);
    x2 = (x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3) ^ (x2 >> 4);
    x2 ^= (x2 << 31) ^ (x2 << 30) ^ (x2 << 29) ^ (x2 << 28);
  }
  __device__ void reset(uint32_t c)
  {
    x1 = 1u | (1u << 31);
    x2 = c ^ ((c ^ (c >> 1) ^ (c >> 2) ^ (c >> 3)) << 31);
    for (int i = 1; i < 50; ++i)
      step();
  }
  __device__ uint32_t next() { step(); return x1 ^ x2; }
};
constexpr int words_max = (6 * NR_CSIRS_DISC_MAX_RB + 31) / 32;
__device__ float warp_sum(float x)
{
  for (int d = 16; d; d /= 2)
    x += __shfl_down_sync(0xffffffff, x, d);
  return x;
}

/* One CTA per (hypothesis, ID, delta, lattice slot). Each warp scores one
 * port/antenna at a time; its lanes score independent coherent blocks. */
__global__ void sweep(nr_csirs_disc_sweep_map_t map, const short2 *rx, const int *slot,
                      int ants, int spf, nr_csirs_disc_sweep_cfg_t cfg, float *scores)
{
  __shared__ uint32_t gold[14][words_max];
  __shared__ float sums[4];
  int candidate = blockIdx.x;
  int id = cfg.id_first + candidate / cfg.delta_max;
  int delta = candidate % cfg.delta_max;
  int l = threadIdx.x;
  if (l < 14 && (map.sym_mask & (1u << l))) {
    uint64_t t = 14 * ((slot[blockIdx.y] + delta) % spf) + l + 1;
    uint32_t init = ((t * (2 * id + 1) << 10) + id) & 0x7fffffff;
    Gold g;
    g.reset(init);
    for (int w = 0; w < map.words; ++w)
      gold[l][w] = g.next();
  }
  __syncthreads();
  int lane = threadIdx.x % 32, warp = threadIdx.x / 32;
  float total = 0;
  for (int task = warp; task < map.ports * ants; task += 4) {
    int p = task / ants, ant = task % ants;
    int begin = map.begin[p], n = map.begin[p+1] - begin;
    // This form also supports block_re greater than n without integer overflow.
    int nb = n ? 1 + (n - 1) / cfg.block_re : 0;
    float rho = 0, active = 0;
    const short2 *samples = rx + ((size_t)blockIdx.y * ants + ant) * map.n_re;
    for (int b = lane; b < nb; b += 32) {
      int first = b * cfg.block_re, count = min(cfg.block_re, n - first);
      float ar = 0, ai = 0, energy = 0;
      for (int k = 0; k < count; ++k) {
        int i = begin + first + k;
        nr_csirs_disc_re_t re = map.re[i];
        unsigned bits = (gold[re.symbol][re.m / 16] >> (2 * (re.m % 16))) & 3;
        // OAI QPSK at amplitude 16384 is exactly (+/-8192,+/-8192).
        // Unit amplitudes cancel from the normalized correlation.
        float xr = (bits & 1 ? -1.f : 1.f) * re.sign;
        float xi = (bits & 2 ? -1.f : 1.f) * re.sign;
        short2 y = samples[i];
        float yr = y.x, yi = y.y;
        ar += yr*xr + yi*xi;
        ai += yi*xr - yr*xi;
        energy += yr*yr + yi*yi;
      }
      if (energy > 0) {
        rho += fminf(1.f, sqrtf((ar*ar + ai*ai) / (energy * (2 * count))));
        active += 1;
      }
    }
    rho = warp_sum(rho);
    active = warp_sum(active);
    if (lane == 0 && active > 0)
      total += rho / active * sqrtf(n / active) / .886f;
  }
  if (lane == 0)
    sums[warp] = total;
  __syncthreads();
  if (threadIdx.x == 0)
    scores[(size_t)blockIdx.y * gridDim.x + candidate] = (sums[0]+sums[1]+sums[2]+sums[3]) / (map.ports*ants);
}

template<typename T> struct Device {
  T *p = nullptr;
  size_t capacity = 0;
  ~Device() { if (p) cudaFree(p); }
  bool alloc(size_t n)
  {
    n = std::max(n, size_t(1));
    if (n <= capacity) return true;
    if (p && cudaFree(p) != cudaSuccess) return false;
    p = nullptr;
    capacity = 0;
    if (cudaMalloc(&p, n * sizeof(T)) != cudaSuccess) return false;
    capacity = n;
    return true;
  }
};
struct Map {
  nr_csirs_disc_sweep_map_t m = {};
  ~Map() { free(m.re); }
};
struct Stream {
  cudaStream_t s = nullptr;
  ~Stream() { if (s) cudaStreamDestroy(s); }
};

int run(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_hyp_t *hyps, int nh,
        const nr_csirs_disc_sweep_cfg_t *cfg, nr_csirs_disc_win_t *out)
{
  const char *enable = getenv("ISAC_CSIRS_DISC");
  if (!enable || strcmp(enable, "1"))
    return 0;
  nr_csirs_disc_sweep_cfg_t c;
  if (!out || nr_csirs_disc_sweep_validate(r, hyps, nh, cfg, &c))
    return -1;
  Stream stream;
  if (cudaStreamCreateWithFlags(&stream.s, cudaStreamNonBlocking) != cudaSuccess)
    return -1;
  const int nc = c.id_count * c.delta_max;
  std::vector<float> mean(nc), values;
  std::vector<short2> packed;
  // Grow workspace only when necessary; avoid four cudaMalloc/cudaFree pairs per hypothesis.
  Device<nr_csirs_disc_re_t> dm;
  Device<short2> dx;
  Device<int> ds;
  Device<float> dz;
  for (int h = 0; h < nh; ++h) {
    Map owner;
    auto &map = owner.m;
    if (nr_csirs_disc_sweep_map(hyps + h, r->n_rb, &map) || map.words > words_max)
      return -1;
    std::vector<int> slots, indices;
    for (int i = 0; i < r->capacity; ++i)
      if (r->meta[i].valid && r->meta[i].abs_slot % hyps[h].period == hyps[h].offset) {
        slots.push_back(r->meta[i].slot);
        indices.push_back(i);
      }
    if(c.max_slots>0 && indices.size()>(size_t)c.max_slots) {
      std::sort(indices.begin(),indices.end(),[&](int a,int b) {return r->meta[a].abs_slot!=r->meta[b].abs_slot?r->meta[a].abs_slot<r->meta[b].abs_slot:a<b;});
      indices.erase(indices.begin(),indices.end()-c.max_slots);
      slots.clear();for(int i:indices) slots.push_back(r->meta[i].slot);
    }
    std::fill(mean.begin(), mean.end(), 0);
    if (slots.empty() || !map.n_re) {
      nr_csirs_disc_sweep_winner(mean.data(), &c, h, slots.size(), out + h);
      continue;
    }
    packed.resize((size_t)slots.size() * r->n_ant * map.n_re);
    for (size_t s = 0; s < slots.size(); ++s)
      for (int a = 0; a < r->n_ant; ++a) {
        const int16_t *src = nr_csirs_disc_ring_sym(r, indices[s], a, 0);
        short2 *dst = packed.data() + (s*r->n_ant+a)*map.n_re;
        for (int i = 0; i < map.n_re; ++i) {
          int k = map.re[i].grid_index;
          dst[i] = make_short2(src[2*k], src[2*k+1]);
        }
      }
    values.resize((size_t)nc * slots.size());
    if (!dm.alloc(map.n_re) || !dx.alloc(packed.size()) || !ds.alloc(slots.size()) || !dz.alloc(values.size()))
      return -1;
    if (cudaMemcpyAsync(dm.p, map.re, map.n_re*sizeof(*map.re), cudaMemcpyHostToDevice, stream.s) != cudaSuccess
        || cudaMemcpyAsync(dx.p, packed.data(), packed.size()*sizeof(short2), cudaMemcpyHostToDevice, stream.s) != cudaSuccess
        || cudaMemcpyAsync(ds.p, slots.data(), slots.size()*sizeof(int), cudaMemcpyHostToDevice, stream.s) != cudaSuccess)
      return -1;
    auto device_map = map;
    device_map.re = dm.p;
    sweep<<<dim3(nc, slots.size()), 128, 0, stream.s>>>(device_map, dx.p, ds.p, r->n_ant, r->slots_per_frame, c, dz.p);
    if (cudaGetLastError() != cudaSuccess
        || cudaMemcpyAsync(values.data(), dz.p, values.size()*sizeof(float), cudaMemcpyDeviceToHost, stream.s) != cudaSuccess
        || cudaStreamSynchronize(stream.s) != cudaSuccess)
      return -1;
    for (int i = 0; i < nc; ++i) {
      double sum = 0;
      for (size_t s = 0; s < slots.size(); ++s)
        sum += values[s*nc+i];
      mean[i] = sum / slots.size();
    }
    nr_csirs_disc_sweep_winner(mean.data(), &c, h, slots.size(), out + h);
  }
  return nh;
}
} // namespace

extern "C" int nr_csirs_disc_gpu_sweep_entry(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_hyp_t *hyps, int nh,
                                            const nr_csirs_disc_sweep_cfg_t *cfg, nr_csirs_disc_win_t *out)
{
  try {
    return run(r, hyps, nh, cfg, out);
  } catch (...) {
    return -1; // allocation failures must not escape the C module ABI
  }
}
