// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// PDCCH-REDESIGN-20261019 micro-benchmark 3: GPU fast-SSC DCI polar decode on the DGX GB10, standalone (nvcc, no cmake).
// Inputs: code_<len>_<AL>.bin written by `polar_bench dump <dir>` (code tables, 256 LLR vectors, expected u from the CPU
// prototype). Output: bit-exactness vs the CPU prototype, then
//   (A) kernel time of a batch (one warp per candidate), launched per batch      -> "launch+exec" and event time
//   (B) persistent kernel: warps spin on a host-mapped sequence flag, read LLRs from mapped host memory, write u and a
//       done counter back; host measures round trip (write LLR + flag -> all done). No launch cost, no stream sync.
// Usage: ./gpu_bench <dir>
#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <vector>
#include <algorithm>

#define CK(x) do { cudaError_t e = (x); if (e != cudaSuccess) { printf("CUDA error %s at %d: %s\n", #x, __LINE__, cudaGetErrorString(e)); exit(1); } } while (0)

struct GNode { uint8_t type, lvl; int16_t l, r; };
enum { T_R0, T_R1, T_REP, T_SPC, T_SPLIT };
struct Tab { int N, n, K, E, mode, root; const uint16_t *rmp; const GNode *nodes; };

__device__ __forceinline__ int qabs(int a) { return a < 0 ? (a == -32768 ? 32767 : -a) : a; }
__device__ __forceinline__ int16_t sat16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

struct Scratch { int16_t *alpha[10]; uint8_t *x; };

__device__ void run(const Tab &t, const GNode *nd, const int16_t *alpha, uint8_t *out, Scratch &s, int lane)
{
  const int sz = 1 << nd->lvl;
  switch (nd->type) {
    case T_R0: for (int i = lane; i < sz; i += 32) out[i] = 0; __syncwarp(); return;
    case T_R1: for (int i = lane; i < sz; i += 32) out[i] = alpha[i] <= 0; __syncwarp(); return;
    case T_REP: {
      int sum = 0;
      for (int i = lane; i < sz; i += 32) sum += alpha[i];
      for (int o = 16; o; o >>= 1) sum += __shfl_xor_sync(0xffffffff, sum, o);
      for (int i = lane; i < sz; i += 32) out[i] = sum <= 0;
      __syncwarp(); return;
    }
    case T_SPC: {
      int par = 0, best = 0x7fffffff;
      for (int i = lane; i < sz; i += 32) {
        const int a = alpha[i]; const int b = a <= 0; out[i] = b; par ^= b;
        const int key = (qabs(a) << 10) | i; // first minimum wins ties like the CPU loop
        best = key < best ? key : best;
      }
      for (int o = 16; o; o >>= 1) { par ^= __shfl_xor_sync(0xffffffff, par, o); const int ob = __shfl_xor_sync(0xffffffff, best, o); best = ob < best ? ob : best; }
      __syncwarp();
      if (lane == 0) out[best & 1023] ^= par;
      __syncwarp(); return;
    }
    default: break;
  }
  const int h = sz >> 1;
  int16_t *nb = s.alpha[nd->lvl - 1];
  for (int i = lane; i < h; i += 32) {
    const int a = alpha[i], b = alpha[h + i];
    const int m = min(qabs(a), qabs(b)); const int sg = (a ^ b) >> 31;
    nb[i] = (int16_t)((m ^ sg) - sg);
  }
  __syncwarp();
  run(t, &t.nodes[nd->l], nb, out, s, lane);
  for (int i = lane; i < h; i += 32) {
    const int a = alpha[i], b = alpha[h + i];
    const int na = a == -32768 ? 32767 : -a;
    nb[i] = sat16(b + (out[i] ? na : a));
  }
  __syncwarp();
  run(t, &t.nodes[nd->r], nb, out + h, s, lane);
  for (int i = lane; i < h; i += 32) out[i] ^= out[h + i];
  __syncwarp();
}

