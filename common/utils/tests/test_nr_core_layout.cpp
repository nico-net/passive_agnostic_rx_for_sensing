// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Host-derived core plan (nr_core_layout): fixtures for the DGX Spark GB10, sens6, small hosts, SMT, NUMA, restricted masks.
//
// DGX fixture (reconstructed, not read from the host): env_dgx.txt lscpu -e (L3 0 = CPUs 0-9, L3 1 = 10-19; A725 2808 MHz
// 0-4 / 10-14, X925 3900 MHz 5-9 / 15-19, no SMT, 1 NUMA node), the 2026-10-09 host rule (isolcpus = nohz_full = 15-19, so the inherited
// mask is 0-14), DPDK lcores 17-19 (CORE-POOL 5), and the on-air layout of POOL_20261021 (RT 5, UHD ctrl 15, ring 16,
// pool 6 7 8 9 3 4, float lane 10-14, GPU worker 10, USS 12, nID 13, GPU check 10-14, recovery 11,13,14, dashboard 2).
// ASSUMED: cpu_capacity big 1017/997, little 460 (only the big/little split matters: 10 % buckets); llc keys 0 and 10.
#include <gtest/gtest.h>
#include <string>
#include <vector>
extern "C" {
#include "nr_core_layout.h"
#include "nr_core_pool.h" // NR_POOL_FLOAT_MERGE_MIN_TOKENS (macro only: the layout test does not link the pool)
}

namespace {
struct Topo {
  nr_cl_topo_t t;
  Topo(int n)
  {
    memset(&t, 0, sizeof(t));
    t.ncpu = n;
    t.gpu_node = -1;
    for (int c = 0; c < NR_CL_MAX_CPUS; c++) t.core[c] = c, t.llc[c] = -1, t.node[c] = -1;
    for (int c = 0; c < n; c++) t.online[c] = t.allowed[c] = 1, t.cap[c] = 1024, t.llc[c] = 0, t.node[c] = 0;
  }
  Topo &cap(int a, int b, int v) { for (int c = a; c <= b; c++) t.cap[c] = v; return *this; }
  Topo &llc(int a, int b, int v) { for (int c = a; c <= b; c++) t.llc[c] = v; return *this; }
  Topo &node(int a, int b, int v) { for (int c = a; c <= b; c++) t.node[c] = v; return *this; }
  Topo &iso(int a, int b) { for (int c = a; c <= b; c++) t.isolated[c] = t.nohz[c] = 1, t.allowed[c] = 0; return *this; }
  Topo &only(std::vector<int> cs) { for (int c = 0; c < t.ncpu; c++) t.allowed[c] = 0; for (int c : cs) t.allowed[c] = 1; return *this; }
  Topo &dpdk(int a, int b) { for (int c = a; c <= b; c++) t.dpdk[c] = 1; return *this; }
  Topo &radio(bool r) { t.radio = r; return *this; }
};
std::string mask(const unsigned char *m, int n = 64)
{
  char b[512];
  nr_cl_mask_str(m, n, b, sizeof(b));
  return b;
}
std::string pool(const nr_core_layout_t &L)
{
  std::string s;
  for (int i = 0; i < L.npool; i++) s += (i ? "," : "") + std::to_string(L.pool[i]);
  return s.empty() ? "-" : s;
}
nr_core_layout_t build(const Topo &T, const nr_cl_over_t *o = nullptr)
{
  nr_core_layout_t L;
  nr_cl_build(&T.t, o, &L);
  char line[4096];
  nr_cl_format(&T.t, &L, line, sizeof(line));
  printf("%s\n", line);
  return L;
}
Topo dgx(bool radio)
{
  Topo T(20);
  T.cap(0, 4, 460).cap(10, 14, 460).cap(5, 9, 1017).cap(15, 19, 1017).cap(7, 7, 997).llc(0, 9, 0).llc(10, 19, 10).iso(15, 19).radio(radio);
  if (radio) T.dpdk(17, 19);
  T.t.rt_capable = radio; /* on air under sudo */
  return T;
}
void expect_dgx_layout(const nr_core_layout_t &L, bool radio)
{
  EXPECT_EQ(L.mode, NR_CL_SEP);
  EXPECT_EQ(L.W, 14);
  EXPECT_EQ(L.rt, 5);
  EXPECT_EQ(L.uhd, radio ? 15 : -1);
  EXPECT_EQ(L.ring, radio ? 16 : -1);
  EXPECT_EQ(pool(L), "6,7,8,9,3,4");
  EXPECT_EQ(mask(L.lane), "10-14");
  EXPECT_EQ(mask(L.hk), "0-2");
  EXPECT_EQ(mask(L.home), "0-2,10-14"); /* = inherited 0-14 minus pool minus RT/IO, as nr_pool site_init built it */
  EXPECT_EQ(L.dashboard, 2);
  EXPECT_EQ(L.gpu, 10);
  EXPECT_EQ(L.uss, 12);
  EXPECT_EQ(L.nid, 13);
  EXPECT_EQ(mask(L.check), "10-14");
  EXPECT_EQ(mask(L.recov), "11,13-14");
}
void expect_inside(const nr_core_layout_t &L, const Topo &T)
{ /* nothing outside the inherited mask except the radio IO on isolated CPUs */
  for (int i = 0; i < L.npool; i++) EXPECT_TRUE(T.t.allowed[L.pool[i]] && !T.t.isolated[L.pool[i]]) << "pool " << L.pool[i];
  for (int c = 0; c < T.t.ncpu; c++) {
    if (L.lane[c] || L.home[c] || L.hk[c] || L.check[c] || L.recov[c]) { EXPECT_TRUE(T.t.allowed[c] && !T.t.isolated[c]) << "cpu " << c; }
  }
  if (L.rt >= 0) { EXPECT_TRUE(T.t.allowed[L.rt]) << "rt"; }
  if (L.dashboard >= 0) { EXPECT_TRUE(T.t.allowed[L.dashboard]) << "dashboard"; }
  for (int i = 0; i < L.npool; i++) EXPECT_FALSE(L.lane[L.pool[i]] || L.hk[L.pool[i]] || L.pool[i] == L.rt) << "pool " << L.pool[i];
}
} // namespace

TEST(CoreLayout, DgxOnAirReproducesTodaysLayout)
{
  Topo T = dgx(true);
  expect_dgx_layout(build(T), true);
}

