/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI G3b: the GPU decode pass serves cells with DIFFERENT FEP geometry (n_rb_dl, first carrier offset, CP lengths, antenna count).
 *
 * The module under test is libpdsch_gpu.so as the fixture configures it (geometry 0 = 273 PRB, 4 RX, N = 4096), plus further geometries registered the way
 * nr_gpu_pass_dl.c does for a cell > 0 (nr_gpu_pass_geo_add -> nr_gpu_pass_warm_geo -> nr_gpu_pass_geo_commit -> nr_gpu_pass_cell_init_geo).
 *   GpuPassGeo.FepEqualsCpu              the FEP of each geometry (nr_pdsch_gpu_fep_dev with nr_gpu_fep_job_t.geo) equals the OAI CPU FEP bit for bit (the receive FEP of slot_fep_nr.c:
 *                                        window, DFT, apply_nr_rotation_symbol_RX with the carrier's n_rb and first carrier offset), on 217 / 162 / 106 (15 kHz CP) / 51 PRB, 4 and 2 RX, another
 *                                        CP length (another time-shift table); an equal geometry is the same id; a bigger one is refused
 *   GpuPassGeo.CellsOfOtherGeometry      cells of 217 PRB / 4 RX and 51 PRB / 2 RX decode on the graph pass: TB bytes, CRC verdict, nvar, rxdataF equal the stage path of that geometry and the
 *                                        transmitted payload; each cell's result is bit-identical alone and interleaved with the other cells; a cell of the same geometry shares the graphs
 *   GpuPassGeo.BudgetExhaustedFallsBack  with no elastic GPU headroom a further geometry is refused (-2), nothing is registered, the cell's jobs return 1 (the CPU path) and the cells that exist are
 *                                        unaffected; the memory of a registered geometry is booked on the PASS module and returned by the commit true-up */
#include <gtest/gtest.h>
#include <cuda_runtime.h>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <random>
extern "C" {
#include "PHY/TOOLS/nr_gpu_mem.h"
}
#include "nr_gpu_pass_fixture.h"
#include "PHY/TOOLS/nr_gpu_membudget.h"
#include "common/utils/nr_cell.h"
extern "C" {
#include "openair1/PHY/TOOLS/tools_defs.h"
void init_timeshift_rotation(const int ofdm_symbol_size, const int nb_prefix_samples, const uint ofdm_offset_divisor, c16_t *timeshift_symbol_rotation);
/* slot_fep_nr.c's static apply_nr_rotation_symbol_RX, call for call */
static void apply_nr_rotation_symbol_RX(const int symbols_per_slot, const int slots_per_subframe, const c16_t *shift_rot, const int first_carrier_offset, c16_t *this_symbol, const c16_t *rot,
                                        int nb_rb, int slot, int symbol)
{
  const int symb_offset = (slot % slots_per_subframe) * symbols_per_slot;
  c16_t rot2 = rot[symbol + symb_offset];
  rot2.i = -rot2.i;
  if (nb_rb & 1) {
    rotate_cpx_vector(this_symbol, rot2, this_symbol, (nb_rb + 1) * 6, 15);
    rotate_cpx_vector(this_symbol + first_carrier_offset - 6, rot2, this_symbol + first_carrier_offset - 6, (nb_rb + 1) * 6, 15);
    mult_cpx_vector(this_symbol, shift_rot, this_symbol, (nb_rb + 1) * 6, 15);
    mult_cpx_vector(this_symbol + first_carrier_offset - 6, shift_rot + first_carrier_offset - 6, this_symbol + first_carrier_offset - 6, (nb_rb + 1) * 6, 15);
  } else {
    rotate_cpx_vector(this_symbol, rot2, this_symbol, nb_rb * 6, 15);
    rotate_cpx_vector(this_symbol + first_carrier_offset, rot2, this_symbol + first_carrier_offset, nb_rb * 6, 15);
    mult_cpx_vector(this_symbol, shift_rot, this_symbol, nb_rb * 6, 15);
    mult_cpx_vector(this_symbol + first_carrier_offset, shift_rot + first_carrier_offset, this_symbol + first_carrier_offset, nb_rb * 6, 15);
  }
}
}

static bool have_gpu() { return nr_gpu_model() >= 0 && getenv("NR_GPU_TEST_SKIP") == nullptr; }
static void pass_model()
{
  if (getenv("NR_GPU_PASS_DISCRETE")) nr_gpu_mem_test_force_discrete(1);
  else nr_gpu_mem_test_force_discrete(0);
}
/* the pass of geometry 0 (the fixture's), v2 graphs, as nr_gpu_pass_test.cc */
static bool pass_ready(GpuPassModule &m)
{
  if (m.pass_state) return m.pass_state > 0;
  m.pass_state = -1;
  if (!m.pass_config || !m.pass_init || !m.pass_try) return false;
  static nr_gpu_buf_t dt; /* persistent: the graphs bake its address */
  if (nr_gpu_buf_alloc(&dt, (size_t)(2 * 20 + 1) * 4096 * 4)) return false;
  for (int d = -20; d <= 20; d++)
    for (int i = 0; i < 4096; i++) {
      const double ph = 2.0 * M_PI * i * d / 4096;
      ((int16_t *)dt.host)[2 * ((size_t)(20 + d) * 4096 + i)] = (int16_t)std::lround(256 * std::cos(ph));
      ((int16_t *)dt.host)[2 * ((size_t)(20 + d) * 4096 + i) + 1] = (int16_t)std::lround(256 * std::sin(ph));
    }
  nr_gpu_buf_h2d_async(&dt, 0, dt.bytes, nullptr);
  cudaDeviceSynchronize();
  nr_gpu_pass_cfg_t c = {4, 273, (const int16_t *)dt.dev, 4096, 20, 1};
  if (m.pass_config(&c) != 0 || m.pass_init(16) != 0) return false;
  m.pass_state = 1;
  return true;
}

/* The receiver commits every GPU module's size at the end of its warm-up (nr_gpu_membudget_commit); only then the elastic headroom is what the modules left over. The test commits a small size for each,
 * as the production sequence does, so that the headroom a further geometry draws on is the budget minus these. */
static void commit_baseline()
{
  static bool done = false;
  if (done) return;
  done = true;
  for (int mod = 0; mod < NR_GPU_MEM_N; mod++) nr_gpu_membudget_commit((nr_gpu_mem_module_t)mod, (size_t)64 << 20, 1.0);
}

namespace geo {
struct Api {
  int (*fep_geo_add)(const nr_gpu_fep_cfg_t *) = nullptr;
  int (*geo_add)(int, const nr_gpu_fep_cfg_t *, int, uint64_t *) = nullptr;
  uint64_t (*geo_commit)(int) = nullptr;
  int (*warm_geo)(const nr_gpu_pass_shape_t *, int, int, int, int) = nullptr;
  int (*cell_init_geo)(int, int, int) = nullptr;
  void (*retire)(int) = nullptr;
  int (*geo_info)(int, int *, int *, uint64_t *, uint64_t *) = nullptr;
  int (*geo_count)(void) = nullptr;
  int (*geo_fep)(int) = nullptr;
};
static bool load(GpuPassModule &m, Api &a)
{
  a.fep_geo_add = (int (*)(const nr_gpu_fep_cfg_t *))dlsym(m.dl, "nr_pdsch_gpu_fep_geo_add");
  a.geo_add = (int (*)(int, const nr_gpu_fep_cfg_t *, int, uint64_t *))dlsym(m.dl, "nr_gpu_pass_geo_add");
  a.geo_commit = (uint64_t (*)(int))dlsym(m.dl, "nr_gpu_pass_geo_commit");
  a.warm_geo = (int (*)(const nr_gpu_pass_shape_t *, int, int, int, int))dlsym(m.dl, "nr_gpu_pass_warm_geo");
  a.cell_init_geo = (int (*)(int, int, int))dlsym(m.dl, "nr_gpu_pass_cell_init_geo");
  a.retire = (void (*)(int))dlsym(m.dl, "nr_gpu_pass_cell_retire");
  a.geo_info = (int (*)(int, int *, int *, uint64_t *, uint64_t *))dlsym(m.dl, "nr_gpu_pass_geo_info");
  a.geo_count = (int (*)(void))dlsym(m.dl, "nr_gpu_pass_geo_count");
  a.geo_fep = (int (*)(int))dlsym(m.dl, "nr_gpu_pass_geo_fep");
  return a.fep_geo_add && a.geo_add && a.geo_commit && a.warm_geo && a.cell_init_geo && a.retire && a.geo_info && a.geo_count && a.geo_fep;
}
/* the FEP cfg of a carrier of n_rb PRBs with `nant` antennas (N = 4096; CP lengths as given) */
static nr_gpu_fep_cfg_t cfg_of(int nant, int n_rb, int ncp = 288, int ncp0 = 352)
{
  return {nant, 4096, ncp, ncp0, 14, 4096 - n_rb * 6, n_rb, 122880, 8};
}
struct Guard { Api &a; int cell; ~Guard() { a.retire(cell); } };
} // namespace geo

/* ---- the FEP of a geometry against the OAI CPU FEP ---- */
TEST(GpuPassGeo, FepEqualsCpu) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  geo::Api api;
  ASSERT_TRUE(geo::load(m, api)) << "libpdsch_gpu.so has no per-geometry entries";
  ASSERT_EQ(0, load_dftslib());
  struct Case { const char *name; int nant, n_rb, ncp, ncp0; };
  const Case cases[] = {{"217 PRB 4 RX", 4, 217, 288, 352}, {"162 PRB 4 RX", 4, 162, 288, 352}, {"106 PRB 3 RX 15 kHz CP", 3, 106, 288, 320},
                        {"51 PRB 2 RX", 2, 51, 288, 352}, {"51 PRB 4 RX, another CP (another time-shift table)", 4, 51, 240, 272}, {"273 PRB 1 RX", 1, 273, 288, 352}};
  const int N = 4096, nsym = 14, div = 8;
  int ids[8], nid = 0;
  for (const Case &c : cases) {
    const nr_gpu_fep_cfg_t cfg = geo::cfg_of(c.nant, c.n_rb, c.ncp, c.ncp0);
    const int id = api.fep_geo_add(&cfg);
    ASSERT_GE(id, 0) << c.name;
    ids[nid++] = id;
    EXPECT_EQ(id, api.fep_geo_add(&cfg)) << "an equal geometry is the same id: " << c.name;
    const int mu = c.ncp0 == 320 ? 0 : 1, spsf = 1 << mu;
    const size_t sps = (size_t)c.ncp0 + N + (size_t)(nsym - 1) * (N + c.ncp);
    const uint32_t ring = (uint32_t)(4 * sps);
    std::vector<std::vector<c16_t>> iq(c.nant, std::vector<c16_t>(ring));
    std::vector<c16_t> ts(N), rot(nsym * spsf);
    init_timeshift_rotation(N, c.ncp, div, ts.data());
    std::mt19937 rng(97 + c.n_rb);
    for (auto &r : rot) { r.r = (int16_t)(rng() % 60000 - 30000); r.i = (int16_t)(rng() % 60000 - 30000); }
    nr_gpu_buf_t rxF = {};
    ASSERT_EQ(0, nr_gpu_buf_alloc(&rxF, (size_t)4 * nsym * N * 4));
    cudaStream_t st;
    ASSERT_EQ(cudaSuccess, cudaStreamCreate(&st));
    int bad = 0, rows = 0;
    for (int amp : {40, 1000, 12000}) {
      for (auto &a : iq)
        for (auto &x : a) { x.r = (int16_t)((int)(rng() % (2 * amp + 1)) - amp); x.i = (int16_t)((int)(rng() % (2 * amp + 1)) - amp); }
      for (int slot = 1; slot <= 2; slot++) {
        const uint32_t ts_slot = (uint32_t)(slot * sps);
        const c16_t *rot_slot = rot.data() + (slot % spsf) * nsym;
        const int16_t *rx[4] = {};
        for (int a = 0; a < c.nant; a++) rx[a] = (const int16_t *)iq[a].data();
        nr_gpu_fep_job_t j = {};
        j.rxdata = rx; j.ring_len = ring; j.ring_offset = ts_slot; j.symbol_rot = (const int16_t *)rot_slot; j.sym_mask = 0x3fff; j.geo = id;
        int fs = 0;
        ASSERT_EQ(0, m.fep_stage(&j, &fs)) << c.name;
        ASSERT_EQ(0, m.fep(&j, &rxF, fs, st)) << c.name;
        nr_gpu_buf_d2h_async(&rxF, 0, rxF.bytes, st);
        ASSERT_EQ(cudaSuccess, cudaStreamSynchronize(st));
        const c16_t *gpu = (const c16_t *)rxF.host;
        const int early = c.ncp / div, fco = N - c.n_rb * 6;
        for (int a = 0; a < c.nant; a++)
          for (int sym = 0; sym < nsym; sym++) {
            uint32_t off = ts_slot;
            for (int l = 0; l <= sym; l++) off += l == 0 ? c.ncp0 : c.ncp;
            off += (uint32_t)(N * sym) - (uint32_t)early;
            alignas(64) static c16_t in[4096], o[4096];
            memcpy(in, &iq[a][off], sizeof(c16_t) * N);
            dft(get_dft(N), (int16_t *)in, (int16_t *)o, 1);
            apply_nr_rotation_symbol_RX(nsym, spsf, ts.data(), fco, o, rot.data(), c.n_rb, slot, sym);
            rows++;
            if (memcmp(o, &gpu[((size_t)a * nsym + sym) * N], sizeof(c16_t) * N)) bad++;
          }
      }
    }
    EXPECT_EQ(0, bad) << c.name << ": " << bad << " of " << rows << " (antenna, symbol) rows differ from the CPU FEP";
    printf("GpuPassGeo.FepEqualsCpu: %-52s id %d rows %d bad %d\n", c.name, id, rows, bad);
    cudaStreamDestroy(st);
    nr_gpu_buf_free(&rxF);
  }
  EXPECT_NE(ids[0], ids[1]);
  /* refused: more antennas than the rings of the first geometry, another FFT size, an empty grid */
  nr_gpu_fep_cfg_t bad = geo::cfg_of(5, 217);
  EXPECT_LT(api.fep_geo_add(&bad), 0) << "5 RX";
  bad = geo::cfg_of(2, 51);
  bad.ofdm_symbol_size = 2048;
  EXPECT_LT(api.fep_geo_add(&bad), 0) << "another FFT size";
  bad = geo::cfg_of(2, 51);
  bad.n_rb_dl = 0;
  EXPECT_LT(api.fep_geo_add(&bad), 0) << "no PRBs";
}

