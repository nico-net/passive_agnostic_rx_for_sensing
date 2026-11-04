#include "nr_traffic_cfg.h"
#include "nr_sense_api.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

extern "C" {
void *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { abort(); }
bool nr_ue_ctx_fast_open=false;
bool nr_cfg_reconf_enabled(void) { return false; }
uint64_t nr_cfg_epoch_cfr_stamp(void) { return 0; }
uint32_t nr_cfg_epoch_dedicated_current(void) { return 0; }
}

struct Sink {
  char path[32] = "/tmp/nr_tcfg_XXXXXX";
  Sink() {
    int fd = mkstemp(path); close(fd);
    nr_sense_cfg_t cfg{}; cfg.records_path = path; cfg.rec_ring_slots = 512; cfg.cfr_ring_frames = 2; cfg.cfr_max_re = 12;
    EXPECT_EQ(nr_sense_open(&cfg), 0);
    nr_traffic_cfg_reset();
  }
  std::vector<std::string> lines() {
    nr_sense_close();
    std::ifstream f(path); std::vector<std::string> v; std::string l;
    while (std::getline(f, l)) v.push_back(l);
    return v;
  }
  ~Sink() { nr_sense_close(); unlink(path); }
};
static nr_traffic_cfg_snap_t snap()
{
  nr_traffic_cfg_snap_t s{};
  s.identity_gen = 3; s.dedicated_epoch = 7; s.coreset_id = 1; s.coreset_start_rb = 0; s.coreset_n_rb = 48; s.coreset_dur = 2;
  s.bwp_start = 0; s.bwp_size = 106; s.layout_rep = -1; s.dmrs_add_pos = 1; s.mcs_table = 0; s.rm_set = -1;
  for (int i = 0; i < 16; i++) { s.s_by_tda[i] = 2; s.l_by_tda[i] = 12; }
  return s;
}
static bool has(const std::string &s, const char *k) { return s.find(k) != std::string::npos; }

TEST(TrafficCfg, SameSnapshotSameVersion)
{
  Sink k; auto s = snap();
  uint32_t a = nr_traffic_cfg_ref(0x4601, &s), b = nr_traffic_cfg_ref(0x4601, &s);
  EXPECT_EQ(a, b);
  auto l = k.lines();
  ASSERT_EQ(l.size(), 1u);
  EXPECT_TRUE(has(l[0], "\"type\":\"config\"")); EXPECT_TRUE(has(l[0], "\"rnti\":17921"));
  EXPECT_TRUE(has(l[0], "\"cfg_ref\":\"3.7.1\"")); EXPECT_TRUE(has(l[0], "\"bwp_size\":106"));
  EXPECT_TRUE(has(l[0], "\"layout_rep\":null")); EXPECT_TRUE(has(l[0], "\"rm_set\":null"));
  EXPECT_TRUE(has(l[0], "\"s_by_tda\":[2,2,"));
}
TEST(TrafficCfg, ChangeBumpsVersion)
{
  Sink k; auto s = snap();
  uint32_t a = nr_traffic_cfg_ref(5, &s);
  s.bwp_size = 217;
  EXPECT_EQ(nr_traffic_cfg_ref(5, &s), a + 1);
  EXPECT_EQ(nr_traffic_cfg_ref(6, &s), a);   // other RNTI has its own version
  auto l = k.lines();
  ASSERT_EQ(l.size(), 3u);
  EXPECT_TRUE(has(l[1], "\"bwp_size\":217")); EXPECT_TRUE(has(l[1], "\"cfg_ref\":\"3.7.2\""));
}
TEST(TrafficCfg, ResetRestartsVersions)
{
  Sink k; auto s = snap();
  nr_traffic_cfg_ref(5, &s); s.bwp_size = 217; nr_traffic_cfg_ref(5, &s);
  nr_traffic_cfg_reset();
  EXPECT_EQ(nr_traffic_cfg_ref(5, &s), 1u);
  EXPECT_EQ(k.lines().size(), 3u);
}
TEST(TrafficCfg, LruEvictsOldest)
{
  Sink k; auto s = snap();
  for (int r = 1; r <= 257; r++) nr_traffic_cfg_ref((uint16_t)r, &s);   // 257 RNTIs in a 256 table: rnti 1 evicted
  nr_traffic_cfg_ref(2, &s); nr_traffic_cfg_ref(1, &s);   // 2 still cached, 1 re-announced
  EXPECT_EQ(k.lines().size(), 258u);
}