TEST(CoreLayout, DgxOnAirWithTheExplicitEnvIsTheSame)
{ /* POOL_20261021 + starttest.sh: the explicit keys name exactly what the topology derives */
  Topo T = dgx(true);
  nr_cl_over_t o;
  nr_cl_over_none(&o);
  o.rt = 5, o.ring = 16, o.gpu = 10, o.uss = 12, o.nid = 13;
  o.has_uhd = o.has_lane = o.has_check = true;
  o.uhd[15] = 1;
  for (int c = 10; c <= 14; c++) o.lane[c] = o.check[c] = 1;
  nr_core_layout_t L = build(T, &o);
  expect_dgx_layout(L, true);
  EXPECT_NE(std::string(L.src).find("rt=env"), std::string::npos);
}

TEST(CoreLayout, DgxWithoutL3InformationStillReproduces)
{ /* GB10 sysfs may not expose index3: the distance-to-RT order alone gives the same split */
  Topo T = dgx(true);
  for (int c = 0; c < 20; c++) T.t.llc[c] = -1;
  expect_dgx_layout(build(T), true);
}

TEST(CoreLayout, DgxRfsimHasNoRadioIo)
{
  Topo T = dgx(false);
  expect_dgx_layout(build(T), false);
}

TEST(CoreLayout, DgxNoIsolationBootKeepsPollersOffThePool)
{
  Topo T(20);
  T.cap(0, 4, 460).cap(10, 14, 460).cap(5, 9, 1017).cap(15, 19, 1017).llc(0, 9, 0).llc(10, 19, 10).dpdk(17, 19).radio(true);
  nr_core_layout_t L = build(T);
  EXPECT_EQ(L.rt, 5);
  EXPECT_EQ(L.uhd, 15); /* big cores of the other cluster, the DPDK lcores excluded: the same 15/16 as with isolcpus */
  EXPECT_EQ(L.ring, 16);
  EXPECT_EQ(pool(L), "6,7,8,9,3,4");
  EXPECT_EQ(mask(L.lane), "10-14");
  for (int i = 0; i < L.npool; i++) EXPECT_TRUE(L.pool[i] != 15 && L.pool[i] != 16 && L.pool[i] < 17);
  expect_inside(L, T);
}

/* sens6 (Intel Core Ultra 5 235): P 0-5 cpu_capacity 1024, E 6-13 cpu_capacity 793 (two 4-CPU L2 clusters, one L3),
 * no SMT, 1 NUMA node. Read on the host 2026-10-24: isolcpus = nohz_full = 1-2 (irqaffinity 0,3-13). The SENS6-SIM
 * 2026-10-23 runs had isolcpus 2-3 (sens6_iso23). */
Topo sens6() { return Topo(14).cap(0, 5, 1024).cap(6, 13, 793).iso(1, 2); }
Topo sens6_iso23() { return Topo(14).cap(0, 5, 1024).cap(6, 13, 793).iso(2, 3); }
void expect_sens6_rest(const nr_core_layout_t &L)
{ /* the general part of the sens6 layout, the same for both boots and for rfsim / on air */
  EXPECT_EQ(L.mode, NR_CL_COMPACT);
  EXPECT_EQ(L.W, 11);
  EXPECT_EQ(pool(L), "4,5,6,7,8,9"); /* 2 P + 4 E tokens (the SIM runs measured 6 7 8 9 4 from the DGX list) */
  EXPECT_EQ(mask(L.lane), "10-13");  /* the SIM runs' float lane (10-14 with 14 missing) */
  EXPECT_EQ(mask(L.hk), "0");
  EXPECT_EQ(mask(L.home), "0,10-13");
  EXPECT_EQ(L.dashboard, 0);
  EXPECT_EQ(L.gpu, 10);
  EXPECT_EQ(L.uss, 12);
  EXPECT_EQ(L.nid, 13);
  EXPECT_EQ(mask(L.check), "10-13");
  EXPECT_EQ(mask(L.recov), "11,13");
}

TEST(CoreLayout, Sens6Rfsim)
{
  Topo T = sens6();
  nr_core_layout_t L = build(T);
  EXPECT_EQ(L.rt, 3); /* the first P core outside the isolated set, 0 stays the housekeeping anchor */
  EXPECT_EQ(L.uhd, -1);
  EXPECT_EQ(L.ring, -1);
  expect_sens6_rest(L);
  expect_inside(L, T);
}

TEST(CoreLayout, Sens6OnAirPutsTheRadioOnTheIsolatedCpus)
{
  Topo T = sens6();
  T.radio(true);
  nr_core_layout_t L = build(T);
  EXPECT_EQ(L.uhd, 1);
  EXPECT_EQ(L.ring, 2);
  EXPECT_EQ(L.rt, 3);
  expect_sens6_rest(L);
  expect_inside(L, T);
}

TEST(CoreLayout, Sens6OldBootIsolated23)
{
  Topo T = sens6_iso23();
  nr_core_layout_t L = build(T);
  EXPECT_EQ(L.rt, 1);
  expect_sens6_rest(L);
  T.radio(true);
  L = build(T);
  EXPECT_EQ(L.uhd, 2);
  EXPECT_EQ(L.ring, 3);
  EXPECT_EQ(L.rt, 1);
  expect_sens6_rest(L);
}

TEST(CoreLayout, Sens6RunArmTasksetWithExplicitRtCore)
{ /* frozen run_arm.sh: taskset -c 0-7, ISAC_UE_RT_CORE=2 (the 2-3 boot): the reader keeps 2, the radio IO never shares it */
  Topo T = sens6_iso23();
  T.only({0, 1, 2, 3, 4, 5, 6, 7}).radio(true);
  nr_cl_over_t o;
  nr_cl_over_none(&o);
  o.rt = 2;
  nr_core_layout_t L = build(T, &o);
  EXPECT_EQ(L.rt, 2);
  EXPECT_EQ(L.uhd, 3);
  EXPECT_EQ(L.ring, 1);
  EXPECT_EQ(L.mode, NR_CL_SMALL);
  EXPECT_EQ(pool(L), "4,5,6,7");
  EXPECT_EQ(mask(L.lane), "0");
}

TEST(CoreLayout, Sens6TasksetNarrowedIsSmall)
{ /* taskset -c 0,1,4-7 on the 2-3 boot: 6 CPUs, W = 5 -> one lane CPU */
  Topo T = sens6_iso23();
  T.only({0, 1, 4, 5, 6, 7});
  nr_core_layout_t L = build(T);
  EXPECT_EQ(L.mode, NR_CL_SMALL);
  EXPECT_EQ(L.W, 5);
  EXPECT_EQ(L.rt, 1);
  EXPECT_EQ(mask(L.lane), "0");
  EXPECT_EQ(pool(L), "4,5,6,7");
  expect_inside(L, T);
}