// one warp decodes one candidate: llr (E int16) -> u bytes (N)
__device__ void decode_one(const Tab &t, const int16_t *llr, uint8_t *ux, int16_t *smem16, uint8_t *smem8, int lane)
{
  Scratch s; int16_t *p = smem16 + 512; // [0,512) = dt
  for (int l = 0; l < 10; l++) { s.alpha[l] = p; p += (1 << l); }
  int16_t *dt = smem16; uint8_t *x = smem8;
  const int N = t.N, E = t.E;
  if (t.mode == 0) {
    for (int i = lane; i < N; i += 32) dt[i] = llr[0] * 0; // placeholder zero
    for (int i = lane; i < N; i += 32) dt[i] = 0;
    __syncwarp();
    for (int i = lane; i < N && i < E; i += 32) dt[t.rmp[i]] = llr[i];
    __syncwarp();
    for (int i = N + lane; i < E; i += 32) dt[t.rmp[i]] = (int16_t)(dt[t.rmp[i]] + llr[i]);
  } else {
    const int16_t fill = t.mode == 1 ? 0 : 32767;
    for (int i = lane; i < N; i += 32) dt[i] = fill;
    __syncwarp();
    for (int i = lane; i < E; i += 32) dt[t.rmp[i]] = llr[i];
  }
  __syncwarp();
  run(t, &t.nodes[t.root], dt, x, s, lane);
  for (int len = 1; len < N; len <<= 1) {
    for (int i = lane; i < N / 2; i += 32) { // pair index -> (block, offset)
      const int blk = i / len, off = i % len; const int base = blk * 2 * len + off;
      x[base] ^= x[base + len];
    }
    __syncwarp();
  }
  for (int i = lane; i < N; i += 32) ux[i] = x[i];
}

__global__ void batch_k(Tab t, const int16_t *llr, uint8_t *ux, int ncand)
{
  __shared__ int16_t sm16[512 + 1024 + 8];
  __shared__ uint8_t sm8[512 + 32];
  const int c = blockIdx.x; if (c >= ncand) return;
  decode_one(t, llr + (size_t)c * t.E, ux + (size_t)c * t.N, sm16, sm8, threadIdx.x);
}

// persistent: block b serves candidate b of every request; host bumps *req (mapped), blocks bump *done after writing u.
__global__ void persist_k(Tab t, volatile const int16_t *llr_h, volatile uint8_t *ux_h, volatile int *req, volatile int *done_cnt, volatile int *stop, int ncand)
{
  __shared__ int16_t sm16[512 + 1024 + 8];
  __shared__ uint8_t sm8[512 + 32];
  __shared__ int16_t lbuf[1728];
  __shared__ uint8_t ubuf[512];
  const int c = blockIdx.x, lane = threadIdx.x;
  int seen = 0;
  for (;;) {
    int r = 0, sp = 0;
    if (lane == 0) { do { r = *req; sp = *stop; } while (r == seen && !sp); }
    r = __shfl_sync(0xffffffff, r, 0); sp = __shfl_sync(0xffffffff, sp, 0);
    if (r == seen && sp) return;
    seen = r;
    if (c < ncand) {
      __threadfence_system();
      for (int i = lane; i < t.E; i += 32) lbuf[i] = llr_h[(size_t)c * t.E + i];
      __syncwarp();
      decode_one(t, lbuf, ubuf, sm16, sm8, lane);
      for (int i = lane; i < t.N; i += 32) ux_h[(size_t)c * t.N + i] = ubuf[i];
      __syncwarp(); __threadfence_system();
      if (lane == 0) atomicAdd((int *)done_cnt, 1);
    }
  }
}

static double now_us() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }

