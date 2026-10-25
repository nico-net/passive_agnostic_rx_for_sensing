// A1 tests: cell index, thread creation, paths, cell-indexed statics.
#include <gtest/gtest.h>
#include <pthread.h>
#include <string>
#include <thread>
extern "C" {
#include "nr_cell.h"
int t_counter_get(void);
void t_counter_add(int);
int *t_counter_addr(void);
int t_pair_a(void);
int t_pair_b(void);
long t_zero_get(void);
int t_big_n(void);
void t_big_set(int);
void t_big_prealloc(int);
}

static std::string P(int c, const char *d, size_t n = 256)
{
  std::string o(n, 'x');
  nr_cell_path(c, d, &o[0], n);
  return std::string(o.c_str());
}

TEST(NrCell, DefaultIsZero)
{
  int r = -1;
  std::thread([&] { r = nr_cell_idx(); }).join();
  EXPECT_EQ(r, 0);
}

TEST(NrCell, BindAndRange)
{
  std::thread([] {
    nr_cell_bind(5);
    EXPECT_EQ(nr_cell_idx(), 5);
    nr_cell_bind(NR_CELL_MAX - 1);
    EXPECT_EQ(nr_cell_idx(), NR_CELL_MAX - 1);
  }).join();
  EXPECT_EXIT(nr_cell_bind(NR_CELL_MAX), ::testing::ExitedWithCode(2), "out of range");
  EXPECT_EXIT(nr_cell_bind(-1), ::testing::ExitedWithCode(2), "out of range");
}

struct Rec { int cell_seen; int grandchild_seen; int explicit_seen; };
static void *grandchild(void *a) { *(int *)a = nr_cell_idx(); return nullptr; }
static void *child(void *a)
{
  Rec *r = (Rec *)a;
  r->cell_seen = nr_cell_idx();
  pthread_t t;
  nr_cell_thread_create(&t, nullptr, grandchild, &r->grandchild_seen, -1);  // inherits
  pthread_join(t, nullptr);
  nr_cell_thread_create(&t, nullptr, grandchild, &r->explicit_seen, 9);     // explicit
  pthread_join(t, nullptr);
  return nullptr;
}

TEST(NrCell, ThreadInheritanceAndExplicit)
{
  Rec r = {-1, -1, -1};
  pthread_t t;
  ASSERT_EQ(nr_cell_thread_create(&t, nullptr, child, &r, 3), 0);
  pthread_join(t, nullptr);
  EXPECT_EQ(r.cell_seen, 3);
  EXPECT_EQ(r.grandchild_seen, 3);
  EXPECT_EQ(r.explicit_seen, 9);
  // creator in cell 0 with cell < 0 -> child 0
  int v = -1;
  ASSERT_EQ(nr_cell_thread_create(&t, nullptr, grandchild, &v, -1), 0);
  pthread_join(t, nullptr);
  EXPECT_EQ(v, 0);
  EXPECT_EXIT(nr_cell_thread_create(&t, nullptr, grandchild, &v, NR_CELL_MAX), ::testing::ExitedWithCode(2), "out of range");
}

TEST(NrCell, PathCell0Unchanged)
{
  for (const char *d : {"/tmp/coresets_discovered.txt", "/tmp/passive_rx", "rel/x.bin", "", "/tmp/.hidden", "a%s%d.b", "/tmp/dir.d/file"})
    EXPECT_EQ(P(0, d), d);
}

TEST(NrCell, PathCells)
{
  EXPECT_EQ(P(1, "/tmp/coresets_discovered.txt"), "/tmp/coresets_discovered.cell1.txt");
  EXPECT_EQ(P(15, "/tmp/pdcch_fixture.bin"), "/tmp/pdcch_fixture.cell15.bin");
  EXPECT_EQ(P(2, "/tmp/passive_rx"), "/tmp/passive_rx/cell2");
  EXPECT_EQ(P(2, "/tmp/passive_rx/"), "/tmp/passive_rx//cell2");
  EXPECT_EQ(P(1, "/tmp/.hidden"), "/tmp/.hidden/cell1");          // leading dot is not an extension
  EXPECT_EQ(P(1, "/tmp/dir.d/file"), "/tmp/dir.d/file/cell1");    // dot in a directory component
  EXPECT_EQ(P(1, "/tmp/a.tar.gz"), "/tmp/a.tar.cell1.gz");
  EXPECT_EQ(P(1, "log.txt"), "log.cell1.txt");
  EXPECT_EXIT(P(NR_CELL_MAX, "/tmp/x"), ::testing::ExitedWithCode(2), "out of range");
  EXPECT_EXIT(P(-1, "/tmp/x"), ::testing::ExitedWithCode(2), "out of range");
}

