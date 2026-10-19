#include <chrono>
#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <cuda_runtime.h>
#include <vector>
extern "C" {
#include "PHY/TOOLS/nr_gpu_mem.h"
}
#include <map>
#include "nr_gpu_pass_fixture.h"
#include "nr_gpu_pass_fp32_gate.h"
#include "nr_gpu_pass_retry.h"
#include "nr_gpu_pass_hq.h"
#include "nr_gpu_platform.h"
#include "nr_gpu_pass_post.h"
#include <random>
#include <dlfcn.h>
#include <cmath>
#include <algorithm>
#include <malloc.h>
#include <atomic>
#include <thread>
#include <memory>
#include <string>
#include <array>
static bool have_gpu() { return nr_gpu_model() >= 0 && getenv("NR_GPU_TEST_SKIP") == nullptr; }

TEST(GpuMem, RoundTripBothModels) {
  if (!have_gpu()) GTEST_SKIP();
  for (int force : {0, 1}) {
    setenv("ISAC_GPU_FORCE_DISCRETE", force ? "1" : "0", 1);
    nr_gpu_buf_t b;
    ASSERT_EQ(0, nr_gpu_buf_alloc(&b, 4096));
    EXPECT_EQ(force ? NR_GPU_MODEL_DISCRETE : nr_gpu_model(), b.model);
    for (int i = 0; i < 4096; i++) ((unsigned char *)b.host)[i] = (unsigned char)i;
    ASSERT_EQ(0, nr_gpu_buf_h2d_async(&b, 0, 4096, nullptr));
    memset(b.host, 0, 4096);
    ASSERT_EQ(0, nr_gpu_buf_d2h_async(&b, 0, 4096, nullptr));
    cudaDeviceSynchronize();
    if (b.model == NR_GPU_MODEL_DISCRETE) {
      for (int i = 0; i < 4096; i++) ASSERT_EQ((unsigned char)i, ((unsigned char *)b.host)[i]);
    }
    nr_gpu_buf_free(&b);
  }
}
TEST(GpuMem, AllocIsFirstTouched) {
  if (!have_gpu()) GTEST_SKIP();
  nr_gpu_buf_t b;
  ASSERT_EQ(0, nr_gpu_buf_alloc(&b, 1 << 20));
  for (size_t i = 0; i < (1u << 20); i += 4096) ASSERT_EQ(0, ((unsigned char *)b.host)[i]);
  nr_gpu_buf_free(&b);
}

/* ===================================================== Task 4: the shape-class graph pass ===================================================== */
/* The graphs are captured per instance under the memory model of the process (ctest runs this suite once as is and once with NR_GPU_PASS_DISCRETE=1). */
/* the FEP module sizes its pass LS rows at init from these (1024 = the split value; a 128-segment row group is 512 rows); set before any test initialises it */
[[maybe_unused]] static const int g_split_ls_rows_env = (setenv("ISAC_PDSCH_GPU_GRAPH", "1", 0), setenv("ISAC_PDSCH_GPU_GRAPH_SPLIT", "1", 0), setenv("ISAC_GPU_PASS_FP32", "0", 0), 0); /* the bit-exact double demod is the reference here (FP32 is the passive default since t26zE) */
static void pass_model()
{
  if (getenv("NR_GPU_PASS_DISCRETE")) setenv("ISAC_GPU_FORCE_DISCRETE", "1", 1);
  else unsetenv("ISAC_GPU_FORCE_DISCRETE");
}
static bool pass_ready(GpuPassModule &m)
{
  if (m.pass_state) return m.pass_state > 0;
  m.pass_state = -1;
  if (!m.pass_config || !m.pass_init || !m.pass_try) return false;
  static nr_gpu_buf_t dt; /* the OAI delay table, as Pass::setup builds it; persistent: the graphs bake its address */
  if (nr_gpu_buf_alloc(&dt, (size_t)(2 * 20 + 1) * 4096 * 4)) return false;
  for (int d = -20; d <= 20; d++)
    for (int i = 0; i < 4096; i++) {
      const double ph = 2.0 * M_PI * i * d / 4096;
      ((int16_t *)dt.host)[2 * ((size_t)(20 + d) * 4096 + i)] = (int16_t)std::lround(256 * std::cos(ph));
      ((int16_t *)dt.host)[2 * ((size_t)(20 + d) * 4096 + i) + 1] = (int16_t)std::lround(256 * std::sin(ph));
    }
  nr_gpu_buf_h2d_async(&dt, 0, dt.bytes, nullptr);
  cudaDeviceSynchronize();
  nr_gpu_pass_cfg_t c = {4, 273, (const int16_t *)dt.dev, 4096, 20, 1}; /* v2 graphs (k_llr_stats): a superset of v1's outputs */
  if (m.pass_config(&c) != 0 || m.pass_init(16) != 0) return false;
  m.pass_state = 1;
  return true;
}
struct PassBufs { std::vector<uint8_t> g, s; };
TEST(GpuPassDl, GraphEqualsStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(m.pass_try) << "libpdsch_gpu.so has no nr_gpu_pass_dl_try";
  ASSERT_TRUE(pass_ready(m)) << "nr_gpu_pass_config/init failed";
  int n_ok = 0, n_fail = 0, n = 0;
  for (auto shape : GpuPassFixture::warm_shapes()) { // 24/48/96/217 PRB x Nl 1..4 x Qm 2..8
    GpuPassFixture fx = GpuPassFixture::from_shape(shape, 11);
    ASSERT_TRUE(fx.tb_ok_built) << "no TB for nrb " << shape.nrb_class << " nl " << shape.nl << " qm " << shape.qm;
    fx.bind();
    std::vector<uint8_t> gb(fx.tb.A / 8 + 64, 0xAA), sb(fx.tb.A / 8 + 64, 0x55);
    nr_gpu_pass_result_t g{}, s{};
    g.tb = gb.data(); s.tb = sb.data();
    ASSERT_EQ(0, m.pass_try(fx.job, &g)) << "nrb " << shape.nrb_class << " nl " << shape.nl << " qm " << shape.qm;
    ASSERT_EQ(0, fx.run_stage_path(&s)); // Task 2 path + CPU CRC
    n++;
    const std::string what = "nrb " + std::to_string(shape.nrb_class) + " nl " + std::to_string(shape.nl) + " qm " + std::to_string(shape.qm) + " A " + std::to_string(fx.tb.A) +
                             " C " + std::to_string(fx.tb.C) + " BG " + std::to_string(fx.tb.BG);
    EXPECT_EQ(s.need_cpu, g.need_cpu) << what;
    EXPECT_EQ(s.tb_ok, g.tb_ok) << what;
    EXPECT_EQ(s.nvar, g.nvar) << "chest post nvar vs the CPU normalisation, " << what;
    EXPECT_EQ(0, memcmp(s.tb, g.tb, g.tbs / 8)) << what;
    EXPECT_EQ(0, memcmp(s.seg_ok, g.seg_ok, sizeof(s.seg_ok))) << what;
    if (g.tb_ok) {
      EXPECT_EQ(0, memcmp(g.tb, fx.tb.payload.data(), fx.tb.A / 8)) << "decoded TB differs from the transmitted one, " << what;
      n_ok++;
    } else n_fail++;
    /* the exact FEP of the caller's rxdataF (DIRECT: written in place; DISCRETE: copied back) equals the stage path's */
    for (int a = 0; a < 4; a++)
      for (int sy = 0; sy < 14; sy++)
        ASSERT_EQ(0, memcmp(&fx.rxF_out[2 * ((size_t)a * GpuPassFixture::RXSTRIDE + (size_t)sy * 4096)], &fx.rxF_ref[2 * ((size_t)a * 14 * 4096 + (size_t)sy * 4096)], 4096 * 4))
            << "rxdataF of the pass, ant " << a << " sym " << sy << ", " << what;
  }
  printf("GpuPassDl.GraphEqualsStagePath: %d shapes, decoded %d, CRC fail on both paths %d\n", n, n_ok, n_fail);
  EXPECT_GE(n_ok, n / 2) << "the synthetic channel should decode most shapes (rank 4 and 256QAM excepted)";
  char buf[512];
  m.pass_stats(buf, sizeof(buf));
  printf("%s\n", buf);
  uint64_t o[16];
  m.pass_counters(o);
  EXPECT_EQ((uint64_t)n, o[1]) << "every shape was a graph hit";
  EXPECT_EQ(0u, o[2] + o[3] + o[4] + o[5]) << "no fallback";
}
/* ===================================================== Task 4b: segmented (PRG / VRB-bundle) grants ===================================================== */
/* The data-ordered PRB list of a segmented grant and its segments, as the passive body builds them (nr_vrb_to_prb_interleaved + nr_prb_segments): interleave = bundle size L
 * (0 = a contiguous run at `start`), prg = the precoding granularity in PRBs (0 = none), bwp = the BWP size the interleaver works in. */
static std::vector<GpuPassFixture::SegDef> seg_layout(int n, int start, int prg, int interleave, int bwp)
{
  std::vector<int> prb;
  if (!interleave) {
    for (int i = 0; i < n; i++) prb.push_back(start + i);
  } else { /* 38.211 7.3.1.6 (BWP start 0, VRB start `start`) */
    const int L = interleave, nb = (bwp + L - 1) / L, C = nb / 2;
    for (int i = 0; i < n; i++) {
      const int v = start + i, j = v / L, f = (j == nb - 1) ? j : (j % 2) * C + j / 2;
      prb.push_back(f * L + (v - j * L));
    }
  }
  std::vector<GpuPassFixture::SegDef> sg;
  for (int i = 0; i < n; i++) {
    const bool cont = !sg.empty() && prb[(size_t)i] == prb[(size_t)i - 1] + 1 && !(prg > 0 && prb[(size_t)i] % prg == 0);
    if (cont) sg.back().n_prb++;
    else sg.push_back({prb[(size_t)i], 1, i});
  }
  return sg;
}
struct SegCase { const char *name; int nl, qm, n, start, prg, interleave, bwp; int bwp_start = 0, ref_point = 0; };
static uint64_t hash64(const void *p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 1099511628211ull; return h; }
/* The segmented graph pass equals the old segmented path for the same grant (Task 2 stages as the passive body runs them for a segmented grant: the chest per segment into scratch
 * rows, each estimate parked at its data offset, the width-weighted nvar, the demod against the gathered virtual allocation, then the CPU steps): TB bytes, CRC verdict, nvar and the
 * per-antenna nvar, the layer-0 estimate rows (every segment's chest result incl. its delay compensation, laid out in data order) and, on a CRC failure, the LLRs the retry arms get. */
TEST(GpuPassDl, SegmentedGraphEqualsStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(m.pass_try && m.pass_warm) << "libpdsch_gpu.so has no nr_gpu_pass_dl_try / nr_gpu_pass_warm";
  ASSERT_TRUE(pass_ready(m)) << "nr_gpu_pass_config/init failed";
  const SegCase cases[] = {
      {"prg2 4 segs rank1", 1, 4, 8, 0, 2, 0, 0},           // 4 x 2 PRB
      {"prg4 aligned 4 segs rank3", 3, 2, 16, 0, 4, 0, 0},  // 4 x 4 PRB
      {"prg2 offset 9 segs rank2", 2, 4, 16, 1, 2, 0, 0},   // 1 + 7 x 2 + 1 PRB: class 16
      {"prg4 offset rank4", 4, 2, 21, 2, 4, 0, 0},          // 2 + 4 x 4 + 3 PRB
      {"interleaved L2 8 segs rank2", 2, 6, 16, 0, 0, 2, 40}, // VRB bundles of 2 scattered over a 40 PRB BWP
      {"interleaved L4 + prg2 rank1", 1, 4, 12, 4, 2, 4, 48},
      {"interleaved L4 rank4", 4, 2, 16, 0, 0, 4, 64},
      {"one segment, prg set rank1", 1, 4, 3, 5, 8, 0, 0},   // nseg 1: segmented class 1 (scratch row + compose)
      {"two segments, nseg class 2 rank2", 2, 4, 13, 3, 8, 0, 0}, // 3..7 | 8..15
      {"BWPStart 12, refPoint 0, prg4 rank2", 2, 4, 12, 1, 4, 0, 0, 12, 0},
      {"BWPStart 20, refPoint 1 interleaved rank1", 1, 4, 12, 0, 0, 2, 32, 20, 1},
  };
  int n = 0, hit_cls[5] = {0, 0, 0, 0, 0}, decoded = 0;
  for (const SegCase &c : cases) {
    const auto sd = seg_layout(c.n, c.start, c.prg, c.interleave, c.bwp);
    const int nsegc = nr_gpu_pass_nseg_class((int)sd.size());
    ASSERT_GT(nsegc, 0) << c.name;
    for (int corrupt : {-1, 0}) { // 0: every coded bit of every block corrupted w.p. 0.3 -> a CRC failure whose LLRs the retry arms get
      nr_gpu_pass_shape_t sh = {(uint16_t)c.n, 13, (uint16_t)c.nl, (uint16_t)c.qm, 2, 1, 1, (uint16_t)nsegc};
      GpuPassFixture fx = GpuPassFixture::from_shape(sh, 17 + n, corrupt, &sd, c.bwp_start, c.ref_point);
      ASSERT_TRUE(fx.tb_ok_built) << c.name;
      fx.shape.ncb_class = (uint16_t)nr_gpu_pass_ncb_class(fx.tb.C);
      fx.shape.nrb_class = (uint16_t)nr_gpu_pass_nrb_class(c.n);
      fx.bind();
      nr_gpu_pass_shape_t w = fx.shape;
      ASSERT_EQ(0, m.pass_warm(&w, c.nl >= 2 ? 3 : 0, 0, 0)) << c.name;
      std::vector<uint8_t> gb(fx.tb.A / 8 + 64, 0xAA), sb(fx.tb.A / 8 + 64, 0x55);
      std::vector<int16_t> brfo((size_t)4 * 2 * 4096 * 2, 0x7e);
      nr_gpu_pass_result_t g{}, s{};
      g.tb = gb.data(); s.tb = sb.data();
      g.brfo_rows = brfo.data();
      ASSERT_EQ(0, m.pass_try(fx.job, &g)) << c.name << " corrupt " << corrupt;
      ASSERT_EQ(0, fx.run_stage_path(&s));
      const std::string what = std::string(c.name) + " nseg " + std::to_string(sd.size()) + " class " + std::to_string(nsegc) + " corrupt " + std::to_string(corrupt) + " A " +
                               std::to_string(fx.tb.A) + " C " + std::to_string(fx.tb.C);
      EXPECT_EQ(s.need_cpu, g.need_cpu) << what;
      EXPECT_EQ(s.tb_ok, g.tb_ok) << what;
      EXPECT_EQ(s.nvar, g.nvar) << "width-weighted chest nvar through the post kernel vs the passive loop's combination, " << what;
      EXPECT_EQ(0, memcmp(s.nvar_ant, g.nvar_ant, sizeof(s.nvar_ant))) << "per-antenna nvar (nr_dl_chest_nvar_ant), " << what;
      EXPECT_EQ(0, memcmp(s.seg_ok, g.seg_ok, sizeof(s.seg_ok))) << what;
      if (!g.need_cpu) EXPECT_EQ(0, memcmp(s.tb, g.tb, g.tbs / 8)) << what;
      EXPECT_EQ(0, memcmp(brfo.data(), fx.est_l0.data(), brfo.size() * 2)) << "layer-0 estimates in data order, " << what;
      if (g.tb_ok) {
        EXPECT_EQ(0, memcmp(g.tb, fx.tb.payload.data(), fx.tb.A / 8)) << "decoded TB differs from the transmitted one, " << what;
        decoded++;
      } else if (!g.need_cpu) {
        EXPECT_EQ(hash64(fx.llr_pass.data(), (size_t)fx.tb.G * 2), hash64(fx.llr_ref.data(), (size_t)fx.tb.G * 2)) << "LLR hash of the failed first body, " << what;
        EXPECT_EQ(0, memcmp(fx.llr_pass.data(), fx.llr_ref.data(), (size_t)fx.tb.G * 2)) << what;
      }
      for (int a = 0; a < 4; a++)
        for (int sy = 0; sy < 14; sy++)
          ASSERT_EQ(0, memcmp(&fx.rxF_out[2 * ((size_t)a * GpuPassFixture::RXSTRIDE + (size_t)sy * 4096)], &fx.rxF_ref[2 * ((size_t)a * 14 * 4096 + (size_t)sy * 4096)], 4096 * 4))
              << "rxdataF of the pass, ant " << a << " sym " << sy << ", " << what;
      n++;
      hit_cls[__builtin_ctz((unsigned)nsegc)]++;
    }
  }
  printf("GpuPassDl.SegmentedGraphEqualsStagePath: %d grants (classes 1/2/4/8/16: %d/%d/%d/%d/%d), decoded %d\n", n, hit_cls[0], hit_cls[1], hit_cls[2], hit_cls[3], hit_cls[4], decoded);
  EXPECT_GE(decoded, 7) << "the clean grants should decode (rank 4 excepted)";
  for (int i = 0; i < 5; i++) EXPECT_GT(hit_cls[i], 0) << "segment-count class " << (1 << i) << " not exercised";
  uint64_t o[16];
  m.pass_counters(o);
  EXPECT_GE(o[12], (uint64_t)1) << "segmented launches counted";
}
/* CRC failure and the stop rule: the coded bits of the code blocks from CB 1 on are corrupted, so CB 0 decodes and the rest cannot. The graph and the stage path agree
 * on every verdict (the first-passing-snapshot rule, the abort of the later blocks, the zero-syndrome CPU hand-over) and on the bytes they leave. */
TEST(GpuPassDl, CrcFailAgreesWithStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(m.pass_try) << "libpdsch_gpu.so has no nr_gpu_pass_dl_try";
  ASSERT_TRUE(pass_ready(m));
  int n = 0, partial = 0, cpu = 0;
  for (auto shape : GpuPassFixture::warm_shapes()) {
    if (shape.qm != 4 || shape.nl > 2) continue;
    GpuPassFixture fx = GpuPassFixture::from_shape(shape, 31, /*corrupt_from_cb=*/1);
    ASSERT_TRUE(fx.tb_ok_built);
    if (fx.tb.C < 2) continue; // needs a block to corrupt
    fx.bind();
    std::vector<uint8_t> gb(fx.tb.A / 8 + 64, 0xAA), sb(fx.tb.A / 8 + 64, 0xAA);
    nr_gpu_pass_result_t g{}, s{};
    g.tb = gb.data(); s.tb = sb.data();
    ASSERT_EQ(0, m.pass_try(fx.job, &g));
    ASSERT_EQ(0, fx.run_stage_path(&s));
    const std::string what = "nrb " + std::to_string(shape.nrb_class) + " nl " + std::to_string(shape.nl) + " C " + std::to_string(fx.tb.C);
    n++;
    EXPECT_EQ(s.tb_ok, g.tb_ok) << what;
    EXPECT_EQ(0, g.tb_ok) << "a corrupted TB must not pass, " << what;
    EXPECT_EQ(s.need_cpu, g.need_cpu) << what;
    EXPECT_EQ(0, memcmp(s.seg_ok, g.seg_ok, sizeof(s.seg_ok))) << what;
    if (!g.need_cpu) EXPECT_EQ(0, memcmp(s.tb, g.tb, g.tbs / 8)) << what;
    partial += g.seg_ok[0] && !g.seg_ok[1];
    cpu += g.need_cpu;
  }
  printf("GpuPassDl.CrcFailAgreesWithStagePath: %d shapes, CB0 ok and CB1 failed in %d, CPU hand-over in %d\n", n, partial, cpu);
  EXPECT_GE(n, 4);
}
/* Concurrency model: 4 instances; 8 threads each running their own grant 40 times. Every launch either decodes the TB it was given or is refused with 1 (counted busy); no
 * launch may return another thread's bytes (the instances' buffers, staging slots and contexts are private). */
/* Every instance, one at a time (the others held busy), decodes the TB of every warm shape: the sequential test above only ever runs on instance 0. */
TEST(GpuPassDl, EveryInstanceDecodesEveryShape) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(m.pass_try && m.pass_hold) << "libpdsch_gpu.so has no nr_gpu_pass_dl_try";
  ASSERT_TRUE(pass_ready(m));
  constexpr unsigned dl_mask = (1u << NR_GPU_PASS_DL) - 1u;
  for (int inst = 0; inst < NR_GPU_PASS_DL; inst++) {
    m.pass_hold(dl_mask & ~(1u << inst), 1);
    int n = 0, bad = 0;
    for (auto shape : GpuPassFixture::warm_shapes()) {
      GpuPassFixture fx = GpuPassFixture::from_shape(shape, 11);
      ASSERT_TRUE(fx.tb_ok_built);
      fx.bind();
      std::vector<uint8_t> tb(fx.tb.A / 8 + 64);
      nr_gpu_pass_result_t r{};
      r.tb = tb.data();
      const int rc = m.pass_try(fx.job, &r);
      n++;
      if (rc != 0 || !r.tb_ok || memcmp(tb.data(), fx.tb.payload.data(), fx.tb.A / 8)) {
        if (bad++ < 3) fprintf(stderr, "instance %d: rc %d tb_ok %d need_cpu %d seg_ok0 %d: nrb %d nl %d qm %d C %d\n", inst, rc, r.tb_ok, r.need_cpu, r.seg_ok[0], shape.nrb_class, shape.nl, shape.qm, fx.tb.C);
      }
    }
    m.pass_hold(dl_mask, 0);
    EXPECT_EQ(0, bad) << "instance " << inst << ": " << bad << " of " << n << " shapes failed";
  }
}
TEST(GpuPassDl, ConcurrentPassesUseOwnInstances) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(m.pass_try) << "libpdsch_gpu.so has no nr_gpu_pass_dl_try";
  ASSERT_TRUE(pass_ready(m));
  const auto shapes = GpuPassFixture::warm_shapes();
  constexpr int NT = 8, NIT = 40;
  std::vector<GpuPassFixture> fx;
  for (int t = 0; t < NT; t++) { // a grant of this shape that decodes when run alone (the channel is random, a few seeds do not at 256QAM)
    for (int seed = 100 + t;; seed += 17) {
      GpuPassFixture f = GpuPassFixture::from_shape(shapes[(t * 5) % shapes.size()], seed);
      ASSERT_TRUE(f.tb_ok_built);
      f.bind();
      std::vector<uint8_t> tb(f.tb.A / 8 + 64);
      nr_gpu_pass_result_t r{};
      r.tb = tb.data();
      ASSERT_EQ(0, m.pass_try(f.job, &r));
      if (r.tb_ok) { fx.push_back(std::move(f)); break; }
      ASSERT_LT(seed, 100 + t + 17 * 20);
    }
  }
  for (auto &f : fx) f.bind();
  uint64_t o0[16], o1[16];
  m.pass_counters(o0);
  std::atomic<int> done{0}, busy{0}, bad{0};
  std::vector<std::thread> th;
  for (int t = 0; t < NT; t++)
    th.emplace_back([&, t] {
      GpuPassFixture &f = fx[t];
      std::vector<uint8_t> tb(f.tb.A / 8 + 64);
      for (int i = 0; i < NIT; i++) {
        nr_gpu_pass_result_t r{};
        r.tb = tb.data();
        const int rc = m.pass_try(f.job, &r);
        if (rc == 1) { busy++; continue; }
        if (rc != 0 || !r.tb_ok || memcmp(tb.data(), f.tb.payload.data(), f.tb.A / 8)) {
          if (bad++ < 3) fprintf(stderr, "thread %d it %d inst %d: rc %d tb_ok %d need_cpu %d seg_ok %d%d%d%d nvar %u gpu_us %u shape nrb %d nl %d C %d\n", t, i, r.instance, rc, r.tb_ok, r.need_cpu, r.seg_ok[0], r.seg_ok[1], r.seg_ok[2], r.seg_ok[3], r.nvar, r.gpu_us, f.shape.nrb_class, f.shape.nl, f.tb.C);
        }
        else done++;
      }
    });
  for (auto &x : th) x.join();
  m.pass_counters(o1);
  printf("GpuPassDl.ConcurrentPassesUseOwnInstances: %d decoded, %d refused busy (counter %lu)\n", done.load(), busy.load(), (unsigned long)(o1[3] - o0[3]));
  EXPECT_EQ(0, bad.load()) << "a launch returned wrong bytes or failed";
  EXPECT_EQ(NT * NIT, done.load() + busy.load());
  EXPECT_GT(done.load(), 0);
  EXPECT_EQ((uint64_t)busy.load(), o1[3] - o0[3]);
}
/* A CRC failure of the pass is the old first body (the controller ruling): the pass hands the caller the final LLRs the old path would have re-decoded in its retry
 * arms (descrambled, K38-normalised), and the per-block verdicts of the failed decode. Everything a retry arm of passive_decode_retry reads from the first body is
 * then in place; the body continues into the arms without a second decode (counted: GPUPASS cont_fail, old_first). The body-level A/B (final TB, CRC, which arm
 * rescued) is the replay smoke; here: LLRs and verdicts equal the stage path on forced failures. */
TEST(GpuPassDl, CrcFailContinuesWithRetriesWithoutRedo) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(m.pass_try) << "libpdsch_gpu.so has no nr_gpu_pass_dl_try";
  ASSERT_TRUE(pass_ready(m));
  int n = 0;
  for (auto shape : GpuPassFixture::warm_shapes()) {
    if (shape.qm != 6 || shape.nl > 2) continue;
    for (int from : {0, 1}) {
      GpuPassFixture fx = GpuPassFixture::from_shape(shape, 41, from);
      ASSERT_TRUE(fx.tb_ok_built);
      if (fx.tb.C <= from) continue;
      fx.bind();
      std::vector<uint8_t> gb(fx.tb.A / 8 + 64), sb(fx.tb.A / 8 + 64);
      nr_gpu_pass_result_t g{}, s{};
      g.tb = gb.data(); s.tb = sb.data();
      ASSERT_EQ(0, m.pass_try(fx.job, &g));
      ASSERT_EQ(0, fx.run_stage_path(&s));
      EXPECT_EQ(0, g.tb_ok);
      EXPECT_EQ(s.tb_ok, g.tb_ok);
      EXPECT_EQ(s.need_cpu, g.need_cpu);
      EXPECT_EQ(0, memcmp(s.seg_ok, g.seg_ok, sizeof(s.seg_ok)));
      if (!g.need_cpu) {
        ASSERT_GE(fx.llr_ref.size(), fx.tb.G);
        EXPECT_EQ(0, memcmp(fx.llr_ref.data(), fx.llr_pass.data(), (size_t)fx.tb.G * 2)) << "LLRs handed to the retry arms differ from the old first body's";
        n++;
      }
    }
  }
  EXPECT_GE(n, 4);
}
TEST(GpuPassDl, UnseenShapeFallsBack) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(m.pass_try) << "libpdsch_gpu.so has no nr_gpu_pass_dl_try";
  ASSERT_TRUE(pass_ready(m));
  GpuPassFixture fx = GpuPassFixture::from_shape({/*nrb*/ 273, 14, 4, 8, 3, 32, 0}, 3); // not warmed
  fx.bind();
  std::vector<uint8_t> tb(fx.tb.A / 8 + 64);
  nr_gpu_pass_result_t r{};
  r.tb = tb.data();
  uint64_t o0[16], o1[16];
  m.pass_counters(o0);
  EXPECT_EQ(1, m.pass_try(fx.job, &r));
  m.pass_counters(o1);
  EXPECT_EQ(o0[0], o1[0]) << "nothing launched";
  EXPECT_EQ(o0[2] + o0[4] + 1, o1[2] + o1[4]) << "counted as a fallback";
}
/* An EVM-carrying class: warmed explicitly, the pass returns the same TB as the class without the EVM branch and fills the EVM sums of the data symbols. */
TEST(GpuPassDl, EvmBranchGraph) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(m.pass_try && m.pass_warm) << "libpdsch_gpu.so has no nr_gpu_pass_dl_try";
  ASSERT_TRUE(pass_ready(m));
  const nr_gpu_pass_shape_t sh = GpuPassFixture::warm_shapes()[4 * 4 * 1 + 4 * 1 + 1]; // nrb 48, nl 2, qm 4
  ASSERT_EQ(0, m.pass_warm(&sh, 3, 0, /*evm=*/1));
  GpuPassFixture fx = GpuPassFixture::from_shape(sh, 5);
  ASSERT_TRUE(fx.tb_ok_built);
  fx.bind();
  std::vector<uint8_t> t0(fx.tb.A / 8 + 64), t1(fx.tb.A / 8 + 64);
  nr_gpu_pass_result_t r0{}, r1{};
  r0.tb = t0.data(); r1.tb = t1.data();
  ASSERT_EQ(0, m.pass_try(fx.job, &r0));
  std::unique_ptr<nr_gpu_demod_job_t> dout(new nr_gpu_demod_job_t());
  r1.demod_out = dout.get();
  fx.job->dj.want_evm = 1;
  fx.job->dj.want_kappa = 1;
  fx.job->dj.evm_fp64 = 0;
  ASSERT_EQ(0, m.pass_try(fx.job, &r1));
  EXPECT_EQ(r0.tb_ok, r1.tb_ok);
  EXPECT_EQ(0, memcmp(t0.data(), t1.data(), fx.tb.A / 8));
  EXPECT_EQ(r0.nvar, r1.nvar);
  int valid = 0;
  for (int sy : fx.data_syms) valid += dout->evm[sy].valid != 0;
  EXPECT_EQ((int)fx.data_syms.size(), valid) << "the EVM chain of the graph produced no sums";
}

/* Stage parity of the launch-only DL entry points: the same slot through FEP -> chest -> demod -> LDPC on one stream, once with the
 * buffers of the platform's own model and once forced DISCRETE (device ring + explicit copies). On GB10 the first run is DIRECT
 * (the kernels read the host IQ ring in place), on sens6 both are DISCRETE. Not compared against the OAI CPU chain: that needs the
 * PHY_NR_UE link set (see the Task 2 report); the end-to-end hard decisions are scored against the transmitted bits instead. */
