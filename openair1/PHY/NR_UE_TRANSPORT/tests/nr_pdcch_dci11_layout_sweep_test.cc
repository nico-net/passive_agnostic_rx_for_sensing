#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdcch_dci11_layout_sweep.h"
}

// RIV width for a BWP, as nr_pdcch_blind_dci_size() computes it.
static uint16_t riv_bits_for(uint16_t n_rb)
{
  return (uint16_t)std::ceil(std::log2((double)n_rb * (double)(n_rb + 1) / 2.0));
}

TEST(Dci11Layout, OffsetsFollowTheSpecFieldOrder) {
  // Fields must come out in TS 38.212 7.3.1.2.2 order and be contiguous.
  nr_dci11_layout_t l{};
  l.bwp_ind = 1; l.pre_mcs = 2; l.pre_ant = 11; l.ant_ports = 4; l.post_ant = 5;
  nr_dci11_offsets_t o{};
  ASSERT_TRUE(nr_dci11_layout_offsets(&l, 16, 2, &o));
  EXPECT_EQ(o.riv, 1 + 1);             // identifier + bwp indicator
  EXPECT_EQ(o.tda, o.riv + 16);
  EXPECT_EQ(o.mcs, o.tda + 2 + 2);     // + tda_bits + pre_mcs
  EXPECT_EQ(o.rv,  o.mcs + 5 + 1);     // + mcs + ndi
  EXPECT_EQ(o.ant_ports, o.rv + 2 + 11);
  EXPECT_EQ(o.dmrs_init, o.ant_ports + 4 + 5);
  EXPECT_EQ(o.total, o.dmrs_init + 1);
}

TEST(Dci11Layout, RejectsImpossibleLayouts) {
  nr_dci11_layout_t l{};
  l.bwp_ind = 3; l.ant_ports = 4;                       // n_dl_bwp gives at most 2 bits
  nr_dci11_offsets_t o{};
  EXPECT_FALSE(nr_dci11_layout_offsets(&l, 16, 2, &o));
  l.bwp_ind = 1; l.ant_ports = 7;                       // antenna ports is 4..6
  EXPECT_FALSE(nr_dci11_layout_offsets(&l, 16, 2, &o));
  EXPECT_FALSE(nr_dci11_layout_offsets(&l, 0, 2, &o));  // a zero-width RIV is not a BWP
}

TEST(Dci11Layout, TheLabCellsKnownGoodLayoutIsEnumerated) {
  // 273 PRB, 47-bit payload, 3-entry TDRA list -> tda = ceil(log2(3)) = 2. This is the
  // configuration the hand-written pdcch_blind_monitor_bwp "0:273:0:47" encodes and that
  // decodes at 76-92 % on this rig, so the sweep MUST be able to reach it.
  const uint16_t rb = riv_bits_for(273);
  std::vector<nr_dci11_layout_t> cands(NR_DCI11_LAYOUT_MAX);
  const int n = nr_dci11_layout_enumerate(rb, 2, 47, cands.data(), NR_DCI11_LAYOUT_MAX);
  ASSERT_GT(n, 0);
  bool total_ok = true;
  for (int i = 0; i < n; i++) {
    nr_dci11_offsets_t o{};
    ASSERT_TRUE(nr_dci11_layout_offsets(&cands[i], rb, 2, &o));
    if (o.total != 47) total_ok = false;
  }
  EXPECT_TRUE(total_ok) << "a candidate does not sum to the observed length";
  std::cerr << "[ MEASURED ] riv_bits=" << rb << " len=47 -> " << n
            << " distinct layouts survive the length constraint\n";
}

TEST(Dci11Layout, TheLengthConstraintIsWhatMakesThisTractable) {
  // Without the observed length the switch space is thousands of combinations; with it, a list.
  const uint16_t rb = riv_bits_for(273);
  std::vector<nr_dci11_layout_t> c(NR_DCI11_LAYOUT_MAX);
  int total = 0;
  for (uint16_t len = 30; len <= 70; len++) {
    const int n = nr_dci11_layout_enumerate(rb, 2, len, c.data(), NR_DCI11_LAYOUT_MAX);
    if (n > 0) total += n;
  }
  const int one = nr_dci11_layout_enumerate(rb, 2, 47, c.data(), NR_DCI11_LAYOUT_MAX);
  EXPECT_GT(total, one * 4) << "the length constraint is not pruning";
  std::cerr << "[ MEASURED ] layouts across lengths 30..70 = " << total
            << ", at the ONE observed length = " << one
            << "  (" << (double)total / (double)one << "x pruning)\n";
}

