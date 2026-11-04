/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
#include <string>
#include <cstring>
#include <vector>
extern "C" {
#include "nr_passive_metrics.h"
}

TEST(PassiveMetrics, SerializesAllFieldsAsOneJsonObject) {
  nr_passive_metrics_t m = {};
  m.t_mono_ns = 123; m.abs_slot = 456; m.pci = 64; m.acq_state = "TRACKING";
  m.pdschq_decoded = 58414; m.pdschq_crc_ok = 57897; m.pdschq_stale_after_decode = 7; m.scanq_queued = 404224; m.scanq_drop_full = 161;
  m.prach_us = 73; m.prach_drops = 2; m.prach_skipped = 1;
  m.sense_cfr_drop = 9; m.sense_rec_drop = 2; m.sense_io_errors = 1; m.sense_recon_skip = 6;
  m.gpu_fep_selfcheck_skipped = 4;
  m.sense_prod_us[NR_SENSE_SRC_PDSCH_DATA]=123;
  m.sense_pack_us[NR_SENSE_SRC_PDSCH_DATA]=45;
  m.sense_prod_count[NR_SENSE_SRC_PDSCH_DATA]=6;
  m.sense_rec_us=78; m.sense_write_us=90;
  m.sense_cfr_drop_src[NR_SENSE_SRC_PUSCH_DATA]=3; m.sense_cfr_exported_src[NR_SENSE_SRC_PDSCH_DATA]=57000;
  m.sense_unexported_pdsch_data=897; m.sense_unexported_pusch_data=5;
  char buf[8192];
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
  EXPECT_NE(s.find("\"prach_us\":73,\"prach_drops\":2,\"prach_skipped\":1"), std::string::npos);
  EXPECT_NE(s.find("\"sense_cfr_drop\":9,\"sense_rec_drop\":2,\"sense_io_errors\":1"), std::string::npos);
  EXPECT_NE(s.find("\"sense_recon_skip\":6,"), std::string::npos);
  EXPECT_NE(s.find("\"gpu_fep_selfcheck_skipped\":4"), std::string::npos);
  EXPECT_NE(s.find("\"sense_prod_us_pdsch_data\":123,\"sense_pack_us_pdsch_data\":45"), std::string::npos);
  EXPECT_NE(s.find("\"sense_prod_count_pdsch_data\":6"), std::string::npos);
  EXPECT_NE(s.find("\"sense_rec_us\":78,\"sense_write_us\":90"), std::string::npos);
  EXPECT_NE(s.find("\"sense_cfr_drop_pusch_data\":3"), std::string::npos);
  EXPECT_NE(s.find("\"sense_cfr_exported_pdsch_data\":57000"), std::string::npos);
  EXPECT_NE(s.find("\"sense_cfr_drop_srs\":0"), std::string::npos);
  EXPECT_NE(s.find("\"sense_unexported_pdsch_data\":897,\"sense_unexported_pusch_data\":5"), std::string::npos);
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
  char buf[8192];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  EXPECT_NE(std::string(buf).find("\"acq_state\":\"UNKNOWN\""), std::string::npos);
}

TEST(PassiveMetrics, UnknownSlotAndPciSerializeAsMinusOne) {
  nr_passive_metrics_t m = {};
  m.abs_slot = -1; m.pci = -1; m.acq_state = "SEARCHING";
  char buf[8192];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  const std::string s(buf);
  EXPECT_NE(s.find("\"abs_slot\":-1"), std::string::npos);
  EXPECT_NE(s.find("\"pci\":-1"), std::string::npos);
}