static void dl_stages_case(int nrb, int nl, int qm, int seed, double max_ber) {
  GpuPassFixture fx = GpuPassFixture::small_grant(nrb, nl, qm, seed);
  std::vector<int16_t> llr_direct, llr_discrete;
  setenv("ISAC_GPU_FORCE_DISCRETE", "0", 1);
  ASSERT_EQ(0, fx.run_stages(&llr_direct));
  const auto rxF0 = fx.rxF_head; const auto chest0 = fx.chest_out; const auto snap0 = fx.snap; const int rc0 = fx.ldpc_rc;
  setenv("ISAC_GPU_FORCE_DISCRETE", "1", 1);
  ASSERT_EQ(0, fx.run_stages(&llr_discrete));
  unsetenv("ISAC_GPU_FORCE_DISCRETE");
  ASSERT_EQ(llr_direct.size(), llr_discrete.size());
  ASSERT_EQ(fx.tx_bits.size(), llr_direct.size());
  EXPECT_EQ(rxF0, fx.rxF_head) << "FEP rxdataF differs between the models";
  EXPECT_EQ(chest0, fx.chest_out) << "chest delays/nvar differ between the models";
  EXPECT_EQ(0, memcmp(llr_direct.data(), llr_discrete.data(), llr_direct.size() * 2)) << "demod LLRs differ between the models";
  EXPECT_EQ(rc0, fx.ldpc_rc);
  if (rc0 == 0) EXPECT_EQ(snap0, fx.snap) << "LDPC snapshots differ between the models";
  else ADD_FAILURE() << "LDPC refused (rc " << rc0 << "): below CC 9 the non-cluster kernels run, nothing may refuse";
  size_t err = 0;
  for (size_t i = 0; i < llr_direct.size(); i++) err += (llr_direct[i] < 0) != (fx.tx_bits[i] != 0);
  printf("nrb=%d nl=%d qm=%d: %zu LLRs, BER vs tx %.4f, LDPC rc %d nsnap %d, parity checked (model 1: %s)\n", nrb, nl, qm, llr_direct.size(),
         (double)err / llr_direct.size(), rc0, fx.ldpc_nsnap, nr_gpu_model() == NR_GPU_MODEL_DIRECT ? "DIRECT" : "DISCRETE");
  EXPECT_LT((double)err / llr_direct.size(), max_ber) << "chain does not demodulate the synthetic slot: " << err << " bit errors";
}
/* GB10 only: the Stage-1 bit-exact reference is today's GPU exact path. Same fixture inputs through the OLD entry points (own waits,
 * host buffers) and through the four _dev functions on the DIRECT model. */
static void dev_vs_old_case(int nrb, int nl, int qm, int seed) {
  GpuPassFixture fx = GpuPassFixture::small_grant(nrb, nl, qm, seed);
  std::vector<int16_t> llr_old, llr_dev;
  setenv("ISAC_GPU_FORCE_DISCRETE", "0", 1);
  ASSERT_EQ(0, fx.run_stages(&llr_old, true));
  const auto rxF0 = fx.rxF_head; const auto chest0 = fx.chest_out; const auto snap0 = fx.snap; const int rc0 = fx.ldpc_rc, ns0 = fx.ldpc_nsnap;
  ASSERT_EQ(0, fx.run_stages(&llr_dev, false));
  unsetenv("ISAC_GPU_FORCE_DISCRETE");
  EXPECT_EQ(rxF0, fx.rxF_head) << "rxdataF: old vs _dev";
  EXPECT_EQ(chest0, fx.chest_out) << "chest delay/nvar: old vs _dev";
  ASSERT_EQ(llr_old.size(), llr_dev.size());
  EXPECT_EQ(0, memcmp(llr_old.data(), llr_dev.data(), llr_old.size() * 2)) << "LLRs: old vs _dev";
  EXPECT_EQ(rc0, fx.ldpc_rc);
  EXPECT_EQ(ns0, fx.ldpc_nsnap);
  EXPECT_EQ(snap0, fx.snap) << "LDPC snapshots/syn0: old vs _dev";
}
TEST(GpuPassStages, DevEqualsOldEntryPoints) {
  if (!have_gpu()) GTEST_SKIP();
  bool ok = false, lib = false;
  GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  unsetenv("ISAC_GPU_FORCE_DISCRETE"); /* GpuMem.RoundTrip leaves it at 1 */
  if (nr_gpu_model() != NR_GPU_MODEL_DIRECT) GTEST_SKIP() << "old entry points need the DIRECT model (GB10)";
  dev_vs_old_case(24, 2, 4, 7);
  dev_vs_old_case(12, 1, 6, 9);
  dev_vs_old_case(24, 4, 2, 5);
}
/* GPU chest rows are independent: a job of NR_GPU_CHEST_MAX_ROWS rows equals the same rows run as consecutive smaller jobs, bit for bit
 * (nr_pdsch_gpu_chest_grant now runs jobs above the buffer size as chunks of it). */
TEST(GpuChestChunks, FullJobEqualsSplitJobs) {
  if (!have_gpu()) GTEST_SKIP();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  unsetenv("ISAC_GPU_FORCE_DISCRETE");
  if (nr_gpu_model() != NR_GPU_MODEL_DIRECT) GTEST_SKIP() << "old entry point needs the DIRECT model (GB10)";
  const int N = 4096, MDC = 20, R = NR_GPU_CHEST_MAX_ROWS;
  nr_gpu_buf_t rx, pil, dt, out[2];
  ASSERT_EQ(0, nr_gpu_buf_alloc(&rx, (size_t)8 * N * 4));
  ASSERT_EQ(0, nr_gpu_buf_alloc(&pil, (size_t)8 * 6 * 273 * 4));
  ASSERT_EQ(0, nr_gpu_buf_alloc(&dt, (size_t)(2 * MDC + 1) * N * 4));
  ASSERT_EQ(0, nr_gpu_buf_alloc(&out[0], (size_t)R * N * 4));
  ASSERT_EQ(0, nr_gpu_buf_alloc(&out[1], (size_t)R * N * 4));
  std::mt19937 g(7);
  for (size_t i = 0; i < rx.bytes / 2; i++) ((int16_t *)rx.host)[i] = (int16_t)((int)(g() % 4001) - 2000);
  for (size_t i = 0; i < pil.bytes / 2; i++) ((int16_t *)pil.host)[i] = (int16_t)((g() & 1 ? 1 : -1) * 11585);
  for (int d = -MDC; d <= MDC; d++)
    for (int i = 0; i < N; i++) {
      const double ph = 2.0 * M_PI * i * d / N;
      ((int16_t *)dt.host)[2 * ((size_t)(MDC + d) * N + i)] = (int16_t)std::lround(256 * std::cos(ph));
      ((int16_t *)dt.host)[2 * ((size_t)(MDC + d) * N + i) + 1] = (int16_t)std::lround(256 * std::sin(ph));
    }
  auto job = std::make_unique<nr_gpu_chest_job_t>();
  auto build = [&](int variant, int first, int n) { /* rows [first, first + n) of the R-row job, outputs into out[variant] at their own offsets */
    memset(job.get(), 0, sizeof(*job));
    job->type = 1; job->N = N; job->n_rows = n; job->max_delay_comp = MDC; job->dt_stride = N; job->delay_table = (const int16_t *)dt.dev;
    memcpy(job->filt[0], filt16_ul_p0, 32); memcpy(job->filt[1], filt16_ul_p1p2, 32);
    memcpy(job->filt[2], filt16_ul_middle, 32); memcpy(job->filt[3], filt16_ul_last, 32);
    for (int i = 0; i < n; i++) {
      const int r = first + i;
      job->rows[i].rxF = (const int16_t *)rx.dev + 2 * ((size_t)(r % 8) * N + (r % 3));
      job->rows[i].pil = (const int16_t *)pil.dev + 2 * (size_t)(r % 8) * 6 * 273;
      job->rows[i].out = (int16_t *)out[variant].dev + 2 * (size_t)r * N;
      job->rows[i].k0 = (1000 + 12 * (r % 40)) % N;
      job->rows[i].nb_rb = 1 + (r * 7) % 100;
    }
  };
  std::vector<int> nv[2], dly[2];
  const int cuts[3] = {0, R / 3, R}; /* full job, then two chunks with an odd split */
  build(0, 0, R);
  ASSERT_EQ(0, m.chest_old(job.get()));
  nv[0].assign(job->nvar, job->nvar + R);
  dly[0].assign(&job->delay[0][0], &job->delay[0][0] + 3 * R);
  for (int c = 0; c < 2; c++) {
    build(1, cuts[c], cuts[c + 1] - cuts[c]);
    ASSERT_EQ(0, m.chest_old(job.get()));
    for (int i = 0; i < cuts[c + 1] - cuts[c]; i++) nv[1].push_back((int)job->nvar[i]);
    for (int i = 0; i < 3 * (cuts[c + 1] - cuts[c]); i++) dly[1].push_back(((int *)job->delay)[i]);
  }
  EXPECT_EQ(nv[0], nv[1]);
  EXPECT_EQ(dly[0], dly[1]);
  EXPECT_EQ(0, memcmp(out[0].host, out[1].host, (size_t)R * N * 4));
  nr_gpu_buf_free(&rx); nr_gpu_buf_free(&pil); nr_gpu_buf_free(&dt); nr_gpu_buf_free(&out[0]); nr_gpu_buf_free(&out[1]);
}

TEST(GpuPassStages, DlStagesSameBitsBothModels) {
  if (!have_gpu()) GTEST_SKIP();
  bool ok = false, lib = false;
  GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok) << "libpdsch_gpu.so lacks the _dev entry points or has no device";
  dl_stages_case(/*nrb=*/24, /*nl=*/2, /*qm=*/4, /*seed=*/7, 0.02);
  dl_stages_case(12, 1, 6, 9, 0.02);
  dl_stages_case(24, 4, 2, 5, 0.15); // rank 4 over a 4-RX channel is ill-conditioned (nr_pdsch_gpu_fep_test): sanity only, parity is the check
}

/* The four _dev calls of one (fep slot, chest slot) captured into a CUDA graph (Relaxed: the module calls cudaHostGetDevicePointer inside),
 * instantiated before any fence; then for three different inputs: stage on the host, replay, and compare with a plain (not captured) run of
 * the same inputs on the same buffers. Both models. */
static void graph_case(int force) {
  setenv("ISAC_GPU_FORCE_DISCRETE", force ? "1" : "0", 1);
  bool ok = false;
  GpuPassModule &m = GpuPassModule::get(&ok);
  ASSERT_TRUE(ok);
  GpuPassFixture in[3] = {GpuPassFixture::small_grant(24, 2, 4, 11), GpuPassFixture::small_grant(24, 2, 4, 12), GpuPassFixture::small_grant(24, 2, 4, 13)};
  GpuPassFixture::Pass P;
  ASSERT_TRUE(P.setup(m, in[0], /*nvar_fixed=*/400));
  std::map<std::pair<int, int>, cudaGraphExec_t> ge;
  GpuPassFixture::Out ref[3], rep[3];
  int rc_ref = 0, rc_rep = 0;
  for (int pass = 0; pass < 2; pass++) /* 0: plain reference of all three inputs, 1: graph replays */
    for (int i = 0; i < 3; i++) {
      ASSERT_TRUE(P.load(in[i]));
      int fs, cs, rc = 0;
      ASSERT_EQ(0, P.stage(&fs, &cs));
      if (pass == 0) {
        ASSERT_EQ(0, P.enqueue_dev(fs, cs, P.s, &rc));
        rc_ref = rc;
      } else {
        auto key = std::make_pair(fs, cs);
        if (!ge.count(key)) {
          cudaGraph_t g;
          ASSERT_EQ(cudaSuccess, cudaStreamBeginCapture(P.s, cudaStreamCaptureModeRelaxed));
          const int er = P.enqueue_dev(fs, cs, P.s, &rc);
          ASSERT_EQ(cudaSuccess, cudaStreamEndCapture(P.s, &g));
          ASSERT_EQ(0, er);
          ASSERT_EQ(cudaSuccess, cudaGraphInstantiate(&ge[key], g, 0));
          cudaGraphDestroy(g);
        } else
          rc = rc_ref;
        ASSERT_EQ(cudaSuccess, cudaGraphLaunch(ge[key], P.s));
        rc_rep = rc;
        EXPECT_EQ(rc_ref, rc_rep);
      }
      ASSERT_TRUE(P.fetch(pass == 0 ? &ref[i] : &rep[i], pass == 0 ? rc : rc_ref));
    }
  for (int i = 0; i < 3; i++) {
    EXPECT_TRUE(ref[i] == rep[i]) << "replayed graph differs from the plain run, input " << i << " force=" << force;
    EXPECT_FALSE(ref[i].llr == ref[(i + 1) % 3].llr) << "inputs should differ";
  }
  for (auto &e : ge) cudaGraphExecDestroy(e.second);
}
TEST(GpuPassStages, DevCapturableAndReplayable) {
  if (!have_gpu()) GTEST_SKIP();
  bool ok = false, lib = false;
  GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  graph_case(0);
  graph_case(1);
  unsetenv("ISAC_GPU_FORCE_DISCRETE");
}


/* ---- Task 3: the host steps between the stages as kernels, each against the CPU function it replaces (OAI crc_byte.c, nr_rate_matching.c) ---- */
extern "C" {
void crcTableInit(void);
int check_crc(uint8_t *decoded_bytes, uint32_t n, uint8_t crc_type);
uint32_t crc24a(unsigned char *inptr, int bitlen);
uint32_t crc24b(unsigned char *inptr, int bitlen);
uint32_t crc16(unsigned char *inptr, int bitlen);
void nr_deinterleaving_ldpc(uint32_t E, uint8_t Qm, int16_t *e, int16_t *f);
int nr_rate_matching_ldpc_rx(uint32_t Tbslbrm, uint8_t BG, uint16_t Z, int16_t *d, int16_t *soft_input, uint8_t C, uint8_t rvidx, uint8_t clear,
                             uint32_t E, uint32_t F, uint32_t Foffset);
}
struct PostSyms {
  nr_gpu_pass_chest_post_fn chest = nullptr;
  nr_gpu_pass_chest_post64_fn chest64 = nullptr;
  nr_gpu_pass_rdm_fn rdm = nullptr;
  nr_gpu_pass_crc_fn crc = nullptr;
  static PostSyms &get() {
    static PostSyms p;
    static bool tried = false;
    if (!tried) {
      tried = true;
      void *dl = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
      if (dl) {
        p.chest = (nr_gpu_pass_chest_post_fn)dlsym(dl, "nr_gpu_pass_launch_chest_post");
        p.chest64 = (nr_gpu_pass_chest_post64_fn)dlsym(dl, "nr_gpu_pass_launch_chest_post_f64");
        p.rdm = (nr_gpu_pass_rdm_fn)dlsym(dl, "nr_gpu_pass_launch_rdm");
        p.crc = (nr_gpu_pass_crc_fn)dlsym(dl, "nr_gpu_pass_launch_crc");
      }
    }
    return p;
  }
};
/* device scratch: cudaMalloc'd once per test, copied synchronously (the kernels under test are the only thing launched on the stream) */
template <class T> struct Dev {
  T *p = nullptr; size_t n = 0;
  ~Dev() { cudaFree(p); }
  void up(const void *h, size_t bytes) { if (bytes > n) { cudaFree(p); cudaMalloc((void **)&p, bytes); n = bytes; } cudaMemcpy(p, h, bytes, cudaMemcpyHostToDevice); }
  void down(void *h, size_t bytes) { cudaMemcpy(h, p, bytes, cudaMemcpyDeviceToHost); }
};

/* one TB: random payload, CRC24A/16, split in C code blocks of pay bytes + CRC24B (valid, from the CPU crc functions); 10 % of the cases get bit flips */
struct CrcCase {
  uint32_t tbs, C, pay, nbits_cb;
  uint8_t tbtype;
  std::vector<uint8_t> bits;                  /* C blocks of ceil(nbits_cb/8) bytes */
  std::vector<nr_gpu_crc_seg_t> segs;
  static CrcCase random(int seed, bool unaligned) {
    std::mt19937 g(seed * 7919u + (unaligned ? 1 : 0));
    CrcCase c;
    c.tbs = 8 * (1 + g() % (g() % 4 == 0 ? 20000 : 480));          /* up to 160 kbit, mostly small */
    const uint32_t B = c.tbs + (c.tbs > 3824 ? 24 : 16);
    c.tbtype = c.tbs > 3824 ? 0 : 2;
    c.C = B <= 8448 ? 1 : (B + 8423) / 8424;
    std::vector<uint8_t> tb((B + 7) / 8 + 8);
    for (uint32_t i = 0; i < c.tbs / 8; i++) tb[i] = g();
    const uint32_t cr = c.tbtype == 0 ? crc24a(tb.data(), c.tbs) >> 8 : crc16(tb.data(), c.tbs) >> 16;
    const int cl = c.tbtype == 0 ? 3 : 2;
    for (int i = 0; i < cl; i++) tb[c.tbs / 8 + i] = (cr >> (8 * (cl - 1 - i))) & 255;
    const uint32_t Bb = B / 8;
    c.pay = c.C == 1 ? Bb : (Bb + c.C - 1) / c.C;
    c.nbits_cb = c.C == 1 ? B : (c.pay + 3) * 8;
    if (unaligned && c.C > 1) c.nbits_cb += g() % 7 + 1; /* check_crc on a bit length that is no whole number of bytes */
    const uint32_t stride = (c.nbits_cb + 7) / 8 + 4;
    c.bits.assign((size_t)c.C * stride, 0);
    for (uint32_t r = 0; r < c.C; r++) {
      uint8_t *b = &c.bits[(size_t)r * stride];
      for (uint32_t i = 0; i < c.pay; i++) b[i] = (r * c.pay + i) < tb.size() && r * c.pay + i < Bb ? tb[r * c.pay + i] : (uint8_t)g();
      if (c.C > 1) {
        const uint32_t v = crc24b(b, c.pay * 8) >> 8;
        for (int i = 0; i < 3; i++) b[c.pay + i] = (v >> (8 * (2 - i))) & 255;
      }
      c.segs.push_back({(uint32_t)(r * stride), c.nbits_cb, c.pay, c.C == 1 ? c.tbtype : 1u});
    }
    if (g() % 10 == 0) { /* corrupt: a few bit flips anywhere in the code blocks (payload, CRC or both) */
      for (int f = 1 + g() % 3; f > 0; f--) {
        const uint32_t r = g() % c.C;
        c.bits[(size_t)r * stride + g() % ((c.nbits_cb + 7) / 8)] ^= 1u << (g() % 8);
      }
    }
    return c;
  }
  int cpu_seg_ok(int r) {
    return check_crc(&bits[segs[r].off], segs[r].n_bits, (uint8_t)segs[r].type);
  }
  int cpu_tb_ok() {
    if (C == 1) return cpu_seg_ok(0);
    std::vector<uint8_t> b(C * pay + 8, 0);
    for (uint32_t r = 0; r < C; r++) memcpy(&b[(size_t)r * pay], &bits[segs[r].off], pay);
    return check_crc(b.data(), tbs + (tbs > 3824 ? 24 : 16), tbtype);
  }
};
TEST(GpuPassPost, CrcMatchesCpu) {
  if (!have_gpu()) GTEST_SKIP();
  PostSyms &S = PostSyms::get();
  ASSERT_TRUE(S.crc) << "libpdsch_gpu.so has no nr_gpu_pass_launch_crc";
  crcTableInit();
  Dev<uint8_t> dbits, dok;
  Dev<nr_gpu_crc_seg_t> dsegs;
  int nbad_cpu = 0;
  for (int seed = 0; seed < 1000; seed++) {
    CrcCase c = CrcCase::random(seed, seed % 5 == 4);
    dbits.up(c.bits.data(), c.bits.size());
    dsegs.up(c.segs.data(), c.segs.size() * sizeof(c.segs[0]));
    dok.up(std::vector<uint8_t>(c.C + 1, 7).data(), c.C + 1);
    ASSERT_EQ(0, S.crc(dbits.p, dsegs.p, (int)c.C, c.tbs, dok.p, dok.p + c.C, nullptr));
    std::vector<uint8_t> ok(c.C + 1);
    dok.down(ok.data(), c.C + 1);
    for (uint32_t r = 0; r < c.C; r++) {
      EXPECT_EQ(c.cpu_seg_ok(r), (int)ok[r]) << "seed " << seed << " seg " << r;
      nbad_cpu += !c.cpu_seg_ok(r);
    }
    EXPECT_EQ(c.cpu_tb_ok(), (int)ok[c.C]) << "seed " << seed;
    if (::testing::Test::HasFailure()) break;
  }
  EXPECT_GT(nbad_cpu, 20) << "the corrupted cases must exist";
}

/* nr_rate_matching_ldpc_rx path of prep_segment, transcribed (the host function is static): same calls, same pack */
static int cpu_prep(const nr_gpu_rdm_seg_t &s, const int16_t *llr, int8_t *l) {
  const int Kc = s.BG == 2 ? 52 : 68, Z = s.Z;
  std::vector<int16_t> e(s.E + 16), d((size_t)Kc * Z + 16, 0);
  { /* the two refusals of nr_rate_matching_ldpc_rx that call LOG_E (no logger in this test): same condition, not called */
    const uint32_t N = s.BG == 1 ? 66 * Z : 50 * Z, Ncb = s.Tbslbrm == 0 ? N : std::min(N, 3 * s.Tbslbrm / (2 * s.C));
    if (s.K - s.F - 2 * Z > Ncb) return -1;
  }
  nr_deinterleaving_ldpc(s.E, s.Qm, e.data(), const_cast<int16_t *>(llr + s.llr_off));
  if (nr_rate_matching_ldpc_rx(s.Tbslbrm, s.BG, Z, d.data(), e.data(), s.C, s.rv, 1, s.E, s.F, s.K - s.F - 2 * Z) == -1) return -1;
  std::vector<int16_t> z((size_t)Kc * Z + 64, 0);
  memset(z.data() + s.Kprime, 127, s.F * sizeof(int16_t));
  memcpy(z.data() + 2 * Z, d.data(), (s.Kprime - 2 * Z) * sizeof(int16_t));
  memcpy(z.data() + s.K, d.data() + (s.K - 2 * Z), (Kc * Z - s.K) * sizeof(int16_t));
  for (int i = 0; i < Kc * Z; i++) l[i] = (int8_t)(z[i] > 127 ? 127 : z[i] < -128 ? -128 : z[i]);
  return 0;
}
TEST(GpuPassPost, RateDematchMatchesCpu) {
  if (!have_gpu()) GTEST_SKIP();
  PostSyms &S = PostSyms::get();
  ASSERT_TRUE(S.rdm) << "libpdsch_gpu.so has no nr_gpu_pass_launch_rdm";
  static const int zs[] = {2, 7, 11, 16, 22, 36, 64, 96, 128, 176, 208, 256, 320, 384};
  Dev<int16_t> dllr;
  Dev<nr_gpu_rdm_seg_t> dsegs;
  Dev<int8_t> dout;
  int n_rej = 0, n_rep = 0, n_lbrm = 0;
  for (int seed = 0; seed < 1000; seed++) {
    std::mt19937 g(seed * 104729u + 5);
    const int nseg = 1 + g() % 3;
    std::vector<nr_gpu_rdm_seg_t> segs(nseg);
    std::vector<int16_t> llr;
    for (auto &s : segs) {
      s.BG = 1 + g() % 2; s.Z = zs[g() % 14];
      const int Kb = s.BG == 1 ? 22 : 10, Kc = s.BG == 1 ? 68 : 52, N = (s.BG == 1 ? 66 : 50) * s.Z;
      s.K = Kb * s.Z;
      s.F = g() % 3 == 0 ? 0 : g() % (s.K / 4 + 1);
      s.Kprime = s.K - s.F;
      s.C = 1 + g() % 40;
      s.rv = g() % 4;
      s.Qm = 2 * (1 + g() % 4);
      const uint32_t Ncb = g() % 3 ? N : (uint32_t)(s.K / 2 + g() % (N - s.K / 2) + 1); /* LBRM binds in a third of the cases */
      s.Tbslbrm = Ncb == (uint32_t)N ? (g() % 2 ? 0 : 1000000) : (uint32_t)((uint64_t)Ncb * 2 * s.C / 3 + 1);
      s.E = s.Qm * (1 + g() % ((g() % 4 == 0 ? 3 * N : N) / s.Qm)); /* up to 3 N: repetition */
      s.llr_off = (uint32_t)llr.size();
      const int amp = g() % 3 == 0 ? 32767 : g() % 2 ? 60 : 3000; /* full range wraps the int16 sum, small stays unsaturated */
      for (uint32_t i = 0; i < s.E; i++) llr.push_back((int16_t)((int)(g() % (2 * amp + 1)) - amp));
      (void)Kc;
    }
    dllr.up(llr.data(), llr.size() * 2);
    dsegs.up(segs.data(), nseg * sizeof(segs[0]));
    const size_t ob = (size_t)nseg * NR_GPU_LDPC_DEV_IN_STRIDE;
    dout.up(std::vector<int8_t>(ob, 55).data(), ob);
    ASSERT_EQ(0, S.rdm(dllr.p, dsegs.p, nseg, dout.p, nullptr));
    std::vector<int8_t> got(ob);
    dout.down(got.data(), ob);
    for (int i = 0; i < nseg; i++) {
      const int Kc = segs[i].BG == 2 ? 52 : 68;
      std::vector<int8_t> ref((size_t)Kc * segs[i].Z + 16, 0);
      const int rc = cpu_prep(segs[i], llr.data(), ref.data());
      if (rc != 0) { std::fill(ref.begin(), ref.end(), 0); n_rej++; }
      n_rep += segs[i].E > (segs[i].BG == 1 ? 66 : 50) * segs[i].Z;
      n_lbrm += segs[i].Tbslbrm != 0 && segs[i].Tbslbrm != 1000000;
      ASSERT_EQ(0, memcmp(ref.data(), &got[(size_t)i * NR_GPU_LDPC_DEV_IN_STRIDE], (size_t)Kc * segs[i].Z))
          << "seed " << seed << " seg " << i << " BG" << segs[i].BG << " Z" << segs[i].Z << " Qm" << segs[i].Qm << " rv" << segs[i].rv << " E" << segs[i].E
          << " F" << segs[i].F << " Tbslbrm " << segs[i].Tbslbrm << " C" << segs[i].C;
    }
  }
  printf("rate dematch: %d CPU-refused segments, %d with repetition, %d with LBRM\n", n_rej, n_rep, n_lbrm);
  EXPECT_GT(n_rep, 50);
  EXPECT_GT(n_lbrm, 50);
}

/* ---- DL HARQ soft combining on the device: k_rdm with hq_mode 1 (first transmission, clear = 1, the sum is stored) and 2 (retransmission, clear = 0, the stored sum is added to) against
 * nr_rate_matching_ldpc_rx on a persistent d buffer, the CPU path's combine (clear flag of d_to_be_cleared). Both the int8 LDPC input and the stored int16 circular buffer are compared bit for bit. */