TEST(Dci11Layout, EveryCandidateIsADistinctLayout) {
  const uint16_t rb = riv_bits_for(106);
  std::vector<nr_dci11_layout_t> c(NR_DCI11_LAYOUT_MAX);
  const int n = nr_dci11_layout_enumerate(rb, 2, 45, c.data(), NR_DCI11_LAYOUT_MAX);
  ASSERT_GT(n, 1);
  for (int i = 0; i < n; i++) {
    for (int j = i + 1; j < n; j++) {
      const bool same = c[i].bwp_ind == c[j].bwp_ind && c[i].pre_mcs == c[j].pre_mcs
                        && c[i].pre_ant == c[j].pre_ant && c[i].ant_ports == c[j].ant_ports
                        && c[i].post_ant == c[j].post_ant;
      EXPECT_FALSE(same) << "duplicate layout at " << i << "," << j
                         << " would win a round-robin by appearing more often";
    }
  }
}

TEST(Dci11Layout, PlausibilityRejectsAReservedMcs) {
  nr_dci11_layout_t l{};
  l.bwp_ind = 0; l.pre_mcs = 0; l.pre_ant = 11; l.ant_ports = 4; l.post_ant = 4;
  nr_dci11_offsets_t o{};
  ASSERT_TRUE(nr_dci11_layout_offsets(&l, 16, 2, &o));
  // Build a payload with MCS = 30 (a reserved retransmission row) and a legal RIV.
  uint64_t p = 0;
  const int mcs_shift = o.total - o.mcs - 5;
  p |= (uint64_t)30 << mcs_shift;
  EXPECT_FALSE(nr_dci11_layout_plausible(&o, p, 273));
  // Same payload, MCS 10 -> plausible.
  p = 0;
  p |= (uint64_t)10 << mcs_shift;
  EXPECT_TRUE(nr_dci11_layout_plausible(&o, p, 273));
}

TEST(Dci11Layout, PlausibilityRejectsAnOutOfRangeRiv) {
  nr_dci11_layout_t l{};
  l.bwp_ind = 0; l.pre_mcs = 0; l.pre_ant = 11; l.ant_ports = 4; l.post_ant = 4;
  nr_dci11_offsets_t o{};
  ASSERT_TRUE(nr_dci11_layout_offsets(&l, 16, 2, &o));
  // 1000 is a legal RIV in a 273-PRB BWP (max 37401) but not in a 24-PRB one (max 300).
  uint64_t p = 0;
  p |= (uint64_t)1000 << (o.total - o.riv - 16);
  p |= (uint64_t)10 << (o.total - o.mcs - 5);
  EXPECT_FALSE(nr_dci11_layout_plausible(&o, p, 24));
  EXPECT_TRUE(nr_dci11_layout_plausible(&o, p, 273));
}

