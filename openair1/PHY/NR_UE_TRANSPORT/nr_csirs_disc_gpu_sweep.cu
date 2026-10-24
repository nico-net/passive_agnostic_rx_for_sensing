/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_sweep_internal.h"
#include "nr_csirs_disc_evidence_internal.h"
#include <cuda_runtime.h>
#include "nr_gpu_green.h"
#include "../TOOLS/nr_gpu_cuda.h"
#include "../TOOLS/nr_gpu_chunk.h"
#include "../TOOLS/nr_gpu_wait.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>
#include <climits>

namespace {
/* Word-wise recurrence from PHY/gold.h; skip Nc=1600. The frozen idsweep
 * example uses the same recurrence, but its DM-RS c_init MUST NOT be reused. */
struct Gold {
  uint32_t x1, x2;
/* step: device member of Gold: advances the two 31-bit LFSR states of the Gold sequence (TS 38.211 5.2.1) by one word of 32 bits. */
  __device__ void step()
  {
    x1 = (x1 >> 1) ^ (x1 >> 4);
    x1 ^= (x1 << 31) ^ (x1 << 28);
    x2 = (x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3) ^ (x2 >> 4);
    x2 ^= (x2 << 31) ^ (x2 << 30) ^ (x2 << 29) ^ (x2 << 28);
  }
/* reset: device member of Gold: initialises x1 and x2 from the c_init value c and advances them 50 steps (the scrambling-sequence start-up of TS 38.211 5.2.1). */
  __device__ void reset(uint32_t c)
  {
    x1 = 1u | (1u << 31);
    x2 = c ^ ((c ^ (c >> 1) ^ (c >> 2) ^ (c >> 3)) << 31);
    for (int i = 1; i < 50; ++i)
      step();
  }
/* next: device member of Gold: advances the generator by one step and returns the next 32-bit output word x1 XOR x2. */
  __device__ uint32_t next() { step(); return x1 ^ x2; }
};
constexpr int words_max = (6 * NR_CSIRS_DISC_MAX_RB + 31) / 32;
/* warp_sum: device helper: sums a float over the 32 lanes of a warp (shuffle-down reduction); the result is valid in lane 0. */
__device__ float warp_sum(float x)
{
  for (int d = 16; d; d /= 2)
    x += __shfl_down_sync(0xffffffff, x, d);
  return x;
}

/* sweep: GPU correlation kernel: one CTA per (hypothesis candidate = scrambling ID and slot delta, slot); scores the slot's CSI-RS resource elements against the Gold-generated reference, per port and antenna.
 *   Each warp takes port/antenna pairs; each lane scores independent blocks of block_re REs. Writes one score per candidate and slot into scores[] (the CPU port_score() / nr_csirs_disc_sweep_cpu() equivalent).
 *   Launched by run() and by nr_csirs_disc_gpu_sweep_prepare().
 */
__global__ void sweep(nr_csirs_disc_sweep_map_t map, const short2 *rx, const int *slot,
                      int ants, int spf, nr_csirs_disc_sweep_cfg_t cfg, float *scores, int cand0 = 0, int nctot = 0)
{
  __shared__ uint32_t gold[14][words_max];
  __shared__ float sums[4];
  int candidate = cand0 + blockIdx.x; /* CSIRS_DISC_CHUNK: a chunk launch covers candidates [cand0, cand0+gridDim.x) of nctot */
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
  /* Staged loads (block_re == 32, the production value): lane L owns block b0+L, i.e. REs 32 apart, so reading map.re[] and rx[] straight from global memory made every warp load touch 32 separate
   * lines (L1-wavefront bound: 213 ms for 1024 IDs x 4 slots x 4 RX x 55552 REs on air / replay, 8% of the issue rate [MEASURED, DGX replay 2026-10-23]). The warp now loads a tile of 8 REs of each
   * of its 32 blocks with coalesced reads into shared memory laid out [k][block] (stride 33: no bank conflicts) and each lane reads its own block from there. The per-lane arithmetic is the
   * unchanged sequence k = 0..count-1, so the scores are bit-identical (tests/passive_rx/gpuspan/disc_sweep_test.cu). Other block_re values use the original loop. */
  __shared__ nr_csirs_disc_re_t tre[4][8][33];
  __shared__ short2 tsm[4][8][33];
  for (int task = warp; task < map.ports * ants; task += 4) {
    int p = task / ants, ant = task % ants;
    int begin = map.begin[p], n = map.begin[p+1] - begin;
    // This form also supports block_re greater than n without integer overflow.
    int nb = n ? 1 + (n - 1) / cfg.block_re : 0;
    float rho = 0, active = 0;
    const short2 *samples = rx + ((size_t)blockIdx.y * ants + ant) * map.n_re;
    if (cfg.block_re == 32) {
      for (int b0 = 0; b0 < nb; b0 += 32) {
        const int b = b0 + lane, rows = min(32, nb - b0);
        const int first = b * 32, count = b < nb ? min(32, n - first) : 0;
        float ar = 0, ai = 0, energy = 0;
        for (int k0 = 0; k0 < 32; k0 += 8) {
          __syncwarp();
          for (int row = lane / 8; row < rows; row += 4) {
            const int idx = (b0 + row) * 32 + k0 + lane % 8;
            if (idx < n) {
              tre[warp][lane % 8][row] = map.re[begin + idx];
              tsm[warp][lane % 8][row] = samples[begin + idx];
            }
          }
          __syncwarp();
          for (int j = 0; j < 8; ++j) {
            if (k0 + j >= count) break;
            nr_csirs_disc_re_t re = tre[warp][j][lane];
            unsigned bits = (gold[re.symbol][re.m / 16] >> (2 * (re.m % 16))) & 3;
            // OAI QPSK at amplitude 16384 is exactly (+/-8192,+/-8192).
            // Unit amplitudes cancel from the normalized correlation.
            float xr = (bits & 1 ? -1.f : 1.f) * re.sign;
            float xi = (bits & 2 ? -1.f : 1.f) * re.sign;
            short2 y = tsm[warp][j][lane];
            float yr = y.x, yi = y.y;
            ar += yr*xr + yi*xi;
            ai += yi*xr - yr*xi;
            energy += yr*yr + yi*yi;
          }
        }
        if (b < nb && energy > 0) {
          rho += fminf(1.f, sqrtf((ar*ar + ai*ai) / (energy * (2 * count))));
          active += 1;
        }
      }
    } else
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
    scores[(size_t)blockIdx.y * (nctot ? nctot : gridDim.x) + candidate] = (sums[0]+sums[1]+sums[2]+sums[3]) / (map.ports*ants);
}

/* Average occasions before computing the candidate distribution. */
template<bool accumulate=false>
__global__ void average_scores(const float *scores, int nc, int slots, float *mean, float *history=nullptr, int previous=0)
{
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= nc) return;
  double sum = 0;
  for (int s = 0; s < slots; ++s) sum += scores[(size_t)s * nc + i];
  if (accumulate) {
    if (previous) sum += (double)history[i]*previous;
    mean[i] = sum/(slots+previous);
    history[i] = mean[i];
  } else mean[i] = sum / slots;
}

/* reduce_scores: GPU kernel (one CTA): finds the best candidate (ID and slot delta, ties to the lower ID) from the averaged scores, and writes the window (z, z_second, noise mean and sigma) to *out.
 *   Noise statistics use FP64 sums over all other candidates. Launched by run() (occurrence and cumulative paths) and by nr_csirs_disc_gpu_sweep_prepare().
 */
__global__ void reduce_scores(const float *mean, nr_csirs_disc_sweep_cfg_t c, int hyp, int slots,
                             nr_csirs_disc_win_t *out)
{
  __shared__ float best[256];
  __shared__ int index[256];
  __shared__ double sum[256], sq[256];
  __shared__ int winner;
  int t = threadIdx.x, nc = c.id_count * c.delta_max;
  float z = 0;
  int at = 0;
  for (int i = t; i < nc; i += 256)
    if (mean[i] > z) { z = mean[i]; at = i; }
  best[t] = z; index[t] = at;
  __syncthreads();
  for (int stride = 128; stride; stride /= 2) {
    if (t < stride && (best[t + stride] > best[t]
        || (best[t + stride] == best[t] && index[t + stride] < index[t]))) {
      best[t] = best[t + stride]; index[t] = index[t + stride];
    }
    __syncthreads();
  }
  if (!t) {
    nr_csirs_disc_win_t w = {};
    w.hyp_index = hyp; w.scramb_id = c.id_first + index[0] / c.delta_max;
    w.slot_delta = index[0] % c.delta_max; w.z = best[0]; w.n_slots = slots;
    *out = w;
    winner = index[0] / c.delta_max;
  }
  __syncthreads();
  double a = 0, b = 0;
  const double origin = c.id_count > 1 ? mean[winner == 0 ? c.delta_max : 0] : 0;
  z = 0;
  for (int i = t; i < nc; i += 256) if (i / c.delta_max != winner) {
    const double v = (double)mean[i] - origin;
    a += v; b += v * v; z = fmaxf(z, mean[i]);
  }
  best[t] = z; sum[t] = a; sq[t] = b;
  __syncthreads();
  for (int stride = 128; stride; stride /= 2) {
    if (t < stride) {
      best[t] = fmaxf(best[t], best[t + stride]);
      sum[t] += sum[t + stride]; sq[t] += sq[t + stride];
    }
    __syncthreads();
  }
  if (!t) {
    out->z_second = best[0];
    if (slots && c.id_count > 1) {
      out->noise_count = (c.id_count - 1) * c.delta_max;
      const double mu = sum[0] / out->noise_count;
      out->noise_mu = origin + mu;
      out->noise_sigma = sqrt(fmax(0.0, sq[0] / out->noise_count - mu * mu));
    }
  }
}

template<typename T> struct Device {
  T *p = nullptr;
  size_t capacity = 0;
  /* No cudaFree here (T26): `workspace` is a static object, so this ran from exit() after the CUDA runtime's own
   * atexit teardown and while other threads still held CUDA work -- libcuda then freed already-released host state
   * ("free(): corrupted unsorted chunks" / "malloc(): unsorted double linked list corrupted" at RAW_IQ_EOF in every
   * PCI 415 replay that reached the end of the capture). Device memory goes with the process. */
  ~Device() = default;
/* alloc: Device<T>::alloc: grows the device buffer to at least n elements (freeing the old one first); returns false when the free or the allocation fails.
 *   Members of the Device template used by Workspace; called by prepare().
 */
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
struct RefineAcc {double a[NR_CSIRS_DISC_MAX_RB],b[NR_CSIRS_DISC_MAX_RB];}; /* refine accumulators (evidence; or signal and weight) */
struct Workspace {
  cudaStream_t stream=nullptr,stream_hi=nullptr,stream_lo=nullptr; // stream = the one of the current job (ISAC_GPU_DISC_SMS_LOW)
  cudaEvent_t done=nullptr;
  Device<nr_csirs_disc_re_t> dm;
  Device<short2> dx,grid;
  Device<int> ds,di,result;
  Device<float> dz,dmean;
  Device<nr_csirs_disc_win_t> dw;
  Device<nr_csirs_disc_slot_meta_t> meta;
  Device<nr_csirs_disc_hyp_t> refined;
  std::vector<nr_csirs_disc_re_t> mapping;
  std::vector<int> slots,indices;
  std::vector<float> mean;
  nr_csirs_disc_evidence_t evidence={};
  Device<RefineAcc> acc;
  Device<double> terms; /* refine: per-unit contributions of one chunk, [unit][NR_CSIRS_DISC_MAX_RB] (row 1: signal then weight arrays) */
  int rb=0,ant=0,capacity=0;
} workspace;
static std::mutex workspace_mutex;
extern "C" int nr_gpu_budget_rt_large_active(void) __attribute__((weak)); // the executable's nr_gpu_budget.c (weak: the module is dlopen'd)
/* pick_stream: start of a job (workspace_mutex held): the low-SM stream while a large real grant is in flight, else the normal one. The previous job is synchronised first, so no queued kernel ever sees the switch. */
static void pick_stream()
{
  auto &w=workspace;
  if(!w.stream_lo) return;
  cudaStream_t s=nr_gpu_green_disc_pick(w.stream_hi,w.stream_lo,nr_gpu_budget_rt_large_active?nr_gpu_budget_rt_large_active():0);
  if(s!=w.stream){nr_gpu_wait_stream(w.stream);w.stream=s;}
}
/* prepare: prepares the GPU sweep workspace for a carrier (rb RBs, ant antennas, capacity slots): creates the stream and completion event and grows every device buffer (mapping, packed samples, scores, metadata).
 *   Grow-only: returns at once when the existing buffers already fit. Returns 0, or -1 on failure. Callers: run(), nr_csirs_disc_gpu_sweep_prepare(), nr_csirs_disc_gpu_refine_entry().
 */
static int prepare(int rb,int ant,int capacity)
{
  auto &w=workspace;
  if(w.rb>=rb && w.ant>=ant && w.capacity>=capacity) return 0;
  if(!w.stream_hi){
    if(!nr_gpu_green_stream(&w.stream_hi,0,false) && cudaStreamCreateWithFlags(&w.stream_hi,cudaStreamNonBlocking)!=cudaSuccess) return -1;
    if(!nr_gpu_green_stream(&w.stream_lo,0,false,-1)) w.stream_lo=nullptr;
    w.stream=w.stream_hi;
  }
  if(!w.done && cudaEventCreateWithFlags(&w.done,cudaEventDisableTiming)!=cudaSuccess) return -1;
  size_t maps=(size_t)rb*16*8*2*4; // conservative mapping capacity for all rows
  // Occupied REs <= 32 ports * 8 RE/RB (row 18).
  size_t packed=(size_t)rb*32*8*ant*capacity;
  if(!w.dm.alloc(maps) || !w.dx.alloc(packed) || !w.grid.alloc((size_t)capacity*ant*14*12*rb)
     || !w.ds.alloc(capacity) || !w.di.alloc(capacity) || !w.dz.alloc((size_t)1024*160*capacity)
     || !w.dmean.alloc(1024*160) || !w.dw.alloc(1) || !w.meta.alloc(capacity)
     || !w.acc.alloc(1) || !w.terms.alloc((size_t)capacity*ant*32*NR_CSIRS_DISC_MAX_RB) || !w.refined.alloc(1) || !w.result.alloc(1)) return -1;
  w.mapping.resize(maps);w.slots.reserve(capacity);w.indices.reserve(capacity);w.mean.resize(1024*160);
  w.rb=rb;w.ant=ant;w.capacity=capacity;return 0;
}
/* complete: records the workspace stream's completion event and blocks the calling CPU thread until the queued work finished; returns the CUDA status.
 *   Callers: copy(), run() and the entry points.
 */
static cudaError_t complete() {
  cudaError_t rc=cudaEventRecord(workspace.done,workspace.stream);
  return rc==cudaSuccess?nr_gpu_wait_event(workspace.done):rc;
}
/* copy: asynchronous memcpy on the workspace stream followed by complete(); returns the CUDA status. */
static cudaError_t copy(void *dst,const void *src,size_t n,cudaMemcpyKind kind) {
  /* wait for queued kernels first: a copy queued behind a long kernel stalls other streams' copies [MEASURED, DGX 300-400 ms] */
  {cudaError_t f=complete();if(f!=cudaSuccess) return f;}
  cudaError_t rc=cudaMemcpyAsync(dst,src,n,kind,workspace.stream);
  return rc==cudaSuccess?complete():rc;
}
/* chunks: runs total units through launch(first,n), draining the workspace stream between chunks (nr_gpu_chunk.h: a long discovery kernel holds the body launch of the real-time pass graphs). */
template<class L> static cudaError_t chunks(NrGpuChunker &c,long total,L launch)
{
  cudaError_t err=cudaSuccess;
  static thread_local NrGpuSpan span; /* GPU run time of the chunk, not its host time (nr_gpu_chunk.h) */
  int rc=nr_gpu_chunked(c,total,[&](long f,long n){span.begin(workspace.stream);launch(f,n);span.end(workspace.stream);err=cudaGetLastError();return (int)err;},[&](){err=complete();return (int)err;},[&](){return span.us();});
  return rc?err:cudaSuccess;
}
/* gather: GPU kernel: packs the mapped resource elements of the selected slots and antennas from the ring grid into the packed sample buffer (one thread per packed element).
 *   Launched by run() and nr_csirs_disc_gpu_sweep_prepare().
 */
__global__ void gather(const short2 *grid,size_t stride,const nr_csirs_disc_re_t *map,int nr,
                      const int *indices,int slots,int ants,int rb,short2 *packed)
{
  size_t id=(size_t)blockIdx.x*blockDim.x+threadIdx.x;
  if(id>=(size_t)slots*ants*nr) return;
  int i=id%nr,a=id/nr%ants,t=id/(nr*ants);
  packed[id]=grid[(size_t)indices[t]*stride+(size_t)a*14*12*rb+map[i].grid_index];
}
/* upload_grid: returns the device pointer of the ring grid for the sweep: the resident device grid when it is valid, otherwise the workspace copy after an upload of the host grid.
 *   Returns NULL when the upload fails. Callers: run() and nr_csirs_disc_gpu_refine_entry().
 */
static const short2 *upload_grid(const nr_csirs_disc_ring_t *r)
{
  if(r->device_grid && r->device_valid) return (const short2 *)r->device_grid;
  if(copy(workspace.grid.p,r->grid_base,(size_t)r->capacity*r->slot_stride*2,cudaMemcpyHostToDevice)!=cudaSuccess) return nullptr;
  return workspace.grid.p;
}

/* run: GPU sweep of a list of hypotheses over the ring (the GPU counterpart of nr_csirs_disc_sweep_cpu()): builds the mapping, gathers the slots, runs sweep(), averages with the cumulative evidence and reduces to a window per hypothesis.
 *   Returns nh on success, -1 on error. Serialised by the caller's workspace mutex. Caller: nr_csirs_disc_gpu_sweep_entry().
 */
int run(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_hyp_t *hyps, int nh,
        const nr_csirs_disc_sweep_cfg_t *cfg, nr_csirs_disc_win_t *out)
{
  nr_csirs_disc_sweep_cfg_t c;
  if (!out || nr_csirs_disc_sweep_validate(r, hyps, nh, cfg, &c))
    return -1;
  if(prepare(r->n_rb,r->n_ant,r->capacity)<0) return -1;
  auto &w=workspace;cudaStream_t stream=w.stream;
  const short2 *grid=upload_grid(r);if(!grid) return -1;
  const int nc=c.id_count*c.delta_max;
  if (nc>1024) memset(&w.evidence,0,sizeof(w.evidence));
  auto &mean=w.mean;auto &dm=w.dm;auto &dx=w.dx;auto &ds=w.ds;
  auto &dz=w.dz;auto &dmean=w.dmean;auto &dw=w.dw;
  for (int h = 0; h < nh; ++h) {
    nr_csirs_disc_sweep_map_t map={};
    if (nr_csirs_disc_sweep_map_into(hyps+h,r->n_rb,&map,w.mapping.data(),w.mapping.size()) || map.words>words_max)
      return -1;
    int bank=nr_csirs_disc_evidence_begin(&w.evidence,r,hyps+h,&c);
    auto *entry=bank>=0?&w.evidence.entry[bank]:nullptr;
    // Reserve the tail of the existing [capacity][160 deltas][1024 IDs]
    // score allocation. With capacity>=2 it is disjoint from <=4*1024
    // active scores. Wide delta calls invalidate metadata above before use.
    float *history=entry?dz.p+dz.capacity-NR_CSIRS_EVIDENCE_BANK*NR_CSIRS_EVIDENCE_IDS
                              +bank*NR_CSIRS_EVIDENCE_IDS:nullptr;
    auto &slots=w.slots;auto &indices=w.indices;slots.clear();indices.clear();
    for (int i = 0; i < r->capacity; ++i)
      if (r->meta[i].valid && r->meta[i].abs_slot % hyps[h].period == hyps[h].offset
          && nr_csirs_disc_evidence_fresh(entry,r->meta[i].abs_slot)) {
        slots.push_back(r->meta[i].slot);
        indices.push_back(i);
      }
    if(c.max_slots>0 && indices.size()>(size_t)c.max_slots) {
      std::sort(indices.begin(),indices.end(),[&](int a,int b) {return r->meta[a].abs_slot!=r->meta[b].abs_slot?r->meta[a].abs_slot<r->meta[b].abs_slot:a<b;});
      indices.erase(indices.begin(),indices.end()-c.max_slots);
      slots.clear();for(int i:indices) slots.push_back(r->meta[i].slot);
    }
    std::fill(mean.begin(), mean.begin()+nc, 0);
    if (entry && slots.empty() && entry->count) {
      reduce_scores<<<1,256,0,stream>>>(history,c,h,entry->count,dw.p);
      if (cudaGetLastError()!=cudaSuccess || copy(out+h,dw.p,sizeof(*out),cudaMemcpyDeviceToHost)!=cudaSuccess)
        return -1;
      continue;
    }
    if (slots.empty() || !map.n_re) {
      nr_csirs_disc_sweep_winner(mean.data(), &c, h, slots.size(), out + h);
      continue;
    }
    if((complete()!=cudaSuccess)
       || cudaMemcpyAsync(dm.p,map.re,map.n_re*sizeof(*map.re),cudaMemcpyHostToDevice,stream)!=cudaSuccess
       || cudaMemcpyAsync(ds.p,slots.data(),slots.size()*sizeof(int),cudaMemcpyHostToDevice,stream)!=cudaSuccess
       || cudaMemcpyAsync(w.di.p,indices.data(),indices.size()*sizeof(int),cudaMemcpyHostToDevice,stream)!=cudaSuccess) return -1;
    size_t packed=(size_t)slots.size()*r->n_ant*map.n_re;
    gather<<<(packed+255)/256,256,0,stream>>>(grid,r->slot_stride/2,dm.p,map.n_re,w.di.p,slots.size(),r->n_ant,r->n_rb,dx.p);
    auto device_map = map;
    device_map.re = dm.p;
    static NrGpuChunker c_sweep{1,0,"c_sweep"};
    if (chunks(c_sweep, nc, [&](long f, long n) {
          sweep<<<dim3(n, slots.size()), 128, 0, stream>>>(device_map, dx.p, ds.p, r->n_ant, r->slots_per_frame, c, dz.p, (int)f, nc);
        }) != cudaSuccess) return -1;
    int previous=nr_csirs_disc_evidence_previous(entry,slots.size());
    if (entry) average_scores<true><<<(nc + 255) / 256, 256, 0, stream>>>(dz.p, nc, slots.size(), dmean.p, history, previous);
    else average_scores<false><<<(nc + 255) / 256, 256, 0, stream>>>(dz.p, nc, slots.size(), dmean.p);
    if (cudaGetLastError() != cudaSuccess) return -1;
    reduce_scores<<<1, 256, 0, stream>>>(dmean.p, c, h, slots.size()+previous, dw.p);
    if (cudaGetLastError() != cudaSuccess || (complete() != cudaSuccess)
        || cudaMemcpyAsync(out + h, dw.p, sizeof(*out), cudaMemcpyDeviceToHost, stream) != cudaSuccess
        || complete() != cudaSuccess)
      return -1;
    if (entry) {
      for (int i:indices) entry->last_slot=std::max(entry->last_slot,r->meta[i].abs_slot);
      entry->count=slots.size()+previous;
      out[h].fresh_slots=slots.size();
    }
  }
  return nh;
}
/* refine_extent: device version of the CPU refine_extent(): per RB, cross-correlates the scrambled reference with the neighbours and keeps the strongest contiguous span.
 *   Returns 0 and updates h->rb_start and h->n_rb, or -1 when no span is found. Called from refine_kernel() (through nr_csirs_disc_refine_cpu).
 */
__device__ static int refine_extent(const nr_csirs_disc_ring_t *r, nr_csirs_disc_hyp_t *h,
                         const nr_csirs_disc_win_t *win, nr_csirs_disc_sweep_map_t map,
                         RefineAcc *acc, long u0, long u1, bool fin)
{
  /* CSIRS_DISC_CHUNK: this call does the units [u0,u1) of (valid slot, antenna, port) in the original order; the evidence sums live in acc
   * between calls; fin (the last chunk) also does the tail. u0 = 0, u1 = LONG_MAX, fin = true is the whole job in one call. */
  double *evidence=acc->a;
  if(u0==0) for(int i=0;i<NR_CSIRS_DISC_MAX_RB;++i) evidence[i]=0;
  uint32_t gold[14][64];
  long unit=0;
  for(int s=0;s<r->capacity;++s) if(r->meta[s].valid && r->meta[s].abs_slot%h->period==h->offset) {
    const long first=unit;unit+=(long)r->n_ant*map.ports;
    if(unit<=u0 || first>=u1) continue;
    for(int l=0;l<14;++l) if(map.sym_mask&(1u<<l)) {
      uint64_t t=14*((r->meta[s].slot+win->slot_delta)%r->slots_per_frame)+l+1;
      uint32_t init=((t*(2*win->scramb_id+1)<<10)+win->scramb_id)&0x7fffffff;
      Gold generator;generator.reset(init);
      for(int w=0;w<map.words;++w) gold[l][w]=generator.next();
    }
    for(int a=0;a<r->n_ant;++a) for(int p=0;p<map.ports;++p) {
      const long ua=first+(long)a*map.ports+p;
      if(ua<u0 || ua>=u1) continue;
      double cr[NR_CSIRS_DISC_MAX_RB]={0},ci[NR_CSIRS_DISC_MAX_RB]={0};
      int count[NR_CSIRS_DISC_MAX_RB]={0};
      const int16_t *rx=(r->grid_base+(size_t)s*r->slot_stride+(size_t)a*14*24*r->n_rb);
      for(int i=map.begin[p];i<map.begin[p+1];++i) {
        const nr_csirs_disc_re_t *re=map.re+i;
        int rb=(re->grid_index%(12*r->n_rb))/12;
        unsigned bits=(gold[re->symbol][re->m/16]>>(2*(re->m%16)))&3;
        double yr=rx[2*re->grid_index],yi=rx[2*re->grid_index+1];
        int xr=(bits&1?-1:1)*re->sign,xi=(bits&2?-1:1)*re->sign;
        cr[rb]+=yr*xr+yi*xi;ci[rb]+=yi*xr-yr*xi;++count[rb];
      }
      const int step=h->density<2?2:1;
      for(int rb=0;rb<r->n_rb;++rb) if(count[rb]) {
        double hr=0,hi=0;int n=0;
        for(int k=rb-2*step;k<=rb+2*step;k+=step) if(k>=0 && k<r->n_rb && k!=rb) {
          hr+=cr[k];hi+=ci[k];n+=count[k];
        }
        if(n) {
          hr/=2*n;hi/=2*n;
          // Leave this RB out of the channel estimate: noise has negative expected gain.
          evidence[rb]+=2*(cr[rb]*hr+ci[rb]*hi)-2*count[rb]*(hr*hr+hi*hi);
        }
      }
    }
  }
  if(!fin) return 0;
  int step=h->density<2?2:1,first=h->density<2?h->density:0;
  double run=0,best=0;int start=first,bs=0,be=-1;
  for(int rb=first;rb<r->n_rb;rb+=step) {
    if(run<=0) {run=evidence[rb];start=rb;} else run+=evidence[rb];
    if(run>best && rb>=h->rb_start && start<h->rb_start+h->n_rb) {best=run;bs=start;be=rb;}
  }
  if(be<bs) return -1;
  h->rb_start=bs;h->n_rb=be-bs+1;return 0;
}

/* nr_csirs_disc_refine_cpu: device version of the CPU extent refinement: sequence-qualified row-1 TRS search (energy-weighted per 4-RB group); other rows use refine_extent().
 *   Returns 0 and updates h->rb_start and h->n_rb, or -1 when the evidence is too weak. Called from refine_kernel().
 */
__device__ static int nr_csirs_disc_refine_cpu(const nr_csirs_disc_ring_t *r, nr_csirs_disc_hyp_t *h,
                           const nr_csirs_disc_win_t *win, nr_csirs_disc_sweep_map_t map,
                           RefineAcc *acc, long u0, long u1, bool fin)
{
  if (h->row != 1) return refine_extent(r,h,win,map,acc,u0,u1,fin);
  /* CSIRS_DISC_CHUNK: units [u0,u1) of (valid slot, antenna); signal / weight are carried in acc (see refine_extent) */
  double *signal=acc->a,*weight=acc->b;
  if(u0==0) for(int i=0;i<NR_CSIRS_DISC_MAX_RB;++i) signal[i]=weight[i]=0;
  uint32_t gold[64];
  int slots=0;
  long unit=0;
  for(int s=0;s<r->capacity;++s) if(r->meta[s].valid && r->meta[s].abs_slot%h->period==h->offset) {
    ++slots;
    const long first=unit;unit+=r->n_ant;
    if(unit<=u0 || first>=u1) continue;
    uint64_t t=14*((r->meta[s].slot+win->slot_delta)%r->slots_per_frame)+h->symb_l0+1;
    uint32_t init=((t*(2*win->scramb_id+1)<<10)+win->scramb_id)&0x7fffffff;
    Gold generator;generator.reset(init);
    for(int w=0;w<map.words;++w) gold[w]=generator.next();
    for(int a=0;a<r->n_ant;++a) for(int rb=0;rb<r->n_rb;++rb) {
      if(first+a<u0 || first+a>=u1) continue;
      const int16_t *rx=(r->grid_base+(size_t)s*r->slot_stride+(size_t)a*14*24*r->n_rb);
      double ar=0,ai=0,energy=0;
      for(int i=3*rb;i<3*rb+3;++i) {
        const nr_csirs_disc_re_t *re=map.re+i;
        unsigned bits=(gold[re->m/16]>>(2*(re->m%16)))&3;
        double yr=rx[2*re->grid_index],yi=rx[2*re->grid_index+1];
        int xr=bits&1?-1:1,xi=bits&2?-1:1;
        ar+=yr*xr+yi*xi;ai+=yi*xr-yr*xi;energy+=yr*yr+yi*yi;
      }
      /* Energy weighting naturally downweights a disconnected RX branch. */
      signal[rb]+=(ar*ar+ai*ai-2*energy)/4;
      weight[rb]+=energy;
    }
  }
  if(!fin) return 0;
  /* TS 38.214 5.2.2.3.1: configured startingRB/nrofRBs are multiples
   * of four. Maximize measured support over that legal grid; no width prior.
   * A capture boundary may clip the final group. */
  double run=0,best=0;int start=0,bs=0,be=-1;
  for(int rb=0;rb<r->n_rb;rb+=4) {
    int end=rb+4<r->n_rb?rb+4:r->n_rb;double e=0;
    for(int k=rb;k<end;++k) e+=weight[k]>0?signal[k]/weight[k]-0.35:-1;
    if(run<=0) {run=e;start=rb;} else run+=e;
    if(run>best && end>h->rb_start && start<h->rb_start+h->n_rb) {best=run;bs=start;be=end-1;}
  }
  if(slots<3 || be-bs+1<4) return -1;
  h->rb_start=bs;h->n_rb=be-bs+1;
  return 0;
}

/* find_unit: device helper: the ring slot and the first unit of the slot that contains unit u of the refinement, or -1 when u is beyond the last matching slot.
 *   Units are numbered in the order of refine_extent() / nr_csirs_disc_refine_cpu(): valid matching slots in ring order, then antenna (then port). per_slot = units per slot. */
__device__ static int find_unit(const nr_csirs_disc_ring_t *r,const nr_csirs_disc_hyp_t *h,long u,long per_slot,long *first)
{
  long unit=0;
  for(int s=0;s<r->capacity;++s) if(r->meta[s].valid && r->meta[s].abs_slot%h->period==h->offset) {
    if(u<unit+per_slot) {*first=unit;return s;}
    unit+=per_slot;
  }
  return -1;
}
/* refine_extent_units: GPU kernel, one CTA per unit (valid slot, antenna, port) = unit u0+blockIdx.x: the parallel form of the unit body of refine_extent().
 *   The per-RB correlations cr / ci / count are sums of integer-valued doubles (|y| < 2^16, at most a few thousand terms): exact in double, so the shared-memory atomic adds are
 *   order-independent. The term added to the evidence of each RB is then computed with the same expression as refine_extent() and stored in terms[blockIdx.x][rb] (0 when the
 *   original adds nothing); refine_accumulate() adds the terms to the evidence in unit order, so the evidence is bit-identical to the serial kernel (tested). Launched by refine_chunk(). */
__global__ void refine_extent_units(nr_csirs_disc_ring_t ring,nr_csirs_disc_hyp_t h,nr_csirs_disc_win_t win,nr_csirs_disc_sweep_map_t map,double *terms,long u0)
{
  const nr_csirs_disc_ring_t *r=&ring;
  __shared__ uint32_t gold[14][words_max];
  __shared__ double cr[NR_CSIRS_DISC_MAX_RB],ci[NR_CSIRS_DISC_MAX_RB];
  __shared__ int count[NR_CSIRS_DISC_MAX_RB];
  __shared__ int sl,a_s,p_s;
  double *out=terms+(size_t)blockIdx.x*NR_CSIRS_DISC_MAX_RB;
  if(threadIdx.x==0) {
    long first=0;const long u=u0+blockIdx.x;
    sl=find_unit(r,&h,u,(long)r->n_ant*map.ports,&first);
    a_s=(int)((u-first)/map.ports);p_s=(int)((u-first)%map.ports);
  }
  __syncthreads();
  if(sl<0) {for(int rb=threadIdx.x;rb<NR_CSIRS_DISC_MAX_RB;rb+=blockDim.x) out[rb]=0;return;}
  for(int i=threadIdx.x;i<NR_CSIRS_DISC_MAX_RB;i+=blockDim.x) {cr[i]=0;ci[i]=0;count[i]=0;}
  if(threadIdx.x<14 && (map.sym_mask&(1u<<threadIdx.x))) {
    const int l=threadIdx.x;
    uint64_t t=14*((r->meta[sl].slot+win.slot_delta)%r->slots_per_frame)+l+1;
    uint32_t init=((t*(2*win.scramb_id+1)<<10)+win.scramb_id)&0x7fffffff;
    Gold generator;generator.reset(init);
    for(int w=0;w<map.words;++w) gold[l][w]=generator.next();
  }
  __syncthreads();
  const int16_t *rx=(r->grid_base+(size_t)sl*r->slot_stride+(size_t)a_s*14*24*r->n_rb);
  for(int i=map.begin[p_s]+threadIdx.x;i<map.begin[p_s+1];i+=blockDim.x) {
    const nr_csirs_disc_re_t *re=map.re+i;
    int rb=(re->grid_index%(12*r->n_rb))/12;
    unsigned bits=(gold[re->symbol][re->m/16]>>(2*(re->m%16)))&3;
    double yr=rx[2*re->grid_index],yi=rx[2*re->grid_index+1];
    int xr=(bits&1?-1:1)*re->sign,xi=(bits&2?-1:1)*re->sign;
    atomicAdd(&cr[rb],yr*xr+yi*xi);atomicAdd(&ci[rb],yi*xr-yr*xi);atomicAdd(&count[rb],1);
  }
  __syncthreads();
  const int step=h.density<2?2:1;
  for(int rb=threadIdx.x;rb<r->n_rb;rb+=blockDim.x) {
    double term=0;
    if(count[rb]) {
      double hr=0,hi=0;int n=0;
      for(int k=rb-2*step;k<=rb+2*step;k+=step) if(k>=0 && k<r->n_rb && k!=rb) {
        hr+=cr[k];hi+=ci[k];n+=count[k];
      }
      if(n) {
        hr/=2*n;hi/=2*n;
        term=2*(cr[rb]*hr+ci[rb]*hi)-2*count[rb]*(hr*hr+hi*hi);
      }
    }
    out[rb]=term;
  }
}
/* refine_trs_units: GPU kernel, one CTA per unit (valid slot, antenna) = unit u0+blockIdx.x: the parallel form of the unit body of the row-1 branch of nr_csirs_disc_refine_cpu().
 *   One thread per RB; the signal and weight terms go to terms_s / terms_w[blockIdx.x][rb] and refine_accumulate() adds them in unit order (bit-identical to the serial kernel, tested). */
__global__ void refine_trs_units(nr_csirs_disc_ring_t ring,nr_csirs_disc_hyp_t h,nr_csirs_disc_win_t win,nr_csirs_disc_sweep_map_t map,double *terms_s,double *terms_w,long u0)
{
  const nr_csirs_disc_ring_t *r=&ring;
  __shared__ uint32_t gold[words_max];
  __shared__ int sl,a_s;
  double *os=terms_s+(size_t)blockIdx.x*NR_CSIRS_DISC_MAX_RB,*ow=terms_w+(size_t)blockIdx.x*NR_CSIRS_DISC_MAX_RB;
  if(threadIdx.x==0) {
    long first=0;const long u=u0+blockIdx.x;
    sl=find_unit(r,&h,u,(long)r->n_ant,&first);
    a_s=(int)(u-first);
    if(sl>=0) {
      uint64_t t=14*((r->meta[sl].slot+win.slot_delta)%r->slots_per_frame)+h.symb_l0+1;
      uint32_t init=((t*(2*win.scramb_id+1)<<10)+win.scramb_id)&0x7fffffff;
      Gold generator;generator.reset(init);
      for(int w=0;w<map.words;++w) gold[w]=generator.next();
    }
  }
  __syncthreads();
  if(sl<0) {for(int rb=threadIdx.x;rb<NR_CSIRS_DISC_MAX_RB;rb+=blockDim.x) os[rb]=ow[rb]=0;return;}
  const int16_t *rx=(r->grid_base+(size_t)sl*r->slot_stride+(size_t)a_s*14*24*r->n_rb);
  for(int rb=threadIdx.x;rb<r->n_rb;rb+=blockDim.x) {
    double ar=0,ai=0,energy=0;
    for(int i=3*rb;i<3*rb+3;++i) {
      const nr_csirs_disc_re_t *re=map.re+i;
      unsigned bits=(gold[re->m/16]>>(2*(re->m%16)))&3;
      double yr=rx[2*re->grid_index],yi=rx[2*re->grid_index+1];
      int xr=bits&1?-1:1,xi=bits&2?-1:1;
      ar+=yr*xr+yi*xi;ai+=yi*xr-yr*xi;energy+=yr*yr+yi*yi;
    }
    os[rb]=(ar*ar+ai*ai-2*energy)/4;
    ow[rb]=energy;
  }
}
/* refine_accumulate: GPU kernel, one thread per RB: acc.a[rb] += ta[i][rb] (and acc.b[rb] += tb[i][rb] when tb is set) for the n units of the chunk in unit order (the order of the serial
 *   kernel); zero first accumulators when the chunk starts at unit 0. */
__global__ void refine_accumulate(RefineAcc *acc,const double *ta,const double *tb,int n,int n_rb,bool zero)
{
  const int rb=blockIdx.x*blockDim.x+threadIdx.x;
  if(rb>=NR_CSIRS_DISC_MAX_RB) return;
  double a=zero?0:acc->a[rb],b=zero?0:acc->b[rb];
  if(rb<n_rb) for(int i=0;i<n;++i) {a+=ta[(size_t)i*NR_CSIRS_DISC_MAX_RB+rb];if(tb) b+=tb[(size_t)i*NR_CSIRS_DISC_MAX_RB+rb];}
  acc->a[rb]=a;if(tb || zero) acc->b[rb]=b;
}
/* refine_kernel: GPU kernel (one thread): runs the device refinement (nr_csirs_disc_refine_cpu) on the ring and stores the refined hypothesis and the result code. */
__global__ void refine_kernel(nr_csirs_disc_ring_t ring,nr_csirs_disc_hyp_t h,nr_csirs_disc_win_t win,
                             nr_csirs_disc_sweep_map_t map,nr_csirs_disc_hyp_t *out,int *result,
                             RefineAcc *acc,long u0,long u1,bool fin)
{
  const int rc=nr_csirs_disc_refine_cpu(&ring,&h,&win,map,acc,u0,u1,fin);
  if(fin) {*result=rc;*out=h;}
}
/* refine_chunk: host side: the refinement units [f,f+n) of total on stream s: one CTA per unit (refine_extent_units / refine_trs_units), the in-order accumulation, and for the last chunk the tail
 *   (the serial kernel with an empty unit range: it runs only the span search on the accumulators). Test hook of tests/passive_rx/gpuspan/disc_refine_test.cu. */
static cudaError_t refine_chunk(const nr_csirs_disc_ring_t &ring,const nr_csirs_disc_hyp_t &h,
                                const nr_csirs_disc_win_t &win,const nr_csirs_disc_sweep_map_t &map,cudaStream_t s,long f,long n,bool last)
{
  auto &w=workspace;
  const int threads=128;
  if(h.row==1) {
    double *ts=w.terms.p,*tw=w.terms.p+(size_t)(w.terms.capacity/NR_CSIRS_DISC_MAX_RB/2)*NR_CSIRS_DISC_MAX_RB;
    refine_trs_units<<<(unsigned)n,threads,0,s>>>(ring,h,win,map,ts,tw,f);
    refine_accumulate<<<(NR_CSIRS_DISC_MAX_RB+127)/128,128,0,s>>>(w.acc.p,ts,tw,(int)n,ring.n_rb,f==0);
  } else {
    refine_extent_units<<<(unsigned)n,threads,0,s>>>(ring,h,win,map,w.terms.p,f);
    refine_accumulate<<<(NR_CSIRS_DISC_MAX_RB+127)/128,128,0,s>>>(w.acc.p,w.terms.p,nullptr,(int)n,ring.n_rb,f==0);
  }
  if(last) refine_kernel<<<1,1,0,s>>>(ring,h,win,map,w.refined.p,w.result.p,w.acc.p,LONG_MAX,LONG_MAX,true);
  return cudaGetLastError();
}
/* refine_launch: host side: the refinement in launches of about 1 ms; total = number of units. */
static cudaError_t refine_launch(const nr_csirs_disc_ring_t &ring,const nr_csirs_disc_hyp_t &h,
                                 const nr_csirs_disc_win_t &win,const nr_csirs_disc_sweep_map_t &map,cudaStream_t s,long total)
{
  /* The refinement was ONE single-thread kernel of 24 ms on average and up to 652 ms [MEASURED gsV2N]; chunked it was still serial (0.46 ms per unit idle, 1.3 ms on replay, 375 ms per call at
   * 32 units per chunk). Each unit is now one CTA (refine_chunk): the units of a chunk run in parallel and the additions into the evidence keep the unit order, so the result is bit-identical. */
  auto &w=workspace;
  static NrGpuChunker c_refine{1,0,"c_refine"};
  if(total<=0) {refine_kernel<<<1,1,0,s>>>(ring,h,win,map,w.refined.p,w.result.p,w.acc.p,0,LONG_MAX,true);return cudaGetLastError();}
  if(total>(long)(w.terms.capacity/NR_CSIRS_DISC_MAX_RB/(h.row==1?2:1))) {refine_kernel<<<1,1,0,s>>>(ring,h,win,map,w.refined.p,w.result.p,w.acc.p,0,LONG_MAX,true);return cudaGetLastError();} /* more units than the term buffer: serial */
  return chunks(c_refine,total,[&](long f,long n) {refine_chunk(ring,h,win,map,s,f,n,f+n>=total);});
}

/* energy_score: device energy statistic of one symbol: returns the energy ratio of the occupied REs against the weakest or the mean of the unoccupied subcarrier classes (EPR when epr is set).
 *   Also reports the old-style score through *old. Returns -1 when the statistic is not defined. Called from energies().
 */
__host__ __device__ double energy_score(const short2 *rx,int rb_count,int symbol,nr_csirs_disc_sweep_map_t map,
                              bool epr,double *old)
{
  uint16_t occupied[NR_CSIRS_DISC_MAX_RB]={};
  int end=epr?map.begin[1]:map.n_re;
  for(int i=0;i<end;++i) if(map.re[i].symbol==symbol) {
    int k=map.re[i].grid_index%(12*rb_count);occupied[k/12]|=1u<<(k%12);
  }
  double on=0,off[12]={},median=0;int non=0,counts[12]={},nmedian=0;
  for(int rb=0;rb<rb_count;++rb) if(occupied[rb]) {
    double values[12];int n=0;
    for(int k=0;k<12;++k) {
      short2 v=rx[symbol*12*rb_count+12*rb+k];double power=(double)v.x*v.x+(double)v.y*v.y;
      if(occupied[rb]&(1<<k)) {on+=power;++non;} else {off[k]+=power;++counts[k];values[n++]=power;}
    }
    for(int i=1;i<n;++i) {double v=values[i];int j=i;while(j && values[j-1]>v) {values[j]=values[j-1];--j;}values[j]=v;}
    if(n) {median+=values[(n-1)/2]*n;nmedian+=n;}
  }
  if(old) *old=non && nmedian && median>0?1-fmin(1.,(on/non)/(median/nmedian)):-1.;
  double weakest=-1,total=0;int classes=0,n=0;
  for(int k=0;k<12;++k) if(counts[k]) {
    double mean=off[k]/counts[k];if(weakest<0 || mean<weakest) weakest=mean;
    ++classes;total+=off[k];n+=counts[k];
  }
  if(epr) return non && n && total>0?(on/non)/(total/n):-1.;
  return non && classes>3 && weakest>0?1-fmin(1.,(on/non)/weakest):-1.;
}
/* energies: (host) computes the energy statistics of a hypothesis's symbol (sequence-free EPR, the energy score and the optional row-5 second symbol) into scores[1..4].
 *   Called by nr_csirs_disc_gpu_confirm_entry() and by the prepare path.
 */
static void energies(const short2 *grid,int rb,nr_csirs_disc_hyp_t h,nr_csirs_disc_sweep_map_t map,double *scores)
{
  double old=-1;
  scores[1]=energy_score(grid,rb,h.symb_l0,map,true,nullptr);
  scores[2]=energy_score(grid,rb,h.symb_l0,map,false,&old);
  scores[3]=h.row==5?energy_score(grid,rb,h.symb_l0+1,map,false,nullptr):scores[2];
  scores[4]=old;
}
} // namespace

/* nr_csirs_disc_gpu_sweep_entry: GPU sweep entry point (C ABI) used by the discovery ops table (nr_csirs_disc_gpu_sweep_entry as the sweep op): runs run() under the workspace mutex.
 *   On failure clears the cumulative evidence bank. Catches C++ exceptions and returns -1 instead of propagating them.
 */
extern "C" int nr_csirs_disc_gpu_sweep_entry(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_hyp_t *hyps, int nh,
                                            const nr_csirs_disc_sweep_cfg_t *cfg, nr_csirs_disc_win_t *out)
{
  if (!nr_gpu_module_ready()) return -1; // CPU tier: the caller falls back to the CPU sweep
  try {
    std::lock_guard<std::mutex> guard(workspace_mutex);
    pick_stream();
    int rc=run(r, hyps, nh, cfg, out);
    if (rc<0) memset(&workspace.evidence,0,sizeof(workspace.evidence));
    return rc;
  } catch (...) {
    return -1; // allocation failures must not escape the C module ABI
  }
}

/* nr_csirs_disc_gpu_sweep_prepare: GPU warm-up entry (C ABI): prepares the sweep workspace and runs the sweep, reduction, refinement and energy kernels once on a dummy slot, so that no CUDA allocation happens during RX.
 *   Returns 0, or -1 on failure. Called by nr_csirs_disc_pipeline_init() through the ops table. Catches C++ exceptions.
 */
extern "C" int nr_csirs_disc_gpu_sweep_prepare(int rb,int ant,int capacity)
{
  if (!nr_gpu_module_ready()) return -1;
  try {
    std::lock_guard<std::mutex> guard(workspace_mutex);
    pick_stream();
    if(prepare(rb,ant,capacity)<0) return -1;
    auto &w=workspace;nr_csirs_disc_sweep_map_t map={};map.ports=1;map.n_re=1;map.words=1;map.sym_mask=1;map.begin[1]=1;map.re=w.dm.p;
    nr_csirs_disc_re_t re={};nr_csirs_disc_slot_meta_t meta={};int zero=0;
    if(copy(w.dm.p,&re,sizeof(re),cudaMemcpyHostToDevice)!=cudaSuccess
       || copy(w.meta.p,&meta,sizeof(meta),cudaMemcpyHostToDevice)!=cudaSuccess
       || copy(w.di.p,&zero,sizeof(zero),cudaMemcpyHostToDevice)!=cudaSuccess
       || copy(w.ds.p,&zero,sizeof(zero),cudaMemcpyHostToDevice)!=cudaSuccess
       || cudaMemsetAsync(w.grid.p,0,(size_t)ant*14*12*rb*sizeof(short2),w.stream)!=cudaSuccess) return -1;
    gather<<<1,32,0,w.stream>>>(w.grid.p,(size_t)ant*14*12*rb,w.dm.p,1,w.di.p,1,ant,rb,w.dx.p);
    nr_csirs_disc_sweep_cfg_t cfg={0,1,1,32,1};
    sweep<<<dim3(1,1),128,0,w.stream>>>(map,w.dx.p,w.ds.p,ant,20,cfg,w.dz.p);
    average_scores<false><<<1,256,0,w.stream>>>(w.dz.p,1,1,w.dmean.p);
    average_scores<true><<<1,256,0,w.stream>>>(w.dz.p,1,1,w.dmean.p,w.dz.p+w.dz.capacity-1024,0);
    reduce_scores<<<1,256,0,w.stream>>>(w.dmean.p,cfg,0,1,w.dw.p);
    nr_csirs_disc_ring_t ring={};ring.n_ant=ant;ring.n_rb=rb;ring.capacity=1;ring.slots_per_frame=20;
    ring.slot_stride=(size_t)ant*14*24*rb;ring.grid_base=(int16_t *)w.grid.p;ring.meta=w.meta.p;
    nr_csirs_disc_hyp_t h={};h.row=1;h.period=4;nr_csirs_disc_win_t win={};
    refine_kernel<<<1,1,0,w.stream>>>(ring,h,win,map,w.refined.p,w.result.p,w.acc.p,0,LONG_MAX,true);
    return cudaGetLastError()==cudaSuccess && complete()==cudaSuccess?0:-1;
  } catch(...) {return -1;}
}
/* nr_csirs_disc_gpu_refine_entry: GPU refinement entry (C ABI): refines the extent of a verified hypothesis on the device (refine_kernel) and copies the refined hypothesis back.
 *   Returns the refinement result (0 ok, -1 not enough evidence or error). Caller: nr_csirs_disc.c (verification batch) through the ops table. Catches C++ exceptions.
 */
extern "C" int nr_csirs_disc_gpu_refine_entry(const nr_csirs_disc_ring_t *r,nr_csirs_disc_hyp_t *h,const nr_csirs_disc_win_t *win)
{
  try {
    std::lock_guard<std::mutex> guard(workspace_mutex);
    pick_stream();
    if(prepare(r->n_rb,r->n_ant,r->capacity)<0) return -1;
    auto &w=workspace;nr_csirs_disc_hyp_t full=*h;full.rb_start=0;full.n_rb=r->n_rb;
    nr_csirs_disc_sweep_map_t map={};
    if(nr_csirs_disc_sweep_map_into(&full,r->n_rb,&map,w.mapping.data(),w.mapping.size())) return -1;
    const short2 *grid=upload_grid(r);if(!grid) return -1;
    if(copy(w.dm.p,map.re,map.n_re*sizeof(*map.re),cudaMemcpyHostToDevice)!=cudaSuccess
       || copy(w.meta.p,r->meta,r->capacity*sizeof(*r->meta),cudaMemcpyHostToDevice)!=cudaSuccess) return -1;
    map.re=w.dm.p;auto ring=*r;ring.grid_base=(int16_t *)grid;ring.meta=w.meta.p;
    long ns=0;for(int i=0;i<r->capacity;++i) ns+=r->meta[i].valid && r->meta[i].abs_slot%h->period==h->offset;
    int result=-1;
    if(refine_launch(ring,*h,*win,map,workspace.stream,ns*r->n_ant*(h->row==1?1:map.ports))!=cudaSuccess || copy(&result,w.result.p,sizeof(int),cudaMemcpyDeviceToHost)!=cudaSuccess
       || copy(h,w.refined.p,sizeof(*h),cudaMemcpyDeviceToHost)!=cudaSuccess) return -1;
    return result;
  } catch(...) {return -1;}
}

/* nr_csirs_disc_gpu_confirm_entry: GPU confirmation entry (C ABI): scores one candidate on a slot (run() for its scrambling ID) and the energy statistics of its symbol, returning them in scores[0..4].
 *   Returns 0, or -1 on failure. Caller: discovery_gpu_score() in nr_csirs_disc_confirm_internal.h through the ops table. Catches C++ exceptions.
 */
extern "C" int nr_csirs_disc_gpu_confirm_entry(const nr_csirs_disc_ring_t *r,const nr_csirs_disc_hyp_t *h,int id,double scores[5])
{
  try {
    std::lock_guard<std::mutex> guard(workspace_mutex);
    pick_stream();
    nr_csirs_disc_sweep_cfg_t cfg={id,1,1,32,1};nr_csirs_disc_win_t win={};
    if(run(r,h,1,&cfg,&win)!=1) return -1;
    auto &w=workspace;nr_csirs_disc_sweep_map_t map={};
    if(nr_csirs_disc_sweep_map_into(h,r->n_rb,&map,w.mapping.data(),w.mapping.size())) return -1;
    const short2 *grid=r->device_valid?(const short2 *)r->device_grid:w.grid.p;
    /* The energy statistics were a <<<1,1>>> kernel of 1.9 ms on average and up to 8 ms, 300 times a second [MEASURED gsT2N, gsV2N]: a stall of the real-time graphs'
     * body launches (nr_gpu_chunk.h). They need only the one or two symbol rows of the first antenna: copy those and run the same function on the host. */
    {
      const int rows=h->row==5?2:1;const size_t rowlen=(size_t)12*r->n_rb;
      static std::vector<short2> stage;stage.assign(14*rowlen,short2{0,0});
      if(h->symb_l0<0 || h->symb_l0+rows>14 || cudaMemcpyAsync(stage.data()+h->symb_l0*rowlen,grid+h->symb_l0*rowlen,rows*rowlen*sizeof(short2),cudaMemcpyDeviceToHost,workspace.stream)!=cudaSuccess
         || complete()!=cudaSuccess) return -1;
      energies(stage.data(),r->n_rb,*h,map,scores);
    }
    scores[0]=win.z;return 0;
  } catch(...) {return -1;}
}
