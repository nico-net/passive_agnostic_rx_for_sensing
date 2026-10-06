/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>
#include <thread>
#include <atomic>

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

TEST(Ring, SnapshotCursorDoesNotHoldProducerLockDuringWork)
{
  nr_csirs_disc_ring_t live={},snapshot={};
  ASSERT_EQ(nr_csirs_disc_ring_init(&live,4,217,20,640,false),0);
  ASSERT_EQ(nr_csirs_disc_ring_init(&snapshot,4,217,20,640,false),0);
  std::vector<int16_t> samples(live.slot_stride,123);
  const int16_t *ants[4];for(int a=0;a<4;++a) ants[a]=samples.data()+a*14*24*217;
  for(unsigned slot=0;slot<100;++slot) ASSERT_GE(nr_csirs_disc_ring_push(&live,ants,12*217,0,slot,slot/20,slot%20),0);
  ASSERT_EQ(nr_csirs_disc_ring_snapshot(&live,&snapshot),0);
  const auto before=__atomic_load_n(&csirs_disc_feed_drops,__ATOMIC_RELAXED);
  // Model a worker stalled in a long sweep, holding only its private snapshot.
  ASSERT_TRUE(nr_csirs_disc_ring_read_lock(&snapshot));
  std::thread producer([&] {
    for(unsigned slot=100;slot<700;++slot) EXPECT_GE(nr_csirs_disc_ring_push(&live,ants,12*217,0,slot,slot/20,slot%20),0);
  });
  producer.join();
  EXPECT_EQ(snapshot.written,100u);
  EXPECT_EQ(snapshot.meta[0].abs_slot,0u);
  EXPECT_EQ(snapshot.grid_base[0],123);
  nr_csirs_disc_ring_read_unlock(&snapshot);
  EXPECT_EQ(__atomic_load_n(&csirs_disc_feed_drops,__ATOMIC_RELAXED),before);
  ASSERT_EQ(nr_csirs_disc_ring_snapshot(&live,&snapshot),0);
  EXPECT_EQ(snapshot.written,700u);
  for(unsigned w=60;w<700;++w) EXPECT_EQ(snapshot.meta[w%640].abs_slot,w);
  EXPECT_TRUE(nr_csirs_disc_ring_snapshot_current(&snapshot));
  nr_csirs_disc_ring_reset(&live);
  EXPECT_FALSE(nr_csirs_disc_ring_snapshot_current(&snapshot));
  ASSERT_EQ(nr_csirs_disc_ring_snapshot(&live,&snapshot),0);
  EXPECT_EQ(snapshot.written,0u);
  EXPECT_TRUE(nr_csirs_disc_ring_snapshot_current(&snapshot));
  for(int i=0;i<snapshot.capacity;++i) EXPECT_FALSE(snapshot.meta[i].valid);
  nr_csirs_disc_ring_free(&snapshot);nr_csirs_disc_ring_free(&live);
}

TEST(Ring, DeviceBatchPublicationWrapAndCancel)
{
  // Host stand-in for managed allocation tests the publication contract without CUDA.
  auto saved_alloc=nr_csirs_disc_alloc_unified;
  auto saved_free=nr_csirs_disc_free_unified;
  nr_csirs_disc_alloc_unified=malloc;nr_csirs_disc_free_unified=free;
  nr_csirs_disc_ring_t ring={},copy={};
  ASSERT_EQ(nr_csirs_disc_ring_init(&ring,4,3,20,3,true),0);
  ASSERT_EQ(nr_csirs_disc_ring_init(&copy,4,3,20,3,false),0);
  for(int pass=0;pass<3;++pass) {
    int first=nr_csirs_disc_ring_write_begin(&ring,2);
    ASSERT_EQ(first,(2*pass)%3);
    nr_csirs_disc_slot_meta_t meta[2];
    for(int i=0;i<2;++i) {
      meta[i]={(uint32_t)(2*pass+i),0,(uint8_t)(2*pass+i),1};
      std::fill_n(ring.grid_base+((first+i)%3)*ring.slot_stride,ring.slot_stride,2*pass+i+10);
    }
    EXPECT_EQ(ring.written,(uint64_t)2*pass); // Uncommitted device output is invisible.
    nr_csirs_disc_ring_write_end(&ring,2,meta);
    ASSERT_EQ(nr_csirs_disc_ring_snapshot(&ring,&copy),0);
    for(int i=0;i<2;++i) {
      EXPECT_EQ(copy.meta[(first+i)%3].abs_slot,(uint32_t)(2*pass+i));
      EXPECT_EQ(copy.grid_base[((first+i)%3)*copy.slot_stride],2*pass+i+10);
    }
  }
  int first=nr_csirs_disc_ring_write_begin(&ring,1);
  nr_csirs_disc_ring_write_end(&ring,1,nullptr);
  EXPECT_EQ(ring.written,6u);EXPECT_FALSE(ring.meta[first].valid);
  nr_csirs_disc_ring_reset(&ring);
  EXPECT_EQ(ring.written,0u);
  nr_csirs_disc_ring_free(&ring);nr_csirs_disc_ring_free(&copy);
  nr_csirs_disc_alloc_unified=saved_alloc;nr_csirs_disc_free_unified=saved_free;
}