TEST(CoreLayout, IsolatedCpusInsideANarrowedMaskAreTheRtSet)
{ /* taskset -c 2-13 on the 2-3 boot: the operator hands us the isolated P cores -> the RT reader goes there, nothing else does */
  Topo T = sens6_iso23();
  std::vector<int> v;
  for (int c = 2; c <= 13; c++) v.push_back(c);
  T.only(v);
  nr_core_layout_t L = build(T);
  EXPECT_EQ(L.rt, 2);
  for (int i = 0; i < L.npool; i++) EXPECT_GE(L.pool[i], 4);
  EXPECT_FALSE(L.lane[3] || L.home[3] || L.hk[3]);
  EXPECT_EQ(L.W, 10);
}

TEST(CoreLayout, FourCpus)
{
  Topo T(4);
  nr_core_layout_t L = build(T);
  EXPECT_EQ(L.mode, NR_CL_SMALL);
  EXPECT_EQ(L.rt, 1);
  EXPECT_EQ(L.W, 3);
  EXPECT_EQ(mask(L.lane), "0");
  EXPECT_EQ(pool(L), "2,3");
  EXPECT_EQ(L.dashboard, 0);
  expect_inside(L, T);
}

TEST(CoreLayout, TwoCpusAndOneCpu)
{
  {
    Topo T(2);
    nr_core_layout_t L = build(T);
    EXPECT_EQ(L.rt, 0);
    EXPECT_EQ(L.W, 1);
    EXPECT_EQ(mask(L.lane), "1");
    EXPECT_EQ(L.npool, 0); /* no pool: the consumers run unpinned on the lane */
    expect_inside(L, T);
  }
  {
    Topo T(1);
    nr_core_layout_t L = build(T);
    EXPECT_EQ(L.rt, 0);
    EXPECT_EQ(L.W, 0);
    EXPECT_EQ(mask(L.lane), "0");
    EXPECT_EQ(L.npool, 0);
    EXPECT_EQ(L.dashboard, 0);
  }
}

TEST(CoreLayout, WRuleThresholds)
{
  struct { int ncpu; nr_cl_mode_t mode; int npool; const char *home; } cases[] = {
      /* equal-capacity hosts, no isolation: W = ncpu - 1 (the RT reader on CPU 1), anchor 0 */
      {6, NR_CL_SMALL, 4, "0"},           /* W 5 */
      {7, NR_CL_COMPACT, 3, "0,5-6"},     /* W 6: lane 2 */
      {8, NR_CL_COMPACT, 4, "0,6-7"},     /* W 7: lane 2 */
      {9, NR_CL_COMPACT, 4, "0,6-8"},     /* W 8: lane 3 */
      {11, NR_CL_COMPACT, 5, "0,7-10"},   /* W 10: lane 4 */
      {14, NR_CL_COMPACT, 8, "0,10-13"},  /* W 13: lane 4 */
      {15, NR_CL_SEP, 6, "0,8-14"},       /* W 14: lane 10-14, hk 0,8,9 */
      {32, NR_CL_SEP, 23, nullptr},       /* W 31 */
  };
  for (auto &k : cases) {
    Topo T(k.ncpu);
    nr_core_layout_t L = build(T);
    EXPECT_EQ(L.mode, k.mode) << k.ncpu;
    EXPECT_EQ(L.npool, k.npool) << k.ncpu;
    if (k.home) { EXPECT_EQ(mask(L.home), k.home) << k.ncpu; }
    expect_inside(L, T);
  }
}

TEST(CoreLayout, SmtKeepsTheRtSiblingIdleAndSpreadsThePool)
{ /* 8 cores / 16 threads, siblings c and c+8 */
  Topo T(16);
  for (int c = 8; c < 16; c++) T.t.core[c] = c - 8;
  nr_core_layout_t L = build(T);
  EXPECT_EQ(L.rt, 1);
  EXPECT_EQ(L.W, 14);
  bool used9 = L.lane[9] || L.home[9] || L.hk[9];
  for (int i = 0; i < L.npool; i++) used9 |= L.pool[i] == 9;
  EXPECT_FALSE(used9) << "the RT reader's sibling must stay idle";
  for (int i = 0; i < L.npool; i++)
    for (int j = i + 1; j < L.npool; j++) EXPECT_NE(T.t.core[L.pool[i]], T.t.core[L.pool[j]]) << "two tokens on one physical core";
  expect_inside(L, T);
}

TEST(CoreLayout, NumaPrefersTheGpuNode)
{
  Topo T(16);
  T.node(0, 7, 0).node(8, 15, 1).llc(0, 7, 0).llc(8, 15, 8);
  T.t.gpu_node = 1;
  nr_core_layout_t L = build(T);
  EXPECT_EQ(L.rt, 8);
  for (int i = 0; i < L.npool; i++) EXPECT_GE(L.pool[i], 8) << "pool on the GPU node";
  for (int c = 8; c < 16; c++) EXPECT_FALSE(L.lane[c]) << "the float lane goes to the other node";
  expect_inside(L, T);
}

TEST(CoreLayout, MissingCpusInOverridesAreIgnored)
{
  Topo T(4);
  nr_cl_over_t o;
  nr_cl_over_none(&o);
  o.rt = 40, o.uss = 99, o.nid = 3;
  o.has_pool = true;
  o.pool[30] = o.pool[31] = 1;
  nr_core_layout_t L = build(T, &o);
  EXPECT_EQ(L.rt, 1);
  EXPECT_EQ(L.uss, -1);
  EXPECT_EQ(L.nid, 3);
  EXPECT_EQ(pool(L), "2,3");
  const std::string s = L.src;
  EXPECT_NE(s.find("rt_env_ignored=40"), std::string::npos) << s;
  EXPECT_NE(s.find("uss_env_ignored=99"), std::string::npos) << s;
  EXPECT_NE(s.find("pool_env_ignored"), std::string::npos) << s;
}

TEST(CoreLayout, ExplicitRtOnAnIsolatedCpuIsHonoured)
{ /* nr-ue.c semantics: ISAC_UE_RT_CORE=2 on sens6 opts the reader into an isolated core */
  Topo T = sens6_iso23();
  nr_cl_over_t o;
  nr_cl_over_none(&o);
  o.rt = 2;
  nr_core_layout_t L = build(T, &o);
  EXPECT_EQ(L.rt, 2);
  EXPECT_EQ(L.W, 12); /* the reader is outside the general set now */
  EXPECT_EQ(L.anchor, 0);
}

