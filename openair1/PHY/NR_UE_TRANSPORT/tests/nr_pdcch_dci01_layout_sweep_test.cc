#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
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
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();  // ~4.3 MB: never on the stack
  nr_dci11_resolver_t &r = *r_heap;
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
    nr_dci11_resolver_observe(&r,nr_dci_bits_from_u64( p));
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
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();  // ~4.3 MB: never on the stack
  nr_dci11_resolver_t &r = *r_heap;
  EXPECT_EQ(nr_dci_resolver_init_from_offsets(&r, 273, bad, 2), 0);
  nr_dci11_offsets_t zero[1]{};
  EXPECT_EQ(nr_dci_resolver_init_from_offsets(&r, 273, zero, 1), 0);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// ---- FDRA modes: PUSCH RA type 0 and dynamicSwitch (full-running-agnosticity Task 10) -----------
// 106 PRB at CRB 0: RIV 13, config1 N_RBG 14, config2 N_RBG 7 -- five distinct FDRA widths.
static void put_bits(uint64_t &p, uint16_t total, uint16_t off, uint8_t nb, uint64_t v)
{
  const int sh = total - off - nb;
  p &= ~(((1ULL << nb) - 1ULL) << sh);
  p |= (v & ((1ULL << nb) - 1ULL)) << sh;
}

static uint32_t ul_fdra_field(const nr_dci11_offsets_t &t, unsigned &seed, bool want_type0)
{
  const uint32_t riv = rand_r(&seed) % (106 * 107 / 2);
  const uint32_t bitmap = t.n_rbg ? 1 + rand_r(&seed) % ((1u << t.n_rbg) - 1) : 0;
  if (t.fdra_mode == NR_FDRA_TYPE1) return riv;
  if (t.fdra_mode <= NR_FDRA_TYPE0_CFG2) return bitmap;
  const int w = t.n_rbg > t.riv_bits ? t.n_rbg : t.riv_bits;
  return want_type0 ? bitmap : ((1u << w) | riv);
}

static uint64_t ul_payload(const nr_dci11_offsets_t &t, unsigned &seed, uint32_t field)
{
  uint64_t p = 0;
  for (int b = 0; b < t.total; b++) p |= (uint64_t)(rand_r(&seed) & 1) << b;
  put_bits(p, t.total, t.riv, (uint8_t)(t.tda - t.riv), field);
  put_bits(p, t.total, t.mcs, 5, rand_r(&seed) % 28);
  put_bits(p, t.total, t.rv, 2, 0);
  return p;
}

// Arm the non-type-1 0_1 layouts exactly as the RT receiver does once the booked type-1 PUSCH failed:
// appended to a live resolver, or -- when no type-1 layout fits the length at all -- the whole initial set.
static int ul_arm(nr_dci11_resolver_t &r, bool init, uint16_t rb, uint8_t tb, uint16_t len, uint16_t bwp_start, uint16_t bwp)
{
  std::vector<nr_dci01_layout_t> c(NR_DCI11_LAYOUT_MAX);
  std::vector<nr_dci11_offsets_t> o(NR_DCI11_LAYOUT_MAX);
  int n = 0;
  for (uint8_t m = NR_FDRA_TYPE0_CFG1; m <= NR_FDRA_DYN_CFG2; m++) {
    const int k = nr_dci01_layout_enumerate_mode(rb, tb, len, bwp_start, bwp, m, c.data() + n, o.data() + n,
                                                 NR_DCI11_LAYOUT_MAX - n);
    n += k > 0 ? k : 0;
  }
  return init ? nr_dci_resolver_init_from_offsets(&r, bwp, o.data(), n) : nr_dci_resolver_append_offsets(&r, o.data(), n);
}

