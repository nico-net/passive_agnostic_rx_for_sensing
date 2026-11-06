// The decoder-estimate copy equals the decoder buffer at the DM-RS symbol, the grant's subcarriers, every layer and RX.
#include "nr_pdsch_dec_est_copy.h"
#include <gtest/gtest.h>
#include <vector>

TEST(DecEstCopy, EqualsTheDecoderBufferAtTheGrantsSubcarriers)
{
  const int nl = 3, nant = 4, stride = 256, nsym = 14, m = 2, off = 24, nsc = 96;
  const size_t row = (size_t)stride * nsym + 16; /* one contiguous 2-D array like allocCast2D, rows padded */
  std::vector<int32_t> flat(row * nl * nant);
  std::vector<int32_t *> buf(nl * nant);
  for (int r = 0; r < nl * nant; r++) {
    buf[r] = &flat[row * r];
    for (size_t i = 0; i < row; i++) buf[r][i] = (int32_t)(r * 100003 + i * 7 + 1);
  }
  std::vector<int32_t> dst((size_t)nl * nant * nsc, -1);
  nr_dec_est_copy(dst.data(), flat.data(), row, nl, nant, m, stride, off, nsc);
  for (int l = 0; l < nl; l++)
    for (int a = 0; a < nant; a++)
      for (int j = 0; j < nsc; j++) ASSERT_EQ(dst[((size_t)l * nant + a) * nsc + j], buf[l * nant + a][(size_t)m * stride + off + j]);
  /* the copy is a snapshot: the decoder freeing / overwriting its buffer does not change it */
  const int32_t keep = dst[5];
  std::fill(flat.begin(), flat.end(), 0);
  EXPECT_EQ(dst[5], keep);
}
