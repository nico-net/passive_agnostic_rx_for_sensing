// Data-aided tap off the decode thread: the snapshot must hold every rxdataF element the tap reads, bit for bit, in the consumer's layout
// (the tap then produces the same CFR rows from the copy). Replays the tap's own index enumeration (nr_pdsch_data_aided.c: re = start_re + j, wrapped at fft)
// over contiguous grants that wrap the carrier edge, start at 0, fill the band and touch the last RB, and checks nothing outside the region is written.
#include <gtest/gtest.h>
#include <vector>
#include <random>
#include "nr_pdsch_da_snap.h"

namespace {
struct Case { int nant, fft, sym0, nsym, start_re, num_sc; };
void run(const Case &c)
{
  const size_t stride = (size_t)14 * c.fft + 3 * 7; /* samples_per_slot_wCP is a little more than 14 symbols of fft (CP lengths) */
  std::vector<uint32_t> src((size_t)c.nant * stride), dst(src.size(), 0xDEADBEEFu);
  std::mt19937 g(7);
  for (auto &v : src) v = g();
  ASSERT_EQ(0, nr_da_snap_copy(dst.data(), src.data(), c.nant, stride, c.fft, c.sym0, c.nsym, c.start_re, c.num_sc));
  std::vector<uint8_t> read(src.size(), 0);
  for (int a = 0; a < c.nant; a++)
    for (int l = c.sym0; l < c.sym0 + c.nsym; l++)
      for (int j = 0; j < c.num_sc; j++) {
        int re = c.start_re + j;
        if (re >= c.fft) re -= c.fft;
        const size_t o = (size_t)a * stride + (size_t)l * c.fft + re;
        read[o] = 1;
        ASSERT_EQ(src[o], dst[o]) << "a " << a << " l " << l << " j " << j;
      }
  for (size_t o = 0; o < dst.size(); o++)
    if (!read[o]) ASSERT_EQ(0xDEADBEEFu, dst[o]) << "wrote outside the read set at " << o;
}
}  // namespace

TEST(DaSnap, CoversTheTapReadSetAndNothingElse)
{
  run({4, 4096, 2, 12, 1000, 217 * 12});
  run({4, 4096, 2, 12, 4096 - 600, 100 * 12}); /* wraps the carrier edge: negative-frequency half first */
  run({4, 4096, 0, 14, 0, 217 * 12});
  run({4, 4096, 3, 1, 4096 - 12, 12});         /* one RB at the last carrier index */
  run({1, 2048, 1, 13, 2048 - 5, 2048});       /* the whole symbol row, starting inside it */
}

TEST(DaSnap, RefusesRegionsOutsideTheBuffers)
{
  std::vector<uint32_t> a(14 * 64), b(14 * 64);
  EXPECT_EQ(-1, nr_da_snap_copy(b.data(), a.data(), 1, a.size(), 64, 13, 2, 0, 12)); /* symbols past the plane */
  EXPECT_EQ(-1, nr_da_snap_copy(b.data(), a.data(), 1, a.size(), 64, 0, 1, 64, 12)); /* start_re >= fft */
  EXPECT_EQ(-1, nr_da_snap_copy(b.data(), a.data(), 1, a.size(), 64, 0, 1, 0, 65));  /* more subcarriers than the symbol */
  EXPECT_EQ(-1, nr_da_snap_copy(b.data(), a.data(), 0, a.size(), 64, 0, 1, 0, 12));
  EXPECT_EQ(0, nr_da_snap_copy(b.data(), a.data(), 1, a.size(), 64, 0, 14, 0, 64));
}