static void resolve_ul_fdra_mode(uint8_t mode)
{
  const uint16_t rb = riv_bits_for(106);
  nr_dci01_layout_t l{};
  l.pre_riv = 0; l.pre_mcs = 0; l.pre_ant = 7; l.ant_ports = 2; l.post_ant = 3;
  l.fdra_mode = mode;
  l.n_rbg = mode == NR_FDRA_TYPE1 ? 0 : (uint8_t)nr_rbg_count(0, 106, nr_fdra_rbg_size(mode, 106));
  nr_dci11_offsets_t t{};
  ASSERT_TRUE(nr_dci01_layout_offsets(&l, rb, 2, &t));
  EXPECT_EQ(t.tda - t.riv, nr_fdra_bits(mode, l.n_rbg, rb));
  // Staged like the receiver: type 1 first, the other modes appended.
  std::vector<nr_dci01_layout_t> c(NR_DCI11_LAYOUT_MAX);
  std::vector<nr_dci11_offsets_t> o(NR_DCI11_LAYOUT_MAX);
  const int n1 = nr_dci01_layout_enumerate(rb, 2, t.total, c.data(), o.data(), NR_DCI11_LAYOUT_MAX);
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();
  nr_dci11_resolver_t &r = *r_heap;
  if (n1 > 0)
    ASSERT_EQ(nr_dci_resolver_init_from_offsets(&r, 106, o.data(), n1), n1);
  else
    ASSERT_NE(mode, NR_FDRA_TYPE1);  // no type-1 layout fits this length: the receiver arms at once
  const int added = mode == NR_FDRA_TYPE1 ? 0 : ul_arm(r, n1 <= 0, rb, 2, t.total, 0, 106);
  const int n = r.n_hyp;
  ASSERT_LT(n, NR_DCI11_LAYOUT_MAX);
  // the truth's index in the combined list: the type-1 enumeration, then the appended modes in order
  int truth = -1;
  std::vector<nr_dci01_layout_t> all(NR_DCI11_LAYOUT_MAX);
  const int na = nr_dci01_layout_enumerate_fdra(rb, 2, t.total, 0, 106, all.data(), nullptr, NR_DCI11_LAYOUT_MAX);
  for (int i = 0; i < na; i++)
    if (!memcmp(&all[i], &l, sizeof(l))) truth = i;
  ASSERT_GE(truth, 0) << "the constructed layout was not enumerated";
  ASSERT_EQ(r.off[truth].fdra_mode, mode);
  unsigned seed = 57u + mode;
  for (int i = 0; i < 800; i++) nr_dci11_resolver_observe(&r,nr_dci_bits_from_u64( ul_payload(t, seed, ul_fdra_field(t, seed, i & 1))));
  ASSERT_TRUE(r.alive[truth]) << "stage 1 deleted the true layout";
  int w = -1;
  for (int i = 0; i < 400000 && w < 0; i++) {
    nr_dci11_offsets_t pick{};
    const int idx = nr_dci11_resolver_next(&r, &pick);
    ASSERT_GE(idx, 0);
    w = nr_dci11_resolver_feed(&r, idx, idx == truth && (double)rand_r(&seed) / RAND_MAX < 0.40);
  }
  ASSERT_EQ(w, truth);
  EXPECT_EQ(r.off[w].fdra_mode, mode);             // (a)
  EXPECT_EQ(r.off[w].mcs, t.mcs);                  // (b)
  EXPECT_EQ(r.off[w].rv, t.rv);
  EXPECT_EQ(r.off[w].ant_ports, t.ant_ports);
  // (c): PRB list of the resolved read vs the constructed allocation
  std::vector<uint16_t> want(NR_PRB_SET_MAX), got(NR_PRB_SET_MAX);
  const bool type0 = mode != NR_FDRA_TYPE1;
  uint32_t field;
  int nw;
  if (type0) {
    field = (1u << (l.n_rbg - 1)) | 0x3u;
    nw = nr_ra_type0_prbs(field, 0, 106, nr_fdra_rbg_size(mode, 106), want.data(), NR_PRB_SET_MAX);
  } else {
    field = 106 * (10 - 1) + 30;  // RIV of start 30, length 10
    for (nw = 0; nw < 10; nw++) want[nw] = (uint16_t)(30 + nw);
  }
  const uint64_t p = ul_payload(t, seed, field);
  const nr_dci11_offsets_t &ro = r.off[w];
  const uint8_t nb = (uint8_t)(ro.tda - ro.riv);
  const uint32_t read = (uint32_t)((p >> (ro.total - ro.riv - nb)) & ((1ULL << nb) - 1ULL));
  const int ng = nr_fdra_prbs(read, ro.fdra_mode, ro.n_rbg, ro.riv_bits, 0, 106, got.data(), NR_PRB_SET_MAX, nullptr);
  ASSERT_EQ(ng, nw);
  for (int i = 0; i < nw; i++) EXPECT_EQ(got[i], want[i]) << "PRB " << i;
  std::cerr << "[ MEASURED ] DCI 0_1 fdra_mode " << (int)mode << ": " << n1 << " type-1 + " << added << " armed at len "
            << t.total << "\n";
}

