/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Unit test: the parallel CSI-RS discovery refinement (refine_chunk: one CTA per unit + in-order accumulation) is bit-identical to the serial kernel (refine_kernel<<<1,1>>>, whole job in one
 * call), for row 4 (4 ports, densities 0/1/2) and the row-1 TRS branch, for several chunkings, two RB widths and a ring in device and in mapped host memory. Compares the accumulators (memcmp),
 * the result code and the refined hypothesis. Exit 0 = pass.
 * Build: nvcc -O2 -std=c++17 -arch=native -I<repo>/openair1/PHY/NR_UE_TRANSPORT -I<repo>/openair1/PHY -I<repo>/openair1 -I<repo>/common/utils -I<repo>
 *        -o disc_refine_test disc_refine_test.cu <repo>/openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_sweep.c   (also needs the green-context helper like disc_unit_bench.cu)  Run: disc_refine_test */
#include "nr_csirs_disc_gpu_sweep.cu"
#include <vector>
#include <chrono>
#include <cstdio>
extern "C" void get_csi_mapping_parms() {}
extern "C" void get_cdm_group_size() {}
#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(2); } } while (0)

struct Case { const char *name; int row, density, rb, ports, res_per_port_rb, slots_valid, ant; bool host_ring; };
static int g_fail = 0;
static void run_case(const Case &c, const std::vector<long> &per_list)
{
  auto &w = workspace; cudaStream_t s = w.stream;
  const int NS = 32, spf = 20, RB = c.rb, ANT = c.ant;
  if (prepare(RB, ANT, NS) < 0) { fprintf(stderr, "prepare failed\n"); exit(2); }
  nr_csirs_disc_ring_t ring = {}; ring.n_ant = ANT; ring.n_rb = RB; ring.slots_per_frame = spf; ring.capacity = NS; ring.slot_stride = (size_t)ANT * 14 * 24 * RB;
  const size_t nel = ring.slot_stride * NS;
  std::vector<int16_t> host(nel); unsigned seed = 12345 + c.row * 7 + c.density;
  auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (int)(seed >> 16); };
  for (size_t i = 0; i < nel; ++i) host[i] = (int16_t)(rnd() % 4001 - 2000) * ((i / (2 * 12 * RB)) % 3 == 0 ? 8 : 1); /* some rows louder: different evidence per RB band */
  if (c.host_ring) { CK(cudaHostAlloc((void **)&ring.grid_base, nel * 2, cudaHostAllocMapped | cudaHostAllocPortable)); memcpy(ring.grid_base, host.data(), nel * 2); }
  else { CK(cudaMalloc((void **)&ring.grid_base, nel * 2)); CK(cudaMemcpy(ring.grid_base, host.data(), nel * 2, cudaMemcpyHostToDevice)); }
  std::vector<nr_csirs_disc_slot_meta_t> meta(NS);
  for (int i = 0; i < NS; ++i) meta[i] = {(uint32_t)(i * 2 + (i % 5 == 3 ? 1 : 0)), 0, (uint8_t)(i % spf), (uint8_t)(i < c.slots_valid ? 1 : 0)}; /* some invalid, some off the period */
  nr_csirs_disc_slot_meta_t *dmeta; CK(cudaMalloc(&dmeta, NS * sizeof(meta[0]))); CK(cudaMemcpy(dmeta, meta.data(), NS * sizeof(meta[0]), cudaMemcpyHostToDevice));
  ring.meta = dmeta;
  nr_csirs_disc_hyp_t h = {}; h.row = c.row; h.density = c.density; h.period = 2; h.offset = 0; h.rb_start = RB / 8; h.n_rb = RB - RB / 4; h.symb_l0 = 1;
  nr_csirs_disc_win_t win = {}; win.scramb_id = 415; win.slot_delta = 3;
  const int P = c.ports, NRE = c.res_per_port_rb * RB;
  std::vector<nr_csirs_disc_re_t> re(P * NRE);
  for (int p = 0; p < P; ++p) for (int i = 0; i < NRE; ++i) {
    int rb = i / c.res_per_port_rb, j = i % c.res_per_port_rb;
    re[p * NRE + i] = {1 * 12 * RB + 12 * rb + (4 * j + p) % 12, (uint16_t)i, 1, (int8_t)((i + p) % 3 ? 1 : -1)};
    re[p * NRE + i].grid_index %= 12 * RB;
  }
  CK(cudaMemcpy(w.dm.p, re.data(), re.size() * sizeof(re[0]), cudaMemcpyHostToDevice));
  nr_csirs_disc_sweep_map_t map = {}; map.ports = P; map.n_re = P * NRE; map.words = (2 * NRE + 31) / 32; map.sym_mask = 1u << 1; map.re = w.dm.p;
  for (int p = 0; p <= P; ++p) map.begin[p] = p * NRE;
  long ns = 0; for (int i = 0; i < NS; ++i) ns += meta[i].valid && meta[i].abs_slot % h.period == h.offset;
  const long total = ns * ANT * (c.row == 1 ? 1 : P);
  /* reference: the serial kernel, whole job in one call */
  refine_kernel<<<1, 1, 0, s>>>(ring, h, win, map, w.refined.p, w.result.p, w.acc.p, 0, LONG_MAX, true); CK(cudaStreamSynchronize(s));
  RefineAcc ra; nr_csirs_disc_hyp_t rh; int rr; CK(cudaMemcpy(&ra, w.acc.p, sizeof(ra), cudaMemcpyDeviceToHost)); CK(cudaMemcpy(&rh, w.refined.p, sizeof(rh), cudaMemcpyDeviceToHost)); CK(cudaMemcpy(&rr, w.result.p, 4, cudaMemcpyDeviceToHost));
  auto now = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
  double t0 = now(); for (int i = 0; i < 3; ++i) { refine_kernel<<<1, 1, 0, s>>>(ring, h, win, map, w.refined.p, w.result.p, w.acc.p, 0, LONG_MAX, true); CK(cudaStreamSynchronize(s)); }
  const double serial_ms = (now() - t0) / 3;
  for (long per : per_list) {
    RefineAcc junk; memset(&junk, 0x5a, sizeof(junk)); CK(cudaMemcpy(w.acc.p, &junk, sizeof(junk), cudaMemcpyHostToDevice)); /* stale accumulators must be re-zeroed by the first chunk */
    t0 = now();
    for (long f = 0; f < total; f += per) { long n = std::min(per, total - f); CK(refine_chunk(ring, h, win, map, s, f, n, f + n >= total)); CK(cudaStreamSynchronize(s)); }
    const double par_ms = now() - t0;
    RefineAcc a; nr_csirs_disc_hyp_t hh; int r2; CK(cudaMemcpy(&a, w.acc.p, sizeof(a), cudaMemcpyDeviceToHost)); CK(cudaMemcpy(&hh, w.refined.p, sizeof(hh), cudaMemcpyDeviceToHost)); CK(cudaMemcpy(&r2, w.result.p, 4, cudaMemcpyDeviceToHost));
    /* row-1 'b' (weight) and row-4 'a' only are meaningful; compare the used arrays bit for bit */
    bool same = !memcmp(a.a, ra.a, sizeof(ra.a)) && (c.row != 1 || !memcmp(a.b, ra.b, sizeof(ra.b))) && rr == r2 && !memcmp(&hh, &rh, sizeof(hh));
    int nz = 0; for (int i = 0; i < RB; ++i) nz += ra.a[i] != 0;
    printf("%-28s units=%-4ld per=%-4ld result=%d span=%d+%d nonzero_rb=%d serial=%.2f ms parallel=%.2f ms : %s\n", c.name, total, per, rr, rh.rb_start, rh.n_rb, nz, serial_ms, par_ms, same ? "BIT-EXACT" : "MISMATCH");
    if (!same) { ++g_fail; for (int i = 0; i < RB; ++i) if (memcmp(&a.a[i], &ra.a[i], 8)) { printf("  first diff rb=%d new=%.17g ref=%.17g\n", i, a.a[i], ra.a[i]); break; } }
  }
  CK(cudaFree(dmeta)); if (c.host_ring) CK(cudaFreeHost(ring.grid_base)); else CK(cudaFree(ring.grid_base));
}
int main()
{
  CK(cudaSetDevice(0)); pick_stream();
  std::vector<long> pers = {1, 3, 32, 1000000};
  Case cs[] = {
    {"row4 d1 4p 217rb 2re/rb", 4, 1, 217, 4, 2, 24, 4, false},
    {"row4 d0 4p 217rb 1re/rb", 4, 0, 217, 4, 1, 32, 4, false},
    {"row4 d2 2p 106rb 2re/rb host", 4, 2, 106, 2, 2, 20, 4, true},
    {"row4 d1 1p 51rb 1 ant", 4, 1, 51, 1, 1, 32, 1, false},
    {"row1 trs 217rb", 1, 1, 217, 1, 3, 32, 4, false},
    {"row1 trs 106rb host", 1, 2, 106, 1, 3, 12, 2, true},
    {"row4 few slots (<3) 217rb", 4, 1, 217, 4, 2, 4, 4, false},
    {"row1 two slots 217rb", 1, 1, 217, 1, 3, 3, 4, false},
  };
  for (auto &c : cs) run_case(c, pers);
  printf("%s\n", g_fail ? "FAIL" : "ALL BIT-EXACT");
  return g_fail ? 1 : 0;
}
