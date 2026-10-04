/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
#include <string>
#include <cstring>
extern "C" {
#include "nr_passive_metrics.h"
}

TEST(PassiveMetrics, SerializesAllFieldsAsOneJsonObject) {
  nr_passive_metrics_t m = {};
  m.t_mono_ns = 123; m.abs_slot = 456; m.pci = 64; m.acq_state = "TRACKING";
  m.pdschq_decoded = 58414; m.pdschq_crc_ok = 57897; m.pdschq_stale_after_decode = 7; m.scanq_queued = 404224; m.scanq_drop_full = 161;
  char buf[4096];
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
  EXPECT_NE(s.find("\"scanq_drop_epoch\":0"), std::string::npos);
  EXPECT_NE(s.find("\"pdcch_inline_drop_epoch\":0"), std::string::npos);
  EXPECT_NE(s.find("\"pdschq_drop_epoch\":0"), std::string::npos);
  EXPECT_NE(s.find("\"puschq_drop_epoch\":0"), std::string::npos);
  EXPECT_NE(s.find("\"pci\":64"), std::string::npos);
  EXPECT_NE(s.find("\"ldpc_cuda_errors\":0,\"ldpc_cuda_fallbacks\":0,\"ldpc_cuda_poisoned\":0,\"ldpc_cuda_disabled\":0,\"ldpc_cuda_breaker_trips\":0,\"ldpc_tb_cpu\":0,\"ldpc_tb_cuda\":0"), std::string::npos);
}

TEST(PassiveMetrics, ReturnsMinusOneWhenBufferTooSmall) {
  nr_passive_metrics_t m = {};
  m.acq_state = "SEARCHING";
  char buf[16];
  EXPECT_EQ(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), -1);
}

TEST(PassiveMetrics, NullStateNameIsReportedAsUnknown) {
  nr_passive_metrics_t m = {};
  char buf[4096];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  EXPECT_NE(std::string(buf).find("\"acq_state\":\"UNKNOWN\""), std::string::npos);
}

TEST(PassiveMetrics, UnknownSlotAndPciSerializeAsMinusOne) {
  nr_passive_metrics_t m = {};
  m.abs_slot = -1; m.pci = -1; m.acq_state = "SEARCHING";
  char buf[4096];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  const std::string s(buf);
  EXPECT_NE(s.find("\"abs_slot\":-1"), std::string::npos);
  EXPECT_NE(s.find("\"pci\":-1"), std::string::npos);
}

TEST(PassiveMetrics, ExactSizeBufferBoundary) {
  nr_passive_metrics_t m = {};
  m.acq_state = "TRACKING";
  char big[4096];
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
  char buf[4096];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  const std::string s(buf);
  for (const char *k : {"\"td_excl_restarts\":3", "\"td_excl_truncs\":4", "\"td_excl_restart_alarms\":5"})
    EXPECT_NE(s.find(k), std::string::npos) << k;
}
TEST(PassiveMetrics, FieldBookKeysPresent) {
  nr_passive_metrics_t m = {};
  m.acq_state = "TRACKING";
  m.td_fb_promotions = 1; m.td_fb_withdrawals = 2; m.td_fb_failopens = 3; m.td_fb_pruned_contexts = 4; m.td_fb_untrusted_ctx = 5;
  char buf[4096];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  const std::string s(buf);
  for (const char *k : {"\"td_fb_promotions\":1", "\"td_fb_withdrawals\":2", "\"td_fb_failopens\":3", "\"td_fb_pruned_contexts\":4", "\"td_fb_untrusted_ctx\":5"})
    EXPECT_NE(s.find(k), std::string::npos) << k;
}
TEST(PassiveMetrics, Bc12aSib1CensusKeysPresentPerDciFormat) {
  nr_passive_metrics_t m = {};
  m.acq_state = "TRACKING";
  m.td_sib1_tdra_match_10 = 1; m.td_sib1_tdra_mismatch_10 = 2; m.td_sib1_tdra_none_10 = 3;
  m.td_sib1_tdra_match_11 = 4; m.td_sib1_tdra_mismatch_11 = 5; m.td_sib1_tdra_none_11 = 6;
  m.td_deftab_match_11 = 7; m.td_deftab_mismatch_11 = 8;
  char buf[4096];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  const std::string s(buf);
  for (const char *k : {"\"td_sib1_tdra_match_10\":1", "\"td_sib1_tdra_mismatch_10\":2", "\"td_sib1_tdra_none_10\":3",
                        "\"td_sib1_tdra_match_11\":4", "\"td_sib1_tdra_mismatch_11\":5", "\"td_sib1_tdra_none_11\":6",
                        "\"td_deftab_match_11\":7", "\"td_deftab_mismatch_11\":8", "\"td_sib1_tdra_match_unk\":0"})
    EXPECT_NE(s.find(k), std::string::npos) << k;
}

TEST(PassiveMetrics, Cb0EliminationKeysPresent) {
  nr_passive_metrics_t m = {};
  m.acq_state = "TRACKING";
  m.td_cb0_grants = 11; m.td_cb0_items = 22; m.td_cb0_budget_skips = 3; m.td_cb0_us_per_item = 361.25;
  m.td_cb0_backend_cpu = 7; m.td_cb0_backend_gpu = 0; m.td_cb0_premise_alarms = 0; m.td_cb0_eliminations = 9;
  m.td_cb0_inadmissible[0] = 4; m.td_cb0_inadmissible[13] = 3; m.td_cb0_inadmissible[15] = 1;
  char buf[4096];
  const int n = nr_passive_metrics_to_json(&m, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  const std::string s(buf, n);
  EXPECT_EQ(s.back(), '}');
  for (const char *k : {"\"td_cb0_grants\":11", "\"td_cb0_items\":22", "\"td_cb0_budget_skips\":3", "\"td_cb0_us_per_item\":361.2",
                        "\"td_cb0_backend\":{\"cpu\":7,\"gpu\":0}", "\"td_cb0_premise_alarms\":0", "\"td_cb0_eliminations\":9",
                        "\"td_cb0_inadmissible\":{\"not_new_rv0\":4,", "\"budget\":3", "\"reindexed\":1}"})
    EXPECT_NE(s.find(k), std::string::npos) << k;
}

TEST(PassiveMetrics, MergedSchemaFitsWithFullResourceListAndLargeCounters) {
  nr_passive_metrics_t m;
  // Maximum integer widths exercise both parents' keys and all CSI-RS records together.
  memset(&m, 0xff, sizeof(m));
  m.acq_state = "TRACKING";
  m.td_cb0_us_per_item = 1000000.0;
  m.csirs_confirm_resource_count = NR_PASSIVE_CSIRS_RESOURCE_METRICS_MAX;
  char buf[8192];
  const int n = nr_passive_metrics_to_json(&m, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  const std::string s(buf, n);
  EXPECT_EQ(s.back(), '}');
  for (const char *key : {"pdschq_stale_after_decode", "pdschq_drop_epoch", "td_cb0_gpu", "td_fb_promotions",
                           "csirs_time_to_confirm_resources"})
    EXPECT_NE(s.find(std::string("\"") + key + "\":"), std::string::npos) << key;
}
