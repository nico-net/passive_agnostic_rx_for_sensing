/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
extern "C" {
#include "nr_csirs_disc_rate_internal.h"
}

static nr_csirs_rl_cfg_t cfg() { return {640, 7, 120000, 0.01, 0.5, 2}; }  // 60 s at 0.5 ms slots

TEST(CsirsRate, ChecksFromMissBound) { auto c = cfg(); EXPECT_EQ(nr_csirs_rl_checks(&c), 7u); }

TEST(CsirsRate, StableOnlyAfterConfirmedAndQuiet) {
  auto c = cfg(); nr_csirs_rl_t s = {};
  nr_csirs_rl_obs_t o = {};
  o.slot = 1000; EXPECT_FALSE(nr_csirs_rl_step(&s, &c, &o)); EXPECT_FALSE(s.stable);          // nothing confirmed
  o.n_conf = 2; o.max_period = 160; o.slot = 2000; EXPECT_FALSE(nr_csirs_rl_step(&s, &c, &o)); // just changed
  o.slot = 2000 + 2 * 7 * 640 - 1; EXPECT_FALSE(nr_csirs_rl_step(&s, &c, &o));
  o.slot = 2000 + 2 * 7 * 640; EXPECT_TRUE(nr_csirs_rl_step(&s, &c, &o)); EXPECT_TRUE(s.stable);
  EXPECT_EQ(s.G, 1u);
  EXPECT_EQ(s.div_s, (120000u / 7 + 639) / 640);  // cycle from notice / checks
  EXPECT_GE(s.div_s, s.G * c.div);                // never above the SEARCH duty
}

TEST(CsirsRate, SlowResourceGetsMoreWindowsAndNeverExceedsBaselineDuty) {
  auto c = cfg(); c.notice_slots = 10000; nr_csirs_rl_t s = {};
  nr_csirs_rl_obs_t o = {}; o.n_conf = 1; o.max_period = 640; o.slot = 0; nr_csirs_rl_step(&s, &c, &o);
  o.slot = 100000; ASSERT_TRUE(nr_csirs_rl_step(&s, &c, &o));
  EXPECT_EQ(s.G, 4u); EXPECT_EQ(s.div_s, 28u);  // 4 windows of 7 -> duty G/div_s == 1/div
}

static nr_csirs_rl_t stable_state(const nr_csirs_rl_cfg_t &c, nr_csirs_rl_obs_t &o) {
  nr_csirs_rl_t s = {}; o = {}; o.n_conf = 1; o.max_period = 160; o.crc_ok = 0; o.crc_dec = 0; o.slot = 0;
  nr_csirs_rl_step(&s, &c, &o);
  o.slot = 100000; o.crc_ok = 500; o.crc_dec = 1000;  // baseline CRC rate 0.5
  nr_csirs_rl_step(&s, &c, &o);
  return s;
}

TEST(CsirsRate, EscalatesOnEvidenceOfChange) {
  auto c = cfg(); nr_csirs_rl_obs_t o;
  { auto s = stable_state(c, o); ASSERT_TRUE(s.stable); o.slot += 10; o.lost = 1; ASSERT_TRUE(nr_csirs_rl_step(&s, &c, &o)); EXPECT_FALSE(s.stable); EXPECT_TRUE(s.rearm); }
  { auto s = stable_state(c, o); o.slot += 10; o.new_pattern = true; ASSERT_TRUE(nr_csirs_rl_step(&s, &c, &o)); EXPECT_FALSE(s.stable); }
  { auto s = stable_state(c, o); o.slot += 10; o.n_conf = 2; ASSERT_TRUE(nr_csirs_rl_step(&s, &c, &o)); EXPECT_FALSE(s.stable); }
  { auto s = stable_state(c, o); o.slot += 10; o.epoch = true; ASSERT_TRUE(nr_csirs_rl_step(&s, &c, &o)); EXPECT_FALSE(s.stable); }
  { auto s = stable_state(c, o); o.slot += 10; o.crc_dec += 400; o.crc_ok += 100;  // 0.25 vs 0.5 over 400
    ASSERT_TRUE(nr_csirs_rl_step(&s, &c, &o)); EXPECT_FALSE(s.stable); EXPECT_STREQ(s.reason, "crc-drop"); }
  { auto s = stable_state(c, o); o.slot += 10; o.crc_dec += 400; o.crc_ok += 195;  // 0.4875: noise
    EXPECT_FALSE(nr_csirs_rl_step(&s, &c, &o)); EXPECT_TRUE(s.stable); }
}
