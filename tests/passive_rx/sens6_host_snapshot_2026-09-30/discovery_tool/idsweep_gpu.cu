// Stage-1 blind nID sweep on the GPU: one thread per (ID, snapshot). Mirrors idsw_cinit / gold_generic /
// idsw_pilot / idsw_best in idsweep_offline.c exactly; the host checks equality against the CPU path.
#include <cstdint>
#include <cstdio>
#include <cuda_runtime.h>

#define NID 65536

__device__ __forceinline__ uint32_t cinit_dev(int nid, int slot, int l, int sps)
{
  uint64_t x = ((uint64_t)sps * slot + l + 1) * ((uint64_t)(nid << 1) + 1);
  x <<= 17;
  x += (uint64_t)(nid << 1);
  return (uint32_t)(x % (1U << 31));
}

// Word-wise Gold generator, identical recurrence to openair1/PHY/gold.h gold_generic().
struct gold_t {
  uint32_t x1, x2;
  __device__ void reset(uint32_t c)
  {
    x1 = 1 + (1U << 31);
    x2 = c ^ ((c ^ (c >> 1) ^ (c >> 2) ^ (c >> 3)) << 31);
    for (int n = 1; n < 50; n++) step();
  }
  __device__ void step()
  {
    x1 = (x1 >> 1) ^ (x1 >> 4);
    x1 = x1 ^ (x1 << 31) ^ (x1 << 28);
    x2 = (x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3) ^ (x2 >> 4);
    x2 = x2 ^ (x2 << 31) ^ (x2 << 30) ^ (x2 << 29) ^ (x2 << 28);
  }
  __device__ uint32_t next() { step(); return x1 ^ x2; }
};

#define MAXW 512 /* gold words per thread: covers CRB index up to ~2700 */

__device__ __forceinline__ void pilot_dev(const uint32_t *g, int m, float &xr, float &xi)
{
  const uint8_t b = ((const uint8_t *)g)[m / 4];
  const int v = (b >> ((m * 2) & 7)) & 3;
  const int idx = (v == 1) ? 2 : (v == 2) ? 1 : v; /* swap2bits {0,2,1,3} */
  xr = (idx == 0 || idx == 1) ? 1.f : -1.f;
  xi = (idx == 0 || idx == 2) ? -1.f : 1.f;       /* tab {1,-1},{1,1},{-1,-1},{-1,1} */
}

// best coherent 6-CRB |corr| over start CRBs in [lo,hi), skipping windows overlapping [ex_lo,ex_hi)
__device__ float best_dev(const float *yr, const float *yi, const float *py, int lo, int hi, int ex_lo, int ex_hi,
                          const uint32_t *g, int m0)
{
  if (hi - lo < 6) return 0.f;
  float wr[6], wi[6], wp[6], sr = 0, si = 0, sp = 0, best = 0;
  for (int c = lo; c < hi; c++) {
    float ar = 0, ai = 0;
    for (int q = 0; q < 3; q++) {
      float xr, xi;
      pilot_dev(g, 3 * (c - m0) + q, xr, xi);
      const float r = yr[3 * c + q], i = yi[3 * c + q];
      ar += r * xr - i * xi;
      ai += r * xi + i * xr;
    }
    const int s = (c - lo) % 6;
    if (c - lo >= 6) { sr -= wr[s]; si -= wi[s]; sp -= wp[s]; }
    wr[s] = ar; wi[s] = ai; wp[s] = py[c];
    sr += ar; si += ai; sp += py[c];
    if (c - lo >= 5) {
      const int c0 = c - 5;
      if (!(ex_hi > ex_lo && c0 < ex_hi && c0 + 6 > ex_lo)) {
        const float v = sp > 0.f ? sqrtf((sr * sr + si * si) / (sp * 36.f)) : 0.f;
        best = fmaxf(best, v);
      }
    }
  }
  return best;
}

