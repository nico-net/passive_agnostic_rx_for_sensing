#include <gtest/gtest.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include "PHY/CODING/nrPolar_tools/nr_polar_dci_defs.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"
#include "PHY/NR_UE_TRANSPORT/nr_dci_bits.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_bank.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h"
#include "openair1/SIMULATION/NR_PHY/hidden_reconfig_schedule.h"
}

namespace {
constexpr uint16_t kRnti = 0x4601;
constexpr int kOldLength = 47;
constexpr uint32_t kGrantPeriod = 10; // 200 grants/s at mu=1.

struct EnvGuard {
  const char *name;
  bool present;
  std::string value;
  explicit EnvGuard(const char *n) : name(n), present(getenv(n) != nullptr), value(present ? getenv(n) : "") {}
  ~EnvGuard() {
    if (present) setenv(name, value.c_str(), 1); else unsetenv(name);
    if (std::string(name) == "ISAC_TD_IGNORE_SIB1") nr_cfg_ignore_sib1_reset_for_test();
  }
};

std::vector<int16_t> polar_grant(int len, uint32_t serial)
{
  uint64_t payload = (1ULL << (len - 1)) | (0x12345u ^ (serial * 0x101u));
  t_nrPolar_params *params = nr_polar_params(NR_POLAR_DCI_MESSAGE_TYPE, len, 8);
  const int bits = params->encoderLength;
  polarReturn(params);
  std::vector<uint32_t> coded((bits + 31) / 32);
  polar_encoder_fast(&payload, coded.data(), kRnti, 1, NR_POLAR_DCI_MESSAGE_TYPE, len, 8);
  std::vector<uint8_t> symbols(bits);
  nr_bit2byte_uint32_8(coded.data(), bits, symbols.data());
  std::vector<int16_t> llr(bits);
  for (int i = 0; i < bits; ++i) llr[i] = symbols[i] ? -6 : 6;
  return llr;
}

struct PolarFixture {
  int actual_length;
  int actual_group;
  std::array<std::vector<int16_t>, 20> grants;
  PolarFixture(int len, int group, uint32_t slot) : actual_length(len), actual_group(group)
  {
    for (int i = 0; i < 20; ++i) grants[i] = polar_grant(len, slot * 20 + i);
  }
  static bool score(int len, int trial, uint16_t *rnti, uint32_t *hash, void *ctx)
  {
    const auto &self = *static_cast<PolarFixture *>(ctx);
    nr_pdcch_blind_raw_result_t raw{};
    if (!nr_pdcch_blind_decode_raw_11(self.grants[trial].data(), 8, len, kRnti, kRnti, &raw)) return false;
    *rnti = raw.rnti;
    *hash = nr_dci_bits_hash(&raw.payload, len);
    return true;
  }
  bool accepts(int len, int group) const
  {
    if (group != actual_group) return false;
    nr_pdcch_blind_raw_result_t raw{};
    return nr_pdcch_blind_decode_raw_11(grants[0].data(), 8, len, kRnti, kRnti, &raw);
  }
};

int32_t legal_row(int, int length, int start, int mapping_b, int add, int maxlen)
{
  return !mapping_b && start == 1 && length == 13 && add == 0 && maxlen == 1 ? 4 : 0;
}

bool pdsch_grant(uint64_t key, uint64_t *generation = nullptr)
{
  nr_pdsch_sweep_ticket_t ticket{};
  nr_pdsch_cfg_hypothesis_t h{}, winner{};
  if (!nr_pdsch_config_sweep_select(key, kRnti, 0, 0, 0, legal_row, &ticket, &h)) return false;
  if (generation) *generation = ticket.generation;
  const bool crc = h.mcs_table == 1 && h.k0 == 0;
  nr_pdsch_config_sweep_feedback(&ticket, crc, &winner);
  return nr_pdsch_config_sweep_is_settled(key, kRnti, 0, 0);
}

nr_pdcch_blind_monitor_cfg_t geometry(int group, int len)
{
  nr_pdcch_blind_monitor_cfg_t c{};
  c.bwp_size = 106;
  c.coreset_rb_offset = group * 6;
  c.coreset_freq_domain = 8;
  c.coreset_duration = 1;
  c.dci_length_override = len;
  return c;
}

struct ReplayResult {
  uint32_t injected_slot = 0;
  uint32_t recovered_slot = 0;
  uint32_t old_epoch_credited = 0;
  uint32_t old_epoch_trials_credited = 0;
  uint32_t stale_dropped = 0;
  bool epoch_changed = false;
  bool old_locked = false;
  bool old_converged = false;
  bool locked = false;
  bool verified = false;
  bool converged = false;
  bool generation_unchanged = false;
  bool sib1_evidence_used = false;
  bool active_elsewhere_used = false;
  bool c0_uss_available = false;
};

ReplayResult run_replay(hidden_reconfig_kind_t kind, bool sib1_available)
{
  ReplayResult r;
  EnvGuard ignore_guard("ISAC_TD_IGNORE_SIB1"), cache_guard("ISAC_SIB1_CACHE");
  const auto change = hidden_reconfig_schedule(kind);
  r.injected_slot = change.slot;
  if (sib1_available) unsetenv("ISAC_TD_IGNORE_SIB1");
  else setenv("ISAC_TD_IGNORE_SIB1", "1", 1);
  setenv("ISAC_SIB1_CACHE", "0", 1);
  nr_cfg_ignore_sib1_reset_for_test();
  nr_cfg_epoch_reset();
  nr_pdcch_blind_rnti_bootstrap_reset_for_test();
  nr_cfg_epoch_set_slots_per_second(2000);
  nr_pdcch_blind_reset_common();
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdcch_coreset_bank_set_remove_hook(nullptr, nullptr);
  while (nr_pdcch_coreset_bank_count()) nr_pdcch_coreset_bank_remove(0);
  nr_cfg_epoch_note_identity(42, 640000, 0);
  nr_cfg_epoch_note_mib(0x1234);
  nr_pdcch_blind_common_config_t common{};
  common.pci = 42;
  common.dl_bwp_size = common.ul_bwp_size = 106;
  if (sib1_available) {
    r.sib1_evidence_used = nr_cfg_epoch_note_sib1(change.old_tac, 0)
        && nr_pdcch_blind_publish_common(&common);
  } else {
    r.sib1_evidence_used = nr_cfg_epoch_note_sib1(change.old_tac, 0)
        || nr_pdcch_blind_publish_common(&common);
  }
  if (!sib1_available) {
    nr_pdcch_blind_monitor_cfg_t c0_uss{};
    r.c0_uss_available = nr_pdcch_blind_monitor_coreset0_uss_cfg(&c0_uss);
  }
  // A verified RAR is a real activity source for both columns of spec 4.6.
  nr_pdcch_blind_rnti_bootstrap_record_verified(kRnti, NR_BLIND_RNTI_CLASS_TC, 0);
  auto old = geometry(change.old_coreset_group, kOldLength);
  const int old_bank = nr_pdcch_coreset_bank_add(&old, kRnti);
  nr_pdcch_dci_length_context_t length{};
  length.rnti = kRnti;
  const uint64_t old_key = (uint64_t(kOldLength) << 32) | change.old_coreset_group;
  // Every synthetic occasion contains 20 distinct real polar codewords for one RNTI.
  for (uint32_t slot = 100; slot < change.slot && (!r.old_locked || !r.old_converged); slot += kGrantPeriod) {
    PolarFixture fixture(kOldLength, change.old_coreset_group, slot);
    if (!r.old_locked) {
      const int found = nr_pdcch_dci_length_sweep_feed(&length.state, PolarFixture::score, &fixture,
                                                       20, 30, 63, kRnti);
      if (found == kOldLength) {
        nr_pdcch_dci_length_context_lock(&length, found);
        r.old_locked = true;
      }
    }
    if (fixture.accepts(kOldLength, change.old_coreset_group)) nr_pdcch_coreset_bank_note_accept(old_bank, slot);
    if (r.old_locked) r.old_converged = pdsch_grant(old_key);
  }
  const uint32_t old_epoch = nr_cfg_epoch_current();
  nr_pdsch_sweep_ticket_t old_ticket{};
  nr_pdsch_cfg_hypothesis_t old_h{};
  nr_pdsch_config_sweep_select(old_key, kRnti, 0, 0, 0, legal_row, &old_ticket, &old_h);

  const int new_len = kind == HIDDEN_RECONF_DCI_LENGTH ? kOldLength + change.dci_length_add : kOldLength;
  const int new_group = kind == HIDDEN_RECONF_CORESET_MOVE ? change.new_coreset_group : change.old_coreset_group;
  const uint64_t new_key = (uint64_t(new_len) << 32) | new_group;
  const bool invisible_sib1_change = kind == HIDDEN_RECONF_SIB1_SEMANTIC && !sib1_available;
  int new_bank = kind == HIDDEN_RECONF_CORESET_MOVE ? -1 : old_bank;
  if (kind == HIDDEN_RECONF_SIB1_SEMANTIC) {
    nr_cfg_epoch_note_sib1(change.new_tac, change.slot);
  } else if (kind == HIDDEN_RECONF_PCI) {
    nr_cfg_epoch_note_identity(42 + change.pci_add, 640000, 0);
  }
  nr_cfg_epoch_drain();
  r.epoch_changed = nr_cfg_epoch_current() != old_epoch;

  // Check the ticket's own context. The local changes have no cell epoch bump;
  // exercise the work guard with a stale queued-work stamp explicitly.
  uint32_t before_passes = 0, before_trials = 0, after_passes = 0, after_trials = 0;
  nr_pdsch_config_sweep_context_stats(old_key, kRnti, 0, 0, &before_passes, &before_trials);
  uint64_t dropped = 0;
  if (!invisible_sib1_change) {
    nr_cfg_epoch_work_t old_work{};
    const uint32_t queued_stamp = r.epoch_changed ? old_epoch : old_epoch ^ 1u;
    nr_cfg_epoch_work_begin(&old_work, queued_stamp, &dropped);
    nr_pdsch_config_sweep_feedback(&old_ticket, true, nullptr);
    nr_cfg_epoch_work_end(&old_work);
  }
  nr_pdsch_config_sweep_context_stats(old_key, kRnti, 0, 0, &after_passes, &after_trials);
  r.old_epoch_credited = after_passes - before_passes;
  r.old_epoch_trials_credited = after_trials - before_trials;
  r.stale_dropped = dropped;

  const uint32_t budget = hidden_reconfig_hard(kind) ? 60000u : 20000u;
  for (uint32_t slot = change.slot; slot <= change.slot + budget; slot += kGrantPeriod) {
    PolarFixture fixture(new_len, new_group, slot);
    bool accepted = fixture.accepts(new_len, new_group);
    if (kind == HIDDEN_RECONF_CORESET_MOVE && new_bank < 0) {
      // Traffic outside the old geometry demotes and ultimately removes it.
      nr_pdcch_coreset_bank_tick(slot, true, 50, 300);
      if (accepted && nr_pdcch_coreset_bank_count() == 0) {
        auto moved = geometry(new_group, new_len);
        new_bank = nr_pdcch_coreset_bank_add(&moved, kRnti);
      }
    }
    if (kind == HIDDEN_RECONF_DCI_LENGTH) {
      if (accepted) nr_pdcch_blind_rnti_bootstrap_record_trusted(kRnti, NR_BLIND_RNTI_CLASS_C, slot);
      const bool active_elsewhere = nr_pdcch_blind_rnti_bootstrap_recent(kRnti, slot, 1);
      r.active_elsewhere_used |= active_elsewhere;
      nr_pdcch_dci_length_context_note_occasion(&length,
          fixture.accepts(kOldLength, change.old_coreset_group), active_elsewhere, 3);
      if (length.len_state == NR_LEN_SUSPECT) {
        const int found = nr_pdcch_dci_length_sweep_feed(&length.state, PolarFixture::score, &fixture,
                                                         20, 30, 63, kRnti);
        if (found == new_len) nr_pdcch_dci_length_context_lock(&length, found);
      }
    }
    if (accepted && new_bank >= 0) {
      nr_pdcch_coreset_bank_note_accept(new_bank, slot);
      nr_pdcch_coreset_bank_note_dci(new_bank, slot, kRnti, slot * 19u);
    }
    uint64_t current_generation = 0;
    const bool settled = accepted && new_bank >= 0 && pdsch_grant(new_key, &current_generation);
    r.locked = length.len_state == NR_LEN_LOCKED && length.found[0] == new_len;
    r.verified = new_bank >= 0 && nr_pdcch_coreset_bank_state(new_bank) == NR_CORESET_VERIFIED;
    r.generation_unchanged = current_generation == old_ticket.generation;
    r.converged = settled && (invisible_sib1_change || new_key != old_key || !r.generation_unchanged);
    if (r.locked && r.verified && r.converged) { r.recovered_slot = slot; break; }
  }
  nr_pdsch_config_sweep_reset_all();
  while (nr_pdcch_coreset_bank_count()) nr_pdcch_coreset_bank_remove(0);
  nr_pdcch_blind_reset_common();
  nr_cfg_epoch_reset();
  fprintf(stderr, "RECONFIG_REPLAY kind=%s sib1=%u injected=%u recovered=%u epoch=%u "
          "old_passes=%u old_trials=%u stale_dropped=%u\n",
          hidden_reconfig_name(kind), sib1_available, r.injected_slot, r.recovered_slot,
          r.epoch_changed, r.old_epoch_credited, r.old_epoch_trials_credited, r.stale_dropped);
  return r;
}

void expect_replay(hidden_reconfig_kind_t kind, bool sib1_available)
{
  if (!nr_cfg_reconf_enabled()) GTEST_SKIP() << "run with ISAC_RECONF=1";
  const ReplayResult r = run_replay(kind, sib1_available);
  const uint32_t budget = hidden_reconfig_hard(kind) ? 60000u : 20000u;
  EXPECT_TRUE(r.old_locked);
  EXPECT_TRUE(r.old_converged);
  EXPECT_TRUE(r.locked);
  EXPECT_TRUE(r.verified);
  EXPECT_TRUE(r.converged);
  EXPECT_GE(r.recovered_slot, r.injected_slot) << hidden_reconfig_name(kind);
  if (r.recovered_slot >= r.injected_slot) {
    EXPECT_LE(r.recovered_slot - r.injected_slot, budget) << hidden_reconfig_name(kind);
  }
  EXPECT_EQ(r.old_epoch_credited, 0u);
  EXPECT_EQ(r.old_epoch_trials_credited, 0u);
  if (kind == HIDDEN_RECONF_DCI_LENGTH) { EXPECT_TRUE(r.active_elsewhere_used); }
  EXPECT_EQ(r.epoch_changed, kind == HIDDEN_RECONF_PCI
      || (kind == HIDDEN_RECONF_SIB1_SEMANTIC && sib1_available));
  if (kind != HIDDEN_RECONF_SIB1_SEMANTIC || sib1_available) {
    EXPECT_GT(r.stale_dropped, 0u);
  }
  if (kind == HIDDEN_RECONF_SIB1_SEMANTIC && !sib1_available) {
    EXPECT_EQ(r.stale_dropped, 0u);
    EXPECT_TRUE(r.generation_unchanged);
  }
  if (sib1_available) EXPECT_TRUE(r.sib1_evidence_used);
  else {
    EXPECT_FALSE(r.sib1_evidence_used);
    EXPECT_FALSE(r.c0_uss_available);
  }
}
} // namespace

