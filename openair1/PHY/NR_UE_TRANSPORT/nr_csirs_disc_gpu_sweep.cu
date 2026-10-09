/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_sweep_internal.h"
#include "nr_csirs_disc_evidence_internal.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>

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
struct Workspace {
  cudaStream_t stream=nullptr;
  cudaEvent_t done=nullptr;
  Device<nr_csirs_disc_re_t> dm;
  Device<short2> dx,grid;
  Device<int> ds,di,result;
  Device<float> dz,dmean;
  Device<nr_csirs_disc_win_t> dw;
  Device<nr_csirs_disc_slot_meta_t> meta;
  Device<nr_csirs_disc_hyp_t> refined;
  Device<double> energy;
  std::vector<nr_csirs_disc_re_t> mapping;
  std::vector<int> slots,indices;
  std::vector<float> mean;
  nr_csirs_disc_evidence_t evidence={};
  int rb=0,ant=0,capacity=0;
} workspace;
static std::mutex workspace_mutex;
/* prepare: prepares the GPU sweep workspace for a carrier (rb RBs, ant antennas, capacity slots): creates the stream and completion event and grows every device buffer (mapping, packed samples, scores, metadata).
 *   Grow-only: returns at once when the existing buffers already fit. Returns 0, or -1 on failure. Callers: run(), nr_csirs_disc_gpu_sweep_prepare(), nr_csirs_disc_gpu_refine_entry().
 */
static int prepare(int rb,int ant,int capacity)
{
  auto &w=workspace;
  if(w.rb>=rb && w.ant>=ant && w.capacity>=capacity) return 0;
  if(!w.stream && cudaStreamCreateWithFlags(&w.stream,cudaStreamNonBlocking)!=cudaSuccess) return -1;
  if(!w.done && cudaEventCreateWithFlags(&w.done,cudaEventBlockingSync|cudaEventDisableTiming)!=cudaSuccess) return -1;
  size_t maps=(size_t)rb*16*8*2*4; // conservative mapping capacity for all rows
  // Occupied REs <= 32 ports * 8 RE/RB (row 18).
  size_t packed=(size_t)rb*32*8*ant*capacity;
  if(!w.dm.alloc(maps) || !w.dx.alloc(packed) || !w.grid.alloc((size_t)capacity*ant*14*12*rb)
     || !w.ds.alloc(capacity) || !w.di.alloc(capacity) || !w.dz.alloc((size_t)1024*160*capacity)
     || !w.dmean.alloc(1024*160) || !w.dw.alloc(1) || !w.meta.alloc(capacity)
     || !w.energy.alloc(5) || !w.refined.alloc(1) || !w.result.alloc(1)) return -1;
  w.mapping.resize(maps);w.slots.reserve(capacity);w.indices.reserve(capacity);w.mean.resize(1024*160);
  w.rb=rb;w.ant=ant;w.capacity=capacity;return 0;
}
/* complete: records the workspace stream's completion event and blocks the calling CPU thread until the queued work finished; returns the CUDA status.
 *   Callers: copy(), run() and the entry points.
 */