__global__ void k_sweep(int nsnap, int ncrb, int c_lo, int c_hi, int cs_lo, int cs_hi, int have_css0, int sps,
                        int words, const int *slot, const int *sym, const float *yr, const float *yi, const float *py,
                        float *v_rest, float *v_css0)
{
  const long t = (long)blockIdx.x * blockDim.x + threadIdx.x;
  if (t >= (long)nsnap * NID) return;
  const int k = (int)(t / NID), id = (int)(t % NID);
  uint32_t g[MAXW];
  gold_t G;
  G.reset(cinit_dev(id, slot[k], sym[k], sps));
  /* first word = output after the 1600-bit skip, same as gold_generic(.., reset=1) */
  G.step();
  g[0] = G.x1 ^ G.x2;
  for (int w = 1; w < words; w++) g[w] = G.next();
  const float *Yr = yr + (size_t)k * ncrb * 3, *Yi = yi + (size_t)k * ncrb * 3, *P = py + (size_t)k * ncrb;
  v_rest[t] = best_dev(Yr, Yi, P, c_lo, c_hi, have_css0 ? cs_lo : 0, have_css0 ? cs_hi : 0, g, 0);
  v_css0[t] = have_css0 ? best_dev(Yr, Yi, P, cs_lo, cs_hi, 0, 0, g, cs_lo) : 0.f;
}

extern "C" int idsw_gpu_batch(int nsnap, int ncrb, int c_lo, int c_hi, int cs_lo, int cs_hi, int have_css0, int sps,
                              const int *slot, const int *sym, const float *yr, const float *yi, const float *py,
                              float *v_rest, float *v_css0)
{
  const int words = (2 * 3 * ncrb) / 32 + 2;
  if (words > MAXW) { fprintf(stderr, "idsw_gpu: %d gold words > %d\n", words, MAXW); return -1; }
  int *d_slot, *d_sym;
  float *d_yr, *d_yi, *d_py, *d_vr, *d_vc;
  const size_t ny = (size_t)nsnap * ncrb * 3, np = (size_t)nsnap * ncrb, nv = (size_t)nsnap * NID;
  if (cudaMalloc(&d_slot, sizeof(int) * nsnap) || cudaMalloc(&d_sym, sizeof(int) * nsnap)
      || cudaMalloc(&d_yr, sizeof(float) * ny) || cudaMalloc(&d_yi, sizeof(float) * ny)
      || cudaMalloc(&d_py, sizeof(float) * np) || cudaMalloc(&d_vr, sizeof(float) * nv)
      || cudaMalloc(&d_vc, sizeof(float) * nv)) {
    fprintf(stderr, "idsw_gpu: cudaMalloc failed\n");
    return -1;
  }
  cudaMemcpy(d_slot, slot, sizeof(int) * nsnap, cudaMemcpyHostToDevice);
  cudaMemcpy(d_sym, sym, sizeof(int) * nsnap, cudaMemcpyHostToDevice);
  cudaMemcpy(d_yr, yr, sizeof(float) * ny, cudaMemcpyHostToDevice);
  cudaMemcpy(d_yi, yi, sizeof(float) * ny, cudaMemcpyHostToDevice);
  cudaMemcpy(d_py, py, sizeof(float) * np, cudaMemcpyHostToDevice);
  const int bs = 128;
  const long nthr = (long)nsnap * NID;
  k_sweep<<<(unsigned)((nthr + bs - 1) / bs), bs>>>(nsnap, ncrb, c_lo, c_hi, cs_lo, cs_hi, have_css0, sps, words,
                                                     d_slot, d_sym, d_yr, d_yi, d_py, d_vr, d_vc);
  cudaError_t e = cudaDeviceSynchronize();
  if (e != cudaSuccess) { fprintf(stderr, "idsw_gpu: %s\n", cudaGetErrorString(e)); return -1; }
  cudaMemcpy(v_rest, d_vr, sizeof(float) * nv, cudaMemcpyDeviceToHost);
  cudaMemcpy(v_css0, d_vc, sizeof(float) * nv, cudaMemcpyDeviceToHost);
  cudaFree(d_slot); cudaFree(d_sym); cudaFree(d_yr); cudaFree(d_yi); cudaFree(d_py); cudaFree(d_vr); cudaFree(d_vc);
  return 0;
}