TEST(CoreLayout, ExplicitPoolList)
{
  Topo T = dgx(true);
  nr_cl_over_t o;
  nr_cl_over_none(&o);
  o.has_pool = true;
  for (int c : {7, 3, 4, 0, 1, 8, 9}) o.pool[c] = 1;
  nr_core_layout_t L = build(T, &o);
  EXPECT_EQ(pool(L), "7,8,9,0,1,3,4");
  EXPECT_FALSE(L.home[7] || L.home[0]);
  EXPECT_TRUE(L.home[2] && L.home[6]);
}

TEST(CoreLayout, ProcessPlanIsInactiveUntilInit)
{
  EXPECT_FALSE(nr_core_layout_active());
  EXPECT_EQ(nr_core_layout(), nullptr);
  EXPECT_EQ(nr_core_layout_filter_cpu(5000, "x"), 5000); /* inactive: identity (legacy behaviour) */
  EXPECT_EQ(nr_core_layout_env_default("ISAC_PDSCH_CORES"), nullptr);
  Topo T = sens6();
  nr_core_layout_set_for_test(&T.t, nullptr);
  ASSERT_TRUE(nr_core_layout_active());
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_PDSCH_CORES"), "4,5,6,7,8,9");
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_PUSCH_CORES"), "");
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_PDCCH_USS_CORE"), "12");
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_UE_RT_CORE"), "3");
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_FLOAT_CORES"), "10-13");
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_DASHBOARD_CORE"), "0");
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_UHD_CTRL_CORES"), "");
  EXPECT_EQ(nr_core_layout_env_default("ISAC_SOMETHING_ELSE"), nullptr);
  EXPECT_EQ(nr_core_layout_filter_cpu(2, "x"), 2);  /* isolated: an explicit pin may name it */
  EXPECT_EQ(nr_core_layout_filter_cpu(14, "x"), -1); /* does not exist */
  EXPECT_EQ(nr_core_layout_filter_cpu(15, "x"), -1); /* the old hard-coded UHD control core */
  EXPECT_EQ(nr_core_layout_filter_cpu(1, "x"), 1);
  /* a 4-CPU host: no lane CPU for the nID worker -> "-1" (highest CPU of the lane), USS / GPU worker float */
  Topo F(4);
  nr_core_layout_set_for_test(&F.t, nullptr);
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_PDCCH_NID_GPU_CORE"), "-1");
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_PDCCH_USS_CORE"), "");
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_FLOAT_CORES"), "0");
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_PDSCH_CORES"), "2,3");
}

TEST(CoreLayout, DgxExportedKeysEqualTodaysSiteValues)
{ /* what nr_core_layout_init() exports on the DGX on air equals the hard-coded/site values the receiver uses today:
   * nr_env_core defaults (PDSCH 6,7,8,9,3,4, GPU worker 10, UHD 15, USS 12, nID 13), --passive-rx float lane 10-14,
   * nr_bgq_start check cores 10-14, nr_dashboard CPU 2, POOL_20261021 RT 5 / ring 16 */
  Topo T = dgx(true);
  nr_core_layout_set_for_test(&T.t, nullptr);
  const std::pair<const char *, const char *> want[] = {
      {"ISAC_UE_RT_CORE", "5"},         {"ISAC_UHD_CTRL_CORES", "15"},   {"ISAC_USRP_RING_CORE", "16"},
      {"ISAC_PDSCH_CORES", "6,7,8,9,3,4"}, {"ISAC_PUSCH_CORES", ""},      {"ISAC_FLOAT_CORES", "10-14"},
      {"ISAC_GPU_CHECK_CORES", "10-14"}, {"ISAC_GPU_WORKER_CORE", "10"}, {"ISAC_PDCCH_USS_CORE", "12"},
      {"ISAC_PDCCH_NID_GPU_CORE", "13"}, {"ISAC_DASHBOARD_CORE", "2"}};
  for (auto &w : want) EXPECT_STREQ(nr_core_layout_env_default(w.first), w.second) << w.first;
  /* rfsim on the DGX: the same general layout, no radio IO */
  Topo R = dgx(false);
  nr_core_layout_set_for_test(&R.t, nullptr);
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_PDSCH_CORES"), "6,7,8,9,3,4");
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_FLOAT_CORES"), "10-14");
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_UHD_CTRL_CORES"), "");
}

TEST(CoreLayout, DashboardOverride)
{
  Topo T = sens6();
  nr_cl_over_t o;
  nr_cl_over_none(&o);
  o.dash = 5;
  EXPECT_EQ(build(T, &o).dashboard, 5);
  o.dash = 40;
  nr_core_layout_t L = build(T, &o);
  EXPECT_EQ(L.dashboard, 0);
  EXPECT_NE(std::string(L.src).find("dashboard_env_ignored=40"), std::string::npos);
}

TEST(CoreLayout, ProbeOfThisHostIsConsistent)
{
  nr_cl_topo_t t;
  ASSERT_EQ(nr_cl_topo_read(&t, false), 0);
  ASSERT_GT(t.ncpu, 0);
  nr_core_layout_t L;
  nr_cl_build(&t, nullptr, &L);
  char line[4096];
  nr_cl_format(&t, &L, line, sizeof(line));
  printf("%s\n", line);
  EXPECT_GE(L.rt, 0);
  EXPECT_TRUE(t.allowed[L.rt]);
  for (int i = 0; i < L.npool; i++) EXPECT_TRUE(t.allowed[L.pool[i]] && t.online[L.pool[i]]);
}

