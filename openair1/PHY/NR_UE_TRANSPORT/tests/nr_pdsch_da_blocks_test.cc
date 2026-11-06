// Partial-TB data-aided CFR: which REs do the passing code blocks pin down (TS 38.212 5.4.2.1 / 5.5, TS 38.211 7.3.1.3).
#include "nr_pdsch_da_blocks.h"
#include <gtest/gtest.h>
#include <vector>

struct Shape { uint32_t C, Qm, Nl, n_re; };
static std::vector<uint32_t> Es(const Shape &s)
{
  std::vector<uint32_t> E(s.C);
  const uint32_t G = s.n_re * s.Nl * s.Qm;
  for (uint32_t r = 0; r < s.C; r++) E[r] = nr_da_block_E(G, s.C, s.Qm, s.Nl, r);
  return E;
}
/* reference: a bit-by-bit walk, independent of the run-length logic */
static std::vector<uint8_t> ref_valid(const Shape &s, const std::vector<uint32_t> &E, const nr_da_cb_pass_t &p)
{
  std::vector<int> blk;
  for (uint32_t r = 0; r < s.C; r++) blk.insert(blk.end(), E[r], (int)r);
  std::vector<uint8_t> v(s.n_re, 1);
  for (uint32_t i = 0; i < s.n_re; i++)
    for (uint32_t b = i * s.Nl * s.Qm; b < (i + 1) * s.Nl * s.Qm; b++)
      if (b >= blk.size() || !nr_da_cb_get(&p, (uint32_t)blk[b])) v[i] = 0;
  return v;
}
static nr_da_cb_pass_t all_pass(uint32_t C)
{
  nr_da_cb_pass_t p; memset(&p, 0, sizeof p); p.C = (uint16_t)C; p.ok = (uint16_t)C;
  for (uint32_t r = 0; r < C; r++) nr_da_cb_set(&p, r, true);
  return p;
}

TEST(DaBlocks, ESumsToGAndIsMultipleOfNlQm)
{
  for (uint32_t Nl = 1; Nl <= 4; Nl++)
    for (uint32_t Qm : {2u, 4u, 6u, 8u})
      for (uint32_t C : {2u, 5u, 17u, 100u}) {
        Shape s{C, Qm, Nl, 1000 + 37 * C};
        uint64_t sum = 0;
        for (uint32_t E : Es(s)) { EXPECT_EQ(E % (Nl * Qm), 0u); sum += E; }
        EXPECT_EQ(sum, (uint64_t)s.n_re * Nl * Qm);
      }
}

TEST(DaBlocks, WholeTbPassedKeepsEveryRe)
{
  for (uint32_t Nl = 1; Nl <= 4; Nl++)
    for (uint32_t Qm : {2u, 4u, 6u, 8u}) {
      Shape s{7, Qm, Nl, 3333};
      auto E = Es(s); auto p = all_pass(s.C);
      std::vector<uint8_t> v(s.n_re);
      EXPECT_EQ(nr_da_re_valid(E.data(), &p, Qm, Nl, s.n_re, v.data()), s.n_re);
    }
}

TEST(DaBlocks, OneFailedBlockInvalidatesExactlyItsRes)
{
  for (uint32_t Nl : {1u, 2u, 3u, 4u})
    for (uint32_t Qm : {2u, 4u, 6u, 8u})
      for (uint32_t C : {2u, 6u, 90u}) { /* 90 blocks: beyond the 64-bit EVMGRANT map */
        Shape s{C, Qm, Nl, 5000 + 11 * C};
        auto E = Es(s);
        for (uint32_t k : {0u, C / 2, C - 1}) {
          auto p = all_pass(C); nr_da_cb_set(&p, k, false); p.ok = (uint16_t)(C - 1);
          std::vector<uint8_t> v(s.n_re);
          const uint32_t n = nr_da_re_valid(E.data(), &p, Qm, Nl, s.n_re, v.data());
          uint32_t S = 0;
          for (uint32_t r = 0; r < k; r++) S += E[r];
          const uint32_t lo = S / (Nl * Qm), hi = (S + E[k]) / (Nl * Qm); /* E is a multiple of Nl*Qm: whole REs */
          for (uint32_t i = 0; i < s.n_re; i++) EXPECT_EQ(v[i], (i < lo || i >= hi) ? 1 : 0) << "Nl " << Nl << " Qm " << Qm << " C " << C << " k " << k << " re " << i;
          EXPECT_EQ(n, s.n_re - (hi - lo));
          EXPECT_EQ(std::vector<uint8_t>(v), ref_valid(s, E, p));
        }
      }
}

TEST(DaBlocks, UntriedAndSeveralFailedBlocksMatchTheBitReference)
{
  for (uint32_t Nl : {1u, 2u, 4u})
    for (uint32_t Qm : {2u, 4u, 6u, 8u}) {
      Shape s{13, Qm, Nl, 4001};
      auto E = Es(s);
      nr_da_cb_pass_t p; memset(&p, 0, sizeof p); p.C = 13;
      for (uint32_t r : {0u, 1u, 4u, 5u}) { nr_da_cb_set(&p, r, true); p.ok++; } /* the rest: failed or untried (the NR_CB_FAIL_LIMIT stop) */
      std::vector<uint8_t> v(s.n_re);
      const uint32_t n = nr_da_re_valid(E.data(), &p, Qm, Nl, s.n_re, v.data());
      EXPECT_EQ(v, ref_valid(s, E, p));
      uint32_t cnt = 0; for (uint8_t b : v) cnt += b;
      EXPECT_EQ(n, cnt);
      EXPECT_GT(n, 0u); EXPECT_LT(n, s.n_re);
    }
}

TEST(DaBlocks, SpanStraddlingABlockBoundaryNeedsBothBlocks)
{
  /* E not a multiple of Nl*Qm (never produced by 5.4.2.1, but the walk must not rely on it): blocks 0 = 7 bits, 1 = 9 bits; RE = 4 bits, so RE 1 = bits 4..7 touches both */
  uint32_t E[2] = {7, 9};
  nr_da_cb_pass_t p; memset(&p, 0, sizeof p); p.C = 2;
  nr_da_cb_set(&p, 0, true);
  uint8_t v[5];
  EXPECT_EQ(nr_da_re_valid(E, &p, 2, 2, 4, v), 1u); /* RE0 (bits 0-3) in block 0 only */
  EXPECT_EQ(v[0], 1); EXPECT_EQ(v[1], 0); EXPECT_EQ(v[2], 0);
  nr_da_cb_set(&p, 1, true);
  EXPECT_EQ(nr_da_re_valid(E, &p, 2, 2, 4, v), 4u);
  EXPECT_EQ(nr_da_re_valid(E, &p, 2, 2, 5, v), 4u); /* RE 4 = bits 16..19: past the codeword */
  EXPECT_EQ(v[4], 0);
}

TEST(DaBlocks, NoBlockPassedMeansNoRe)
{
  Shape s{4, 6, 2, 900}; auto E = Es(s);
  nr_da_cb_pass_t p; memset(&p, 0, sizeof p); p.C = 4;
  std::vector<uint8_t> v(s.n_re);
  EXPECT_EQ(nr_da_re_valid(E.data(), &p, 6, 2, s.n_re, v.data()), 0u);
}