namespace geo {
/* a cell of a registered geometry: warm the shapes the carrier can use, true up the reservation, register the cell */
struct CellGeo { int cell = -1, gid = -1, nant = 0, n_rb = 0, fep = -1; uint64_t reserved = 0, used = 0; int warmed = 0; };
static bool make(GpuPassModule &m, Api &a, int cell, int nant, int n_rb, CellGeo *out, bool warm = true)
{
  const nr_gpu_fep_cfg_t cfg = cfg_of(nant, n_rb);
  const int cls = nr_gpu_pass_nrb_class(n_rb);
  uint64_t res = 0;
  const int gid = a.geo_add(cell, &cfg, cls, &res);
  out->cell = cell; out->gid = gid; out->nant = nant; out->n_rb = n_rb; out->reserved = res;
  if (gid < 0) return false;
  out->fep = a.geo_fep(gid);
  if (warm) {
    for (const nr_gpu_pass_shape_t &sh : GpuPassFixture::warm_shapes()) {
      if ((int)sh.nrb_class > cls || (int)sh.nl > nant || sh.qm != 2) continue; /* the key does not carry the modulation order: one shape per (PRB class, layers) */
      if (a.warm_geo(&sh, sh.nl >= 2 ? 3 : 0, 0, 0, gid) == 0) out->warmed++;
    }
    out->used = a.geo_commit(gid);
    if (!out->used || !out->warmed) return false;
  }
  return a.cell_init_geo(cell, n_rb, gid) == 0;
}
struct Res {
  int rc = -1;
  std::vector<uint8_t> tb;
  nr_gpu_pass_result_t r{};
  std::vector<int16_t> llr, rxF;
};
static Res run(GpuPassModule &m, GpuPassFixture &f, int cell)
{
  Res o;
  f.job->cell = (uint8_t)cell;
  f.job->llr_out_all = 1;
  std::fill(f.llr_pass.begin(), f.llr_pass.end(), (int16_t)0);
  std::fill(f.rxF_out.begin(), f.rxF_out.end(), (int16_t)0);
  o.tb.assign(f.tb_info_bytes() + 64, 0);
  o.r.tb = o.tb.data();
  o.rc = m.pass_try(f.job, &o.r);
  o.llr.assign(f.llr_pass.begin(), f.llr_pass.begin() + f.job->G);
  o.rxF = f.rxF_out;
  return o;
}
static bool same(const Res &a, const Res &b, size_t tbb)
{
  return a.rc == b.rc && a.r.tb_ok == b.r.tb_ok && a.r.need_cpu == b.r.need_cpu && a.r.nvar == b.r.nvar && a.r.log2_maxh == b.r.log2_maxh && !memcmp(a.r.seg_ok, b.r.seg_ok, sizeof(a.r.seg_ok)) &&
         !memcmp(a.tb.data(), b.tb.data(), tbb) && a.llr == b.llr && a.rxF == b.rxF;
}
struct Carrier { // fixtures are built for one carrier at a time (statics of the fixture)
  Carrier(int rb, int nant) { GpuPassFixture::geom_rb = rb; GpuPassFixture::geom_nant = nant; }
  ~Carrier() { GpuPassFixture::geom_rb = GpuPassFixture::NRB; GpuPassFixture::geom_nant = GpuPassFixture::NANT; }
};
static nr_gpu_pass_shape_t shape_of(int nrb, int nl, int qm)
{
  for (const nr_gpu_pass_shape_t &s : GpuPassFixture::warm_shapes())
    if ((int)s.nrb_class == nrb && (int)s.nl == nl && (int)s.qm == qm) return s;
  return {};
}
} // namespace geo

