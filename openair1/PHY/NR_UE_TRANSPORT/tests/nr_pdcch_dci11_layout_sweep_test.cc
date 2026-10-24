#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
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
  l.bwp_ind = 6; l.ant_ports = 4;                       // carrier indicator 3 + BWP indicator 2 at most (DCI11-RESOLVER)
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
                        && c[i].post_ant == c[j].post_ant && c[i].dmrs_type == c[j].dmrs_type && c[i].tail == c[j].tail;
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
  EXPECT_FALSE(nr_dci11_layout_plausible(&o,nr_dci_bits_from_u64( p), 273));
  // Same payload, MCS 10 -> plausible.
  p = 0;
  p |= (uint64_t)10 << mcs_shift;
  EXPECT_TRUE(nr_dci11_layout_plausible(&o,nr_dci_bits_from_u64( p), 273));
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
  EXPECT_FALSE(nr_dci11_layout_plausible(&o,nr_dci_bits_from_u64( p), 24));
  EXPECT_TRUE(nr_dci11_layout_plausible(&o,nr_dci_bits_from_u64( p), 273));
}

// TS 38.212 Table 7.3.1.2.2-{1,2,3,4} row counts, exactly as nr_pdcch_blind_monitor.c's
// g_table_7_3_2_3_3_{1,2,3,4} arrays are sized (the actual DCI 1_1 extraction indexes those, so a
// disagreement here would silently mis-prune the layout search). Width 4 is always DM-RS type 1
// maxLength 1; width 5 is type 1 maxLength 2 OR type 2 maxLength 1, told apart by dmrs_type; width 6
// is always type 2 maxLength 2.
TEST(Dci11Layout, ApValidRowsCoversAllFourAntennaPortsTables) {
  nr_dci11_layout_t l{};
  l.bwp_ind = 0; l.pre_mcs = 0; l.pre_ant = 11; l.post_ant = 4;
  nr_dci11_offsets_t o{};

  l.ant_ports = 4; l.dmrs_type = 0;
  ASSERT_TRUE(nr_dci11_layout_offsets(&l, 16, 2, &o));
  EXPECT_EQ(o.ap_valid_rows, 12);

  l.ant_ports = 5; l.dmrs_type = 0;
  ASSERT_TRUE(nr_dci11_layout_offsets(&l, 16, 2, &o));
  EXPECT_EQ(o.ap_valid_rows, 31);

  l.ant_ports = 5; l.dmrs_type = 1;
  ASSERT_TRUE(nr_dci11_layout_offsets(&l, 16, 2, &o));
  EXPECT_EQ(o.ap_valid_rows, 24);

  l.ant_ports = 6; l.dmrs_type = 1;
  ASSERT_TRUE(nr_dci11_layout_offsets(&l, 16, 2, &o));
  EXPECT_EQ(o.ap_valid_rows, 58);
}

// Before this fix, ap_valid_rows was 0 at every width but 4, so an out-of-range antenna_ports code
// point on a DM-RS-type-2 (or maxLength-2 type-1) layout was NOT rejected by this "free" plausibility
// signal -- it just meant the search converged slower, not that it converged wrong, but the whole
// point of this check is to speed up exactly this. Table 7.3.1.2.2-3 (width 5, type 2) has 24 rows.
TEST(Dci11Layout, PlausibilityRejectsAnOutOfRangeType2AntennaPorts) {
  nr_dci11_layout_t l{};
  l.bwp_ind = 0; l.pre_mcs = 0; l.pre_ant = 11; l.ant_ports = 5; l.dmrs_type = 1; l.post_ant = 4;
  nr_dci11_offsets_t o{};
  ASSERT_TRUE(nr_dci11_layout_offsets(&l, 16, 2, &o));
  ASSERT_EQ(o.ap_valid_rows, 24);

  auto build = [&](uint32_t ap) {
    uint64_t p = 0;
    p |= (uint64_t)10 << (o.total - o.mcs - 5);                        // legal MCS
    p |= (uint64_t)ap << (o.total - o.ant_ports - o.ant_ports_bits);   // antenna_ports field
    return p;
  };
  EXPECT_TRUE(nr_dci11_layout_plausible(&o,nr_dci_bits_from_u64( build(23)), 273));   // last valid row of table -3
  EXPECT_FALSE(nr_dci11_layout_plausible(&o,nr_dci_bits_from_u64( build(24)), 273));  // one past it
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
  for (int i = 0; i < N; i++) if (nr_dci11_layout_plausible(&t_off,nr_dci_bits_from_u64( payloads[i]), 273)) t_ok++;
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
    for (int i = 0; i < N; i++) if (nr_dci11_layout_plausible(&w,nr_dci_bits_from_u64( payloads[i]), 273)) ok++;
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
    for (int i = 0; i < N; i++) if (nr_dci11_layout_plausible(&w,nr_dci_bits_from_u64( payloads[i]), 273)) ok++;
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
    nr_dci11_resolver_observe(&r,nr_dci_bits_from_u64( p));
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
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();  // ~4.3 MB: never on the stack
  nr_dci11_resolver_t &r = *r_heap;
  // 9 bits cannot hold a 16-bit RIV plus the fixed fields -- no layout can sum to it. That is a
  // real signal (riv_bits / tda_bits / observed_len disagree), not a resolver failure.
  EXPECT_EQ(nr_dci11_resolver_init(&r, 273, 16, 2, 9), 0);
  nr_dci11_offsets_t o{};
  EXPECT_EQ(nr_dci11_resolver_next(&r, &o), -1);
}

