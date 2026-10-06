/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc.h"
#include "nr_csirs_disc_sweep_internal.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <random>
#include <vector>
#ifdef NR_CSIRS_DISC_TEST_CUDA
#include <cuda_runtime_api.h>
#endif
extern "C" {
void nr_csirs_disc_test_init(void);
int nr_csirs_disc_test_mapping(const nr_csirs_disc_hyp_t *, int, int, int);
int nr_csirs_disc_test_reference(const nr_csirs_disc_hyp_t *, int, int, int, int16_t *);
double nr_csirs_blind_correlate_blocks_shift(const int16_t *, const int16_t *, int, int, int, int *);
}
namespace {
using Sweep = decltype(&nr_csirs_disc_sweep_cpu);
std::mt19937 rng(0x415c5d);
nr_csirs_disc_hyp_t hyp(int row, int rb = 9, int nrb = 64)
{
  nr_csirs_disc_hyp_t h = {};
  h.row = row;
  static const int bits[] = {0,1,1,1,1,1,4,2,2,6,3,4,4,3,3,3,4,4,4};
  static const int cdm[] = {0,0,0,1,1,1,1,1,2,1,2,1,2,1,2,3,1,2,3};
  h.freq_domain = (1 << bits[row]) - 1;
  h.symb_l0 = 4;
  h.symb_l1 = 9;
  h.cdm_type = cdm[row];
  h.density = row == 1 ? 3 : 2;
  h.rb_start = rb;
  h.n_rb = nrb;
  h.period = 5;
  h.offset = 2;
  return h;
}
struct Grid {
  nr_csirs_disc_ring_t r = {};
  std::vector<int16_t> samples;
  std::vector<nr_csirs_disc_slot_meta_t> meta;
  Grid(int nrb = 128, int slots = 8) : samples((size_t)slots*4*14*24*nrb), meta(slots)
  {
    r.n_ant = 4;
    r.n_rb = nrb;
    r.slots_per_frame = 20;
    r.capacity = slots;
    r.slot_stride = (size_t)4*14*24*nrb;
    r.grid_base = samples.data();
    r.meta = meta.data();
    r.written = slots;
    for (int s = 0; s < slots; ++s) {
      // All on-lattice, non-chronological ring order, wraps slot and frame numbering.
      unsigned abs = 997 + ((s + 3) % slots)*5;
      meta[s] = {abs, uint16_t(abs/20), uint8_t(abs%20), 1};
    }
    noise();
  }
  void noise()
  {
    std::normal_distribution<float> n(0, 100);
    for (auto &v : samples)
      v = std::lround(n(rng));
  }
  void add(const nr_csirs_disc_hyp_t &h, int id, int delta, float snr_db = 10)
  {
    std::vector<int16_t> ref((size_t)32*14*24*r.n_rb);
    int gs = 1 << h.cdm_type;
    // Equal energy, distinct port phases on every antenna. No coherent antenna/port sum in scoring.
    float gain = 100*std::pow(10.f, snr_db/20)/8192/std::sqrt(float(gs));
    for (int s = 0; s < r.capacity; ++s) {
      if (!meta[s].valid || meta[s].abs_slot % h.period != h.offset)
        continue;
      int ports = nr_csirs_disc_test_reference(&h, r.n_rb, (meta[s].slot+delta)%r.slots_per_frame, id, ref.data());
      for (int a = 0; a < 4; ++a) {
        auto *dst = nr_csirs_disc_ring_sym(&r, s, a, 0);
        for (int p = 0; p < ports; ++p) {
          auto channel = std::polar(gain, float(.73*a + 1.17*p));
          const auto *src = ref.data() + (size_t)p*14*24*r.n_rb;
          for (int k = 0; k < 14*12*r.n_rb; ++k) {
            auto v = std::complex<float>(src[2*k], src[2*k+1])*channel;
            dst[2*k] += std::lround(v.real());
            dst[2*k+1] += std::lround(v.imag());
          }
        }
      }
    }
  }
};

void recovery(Sweep sweep)
{
  for (int row : {1,2,4,6,11,16}) {
    auto h = hyp(row, rng()%24, 48+16*(rng()%3));
    int id = rng()%1024;
    int delta = row == 2 ? 19 : 0;
    Grid grid;
    grid.add(h, id, delta);
    // A wider hypothesis includes noise-only RBs and should lose even at its best ID.
    auto wide = h;
    wide.rb_start = 0;
    wide.n_rb = grid.r.n_rb;
    nr_csirs_disc_hyp_t hs[] = {h, wide};
    nr_csirs_disc_sweep_cfg_t cfg = {0,1024,row == 2 ? 20 : 1,32};
    nr_csirs_disc_win_t w[2];
    ASSERT_EQ(sweep(&grid.r, hs, 2, &cfg, w), 2);
    EXPECT_EQ(w[0].scramb_id, id) << row;
    EXPECT_EQ(w[0].slot_delta, delta) << row;
    EXPECT_GE(w[0].z, 4) << row;
    EXPECT_GT(w[0].z, w[1].z) << row;
    EXPECT_GT(w[0].z, w[0].z_second*1.5) << row;
    EXPECT_EQ(w[0].n_slots, 8);
    printf("row=%d id=%d delta=%d z=%.6f runner=%.6f wide=%.6f\n", row,id,delta,w[0].z,w[0].z_second,w[1].z);
  }
}
TEST(CsirsDiscSweep, Recovery) { recovery(nr_csirs_disc_sweep_cpu); }

TEST(CsirsDiscSweep, AllRowsMappingAndHalfDensity)
{
  for (int row = 1; row <= 18; ++row)
    for (int density : {0,1,2,3}) {
      if ((row == 1) != (density == 3)) continue;
      // Odd end at the maximum grid edge exposes truncated modulation lengths.
      auto h = hyp(row,211,64);
      h.density = density;
      EXPECT_EQ(nr_csirs_disc_test_mapping(&h,275,19,1023),0) << row << ":" << density;
    }
  for (int density : {0,1}) {
    auto h = hyp(2,6,61);
    h.density = density;
    Grid grid(67,3);
    grid.add(h,41,7);
    nr_csirs_disc_sweep_cfg_t cfg = {0,64,8,16};
    nr_csirs_disc_win_t w;
    ASSERT_EQ(nr_csirs_disc_sweep_cpu(&grid.r,&h,1,&cfg,&w),1);
    EXPECT_EQ(w.scramb_id,41);
    EXPECT_EQ(w.slot_delta,7);
  }
}

TEST(CsirsDiscSweep, OtaLikeExtentAndCpuTime)
{
  auto h = hyp(1,0,52);
  Grid grid(217);
  grid.add(h,415,0);
  nr_csirs_disc_win_t w;
  auto start = std::chrono::steady_clock::now();
  ASSERT_EQ(nr_csirs_disc_sweep_cpu(&grid.r,&h,1,nullptr,&w),1);
  auto ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  EXPECT_EQ(w.scramb_id,415);
  EXPECT_EQ(w.slot_delta,0);
  EXPECT_GE(w.z,4);
  printf("CPU one hypothesis x 1024 IDs: %.3f ms (217 RB grid, 52 RB extent, 8 lattice slots, 4 RX)\n",ms);
}

TEST(CsirsDiscSweep, NoiseCalibration200Trials)
{
  auto h = hyp(1,0,64);
  Grid grid(64,8);
  nr_csirs_disc_sweep_cfg_t cfg = {0,1024,1,32};
  double sum = 0, fixed_sum = 0;
  for (int i = 0; i < 200; ++i) {
    grid.noise();
    nr_csirs_disc_win_t w;
    ASSERT_EQ(nr_csirs_disc_sweep_cpu(&grid.r,&h,1,&cfg,&w),1);
    sum += w.z;
    nr_csirs_disc_sweep_cfg_t fixed = {415,1,1,32};
    ASSERT_EQ(nr_csirs_disc_sweep_cpu(&grid.r,&h,1,&fixed,&w),1);
    fixed_sum += w.z;
  }
  printf("noise z mean (200 trials, 1024-ID maximum): %.6f; fixed ID: %.6f\n",sum/200,fixed_sum/200);
  EXPECT_GE(fixed_sum/200,.85);
  EXPECT_LE(fixed_sum/200,1.15);
  EXPECT_GE(sum/200,.85);
  EXPECT_LE(sum/200,1.15);
}

TEST(CsirsDiscSweep, CorrelatorParityAndRunnerUp)
{
  auto h = hyp(4,7,53); // partial last coherent block
  Grid grid(64,3);
  grid.add(h,13,3);
  nr_csirs_disc_sweep_cfg_t cfg = {10,7,5,31};
  nr_csirs_disc_win_t w;
  ASSERT_EQ(nr_csirs_disc_sweep_cpu(&grid.r,&h,1,&cfg,&w),1);
  float runner = 0;
  for (int id = 10; id < 17; ++id) {
    auto one = cfg;
    one.id_first = id;
    one.id_count = 1;
    nr_csirs_disc_win_t wi;
    ASSERT_EQ(nr_csirs_disc_sweep_cpu(&grid.r,&h,1,&one,&wi),1);
    if (id != w.scramb_id)
      runner = std::max(runner,wi.z);
  }
  EXPECT_FLOAT_EQ(w.z_second,runner);
  std::vector<int16_t> ref((size_t)32*14*24*64);
  double sum = 0;
  for (int s = 0; s < 3; ++s) {
    int ports = nr_csirs_disc_test_reference(&h,64,(grid.meta[s].slot+w.slot_delta)%20,w.scramb_id,ref.data());
    for (int p = 0; p < ports; ++p)
      for (int a = 0; a < 4; ++a)
        sum += nr_csirs_blind_correlate_blocks_shift(nr_csirs_disc_ring_sym(&grid.r,s,a,0),
                                                    ref.data()+(size_t)p*14*24*64,14*12*64,31,0,nullptr)/(ports*4*3);
  }
  EXPECT_NEAR(w.z,sum,1e-6);
}

TEST(CsirsDiscSweep, LatticeAndFourAntennas)
{
  auto h = hyp(1,0,32);
  Grid grid(64,8);
  grid.meta[0].valid = 0;
  grid.meta[1].abs_slot++;
  grid.add(h,321,0);
  // Off-lattice/invalid entries must not affect score even when they contain another strong ID.
  Grid reference(64,8);
  reference.samples = grid.samples;
  reference.r.grid_base = reference.samples.data();
  reference.meta = grid.meta;
  reference.r.meta = reference.meta.data();
  std::fill(grid.samples.begin(), grid.samples.begin()+2*grid.r.slot_stride, 30000);
  nr_csirs_disc_sweep_cfg_t cfg = {300,32,1,32};
  nr_csirs_disc_win_t a,b;
  ASSERT_EQ(nr_csirs_disc_sweep_cpu(&grid.r,&h,1,&cfg,&a),1);
  ASSERT_EQ(nr_csirs_disc_sweep_cpu(&reference.r,&h,1,&cfg,&b),1);
  EXPECT_FLOAT_EQ(a.z,b.z);
  EXPECT_EQ(a.n_slots,6);
  EXPECT_EQ(a.scramb_id,321);
  // Leave only antenna 3: recovery proves all four branches participate.
  for (int s = 0; s < grid.r.capacity; ++s)
    std::fill(nr_csirs_disc_ring_sym(&grid.r,s,0,0),nr_csirs_disc_ring_sym(&grid.r,s,3,0),0);
  ASSERT_EQ(nr_csirs_disc_sweep_cpu(&grid.r,&h,1,&cfg,&a),1);
  EXPECT_EQ(a.scramb_id,321);
}

TEST(CsirsDiscSweep, InvalidEmptyAndDisabled)
{
  auto h = hyp(6);
  Grid grid;
  nr_csirs_disc_win_t w = {};
  nr_csirs_disc_sweep_cfg_t c = {0,1,1,32};
  h.freq_domain = 1;
  EXPECT_EQ(nr_csirs_disc_sweep_cpu(&grid.r,&h,1,&c,&w),-1);
  h = hyp(1);
  c.id_first = 1024;
  EXPECT_EQ(nr_csirs_disc_sweep_cpu(&grid.r,&h,1,&c,&w),-1);
  c.id_first = 0;
  for (auto &m : grid.meta) m.valid = 0;
  ASSERT_EQ(nr_csirs_disc_sweep_cpu(&grid.r,&h,1,&c,&w),1);
  EXPECT_EQ(w.z,0);
  EXPECT_EQ(w.n_slots,0);
  unsetenv("ISAC_CSIRS_DISC");
  EXPECT_EQ(nr_csirs_disc_sweep_cpu(nullptr,nullptr,0,nullptr,nullptr),0);
  setenv("ISAC_CSIRS_DISC","1",1);
}

[[maybe_unused]] Sweep gpu(void **module)
{
  *module = dlopen("./libnr_csirs_disc_gpu_sweep.so",RTLD_NOW|RTLD_LOCAL);
  if (!*module) return nullptr;
  return reinterpret_cast<Sweep>(dlsym(*module,"nr_csirs_disc_gpu_sweep_entry"));
}
TEST(CsirsDiscSweep, GpuEqualsCpu)
{
#ifdef NR_CSIRS_DISC_TEST_CUDA
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess || !count) GTEST_SKIP() << "no CUDA device";
  void *module;
  Sweep fn = gpu(&module);
  ASSERT_NE(fn,nullptr) << dlerror();
  for (int row : {1,2,4,6,8,11,15,16,17,18}) {
    auto h = hyp(row,3,53);
    Grid grid(64,4);
    grid.add(h,415,19);
    nr_csirs_disc_sweep_cfg_t cfg = {0,1024,20,32};
    nr_csirs_disc_win_t cpu,g;
    ASSERT_EQ(nr_csirs_disc_sweep_cpu(&grid.r,&h,1,&cfg,&cpu),1);
    ASSERT_EQ(fn(&grid.r,&h,1,&cfg,&g),1);
    EXPECT_EQ(cpu.scramb_id,g.scramb_id) << row;
    EXPECT_EQ(cpu.slot_delta,g.slot_delta) << row;
    EXPECT_EQ(cpu.n_slots,g.n_slots);
    EXPECT_NEAR(cpu.z,g.z,1e-3) << row;
    EXPECT_NEAR(cpu.z_second,g.z_second,1e-3) << row;
    for (int density : {0,1}) {
      if (row == 1) continue;
      h.density = density;
      h.n_rb = 54; // odd exclusive end, even-half-density last RE
      grid.noise();
      grid.add(h,415,2);
      cfg = {400,32,4,17};
      ASSERT_EQ(nr_csirs_disc_sweep_cpu(&grid.r,&h,1,&cfg,&cpu),1);
      ASSERT_EQ(fn(&grid.r,&h,1,&cfg,&g),1);
      EXPECT_EQ(cpu.scramb_id,g.scramb_id);
      EXPECT_EQ(cpu.slot_delta,g.slot_delta);
      EXPECT_NEAR(cpu.z,g.z,1e-3);
      EXPECT_NEAR(cpu.z_second,g.z_second,1e-3);
    }
    // Also compare noise and exact ties (all-zero input).
    for (int mode = 0; mode < 2; ++mode) {
      grid.noise();
      if (mode) std::fill(grid.samples.begin(),grid.samples.end(),0);
      cfg = {410,16,3,17};
      ASSERT_EQ(nr_csirs_disc_sweep_cpu(&grid.r,&h,1,&cfg,&cpu),1);
      ASSERT_EQ(fn(&grid.r,&h,1,&cfg,&g),1);
      EXPECT_EQ(cpu.scramb_id,g.scramb_id);
      EXPECT_EQ(cpu.slot_delta,g.slot_delta);
      EXPECT_NEAR(cpu.z,g.z,1e-3);
      EXPECT_NEAR(cpu.z_second,g.z_second,1e-3);
    }
  }
  dlclose(module);
#else
  GTEST_SKIP() << "CPU build; CUDA comparison available in build_gpu";
#endif
}