TEST(Dci01Fdra, ResolvesType1) { resolve_ul_fdra_mode(NR_FDRA_TYPE1); }
TEST(Dci01Fdra, ResolvesType0Config1) { resolve_ul_fdra_mode(NR_FDRA_TYPE0_CFG1); }
TEST(Dci01Fdra, ResolvesType0Config2) { resolve_ul_fdra_mode(NR_FDRA_TYPE0_CFG2); }
TEST(Dci01Fdra, ResolvesDynamicSwitchConfig1) { resolve_ul_fdra_mode(NR_FDRA_DYN_CFG1); }
TEST(Dci01Fdra, ResolvesDynamicSwitchConfig2) { resolve_ul_fdra_mode(NR_FDRA_DYN_CFG2); }

TEST(Dci01Fdra, TypeZeroTruthWithConstantLeadingBitsKeepsType1AliveUntilTbCrcRefutesIt) {
  // Reviewer's I1 scene: type-0 truth, identifier + 3 UL-SUL/BWP bits constant zero, contiguous RBG runs.
  // Stage 1 CANNOT refute type 1 here (a type-1 window starting on the constant bits always reads an
  // in-range RIV), which is why the refusal is driven by the booked PUSCH's TB CRC and not by stage 1.
  const uint16_t rb = riv_bits_for(106);
  for (uint8_t mode : {NR_FDRA_TYPE0_CFG1, NR_FDRA_DYN_CFG1}) {
    nr_dci01_layout_t l{};
    l.pre_riv = 3; l.pre_mcs = 1; l.pre_ant = 9; l.ant_ports = 3; l.post_ant = 3; l.fdra_mode = mode;
    l.n_rbg = (uint8_t)nr_rbg_count(0, 106, nr_fdra_rbg_size(mode, 106));
    nr_dci11_offsets_t t{};
    ASSERT_TRUE(nr_dci01_layout_offsets(&l, rb, 2, &t));
    std::vector<nr_dci01_layout_t> c(NR_DCI11_LAYOUT_MAX);
    std::vector<nr_dci11_offsets_t> o(NR_DCI11_LAYOUT_MAX);
    const int n1 = nr_dci01_layout_enumerate(rb, 2, t.total, c.data(), o.data(), NR_DCI11_LAYOUT_MAX);
    ASSERT_GT(n1, 0);
    auto r_heap = std::make_unique<nr_dci11_resolver_t>();
    nr_dci11_resolver_t &r = *r_heap;
    ASSERT_EQ(nr_dci_resolver_init_from_offsets(&r, 106, o.data(), n1), n1);
    nr_dci11_resolver_set_tda_count(&r, 3);
    unsigned seed = 7;
    auto payload = [&]() {
      uint64_t p = 0;
      for (int b = 0; b < t.total; b++) p |= (uint64_t)(rand_r(&seed) & 1) << b;
      const int a = rand_r(&seed) % l.n_rbg, len = 1 + rand_r(&seed) % (l.n_rbg - a);
      put_bits(p, t.total, t.riv, (uint8_t)(t.tda - t.riv), ((1u << len) - 1) << (l.n_rbg - a - len));
      put_bits(p, t.total, t.tda, 2, rand_r(&seed) % 3);
      put_bits(p, t.total, t.mcs, 5, rand_r(&seed) % 28);
      put_bits(p, t.total, t.rv, 2, 0);
      put_bits(p, t.total, 0, 1 + 3, 0);  // identifier (UL = 0) + UL-SUL/BWP: constant zero
      return p;
    };
    for (int k = 0; k < 20000; k++) nr_dci11_resolver_observe(&r,nr_dci_bits_from_u64( payload()));
    const int type1_alive = r.n_alive;
    EXPECT_GT(type1_alive, 0) << "stage 1 refuted every type-1 layout -- then stage 1 COULD drive the refusal";
    // The oracle-class 0_1 PUSCH decodes nothing on this cell while DCI 0_0 PUSCH does (link healthy):
    // the verdict walks BOOK -> ARM -> REFUSE.
    nr_dci01_fdra_evidence_t ev{};
    for (int k = 0; k < NR_DCI11_FDRA_ARM_MIN_TRIALS - 1; k++) {
      nr_dci01_fdra_note(&ev, true, false);
      if ((k % 8) == 0) nr_dci01_fdra_note(&ev, false, true);  // a 0_0 pass: link health, NOT a type-1 pass
    }
    EXPECT_EQ(ev.t1_ok, 0u) << "a DCI 0_0 pass was counted as a 0_1 type-1 pass";
    EXPECT_EQ(nr_dci01_fdra_verdict(&ev, false, &r), NR_DCI01_FDRA_BOOK);
    nr_dci01_fdra_note(&ev, true, false);
    EXPECT_EQ(nr_dci01_fdra_verdict(&ev, false, &r), NR_DCI01_FDRA_ARM);
    EXPECT_EQ(nr_dci01_fdra_verdict(&ev, true, &r), NR_DCI01_FDRA_BOOK)
        << "armed but nothing non-type-1 alive: no reason to refuse";
    const int added = ul_arm(r, false, rb, 2, t.total, 0, 106);
    ASSERT_GT(added, 0);
    int truth = -1;
    for (int i = n1; i < r.n_hyp; i++)
      if (r.off[i].fdra_mode == mode && r.off[i].riv == t.riv && r.off[i].mcs == t.mcs && r.off[i].ant_ports == t.ant_ports
          && r.off[i].total == t.total && r.off[i].rv == t.rv && r.off[i].dmrs_init == t.dmrs_init)
        truth = i;
    ASSERT_GE(truth, 0) << "arming did not add the true layout";
    for (int k = 0; k < 20000; k++) nr_dci11_resolver_observe(&r,nr_dci_bits_from_u64( payload()));
    EXPECT_TRUE(r.alive[truth]) << "stage 1 deleted the armed truth";
    EXPECT_EQ(nr_dci01_fdra_verdict(&ev, true, &r), NR_DCI01_FDRA_REFUSE) << "the refusal did not fire on a type-0 cell";
    // Refusal applies to oracle-class 0_1 only: 0_0 and discovery grants stay booked; 1 in 64 is a probe.
    EXPECT_TRUE(nr_dci01_fdra_book(NR_DCI01_FDRA_REFUSE, false, 0, true)) << "a DCI 0_0 grant was refused";
    EXPECT_FALSE(nr_dci01_fdra_book(NR_DCI01_FDRA_REFUSE, true, 1, true));
    EXPECT_TRUE(nr_dci01_fdra_book(NR_DCI01_FDRA_REFUSE, true, NR_DCI01_FDRA_PROBE_EVERY, true)) << "no probe";
    EXPECT_TRUE(nr_dci01_fdra_book(NR_DCI01_FDRA_BOOK, true, 0, true));
    // Final review I3: without ISAC_UL_FDRA_REFUSE=1 a REFUSE verdict books every grant (would-refuse only).
    EXPECT_TRUE(nr_dci01_fdra_book(NR_DCI01_FDRA_REFUSE, true, 1, false)) << "refused although enforcement is off";
    // More 0_0 passes never flip it; one oracle pass ends it for good.
    for (int k = 0; k < 100; k++) nr_dci01_fdra_note(&ev, false, true);
    EXPECT_EQ(nr_dci01_fdra_verdict(&ev, true, &r), NR_DCI01_FDRA_REFUSE) << "0_0 passes disabled the refusal";
    nr_dci01_fdra_evidence_t proven = ev;
    nr_dci01_fdra_note(&proven, true, true);
    EXPECT_EQ(nr_dci01_fdra_verdict(&proven, true, &r), NR_DCI01_FDRA_BOOK) << "one 0_1 type-1 pass must end the refusal";
    // Link health is required: the same failing 0_1 reads with no recent 0_0 pass never refuse (dead link).
    nr_dci01_fdra_evidence_t dead{};
    for (int k = 0; k < 4 * NR_DCI11_FDRA_ARM_MIN_TRIALS; k++) nr_dci01_fdra_note(&dead, true, false);
    EXPECT_EQ(nr_dci01_fdra_verdict(&dead, false, &r), NR_DCI01_FDRA_BOOK) << "armed on a dead link";
    EXPECT_EQ(nr_dci01_fdra_verdict(&dead, true, &r), NR_DCI01_FDRA_BOOK) << "refused on a dead link";
    nr_dci01_fdra_evidence_t stale = dead;
    stale.link_ok = 1;
    stale.t1_try_at_link = 0;  // the link worked once, long before the 0_1 reads started failing
    EXPECT_EQ(nr_dci01_fdra_verdict(&stale, true, &r), NR_DCI01_FDRA_BOOK) << "stale link health accepted";
    int other = 0;
    for (int i = 0; i < r.n_hyp; i++) other += r.alive[i] && r.off[i].fdra_mode != NR_FDRA_TYPE1;
    std::cerr << "[ MEASURED ] UL type-0 truth mode " << (int)mode << " pre_riv 3: " << type1_alive << "/" << n1
              << " type-1 layouts survive stage 1; after arming " << other << " non-type-1 alive -> REFUSE\n";
  }
}