struct Host { int len, al, N, n, K, E, mode, root, nn, NV; std::vector<uint16_t> rmp; std::vector<GNode> nodes; std::vector<int16_t> llr; std::vector<uint8_t> u; };
static bool load(const char *dir, int len, int al, Host &h)
{
  char fn[256]; snprintf(fn, sizeof fn, "%s/code_%d_%d.bin", dir, len, al);
  FILE *f = fopen(fn, "rb"); if (!f) return false;
  int32_t hd[8]; if (fread(hd, 4, 8, f) != 8) return false;
  h.len = hd[0]; h.al = hd[1]; h.N = hd[2]; h.n = hd[3]; h.K = hd[4]; h.E = hd[5]; h.mode = hd[6]; h.nn = hd[7];
  int32_t rt; if (fread(&rt, 4, 1, f) != 1) return false; h.root = rt;
  h.rmp.resize(h.E); if (fread(h.rmp.data(), 2, h.E, f) != (size_t)h.E) return false;
  std::vector<uint8_t> info(h.N); if (fread(info.data(), 1, h.N, f) != (size_t)h.N) return false;
  int32_t ni; if (fread(&ni, 4, 1, f) != 1) return false; fseek(f, 4L * ni, SEEK_CUR);
  h.nodes.resize(h.nn); if (fread(h.nodes.data(), sizeof(GNode), h.nn, f) != (size_t)h.nn) return false;
  if (fread(&h.NV, 4, 1, f) != 1) return false;
  h.llr.resize((size_t)h.NV * h.E); h.u.resize((size_t)h.NV * h.N);
  for (int v = 0; v < h.NV; v++) {
    if (fread(&h.llr[(size_t)v * h.E], 2, h.E, f) != (size_t)h.E) return false;
    if (fread(&h.u[(size_t)v * h.N], 1, h.N, f) != (size_t)h.N) return false;
  }
  fclose(f); return true;
}