TEST(CsirsDiscSweep, DISABLED_GpuBudget)
{
#ifdef NR_CSIRS_DISC_TEST_CUDA
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess || !count) GTEST_SKIP() << "no CUDA device";
  void *module;
  Sweep fn = gpu(&module);
  ASSERT_NE(fn,nullptr) << dlerror();
  Grid grid(217);
  std::vector<nr_csirs_disc_hyp_t> hs(64,hyp(1,0,217));
  nr_csirs_disc_sweep_cfg_t cfg = {0,1024,1,32};
  std::vector<nr_csirs_disc_win_t> w(64);
  ASSERT_EQ(fn(&grid.r,hs.data(),64,&cfg,w.data()),64); // warm-up
  for (int i = 0; i < 5; ++i) {
    auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(fn(&grid.r,hs.data(),64,&cfg,w.data()),64);
    printf("GPU 64 hypotheses x 1024 IDs x 8 slots x 651 RE x 4 RX: %.3f ms\n",
           std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
  }
  dlclose(module);
#else
  GTEST_SKIP() << "CPU build";
#endif
}
} // namespace
int main(int argc,char **argv)
{
  ::testing::InitGoogleTest(&argc,argv);
  nr_csirs_disc_test_init();
  setenv("ISAC_CSIRS_DISC","1",1);
  return RUN_ALL_TESTS();
}