// ---------------------------------------------------------------------------------------------------------------------
// B2 (mpci-main plan R3): N cells, one RT reader per cell. Fixtures are SYNTHETIC hosts (labelled in each name):
//  dgx = DGX Spark GB10 (above), sens6 = x86 14 cpus 6P+8E isolcpus 1-2 (above), cloud4 = 4-cpu x86, big64 = 64 cpus,
//  2 NUMA nodes (0-31 / 32-63), one L3 per 16 cpus, GPU on node 1, 2 capacity classes (ASSUMED; scaling proof only).
namespace {
Topo big64()
{
  Topo T(64);
  T.node(0, 31, 0).node(32, 63, 1).llc(0, 15, 0).llc(16, 31, 16).llc(32, 47, 32).llc(48, 63, 48).cap(0, 15, 800).cap(32, 47, 800);
  T.t.gpu_node = 1;
  return T;
}
Topo big64_smt()
{ /* the same with 2-way SMT: siblings c and c+32 */
  Topo T = big64();
  for (int c = 32; c < 64; c++) T.t.core[c] = c - 32;
  return T;
}
struct Fix { const char *name; Topo T; };
std::vector<Fix> fixtures()
{
  std::vector<Fix> v;
  v.push_back({"dgx_onair", dgx(true)});
  v.push_back({"dgx_rfsim", dgx(false)});
  v.push_back({"sens6_rfsim", sens6()});
  v.push_back({"sens6_onair", sens6().radio(true)});
  v.push_back({"sens6_iso23", sens6_iso23()});
  v.push_back({"cloud4", Topo(4)});
  v.push_back({"cpus2", Topo(2)});
  v.push_back({"cpus1", Topo(1)});
  v.push_back({"big64", big64()});
  v.push_back({"big64_onair", big64().radio(true)});
  v.push_back({"big64_smt", big64_smt()});
  { Topo T(16); for (int c = 8; c < 16; c++) T.t.core[c] = c - 8; v.push_back({"smt16", T}); }
  { Topo T(16); T.node(0, 7, 0).node(8, 15, 1).llc(0, 7, 0).llc(8, 15, 8); T.t.gpu_node = 1; v.push_back({"numa16", T}); }
  return v;
}
nr_core_layout_t buildn(const Topo &T, int n, const nr_cl_over_t *o = nullptr)
{
  nr_core_layout_t L;
  nr_cl_build_n(&T.t, o, n, &L);
  return L;
}
bool role_cpu(const nr_core_layout_t &L, int c)
{
  return L.lane[c] || L.home[c] || L.hk[c] || L.check[c] || L.recov[c];
}
void expect_invariants(const nr_core_layout_t &L, const Topo &T, const char *name, int n)
{
  const nr_cl_topo_t &t = T.t;
  std::string w = std::string(name) + " n=" + std::to_string(n);
  // readers: distinct, usable, never DPDK, never the anchor, not the radio IO
  EXPECT_EQ(L.n_cells, n) << w;
  EXPECT_EQ(L.rt_cell[0], L.rt) << w;
  for (int i = 0; i < n; i++) {
    const int r = L.rt_cell[i];
    if (r < 0) continue;
    EXPECT_TRUE(L.usable[r]) << w << " reader " << i;
    EXPECT_FALSE(t.dpdk[r]) << w;
    if (L.anchor >= 0) { EXPECT_NE(r, L.anchor) << w << " reader on the housekeeping anchor"; }
    EXPECT_NE(r, L.uhd) << w;
    EXPECT_NE(r, L.ring) << w;
    for (int j = i + 1; j < n; j++) { if (L.rt_cell[j] >= 0) { EXPECT_NE(r, L.rt_cell[j]) << w; } }
    for (int k = 0; k < L.npool; k++) EXPECT_NE(L.pool[k], r) << w << " pool token on a reader";
    if (L.W > 0) { EXPECT_FALSE(role_cpu(L, r)) << w << " reader " << r << " carries a non-RT role"; } /* W == 0: the reader is also the only home (existing rule) */
  }
  for (int i = n; i < NR_CL_MAX_CELLS; i++) EXPECT_EQ(L.rt_cell[i], -1) << w;
  // pool: disjoint from the radio IO and from every non-pool role; on non-isolated allowed CPUs
  for (int k = 0; k < L.npool; k++) {
    const int p = L.pool[k];
    EXPECT_TRUE(t.allowed[p] && !t.isolated[p] && !t.dpdk[p]) << w << " pool " << p;
    EXPECT_NE(p, L.uhd) << w;
    EXPECT_NE(p, L.ring) << w;
    EXPECT_FALSE(L.uhd_mask[p]) << w;
    EXPECT_FALSE(role_cpu(L, p)) << w << " pool " << p << " also in a mask";
    for (int j = k + 1; j < L.npool; j++) EXPECT_NE(p, L.pool[j]) << w;
  }
  // no non-RT role on an isolated cpu; every role on a usable cpu
  for (int c = 0; c < t.ncpu; c++) {
    if (role_cpu(L, c)) {
      EXPECT_FALSE(t.isolated[c]) << w << " cpu " << c;
      EXPECT_TRUE(L.usable[c]) << w << " cpu " << c;
    }
    if (c == L.uhd || c == L.ring) { EXPECT_TRUE(L.usable[c]) << w; }
  }
  for (int r : {L.gpu, L.uss, L.nid, L.dashboard}) {
    if (r >= 0) { EXPECT_TRUE(L.usable[r]) << w << " role cpu " << r; }
  }
  // SMT siblings of readers stay out of every general mask when the host has >= 8 general cpus
  int general = 0;
  for (int c = 0; c < t.ncpu; c++) general += t.online[c] && t.allowed[c] && !t.isolated[c] && !t.dpdk[c];
  if (general >= 8)
    for (int i = 0; i < n; i++)
      for (int c = 0; c < t.ncpu && L.rt_cell[i] >= 0; c++) {
        if (c == L.rt_cell[i] || t.core[c] != t.core[L.rt_cell[i]]) continue;
        bool sibling_is_reader = false;
        for (int j = 0; j < n; j++) sibling_is_reader |= L.rt_cell[j] == c;
        if (!sibling_is_reader) { EXPECT_FALSE(role_cpu(L, c)) << w << " sibling " << c << " of reader " << L.rt_cell[i]; }
        for (int k = 0; k < L.npool; k++) { if (!sibling_is_reader) { EXPECT_NE(L.pool[k], c) << w; } }
      }
  // the floor: the plan reports hostable exactly when every cell has a reader and the pool has >= NR_CL_MIN_POOL tokens
  bool all = true;
  for (int i = 0; i < n; i++) all &= L.rt_cell[i] >= 0;
  nr_core_layout_t again;
  EXPECT_EQ(nr_cl_build_n(&t, nullptr, n, &again) == 0, all && L.npool >= NR_CL_MIN_POOL) << w;
  if (all && L.npool >= NR_CL_MIN_POOL) { EXPECT_LE(n, L.max_cells) << w; }
}
} // namespace