static int cpu_prep_hq(const nr_gpu_rdm_seg_t &s, const int16_t *llr, int8_t *l, std::vector<int16_t> &d, int clear) {
  const int Kc = s.BG == 2 ? 52 : 68, Z = s.Z;
  std::vector<int16_t> e(s.E + 16);
  {
    const uint32_t N = s.BG == 1 ? 66 * Z : 50 * Z, Ncb = s.Tbslbrm == 0 ? N : std::min(N, 3 * s.Tbslbrm / (2 * s.C));
    if (s.K - s.F - 2 * Z > Ncb) return -1;
  }
  nr_deinterleaving_ldpc(s.E, s.Qm, e.data(), const_cast<int16_t *>(llr + s.llr_off));
  if (nr_rate_matching_ldpc_rx(s.Tbslbrm, s.BG, Z, d.data(), e.data(), s.C, s.rv, clear, s.E, s.F, s.K - s.F - 2 * Z) == -1) return -1;
  std::vector<int16_t> z((size_t)Kc * Z + 64, 0);
  memset(z.data() + s.Kprime, 127, s.F * sizeof(int16_t));
  memcpy(z.data() + 2 * Z, d.data(), (s.Kprime - 2 * Z) * sizeof(int16_t));
  memcpy(z.data() + s.K, d.data() + (s.K - 2 * Z), (Kc * Z - s.K) * sizeof(int16_t));
  for (int i = 0; i < Kc * Z; i++) l[i] = (int8_t)(z[i] > 127 ? 127 : z[i] < -128 ? -128 : z[i]);
  return 0;
}
TEST(GpuPassPost, RateDematchHarqCombineMatchesCpu) {
  if (!have_gpu()) GTEST_SKIP();
  PostSyms &S = PostSyms::get();
  ASSERT_TRUE(S.rdm);
  static const int zs[] = {2, 7, 11, 16, 22, 36, 64, 96, 128, 176, 208, 256, 320, 384};
  Dev<int16_t> dllr, dhq;
  Dev<nr_gpu_rdm_seg_t> dsegs;
  Dev<int8_t> dout;
  int n_comb = 0, n_lbrm = 0, n_sat = 0;
  for (int seed = 0; seed < 600; seed++) {
    std::mt19937 g(seed * 7919u + 11);
    const int nseg = 1 + g() % 3;
    std::vector<nr_gpu_rdm_seg_t> base(nseg);
    for (auto &s : base) {
      s.BG = 1 + g() % 2; s.Z = zs[g() % 14];
      const int Kb = s.BG == 1 ? 22 : 10, N = (s.BG == 1 ? 66 : 50) * s.Z;
      s.K = Kb * s.Z;
      s.F = g() % 3 == 0 ? 0 : g() % (s.K / 4 + 1);
      s.Kprime = s.K - s.F;
      s.C = 1 + g() % 40;
      const uint32_t Ncb = g() % 3 ? N : (uint32_t)(s.K / 2 + g() % (N - s.K / 2) + 1);
      s.Tbslbrm = Ncb == (uint32_t)N ? (g() % 2 ? 0 : 1000000) : (uint32_t)((uint64_t)Ncb * 2 * s.C / 3 + 1);
      s.Qm = 2 * (1 + g() % 4);
      n_lbrm += s.Tbslbrm != 0 && s.Tbslbrm != 1000000;
    }
    const size_t hq_n = (size_t)nseg * NR_GPU_HQ_SEG;
    std::vector<int16_t> garbage(hq_n);
    for (auto &v : garbage) v = (int16_t)g(); /* a slot of an earlier, unrelated TB: the first transmission must clear it */
    dhq.up(garbage.data(), hq_n * 2);
    std::vector<std::vector<int16_t>> cpu_d(nseg);
    for (int i = 0; i < nseg; i++) cpu_d[i].assign((size_t)(base[i].BG == 2 ? 52 : 68) * base[i].Z + 16, 0x1234); /* the CPU buffer is also dirty before the clear */
    const int ntx = 2 + g() % 2;
    for (int tx = 0; tx < ntx; tx++) {
      std::vector<nr_gpu_rdm_seg_t> segs = base;
      std::vector<int16_t> llr;
      const int amp = g() % 3 == 0 ? 32767 : g() % 2 ? 60 : 3000; /* full range wraps the int16 sum, 60 never saturates the int8 input */
      for (int i = 0; i < nseg; i++) {
        auto &s = segs[i];
        s.rv = g() % 4; /* every transmission its own redundancy version and its own E (repetition up to 2 N) */
        const int N = (s.BG == 1 ? 66 : 50) * s.Z;
        s.E = s.Qm * (1 + g() % ((g() % 4 == 0 ? 2 * N : N) / s.Qm));
        s.llr_off = (uint32_t)llr.size();
        for (uint32_t k = 0; k < s.E; k++) llr.push_back((int16_t)((int)(g() % (2 * amp + 1)) - amp));
        s.hq_mode = tx == 0 ? 1 : 2;
        s.hq = dhq.p + (size_t)i * NR_GPU_HQ_SEG;
      }
      dllr.up(llr.data(), llr.size() * 2);
      dsegs.up(segs.data(), nseg * sizeof(segs[0]));
      const size_t ob = (size_t)nseg * NR_GPU_LDPC_DEV_IN_STRIDE;
      dout.up(std::vector<int8_t>(ob, 55).data(), ob);
      ASSERT_EQ(0, S.rdm(dllr.p, dsegs.p, nseg, dout.p, nullptr));
      std::vector<int8_t> got(ob);
      dout.down(got.data(), ob);
      std::vector<int16_t> hq(hq_n);
      dhq.down(hq.data(), hq_n * 2);
      for (int i = 0; i < nseg; i++) {
        const int Kc = segs[i].BG == 2 ? 52 : 68, N = (segs[i].BG == 1 ? 66 : 50) * segs[i].Z;
        std::vector<int8_t> ref((size_t)Kc * segs[i].Z + 16, 0);
        const int rc = cpu_prep_hq(segs[i], llr.data(), ref.data(), cpu_d[i], tx == 0);
        if (rc != 0) continue; /* a CPU-refused parameter set (the kernel writes zeros, tested above); the stored sum is not defined then */
        n_comb += tx > 0;
        ASSERT_EQ(0, memcmp(ref.data(), &got[(size_t)i * NR_GPU_LDPC_DEV_IN_STRIDE], (size_t)Kc * segs[i].Z))
            << "LDPC input, seed " << seed << " tx " << tx << " seg " << i << " BG" << segs[i].BG << " Z" << segs[i].Z << " rv" << segs[i].rv << " E" << segs[i].E << " Tbslbrm " << segs[i].Tbslbrm;
        ASSERT_EQ(0, memcmp(cpu_d[i].data(), &hq[(size_t)i * NR_GPU_HQ_SEG], (size_t)N * 2))
            << "stored soft sum, seed " << seed << " tx " << tx << " seg " << i << " BG" << segs[i].BG << " Z" << segs[i].Z;
        for (int k = 0; k < Kc * (int)segs[i].Z; k++) n_sat += ref[k] == 127 || ref[k] == -128;
      }
      if (::testing::Test::HasFatalFailure()) return;
    }
  }
  printf("HARQ combine in k_rdm: %d combined transmissions bit-equal to nr_rate_matching_ldpc_rx (clear = 0), %d LBRM segments, %d saturated inputs\n", n_comb, n_lbrm, n_sat);
  EXPECT_GT(n_comb, 500);
  EXPECT_GT(n_sat, 1000);
}
/* The soft store round trip between the host layout of the CPU store (block r at r * Kc * Z) and the device blocks of an entry: the fallback path between the two decoders. */
TEST(GpuPassHarq, BlockCopiesRoundTrip) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m));
  auto up = (nr_gpu_pass_hq_copy_fn)dlsym(m.dl, "nr_gpu_pass_hq_up");
  auto down = (nr_gpu_pass_hq_copy_fn)dlsym(m.dl, "nr_gpu_pass_hq_down");
  ASSERT_TRUE(up && down);
  std::mt19937 g(5);
  for (auto cfg : {std::array<int, 3>{1, 68 * 384, 66 * 384}, {7, 68 * 64, 66 * 64}, {32, 52 * 128, 50 * 128}, {3, 68 * 2, 66 * 2}, {NR_GPU_PASS_MAX_CB, 68 * 8, 66 * 8}}) {
    const int C = cfg[0], kcz = cfg[1], n = cfg[2];
    std::vector<uint16_t> blk((size_t)C); /* scattered blocks of the pool, as an entry holds them after evictions */
    for (int r = 0; r < C; r++) blk[r] = (uint16_t)((r * 7919 + 13) % NR_GPU_HQ_BLK);
    std::sort(blk.begin(), blk.end());
    blk.erase(std::unique(blk.begin(), blk.end()), blk.end());
    const int Cu = (int)blk.size();
    std::vector<int16_t> a((size_t)Cu * kcz), b((size_t)Cu * kcz, 0x7777);
    for (auto &v : a) v = (int16_t)g();
    ASSERT_EQ(0, up(blk.data(), a.data(), Cu, kcz, n));
    ASSERT_EQ(0, down(blk.data(), b.data(), Cu, kcz, n));
    for (int r = 0; r < Cu; r++) ASSERT_EQ(0, memcmp(&a[(size_t)r * kcz], &b[(size_t)r * kcz], (size_t)n * 2)) << "block " << r;
  }
  std::vector<uint16_t> bad = {(uint16_t)NR_GPU_HQ_BLK};
  EXPECT_NE(0, up(bad.data(), std::vector<int16_t>(100).data(), 1, 100, 100));
  EXPECT_NE(0, up(bad.data(), std::vector<int16_t>(100).data(), NR_GPU_PASS_MAX_CB + 1, 100, 100));
}
/* A failed first transmission and a retransmission, both through the whole pass: the combined verdict decodes transport blocks that neither transmission decodes alone, the TB is the transmitted one,
 * and the pass takes both (rc 0). The store is the device slot; the CPU combine is the reference of the k_rdm test above (bit-equal LDPC inputs, hence the same verdict). */
TEST(GpuPassHarq, CombinedRetransmissionDecodesWhatNeitherDecodesAlone) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && m.pass_try && pass_ready(m));
  const nr_gpu_pass_shape_t sh = GpuPassFixture::warm_shapes()[16 + 6 + 0]; /* 48 PRB, nl 2, qm 6 */
  int tried = 0, comb_ok = 0, alone_ok = 0, comb_only = 0, taken = 0;
  for (double snr : {7.0, 5.0, 3.0, 1.0, -1.0}) {
    GpuPassFixture::noise_db = snr;
    for (int seed = 1; seed <= 10; seed++) {
      GpuPassFixture f0 = GpuPassFixture::from_shape(sh, seed, -1, nullptr, 0, 0, 0, seed);
      GpuPassFixture f2 = GpuPassFixture::from_shape(sh, seed + 100, -1, nullptr, 0, 0, 2, seed);
      GpuPassFixture f2a = GpuPassFixture::from_shape(sh, seed + 100, -1, nullptr, 0, 0, 2, seed);
      ASSERT_TRUE(f0.tb_ok_built && f2.tb_ok_built);
      ASSERT_EQ(f0.tb.payload, f2.tb.payload);
      f0.bind(); f2.bind(); f2a.bind();
      std::vector<uint8_t> b0(f0.tb.A / 8 + 64), b2(f0.tb.A / 8 + 64), ba(f0.tb.A / 8 + 64);
      nr_gpu_pass_result_t r0{}, r2{}, ra{};
      r0.tb = b0.data(); r2.tb = b2.data(); ra.tb = ba.data();
      f0.job->hq_mode = 1; f2.job->hq_mode = 2;
      for (int r = 0; r < f0.tb.C; r++) f0.job->hq_blk[r] = f2.job->hq_blk[r] = (uint16_t)(1000 + 3 * r); /* the blocks of the entry */
      const int rc0 = m.pass_try(f0.job, &r0);
      ASSERT_EQ(0, rc0) << "a first transmission with combining armed takes the pass";
      if (r0.tb_ok || r0.need_cpu || r0.skip) continue; /* only failed first transmissions are stored */
      ASSERT_EQ(0, m.pass_try(f2a.job, &ra)); /* the retransmission alone (hq_mode 0) */
      const int rc2 = m.pass_try(f2.job, &r2);
      ASSERT_EQ(0, rc2) << "a retransmission with combining armed takes the pass";
      taken += 2;
      tried++;
      alone_ok += ra.tb_ok;
      comb_ok += r2.tb_ok;
      comb_only += r2.tb_ok && !ra.tb_ok;
      if (r2.tb_ok) EXPECT_EQ(0, memcmp(r2.tb, f0.tb.payload.data(), f0.tb.A / 8)) << "the combined TB is the transmitted one";
      if (ra.tb_ok) EXPECT_TRUE(r2.tb_ok) << "combining never loses a TB the retransmission decodes alone"; /* the stored sum only adds evidence here */
    }
  }
  GpuPassFixture::noise_db = 20.0;
  printf("GpuPassHarq: %d failed first transmissions, retransmission alone decoded %d, combined %d (%d only combined), %d pass runs\n", tried, alone_ok, comb_ok, comb_only, taken);
  EXPECT_GE(tried, 5);
  EXPECT_GE(comb_only, 1) << "the combine must rescue at least one TB";
  EXPECT_GE(comb_ok, alone_ok);
}
/* The block pool of the HARQ entries: blocks are handed out on demand, an entry that holds enough keeps its blocks, a short pool takes them from the victims (least recently used idle entry first), a
 * request no eviction can satisfy fails with the entry empty and the pool whole, and every block is accounted for (none lost, none given twice). */
namespace {
struct PoolEnt { nr_hq_blocks_t b; int last; bool idle; };
struct PoolCtx { std::vector<PoolEnt> *e; std::vector<int> dropped; };
nr_hq_blocks_t *pool_victim(void *ctx, const nr_hq_blocks_t *except) {
  PoolCtx *c = (PoolCtx *)ctx;
  PoolEnt *v = nullptr;
  for (auto &x : *c->e)
    if (&x.b != except && x.b.n > 0 && x.idle && (!v || x.last < v->last)) v = &x;
  if (v) c->dropped.push_back(v->last);
  return v ? &v->b : nullptr;
}
}  // namespace
TEST(GpuPassHarq, BlockPoolHandsOutKeepsAndEvictsWithoutLeaks) {
  auto pool = std::make_unique<nr_hq_pool_t>();
  memset(pool.get(), 0, sizeof(*pool));
  std::vector<PoolEnt> ents(64);
  for (int i = 0; i < 64; i++) { memset(&ents[i], 0, sizeof(PoolEnt)); ents[i].last = i + 1; ents[i].idle = true; }
  PoolCtx ctx{&ents, {}};
  auto held = [&] { int n = 0; for (auto &x : ents) n += x.b.n; return n; };
  auto check = [&](const char *what) {
    std::vector<int> seen(NR_GPU_HQ_BLK, 0);
    for (int i = 0; i < pool->nfree; i++) seen[pool->free_id[i]]++;
    for (auto &x : ents) for (int i = 0; i < x.b.n; i++) seen[x.b.id[i]]++;
    for (int i = 0; i < NR_GPU_HQ_BLK; i++) ASSERT_EQ(1, seen[i]) << what << ": block " << i << " is " << (seen[i] ? "held twice" : "lost");
    ASSERT_EQ(NR_GPU_HQ_BLK, pool->nfree + held()) << what;
  };
  int ev = 0;
  ASSERT_EQ(0, nr_hq_pool_ensure(pool.get(), &ents[0].b, 20, pool_victim, &ctx, &ev));
  EXPECT_EQ(20, ents[0].b.n);
  const uint16_t first[2] = {ents[0].b.id[0], ents[0].b.id[19]};
  EXPECT_EQ(0, nr_hq_pool_ensure(pool.get(), &ents[0].b, 12, pool_victim, &ctx, &ev)) << "enough held: kept";
  EXPECT_EQ(20, ents[0].b.n);
  EXPECT_EQ(first[0], ents[0].b.id[0]);
  EXPECT_EQ(first[1], ents[0].b.id[19]);
  EXPECT_EQ(0, ev);
  check("after the first");
  /* fill the pool: 62 entries of 32 blocks = 1984 + 20 = 2004 of 2048 */
  for (int i = 1; i < 63; i++) ASSERT_EQ(0, nr_hq_pool_ensure(pool.get(), &ents[i].b, 32, pool_victim, &ctx, &ev)) << i;
  EXPECT_EQ(0, ev);
  check("filled");
  EXPECT_EQ(NR_GPU_HQ_BLK - 20 - 62 * 32, pool->nfree);
  /* entry 63 needs 160 blocks: 44 free, so the three least recently used idle entries (last = 1, 2, 3 -> entries 0, 1, 2) give 20 + 32 + 32 = 84 -> 128 >= 160? no: a fourth is taken */
  ents[63].idle = false;
  ASSERT_EQ(0, nr_hq_pool_ensure(pool.get(), &ents[63].b, 160, pool_victim, &ctx, &ev));
  EXPECT_EQ(160, ents[63].b.n);
  EXPECT_EQ(4, ev);
  ASSERT_EQ(4u, ctx.dropped.size());
  for (int i = 0; i < 4; i++) EXPECT_EQ(i + 1, ctx.dropped[i]) << "least recently used first";
  for (int i = 0; i < 4; i++) EXPECT_EQ(0, ents[i].b.n);
  check("after the eviction");
  /* a request nothing can satisfy: every other entry busy */
  for (auto &x : ents) x.idle = false;
  const int before = held();
  ents[62].idle = false;
  EXPECT_EQ(-1, nr_hq_pool_ensure(pool.get(), &ents[62].b, NR_GPU_PASS_MAX_CB, pool_victim, &ctx, &ev)) << "62 holds 32; 160 more than free plus nothing evictable";
  EXPECT_EQ(0, ents[62].b.n) << "left empty";
  EXPECT_EQ(before - 32, held());
  check("after the refusal");
  EXPECT_EQ(-1, nr_hq_pool_ensure(pool.get(), &ents[5].b, 0, nullptr, nullptr, nullptr));
  EXPECT_EQ(-1, nr_hq_pool_ensure(pool.get(), &ents[5].b, NR_GPU_PASS_MAX_CB + 1, nullptr, nullptr, nullptr));
  for (auto &x : ents) nr_hq_pool_put(pool.get(), &x.b);
  EXPECT_EQ(NR_GPU_HQ_BLK, pool->nfree);
  check("released");
}
/* A real grant (not SI) with combining on is not refused by the HARQ gate; only the deferred SI soft combining is. */
TEST(GpuPassHarq, OnlySoftCombiningOfSiStaysOffThePass) {
  EXPECT_EQ(0, nr_gpu_pass_hq_refuses(/*armed*/ 1, /*si*/ 0));
  EXPECT_EQ(0, nr_gpu_pass_hq_refuses(0, 0));
  EXPECT_EQ(0, nr_gpu_pass_hq_refuses(0, 1));
  EXPECT_EQ(1, nr_gpu_pass_hq_refuses(1, 1));
}

/* Transport blocks below ~700 bits have a lifting size Z that is not a multiple of 4 (one code block): the generic LDPC kernel, its own L sub-graph (nz4 class). The pass equals the stage path. */
TEST(GpuPassTb, OddLiftingSizeEqualsStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m) && m.pass_warm && m.pass_split);
  int n = 0, n_ok = 0, odd = 0;
  for (int A : {64, 72, 88, 136, 232, 280}) {
    GpuPassFixture::force_A = A;
    for (double snr : {20.0, 0.0}) {
      GpuPassFixture::noise_db = snr;
      nr_gpu_pass_shape_t sh = GpuPassFixture::warm_shapes()[5]; /* 24 PRB nl 2 qm 4 */
      GpuPassFixture fx = GpuPassFixture::from_shape(sh, 21);
      ASSERT_TRUE(fx.tb_ok_built) << "A " << A;
      ASSERT_EQ(1, fx.tb.C);
      fx.shape.ncb_class = 1;
      fx.bind();
      fx.job->llr_out_all = 1;
      const bool nz4 = (fx.tb.Z & 3) != 0;
      odd += nz4;
      m.pass_split(1);
      nr_gpu_pass_shape_t w = fx.job->shape;
      w.nz4 = nz4;
      const std::string what = "A " + std::to_string(A) + " Z " + std::to_string(fx.tb.Z) + " K " + std::to_string(fx.tb.K) + " snr " + std::to_string(snr);
      ASSERT_EQ(0, m.pass_warm(&w, fx.job->dj.qam_mode, fx.job->dj.align, fx.job->dj.want_evm)) << what;
      std::vector<uint8_t> gb(fx.tb.A / 8 + 64, 0xAA), sb(fx.tb.A / 8 + 64, 0x55);
      nr_gpu_pass_result_t g{}, s{};
      g.tb = gb.data(); s.tb = sb.data();
      const int rc = m.pass_try(fx.job, &g);
      m.pass_split(0);
      ASSERT_EQ(0, rc) << "the pass takes it: " << what;
      ASSERT_EQ(0, fx.run_stage_path(&s)) << what;
      EXPECT_EQ(s.need_cpu, g.need_cpu) << what;
      EXPECT_EQ(s.tb_ok, g.tb_ok) << what;
      EXPECT_EQ(0, memcmp(s.seg_ok, g.seg_ok, sizeof(s.seg_ok))) << what;
      if (!g.need_cpu) {
        EXPECT_EQ(0, memcmp(s.tb, g.tb, g.tbs / 8)) << what;
        EXPECT_EQ(0, memcmp(fx.llr_ref.data(), fx.llr_pass.data(), (size_t)fx.tb.G * 2)) << what << ": final LLRs";
      }
      if (g.tb_ok) EXPECT_EQ(0, memcmp(g.tb, fx.tb.payload.data(), fx.tb.A / 8)) << what;
      n++;
      n_ok += g.tb_ok;
    }
  }
  GpuPassFixture::force_A = 0;
  GpuPassFixture::noise_db = 20.0;
  printf("GpuPassTb.OddLiftingSizeEqualsStagePath: %d grants (%d with Z not a multiple of 4), decoded %d\n", n, odd, n_ok);
  EXPECT_GE(odd, 6);
  EXPECT_GE(n_ok, 5);
}

/* Transport blocks of more than 32 code blocks (TBS above 269 kbit: a wide grant at a high rate), up to the 150 of 273 PRB x 4 layers x 156 RE x 256QAM at the top rate. The pass takes them
 * (code-block classes 64, 128, 160) and equals the stage path: TB, CRC verdict, per-block verdicts, final LLRs. */
TEST(GpuPassTb, ManyCodeBlocksEqualStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m) && m.pass_warm && m.pass_split);
  GpuPassFixture::max_cb = NR_GPU_PASS_MAX_CB;
  struct Case { int nrb, nl, qm, A, ndm; };
  int n = 0, n_ok = 0, cmax = 0;
  for (const Case &cs : {Case{136, 4, 8, 300000, 2}, Case{273, 4, 8, 500000, 2}, Case{273, 4, 8, 1100000, 1}, Case{217, 3, 6, 290000, 2}, Case{217, 4, 4, 290000, 2}, Case{217, 3, 6, 340000, 2}}) /* the third: C = 131 at rate 0.87, a CRC failure on both paths */
    for (double snr : {40.0, 12.0}) {
      GpuPassFixture::force_A = cs.A;
      GpuPassFixture::noise_db = snr;
      GpuPassFixture::n_dm = cs.ndm;
      nr_gpu_pass_shape_t sh = {(uint16_t)cs.nrb, 13, (uint16_t)cs.nl, (uint16_t)cs.qm, (uint16_t)cs.ndm, 1, 1};
      GpuPassFixture fx = GpuPassFixture::from_shape(sh, 9);
      ASSERT_TRUE(fx.tb_ok_built) << "A " << cs.A << " nrb " << cs.nrb;
      ASSERT_GT(fx.tb.C, 32);
      ASSERT_LE(fx.tb.C, NR_GPU_PASS_MAX_CB);
      cmax = std::max(cmax, fx.tb.C);
      fx.shape.ncb_class = (uint16_t)nr_gpu_pass_ncb_class(fx.tb.C);
      fx.bind();
      fx.job->llr_out_all = 1;
      const std::string what = "nrb " + std::to_string(cs.nrb) + " nl " + std::to_string(cs.nl) + " qm " + std::to_string(cs.qm) + " C " + std::to_string(fx.tb.C) + " A " + std::to_string(fx.tb.A) + " snr " + std::to_string(snr);
      m.pass_split(1);
      nr_gpu_pass_shape_t w = fx.job->shape;
      ASSERT_EQ(0, m.pass_warm(&w, fx.job->dj.qam_mode, fx.job->dj.align, fx.job->dj.want_evm)) << what;
      std::vector<uint8_t> gb(fx.tb.A / 8 + 64, 0xAA), sb(fx.tb.A / 8 + 64, 0x55);
      nr_gpu_pass_result_t g{}, s{};
      g.tb = gb.data(); s.tb = sb.data();
      const int rc = m.pass_try(fx.job, &g);
      m.pass_split(0);
      ASSERT_EQ(0, rc) << "the pass takes it: " << what;
      ASSERT_EQ(0, fx.run_stage_path(&s)) << what;
      EXPECT_EQ(s.need_cpu, g.need_cpu) << what;
      EXPECT_EQ(s.tb_ok, g.tb_ok) << what;
      EXPECT_EQ(0, memcmp(s.seg_ok, g.seg_ok, sizeof(s.seg_ok))) << what;
      if (!g.need_cpu) {
        EXPECT_EQ(0, memcmp(s.tb, g.tb, g.tbs / 8)) << what;
        EXPECT_EQ(0, memcmp(fx.llr_ref.data(), fx.llr_pass.data(), (size_t)fx.tb.G * 2)) << what << ": final LLRs";
      }
      if (g.tb_ok) EXPECT_EQ(0, memcmp(g.tb, fx.tb.payload.data(), fx.tb.A / 8)) << what;
      printf("  many-CB %s: pass tb_ok %d, blocks ok %d\n", what.c_str(), (int)g.tb_ok, (int)std::count(g.seg_ok, g.seg_ok + fx.tb.C, (uint8_t)1));
      n++;
      n_ok += g.tb_ok;
    }
  GpuPassFixture::max_cb = 8;
  GpuPassFixture::force_A = 0;
  GpuPassFixture::n_dm = 2;
  GpuPassFixture::noise_db = 20.0;
  printf("GpuPassTb.ManyCodeBlocksEqualStagePath: %d grants, up to %d code blocks, decoded %d\n", n, cmax, n_ok);
  EXPECT_GE(cmax, 100);
  EXPECT_GE(n_ok, 1);
}

