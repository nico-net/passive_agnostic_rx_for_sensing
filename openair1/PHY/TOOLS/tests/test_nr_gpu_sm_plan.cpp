// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Pure SM plan (nr_gpu_sm_plan.h) on fake devices. Device values are fixtures, labelled:
//  48 SMs gran 8 = DGX GB10 (the layout nr_gpu_green.h builds today: 8 / 40, DL 32 / UL 40 / probe 8) [CODE-READ + log line of the DGX runs];
//  34 SMs = RTX 4060 Ti [INFERRED, public spec; granularity ASSUMED 2 and also swept]; 132 SMs = H100 SXM [INFERRED, public spec]; 24/16/8 synthetic.
#include <gtest/gtest.h>
#include "nr_gpu_sm_plan.h"

namespace {
nr_gpu_sm_plan_t plan(int sm, int gran, bool green = true, int tier = NR_GPU_DISCRETE)
{
  nr_gpu_sm_in_t in = {};
  in.sm_count = sm;
  in.granularity = gran;
  in.green = green;
  in.tier = tier;
  nr_gpu_sm_plan_t p;
  nr_gpu_sm_plan(&in, &p);
  return p;
}
// The literal rule of nr_gpu_green.h as it stands (CODE-READ), as an oracle for the GB10: disc = round_up(max(1, SM/6), gran), split when
// rt >= 32 and the all-SM cut (minCount 4) has g < 16 and g >= disc_n + 1.
struct Old { int disc, rt; bool split; int dl, ul, probe; };
Old old_rule(int sm, int gran)
{
  Old o = {};
  const int req = sm / 6 > 1 ? sm / 6 : 1;
  o.disc = (req + gran - 1) / gran * gran;
  o.rt = sm - o.disc;
  if (o.rt < 32) return o;
  const int sz = (4 + gran - 1) / gran * gran, g = sm / sz, rem = sm - g * sz, tot = g + (rem > 0), dn = (o.disc + sz - 1) / sz;
  if (g >= 16 || g < dn + 1) return o;
  o.split = true;
  o.probe = sz;
  o.dl = sm - (dn + 1) * sz;
  o.ul = sm - dn * sz;
  (void)tot;
  return o;
}
} // namespace

TEST(GpuSmPlan, Gb10EqualsToday)
{
  const nr_gpu_sm_plan_t p = plan(48, 8, true, NR_GPU_SHARED);
  EXPECT_TRUE(p.green);
  EXPECT_STREQ(p.reason, "ok");
  EXPECT_EQ(p.disc_sms, 8);
  EXPECT_EQ(p.rt_sms, 40);
  EXPECT_TRUE(p.split2);
  EXPECT_EQ(p.dl_sms, 32);
  EXPECT_EQ(p.ul_sms, 40);
  EXPECT_EQ(p.probe_sms, 8);
  EXPECT_EQ(p.group_req, 4); /* the call the code makes today */
  EXPECT_EQ(p.group_sz, 8);
  EXPECT_EQ(p.full_groups, 6);
  EXPECT_EQ(p.disc_groups, 1);
  EXPECT_EQ(p.dl_groups, 4);
  EXPECT_EQ(p.ul_groups, 5);
  EXPECT_EQ(p.idle_sms, 0);
  EXPECT_EQ(p.dl_pct, 80);
  const Old o = old_rule(48, 8);
  EXPECT_TRUE(o.split);
  EXPECT_EQ(o.disc, p.disc_sms);
  EXPECT_EQ(o.dl, p.dl_sms);
  EXPECT_EQ(o.ul, p.ul_sms);
  EXPECT_EQ(o.probe, p.probe_sms);
  // the tier does not change the partition (SHARED and DISCRETE are the same plan)
  const nr_gpu_sm_plan_t q = plan(48, 8, true, NR_GPU_DISCRETE);
  EXPECT_EQ(0, memcmp(&p, &q, sizeof(p)));
}