int main(int argc, char **argv)
{
  const char *dir = argc > 1 ? argv[1] : ".";
  CK(cudaSetDevice(0));
  cudaDeviceProp pr; CK(cudaGetDeviceProperties(&pr, 0));
  printf("# gpu_bench on %s sm_%d%d, %d SMs\n", pr.name, pr.major, pr.minor, pr.multiProcessorCount);
  const int cfgs[][2] = {{47, 1}, {47, 2}, {47, 4}, {47, 8}};
  for (auto &cf : cfgs) {
    Host h; if (!load(dir, cf[0], cf[1], h)) { printf("missing code file for %d/%d\n", cf[0], cf[1]); continue; }
    uint16_t *drmp; GNode *dn; CK(cudaMalloc(&drmp, h.E * 2)); CK(cudaMalloc(&dn, h.nn * sizeof(GNode)));
    CK(cudaMemcpy(drmp, h.rmp.data(), h.E * 2, cudaMemcpyHostToDevice)); CK(cudaMemcpy(dn, h.nodes.data(), h.nn * sizeof(GNode), cudaMemcpyHostToDevice));
    Tab t{h.N, h.n, h.K, h.E, h.mode, h.root, drmp, dn};
    const int MAXC = 128;
    int16_t *dllr; uint8_t *dux; CK(cudaMalloc(&dllr, (size_t)MAXC * h.E * 2)); CK(cudaMalloc(&dux, (size_t)MAXC * h.N));
    std::vector<int16_t> hl((size_t)MAXC * h.E); for (int c = 0; c < MAXC; c++) memcpy(&hl[(size_t)c * h.E], &h.llr[(size_t)(c % h.NV) * h.E], h.E * 2);
    CK(cudaMemcpy(dllr, hl.data(), hl.size() * 2, cudaMemcpyHostToDevice));
    batch_k<<<MAXC, 32>>>(t, dllr, dux, MAXC); CK(cudaDeviceSynchronize());
    std::vector<uint8_t> hu((size_t)MAXC * h.N); CK(cudaMemcpy(hu.data(), dux, hu.size(), cudaMemcpyDeviceToHost));
    int bad = 0; for (int c = 0; c < MAXC; c++) bad += memcmp(&hu[(size_t)c * h.N], &h.u[(size_t)(c % h.NV) * h.N], h.N) != 0;
    printf("\nlen %d AL %d (N=%d E=%d nodes=%d): GPU u vs CPU prototype u: %d/%d candidates differ\n", cf[0], cf[1], h.N, h.E, h.nn, bad, MAXC);
    // (A) batch kernel, event time and wall (launch + sync) time
    cudaEvent_t e0, e1; cudaEventCreate(&e0); cudaEventCreate(&e1);
    for (int nc : {1, 8, 32, 128}) {
      std::vector<double> ev, wall;
      for (int r = 0; r < 300; r++) {
        const double w0 = now_us();
        cudaEventRecord(e0); batch_k<<<nc, 32>>>(t, dllr, dux, nc); cudaEventRecord(e1); cudaEventSynchronize(e1);
        wall.push_back(now_us() - w0); float ms; cudaEventElapsedTime(&ms, e0, e1); ev.push_back(ms * 1000.0);
      }
      std::sort(ev.begin() + 20, ev.end()); std::sort(wall.begin() + 20, wall.end());
      printf("  (A) batch of %3d: kernel (event) p50 %6.1f us | launch+exec+sync wall p50 %6.1f p99 %6.1f us\n", nc, ev[20 + (ev.size() - 20) / 2], wall[20 + (wall.size() - 20) / 2], wall[(size_t)(wall.size() * .99)]);
    }
    // (B) persistent kernel with host-mapped flags
    int ncand = 8;
    int16_t *hllr; uint8_t *hux; int *hflags; // hflags: req, done, stop
    CK(cudaHostAlloc(&hllr, (size_t)MAXC * h.E * 2, cudaHostAllocMapped)); CK(cudaHostAlloc(&hux, (size_t)MAXC * h.N, cudaHostAllocMapped));
    CK(cudaHostAlloc(&hflags, 64 * 4, cudaHostAllocMapped));
    int16_t *dhllr; uint8_t *dhux; int *dflags;
    CK(cudaHostGetDevicePointer(&dhllr, hllr, 0)); CK(cudaHostGetDevicePointer(&dhux, hux, 0)); CK(cudaHostGetDevicePointer(&dflags, hflags, 0));
    for (int nc : {1, 8, 32}) {
      ncand = nc; hflags[0] = hflags[16] = hflags[32] = 0;
      cudaStream_t st; cudaStreamCreate(&st);
      persist_k<<<nc, 32, 0, st>>>(t, dhllr, dhux, dflags, dflags + 16, dflags + 32, nc);
      std::vector<double> rt;
      int req = 0; bool okall = true;
      for (int r = 0; r < 3000; r++) {
        for (int c = 0; c < nc; c++) memcpy(&hllr[(size_t)c * h.E], &h.llr[(size_t)((r + c) % h.NV) * h.E], h.E * 2);
        __atomic_store_n(&hflags[16], 0, __ATOMIC_RELEASE);
        const double w0 = now_us();
        __atomic_store_n(&hflags[0], ++req, __ATOMIC_RELEASE);
        while (__atomic_load_n(&hflags[16], __ATOMIC_ACQUIRE) < nc) { }
        rt.push_back(now_us() - w0);
        if (r % 500 == 0) for (int c = 0; c < nc; c++) okall &= memcmp(&hux[(size_t)c * h.N], &h.u[(size_t)((r + c) % h.NV) * h.N], h.N) == 0;
        const double sl = now_us() + 500; while (now_us() < sl) { } // 500 us cadence like slots
      }
      hflags[32] = 1; __atomic_store_n(&hflags[0], ++req, __ATOMIC_RELEASE); CK(cudaStreamSynchronize(st));
      std::sort(rt.begin() + 50, rt.end());
      printf("  (B) persistent, %2d cand: round trip (LLR write -> all u back) p50 %6.1f p90 %6.1f p99 %6.1f us, u exact: %s\n", nc,
             rt[50 + (rt.size() - 50) / 2], rt[(size_t)(rt.size() * .9)], rt[(size_t)(rt.size() * .99)], okall ? "yes" : "NO");
      cudaStreamDestroy(st);
    }
    cudaFreeHost(hllr); cudaFreeHost(hux); cudaFreeHost(hflags); cudaFree(dllr); cudaFree(dux); cudaFree(drmp); cudaFree(dn);
  }
  return 0;
}
