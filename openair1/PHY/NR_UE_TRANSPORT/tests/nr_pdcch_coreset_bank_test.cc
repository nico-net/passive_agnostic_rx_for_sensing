#include <atomic>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

extern "C" {
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_bank.h"
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