TEST(Dci11Layout, AWrongLayoutIsRejectedMoreOftenThanARightOne) {
  // The confusion the sweep actually faces is between layouts of the SAME total length -- the
  // length is already derived, so every candidate agrees on it and differs only internally.
  // (An earlier version of this test compared layouts of DIFFERENT lengths against one payload,
  // which is meaningless: a payload is a fixed bit string of a known size.)
  const uint16_t rb = riv_bits_for(273);
  std::vector<nr_dci11_layout_t> c(NR_DCI11_LAYOUT_MAX);
  const int n = nr_dci11_layout_enumerate(rb, 2, 47, c.data(), NR_DCI11_LAYOUT_MAX);
  ASSERT_GT(n, 1);

  // Take the first candidate as the truth and score every other candidate against payloads a gNB
  // would emit under it.
  nr_dci11_offsets_t t_off{};
  ASSERT_TRUE(nr_dci11_layout_offsets(&c[0], rb, 2, &t_off));

  unsigned seed = 11;
  const int N = 3000;
  std::vector<uint64_t> payloads(N);
  for (int i = 0; i < N; i++) {
    // START from a fully random payload. An earlier version zeroed every bit it did not set, which
    // is not what a gNB emits -- HARQ pid, DAI, TPC and the PUCCH resource indicator all carry
    // real values. Those zeros made a rival reading a SHIFTED MCS offset see 0 and pass, flattering
    // stage 1's rivals and hiding the blind spot this test exists to pin.
    uint64_t p = 0;
    for (int b = 0; b < t_off.total; b++) {
      p |= (uint64_t)(rand_r(&seed) & 1) << b;
    }
    // then overwrite the fields a real DCI constrains
    const uint64_t riv_mask = ((1ULL << rb) - 1ULL) << (t_off.total - t_off.riv - rb);
    const uint64_t mcs_mask = 31ULL << (t_off.total - t_off.mcs - 5);
    const uint64_t ap_mask =
        ((1ULL << c[0].ant_ports) - 1ULL) << (t_off.total - t_off.ant_ports - c[0].ant_ports);
    p &= ~(riv_mask | mcs_mask | ap_mask);
    p |= (uint64_t)(rand_r(&seed) % (273 * 274 / 2)) << (t_off.total - t_off.riv - rb);
    p |= (uint64_t)(rand_r(&seed) % 28) << (t_off.total - t_off.mcs - 5);
    p |= (uint64_t)(rand_r(&seed) % 12) << (t_off.total - t_off.ant_ports - c[0].ant_ports);
    payloads[i] = p;
  }

  int t_ok = 0;
  for (int i = 0; i < N; i++) if (nr_dci11_layout_plausible(&t_off, payloads[i], 273)) t_ok++;
  EXPECT_EQ(t_ok, N) << "the true layout must never be rejected by stage 1";

  // How far does stage 1 get, and what is it STRUCTURALLY unable to do?
  //
  // It reads RIV, MCS and antenna ports, so it is blind by construction to layouts that differ
  // only AFTER the antenna-ports field -- post_ant (TCI | SRS | CBG | flush) moves the DM-RS-init
  // offset and nothing stage 1 inspects. Those are not equivalent layouts (DM-RS init selects
  // nSCID, so decoding them wrong still fails), they are simply stage 2's job. This test pins both
  // the pruning that works AND the blind spot, so neither is mistaken for the other later.
  int survivors = 0, rivals = 0;
  double sum_rival = 0.0;
  for (int j = 1; j < n; j++) {
    nr_dci11_offsets_t w{};
    ASSERT_TRUE(nr_dci11_layout_offsets(&c[j], rb, 2, &w));
    int ok = 0;
    for (int i = 0; i < N; i++) if (nr_dci11_layout_plausible(&w, payloads[i], 273)) ok++;
    if (ok >= t_ok) survivors++;             // indistinguishable from the truth at stage 1
    sum_rival += 100.0 * ok / N;
    rivals++;
  }
  const double mean_rival = sum_rival / rivals;
  std::cerr << "[ MEASURED ] stage-1 over " << n << " same-length layouts: truth "
            << (100.0 * t_ok / N) << "%, mean rival " << mean_rival << "%, "
            << survivors << " of " << rivals << " rivals survive to stage 2\n";

  EXPECT_LT(mean_rival, 95.0) << "stage 1 is not pruning meaningfully";
  EXPECT_LT(survivors, rivals) << "stage 1 rejected nothing at all";
  // WHAT STAGE 1 CAN AND CANNOT DO, pinned so the boundary is not rediscovered later.
  //
  // Always active: the RIV range check and the reserved-MCS check. So no survivor may place those
  // two fields anywhere but where the truth does -- if one did, the oracle would have a real hole.
  //
  // Structurally blind, by construction and on purpose:
  //   (a) layouts differing only AFTER the antenna-ports field (post_ant = TCI|SRS|CBG|flush).
  //       Nothing stage 1 reads lives there. They are not equivalent layouts -- post_ant moves the
  //       DM-RS-init offset and nSCID still decodes wrong -- they are stage 2's job.
  //   (b) layouts whose antenna-ports width is 5 or 6. The 12-of-16 row check is applied ONLY at
  //       width 4, the one row count verified in-tree (g_table_7_3_2_3_3_1). Guessing the wider
  //       tables' row counts would invent rejections. Extending this is real future work and would
  //       tighten stage 1 further.
  for (int j = 1; j < n; j++) {
    nr_dci11_offsets_t w{};
    ASSERT_TRUE(nr_dci11_layout_offsets(&c[j], rb, 2, &w));
    int ok = 0;
    for (int i = 0; i < N; i++) if (nr_dci11_layout_plausible(&w, payloads[i], 273)) ok++;
    if (ok < t_ok) {
      continue;   // stage 1 rejected it, nothing to prove
    }
    EXPECT_EQ(w.riv, t_off.riv) << "a layout with a DIFFERENT RIV offset survived stage 1";
    EXPECT_EQ(w.mcs, t_off.mcs) << "a layout with a DIFFERENT MCS offset survived stage 1";
    const bool blind_a = (w.ant_ports == t_off.ant_ports);   // differs only after the ports field
    const bool blind_b = (c[j].ant_ports != 4);              // ports check not applied at 5/6
    EXPECT_TRUE(blind_a || blind_b)
        << "a survivor is outside both documented blind spots -- the oracle has a hole";
  }
}

