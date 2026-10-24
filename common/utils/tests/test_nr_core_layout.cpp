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
