#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdcch_dci01_layout_sweep.h"
}

static uint16_t riv_bits_for(uint16_t n_rb)
{
  return (uint16_t)std::ceil(std::log2((double)n_rb * (double)(n_rb + 1) / 2.0));
}

TEST(Dci01Layout, OffsetsFollowTheUplinkFieldOrder) {
  // TS 38.212 7.3.1.1.2 differs from 1_1: a UL/SUL indicator and a frequency-hopping flag sit
  // before the MCS. Getting that wrong shifts every later field, which is the whole failure mode
  // this module exists to prevent.
  nr_dci01_layout_t l{};
  l.pre_riv = 2; l.pre_mcs = 1; l.pre_ant = 12; l.ant_ports = 3; l.post_ant = 5;
  nr_dci11_offsets_t o{};
  ASSERT_TRUE(nr_dci01_layout_offsets(&l, 16, 2, &o));
  EXPECT_EQ(o.riv, 1 + 2);
  EXPECT_EQ(o.tda, o.riv + 16);
  EXPECT_EQ(o.mcs, o.tda + 2 + 1);       // + tda + frequency hopping flag
  EXPECT_EQ(o.rv, o.mcs + 5 + 1);
  EXPECT_EQ(o.ant_ports, o.rv + 2 + 12);
  EXPECT_EQ(o.ant_ports_bits, 3);
  EXPECT_EQ(o.total, o.ant_ports + 3 + 5 + 1);   // + ports + post_ant + UL-SCH indicator
}

TEST(Dci01Layout, RejectsImpossibleLayouts) {
  nr_dci01_layout_t l{};
  nr_dci11_offsets_t o{};
  l.pre_riv = 4; l.ant_ports = 3;  EXPECT_FALSE(nr_dci01_layout_offsets(&l, 16, 2, &o));
  l.pre_riv = 1; l.ant_ports = 6;  EXPECT_FALSE(nr_dci01_layout_offsets(&l, 16, 2, &o));
  l.ant_ports = 1;                 EXPECT_FALSE(nr_dci01_layout_offsets(&l, 16, 2, &o));
  l.ant_ports = 3; l.pre_mcs = 2;  EXPECT_FALSE(nr_dci01_layout_offsets(&l, 16, 2, &o));
}

TEST(Dci01Layout, TheLengthConstraintPrunesTheLargerUplinkSpace) {
  // DCI 0_1's switch space is bigger than 1_1's -- SRI, precoding and CSI request are each up to
  // seven values -- so the constraint matters more here, not less.
  const uint16_t rb = riv_bits_for(273);
  std::vector<nr_dci01_layout_t> c(NR_DCI11_LAYOUT_MAX);
  std::vector<nr_dci11_offsets_t> o(NR_DCI11_LAYOUT_MAX);
  int across = 0;
  for (uint16_t len = 30; len <= 80; len++) {
    const int n = nr_dci01_layout_enumerate(rb, 2, len, c.data(), o.data(), NR_DCI11_LAYOUT_MAX);
    if (n > 0) across += n;
  }
  const int one = nr_dci01_layout_enumerate(rb, 2, 50, c.data(), o.data(), NR_DCI11_LAYOUT_MAX);
  ASSERT_GT(one, 0);
  EXPECT_GT(across, one * 4);
  for (int i = 0; i < one; i++) EXPECT_EQ(o[i].total, 50) << "candidate " << i;
  std::cerr << "[ MEASURED ] DCI 0_1: " << across << " layouts across lengths 30..80, "
            << one << " at the observed length (" << (double)across / one << "x)\n";
}

TEST(Dci01Layout, EveryCandidateIsDistinct) {
  const uint16_t rb = riv_bits_for(273);
  std::vector<nr_dci01_layout_t> c(NR_DCI11_LAYOUT_MAX);
  const int n = nr_dci01_layout_enumerate(rb, 2, 50, c.data(), nullptr, NR_DCI11_LAYOUT_MAX);
  ASSERT_GT(n, 1);
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      EXPECT_FALSE(c[i].pre_riv == c[j].pre_riv && c[i].pre_mcs == c[j].pre_mcs
                   && c[i].pre_ant == c[j].pre_ant && c[i].ant_ports == c[j].ant_ports
                   && c[i].post_ant == c[j].post_ant)
          << "duplicate would win a round-robin by appearing more often";
}