TEST(GpuSmPlan, Rtx4060TiOneRealTimeContext)
{
  for (int gran : {1, 2, 4, 8}) {
    const nr_gpu_sm_plan_t p = plan(34, gran);
    EXPECT_TRUE(p.green) << gran;
    EXPECT_EQ(p.disc_sms, gran == 1 ? 5 : gran == 2 ? 6 : 8) << gran; /* 34 / 6 = 5, rounded up to the granularity */
    EXPECT_EQ(p.rt_sms, 34 - p.disc_sms) << gran;
    EXPECT_GE(p.rt_sms, 26) << gran; /* RT 26-29 */
    EXPECT_LE(p.rt_sms, 29) << gran;
    EXPECT_FALSE(p.split2) << gran; /* one real-time context, stream priority only */
    EXPECT_EQ(p.dl_sms + p.ul_sms + p.probe_sms, 0) << gran;
  }
  const nr_gpu_sm_plan_t p = plan(34, 2);
  EXPECT_EQ(p.disc_sms, 6);
  EXPECT_EQ(p.rt_sms, 28);
  EXPECT_EQ(old_rule(34, 2).split, false); /* today: rt 28 < 32, also one context */
}

TEST(GpuSmPlan, SmallDevicesAreNotSplit)
{
  for (int sm : {24, 16, 12}) {
    for (int gran : {1, 2, 4}) {
      const nr_gpu_sm_plan_t p = plan(sm, gran);
      EXPECT_TRUE(p.green) << sm << "/" << gran;
      EXPECT_FALSE(p.split2) << sm << "/" << gran;
      EXPECT_EQ(p.disc_sms + p.rt_sms, sm);
    }
  }
  EXPECT_EQ(plan(24, 2).disc_sms, 4);
  EXPECT_EQ(plan(24, 2).rt_sms, 20);
  EXPECT_EQ(plan(16, 2).disc_sms, 2);
  EXPECT_EQ(plan(16, 4).disc_sms, 4);
}

TEST(GpuSmPlan, NoRoomForRealTimeSmsMeansNoGreen)
{
  const nr_gpu_sm_plan_t p = plan(8, 8); /* disc rounds up to all 8 SMs: nothing left for real time */
  EXPECT_FALSE(p.green);
  EXPECT_STREQ(p.reason, "no_rt_sms");
  EXPECT_EQ(p.rt_sms, 0);
  const nr_gpu_sm_plan_t q = plan(8, 2);
  EXPECT_TRUE(q.green);
  EXPECT_EQ(q.disc_sms, 2);
  EXPECT_EQ(q.rt_sms, 6);
  EXPECT_FALSE(q.split2);
  EXPECT_FALSE(plan(1, 1).green); /* disc 1, rt 0 */
}

TEST(GpuSmPlan, NoGreenWhenUnavailable)
{
  for (int sm : {48, 34, 8}) {
    nr_gpu_sm_plan_t p = plan(sm, 8, false, NR_GPU_DISCRETE); /* CUDA < 12.5: the stub */
    EXPECT_FALSE(p.green);
    EXPECT_STREQ(p.reason, "no_green");
    EXPECT_EQ(p.disc_sms, 0);
    EXPECT_EQ(p.rt_sms, 0);
    EXPECT_FALSE(p.split2);
    p = plan(sm, 8, true, NR_GPU_NONE); /* no GPU at all */
    EXPECT_FALSE(p.green);
    EXPECT_STREQ(p.reason, "no_gpu");
  }
  EXPECT_STREQ(plan(0, 8).reason, "no_sm");
  EXPECT_STREQ(plan(-3, 8).reason, "no_sm");
}

TEST(GpuSmPlan, LargeDeviceSplitsAndKeepsTheRatios)
{
  for (int gran : {1, 2, 4, 8}) {
    const nr_gpu_sm_plan_t p = plan(132, gran);
    EXPECT_TRUE(p.green) << gran;
    EXPECT_EQ(p.disc_sms, (22 + gran - 1) / gran * gran) << gran;
    EXPECT_TRUE(p.split2) << gran;
    EXPECT_LE(p.full_groups, 15) << gran;
    EXPECT_GE(p.dl_pct, 60) << gran;
    EXPECT_EQ(p.ul_sms, p.dl_sms + p.probe_sms) << gran;
    EXPECT_GE(p.group_sz, p.group_req) << gran;
  }
  const nr_gpu_sm_plan_t p = plan(132, 8);
  EXPECT_EQ(p.group_sz, 16); /* 132 / 8 = 16 full groups would exceed the array bound of nr_gpu_green.h (g < 16) */
  EXPECT_EQ(p.probe_sms, 16);
  EXPECT_FALSE(old_rule(132, 8).split) << "today's code rejects g == 16 and keeps one context";
}