TEST(GpuPassGeo, CellsOfOtherGeometry) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m));
  geo::Api api;
  ASSERT_TRUE(geo::load(m, api));
  commit_baseline();
  const int ng0 = api.geo_count();
  geo::CellGeo big, small, twin;
  ASSERT_TRUE(geo::make(m, api, 3, 4, 217, &big)) << "217 PRB / 4 RX: gid " << big.gid;
  geo::Guard g3{api, 3};
  ASSERT_TRUE(geo::make(m, api, 4, 2, 51, &small)) << "51 PRB / 2 RX: gid " << small.gid;
  geo::Guard g4{api, 4};
  EXPECT_GT(big.gid, 0);
  EXPECT_GT(small.gid, big.gid);
  EXPECT_EQ(ng0 + 2, api.geo_count());
  printf("GpuPassGeo.CellsOfOtherGeometry: 217 PRB / 4 RX: gid %d, %d classes, reserved %lu KB, graphs %lu KB (booked on PASS: %lu KB)\n", big.gid, big.warmed, (unsigned long)(big.reserved >> 10),
         (unsigned long)(big.used >> 10), (unsigned long)(nr_gpu_membudget_dyn_bytes(NR_GPU_MEM_PASS) >> 10));
  printf("GpuPassGeo.CellsOfOtherGeometry: 51 PRB / 2 RX: gid %d, %d classes, reserved %lu KB, graphs %lu KB\n", small.gid, small.warmed, (unsigned long)(small.reserved >> 10), (unsigned long)(small.used >> 10));
  EXPECT_LT(small.used, big.used) << "a narrower carrier / fewer antennas needs fewer and smaller graphs";
  /* a third cell of the 217 PRB / 4 RX geometry shares the entry and its graphs: no new geometry, no new reservation, no new class */
  uint64_t o0[16], o1[16];
  m.pass_counters(o0);
  ASSERT_TRUE(geo::make(m, api, 5, 4, 217, &twin, false));
  geo::Guard g5{api, 5};
  EXPECT_EQ(big.gid, twin.gid) << "same geometry, same entry";
  EXPECT_EQ(0u, twin.reserved);
  EXPECT_EQ(ng0 + 2, api.geo_count());
  /* grants: the cell-0 geometry (273 PRB, 4 RX), 217 PRB / 4 RX, 51 PRB / 2 RX */
  struct G { const char *name; int rb, nant, cell, gid, nrb, nl, qm; };
  const G grants[] = {{"cell 0: 273 PRB 4 RX", 273, 4, 0, 0, 96, 2, 4}, {"cell 3: 217 PRB 4 RX", 217, 4, 3, big.gid, 96, 2, 4}, {"cell 3: 217 PRB 4 RX, 4 layers", 217, 4, 3, big.gid, 48, 4, 2},
                      {"cell 4: 51 PRB 2 RX", 51, 2, 4, small.gid, 24, 2, 4}, {"cell 4: 51 PRB 2 RX, 1 layer", 51, 2, 4, small.gid, 48, 1, 6}, {"cell 5: 217 PRB 4 RX (shared)", 217, 4, 5, big.gid, 96, 2, 4}};
  std::vector<GpuPassFixture> fx;
  fx.reserve(8);
  std::vector<geo::Res> solo;
  int decoded = 0;
  for (const G &g : grants) {
    geo::Carrier cr(g.rb, g.nant);
    nr_gpu_pass_shape_t sh = geo::shape_of(g.nrb, g.nl, g.qm);
    ASSERT_NE(0, sh.nrb_class) << g.name;
    fx.push_back(GpuPassFixture::from_shape(sh, 21 + (int)fx.size()));
    GpuPassFixture &f = fx.back();
    ASSERT_TRUE(f.tb_ok_built) << g.name;
    f.geo_id = g.gid ? api.geo_fep(g.gid) : 0;
    f.bind();
    ASSERT_EQ(g.nant, f.nant_);
    ASSERT_EQ(g.rb, f.crb_);
  }
  for (size_t i = 0; i < fx.size(); i++) fx[i].bind(); /* the vector settled: the jobs point into the fixtures at their final addresses */
  for (size_t i = 0; i < fx.size(); i++) {
    const G &g = grants[i];
    GpuPassFixture &f = fx[i];
    /* the stage path of this geometry (FEP with the geometry's kernel arguments, chest, demod, CPU CRC) and the transmitted payload */
    std::vector<uint8_t> sb(f.tb.A / 8 + 64, 0x55);
    nr_gpu_pass_result_t s{};
    s.tb = sb.data();
    ASSERT_EQ(0, f.run_stage_path(&s)) << g.name;
    geo::Res r = geo::run(m, f, g.cell);
    ASSERT_EQ(0, r.rc) << g.name << ": the pass must take a cell of a registered geometry";
    EXPECT_EQ(s.need_cpu, r.r.need_cpu) << g.name;
    EXPECT_EQ(s.tb_ok, r.r.tb_ok) << g.name;
    EXPECT_EQ(s.nvar, r.r.nvar) << "chest post nvar vs the stage path, " << g.name;
    EXPECT_EQ(0, memcmp(s.tb, r.r.tb, r.r.tbs / 8)) << g.name;
    EXPECT_EQ(0, memcmp(s.seg_ok, r.r.seg_ok, sizeof(s.seg_ok))) << g.name;
    if (r.r.tb_ok) {
      EXPECT_EQ(0, memcmp(r.r.tb, f.tb.payload.data(), f.tb.A / 8)) << "decoded TB differs from the transmitted one, " << g.name;
      decoded++;
    }
    for (int a = 0; a < g.nant; a++)
      for (int sy = 0; sy < 14; sy++)
        ASSERT_EQ(0, memcmp(&f.rxF_out[2 * ((size_t)a * GpuPassFixture::RXSTRIDE + (size_t)sy * 4096)], &f.rxF_ref[2 * ((size_t)a * 14 * 4096 + (size_t)sy * 4096)], 4096 * 4))
            << "rxdataF of the pass vs the stage path, ant " << a << " sym " << sy << ", " << g.name;
    solo.push_back(std::move(r));
  }
  EXPECT_GE(decoded, 4) << "the synthetic channel decodes most of these grants";
  /* interleaved: every run equals its first (solo) run bit for bit, whatever the previous run's geometry was */
  for (int rep = 0; rep < 3; rep++)
    for (size_t k = 0; k < fx.size(); k++) {
      const size_t i = (k * 5 + (size_t)rep) % fx.size();
      geo::Res r = geo::run(m, fx[i], grants[i].cell);
      EXPECT_TRUE(geo::same(r, solo[i], fx[i].tb_info_bytes())) << grants[i].name << " interleaved, rep " << rep;
    }
  m.pass_counters(o1);
  EXPECT_EQ(0u, (o1[2] - o0[2]) + (o1[3] - o0[3]) + (o1[4] - o0[4]) + (o1[5] - o0[5])) << "no unseen class, busy, unsupported or error";
  EXPECT_EQ(o0[9], o1[9]) << "the grants captured nothing new: every one was a graph hit (the twin cell added no class)";
  /* a cell of an unregistered geometry (never added) is refused cleanly */
  geo::Res bad = geo::run(m, fx[1], 9);
  EXPECT_EQ(1, bad.rc);
  /* a job of cell 3 carrying the grid of cell 4 (antennas differ) is refused, not decoded with the wrong geometry */
  geo::Res wrong = geo::run(m, fx[1], 4);
  EXPECT_EQ(1, wrong.rc) << "the job's antenna count must be the cell's geometry's";
}