TEST(CoreLayoutN, OneCellEqualsTheHistoricalBuilderByteForByte)
{
  for (auto &f : fixtures()) {
    nr_core_layout_t a, b;
    memset(&a, 0x5a, sizeof(a));
    memset(&b, 0xa5, sizeof(b));
    nr_cl_build(&f.T.t, nullptr, &a);
    nr_cl_build_n(&f.T.t, nullptr, 1, &b);
    EXPECT_EQ(0, memcmp(&a, &b, sizeof(a))) << f.name;
    char la[4096], lb[4096];
    nr_cl_format(&f.T.t, &a, la, sizeof(la));
    nr_cl_format(&f.T.t, &b, lb, sizeof(lb));
    EXPECT_STREQ(la, lb) << f.name;
    EXPECT_EQ(std::string(la).find(" cells="), std::string::npos) << f.name << ": the N=1 line must not change";
    EXPECT_EQ(b.rt_cell[0], b.rt) << f.name;
    EXPECT_EQ(b.n_cells, 1) << f.name;
    nr_core_layout_t c;
    nr_cl_build_n(&f.T.t, nullptr, 0, &c); /* clamped to 1 */
    EXPECT_EQ(0, memcmp(&c, &b, sizeof(c))) << f.name;
  }
}

TEST(CoreLayoutN, OneCellEqualsWithOverrides)
{
  Topo T = dgx(true);
  nr_cl_over_t o;
  nr_cl_over_none(&o);
  o.rt = 5, o.ring = 16, o.gpu = 10, o.uss = 12, o.nid = 13;
  nr_core_layout_t a, b;
  nr_cl_build(&T.t, &o, &a);
  nr_cl_build_n(&T.t, &o, 1, &b);
  EXPECT_EQ(0, memcmp(&a, &b, sizeof(a)));
}

TEST(CoreLayoutN, InvariantsAndDeterminismOnEveryFixture)
{
  for (auto &f : fixtures())
    for (int n = 1; n <= 4; n++) {
      nr_core_layout_t L = buildn(f.T, n), L2 = buildn(f.T, n);
      EXPECT_EQ(0, memcmp(&L, &L2, sizeof(L))) << f.name << " n=" << n << " not deterministic";
      expect_invariants(L, f.T, f.name, n);
      char la[4096], lb[4096];
      nr_cl_format(&f.T.t, &L, la, sizeof(la));
      nr_cl_format(&f.T.t, &L2, lb, sizeof(lb));
      EXPECT_STREQ(la, lb);
      if (n > 1) { EXPECT_NE(std::string(la).find(" cells=" + std::to_string(n) + " rt_cells="), std::string::npos) << la; }
    }
}

TEST(CoreLayoutN, ReaderZeroAndRadioIoDoNotDependOnTheCellCount)
{
  for (auto &f : fixtures()) {
    const nr_core_layout_t one = buildn(f.T, 1);
    for (int n = 2; n <= 4; n++) {
      const nr_core_layout_t L = buildn(f.T, n);
      EXPECT_EQ(L.rt, one.rt) << f.name;
      EXPECT_EQ(L.uhd, one.uhd) << f.name;
      EXPECT_EQ(L.ring, one.ring) << f.name;
      EXPECT_EQ(0, memcmp(L.uhd_mask, one.uhd_mask, sizeof(L.uhd_mask))) << f.name;
      EXPECT_EQ(L.anchor, one.anchor) << f.name;
    }
  }
}

TEST(CoreLayoutN, ReadersFollowTheSingleReaderRanking)
{
  { /* DGX: the four X925 cores below the radio cluster, lowest number first; the pool shrinks with W by the existing rules */
    Topo T = dgx(true);
    nr_core_layout_t L = buildn(T, 4);
    EXPECT_EQ(L.rt_cell[0], 5);
    EXPECT_EQ(L.rt_cell[1], 6);
    EXPECT_EQ(L.rt_cell[2], 7);
    EXPECT_EQ(L.rt_cell[3], 8);
    EXPECT_EQ(L.W, 11);
    EXPECT_EQ(L.mode, NR_CL_COMPACT);
    EXPECT_EQ(L.uhd, 15);
    EXPECT_EQ(L.ring, 16);
    EXPECT_EQ(L.npool, 9); /* B6: sized by demand: W - home(2) = 9 of the 4 x 6 the cells would ask (was 6 with the 5-CPU lane) */
    char line[4096];
    nr_cl_format(&T.t, &L, line, sizeof(line));
    EXPECT_NE(std::string(line).find(" cells=4 rt_cells=5,6,7,8 max_cells=12"), std::string::npos) << line;
  }
  { /* sens6: reader 0 on the first P core outside the isolated set, then the next P cores, then the E cores; 0 stays the anchor */
    Topo T = sens6();
    nr_core_layout_t L = buildn(T, 4);
    EXPECT_EQ(L.rt_cell[0], 3);
    EXPECT_EQ(L.rt_cell[1], 4);
    EXPECT_EQ(L.rt_cell[2], 5);
    EXPECT_EQ(L.rt_cell[3], 6);
    EXPECT_EQ(L.anchor, 0);
  }
  { /* big host with the GPU on node 1: every reader on node 1 until it is used up */
    Topo T = big64();
    nr_core_layout_t L = buildn(T, 4);
    for (int i = 0; i < 4; i++) EXPECT_GE(L.rt_cell[i], 32) << i;
    for (int k = 0; k < L.npool; k++) EXPECT_NE(L.pool[k], 0);
  }
  { /* SMT: the second reader takes another physical core, not the sibling of the first */
    Topo T = big64_smt();
    nr_core_layout_t L = buildn(T, 2);
    EXPECT_NE(T.t.core[L.rt_cell[0]], T.t.core[L.rt_cell[1]]);
  }
}

TEST(CoreLayoutN, MaxCellsPerFixture)
{
  // max_cells = most cells with a distinct reader each (never the anchor) and a pool of >= NR_CL_MIN_POOL (=2) tokens.
  // General CPUs C minus the anchor are the candidates; W = |C| - cells >= 3 gives pool = W - 1 >= 2 in SMALL mode.
  struct { const char *name; int expect; } e[] = {
      {"dgx_onair", 12}, {"dgx_rfsim", 12}, {"sens6_rfsim", 9}, {"sens6_onair", 9}, {"sens6_iso23", 9}, {"cloud4", 1},
      {"cpus2", 0},      {"cpus1", 0},      {"big64", 16},      {"big64_onair", 16}, {"big64_smt", 16}, {"smt16", 6}, {"numa16", 13},
  };
  for (auto &f : fixtures()) {
    int want = -99;
    for (auto &k : e) if (!strcmp(k.name, f.name)) want = k.expect;
    EXPECT_EQ(nr_cl_max_cells(&f.T.t, nullptr), want) << f.name;
    EXPECT_EQ(buildn(f.T, 1).max_cells, want) << f.name;
    EXPECT_EQ(buildn(f.T, 3).max_cells, want) << f.name;
    if (want >= 1 && want < NR_CL_MAX_CELLS) {
      nr_core_layout_t X;
      EXPECT_EQ(nr_cl_build_n(&f.T.t, nullptr, want, &X), 0) << f.name;
      EXPECT_EQ(nr_cl_build_n(&f.T.t, nullptr, want + 1, &X), 1) << f.name;
    }
  }
}