TEST(Dci11Resolver, Stage1NarrowsButNeverEmptiesTheSet) {
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();  // ~4.3 MB: never on the stack
  nr_dci11_resolver_t &r = *r_heap;
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
    nr_dci11_resolver_observe(&r,nr_dci_bits_from_u64( p));
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
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();  // ~4.3 MB: never on the stack
  nr_dci11_resolver_t &r = *r_heap;
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
    nr_dci11_resolver_observe(&r,nr_dci_bits_from_u64( p));
  }
  EXPECT_TRUE(r.alive[truth]) << "a 20 % retransmission rate deleted the true layout";
  std::cerr << "[ MEASURED ] with 20% retransmissions: " << n << " -> " << r.n_alive
            << " live, truth survived\n";
}

TEST(Dci11Resolver, FindsTheTruthEndToEnd) {
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();  // ~4.3 MB: never on the stack
  nr_dci11_resolver_t &r = *r_heap;
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
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();  // ~4.3 MB: never on the stack
  nr_dci11_resolver_t &r = *r_heap;
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
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();  // ~4.3 MB: never on the stack
  nr_dci11_resolver_t &r = *r_heap;
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
  // ISAC_DCI11_HARQPID is baked on (presence-only): the 4 pid bits are carved out of the 6 residual bits
  EXPECT_EQ(f.tb2_bits + f.harq_pid_bits + f.dai_bits, 11 - 5) << "TPC and PUCCH-RI were double counted";
  EXPECT_TRUE(nr_dci11_layout_apply_roundtrip(&l, 16, 2));
}