/* the CPU math, transcribed from passive_decode_body_core (nvar normalisation + per-branch substitution, DM-RS FO/SFO), FP64 */
static double rel(double a, double b) { return b != 0.0 ? fabs(a - b) / fabs(b) : fabs(a); }
struct ChestRef {
  uint32_t nvar, flags;
  double cfo_hz, sfo_ppm, sfo_dk, coh_gate, coherence;
  int32_t kn0, kn1;
  int64_t re[2], im[2], ksum[2];
  double magsum[2];
};
static ChestRef chest_finalize(const nr_gpu_chest_post_par_t &p, const int64_t *re_, const int64_t *im_, const int64_t *ks_, const double *mag_, int kn0, int kn1);
static ChestRef chest_ref(const nr_gpu_chest_post_par_t &p) {
  ChestRef o = {};
  uint32_t nvar = 0, nvar_ant[8] = {0};
  const int nk = p.n_dmrs_sym * p.nl;
  for (int k = 0; k < nk; k++) {
    const uint32_t *q = &p.nvar_rows[k * p.nrx];
    uint64_t acc = 0; int cnt = 0;
    for (int a = 0; a < p.nrx; a++) if (q[a] > 0) { acc += q[a]; cnt++; }
    nvar += cnt > 0 ? (uint32_t)(acc / (uint64_t)cnt) : 0;
    for (int a = 0; a < 8; a++) nvar_ant[a] = a < p.nrx ? q[a] : 0;
  }
  const uint32_t den = p.nvfix ? (uint32_t)(p.n_dmrs_sym * p.nl) : (uint32_t)(p.number_symbols * p.nl * p.nrx);
  const uint32_t nvar_den = den > 0 ? den : 1u;
  nvar /= nvar_den;
  if (p.branch >= 0 && p.branch < 8 && nvar_ant[p.branch] > 0) {
    const uint64_t scaled = (uint64_t)nvar_ant[p.branch] * (uint64_t)(p.n_dmrs_sym * p.nl);
    const uint32_t nvar_branch = (uint32_t)(scaled / nvar_den);
    if (nvar_branch > 0) nvar = nvar_branch;
  }
  o.nvar = nvar;
  if (!p.h0) return o;
  const uint32_t N = p.N;
  const struct { int16_t r, i; } *h0 = (const decltype(h0))p.h0, *h1 = (const decltype(h1))p.h1;
  uint32_t kmin = N, kmax = 0;
  for (uint32_t k = 0; k < N; k++)
    if (h0[k].r != 0 || h0[k].i != 0 || h1[k].r != 0 || h1[k].i != 0) { if (k < kmin) kmin = k; if (k > kmax) kmax = k; }
  const uint32_t kmid = (kmin <= kmax) ? (kmin + kmax) / 2 : 0;
  double re[2] = {0, 0}, im[2] = {0, 0}, ksum[2] = {0, 0}, magsum[2] = {0, 0};
  uint32_t kn[2] = {0, 0};
  for (uint32_t k = 0; k < N; k++) {
    const double rr = (double)h1[k].r * h0[k].r + (double)h1[k].i * h0[k].i;
    const double ii = (double)h1[k].i * h0[k].r - (double)h1[k].r * h0[k].i;
    if (rr == 0.0 && ii == 0.0) continue;
    const int h = (k <= kmid) ? 0 : 1;
    re[h] += rr; im[h] += ii; ksum[h] += (double)k;
    magsum[h] += sqrt(((double)h1[k].r * h1[k].r + (double)h1[k].i * h1[k].i) * ((double)h0[k].r * h0[k].r + (double)h0[k].i * h0[k].i));
    kn[h]++;
  }
  int64_t rei[2] = {(int64_t)re[0], (int64_t)re[1]}, imi[2] = {(int64_t)im[0], (int64_t)im[1]}, ksi[2] = {(int64_t)ksum[0], (int64_t)ksum[1]};
  const double mg[2] = {magsum[0], magsum[1]};
  const ChestRef f = chest_finalize(p, rei, imi, ksi, mg, kn[0], kn[1]);
  o.flags = f.flags; o.cfo_hz = f.cfo_hz; o.sfo_ppm = f.sfo_ppm; o.sfo_dk = f.sfo_dk; o.coh_gate = f.coh_gate; o.coherence = f.coherence;
  o.kn0 = f.kn0; o.kn1 = f.kn1;
  for (int h = 0; h < 2; h++) { o.re[h] = rei[h]; o.im[h] = imi[h]; o.ksum[h] = ksi[h]; o.magsum[h] = mg[h]; }
  return o;
}
/* the scalar lines after the sums, verbatim from the CPU */
static ChestRef chest_finalize(const nr_gpu_chest_post_par_t &p, const int64_t *re_, const int64_t *im_, const int64_t *ks_, const double *mag_, int kn0, int kn1) {
  ChestRef o = {};
  const double dt_d = p.dt_d;
  const double re[2] = {(double)re_[0], (double)re_[1]}, im[2] = {(double)im_[0], (double)im_[1]}, ksum[2] = {(double)ks_[0], (double)ks_[1]};
  const double magsum[2] = {mag_[0], mag_[1]};
  const uint32_t kn[2] = {(uint32_t)kn0, (uint32_t)kn1};
  const double coh_num = sqrt((re[0] + re[1]) * (re[0] + re[1]) + (im[0] + im[1]) * (im[0] + im[1]));
  const double coh_den = magsum[0] + magsum[1];
  const double coherence = (coh_den > 0.0) ? (coh_num / coh_den) : 0.0;
  const double coh_h[2] = {(magsum[0] > 0.0) ? sqrt(re[0] * re[0] + im[0] * im[0]) / magsum[0] : 0.0,
                           (magsum[1] > 0.0) ? sqrt(re[1] * re[1] + im[1] * im[1]) / magsum[1] : 0.0};
  const double coh_min = (coh_h[0] < coh_h[1]) ? coh_h[0] : coh_h[1];
  const bool halves_usable = (kn[0] >= 8 && kn[1] >= 8);
  const double coh_gate = halves_usable ? coh_min : coherence;
  o.kn0 = kn[0]; o.kn1 = kn[1]; o.coherence = coherence; o.coh_gate = coh_gate;
  if (kn[0] + kn[1] > 0 && coh_gate < 0.30) o.flags |= NR_GPU_POST_FO_REJECTED;
  if (kn[0] + kn[1] > 0 && coh_gate >= 0.30) {
    o.flags |= NR_GPU_POST_FO_VALID;
    o.cfo_hz = atan2(im[0] + im[1], re[0] + re[1]) / (2.0 * M_PI * dt_d);
    if (kn[0] > 16 && kn[1] > 16 && coh_h[0] >= 0.30 && coh_h[1] >= 0.30) {
      const double p_lo = atan2(im[0], re[0]), p_hi = atan2(im[1], re[1]);
      const double k_lo = ksum[0] / (double)kn[0], k_hi = ksum[1] / (double)kn[1];
      double dphi = p_hi - p_lo;
      while (dphi > M_PI) dphi -= 2.0 * M_PI;
      while (dphi < -M_PI) dphi += 2.0 * M_PI;
      const double dk = k_hi - k_lo;
      if (dk != 0.0) {
        o.sfo_ppm = -(dphi / dk) / (2.0 * M_PI * p.df_d * dt_d) * 1.0e6;
        o.sfo_dk = (dk < 0.0) ? -dk : dk;
        o.flags |= NR_GPU_POST_SFO_MEASURED;
      }
    }
  }
  return o;
}
TEST(GpuPassPost, ChestPostMatchesCpu) {
  if (!have_gpu()) GTEST_SKIP();
  PostSyms &S = PostSyms::get();
  ASSERT_TRUE(S.chest && S.chest64) << "libpdsch_gpu.so has no nr_gpu_pass_launch_chest_post[_f64]";
  Dev<uint32_t> drows;
  Dev<int16_t> dh;
  Dev<nr_gpu_chest_post_out_t> dof;
  Dev<nr_gpu_chest_post_out64_t> do64;
  double worst = 0;
  int n_sfo = 0, n_rej = 0, n_exact = 0;
  double rel64_max = 0;
  for (int seed = 0; seed < 1000; seed++) {
    std::mt19937 g(seed * 31337u + 3);
    std::normal_distribution<double> G(0, 1);
    nr_gpu_chest_post_par_t p = {};
    p.nrx = 1 << (g() % 3); p.nl = 1 + g() % 4; p.n_dmrs_sym = 2 + g() % 2;
    p.number_symbols = 6 + g() % 8; p.nvfix = g() % 2; p.branch = g() % 3 == 0 ? -1 : (int)(g() % p.nrx);
    std::vector<uint32_t> rows(p.n_dmrs_sym * p.nl * p.nrx);
    for (auto &r : rows) r = g() % 5 == 0 ? 0 : g() % (g() % 2 ? 100000 : 3000000);
    const int N = 4096;
    p.N = N; p.n_rb_dl = 273; p.df_d = g() % 2 ? 30000.0 : 15000.0;
    p.dt_d = 1.0e-3 / (p.df_d == 30000.0 ? 2 : 1) / 14 * (4 + g() % 8);
    p.df_f = (float)p.df_d; p.dt_f = (float)p.dt_d;
    const int nrb = 1 + g() % 273, start = g() % (273 - nrb + 1) * 12 % (N - 12 * nrb);
    const double cfo = (G(g) * 80) * 2 * M_PI * p.dt_d, sfo = G(g) * 8e-6 * 2 * M_PI * p.df_d * p.dt_d;
    const double amp = 20.0 + g() % 9000, noise = g() % 4 == 0 ? 6.0 : g() % 2 ? 0.1 : 0.5;
    std::vector<int16_t> h0c(2 * N, 0), h1c(2 * N, 0);
    for (int k = 0; k < nrb * 12; k++) {
      const std::complex<double> a(G(g) * amp, G(g) * amp);
      const std::complex<double> b = a * std::polar(1.0, cfo - sfo * k) + std::complex<double>(G(g), G(g)) * (noise * amp);
      auto cl = [](double v) { return (int16_t)std::max(-32768.0, std::min(32767.0, std::round(v))); };
      h0c[2 * (start + k)] = cl(a.real()); h0c[2 * (start + k) + 1] = cl(a.imag());
      h1c[2 * (start + k)] = cl(b.real()); h1c[2 * (start + k) + 1] = cl(b.imag());
    }
    if (g() % 8 == 0) { h0c.assign(2 * N, 0); h1c.assign(2 * N, 0); } /* no estimate at all */
    std::vector<int16_t> hh(h0c); hh.insert(hh.end(), h1c.begin(), h1c.end());
    drows.up(rows.data(), rows.size() * 4);
    dh.up(hh.data(), hh.size() * 2);
    p.nvar_rows = drows.p; p.h0 = dh.p; p.h1 = dh.p + 2 * N;
    nr_gpu_chest_post_par_t pc = p; pc.nvar_rows = rows.data(); pc.h0 = h0c.data(); pc.h1 = h1c.data();
    const ChestRef ref = chest_ref(pc);
    nr_gpu_chest_post_out_t of; nr_gpu_chest_post_out64_t o64;
    do64.up(&o64, sizeof o64); dof.up(&of, sizeof of);
    ASSERT_EQ(0, S.chest(&p, dof.p, nullptr));
    ASSERT_EQ(0, S.chest64(&p, do64.p, nullptr));
    dof.down(&of, sizeof of); do64.down(&o64, sizeof o64);
    n_sfo += (ref.flags & NR_GPU_POST_SFO_MEASURED) != 0; n_rej += (ref.flags & NR_GPU_POST_FO_REJECTED) != 0;
    /* integer part: exact in both builds */
    ASSERT_EQ(1u, of.nvar_valid); ASSERT_EQ(1u, o64.nvar_valid);
    ASSERT_EQ(ref.nvar, of.nvar) << seed; ASSERT_EQ(ref.nvar, o64.nvar) << seed;
    ASSERT_EQ(ref.flags, of.flags) << seed; ASSERT_EQ(ref.flags, o64.flags) << seed;
    ASSERT_EQ(ref.kn0, of.kn0); ASSERT_EQ(ref.kn1, of.kn1); ASSERT_EQ(ref.kn0, o64.kn0); ASSERT_EQ(ref.kn1, o64.kn1);
    /* FP64 build: bit-equal, but for the two atan2() results (cfo, sfo): CUDA's atan2 is not glibc's (last bits, see the header) */
    ASSERT_EQ(ref.re[0], o64.re[0]); ASSERT_EQ(ref.re[1], o64.re[1]); ASSERT_EQ(ref.im[0], o64.im[0]); ASSERT_EQ(ref.im[1], o64.im[1]);
    ASSERT_EQ(ref.ksum[0], o64.ksum[0]); ASSERT_EQ(ref.ksum[1], o64.ksum[1]); ASSERT_EQ(ref.magsum[0], o64.magsum[0]) << seed; ASSERT_EQ(ref.magsum[1], o64.magsum[1]) << seed;
    ASSERT_EQ(ref.sfo_dk, o64.sfo_dk) << seed; ASSERT_EQ(ref.coh_gate, o64.coh_gate) << seed; ASSERT_EQ(ref.coherence, o64.coherence) << seed;
    {
      const ChestRef fin = chest_finalize(pc, o64.re, o64.im, o64.ksum, o64.magsum, o64.kn0, o64.kn1);
      ASSERT_EQ(ref.cfo_hz, fin.cfo_hz) << seed; ASSERT_EQ(ref.sfo_ppm, fin.sfo_ppm) << seed; /* the host half on the device sums: the CPU's bits */
    }
    n_exact += ref.cfo_hz == o64.cfo_hz && ref.sfo_ppm == o64.sfo_ppm;
    rel64_max = std::max(rel64_max, std::max(rel(o64.cfo_hz, ref.cfo_hz), rel(o64.sfo_ppm, ref.sfo_ppm)));
    /* float build: 1e-6 relative */
    const double e[5] = {rel(of.cfo_hz, ref.cfo_hz), rel(of.sfo_ppm, ref.sfo_ppm), rel(of.sfo_dk, ref.sfo_dk), rel(of.coh_gate, ref.coh_gate), rel(of.coherence, ref.coherence)};
    for (double x : e) { if (x > worst) worst = x; }
    for (int i = 0; i < 5; i++) EXPECT_LE(e[i], 1e-6) << "seed " << seed << " field " << i;
    if (::testing::Test::HasFailure()) break;
  }
  printf("chest post: worst float relative error %.3g, %d SFO measured, %d rejected; FP64 atan2-derived cfo/sfo bit-equal in %d of 1000 grants, worst relative difference %.3g\n", worst, n_sfo, n_rej, n_exact, rel64_max);
  EXPECT_LE(rel64_max, 1e-12); /* glibc vs CUDA atan2: the last bit of two angles, amplified by the cancellation in their difference */
  EXPECT_GT(n_sfo, 200);
  EXPECT_GT(n_rej, 20);
}

/* the launch wrappers enqueue only: all four captured into one graph (Global mode: any sync/alloc/copy in them would fail the capture), replayed twice */
TEST(GpuPassPost, LaunchersAreGraphCapturable) {
  if (!have_gpu()) GTEST_SKIP();
  PostSyms &S = PostSyms::get();
  ASSERT_TRUE(S.chest && S.chest64 && S.rdm && S.crc);
  crcTableInit();
  CrcCase c = CrcCase::random(3, false);
  Dev<uint8_t> dbits, dok;
  Dev<nr_gpu_crc_seg_t> dsegs;
  dbits.up(c.bits.data(), c.bits.size());
  dsegs.up(c.segs.data(), c.segs.size() * sizeof(c.segs[0]));
  dok.up(std::vector<uint8_t>(c.C + 1, 7).data(), c.C + 1);
  nr_gpu_rdm_seg_t rs = {0, 96, 2, 2, 8, 22 * 8, 0, 22 * 8, 0, 1, 0};
  std::vector<int16_t> llr(96, 5);
  Dev<int16_t> dllr; Dev<nr_gpu_rdm_seg_t> drs; Dev<int8_t> dout;
  dllr.up(llr.data(), 192); drs.up(&rs, sizeof rs); dout.up(std::vector<int8_t>(NR_GPU_LDPC_DEV_IN_STRIDE, 9).data(), NR_GPU_LDPC_DEV_IN_STRIDE);
  uint32_t rows[8] = {100, 200, 0, 400, 100, 200, 300, 400};
  Dev<uint32_t> drows; drows.up(rows, sizeof rows);
  Dev<nr_gpu_chest_post_out_t> dof; Dev<nr_gpu_chest_post_out64_t> do64;
  nr_gpu_chest_post_out_t zf = {}; nr_gpu_chest_post_out64_t z64 = {};
  dof.up(&zf, sizeof zf); do64.up(&z64, sizeof z64);
  nr_gpu_chest_post_par_t p = {};
  p.nvar_rows = drows.p; p.nrx = 4; p.nl = 1; p.n_dmrs_sym = 2; p.number_symbols = 12; p.branch = -1; p.N = 4096; p.dt_d = 3e-4; p.df_d = 30000; p.dt_f = 3e-4f; p.df_f = 30000.f;
  cudaStream_t st; ASSERT_EQ(cudaSuccess, cudaStreamCreate(&st));
  cudaGraph_t g; cudaGraphExec_t ge;
  ASSERT_EQ(cudaSuccess, cudaStreamBeginCapture(st, cudaStreamCaptureModeGlobal));
  EXPECT_EQ(0, S.crc(dbits.p, dsegs.p, (int)c.C, c.tbs, dok.p, dok.p + c.C, st));
  EXPECT_EQ(0, S.rdm(dllr.p, drs.p, 1, dout.p, st));
  EXPECT_EQ(0, S.chest(&p, dof.p, st));
  EXPECT_EQ(0, S.chest64(&p, do64.p, st));
  ASSERT_EQ(cudaSuccess, cudaStreamEndCapture(st, &g));
  ASSERT_EQ(cudaSuccess, cudaGraphInstantiate(&ge, g, 0));
  for (int rep = 0; rep < 2; rep++) {
    ASSERT_EQ(cudaSuccess, cudaGraphLaunch(ge, st));
    ASSERT_EQ(cudaSuccess, cudaStreamSynchronize(st));
  }
  std::vector<uint8_t> ok(c.C + 1);
  dok.down(ok.data(), c.C + 1);
  for (uint32_t r = 0; r < c.C; r++) EXPECT_EQ(c.cpu_seg_ok(r), (int)ok[r]);
  EXPECT_EQ(c.cpu_tb_ok(), (int)ok[c.C]);
  nr_gpu_chest_post_out_t of; do64.down(&z64, sizeof z64); dof.down(&of, sizeof of);
  EXPECT_EQ(of.nvar, z64.nvar);
  EXPECT_GT(of.nvar, 0u);
  cudaGraphExecDestroy(ge); cudaGraphDestroy(g); cudaStreamDestroy(st);
}

/* I1/I2 of the review: no DM-RS rows leaves nvar untouched and says so; a TB without reassembly information is refused without dividing */
TEST(GpuPassPost, DegenerateInputs) {
  if (!have_gpu()) GTEST_SKIP();
  PostSyms &S = PostSyms::get();
  ASSERT_TRUE(S.chest && S.crc);
  Dev<uint32_t> drows; uint32_t rows[4] = {1, 2, 3, 4}; drows.up(rows, sizeof rows);
  nr_gpu_chest_post_par_t p = {};
  p.nvar_rows = drows.p; p.nrx = 4; p.nl = 1; p.n_dmrs_sym = 0; p.number_symbols = 12; p.branch = -1;
  nr_gpu_chest_post_out_t o; memset(&o, 0x5a, sizeof o); o.nvar = 0xdeadbeef; o.nvar_valid = 7;
  Dev<nr_gpu_chest_post_out_t> dof; dof.up(&o, sizeof o);
  ASSERT_EQ(0, S.chest(&p, dof.p, nullptr));
  dof.down(&o, sizeof o);
  EXPECT_EQ(0u, o.nvar_valid);
  EXPECT_EQ(0xdeadbeefu, o.nvar);
  crcTableInit();
  CrcCase c = CrcCase::random(11, false);
  while (c.C < 2) c = CrcCase::random(c.tbs + 12, false);
  Dev<uint8_t> dbits, dok; Dev<nr_gpu_crc_seg_t> dsegs;
  auto segs = c.segs; segs[0].pay_bytes = 0;
  dbits.up(c.bits.data(), c.bits.size()); dsegs.up(segs.data(), segs.size() * sizeof(segs[0]));
  dok.up(std::vector<uint8_t>(c.C + 1, 7).data(), c.C + 1);
  ASSERT_EQ(0, S.crc(dbits.p, dsegs.p, (int)c.C, c.tbs, dok.p, dok.p + c.C, nullptr));
  std::vector<uint8_t> ok(c.C + 1); dok.down(ok.data(), c.C + 1);
  EXPECT_EQ(0, (int)ok[c.C]);
  for (uint32_t r = 0; r < c.C; r++) EXPECT_EQ(c.cpu_seg_ok(r), (int)ok[r]);
  dok.up(std::vector<uint8_t>(c.C + 1, 7).data(), c.C + 1);
  ASSERT_EQ(0, S.crc(dbits.p, dsegs.p, 0, c.tbs, dok.p, dok.p + c.C, nullptr)); /* nseg == 0 */
  dok.down(ok.data(), c.C + 1);
  EXPECT_EQ(0, (int)ok[c.C]);
}


/* ===================================================== Task 5: FP32 demod (RTX 4070) ===================================================== */
/* The float instantiation of the align / multi-layer MMSE kernels is not bit-exact with the double one by design. Acceptance on the fixture sweep (the 64 warm shapes,
 * qam_mode 3 = the exact MMSE for Nl >= 2): LLR sign agreement >= 99.9 %, CRC-OK count within 1 % (+1). The sweep length is NR_GPU_FP32_SEEDS (default 4 on a shared host; 400 for
 * the full sweep), the SNR of seed s is 4 + 5 s mod 20 dB. NoDoubleInFp32Kernels is the ctest script tests/check_no_fp64_sass.sh. */
TEST(GpuPassFp32, SignAgreementAndCrc) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(m.pass_try && m.pass_warm);
  ASSERT_TRUE(pass_ready(m));
  size_t fr0 = 0, fr1 = 0, tot = 0;
  cudaMemGetInfo(&fr0, &tot);
  for (auto sh : GpuPassFixture::warm_shapes()) { // the FP32 twin of every default class (pass_warm skips the ones already cached)
    sh.fp32 |= NR_GPU_PASS_FP32_DEMOD;
    ASSERT_EQ(0, m.pass_warm(&sh, sh.nl >= 2 ? 3 : 0, 0, 0)) << "nrb " << sh.nrb_class << " nl " << sh.nl;
  }
  cudaMemGetInfo(&fr1, &tot);
  printf("GpuPassFp32: 16 FP32 classes (NI instances each) added %.1f MB of device memory\n", ((double)fr0 - (double)fr1) / 1048576.0);
  const char *se = getenv("NR_GPU_FP32_SEEDS");
  const int seeds = se ? atoi(se) : 4;
  int crc64 = 0, crc32 = 0, n = 0, diff = 0;
  size_t same = 0, total = 0;
  double worst = 1.0;
  for (int seed = 0; seed < seeds; seed++) {
    GpuPassFixture::noise_db = 4 + (seed * 5) % 20;
    for (auto shape : GpuPassFixture::warm_shapes()) {
      GpuPassFixture fx = GpuPassFixture::from_shape(shape, 100 + seed);
      if (!fx.tb_ok_built) continue;
      fx.bind();
      auto a = fx.run_pass(m, 0), b = fx.run_pass(m, 1);
      ASSERT_EQ(0, a.rc) << "nrb " << shape.nrb_class << " nl " << shape.nl << " qm " << shape.qm;
      ASSERT_EQ(0, b.rc) << "FP32 class not warm or refused, nrb " << shape.nrb_class << " nl " << shape.nl << " qm " << shape.qm;
      n++;
      crc64 += a.tb_ok;
      crc32 += b.tb_ok;
      size_t s0 = same, t0 = total;
      GpuPassFixture::count_sign_agreement(a, b, &same, &total);
      if (total > t0) worst = std::min(worst, (double)(same - s0) / (double)(total - t0));
      if (shape.nl >= 2 && a.llr != b.llr) diff++;
    }
  }
  GpuPassFixture::noise_db = 20.0;
  printf("GpuPassFp32.SignAgreementAndCrc: %d grants, CRC ok fp64 %d fp32 %d, sign agreement %.7f (%zu of %zu LLRs opposite; worst grant %.5f), %d multi-layer grants with different LLRs\n", n, crc64, crc32,
         total ? (double)same / (double)total : 0.0, total - same, total, worst, diff);
  ASSERT_GT(total, 0u);
  EXPECT_GE((double)same / (double)total, 0.999);
  EXPECT_NEAR(crc32, crc64, 0.01 * crc64 + 1);
  EXPECT_GT(diff, 0) << "the float kernels never ran: the FP32 class is not selected";
}

/* The EVM chain of an FP32 class is float end to end (storage and sums). Documented tolerance against the default chain (float per-RE math, double sums): every
 * per-symbol sum the host folds (err_l, qr, qi, qd, qx, cr, ci, P, Y) within 2e-3 relative (float sums of ~3e3 terms, pairwise tree: ~1e-6 expected). */
TEST(GpuPassFp32, EvmChainMatchesFp64Sums) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(pass_ready(m));
  double worst = 0;
  for (int which = 0; which < 3; which++) { // nrb 48: Nl 1, 2 (Cramer), 4 (generic elimination)
    const nr_gpu_pass_shape_t base = GpuPassFixture::warm_shapes()[4 * 4 * 1 + 4 * (which == 0 ? 0 : which == 1 ? 1 : 3) + 1]; // qm 4
    nr_gpu_pass_shape_t s32 = base;
    s32.fp32 |= NR_GPU_PASS_FP32_DEMOD;
    ASSERT_EQ(0, m.pass_warm(&base, base.nl >= 2 ? 3 : 0, 0, 1));
    ASSERT_EQ(0, m.pass_warm(&s32, base.nl >= 2 ? 3 : 0, 0, 1));
    GpuPassFixture fx = GpuPassFixture::from_shape(base, 5);
    ASSERT_TRUE(fx.tb_ok_built);
    fx.bind();
    std::unique_ptr<nr_gpu_demod_job_t> d0(new nr_gpu_demod_job_t()), d1(new nr_gpu_demod_job_t());
    for (int f = 0; f < 2; f++) {
      fx.job->shape.fp32 = f ? s32.fp32 : base.fp32;
      fx.job->dj.demod_fp32 = f;
      fx.job->dj.want_evm = 1;
      fx.job->dj.want_kappa = 1;
      fx.job->dj.evm_fp64 = 0;
      std::vector<uint8_t> tb(fx.tb.A / 8 + 64);
      nr_gpu_pass_result_t r{};
      r.tb = tb.data();
      r.demod_out = f ? d1.get() : d0.get();
      ASSERT_EQ(0, m.pass_try(fx.job, &r)) << "nl " << base.nl << " fp32 " << f;
    }
    auto rel = [&](double a, double b) { return std::fabs(a - b) / std::max(1.0, std::fabs(b)); };
    int valid = 0;
    for (int sy : fx.data_syms) {
      const nr_gpu_evm_sym_t &a = d0->evm[sy], &b = d1->evm[sy];
      ASSERT_EQ(a.valid, b.valid);
      if (!a.valid) continue;
      valid++;
      for (int l = 0; l < base.nl; l++)
        for (double w : {rel(b.err_l[l], a.err_l[l]), rel(b.qr[l], a.qr[l]), rel(b.qi[l], a.qi[l]), rel(b.qd[l], a.qd[l]), rel(b.qx[l], a.qx[l])}) worst = std::max(worst, w);
      for (int q = 0; q < a.nb; q++)
        for (double w : {rel(b.cr[q], a.cr[q]), rel(b.ci[q], a.ci[q]), rel(b.P[q], a.P[q]), rel(b.Y[q], a.Y[q])}) worst = std::max(worst, w);
      EXPECT_EQ(a.nv, b.nv);
    }
    EXPECT_GT(valid, 0) << "no EVM sums, nl " << base.nl;
  }
  printf("GpuPassFp32.EvmChainMatchesFp64Sums: worst relative difference %.3e\n", worst);
  EXPECT_LE(worst, 2e-3);
}
/* The rate gate of the FP32 shadow checks (nr_gpu_pass_fp32_gate.h): flips above 1 % of the shadowed grants, after a minimum sample of 64, disable the pass. */
TEST(GpuPassFp32, ShadowGateLogic) {
  EXPECT_FALSE(nr_fp32_gate_bad(63, 63)) << "below the minimum sample";
  EXPECT_TRUE(nr_fp32_gate_bad(64, 64));
  EXPECT_FALSE(nr_fp32_gate_bad(200, 2)) << "exactly 1 % keeps the pass";
  EXPECT_TRUE(nr_fp32_gate_bad(200, 3));
  EXPECT_FALSE(nr_fp32_gate_bad(10000, 100));
  EXPECT_TRUE(nr_fp32_gate_bad(10000, 101));
  { // a sequence: 1 flip in 100 grants (1 %) keeps the pass, the second flip (1.98 %) trips it
    nr_fp32_gate_t g = {};
    int tripped = 0;
    for (int i = 0; i < 100; i++) tripped |= nr_fp32_gate_note(&g, i == 99);
    EXPECT_EQ(0, tripped);
    EXPECT_EQ(1, nr_fp32_gate_note(&g, 1)) << "2 flips in 101: 1.98 % -> tripped";
  }
  { // an early burst is not judged before the minimum sample
    nr_fp32_gate_t g = {};
    for (int i = 0; i < 63; i++) EXPECT_EQ(0, nr_fp32_gate_note(&g, 1)) << i;
    EXPECT_EQ(1, nr_fp32_gate_note(&g, 1));
  }
  { // 3 flips among 400 grants (0.75 %) keep the pass; one flip within the first 64 grants is judged only at the 64th
    nr_fp32_gate_t g = {};
    int tripped = 0;
    for (int i = 0; i < 300; i++) tripped |= nr_fp32_gate_note(&g, 0);
    for (int i = 0; i < 3; i++) tripped |= nr_fp32_gate_note(&g, 1);
    for (int i = 0; i < 97; i++) tripped |= nr_fp32_gate_note(&g, 0);
    EXPECT_EQ(0, tripped) << "3 flips in 400 = 0.75 %";
    nr_fp32_gate_t e = {};
    for (int i = 0; i < 63; i++) nr_fp32_gate_note(&e, i == 5);
    EXPECT_EQ(1, nr_fp32_gate_note(&e, 0)) << "1 flip in 64 = 1.56 %";
  }
}
/* ===================================================== Task 7: the PUSCH (UL) pass ===================================================== */
extern "C" {
#include "nr_gpu_pass_ul.h"
#include "nr_gpu_pass_ul_ref.h"
int load_dftslib(void);
}
/* The UL slot of the fixture: its IQ ring as the linear snapshot of the passive UL decoder (PRE samples of margin in front), the FFT windows at a timing advance `ta`
 * (nr_symbol_fep_ul: the window starts early by the cyclic prefix fraction and by the advance), and the job description nr_gpu_pass_ul_run takes. */
struct UlSlot {
  static constexpr int PRE = 1024, N = GpuPassFixture::N, NS = GpuPassFixture::NSYM;
  std::vector<int16_t> lin[4], rxF;
  size_t rx_stride = (size_t)NS * N + 512; /* c16 between two antennas of the caller's rxdataF */
  nr_gpu_pass_ul_in_t in = {};
  nr_gpu_pass_ul_out_t out = {};
  std::vector<uint8_t> tb;
  std::vector<int16_t> llr;
  int ta = 0;
  void build(const GpuPassFixture &fx, int ta_, int keep, int passive, int ctx)
  {
    ta = ta_;
    const int total = PRE + GpuPassFixture::SLOT + 2048;
    for (int a = 0; a < 4; a++) {
      lin[a].assign((size_t)2 * total, 0);
      for (int n = -PRE; n < GpuPassFixture::SLOT + 2048 - PRE; n++) {
        const size_t p = ((size_t)fx.ring_offset + (size_t)(n + (int)fx.ring_len)) % fx.ring_len;
        lin[a][2 * (size_t)(PRE + n)] = fx.ring[a][2 * p];
        lin[a][2 * (size_t)(PRE + n) + 1] = fx.ring[a][2 * p + 1];
      }
    }
    rxF.assign((size_t)2 * 4 * rx_stride, 0x5a5a);
    tb.assign((size_t)fx.tb.A / 8 + 64, 0xcc);
    llr.assign((size_t)fx.tb.G + 64, 0x1111);
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.rnti = (uint16_t)fx.tb.rnti; in.slot = 0; in.start_symbol = fx.data_syms.front(); in.num_symbols = 14 - in.start_symbol; in.num_rb = fx.nrb; in.bwp_start = 0; in.rb_start = fx.start_rb;
    in.ul_dmrs_symb_pos = (1u << fx.dm_sym[0]) | (1u << fx.dm_sym[1]);
    in.dmrs_config_type = 0; in.n_dmrs_cdm_groups_no_data = 2; in.nscid = 0; in.dmrs_scrambling_id = 0; in.dmrs_ports = 1;
    in.qam_mod_order = fx.qm; in.nr_of_layers = 1; in.rv = 0; in.bg = fx.tb.BG; in.tbs_bits = (uint32_t)fx.tb.A; in.tbslbrm = 0; in.data_scrambling_id = (int)fx.tb.nid;
    in.max_ldpc_iterations = 8;
    in.N = N; in.symbols_per_slot = NS; in.n_rb_ul = 273; in.first_carrier_offset = GpuPassFixture::FCO; in.ncp_normal = 1; in.nb_rx = 4;
    in.passive = passive; in.keep = keep; in.timing_keep = keep;
    const int early = GpuPassFixture::NCP / 8;
    for (int m = 0; m < NS; m++) {
      const int start = m == 0 ? 0 : GpuPassFixture::NCP0 + N + (m - 1) * (N + GpuPassFixture::NCP), ncp = m == 0 ? GpuPassFixture::NCP0 : GpuPassFixture::NCP;
      in.win[m] = PRE + start + ncp - early - ta;
    }
    for (int a = 0; a < 4; a++) {
      in.iq[a] = lin[a].data();
      in.rx[a] = rxF.data() + 2 * (size_t)a * rx_stride;
    }
    in.pil_override[0] = fx.pilot[0][0].data();
    in.pil_override[1] = fx.pilot[1][0].data();
    in.iq_len = (uint32_t)total;
    in.symbol_rot = fx.rot.data();
    in.ctx = ctx;
    in.tb = tb.data();
    in.llr_out = llr.data();
    in.llr_out_all = 1;
    in.want_est = 1;
  }
};

static const int16_t *ul_delay_table()
{
  static std::vector<int16_t> dt;
  if (dt.empty()) {
    dt.assign((size_t)(2 * 20 + 1) * 4096 * 2, 0);
    for (int d = -20; d <= 20; d++)
      for (int i = 0; i < 4096; i++) {
        const double ph = 2.0 * M_PI * i * d / 4096;
        dt[2 * ((size_t)(20 + d) * 4096 + i)] = (int16_t)std::lround(256 * std::cos(ph));
        dt[2 * ((size_t)(20 + d) * 4096 + i) + 1] = (int16_t)std::lround(256 * std::sin(ph));
      }
  }
  return dt.data();
}

