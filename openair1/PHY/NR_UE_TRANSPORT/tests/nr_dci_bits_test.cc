#include <gtest/gtest.h>

#include "nr_dci_bits.h"

TEST(NrDciBits, LegacyFields)
{
  constexpr uint64_t payload = 0x5a35c19e4b27ULL & ((1ULL << 47) - 1);
  const nr_dci_bits_t bits = nr_dci_bits_from_u64(payload);
  for (int pos = 0; pos < 47; ++pos)
    for (int width = 1; width <= 16 && pos + width <= 47; ++width)
      EXPECT_EQ(nr_dci_bits_field(&bits, 47, pos, width),
                (payload >> (47 - pos - width)) & ((1ULL << width) - 1))
          << "pos=" << pos << " width=" << width;
}

TEST(NrDciBits, WideBoundaryBits)
{
  nr_dci_bits_t bits{};
  for (int pos : {0, 63, 64, 127, 128, 139}) {
    const int bit = 139 - pos;
    bits.w[bit / 64] |= 1ULL << (bit % 64);
  }
  for (int pos = 0; pos < 140; ++pos) {
    const bool set = pos == 0 || pos == 63 || pos == 64 || pos == 127 || pos == 128 || pos == 139;
    EXPECT_EQ(nr_dci_bits_field(&bits, 140, pos, 1), set) << "pos=" << pos;
  }
  EXPECT_EQ(nr_dci_bits_field(&bits, 140, 60, 8), 0x18u);
  EXPECT_EQ(nr_dci_bits_field(&bits, 140, 0, 64), (1ULL << 63) | 1ULL);
}

TEST(NrDciBits, EqualityAndHash)
{
  const nr_dci_bits_t a = nr_dci_bits_from_u64(0x1234);
  nr_dci_bits_t b = a;
  EXPECT_TRUE(nr_dci_bits_eq(&a, &b));
  EXPECT_EQ(nr_dci_bits_hash(&a, 47), nr_dci_bits_hash(&b, 47));
  b.w[1] = 1;
  EXPECT_FALSE(nr_dci_bits_eq(&a, &b));
  EXPECT_NE(nr_dci_bits_hash(&a, 140), nr_dci_bits_hash(&b, 140));
  EXPECT_NE(nr_dci_bits_hash(&a, 47), nr_dci_bits_hash(&a, 48));
}
