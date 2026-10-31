#include <atomic>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

extern "C" {
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_bank.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, const int, const char *, const int) { abort(); }
}

namespace {
nr_pdcch_blind_monitor_cfg_t config(int offset)
{
  nr_pdcch_blind_monitor_cfg_t c{};
  c.bwp_size = 106;
  c.coreset_rb_offset = offset;
  c.coreset_freq_domain = 4;
  c.coreset_duration = 2;
  c.dci_length_override = 47;
  return c;
}

class CoresetBank : public ::testing::Test {
 protected:
  void TearDown() override {
    nr_pdcch_coreset_bank_set_remove_hook(nullptr, nullptr);
    while (nr_pdcch_coreset_bank_count())
      EXPECT_EQ(nr_pdcch_coreset_bank_remove(0), 0);
  }
  int add(int offset, uint16_t owner) {
    auto c = config(offset);
    return nr_pdcch_coreset_bank_add(&c, owner);
  }
};
}

TEST_F(CoresetBank, IdleCellNeverDemotes)
{
  ASSERT_EQ(add(0, 0x1234), 0);
  nr_pdcch_coreset_bank_note_accept(0, 100);
  nr_pdcch_coreset_bank_tick(1000, false, 50, 300);
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(0)->state, NR_CORESET_VERIFIED);
}

TEST_F(CoresetBank, SingleBankSib1AbsentOutsideOccupancyGoesStale)
{
  ASSERT_EQ(add(0, 0x1234), 0); // no CORESET#0 and no second bank
  nr_pdcch_coreset_bank_note_accept(0, 100);
  EXPECT_FALSE(nr_pdcch_coreset_bank_occupancy_outside(6, 0));
  EXPECT_TRUE(nr_pdcch_coreset_bank_occupancy_outside(30, 0));
  nr_pdcch_coreset_bank_tick(151, nr_pdcch_coreset_bank_occupancy_outside(30, 0), 50, 300);
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(0)->state, NR_CORESET_STALE);
}

TEST_F(CoresetBank, SingleBankSib1AbsentIdleNeverDemotes)
{
  ASSERT_EQ(add(0, 0x1234), 0);
  nr_pdcch_coreset_bank_note_accept(0, 100);
  nr_pdcch_coreset_bank_tick(1000, false, 50, 300);
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(0)->state, NR_CORESET_VERIFIED);
}

TEST_F(CoresetBank, StaleWhenOtherTrafficVisible)
{
  ASSERT_EQ(add(0, 0x1234), 0);
  nr_pdcch_coreset_bank_note_accept(0, 100);
  nr_pdcch_coreset_bank_tick(151, true, 50, 300);
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(0)->state, NR_CORESET_STALE);
}

TEST_F(CoresetBank, StaleReverifiedReturnsVerified)
{
  ASSERT_EQ(add(0, 0x1234), 0);
  nr_pdcch_coreset_bank_note_accept(0, 100);
  nr_pdcch_coreset_bank_tick(151, true, 50, 300);
  nr_pdcch_coreset_bank_note_accept(0, 160);
  nr_pdcch_coreset_bank_note_dci(0, 160, 0x1234, 0x1111);
  nr_pdcch_coreset_bank_note_dci(0, 161, 0x1234, 0x1111);
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(0)->state, NR_CORESET_STALE);
  nr_pdcch_coreset_bank_note_dci(0, 162, 0x1234, 0x2222);
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(0)->state, NR_CORESET_VERIFIED);
}

TEST_F(CoresetBank, StaleRemovedAfterTRemove)
{
  ASSERT_EQ(add(0, 0x1234), 0);
  nr_pdcch_coreset_bank_note_accept(0, 100);
  nr_pdcch_coreset_bank_tick(151, true, 50, 300);
  nr_pdcch_coreset_bank_tick(451, true, 50, 300);
  EXPECT_EQ(nr_pdcch_coreset_bank_count(), 0);
}

