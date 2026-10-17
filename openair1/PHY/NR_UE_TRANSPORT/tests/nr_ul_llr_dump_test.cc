#include <gtest/gtest.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>
extern "C" {
#include "nr_ul_llr_dump.h"
}

static std::string tmpdir()
{
  char t[] = "/tmp/ulllrdumpXXXXXX";
  return mkdtemp(t);
}
static int count_ext(const std::string &d, const char *ext)
{
  int n = 0;
  DIR *D = opendir(d.c_str());
  if (!D) return 0;
  while (dirent *e = readdir(D)) {
    const char *dot = strrchr(e->d_name, '.');
    n += dot && !strcmp(dot, ext);
  }
  closedir(D);
  return n;
}
static nr_ul_llr_dump_rec_t good()
{
  nr_ul_llr_dump_rec_t r;
  memset(&r, 0, sizeof(r));
  r.snr_db = 5; r.pci = 923; r.rnti = 0x585a; r.tried_id = 923; r.search_space = "unknown"; r.cce = -1; r.al = -1;
  r.num_rb = 20; r.mcs = 5; r.G = 3000; r.abs_slot = 100; r.tbs_bits = 400; r.qm = 2;
  return r;
}

TEST(UlLlrDump, OffWritesNothing)
{
  unsetenv("ISAC_UL_LLR_DUMP");
  nr_ul_llr_dump_reset();
  EXPECT_FALSE(nr_ul_llr_dump_enabled());
  std::vector<int16_t> l(3000, 7);
  nr_ul_llr_dump_rec_t r = good();
  EXPECT_EQ(0, nr_ul_llr_dump_maybe(&r, l.data()));
}

TEST(UlLlrDump, GateRoundTripAndCap)
{
  std::string d = tmpdir();
  setenv("ISAC_UL_LLR_DUMP", d.c_str(), 1);
  setenv("ISAC_UL_LLR_DUMP_MAX", "3", 1);
  setenv("ISAC_UL_LLR_DUMP_OK_MAX", "1", 1);
  nr_ul_llr_dump_reset();
  ASSERT_TRUE(nr_ul_llr_dump_enabled());
  std::vector<int16_t> l(3000);
  for (int i = 0; i < 3000; i++) l[i] = (int16_t)(i * 7 - 9000);
  nr_ul_llr_dump_rec_t r = good();
  nr_ul_llr_dump_rec_t bad = r;
  bad.probe = true; EXPECT_EQ(0, nr_ul_llr_dump_maybe(&bad, l.data()));
  bad = r; bad.num_rb = 3; EXPECT_EQ(0, nr_ul_llr_dump_maybe(&bad, l.data()));
  bad = r; bad.mcs = 27; EXPECT_EQ(0, nr_ul_llr_dump_maybe(&bad, l.data()));
  bad = r; bad.snr_db = -1; EXPECT_EQ(0, nr_ul_llr_dump_maybe(&bad, l.data()));
  bad = r; bad.snr_db = NAN; EXPECT_EQ(0, nr_ul_llr_dump_maybe(&bad, l.data()));
  nr_ul_llr_dump_flush();
  EXPECT_EQ(0, count_ext(d, ".txt"));
  int queued = 0;
  for (int i = 0; i < 10; i++) { r.abs_slot = 100 + i; queued += nr_ul_llr_dump_maybe(&r, l.data()); nr_ul_llr_dump_flush(); }
  EXPECT_EQ(3, queued); /* cap */
  nr_ul_llr_dump_rec_t ok = r; ok.crc_ok = true;
  EXPECT_EQ(1, nr_ul_llr_dump_maybe(&ok, l.data())); /* separate control cap */
  EXPECT_EQ(0, nr_ul_llr_dump_maybe(&ok, l.data()));
  nr_ul_llr_dump_flush();
  EXPECT_EQ(4, count_ext(d, ".txt"));
  EXPECT_EQ(4, count_ext(d, ".llr"));
  /* round trip of one file + sidecar keys */
  DIR *D = opendir(d.c_str());
  bool seen = false;
  while (dirent *e = readdir(D)) {
    const char *dot = strrchr(e->d_name, '.');
    if (!dot || strcmp(dot, ".llr")) continue;
    FILE *f = fopen((d + "/" + e->d_name).c_str(), "rb");
    std::vector<int16_t> b(3000);
    ASSERT_EQ(3000u, fread(b.data(), 2, 3000, f));
    fclose(f);
    EXPECT_EQ(l, b);
    seen = true;
  }
  closedir(D);
  EXPECT_TRUE(seen);
  D = opendir(d.c_str());
  while (dirent *e = readdir(D)) {
    const char *dot = strrchr(e->d_name, '.');
    if (!dot || strcmp(dot, ".txt")) continue;
    char line[2000] = {0};
    FILE *f = fopen((d + "/" + e->d_name).c_str(), "r");
    ASSERT_TRUE(fgets(line, sizeof(line), f));
    fclose(f);
    EXPECT_NE(nullptr, strstr(line, "dci_format=0_1"));
    EXPECT_NE(nullptr, strstr(line, "search_space=unknown"));
    EXPECT_NE(nullptr, strstr(line, "tried_id=923"));
    EXPECT_NE(nullptr, strstr(line, "rnti=0x585a"));
    break;
  }
  closedir(D);
  unsetenv("ISAC_UL_LLR_DUMP");
  unsetenv("ISAC_UL_LLR_DUMP_MAX");
  unsetenv("ISAC_UL_LLR_DUMP_OK_MAX");
}
