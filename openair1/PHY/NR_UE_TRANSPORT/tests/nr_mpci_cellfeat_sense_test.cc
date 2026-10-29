/* mpci CELLFEAT: the sense-API session is per cell. Cell A's files / counters must not change when cell B runs (one thread re-binding between calls,
 * and two threads bound to their cell), a cell that closes leaves the other one open, and the per-cell names come from nr_cell_path rules. */
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include "common/utils/nr_cell.h"
#include "nr_sense_api.h"

extern "C" {
void *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { abort(); }
}

namespace {
std::string slurp(const std::string &p)
{
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
struct Dir {
  std::string path;
  Dir()
  {
    char t[] = "/tmp/mpci_cellfeat_XXXXXX";
    path = mkdtemp(t);
  }
  ~Dir() { std::string c = "rm -rf " + path; (void)!system(c.c_str()); }
  std::string f(const std::string &n) const { return path + "/" + n; }
};
/* what a cell does with its session: nframes CFR frames of `src`, nrec records tagged with `tag` */
void workload(int nframes, int nrec, int src, const char *tag, int step)
{
  float iq[2 * 8];
  uint16_t k[8];
  uint8_t l[8];
  for (int i = 0; i < 16; i++) iq[i] = i + 0.5f + step;
  for (int i = 0; i < 8; i++) { k[i] = 10 + i; l[i] = i % 3; }
  for (int i = 0; i < nframes; i++)
    nr_sense_push_cfr((nr_sense_src_t)src, 1, iq, 8, 8, k, l, 100 + i, 0.25f, 1.5f, 1, 2, nullptr);
  for (int i = 0; i < nrec; i++) {
    char line[128];
    snprintf(line, sizeof(line), "{\"schema\":2,\"type\":\"rs\",\"tag\":\"%s\",\"i\":%d}", tag, i + step);
    nr_sense_push_record(line);
  }
}
struct Out { std::string rec, cfr; nr_sense_stats_t st; };
/* one cell alone: open, workload (called in `steps` slices to mirror the interleaved run), close */
Out solo(int cell, const Dir &d, const char *name, int frames, int recs, int src, const char *tag, int slices)
{
  nr_cell_bind(cell);
  std::string r = d.f(std::string(name) + ".rec"), c = d.f(std::string(name) + ".cfr");
  nr_sense_cfg_t cfg{r.c_str(), c.c_str(), nullptr, 16, 64, 64, 1};
  EXPECT_EQ(nr_sense_open(&cfg), 0);
  for (int s = 0; s < slices; s++) workload(frames, recs, src, tag, s * 100);
  nr_sense_close();
  Out o{slurp(r), slurp(c), {}};
  nr_sense_stats(&o.st);
  nr_cell_bind(0);
  return o;
}
}

TEST(CellFeatSense, TwoCellsInterleavedOnOneThreadEqualSoloRuns)
{
  Dir d;
  const Out a = solo(0, d, "soloA", 2, 3, NR_SENSE_SRC_CSIRS, "A", 3);
  const Out b = solo(1, d, "soloB", 1, 5, NR_SENSE_SRC_PRACH, "B", 3);
  ASSERT_FALSE(a.rec.empty());
  ASSERT_FALSE(b.rec.empty());
  ASSERT_NE(a.rec, b.rec);

  std::string ra = d.f("a.rec"), ca = d.f("a.cfr"), rb = d.f("b.rec"), cb = d.f("b.cfr");
  nr_sense_cfg_t cfga{ra.c_str(), ca.c_str(), nullptr, 16, 64, 64, 1}, cfgb{rb.c_str(), cb.c_str(), nullptr, 16, 64, 64, 1};
  nr_cell_bind(0);
  ASSERT_EQ(nr_sense_open(&cfga), 0);
  nr_cell_bind(1);
  ASSERT_EQ(nr_sense_open(&cfgb), 0);
  for (int s = 0; s < 3; s++) { /* alternate slices, the one thread re-binds between calls */
    nr_cell_bind(0);
    workload(2, 3, NR_SENSE_SRC_CSIRS, "A", s * 100);
    nr_cell_bind(1);
    workload(1, 5, NR_SENSE_SRC_PRACH, "B", s * 100);
  }
  nr_cell_bind(1);
  nr_sense_close();
  nr_sense_stats_t sb{};
  nr_sense_stats(&sb);
  nr_cell_bind(0);
  EXPECT_TRUE(nr_sense_enabled()) << "closing cell 1 must not close cell 0";
  nr_sense_close();
  nr_sense_stats_t sa{};
  nr_sense_stats(&sa);
  EXPECT_EQ(slurp(ra), a.rec);
  EXPECT_EQ(slurp(ca), a.cfr);
  EXPECT_EQ(slurp(rb), b.rec);
  EXPECT_EQ(slurp(cb), b.cfr);
  EXPECT_EQ(sa.rec_pushed, a.st.rec_pushed);
  EXPECT_EQ(sa.cfr_pushed, a.st.cfr_pushed);
  EXPECT_EQ(sb.rec_pushed, b.st.rec_pushed);
  EXPECT_EQ(sb.cfr_pushed, b.st.cfr_pushed);
  EXPECT_EQ(sa.rec_pushed, 9u);
  EXPECT_EQ(sb.rec_pushed, 15u);
}

TEST(CellFeatSense, TwoBoundThreadsEqualSoloRuns)
{
  Dir d;
  const Out a = solo(0, d, "soloA", 2, 3, NR_SENSE_SRC_CSIRS, "A", 4);
  const Out b = solo(1, d, "soloB", 3, 2, NR_SENSE_SRC_SRS, "B", 4);
  std::string ra = d.f("a.rec"), ca = d.f("a.cfr"), rb = d.f("b.rec"), cb = d.f("b.cfr");
  nr_sense_cfg_t cfga{ra.c_str(), ca.c_str(), nullptr, 16, 64, 64, 1}, cfgb{rb.c_str(), cb.c_str(), nullptr, 16, 64, 64, 1};
  auto run = [](int cell, const nr_sense_cfg_t *cfg, int fr, int rc, int src, const char *tag) {
    nr_cell_bind(cell);
    ASSERT_EQ(nr_sense_open(cfg), 0);
    for (int s = 0; s < 4; s++) workload(fr, rc, src, tag, s * 100);
    nr_sense_close();
  };
  std::thread t0(run, 0, &cfga, 2, 3, (int)NR_SENSE_SRC_CSIRS, "A"), t1(run, 1, &cfgb, 3, 2, (int)NR_SENSE_SRC_SRS, "B");
  t0.join();
  t1.join();
  EXPECT_EQ(slurp(ra), a.rec);
  EXPECT_EQ(slurp(ca), a.cfr);
  EXPECT_EQ(slurp(rb), b.rec);
  EXPECT_EQ(slurp(cb), b.cfr);
}

TEST(CellFeatSense, ClosedCellCountsAfterCloseOtherCellKeepsRunning)
{
  Dir d;
  std::string r0 = d.f("0.rec"), r1 = d.f("1.rec");
  nr_sense_cfg_t c0{r0.c_str(), nullptr, nullptr, 4, 8, 16, 1}, c1{r1.c_str(), nullptr, nullptr, 4, 8, 16, 1};
  nr_cell_bind(0);
  ASSERT_EQ(nr_sense_open(&c0), 0);
  nr_cell_bind(1);
  ASSERT_EQ(nr_sense_open(&c1), 0);
  nr_cell_bind(0);
  nr_sense_close();
  EXPECT_FALSE(nr_sense_enabled());
  EXPECT_FALSE(nr_sense_push_record("{\"x\":1}"));
  nr_cell_bind(1);
  EXPECT_TRUE(nr_sense_enabled());
  EXPECT_TRUE(nr_sense_push_record("{\"x\":2}"));
  nr_sense_close();
  nr_cell_bind(0);
  nr_sense_stats_t s0{};
  nr_sense_stats(&s0);
  EXPECT_EQ(s0.after_close, 1u);
  nr_cell_bind(1);
  nr_sense_stats_t s1{};
  nr_sense_stats(&s1);
  EXPECT_EQ(s1.after_close, 0u);
  EXPECT_EQ(slurp(r1), "{\"x\":2}\n");
  EXPECT_EQ(slurp(r0), "");
  nr_cell_bind(0);
}

TEST(CellFeatSense, PerCellNamesKeepCellZeroBytes)
{
  Dir d;
  char o[512];
  nr_sense_cell_file(0, "/tmp/records.jsonl", o, sizeof(o));
  EXPECT_STREQ(o, "/tmp/records.jsonl");
  nr_sense_cell_file(0, "", o, sizeof(o));
  EXPECT_STREQ(o, "");
  nr_sense_cell_file(1, "/tmp/records.jsonl", o, sizeof(o));
  EXPECT_STREQ(o, "/tmp/records.cell1.jsonl");
  nr_sense_cell_file(3, "/tmp/a/records", o, sizeof(o));
  EXPECT_STREQ(o, "/tmp/a/records.cell3") << "an extension-less file name must stay a file";
  nr_sense_cell_file(2, d.path.c_str(), o, sizeof(o));
  EXPECT_EQ(std::string(o), d.path + "/cell2") << "an existing directory takes nr_cell_path's directory form";
  nr_sense_cell_file(1, "", o, sizeof(o));
  EXPECT_STREQ(o, "");
  char e[256];
  EXPECT_EQ(nr_sense_cell_endpoint(0, "tcp://*:5555", e, sizeof(e)), 0);
  EXPECT_STREQ(e, "tcp://*:5555");
  EXPECT_EQ(nr_sense_cell_endpoint(2, "tcp://*:5555", e, sizeof(e)), 0);
  EXPECT_STREQ(e, "tcp://*:5557");
  EXPECT_EQ(nr_sense_cell_endpoint(1, "tcp://10.0.0.1:65535", e, sizeof(e)), -1) << "port overflow";
  EXPECT_EQ(nr_sense_cell_endpoint(1, "tcp://host", e, sizeof(e)), -1);
  EXPECT_EQ(nr_sense_cell_endpoint(1, "ipc:///tmp/sense.sock", e, sizeof(e)), 0);
  EXPECT_STREQ(e, "ipc:///tmp/sense.cell1.sock");
  EXPECT_EQ(nr_sense_cell_endpoint(1, "inproc://sense", e, sizeof(e)), 0);
  EXPECT_STREQ(e, "inproc://sense.cell1");
  EXPECT_EQ(nr_sense_cell_endpoint(1, "", e, sizeof(e)), 0);
  EXPECT_STREQ(e, "");
  EXPECT_EQ(nr_sense_cell_endpoint(1, "udp://x", e, sizeof(e)), -1);
}