TEST_F(CoresetBank, RemoveCompactsAndReopensKeyedContexts)
{
  ASSERT_EQ(add(0, 0x1234), 0);
  ASSERT_EQ(add(24, 0x5678), 1);
  struct State { int calls = 0; int offset = -1; bool length_live = true; bool td_live = true; } state;
  nr_pdcch_coreset_bank_set_remove_hook([](const nr_pdcch_blind_monitor_cfg_t *c, void *p) {
    auto *s = static_cast<State *>(p);
    ++s->calls;
    s->offset = c->coreset_rb_offset;
    if (c->coreset_rb_offset == 0) {
      s->length_live = false;
      s->td_live = false;
    }
  }, &state);
  EXPECT_EQ(nr_pdcch_coreset_bank_remove(0), 0);
  EXPECT_EQ(state.calls, 1);
  EXPECT_EQ(state.offset, 0);
  EXPECT_FALSE(state.length_live);
  EXPECT_FALSE(state.td_live);
  ASSERT_EQ(nr_pdcch_coreset_bank_count(), 1);
  EXPECT_EQ(nr_pdcch_coreset_bank_cfg(0)->coreset_rb_offset, 24);
}

TEST_F(CoresetBank, RemovalWaitsForActiveDispatcher)
{
  ASSERT_EQ(add(0, 0x1234), 0);
  nr_pdcch_coreset_bank_dispatch_enter();
  const auto *borrowed = nr_pdcch_coreset_bank_cfg(0);
  ASSERT_EQ(nr_pdcch_coreset_bank_remove(0), 0);
  EXPECT_EQ(borrowed->coreset_rb_offset, 0);
  EXPECT_EQ(nr_pdcch_coreset_bank_state(0), NR_CORESET_REMOVED);
  nr_pdcch_coreset_bank_dispatch_leave();
  EXPECT_EQ(nr_pdcch_coreset_bank_count(), 0);
}

TEST_F(CoresetBank, ConcurrentNotesAndTicksStayConsistent)
{
  ASSERT_EQ(add(0, 0x1234), 0);
  nr_pdcch_coreset_bank_note_accept(0, 1);
  std::thread a([] { for (uint64_t i = 2; i < 10000; ++i) nr_pdcch_coreset_bank_note_accept(0, i); });
  std::thread b([] { for (uint64_t i = 2; i < 10000; ++i) nr_pdcch_coreset_bank_tick(i, false, 50, 300); });
  a.join(); b.join();
  ASSERT_EQ(nr_pdcch_coreset_bank_count(), 1);
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(0)->state, NR_CORESET_VERIFIED);
}