TEST(ReconfigReplay, DciLengthSib1Available) { expect_replay(HIDDEN_RECONF_DCI_LENGTH, true); }
TEST(ReconfigReplay, DciLengthSib1Absent) { expect_replay(HIDDEN_RECONF_DCI_LENGTH, false); }
TEST(ReconfigReplay, CoresetMoveSib1Available) { expect_replay(HIDDEN_RECONF_CORESET_MOVE, true); }
TEST(ReconfigReplay, CoresetMoveSib1Absent) { expect_replay(HIDDEN_RECONF_CORESET_MOVE, false); }

// R10 must turn an old Technique D winner into VERIFY after a SIB1-triggered epoch bump.
TEST(ReconfigReplay, DISABLED_Sib1SemanticChangeSib1Available) { expect_replay(HIDDEN_RECONF_SIB1_SEMANTIC, true); }
// TAC-only change is invisible without SIB1: no epoch bump or Technique D reset.
TEST(ReconfigReplay, Sib1SemanticChangeSib1Absent) { expect_replay(HIDDEN_RECONF_SIB1_SEMANTIC, false); }
// R10 must discard the old cell's Technique D winner on HARD_RESET.
TEST(ReconfigReplay, DISABLED_PciChangeSib1Available) { expect_replay(HIDDEN_RECONF_PCI, true); }
// R10 must discard the old cell's Technique D winner on HARD_RESET without SIB1.
TEST(ReconfigReplay, DISABLED_PciChangeSib1Absent) { expect_replay(HIDDEN_RECONF_PCI, false); }
