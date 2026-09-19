/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 */

/*! \file nr_passive_mac_ta_test.cc
 * \brief Timing advance parsed out of overheard MAC PDUs. The parsers run on payloads whose RNTI
 * class is only a hypothesis, so rejecting non-RARs matters as much as parsing real ones.
 */

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
#include "openair1/PHY/NR_UE_TRANSPORT/nr_passive_mac_ta.h"
}

namespace {
/* TS 38.321 6.1.5 / 6.2.3: E=0 T=1 RAPID, then R(1) TA(12) UL-grant(27) TC-RNTI(16). */
std::vector<uint8_t> make_rar(uint8_t rapid, uint16_t ta, uint16_t tc_rnti)
{
  std::vector<uint8_t> p(8, 0);
  p[0] = (uint8_t)(0x40 | (rapid & 0x3F));      // E=0, T=1
  p[1] = (uint8_t)((ta >> 5) & 0x7F);            // R(1) + TA[11:5]
  p[2] = (uint8_t)((ta & 0x1F) << 3);            // TA[4:0] + start of the UL grant
  p[6] = (uint8_t)(tc_rnti >> 8);
  p[7] = (uint8_t)(tc_rnti & 0xFF);
  return p;
}
} // namespace

TEST(PassiveMacTa, ParsesARealRarAndRecoversTheAdvance)
{
  const std::vector<uint8_t> pdu = make_rar(17, 1234, 0x4601);
  uint8_t rapid = 0;
  uint16_t ta = 0, tc = 0;
  ASSERT_TRUE(nr_passive_mac_rar_ta(pdu.data(), (uint32_t)pdu.size(), &rapid, &ta, &tc));
  EXPECT_EQ(rapid, 17);
  EXPECT_EQ(ta, 1234);
  EXPECT_EQ(tc, 0x4601);
}

TEST(PassiveMacTa, SkipsABackoffIndicatorSubheaderToReachTheRar)
{
  std::vector<uint8_t> pdu;
  pdu.push_back(0x80);                            // E=1, T=0, R=00, BI=0: no payload follows
  const std::vector<uint8_t> rar = make_rar(3, 99, 0x1234);
  pdu.insert(pdu.end(), rar.begin(), rar.end());
  uint8_t rapid = 0;
  uint16_t ta = 0, tc = 0;
  ASSERT_TRUE(nr_passive_mac_rar_ta(pdu.data(), (uint32_t)pdu.size(), &rapid, &ta, &tc));
  EXPECT_EQ(rapid, 3);
  EXPECT_EQ(ta, 99);
}

TEST(PassiveMacTa, RejectsRandomPayloadsRatherThanInventingAnAdvance)
{
  /* This is the case that matters: the parser is fed every CRC-OK transport block whose DCI said
   * RA-RNTI, and a false-accepted DCI yields an arbitrary payload. A structural check must throw
   * those out, or the receiver reports ranges for UEs that do not exist. */
  std::mt19937 g(2026);
  int accepted = 0;
  for (int t = 0; t < 2000; t++) {
    std::vector<uint8_t> pdu(16);
    for (auto &b : pdu) b = (uint8_t)(g() & 0xFF);
    uint16_t ta = 0, tc = 0;
    uint8_t rapid = 0;
    if (nr_passive_mac_rar_ta(pdu.data(), (uint32_t)pdu.size(), &rapid, &ta, &tc)) {
      accepted++;
      EXPECT_LE(ta, 3846);      // whatever it accepts must at least be in range
      EXPECT_NE(tc, 0);
    }
  }
  /* Measured progression on these exact draws: field ranges alone 700+/2000, + zero-padding rule
   * 248, + full chain validation 54, + validating EVERY RAR in the chain ~20. The residual is
   * structural: a 16-byte random payload can split into two back-to-back RARs that fill the PDU
   * exactly, so there is no padding left to contradict. That is bounded, not eliminated -- which is
   * why the caller only parses payloads whose DCI CLASS was RA-RNTI, and only logs. */
  EXPECT_LE(accepted, 40);
}

TEST(PassiveMacTa, FindsTheTaCommandCeAfterOtherSubheaders)
{
  /* R/F/LCID subheaders: one logical channel with an 8-bit length, then the TA CE (LCID 0x3D). */
  std::vector<uint8_t> pdu;
  pdu.push_back(0x01);            // LCID 1, F=0 -> 8-bit L
  pdu.push_back(3);               // L = 3
  pdu.insert(pdu.end(), {0xAA, 0xBB, 0xCC});
  pdu.push_back(0x3D);            // TA command CE
  pdu.push_back((uint8_t)((2 << 6) | 45));  // TAG 2, TA command 45
  uint8_t tag = 0, cmd = 0;
  ASSERT_TRUE(nr_passive_mac_dlsch_ta(pdu.data(), (uint32_t)pdu.size(), &tag, &cmd));
  EXPECT_EQ(tag, 2);
  EXPECT_EQ(cmd, 45);
}

TEST(PassiveMacTa, ReportsNothingWhenThePduCarriesNoTaCe)
{
  std::vector<uint8_t> pdu = {0x01, 2, 0x11, 0x22, 0x3F /* padding to the end */};
  uint8_t tag = 0, cmd = 0;
  EXPECT_FALSE(nr_passive_mac_dlsch_ta(pdu.data(), (uint32_t)pdu.size(), &tag, &cmd));
}

TEST(PassiveMacTa, RangeArithmeticMatchesTheSpec)
{
  /* TS 38.213 4.2: N_TA = T_A * 16 * 64 / 2^mu in units of T_c = 1/(480000*4096) s; the advance is
   * the round trip, so range = c * N_TA * T_c / 2. At mu=1, one T_A step is
   * 16*64/2 * (1/1.96608e9) s = 260.4 ns round trip = 39.0 m one way. */
  const double one_step = nr_passive_mac_ta_metres(1, 1);
  EXPECT_NEAR(one_step, 39.06, 0.1);
  EXPECT_NEAR(nr_passive_mac_ta_metres(100, 1), 100.0 * one_step, 1e-6);
  EXPECT_NEAR(nr_passive_mac_ta_metres(100, 0), 2.0 * nr_passive_mac_ta_metres(100, 1), 1e-6);
  EXPECT_LT(nr_passive_mac_ta_metres(0, 1), 1e-9);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