int main(int argc, char **argv)
{
  logInit();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

TEST_F(CoresetBank, SingleSpuriousOccupancyHitNeverDemotes) {
  ASSERT_EQ(add(0,0x1234),0);
  nr_pdcch_coreset_bank_note_accept(0,100);
  uint8_t history=0;
  for (int i=0;i<20;++i) {
    const bool elsewhere=nr_pdcch_coreset_bank_occupancy_sample(&history,i==0);
    EXPECT_FALSE(elsewhere);
    nr_pdcch_coreset_bank_tick(1000+i,elsewhere,50,300);
    EXPECT_EQ(nr_pdcch_coreset_bank_state(0),NR_CORESET_VERIFIED);
  }
}

TEST_F(CoresetBank, RepeatedOccupancySamplesDemote) {
  ASSERT_EQ(add(0,0x1234),0);
  nr_pdcch_coreset_bank_note_accept(0,100);
  uint8_t history=0;
  EXPECT_FALSE(nr_pdcch_coreset_bank_occupancy_sample(&history,true));
  EXPECT_FALSE(nr_pdcch_coreset_bank_occupancy_sample(&history,false));
  EXPECT_FALSE(nr_pdcch_coreset_bank_occupancy_sample(&history,true));
  const bool elsewhere=nr_pdcch_coreset_bank_occupancy_sample(&history,true);
  ASSERT_TRUE(elsewhere);
  nr_pdcch_coreset_bank_tick(1000,elsewhere,50,300);
  EXPECT_EQ(nr_pdcch_coreset_bank_state(0),NR_CORESET_STALE);
  for (int i=0;i<8;++i) nr_pdcch_coreset_bank_occupancy_sample(&history,false);
  EXPECT_FALSE(nr_pdcch_coreset_bank_occupancy_sample(&history,true));
}

TEST_F(CoresetBank, AcceptEveryOccasionAcrossSfnWrapStaysVerified) {
  ASSERT_EQ(add(0,0x1234),0);
  constexpr uint64_t wrap=1024*20; // 10.24 seconds, mu=1
  nr_pdcch_coreset_bank_tick(wrap-10,false,10000,60000);
  for (uint64_t mono=wrap-9;mono<wrap+20000;++mono) {
    nr_pdcch_coreset_bank_note_accept(0,mono);
    nr_pdcch_coreset_bank_note_dci(0,mono,0x1234,mono);
    nr_pdcch_coreset_bank_tick(mono,true,10000,60000);
    ASSERT_EQ(nr_pdcch_coreset_bank_state(0),NR_CORESET_VERIFIED);
  }
}

TEST_F(CoresetBank, HintConfirmedRestoresTrusted) {
  if (!nr_cfg_reconf_enabled()) GTEST_SKIP();
  nr_cfg_epoch_reset();
  ASSERT_EQ(add(0, 0x1234), 0);
  nr_cfg_epoch_note_rnti_reopened(0x1111, true, 100);
  nr_cfg_epoch_note_rnti_reopened(0x2222, true, 101);
  nr_cfg_epoch_drain();
  EXPECT_EQ(nr_pdcch_coreset_bank_state(0), NR_CORESET_STALE);
  nr_pdcch_coreset_bank_note_dci(0, 160, 0x1234, 111);
  EXPECT_EQ(nr_pdcch_coreset_bank_state(0), NR_CORESET_STALE);
  nr_pdcch_coreset_bank_note_dci(0, 161, 0x1234, 222);
  EXPECT_EQ(nr_pdcch_coreset_bank_state(0), NR_CORESET_VERIFIED);
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(0)->verified_epoch, nr_cfg_epoch_current());
}
TEST_F(CoresetBank, HardResetNeverReusesOldIdentityState) {
  if (!nr_cfg_reconf_enabled()) GTEST_SKIP();
  nr_cfg_epoch_reset();
  nr_cfg_epoch_note_identity(1, 100, 200);
  ASSERT_EQ(add(0, 0x1234), 0);
  nr_cfg_epoch_note_identity(2, 100, 200);
  nr_cfg_epoch_drain();
  EXPECT_EQ(nr_pdcch_coreset_bank_count(), 0);
  EXPECT_FALSE(nr_pdcch_coreset_bank_has_owner(0x1234));
}

