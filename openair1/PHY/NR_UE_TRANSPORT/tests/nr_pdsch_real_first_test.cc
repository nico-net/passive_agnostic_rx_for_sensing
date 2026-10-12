#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_real_first.h"
}

struct E {
  bool probe;
  long slot;
  int id;
};
static bool p_of(const void *e) { return ((const E *)e)->probe; }
static long s_of(const void *e) { return ((const E *)e)->slot; }

/* ring of depth 6 starting at tail 4 (wraps); returns the ids in ring order */
static std::vector<int> run(std::vector<E> in, int *moved)
{
  const int depth = 6, tail = 4, n = (int)in.size();
  E ring[depth] = {};
  uint64_t ns[depth] = {};
  for (int i = 0; i < n; i++) {
    ring[(tail + i) % depth] = in[i];
    ns[(tail + i) % depth] = 100 + in[i].id;
  }
  E scratch;
  *moved = nr_real_first_promote(ring, sizeof(E), ns, tail, n, depth, p_of, s_of, &scratch);
  std::vector<int> out;
  for (int i = 0; i < n; i++) {
    const E &e = ring[(tail + i) % depth];
    EXPECT_EQ(ns[(tail + i) % depth], 100u + e.id); /* the timestamp moves with its entry */
    out.push_back(e.id);
  }
  return out;
}

TEST(RealFirst, FullDecodeSlotMovesAheadOfOlderProbes)
{
  int moved;
  /* probes of slot 10, full decode + probe of slot 11, probe of slot 12 */
  auto o = run({{true, 10, 0}, {true, 10, 1}, {false, 11, 2}, {true, 11, 3}, {true, 12, 4}}, &moved);
  EXPECT_EQ(moved, 2);
  EXPECT_EQ(o, (std::vector<int>{2, 3, 0, 1, 4}));
}

TEST(RealFirst, NoChange)
{
  int moved;
  auto o = run({{false, 10, 0}, {true, 11, 1}, {false, 12, 2}}, &moved); /* head already a full decode */
  EXPECT_EQ(moved, 0);
  EXPECT_EQ(o, (std::vector<int>{0, 1, 2}));
  o = run({{true, 10, 0}, {true, 11, 1}}, &moved); /* probes only */
  EXPECT_EQ(moved, 0);
  o = run({{true, 10, 0}, {false, 10, 1}, {true, 11, 2}}, &moved); /* full decode in the head's slot: one group already */
  EXPECT_EQ(moved, 0);
  EXPECT_EQ(o, (std::vector<int>{0, 1, 2}));
}

TEST(RealFirst, SameSlotEntriesKeepOrderAcrossTheRing)
{
  int moved;
  auto o = run({{true, 9, 0}, {false, 11, 1}, {true, 10, 2}, {true, 11, 3}, {false, 11, 4}, {true, 12, 5}}, &moved);
  EXPECT_EQ(moved, 3);
  EXPECT_EQ(o, (std::vector<int>{1, 3, 4, 0, 2, 5}));
}
