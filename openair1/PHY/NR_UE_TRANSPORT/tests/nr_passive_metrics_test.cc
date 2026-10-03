/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
#include <string>
extern "C" {
#include "nr_passive_metrics.h"
}

TEST(PassiveMetrics, SerializesAllFieldsAsOneJsonObject) {
  nr_passive_metrics_t m = {};
  m.t_mono_ns = 123; m.abs_slot = 456; m.pci = 64; m.acq_state = "TRACKING";
  m.pdschq_decoded = 58414; m.pdschq_crc_ok = 57897; m.pdschq_stale_after_decode = 7; m.scanq_queued = 404224; m.scanq_drop_full = 161;
  char buf[2048];
  const int n = nr_passive_metrics_to_json(&m, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  const std::string s(buf, n);
  EXPECT_EQ(s.front(), '{'); EXPECT_EQ(s.back(), '}');
  EXPECT_EQ(s.find('\n'), std::string::npos);
  EXPECT_NE(s.find("\"schema\":1"), std::string::npos);
  EXPECT_NE(s.find("\"acq_state\":\"TRACKING\""), std::string::npos);
  EXPECT_NE(s.find("\"pdschq_crc_ok\":57897"), std::string::npos);
  EXPECT_NE(s.find("\"pdschq_stale_after_decode\":7"), std::string::npos);
  EXPECT_NE(s.find("\"scanq_drop_full\":161"), std::string::npos);
  EXPECT_NE(s.find("\"pci\":64"), std::string::npos);
  EXPECT_NE(s.find("\"ldpc_cuda_errors\":0,\"ldpc_cuda_fallbacks\":0,\"ldpc_cuda_poisoned\":0,\"ldpc_cuda_disabled\":0,\"ldpc_tb_cpu\":0,\"ldpc_tb_cuda\":0"), std::string::npos);
}

TEST(PassiveMetrics, ReturnsMinusOneWhenBufferTooSmall) {
  nr_passive_metrics_t m = {};
  m.acq_state = "SEARCHING";
  char buf[16];
  EXPECT_EQ(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), -1);
}

TEST(PassiveMetrics, NullStateNameIsReportedAsUnknown) {
  nr_passive_metrics_t m = {};
  char buf[2048];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  EXPECT_NE(std::string(buf).find("\"acq_state\":\"UNKNOWN\""), std::string::npos);
}

TEST(PassiveMetrics, UnknownSlotAndPciSerializeAsMinusOne) {
  nr_passive_metrics_t m = {};
  m.abs_slot = -1; m.pci = -1; m.acq_state = "SEARCHING";
  char buf[2048];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  const std::string s(buf);
  EXPECT_NE(s.find("\"abs_slot\":-1"), std::string::npos);
  EXPECT_NE(s.find("\"pci\":-1"), std::string::npos);
}

TEST(PassiveMetrics, ExactSizeBufferBoundary) {
  nr_passive_metrics_t m = {};
  m.acq_state = "TRACKING";
  char big[2048];
  const int w = nr_passive_metrics_to_json(&m, big, sizeof(big));
  ASSERT_GT(w, 0);
  std::string a(w + 1, 'x'), b(w, 'x');
  EXPECT_EQ(nr_passive_metrics_to_json(&m, &a[0], w + 1), w);
  EXPECT_EQ(nr_passive_metrics_to_json(&m, &b[0], w), -1);
}

TEST(PassiveMetrics, TdExclRestartKeysPresent) {
  nr_passive_metrics_t m = {};
  m.acq_state = "TRACKING";
  m.td_excl_restarts = 3; m.td_excl_truncs = 4; m.td_excl_restart_alarms = 5;
  char buf[2048];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  const std::string s(buf);
  for (const char *k : {"\"td_excl_restarts\":3", "\"td_excl_truncs\":4", "\"td_excl_restart_alarms\":5"})
    EXPECT_NE(s.find(k), std::string::npos) << k;
}
TEST(PassiveMetrics, Bc12aSib1CensusKeysPresentPerDciFormat) {
  nr_passive_metrics_t m = {};
  m.acq_state = "TRACKING";
  m.td_sib1_tdra_match_10 = 1; m.td_sib1_tdra_mismatch_10 = 2; m.td_sib1_tdra_none_10 = 3;
  m.td_sib1_tdra_match_11 = 4; m.td_sib1_tdra_mismatch_11 = 5; m.td_sib1_tdra_none_11 = 6;
  m.td_deftab_match_11 = 7; m.td_deftab_mismatch_11 = 8;
  char buf[2048];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  const std::string s(buf);
  for (const char *k : {"\"td_sib1_tdra_match_10\":1", "\"td_sib1_tdra_mismatch_10\":2", "\"td_sib1_tdra_none_10\":3",
                        "\"td_sib1_tdra_match_11\":4", "\"td_sib1_tdra_mismatch_11\":5", "\"td_sib1_tdra_none_11\":6",
                        "\"td_deftab_match_11\":7", "\"td_deftab_mismatch_11\":8", "\"td_sib1_tdra_match_unk\":0"})
    EXPECT_NE(s.find(k), std::string::npos) << k;
}