TEST(Dci01Fdra, EveryStageFitsTheCap) {
  // Bounded sweep: BWP 6..273 (aligned and worst-misaligned CRB start), lengths 28..62, UL TDA width 0..4
  // (the receiver fixes ONE width). The 0_1 stages are the type-1 set, then the armed set = modes 1..4
  // together; counts depend only on the FDRA width, so they are memoised on it.
  const int LMIN = 28, LMAX = 62, WMAX = 24;
  std::vector<int> memo((WMAX + 1) * (LMAX + 1) * 5, -1);
  std::vector<nr_dci01_layout_t> c(1 << 16);
  auto count = [&](int w, int L, int tb) {
    int &k = memo[(w * (LMAX + 1) + L) * 5 + tb];
    if (k < 0) k = nr_dci01_layout_enumerate((uint16_t)w, (uint8_t)tb, (uint16_t)L, c.data(), nullptr, (int)c.size());
    return k;
  };
  int worst1 = 0, worstA = 0, aN = 0, aL = 0;
  for (int N = 6; N <= 273; N++) {
    const int rb = riv_bits_for(N);
    for (int start : {0, 15})
      for (int L = LMIN; L <= LMAX; L++)
        for (int tb = 0; tb <= 4; tb++) {
          int armed = 0;
          for (int m = NR_FDRA_TYPE0_CFG1; m <= NR_FDRA_DYN_CFG2; m++) {
            const int P = nr_fdra_rbg_size(m, N);
            if (P == 0 || ((m == NR_FDRA_TYPE0_CFG2 || m == NR_FDRA_DYN_CFG2) && P == nr_fdra_rbg_size(m - 1, N)))
              continue;
            const int w = nr_fdra_bits(m, nr_rbg_count(start, N, P), rb);
            ASSERT_LE(w, WMAX);
            armed += count(w, L, tb);
          }
          const int t1 = count(rb, L, tb);
          if (t1 > worst1) worst1 = t1;
          if (armed > worstA) { worstA = armed; aN = N; aL = L; }
          EXPECT_LT(t1, NR_DCI01_LAYOUT_MAX);
          EXPECT_LT(t1 + armed, NR_DCI01_LAYOUT_MAX) << "N=" << N << " L=" << L << " tb=" << tb;
        }
  }
  std::cerr << "[ MEASURED ] DCI 0_1 stages: type-1 max " << worst1 << ", armed set (modes 1..4) max " << worstA << " at "
            << aN << " PRB / " << aL << " bits (cap " << NR_DCI01_LAYOUT_MAX << ")\n";
}