TEST(Dci11Handover, HarqPidIsHandedOverAsFourBitsWhenEnabled) {
  // HARQPID: without the flag the 4 pid bits ride in tb2_bits and every grant reads pid 0.
  nr_dci11_layout_t l{};
  l.bwp_ind = 0; l.pre_mcs = 0; l.pre_ant = 11; l.ant_ports = 4; l.post_ant = 4;
  nr_dci11_field_bits_t f{};
  ASSERT_TRUE(nr_dci11_layout_to_field_bits(&l, &f));
  // HARQPID is baked on (presence-only flag): the "off" branch is no longer reachable
  EXPECT_EQ(f.harq_pid_bits, 4);
  EXPECT_EQ(f.tb2_bits, 0);
  EXPECT_EQ(f.dai_bits, 11 - 5 - 4);  // total width unchanged
  EXPECT_EQ(f.tb2_bits + f.harq_pid_bits + f.dai_bits, 11 - 5);
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

TEST(Dci11Stage1, LoadedCellStatisticsNeverDeleteTheTruth) {
  // OTA 2026-09-15 (v2l): the layout that decodes at 72 % by TB CRC was NOT in the top 4 by payload statistics. The
  // distributional score (a field-structure prior) is gone entirely (DCI11-RESOLVER: only DM-RS fit and CRC rank); what is
  // left of stage 1 -- impossibility and the reserved-MCS rate -- must keep the truth on the lab-cell statistics.
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();
  nr_dci11_resolver_t &r = *r_heap;
  const uint16_t rb = riv_bits_for(273);
  const int n = nr_dci11_resolver_init(&r, 273, rb, 4, 47);
  ASSERT_GT(n, 4);
  const int truth = pick_truth(r);
  unsigned seed = 77;
  for (int i = 0; i < 20000; i++) nr_dci11_resolver_observe(&r,nr_dci_bits_from_u64( cell_payload(r.off[truth], rb, seed, 10)));
  EXPECT_TRUE(r.alive[truth]) << "stage 1 deleted the true layout";
  EXPECT_GT(r.n_alive, 4) << "payload statistics must never narrow to a handful (only the TB CRC may)";
  std::cerr << "[ MEASURED ] loaded-cell statistics: " << n << " -> " << r.n_alive << " live after 20000 payloads\n";
}

TEST(Dci11Stage1, AHeavilyRetransmittingCellStillKeepsTheTruth) {
  // A commercial cell with weak UEs retransmits far more than the lab: 35 % retx, so MCS 28-31 and
  // RV 2/3 are common in the TRUE fields and their histograms are much less peaked.
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();  // ~4.3 MB: never on the stack
  nr_dci11_resolver_t &r = *r_heap;
  const uint16_t rb = riv_bits_for(273);
  const int n = nr_dci11_resolver_init(&r, 273, rb, 4, 47);
  ASSERT_GT(n, 4);
  const int truth = pick_truth(r);
  unsigned seed = 91;
  for (int i = 0; i < 30000; i++) nr_dci11_resolver_observe(&r,nr_dci_bits_from_u64( cell_payload(r.off[truth], rb, seed, 35)));
  EXPECT_TRUE(r.alive[truth]) << "a 35 % retransmission rate deleted the true layout";
  EXPECT_GE(r.n_alive, 1);
  std::cerr << "[ MEASURED ] 35% retransmissions: " << n << " -> " << r.n_alive << " live\n";
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
  EXPECT_TRUE(nr_dci11_layout_plausible(&o,nr_dci_bits_from_u64( p | (2ULL << sh)), 273));
  EXPECT_FALSE(nr_dci11_layout_plausible(&o,nr_dci_bits_from_u64( p | (3ULL << sh)), 273)) << "index 3 of a 3-row list accepted";
  o.tda_valid = 0;
  EXPECT_TRUE(nr_dci11_layout_plausible(&o,nr_dci_bits_from_u64( p | (3ULL << sh)), 273)) << "the test must stay off when unknown";
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

TEST(Dci11Layout, UnknownTdaWidthEnumeratesEveryWidthAndContainsBothRealCells) {
  // The TDRA list size is an RRC switch the receiver cannot read. Two real cells, one payload length:
  // the OAI rfsim cell has 3 entries (2-bit TDA), the srsRAN OTA cell has 2 (1-bit); a layout with
  // pre_ant = harq4+dai2+tpc2+ri3+k1(3) = 14, ant 4, srs 2 sums to 47 only at 1 bit. With the width
  // searched, both truths are hypotheses; with the old 4-bit default, neither was (OTA 2026-09-15).
  const uint16_t rb = riv_bits_for(273);
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();  // ~4.3 MB: never on the stack
  nr_dci11_resolver_t &r = *r_heap;
  const int n = nr_dci11_resolver_init(&r, 273, rb, NR_DCI11_TDA_UNKNOWN, 47);
  ASSERT_GT(n, 0);
  bool has_1bit_truth = false, has_2bit = false, widths[5] = {false, false, false, false, false};
  for (int i = 0; i < n; i++) {
    EXPECT_EQ(r.off[i].total, 47);
    widths[r.off[i].tda_bits] = true;
    if (r.off[i].tda_bits == 1 && r.hyp[i].bwp_ind == 0 && r.hyp[i].pre_mcs == 0 && r.hyp[i].pre_ant == 14
        && r.hyp[i].ant_ports == 4 && r.hyp[i].post_ant == 2)
      has_1bit_truth = true;
    if (r.off[i].tda_bits == 2) has_2bit = true;
  }
  EXPECT_TRUE(has_1bit_truth) << "the srsRAN rank-4 cell's layout must be a hypothesis";
  EXPECT_TRUE(has_2bit) << "the rfsim cell's 2-bit width must still be represented";
  EXPECT_TRUE(widths[0] || widths[1]) << "narrow widths enumerated";
  std::cerr << "[ MEASURED ] searched TDA width: " << n << " hypotheses at len 47\n";
}

TEST(Dci11Layout, AFiveBitAntennaPortsFieldIsTwoLayouts) {
  // Table 7.3.1.2.2-2 (type 1, maxLength 2) and -3 (type 2, maxLength 1) are both 5 bits wide: the
  // same offsets, different port sets, so the DM-RS type is a layout dimension at that width only.
  const uint16_t rb = riv_bits_for(106);
  std::vector<nr_dci11_layout_t> c(NR_DCI11_LAYOUT_MAX);
  const int n = nr_dci11_layout_enumerate(rb, 2, 45, c.data(), NR_DCI11_LAYOUT_MAX);
  int five_t1 = 0, five_t2 = 0, four_t2 = 0, six_t1 = 0;
  for (int i = 0; i < n; i++) {
    if (c[i].ant_ports == 5) (c[i].dmrs_type ? five_t2 : five_t1)++;
    if (c[i].ant_ports == 4 && c[i].dmrs_type) four_t2++;
    if (c[i].ant_ports == 6 && !c[i].dmrs_type) six_t1++;
  }
  EXPECT_EQ(five_t1, five_t2);
  EXPECT_GT(five_t1, 0);
  EXPECT_EQ(four_t2, 0);
  EXPECT_EQ(six_t1, 0);
}

// ---- FDRA modes: RA type 0 and dynamicSwitch (full-running-agnosticity Task 10) ----------------
// 106 PRB at CRB 0: RIV 13 bits, rbg-Size config1 P=8 -> N_RBG 14, config2 P=16 -> N_RBG 7, so all
// five modes have distinct widths (13 / 14 / 7 / 15 / 14).
static const uint16_t kFdraBwpStart = 0, kFdraBwp = 106;

static void put_bits(uint64_t &p, uint16_t total, uint16_t off, uint8_t nb, uint64_t v)
{
  const int sh = total - off - nb;
  p &= ~(((1ULL << nb) - 1ULL) << sh);
  p |= (v & ((1ULL << nb) - 1ULL)) << sh;
}

static uint32_t riv_of(int start, int len, int N)
{
  return (len - 1 <= N / 2) ? (uint32_t)(N * (len - 1) + start) : (uint32_t)(N * (N - len + 1) + (N - 1 - start));
}

// A valid FDRA field for this layout's mode; dynamicSwitch alternates the two branches via want_type0.
static uint32_t fdra_field(const nr_dci11_offsets_t &t, unsigned &seed, bool want_type0)
{
  const int N = kFdraBwp;
  const uint32_t riv = rand_r(&seed) % (N * (N + 1) / 2);
  const uint32_t bitmap = t.n_rbg ? 1 + rand_r(&seed) % ((1u << t.n_rbg) - 1) : 0;
  switch (t.fdra_mode) {
    case NR_FDRA_TYPE1: return riv;
    case NR_FDRA_TYPE0_CFG1: case NR_FDRA_TYPE0_CFG2: return bitmap;
    default: {
      const int w = t.n_rbg > t.riv_bits ? t.n_rbg : t.riv_bits;
      return want_type0 ? bitmap : ((1u << w) | riv);
    }
  }
}

static uint64_t fdra_payload(const nr_dci11_offsets_t &t, unsigned &seed, uint32_t field)
{
  uint64_t p = 0;
  for (int b = 0; b < t.total; b++) p |= (uint64_t)(rand_r(&seed) & 1) << b;
  put_bits(p, t.total, t.riv, (uint8_t)(t.tda - t.riv), field);
  put_bits(p, t.total, t.mcs, 5, rand_r(&seed) % 28);
  put_bits(p, t.total, t.rv, 2, 0);
  if (t.ap_valid_rows) put_bits(p, t.total, t.ant_ports, t.ant_ports_bits, rand_r(&seed) % t.ap_valid_rows);
  return p;
}

static uint32_t read_fdra(const nr_dci11_offsets_t &t, uint64_t p)
{
  const uint8_t nb = (uint8_t)(t.tda - t.riv);
  return (uint32_t)((p >> (t.total - t.riv - nb)) & ((1ULL << nb) - 1ULL));
}

// (c): the decoded PRB list of the resolved layout equals nr_ra_type0_prbs() of the constructed bitmap
// (type 0, and dynamicSwitch's type-0 branch) or the constructed RIV's contiguous range (type 1).
static void expect_prbs(const nr_dci11_offsets_t &resolved, const nr_dci11_offsets_t &truth, bool type0,
                        unsigned &seed)
{
  const int N = kFdraBwp;
  const int P = nr_fdra_rbg_size(truth.fdra_mode, N);
  std::vector<uint16_t> want(NR_PRB_SET_MAX), got(NR_PRB_SET_MAX);
  int nw;
  uint32_t field;
  if (type0) {
    const uint32_t bitmap = (1u << (truth.n_rbg - 1)) | 0x5u;  // RBG 0 (MSB) plus two late RBGs
    nw = nr_ra_type0_prbs(bitmap, kFdraBwpStart, N, P, want.data(), NR_PRB_SET_MAX);
    field = bitmap;
  } else {
    const int S = 17, L = 40;
    for (nw = 0; nw < L; nw++) want[nw] = (uint16_t)(S + nw);
    field = riv_of(S, L, N);
    if (truth.fdra_mode >= NR_FDRA_DYN_CFG1) field |= 1u << (truth.n_rbg > truth.riv_bits ? truth.n_rbg : truth.riv_bits);
  }
  const uint64_t p = fdra_payload(truth, seed, field);
  int t0 = -1;
  const int ng = nr_fdra_prbs(read_fdra(resolved, p), resolved.fdra_mode, resolved.n_rbg, resolved.riv_bits, kFdraBwpStart,
                              N, got.data(), NR_PRB_SET_MAX, &t0);
  EXPECT_EQ(t0, type0 ? 1 : 0);
  ASSERT_EQ(ng, nw);
  for (int i = 0; i < nw; i++) EXPECT_EQ(got[i], want[i]) << "PRB " << i;
}

static int find_layout(const nr_dci11_resolver_t &r, const nr_dci11_layout_t &l)
{
  for (int i = 0; i < r.n_hyp; i++)
    if (!memcmp(&r.hyp[i], &l, sizeof(l))) return i;
  return -1;
}

// Every FDRA mode end to end (DCI11-RESOLVER: no staging -- every mode is in the set from the start): the truth of each mode
// is enumerated at init, stage 1 keeps it, and stage 2 (only the truth decodes) converges on it.
static void resolve_fdra_mode(uint8_t mode)
{
  const uint16_t rb = riv_bits_for(kFdraBwp);
  nr_dci11_layout_t l{};
  l.bwp_ind = 0; l.pre_mcs = 0; l.pre_ant = 14; l.ant_ports = 4; l.post_ant = 2; l.dmrs_type = 0;
  l.fdra_mode = mode;
  l.n_rbg = mode == NR_FDRA_TYPE1 ? 0
                                  : (uint8_t)nr_rbg_count(kFdraBwpStart, kFdraBwp, nr_fdra_rbg_size(mode, kFdraBwp));
  nr_dci11_offsets_t o{};
  ASSERT_TRUE(nr_dci11_layout_offsets(&l, rb, 2, &o));
  EXPECT_EQ(o.tda - o.riv, nr_fdra_bits(mode, l.n_rbg, rb));
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();
  nr_dci11_resolver_t &r = *r_heap;
  const int n0 = nr_dci11_resolver_init_fdra(&r, kFdraBwpStart, kFdraBwp, rb, 2, o.total);
  ASSERT_GT(n0, 1);
  int modes = 0;
  for (int i = 0; i < n0; i++) modes |= 1 << r.off[i].fdra_mode;
  int truth = find_layout(r, l), w = -1;
  ASSERT_GE(truth, 0) << "the truth of every FDRA mode must be in the set at init";
  unsigned seed = 31u + mode;
  for (int i = 0; i < 800; i++) nr_dci11_resolver_observe(&r,nr_dci_bits_from_u64( fdra_payload(o, seed, fdra_field(o, seed, i & 1))));
  ASSERT_TRUE(r.alive[truth]) << "stage 1 deleted the truth";
  for (int i = 0; i < 4000000 && w < 0; i++) {
    nr_dci11_offsets_t pick{};
    const int idx = nr_dci11_resolver_next(&r, &pick);
    ASSERT_GE(idx, 0);
    w = nr_dci11_resolver_feed(&r, idx, idx == truth && (double)rand_r(&seed) / RAND_MAX < 0.40);
  }
  ASSERT_EQ(w, truth);
  EXPECT_EQ(r.hyp[w].fdra_mode, mode);                                     // (a)
  EXPECT_EQ(r.off[w].mcs, o.mcs);                                          // (b)
  EXPECT_EQ(r.off[w].rv, o.rv);
  EXPECT_EQ(r.off[w].ant_ports, o.ant_ports);
  EXPECT_TRUE(nr_dci11_layout_apply_roundtrip(&r.hyp[w], rb, 2)) << "field-bits handover shifts a type-0 layout";
  const bool t0 = mode != NR_FDRA_TYPE1;
  expect_prbs(r.off[w], o, t0, seed);                                      // (c)
  if (mode >= NR_FDRA_DYN_CFG1) expect_prbs(r.off[w], o, false, seed);     // dynamicSwitch, RIV branch
  std::cerr << "[ MEASURED ] fdra_mode " << (int)mode << ": " << n0 << " layouts at len " << o.total << " (modes present 0x"
            << std::hex << modes << std::dec << "), " << r.n_alive << " live after stage 1, converged on " << w << "\n";
}

TEST(Dci11Fdra, ResolvesType1) { resolve_fdra_mode(NR_FDRA_TYPE1); }
TEST(Dci11Fdra, ResolvesType0Config1) { resolve_fdra_mode(NR_FDRA_TYPE0_CFG1); }
TEST(Dci11Fdra, ResolvesType0Config2) { resolve_fdra_mode(NR_FDRA_TYPE0_CFG2); }
TEST(Dci11Fdra, ResolvesDynamicSwitchConfig1) { resolve_fdra_mode(NR_FDRA_DYN_CFG1); }
TEST(Dci11Fdra, ResolvesDynamicSwitchConfig2) { resolve_fdra_mode(NR_FDRA_DYN_CFG2); }

TEST(Dci11Fdra, PlausibilityFollowsTheMode) {
  const uint16_t rb = riv_bits_for(kFdraBwp);
  nr_dci11_layout_t l{};
  l.pre_ant = 14; l.ant_ports = 4; l.post_ant = 2;
  l.fdra_mode = NR_FDRA_TYPE0_CFG1; l.n_rbg = 14;
  nr_dci11_offsets_t o{};
  ASSERT_TRUE(nr_dci11_layout_offsets(&l, rb, 2, &o));
  uint64_t p = 0;
  put_bits(p, o.total, o.mcs, 5, 10);
  EXPECT_FALSE(nr_dci11_layout_plausible(&o,nr_dci_bits_from_u64( p), kFdraBwp)) << "an empty RBG bitmap allocates nothing";
  put_bits(p, o.total, o.riv, 14, 0x3FFF);  // all 14 RBGs: fine for a bitmap, an out-of-BWP RIV value
  EXPECT_TRUE(nr_dci11_layout_plausible(&o,nr_dci_bits_from_u64( p), kFdraBwp));
  l.fdra_mode = NR_FDRA_DYN_CFG1;           // width 15, MSB 0 -> type 0
  ASSERT_TRUE(nr_dci11_layout_offsets(&l, rb, 2, &o));
  p = 0;
  put_bits(p, o.total, o.mcs, 5, 10);
  EXPECT_FALSE(nr_dci11_layout_plausible(&o,nr_dci_bits_from_u64( p), kFdraBwp));
  put_bits(p, o.total, o.riv, 15, (1u << 14) | 8000u);  // MSB 1 -> RIV 8000 >= 106*107/2
  EXPECT_FALSE(nr_dci11_layout_plausible(&o,nr_dci_bits_from_u64( p), kFdraBwp));
  put_bits(p, o.total, o.riv, 15, (1u << 14) | 500u);
  EXPECT_TRUE(nr_dci11_layout_plausible(&o,nr_dci_bits_from_u64( p), kFdraBwp));
}

TEST(Dci11Fdra, EveryModeIsInTheSetFromTheStart) {
  // DCI11-RESOLVER: the type-1-first staging (arming RA type 0 / dynamicSwitch only after a TB-CRC refutation of every
  // type-1 layout) was a prior; the set now holds every mode at init, and holds exactly what each mode's enumeration gives.
  const uint16_t rb = riv_bits_for(106);
  std::vector<nr_dci11_layout_t> tmp(1 << 16);
  for (int L : {45, 46, 48, 49, 50}) {
    auto r_heap = std::make_unique<nr_dci11_resolver_t>();
    nr_dci11_resolver_t &r = *r_heap;
    const int n = nr_dci11_resolver_init_fdra(&r, 0, 106, rb, NR_DCI11_TDA_UNKNOWN, (uint16_t)L);
    ASSERT_GT(n, 0);
    EXPECT_EQ((long)n, nr_dci11_layout_space_count(0, 106, rb, NR_DCI11_TDA_UNKNOWN, (uint16_t)L));
    EXPECT_EQ(r.n_truncated, 0u);
    std::cerr << "[ MEASURED ] 106 PRB L=" << L << ":";
    for (int m = NR_FDRA_TYPE1; m <= NR_FDRA_DYN_CFG2; m++) {
      int want = 0, have = 0;
      for (uint8_t tb = 0; tb <= 4; tb++)
        want += nr_dci11_layout_enumerate_mode(rb, tb, (uint16_t)L, 0, 106, (uint8_t)m, tmp.data(), (int)tmp.size());
      for (int i = 0; i < n; i++) have += r.off[i].fdra_mode == m;
      EXPECT_EQ(have, want) << "mode " << m << " at L=" << L;
      EXPECT_GT(have, 0) << "mode " << m << " missing at L=" << L;
      std::cerr << " mode " << m << " " << have;
    }
    std::cerr << " | total " << n << "\n";
  }
}

TEST(Dci11Fdra, EveryLengthIsCompleteOrReportsWhatIsMissing) {
  // Capacity (NR_DCI11_LAYOUT_MAX): BWP 6..273 (aligned and worst-misaligned CRB start), DCI length 28..62, TDA width
  // searched. Where the standard space fits, the resolver holds ALL of it (FieldByFieldSpaceEqualsTheResolver); where it does
  // not, nothing is dropped silently: every layout not stored is counted.
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();
  nr_dci11_resolver_t &r = *r_heap;
  long worst = 0;
  int wN = 0, wL = 0, over = 0, total = 0;
  for (int N = 6; N <= 273; N += 3) {
    const int rb = riv_bits_for(N);
    for (int start : {0, 15})
      for (int L = 28; L <= 62; L++) {
        const long sp = nr_dci11_layout_space_count((uint16_t)start, (uint16_t)N, (uint16_t)rb, NR_DCI11_TDA_UNKNOWN, (uint16_t)L);
        total++;
        if (sp > worst) { worst = sp; wN = N; wL = L; }
        if (sp <= NR_DCI11_LAYOUT_MAX) continue;
        if (++over > 6) continue; /* a sample of the over-capacity points pins the reporting */
        nr_dci11_resolver_init_fdra(&r, (uint16_t)start, (uint16_t)N, (uint16_t)rb, NR_DCI11_TDA_UNKNOWN, (uint16_t)L);
        EXPECT_EQ((long)r.n_hyp + (long)r.n_truncated, sp) << "N=" << N << " L=" << L << ": a layout vanished without a count";
        EXPECT_EQ((long)r.n_space, sp);
      }
  }
  std::cerr << "[ MEASURED ] standard space at one length: max " << worst << " (" << wN << " PRB, " << wL << " bits); " << over
            << " of " << total << " (BWP, start, length) points exceed the capacity " << NR_DCI11_LAYOUT_MAX << "\n";
}

// T26 [OFFLINE VERIFIED replay raw_pci415_12s_20261008]: the first 80 CRC-verified (mismatched_bits <= 1) DCI 1_1
// payloads of C-RNTI 0x8f5a, length 49, 217-PRB BWP. Field statistics (403 DCIs) fit only: id 1 | BWP ind 1 (=1) |
// RA type 0 bitmap 14 RBGs (P=16) | 4 | MCS 5 | NDI | RV 2 | HARQ 4 (increments mod 16 in 368/402 consecutive
// DCIs) | DAI 2 | TPC 2 | PUCCH RI 3 | K1 3 | AP 4 | SRS 2 | DM-RS init 1. 13 % carry the full-band bitmap
// 0x3fff, which every type-1 layout reads as RIV 0x7fff > 217*218/2: impossible for type 1.
static const uint64_t kPci415Dci11[] = {0x18201009c2098ULL,0x1ffff149e2080ULL,0x1ffff1400a000ULL,0x1fffe14022380ULL,0x1fe781404a200ULL,0x1800900062098ULL,0x1fdfe94082100ULL,0x1ffff140aa080ULL,0x1ffff140d2000ULL,0x1fffe140e2380ULL,0x1fffe1410a200ULL,0x1ffff15132180ULL,0x184010016a098ULL,0x1801100182098ULL,0x18801001a2098ULL,0x1a001001c20b8ULL,0x18201001e2138ULL,0x18011008020b8ULL,0x18008018223b8ULL,0x1801084842238ULL,0x1808081862138ULL,0x18021008820b8ULL,0x18801008a4138ULL,0x18101008c2138ULL,0x18041008e20b8ULL,0x1820000902238ULL,0x1a001009220b8ULL,0x1804001942238ULL,0x18081009620b8ULL,0x18005009821b8ULL,0x1c001009a20b8ULL,0x18201009c21b8ULL,0x188011cdaa0b8ULL,0x19001009f2038ULL,0x1900510022090ULL,0x18009000620b8ULL,0x1820001082238ULL,0x18801000a21b8ULL,0x18401000c20b8ULL,0x18011010e21b8ULL,0x18011001020b8ULL,0x18201001221b8ULL,0x188010014a0b8ULL,0x18021001621b8ULL,0x1880100182138ULL,0x181010c1a2110ULL,0x1a001001c00b8ULL,0x18081001e21b8ULL,0x180080e802210ULL,0x18011008220b8ULL,0x18201028421b8ULL,0x188010086a0b8ULL,0x1800900882098ULL,0x1fdb5138a2080ULL,0x1fffe168e2380ULL,0x1c00100922098ULL,0x1fdbc16962380ULL,0x190010d9a2000ULL,0x1fffe169c2380ULL,0x1802100012198ULL,0x1810100022098ULL,0x1800500042098ULL,0x1c00100082098ULL,0x18005020aa018ULL,0x18101000c2098ULL,0x18081000e2098ULL,0x1840103100118ULL,0x1802101122198ULL,0x180110018a118ULL,0x18021001b2098ULL,0x18080031c2398ULL,0x1a001011e2198ULL,0x1900100842098ULL,0x1a001008ea098ULL,0x1880101902018ULL,0x1a00003922218ULL,0x1c0010094a198ULL,0x1800504962198ULL,0x1900101982198ULL,0x1a001039aa118ULL};

TEST(Dci11Fdra, HardRefutationRemovesType1OnTheLiveCellPayloads) {
  // Every mode is in the set from the start (DCI11-RESOLVER), so the 13 % full-band bitmaps remove every type-1 layout by
  // hard refutation while the field-statistics layout (RA type 0 cfg1) stays alive -- no arming step in between.
  static nr_dci11_resolver_t r;
  const uint16_t rb = riv_bits_for(217);
  ASSERT_GT(nr_dci11_resolver_init_fdra(&r, 0, 217, rb, NR_DCI11_TDA_UNKNOWN, 49), 0);
  for (uint64_t p : kPci415Dci11) nr_dci11_resolver_observe(&r, nr_dci_bits_from_u64(p));
  int truth = -1, live_type1 = 0;
  for (int i = 0; i < r.n_hyp; i++) {
    if (!r.alive[i]) continue;
    live_type1 += r.off[i].fdra_mode == NR_FDRA_TYPE1;
    if (r.off[i].fdra_mode == NR_FDRA_TYPE0_CFG1 && r.off[i].riv == 2 && r.off[i].tda == 16 && r.off[i].mcs == 20
        && r.off[i].ant_ports == 42 && r.off[i].ant_ports_bits == 4)
      truth = i;
  }
  EXPECT_EQ(live_type1, 0) << "every type-1 layout reads an impossible RIV on the full-band DCIs";
  ASSERT_GE(truth, 0) << "the field-statistics layout must be alive";
  EXPECT_EQ(r.impossible[truth], 0u);
  std::cerr << "[ MEASURED ] PCI 415 payloads: " << r.n_alive << " live of " << r.n_hyp << " layouts\n";
  // + HARQ-sequence evidence: the same payloads, consecutive for one RNTI
  static nr_dci11_resolver_t h;
  ASSERT_GT(nr_dci11_resolver_init_fdra(&h, 0, 217, rb, NR_DCI11_TDA_UNKNOWN, 49), 0);
  for (uint64_t p : kPci415Dci11) nr_dci11_resolver_observe_rnti(&h, nr_dci_bits_from_u64(p), 0x8f5a);
  int h_truth = -1;
  for (int i = 0; i < h.n_hyp; i++)
    if (h.alive[i] && h.off[i].fdra_mode == NR_FDRA_TYPE0_CFG1 && h.off[i].riv == 2 && h.off[i].tda == 16
        && h.off[i].mcs == 20 && h.off[i].ant_ports == 42 && h.off[i].ant_ports_bits == 4)
      h_truth = i;
  ASSERT_GE(h_truth, 0) << "HARQ-sequence pruning must keep the truth";
  for (int i = 0; i < h.n_hyp; i++)
    if (h.alive[i]) EXPECT_EQ(h.off[i].mcs, 20) << "every survivor reads MCS where the HARQ counter says it is";
  std::cerr << "[ MEASURED ] PCI 415 payloads + HARQ sequence: " << h.n_alive << " live of " << h.n_hyp << " layouts\n";
}

#include "nr_dci11_space_test.inc"