TEST(GpuSmPlan, GranularitySweepOnTheGb10Count)
{
  struct { int gran; int disc; bool split; int dl, ul, probe; } e[] = {
      {1, 8, true, 36, 40, 4},  /* groups of 4: discovery 2 groups */
      {2, 8, true, 36, 40, 4},
      {4, 8, true, 36, 40, 4},
      {8, 8, true, 32, 40, 8},
  };
  for (auto &k : e) {
    const nr_gpu_sm_plan_t p = plan(48, k.gran);
    EXPECT_EQ(p.disc_sms, k.disc) << k.gran;
    EXPECT_EQ(p.split2, k.split) << k.gran;
    EXPECT_EQ(p.dl_sms, k.dl) << k.gran;
    EXPECT_EQ(p.ul_sms, k.ul) << k.gran;
    EXPECT_EQ(p.probe_sms, k.probe) << k.gran;
  }
  // a wide granularity raises the DL width: 4 instances x 16 SMs = 64 > 48 -> no split
  EXPECT_FALSE(plan(48, 16).split2);
  EXPECT_FALSE(plan(48, 12).split2);
}

TEST(GpuSmPlan, SplitInstancesAreAnInput)
{
  nr_gpu_sm_in_t in = {};
  in.sm_count = 48, in.granularity = 8, in.green = 1, in.tier = NR_GPU_SHARED;
  nr_gpu_sm_plan_t p;
  in.dl_instances = 6; /* 6 x 8 = 48 > 32 */
  nr_gpu_sm_plan(&in, &p);
  EXPECT_FALSE(p.split2);
  in.dl_instances = 2;
  nr_gpu_sm_plan(&in, &p);
  EXPECT_TRUE(p.split2);
  in.dl_instances = 0; /* default 4 */
  nr_gpu_sm_plan(&in, &p);
  EXPECT_TRUE(p.split2);
}

TEST(GpuSmPlan, InvariantsOverAllDevices)
{
  for (int sm = 1; sm <= 200; sm++)
    for (int gran : {0, 1, 2, 3, 4, 8, 16}) {
      const nr_gpu_sm_plan_t p = plan(sm, gran);
      const int g = gran > 1 ? gran : 1;
      if (!p.green) {
        EXPECT_FALSE(p.split2);
        EXPECT_EQ(p.rt_sms, 0);
        EXPECT_STREQ(p.reason, "no_rt_sms");
        continue;
      }
      ASSERT_EQ(p.disc_sms + p.rt_sms, sm) << sm << "/" << gran;
      EXPECT_EQ(p.disc_sms % g, 0) << sm << "/" << gran;
      EXPECT_GE(p.disc_sms, sm / 6 > 1 ? sm / 6 : 1);
      EXPECT_GT(p.rt_sms, 0);
      EXPECT_LE(p.full_groups, 15) << sm << "/" << gran;
      EXPECT_EQ(p.group_sz % g, 0);
      EXPECT_GE(p.group_sz, 4);
      EXPECT_EQ(p.full_groups * p.group_sz + p.tail_sms, sm);
      if (!p.split2) { EXPECT_EQ(p.dl_sms + p.ul_sms + p.probe_sms + p.dl_groups + p.ul_groups, 0); continue; }
      EXPECT_EQ(p.dl_sms + p.probe_sms, p.ul_sms) << sm << "/" << gran;
      EXPECT_EQ(p.disc_groups * p.group_sz + p.ul_sms, sm);
      EXPECT_EQ(p.idle_sms, p.disc_groups * p.group_sz - p.disc_sms);
      EXPECT_GE(p.idle_sms, 0);
      EXPECT_GE(p.dl_pct, 60) << sm << "/" << gran;
      EXPECT_GE(p.dl_sms, 4 * ((8 + g - 1) / g * g)) << sm << "/" << gran;
      EXPECT_LE(p.ul_sms, p.rt_sms + p.idle_sms);
      EXPECT_EQ(p.probe_sms, p.group_sz);
      // the new rule is never looser than the literal 32 where the old code could split at all
      const Old o = old_rule(sm, g);
      if (o.split) { EXPECT_GE(o.rt, 32); }
      EXPECT_GE(p.rt_sms, 32) << sm << "/" << gran; /* DL >= 32 implies rt >= 32 */
    }
}