/* the module up for the PUSCH pass: instances, the host builder's table of classes (one), then classes on demand */
static bool ul_ready(GpuPassModule &m)
{
  static int state = 0;
  if (state) return state > 0;
  state = -1;
  setenv("ISAC_UL_GPU_PASS", "1", 1);
  setenv("ISAC_UL_GPU_PASS_CLASSES", "1", 1);
  if (!pass_ready(m) || !m.pass_warm) return false;
  if (load_dftslib() != 0) return false;
  if (nr_gpu_pass_ul_warm(m.dl, 4, 0) < 1) return false;
  for (int c = 0; c < 8; c++) nr_gpu_pass_ul_prewarm(c);
  state = 1;
  return true;
}
static void ul_warm_class(GpuPassModule &m, int nrb, int nd, int C)
{
  static std::map<uint64_t, int> done;
  const int cls = nr_gpu_pass_nrb_class(nrb), ncb = nr_gpu_pass_ncb_class(C);
  const uint64_t key = (uint64_t)cls << 20 | (uint64_t)nd << 10 | (uint64_t)ncb;
  if (done.count(key)) return;
  const nr_gpu_pass_shape_t sh = {(uint16_t)cls, 13, 1, 2, (uint16_t)nd, (uint16_t)ncb, 0, 0, 1};
  done[key] = m.pass_warm(&sh, 0, 0, 0);
  ASSERT_EQ(0, done[key]) << "warm UL class nrb " << cls << " nd " << nd << " ncb " << ncb;
}

/* the CPU reference of the whole slot: FEP (the exact FEP stage at the same window), the ported chest, the real equaliser, the descrambling and the CPU decode tail */
struct UlRef {
  std::vector<int16_t> rxF;   /* [4][14][N] c16 */
  std::vector<int16_t> est;   /* [4][14][N] c16 */
  std::vector<int16_t> llr;   /* descrambled */
  int delay[2][4][3] = {};
  uint64_t acc[2][4] = {};
  int log2_maxh = 0, avg[4] = {};
  nr_gpu_pass_result_t res = {};
  std::vector<uint8_t> tb;
  bool ok = false;
};
static UlRef ul_reference(GpuPassModule &m, GpuPassFixture &fx, int ta, int keep)
{
  UlRef r;
  using Pass = GpuPassFixture::Pass;
  Pass P;
  if (!P.setup(m, fx, -1) || !P.load(fx)) return r;
  P.fj.ring_offset = (uint32_t)(((long)fx.ring_offset + (long)fx.ring_len - ta) % (long)fx.ring_len);
  int fs = 0, cs = 0;
  if (P.stage(&fs, &cs) || m.fep(&P.fj, &P.rxF, fs, P.s)) return r;
  nr_gpu_buf_d2h_async(&P.rxF, 0, P.rxsz, P.s);
  if (cudaStreamSynchronize(P.s)) return r;
  const int N = 4096;
  r.rxF.assign((int16_t *)P.rxF.host, (int16_t *)P.rxF.host + 2 * (size_t)4 * 14 * N);
  r.est.assign((size_t)2 * 4 * 14 * N, 0);
  const int k0 = (GpuPassFixture::FCO + 12 * fx.start_rb) % N;
  for (int k = 0; k < 2; k++)
    for (int a = 0; a < 4; a++) {
      uint64_t noise = 0;
      nr_gpu_pass_ul_ref_chest((const c16_t *)&r.rxF[2 * ((size_t)a * 14 + fx.dm_sym[k]) * N], (const c16_t *)fx.pilot[k][0].data(), k0, fx.nrb, 0, (const c16_t *)ul_delay_table(), N, 20,
                               (c16_t *)&r.est[2 * ((size_t)a * 14 + fx.dm_sym[k]) * N], r.delay[k][a], &noise);
      r.acc[k][a] = noise;
    }
  std::vector<int16_t> llr((size_t)fx.tb.G + 64, 0);
  const uint32_t mask = (1u << fx.dm_sym[0]) | (1u << fx.dm_sym[1]);
  const int n = nr_gpu_pass_ul_ref_eq(4, fx.nrb, fx.qm, keep, 0, fx.data_syms.front(), 14 - fx.data_syms.front(), mask, 2, k0, (const c16_t *)r.rxF.data(), (const c16_t *)r.est.data(), llr.data(), &r.log2_maxh, r.avg);
  if (n != (int)fx.tb.G) return r;
  for (uint32_t i = 0; i < fx.tb.G; i++)
    if (fx.tb.scr[i >> 5] >> (i & 31) & 1u) llr[i] = (int16_t)(-llr[i]);
  llr.resize(fx.tb.G);
  r.llr = llr;
  r.tb.assign((size_t)fx.tb.A / 8 + 64, 0);
  r.res.tb = r.tb.data();
  std::vector<int16_t> l2 = llr;
  if (fx.decode_llr(m, P, P.s, l2, &r.res) != 0) return r;
  r.ok = true;
  return r;
}

/* negative control: with g_ul_neg the reference is deliberately perturbed (a different timing advance / receive branch) and the mismatches are counted instead of failing the test */
static bool g_ul_neg = false;
static int g_ul_neg_count = 0, g_ul_ref_ta_delta = 0, g_ul_ref_keep_flip = 0;
static int ul_case(GpuPassModule &m, int nrb, int qm, int seed, int ta, int keep, double noise_db, const char *what, int first_sym = 1)
{
  GpuPassFixture::noise_db = noise_db;
  GpuPassFixture::first_sym = first_sym;
  const nr_gpu_pass_shape_t sh = {(uint16_t)nrb, 13, 1, (uint16_t)qm, 2, 1, 1};
  GpuPassFixture fx = GpuPassFixture::from_shape(sh, seed);
  GpuPassFixture::noise_db = 20.0;
  GpuPassFixture::first_sym = 1;
  if (!fx.tb_ok_built) return 2; /* no TB for the shape */
  ul_warm_class(m, nrb, 2, fx.tb.C);
  if (::testing::Test::HasFatalFailure()) return -1;
  UlSlot sl;
  sl.build(fx, ta, keep, keep >= 0 ? 1 : 0, 0);
  const int rc = nr_gpu_pass_ul_run(&sl.in, &sl.out);
  EXPECT_EQ(0, rc) << what;
  if (rc != 0) return -1;
  UlRef ref = ul_reference(m, fx, ta + g_ul_ref_ta_delta, g_ul_ref_keep_flip ? (keep < 0 ? 0 : -1) : keep);
  EXPECT_TRUE(ref.ok) << what << ": no CPU reference";
  if (!ref.ok) return -1;
  int bad = 0;
  auto chk = [&](bool c, const char *w) {
    if (c) return;
    bad++;
    if (g_ul_neg) g_ul_neg_count++;
    else ADD_FAILURE() << what << ": " << w;
  };
  /* the transformed symbols of the grant, then the chest, the scale and the LLRs, bit for bit */
  for (int a = 0; a < 4; a++)
    for (int s = fx.data_syms.front(); s < 14; s++)
      chk(!memcmp(&sl.rxF[2 * (size_t)a * sl.rx_stride + 2 * (size_t)s * 4096], &ref.rxF[2 * ((size_t)a * 14 + s) * 4096], 4096 * 4), "rxdataF of the pass differs from the exact FEP");
  const nr_gpu_pass_result_t &r = sl.out.r;
  for (int k = 0; k < 2; k++)
    for (int a = 0; a < 4; a++) {
      const int row = k * 4 + a;
      chk(r.ul_delay[row][0] == ref.delay[k][a][0] && r.ul_delay[row][1] == ref.delay[k][a][1] && r.ul_delay[row][2] == ref.delay[k][a][2], "delay triple");
      chk(r.ul_acc[row] == ref.acc[k][a], "noise sum");
      uint64_t hs = 0;
      const int16_t *e = &ref.est[2 * ((size_t)a * 14 + fx.dm_sym[k]) * 4096];
      for (int i = 0; i < 12 * fx.nrb; i++) hs += (uint32_t)((int)e[2 * i] * e[2 * i] + (int)e[2 * i + 1] * e[2 * i + 1]);
      chk(r.ul_hsum[row] == hs, "sum |H|^2");
      chk(!memcmp(sl.out.est + (size_t)row * 4096 * 2, &ref.est[2 * ((size_t)a * 14 + fx.dm_sym[k]) * 4096], 12 * fx.nrb * 4), "chest estimate");
    }
  chk(sl.out.log2_maxh == ref.log2_maxh, "log2_maxh");
  for (int a = 0; a < 4; a++) chk(r.ul_avg[a] == ref.avg[a], "channel level");
  chk(!memcmp(sl.llr.data(), ref.llr.data(), (size_t)fx.tb.G * 2), "LLRs differ from the CPU equaliser");
  chk(r.need_cpu == ref.res.need_cpu, "need_cpu");
  chk(r.tb_ok == ref.res.tb_ok, "TB verdict");
  chk(!memcmp(r.seg_ok, ref.res.seg_ok, fx.tb.C), "segment verdicts");
  if (r.tb_ok && ref.res.tb_ok) {
    chk(!memcmp(sl.tb.data(), ref.tb.data(), (size_t)fx.tb.A / 8), "TB bytes");
    chk(!memcmp(sl.tb.data(), fx.tb.payload.data(), (size_t)fx.tb.A / 8), "TB differs from the transmitted one");
  }
  /* the fold of the old path's noise: nvar = mean over the DM-RS symbols of the per-symbol mean */
  uint32_t nv = 0;
  for (int k = 0; k < 2; k++) {
    uint64_t t = 0;
    for (int a = 0; a < 4; a++) t += ref.acc[k][a];
    nv += (uint32_t)(t / (uint64_t)(4 * 12 * fx.nrb));
  }
  chk(sl.out.nvar == nv / 2, "nvar fold");
  return bad ? -1 : (ref.res.tb_ok ? 1 : 0);
}

TEST(GpuPassUl, MatchesCpuDecode) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(m.pass_try && m.pass_warm) << "libpdsch_gpu.so has no PUSCH pass";
  ASSERT_TRUE(ul_ready(m)) << "UL pass setup failed";
  static const int nrbs[] = {6, 12, 24, 48, 50, 96, 106, 136, 217};
  static const int qms[] = {2, 4, 6, 8};
  static const int tas[] = {0, 3, -4, 9, 0, 5};
  int n = 0, dec_ok = 0, dec_fail = 0, skipped = 0;
  const int cases = getenv("NR_GPU_UL_CASES") ? atoi(getenv("NR_GPU_UL_CASES")) : 200;
  for (int seed = 0; seed < cases; seed++) {
    const int nrb = nrbs[seed % 9], qm = qms[(seed / 3) % 4], ta = tas[seed % 6], keep = (seed % 5 == 0) ? 0 : -1;
    const double snr = (seed % 7 == 6) ? 4.0 : (seed % 11 == 10) ? -8.0 : (seed % 13 == 12) ? -18.0 : 20.0; /* low and very low SNR slots (the live UL is at -5 .. -30 dB): the estimates and the CRC verdicts must agree there too */
    char what[160];
    snprintf(what, sizeof(what), "seed %d nrb %d qm %d ta %d keep %d snr %.0f first %d", seed, nrb, qm, ta, keep, snr, (seed % 4 == 3) ? 0 : 1);
    const int first = (seed % 4 == 3) ? 0 : 1; /* a quarter of the slots carry a PUSCH that starts at symbol 0 */
    const int r = ul_case(m, nrb, qm, seed, ta, keep, snr, what, first);
    if (r == 2) { skipped++; continue; }
    ASSERT_GE(r, 0) << what;
    n++;
    dec_ok += r == 1;
    dec_fail += r == 0;
    if (HasFailure()) break;
  }
  printf("GpuPassUl.MatchesCpuDecode: %d slots compared bit for bit (decoded %d, CRC fail on both paths %d, %d shapes without a TB), %s memory model\n", n, dec_ok, dec_fail, skipped,
         nr_gpu_model() == NR_GPU_MODEL_DIRECT ? "DIRECT" : "DISCRETE");
  EXPECT_GE(n, cases * 2 / 3);
  EXPECT_GE(dec_ok, n / 2);
}

/* the comparison is not vacuous: a reference with another timing advance, or another receive branch, is flagged on every slot */
TEST(GpuPassUl, ReferenceDetectsMismatch) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(ul_ready(m));
  g_ul_neg = true;
  for (int mode = 0; mode < 2; mode++) {
    g_ul_ref_ta_delta = mode == 0 ? 2 : 0;
    g_ul_ref_keep_flip = mode == 1;
    for (int seed = 100; seed < 106; seed++) {
      g_ul_neg_count = 0;
      const int r = ul_case(m, 48, 4, seed, 3, -1, 20.0, "negative control");
      EXPECT_NE(2, r);
      EXPECT_GT(g_ul_neg_count, 0) << "mode " << mode << " seed " << seed << ": a perturbed reference went unnoticed";
    }
  }
  g_ul_neg = false;
  g_ul_ref_ta_delta = 0;
  g_ul_ref_keep_flip = 0;
}

TEST(GpuPassUl, TransformPrecodingFallsBack) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(ul_ready(m));
  const nr_gpu_pass_shape_t sh = {24, 13, 1, 2, 2, 1, 1};
  GpuPassFixture fx = GpuPassFixture::from_shape(sh, 5);
  ASSERT_TRUE(fx.tb_ok_built);
  ul_warm_class(m, 24, 2, fx.tb.C);
  ASSERT_FALSE(HasFatalFailure());
  UlSlot sl;
  sl.build(fx, 0, -1, 0, 0);
  uint64_t o0[16], o1[16];
  m.pass_counters(o0);
  void *dl = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_NOLOAD);
  auto ulc = (void (*)(uint64_t *))dlsym(dl, "nr_gpu_pass_ul_counters");
  ASSERT_TRUE(ulc);
  uint64_t u0[16], u1[16];
  ulc(u0);
  sl.in.transform_precoding = 1;
  EXPECT_EQ(1, nr_gpu_pass_ul_run(&sl.in, &sl.out)) << "DFT-s-OFDM must stay on the old path";
  sl.in.transform_precoding = 0;
  sl.in.frequency_hopping = 1;
  EXPECT_EQ(1, nr_gpu_pass_ul_run(&sl.in, &sl.out)) << "frequency hopping must stay on the old path";
  sl.in.frequency_hopping = 0;
  sl.in.rv = 2;
  EXPECT_EQ(1, nr_gpu_pass_ul_run(&sl.in, &sl.out)) << "a redundancy version other than 0 needs HARQ combining";
  sl.in.rv = 0;
  sl.in.dmrs_config_type = 1;
  EXPECT_EQ(1, nr_gpu_pass_ul_run(&sl.in, &sl.out)) << "DM-RS type 2 is not in the pass";
  sl.in.dmrs_config_type = 0;
  ulc(u1);
  EXPECT_EQ(u0[0], u1[0]) << "no graph may have been launched for a refused grant";
  EXPECT_EQ(0, nr_gpu_pass_ul_run(&sl.in, &sl.out)) << "the same slot with the refusals removed takes the pass";
  EXPECT_TRUE(sl.out.r.tb_ok);
  ulc(u1);
  EXPECT_EQ(u0[0] + 1, u1[0]);
  (void)o1;
}

TEST(GpuPassUl, CacheMissUsesOldPath) {
  uint64_t h0, m0, s0, p0, h1, m1, s1, p1;
  ul_hyp_cache_clear();
  nr_gpu_pass_ul_cache_counts(&h0, &m0, &s0, &p0);
  ul_hyp_t h = {};
  EXPECT_EQ(0, ul_hyp_cache_get(0x4242, &h)) << "unknown RNTI";
  EXPECT_EQ(0, nr_gpu_pass_ul_hyp_lookup(0x4242, 7, 1000, &h)) << "unknown RNTI: the old path decodes";
  nr_gpu_pass_ul_cache_counts(&h1, &m1, &s1, &p1);
  EXPECT_EQ(m0 + 1, m1) << "ul_cache_miss";
  EXPECT_EQ(h0, h1);
  h.ta = 123; h.dmrs_id = 321; h.have_dmrs = 1; h.epoch = 7; h.abs_slot = 1000;
  ul_hyp_cache_put(0x4242, &h);
  ul_hyp_t g = {};
  EXPECT_EQ(1, ul_hyp_cache_get(0x4242, &g));
  EXPECT_EQ(123, g.ta);
  EXPECT_EQ(321, g.dmrs_id);
  EXPECT_EQ(1, nr_gpu_pass_ul_hyp_lookup(0x4242, 7, 1010, &g)) << "a locked grant of a known RNTI takes the pass";
  EXPECT_EQ(0, nr_gpu_pass_ul_hyp_lookup(0x4242, 8, 1010, &g)) << "another configuration epoch: stale";
  EXPECT_EQ(0, nr_gpu_pass_ul_hyp_lookup(0x4242, 7, 1000 + 5000, &g)) << "older than the maximum age: stale";
  EXPECT_EQ(0, nr_gpu_pass_ul_hyp_lookup(0x4243, 7, 1010, &g)) << "another RNTI";
  nr_gpu_pass_ul_cache_counts(&h1, &m1, &s1, &p1);
  EXPECT_EQ(h0 + 1, h1);
  EXPECT_EQ(m0 + 2, m1);
  EXPECT_EQ(s0 + 2, s1);
  EXPECT_EQ(p0 + 1, p1);
  /* the entry is replaced in place, a different RNTI gets its own, 64 entries then evict the least recently written */
  h.ta = 5;
  ul_hyp_cache_put(0x4242, &h);
  EXPECT_EQ(1, ul_hyp_cache_get(0x4242, &g));
  EXPECT_EQ(5, g.ta);
  for (int r = 1; r <= 70; r++) { h.ta = r; ul_hyp_cache_put((uint16_t)(0x5000 + r), &h); }
  EXPECT_EQ(0, ul_hyp_cache_get(0x4242, &g)) << "the oldest entry was evicted";
  EXPECT_EQ(1, ul_hyp_cache_get(0x5000 + 70, &g));
  EXPECT_EQ(70, g.ta);
  ul_hyp_cache_drop(0x5000 + 70);
  EXPECT_EQ(0, ul_hyp_cache_get(0x5000 + 70, &g));
  ul_hyp_cache_clear();
  EXPECT_EQ(0, ul_hyp_cache_get(0x5000 + 69, &g));
}

TEST(GpuPassUl, UnseenClassAndConcurrency) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(ul_ready(m));
  /* a shape the warm-up never built (ndmrs 4 at 6 PRB): the host builder hands the grant back */
  const nr_gpu_pass_shape_t sh = {12, 13, 1, 2, 2, 1, 1};
  GpuPassFixture fx = GpuPassFixture::from_shape(sh, 9);
  ASSERT_TRUE(fx.tb_ok_built);
  void *dl = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_NOLOAD);
  auto ulc = (void (*)(uint64_t *))dlsym(dl, "nr_gpu_pass_ul_counters");
  ASSERT_TRUE(ulc);
  UlSlot sl;
  sl.build(fx, 0, -1, 0, 0);
  uint64_t u0[16], u1[16];
  ulc(u0);
  nr_gpu_pass_ul_in_t in2 = sl.in;
  in2.ul_dmrs_symb_pos = (1u << 2) | (1u << 5) | (1u << 8) | (1u << 11); /* four DM-RS symbols: ndmrs 4, never warmed */
  nr_gpu_pass_ul_out_t out2;
  EXPECT_EQ(1, nr_gpu_pass_ul_run(&in2, &out2));
  ulc(u1);
  EXPECT_EQ(u0[2] + 1, u1[2]) << "unseen class counted by the module";
  /* eight threads, each its own decode context, same slot: every pass decodes the same TB */
  ul_warm_class(m, 12, 2, fx.tb.C);
  ASSERT_FALSE(HasFatalFailure());
  std::atomic<int> good{0}, fail{0};
  std::vector<std::thread> th;
  for (int t = 0; t < 8; t++)
    th.emplace_back([&, t]() {
      UlSlot s;
      s.build(fx, 0, -1, 0, t);
      for (int i = 0; i < 6; i++) {
        s.tb.assign(s.tb.size(), 0xcc);
        const int rc = nr_gpu_pass_ul_run(&s.in, &s.out);
        if (rc == 1) continue; /* all four instances busy: the old path would decode it */
        if (rc == 0 && s.out.r.tb_ok && !memcmp(s.tb.data(), fx.tb.payload.data(), fx.tb.A / 8)) good++;
        else fail++;
      }
    });
  for (auto &t : th) t.join();
  EXPECT_EQ(0, fail.load());
  EXPECT_GT(good.load(), 8);
}

/* need_cpu (zero syndrome with a failing CRC): the pass left no LLRs, so the call site must take the old path from scratch; a shadowed pass is only compared */
TEST(GpuPassUl, NeedCpuMeansOldPath) {
  nr_gpu_pass_ul_out_t o = {};
  o.r.need_cpu = 1;
  EXPECT_EQ(0, nr_gpu_pass_ul_accept(&o, 0)) << "need_cpu must not be used";
  EXPECT_EQ(1, nr_gpu_pass_ul_accept(&o, 1)) << "a shadowed pass is compared, not used";
  o.r.need_cpu = 0;
  EXPECT_EQ(1, nr_gpu_pass_ul_accept(&o, 0));
}
/* the fail streak is one atomic step: three fails drop the entry, a refresh in between restarts it */
TEST(GpuPassUl, FailStreakIsAtomic) {
  ul_hyp_cache_clear();
  ul_hyp_t h = {};
  h.ta = 7; h.epoch = 1;
  ul_hyp_cache_put(0x77, &h);
  ul_hyp_cache_note_fail(0x77, 3);
  ul_hyp_cache_note_fail(0x77, 3);
  ul_hyp_t g;
  ASSERT_EQ(1, ul_hyp_cache_get(0x77, &g));
  EXPECT_EQ(2, g.fail_streak);
  ul_hyp_cache_put(0x77, &h); /* a CRC-OK refresh */
  ul_hyp_cache_note_fail(0x77, 3);
  ASSERT_EQ(1, ul_hyp_cache_get(0x77, &g));
  EXPECT_EQ(1, g.fail_streak);
  ul_hyp_cache_note_fail(0x77, 3);
  ul_hyp_cache_note_fail(0x77, 3);
  EXPECT_EQ(0, ul_hyp_cache_get(0x77, &g)) << "dropped at three";
  ul_hyp_cache_note_fail(0x99, 3); /* unknown RNTI: nothing */
  std::vector<std::thread> th;
  ul_hyp_cache_put(0x78, &h);
  for (int t = 0; t < 4; t++) th.emplace_back([]() { for (int i = 0; i < 1000; i++) ul_hyp_cache_note_fail(0x78, 1 << 30); });
  for (auto &t : th) t.join();
  ASSERT_EQ(1, ul_hyp_cache_get(0x78, &g));
  EXPECT_EQ(4000 & 255, g.fail_streak) << "no lost increments (uint8 streak wraps)";
}

/* ===================================================== DL GPU data path v2, Task 1 (ISAC_PDSCH_GPU_GRAPH) ===================================================== */
#include "nr_gpu_graph_pool.h"
/* v2: the pass's LLR statistics equal the stage path's host loops (passive_decode_body_core, demod_ok block) for every warm shape, CRC ok and CRC fail. */
TEST(GpuGraph, LlrStatsEqualStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m));
  int n = 0, n_fail = 0;
  for (auto shape : GpuPassFixture::warm_shapes()) {
    for (double snr : {20.0, -3.0}) { /* decodes / fails */
      GpuPassFixture::noise_db = snr;
      GpuPassFixture fx = GpuPassFixture::from_shape(shape, 13);
      ASSERT_TRUE(fx.tb_ok_built);
      fx.bind();
      std::vector<uint8_t> gb(fx.tb.A / 8 + 64), sb(fx.tb.A / 8 + 64);
      nr_gpu_pass_result_t g{}, s{};
      g.tb = gb.data(); s.tb = sb.data();
      ASSERT_EQ(0, m.pass_try(fx.job, &g));
      ASSERT_EQ(0, fx.run_stage_path(&s));
      const nr_gpu_llr_stats_t ref = fx.host_llr_stats();
      ASSERT_GT(ref.llr_n, 0u);
      EXPECT_EQ(0, memcmp(&ref, &g.llr_stats, sizeof(ref))) << "nrb " << shape.nrb_class << " nl " << shape.nl << " qm " << shape.qm << " snr " << snr
                                                            << ": n " << g.llr_stats.llr_n << "/" << ref.llr_n << " absum " << g.llr_stats.llr_absum << "/" << ref.llr_absum
                                                            << " sgnsum " << g.llr_stats.llr_sgnsum << "/" << ref.llr_sgnsum << " pos " << g.llr_stats.llr_pos << "/" << ref.llr_pos;
      n++;
      n_fail += !g.tb_ok;
    }
  }
  GpuPassFixture::noise_db = 20.0;
  printf("GpuGraph.LlrStatsEqualStagePath: %d passes, %d CRC fail\n", n, n_fail);
  EXPECT_GT(n_fail, 0) << "the -3 dB arm should fail some CRCs";
  /* odd G: the stride-32 and i + 3 < G edges of the loops (G = 3: magnitudes only; 33, 35: a second sample, sign stats of one; 0: the pass refuses the job) */
  GpuPassFixture fx = GpuPassFixture::from_shape(GpuPassFixture::warm_shapes()[4 * 4 + 1], 13);
  ASSERT_TRUE(fx.tb_ok_built);
  nr_gpu_pass_result_t s0{};
  std::vector<uint8_t> sb(fx.tb.A / 8 + 64), gb(fx.tb.A / 8 + 64);
  s0.tb = sb.data();
  ASSERT_EQ(0, fx.run_stage_path(&s0));
  const std::vector<int16_t> pre = fx.llr_pre, post = fx.llr_post;
  for (uint32_t G : {3u, 33u, 35u, 0u}) {
    fx.bind();
    fx.job->G = G;
    fx.job->norm = 0;
    nr_gpu_pass_result_t g{};
    g.tb = gb.data();
    if (!G) {
      EXPECT_EQ(1, m.pass_try(fx.job, &g)) << "G = 0 is refused: no statistics are produced or read";
      continue;
    }
    ASSERT_EQ(0, m.pass_try(fx.job, &g)) << "G " << G;
    fx.llr_pre.assign(pre.begin(), pre.begin() + G);
    fx.llr_post.assign(post.begin(), post.begin() + G);
    const nr_gpu_llr_stats_t ref = fx.host_llr_stats();
    EXPECT_EQ(0, memcmp(&ref, &g.llr_stats, sizeof(ref))) << "G " << G << ": n " << g.llr_stats.llr_n << "/" << ref.llr_n << " nbit " << g.llr_stats.nbit[0] << "/" << ref.nbit[0];
  }
}
/* v2: a thread that reaches the pass late (the async recovery worker) claims a prewarmed buffer set instead of hitting the nomem gate; an empty pool returns NULL,
 * counted, and nothing is allocated on the GPU (the claim only pops a pre-filled stack). */
TEST(GpuGraph, LateThreadClaimsPrewarmedPool) {
  static nr_gpu_graph_pool_t pool;
  ASSERT_EQ(2, nr_gpu_graph_pool_fill(&pool, 2, 4096, 1024)); /* pre-fence */
  uint64_t a0 = 0, d0 = 0, a1 = 0, d1 = 0;
  nr_gpu_memory_stats(&a0, &d0);
  nr_gpu_graph_set_t *s1 = nullptr, *s2 = nullptr, *s3 = nullptr;
  std::thread t([&] { s1 = nr_gpu_graph_pool_take(&pool); });
  t.join();
  ASSERT_NE(nullptr, s1);
  EXPECT_TRUE(s1->out && s1->job && s1->llr && s1->shadow);
  s2 = nr_gpu_graph_pool_take(&pool);
  ASSERT_NE(nullptr, s2);
  EXPECT_NE(s1, s2) << "two threads never share a set";
  s3 = nr_gpu_graph_pool_take(&pool);
  EXPECT_EQ(nullptr, s3) << "pool exhausted -> NULL, counted, never an allocation";
  EXPECT_EQ(2u, pool.claimed);
  EXPECT_EQ(1u, pool.empty);
  nr_gpu_memory_stats(&a1, &d1);
  EXPECT_EQ(a0, a1);
  EXPECT_EQ(d0, d1);
  /* concurrent claims: every set handed out once */
  static nr_gpu_graph_pool_t p8;
  ASSERT_EQ(NR_GPU_GRAPH_POOL, nr_gpu_graph_pool_fill(&p8, NR_GPU_GRAPH_POOL, 64, 64));
  std::atomic<int> got{0};
  std::vector<std::thread> th;
  std::vector<nr_gpu_graph_set_t *> seen(16, nullptr);
  for (int i = 0; i < 16; i++) th.emplace_back([&, i] { seen[i] = nr_gpu_graph_pool_take(&p8); if (seen[i]) got++; });
  for (auto &x : th) x.join();
  EXPECT_EQ(NR_GPU_GRAPH_POOL, got.load());
  EXPECT_EQ((uint64_t)NR_GPU_GRAPH_POOL, p8.claimed);
  std::sort(seen.begin(), seen.end());
  EXPECT_EQ(seen.end(), std::adjacent_find(seen.begin() + (16 - NR_GPU_GRAPH_POOL), seen.end()));
  EXPECT_EQ(16u - NR_GPU_GRAPH_POOL, p8.empty);
  /* the claim of nr_gpu_graph_pool_claim: v2 off claims nothing and counts nothing; a released set (its thread exited) is served again */
  EXPECT_EQ(nullptr, nr_gpu_graph_pool_claim_set(&pool, 0));
  EXPECT_EQ(2u, pool.claimed);
  EXPECT_EQ(1u, pool.empty);
  EXPECT_FALSE(nr_gpu_graph_pool_avail(&pool));
  nr_gpu_graph_pool_put(&pool, s1);
  EXPECT_TRUE(nr_gpu_graph_pool_avail(&pool));
  EXPECT_EQ(s1, nr_gpu_graph_pool_claim_set(&pool, 1));
  EXPECT_EQ(1u, pool.released);
  EXPECT_EQ(3u, pool.claimed);
}