TEST_F(CoresetBank, IdleCellEpochStaleNeverRemoved) {
  if (!nr_cfg_reconf_enabled()) GTEST_SKIP();
  nr_cfg_epoch_reset();
  ASSERT_EQ(add(0, 0x1234), 0);
  nr_cfg_epoch_note_continuity_loss();
  nr_cfg_epoch_drain();
  for (uint64_t slot : {100u, 1000u, 10000u}) {
    nr_pdcch_coreset_bank_tick(slot, false, 50, 300);
    EXPECT_EQ(nr_pdcch_coreset_bank_state(0), NR_CORESET_STALE);
    EXPECT_EQ(nr_pdcch_coreset_bank_count(), 1);
  }
  nr_pdcch_coreset_bank_tick(10001, true, 50, 300);
  EXPECT_EQ(nr_pdcch_coreset_bank_state(0), NR_CORESET_STALE);
  nr_pdcch_coreset_bank_tick(10301, true, 50, 300);
  EXPECT_EQ(nr_pdcch_coreset_bank_count(), 0);
}
TEST_F(CoresetBank, ShortGapSoftDoesNotReverifyAll) {
  if (!nr_cfg_reconf_enabled()) GTEST_SKIP();
  nr_cfg_epoch_reset();
  ASSERT_EQ(add(0, 0x1234), 0);
  for (int cause = 0; cause < 3; ++cause) {
    if (cause == 0) nr_cfg_epoch_note_continuity_loss_samples(9, 1);
    if (cause == 1) nr_cfg_epoch_note_csirs_map_change();
    if (cause == 2) nr_cfg_epoch_note_bwp_change();
    nr_cfg_epoch_drain();
    EXPECT_EQ(nr_pdcch_coreset_bank_state(0), NR_CORESET_VERIFIED);
  }
}

TEST_F(CoresetBank, LazyLookupCannotMissHardBeforeSoft) {
  if (!nr_cfg_reconf_enabled()) GTEST_SKIP();
  nr_cfg_epoch_reset();
  ASSERT_EQ(add(0, 0x1234), 0);
  nr_cfg_epoch_note_continuity_loss();
  nr_cfg_epoch_note_bwp_change();
  nr_pdcch_coreset_bank_dispatch_enter();
  EXPECT_EQ(nr_pdcch_coreset_bank_state(0), NR_CORESET_STALE);
  nr_pdcch_coreset_bank_dispatch_leave();
  nr_cfg_epoch_drain();
}

// ---- B1/B2 downstream PDSCH certification (2026-10-30) ----
namespace {
nr_pdcch_blind_monitor_cfg_t cfgn(int nid, int offset, int symbol = 0)
{
  auto c = config(offset);
  c.coreset_pdcch_dmrs_scrambling_id = nid;
  c.ss_first_symbol = symbol;
  return c;
}
constexpr uint64_t kSlot0 = 4000000000ull;
constexpr uint64_t kSlot1 = kSlot0 + 10000000ull; // tests below start after the first backoff test's clock // beyond every slot other tests of this binary used (the bank clock is monotonic)
void grants(int idx, uint16_t rnti, int n, int passes)
{
  for (int i = 0; i < n; ++i) {
    nr_pdcch_coreset_bank_note_grant(idx, rnti);
    if (i < passes) nr_pdcch_coreset_bank_note_pdsch(rnti, true, true);
    else nr_pdcch_coreset_bank_note_pdsch(rnti, true, false);
  }
}
}

TEST(CoresetCert, WilsonAndZeroPassRuleArePure)
{
  double lo, hi;
  nr_pdcch_cert_wilson(0, 0, NR_PDCCH_CERT_Z, &lo, &hi);
  EXPECT_EQ(lo, 0.0); EXPECT_EQ(hi, 1.0);
  nr_pdcch_cert_wilson(10, 100, NR_PDCCH_CERT_Z, &lo, &hi);
  EXPECT_GT(lo, 0.03); EXPECT_LT(lo, 0.10); EXPECT_GT(hi, 0.10); EXPECT_LT(hi, 0.25);
  double lo2, hi2;
  nr_pdcch_cert_wilson(100, 1000, NR_PDCCH_CERT_Z, &lo2, &hi2);
  EXPECT_GT(lo2, lo); EXPECT_LT(hi2, hi); // more data tightens both bounds
  nr_pdcch_cert_wilson(0, 200, NR_PDCCH_CERT_Z, &lo, &hi);
  EXPECT_EQ(lo, 0.0);
  EXPECT_NEAR(hi, NR_PDCCH_CERT_Z * NR_PDCCH_CERT_Z / (200 + NR_PDCCH_CERT_Z * NR_PDCCH_CERT_Z), 1e-9);
  EXPECT_FALSE(nr_pdcch_cert_zero_pass_significant(1000, 0.0, 1e-3)); // no competitor rate, no verdict
  EXPECT_FALSE(nr_pdcch_cert_zero_pass_significant(0, 0.5, 1e-3));
  EXPECT_FALSE(nr_pdcch_cert_zero_pass_significant(100, 0.05, 1e-3)); // 0.95^100 = 0.006
  EXPECT_TRUE(nr_pdcch_cert_zero_pass_significant(135, 0.05, 1e-3));  // 0.95^135 = 0.001
  EXPECT_TRUE(nr_pdcch_cert_zero_pass_significant(1, 1.0, 1e-3));
}

