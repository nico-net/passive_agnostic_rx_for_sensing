/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Per-path GPU/CPU choice (nr_gpu_path.h): pure policy, registry, environment, the SHARED (GB10) invariant on a mocked platform. No device. */
#include <gtest/gtest.h>
#include "nr_gpu_path.h"
#include "nr_gpu_platform_internal.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
namespace {
int attrs[128];
std::vector<std::string> lines;
void log_message(int, const char *m) { lines.push_back(m); }
int get_count(int *n) { *n = 1; return 0; }
int get_device(int *d) { *d = 0; return 0; }
int attr(int *v, int a, int) { *v = attrs[a]; return 0; }
int mem_info(size_t *f, size_t *t) { *f = 1024; *t = 2048; return 0; }
int fail_alloc(void **, size_t) { return 2; }
int fail_host(void **, size_t, unsigned) { return 2; }
int ok0(void *) { return 0; }
int dptr(void **d, void *h, unsigned) { *d = h; return 0; }
int reg(void *, size_t, unsigned) { return 0; }
int copy(void *, const void *, size_t, int, void *) { return 0; }
int sync0(void *) { return 0; }
int dsync() { return 0; }
int flags(unsigned) { return 0; }
int lasterr() { return 0; }
int probe_ok() { return NR_GPU_PROBE_OK; }
const nr_gpu_runtime_t runtime = {get_count, get_device, attr, mem_info, fail_alloc, fail_host, ok0, ok0, dptr, reg, ok0, copy, sync0, dsync, flags, lasterr, nullptr};
const char *envs[] = {"NR_GPU_PATH_SELECT", "NR_GPU_PATH_TRIAL", "NR_GPU_PATH_MARGIN", "NR_GPU_PATH_FEP", "NR_GPU_PATH_LDPC_DL", "NR_GPU_PATH_PASS_DL",
                      "NR_GPU_PATH_CHEST", "NR_GPU_FORCE_MODEL"};
/* mocked device: GB10 (integrated, pageable through the host tables) or a PCIe card with HMM (sens6 RTX 4060 Ti) */
void mock(bool gb10)
{
  memset(attrs, 0, sizeof(attrs));
  attrs[19] = attrs[41] = attrs[89] = 1; /* map host, UVA, managed */
  attrs[75] = gb10 ? 12 : 8;
  attrs[76] = gb10 ? 1 : 9;
  attrs[18] = gb10;     /* integrated */
  attrs[88] = 1;        /* pageable access (HMM on sens6) */
  attrs[100] = gb10;    /* host page tables */
  nr_gpu_test_reset(&runtime);
  nr_gpu_test_set_probe(probe_ok);
}
class Path : public testing::Test {
  void SetUp() override
  {
    for (const char *e : envs) unsetenv(e);
    nr_gpu_set_logger(log_message);
    lines.clear();
    nr_gpu_path_test_reset();
  }
  void TearDown() override
  {
    for (const char *e : envs) unsetenv(e);
    nr_gpu_path_test_reset();
    nr_gpu_set_logger(nullptr);
  }
};
int n_lines(const char *needle)
{
  int n = 0;
  for (auto &l : lines) n += l.find(needle) != std::string::npos;
  return n;
}
} // namespace

TEST_F(Path, ParseOverrideAndSelect)
{
  EXPECT_EQ(nr_gpu_path_parse_override(nullptr), NR_GPU_PATH_UNSET);
  EXPECT_EQ(nr_gpu_path_parse_override(""), NR_GPU_PATH_UNSET);
  EXPECT_EQ(nr_gpu_path_parse_override("auto"), NR_GPU_PATH_UNSET);
  EXPECT_EQ(nr_gpu_path_parse_override("gpu"), NR_GPU_PATH_GPU);
  EXPECT_EQ(nr_gpu_path_parse_override("cpu"), NR_GPU_PATH_CPU);
  EXPECT_EQ(nr_gpu_path_parse_override("GPU"), -2);
  EXPECT_EQ(nr_gpu_path_parse_select(nullptr), NR_GPU_PATH_SEL_AUTO);
  EXPECT_EQ(nr_gpu_path_parse_select("bench"), NR_GPU_PATH_SEL_BENCH);
  EXPECT_EQ(nr_gpu_path_parse_select("off"), NR_GPU_PATH_SEL_OFF);
  EXPECT_EQ(nr_gpu_path_parse_select("x"), -1);
}

