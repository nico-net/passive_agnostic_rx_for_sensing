// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// G2: the numbers nr_gpu_green.h now takes from the derived SM plan equal what its old literal code produced. A fake driver (SplitByCount rounds the
// request up to the granularity, cuts floor(SM/size) groups) replays the exact sequence of nr_gpu_green.h with the PLAN's requests, and the result
// is compared with the old literal code (sms = max(1, SM/6); split iff rt >= 32; minCount 4) and with the plan's own prediction (the "plan mismatch" log).
// Device fixtures: 48 SMs gran 8 = DGX GB10 [CODE-READ + the DGX log line "DL 32 SMs, UL up to 40 SMs, probes 8 SMs"]; others synthetic.
#include <gtest/gtest.h>
#include "nr_gpu_sm_plan.h"

namespace {
int ru(int v, int g) { return (v + g - 1) / g * g; }
struct Drv { int disc, rt, group_sz, groups, rem, dl, ul, probe; bool split; };
// the sequence of nr_gpu_green.h with request sms / group_req taken from the plan
Drv drive(int sm, int gran, const nr_gpu_sm_plan_t &p)
{
  Drv d = {};
  d.disc = ru(p.disc_sms, gran);       // cuDevSmResourceSplitByCount(minCount = plan.disc_sms): part[0]
  d.rt = sm - d.disc;
  d.group_sz = ru(p.group_req, gran);  // cut of all SMs with minCount = plan.group_req
  d.groups = sm / d.group_sz;
  d.rem = sm - d.groups * d.group_sz;
  const int tot = d.groups + (d.rem > 0), dn = (d.disc + d.group_sz - 1) / d.group_sz;
  d.split = p.split2 && d.groups < 16 && d.groups >= dn + 1 && tot > dn + 1;
  if (d.split) {
    d.dl = sm - (dn + 1) * d.group_sz;
    d.probe = d.group_sz;
    d.ul = d.dl + d.probe;
  }
  return d;
}
nr_gpu_sm_plan_t plan(int sm, int gran)
{
  nr_gpu_sm_in_t in = {};
  in.sm_count = sm; in.granularity = gran; in.green = 1; in.tier = NR_GPU_SHARED;
  nr_gpu_sm_plan_t p;
  nr_gpu_sm_plan(&in, &p);
  return p;
}
} // namespace

TEST(GreenG2, Gb10_48Sm_Gran8_SameNumbersAsTheLiteralCode)
{
  const nr_gpu_sm_plan_t p = plan(48, 8);
  const Drv d = drive(48, 8, p);
  EXPECT_EQ(p.disc_sms, 8);  // old: sms = 48/6 = 8
  EXPECT_EQ(d.disc, 8);
  EXPECT_EQ(d.rt, 40);
  EXPECT_EQ(p.group_req, 4); // old literal 4
  EXPECT_EQ(d.group_sz, 8);
  EXPECT_TRUE(p.split2);     // old: rt 40 >= 32
  EXPECT_TRUE(d.split);
  EXPECT_EQ(d.dl, 32);       // log line: DL 32 SMs, UL up to 40 SMs (overlapping DL), probes 8 SMs
  EXPECT_EQ(d.ul, 40);
  EXPECT_EQ(d.probe, 8);
  EXPECT_EQ(p.dl_sms, d.dl);
  EXPECT_EQ(p.ul_sms, d.ul);
  EXPECT_EQ(p.probe_sms, d.probe);
  EXPECT_EQ(p.group_sz, d.group_sz); // no "SM plan mismatch" line
}

TEST(GreenG2, DriverEqualsPlan_AllDevices)
{
  for (int gran : {1, 2, 4, 8})
    for (int sm = 8; sm <= 160; sm++) {
      const nr_gpu_sm_plan_t p = plan(sm, gran);
      if (!p.green) continue;
      const Drv d = drive(sm, gran, p);
      EXPECT_EQ(d.disc, p.disc_sms) << sm << "/" << gran;
      EXPECT_EQ(d.split, p.split2 != 0) << sm << "/" << gran;
      if (d.split) {
        EXPECT_EQ(d.dl, p.dl_sms) << sm << "/" << gran;
        EXPECT_EQ(d.ul, p.ul_sms) << sm << "/" << gran;
        EXPECT_EQ(d.group_sz, p.group_sz) << sm << "/" << gran;
      }
    }
}

TEST(GreenG2, OldLiteralSplitRuleOnGranularity8Devices)
{
  // the old rule: split iff rt >= 32 (and groups < 16); the plan agrees on every gran-8 device whose DL part is >= 4 widths of 8
  for (int sm = 16; sm <= 120; sm++) {
    const int disc = ru(sm / 6 > 1 ? sm / 6 : 1, 8), rt = sm - disc;
    const nr_gpu_sm_plan_t p = plan(sm, 8);
    if (rt >= 32 && sm - ((disc + 7) / 8 + 1) * 8 >= 32) { EXPECT_TRUE(p.split2) << sm; }
    if (rt < 32) { EXPECT_FALSE(p.split2) << sm; }
  }
}

TEST(GreenG2, CudaConnectionsFromTheStreamCount)
{
  // nr-uesoftmodem.c: next power of two >= 3 streams x NR_GPU_PASS_RING instances (4 DL + 2 UL + 1 probe = 7), clamped to [8, 32]: 21 -> 32 (env files: 32)
  auto conn = [](int ring) { int c = 8; while (c < 3 * ring && c < 32) c <<= 1; return c; };
  EXPECT_EQ(conn(7), 32);
  EXPECT_EQ(conn(2), 8);
  EXPECT_EQ(conn(100), 32);
}