/* ===================================================== v2 Task 2: probes on the pass (ISAC_PDSCH_GPU_GRAPH_COVER) ===================================================== */
/* a layout probe (code block 0 only, the body's symbol horizon) through the pass equals the stage path's probe body: seg_ok[0], need_cpu, the final LLRs
 * (zeros past the horizon) and the LLR statistics; nothing past block 0 is reported and the TB is never ok when C > 1. CRC ok and CRC fail. */
TEST(GpuGraphCover, ProbeEqualsStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m) && m.pass_warm);
  int n = 0, n_hor = 0, n_ok = 0;
  for (auto shape : GpuPassFixture::warm_shapes()) {
    for (double snr : {20.0, -3.0}) {
      GpuPassFixture::noise_db = snr;
      GpuPassFixture fx = GpuPassFixture::from_shape(shape, 17);
      ASSERT_TRUE(fx.tb_ok_built);
      fx.make_probe();
      nr_gpu_pass_shape_t w = fx.job->shape; /* the probe's key: the class of one code block (warmed by nr_gpu_pass_warmup for every PRB class) */
      ASSERT_EQ(0, m.pass_warm(&w, shape.nl >= 2 ? 3 : 0, 0, 0));
      ASSERT_EQ(0, fx.job->llr_out_all) << "the body's setting: a probe's LLRs reach llr_out without it"; /* (a CRC-ok C = 1 probe left llr_out stale: replay 2026-10-17) */
      std::vector<uint8_t> gb(fx.tb.A / 8 + 64), sb(fx.tb.A / 8 + 64);
      nr_gpu_pass_result_t g{}, s{};
      g.tb = gb.data(); s.tb = sb.data();
      const std::string what = "nrb " + std::to_string(shape.nrb_class) + " nl " + std::to_string(shape.nl) + " qm " + std::to_string(shape.qm) + " C " + std::to_string(fx.tb.C) +
                               " horizon " + std::to_string(fx.probe_last) + " snr " + std::to_string(snr);
      ASSERT_EQ(0, m.pass_try(fx.job, &g)) << what;
      ASSERT_EQ(0, fx.run_stage_path(&s)) << what;
      EXPECT_EQ(s.seg_ok[0], g.seg_ok[0]) << what;
      EXPECT_EQ(s.need_cpu, g.need_cpu) << what;
      for (int r = 1; r < NR_GPU_PASS_MAX_CB; r++) ASSERT_EQ(0, g.seg_ok[r]) << what << " block " << r;
      EXPECT_EQ(0, g.tb_ok) << what;
      EXPECT_EQ(0, memcmp(fx.llr_ref.data(), fx.llr_pass.data(), (size_t)fx.tb.G * 2)) << what << ": final LLRs";
      const nr_gpu_llr_stats_t ref = fx.host_llr_stats();
      EXPECT_EQ(0, memcmp(&ref, &g.llr_stats, sizeof(ref))) << what << ": llr_n " << g.llr_stats.llr_n << "/" << ref.llr_n << " zero " << g.llr_stats.llr_zero << "/" << ref.llr_zero;
      if (g.seg_ok[0] && fx.tb.C > 1) EXPECT_EQ(0, memcmp(gb.data(), fx.tb.payload.data(), (fx.tb.K >> 3) - (fx.tb.F >> 3) - 3)) << what << ": block 0 bytes";
      n++;
      n_hor += fx.probe_last >= 0;
      n_ok += g.seg_ok[0];
    }
  }
  GpuPassFixture::noise_db = 20.0;
  printf("GpuGraphCover.ProbeEqualsStagePath: %d probes, %d with a horizon, %d block-0 CRC ok\n", n, n_hor, n_ok);
  EXPECT_GT(n_hor, 0);
  EXPECT_GT(n_ok, 0);
  EXPECT_LT(n_ok, n) << "the -3 dB arm should fail some";
}
/* v2 Task 2 review: a probe never takes the last free instance (ISAC_PDSCH_GPU_GRAPH_PROBE_RESERVE, default 1): with one instance free a probe is refused and counted
 * probe_busy (o[15]), not busy (o[3]); a real grant still gets it. */
/* Fix A+D pools: DL real grants, UL real grants and probes own disjoint instances. (a) a probe runs on the probe instance while the DL pool has two free instances; (b) with the DL pool
 * full a real grant takes the overflow (probe) instance AT ONCE (no wait, no old path); (c) a probe is refused when fewer than two DL instances are free; (d) with the DL pool full and
 * a probe still running, the grant waits for that probe only (bounded, 400 us) and decodes when it ends; if the probe outlasts the wait the old path takes it, counted busy. */
TEST(GpuGraphCover, PoolsAreDisjointAndProbesYield) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m) && m.pass_warm && m.pass_hold);
  constexpr unsigned dl_mask = (1u << NR_GPU_PASS_DL) - 1u, probe_bit = ((1u << NR_GPU_PASS_PROBE) - 1u) << (NR_GPU_PASS_DL + NR_GPU_PASS_UL);
  const auto shape = GpuPassFixture::warm_shapes()[0];
  GpuPassFixture fx = GpuPassFixture::from_shape(shape, 23);
  ASSERT_TRUE(fx.tb_ok_built);
  GpuPassFixture px = GpuPassFixture::from_shape(shape, 23);
  px.make_probe();
  nr_gpu_pass_shape_t w = px.job->shape;
  ASSERT_EQ(0, m.pass_warm(&w, shape.nl >= 2 ? 3 : 0, 0, 0));
  std::vector<uint8_t> gb(fx.tb.A / 8 + 64);
  nr_gpu_pass_result_t g{};
  g.tb = gb.data();
  uint64_t o0[16], o1[16];
  m.pass_hold(dl_mask & 3u, 1); // two DL instances busy, two free
  px.bind();
  EXPECT_EQ(0, m.pass_try(px.job, &g)) << "(a) the probe was refused with two DL instances free";
  m.pass_hold(dl_mask & 3u, 0);
  m.pass_hold(dl_mask & 7u, 1); // one free
  m.pass_counters(o0);
  px.bind();
  EXPECT_EQ(1, m.pass_try(px.job, &g)) << "(c) a probe ran with one DL instance free";
  m.pass_counters(o1);
  EXPECT_EQ(o0[15] + 1, o1[15]) << "probe_busy";
  m.pass_hold(dl_mask & 7u, 0);
  m.pass_hold(dl_mask, 1);
  fx.bind();
  m.pass_counters(o0);
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_EQ(0, m.pass_try(fx.job, &g)) << "(b) the real grant did not take the overflow instance";
  const double dt = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  m.pass_counters(o1);
  EXPECT_EQ(o0[3], o1[3]) << "(b) no busy fallback";
  EXPECT_EQ(NR_GPU_PASS_DL + NR_GPU_PASS_UL, (int)g.instance) << "(b) ran on the probe-pool instance";
  printf("GpuGraphCover.Pools: overflow grant wall %.0f us\n", dt);
  // (d) the probe instance is busy with a (simulated) running probe
  int rc_d = 1;
  for (int att = 0; att < 5 && rc_d != 0; att++) { // the releasing thread can be descheduled for longer than the 400 us wait on a loaded host: one success of five tries shows the wait works
    m.pass_hold(probe_bit, 1);
    std::atomic<bool> go{false};
    std::thread rel([&] { while (!go) {} const auto t = std::chrono::steady_clock::now(); while (std::chrono::steady_clock::now() - t < std::chrono::microseconds(60)) {} m.pass_hold(probe_bit, 0); });
    go = true;
    rc_d = m.pass_try(fx.job, &g);
    rel.join();
    m.pass_hold(probe_bit, 0);
  }
  EXPECT_EQ(0, rc_d) << "(d) the grant never waited for the running probe in five tries";
  m.pass_hold(probe_bit, 1);
  m.pass_counters(o0);
  EXPECT_EQ(1, m.pass_try(fx.job, &g)) << "(d) a probe that outlasts the bounded wait sends the grant to the old path";
  m.pass_counters(o1);
  EXPECT_EQ(o0[3] + 1, o1[3]) << "(d) counted busy";
  m.pass_hold(dl_mask | probe_bit, 0);
}
/* a slot-share grant: the estimate covers the group's union above the grant (share_chest_band), the demod reads the grant's REs; equal to the stage path's
 * share body (TB, seg_ok, final LLRs, nvar, LLR statistics), alone and as a probe. */
TEST(GpuGraphCover, ShareWidenedChestEqualsStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m) && m.pass_warm);
  int n = 0;
  for (int w : {7, 20})
    for (int si : {1, 16 + 6, 32 + 11, 48 + 5}) /* warm_shapes(): 24 PRB nl 1 qm 4, 48 nl 2 qm 6, 96 nl 3 qm 8, 217 nl 2 qm 4 */
      for (int probe : {0, 1}) {
        const nr_gpu_pass_shape_t shape = GpuPassFixture::warm_shapes()[si];
        if (shape.nrb_class + w > 263) continue; /* the band stays inside the carrier (start_rb 10) */
        GpuPassFixture::widen_rb = w;
        GpuPassFixture fx = GpuPassFixture::from_shape(shape, 23);
        GpuPassFixture::widen_rb = 0;
        ASSERT_TRUE(fx.tb_ok_built);
        if (probe) fx.make_probe();
        nr_gpu_pass_shape_t sw = fx.job->shape;
        ASSERT_EQ(0, m.pass_warm(&sw, shape.nl >= 2 ? 3 : 0, 0, 0));
        std::vector<uint8_t> gb(fx.tb.A / 8 + 64, 0xAA), sb(fx.tb.A / 8 + 64, 0x55);
        nr_gpu_pass_result_t g{}, s{};
        g.tb = gb.data(); s.tb = sb.data();
        const std::string what = "nrb " + std::to_string(shape.nrb_class) + " +" + std::to_string(w) + " nl " + std::to_string(shape.nl) + " probe " + std::to_string(probe);
        ASSERT_EQ(0, m.pass_try(fx.job, &g)) << what;
        ASSERT_EQ(0, fx.run_stage_path(&s)) << what;
        EXPECT_EQ(s.nvar, g.nvar) << what;
        EXPECT_EQ(probe ? 0 : s.tb_ok, g.tb_ok) << what; /* a probe is never a decoded TB */
        EXPECT_EQ(0, memcmp(s.seg_ok, g.seg_ok, sizeof(s.seg_ok))) << what;
        if (!probe) EXPECT_TRUE(g.tb_ok) << what << ": 20 dB decodes";
        if (g.tb_ok) EXPECT_EQ(0, memcmp(gb.data(), sb.data(), fx.tb.A / 8)) << what;
        else EXPECT_EQ(0, memcmp(fx.llr_ref.data(), fx.llr_pass.data(), (size_t)fx.tb.G * 2)) << what << ": final LLRs";
        const nr_gpu_llr_stats_t ref = fx.host_llr_stats();
        EXPECT_EQ(0, memcmp(&ref, &g.llr_stats, sizeof(ref))) << what;
        n++;
      }
  printf("GpuGraphCover.ShareWidenedChestEqualsStagePath: %d grants\n", n);
  EXPECT_GE(n, 12);
}
/* a probe whose key was never warmed: dl_try returns 1 and counts it unseen; nothing is allocated (platform or module) */
TEST(GpuGraphCover, ProbeUnseenKeyFallsBackWithoutAlloc) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m));
  GpuPassFixture fx = GpuPassFixture::from_shape({6, 13, 4, 2, 2, 1, 1}, 19); /* PRB class 6: no test warms it */
  ASSERT_TRUE(fx.tb_ok_built);
  fx.make_probe();
  std::vector<uint8_t> gb(fx.tb.A / 8 + 64);
  nr_gpu_pass_result_t g{};
  g.tb = gb.data();
  uint64_t a0, d0, a1, d1, o0[16], o1[16];
  nr_gpu_memory_stats(&a0, &d0);
  m.pass_counters(o0);
  EXPECT_EQ(1, m.pass_try(fx.job, &g));
  m.pass_counters(o1);
  nr_gpu_memory_stats(&a1, &d1);
  EXPECT_EQ(o0[2] + 1, o1[2]) << "counted unseen";
  EXPECT_EQ(o0[10], o1[10]) << "the module built (allocated) a graph";
  EXPECT_EQ(o0[9], o1[9]);
  EXPECT_EQ(a0, a1) << "the platform allocated";
  EXPECT_EQ(d0, d1);
}

/* ===================================================== DL GPU data path v2, Task 3 (ISAC_PDSCH_GPU_GRAPH_SPLIT) ===================================================== */
/* the C, D and L sub-graphs launched back to back equal the single v1 graph and the stage path, every warm shape, CRC ok and CRC fail: TB, seg_ok, CRC verdict,
 * need_cpu, nvar and per-antenna nvar, the chest post outputs, LLR statistics, the final LLRs of a failing TB and the caller's rxdataF. */
TEST(GpuGraphSplit, ThreeLaunchesEqualSingleGraphAndStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m) && m.pass_warm && m.pass_split);
  int n = 0, n_fail = 0;
  uint64_t o0[16], o1[16];
  for (auto shape : GpuPassFixture::warm_shapes()) {
    for (double snr : {20.0, -3.0}) {
      GpuPassFixture::noise_db = snr;
      GpuPassFixture fx = GpuPassFixture::from_shape(shape, 11);
      ASSERT_TRUE(fx.tb_ok_built);
      fx.bind();
      fx.job->llr_out_all = 1; /* the final LLRs of every pass, CRC ok too */
      const std::string what = "nrb " + std::to_string(shape.nrb_class) + " nl " + std::to_string(shape.nl) + " qm " + std::to_string(shape.qm) + " snr " + std::to_string(snr);
      std::vector<uint8_t> b1(fx.tb.A / 8 + 64, 0xAA), bg(fx.tb.A / 8 + 64, 0x5A), bs(fx.tb.A / 8 + 64, 0x55);
      nr_gpu_pass_result_t one{}, g{}, s{};
      one.tb = b1.data(); g.tb = bg.data(); s.tb = bs.data();
      m.pass_split(0);
      ASSERT_EQ(0, m.pass_try(fx.job, &one)) << what << ": single graph";
      const std::vector<int16_t> llr_one = fx.llr_pass, rxF_one = fx.rxF_out;
      m.pass_split(1);
      nr_gpu_pass_shape_t w = fx.job->shape;
      ASSERT_EQ(0, m.pass_warm(&w, fx.job->dj.qam_mode, fx.job->dj.align, fx.job->dj.want_evm)) << what;
      m.pass_counters(o0);
      ASSERT_EQ(0, m.pass_try(fx.job, &g)) << what << ": split";
      m.pass_counters(o1);
      m.pass_split(0);
      EXPECT_EQ(o0[1] + 1, o1[1]) << what << ": a graph hit";
      EXPECT_EQ(o0[2] + o0[3] + o0[4] + o0[5], o1[2] + o1[3] + o1[4] + o1[5]) << what << ": no fallback";
      ASSERT_EQ(0, fx.run_stage_path(&s)) << what;
      for (const nr_gpu_pass_result_t *r : {&s, &one}) {
        const char *ref = r == &s ? " vs stage path" : " vs single graph";
        EXPECT_EQ(r->need_cpu, g.need_cpu) << what << ref;
        EXPECT_EQ(r->tb_ok, g.tb_ok) << what << ref;
        EXPECT_EQ(r->nvar, g.nvar) << what << ref;
        EXPECT_EQ(0, memcmp(r->nvar_ant, g.nvar_ant, sizeof(g.nvar_ant))) << what << ref;
        EXPECT_EQ(0, memcmp(r->seg_ok, g.seg_ok, sizeof(g.seg_ok))) << what << ref;
        EXPECT_EQ(0, memcmp(r->tb, g.tb, g.tbs / 8)) << what << ref;
      }
      EXPECT_EQ(0, memcmp(&one.post, &g.post, sizeof(g.post))) << what << ": chest post outputs";
      EXPECT_EQ(0, memcmp(&one.llr_stats, &g.llr_stats, sizeof(g.llr_stats))) << what << ": LLR statistics";
      const nr_gpu_llr_stats_t ref = fx.host_llr_stats();
      EXPECT_EQ(0, memcmp(&ref, &g.llr_stats, sizeof(ref))) << what << ": LLR statistics vs the host loops";
      EXPECT_EQ(0, memcmp(llr_one.data(), fx.llr_pass.data(), (size_t)fx.tb.G * 2)) << what << ": final LLRs vs single graph";
      if (!g.tb_ok && !g.need_cpu) {
        EXPECT_EQ(0, memcmp(fx.llr_ref.data(), fx.llr_pass.data(), (size_t)fx.tb.G * 2)) << what << ": final LLRs vs stage path";
        n_fail++;
      }
      EXPECT_TRUE(rxF_one == fx.rxF_out) << what << ": rxdataF";
      for (int a = 0; a < 4; a++)
        for (int sy = 0; sy < 14; sy++)
          ASSERT_EQ(0, memcmp(&fx.rxF_out[2 * ((size_t)a * GpuPassFixture::RXSTRIDE + (size_t)sy * 4096)], &fx.rxF_ref[2 * ((size_t)a * 14 * 4096 + (size_t)sy * 4096)], 4096 * 4))
              << "rxdataF, ant " << a << " sym " << sy << ", " << what;
      n++;
    }
  }
  GpuPassFixture::noise_db = 20.0;
  printf("GpuGraphSplit.ThreeLaunchesEqualSingleGraphAndStagePath: %d passes, %d CRC fail\n", n, n_fail);
  EXPECT_GT(n_fail, 0);
}
/* segmented grants of 17-32 segments (class 32, split only): the split pass equals the stage path's segmented body; the single-graph mode refuses the class */
TEST(GpuGraphSplit, ThirtyTwoSegmentsEqualStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m) && m.pass_warm && m.pass_split);
  const SegCase cases[] = {
      {"prg2 64 PRB: 32 segs rank1", 1, 4, 64, 0, 2, 0, 0},
      {"prg2 34 PRB: 17 segs rank1", 1, 2, 34, 0, 2, 0, 0},
      {"interleaved L2 60 PRB in 273: 30 segs rank1", 1, 4, 60, 0, 0, 2, 273},
      {"interleaved L2 + BWPStart 12 refPoint 0, 24 segs rank1", 1, 6, 48, 0, 0, 2, 200, 12, 0},
  };
  int n = 0, decoded = 0;
  for (const SegCase &c : cases) {
    const auto sd = seg_layout(c.n, c.start, c.prg, c.interleave, c.bwp);
    ASSERT_GT(sd.size(), 16u) << c.name;
    ASSERT_LE(sd.size(), 32u) << c.name;
    const int nsegc = nr_gpu_pass_nseg_class((int)sd.size());
    ASSERT_EQ(32, nsegc) << c.name;
    for (int corrupt : {-1, 0}) {
      nr_gpu_pass_shape_t sh = {(uint16_t)c.n, 13, (uint16_t)c.nl, (uint16_t)c.qm, 2, 1, 1, (uint16_t)nsegc};
      GpuPassFixture fx = GpuPassFixture::from_shape(sh, 31 + n, corrupt, &sd, c.bwp_start, c.ref_point);
      ASSERT_TRUE(fx.tb_ok_built) << c.name;
      fx.shape.ncb_class = (uint16_t)nr_gpu_pass_ncb_class(fx.tb.C);
      fx.shape.nrb_class = (uint16_t)nr_gpu_pass_nrb_class(c.n);
      fx.bind();
      nr_gpu_pass_shape_t w = fx.shape;
      m.pass_split(0);
      EXPECT_EQ(-1, m.pass_warm(&w, 0, fx.job->dj.align, fx.job->dj.want_evm)) << c.name << ": the single-graph mode has no 32-segment class";
      m.pass_split(1);
      ASSERT_EQ(0, m.pass_warm(&w, 0, fx.job->dj.align, fx.job->dj.want_evm)) << c.name;
      std::vector<uint8_t> gb(fx.tb.A / 8 + 64, 0xAA), sb(fx.tb.A / 8 + 64, 0x55);
      std::vector<int16_t> brfo((size_t)4 * 2 * 4096 * 2, 0x7e);
      nr_gpu_pass_result_t g{}, s{};
      g.tb = gb.data(); s.tb = sb.data();
      g.brfo_rows = brfo.data();
      const int rc = m.pass_try(fx.job, &g);
      m.pass_split(0);
      ASSERT_EQ(0, rc) << c.name << " corrupt " << corrupt;
      ASSERT_EQ(0, fx.run_stage_path(&s));
      const std::string what = std::string(c.name) + " nseg " + std::to_string(sd.size()) + " corrupt " + std::to_string(corrupt) + " C " + std::to_string(fx.tb.C);
      EXPECT_EQ(s.need_cpu, g.need_cpu) << what;
      EXPECT_EQ(s.tb_ok, g.tb_ok) << what;
      EXPECT_EQ(s.nvar, g.nvar) << what;
      EXPECT_EQ(0, memcmp(s.nvar_ant, g.nvar_ant, sizeof(s.nvar_ant))) << what;
      EXPECT_EQ(0, memcmp(s.seg_ok, g.seg_ok, sizeof(s.seg_ok))) << what;
      if (!g.need_cpu) EXPECT_EQ(0, memcmp(s.tb, g.tb, g.tbs / 8)) << what;
      EXPECT_EQ(0, memcmp(brfo.data(), fx.est_l0.data(), brfo.size() * 2)) << "layer-0 estimates in data order, " << what;
      if (g.tb_ok) {
        EXPECT_EQ(0, memcmp(g.tb, fx.tb.payload.data(), fx.tb.A / 8)) << what;
        decoded++;
      } else if (!g.need_cpu)
        EXPECT_EQ(0, memcmp(fx.llr_pass.data(), fx.llr_ref.data(), (size_t)fx.tb.G * 2)) << what;
      for (int a = 0; a < 4; a++)
        for (int sy = 0; sy < 14; sy++)
          ASSERT_EQ(0, memcmp(&fx.rxF_out[2 * ((size_t)a * GpuPassFixture::RXSTRIDE + (size_t)sy * 4096)], &fx.rxF_ref[2 * ((size_t)a * 14 * 4096 + (size_t)sy * 4096)], 4096 * 4))
              << "rxdataF, ant " << a << " sym " << sy << ", " << what;
      n++;
    }
  }
  printf("GpuGraphSplit.ThirtyTwoSegmentsEqualStagePath: %d grants, decoded %d\n", n, decoded);
  EXPECT_GE(decoded, 3) << "the clean grants should decode";
}
/* ISAC_PDSCH_GPU_GRAPH_SEG128: segmented grants of 33-128 segments (classes 64 and 128, split only). The C sub-graph runs the chest rows in chunks of whole (DM-RS symbol, layer)
 * groups that reuse the scratch / LS rows (this process: 512 scratch rows, 1024 LS rows; the stage path chunks its own way), up to 2048 rows. The split pass equals the stage path's
 * segmented body, CRC ok and CRC fail; the single-graph mode refuses the classes. */
TEST(GpuGraphSplit, BeyondThirtyTwoSegmentsEqualStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m) && m.pass_warm && m.pass_split);
  const SegCase cases[] = {
      {"prg2 128 PRB: 64 segs rank 1", 1, 4, 128, 0, 2, 0, 0},
      {"prg2 80 PRB: 40 segs rank 2 (2 chunks)", 2, 2, 80, 0, 2, 0, 0},
      {"interleaved L2 120 PRB in 273: 60 segs rank 1", 1, 6, 120, 0, 0, 2, 273},
      {"prg2 217 PRB: 109 segs rank 1 (2 chunks)", 1, 4, 217, 0, 2, 0, 0},
      {"interleaved L2 200 PRB in 273: 100 segs rank 2 (4 chunks, 2048 rows)", 2, 4, 200, 0, 0, 2, 273},
      {"interleaved L2 + BWPStart 12 refPoint 0, 50 segs rank 1", 1, 4, 100, 0, 0, 2, 240, 12, 0},
  };
  int n = 0, decoded = 0;
  for (const SegCase &c : cases) {
    const auto sd = seg_layout(c.n, c.start, c.prg, c.interleave, c.bwp);
    ASSERT_GT(sd.size(), 32u) << c.name;
    ASSERT_LE(sd.size(), 128u) << c.name;
    const int nsegc = nr_gpu_pass_nseg_class((int)sd.size());
    ASSERT_GE(nsegc, 64) << c.name;
    for (int corrupt : {-1, 0}) {
      nr_gpu_pass_shape_t sh = {(uint16_t)c.n, 13, (uint16_t)c.nl, (uint16_t)c.qm, 2, 1, 1, (uint16_t)nsegc};
      GpuPassFixture fx = GpuPassFixture::from_shape(sh, 51 + n, corrupt, &sd, c.bwp_start, c.ref_point);
      ASSERT_TRUE(fx.tb_ok_built) << c.name;
      fx.shape.ncb_class = (uint16_t)nr_gpu_pass_ncb_class(fx.tb.C);
      fx.shape.nrb_class = (uint16_t)nr_gpu_pass_nrb_class(c.n);
      fx.bind();
      fx.job->llr_out_all = 1;
      nr_gpu_pass_shape_t w = fx.shape;
      m.pass_split(0);
      EXPECT_EQ(-1, m.pass_warm(&w, fx.job->dj.qam_mode, fx.job->dj.align, fx.job->dj.want_evm)) << c.name << ": the single-graph mode has no class above 32 segments";
      m.pass_split(1);
      ASSERT_EQ(0, m.pass_warm(&w, fx.job->dj.qam_mode, fx.job->dj.align, fx.job->dj.want_evm)) << c.name;
      std::vector<uint8_t> gb(fx.tb.A / 8 + 64, 0xAA), sb(fx.tb.A / 8 + 64, 0x55);
      nr_gpu_pass_result_t g{}, s{};
      g.tb = gb.data(); s.tb = sb.data();
      const int rc = m.pass_try(fx.job, &g);
      m.pass_split(0);
      ASSERT_EQ(0, rc) << c.name << " corrupt " << corrupt;
      ASSERT_EQ(0, fx.run_stage_path(&s));
      const std::string what = std::string(c.name) + " nseg " + std::to_string(sd.size()) + " corrupt " + std::to_string(corrupt) + " C " + std::to_string(fx.tb.C);
      EXPECT_EQ(s.need_cpu, g.need_cpu) << what;
      EXPECT_EQ(s.tb_ok, g.tb_ok) << what;
      EXPECT_EQ(s.nvar, g.nvar) << what;
      EXPECT_EQ(0, memcmp(s.nvar_ant, g.nvar_ant, sizeof(s.nvar_ant))) << what;
      EXPECT_EQ(0, memcmp(s.seg_ok, g.seg_ok, sizeof(s.seg_ok))) << what;
      if (!g.need_cpu) {
        EXPECT_EQ(0, memcmp(s.tb, g.tb, g.tbs / 8)) << what;
        EXPECT_EQ(0, memcmp(fx.llr_pass.data(), fx.llr_ref.data(), (size_t)fx.tb.G * 2)) << what << ": final LLRs";
      }
      const nr_gpu_llr_stats_t ref = fx.host_llr_stats();
      EXPECT_EQ(0, memcmp(&ref, &g.llr_stats, sizeof(ref))) << what;
      if (g.tb_ok) {
        EXPECT_EQ(0, memcmp(g.tb, fx.tb.payload.data(), fx.tb.A / 8)) << what;
        decoded++;
      }
      for (int a = 0; a < 4; a++)
        for (int sy = 0; sy < 14; sy++)
          ASSERT_EQ(0, memcmp(&fx.rxF_out[2 * ((size_t)a * GpuPassFixture::RXSTRIDE + (size_t)sy * 4096)], &fx.rxF_ref[2 * ((size_t)a * 14 * 4096 + (size_t)sy * 4096)], 4096 * 4))
              << "rxdataF, ant " << a << " sym " << sy << ", " << what;
      n++;
    }
  }
  printf("GpuGraphSplit.BeyondThirtyTwoSegmentsEqualStagePath: %d grants, decoded %d\n", n, decoded);
  EXPECT_GE(decoded, 3) << "the clean grants should decode";
  EXPECT_LT(decoded, n);
}
/* the production-like warm set (nr_gpu_pass_warmup's loops: DM-RS 3,2 x PRB classes x layers x code block classes x two demod variants x EVM, plus the segment classes
 * 1..32 within the chest rows) in split mode: the GPUGRAPH census says mb (instance buffers + sub-graphs) <= 1024 and execs = 4 x (C + D + L) */
