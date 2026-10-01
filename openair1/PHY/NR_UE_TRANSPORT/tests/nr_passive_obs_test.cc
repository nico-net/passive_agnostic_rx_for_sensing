#include <gtest/gtest.h>
#include <cmath>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
extern "C" {
#include "nr_passive_obs.h"
}

static nr_passive_obs_t sample() {
  nr_passive_obs_t o = {};
  o.abs_slot = 1000; o.frame = 12; o.slot = 3; o.pci = 64; o.dir = NR_OBS_DIR_DL; o.rnti = 0x4768; o.rnti_class = 0;
  o.start_rb = 0; o.nb_rb = 106; o.start_sym = 1; o.nb_sym = 13; o.mcs = 9; o.mcs_table = 0; o.qm = 2; o.nl = 1;
  o.dmrs_symb_pos = 0x804; o.dmrs_scrambling_id = 64; o.tbs = 25104; o.harq_pid = 3; o.rv = 0; o.ndi = 1;
  o.crc = NR_OBS_CRC_OK; o.nvar = 12.5f; o.snr_db = NAN; o.fo_comp_hz = -13.4f; o.delay_samples = NAN;
  o.carrier_hz = 3619200000LL; o.scs_khz = 30; o.fs_hz = 61440000;
  return o;
}

TEST(PassiveObs, JsonHasSchemaAndNullsForNan) {
  char buf[1024];
  const nr_passive_obs_t o = sample();
  const int n = nr_passive_obs_to_json(&o, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  const std::string s(buf, n);
  EXPECT_NE(s.find("\"schema\":1"), std::string::npos);
  EXPECT_NE(s.find("\"dir\":\"DL\""), std::string::npos);
  EXPECT_NE(s.find("\"rnti\":18280"), std::string::npos);
  EXPECT_NE(s.find("\"snr_db\":null"), std::string::npos);
  EXPECT_NE(s.find("\"crc\":1"), std::string::npos);
  EXPECT_EQ(s.find('\n'), std::string::npos);
}

TEST(PassiveObs, UnknownsAreNullAndFloatsKeepPrecision) {
  char buf[1024];
  nr_passive_obs_t o = sample();
  o.harq_pid = -1; o.crc = NR_OBS_CRC_NA; o.carrier_hz = -1; o.abs_slot = -1; o.nvar = INFINITY;
  const int n = nr_passive_obs_to_json(&o, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  const std::string s(buf, n);
  for (const char *k : {"\"harq_pid\":null", "\"crc\":null", "\"carrier_hz\":null", "\"abs_slot\":null",
                        "\"nvar\":null", "\"fo_comp_hz\":-13.4", "\"fs_hz\":61440000", "\"scs_khz\":30"})
    EXPECT_NE(s.find(k), std::string::npos) << k;
  EXPECT_EQ(s.find("-1,"), std::string::npos);   // no integer sentinel leaks into JSON
  EXPECT_EQ(nr_passive_obs_to_json(&o, buf, 16), -1);
}

TEST(PassiveObs, PushWithoutOpenIsNoop) {
  const nr_passive_obs_t o = sample();
  EXPECT_FALSE(nr_passive_obs_push(&o));
}

TEST(PassiveObs, WritesEveryRecordFromManyThreads) {
  char path[] = "/tmp/obs_test_XXXXXX"; const int fd = mkstemp(path); close(fd);
  ASSERT_TRUE(nr_passive_obs_open(path, 1 << 16));
  EXPECT_FALSE(nr_passive_obs_open(path, 16)); // open twice is rejected
  std::vector<std::thread> th;
  for (int t = 0; t < 4; t++) th.emplace_back([] { nr_passive_obs_t o = sample(); for (int i = 0; i < 5000; i++) nr_passive_obs_push(&o); });
  for (auto &x : th) x.join();
  nr_passive_obs_close();
  { nr_passive_obs_t o = sample(); EXPECT_FALSE(nr_passive_obs_push(&o)); } // push after close is a no-op
  uint64_t p, w, d; nr_passive_obs_stats(&p, &w, &d);
  EXPECT_EQ(p, 20000u); EXPECT_EQ(d, 0u); EXPECT_EQ(w, 20000u);
  std::ifstream f(path); int lines = 0; std::string l; while (std::getline(f, l)) lines++;
  EXPECT_EQ(lines, 20000); unlink(path);
}

TEST(PassiveObs, FullRingDropsAndCountsInsteadOfBlocking) {
  char path[] = "/tmp/obs_test_XXXXXX"; const int fd = mkstemp(path); close(fd);
  setenv("ISAC_OBS_TEST_WRITER_PAUSE_MS", "300", 1); // writer sleeps first: forces the ring to fill
  ASSERT_TRUE(nr_passive_obs_open(path, 64));
  const nr_passive_obs_t o = sample(); int ok = 0;
  for (int i = 0; i < 1000; i++) ok += nr_passive_obs_push(&o);
  uint64_t p, w, d; nr_passive_obs_stats(&p, &w, &d);
  EXPECT_LE(ok, 64); EXPECT_EQ(p + d, 1000u); EXPECT_GE(d, 936u);
  nr_passive_obs_close(); unsetenv("ISAC_OBS_TEST_WRITER_PAUSE_MS"); unlink(path);
}