TEST(Dci01Layout, PtrsAssociationOnlyWithAWideEnoughPortField) {
  // A PT-RS/DM-RS association field exists only when uplink PT-RS does, which needs transform
  // precoding off -- the same condition that admits the wider antenna-port tables. Allowing the
  // pair otherwise invents layouts no RRC can produce, and every invented layout is one more
  // hypothesis the TB CRC has to spend grants rejecting.
  const uint16_t rb = riv_bits_for(273);
  std::vector<nr_dci01_layout_t> c(NR_DCI11_LAYOUT_MAX);
  int checked = 0;
  for (uint16_t len = 30; len <= 80; len++) {
    const int n = nr_dci01_layout_enumerate(rb, 2, len, c.data(), nullptr, NR_DCI11_LAYOUT_MAX);
    for (int i = 0; i < n; i++) { EXPECT_GE(c[i].ant_ports, 2); checked++; }
  }
  EXPECT_GT(checked, 0);
}

TEST(Dci01Resolver, TheSharedResolverScoresUplinkLayouts) {
  // The resolver only ever reads offsets, so it is format-agnostic. This is the property that let
  // DCI 0_1 reuse ~200 lines of pruning and Wilson scoring rather than duplicate them.
  const uint16_t rb = riv_bits_for(273);
  std::vector<nr_dci01_layout_t> c(NR_DCI11_LAYOUT_MAX);
  std::vector<nr_dci11_offsets_t> o(NR_DCI11_LAYOUT_MAX);
  const int n = nr_dci01_layout_enumerate(rb, 2, 50, c.data(), o.data(), NR_DCI11_LAYOUT_MAX);
  ASSERT_GT(n, 1);
  nr_dci11_resolver_t r{};
  ASSERT_EQ(nr_dci_resolver_init_from_offsets(&r, 273, o.data(), n), n);

  const int truth = n / 3;
  unsigned seed = 17;
  for (int i = 0; i < 800; i++) {
    uint64_t p = 0;
    const nr_dci11_offsets_t &t = o[truth];
    for (int b = 0; b < t.total; b++) p |= (uint64_t)(rand_r(&seed) & 1) << b;
    p &= ~(((1ULL << rb) - 1ULL) << (t.total - t.riv - rb));
    p &= ~(31ULL << (t.total - t.mcs - 5));
    p |= (uint64_t)(rand_r(&seed) % (273 * 274 / 2)) << (t.total - t.riv - rb);
    p |= (uint64_t)(rand_r(&seed) % 28) << (t.total - t.mcs - 5);
    nr_dci11_resolver_observe(&r, p);
  }
  EXPECT_LT(r.n_alive, n) << "stage 1 pruned nothing on the uplink format";
  EXPECT_GE(r.n_alive, 1);
  EXPECT_TRUE(r.alive[truth]) << "stage 1 deleted the layout the payloads were built from";

  int w = -1;
  for (int i = 0; i < 60000 && w < 0; i++) {
    nr_dci11_offsets_t pick{};
    const int idx = nr_dci11_resolver_next(&r, &pick);
    ASSERT_GE(idx, 0);
    const double u = (double)rand_r(&seed) / (double)RAND_MAX;
    w = nr_dci11_resolver_feed(&r, idx, idx == truth && u < 0.40);
  }
  if (w != truth) {
    int best = -1;
    for (int i = 0; i < r.n_hyp; i++) {
      if (!r.alive[i]) continue;
      if (best < 0 || (r.trials[i] && (double)r.ok[i] / r.trials[i]
                                          > (double)r.ok[best] / (r.trials[best] ? r.trials[best] : 1)))
        best = i;
    }
    int zero_trial = 0, min_tr = 1 << 30;
    for (int i = 0; i < r.n_hyp; i++) {
      if (!r.alive[i]) continue;
      if (r.trials[i] == 0) zero_trial++;
      if ((int)r.trials[i] < min_tr) min_tr = r.trials[i];
    }
    std::cerr << "[ DEBUG ] truth=" << truth << " alive=" << r.alive[truth]
              << " trials=" << r.trials[truth] << " ok=" << r.ok[truth]
              << " | best=" << best << " trials=" << (best >= 0 ? r.trials[best] : 0)
              << " ok=" << (best >= 0 ? r.ok[best] : 0)
              << " | alive=" << r.n_alive << " with_zero_trials=" << zero_trial
              << " min_trials=" << min_tr << "\n";
  }
  EXPECT_EQ(w, truth);
  std::cerr << "[ MEASURED ] DCI 0_1 resolver: " << n << " -> " << r.n_alive
            << " after stage 1, converged on " << w << "\n";
}

TEST(Dci01Resolver, RejectsAnInconsistentOffsetsList) {
  // Mixed payload sizes would make stage 1 read past the end of some candidates.
  nr_dci11_offsets_t bad[2]{};
  bad[0].total = 47; bad[1].total = 50;
  nr_dci11_resolver_t r{};
  EXPECT_EQ(nr_dci_resolver_init_from_offsets(&r, 273, bad, 2), 0);
  nr_dci11_offsets_t zero[1]{};
  EXPECT_EQ(nr_dci_resolver_init_from_offsets(&r, 273, zero, 1), 0);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