TEST(GpuPassGeo, BudgetExhaustedFallsBack) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m));
  geo::Api api;
  ASSERT_TRUE(geo::load(m, api));
  commit_baseline();
  const size_t head = nr_gpu_membudget_headroom();
  if (head == SIZE_MAX) GTEST_SKIP() << "the GPU budget is unlimited on this device: nothing to exhaust";
  const int ng0 = api.geo_count();
  /* the whole elastic headroom is taken by another module */
  ASSERT_EQ(1, nr_gpu_membudget_dyn_acquire(NR_GPU_MEM_CSIRS, head));
  geo::CellGeo cg;
  const bool made = geo::make(m, api, 6, 3, 106, &cg);
  EXPECT_FALSE(made);
  EXPECT_EQ(-2, cg.gid) << "refused for the budget";
  EXPECT_EQ(ng0, api.geo_count()) << "nothing was registered";
  EXPECT_NE(0, api.cell_init_geo(6, 106, -2)) << "no cell registration without a geometry";
  geo::Carrier cr(106, 3);
  GpuPassFixture f = GpuPassFixture::from_shape(geo::shape_of(48, 2, 4), 5);
  ASSERT_TRUE(f.tb_ok_built);
  f.bind();
  geo::Res r = geo::run(m, f, 6);
  EXPECT_EQ(1, r.rc) << "the cell's jobs fall back to the CPU path";
  nr_gpu_membudget_dyn_release(NR_GPU_MEM_CSIRS, head);
  /* with the headroom back the same geometry registers, and its graphs are booked on the PASS module */
  const size_t pass0 = nr_gpu_membudget_dyn_bytes(NR_GPU_MEM_PASS);
  geo::CellGeo cg2;
  ASSERT_TRUE(geo::make(m, api, 6, 3, 106, &cg2));
  geo::Guard g6{api, 6};
  EXPECT_GT(cg2.gid, 0);
  /* the PASS module holds the geometry's graphs at their measured size (the commit trued the reservation up) plus the cell's HARQ slab */
  const size_t hq = (size_t)NR_GPU_HQ_BLK * NR_GPU_HQ_SEG * 2, dyn = nr_gpu_membudget_dyn_bytes(NR_GPU_MEM_PASS) - pass0;
  EXPECT_TRUE(dyn == cg2.used + hq || dyn == cg2.used) << "dyn " << dyn << " used " << cg2.used << " harq " << hq;
  printf("GpuPassGeo.BudgetExhaustedFallsBack: refused with no headroom; 106 PRB / 3 RX then took %lu KB (%d classes)\n", (unsigned long)(cg2.used >> 10), cg2.warmed);
  GpuPassFixture f2 = GpuPassFixture::from_shape(geo::shape_of(48, 2, 4), 5);
  ASSERT_TRUE(f2.tb_ok_built);
  f2.geo_id = api.geo_fep(cg2.gid);
  f2.bind();
  geo::Res r2 = geo::run(m, f2, 6);
  EXPECT_EQ(0, r2.rc);
  EXPECT_TRUE(r2.r.tb_ok);
}