TEST_F(Path, PolicyBeginTable)
{
  const char *r;
  /* SHARED: GPU without measurement unless bench is selected (GB10 invariant) */
  EXPECT_EQ(nr_gpu_path_policy_begin(NR_GPU_SHARED, NR_GPU_PATH_SEL_AUTO, NR_GPU_PATH_UNSET, 1, NR_GPU_PATH_MEASURE_BENCH, &r), NR_GPU_PATH_GPU);
  EXPECT_STREQ(r, "shared");
  EXPECT_EQ(nr_gpu_path_policy_begin(NR_GPU_SHARED, NR_GPU_PATH_SEL_AUTO, NR_GPU_PATH_UNSET, 1, NR_GPU_PATH_MEASURE_TRIAL, &r), NR_GPU_PATH_GPU);
  EXPECT_EQ(nr_gpu_path_policy_begin(NR_GPU_SHARED, NR_GPU_PATH_SEL_AUTO, NR_GPU_PATH_UNSET, 1, NR_GPU_PATH_MEASURE_NONE, &r), NR_GPU_PATH_GPU);
  EXPECT_EQ(nr_gpu_path_policy_begin(NR_GPU_SHARED, NR_GPU_PATH_SEL_BENCH, NR_GPU_PATH_UNSET, 1, NR_GPU_PATH_MEASURE_BENCH, &r), NR_GPU_PATH_TRIAL);
  EXPECT_EQ(nr_gpu_path_policy_begin(NR_GPU_SHARED, NR_GPU_PATH_SEL_BENCH, NR_GPU_PATH_UNSET, 1, NR_GPU_PATH_MEASURE_NONE, &r), NR_GPU_PATH_GPU);
  EXPECT_STREQ(r, "shared");
  /* DISCRETE: measure; nothing to measure -> fail closed */
  EXPECT_EQ(nr_gpu_path_policy_begin(NR_GPU_DISCRETE, NR_GPU_PATH_SEL_AUTO, NR_GPU_PATH_UNSET, 1, NR_GPU_PATH_MEASURE_BENCH, &r), NR_GPU_PATH_TRIAL);
  EXPECT_STREQ(r, "measure");
  EXPECT_EQ(nr_gpu_path_policy_begin(NR_GPU_DISCRETE, NR_GPU_PATH_SEL_AUTO, NR_GPU_PATH_UNSET, 1, NR_GPU_PATH_MEASURE_NONE, &r), NR_GPU_PATH_CPU);
  EXPECT_STREQ(r, "direct_only");
  /* overrides; the module state wins over gpu */
  EXPECT_EQ(nr_gpu_path_policy_begin(NR_GPU_DISCRETE, NR_GPU_PATH_SEL_AUTO, NR_GPU_PATH_GPU, 1, NR_GPU_PATH_MEASURE_NONE, &r), NR_GPU_PATH_GPU);
  EXPECT_STREQ(r, "env");
  EXPECT_EQ(nr_gpu_path_policy_begin(NR_GPU_SHARED, NR_GPU_PATH_SEL_AUTO, NR_GPU_PATH_CPU, 1, NR_GPU_PATH_MEASURE_NONE, &r), NR_GPU_PATH_CPU);
  EXPECT_STREQ(r, "env");
  EXPECT_EQ(nr_gpu_path_policy_begin(NR_GPU_DISCRETE, NR_GPU_PATH_SEL_AUTO, NR_GPU_PATH_GPU, 0, NR_GPU_PATH_MEASURE_BENCH, &r), NR_GPU_PATH_CPU);
  EXPECT_STREQ(r, "gpu_unavailable");
  /* select off = the behaviour before this layer */
  EXPECT_EQ(nr_gpu_path_policy_begin(NR_GPU_DISCRETE, NR_GPU_PATH_SEL_OFF, NR_GPU_PATH_UNSET, 1, NR_GPU_PATH_MEASURE_NONE, &r), NR_GPU_PATH_GPU);
  EXPECT_STREQ(r, "select_off");
  EXPECT_EQ(nr_gpu_path_policy_begin(NR_GPU_NONE, NR_GPU_PATH_SEL_AUTO, NR_GPU_PATH_UNSET, 1, NR_GPU_PATH_MEASURE_BENCH, &r), NR_GPU_PATH_CPU);
  EXPECT_STREQ(r, "cpu_tier");
}

