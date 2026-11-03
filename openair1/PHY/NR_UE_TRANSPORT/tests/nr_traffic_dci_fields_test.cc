#include "nr_traffic_dci_fields.h"
#include <gtest/gtest.h>
#include <cstdio>

// Set `w` bits of `v` at MSB-first position `pos` of a len-bit payload.
static void put(nr_dci_bits_t &p, int len, int pos, int w, unsigned v)
{
  for (int i = 0; i < w; i++) {
    int bit = len - 1 - (pos + i);
    if ((v >> (w - 1 - i)) & 1)
      p.w[bit / 64] |= 1ULL << (bit % 64);
  }
}

TEST(TrafficDciFields, Dci10HarqFields)
{
  nr_dci_bits_t p{};
  const int fdra = 15, len = 1 + fdra + 4 + 1 + 5 + 1 + 2 + 4 + 2 + 2 + 3 + 3, pos = len - 10;
  put(p, len, pos, 2, 2); put(p, len, pos + 2, 2, 1); put(p, len, pos + 4, 3, 5); put(p, len, pos + 7, 3, 3);
  nr_dci_harq_fields_t f;
  ASSERT_TRUE(nr_dci10_harq_fields(p, len, fdra, &f));
  EXPECT_EQ(f.dai, 2); EXPECT_EQ(f.tpc, 1); EXPECT_EQ(f.pri, 5); EXPECT_EQ(f.k1, 3);
}

TEST(TrafficDciFields, Dci10TooShortFails)
{
  nr_dci_bits_t p{};
  nr_dci_harq_fields_t f{};
  EXPECT_FALSE(nr_dci10_harq_fields(p, 1 + 15 + 4 + 1 + 5 + 1 + 2 + 4 + 9, 15, &f));
  EXPECT_EQ(f.dai, -1); EXPECT_EQ(f.k1, -1);
}

static bool run11(int pre_ant, nr_dci_harq_fields_t *f, nr_dci_bits_t p = {})
{
  nr_dci11_layout_t l{};
  l.pre_ant = (uint8_t)pre_ant; l.ant_ports = 4; l.fdra_mode = NR_FDRA_TYPE1;
  nr_dci11_offsets_t off{};
  if (!nr_dci11_layout_offsets(&l, 11, 4, &off)) return false;
  return nr_dci11_harq_fields(p, off.total, &l, &off, f);
}

TEST(TrafficDciFields, Dci11UniqueSplitParses)
{
  // pre_ant 14 is NOT unique under the sweep tables (see PreAntUniqueValues); 9 = harq4+tpc2+pri3 and 29 = tb2 8+harq5+dai6+2+tpc2 2+3+k1 3 are.
  const int pre_ant = 9;
  nr_dci11_layout_t l{};
  l.pre_ant = pre_ant; l.ant_ports = 4; l.fdra_mode = NR_FDRA_TYPE1;
  nr_dci11_offsets_t off{};
  ASSERT_TRUE(nr_dci11_layout_offsets(&l, 11, 4, &off));
  nr_dci_bits_t p{};
  const int s = off.ant_ports - pre_ant;
  put(p, off.total, s, 4, 0xA); put(p, off.total, s + 4, 2, 3); put(p, off.total, s + 6, 3, 6);
  nr_dci_harq_fields_t f;
  ASSERT_TRUE(nr_dci11_harq_fields(p, off.total, &l, &off, &f));
  EXPECT_EQ(f.dai, -1); EXPECT_EQ(f.tpc, 3); EXPECT_EQ(f.pri, 6); EXPECT_EQ(f.k1, -1);
}

TEST(TrafficDciFields, PreAntAmbiguousGivesNull)
{
  nr_dci_harq_fields_t f{};
  EXPECT_FALSE(run11(14, &f));
  EXPECT_EQ(f.dai, -1); EXPECT_EQ(f.tpc, -1); EXPECT_EQ(f.pri, -1); EXPECT_EQ(f.k1, -1);
}

TEST(TrafficDciFields, PreAntUniqueValues)
{
  for (int v = 0; v < 64; v++) {
    nr_dci_harq_fields_t f;
    if (run11(v, &f)) printf("unique pre_ant=%d\n", v);
  }
}
