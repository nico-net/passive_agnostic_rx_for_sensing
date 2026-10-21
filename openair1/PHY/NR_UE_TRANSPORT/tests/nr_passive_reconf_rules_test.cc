#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "nr_passive_reconf_rules.h"
#include "nr_passive_mac_ta.h"
}

static void put(uint8_t *b, unsigned pos, unsigned n, uint64_t v) /* MSB-first */
{
  for (unsigned i = 0; i < n; i++) {
    const unsigned bit = (v >> (n - 1 - i)) & 1u, p = pos + i;
    b[p >> 3] = (uint8_t)((b[p >> 3] & ~(0x80u >> (p & 7))) | (bit ? (0x80u >> (p & 7)) : 0));
  }
}

// TS 38.331 UL-CCCH-Message: RRCResumeRequest = 0 | 01 | ShortI-RNTI(24) | MAC-I(16) | cause(4) | spare(1)
TEST(ReconfRules, ResumeRequestCarriesTheShortIRnti) {
  uint8_t sdu[6] = {0};
  put(sdu, 0, 1, 0); put(sdu, 1, 2, 1); put(sdu, 3, 24, 0xABCDEF); put(sdu, 27, 16, 0x1234); put(sdu, 43, 4, 4);
  nr_reconf_id_t id;
  ASSERT_TRUE(nr_reconf_ccch_identity(sdu, &id));
  EXPECT_EQ(id.kind, NR_RECONF_ID_RESUME);
  EXPECT_EQ(id.value, 0xABCDEFu);
  EXPECT_TRUE(nr_reconf_id_linkable(&id));
}
// RRCSetupRequest = 0 | 00 | choice(1) | 39 bit | cause(4) | spare(1)
TEST(ReconfRules, SetupRequestStmsiLinksButRandomValueDoesNot) {
  uint8_t sdu[6] = {0};
  put(sdu, 0, 3, 0); put(sdu, 3, 1, 0); put(sdu, 4, 39, 0x5A5A5A5A5Aull); put(sdu, 43, 4, 3);
  nr_reconf_id_t id;
  ASSERT_TRUE(nr_reconf_ccch_identity(sdu, &id));
  EXPECT_EQ(id.kind, NR_RECONF_ID_SETUP_STMSI);
  EXPECT_EQ(id.value, 0x5A5A5A5A5Aull);
  EXPECT_TRUE(nr_reconf_id_linkable(&id));
  put(sdu, 3, 1, 1);
  ASSERT_TRUE(nr_reconf_ccch_identity(sdu, &id));
  EXPECT_EQ(id.kind, NR_RECONF_ID_SETUP_RANDOM);
  EXPECT_FALSE(nr_reconf_id_linkable(&id));
  put(sdu, 1, 2, 2); // rrcReestablishmentRequest is not an attach identity
  EXPECT_FALSE(nr_reconf_ccch_identity(sdu, &id));
  put(sdu, 0, 1, 1);
  EXPECT_FALSE(nr_reconf_ccch_identity(sdu, &id));
}
TEST(ReconfRules, ContentionResolutionIdentityMustEqualMsg3) {
  const uint8_t m3[6] = {1, 2, 3, 4, 5, 6};
  uint8_t cr[6];
  memcpy(cr, m3, 6);
  EXPECT_TRUE(nr_reconf_cr_matches(m3, cr));
  cr[5] ^= 1;
  EXPECT_FALSE(nr_reconf_cr_matches(m3, cr));
  uint8_t b[6];
  nr_reconf_unpack48(nr_reconf_pack48(m3), b);
  EXPECT_EQ(memcmp(b, m3, 6), 0);
}
// TS 38.321 6.1.2: Msg3 = [C-RNTI CE (LCID 58, 2 B)] CCCH (LCID 52, 6 B)
TEST(ReconfRules, Msg3CcchIsFoundBehindACrntiCe) {
  const uint8_t pdu[] = {0x34, 1, 2, 3, 4, 5, 6, 0x3F, 0};
  uint8_t out[6];
  ASSERT_TRUE(nr_reconf_ulsch_ccch(pdu, sizeof(pdu), out));
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[5], 6);
  const uint8_t two[] = {0x3A, 0x12, 0x34, 0x34, 9, 8, 7, 6, 5, 4};
  ASSERT_TRUE(nr_reconf_ulsch_ccch(two, sizeof(two), out));
  EXPECT_EQ(out[0], 9);
  const uint8_t none[] = {0x01, 3, 0xAA, 0xBB, 0xCC};
  EXPECT_FALSE(nr_reconf_ulsch_ccch(none, sizeof(none), out));
}
// TS 38.321 6.1.2 / Table 6.2.1-1: Msg4 = UE Contention Resolution Identity CE (LCID 62) + CCCH SDU (LCID 0, F=0, L)
TEST(ReconfRules, DlschScanFindsCrCcchAndSrb1) {
  const uint8_t msg4[] = {0x3E, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0x00, 0x03, 0x11, 0x22, 0x33};
  nr_passive_mac_dl_scan_t s;
  ASSERT_TRUE(nr_passive_mac_dlsch_scan(msg4, sizeof(msg4), &s));
  EXPECT_TRUE(s.has_cr);
  EXPECT_EQ(s.cr[0], 0xA1);
  EXPECT_EQ(s.cr[5], 0xA6);
  EXPECT_EQ(s.ccch_bytes, 3u);
  uint8_t big[2 + 64 + 1] = {0x01, 64};
  ASSERT_TRUE(nr_passive_mac_dlsch_scan(big, sizeof(big), &s));
  EXPECT_FALSE(s.has_cr);
  EXPECT_EQ(s.srb1_max_bytes, 64u);
  const uint8_t trunc[] = {0x3E, 1, 2, 3};
  EXPECT_FALSE(nr_passive_mac_dlsch_scan(trunc, sizeof(trunc), &s));
  const uint8_t noise[] = {0x2F, 0x00, 0x00};
  EXPECT_FALSE(nr_passive_mac_dlsch_scan(noise, sizeof(noise), &s));
}
// TS 38.331 12: 10 ms (setup/resume/reestablishment), 16 ms (reconfiguration); mu scales the slots
TEST(ReconfRules, RrcProcessingDelayInSlots) {
  EXPECT_EQ(nr_reconf_rrc_delay_slots(NR_RECONF_RRC_SETUP, 0, false), 10);
  EXPECT_EQ(nr_reconf_rrc_delay_slots(NR_RECONF_RRC_RECONF, 1, false), 32);
  EXPECT_EQ(nr_reconf_rrc_delay_slots(NR_RECONF_RRC_RECONF, 1, true), 32 + 5);
  EXPECT_LT(nr_reconf_rrc_delay_slots(NR_RECONF_RRC_RECONF, 4, false), 0);
}
// TS 38.133 8.6.2
TEST(ReconfRules, BwpSwitchDelayTable) {
  EXPECT_EQ(nr_reconf_bwp_switch_delay_slots(1, 1), 2);
  EXPECT_EQ(nr_reconf_bwp_switch_delay_slots(1, 2), 5);
  EXPECT_EQ(nr_reconf_bwp_switch_delay_slots(0, 2), 3);
  EXPECT_LT(nr_reconf_bwp_switch_delay_slots(5, 1), 0);
}
// TS 38.321 5.3.2.2 / 38.214 5.1.3.1
TEST(ReconfRules, UntoggledNdiWithAnotherTbsIsAHarqReset) {
  EXPECT_TRUE(nr_reconf_hq_discontinuity(true, 1, 8000, 1, 12000));
  EXPECT_FALSE(nr_reconf_hq_discontinuity(true, 1, 8000, 1, 8000));  // a retransmission
  EXPECT_FALSE(nr_reconf_hq_discontinuity(true, 1, 8000, 0, 12000)); // toggled: new data
  EXPECT_FALSE(nr_reconf_hq_discontinuity(false, 1, 8000, 1, 12000));
  EXPECT_FALSE(nr_reconf_hq_discontinuity(true, 1, 0, 1, 12000));
}
TEST(ReconfRules, RrcWindowHoldsThenArmsAcceleratedRelockThenExpires) {
  nr_reconf_reset();
  const int mu = 1;
  EXPECT_EQ(nr_reconf_rrc_phase(0x4601, 1000, mu), NR_RECONF_PHASE_NONE);
  nr_reconf_rrc_expect(0x4601, 1000, NR_RECONF_RRC_RECONF, mu, false); // effective at 1032
  EXPECT_EQ(nr_reconf_rrc_effective_slot(0x4601), 1032);
  EXPECT_EQ(nr_reconf_rrc_phase(0x4601, 999, mu), NR_RECONF_PHASE_NONE);
  EXPECT_EQ(nr_reconf_rrc_phase(0x4601, 1031, mu), NR_RECONF_PHASE_HOLD);
  EXPECT_EQ(nr_reconf_rrc_n_suspect(0x4601, 1031, mu, 200), 200u);
  EXPECT_EQ(nr_reconf_rrc_phase(0x4601, 1032, mu), NR_RECONF_PHASE_ARMED);
  EXPECT_EQ(nr_reconf_rrc_n_suspect(0x4601, 1040, mu, 200), (uint32_t)NR_RECONF_ARM_N_SUSPECT);
  EXPECT_EQ(nr_reconf_rrc_n_suspect(0x4601, 1040, mu, 5), 5u);
  EXPECT_EQ(nr_reconf_rrc_n_suspect(0x4602, 1040, mu, 200), 200u); // another RNTI is untouched
  EXPECT_EQ(nr_reconf_rrc_phase(0x4601, 1032 + (NR_RECONF_ARM_MS << mu), mu), NR_RECONF_PHASE_NONE);
}
TEST(ReconfRules, DciBwpSwitchIsAppliedAfterTheSwitchDelay) {
  nr_reconf_reset();
  nr_reconf_bwp_switch_dci(0x4601, 2, 5000, 1); // mu 1: new BWP grants between +2 and +5 slots
  int64_t a = 0, b = 0;
  ASSERT_TRUE(nr_reconf_bwp_expected_window(0x4601, &a, &b));
  EXPECT_EQ(a, 5002);
  EXPECT_EQ(b, 5005);
  uint16_t rnti;
  int entry;
  EXPECT_FALSE(nr_reconf_bwp_due(5004, &rnti, &entry));
  ASSERT_TRUE(nr_reconf_bwp_due(5005, &rnti, &entry));
  EXPECT_EQ(rnti, 0x4601);
  EXPECT_EQ(entry, 2);
  EXPECT_FALSE(nr_reconf_bwp_due(9999, &rnti, &entry)); // once
  EXPECT_FALSE(nr_reconf_bwp_expected_window(0x4601, &a, &b));
}
