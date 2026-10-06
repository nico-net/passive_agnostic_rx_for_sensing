/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

extern "C" {
#include "nr_csirs_disc.h"
}

TEST(AEnabled, OffByDefault)
{
  unsetenv("ISAC_CSIRS_DISC");
  EXPECT_FALSE(nr_csirs_disc_enabled());
}

class RingCopyTest : public ::testing::TestWithParam<int> {};

TEST_P(RingCopyTest, CopiesCrbGridAcrossFftWrapBitExactly)
{
  constexpr int fft = 64;
  constexpr int first = 50;
  constexpr int n_rb = 4;
  const int n_ant = GetParam();
  std::vector<std::vector<int16_t>> storage(n_ant, std::vector<int16_t>(NR_CSIRS_DISC_NSYM * fft * 2));
  std::vector<const int16_t *> rows(n_ant);
  for (int ant = 0; ant < n_ant; ++ant) {
    rows[ant] = storage[ant].data();
    for (int sym = 0; sym < NR_CSIRS_DISC_NSYM; ++sym) {
      for (int sc = 0; sc < fft; ++sc) {
        const size_t p = 2 * (static_cast<size_t>(sym) * fft + sc);
        storage[ant][p] = static_cast<int16_t>(10000 * ant + 500 * sym + sc);
        storage[ant][p + 1] = static_cast<int16_t>(-10000 * ant - 500 * sym - sc);
      }
    }
  }

  nr_csirs_disc_ring_t ring{};
  ASSERT_EQ(nr_csirs_disc_ring_init(&ring, n_ant, n_rb, 20, 3, false), 0);
  ASSERT_EQ(nr_csirs_disc_ring_push(&ring, rows.data(), fft, first, 12345, 617, 5), 0);
  EXPECT_EQ(ring.written, 1u);
  EXPECT_EQ(ring.meta[0].abs_slot, 12345u);
  EXPECT_EQ(ring.meta[0].frame, 617u);
  EXPECT_EQ(ring.meta[0].slot, 5u);
  EXPECT_EQ(ring.meta[0].valid, 1u);

  for (int ant = 0; ant < n_ant; ++ant) {
    for (int sym = 0; sym < NR_CSIRS_DISC_NSYM; ++sym) {
      const int16_t *copied = nr_csirs_disc_ring_sym(&ring, 0, ant, sym);
      for (int k = 0; k < 12 * n_rb; ++k) {
        const int source_sc = (first + k) % fft;
        const size_t source = 2 * (static_cast<size_t>(sym) * fft + source_sc);
        EXPECT_EQ(copied[2 * k], storage[ant][source]);
        EXPECT_EQ(copied[2 * k + 1], storage[ant][source + 1]);
      }
    }
  }
  nr_csirs_disc_ring_free(&ring);
}

INSTANTIATE_TEST_SUITE_P(OneAndFourAntennas, RingCopyTest, ::testing::Values(1, 4));

TEST(Ring, WrapsAndReplacesMetadata)
{
  constexpr int fft = 16;
  std::vector<int16_t> samples(NR_CSIRS_DISC_NSYM * fft * 2);
  const int16_t *rows[] = {samples.data()};
  nr_csirs_disc_ring_t ring{};
  ASSERT_EQ(nr_csirs_disc_ring_init(&ring, 1, 1, 20, 2, false), 0);
  EXPECT_EQ(nr_csirs_disc_ring_push(&ring, rows, fft, 12, 10, 1, 0), 0);
  EXPECT_EQ(nr_csirs_disc_ring_push(&ring, rows, fft, 12, 11, 1, 1), 1);
  EXPECT_EQ(nr_csirs_disc_ring_push(&ring, rows, fft, 12, 12, 1, 2), 0);
  EXPECT_EQ(ring.written, 3u);
  EXPECT_EQ(ring.meta[0].abs_slot, 12u);
  EXPECT_EQ(ring.meta[1].abs_slot, 11u);
  nr_csirs_disc_ring_free(&ring);
}

TEST(Ring, DropsWithoutBlockingWhileReaderOwnsRing)
{
  constexpr int fft = 16;
  std::vector<int16_t> samples(NR_CSIRS_DISC_NSYM * fft * 2);
  const int16_t *rows[] = {samples.data()};
  nr_csirs_disc_ring_t ring{};
  ASSERT_EQ(nr_csirs_disc_ring_init(&ring, 1, 1, 20, 2, false), 0);
  const uint64_t before = __atomic_load_n(&csirs_disc_feed_drops, __ATOMIC_RELAXED);
  ASSERT_TRUE(nr_csirs_disc_ring_read_lock(&ring));
  EXPECT_EQ(nr_csirs_disc_ring_push(&ring, rows, fft, 12, 10, 1, 0), -1);
  nr_csirs_disc_ring_read_unlock(&ring);
  EXPECT_EQ(__atomic_load_n(&csirs_disc_feed_drops, __ATOMIC_RELAXED), before + 1);
  EXPECT_EQ(ring.written, 0u);
  nr_csirs_disc_ring_free(&ring);
}