TEST_F(Path, VerdictFailsClosed)
{
  const char *r;
  EXPECT_EQ(nr_gpu_path_policy_verdict(100, 200, 8, 8, 8, 1.0, &r), NR_GPU_PATH_GPU);
  EXPECT_STREQ(r, "faster");
  EXPECT_EQ(nr_gpu_path_policy_verdict(200, 100, 8, 8, 8, 1.0, &r), NR_GPU_PATH_CPU);
  EXPECT_STREQ(r, "slower");
  EXPECT_EQ(nr_gpu_path_policy_verdict(100, 100, 8, 8, 8, 1.0, &r), NR_GPU_PATH_CPU); /* a tie stays on the CPU */
  EXPECT_EQ(nr_gpu_path_policy_verdict(150, 100, 8, 8, 8, 2.0, &r), NR_GPU_PATH_GPU); /* margin */
  for (double bad : {0.0, -1.0, (double)NAN, (double)INFINITY}) {
    EXPECT_EQ(nr_gpu_path_policy_verdict(bad, 100, 8, 8, 8, 1.0, &r), NR_GPU_PATH_CPU);
    EXPECT_STREQ(r, "bench_fail");
    EXPECT_EQ(nr_gpu_path_policy_verdict(100, bad, 8, 8, 8, 1.0, &r), NR_GPU_PATH_CPU);
  }
  EXPECT_EQ(nr_gpu_path_policy_verdict(1, 100, 7, 8, 8, 1.0, &r), NR_GPU_PATH_CPU);
  EXPECT_STREQ(r, "bench_fail");
}

TEST_F(Path, Median)
{
  const double a[] = {5, 1, 3}, b[] = {4, 1, 3, 2};
  EXPECT_DOUBLE_EQ(nr_gpu_path_median(a, 3), 3);
  EXPECT_DOUBLE_EQ(nr_gpu_path_median(b, 4), 2.5);
  EXPECT_TRUE(std::isnan(nr_gpu_path_median(a, 0)));
}

TEST_F(Path, SharedTierGb10Unchanged)
{
  /* GB10-like mocked device: the platform picks SHARED; every path is GPU at once, one line each, no trial, samples ignored */
  mock(true);
  ASSERT_EQ(nr_gpu_platform_probe()->model, NR_GPU_SHARED);
  for (int p = 0; p < NR_GPU_PATH_N; p++) {
    const int m = p == NR_GPU_PATH_FEP ? NR_GPU_PATH_MEASURE_BENCH : NR_GPU_PATH_MEASURE_TRIAL;
    EXPECT_EQ(nr_gpu_path_begin((nr_gpu_path_t)p, 1, m), NR_GPU_PATH_GPU) << nr_gpu_path_name((nr_gpu_path_t)p);
    EXPECT_EQ(nr_gpu_path_use_gpu((nr_gpu_path_t)p), 1);
    for (int i = 0; i < 100; i++) {
      EXPECT_EQ(nr_gpu_path_pick((nr_gpu_path_t)p), 1);
      nr_gpu_path_sample((nr_gpu_path_t)p, 1, 1e9); /* a slow GPU never moves a SHARED path */
      nr_gpu_path_sample((nr_gpu_path_t)p, 0, 1);
    }
    EXPECT_EQ(nr_gpu_path_finish((nr_gpu_path_t)p, 1e9, 1, 8, nullptr), NR_GPU_PATH_GPU);
    nr_gpu_path_fail((nr_gpu_path_t)p, "x");
    EXPECT_EQ(nr_gpu_path_state((nr_gpu_path_t)p), NR_GPU_PATH_GPU);
  }
  EXPECT_EQ(n_lines("GPU_PATH path="), NR_GPU_PATH_N);
  EXPECT_EQ(n_lines("tier=shared choice=gpu reason=shared"), NR_GPU_PATH_N);
  nr_gpu_test_reset(&runtime);
}

TEST_F(Path, DiscreteTierMeasures)
{
  mock(false); /* sens6: HMM over PCIe -> DISCRETE */
  ASSERT_EQ(nr_gpu_platform_probe()->model, NR_GPU_DISCRETE);
  EXPECT_EQ(nr_gpu_path_begin(NR_GPU_PATH_FEP, 1, NR_GPU_PATH_MEASURE_BENCH), NR_GPU_PATH_TRIAL);
  EXPECT_EQ(nr_gpu_path_begin(NR_GPU_PATH_CHEST, 1, NR_GPU_PATH_MEASURE_NONE), NR_GPU_PATH_CPU);
  EXPECT_NE(std::string(nr_gpu_path_test_last_line()).find("path=chest tier=discrete choice=cpu reason=direct_only"), std::string::npos);
  EXPECT_EQ(nr_gpu_path_finish(NR_GPU_PATH_FEP, 90, 300, 8, "N=4096"), NR_GPU_PATH_GPU);
  EXPECT_NE(std::string(nr_gpu_path_test_last_line()).find("path=fep tier=discrete choice=gpu reason=faster mode=bench"), std::string::npos);
  /* a second begin keeps the decision */
  EXPECT_EQ(nr_gpu_path_begin(NR_GPU_PATH_FEP, 1, NR_GPU_PATH_MEASURE_BENCH), NR_GPU_PATH_GPU);
  nr_gpu_test_reset(&runtime);
}