// ---- stateful resolver -----------------------------------------------------------------------

// Drive the resolver end to end against a known truth: stage-1 payloads, then stage-2 TB outcomes
// where only the true layout decodes. Returns the winner index, or -1.
static int drive_resolver(nr_dci11_resolver_t &r, int truth, double p_true, int n_obs, int n_dec,
                          uint16_t riv_bits, unsigned seed = 5)
{
  // stage 1: payloads a gNB would emit under the truth
  for (int i = 0; i < n_obs; i++) {
    uint64_t p = 0;
    for (int b = 0; b < r.off[truth].total; b++) p |= (uint64_t)(rand_r(&seed) & 1) << b;
    const nr_dci11_offsets_t &t = r.off[truth];
    const uint64_t riv_mask = ((1ULL << riv_bits) - 1ULL) << (t.total - t.riv - riv_bits);
    const uint64_t mcs_mask = 31ULL << (t.total - t.mcs - 5);
    const uint64_t ap_mask = ((1ULL << t.ant_ports_bits) - 1ULL)
                             << (t.total - t.ant_ports - t.ant_ports_bits);
    p &= ~(riv_mask | mcs_mask | ap_mask);
    p |= (uint64_t)(rand_r(&seed) % (273 * 274 / 2)) << (t.total - t.riv - riv_bits);
    p |= (uint64_t)(rand_r(&seed) % 28) << (t.total - t.mcs - 5);
    p |= (uint64_t)(rand_r(&seed) % 12) << (t.total - t.ant_ports - t.ant_ports_bits);
    nr_dci11_resolver_observe(&r, p);
  }
  // stage 2: only the truth decodes
  for (int i = 0; i < n_dec; i++) {
    nr_dci11_offsets_t o{};
    const int idx = nr_dci11_resolver_next(&r, &o);
    if (idx < 0) return -1;
    const double u = (double)rand_r(&seed) / (double)RAND_MAX;
    const int w = nr_dci11_resolver_feed(&r, idx, idx == truth && u < p_true);
    if (w >= 0) return w;
  }
  return nr_dci11_resolver_winner(&r);
}

TEST(Dci11Resolver, InitRefusesWhenNoLayoutFitsTheLength) {
  nr_dci11_resolver_t r{};
  // 9 bits cannot hold a 16-bit RIV plus the fixed fields -- no layout can sum to it. That is a
  // real signal (riv_bits / tda_bits / observed_len disagree), not a resolver failure.
  EXPECT_EQ(nr_dci11_resolver_init(&r, 273, 16, 2, 9), 0);
  nr_dci11_offsets_t o{};
  EXPECT_EQ(nr_dci11_resolver_next(&r, &o), -1);
}

