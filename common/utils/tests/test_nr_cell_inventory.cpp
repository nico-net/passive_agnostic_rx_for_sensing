// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Inventory v1 parser + atomic-file watcher (task D1). Fixtures: tests/passive_rx/mpci/inventory/*.json (receiver-side front-end
// measurements, see their _comment). [OFFLINE VERIFIED, unit]
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
extern "C" {
#include "nr_cell_inventory.h"
}

static std::string slurp(const std::string &p)
{
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
static std::string fx(const char *n)
{
  return std::string(MPCI_INV_DIR) + "/" + n;
}
static std::string cellj(const char *id, const char *extra = "")
{
  return std::string("{\"id\":\"") + id + "\",\"centre_hz\":3680010000,\"gscn\":7971,\"ssb_hz\":3679680000,\"ssb_index\":4,\"pci\":415,"
         "\"cfo_hz\":-4849,\"n_rb_hint\":null,\"scs_khz\":30,\"sss_gap\":4.5,\"dmrs_ratio\":3.7,\"first_seen_ns\":5,\"state\":\"new\"" + extra + "}";
}
static std::string doc(const std::string &cells, const char *schema = "1", const char *win = "{\"centre_hz\":3680010000,\"rate_msps\":122.88,\"n_rx\":4}")
{
  return std::string("{\"schema\":") + schema + ",\"window\":" + win + ",\"cells\":[" + cells + "]}";
}
static int parse(const std::string &t, nr_cell_inventory_t *o, std::string *e = nullptr)
{
  char err[200];
  int r = nr_cell_inventory_parse(t.data(), t.size(), o, err, sizeof err);
  if (e) *e = err;
  return r;
}

TEST(NrCellInventory, FixtureLiveTuned)
{
  nr_cell_inventory_t inv;
  std::string e;
  ASSERT_EQ(0, parse(slurp(fx("live_tuned_20261023.json")), &inv, &e)) << e;
  ASSERT_EQ(7, inv.n);
  EXPECT_STREQ("pci923", inv.cell[1].id);
  EXPECT_EQ(923, inv.cell[1].desc.pci);
  EXPECT_DOUBLE_EQ(3759990000.0, inv.cell[1].desc.centre_hz);
  EXPECT_EQ(217, inv.cell[1].desc.n_rb);
  EXPECT_EQ(51, inv.cell[3].desc.n_rb);
  EXPECT_DOUBLE_EQ(2178, inv.cell[3].desc.cfo_hz);
  EXPECT_EQ(4, inv.cell[0].desc.n_rx);                         // from window.n_rx
  EXPECT_DOUBLE_EQ(3600000000.0, inv.cell[0].desc.window_centre_hz);
  EXPECT_EQ(NR_CELLD_MAINTAIN, inv.cell[0].desc.state);
  EXPECT_EQ(NR_CELLD_NEW, inv.cell[6].desc.state);
  // co-channel pairs: equal centre and SSB frequency, two entries
  EXPECT_DOUBLE_EQ(inv.cell[2].desc.centre_hz, inv.cell[5].desc.centre_hz);
  EXPECT_DOUBLE_EQ(inv.cell[2].desc.ssb_hz, inv.cell[5].desc.ssb_hz);
  EXPECT_DOUBLE_EQ(inv.cell[0].desc.ssb_hz, inv.cell[6].desc.ssb_hz);
  EXPECT_EQ(-1, inv.cell[6].desc.ssb_index);
  EXPECT_DOUBLE_EQ(3679680000.0, inv.cell[0].desc.ssb_hz);   // GSCN 7971
}
TEST(NrCellInventory, FixturePass1)
{
  nr_cell_inventory_t inv;
  std::string e;
  ASSERT_EQ(0, parse(slurp(fx("pass1_20261012.json")), &inv, &e)) << e;
  ASSERT_EQ(6, inv.n);
  for (int i = 0; i < inv.n; i++) EXPECT_EQ(0, inv.cell[i].desc.n_rb);  // null n_rb_hint -> 0 (unknown)
  EXPECT_DOUBLE_EQ(3731850000.0, inv.cell[1].desc.centre_hz);
}
TEST(NrCellInventory, StrictRejections)
{
  nr_cell_inventory_t keep, o;
  ASSERT_EQ(0, parse(doc(cellj("a")), &keep));
  o = keep;
  struct { std::string t; const char *why; } bad[] = {
    {doc(cellj("a"), "2"), "schema 2"},
    {std::string("{\"window\":{\"centre_hz\":1e9,\"rate_msps\":1,\"n_rx\":1},\"cells\":[]}"), "no schema"},
    {std::string("[") + cellj("a") + "]", "bare list (today's cells.json)"},
    {doc(cellj("a") + "," + cellj("a")), "duplicate id"},
    {doc(cellj("a"), "1", "{\"centre_hz\":3.6e9,\"rate_msps\":122.88}"), "window without n_rx"},
    {doc("{\"id\":\"a\"}"), "cell without required keys"},
    {doc(cellj("a", ",\"x\":")), "dangling value"},
    {doc(cellj("a")) + "x", "trailing data"},
    {doc(cellj("a")).substr(0, 100), "truncated"},
    {doc(cellj("a", ",\"state\":\"bogus\"")), "bad state"},
    {doc(cellj("a", ",\"pci\":1008")), "pci range"},
    {doc(cellj("a", ",\"scs_khz\":31")), "scs"},
    {doc(cellj("a", ",\"n_rb_hint\":0")), "n_rb_hint 0"},
    {doc(""), "ok-but-empty is accepted below"},
  };
  for (size_t i = 0; i + 1 < sizeof bad / sizeof bad[0]; i++) {
    std::string e;
    EXPECT_EQ(-1, parse(bad[i].t, &o, &e)) << bad[i].why;
    EXPECT_FALSE(e.empty()) << bad[i].why;
    EXPECT_EQ(1, o.n) << "output untouched: " << bad[i].why;
  }
  EXPECT_EQ(0, parse(bad[13].t, &o));  // empty cells list is a valid inventory
  EXPECT_EQ(0, o.n);
}
TEST(NrCellInventory, UnknownKeysAndLongKeysIgnored)
{
  nr_cell_inventory_t o;
  std::string t = "{\"schema\":1,\"_comment\":\"hello \\\"x\\\" \\u00e9\",\"extra\":{\"a\":[1,2,{\"b\":null}],\"c\":true},"
                  "\"window\":{\"centre_hz\":3.6e9,\"rate_msps\":122.88,\"n_rx\":2,\"note\":\"w\"},\"cells\":[" +
                  cellj("a", ",\"a_key_that_is_much_longer_than_the_forty_char_key_buffer\":[0.5e-3,false]") + "]}";
  std::string e;
  ASSERT_EQ(0, parse(t, &o, &e)) << e;
  EXPECT_EQ(2, o.cell[0].desc.n_rx);
}
TEST(NrCellInventory, DepthBounded)
{
  nr_cell_inventory_t o;
  std::string deep;
  for (int i = 0; i < 200; i++) deep += "[";
  std::string t = "{\"schema\":1,\"x\":" + deep + ",\"window\":{\"centre_hz\":3.6e9,\"rate_msps\":1,\"n_rx\":1},\"cells\":[]}";
  EXPECT_EQ(-1, parse(t, &o));
}
TEST(NrCellInventory, DefaultPath)
{
  char p[64];
  EXPECT_EQ(0, nr_cell_inventory_default_path("/tmp/run1", p, sizeof p));
  EXPECT_STREQ("/tmp/run1/cells.json", p);
  EXPECT_EQ(0, nr_cell_inventory_default_path("/tmp/run1//", p, sizeof p));
  EXPECT_STREQ("/tmp/run1/cells.json", p);
  EXPECT_EQ(-1, nr_cell_inventory_default_path("/tmp/a_long_directory_name", p, 8));
}

struct Dir {
  std::string d;
  Dir()
  {
    char t[] = "/tmp/nr_inv_testXXXXXX";
    d = mkdtemp(t);
  }
  ~Dir()
  {
    std::string c = "rm -rf " + d;
    if (system(c.c_str())) {}
  }
  std::string path(const char *n) { return d + "/" + n; }
};
static void write_atomic(const std::string &path, const std::string &txt)
{
  std::string tmp = path + ".tmp";
  { std::ofstream f(tmp, std::ios::binary); f << txt; }
  ASSERT_EQ(0, rename(tmp.c_str(), path.c_str()));
}
static void write_plain(const std::string &path, const std::string &txt)
{
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << txt;
}

TEST(NrCellInventoryWatch, PollSemantics)
{
  Dir D;
  std::string p = D.path("cells.json");
  nr_cell_inventory_watch_t w;
  ASSERT_EQ(0, nr_cell_inventory_watch_init(&w, p.c_str()));
  char err[200];
  EXPECT_EQ(0, nr_cell_inventory_poll(&w, err, sizeof err));  // no file yet
  EXPECT_FALSE(w.have_inv);
  write_atomic(p, doc(cellj("a")));
  EXPECT_EQ(1, nr_cell_inventory_poll(&w, err, sizeof err));
  EXPECT_EQ(1u, w.generation);
  EXPECT_EQ(1, w.inv.n);
  EXPECT_EQ(0, nr_cell_inventory_poll(&w, err, sizeof err));  // unchanged
  EXPECT_EQ(1u, w.generation);
  // truncated file (non-atomic writer caught half way): rejected, previous kept
  std::string full = doc(cellj("a") + "," + cellj("b"));
  write_atomic(p, full.substr(0, full.size() / 2));
  EXPECT_EQ(-1, nr_cell_inventory_poll(&w, err, sizeof err));
  EXPECT_NE(std::string(err).find("previous inventory kept"), std::string::npos) << err;
  EXPECT_EQ(1u, w.generation);
  EXPECT_EQ(1, w.inv.n);
  EXPECT_EQ(0, nr_cell_inventory_poll(&w, err, sizeof err));  // rejected file is not re-parsed
  // invalid (bare list: the front end's current format)
  write_atomic(p, "[1,2]");
  EXPECT_EQ(-1, nr_cell_inventory_poll(&w, err, sizeof err));
  EXPECT_EQ(1, w.inv.n);
  // empty file
  write_plain(p, "");
  EXPECT_EQ(-1, nr_cell_inventory_poll(&w, err, sizeof err));
  // the complete file arrives: accepted
  write_atomic(p, full);
  EXPECT_EQ(1, nr_cell_inventory_poll(&w, err, sizeof err));
  EXPECT_EQ(2u, w.generation);
  EXPECT_EQ(2, w.inv.n);
}
TEST(NrCellInventoryWatch, ChangeDetectedBySizeAndInode)
{
  Dir D;
  std::string p = D.path("cells.json");
  nr_cell_inventory_watch_t w;
  nr_cell_inventory_watch_init(&w, p.c_str());
  char err[200];
  std::string a = doc(cellj("a")), b = doc(cellj("b"));  // same size, different content
  ASSERT_EQ(a.size(), b.size());
  write_atomic(p, a);
  ASSERT_EQ(1, nr_cell_inventory_poll(&w, err, sizeof err));
  struct stat st;
  stat(p.c_str(), &st);
  // rewrite with the same size and force the same mtime: only the inode (rename) differs
  write_atomic(p, b);
  struct timespec ts[2] = {st.st_atim, st.st_mtim};
  utimensat(AT_FDCWD, p.c_str(), ts, 0);
  EXPECT_EQ(1, nr_cell_inventory_poll(&w, err, sizeof err));
  EXPECT_STREQ("b", w.inv.cell[0].id);
}
TEST(NrCellInventoryWatch, PathTooLong)
{
  nr_cell_inventory_watch_t w;
  std::string p(600, 'x');
  EXPECT_EQ(-1, nr_cell_inventory_watch_init(&w, p.c_str()));
}