TEST_F(CoresetBank, NoPassEntryRetiredOnlyWhenCompetitorRateExcludesIt)
{
  auto a = cfgn(101, 0), b = cfgn(101, 0, 1);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&a, 0x1001), 0);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&b, 0x1002), 1);
  grants(0, 0x1001, 200, 20); // 10 % passes
  grants(1, 0x1002, 60, 0);   // too few grants to exclude 0 % against the lower bound of 10 %
  EXPECT_EQ(nr_pdcch_coreset_bank_assess(kSlot0), 0);
  EXPECT_EQ(nr_pdcch_coreset_bank_count(), 2);
  grants(1, 0x1002, 200, 0);
  EXPECT_EQ(nr_pdcch_coreset_bank_assess(kSlot0 + 10), 1);
  ASSERT_EQ(nr_pdcch_coreset_bank_count(), 1);
  EXPECT_EQ(nr_pdcch_coreset_bank_cfg(0)->ss_first_symbol, 0);
  EXPECT_TRUE(nr_pdcch_coreset_bank_certified(0));
}

TEST_F(CoresetBank, LoneEntryAndZeroBothNeverRetired)
{
  auto a = cfgn(111, 0), b = cfgn(112, 24);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&a, 0x1101), 0);
  grants(0, 0x1101, 5000, 0); // alone: nothing to compare with
  EXPECT_EQ(nr_pdcch_coreset_bank_assess(kSlot0 + 20), 0);
  EXPECT_FALSE(nr_pdcch_coreset_bank_certified(0));
  EXPECT_FALSE(nr_pdcch_coreset_bank_nid_certified(111));
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&b, 0x1102), 1);
  grants(1, 0x1102, 5000, 0); // two entries, neither ever passes: no competitor rate, both stay
  EXPECT_EQ(nr_pdcch_coreset_bank_assess(kSlot0 + 21), 0);
  EXPECT_EQ(nr_pdcch_coreset_bank_count(), 2);
}

TEST_F(CoresetBank, RetiredFootprintBacksOffExponentiallyAndEpochClears)
{
  auto a = cfgn(121, 0), b = cfgn(121, 0, 1);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&a, 0x1201), 0);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&b, 0x1202), 1);
  grants(0, 0x1201, 300, 60);
  grants(1, 0x1202, 300, 0);
  const uint64_t t1 = kSlot0 + 1000;
  nr_pdcch_coreset_bank_tick(t1, false, 1000000, 3000000);
  ASSERT_EQ(nr_pdcch_coreset_bank_count(), 1);
  EXPECT_TRUE(nr_pdcch_coreset_bank_readmit_blocked(121, 0, 4));
  EXPECT_FALSE(nr_pdcch_coreset_bank_readmit_blocked(999, 0, 4)); // other nID
  EXPECT_EQ(nr_pdcch_coreset_bank_add(&b, 0x1202), -1);            // deferred, not locked out forever
  const uint64_t span = t1 - 1; // born_slot was the bank clock at add time
  (void)span;
  // the backoff is as long as the evidence took (>= 1 slot) and ends: far in the future the footprint is free again
  nr_pdcch_coreset_bank_tick(t1 + 1000000ull, false, 1000000000ull, 3000000000ull);
  EXPECT_FALSE(nr_pdcch_coreset_bank_readmit_blocked(121, 0, 4));
  const int again = nr_pdcch_coreset_bank_add(&b, 0x1202);
  ASSERT_GE(again, 0);
  // second retirement of the same footprint: the park doubles
  const uint64_t t2 = t1 + 1000000ull + 5000;
  grants(again, 0x1202, 300, 0);
  nr_pdcch_coreset_bank_tick(t2, false, 1000000000ull, 3000000000ull);
  ASSERT_EQ(nr_pdcch_coreset_bank_count(), 1);
  const uint64_t park2 = (t2 - (t1 + 1000000ull)) ; // evidence time of the second round
  (void)park2;
  EXPECT_TRUE(nr_pdcch_coreset_bank_readmit_blocked(121, 0, 4));
}