TEST(Dci11Resolver, Stage1NarrowsButNeverEmptiesTheSet) {
  nr_dci11_resolver_t r{};
  const uint16_t rb = riv_bits_for(273);
  const int n = nr_dci11_resolver_init(&r, 273, rb, 2, 47);
  ASSERT_GT(n, 1);
  unsigned seed = 3;
  // Feed payloads consistent with candidate 0.
  for (int i = 0; i < 600; i++) {
    uint64_t p = 0;
    for (int b = 0; b < r.off[0].total; b++) p |= (uint64_t)(rand_r(&seed) & 1) << b;
    const nr_dci11_offsets_t &t = r.off[0];
    p &= ~(((1ULL << rb) - 1ULL) << (t.total - t.riv - rb));
    p &= ~(31ULL << (t.total - t.mcs - 5));
    // ALSO constrain the antenna-ports codepoint. Leaving it random made this payload stream
    // inconsistent with candidate 0 itself, and stage 1 correctly deleted the very layout the test
    // claimed to be emulating -- a test bug that looked exactly like a resolver bug.
    p &= ~(((1ULL << t.ant_ports_bits) - 1ULL) << (t.total - t.ant_ports - t.ant_ports_bits));
    p |= (uint64_t)(rand_r(&seed) % (273 * 274 / 2)) << (t.total - t.riv - rb);
    p |= (uint64_t)(rand_r(&seed) % 28) << (t.total - t.mcs - 5);
    p |= (uint64_t)(rand_r(&seed) % 12) << (t.total - t.ant_ports - t.ant_ports_bits);
    nr_dci11_resolver_observe(&r, p);
  }
  EXPECT_LT(r.n_alive, n) << "stage 1 pruned nothing";
  EXPECT_GE(r.n_alive, 1) << "stage 1 emptied the set -- it can then never converge";
  EXPECT_TRUE(r.alive[0]) << "stage 1 dropped the layout the payloads were built from";
  std::cerr << "[ MEASURED ] resolver stage 1: " << n << " -> " << r.n_alive << " live\n";
}

TEST(Dci11Resolver, ARetransmittingCellDoesNotDeleteTheTruth) {
  // MCS 28-31 are reserved retransmission rows. On a cell that retransmits, the TRUE layout emits
  // them legitimately and the reserved-MCS test flags it. If stage 1 treated that as an invariant
  // it would delete the answer on exactly the cells that exercise HARQ.
  nr_dci11_resolver_t r{};
  const uint16_t rb = riv_bits_for(273);
  const int n = nr_dci11_resolver_init(&r, 273, rb, 2, 47);
  ASSERT_GT(n, 1);
  const int truth = 3;
  unsigned seed = 21;
  for (int i = 0; i < 1200; i++) {
    uint64_t p = 0;
    for (int b = 0; b < r.off[truth].total; b++) p |= (uint64_t)(rand_r(&seed) & 1) << b;
    const nr_dci11_offsets_t &t = r.off[truth];
    p &= ~(((1ULL << rb) - 1ULL) << (t.total - t.riv - rb));
    p &= ~(31ULL << (t.total - t.mcs - 5));
    p &= ~(((1ULL << t.ant_ports_bits) - 1ULL) << (t.total - t.ant_ports - t.ant_ports_bits));
    // 20 % of grants are retransmissions carrying a reserved MCS row
    const uint32_t mcs = (rand_r(&seed) % 100 < 20) ? (28 + rand_r(&seed) % 4)
                                                    : (uint32_t)(rand_r(&seed) % 28);
    p |= (uint64_t)(rand_r(&seed) % (273 * 274 / 2)) << (t.total - t.riv - rb);
    p |= (uint64_t)mcs << (t.total - t.mcs - 5);
    p |= (uint64_t)(rand_r(&seed) % 12) << (t.total - t.ant_ports - t.ant_ports_bits);
    nr_dci11_resolver_observe(&r, p);
  }
  EXPECT_TRUE(r.alive[truth]) << "a 20 % retransmission rate deleted the true layout";
  std::cerr << "[ MEASURED ] with 20% retransmissions: " << n << " -> " << r.n_alive
            << " live, truth survived\n";
}

TEST(Dci11Resolver, FindsTheTruthEndToEnd) {
  nr_dci11_resolver_t r{};
  const uint16_t rb = riv_bits_for(273);
  const int n = nr_dci11_resolver_init(&r, 273, rb, 2, 47);
  ASSERT_GT(n, 1);
  const int truth = n / 2;
  const int w = drive_resolver(r, truth, 0.40, 800, 40000, rb);
  EXPECT_EQ(w, truth);
  std::cerr << "[ MEASURED ] resolver converged on candidate " << w << " of " << n << "\n";
}

