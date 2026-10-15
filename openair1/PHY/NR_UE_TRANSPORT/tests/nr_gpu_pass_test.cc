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
  nr_gpu_pass_cfg_t c = {4, 273, (const int16_t *)dt.dev, 4096, 20};
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
  for (int inst = 0; inst < NR_GPU_PASS_RING; inst++) {
    m.pass_hold(0xfu & ~(1u << inst), 1);
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
    m.pass_hold(0xfu, 0);
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
  EXPECT_EQ(heap0, (size_t)mallinfo2().uordblks) << "the pass path allocated host memory";
  EXPECT_EQ(arenas0, (size_t)(mallinfo2().arena + mallinfo2().hblkhd));
  EXPECT_EQ(pa0, pa1) << "the platform allocated on the pass path";
  EXPECT_EQ(pd0, pd1);
  EXPECT_EQ(o0[10], o1[10]) << "the module allocated (captured) on the pass path";
  EXPECT_EQ(o0[1] + 1000 + (have_seg ? 200 : 0), o1[1]);
  EXPECT_EQ(o0[12] + (have_seg ? 200 : 0), o1[12]) << "segmented launches";
  EXPECT_EQ(0, memcmp(tb.data(), fx.tb.payload.data(), fx.tb.A / 8));
  if (have_seg) EXPECT_EQ(0, memcmp(stb.data(), sfx.tb.payload.data(), sfx.tb.A / 8));
}