static cudaError_t complete() {
  cudaError_t rc=cudaEventRecord(workspace.done,workspace.stream);
  return rc==cudaSuccess?cudaEventSynchronize(workspace.done):rc;
}
/* copy: asynchronous memcpy on the workspace stream followed by complete(); returns the CUDA status. */
static cudaError_t copy(void *dst,const void *src,size_t n,cudaMemcpyKind kind) {
  cudaError_t rc=cudaMemcpyAsync(dst,src,n,kind,workspace.stream);
  return rc==cudaSuccess?complete():rc;
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
 *   Returns nh on success, 0 when ISAC_CSIRS_DISC is not "1", -1 on error. Serialised by the caller's workspace mutex. Caller: nr_csirs_disc_gpu_sweep_entry().
 */
int run(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_hyp_t *hyps, int nh,
        const nr_csirs_disc_sweep_cfg_t *cfg, nr_csirs_disc_win_t *out)
{
  const char *enable = getenv("ISAC_CSIRS_DISC");
  if (!enable || strcmp(enable, "1"))
    return 0;
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
    if(cudaMemcpyAsync(dm.p,map.re,map.n_re*sizeof(*map.re),cudaMemcpyHostToDevice,stream)!=cudaSuccess
       || cudaMemcpyAsync(ds.p,slots.data(),slots.size()*sizeof(int),cudaMemcpyHostToDevice,stream)!=cudaSuccess
       || cudaMemcpyAsync(w.di.p,indices.data(),indices.size()*sizeof(int),cudaMemcpyHostToDevice,stream)!=cudaSuccess) return -1;
    size_t packed=(size_t)slots.size()*r->n_ant*map.n_re;
    gather<<<(packed+255)/256,256,0,stream>>>(grid,r->slot_stride/2,dm.p,map.n_re,w.di.p,slots.size(),r->n_ant,r->n_rb,dx.p);
    auto device_map = map;
    device_map.re = dm.p;
    sweep<<<dim3(nc, slots.size()), 128, 0, stream>>>(device_map, dx.p, ds.p, r->n_ant, r->slots_per_frame, c, dz.p);
    if (cudaGetLastError() != cudaSuccess) return -1;
    int previous=nr_csirs_disc_evidence_previous(entry,slots.size());
    if (entry) average_scores<true><<<(nc + 255) / 256, 256, 0, stream>>>(dz.p, nc, slots.size(), dmean.p, history, previous);
    else average_scores<false><<<(nc + 255) / 256, 256, 0, stream>>>(dz.p, nc, slots.size(), dmean.p);
    if (cudaGetLastError() != cudaSuccess) return -1;
    reduce_scores<<<1, 256, 0, stream>>>(dmean.p, c, h, slots.size()+previous, dw.p);
    if (cudaGetLastError() != cudaSuccess
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
                         const nr_csirs_disc_win_t *win, nr_csirs_disc_sweep_map_t map)
{
  double evidence[NR_CSIRS_DISC_MAX_RB]={0};
  uint32_t gold[14][64];
  for(int s=0;s<r->capacity;++s) if(r->meta[s].valid && r->meta[s].abs_slot%h->period==h->offset) {
    for(int l=0;l<14;++l) if(map.sym_mask&(1u<<l)) {
      uint64_t t=14*((r->meta[s].slot+win->slot_delta)%r->slots_per_frame)+l+1;
      uint32_t init=((t*(2*win->scramb_id+1)<<10)+win->scramb_id)&0x7fffffff;
      Gold generator;generator.reset(init);
      for(int w=0;w<map.words;++w) gold[l][w]=generator.next();
    }
    for(int a=0;a<r->n_ant;++a) for(int p=0;p<map.ports;++p) {
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
                           const nr_csirs_disc_win_t *win, nr_csirs_disc_sweep_map_t map)
{
  if (h->row != 1) return refine_extent(r,h,win,map);
  double signal[NR_CSIRS_DISC_MAX_RB]={0}, weight[NR_CSIRS_DISC_MAX_RB]={0};
  uint32_t gold[64];
  int slots=0;
  for(int s=0;s<r->capacity;++s) if(r->meta[s].valid && r->meta[s].abs_slot%h->period==h->offset) {
    ++slots;
    uint64_t t=14*((r->meta[s].slot+win->slot_delta)%r->slots_per_frame)+h->symb_l0+1;
    uint32_t init=((t*(2*win->scramb_id+1)<<10)+win->scramb_id)&0x7fffffff;
    Gold generator;generator.reset(init);
    for(int w=0;w<map.words;++w) gold[w]=generator.next();
    for(int a=0;a<r->n_ant;++a) for(int rb=0;rb<r->n_rb;++rb) {
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

/* refine_kernel: GPU kernel (one thread): runs the device refinement (nr_csirs_disc_refine_cpu) on the ring and stores the refined hypothesis and the result code. */
__global__ void refine_kernel(nr_csirs_disc_ring_t ring,nr_csirs_disc_hyp_t h,nr_csirs_disc_win_t win,
                             nr_csirs_disc_sweep_map_t map,nr_csirs_disc_hyp_t *out,int *result)
{
  *result=nr_csirs_disc_refine_cpu(&ring,&h,&win,map);*out=h;
}

/* energy_score: device energy statistic of one symbol: returns the energy ratio of the occupied REs against the weakest or the mean of the unoccupied subcarrier classes (EPR when epr is set).
 *   Also reports the old-style score through *old. Returns -1 when the statistic is not defined. Called from energies().
 */
__device__ double energy_score(const short2 *rx,int rb_count,int symbol,nr_csirs_disc_sweep_map_t map,
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
/* energies: GPU kernel (one thread): computes the energy statistics of a hypothesis's symbol (sequence-free EPR, the energy score and the optional row-5 second symbol) into scores[1..4].
 *   Called by nr_csirs_disc_gpu_confirm_entry() and by the prepare path.
 */
__global__ void energies(const short2 *grid,int rb,nr_csirs_disc_hyp_t h,nr_csirs_disc_sweep_map_t map,double *scores)
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
  try {
    std::lock_guard<std::mutex> guard(workspace_mutex);
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
  try {
    std::lock_guard<std::mutex> guard(workspace_mutex);
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
    const char *cum=getenv("ISAC_CSIRS_DISC_CUMULATIVE");
    if(cum && !strcmp(cum,"1"))
      average_scores<true><<<1,256,0,w.stream>>>(w.dz.p,1,1,w.dmean.p,w.dz.p+w.dz.capacity-1024,0);
    reduce_scores<<<1,256,0,w.stream>>>(w.dmean.p,cfg,0,1,w.dw.p);
    nr_csirs_disc_ring_t ring={};ring.n_ant=ant;ring.n_rb=rb;ring.capacity=1;ring.slots_per_frame=20;
    ring.slot_stride=(size_t)ant*14*24*rb;ring.grid_base=(int16_t *)w.grid.p;ring.meta=w.meta.p;
    nr_csirs_disc_hyp_t h={};h.row=1;h.period=4;nr_csirs_disc_win_t win={};
    refine_kernel<<<1,1,0,w.stream>>>(ring,h,win,map,w.refined.p,w.result.p);
    energies<<<1,1,0,w.stream>>>(w.grid.p,rb,h,map,w.energy.p);
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
    if(prepare(r->n_rb,r->n_ant,r->capacity)<0) return -1;
    auto &w=workspace;nr_csirs_disc_hyp_t full=*h;full.rb_start=0;full.n_rb=r->n_rb;
    nr_csirs_disc_sweep_map_t map={};
    if(nr_csirs_disc_sweep_map_into(&full,r->n_rb,&map,w.mapping.data(),w.mapping.size())) return -1;
    const short2 *grid=upload_grid(r);if(!grid) return -1;
    if(copy(w.dm.p,map.re,map.n_re*sizeof(*map.re),cudaMemcpyHostToDevice)!=cudaSuccess
       || copy(w.meta.p,r->meta,r->capacity*sizeof(*r->meta),cudaMemcpyHostToDevice)!=cudaSuccess) return -1;
    map.re=w.dm.p;auto ring=*r;ring.grid_base=(int16_t *)grid;ring.meta=w.meta.p;
    refine_kernel<<<1,1,0,workspace.stream>>>(ring,*h,*win,map,w.refined.p,w.result.p);
    int result=-1;
    if(cudaGetLastError()!=cudaSuccess || copy(&result,w.result.p,sizeof(int),cudaMemcpyDeviceToHost)!=cudaSuccess
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
    nr_csirs_disc_sweep_cfg_t cfg={id,1,1,32,1};nr_csirs_disc_win_t win={};
    if(run(r,h,1,&cfg,&win)!=1) return -1;
    auto &w=workspace;nr_csirs_disc_sweep_map_t map={};
    if(nr_csirs_disc_sweep_map_into(h,r->n_rb,&map,w.mapping.data(),w.mapping.size())) return -1;
    map.re=w.dm.p;
    const short2 *grid=r->device_valid?(const short2 *)r->device_grid:w.grid.p;
    energies<<<1,1,0,workspace.stream>>>(grid,r->n_rb,*h,map,w.energy.p);
    if(cudaGetLastError()!=cudaSuccess || copy(scores,w.energy.p,5*sizeof(double),cudaMemcpyDeviceToHost)!=cudaSuccess) return -1;
    scores[0]=win.z;return 0;
  } catch(...) {return -1;}
}