TEST(GpuGraphSplit, WarmSetFitsBudget) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m) && m.pass_warm && m.pass_split);
  m.pass_split(1);
  /* the production warm set of nr_gpu_pass_warmup (nr_gpu_pass_dl.c): contiguous classes with 3, 2 and 1 DM-RS symbols (4 last), segmented classes (1 .. 128 segments) with 3 and 2, every code-block count
   * class up to 160, every demod variant the receiver policy can ask for (4 MMSE modes for nl >= 2, 2 alignments for nl = 1), and the generic-LDPC (nz4) L graph of the one-block classes */
  static const int nrbs[] = {273, 217, 136, 96, 48, 24, 12, 6}, ndc[] = {3, 2, 1}, nds[] = {3, 2}, segc[] = {1, 2, 4, 8, 16, 32, 64, 128}, ncbc[] = {1, 2, 4, 8, 16, 32, 64, 128, NR_GPU_PASS_MAX_CB};
  int made = 0, refused = 0;
  const auto t0 = std::chrono::steady_clock::now();
  auto warm_cls = [&](int nd, int nrb, int nl, int ns) {
    const int cmax = nrb * nl * 132 * 8 * 93 / 100 / 8400 + 1;
    for (int ci = 0; ci < 9 && (ci == 0 || ncbc[ci - 1] < cmax); ci++)
      for (int v = 0; v < (nl >= 2 ? 4 : 2); v++) {
        const nr_gpu_pass_shape_t sh = {(uint16_t)nrb, 13, (uint16_t)nl, 6, (uint16_t)nd, (uint16_t)ncbc[ci], 1, (uint16_t)ns};
        if (m.pass_warm(&sh, nl >= 2 ? v : 0, nl == 1 ? v : 0, 1) == 0) made++;
        else refused++; /* more chest rows than the stage buffers of this process take */
      }
  };
  for (int nd : ndc)
    for (int nrb : nrbs)
      for (int nl = 1; nl <= 4; nl++) warm_cls(nd, nrb, nl, 0);
  for (int nrb : nrbs)
    for (int nl = 1; nl <= 4; nl++)
      for (int v = 0; v < (nl >= 2 ? 4 : 2); v++) {
        nr_gpu_pass_shape_t sh = {(uint16_t)nrb, 13, (uint16_t)nl, 6, 3, 1, 1};
        sh.nz4 = 1;
        if (m.pass_warm(&sh, nl >= 2 ? v : 0, nl == 1 ? v : 0, 1) == 0) made++;
        else refused++;
      }
  for (int nd : nds)
    for (int nrb : nrbs)
      for (int nl = 1; nl <= 4; nl++)
        for (int ns : segc)
          if (ns <= nrb) warm_cls(nd, nrb, nl, ns);
  for (int nl = 1; nl <= 4; nl++)
    for (int nrb : nrbs) warm_cls(4, nrb, nl, 0);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  char buf[1024];
  m.pass_stats(buf, sizeof(buf));
  m.pass_split(0);
  const char *p = strstr(buf, "GPUGRAPH split ");
  ASSERT_NE(nullptr, p) << buf;
  unsigned long C = 0, D = 0, L = 0, ex = 0, mb = 0, capped = 0;
  ASSERT_EQ(5, sscanf(p, "GPUGRAPH split C=%lu D=%lu L=%lu execs=%lu mb=%lu", &C, &D, &L, &ex, &mb)) << p;
  const char *q = strstr(p, "capped=");
  ASSERT_NE(nullptr, q);
  capped = strtoul(q + 7, nullptr, 10);
  printf("GpuGraphSplit.WarmSetFitsBudget: %d classes covered (%d refused) in %.0f ms: %s\n", made, refused, ms, p);
  EXPECT_GT(made, 1000) << "the product of the class set is covered by the sum of the sub-graphs";
  EXPECT_EQ(0u, capped) << "the warm set hit the memory cap";
  EXPECT_LE(mb, 3072u); /* the cap of a device with this much memory (8 GB: 2048)*/
  EXPECT_EQ(ex, (NR_GPU_PASS_DL + NR_GPU_PASS_PROBE) * (C + D + L));
  EXPECT_LE(C + D + L, 900u);
}

/* review S6: grants with ONE DM-RS symbol (the split warm set adds ndmrs 1 contiguous C keys): the split pass equals the stage path, CRC ok and CRC fail */
TEST(GpuGraphSplit, OneDmrsSymbolEqualsStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m) && m.pass_warm && m.pass_split);
  GpuPassFixture::n_dm = 1;
  int n = 0, n_ok = 0;
  for (int si : {0, 5, 16 + 6, 32 + 11, 48 + 5}) /* 24 PRB nl 1 qm 2, 24 nl 2 qm 4, 48 nl 2 qm 6, 96 nl 3 qm 8, 217 nl 2 qm 4 */
    for (double snr : {20.0, -3.0}) {
      GpuPassFixture::noise_db = snr;
      nr_gpu_pass_shape_t sh = GpuPassFixture::warm_shapes()[si];
      sh.ndmrs = 1;
      GpuPassFixture fx = GpuPassFixture::from_shape(sh, 13);
      ASSERT_TRUE(fx.tb_ok_built);
      fx.shape.ncb_class = (uint16_t)nr_gpu_pass_ncb_class(fx.tb.C);
      fx.bind();
      fx.job->llr_out_all = 1;
      const std::string what = "ndmrs 1 nrb " + std::to_string(sh.nrb_class) + " nl " + std::to_string(sh.nl) + " qm " + std::to_string(sh.qm) + " snr " + std::to_string(snr);
      m.pass_split(1);
      nr_gpu_pass_shape_t w = fx.job->shape;
      ASSERT_EQ(0, m.pass_warm(&w, fx.job->dj.qam_mode, fx.job->dj.align, fx.job->dj.want_evm)) << what;
      std::vector<uint8_t> gb(fx.tb.A / 8 + 64, 0xAA), sb(fx.tb.A / 8 + 64, 0x55);
      nr_gpu_pass_result_t g{}, s{};
      g.tb = gb.data(); s.tb = sb.data();
      const int rc = m.pass_try(fx.job, &g);
      m.pass_split(0);
      ASSERT_EQ(0, rc) << what;
      ASSERT_EQ(0, fx.run_stage_path(&s)) << what;
      EXPECT_EQ(s.need_cpu, g.need_cpu) << what;
      EXPECT_EQ(s.tb_ok, g.tb_ok) << what;
      EXPECT_EQ(s.nvar, g.nvar) << what;
      EXPECT_EQ(0, memcmp(s.nvar_ant, g.nvar_ant, sizeof(s.nvar_ant))) << what;
      EXPECT_EQ(0, memcmp(s.seg_ok, g.seg_ok, sizeof(s.seg_ok))) << what;
      if (!g.need_cpu) {
        EXPECT_EQ(0, memcmp(s.tb, g.tb, g.tbs / 8)) << what;
        EXPECT_EQ(0, memcmp(fx.llr_ref.data(), fx.llr_pass.data(), (size_t)fx.tb.G * 2)) << what << ": final LLRs";
      }
      const nr_gpu_llr_stats_t ref = fx.host_llr_stats();
      EXPECT_EQ(0, memcmp(&ref, &g.llr_stats, sizeof(ref))) << what;
      EXPECT_TRUE(fx.rxF_out.size() > 0);
      for (int a = 0; a < 4; a++)
        for (int sy = 0; sy < 14; sy++)
          ASSERT_EQ(0, memcmp(&fx.rxF_out[2 * ((size_t)a * GpuPassFixture::RXSTRIDE + (size_t)sy * 4096)], &fx.rxF_ref[2 * ((size_t)a * 14 * 4096 + (size_t)sy * 4096)], 4096 * 4))
              << "rxdataF, ant " << a << " sym " << sy << ", " << what;
      n++;
      n_ok += g.tb_ok;
    }
  GpuPassFixture::n_dm = 2;
  GpuPassFixture::noise_db = 20.0;
  printf("GpuGraphSplit.OneDmrsSymbolEqualsStagePath: %d grants, decoded %d\n", n, n_ok);
  EXPECT_GE(n_ok, 3);
  EXPECT_LT(n_ok, n);
}

/* ===================================================== DL GPU data path v2, Task 4 (ISAC_PDSCH_GPU_GRAPH_RETRY) ===================================================== */
/* One decode call (nr_gpu_pass_call_begin / _end) holds its instance: the bodies of the call enter at C when the chest inputs changed (first body, other pilots = a CDM /
 * DM-RS hypothesis change, after a dirty mark) and at D otherwise (a probe of the same grant: other demod horizon, code blocks and PassBlk; an RV change: other rate-dematching
 * tables). Every body equals the stage path from scratch (TB, CRC verdict, need_cpu, seg_ok, nvar, final LLRs, LLR statistics, rxdataF). DISCRETE: a D entry reading the
 * previous body's PassBlk / geometry would differ (review T4-1). A fallback in the middle of the call (unseen class) keeps the hold; call_end releases the instance. */
TEST(GpuGraphRetry, EntryFollowsChangedInput) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m) && m.pass_warm && m.pass_split && m.pass_hold && m.call_begin && m.call_end && m.call_dirty && m.retry_counters);
  auto warm = [&](GpuPassFixture &f) {
    nr_gpu_pass_shape_t w = f.job->shape;
    return m.pass_warm(&w, f.job->dj.qam_mode, f.job->dj.align, f.job->dj.want_evm);
  };
  int8_t inst = -1;
  /* one body of the call; want_d: the entry expected (1 = D) */
  auto body = [&](GpuPassFixture &f, int want_d, const std::string &what) {
    f.job->llr_out_all = 1;
    std::vector<uint8_t> gb(f.tb.A / 8 + 64, 0xAA), sb(f.tb.A / 8 + 64, 0x55);
    nr_gpu_pass_result_t g{}, s{};
    g.tb = gb.data(); s.tb = sb.data();
    uint64_t c0[4], c1[4];
    m.retry_counters(c0);
    ASSERT_EQ(0, m.pass_try(f.job, &g)) << what;
    m.retry_counters(c1);
    EXPECT_EQ(c0[1] + (want_d ? 1 : 0), c1[1]) << what << ": entry at D";
    EXPECT_EQ(c0[0] + (want_d ? 0 : 1), c1[0]) << what << ": entry at C";
    if (inst >= 0) EXPECT_EQ(inst, g.instance) << what << ": the held instance";
    inst = g.instance;
    ASSERT_EQ(0, f.run_stage_path(&s)) << what;
    EXPECT_EQ(s.need_cpu, g.need_cpu) << what;
    EXPECT_EQ(f.is_probe ? 0 : s.tb_ok, g.tb_ok) << what;
    EXPECT_EQ(s.nvar, g.nvar) << what;
    EXPECT_EQ(0, memcmp(s.seg_ok, g.seg_ok, sizeof(s.seg_ok))) << what;
    if (!g.need_cpu) {
      if (g.tb_ok) EXPECT_EQ(0, memcmp(s.tb, g.tb, g.tbs / 8)) << what;
      EXPECT_EQ(0, memcmp(f.llr_ref.data(), f.llr_pass.data(), (size_t)f.tb.G * 2)) << what << ": final LLRs";
    }
    const nr_gpu_llr_stats_t ref = f.host_llr_stats();
    EXPECT_EQ(0, memcmp(&ref, &g.llr_stats, sizeof(ref))) << what << ": LLR statistics";
    for (int a = 0; a < 4; a++)
      for (int sy = 0; sy < 14; sy++)
        ASSERT_EQ(0, memcmp(&f.rxF_out[2 * ((size_t)a * GpuPassFixture::RXSTRIDE + (size_t)sy * 4096)], &f.rxF_ref[2 * ((size_t)a * 14 * 4096 + (size_t)sy * 4096)], 4096 * 4))
            << "rxdataF, ant " << a << " sym " << sy << ", " << what;
  };
  /* the next body's job on the same grant object (same IQ ring, pilot and rxdataF buffers, as the retry arms of one call); rxdataF keeps what the earlier bodies wrote */
  auto rebind = [](GpuPassFixture &f) {
    const std::vector<int16_t> keep = f.rxF_out;
    f.bind();
    f.rxF_out = keep;
  };
  GpuPassFixture ux = GpuPassFixture::from_shape({6, 13, 4, 2, 2, 1, 1}, 19); /* PRB class 6: never warmed (an unseen key in the middle of the call) */
  m.pass_split(1);
  int n = 0;
  const auto sd = seg_layout(32, 0, 4, 0, 0); /* review nit: a segmented grant too (PRG 4: 8 segments) */
  for (int si : {0, 5, 16 + 6, 48 + 5, -1}) { /* 24 PRB nl 1 qm 2, 24 nl 2 qm 4, 48 nl 2 qm 6, 217 nl 2 qm 4, segmented 32 PRB nl 1 qm 4 */
    const nr_gpu_pass_shape_t shape = si >= 0 ? GpuPassFixture::warm_shapes()[si] : nr_gpu_pass_shape_t{32, 13, 1, 4, 2, 1, 1, (uint16_t)nr_gpu_pass_nseg_class((int)sd.size())};
    GpuPassFixture fx = GpuPassFixture::from_shape(shape, 41 + si, -1, si >= 0 ? nullptr : &sd);
    ASSERT_TRUE(fx.tb_ok_built);
    if (si < 0) {
      fx.shape.ncb_class = (uint16_t)nr_gpu_pass_ncb_class(fx.tb.C);
      fx.shape.nrb_class = (uint16_t)nr_gpu_pass_nrb_class(32);
      fx.bind();
    }
    const std::string what = "nrb " + std::to_string(shape.nrb_class) + " nl " + std::to_string(shape.nl) + " qm " + std::to_string(shape.qm) + " C " + std::to_string(fx.tb.C) +
                             " nseg " + std::to_string(fx.segs.size());
    { /* every key of the call, before it (warming captures on every instance) */
      GpuPassFixture px = fx;
      px.make_probe();
      ASSERT_EQ(0, warm(fx)) << what;
      ASSERT_EQ(0, warm(px)) << what;
    }
    inst = -1;
    uint64_t h0[4], h1[4];
    m.retry_counters(h0);
    m.call_begin();
    body(fx, 0, what + " first body");
    ASSERT_FALSE(HasFatalFailure());
    fx.is_probe = true; /* make_probe() without clearing rxdataF */
    rebind(fx);
    body(fx, 1, what + " probe of the same grant");
    std::vector<uint8_t> ub(ux.tb.A / 8 + 64);
    nr_gpu_pass_result_t ur{};
    ur.tb = ub.data();
    EXPECT_EQ(1, m.pass_try(ux.job, &ur)) << what << ": unseen key in the middle of the call";
    fx.is_probe = false; fx.n_decode = 0; fx.probe_last = -1;
    fx.tb.rv = 2;
    rebind(fx);
    body(fx, 1, what + " rv 2");
    fx.tb.rv = 0;
    for (auto &v : fx.pilot[0][0]) v = (int16_t)-v; /* the DM-RS hypothesis changes in place (same buffer): C */
    rebind(fx);
    body(fx, 0, what + " other pilots");
    rebind(fx);
    body(fx, 1, what + " same inputs again");
    fx.ring_offset -= 8; /* review nit: a non-pilot C input (the FFT window inside the CP) changes: C */
    rebind(fx);
    body(fx, 0, what + " ring offset moved");
    fx.ring_offset += 8;
    rebind(fx);
    body(fx, 0, what + " ring offset back");
    m.call_dirty(); /* review S3: releases the instance; the call's next pass acquires (and holds) one again */
    inst = -1;
    rebind(fx);
    body(fx, 0, what + " after a dirty mark");
    m.call_end();
    m.retry_counters(h1);
    EXPECT_EQ(h0[2] + 2, h1[2]) << what << ": one held call, held again after the dirty release";
    EXPECT_EQ(h0[3] + 1, h1[3]) << what << ": one dirty invalidation";
    /* released: with every other instance busy a pass still finds this one; outside a call the same inputs enter at C */
    m.pass_hold(((1u << NR_GPU_PASS_RING) - 1u) & ~(1u << inst), 1);
    rebind(fx);
    body(fx, 0, what + " after call_end");
    m.pass_hold(((1u << NR_GPU_PASS_RING) - 1u) & ~(1u << inst), 0);
    for (auto &v : fx.pilot[0][0]) v = (int16_t)-v;
    n++;
  }
  m.pass_split(0);
  printf("GpuGraphRetry.EntryFollowsChangedInput: %d calls x 9 bodies\n", n);
}

/* ===================================================== DL GPU data path v2: DM-RS type 2 in the pass (ISAC_PDSCH_GPU_GRAPH_DMRS2) ===================================================== */
/* DM-RS configuration type 2 grants (2 adjacent-RE pairs per RB and CDM group; layers 0-1 in CDM group 0, layers 2-3 in group 1 with delta 2) through the split pass: the chest rows
 * carry the type (the captured k_ls has type 1 baked in), the pilots are 4 c16 per RB. Every output equals the stage path (nr_pdsch_gpu_chest_grant's type-2 rows): TB, CRC verdict,
 * need_cpu, seg_ok, nvar and per-antenna nvar, final LLRs, LLR statistics, rxdataF. Several PRB classes, layer counts (1, 2: one CDM group; 3, 4: two), 1 and 2 DM-RS symbols,
 * CRC ok and CRC fail, contiguous and segmented. */
TEST(GpuGraphDmrs2, Type2EqualsStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m) && m.pass_warm && m.pass_split);
  GpuPassFixture::dmrs_type = 2;
  const auto sd = seg_layout(32, 0, 4, 0, 0); /* PRG 4: 8 segments */
  int n = 0, n_ok = 0;
  for (int ndm : {2, 1})
    for (int si : {0, 5, 16 + 6, 16 + 13, 32 + 11, 48 + 5, -1}) /* 24 PRB nl 1 qm 2, 24 nl 2 qm 4, 48 nl 2 qm 6, 48 nl 4 qm 4, 96 nl 3 qm 8, 217 nl 2 qm 4, segmented 32 PRB nl 2 qm 4 */
      for (double snr : {20.0, -3.0}) {
        GpuPassFixture::n_dm = ndm;
        GpuPassFixture::noise_db = snr;
        nr_gpu_pass_shape_t sh = si >= 0 ? GpuPassFixture::warm_shapes()[si] : nr_gpu_pass_shape_t{32, 13, 2, 4, 2, 1, 1, (uint16_t)nr_gpu_pass_nseg_class((int)sd.size())};
        sh.ndmrs = (uint16_t)ndm;
        GpuPassFixture fx = GpuPassFixture::from_shape(sh, 17 + si, -1, si >= 0 ? nullptr : &sd);
        ASSERT_TRUE(fx.tb_ok_built);
        ASSERT_EQ(2, fx.dtype);
        fx.shape.ncb_class = (uint16_t)nr_gpu_pass_ncb_class(fx.tb.C);
        fx.shape.nrb_class = (uint16_t)nr_gpu_pass_nrb_class(si >= 0 ? sh.nrb_class : 32);
        fx.bind();
        fx.job->llr_out_all = 1;
        const std::string what = "type 2 ndmrs " + std::to_string(ndm) + " nrb " + std::to_string(sh.nrb_class) + " nl " + std::to_string(sh.nl) + " qm " + std::to_string(sh.qm) +
                                 " nseg " + std::to_string(fx.segs.size()) + " snr " + std::to_string(snr);
        m.pass_split(1);
        nr_gpu_pass_shape_t w = fx.job->shape;
        ASSERT_EQ(0, m.pass_warm(&w, fx.job->dj.qam_mode, fx.job->dj.align, fx.job->dj.want_evm)) << what;
        std::vector<uint8_t> gb(fx.tb.A / 8 + 64, 0xAA), sb(fx.tb.A / 8 + 64, 0x55);
        nr_gpu_pass_result_t g{}, s{};
        g.tb = gb.data(); s.tb = sb.data();
        const int rc = m.pass_try(fx.job, &g);
        m.pass_split(0);
        ASSERT_EQ(0, rc) << what;
        ASSERT_EQ(0, fx.run_stage_path(&s)) << what;
        EXPECT_EQ(s.need_cpu, g.need_cpu) << what;
        EXPECT_EQ(s.tb_ok, g.tb_ok) << what;
        EXPECT_EQ(s.nvar, g.nvar) << what;
        EXPECT_EQ(0, memcmp(s.nvar_ant, g.nvar_ant, sizeof(s.nvar_ant))) << what;
        EXPECT_EQ(0, memcmp(s.seg_ok, g.seg_ok, sizeof(s.seg_ok))) << what;
        if (!g.need_cpu) {
          EXPECT_EQ(0, memcmp(s.tb, g.tb, g.tbs / 8)) << what;
          EXPECT_EQ(0, memcmp(fx.llr_ref.data(), fx.llr_pass.data(), (size_t)fx.tb.G * 2)) << what << ": final LLRs";
        }
        if (g.tb_ok) EXPECT_EQ(0, memcmp(g.tb, fx.tb.payload.data(), fx.tb.A / 8)) << what << ": the transmitted TB";
        const nr_gpu_llr_stats_t ref = fx.host_llr_stats();
        EXPECT_EQ(0, memcmp(&ref, &g.llr_stats, sizeof(ref))) << what;
        for (int a = 0; a < 4; a++)
          for (int sy = 0; sy < 14; sy++)
            ASSERT_EQ(0, memcmp(&fx.rxF_out[2 * ((size_t)a * GpuPassFixture::RXSTRIDE + (size_t)sy * 4096)], &fx.rxF_ref[2 * ((size_t)a * 14 * 4096 + (size_t)sy * 4096)], 4096 * 4))
                << "rxdataF, ant " << a << " sym " << sy << ", " << what;
        n++;
        n_ok += g.tb_ok;
      }
  GpuPassFixture::dmrs_type = 1;
  GpuPassFixture::n_dm = 2;
  GpuPassFixture::noise_db = 20.0;
  printf("GpuGraphDmrs2.Type2EqualsStagePath: %d grants, decoded %d\n", n, n_ok);
  EXPECT_GE(n_ok, n / 3) << "the clean type-2 grants should decode";
  EXPECT_LT(n_ok, n);
}

/* ===================================================== Fix A+D: benches (BENCH numbers on the DGX GPU, synthetic grants, no receiver running; not on-air) ===================================================== */
namespace fixad {
struct Lat {
  std::vector<double> wall, prep, launch, gstart, gspan, wake;
  void add(const nr_gpu_pass_result_t &r)
  {
    wall.push_back(r.gpu_us);
    prep.push_back(r.prep_ns / 1e3);
    launch.push_back(r.launch_ns / 1e3);
    gstart.push_back(r.gstart_ns / 1e3);
    gspan.push_back(r.gspan_ns / 1e3);
    wake.push_back(std::max(0.0, r.gpu_us - (r.gstart_ns + r.gspan_ns) / 1e3));
  }
};
static double q(std::vector<double> v, double p)
{
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, (size_t)(p * (double)v.size()))];
}
static void line(const char *what, const char *mode, const Lat &l, double stage_us = -1)
{
  printf("BENCH %-22s %-12s n=%3zu  prep %6.1f  launch(submit) %6.1f  submit->GPUstart %6.1f/%7.1f  GPU span %6.1f/%7.1f  GPUend->host %6.1f/%7.1f  wall %6.1f/%7.1f us (p50/p99)%s\n", what, mode,
         l.wall.size(), q(l.prep, 0.5), q(l.launch, 0.5), q(l.gstart, 0.5), q(l.gstart, 0.99), q(l.gspan, 0.5), q(l.gspan, 0.99), q(l.wake, 0.5), q(l.wake, 0.99), q(l.wall, 0.5), q(l.wall, 0.99),
         stage_us >= 0 ? (std::string("  [stage path wall p50 ") + std::to_string((int)stage_us) + " us]").c_str() : "");
}
}  // namespace fixad

/* typical shapes: DL 1/4/8/16 code blocks, before (the Task 2 stage path: separate FEP / chest / demod / LDPC submissions with host steps between) and after (one submission), and the
 * completion wait before (spin 600 us then 20 us naps) and after (spin 150 us then a blocking event wait; and blocking only). */
TEST(GpuPassBench, DlSubmissionBreakdown) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m) && m.set_spin);
  std::map<int, nr_gpu_pass_shape_t> pick;
  for (int nrb : {24, 48, 96, 136, 217})
    for (int nl : {1, 2, 4})
      for (int qm : {2, 4, 6, 8}) {
        const nr_gpu_pass_shape_t sh = {(uint16_t)nrb, 13, (uint16_t)nl, (uint16_t)qm, 2, 32, 1};
        GpuPassFixture f = GpuPassFixture::from_shape(sh, 11);
        if (!f.tb_ok_built) continue;
        const int c = (int)f.tb.C;
        if ((c == 1 || c == 4 || c == 8 || c == 16) && !pick.count(c)) pick[c] = sh;
      }
  ASSERT_FALSE(pick.empty());
  for (auto &kv : pick) {
    GpuPassFixture fx = GpuPassFixture::from_shape(kv.second, 11);
    fx.bind();
    std::vector<uint8_t> tb(fx.tb.A / 8 + 64);
    nr_gpu_pass_result_t r{};
    r.tb = tb.data();
    nr_gpu_pass_shape_t ws = fx.job->shape;
    ASSERT_EQ(0, m.pass_warm(&ws, fx.job->dj.qam_mode, fx.job->dj.align, fx.job->dj.want_evm));
    ASSERT_EQ(0, m.pass_try(fx.job, &r));
    char what[64];
    snprintf(what, sizeof(what), "DL C=%d nrb %d nl %d qm %d", kv.first, kv.second.nrb_class, kv.second.nl, kv.second.qm);
    std::vector<double> st;
    for (int i = 0; i < 40; i++) {
      nr_gpu_pass_result_t s{};
      std::vector<uint8_t> sb(fx.tb.A / 8 + 64);
      s.tb = sb.data();
      const auto t0 = std::chrono::steady_clock::now();
      ASSERT_EQ(0, fx.run_stage_path(&s));
      st.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    }
    for (int mode : {-600, 0, 60}) {
      m.set_spin(mode);
      fixad::Lat l;
      for (int i = 0; i < 20; i++) m.pass_try(fx.job, &r);
      for (int i = 0; i < 300; i++) {
        ASSERT_EQ(0, m.pass_try(fx.job, &r));
        l.add(r);
      }
      fixad::line(what, mode < 0 ? "poll+nap(old)" : mode == 0 ? "blocking" : "sleep+spin60", l, mode == 60 ? fixad::q(st, 0.5) : -1);
    }
    m.set_spin(60);
  }
}

TEST(GpuPassBench, UlSubmissionBreakdown) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && ul_ready(m) && m.set_spin);
  std::map<int, std::pair<int, int>> pick; // C -> (nrb, qm)
  for (int nrb : {24, 48, 96, 136, 217})
    for (int qm : {2, 4, 6})
      for (int sd : {3}) {
        const nr_gpu_pass_shape_t sh = {(uint16_t)nrb, 13, 1, (uint16_t)qm, 2, 1, 1};
        GpuPassFixture::noise_db = 20.0;
        GpuPassFixture f = GpuPassFixture::from_shape(sh, sd);
        if (!f.tb_ok_built) continue;
        const int c = (int)f.tb.C;
        if ((c == 1 || c == 2 || c == 4) && !pick.count(c)) pick[c] = {nrb, qm};
      }
  ASSERT_FALSE(pick.empty());
  for (auto &kv : pick) {
    const int nrb = kv.second.first, qm = kv.second.second;
    GpuPassFixture::noise_db = 20.0;
    GpuPassFixture::first_sym = 1;
    const nr_gpu_pass_shape_t sh = {(uint16_t)nrb, 13, 1, (uint16_t)qm, 2, 1, 1};
    GpuPassFixture fx = GpuPassFixture::from_shape(sh, 3);
    ul_warm_class(m, nrb, 2, fx.tb.C);
    ASSERT_FALSE(HasFatalFailure());
    UlSlot sl;
    sl.build(fx, 0, -1, 0, 0);
    char what[64];
    snprintf(what, sizeof(what), "UL C=%d nrb %d qm %d", kv.first, nrb, qm);
    std::vector<double> st;
    for (int i = 0; i < 20; i++) {
      const auto t0 = std::chrono::steady_clock::now();
      UlRef ref = ul_reference(m, fx, 0, -1);
      ASSERT_TRUE(ref.ok);
      st.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    }
    for (int mode : {-600, 0, 60}) {
      m.set_spin(mode);
      fixad::Lat l;
      for (int i = 0; i < 20; i++) nr_gpu_pass_ul_run(&sl.in, &sl.out);
      for (int i = 0; i < 300; i++) {
        ASSERT_EQ(0, nr_gpu_pass_ul_run(&sl.in, &sl.out));
        l.add(sl.out.r);
      }
      fixad::line(what, mode < 0 ? "poll+nap(old)" : mode == 0 ? "blocking" : "sleep+spin60", l, mode == 60 ? fixad::q(st, 0.5) : -1);
    }
    m.set_spin(60);
  }
}

/* Interference proof (bench, synthetic grants): the submit -> GPU-start latency (event stamp at the launch call to the first graph node) and the wall of a real DL pass, alone and with a
 * concurrent load of 2 UL passes in a loop plus a DL probe loop; and the same for a real UL pass against a DL load. The pools are disjoint (instances, streams, staging slots, demod and LDPC
 * contexts), so the two columns must agree within noise. Shared hardware remains: the RT context's SMs and the copy engines (see the report). */