TEST(NrCell, PathTruncation)
{
  char o[8];
  memset(o, 'z', sizeof o);
  nr_cell_path(1, "/tmp/abcdefghij.txt", o, sizeof o);
  EXPECT_EQ(strlen(o), 7u);
  EXPECT_EQ(std::string(o), "/tmp/ab");
  nr_cell_path(0, "/tmp/abcdefghij.txt", o, 5);
  EXPECT_EQ(std::string(o), "/tmp");
  nr_cell_path(1, "/x.y", o, 1);
  EXPECT_EQ(o[0], 0);
  char z = 'q';
  nr_cell_path(1, "/x.y", &z, 0);  // n == 0 writes nothing
  EXPECT_EQ(z, 'q');
}

TEST(NrCell, StaticsInitializerReplicated)
{
  for (int c = 0; c < NR_CELL_MAX; c++) {
    std::thread([c] {
      nr_cell_bind(c);
      EXPECT_EQ(t_counter_get(), 3);
      EXPECT_EQ(t_pair_a(), 7);
      EXPECT_EQ(t_pair_b(), 9);
      EXPECT_EQ(t_zero_get(), 0);
    }).join();
  }
}

TEST(NrCell, StaticsIsolationTwoThreads)
{
  pthread_barrier_t b;
  pthread_barrier_init(&b, nullptr, 2);
  int seen[3] = {-1, -1, -1};
  int *addr[3] = {nullptr, nullptr, nullptr};
  auto run = [&](int c, int add) {
    nr_cell_bind(c);
    t_counter_add(add);
    addr[c] = t_counter_addr();
    pthread_barrier_wait(&b);  // both written
    seen[c] = t_counter_get();
  };
  std::thread a(run, 1, 100), bb(run, 2, 1000);
  a.join();
  bb.join();
  pthread_barrier_destroy(&b);
  EXPECT_EQ(seen[1], 103);
  EXPECT_EQ(seen[2], 1003);
  EXPECT_NE(addr[1], addr[2]);
  EXPECT_EQ(t_counter_get(), 3);  // cell 0 (main thread) untouched
}

TEST(NrCell, HeapLazyAndFence)
{
  // pre-fence lazy allocation per cell, isolated
  std::thread([] { nr_cell_bind(2); EXPECT_EQ(t_big_n(), 0); t_big_set(42); EXPECT_EQ(t_big_n(), 42); }).join();
  std::thread([] { nr_cell_bind(3); EXPECT_EQ(t_big_n(), 0); }).join();
  std::thread([] { nr_cell_bind(2); EXPECT_EQ(t_big_n(), 42); }).join();
  t_big_prealloc(7);
  EXPECT_EQ(nr_cell_alloc_fenced(), 0);
  // after the fence (in a death-test child): allocated slot fine, new slot fatal
  EXPECT_EXIT(
      {
        nr_cell_alloc_fence();
        nr_cell_bind(2);
        if (t_big_n() != 42) _exit(1);
        nr_cell_bind(7);
        if (t_big_n() != 0) _exit(1);
        nr_cell_bind(11);
        t_big_n();
        _exit(0);
      },
      ::testing::ExitedWithCode(2), "after the RX fence");
}

TEST(NrCell, HeapConcurrentFirstAccess)
{
  std::thread th[8];
  int ok[8] = {};
  for (int i = 0; i < 8; i++)
    th[i] = std::thread([i, &ok] { nr_cell_bind(12); t_big_set(5); ok[i] = t_big_n() == 5; });
  for (auto &t : th) t.join();
  for (int i = 0; i < 8; i++) EXPECT_TRUE(ok[i]);
}
