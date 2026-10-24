/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Host-RAM budget: parsers, the probe on fixture /proc + /sys/fs/cgroup trees (128 GB unified, 14.8 GB discrete, 4 GB, cgroup v2 / v1 limited), shares and fit. */
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include "nr_host_membudget.h"
namespace {
constexpr size_t MB = (size_t)1 << 20, GB = (size_t)1 << 30;
void put(const std::string &path, const std::string &text)
{
  std::string d;
  for (size_t i = 1; i < path.size(); i++)
    if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
  FILE *f = fopen(path.c_str(), "w");
  ASSERT_NE(nullptr, f);
  fputs(text.c_str(), f);
  fclose(f);
}
struct Fix {
  std::string root;
  Fix()
  {
    char t[] = "/tmp/hostmem_fixXXXXXX";
    root = mkdtemp(t);
  }
  ~Fix() { std::string c = "rm -rf " + root; if (system(c.c_str())) {} }
  void meminfo(size_t avail_mb) { put(root + "/proc/meminfo", "MemTotal: 1 kB\nMemAvailable:   " + std::to_string(avail_mb * 1024) + " kB\nSwapTotal: 8388604 kB\n"); }
  void v2(const std::string &path, const std::string &max, const std::string &high, size_t cur_mb)
  {
    put(root + "/proc/self/cgroup", "0::" + path + "\n");
    put(root + "/cg" + path + "/memory.max", max + "\n");
    put(root + "/cg" + path + "/memory.high", high + "\n");
    put(root + "/cg" + path + "/memory.current", std::to_string(cur_mb * MB) + "\n");
  }
  nr_host_probe_t probe() { nr_host_probe_t p; nr_host_probe_at((root + "/proc").c_str(), (root + "/cg").c_str(), &p); return p; }
};
}
TEST(HostParse, MeminfoLimitsAndPaths) {
  size_t v;
  ASSERT_EQ(0, nr_host_parse_meminfo("MemTotal: 16 kB\nMemAvailable:   2048 kB\n", &v));
  EXPECT_EQ(2048u << 10, v);
  EXPECT_NE(0, nr_host_parse_meminfo("MemTotal: 16 kB\n", &v));
  ASSERT_EQ(0, nr_host_parse_limit("max\n", &v)); EXPECT_EQ(SIZE_MAX, v);
  ASSERT_EQ(0, nr_host_parse_limit("11811160064\n", &v)); EXPECT_EQ(11811160064u, v);
  ASSERT_EQ(0, nr_host_parse_limit("9223372036854771712\n", &v)); EXPECT_EQ(SIZE_MAX, v); /* v1 "none" */
  EXPECT_NE(0, nr_host_parse_limit("garbage", &v));
  char p[128];
  ASSERT_EQ(0, nr_host_parse_cgroup_path("0::/user.slice/run-r1.scope\n", 0, p, sizeof p)); EXPECT_STREQ("/user.slice/run-r1.scope", p);
  ASSERT_EQ(0, nr_host_parse_cgroup_path("12:cpu,cpuacct:/x\n4:memory:/docker/abc\n1:name=systemd:/y\n", 1, p, sizeof p)); EXPECT_STREQ("/docker/abc", p);
  ASSERT_EQ(0, nr_host_parse_cgroup_path("5:cpu,memory:/z\n", 1, p, sizeof p)); EXPECT_STREQ("/z", p);
  EXPECT_NE(0, nr_host_parse_cgroup_path("1:cpu:/x\n", 1, p, sizeof p));
}
TEST(HostBudget, CalcAndFit) {
  EXPECT_EQ(SIZE_MAX, nr_host_membudget_calc(SIZE_MAX));
  EXPECT_EQ(0u, nr_host_membudget_calc(1 * GB));            /* below the reserve */
  EXPECT_EQ(10 * GB / 5 * 4 - NR_HOST_MB_RESERVE, nr_host_membudget_calc(10 * GB));
  EXPECT_EQ(2000, nr_host_membudget_fit(SIZE_MAX, 0, 3 * MB, 64, 2000));
  EXPECT_EQ(2000, nr_host_membudget_fit(2000 * 3 * MB, 0, 3 * MB, 64, 2000));
  EXPECT_EQ(1000, nr_host_membudget_fit(1000 * 3 * MB, 0, 3 * MB, 64, 2000));
  EXPECT_EQ(0, nr_host_membudget_fit(63 * 3 * MB, 0, 3 * MB, 64, 2000));         /* below the floor: not ready */
  EXPECT_EQ(0, nr_host_membudget_fit(10 * MB, 20 * MB, 3 * MB, 64, 2000));       /* share below the fixed part */
}
TEST(HostProbe, Gb10Unified128GbIsFarAboveEveryNeed) {
  Fix f; f.meminfo(118 * 1024); f.v2("/user.slice/x.scope", "max", "max", 6000);
  auto p = f.probe();
  EXPECT_EQ(118 * GB, p.mem_available); EXPECT_EQ(SIZE_MAX, p.cgroup_headroom); EXPECT_EQ(118 * GB, p.avail); EXPECT_STREQ("MemAvailable", p.limited_by);
  const size_t B = nr_host_membudget_calc(p.avail);
  EXPECT_GT(B, 90 * GB);
  /* the 273 PRB GrantWork pool (12 GB) fits whole in its 60 % share */
  EXPECT_EQ(4096, nr_host_membudget_fit(B / 100 * 60, 0, 2935296, 64, 4096));
}
TEST(HostProbe, Sens6DiscreteBudgetCutsTheGrantWorkPool) {
  Fix f; f.meminfo(13200); f.v2("/", "max", "max", 0);
  auto p = f.probe();
  EXPECT_EQ(13200 * MB, p.avail);
  const size_t B = nr_host_membudget_calc(p.avail);
  const size_t share = B / 100 * 60;
  const long n = nr_host_membudget_fit(share, 0, 2935296, 64, 4096);   /* 273 PRB, 4 consumers x depth 64 x 16 */
  EXPECT_GE(n, 64); EXPECT_LT(n, 4096);
  EXPECT_LE((size_t)n * 2935296, share);
  EXPECT_LT((size_t)n * 2935296, 6 * GB);                               /* never the 12 GB that killed the host */
}
TEST(HostProbe, FourGbHostScalesAndATwoGbHostGoesToTheCpuPath) {
  Fix f; f.meminfo(3500); f.v2("/", "max", "max", 0);
  const size_t B = nr_host_membudget_calc(f.probe().avail);
  EXPECT_LT(B, 2 * GB);
  const long n = nr_host_membudget_fit(B / 100 * 60, 0, 2935296, 64, 4096);   /* 273 PRB pool on a 4 GB host: scaled, above the floor */
  EXPECT_GE(n, 64); EXPECT_LT(n, 1000);
  Fix g; g.meminfo(2000); g.v2("/", "max", "max", 0);
  const size_t B2 = nr_host_membudget_calc(g.probe().avail);
  EXPECT_EQ(0, nr_host_membudget_fit(B2 / 100 * 60, 0, 2935296, 64, 4096));    /* 2 GB: below the floor -> no pool, CPU path */
}
TEST(HostProbe, CgroupV2LimitWinsOverMemAvailableAndCountsUsage) {
  Fix f; f.meminfo(13200); f.v2("/user.slice/run-r42.scope", "11811160064", "max", 1000);   /* MemoryMax=11G, 1000 MB used */
  auto p = f.probe();
  EXPECT_EQ(11264 * MB - 1000 * MB, p.cgroup_headroom);
  EXPECT_EQ(p.cgroup_headroom, p.avail); EXPECT_STREQ("cgroup_v2", p.limited_by);
}
TEST(HostProbe, CgroupHighAndAncestorLimitsAreHonoured) {
  Fix f; f.meminfo(13200); f.v2("/a/b", "max", "8589934592", 2048);                          /* memory.high 8 GiB, 2 GiB used */
  put(f.root + "/cg/a/memory.max", "6442450944\n"); put(f.root + "/cg/a/memory.high", "max\n"); put(f.root + "/cg/a/memory.current", std::to_string(5000 * MB) + "\n"); /* parent 6 GiB max, 5000 MB used */
  auto p = f.probe();
  EXPECT_EQ(6144 * MB - 5000 * MB, p.avail);                                                   /* the parent is the tighter one */
  EXPECT_STREQ("cgroup_v2", p.limited_by);
}
TEST(HostProbe, CgroupV1Fallback) {
  Fix f; f.meminfo(13200);
  put(f.root + "/proc/self/cgroup", "4:memory:/docker/abc\n");
  put(f.root + "/cg/memory/docker/abc/memory.limit_in_bytes", "4294967296\n");
  put(f.root + "/cg/memory/docker/abc/memory.usage_in_bytes", std::to_string(1024 * MB) + "\n");
  auto p = f.probe();
  EXPECT_EQ(3072 * MB, p.avail); EXPECT_STREQ("cgroup_v1", p.limited_by);
}
TEST(HostProbe, UnlimitedCgroupAndMissingFilesFallBackToMemAvailable) {
  Fix f; f.meminfo(9000); put(f.root + "/proc/self/cgroup", "4:memory:/x\n");
  put(f.root + "/cg/memory/x/memory.limit_in_bytes", "9223372036854771712\n"); put(f.root + "/cg/memory/x/memory.usage_in_bytes", "1000\n");
  EXPECT_EQ(9000 * MB, f.probe().avail);
  Fix g;                                                     /* no /proc/meminfo at all: unknown -> unlimited */
  auto p = g.probe();
  EXPECT_EQ(SIZE_MAX, p.avail); EXPECT_STREQ("unknown", p.limited_by);
}
static size_t g_gpu = 0;
static size_t gpu_committed() { return g_gpu; }
TEST(HostState, SharesFixedCommitsUnifiedDeductionAndFormat) {
  setenv("NR_HOST_MEM_BUDGET_MB", "13200", 1);
  nr_host_membudget_test_reset();
  const size_t B = nr_host_membudget_total();
  EXPECT_EQ(nr_host_membudget_calc(13200 * MB), B);
  const size_t s0 = nr_host_membudget_share(NR_HOST_MEM_GW_POOL);
  EXPECT_EQ(B / 100 * 60, s0);
  nr_host_membudget_commit(NR_HOST_MEM_QUEUE, 1 * GB, 1.0);       /* fixed modules reduce what the scalable pool may take */
  nr_host_membudget_commit(NR_HOST_MEM_CSIRS, 600 * MB, 1.0);
  EXPECT_EQ((B - 1 * GB - 600 * MB) / 100 * 60, nr_host_membudget_share(NR_HOST_MEM_GW_POOL));
  EXPECT_EQ(1 * GB + 600 * MB, nr_host_membudget_committed());
  /* unified memory: the GPU's commitments come out of the same pool */
  g_gpu = 3 * GB;
  nr_host_membudget_set_unified(gpu_committed);
  EXPECT_EQ(B - 3 * GB, nr_host_membudget_total());
  g_gpu = 100 * GB;                                               /* more than the host budget: clamps at 0, never wraps */
  EXPECT_EQ(0u, nr_host_membudget_total());
  nr_host_membudget_set_unified(nullptr);
  nr_host_membudget_commit(NR_HOST_MEM_GW_POOL, 1500 * MB, 0.37);
  nr_host_membudget_note_failure(NR_HOST_MEM_GW_POOL);
  EXPECT_EQ(1u, nr_host_membudget_failures(NR_HOST_MEM_GW_POOL));
  char buf[512];
  nr_host_membudget_format(buf, sizeof buf);
  EXPECT_NE(nullptr, strstr(buf, "avail_mb=13200 limited_by=NR_HOST_MEM_BUDGET_MB")) << buf;
  EXPECT_NE(nullptr, strstr(buf, "gw_pool=1500MB/scale=0.370/failures=1")) << buf;
  EXPECT_NE(nullptr, strstr(buf, "queue=1024MB")) << buf;
  unsetenv("NR_HOST_MEM_BUDGET_MB");
  nr_host_membudget_test_reset();
}
TEST(HostState, UnlimitedWhenUnknownKeepsEverySize) {
  setenv("NR_HOST_MEM_BUDGET_MB", "1", 1); unsetenv("NR_HOST_MEM_BUDGET_MB");
  nr_host_membudget_test_reset();
  EXPECT_EQ(SIZE_MAX, nr_host_membudget_calc(SIZE_MAX));
  EXPECT_EQ(4096, nr_host_membudget_fit(SIZE_MAX, 0, 2935296, 64, 4096));
}