/* Split mode: the C and D sub-graphs are captured per geometry, the L sub-graph (rate dematch + LDPC + CRC) is geometry free and shared with every other geometry. */
TEST(GpuPassGeo, SplitModeSharesTheLdpcGraph) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m));
  ASSERT_TRUE(m.pass_split) << "no nr_gpu_pass_test_split";
  geo::Api api;
  ASSERT_TRUE(geo::load(m, api));
  commit_baseline();
  m.pass_split(1);
  uint64_t o0[16], o1[16];
  m.pass_counters(o0);
  geo::CellGeo a, b;
  const bool made_a = geo::make(m, api, 7, 4, 133, &a);
  geo::Guard g7{api, 7};
  m.pass_counters(o1);
  const uint64_t keys_a = o1[9] - o0[9];
  ASSERT_TRUE(made_a) << "gid " << a.gid;
  const bool made_b = geo::make(m, api, 8, 2, 133, &b); /* same carrier, fewer antennas: its own C and D graphs, the same L graphs */
  geo::Guard g8{api, 8};
  m.pass_counters(o0);
  const uint64_t keys_b = o0[9] - o1[9];
  m.pass_split(0);
  ASSERT_TRUE(made_b) << "gid " << b.gid;
  EXPECT_NE(a.gid, b.gid);
  printf("GpuPassGeo.SplitModeSharesTheLdpcGraph: 133 PRB / 4 RX: %lu new graph keys, %lu KB; 133 PRB / 2 RX: %lu new keys, %lu KB\n", (unsigned long)keys_a, (unsigned long)(a.used >> 10), (unsigned long)keys_b,
         (unsigned long)(b.used >> 10));
  EXPECT_GT(keys_a, 0u);
  EXPECT_LT(keys_b, keys_a) << "the second geometry added fewer keys than the first: its L sub-graphs already existed";
  /* a grant of each decodes against its stage path and the payload (split launch: C, D, L back to back) */
  m.pass_split(1);
  for (const geo::CellGeo *cg : {&a, &b}) {
    geo::Carrier cr(133, cg->nant);
    GpuPassFixture f = GpuPassFixture::from_shape(geo::shape_of(48, cg->nant >= 2 ? 2 : 1, 4), 31);
    ASSERT_TRUE(f.tb_ok_built);
    f.geo_id = cg->fep;
    f.bind();
    std::vector<uint8_t> sb(f.tb.A / 8 + 64, 0x55);
    nr_gpu_pass_result_t s{};
    s.tb = sb.data();
    ASSERT_EQ(0, f.run_stage_path(&s));
    geo::Res r = geo::run(m, f, cg->cell);
    ASSERT_EQ(0, r.rc) << "split pass of cell " << cg->cell;
    EXPECT_EQ(s.tb_ok, r.r.tb_ok);
    EXPECT_EQ(s.nvar, r.r.nvar);
    EXPECT_EQ(0, memcmp(s.tb, r.r.tb, r.r.tbs / 8));
    if (r.r.tb_ok) EXPECT_EQ(0, memcmp(r.r.tb, f.tb.payload.data(), f.tb.A / 8));
  }
  m.pass_split(0);
}