TEST_F(Path, BenchFailIsCpu)
{
  EXPECT_EQ(nr_gpu_path_begin_tier(NR_GPU_PATH_FEP, NR_GPU_DISCRETE, 1, NR_GPU_PATH_MEASURE_BENCH), NR_GPU_PATH_TRIAL);
  EXPECT_EQ(nr_gpu_path_finish(NR_GPU_PATH_FEP, -1, 300, 0, "gpu_error"), NR_GPU_PATH_CPU);
  EXPECT_NE(std::string(nr_gpu_path_test_last_line()).find("reason=bench_fail"), std::string::npos);
  EXPECT_EQ(nr_gpu_path_use_gpu(NR_GPU_PATH_FEP), 0);
}

TEST_F(Path, OnlineTrialAlternatesAndDecides)
{
  setenv("NR_GPU_PATH_TRIAL", "4", 1);
  EXPECT_EQ(nr_gpu_path_begin_tier(NR_GPU_PATH_PASS_DL, NR_GPU_DISCRETE, 1, NR_GPU_PATH_MEASURE_TRIAL), NR_GPU_PATH_TRIAL);
  int g = 0, c = 0;
  for (int i = 0; i < 8; i++) {
    const int s = nr_gpu_path_pick(NR_GPU_PATH_PASS_DL);
    (s ? g : c)++;
    nr_gpu_path_sample(NR_GPU_PATH_PASS_DL, s, s ? 10.0 + i : 50.0 + i);
  }
  EXPECT_EQ(g, 4);
  EXPECT_EQ(c, 4);
  EXPECT_EQ(nr_gpu_path_state(NR_GPU_PATH_PASS_DL), NR_GPU_PATH_GPU);
  EXPECT_NE(std::string(nr_gpu_path_test_last_line()).find("path=pass_dl tier=discrete choice=gpu reason=faster mode=online"), std::string::npos);
  /* decided: always GPU, samples ignored */
  for (int i = 0; i < 10; i++) EXPECT_EQ(nr_gpu_path_pick(NR_GPU_PATH_PASS_DL), 1);
}

TEST_F(Path, OnlineTrialSlowGpuGoesCpuAndOutliersDoNotFlip)
{
  setenv("NR_GPU_PATH_TRIAL", "5", 1);
  nr_gpu_path_begin_tier(NR_GPU_PATH_LDPC_DL, NR_GPU_DISCRETE, 1, NR_GPU_PATH_MEASURE_TRIAL);
  const double gpu[] = {30, 31, 29, 1, 32}, cpu[] = {20, 21, 500, 19, 22}; /* medians 30 vs 21 despite one fast GPU and one slow CPU outlier */
  for (int i = 0; i < 5; i++) { /* paired samples (LDPC: the same TB on both sides) */
    nr_gpu_path_sample(NR_GPU_PATH_LDPC_DL, 1, gpu[i]);
    nr_gpu_path_sample(NR_GPU_PATH_LDPC_DL, 0, cpu[i]);
  }
  EXPECT_EQ(nr_gpu_path_state(NR_GPU_PATH_LDPC_DL), NR_GPU_PATH_CPU);
  EXPECT_NE(std::string(nr_gpu_path_test_last_line()).find("reason=slower"), std::string::npos);
  EXPECT_EQ(nr_gpu_path_pick(NR_GPU_PATH_LDPC_DL), 0);
}

TEST_F(Path, PickCatchesUpTheSideWithFewerSamples)
{
  setenv("NR_GPU_PATH_TRIAL", "4", 1);
  nr_gpu_path_begin_tier(NR_GPU_PATH_PASS_DL, NR_GPU_DISCRETE, 1, NR_GPU_PATH_MEASURE_TRIAL);
  nr_gpu_path_sample(NR_GPU_PATH_PASS_DL, 0, 5);
  nr_gpu_path_sample(NR_GPU_PATH_PASS_DL, 0, 5);
  EXPECT_EQ(nr_gpu_path_pick(NR_GPU_PATH_PASS_DL), 1);
  nr_gpu_path_sample(NR_GPU_PATH_PASS_DL, 1, 5);
  EXPECT_EQ(nr_gpu_path_pick(NR_GPU_PATH_PASS_DL), 1);
}