TEST(CoreLayoutN, CellCountIsClampedAndTheLineListsReadersOnlyAboveOne)
{
  Topo T = big64();
  nr_core_layout_t L = buildn(T, 99);
  EXPECT_EQ(L.n_cells, NR_CL_MAX_CELLS);
  for (int i = 0; i < NR_CL_MAX_CELLS; i++) EXPECT_GE(L.rt_cell[i], 0);
  expect_invariants(L, T, "big64", NR_CL_MAX_CELLS);
  Topo c4(4);
  nr_core_layout_t M = buildn(c4, 2); /* beyond max_cells: best effort, reported */
  EXPECT_EQ(nr_cl_build_n(&c4.t, nullptr, 2, &M), 1);
  EXPECT_EQ(M.max_cells, 1);
  EXPECT_LT(M.npool, NR_CL_MIN_POOL);
}

TEST(CoreLayoutN, ProcessPlanWithCellCount)
{
  Topo T = dgx(true);
  nr_core_layout_set_for_test(&T.t, nullptr);
  ASSERT_NE(nr_core_layout(), nullptr);
  EXPECT_EQ(nr_core_layout()->n_cells, 1);
  EXPECT_STREQ(nr_core_layout_env_default("ISAC_UE_RT_CORE"), "5");
}

// ---- CHANPOOL: the wideband feed reader as one more RT-like I/O reader of the plan ----
TEST(CoreLayoutFeed, NoFeedIsTheHistoricalBuilderByteForByte)
{
  for (bool radio : {false, true}) {
    Topo T = dgx(radio);
    for (int n : {1, 2, 3}) {
      nr_core_layout_t a, b;
      nr_cl_build_n(&T.t, nullptr, n, &a);
      nr_cl_build_nf(&T.t, nullptr, n, false, &b);
      char la[4096], lb[4096];
      nr_cl_format(&T.t, &a, la, sizeof(la));
      nr_cl_format(&T.t, &b, lb, sizeof(lb));
      EXPECT_STREQ(la, lb);
      EXPECT_EQ(b.feed, -1);
      EXPECT_FALSE(b.feed_req);
      EXPECT_EQ(std::string(lb).find("feed="), std::string::npos);
    }
  }
}

TEST(CoreLayoutFeed, DgxTwoCellsFeedReaderIsDistinctFastAndOffThePool)
{ // [DGX GB10 fixture, reconstructed] 2 cells + feed
  Topo T = dgx(true);
  nr_core_layout_t L, L0;
  nr_cl_build_n(&T.t, nullptr, 2, &L0);
  EXPECT_EQ(nr_cl_build_nf(&T.t, nullptr, 2, true, &L), 0);
  ASSERT_TRUE(L.feed_req);
  ASSERT_GE(L.feed, 0);
  EXPECT_NE(L.feed, L.rt_cell[0]);
  EXPECT_NE(L.feed, L.rt_cell[1]);
  EXPECT_NE(L.feed, L.ring);
  EXPECT_NE(L.feed, L.uhd);
  EXPECT_FALSE(L.uhd_mask[L.feed]);
  EXPECT_GE(T.t.cap[L.feed] * 10, T.t.cap[L.rt_cell[1]] * 9) << "same ranking as the cells' readers: a fast CPU while one is left";
  for (int i = 0; i < L.npool; i++) EXPECT_NE(L.pool[i], L.feed);
  EXPECT_FALSE(L.lane[L.feed] || L.hk[L.feed] || L.home[L.feed]);
  EXPECT_EQ(L.W, L0.W - 1); // one more reader off the same W
  EXPECT_EQ(L.rt_cell[0], L0.rt_cell[0]);
  EXPECT_EQ(L.rt_cell[1], L0.rt_cell[1]);
  char line[4096];
  nr_cl_format(&T.t, &L, line, sizeof(line));
  EXPECT_NE(std::string(line).find(" feed=" + std::to_string(L.feed)), std::string::npos) << line;
  expect_inside(L, T);
}

TEST(CoreLayoutFeed, SmallHostsKeepAnOpenFeedReaderOrNone)
{ // [4-cpu cloud x86 fixture]: no CPU left -> feed = -1 (the reader floats on the home lane), never a pool or RT cpu
  Topo T(4);
  nr_core_layout_t L;
  nr_cl_build_nf(&T.t, nullptr, 2, true, &L);
  ASSERT_TRUE(L.feed_req);
  if (L.feed >= 0) {
    EXPECT_NE(L.feed, L.rt_cell[0]);
    EXPECT_NE(L.feed, L.rt_cell[1]);
    for (int i = 0; i < L.npool; i++) EXPECT_NE(L.pool[i], L.feed);
  }
}

TEST(CoreLayoutFeed, HybridSens6LikeHost)
{ // [sens6 x86 fixture: 14 cpus, isolcpus 1-2 (radio IO), no hybrid]: feed taken from the general CPUs, not the isolated radio IO ones
  Topo T(14);
  T.iso(1, 2).radio(true);
  nr_core_layout_t L;
  nr_cl_build_nf(&T.t, nullptr, 2, true, &L);
  if (L.feed >= 0) {
    EXPECT_FALSE(T.t.isolated[L.feed] && (L.feed == L.ring || L.feed == L.uhd));
    EXPECT_NE(L.feed, L.rt_cell[0]);
    EXPECT_NE(L.feed, L.rt_cell[1]);
  }
}

TEST(CoreLayoutFeed, ProcessPlanAccessor)
{
  Topo T = dgx(true);
  EXPECT_EQ(nr_core_layout_feed_cpu(), -1); // inactive or not planned
  (void)T;
}