TEST(PassiveMetrics, ExactSizeBufferBoundary) {
  nr_passive_metrics_t m = {};
  m.acq_state = "TRACKING";
  char big[8192];
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
  char buf[8192];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  const std::string s(buf);
  for (const char *k : {"\"td_excl_restarts\":3", "\"td_excl_truncs\":4", "\"td_excl_restart_alarms\":5"})
    EXPECT_NE(s.find(k), std::string::npos) << k;
}
TEST(PassiveMetrics, FieldBookKeysPresent) {
  nr_passive_metrics_t m = {};
  m.acq_state = "TRACKING";
  m.td_fb_promotions = 1; m.td_fb_withdrawals = 2; m.td_fb_failopens = 3; m.td_fb_pruned_contexts = 4; m.td_fb_untrusted_ctx = 5;
  char buf[8192];
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
  char buf[8192];
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
  char buf[8192];
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
  char buf[20480];
  const int n = nr_passive_metrics_to_json(&m, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  const std::string s(buf, n);
  EXPECT_EQ(s.back(), '}');
  for (const char *key : {"pdschq_stale_after_decode", "pdschq_drop_epoch", "td_cb0_gpu", "td_fb_promotions",
                           "csirs_time_to_confirm_resources"})
    EXPECT_NE(s.find(std::string("\"") + key + "\":"), std::string::npos) << key;
}
TEST(PassiveMetrics,SrsCountersPresent) {
  nr_passive_metrics_t m={};char buf[8192];
  ASSERT_GT(nr_passive_metrics_to_json(&m,buf,sizeof(buf)),0);
  EXPECT_NE(std::string(buf).find("\"srs_us\":0"),std::string::npos);
  EXPECT_NE(std::string(buf).find("\"srs_jobs_dropped\":0"),std::string::npos);
  EXPECT_NE(std::string(buf).find("\"srs_pregate_skip\":0"),std::string::npos);
  EXPECT_NE(std::string(buf).find("\"srs_backoff_skip\":0"),std::string::npos);
  EXPECT_NE(std::string(buf).find("\"srs_budget_skip\":0"),std::string::npos);
}
TEST(PassiveMetrics,PeriodicCountersPerType) {
  nr_passive_metrics_t m={};char buf[16384];
  m.periodic[NR_PTRACK_SRS]={5,3,2,1,1,4};m.srs_audits=9;
  ASSERT_GT(nr_passive_metrics_to_json(&m,buf,sizeof(buf)),0);
  std::string j(buf);
  for(const char *t:{"srs","csirs","pucch","cg"}) for(const char *k:{"predicted","hit","miss","demoted"})
    EXPECT_NE(j.find(std::string("\"periodic_")+t+"_"+k+"\":"),std::string::npos) << t << k;
  EXPECT_NE(j.find("\"periodic_srs_predicted\":5,\"periodic_srs_hit\":3,\"periodic_srs_miss\":2,\"periodic_srs_demoted\":1"),std::string::npos);
  EXPECT_NE(j.find("\"srs_audits\":9"),std::string::npos);
  EXPECT_EQ(j.back(),'}');
}

TEST(PassiveMetrics, PdschStageHistogramsAndReasons) {
  nr_passive_metrics_t m = {};
  char buf[16384];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  EXPECT_EQ(std::string(buf).find("pdschq_latency"), std::string::npos);
  m.pdsch_schedule_enabled = 1;
  m.pdschq_drop_narrow = 9;
  m.pdsch_schedule.hist[NR_PDSCH_LAT_FEP][8] = 17;
  m.pdsch_schedule.reason[NR_PDSCH_DROP_COPY] = 5;
  const int n = nr_passive_metrics_to_json(&m, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  const std::string s(buf);
  EXPECT_NE(s.find("\"enqueue_fep\":[0,0,0,0,0,0,0,0,17]"), std::string::npos);
  EXPECT_NE(s.find("\"copy_expired\":5"), std::string::npos);
  EXPECT_NE(s.find("\"pdschq_drop_narrow\":9"), std::string::npos);
  EXPECT_EQ(s.back(), '}');
  std::vector<char> exact(n + 1), shortbuf(n);
  EXPECT_EQ(nr_passive_metrics_to_json(&m, exact.data(), exact.size()), n);
  EXPECT_EQ(nr_passive_metrics_to_json(&m, shortbuf.data(), shortbuf.size()), -1);
}

TEST(PassiveMetrics, NidCapturePolicyCounters) {
  nr_passive_metrics_t m={};char buf[16384];
  ASSERT_GT(nr_passive_metrics_to_json(&m,buf,sizeof(buf)),0);
  EXPECT_EQ(std::string(buf).find("nid_capture"),std::string::npos);
  m.nid_capture_offered=100;m.nid_capture_taken=10;m.nid_capture_skipped_empty=89;
  m.nid_capture_noise_floor_db=42.125;
  ASSERT_GT(nr_passive_metrics_to_json(&m,buf,sizeof(buf)),0);
  std::string s(buf);
  EXPECT_NE(s.find("\"nid_capture_offered\":100"),std::string::npos);
  EXPECT_NE(s.find("\"nid_capture_taken\":10"),std::string::npos);
  EXPECT_NE(s.find("\"nid_capture_skipped_empty\":89"),std::string::npos);
  EXPECT_NE(s.find("\"nid_capture_noise_floor_db\":42.125"),std::string::npos);
}

TEST(PassiveMetrics, TailKeysSerializedAndWeakGetterKeysOmitted) {
  nr_passive_metrics_t m = {};
  m.acq_state = "TRACKING";
  m.t_real_ns = 1700000000123456789ull; m.proc_utime_us = 11; m.proc_stime_us = 22; m.proc_maxrss_kb = 33; m.proc_minflt = 44; m.proc_majflt = 5;
  char buf[16384];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  std::string s(buf);
  EXPECT_NE(s.find("\"t_real_ns\":1700000000123456789,\"proc_utime_us\":11,\"proc_stime_us\":22,\"proc_maxrss_kb\":33,\"proc_minflt\":44,\"proc_majflt\":5"), std::string::npos);
  for (const char *k : {"cfg_epoch", "cfg_identity_gen", "recov_", "retry_yield_"})  // getter absent: omitted, not 0
    EXPECT_EQ(s.find(k), std::string::npos) << k;
  m.has_cfg_epoch = m.has_recov = m.has_retry_yield = 1;
  m.cfg_epoch = 7; m.cfg_identity_gen = 2; m.recov_submitted = 9; m.recov_yielded = 3; m.recov_late_ms_p50 = 1; m.recov_late_ms_p99 = 6;
  m.retry_yield_chains = 100; m.retry_yield_yields = 4; m.retry_yield_gated = 8;
  const int n = nr_passive_metrics_to_json(&m, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  s.assign(buf, n);
  EXPECT_EQ(s.back(), '}');
  for (const char *k : {"\"cfg_epoch\":7,\"cfg_identity_gen\":2", "\"recov_submitted\":9", "\"recov_yielded\":3,\"recov_late_ms_p50\":1,\"recov_late_ms_p99\":6",
                        "\"retry_yield_chains\":100,\"retry_yield_yields\":4,\"retry_yield_gated\":8"})
    EXPECT_NE(s.find(k), std::string::npos) << k;
}

TEST(PassiveMetrics, TrafficKeysPresentOnlyInTrafficMode) {
  nr_passive_metrics_t m = {};
  m.acq_state = "TRACKING";
  char buf[8192];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  EXPECT_EQ(std::string(buf).find("dci_accepted_10"), std::string::npos);  // legacy line unchanged
  EXPECT_EQ(std::string(buf).find("probe_skip"), std::string::npos);
  m.has_traffic = 1;
  m.traffic.accepted[NR_DCI_REC_FMT_10] = 7; m.traffic.state[NR_LAYOUT_UNSOLVED] = 3; m.traffic.emit_us_p99 = 9;
  m.dci_rec_generated = 5; m.traffic.reopens = 2; m.traffic.reopens_ul = 4;
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  const std::string s(buf);
  for (const char *k : {"\"dci_accepted_10\":7", "\"dci_accepted_11\":0", "\"dci_accepted_00\":0", "\"dci_accepted_01\":0",
                        "\"dci_state_fixed\":0", "\"dci_state_solved\":0", "\"dci_state_cand\":0", "\"dci_state_unsolved\":3",
                        "\"dci_rec_generated\":5", "\"dci_rec_persisted\":0", "\"dci_rec_lost\":0", "\"check_decodes\":0",
                        "\"check_fail\":0", "\"probe_decodes\":0", "\"reopens\":2", "\"reopens_ul\":4", "\"dci_emit_us_p50\":0", "\"dci_emit_us_p99\":9",
                        "\"traffic_skip\":0", "\"dci_dup\":0", "\"check_dl\":0", "\"check_ul\":0", "\"probe_dl\":0", "\"probe_ul\":0",
                        "\"probe_skip\":0", "\"ul_decode_skip\":0"})
    EXPECT_NE(s.find(k), std::string::npos) << k;
}

TEST(PassiveMetrics, AdmitClassCountersOmittedUntilAttempted)
{
  nr_passive_metrics_t m = {};
  m.acq_state = "TRACKING";
  char buf[8192];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  EXPECT_EQ(std::string(buf).find("admit_janon"), std::string::npos);
  m.admit_cls[0][0] = 9; m.admit_cls[0][1] = 4; m.admit_cls[0][2] = 3; m.admit_cls[0][3] = 1;
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  const std::string s(buf);
  for (const char *k : {"\"admit_janon_attempts\":9", "\"admit_janon_skipped\":4", "\"admit_janon_recovered\":3", "\"admit_janon_expired\":1"})
    EXPECT_NE(s.find(k), std::string::npos) << k;
  EXPECT_EQ(s.find("admit_dmrs_id"), std::string::npos);
}
