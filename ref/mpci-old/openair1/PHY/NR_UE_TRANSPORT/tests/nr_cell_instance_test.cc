// MPCI M2: per-cell instance tag. Two forked processes (one per cell) must never share a host-global path.
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
extern "C" {
#include "common/utils/nr_cell_instance.h"
#include "nr_passive_cfg_sources.h"
#include "nr_passive_metrics.h"
}

static std::string path_of(const char *def)
{
  char p[256];
  nr_cell_path(def, p, sizeof(p));
  return p;
}

TEST(CellInstance, UnsetIsUnchanged)
{
  unsetenv("ISAC_CELL_INSTANCE");
  EXPECT_EQ(nr_cell_instance(), nullptr);
  EXPECT_EQ(path_of("/tmp/coresets_discovered.txt"), "/tmp/coresets_discovered.txt");
  EXPECT_EQ(path_of("/tmp/passive_rx"), "/tmp/passive_rx");
  setenv("ISAC_CELL_INSTANCE", "", 1);
  EXPECT_EQ(path_of("/tmp/passive_rx"), "/tmp/passive_rx");
  char p[256];
  nr_cfg_sib1_cache_name(p, sizeof(p), nullptr, 7, 0, false);
  EXPECT_STREQ(p, "/tmp/passive_rx/sib1_common_pci7.bin");
  nr_passive_metrics_t m = {};
  char j[16384];
  ASSERT_GT(nr_passive_metrics_to_json(&m, j, sizeof(j)), 0);
  EXPECT_EQ(std::string(j).find("\"cell\""), std::string::npos);
  EXPECT_EQ(std::string(j).rfind("{\"schema\":", 0), 0u);
}

TEST(CellInstance, TaggedPaths)
{
  setenv("ISAC_CELL_INSTANCE", "cellA-1", 1);
  EXPECT_EQ(path_of("/tmp/coresets_discovered.txt"), "/tmp/coresets_discovered.cellA-1.txt");
  EXPECT_EQ(path_of("/tmp/passive_rx"), "/tmp/passive_rx/cellA-1");
  EXPECT_EQ(path_of("/tmp/pdcch_fixture.bin"), "/tmp/pdcch_fixture.cellA-1.bin");
  char p[256];
  nr_cfg_sib1_cache_name(p, sizeof(p), nullptr, 7, 0, false);
  EXPECT_STREQ(p, "/tmp/passive_rx/cellA-1/sib1_common_pci7.bin");
  nr_cfg_sib1_cache_name(p, sizeof(p), "/explicit", 7, 0, false);  // explicit dir wins
  EXPECT_STREQ(p, "/explicit/sib1_common_pci7.bin");
  nr_passive_metrics_t m = {};
  char j[16384];
  ASSERT_GT(nr_passive_metrics_to_json(&m, j, sizeof(j)), 0);
  EXPECT_EQ(std::string(j).rfind("{\"schema\":", 0), 0u);
  EXPECT_NE(std::string(j).find(",\"cell\":\"cellA-1\","), std::string::npos);
  unsetenv("ISAC_CELL_INSTANCE");
}

TEST(CellInstance, InvalidTagIsFatal)
{
  for (const char *bad : {"a/b", "a b", "../x", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}) {
    setenv("ISAC_CELL_INSTANCE", bad, 1);
    EXPECT_EXIT(nr_cell_instance(), ::testing::ExitedWithCode(2), "invalid");
  }
  unsetenv("ISAC_CELL_INSTANCE");
}

// Two cell processes, same PCI, default dirs: each writes its SIB1 cache file; they must land in different files.
TEST(CellInstance, TwoProcessesSamePciDifferentFiles)
{
  const char *tags[2] = {"mpciTestA", "mpciTestB"};
  std::string files[2];
  for (int i = 0; i < 2; i++) {
    int pfd[2];
    ASSERT_EQ(pipe(pfd), 0);
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      setenv("ISAC_CELL_INSTANCE", tags[i], 1);
      char p[256], d[256];
      nr_cfg_sib1_cache_name(p, sizeof(p), nullptr, 42, 0, false);
      snprintf(d, sizeof(d), "%s", p);
      *strrchr(d, '/') = 0;
      nr_cell_mkdirs(d);
      FILE *f = fopen(p, "wb");
      if (!f) _exit(3);
      fputs(tags[i], f);
      fclose(f);
      (void)!write(pfd[1], p, strlen(p));
      _exit(0);
    }
    close(pfd[1]);
    char buf[256] = {};
    (void)!read(pfd[0], buf, sizeof(buf) - 1);
    close(pfd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    ASSERT_TRUE(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    files[i] = buf;
  }
  EXPECT_NE(files[0], files[1]);
  for (int i = 0; i < 2; i++) {
    char c[64] = {};
    FILE *f = fopen(files[i].c_str(), "rb");
    ASSERT_NE(f, nullptr);
    (void)!fread(c, 1, sizeof(c) - 1, f);
    fclose(f);
    EXPECT_STREQ(c, tags[i]);  // not overwritten by the sibling
    unlink(files[i].c_str());
    std::string d = files[i].substr(0, files[i].rfind('/'));
    rmdir(d.c_str());
  }
}