TEST_F(Path, TrialIncompleteFailsClosed)
{
  setenv("NR_GPU_PATH_TRIAL", "2", 1);
  nr_gpu_path_begin_tier(NR_GPU_PATH_PASS_UL, NR_GPU_DISCRETE, 1, NR_GPU_PATH_MEASURE_TRIAL);
  int last = -1;
  for (int i = 0; i < 200 && nr_gpu_path_state(NR_GPU_PATH_PASS_UL) == NR_GPU_PATH_TRIAL; i++) {
    last = nr_gpu_path_pick(NR_GPU_PATH_PASS_UL);
    nr_gpu_path_sample(NR_GPU_PATH_PASS_UL, 0, 1); /* only the CPU side is ever measured */
  }
  (void)last;
  EXPECT_EQ(nr_gpu_path_state(NR_GPU_PATH_PASS_UL), NR_GPU_PATH_CPU);
  EXPECT_NE(std::string(nr_gpu_path_test_last_line()).find("reason=trial_incomplete"), std::string::npos);
}

TEST_F(Path, GpuErrorDuringTrialIsCpu)
{
  nr_gpu_path_begin_tier(NR_GPU_PATH_LDPC_UL, NR_GPU_DISCRETE, 1, NR_GPU_PATH_MEASURE_TRIAL);
  nr_gpu_path_fail(NR_GPU_PATH_LDPC_UL, "launch");
  EXPECT_EQ(nr_gpu_path_state(NR_GPU_PATH_LDPC_UL), NR_GPU_PATH_CPU);
  EXPECT_NE(std::string(nr_gpu_path_test_last_line()).find("reason=gpu_error (launch)"), std::string::npos);
}

TEST_F(Path, EnvOverridesAndSelect)
{
  setenv("NR_GPU_PATH_FEP", "cpu", 1);
  setenv("NR_GPU_PATH_LDPC_DL", "gpu", 1);
  setenv("NR_GPU_PATH_PASS_DL", "bogus", 1);
  EXPECT_EQ(nr_gpu_path_begin_tier(NR_GPU_PATH_FEP, NR_GPU_SHARED, 1, NR_GPU_PATH_MEASURE_BENCH), NR_GPU_PATH_CPU);
  EXPECT_EQ(nr_gpu_path_begin_tier(NR_GPU_PATH_LDPC_DL, NR_GPU_DISCRETE, 1, NR_GPU_PATH_MEASURE_TRIAL), NR_GPU_PATH_GPU);
  EXPECT_EQ(nr_gpu_path_begin_tier(NR_GPU_PATH_PASS_DL, NR_GPU_DISCRETE, 1, NR_GPU_PATH_MEASURE_TRIAL), NR_GPU_PATH_TRIAL);
  EXPECT_EQ(n_lines("NR_GPU_PATH_PASS_DL=\"bogus\""), 1);
  EXPECT_EQ(nr_gpu_path_begin_tier(NR_GPU_PATH_UL_FEP, NR_GPU_DISCRETE, 0, NR_GPU_PATH_MEASURE_NONE), NR_GPU_PATH_CPU);
  nr_gpu_path_test_reset();
  setenv("NR_GPU_PATH_SELECT", "off", 1);
  EXPECT_EQ(nr_gpu_path_begin_tier(NR_GPU_PATH_CHEST, NR_GPU_DISCRETE, 1, NR_GPU_PATH_MEASURE_NONE), NR_GPU_PATH_GPU);
  nr_gpu_path_test_reset();
  setenv("NR_GPU_PATH_SELECT", "bench", 1);
  unsetenv("NR_GPU_PATH_FEP");
  EXPECT_EQ(nr_gpu_path_begin_tier(NR_GPU_PATH_FEP, NR_GPU_SHARED, 1, NR_GPU_PATH_MEASURE_BENCH), NR_GPU_PATH_TRIAL);
}

TEST_F(Path, ConcurrentSamplesDecideOnce)
{
  setenv("NR_GPU_PATH_TRIAL", "16", 1);
  nr_gpu_path_begin_tier(NR_GPU_PATH_LDPC_DL, NR_GPU_DISCRETE, 1, NR_GPU_PATH_MEASURE_TRIAL);
  std::vector<std::thread> th;
  for (int t = 0; t < 8; t++)
    th.emplace_back([t] {
      for (int i = 0; i < 50; i++) nr_gpu_path_sample(NR_GPU_PATH_LDPC_DL, t & 1, (t & 1) ? 1.0 : 2.0);
    });
  for (auto &x : th) x.join();
  EXPECT_EQ(nr_gpu_path_state(NR_GPU_PATH_LDPC_DL), NR_GPU_PATH_GPU);
  EXPECT_EQ(n_lines("path=ldpc_dl tier=discrete choice=gpu"), 1);
}
