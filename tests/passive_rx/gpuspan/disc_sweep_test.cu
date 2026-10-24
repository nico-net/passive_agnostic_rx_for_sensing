/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Unit test + timing: the staged-load CSI-RS discovery sweep kernel (nr_csirs_disc_gpu_sweep.cu, block_re == 32) gives bit-identical scores to the original kernel (sweep_ref in disc_sweep_ref.inc, verbatim from
 * main e9c874c308), for several maps (ports, REs per port with a partial last block), slot / antenna counts and a block_re != 32 fallback. Prints GPU time of both on the (idle) GPU for the largest case.
 * Build: nvcc -O2 -std=c++17 -arch=native -I<repo>/openair1/PHY/NR_UE_TRANSPORT -I<repo>/openair1/PHY -I<repo>/openair1 -I<repo>/common/utils -I<repo>
 *        -o disc_sweep_test disc_sweep_test.cu <repo>/openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_sweep.c -lcuda    Run: disc_sweep_test   (exit 0 = pass) */
#include "nr_csirs_disc_gpu_sweep.cu"
#include <vector>
#include <cstdio>
extern "C" void get_csi_mapping_parms() {}
extern "C" void get_cdm_group_size() {}
#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(2); } } while (0)
namespace {
#include "disc_sweep_ref.inc"
}
static int g_fail = 0;
struct Case { const char *name; int ports, nre_port, slots, ants, nc, block_re; };
static void run_case(const Case &c, bool timing)
{
  auto &w = workspace; cudaStream_t s = w.stream;
  const int RB = 217, spf = 20, nre = c.ports * c.nre_port;
  const int words = 28; /* m < 16 * words */
  if (prepare(RB, c.ants, 32) < 0) { fprintf(stderr, "prepare failed\n"); exit(2); }
  unsigned seed = 777 + c.ports; auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (int)(seed >> 8); };
  std::vector<nr_csirs_disc_re_t> re(nre);
  for (int i = 0; i < nre; ++i) re[i] = {i, (uint16_t)(rnd() % (16 * words)), (uint8_t)(8 + (rnd() % 2) + 2 * (rnd() % 2)), (int8_t)(rnd() % 2 ? 1 : -1)};
  std::vector<short2> x((size_t)c.slots * c.ants * nre);
  for (auto &v : x) v = {(short)(rnd() % 4001 - 2000), (short)(rnd() % 4001 - 2000)};
  short2 *dx; CK(cudaMalloc(&dx, x.size() * 4)); CK(cudaMemcpy(dx, x.data(), x.size() * 4, cudaMemcpyHostToDevice));
  nr_csirs_disc_re_t *dm; CK(cudaMalloc(&dm, nre * sizeof(re[0]))); CK(cudaMemcpy(dm, re.data(), nre * sizeof(re[0]), cudaMemcpyHostToDevice));
  std::vector<int> sl(c.slots); for (int i = 0; i < c.slots; ++i) sl[i] = i * 3 + 1;
  int *ds; CK(cudaMalloc(&ds, 4 * c.slots)); CK(cudaMemcpy(ds, sl.data(), 4 * c.slots, cudaMemcpyHostToDevice));
  nr_csirs_disc_sweep_map_t map = {}; map.ports = c.ports; map.n_re = nre; map.words = words; map.sym_mask = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11); map.re = dm;
  for (int p = 0; p <= c.ports; ++p) map.begin[p] = p * c.nre_port;
  nr_csirs_disc_sweep_cfg_t cfg = {0, c.nc, 1, c.block_re};
  float *za, *zb; CK(cudaMalloc(&za, 4 * (size_t)c.nc * c.slots)); CK(cudaMalloc(&zb, 4 * (size_t)c.nc * c.slots));
  CK(cudaMemset(za, 0, 4 * (size_t)c.nc * c.slots)); CK(cudaMemset(zb, 0, 4 * (size_t)c.nc * c.slots));
  cudaEvent_t e0, e1; CK(cudaEventCreate(&e0)); CK(cudaEventCreate(&e1)); float tr = 0, tn = 0;
  for (int rep = 0; rep < (timing ? 3 : 1); ++rep) {
    CK(cudaEventRecord(e0, s)); sweep_ref<<<dim3(c.nc, c.slots), 128, 0, s>>>(map, dx, ds, c.ants, spf, cfg, za, 0, c.nc); CK(cudaEventRecord(e1, s)); CK(cudaEventSynchronize(e1)); CK(cudaEventElapsedTime(&tr, e0, e1));
    CK(cudaEventRecord(e0, s)); sweep<<<dim3(c.nc, c.slots), 128, 0, s>>>(map, dx, ds, c.ants, spf, cfg, zb, 0, c.nc); CK(cudaEventRecord(e1, s)); CK(cudaEventSynchronize(e1)); CK(cudaEventElapsedTime(&tn, e0, e1));
  }
  CK(cudaGetLastError());
  std::vector<float> ha((size_t)c.nc * c.slots), hb(ha.size());
  CK(cudaMemcpy(ha.data(), za, ha.size() * 4, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(hb.data(), zb, hb.size() * 4, cudaMemcpyDeviceToHost));
  const bool same = !memcmp(ha.data(), hb.data(), ha.size() * 4);
  double mx = 0; for (float v : ha) mx = std::max<double>(mx, v);
  printf("%-34s ports=%-2d nre=%-6d slots=%d ants=%d ids=%-4d block_re=%-2d max_score=%.3f ref=%.3f ms new=%.3f ms : %s\n", c.name, c.ports, nre, c.slots, c.ants, c.nc, c.block_re, mx, tr, tn, same ? "BIT-EXACT" : "MISMATCH");
  if (!same) ++g_fail;
  cudaFree(dx); cudaFree(dm); cudaFree(ds); cudaFree(za); cudaFree(zb);
}
int main()
{
  CK(cudaSetDevice(0)); pick_stream();
  Case cs[] = {
    {"1 port 651 RE (row 1)", 1, 651, 8, 4, 64, 32},
    {"4 ports 217 RE", 4, 217, 4, 4, 64, 32},
    {"2 ports 31 RE (one partial block)", 2, 31, 3, 2, 33, 32},
    {"16 ports 868 RE (row 12)", 16, 868, 4, 4, 64, 32},
    {"32 ports 1736 RE (row 17 / 18)", 32, 1736, 4, 4, 64, 32},
    {"block_re=16 fallback", 4, 217, 4, 4, 64, 16},
    {"32 ports 1736 RE, 1024 IDs (timing)", 32, 1736, 4, 4, 1024, 32},
  };
  for (auto &c : cs) run_case(c, c.nc == 1024);
  printf("%s\n", g_fail ? "FAIL" : "ALL BIT-EXACT");
  return g_fail ? 1 : 0;
}