TEST_F(CoresetBank, SecondRetirementParksTwiceAsLong)
{
  auto a = cfgn(131, 0), b = cfgn(131, 0, 1);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&a, 0x1301), 0);
  nr_pdcch_coreset_bank_tick(kSlot1 + 100000, false, 1u << 30, 1u << 30); // bank clock
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&b, 0x1302), 1);                   // born at +100000
  grants(0, 0x1301, 300, 60);
  grants(1, 0x1302, 300, 0);
  nr_pdcch_coreset_bank_tick(kSlot1 + 101000, false, 1u << 30, 1u << 30); // evidence took 1000 slots -> parked until +102000
  EXPECT_TRUE(nr_pdcch_coreset_bank_readmit_blocked(131, 0, 4));
  nr_pdcch_coreset_bank_tick(kSlot1 + 101999, false, 1u << 30, 1u << 30);
  EXPECT_TRUE(nr_pdcch_coreset_bank_readmit_blocked(131, 0, 4));
  nr_pdcch_coreset_bank_tick(kSlot1 + 102000, false, 1u << 30, 1u << 30);
  EXPECT_FALSE(nr_pdcch_coreset_bank_readmit_blocked(131, 0, 4));
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&b, 0x1302), 1);                   // born at +102000
  grants(1, 0x1302, 300, 0);
  nr_pdcch_coreset_bank_tick(kSlot1 + 103000, false, 1u << 30, 1u << 30); // span 1000 again, second retirement: 2 * 1000
  EXPECT_TRUE(nr_pdcch_coreset_bank_readmit_blocked(131, 0, 4));
  nr_pdcch_coreset_bank_tick(kSlot1 + 104999, false, 1u << 30, 1u << 30);
  EXPECT_TRUE(nr_pdcch_coreset_bank_readmit_blocked(131, 0, 4));
  nr_pdcch_coreset_bank_tick(kSlot1 + 105000, false, 1u << 30, 1u << 30);
  EXPECT_FALSE(nr_pdcch_coreset_bank_readmit_blocked(131, 0, 4));
}

TEST_F(CoresetBank, ReconfigurationClearsTheRetiredList)
{
  if (!nr_cfg_reconf_enabled()) GTEST_SKIP();
  nr_cfg_epoch_reset();
  auto a = cfgn(141, 0), b = cfgn(141, 0, 1);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&a, 0x1401), 0);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&b, 0x1402), 1);
  grants(0, 0x1401, 300, 60);
  grants(1, 0x1402, 300, 0);
  nr_pdcch_coreset_bank_tick(kSlot1 + 300000, false, 1u << 30, 1u << 30);
  ASSERT_TRUE(nr_pdcch_coreset_bank_readmit_blocked(141, 0, 4));
  nr_cfg_epoch_note_continuity_loss();
  nr_cfg_epoch_drain();
  EXPECT_FALSE(nr_pdcch_coreset_bank_readmit_blocked(141, 0, 4)); // a CORESET that is legitimately new after a reconfiguration is not locked out
  EXPECT_GE(nr_pdcch_coreset_bank_add(&b, 0x1402), 0);
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(nr_pdcch_coreset_bank_count() - 1)->grants, 0u); // evidence restarts
}