TEST(Dci11Resolver, DoesNotConvergeWhenNothingDecodes) {
  // A dead link must not let elimination promote a layout that never decoded anything.
  nr_dci11_resolver_t r{};
  const uint16_t rb = riv_bits_for(273);
  ASSERT_GT(nr_dci11_resolver_init(&r, 273, rb, 2, 47), 1);
  unsigned seed = 9;
  for (int i = 0; i < 20000; i++) {
    nr_dci11_offsets_t o{};
    const int idx = nr_dci11_resolver_next(&r, &o);
    ASSERT_GE(idx, 0);
    nr_dci11_resolver_feed(&r, idx, false);
  }
  EXPECT_EQ(nr_dci11_resolver_winner(&r), -1);
  (void)seed;
}

TEST(Dci11Resolver, PinsTheWinnerOnceDecided) {
  nr_dci11_resolver_t r{};
  const uint16_t rb = riv_bits_for(273);
  const int n = nr_dci11_resolver_init(&r, 273, rb, 2, 47);
  ASSERT_GT(n, 1);
  const int truth = 0;
  const int w = drive_resolver(r, truth, 0.60, 800, 40000, rb);
  ASSERT_EQ(w, truth);
  for (int i = 0; i < 50; i++) {
    nr_dci11_offsets_t o{};
    EXPECT_EQ(nr_dci11_resolver_next(&r, &o), truth);
    EXPECT_EQ(nr_dci11_resolver_feed(&r, truth, false), truth) << "a settled winner must not move";
  }
}

// ---- handover to the extractor ----------------------------------------------------------------

TEST(Dci11Handover, EveryEnumeratedLayoutRoundTripsToFieldBits) {
  // The mapping carries group TOTALS on one member each, so it is exactly the kind of arithmetic
  // that shifts a field by a few bits and produces a decode that fails with no diagnostic -- how
  // the original bwp_indicator/TDA bug behaved. Check it on EVERY candidate, not a sample.
  for (uint16_t rb : {(uint16_t)riv_bits_for(106), (uint16_t)riv_bits_for(273)}) {
    for (uint8_t tda : {(uint8_t)0, (uint8_t)2, (uint8_t)4}) {
      std::vector<nr_dci11_layout_t> c(NR_DCI11_LAYOUT_MAX);
      int checked = 0;
      for (uint16_t len = 30; len <= 70; len++) {
        const int n = nr_dci11_layout_enumerate(rb, tda, len, c.data(), NR_DCI11_LAYOUT_MAX);
        for (int i = 0; i < n; i++) {
          EXPECT_TRUE(nr_dci11_layout_apply_roundtrip(&c[i], rb, tda))
              << "layout " << i << " at riv=" << rb << " tda=" << (int)tda << " len=" << len
              << " does not survive the handover";
          checked++;
        }
      }
      EXPECT_GT(checked, 0);
    }
  }
}

TEST(Dci11Handover, TheConstantTpcAndPucchFieldsAreNotDoubleCounted) {
  // pre_ant includes TPC(2) + PUCCH-RI(3), but nr_pdcch_blind_dci_size_ex() counts those itself.
  // Handing the raw pre_ant over would grow the payload by 5 bits and shift everything after the
  // MCS -- so the subtraction is load-bearing, and this pins it.
  nr_dci11_layout_t l{};
  l.bwp_ind = 0; l.pre_mcs = 0; l.pre_ant = 11; l.ant_ports = 4; l.post_ant = 4;
  nr_dci11_field_bits_t f{};
  ASSERT_TRUE(nr_dci11_layout_to_field_bits(&l, &f));
  EXPECT_EQ(f.tb2_bits, 11 - 5) << "TPC and PUCCH-RI were double counted";
  EXPECT_TRUE(nr_dci11_layout_apply_roundtrip(&l, 16, 2));
}