// ---------------------------------------------------------------------------------------------------------------------
// B6: the pool sized by the admitted cells' demand within the host's cores. Fixtures are the synthetic hosts above (labelled in each test).
TEST(CoreLayoutPool, HomeSetAndDemandRulesArePure)
{
  EXPECT_EQ(nr_cl_home_cpus(11, 1), 1); // FLOAT-LANE-POOL: 1 below W = 12
  EXPECT_EQ(nr_cl_home_cpus(12, 1), 2);
  EXPECT_EQ(nr_cl_home_cpus(40, 1), 2);
  EXPECT_EQ(nr_cl_home_cpus(11, 2), 1); // ceil(0.4 x 2) = 1
  EXPECT_EQ(nr_cl_home_cpus(11, 3), 2); // ceil(0.4 x 3) = 2
  EXPECT_EQ(nr_cl_home_cpus(11, 6), 3); // ceil(0.4 x 6) = 3: each cell has its own actors and callbacks
  EXPECT_EQ(nr_cl_pool_for_cells(12, 1, 6), 6) << "one cell: the plan's own pool";
  EXPECT_EQ(nr_cl_pool_for_cells(12, 2, 6), 10) << "W - home";
  EXPECT_EQ(nr_cl_pool_for_cells(30, 2, 6), 12) << "capped by the demand of two cells (2 x the one-cell pool)";
  EXPECT_EQ(nr_cl_pool_for_cells(60, 2, 31), 32) << "capped at NR_CL_MAX_POOL";
  EXPECT_EQ(nr_cl_pool_for_cells(2, 4, 6), 0) << "never negative";
}

TEST(CoreLayoutPool, DgxTwoToFourCellsGetTheirDemandNotTheFiveCpuLane)
{ // [DGX GB10 fixture, reconstructed]. Measured on air with the old rule: 2 cells + feed = 7 tokens (CORE_POOL: 8 9 1 2 3 4 10).
  Topo T = dgx(true);
  struct { int n; bool feed; int W, npool; } e[] = {{2, false, 13, 11}, {2, true, 12, 10}, {3, true, 11, 9}, {4, true, 10, 8}, {5, true, 9, 7}};
  for (auto &x : e) {
    nr_core_layout_t L;
    nr_cl_build_nf(&T.t, nullptr, x.n, x.feed, &L);
    EXPECT_EQ(L.W, x.W) << x.n;
    EXPECT_EQ(L.npool, x.npool) << x.n << " feed " << x.feed;
    EXPECT_EQ(L.npool, nr_cl_pool_for_cells(L.W, x.n, 6)) << "the pure rule is the layout";
    EXPECT_GE(L.npool, NR_POOL_FLOAT_MERGE_MIN_TOKENS) << "floats lease the pool";
    // the lane is the home set: lane = home = hk = check = recov, gpu / uss / nid unpinned (as SMALL), nothing of it in the pool
    EXPECT_EQ(mask(L.lane), mask(L.home));
    EXPECT_EQ(mask(L.hk), mask(L.home));
    EXPECT_EQ(mask(L.check), mask(L.home));
    EXPECT_EQ(mask(L.recov), mask(L.home));
    int nh = 0;
    for (int c = 0; c < T.t.ncpu; c++) nh += L.home[c];
    EXPECT_EQ(nh, nr_cl_home_cpus(L.W, x.n));
    EXPECT_TRUE(L.home[L.anchor]) << "the housekeeping anchor stays";
    EXPECT_EQ(L.gpu, -1);
    EXPECT_EQ(L.uss, -1);
    EXPECT_EQ(L.nid, -1);
    expect_inside(L, T);
    nr_core_layout_t S;
    nr_cl_build_n(&T.t, nullptr, 1, &S);
    EXPECT_GT(L.npool * 2, S.npool * 1) << "more than a cell's share of the one-cell pool";
  }
}

TEST(CoreLayoutPool, OneCellAndSmallHostsKeepTheHistoricalRules)
{
  Topo T = dgx(true);
  nr_core_layout_t L;
  nr_cl_build_n(&T.t, nullptr, 1, &L);
  expect_dgx_layout(L, true); // N = 1: 6 tokens, 5-CPU lane, GPU worker 10, USS 12, nID 13
  // 8 general cpus (cloud / small x86): two cells would leave W - home < NR_POOL_FLOAT_MERGE_MIN_TOKENS tokens: no merged float lane, the lane rules stay
  Topo S(6);
  nr_core_layout_t M;
  nr_cl_build_n(&S.t, nullptr, 2, &M);
  EXPECT_LT(M.W - nr_cl_home_cpus(M.W, 2), NR_POOL_FLOAT_MERGE_MIN_TOKENS);
  EXPECT_LT(M.npool, NR_POOL_FLOAT_MERGE_MIN_TOKENS);
  EXPECT_GE(M.max_cells, 1);
}

TEST(CoreLayoutPool, NeverFewerTokensThanTheLaneRulesAndNeverMoreThanTheDemand)
{
  for (auto &f : fixtures())
    for (int n = 2; n <= 4; n++) {
      nr_core_layout_t L, S;
      nr_cl_build_n(&f.T.t, nullptr, n, &L);
      nr_cl_build_n(&f.T.t, nullptr, 1, &S);
      if (L.mode == NR_CL_SMALL || L.W < 1) continue;
      const int old_np = L.mode == NR_CL_SEP ? L.W - NR_CL_LANE - NR_CL_SEP_HK : L.W - (L.W >= 10 ? 4 : L.W >= 8 ? 3 : 2) - (L.anchor >= 0 ? 1 : 0);
      EXPECT_GE(L.npool, old_np < NR_CL_MAX_POOL ? old_np : NR_CL_MAX_POOL) << f.name << " n=" << n;
      const bool demand = L.npool > old_np;
      if (demand) {
        EXPECT_LE(L.npool, n * S.npool) << f.name << " n=" << n;
        EXPECT_LE(L.npool, L.W - nr_cl_home_cpus(L.W, n)) << f.name << " n=" << n;
      }
    }
}

TEST(CoreLayoutPool, SurplusAboveTheDemandJoinsTheHomeSet)
{ // [synthetic 64-cpu host, 2 NUMA nodes]: two cells ask for at most NR_CL_MAX_POOL tokens, the rest of W is home, not an idle exclusive core
  Topo T = big64();
  nr_core_layout_t L;
  nr_cl_build_n(&T.t, nullptr, 2, &L);
  EXPECT_EQ(L.npool, NR_CL_MAX_POOL);
  int nh = 0;
  for (int c = 0; c < T.t.ncpu; c++) nh += L.home[c];
  EXPECT_EQ(nh, L.W - L.npool);
  EXPECT_GE(nh, nr_cl_home_cpus(L.W, 2));
}