TEST_F(CoresetBank, SameNidSameRbsOtherSymbolIsOnProbationUntilItsOwnPass)
{
  auto a = cfgn(151, 0, 0), twin = cfgn(151, 0, 1), other = cfgn(152, 24, 0);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&a, 0x1501), 0);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&twin, 0x1502), 1);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&other, 0x1503), 2);
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(0)->probation, 0);
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(1)->probation, 1);
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(2)->probation, 0); // other nID: independent CORESET
  grants(0, 0x1501, 100, 10);
  EXPECT_FALSE(nr_pdcch_coreset_bank_certified(1));
  grants(1, 0x1502, 1, 1); // its own TB pass: certified
  EXPECT_TRUE(nr_pdcch_coreset_bank_certified(1));
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(1)->probation, 0);
}

TEST_F(CoresetBank, ProbationTwinWithoutPassIsRetiredAndNotReadmittedAtOnce)
{
  auto a = cfgn(161, 0, 0), twin = cfgn(161, 0, 1);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&a, 0x1601), 0);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&twin, 0x1602), 1);
  grants(0, 0x1601, 400, 30);    // the real CORESET: 7.5 % pass
  grants(1, 0x1602, 905, 0);     // measured: bank1-only DCIs, 0 TB passes
  nr_pdcch_coreset_bank_tick(kSlot1 + 500000, false, 1u << 30, 1u << 30);
  ASSERT_EQ(nr_pdcch_coreset_bank_count(), 1);
  EXPECT_EQ(nr_pdcch_coreset_bank_cfg(0)->ss_first_symbol, 0);
  EXPECT_EQ(nr_pdcch_coreset_bank_add(&twin, 0x1602), -1);
  EXPECT_TRUE(nr_pdcch_coreset_bank_nid_certified(161)); // the surviving entry certifies the nID
}

TEST_F(CoresetBank, PassesAreCreditedToTheEntryThatProducedTheGrant)
{
  auto a = cfgn(171, 0), b = cfgn(172, 24);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&a, 0x1701), 0);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&b, 0x1702), 1);
  nr_pdcch_coreset_bank_note_grant(0, 0x1701);
  nr_pdcch_coreset_bank_note_grant(1, 0x1702);
  nr_pdcch_coreset_bank_note_pdsch(0x1702, true, true);
  nr_pdcch_coreset_bank_note_pdsch(0x1701, false, true); // a common-class outcome is not evidence for a dedicated CORESET
  nr_pdcch_coreset_bank_note_pdsch(0x1701, true, false);
  nr_pdcch_coreset_bank_note_pdsch(0x1999, true, true);  // an RNTI no entry produced
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(0)->passes, 0u);
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(1)->passes, 1u);
  // compaction moves entries but not their credit
  ASSERT_EQ(nr_pdcch_coreset_bank_remove(0), 0);
  nr_pdcch_coreset_bank_note_pdsch(0x1702, true, true);
  EXPECT_EQ(nr_pdcch_coreset_bank_entry(0)->passes, 2u);
}

TEST_F(CoresetBank, DifferentNidEntryIsNeverRetiredForItsPdschYield)
{
  auto a = cfgn(181, 0), b = cfgn(182, 24); // two genuinely different CORESETs of one cell
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&a, 0x1801), 0);
  ASSERT_EQ(nr_pdcch_coreset_bank_add(&b, 0x1802), 1);
  grants(0, 0x1801, 1000, 200);
  grants(1, 0x1802, 5000, 0);
  EXPECT_EQ(nr_pdcch_coreset_bank_assess(kSlot1 + 700000), 0);
  EXPECT_EQ(nr_pdcch_coreset_bank_count(), 2);
}