TEST(Dci11Handover, RefusesALayoutItCannotRepresent) {
  nr_dci11_layout_t l{};
  l.bwp_ind = 0; l.pre_mcs = 0; l.pre_ant = 3; l.ant_ports = 4; l.post_ant = 0;  // below TPC+RI
  nr_dci11_field_bits_t f{};
  EXPECT_FALSE(nr_dci11_layout_to_field_bits(&l, &f));
  EXPECT_FALSE(nr_dci11_layout_apply_roundtrip(&l, 16, 2));
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// ---- stage 1: distributional pruning, impossible TDA, Thompson (2026-09-15) --------------------

// Payloads with the field STATISTICS of the loaded lab cell (not uniform random fields): under load
// the scheduler fills the band (RIV = full-band 545 most of the time), uses MCS 25 with MCS 24 for
// retransmissions, one or two TDA rows, RV 0 except retransmissions, and a constant antenna-ports
// codepoint for a single-layer UE. Everything the DCI does not constrain (HARQ PID, DAI, TPC, PUCCH
// RI, ...) is left random, which is what a misaligned read would pick up.
static uint64_t cell_payload(const nr_dci11_offsets_t &t, uint16_t rb, unsigned &seed, int retx_pct)
{
  uint64_t p = 0;
  for (int b = 0; b < t.total; b++) p |= (uint64_t)(rand_r(&seed) & 1) << b;
  auto put = [&](uint16_t off, uint8_t nb, uint64_t v) {
    const int sh = t.total - off - nb;
    p &= ~(((1ULL << nb) - 1ULL) << sh);
    p |= (v & ((1ULL << nb) - 1ULL)) << sh;
  };
  const bool retx = (int)(rand_r(&seed) % 100) < retx_pct;
  put(0, 1, 1);                                                           // identifier: DL
  put(t.riv, (uint8_t)rb, (rand_r(&seed) % 10) ? 545 : rand_r(&seed) % (273 * 274 / 2));
  if (t.tda_bits) put(t.tda, t.tda_bits, (rand_r(&seed) % 10) < 7 ? 0 : 1);
  uint32_t mcs = 25;
  if (retx) mcs = (rand_r(&seed) % 2) ? 24 : 28 + rand_r(&seed) % 4;
  put(t.mcs, 5, mcs);
  put(t.rv, 2, retx ? 2 + rand_r(&seed) % 2 : 0);
  put(t.ant_ports, t.ant_ports_bits, 0);
  return p;
}

static int pick_truth(const nr_dci11_resolver_t &r)
{
  for (int i = 0; i < r.n_hyp; i++)
    if (r.hyp[i].bwp_ind == 1 && r.hyp[i].ant_ports == 4) return i;
  return 0;
}

TEST(Dci11Stage1, TheLoadedCellsFieldStructurePrunesToAHandful) {
  // The OTA failure this fixes: 15 of 15 layouts alive after 1.2M payloads, so stage 2 never
  // engaged. Same geometry as the lab cell (273 PRB, 16-bit RIV, 4-bit TDA, 47-bit DCI).
  nr_dci11_resolver_t r{};
  const uint16_t rb = riv_bits_for(273);
  const int n = nr_dci11_resolver_init(&r, 273, rb, 4, 47);
  ASSERT_GT(n, 4);
  const int truth = pick_truth(r);
  unsigned seed = 77;
  for (int i = 0; i < 20000; i++) nr_dci11_resolver_observe(&r, cell_payload(r.off[truth], rb, seed, 10));
  EXPECT_TRUE(r.alive[truth]) << "distributional pruning deleted the true layout";
  EXPECT_LE(r.n_alive, 4) << "stage 1 still cannot separate layouts on a structured cell";
  std::cerr << "[ MEASURED ] loaded-cell statistics: " << n << " -> " << r.n_alive
            << " live after 20000 payloads (" << r.dropped_dist << " by distribution)\n";
}

TEST(Dci11Stage1, AHeavilyRetransmittingCellStillKeepsTheTruth) {
  // A commercial cell with weak UEs retransmits far more than the lab: 35 % retx, so MCS 28-31 and
  // RV 2/3 are common in the TRUE fields and their histograms are much less peaked.
  nr_dci11_resolver_t r{};
  const uint16_t rb = riv_bits_for(273);
  const int n = nr_dci11_resolver_init(&r, 273, rb, 4, 47);
  ASSERT_GT(n, 4);
  const int truth = pick_truth(r);
  unsigned seed = 91;
  for (int i = 0; i < 30000; i++) nr_dci11_resolver_observe(&r, cell_payload(r.off[truth], rb, seed, 35));
  EXPECT_TRUE(r.alive[truth]) << "a 35 % retransmission rate deleted the true layout";
  EXPECT_GE(r.n_alive, 1);
  std::cerr << "[ MEASURED ] 35% retransmissions: " << n << " -> " << r.n_alive << " live\n";
}

TEST(Dci11Stage1, TheTruthScoresAboveEveryPrunedLayout) {
  nr_dci11_resolver_t r{};
  const uint16_t rb = riv_bits_for(273);
  nr_dci11_resolver_init(&r, 273, rb, 4, 47);
  const int truth = pick_truth(r);
  unsigned seed = 5;
  for (int i = 0; i < 5000; i++) nr_dci11_resolver_observe(&r, cell_payload(r.off[truth], rb, seed, 10));
  const double st = nr_dci11_resolver_score(&r, truth);
  for (int i = 0; i < r.n_hyp; i++)
    if (!r.alive[i]) EXPECT_GT(st, nr_dci11_resolver_score(&r, i)) << "pruned layout " << i << " outscored the truth";
}

TEST(Dci11Stage1, ATdaIndexBeyondTheListIsImpossible) {
  nr_dci11_layout_t l{};
  l.bwp_ind = 1; l.ant_ports = 4; l.pre_ant = 11;
  nr_dci11_offsets_t o{};
  ASSERT_TRUE(nr_dci11_layout_offsets(&l, 16, 2, &o));
  o.tda_valid = 3;                                   // three TDA rows configured
  unsigned seed = 1;
  uint64_t p = cell_payload(o, 16, seed, 0);
  const int sh = o.total - o.tda - 2;
  p &= ~(3ULL << sh);
  EXPECT_TRUE(nr_dci11_layout_plausible(&o, p | (2ULL << sh), 273));
  EXPECT_FALSE(nr_dci11_layout_plausible(&o, p | (3ULL << sh), 273)) << "index 3 of a 3-row list accepted";
  o.tda_valid = 0;
  EXPECT_TRUE(nr_dci11_layout_plausible(&o, p | (3ULL << sh), 273)) << "the test must stay off when unknown";
}

TEST(Dci11Thompson, TrialsConcentrateOnTheArmThatDecodes) {
  // The dilution Thompson replaces: round-robin over N candidates gives the right one 1/N of the
  // grants. Here the right arm must end up with the large majority, and no arm may be starved of a
  // first look (nothing is deleted, a 20 % retransmission rate cannot lose the truth).
  const double rate[6] = {0.02, 0.0, 0.45, 0.0, 0.08, 0.0};
  uint32_t ok[6] = {0}, tr[6] = {0};
  uint64_t rng = 12345;
  unsigned seed = 3;
  for (int t = 0; t < 3000; t++) {
    const int a = nr_dci11_thompson_pick(ok, tr, nullptr, 6, &rng);
    ASSERT_GE(a, 0);
    tr[a]++;
    if ((double)rand_r(&seed) / RAND_MAX < rate[a]) ok[a]++;
  }
  for (int a = 0; a < 6; a++) EXPECT_GE(tr[a], 1u) << "arm " << a << " never sampled";
  EXPECT_GT(tr[2], 0.8 * 3000) << "the decoding arm did not get the majority of trials";
  std::cerr << "[ MEASURED ] Thompson trials per arm: " << tr[0] << " " << tr[1] << " " << tr[2] << " "
            << tr[3] << " " << tr[4] << " " << tr[5] << " (round-robin would give 500 each)\n";
}

TEST(Dci11Thompson, AStage1PriorSteersTheFirstGrants) {
  uint32_t ok[4] = {0}, tr[4] = {0};
  const double prior[4] = {0.0, 0.0, 4.0, 0.0};
  uint64_t rng = 7;
  int hits = 0;
  for (int t = 0; t < 1000; t++) hits += (nr_dci11_thompson_pick(ok, tr, prior, 4, &rng) == 2);
  EXPECT_GT(hits, 500) << "a prior-favoured arm was not preferred before any evidence";
  EXPECT_LT(hits, 1000) << "a prior must bias, not exclude";
}