TEST(GpuPassBench, DlUlInterference) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && ul_ready(m) && m.set_spin);
  m.set_spin(60);
  const nr_gpu_pass_shape_t dsh = {96, 13, 2, 4, 2, 8, 1};
  GpuPassFixture dl = GpuPassFixture::from_shape(dsh, 11);
  ASSERT_TRUE(dl.tb_ok_built);
  dl.bind();
  GpuPassFixture px = GpuPassFixture::from_shape(dsh, 11);
  px.make_probe();
  nr_gpu_pass_shape_t pw = px.job->shape;
  ASSERT_EQ(0, m.pass_warm(&pw, 3, 0, 0));
  GpuPassFixture::noise_db = 20.0;
  GpuPassFixture::first_sym = 1;
  const nr_gpu_pass_shape_t ush = {106, 13, 1, 4, 2, 1, 1};
  GpuPassFixture ul = GpuPassFixture::from_shape(ush, 3);
  ASSERT_TRUE(ul.tb_ok_built);
  ul_warm_class(m, 106, 2, ul.tb.C);
  ASSERT_FALSE(HasFatalFailure());
  UlSlot sl[3];
  for (int i = 0; i < 3; i++) sl[i].build(ul, 0, -1, 0, i);
  auto run_dl_main = [&](int n, fixad::Lat *l) {
    std::vector<uint8_t> tb(dl.tb.A / 8 + 64);
    nr_gpu_pass_result_t r{};
    r.tb = tb.data();
    for (int i = 0; i < 30; i++) m.pass_try(dl.job, &r);
    for (int i = 0; i < n; i++) {
      if (m.pass_try(dl.job, &r) == 0) l->add(r);
    }
  };
  auto run_ul_main = [&](int n, fixad::Lat *l) {
    for (int i = 0; i < 30; i++) nr_gpu_pass_ul_run(&sl[2].in, &sl[2].out);
    for (int i = 0; i < n; i++)
      if (nr_gpu_pass_ul_run(&sl[2].in, &sl[2].out) == 0) l->add(sl[2].out.r);
  };
  const int NREP = 600;
  fixad::Lat dl_alone, dl_loaded, ul_alone, ul_loaded;
  run_dl_main(NREP, &dl_alone);
  run_ul_main(NREP, &ul_alone);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> n_ul{0}, n_pr{0}, n_dl{0};
  { // DL main against UL x2 + probe load
    std::vector<std::thread> th;
    for (int i = 0; i < 2; i++)
      th.emplace_back([&, i] { while (!stop) { if (nr_gpu_pass_ul_run(&sl[i].in, &sl[i].out) == 0) n_ul++; } });
    th.emplace_back([&] {
      std::vector<uint8_t> tb(px.tb.A / 8 + 64);
      nr_gpu_pass_result_t r{};
      r.tb = tb.data();
      std::vector<int16_t> llr(px.tb.G + 64);
      px.job->llr_out = llr.data();
      while (!stop) { if (m.pass_try(px.job, &r) == 0) n_pr++; }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    run_dl_main(NREP, &dl_loaded);
    stop = true;
    for (auto &t : th) t.join();
  }
  stop = false;
  { // UL main against DL x2 + probe load
    std::vector<std::thread> th;
    GpuPassFixture dl2 = GpuPassFixture::from_shape(dsh, 12);
    dl2.bind();
    for (int i = 0; i < 2; i++)
      th.emplace_back([&, i] {
        std::vector<uint8_t> tb(dl.tb.A / 8 + 64);
        nr_gpu_pass_result_t r{};
        r.tb = tb.data();
        const nr_gpu_pass_job_t *jj = i ? dl2.job : dl.job;
        while (!stop) { if (m.pass_try(jj, &r) == 0) n_dl++; }
      });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    run_ul_main(NREP, &ul_loaded);
    stop = true;
    for (auto &t : th) t.join();
  }
  fixad::line("DL real C=8 96PRB nl2", "alone", dl_alone);
  fixad::line("DL real C=8 96PRB nl2", "+2UL+probe", dl_loaded);
  fixad::line("UL real 106PRB qm4", "alone", ul_alone);
  fixad::line("UL real 106PRB qm4", "+2DL load", ul_loaded);
  printf("BENCH load passes completed during the loaded runs: UL %lu, DL probe %lu, DL %lu\n", (unsigned long)n_ul.load(), (unsigned long)n_pr.load(), (unsigned long)n_dl.load());
  // within noise: submit -> GPU start p50 (the quantity the pools protect) differs by less than 25 us or 25 %
  const double a = fixad::q(dl_alone.gstart, 0.5), b = fixad::q(dl_loaded.gstart, 0.5);
  EXPECT_LT(b, a + std::max(25.0, 0.25 * a)) << "DL submit->GPU start p50 alone " << a << " us, loaded " << b << " us";
  const double c = fixad::q(ul_alone.gstart, 0.5), d = fixad::q(ul_loaded.gstart, 0.5);
  EXPECT_LT(d, c + std::max(25.0, 0.25 * c)) << "UL submit->GPU start p50 alone " << c << " us, loaded " << d << " us";
}

/* P99-UL Task 3: the DL / UL share of the real-time SMs. GPU span (graph head to completion, device-side event stamps) of typical DL grants of 1 / 4 / 8 / 16 code blocks and of UL grants, alone and
 * under the other direction's load (2 UL loops + a probe loop against DL; 2 DL loops against UL). What the partition is comes from the process environment (nr_gpu_green.h: the DL / UL split
 * of the real-time SMs is printed at start-up; ISAC_GPU_RT_ISOLATE=0 puts every stream on all SMs); BENCH_TAG labels the run. A bench: no assertion on the numbers except that the passes ran. */
TEST(GpuPassBench, DlSmShare) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && ul_ready(m) && m.set_spin);
  m.set_spin(60);
  const char *tag = getenv("BENCH_TAG") ? getenv("BENCH_TAG") : "default";
  std::map<int, nr_gpu_pass_shape_t> pick;
  GpuPassFixture::max_cb = 32; /* a 16-block grant needs a wide high-order one */
  struct RestoreMaxCb { ~RestoreMaxCb() { GpuPassFixture::max_cb = 8; } } restore_max_cb;
  for (int nrb : {24, 48, 96, 136, 217})
    for (int nl : {1, 2, 4})
      for (int qm : {2, 4, 6, 8}) {
        const nr_gpu_pass_shape_t sh = {(uint16_t)nrb, 13, (uint16_t)nl, (uint16_t)qm, 2, 32, 1};
        GpuPassFixture f = GpuPassFixture::from_shape(sh, 11);
        if (!f.tb_ok_built) continue;
        const int c = (int)f.tb.C;
        if ((c == 1 || c == 4 || c == 8 || (c >= 14 && c <= 17)) && !pick.count(c)) pick[c] = sh;
      }
  ASSERT_FALSE(pick.empty());
  GpuPassFixture::noise_db = 20.0;
  GpuPassFixture::first_sym = 1;
  struct UlCase { int nrb, qm; };
  const UlCase ucs[] = {{106, 4}, {217, 4}};
  std::vector<std::unique_ptr<GpuPassFixture>> uls;
  std::vector<std::array<UlSlot, 3>> uslots(2);
  for (int u = 0; u < 2; u++) {
    const nr_gpu_pass_shape_t ush = {(uint16_t)ucs[u].nrb, 13, 1, (uint16_t)ucs[u].qm, 2, 1, 1};
    uls.emplace_back(new GpuPassFixture(GpuPassFixture::from_shape(ush, 3)));
    ASSERT_TRUE(uls.back()->tb_ok_built);
    ul_warm_class(m, ucs[u].nrb, 2, uls.back()->tb.C);
    ASSERT_FALSE(HasFatalFailure());
    for (int i = 0; i < 3; i++) uslots[u][i].build(*uls.back(), 0, -1, 0, i);
  }
  const nr_gpu_pass_shape_t psh = {96, 13, 2, 4, 2, 8, 1};
  GpuPassFixture px = GpuPassFixture::from_shape(psh, 11);
  px.make_probe();
  nr_gpu_pass_shape_t pw = px.job->shape;
  ASSERT_EQ(0, m.pass_warm(&pw, 3, 0, 0));
  const int NREP = 400;
  auto run_dl = [&](GpuPassFixture &dl, fixad::Lat *l) {
    std::vector<uint8_t> tb(dl.tb.A / 8 + 64);
    nr_gpu_pass_result_t r{};
    r.tb = tb.data();
    for (int i = 0; i < 30; i++) m.pass_try(dl.job, &r);
    for (int i = 0; i < NREP; i++)
      if (m.pass_try(dl.job, &r) == 0) l->add(r);
  };
  auto run_ul = [&](UlSlot &sl, fixad::Lat *l) {
    for (int i = 0; i < 30; i++) nr_gpu_pass_ul_run(&sl.in, &sl.out);
    for (int i = 0; i < NREP; i++)
      if (nr_gpu_pass_ul_run(&sl.in, &sl.out) == 0) l->add(sl.out.r);
  };
  std::vector<std::unique_ptr<GpuPassFixture>> dls;
  for (auto &kv : pick) {
    dls.emplace_back(new GpuPassFixture(GpuPassFixture::from_shape(kv.second, 11)));
    GpuPassFixture &dl = *dls.back();
    dl.bind();
    nr_gpu_pass_shape_t ws = dl.job->shape;
    ASSERT_EQ(0, m.pass_warm(&ws, dl.job->dj.qam_mode, dl.job->dj.align, dl.job->dj.want_evm));
    char what[96];
    snprintf(what, sizeof(what), "[%s] DL C=%d nrb %d nl %d qm %d", tag, kv.first, kv.second.nrb_class, kv.second.nl, kv.second.qm);
    fixad::Lat alone, loaded;
    run_dl(dl, &alone);
    std::atomic<bool> stop{false};
    std::vector<std::thread> th;
    for (int i = 0; i < 2; i++)
      th.emplace_back([&, i] { while (!stop) nr_gpu_pass_ul_run(&uslots[0][i].in, &uslots[0][i].out); });
    th.emplace_back([&] {
      std::vector<uint8_t> tb(px.tb.A / 8 + 64);
      nr_gpu_pass_result_t r{};
      r.tb = tb.data();
      std::vector<int16_t> llr(px.tb.G + 64);
      px.job->llr_out = llr.data();
      while (!stop) m.pass_try(px.job, &r);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    run_dl(dl, &loaded);
    stop = true;
    for (auto &t : th) t.join();
    fixad::line(what, "alone", alone);
    fixad::line(what, "+2UL+probe", loaded);
    EXPECT_GE(alone.gspan.size(), (size_t)NREP / 2);
  }
  for (int u = 0; u < 2; u++) {
    char what[96];
    snprintf(what, sizeof(what), "[%s] UL nrb %d qm %d", tag, ucs[u].nrb, ucs[u].qm);
    fixad::Lat alone, loaded;
    run_ul(uslots[u][2], &alone);
    GpuPassFixture dl2a = GpuPassFixture::from_shape(pick.rbegin()->second, 12), dl2b = GpuPassFixture::from_shape(pick.rbegin()->second, 13);
    dl2a.bind();
    dl2b.bind();
    std::atomic<bool> stop{false};
    std::vector<std::thread> th;
    for (int i = 0; i < 2; i++)
      th.emplace_back([&, i] {
        std::vector<uint8_t> tb(dl2a.tb.A / 8 + 64);
        nr_gpu_pass_result_t r{};
        r.tb = tb.data();
        const nr_gpu_pass_job_t *jj = i ? dl2b.job : dl2a.job;
        while (!stop) m.pass_try(jj, &r);
      });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    run_ul(uslots[u][2], &loaded);
    stop = true;
    for (auto &t : th) t.join();
    fixad::line(what, "alone", alone);
    fixad::line(what, "+2DL", loaded);
    EXPECT_GE(alone.gspan.size(), (size_t)NREP / 2);
  }
}

/* ===================================================== Fix A+D: the no-chance gate inside the submission (nr_snr_gate.h) ===================================================== */
#include "nr_snr_gate.h"
#include "nr_pdsch_dmrs_snr.h"
/* DL: the graph's own DM-RS comb SNR (r.gate_db) against required(mcs, tbl) - 5 dB. A grant more than 5 dB below skips (demod / LDPC / CRC nodes do not run, tb_ok = 0, skip = 1);
 * every other grant is decoded and equals the ungated pass (TB bytes, CRC verdict, segment verdicts, nvar). The sweep over the SNR of the slot and over every MCS of tables 0 and 1
 * gives skipped and decoded grants on the same slot, i.e. grants at req - 6 dB (skipped) and req - 4 dB (decoded). */
TEST(GpuPassGate, DlSkipsNoChanceAndKeepsTheRest) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m) && m.pass_warm && m.pass_split);
  m.pass_split(1);
  int n_skip = 0, n_run = 0, n_near4 = 0, n_near6 = 0;
  static const nr_gpu_pass_shape_t shapes[] = {{48, 13, 2, 4, 2, 4, 1}, {96, 13, 1, 6, 2, 8, 1}, {24, 13, 4, 2, 2, 1, 1}};
  for (const auto &sh : shapes)
    for (double noise : {25.0, 10.0, 3.0, -3.0}) {
      GpuPassFixture::noise_db = noise;
      GpuPassFixture fx = GpuPassFixture::from_shape(sh, 11);
      GpuPassFixture::noise_db = 20.0;
      if (!fx.tb_ok_built) continue;
      fx.bind();
      nr_gpu_pass_shape_t w = fx.job->shape;
      ASSERT_EQ(0, m.pass_warm(&w, fx.job->dj.qam_mode, fx.job->dj.align, fx.job->dj.want_evm));
      std::vector<uint8_t> ub(fx.tb.A / 8 + 64, 0), gbuf(fx.tb.A / 8 + 64, 0);
      nr_gpu_pass_result_t u{}, g{};
      u.tb = ub.data();
      g.tb = gbuf.data();
      fx.job->gate_on = 0;
      std::vector<int16_t> brfo((size_t)4 * 2 * 4096 * 2);
      u.brfo_rows = brfo.data();
      ASSERT_EQ(0, m.pass_try(fx.job, &u));
      ASSERT_FALSE(u.skip);
      const double db = u.gate_db;
      if (sh.nl == 1) { // the host function of the EVMGRANT dmrs_snr field (nr_pdsch_dmrs_snr.h) on the same estimate rows and residual noise
        const int16_t *rows[4] = {};
        for (int a = 0; a < 4; a++) rows[a] = brfo.data() + ((size_t)a * 2 + 1) * 4096 * 2;
        nr_dmrs_snr_t ref;
        nr_dmrs_snr_init(&ref);
        nr_dmrs_snr_fill(&ref, rows, 4, 12 * fx.job->nrb, u.nvar_ant, fx.job->dmrs_type, 1.0);
        EXPECT_NEAR(ref.comb_db, db, 0.05) << "device DM-RS SNR vs the host function, nrb " << sh.nrb_class << " noise " << noise;
      }
      u.brfo_rows = nullptr;
      for (int tbl : {0, 1})
        for (int mcs = 0; mcs < 28; mcs++) {
          const double rq = nr_required_snr_db(mcs, tbl);
          if (!std::isfinite(rq)) continue;
          fx.job->gate_on = 1;
          fx.job->gate_thr_db = (float)(rq - NR_SNR_GATE_MARGIN_DB);
          memset(gbuf.data(), 0xEE, gbuf.size());
          g = {};
          g.tb = gbuf.data();
          ASSERT_EQ(0, m.pass_try(fx.job, &g));
          const bool want = nr_snr_no_chance(db, mcs, tbl);
          EXPECT_EQ(want, g.skip != 0) << "snr " << db << " req " << rq << " mcs " << mcs << " tbl " << tbl;
          EXPECT_FLOAT_EQ((float)db, g.gate_db);
          if (db - rq < -4.0 && db - rq > -5.0) n_near4++;
          if (db - rq < -5.0 && db - rq > -7.0) n_near6++;
          if (g.skip) {
            n_skip++;
            EXPECT_EQ(0, g.tb_ok);
            EXPECT_EQ(0, g.need_cpu);
          } else {
            n_run++;
            EXPECT_EQ(u.tb_ok, g.tb_ok) << "the CRC verdict of a non-gated grant changed";
            EXPECT_EQ(0, memcmp(u.seg_ok, g.seg_ok, sizeof(u.seg_ok)));
            EXPECT_EQ(u.nvar, g.nvar);
            if (u.tb_ok) EXPECT_EQ(0, memcmp(ub.data(), gbuf.data(), fx.tb.A / 8));
          }
        }
    }
  m.pass_split(0);
  printf("GpuPassGate.Dl: %d skipped, %d decoded as ungated (%d grants within req-5..req-4, %d within req-7..req-5 dB)\n", n_skip, n_run, n_near4, n_near6);
  EXPECT_GT(n_skip, 0);
  EXPECT_GT(n_run, 0);
  EXPECT_GT(n_near4, 0) << "no grant at about req - 4 dB in the sweep";
  EXPECT_GT(n_near6, 0) << "no grant at about req - 6 dB in the sweep";
}

/* UL: the same on the PUSCH graph (the UL DM-RS SNR UL_PUSCH_CRC reports): skipped grants have tb_ok 0 and skip 1; the others equal the ungated pass; gate_db is the host's
 * nr_ulsch_dmrs_snr_db of the same slot within single-precision rounding. */
TEST(GpuPassGate, UlSkipsNoChanceAndKeepsTheRest) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && ul_ready(m));
  int n_skip = 0, n_run = 0;
  for (int nrb : {24, 106})
    for (double noise : {25.0, 8.0, 0.0, -6.0}) {
      GpuPassFixture::noise_db = noise;
      GpuPassFixture::first_sym = 1;
      const nr_gpu_pass_shape_t sh = {(uint16_t)nrb, 13, 1, 4, 2, 1, 1};
      GpuPassFixture fx = GpuPassFixture::from_shape(sh, 3);
      GpuPassFixture::noise_db = 20.0;
      if (!fx.tb_ok_built) continue;
      ul_warm_class(m, nrb, 2, fx.tb.C);
      ASSERT_FALSE(HasFatalFailure());
      UlSlot sl;
      sl.build(fx, 0, -1, 0, 0);
      sl.in.gate_on = 0;
      ASSERT_EQ(0, nr_gpu_pass_ul_run(&sl.in, &sl.out));
      const nr_gpu_pass_result_t u = sl.out.r;
      const std::vector<uint8_t> utb = sl.tb;
      const double host_db = sl.out.dmrs_snr_db;
      if (std::isfinite(host_db)) EXPECT_NEAR(host_db, u.gate_db, 0.05) << "device vs host UL DM-RS SNR";
      for (int tbl : {0, 1})
        for (int mcs = 0; mcs < 28; mcs++) {
          const double rq = nr_required_snr_db(mcs, tbl);
          if (!std::isfinite(rq)) continue;
          sl.build(fx, 0, -1, 0, 0);
          sl.in.gate_on = 1;
          sl.in.gate_thr_db = (float)(rq - NR_SNR_GATE_MARGIN_DB);
          ASSERT_EQ(0, nr_gpu_pass_ul_run(&sl.in, &sl.out));
          const nr_gpu_pass_result_t &g = sl.out.r;
          const bool want = nr_snr_no_chance(u.gate_db, mcs, tbl);
          EXPECT_EQ(want, g.skip != 0) << "snr " << u.gate_db << " req " << rq;
          if (g.skip) {
            n_skip++;
            EXPECT_EQ(0, g.tb_ok);
          } else {
            n_run++;
            EXPECT_EQ(u.tb_ok, g.tb_ok);
            EXPECT_EQ(0, memcmp(u.seg_ok, g.seg_ok, sizeof(u.seg_ok)));
            if (u.tb_ok) EXPECT_EQ(0, memcmp(utb.data(), sl.tb.data(), fx.tb.A / 8));
          }
        }
    }
  printf("GpuPassGate.Ul: %d skipped, %d decoded as ungated\n", n_skip, n_run);
  EXPECT_GT(n_skip, 0);
  EXPECT_GT(n_run, 0);
}

/* Suite GpuPassFence is defined last and so runs last (gtest runs suites in the order of their first test): it raises the allocation fence of the platform. A (BG, Z, R) the init did not build is refused and counted, nothing is
 * allocated or built on the launch path. */
extern "C" void nr_dl_gpu_memory_stats(uint64_t *, uint64_t *);
TEST(GpuPassFence, LdpcDevNoAllocOnMiss) {
  if (!have_gpu()) GTEST_SKIP();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  void *dl = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_NOLOAD);
  auto misses = (unsigned (*)(void))dlsym(dl, "nr_pdsch_gpu_ldpc_dev_misses");
  auto stats = (void (*)(uint64_t *, uint64_t *))dlsym(dl, "nr_dl_gpu_memory_stats");
  ASSERT_TRUE(misses && stats);
  unsetenv("ISAC_GPU_FORCE_DISCRETE");
  nr_gpu_buf_t lin = {}, bits = {};
  ASSERT_EQ(0, nr_gpu_buf_alloc(&lin, NR_GPU_LDPC_DEV_IN_BYTES));
  ASSERT_EQ(0, nr_gpu_buf_alloc(&bits, NR_GPU_LDPC_DEV_BITS_BYTES));
  nr_gpu_rx_started(); /* the fence */
  uint64_t a0, d0, a1, d1;
  stats(&a0, &d0);
  const unsigned m0 = misses();
  nr_gpu_ldpc_job_t bad = {};
  bad.BG = 2; bad.R = 7; bad.Z = 64; bad.K = 640; bad.it_max = 8; /* R = 7 is no code rate of BG2: never built */
  EXPECT_EQ(-1, m.ldpc(&bad, 1, &lin, &bits, nullptr));
  EXPECT_EQ(m0 + 1, misses());
  nr_gpu_ldpc_job_t good = bad;
  good.R = 15;
  EXPECT_EQ(0, m.ldpc(&good, 1, &lin, &bits, nullptr)); /* a prepared key still launches behind the fence */
  cudaDeviceSynchronize();
  stats(&a1, &d1);
  EXPECT_EQ(a0, a1) << "the launch path allocated";
  EXPECT_EQ(d0, d1) << "an allocation was attempted behind the fence";
  EXPECT_EQ(m0 + 1, misses());
}

/* After the fence: the pass launches a warm class 1000 times, graph hits grow, nothing is allocated by the platform or by the module. */
TEST(GpuPassFence, NoAllocAfterFence) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(m.pass_try) << "libpdsch_gpu.so has no nr_gpu_pass_dl_try";
  ASSERT_TRUE(pass_ready(m));
  const nr_gpu_pass_shape_t sh = GpuPassFixture::warm_shapes()[4 * 4 * 1 + 4 * 0 + 0]; // nrb 48, nl 1, qm 2
  GpuPassFixture fx = GpuPassFixture::from_shape(sh, 21);
  ASSERT_TRUE(fx.tb_ok_built);
  fx.bind();
  std::vector<uint8_t> tb(fx.tb.A / 8 + 64);
  nr_gpu_pass_result_t r{};
  r.tb = tb.data();
  /* Task 4b: a segmented grant (4 PRG segments, class 4) warmed before the fence, launched after it */
  const auto sd = seg_layout(16, 0, 4, 0, 0);
  nr_gpu_pass_shape_t ssh = {16, 13, 2, 4, 2, 1, 1, (uint16_t)nr_gpu_pass_nseg_class((int)sd.size())};
  GpuPassFixture sfx = GpuPassFixture::from_shape(ssh, 22, -1, &sd);
  ASSERT_TRUE(sfx.tb_ok_built);
  sfx.shape.ncb_class = (uint16_t)nr_gpu_pass_ncb_class(sfx.tb.C);
  sfx.shape.nrb_class = (uint16_t)nr_gpu_pass_nrb_class(16);
  sfx.bind();
  /* warm the class before the fence; when another test already raised it the class was warmed by SegmentedGraphEqualsStagePath, else the segmented part is skipped */
  nr_gpu_pass_shape_t sw = sfx.shape;
  bool have_seg = m.pass_warm(&sw, 3, 0, 0) == 0;
  if (!have_seg) {
    nr_gpu_pass_result_t pr{};
    std::vector<uint8_t> ptb(sfx.tb.A / 8 + 64);
    pr.tb = ptb.data();
    have_seg = m.pass_try(sfx.job, &pr) == 0;
  }
  std::vector<uint8_t> stb(sfx.tb.A / 8 + 64);
  nr_gpu_pass_result_t sr{};
  sr.tb = stb.data();
  nr_gpu_rx_started(); /* the fence */
  uint64_t pa0, pd0, pa1, pd1, o0[16], o1[16];
  nr_gpu_memory_stats(&pa0, &pd0);
  m.pass_counters(o0);
  const size_t heap0 = mallinfo2().uordblks, arenas0 = mallinfo2().arena + mallinfo2().hblkhd;
  for (int i = 0; i < 1000; i++) {
    ASSERT_EQ(0, m.pass_try(fx.job, &r)) << "launch " << i;
    ASSERT_TRUE(r.tb_ok) << "launch " << i;
    if (have_seg && i % 5 == 0) {
      ASSERT_EQ(0, m.pass_try(sfx.job, &sr)) << "segmented launch " << i;
      ASSERT_TRUE(sr.tb_ok) << "segmented launch " << i;
    }
  }
  nr_gpu_memory_stats(&pa1, &pd1);
  m.pass_counters(o1);
  /* host side: no malloc on the pass path (the per-consumer buffers of the body are allocated at consumer start; the module's pass path owns none) */
  EXPECT_NEAR((double)heap0, (double)mallinfo2().uordblks, 512.0) << "the pass path allocated host memory (512 B: the driver's wait nodes of a rare blocking wait)";
  EXPECT_EQ(arenas0, (size_t)(mallinfo2().arena + mallinfo2().hblkhd));
  EXPECT_EQ(pa0, pa1) << "the platform allocated on the pass path";
  EXPECT_EQ(pd0, pd1);
  EXPECT_EQ(o0[10], o1[10]) << "the module allocated (captured) on the pass path";
  EXPECT_EQ(o0[1] + 1000 + (have_seg ? 200 : 0), o1[1]);
  EXPECT_EQ(o0[12] + (have_seg ? 200 : 0), o1[12]) << "segmented launches";
  EXPECT_EQ(0, memcmp(tb.data(), fx.tb.payload.data(), fx.tb.A / 8));
  if (have_seg) EXPECT_EQ(0, memcmp(stb.data(), sfx.tb.payload.data(), sfx.tb.A / 8));
}

/* The PUSCH pass behind the fence: a warm class launches 500 times, nothing is allocated by the platform, the module or the host builder (its Gold-sequence cache is warm from one earlier pass). */
TEST(GpuPassFence, UlNoAllocAfterFence) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  ASSERT_TRUE(ul_ready(m));
  const nr_gpu_pass_shape_t sh = {48, 13, 1, 2, 2, 1, 1};
  GpuPassFixture fx = GpuPassFixture::from_shape(sh, 31);
  ASSERT_TRUE(fx.tb_ok_built);
  ul_warm_class(m, 48, 2, fx.tb.C);
  ASSERT_FALSE(HasFatalFailure());
  UlSlot sl;
  sl.build(fx, 0, -1, 0, 0);
  ASSERT_EQ(0, nr_gpu_pass_ul_run(&sl.in, &sl.out)); /* warms the Gold cache of the host builder */
  ASSERT_TRUE(sl.out.r.tb_ok);
  void *dl = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_NOLOAD);
  auto ulc = (void (*)(uint64_t *))dlsym(dl, "nr_gpu_pass_ul_counters");
  ASSERT_TRUE(ulc);
  nr_gpu_rx_started(); /* the fence */
  uint64_t pa0, pd0, pa1, pd1, u0[16], u1[16], o0[16], o1[16];
  nr_gpu_memory_stats(&pa0, &pd0);
  m.pass_counters(o0);
  ulc(u0);
  const size_t heap0 = mallinfo2().uordblks, arenas0 = mallinfo2().arena + mallinfo2().hblkhd;
  for (int i = 0; i < 500; i++) {
    sl.tb.assign(sl.tb.size(), 0xcc);
    ASSERT_EQ(0, nr_gpu_pass_ul_run(&sl.in, &sl.out)) << "launch " << i;
    ASSERT_TRUE(sl.out.r.tb_ok) << "launch " << i;
  }
  nr_gpu_memory_stats(&pa1, &pd1);
  m.pass_counters(o1);
  ulc(u1);
  EXPECT_NEAR((double)heap0, (double)mallinfo2().uordblks, 512.0) << "the UL pass path allocated host memory (512 B: the driver's wait nodes of a rare blocking wait)";
  EXPECT_EQ(arenas0, (size_t)(mallinfo2().arena + mallinfo2().hblkhd));
  EXPECT_EQ(pa0, pa1) << "the platform allocated on the pass path";
  EXPECT_EQ(pd0, pd1);
  EXPECT_EQ(o0[10], o1[10]) << "the module captured a graph on the pass path";
  EXPECT_EQ(u0[0] + 500, u1[0]);
  EXPECT_EQ(u0[1] + 500, u1[1]);
  EXPECT_EQ(0, memcmp(sl.tb.data(), fx.tb.payload.data(), fx.tb.A / 8));
}

/* last: tripping the gate switches the pass off process-wide */
/* the CRC-OK snr gate (the FP32 rate gate on snr_big): nothing before 64 shadows, 1 % keeps the pass, above 1 % trips */
TEST(GpuPassZUlSnrGate, Arithmetic) {
  EXPECT_FALSE(nr_fp32_gate_bad(63, 63));
  EXPECT_TRUE(nr_fp32_gate_bad(64, 64));
  EXPECT_FALSE(nr_fp32_gate_bad(100, 1));
  EXPECT_TRUE(nr_fp32_gate_bad(101, 2));
  int trip = 0;
  for (int i = 0; i < 99; i++) trip |= nr_gpu_pass_ul_snr_note(0);
  EXPECT_EQ(0, trip);
  EXPECT_EQ(0, nr_gpu_pass_ul_snr_note(1)) << "1 big in 100 keeps the pass";
  EXPECT_EQ(1, nr_gpu_pass_ul_snr_note(1)) << "2 big in 101 trips";
}


/* Task 8b: ISAC_GPU_PASS_RETRY_OLD sends exactly the retry bodies (2nd+ body of a call, every worker body) to the old path, never the first body, never with the flag off */
TEST(GpuPassRetry, OnlyRetryBodiesGoOld)
{
  EXPECT_EQ(nr_gpu_pass_retry_to_old(1, 1, 0, 0), 0);
  EXPECT_EQ(nr_gpu_pass_retry_to_old(1, 2, 0, 0), 1);
  EXPECT_EQ(nr_gpu_pass_retry_to_old(1, 1, 1, 0), 1);
  EXPECT_EQ(nr_gpu_pass_retry_to_old(0, 3, 1, 0), 0);
  EXPECT_EQ(nr_gpu_pass_retry_to_old(1, 2, 0, 1), 0); /* quiet / predicted / A-B alternate body keeps the pass */
  EXPECT_EQ(nr_gpu_pass_retry_to_old(1, 1, 1, 1), 0);
}
